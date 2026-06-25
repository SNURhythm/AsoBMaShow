#include "targets.h"
#include "AndroidNatives.h"

#if TARGET_OS_ANDROID

#include "audio/NativeMusicPlayer.h"

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_log.h>
#include <SDL2/SDL_system.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <jni.h>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {

constexpr const char *kAndroidTreeSentinel = "@androidtree@";
constexpr const char *kErrorPrefix = "__ERROR__:";
constexpr const char *kPendingImportResult = "__PENDING_ARCHIVE_IMPORT__";

std::mutex gAndroidTreeMutex;
std::unordered_map<std::string, std::string> gTreeUrisById;
std::mutex gExternalActivityPauseMutex;
std::condition_variable gExternalActivityPauseCv;
bool gExternalActivityPauseRequested = false;
bool gExternalActivityPauseAcknowledged = false;
constexpr Sint32 kExternalActivityPauseWakeCode = 0x41535050;

struct AndroidDownloadProgressBridge {
  std::atomic_bool *cancelled = nullptr;
  AndroidDownloadProgressCallback *progressCallback = nullptr;
};

std::mutex gAndroidDownloadProgressMutex;
std::unordered_map<jlong, AndroidDownloadProgressBridge *>
    gAndroidDownloadProgressBridges;
jlong gNextAndroidDownloadProgressToken = 1;

struct UniqueFd {
  explicit UniqueFd(int fd) : value(fd) {}
  ~UniqueFd() {
    if (value >= 0) {
      close(value);
    }
  }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  int value;
};

std::uint64_t fnv1a64(const std::string &value) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[static_cast<std::size_t>(i)] = digits[value & 0xfu];
    value >>= 4;
  }
  return out;
}

std::string sanitizePathComponent(std::string value) {
  for (char &c : value) {
    if (c == '/' || c == '\\' || c == '\0') {
      c = '_';
    }
  }
  if (value.empty()) {
    value = "Library";
  }
  return value;
}

std::filesystem::path makeAndroidTreeRootPath(const std::string &treeUri,
                                              const std::string &displayName) {
  return std::filesystem::path(kAndroidTreeSentinel) / hex64(fnv1a64(treeUri)) /
         sanitizePathComponent(displayName);
}

bool splitAndroidTreePath(const std::filesystem::path &path,
                          std::string &treeId,
                          std::filesystem::path &relativePath) {
  treeId.clear();
  relativePath.clear();
  std::vector<std::string> parts;
  for (const auto &part : path.lexically_normal()) {
    parts.push_back(part.generic_string());
  }
  if (parts.size() < 3 || parts[0] != kAndroidTreeSentinel ||
      parts[1].empty()) {
    return false;
  }
  treeId = parts[1];
  for (std::size_t i = 3; i < parts.size(); ++i) {
    relativePath /= parts[i];
  }
  return true;
}

std::optional<std::string> treeUriForId(const std::string &treeId) {
  std::lock_guard<std::mutex> lock(gAndroidTreeMutex);
  const auto it = gTreeUrisById.find(treeId);
  if (it == gTreeUrisById.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::string jstringToUtf8(JNIEnv *env, jstring value) {
  if (env == nullptr || value == nullptr) {
    return {};
  }
  const char *chars = env->GetStringUTFChars(value, nullptr);
  if (chars == nullptr) {
    return {};
  }
  std::string result(chars);
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

bool clearPendingJavaException(JNIEnv *env, std::string &errorMessage) {
  if (env == nullptr || !env->ExceptionCheck()) {
    return false;
  }
  env->ExceptionDescribe();
  env->ExceptionClear();
  errorMessage = "Android Java call failed.";
  return true;
}

jlong registerAndroidDownloadProgressBridge(
    AndroidDownloadProgressBridge &bridge) {
  std::lock_guard<std::mutex> lock(gAndroidDownloadProgressMutex);
  const jlong token = gNextAndroidDownloadProgressToken++;
  if (gNextAndroidDownloadProgressToken <= 0) {
    gNextAndroidDownloadProgressToken = 1;
  }
  gAndroidDownloadProgressBridges[token] = &bridge;
  return token;
}

void unregisterAndroidDownloadProgressBridge(jlong token) {
  std::lock_guard<std::mutex> lock(gAndroidDownloadProgressMutex);
  gAndroidDownloadProgressBridges.erase(token);
}

AndroidDownloadProgressBridge *androidDownloadProgressBridge(jlong token) {
  std::lock_guard<std::mutex> lock(gAndroidDownloadProgressMutex);
  const auto it = gAndroidDownloadProgressBridges.find(token);
  return it == gAndroidDownloadProgressBridges.end() ? nullptr : it->second;
}

std::string callActivityStringMethod(const char *methodName,
                                     const char *signature,
                                     const char *argument,
                                     std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    errorMessage = "Android activity is not available.";
    return {};
  }

  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    errorMessage = "Android activity class is not available.";
    env->DeleteLocalRef(activity);
    return {};
  }

  jmethodID method = env->GetMethodID(activityClass, methodName, signature);
  if (method == nullptr) {
    errorMessage = std::string("Android activity method missing: ") +
                   methodName;
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }

  jstring javaArgument = nullptr;
  jobject javaResult = nullptr;
  if (argument != nullptr) {
    javaArgument = env->NewStringUTF(argument);
    javaResult = env->CallObjectMethod(activity, method, javaArgument);
  } else {
    javaResult = env->CallObjectMethod(activity, method);
  }

  if (clearPendingJavaException(env, errorMessage)) {
    if (javaArgument != nullptr) {
      env->DeleteLocalRef(javaArgument);
    }
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }

  std::string result = jstringToUtf8(env, static_cast<jstring>(javaResult));
  if (javaResult != nullptr) {
    env->DeleteLocalRef(javaResult);
  }
  if (javaArgument != nullptr) {
    env->DeleteLocalRef(javaArgument);
  }
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return result;
}

std::string callActivityStringMethod2(const char *methodName,
                                      const char *signature,
                                      const char *argument1,
                                      const char *argument2,
                                      std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    errorMessage = "Android activity is not available.";
    return {};
  }

  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    errorMessage = "Android activity class is not available.";
    env->DeleteLocalRef(activity);
    return {};
  }

  jmethodID method = env->GetMethodID(activityClass, methodName, signature);
  if (method == nullptr) {
    errorMessage = std::string("Android activity method missing: ") +
                   methodName;
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }

  jstring javaArgument1 = env->NewStringUTF(argument1 != nullptr ? argument1 : "");
  jstring javaArgument2 = env->NewStringUTF(argument2 != nullptr ? argument2 : "");
  jobject javaResult =
      env->CallObjectMethod(activity, method, javaArgument1, javaArgument2);

  if (clearPendingJavaException(env, errorMessage)) {
    env->DeleteLocalRef(javaArgument1);
    env->DeleteLocalRef(javaArgument2);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }

  std::string result = jstringToUtf8(env, static_cast<jstring>(javaResult));
  if (javaResult != nullptr) {
    env->DeleteLocalRef(javaResult);
  }
  env->DeleteLocalRef(javaArgument1);
  env->DeleteLocalRef(javaArgument2);
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return result;
}

std::string callActivityStringMethod2Long(const char *methodName,
                                          const char *signature,
                                          const char *argument1,
                                          const char *argument2,
                                          jlong argument3,
                                          std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    errorMessage = "Android activity is not available.";
    return {};
  }

  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    errorMessage = "Android activity class is not available.";
    env->DeleteLocalRef(activity);
    return {};
  }

  jmethodID method = env->GetMethodID(activityClass, methodName, signature);
  if (method == nullptr) {
    errorMessage = std::string("Android activity method missing: ") +
                   methodName;
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }

  jstring javaArgument1 =
      env->NewStringUTF(argument1 != nullptr ? argument1 : "");
  jstring javaArgument2 =
      env->NewStringUTF(argument2 != nullptr ? argument2 : "");
  jobject javaResult = env->CallObjectMethod(
      activity, method, javaArgument1, javaArgument2, argument3);

  if (clearPendingJavaException(env, errorMessage)) {
    env->DeleteLocalRef(javaArgument1);
    env->DeleteLocalRef(javaArgument2);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }

  std::string result = jstringToUtf8(env, static_cast<jstring>(javaResult));
  if (javaResult != nullptr) {
    env->DeleteLocalRef(javaResult);
  }
  env->DeleteLocalRef(javaArgument1);
  env->DeleteLocalRef(javaArgument2);
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return result;
}

std::optional<int> callActivityIntMethod2(const char *methodName,
                                          const char *signature,
                                          const char *argument1,
                                          const char *argument2,
                                          std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    errorMessage = "Android activity is not available.";
    return std::nullopt;
  }

  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    errorMessage = "Android activity class is not available.";
    env->DeleteLocalRef(activity);
    return std::nullopt;
  }

  jmethodID method = env->GetMethodID(activityClass, methodName, signature);
  if (method == nullptr) {
    errorMessage = std::string("Android activity method missing: ") +
                   methodName;
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return std::nullopt;
  }

  jstring javaArgument1 = env->NewStringUTF(argument1 != nullptr ? argument1 : "");
  jstring javaArgument2 = env->NewStringUTF(argument2 != nullptr ? argument2 : "");
  const jint javaResult =
      env->CallIntMethod(activity, method, javaArgument1, javaArgument2);

  if (clearPendingJavaException(env, errorMessage)) {
    env->DeleteLocalRef(javaArgument1);
    env->DeleteLocalRef(javaArgument2);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return std::nullopt;
  }

  env->DeleteLocalRef(javaArgument1);
  env->DeleteLocalRef(javaArgument2);
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return static_cast<int>(javaResult);
}

bool parseBridgeResult(const std::string &result, std::string &value,
                       std::string &errorMessage) {
  if (result.rfind(kErrorPrefix, 0) == 0) {
    errorMessage = result.substr(std::char_traits<char>::length(kErrorPrefix));
    return false;
  }
  if (result.empty()) {
    errorMessage = "Android operation was cancelled.";
    return false;
  }
  value = result;
  return true;
}

std::vector<std::string> splitLines(const std::string &value) {
  std::vector<std::string> lines;
  std::stringstream stream(value);
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty()) {
      lines.push_back(std::move(line));
    }
  }
  return lines;
}

std::string musicMetadataPayload(const AndroidNativeMusicMetadata &metadata) {
  auto sanitizeLine = [](std::string value) {
    std::replace(value.begin(), value.end(), '\n', ' ');
    std::replace(value.begin(), value.end(), '\r', ' ');
    return value;
  };
  return sanitizeLine(metadata.title) + "\n" + sanitizeLine(metadata.artist) +
         "\n" + sanitizeLine(metadata.album);
}

long long parseLongLongOrZero(const std::string &value) {
  try {
    return std::stoll(value);
  } catch (...) {
    return 0;
  }
}

} // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeDownloadUrlToFileProgress(
    JNIEnv *, jclass, jlong progressToken, jlong downloadedBytes,
    jlong totalBytes) {
  AndroidDownloadProgressCallback *progressCallback = nullptr;
  if (AndroidDownloadProgressBridge *bridge =
          androidDownloadProgressBridge(progressToken);
      bridge != nullptr) {
    progressCallback = bridge->progressCallback;
  }
  if (progressCallback == nullptr || !*progressCallback) {
    return;
  }
  (*progressCallback)(downloadedBytes > 0
                          ? static_cast<std::uint64_t>(downloadedBytes)
                          : 0,
                      totalBytes > 0 ? static_cast<std::uint64_t>(totalBytes)
                                     : 0);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeDownloadUrlToFileCancelled(
    JNIEnv *, jclass, jlong progressToken) {
  if (AndroidDownloadProgressBridge *bridge =
          androidDownloadProgressBridge(progressToken);
      bridge != nullptr && bridge->cancelled != nullptr &&
      bridge->cancelled->load()) {
    return JNI_TRUE;
  }
  return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeMusicControlEvent(
    JNIEnv *env, jclass, jstring eventName) {
  const std::string event = jstringToUtf8(env, eventName);
  if (event == "previous") {
    native_music_player::NotifyControlEvent(
        native_music_player::ControlEvent::Previous);
  } else if (event == "next") {
    native_music_player::NotifyControlEvent(
        native_music_player::ControlEvent::Next);
  } else if (event == "finished") {
    native_music_player::NotifyControlEvent(
        native_music_player::ControlEvent::Finished);
  }
}

std::string GetAndroidExternalFilesDir() {
  if (const char *external = SDL_AndroidGetExternalStoragePath();
      external != nullptr && external[0] != '\0') {
    return external;
  }
  if (const char *internal = SDL_AndroidGetInternalStoragePath();
      internal != nullptr && internal[0] != '\0') {
    return internal;
  }
  return ".";
}

std::string GetAndroidInternalFilesDir() {
  if (const char *internal = SDL_AndroidGetInternalStoragePath();
      internal != nullptr && internal[0] != '\0') {
    return internal;
  }

  std::string callError;
  const std::string result = callActivityStringMethod(
      "getInternalFilesDirPath", "()Ljava/lang/String;", nullptr, callError);
  if (callError.empty() && !result.empty() &&
      result.rfind(kErrorPrefix, 0) != 0) {
    return result;
  }
  return GetAndroidExternalFilesDir();
}

bool AndroidBuildHasManageExternalStorage() {
  std::string callError;
  const std::string result = callActivityStringMethod(
      "hasManageExternalStorageBuildVariant", "()Ljava/lang/String;", nullptr,
      callError);
  return callError.empty() && result == "1";
}

bool PickAndroidChartFolder(std::filesystem::path &rootPath,
                            std::string &treeUri,
                            std::string &errorMessage) {
  rootPath.clear();
  treeUri.clear();
  RequestAndroidExternalActivityRenderPause();
  struct ExternalActivityPauseReset {
    ~ExternalActivityPauseReset() { FinishAndroidExternalActivityRenderPause(); }
  } externalActivityPauseReset;

  {
    std::string permissionError;
    const std::string permissionResult = callActivityStringMethod(
        "ensureManageExternalStorageAccess", "()Ljava/lang/String;", nullptr,
        permissionError);
    if (!permissionError.empty()) {
      SDL_Log("Android all-files permission request failed: %s",
              permissionError.c_str());
    } else if (permissionResult.rfind(kErrorPrefix, 0) == 0) {
      SDL_Log("Android all-files permission unavailable: %s",
              permissionResult
                  .substr(std::char_traits<char>::length(kErrorPrefix))
                  .c_str());
    }
  }

  std::string callError;
  const std::string result =
      callActivityStringMethod("pickChartFolder", "()Ljava/lang/String;",
                               nullptr, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string value;
  if (!parseBridgeResult(result, value, errorMessage)) {
    return false;
  }
  const std::size_t separator = value.find('\n');
  if (separator == std::string::npos) {
    errorMessage = "Android folder picker returned invalid data.";
    return false;
  }
  treeUri = value.substr(0, separator);
  const std::size_t secondSeparator = value.find('\n', separator + 1);
  const std::string displayName =
      secondSeparator == std::string::npos
          ? value.substr(separator + 1)
          : value.substr(separator + 1, secondSeparator - separator - 1);
  const std::string directPath =
      secondSeparator == std::string::npos ? std::string()
                                           : value.substr(secondSeparator + 1);
  if (!directPath.empty()) {
    rootPath = std::filesystem::path(directPath).lexically_normal();
    treeUri.clear();
    return true;
  }
  rootPath = makeAndroidTreeRootPath(treeUri, displayName);
  RegisterAndroidChartFolder(rootPath, treeUri);
  return true;
}

bool PickAndroidArchiveForImport(std::filesystem::path &archivePath,
                                 std::string &errorMessage) {
  archivePath.clear();
  RequestAndroidExternalActivityRenderPause();
  struct ExternalActivityPauseReset {
    ~ExternalActivityPauseReset() { FinishAndroidExternalActivityRenderPause(); }
  } externalActivityPauseReset;

  std::string callError;
  const std::string result = callActivityStringMethod(
      "pickArchiveForImport", "()Ljava/lang/String;", nullptr, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string value;
  if (!parseBridgeResult(result, value, errorMessage)) {
    return false;
  }
  if (value == kPendingImportResult) {
    archivePath.clear();
    return true;
  }
  archivePath = std::filesystem::path(value).lexically_normal();
  return true;
}

bool PickAndroidFolderForImport(std::filesystem::path &folderPath,
                                std::string &errorMessage) {
  folderPath.clear();
  RequestAndroidExternalActivityRenderPause();
  struct ExternalActivityPauseReset {
    ~ExternalActivityPauseReset() { FinishAndroidExternalActivityRenderPause(); }
  } externalActivityPauseReset;

  std::string callError;
  const std::string result = callActivityStringMethod(
      "pickFolderForImport", "()Ljava/lang/String;", nullptr, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string value;
  if (!parseBridgeResult(result, value, errorMessage)) {
    return false;
  }
  if (value == kPendingImportResult) {
    folderPath.clear();
    return true;
  }
  folderPath = std::filesystem::path(value).lexically_normal();
  return true;
}

std::optional<std::filesystem::path>
ConsumePendingAndroidArchiveImport(std::string &errorMessage) {
  errorMessage.clear();
  std::string callError;
  const std::string result = callActivityStringMethod(
      "consumePendingArchiveImport", "()Ljava/lang/String;", nullptr,
      callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return std::nullopt;
  }
  if (result.empty()) {
    return std::nullopt;
  }
  if (result.rfind(kErrorPrefix, 0) == 0) {
    errorMessage = result.substr(std::char_traits<char>::length(kErrorPrefix));
    return std::nullopt;
  }
  return std::filesystem::path(result).lexically_normal();
}

void RegisterAndroidChartFolder(const std::filesystem::path &rootPath,
                                const std::string &treeUri) {
  std::string treeId;
  std::filesystem::path relativePath;
  if (!splitAndroidTreePath(rootPath, treeId, relativePath) ||
      !relativePath.empty() || treeUri.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(gAndroidTreeMutex);
  gTreeUrisById[treeId] = treeUri;
}

bool IsAndroidTreePath(const std::filesystem::path &path) {
  std::string treeId;
  std::filesystem::path relativePath;
  return splitAndroidTreePath(path, treeId, relativePath);
}

bool ExistsAndroidTreeFile(const std::filesystem::path &path,
                           std::string &errorMessage) {
  std::string treeId;
  std::filesystem::path relativePath;
  if (!splitAndroidTreePath(path, treeId, relativePath) ||
      relativePath.empty()) {
    errorMessage = "Android file path is invalid.";
    return false;
  }
  const auto treeUri = treeUriForId(treeId);
  if (!treeUri.has_value()) {
    errorMessage = "Android file permission is not registered.";
    return false;
  }

  std::string callError;
  const std::string result = callActivityStringMethod2(
      "existsTreeFile", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
      treeUri->c_str(), relativePath.generic_string().c_str(), callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string value;
  if (!parseBridgeResult(result, value, errorMessage)) {
    return false;
  }
  return value == "1";
}

bool ListAndroidTreeChartFiles(const std::filesystem::path &rootPath,
                               std::vector<std::filesystem::path> &chartPaths,
                               std::string &errorMessage,
                               const std::stop_token *stopToken) {
  chartPaths.clear();
  std::string treeId;
  std::filesystem::path relativePath;
  if (!splitAndroidTreePath(rootPath, treeId, relativePath) ||
      !relativePath.empty()) {
    errorMessage = "Android chart folder path is invalid.";
    return false;
  }
  const auto treeUri = treeUriForId(treeId);
  if (!treeUri.has_value()) {
    errorMessage = "Android chart folder permission is not registered.";
    return false;
  }
  if (stopToken != nullptr && stopToken->stop_requested()) {
    errorMessage = "Scan cancelled.";
    return false;
  }

  std::string callError;
  const std::string result = callActivityStringMethod2(
      "listChartFiles", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
      treeUri->c_str(), rootPath.generic_string().c_str(), callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string value;
  if (!parseBridgeResult(result, value, errorMessage)) {
    return false;
  }
  for (const auto &line : splitLines(value)) {
    if (stopToken != nullptr && stopToken->stop_requested()) {
      errorMessage = "Scan cancelled.";
      return false;
    }
    chartPaths.emplace_back(line);
  }
  return true;
}

bool ClearAndroidTreeTransientFileCache(std::string &errorMessage) {
  std::string callError;
  const std::string result = callActivityStringMethod(
      "clearTransientTreeFileCache", "()Ljava/lang/String;", nullptr,
      callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string value;
  return parseBridgeResult(result, value, errorMessage);
}

bool CacheAndroidTreeDirectory(const std::filesystem::path &directoryPath,
                               std::string &errorMessage) {
  std::string treeId;
  std::filesystem::path relativePath;
  if (!splitAndroidTreePath(directoryPath, treeId, relativePath)) {
    errorMessage = "Android directory path is invalid.";
    return false;
  }
  const auto treeUri = treeUriForId(treeId);
  if (!treeUri.has_value()) {
    errorMessage = "Android directory permission is not registered.";
    return false;
  }

  std::string callError;
  const std::string result = callActivityStringMethod2(
      "cacheTreeDirectory", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
      treeUri->c_str(), relativePath.generic_string().c_str(), callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

bool ReadAndroidTreeFile(const std::filesystem::path &path,
                         std::vector<unsigned char> &bytes,
                         std::string &errorMessage) {
  bytes.clear();
  const auto fd = OpenAndroidTreeFileDescriptor(path, errorMessage);
  if (!fd.has_value()) {
    return false;
  }

  UniqueFd fdGuard(*fd);
  std::array<unsigned char, 64 * 1024> buffer{};
  while (true) {
    const ssize_t readCount = read(fdGuard.value, buffer.data(), buffer.size());
    if (readCount > 0) {
      bytes.insert(bytes.end(), buffer.begin(),
                   buffer.begin() + static_cast<std::ptrdiff_t>(readCount));
      continue;
    }
    if (readCount == 0) {
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    errorMessage = "Android file descriptor read failed.";
    bytes.clear();
    return false;
  }
}

std::optional<int> OpenAndroidTreeFileDescriptor(const std::filesystem::path &path,
                                                 std::string &errorMessage) {
  std::string treeId;
  std::filesystem::path relativePath;
  if (!splitAndroidTreePath(path, treeId, relativePath) ||
      relativePath.empty()) {
    errorMessage = "Android file path is invalid.";
    return std::nullopt;
  }
  const auto treeUri = treeUriForId(treeId);
  if (!treeUri.has_value()) {
    errorMessage = "Android file permission is not registered.";
    return std::nullopt;
  }

  std::string callError;
  const auto fd = callActivityIntMethod2(
      "openTreeFileDescriptor", "(Ljava/lang/String;Ljava/lang/String;)I",
      treeUri->c_str(), relativePath.generic_string().c_str(), callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return std::nullopt;
  }
  if (!fd.has_value() || *fd < 0) {
    errorMessage = "Android file descriptor open failed.";
    return std::nullopt;
  }
  return fd;
}

bool OpenURLInAndroidBrowser(const std::string &url,
                             std::string &errorMessage) {
  std::string callError;
  const std::string result =
      callActivityStringMethod("openExternalUrl", "(Ljava/lang/String;)"
                                                  "Ljava/lang/String;",
                               url.c_str(), callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

bool DownloadURLTextAndroid(const std::string &url, std::string &body,
                            std::string &errorMessage) {
  body.clear();
  std::string callError;
  const std::string result =
      callActivityStringMethod("downloadUrlText", "(Ljava/lang/String;)"
                                                  "Ljava/lang/String;",
                               url.c_str(), callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  return parseBridgeResult(result, body, errorMessage);
}

bool PostURLTextAndroid(const std::string &url, std::string &body,
                        std::string &errorMessage) {
  body.clear();
  std::string callError;
  const std::string result =
      callActivityStringMethod("postUrlText", "(Ljava/lang/String;)"
                                              "Ljava/lang/String;",
                               url.c_str(), callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  return parseBridgeResult(result, body, errorMessage);
}

bool DownloadURLToFileAndroid(const std::string &url,
                              const std::filesystem::path &path,
                              std::atomic_bool &cancelled,
                              AndroidDownloadProgressCallback progressCallback,
                              std::string &errorMessage) {
  AndroidDownloadProgressBridge bridge{.cancelled = &cancelled,
                                       .progressCallback = &progressCallback};
  const jlong progressToken = registerAndroidDownloadProgressBridge(bridge);
  struct ProgressBridgeCleanup {
    jlong token;
    ~ProgressBridgeCleanup() {
      unregisterAndroidDownloadProgressBridge(token);
    }
  } cleanup{progressToken};

  std::string callError;
  const std::string pathText = path.string();
  const std::string result = callActivityStringMethod2Long(
      "downloadUrlToFile",
      "(Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;", url.c_str(),
      pathText.c_str(), progressToken, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

bool LoadAndroidNativeMusicFile(const std::string &filePath,
                                const AndroidNativeMusicMetadata &metadata,
                                std::string &errorMessage) {
  const std::string payload = musicMetadataPayload(metadata);
  std::string callError;
  const std::string result = callActivityStringMethod2Long(
      "loadNativeMusic",
      "(Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;",
      filePath.c_str(), payload.c_str(), metadata.durationMicros, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

bool PlayAndroidNativeMusic(std::string &errorMessage) {
  std::string callError;
  const std::string result =
      callActivityStringMethod("playNativeMusic", "()Ljava/lang/String;",
                               nullptr, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

bool PauseAndroidNativeMusic(std::string &errorMessage) {
  std::string callError;
  const std::string result =
      callActivityStringMethod("pauseNativeMusic", "()Ljava/lang/String;",
                               nullptr, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

bool StopAndroidNativeMusic(std::string &errorMessage) {
  std::string callError;
  const std::string result =
      callActivityStringMethod("stopNativeMusic", "()Ljava/lang/String;",
                               nullptr, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

bool SeekAndroidNativeMusic(long long positionMicros,
                            std::string &errorMessage) {
  std::string callError;
  const std::string positionText = std::to_string(std::max(0LL, positionMicros));
  const std::string result =
      callActivityStringMethod("seekNativeMusic", "(Ljava/lang/String;)"
                                                  "Ljava/lang/String;",
                               positionText.c_str(), callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

AndroidNativeMusicState GetAndroidNativeMusicState() {
  AndroidNativeMusicState state;
  std::string callError;
  const std::string result =
      callActivityStringMethod("nativeMusicState", "()Ljava/lang/String;",
                               nullptr, callError);
  if (!callError.empty() || result.rfind(kErrorPrefix, 0) == 0) {
    return state;
  }

  const std::vector<std::string> lines = splitLines(result);
  if (lines.size() < 4) {
    return state;
  }
  state.loaded = lines[0] == "1";
  state.playing = lines[1] == "1";
  state.positionMicros = parseLongLongOrZero(lines[2]);
  state.durationMicros = parseLongLongOrZero(lines[3]);
  return state;
}

void RequestAndroidExternalActivityRenderPause() {
  {
    std::lock_guard<std::mutex> lock(gExternalActivityPauseMutex);
    gExternalActivityPauseRequested = true;
    gExternalActivityPauseAcknowledged = false;
  }

  SDL_Event event{};
  event.type = SDL_USEREVENT;
  event.user.code = kExternalActivityPauseWakeCode;
  SDL_PushEvent(&event);

  std::unique_lock<std::mutex> lock(gExternalActivityPauseMutex);
  if (!gExternalActivityPauseCv.wait_for(lock, std::chrono::seconds(2), [] {
        return gExternalActivityPauseAcknowledged;
      })) {
    SDL_Log("Timed out waiting for Android render pause before external activity");
  }
}

void FinishAndroidExternalActivityRenderPause() {
  {
    std::lock_guard<std::mutex> lock(gExternalActivityPauseMutex);
    gExternalActivityPauseRequested = false;
  }

  SDL_Event event{};
  event.type = SDL_USEREVENT;
  event.user.code = kExternalActivityPauseWakeCode;
  SDL_PushEvent(&event);
  gExternalActivityPauseCv.notify_all();
}

bool IsAndroidExternalActivityRenderPauseRequested() {
  std::lock_guard<std::mutex> lock(gExternalActivityPauseMutex);
  return gExternalActivityPauseRequested;
}

void NotifyAndroidExternalActivityRenderPaused() {
  {
    std::lock_guard<std::mutex> lock(gExternalActivityPauseMutex);
    if (!gExternalActivityPauseRequested) {
      return;
    }
    gExternalActivityPauseAcknowledged = true;
  }
  gExternalActivityPauseCv.notify_all();
}

#endif
