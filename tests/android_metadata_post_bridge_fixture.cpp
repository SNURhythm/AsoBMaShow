#include "AndroidNatives.h"

#include <jni.h>
#include <mutex>
#include <type_traits>
#include <unordered_map>

thread_local JNIEnv *fixtureEnv = nullptr;
thread_local jobject fixtureActivity = nullptr;
std::atomic_bool fixtureCancelled{false};
constexpr const char *kErrorPrefix = "__ERROR__:";

void *SDL_GetAndroidJNIEnv() { return fixtureEnv; }
void *SDL_GetAndroidActivity() {
  return fixtureEnv->NewLocalRef(fixtureActivity);
}

#include "android_metadata_post_methods.inc"

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_cancel(JNIEnv *, jclass) {
  fixtureCancelled.store(true);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_registeredBridges(JNIEnv *, jclass) {
  std::lock_guard lock(gAndroidDownloadProgressMutex);
  return static_cast<jint>(gAndroidDownloadProgressBridges.size());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_request(
    JNIEnv *env, jobject activity, jstring url, jboolean cancellable) {
  fixtureEnv = env;
  fixtureActivity = activity;
  std::string error;
  auto result = postUrlText(jstringToUtf8(env, url), error,
                           cancellable ? &fixtureCancelled : nullptr,
                           16 * 1024 * 1024);
  return utf8ToJString(env, result ? result->c_str() : ("ERROR:" + error).c_str());
}

template <typename Request>
bool requestBridge(Request request, const std::string &url, std::string &body,
                   std::string &error, AndroidDownloadCheckpoint checkpoint) {
  if constexpr (std::is_invocable_v<Request, const std::string &, std::string &,
                                    std::string &, AndroidDownloadCheckpoint>) {
    return request(url, body, error, std::move(checkpoint));
  } else {
    return request(url, body, error);
  }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_requestBridge(
    JNIEnv *env, jobject activity, jstring url, jboolean cancellable) {
  fixtureEnv = env;
  fixtureActivity = activity;
  std::string body;
  std::string error;
  AndroidDownloadCheckpoint checkpoint;
  if (cancellable) checkpoint = [] { return !fixtureCancelled.load(); };
  const bool success = requestBridge(&PostURLTextAndroid, jstringToUtf8(env, url),
                                    body, error, std::move(checkpoint));
  return utf8ToJString(env, success ? body.c_str() : ("ERROR:" + error).c_str());
}

template <typename Request>
bool downloadBridge(Request request, const std::string &url,
                    const std::filesystem::path &path, std::string &error,
                    long *status) {
  if constexpr (std::is_invocable_v<Request, const std::string &,
                                    const std::filesystem::path &, std::atomic_bool &,
                                    AndroidDownloadProgressCallback, std::string &, long *>) {
    return request(url, path, fixtureCancelled, {}, error, status);
  } else {
    return request(url, path, fixtureCancelled, {}, error);
  }
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_requestFile(
    JNIEnv *env, jobject activity, jstring url, jstring path, jboolean wantStatus) {
  fixtureEnv = env;
  fixtureActivity = activity;
  std::string error;
  long status = 999;
  const bool success = downloadBridge(&DownloadURLToFileAndroid,
      jstringToUtf8(env, url), std::filesystem::path(jstringToUtf8(env, path)),
      error, wantStatus ? &status : nullptr);
  const std::string result = std::to_string(status) + ":" +
      (success ? "OK" : "ERROR:" + error);
  return utf8ToJString(env, result.c_str());
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_AsoBMaShowActivity_reservePath(
    JNIEnv *env, jobject activity, jstring path) {
  fixtureEnv = env;
  fixtureActivity = activity;
  std::string error;
  auto lease = ReserveAndroidStorageSubtrees({std::filesystem::path(jstringToUtf8(env, path))}, error);
  return lease != nullptr;
}
