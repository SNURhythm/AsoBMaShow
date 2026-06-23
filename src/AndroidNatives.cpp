#include "targets.h"
#include "AndroidNatives.h"

#if TARGET_OS_ANDROID

#include <SDL2/SDL_events.h>
#include <SDL2/SDL_log.h>
#include <SDL2/SDL_system.h>
#include <algorithm>
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

namespace {

constexpr const char *kAndroidTreeSentinel = "@androidtree@";
constexpr const char *kErrorPrefix = "__ERROR__:";

std::mutex gAndroidTreeMutex;
std::unordered_map<std::string, std::string> gTreeUrisById;
std::mutex gExternalActivityPauseMutex;
std::condition_variable gExternalActivityPauseCv;
bool gExternalActivityPauseRequested = false;
bool gExternalActivityPauseAcknowledged = false;
constexpr Sint32 kExternalActivityPauseWakeCode = 0x41535050;

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

bool callActivityByteArrayMethod2(const char *methodName,
                                  const char *signature,
                                  const char *argument1,
                                  const char *argument2,
                                  std::vector<unsigned char> &bytes,
                                  std::string &errorMessage) {
  errorMessage.clear();
  bytes.clear();
  auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
  auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
  if (env == nullptr || activity == nullptr) {
    errorMessage = "Android activity is not available.";
    return false;
  }

  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass == nullptr) {
    errorMessage = "Android activity class is not available.";
    env->DeleteLocalRef(activity);
    return false;
  }

  jmethodID method = env->GetMethodID(activityClass, methodName, signature);
  if (method == nullptr) {
    errorMessage = std::string("Android activity method missing: ") +
                   methodName;
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return false;
  }

  jstring javaArgument1 = env->NewStringUTF(argument1 != nullptr ? argument1 : "");
  jstring javaArgument2 = env->NewStringUTF(argument2 != nullptr ? argument2 : "");
  auto javaResult = static_cast<jbyteArray>(
      env->CallObjectMethod(activity, method, javaArgument1, javaArgument2));

  if (clearPendingJavaException(env, errorMessage)) {
    env->DeleteLocalRef(javaArgument1);
    env->DeleteLocalRef(javaArgument2);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return false;
  }

  if (javaResult == nullptr) {
    errorMessage = "Android file read failed.";
    env->DeleteLocalRef(javaArgument1);
    env->DeleteLocalRef(javaArgument2);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return false;
  }

  const jsize length = env->GetArrayLength(javaResult);
  if (length < 0) {
    errorMessage = "Android file read returned invalid data.";
    env->DeleteLocalRef(javaResult);
    env->DeleteLocalRef(javaArgument1);
    env->DeleteLocalRef(javaArgument2);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return false;
  }
  bytes.resize(static_cast<std::size_t>(length));
  if (length > 0) {
    env->GetByteArrayRegion(
        javaResult, 0, length, reinterpret_cast<jbyte *>(bytes.data()));
  }

  env->DeleteLocalRef(javaResult);
  env->DeleteLocalRef(javaArgument1);
  env->DeleteLocalRef(javaArgument2);
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return true;
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

} // namespace

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

bool PickAndroidChartFolder(std::filesystem::path &rootPath,
                            std::string &treeUri,
                            std::string &errorMessage) {
  rootPath.clear();
  treeUri.clear();
  RequestAndroidExternalActivityRenderPause();
  struct ExternalActivityPauseReset {
    ~ExternalActivityPauseReset() { FinishAndroidExternalActivityRenderPause(); }
  } externalActivityPauseReset;

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
  const std::string displayName = value.substr(separator + 1);
  rootPath = makeAndroidTreeRootPath(treeUri, displayName);
  RegisterAndroidChartFolder(rootPath, treeUri);
  return true;
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

bool ReadAndroidTreeFile(const std::filesystem::path &path,
                         std::vector<unsigned char> &bytes,
                         std::string &errorMessage) {
  bytes.clear();
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
  return callActivityByteArrayMethod2(
      "readTreeFile", "(Ljava/lang/String;Ljava/lang/String;)[B",
      treeUri->c_str(), relativePath.generic_string().c_str(), bytes,
      errorMessage);
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
