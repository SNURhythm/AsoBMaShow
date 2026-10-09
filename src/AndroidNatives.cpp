#include "targets.h"
#include "AndroidNatives.h"

#if TARGET_OS_ANDROID

#include "StableHash.h"
#include "ArchiveFile.h"
#include "archive/ArchiveReferenceRegistry.h"
#include "i18n/Localization.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <thread>
#include "PlatformDocumentHandoff.h"
#include "skin/package/SkinDirectoryRename.h"
#include "audio/NativeMusicPlayer.h"
#include "library/ChartLibraryTaskService.h"
#include "library/DocumentsLibraryRefresh.h"
#include "repositories/ChartRepository.h"
#include "platform/ScreenOrientation.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <jni.h>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <unistd.h>

namespace {

constexpr const char *kAndroidTreeSentinel = "@androidtree@";
constexpr const char *kErrorPrefix = "__ERROR__:";
constexpr const char *kSuccessResult = "__OK__";
constexpr const char *kPendingImportResult = "__PENDING_ARCHIVE_IMPORT__";

std::mutex gAndroidTreeMutex;
std::unordered_map<std::string, std::string> gTreeUrisById;
std::mutex gExternalActivityPauseMutex;
std::condition_variable gExternalActivityPauseCv;
bool gExternalActivityPauseRequested = false;
bool gExternalActivityPauseAcknowledged = false;
constexpr Sint32 kExternalActivityPauseWakeCode = 0x41535050;
std::mutex gAndroidDocumentCommitMutex;
std::unordered_map<std::string, std::function<bool()>>
    gAndroidDocumentCommitHandlers;
struct DirectoryImportProgressBridge {
  std::shared_ptr<PlatformDirectoryImportProgress> progress;
  bool renderPaused = true;
};
std::mutex gDirectoryImportProgressMutex;
std::unordered_map<std::string, DirectoryImportProgressBridge> gDirectoryImportProgress;
std::mutex gAndroidImportTasksMutex;
chart_library_tasks::ChartLibraryTaskService *gAndroidImportTasks = nullptr;

std::string pathToUtf8(const std::filesystem::path &path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

struct AndroidDownloadProgressBridge {
  std::atomic_bool *cancelled = nullptr;
  AndroidDownloadProgressCallback *progressCallback = nullptr;
  AndroidDownloadCheckpoint *checkpoint = nullptr;
  AndroidDownloadPauseProbe *pauseRequested = nullptr;
};

std::mutex gAndroidDownloadProgressMutex;
std::unordered_map<jlong, AndroidDownloadProgressBridge *>
    gAndroidDownloadProgressBridges;
jlong gNextAndroidDownloadProgressToken = 1;

std::mutex gAndroidFolderPickerMutex;
std::unordered_map<std::string, std::stop_token> gAndroidFolderPickerStops;
std::uint64_t gNextAndroidFolderPickerToken = 1;

struct AndroidFolderPickerCancellation {
  explicit AndroidFolderPickerCancellation(std::stop_token stopToken) {
    std::lock_guard lock(gAndroidFolderPickerMutex);
    token = std::to_string(gNextAndroidFolderPickerToken++);
    gAndroidFolderPickerStops.emplace(token, stopToken);
  }

  ~AndroidFolderPickerCancellation() {
    std::lock_guard lock(gAndroidFolderPickerMutex);
    gAndroidFolderPickerStops.erase(token);
  }

  AndroidFolderPickerCancellation(const AndroidFolderPickerCancellation &) = delete;
  AndroidFolderPickerCancellation &operator=(const AndroidFolderPickerCancellation &) = delete;

  std::string token;
};

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
  return std::filesystem::path(kAndroidTreeSentinel) /
         stable_hash::hex64(stable_hash::fnv1a64(treeUri)) /
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

void appendUtf8(std::string &output, std::uint32_t codePoint) {
  if (codePoint <= 0x7f) {
    output.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  } else if (codePoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  }
}

std::string jstringToUtf8(JNIEnv *env, jstring value) {
  if (env == nullptr || value == nullptr) {
    return {};
  }
  const jsize length = env->GetStringLength(value);
  const jchar *chars = env->GetStringChars(value, nullptr);
  if (chars == nullptr) {
    return {};
  }
  std::string result;
  result.reserve(static_cast<std::size_t>(length) * 3);
  for (jsize index = 0; index < length; ++index) {
    std::uint32_t codePoint = chars[index];
    if (codePoint >= 0xd800 && codePoint <= 0xdbff) {
      if (index + 1 < length && chars[index + 1] >= 0xdc00 &&
          chars[index + 1] <= 0xdfff) {
        codePoint =
            0x10000 + ((codePoint - 0xd800) << 10) + (chars[++index] - 0xdc00);
      } else {
        codePoint = 0xfffd;
      }
    } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
      codePoint = 0xfffd;
    }
    appendUtf8(result, codePoint);
  }
  env->ReleaseStringChars(value, chars);
  return result;
}

jstring utf8ToJString(JNIEnv *env, const char *value) {
  if (env == nullptr) {
    return nullptr;
  }
  const auto *bytes =
      reinterpret_cast<const unsigned char *>(value != nullptr ? value : "");
  const std::size_t length =
      std::char_traits<char>::length(value != nullptr ? value : "");
  std::vector<jchar> utf16;
  utf16.reserve(length);
  for (std::size_t index = 0; index < length;) {
    std::uint32_t codePoint = 0xfffd;
    std::size_t count = 1;
    const unsigned char first = bytes[index];
    if (first <= 0x7f) {
      codePoint = first;
    } else if (first >= 0xc2 && first <= 0xdf && index + 1 < length &&
               (bytes[index + 1] & 0xc0) == 0x80) {
      codePoint = ((first & 0x1f) << 6) | (bytes[index + 1] & 0x3f);
      count = 2;
    } else if (first >= 0xe0 && first <= 0xef && index + 2 < length &&
               (bytes[index + 1] & 0xc0) == 0x80 &&
               (bytes[index + 2] & 0xc0) == 0x80) {
      const std::uint32_t candidate = ((first & 0x0f) << 12) |
                                      ((bytes[index + 1] & 0x3f) << 6) |
                                      (bytes[index + 2] & 0x3f);
      if (candidate >= 0x800 && !(candidate >= 0xd800 && candidate <= 0xdfff)) {
        codePoint = candidate;
        count = 3;
      }
    } else if (first >= 0xf0 && first <= 0xf4 && index + 3 < length &&
               (bytes[index + 1] & 0xc0) == 0x80 &&
               (bytes[index + 2] & 0xc0) == 0x80 &&
               (bytes[index + 3] & 0xc0) == 0x80) {
      const std::uint32_t candidate =
          ((first & 0x07) << 18) | ((bytes[index + 1] & 0x3f) << 12) |
          ((bytes[index + 2] & 0x3f) << 6) | (bytes[index + 3] & 0x3f);
      if (candidate >= 0x10000 && candidate <= 0x10ffff) {
        codePoint = candidate;
        count = 4;
      }
    }
    index += count;
    if (codePoint <= 0xffff) {
      utf16.push_back(static_cast<jchar>(codePoint));
    } else {
      codePoint -= 0x10000;
      utf16.push_back(static_cast<jchar>(0xd800 + (codePoint >> 10)));
      utf16.push_back(static_cast<jchar>(0xdc00 + (codePoint & 0x3ff)));
    }
  }
  static constexpr jchar empty = 0;
  return env->NewString(utf16.empty() ? &empty : utf16.data(),
                        static_cast<jsize>(utf16.size()));
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
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
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
    javaArgument = utf8ToJString(env, argument);
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
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
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
      utf8ToJString(env, argument1 != nullptr ? argument1 : "");
  jstring javaArgument2 =
      utf8ToJString(env, argument2 != nullptr ? argument2 : "");
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
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
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
      utf8ToJString(env, argument1 != nullptr ? argument1 : "");
  jstring javaArgument2 =
      utf8ToJString(env, argument2 != nullptr ? argument2 : "");
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

std::string callActivityStringMethodLong(const char *methodName,
                                         const char *signature,
                                         const char *argument,
                                         jlong longArgument,
                                         std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
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

  jstring javaArgument =
      utf8ToJString(env, argument != nullptr ? argument : "");
  jobject javaResult =
      env->CallObjectMethod(activity, method, javaArgument, longArgument);

  if (clearPendingJavaException(env, errorMessage)) {
    env->DeleteLocalRef(javaArgument);
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }

  std::string result = jstringToUtf8(env, static_cast<jstring>(javaResult));
  if (javaResult != nullptr) {
    env->DeleteLocalRef(javaResult);
  }
  env->DeleteLocalRef(javaArgument);
  env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  return result;
}

std::string
callActivityStringMethod4Long(const char *methodName, const char *signature,
                              const char *argument1, const char *argument2,
                              const char *argument3, const char *argument4,
                              jlong argument5, std::string &errorMessage) {
  errorMessage.clear();
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
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
    errorMessage =
        std::string("Android activity method missing: ") + methodName;
    env->DeleteLocalRef(activityClass);
    env->DeleteLocalRef(activity);
    return {};
  }

  jstring javaArguments[] = {
      utf8ToJString(env, argument1 != nullptr ? argument1 : ""),
      utf8ToJString(env, argument2 != nullptr ? argument2 : ""),
      utf8ToJString(env, argument3 != nullptr ? argument3 : ""),
      utf8ToJString(env, argument4 != nullptr ? argument4 : ""),
  };
  jobject javaResult = env->CallObjectMethod(activity, method, javaArguments[0],
                                             javaArguments[1], javaArguments[2],
                                             javaArguments[3], argument5);
  const bool exception = clearPendingJavaException(env, errorMessage);
  std::string result;
  if (!exception) {
    result = jstringToUtf8(env, static_cast<jstring>(javaResult));
  }
  if (javaResult != nullptr) {
    env->DeleteLocalRef(javaResult);
  }
  for (jstring argument : javaArguments) {
    if (argument != nullptr) {
      env->DeleteLocalRef(argument);
    }
  }
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
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
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

  jstring javaArgument1 =
      utf8ToJString(env, argument1 != nullptr ? argument1 : "");
  jstring javaArgument2 =
      utf8ToJString(env, argument2 != nullptr ? argument2 : "");
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

std::string sanitizeMusicPayloadField(std::string value) {
  std::replace(value.begin(), value.end(), '\n', ' ');
  std::replace(value.begin(), value.end(), '\r', ' ');
  std::replace(value.begin(), value.end(), '\t', ' ');
  return value;
}

std::string musicMetadataPayload(const AndroidNativeMusicMetadata &metadata) {
  return sanitizeMusicPayloadField(metadata.title) + "\n" +
         sanitizeMusicPayloadField(metadata.artist) + "\n" +
         sanitizeMusicPayloadField(metadata.album) + "\n" +
         sanitizeMusicPayloadField(metadata.artworkPath);
}

std::string musicQueuePayload(const AndroidNativeMusicQueue &queue) {
  std::ostringstream stream;
  for (std::size_t i = 0; i < queue.items.size(); ++i) {
    if (i > 0) {
      stream << '\n';
    }
    const auto &item = queue.items[i];
    stream << item.itemId << '\t'
           << sanitizeMusicPayloadField(item.metadata.title) << '\t'
           << sanitizeMusicPayloadField(item.metadata.artist) << '\t'
           << sanitizeMusicPayloadField(item.metadata.album) << '\t'
           << sanitizeMusicPayloadField(item.metadata.artworkPath) << '\t'
           << std::max(0LL, item.metadata.durationMicros);
  }
  return stream.str();
}

long long parseLongLongOrZero(const std::string &value) {
  try {
    return std::stoll(value);
  } catch (...) {
    return 0;
  }
}

} // namespace

void screen_orientation::apply(Mode mode, bool lockCurrent) {
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  if (env == nullptr) return;
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (activity == nullptr) return;
  jclass activityClass = env->GetObjectClass(activity);
  if (activityClass != nullptr) {
    jmethodID method = env->GetMethodID(activityClass, "setScreenOrientation", "(IZ)V");
    if (method != nullptr) {
      env->CallVoidMethod(activity, method, static_cast<jint>(mode),
                          lockCurrent ? JNI_TRUE : JNI_FALSE);
    }
    env->DeleteLocalRef(activityClass);
  }
  if (env->ExceptionCheck()) {
    env->ExceptionClear();
    SDL_Log("Could not apply Android screen orientation");
  }
  env->DeleteLocalRef(activity);
}

namespace {
std::mutex androidArchiveLifecycleMutex;
archive_source::Registry &androidArchiveSources() {
  static archive_source::Registry sources([](const std::string &uri) -> archive_source::Access {
    std::string error;
    const auto descriptor = callActivityIntMethod2("openArchiveFileDescriptor",
        "(Ljava/lang/String;Ljava/lang/String;)I", uri.c_str(), "", error);
    if (!descriptor || *descriptor < 0) return {{}, {}, error.empty() ? "Could not open archive source." : error};
    auto owner = std::shared_ptr<int>(new int(*descriptor), [](int *fd) { ::close(*fd); delete fd; });
    struct stat state{};
    if (::fstat(*descriptor, &state) != 0 || !S_ISREG(state.st_mode) ||
        ::lseek(*descriptor, 0, SEEK_SET) < 0) {
      return {{}, {}, "__STREAM_ONLY__:This provider does not support direct archive reads."};
    }
    const auto path = std::filesystem::path("/proc/self/fd") / std::to_string(*descriptor);
    // A fresh open, unlike dup(), owns an independent offset for every decoder.
    const int probe = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (probe < 0) return {{}, {}, "__STREAM_ONLY__:This provider cannot reopen archive readers."};
    ::close(probe);
    return {path, std::move(owner), {}};
  });
  return sources;
}
}

void RegisterAndroidArchiveReference(const std::filesystem::path &path,
                                     const std::string &uri, bool refresh) {
  std::lock_guard lock(androidArchiveLifecycleMutex);
  androidArchiveSources().registerSource(path, uri);
  if (refresh) androidArchiveSources().invalidate(path);
  archive_source::setResolver([](const auto &source) { return androidArchiveSources().acquire(source); });
}

void DiscardAndroidArchiveReference(const std::filesystem::path &path,
                                    const std::string &newGrantUri) {
  std::lock_guard lock(androidArchiveLifecycleMutex);
  const auto removedUri = androidArchiveSources().remove(path);
  const auto &uri = removedUri.empty() ? newGrantUri : removedUri;
  if (!uri.empty() && !androidArchiveSources().referencesUri(uri)) {
    std::string error;
    callActivityStringMethod("releaseArchivePermission", "(Ljava/lang/String;)Ljava/lang/String;",
                             uri.c_str(), error);
  }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeInspectArchiveImport(
    JNIEnv *env, jclass, jstring tokenValue, jstring pathValue, jstring uriValue) {
  try {
    const auto token = jstringToUtf8(env, tokenValue);
    const auto path = std::filesystem::path(jstringToUtf8(env, pathValue));
    const auto uri = jstringToUtf8(env, uriValue);
    if (!uri.empty()) {
      if (!archive_source::validReference(path)) return utf8ToJString(env, "__ERROR__:Invalid archive reference.");
      RegisterAndroidArchiveReference(path, uri);
    }
    const auto source = archive_source::resolve(path);
    if (!source) return utf8ToJString(env, (source.error.starts_with("__STREAM_ONLY__:")
        ? source.error : "__ERROR__:" + source.error).c_str());
    const auto checkpoint = [&] {
      for (;;) {
        int state;
        {
          std::lock_guard lock(gAndroidImportTasksMutex);
          state = gAndroidImportTasks ? gAndroidImportTasks->androidImportCopyState(token) : -1;
        }
        if (state != 0) return state > 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    };
    std::vector<archive_file::Entry> entries;
    std::string error;
    if (!archive_file::listEntriesBounded(path, entries, 1'000'000, &error, checkpoint))
      return utf8ToJString(env, ("__ERROR__:" + error).c_str());
    const bool solid = std::any_of(entries.begin(), entries.end(), [](const auto &entry) { return !entry.directory && entry.solid; });
    return utf8ToJString(env, solid ? "solid" : "non-solid");
  } catch (const std::exception &error) {
    return utf8ToJString(env, (std::string("__ERROR__:") + error.what()).c_str());
  } catch (...) {
    return utf8ToJString(env, "__ERROR__:Could not inspect archive.");
  }
}

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeDiscardArchiveImport(
    JNIEnv *env, jclass, jstring path, jstring grantUri) {
  try { DiscardAndroidArchiveReference(jstringToUtf8(env, path), jstringToUtf8(env, grantUri)); }
  catch (...) { SDL_Log("Could not retire archive import source"); }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeArchiveImportText(
    JNIEnv *env, jclass, jstring key) {
  return utf8ToJString(env, i18n::tr(jstringToUtf8(env, key)).c_str());
}

void RegisterAndroidImportTasks(chart_library_tasks::ChartLibraryTaskService &tasks) {
  std::lock_guard lock(gAndroidImportTasksMutex);
  gAndroidImportTasks = &tasks;
}

void UnregisterAndroidImportTasks(chart_library_tasks::ChartLibraryTaskService &tasks) {
  std::lock_guard lock(gAndroidImportTasksMutex);
  if (gAndroidImportTasks == &tasks) {
    tasks.cancelAndroidImports();
    gAndroidImportTasks = nullptr;
  }
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeRefreshDocumentsLibrary(
    JNIEnv *, jclass) {
  std::lock_guard lock(gAndroidImportTasksMutex);
  if (gAndroidImportTasks == nullptr) return 0;
  try {
    const auto id = chart_library_tasks::enqueueDocumentsLibraryRefresh(
        *gAndroidImportTasks, ChartRepository::DefaultBmsFolderPath());
    if (id != 0) {
      SDL_Log("Android Documents library refresh queued: %llu", static_cast<unsigned long long>(id));
    }
    return static_cast<jlong>(id);
  } catch (...) {
    return 0;
  }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeDocumentsRefreshStatus(
    JNIEnv *, jclass, jlong taskId) {
  std::lock_guard lock(gAndroidImportTasksMutex);
  if (gAndroidImportTasks == nullptr) return -1;
  try {
    for (const auto &task : gAndroidImportTasks->snapshot().tasks) {
      if (task.id != static_cast<std::uint64_t>(taskId)) continue;
      if (task.status == chart_library_tasks::TaskStatus::Complete) return 1;
      if (task.status == chart_library_tasks::TaskStatus::Failed) return -1;
      return 0;
    }
  } catch (...) {
  }
  return -1;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeBeginChartImport(
    JNIEnv *env, jclass, jstring token, jboolean folder, jboolean moveSource) {
  std::lock_guard lock(gAndroidImportTasksMutex);
  if (gAndroidImportTasks == nullptr) {
    return 0;
  }
  return gAndroidImportTasks->beginAndroidImport(jstringToUtf8(env, token),
                                                folder == JNI_TRUE, moveSource == JNI_TRUE) ? 1 : -1;
}

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeChartImportProgress(
    JNIEnv *env, jclass, jstring token, jint files, jint totalFiles,
    jlong bytes, jlong totalBytes, jstring name, jint phase) {
  std::lock_guard lock(gAndroidImportTasksMutex);
  if (gAndroidImportTasks == nullptr) return;
  try {
    gAndroidImportTasks->updateAndroidImportProgress(jstringToUtf8(env, token),
        files, totalFiles, bytes > 0 ? static_cast<std::uint64_t>(bytes) : 0,
        totalBytes, jstringToUtf8(env, name), phase);
  } catch (...) {
    SDL_Log("Could not publish Android import progress");
  }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeChartImportCopyState(
    JNIEnv *env, jclass, jstring token) {
  std::lock_guard lock(gAndroidImportTasksMutex);
  return gAndroidImportTasks == nullptr
             ? -1
             : gAndroidImportTasks->androidImportCopyState(jstringToUtf8(env, token));
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeFinishChartImport(
    JNIEnv *env, jclass, jstring token, jboolean folder, jstring path,
    jstring error, jstring retainedError, jstring archiveUri, jboolean newGrant) {
  try {
    const auto sourcePath = std::filesystem::path(jstringToUtf8(env, path));
    const auto uri = jstringToUtf8(env, archiveUri);
    std::shared_ptr<chart_library_tasks::AndroidArchiveImportOwner> owner;
    if (folder != JNI_TRUE && archive_source::isReference(sourcePath)) {
      owner = std::make_shared<chart_library_tasks::AndroidArchiveImportOwner>();
      owner->cleanup = [sourcePath, grant = newGrant == JNI_TRUE ? uri : std::string{}](bool retained) {
        if (!retained) DiscardAndroidArchiveReference(sourcePath, grant);
      };
    }
    std::lock_guard lock(gAndroidImportTasksMutex);
    return gAndroidImportTasks != nullptr && gAndroidImportTasks->finishAndroidImport(
        jstringToUtf8(env, token), folder == JNI_TRUE, sourcePath,
        jstringToUtf8(env, error), jstringToUtf8(env, retainedError), uri,
        newGrant == JNI_TRUE, std::move(owner)) ? JNI_TRUE : JNI_FALSE;
  } catch (...) { return JNI_FALSE; }
}

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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeChartFolderPickerCancelled(
    JNIEnv *env, jclass, jstring cancellationToken) {
  const std::string token = jstringToUtf8(env, cancellationToken);
  std::lock_guard lock(gAndroidFolderPickerMutex);
  const auto found = gAndroidFolderPickerStops.find(token);
  return found == gAndroidFolderPickerStops.end() || found->second.stop_requested()
             ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeDownloadUrlTextCheckpoint(
    JNIEnv *, jclass, jlong checkpointToken) {
  AndroidDownloadCheckpoint *checkpoint = nullptr;
  if (AndroidDownloadProgressBridge *bridge =
          androidDownloadProgressBridge(checkpointToken);
      bridge != nullptr) {
    checkpoint = bridge->checkpoint;
  }
  return checkpoint == nullptr || !*checkpoint || (*checkpoint)() ? JNI_TRUE
                                                                  : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeDownloadUrlTextPauseRequested(
    JNIEnv *, jclass, jlong checkpointToken) {
  AndroidDownloadPauseProbe *pauseRequested = nullptr;
  if (AndroidDownloadProgressBridge *bridge =
          androidDownloadProgressBridge(checkpointToken);
      bridge != nullptr) {
    pauseRequested = bridge->pauseRequested;
  }
  return pauseRequested != nullptr && *pauseRequested && (*pauseRequested)()
             ? JNI_TRUE
             : JNI_FALSE;
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeCommitDocumentHandoff(
    JNIEnv *env, jclass, jstring operationToken) {
  const std::string token = jstringToUtf8(env, operationToken);
  std::function<bool()> commitHandler;
  {
    std::lock_guard lock(gAndroidDocumentCommitMutex);
    const auto found = gAndroidDocumentCommitHandlers.find(token);
    if (found == gAndroidDocumentCommitHandlers.end()) {
      return JNI_FALSE;
    }
    commitHandler = found->second;
  }
  try {
    return commitHandler && commitHandler() ? JNI_TRUE : JNI_FALSE;
  } catch (...) {
    return JNI_FALSE;
  }
}

std::string GetAndroidExternalFilesDir() {
  if (const char *external = SDL_GetAndroidExternalStoragePath();
      external != nullptr && external[0] != '\0') {
    // SDL returns an absolute external path, which may still contain a system
    // alias. Resolve only this trusted container, before appending Documents.
    std::error_code error;
    const auto container = std::filesystem::canonical(external, error);
    return error ? std::string(external) : container.string();
  }
  if (const char *internal = SDL_GetAndroidInternalStoragePath();
      internal != nullptr && internal[0] != '\0') {
    return internal;
  }
  return ".";
}

std::string GetAndroidInternalFilesDir() {
  if (const char *internal = SDL_GetAndroidInternalStoragePath();
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
  return {};
}

std::string GetAndroidCacheDir() {
  const char *cache = SDL_GetAndroidCachePath();
  if (cache == nullptr || cache[0] == '\0') return {};
  // Preserve the Java bridge's canonical private-container path and fail-closed
  // behavior. SDL caches the Context.getCacheDir() lookup for this process.
  std::error_code error;
  const auto directory = std::filesystem::canonical(cache, error);
  return error ? std::string{} : directory.string();
}

std::string GetAndroidPreferredLanguageTags() {
  std::string callError;
  const std::string result = callActivityStringMethod(
      "getPreferredLanguageTags", "()Ljava/lang/String;", nullptr, callError);
  if (!callError.empty()) {
    auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    if (env != nullptr && env->ExceptionCheck()) env->ExceptionClear();
    return {};
  }
  return result;
}

std::optional<std::string> ConvertAndroidMs932ToUtf8(std::string_view value) {
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  if (env == nullptr ||
      value.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    return std::nullopt;
  }
  jbyteArray bytes = env->NewByteArray(static_cast<jsize>(value.size()));
  jclass stringClass = env->FindClass("java/lang/String");
  jstring charset = env->NewStringUTF("MS932");
  if (bytes == nullptr || stringClass == nullptr || charset == nullptr) {
    env->ExceptionClear();
    if (bytes != nullptr) env->DeleteLocalRef(bytes);
    if (stringClass != nullptr) env->DeleteLocalRef(stringClass);
    if (charset != nullptr) env->DeleteLocalRef(charset);
    return std::nullopt;
  }
  env->SetByteArrayRegion(
      bytes, 0, static_cast<jsize>(value.size()),
      reinterpret_cast<const jbyte *>(value.data()));
  const jmethodID constructor =
      env->GetMethodID(stringClass, "<init>", "([BLjava/lang/String;)V");
  jobject decoded = constructor != nullptr
                        ? env->NewObject(stringClass, constructor, bytes, charset)
                        : nullptr;
  std::optional<std::string> result;
  if (!env->ExceptionCheck() && decoded != nullptr) {
    result = jstringToUtf8(env, static_cast<jstring>(decoded));
  } else {
    env->ExceptionClear();
  }
  if (decoded != nullptr) env->DeleteLocalRef(decoded);
  env->DeleteLocalRef(charset);
  env->DeleteLocalRef(stringClass);
  env->DeleteLocalRef(bytes);
  return result;
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
                            std::string &errorMessage,
                            std::stop_token stopToken) {
  rootPath.clear();
  treeUri.clear();
  if (stopToken.stop_requested()) return false;
  AndroidFolderPickerCancellation cancellation(stopToken);
  RequestAndroidExternalActivityRenderPause();
  struct ExternalActivityPauseReset {
    ~ExternalActivityPauseReset() { FinishAndroidExternalActivityRenderPause(); }
  } externalActivityPauseReset;

  {
    std::string permissionError;
    const std::string permissionResult = callActivityStringMethod(
        "ensureManageExternalStorageAccess", "(Ljava/lang/String;)Ljava/lang/String;",
        cancellation.token.c_str(),
        permissionError);
    if (stopToken.stop_requested() || permissionResult == "__CANCELLED__") {
      return false;
    }
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
      callActivityStringMethod("pickChartFolder", "(Ljava/lang/String;)Ljava/lang/String;",
                               cancellation.token.c_str(), callError);
  if (stopToken.stop_requested() || result == "__CANCELLED__") return false;
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
                                std::string &errorMessage, bool moveSource) {
  folderPath.clear();
  RequestAndroidExternalActivityRenderPause();
  struct ExternalActivityPauseReset {
    ~ExternalActivityPauseReset() { FinishAndroidExternalActivityRenderPause(); }
  } externalActivityPauseReset;

  std::string callError;
  const std::string result = callActivityStringMethod(
      "pickFolderForImport", "(Ljava/lang/String;)Ljava/lang/String;",
      moveSource ? "move" : "copy", callError);
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

bool RegisterAndroidDocumentHandoff(std::uint64_t operationToken,
                                    std::string &errorMessage) {
  const auto tokenText = std::to_string(operationToken);
  const std::string result = callActivityStringMethod(
      "registerDocumentHandoff", "(Ljava/lang/String;)Ljava/lang/String;",
      tokenText.c_str(), errorMessage);
  if (!errorMessage.empty()) {
    return false;
  }
  if (result == kSuccessResult) {
    return true;
  }
  errorMessage =
      result.rfind(kErrorPrefix, 0) == 0
          ? result.substr(std::char_traits<char>::length(kErrorPrefix))
          : "Android could not register the document handoff.";
  return false;
}

void RetireAndroidDocumentHandoff(std::uint64_t operationToken) {
  const auto tokenText = std::to_string(operationToken);
  std::string ignoredError;
  (void)callActivityStringMethod("retireDocumentHandoff",
                                 "(Ljava/lang/String;)Ljava/lang/String;",
                                 tokenText.c_str(), ignoredError);
}

bool RegisterAndroidDocumentCommit(std::uint64_t operationToken,
                                   std::function<bool()> commitHandler) {
  if (!commitHandler) {
    return false;
  }
  std::lock_guard lock(gAndroidDocumentCommitMutex);
  return gAndroidDocumentCommitHandlers
      .emplace(std::to_string(operationToken), std::move(commitHandler))
      .second;
}

void UnregisterAndroidDocumentCommit(std::uint64_t operationToken) {
  std::lock_guard lock(gAndroidDocumentCommitMutex);
  gAndroidDocumentCommitHandlers.erase(std::to_string(operationToken));
}

std::string ImportAndroidDocument(std::uint64_t operationToken,
                                  const std::string &mimeType,
                                  std::uint64_t maxBytes) {
  RequestAndroidExternalActivityRenderPause();
  struct ExternalActivityPauseReset {
    ~ExternalActivityPauseReset() {
      FinishAndroidExternalActivityRenderPause();
    }
  } externalActivityPauseReset;

  std::string callError;
  const auto tokenText = std::to_string(operationToken);
  const std::string result = callActivityStringMethod2Long(
      "importDocument",
      "(Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;",
      tokenText.c_str(), mimeType.c_str(), static_cast<jlong>(maxBytes),
      callError);
  if (!callError.empty()) {
    return std::string(kErrorPrefix) + callError;
  }
  return result.empty() ? std::string(kErrorPrefix) +
                              "Android document import returned no result."
                        : result;
}

namespace skin {
int renameAndroidSkinDirectoryWithMutationLock(int sourceParent,
                                               const char *sourceName,
                                               int destinationParent,
                                               const char *destinationName) noexcept {
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (env == nullptr || activity == nullptr) {
    errno = EIO;
    return -1;
  }
  jclass activityClass = env->GetObjectClass(activity);
  jmethodID method = activityClass == nullptr ? nullptr : env->GetMethodID(
      activityClass, "getDocumentsMutationLock", "()Ljava/lang/Object;");
  jobject monitor = method == nullptr ? nullptr : env->CallObjectMethod(activity, method);
  if (activityClass) env->DeleteLocalRef(activityClass);
  env->DeleteLocalRef(activity);
  if (env->ExceptionCheck() || monitor == nullptr) {
    env->ExceptionClear();
    if (monitor) env->DeleteLocalRef(monitor);
    errno = EIO;
    return -1;
  }
  if (env->MonitorEnter(monitor) != JNI_OK) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(monitor);
    errno = EIO;
    return -1;
  }
  const int result = skinRenameDirectoryUnderMutationLock(
      sourceParent, sourceName, destinationParent, destinationName);
  const int renameError = errno;
  // Keep the retained monitor, even if the Activity is destroyed during rename.
  env->MonitorExit(monitor);
  env->DeleteLocalRef(monitor);
  errno = renameError;
  return result;
}
} // namespace skin

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_nativeSkinDirectoryImportProgress(
    JNIEnv *env, jclass, jstring token, jlong bytes, jlong files) {
  std::lock_guard lock(gDirectoryImportProgressMutex);
  const auto found = gDirectoryImportProgress.find(jstringToUtf8(env, token));
  if (found == gDirectoryImportProgress.end()) return;
  auto &bridge = found->second;
  // The picker has returned. I/O continues on this worker while SDL renders.
  if (bridge.renderPaused) {
    bridge.renderPaused = false;
    FinishAndroidExternalActivityRenderPause();
  }
  if (bridge.progress) {
    bridge.progress->completedBytes.store(std::max<jlong>(0, bytes));
    bridge.progress->completedFiles.store(std::max<jlong>(0, files));
    bridge.progress->started.store(true, std::memory_order_release);
  }
}

std::string ImportAndroidDirectory(std::uint64_t operationToken,
                                   std::uint64_t maxBytes, std::uint64_t maxFiles,
                                   std::uint64_t maxDepth, std::uint64_t maxPathBytes,
                                   std::uint64_t maxRegularFileBytes,
                                   std::string *originalSourceName,
                                   std::shared_ptr<PlatformDirectoryImportProgress> progress) {
  RequestAndroidExternalActivityRenderPause();
  const auto tokenText = std::to_string(operationToken);
  {
    std::lock_guard lock(gDirectoryImportProgressMutex);
    gDirectoryImportProgress.emplace(tokenText, DirectoryImportProgressBridge{std::move(progress)});
  }
  struct ProgressReset {
    const std::string &token;
    ~ProgressReset() {
      std::lock_guard lock(gDirectoryImportProgressMutex);
      const auto found = gDirectoryImportProgress.find(token);
      if (found != gDirectoryImportProgress.end()) {
        if (found->second.renderPaused) FinishAndroidExternalActivityRenderPause();
        gDirectoryImportProgress.erase(found);
      }
    }
  } progressReset{tokenText};
  const auto limits = std::to_string(maxBytes) + "," + std::to_string(maxFiles) +
                      "," + std::to_string(maxDepth) + "," +
                      std::to_string(maxPathBytes) + "," +
                      std::to_string(maxRegularFileBytes);
  std::string callError;
  auto result = callActivityStringMethod2(
      "importDirectory", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;",
      tokenText.c_str(), limits.c_str(), callError);
  if (!callError.empty()) {
    return std::string(kErrorPrefix) + callError;
  }
  const auto separator = result.find('\n');
  if (!result.empty() && result.front() == '/' && separator != std::string::npos) {
    if (originalSourceName) {
      *originalSourceName = result.substr(separator + 1);
    }
    result.resize(separator);
  }
  return result.empty() ? std::string(kErrorPrefix) +
                              "Android folder import returned no result."
                        : result;
}

std::string ExportAndroidDocument(std::uint64_t operationToken,
                                  const std::filesystem::path &localPath,
                                  const std::string &mimeType,
                                  const std::string &suggestedName,
                                  std::uint64_t maxBytes) {
  RequestAndroidExternalActivityRenderPause();
  struct ExternalActivityPauseReset {
    ~ExternalActivityPauseReset() {
      FinishAndroidExternalActivityRenderPause();
    }
  } externalActivityPauseReset;

  std::string callError;
  const auto tokenText = std::to_string(operationToken);
  const std::string pathText = pathToUtf8(localPath);
  const std::string result = callActivityStringMethod4Long(
      "exportDocument",
      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;"
      "Ljava/lang/String;J)Ljava/lang/String;",
      tokenText.c_str(), pathText.c_str(), mimeType.c_str(),
      suggestedName.c_str(), static_cast<jlong>(maxBytes), callError);
  if (!callError.empty()) {
    return std::string(kErrorPrefix) + callError;
  }
  return result.empty() ? std::string(kErrorPrefix) +
                              "Android document export returned no result."
                        : result;
}

void CancelAndroidDocument(std::uint64_t operationToken) {
  const auto tokenText = std::to_string(operationToken);
  std::string ignoredError;
  (void)callActivityStringMethod("cancelDocumentHandoff",
                                 "(Ljava/lang/String;)Ljava/lang/String;",
                                 tokenText.c_str(), ignoredError);
}

bool ValidateAndroidTemporaryDocument(const std::filesystem::path &localPath,
                                      std::string &errorMessage) {
  const std::string pathText = pathToUtf8(localPath);
  const std::string result = callActivityStringMethod(
      "validateDocumentHandoffImport", "(Ljava/lang/String;)Ljava/lang/String;",
      pathText.c_str(), errorMessage);
  if (!errorMessage.empty()) {
    return false;
  }
  if (result == kSuccessResult) {
    return true;
  }
  errorMessage =
      result.rfind(kErrorPrefix, 0) == 0
          ? result.substr(std::char_traits<char>::length(kErrorPrefix))
          : "Android rejected temporary document ownership.";
  return false;
}

bool CleanupAndroidTemporaryDocument(const std::filesystem::path &localPath,
                                     std::string &errorMessage) {
  const std::string pathText = pathToUtf8(localPath);
  const std::string result = callActivityStringMethod(
      "cleanupDocumentHandoffImport", "(Ljava/lang/String;)Ljava/lang/String;",
      pathText.c_str(), errorMessage);
  if (!errorMessage.empty()) {
    return false;
  }
  if (result == kSuccessResult) {
    return true;
  }
  errorMessage =
      result.rfind(kErrorPrefix, 0) == 0
          ? result.substr(std::char_traits<char>::length(kErrorPrefix))
          : "Android could not clean up the temporary document.";
  return false;
}

bool ValidateAndroidTemporaryDirectory(const std::filesystem::path &localPath,
                                      std::string &errorMessage) {
  const std::string pathText = pathToUtf8(localPath);
  const std::string result = callActivityStringMethod(
      "validateDirectoryHandoffImport", "(Ljava/lang/String;)Ljava/lang/String;",
      pathText.c_str(), errorMessage);
  if (!errorMessage.empty()) {
    return false;
  }
  if (result == kSuccessResult) {
    return true;
  }
  errorMessage =
      result.rfind(kErrorPrefix, 0) == 0
          ? result.substr(std::char_traits<char>::length(kErrorPrefix))
          : "Android rejected temporary directory ownership.";
  return false;
}

bool CleanupAndroidTemporaryDirectory(const std::filesystem::path &localPath,
                                     std::string &errorMessage) {
  const std::string pathText = pathToUtf8(localPath);
  const std::string result = callActivityStringMethod(
      "cleanupDirectoryHandoffImport", "(Ljava/lang/String;)Ljava/lang/String;",
      pathText.c_str(), errorMessage);
  if (!errorMessage.empty()) {
    return false;
  }
  if (result == kSuccessResult) {
    return true;
  }
  errorMessage =
      result.rfind(kErrorPrefix, 0) == 0
          ? result.substr(std::char_traits<char>::length(kErrorPrefix))
          : "Android could not clean up the temporary directory.";
  return false;
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
                               std::vector<AndroidTreeChartFile> &chartFiles,
                               std::string &errorMessage,
                               const std::stop_token *stopToken) {
  chartFiles.clear();
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
    const std::size_t separator = line.rfind('\t');
    const bool hasDocument =
        separator != std::string::npos && separator + 2 == line.size() &&
        (line[separator + 1] == '0' || line[separator + 1] == '1');
    chartFiles.push_back({
        .path = std::filesystem::path(hasDocument ? line.substr(0, separator)
                                                   : line),
        .hasDocument = hasDocument && line[separator + 1] == '1',
    });
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

bool OpenAndroidDocumentsFolder(std::string &errorMessage) {
  std::string callError;
  const auto result = callActivityStringMethod(
      "openDocumentsFolder", "()Ljava/lang/String;", nullptr, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
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
                            std::string &errorMessage,
                            AndroidDownloadCheckpoint checkpoint,
                            AndroidDownloadPauseProbe pauseRequested) {
  body.clear();
  AndroidDownloadProgressBridge bridge;
  jlong checkpointToken = 0;
  if (checkpoint || pauseRequested) {
    bridge.checkpoint = &checkpoint;
    bridge.pauseRequested = &pauseRequested;
    checkpointToken = registerAndroidDownloadProgressBridge(bridge);
  }
  struct CheckpointBridgeCleanup {
    jlong token;
    ~CheckpointBridgeCleanup() {
      if (token != 0) unregisterAndroidDownloadProgressBridge(token);
    }
  } cleanup{checkpointToken};

  std::string callError;
  const std::string result = callActivityStringMethodLong(
      "downloadUrlText", "(Ljava/lang/String;J)Ljava/lang/String;",
      url.c_str(), checkpointToken, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  return parseBridgeResult(result, body, errorMessage);
}

bool PostURLTextAndroid(const std::string &url, std::string &body,
                        std::string &errorMessage,
                        AndroidDownloadCheckpoint checkpoint) {
  body.clear();
  AndroidDownloadProgressBridge bridge;
  jlong checkpointToken = 0;
  if (checkpoint) {
    bridge.checkpoint = &checkpoint;
    checkpointToken = registerAndroidDownloadProgressBridge(bridge);
  }
  struct CheckpointBridgeCleanup {
    jlong token;
    ~CheckpointBridgeCleanup() {
      if (token != 0) unregisterAndroidDownloadProgressBridge(token);
    }
  } cleanup{checkpointToken};

  std::string callError;
  const std::string result = callActivityStringMethodLong(
      "postUrlText", "(Ljava/lang/String;J)Ljava/lang/String;",
      url.c_str(), checkpointToken, callError);
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
  const std::string pathText = pathToUtf8(path);
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

bool UpdateAndroidNativeMusicMetadata(
    const AndroidNativeMusicMetadata &metadata, std::string &errorMessage) {
  const std::string payload = musicMetadataPayload(metadata);
  std::string callError;
  const std::string result = callActivityStringMethodLong(
      "updateNativeMusicMetadata", "(Ljava/lang/String;J)Ljava/lang/String;",
      payload.c_str(), metadata.durationMicros, callError);
  if (!callError.empty()) {
    errorMessage = callError;
    return false;
  }
  std::string ignored;
  return parseBridgeResult(result, ignored, errorMessage);
}

bool UpdateAndroidNativeMusicQueue(const AndroidNativeMusicQueue &queue,
                                   std::string &errorMessage) {
  const std::string payload = musicQueuePayload(queue);
  std::string callError;
  const std::string result = callActivityStringMethod2Long(
      "updateNativeMusicQueue",
      "(Ljava/lang/String;Ljava/lang/String;J)Ljava/lang/String;",
      queue.title.c_str(), payload.c_str(),
      static_cast<jlong>(queue.currentIndex), callError);
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

bool SetAndroidNativeMusicPlaybackRate(int percent, bool timeStretch,
                                       std::string &errorMessage) {
  std::string callError;
  const std::string rateText = std::to_string(percent) + "\n" +
                               (timeStretch ? "time-stretch" : "pitch-shift");
  const std::string result = callActivityStringMethod(
      "setNativeMusicPlaybackRate",
      "(Ljava/lang/String;)Ljava/lang/String;", rateText.c_str(),
      callError);
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
  event.type = SDL_EVENT_USER;
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
  event.type = SDL_EVENT_USER;
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
