#include "../targets.h"
#if TARGET_OS_ANDROID
#include "DownloadPlatform.h"
#include "DownloadService.h"
#include "../../yoga/lib/nlohmann/json.hpp"
#include <jni.h>
#include <set>

namespace {
JavaVM *downloadVM = nullptr;
jclass downloadClass = nullptr;
std::filesystem::path downloadRoot;
class Environment {
public:
  Environment() {
    if (!downloadVM) return;
    if (downloadVM->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
      attached = downloadVM->AttachCurrentThread(&env, nullptr) == JNI_OK;
    }
  }
  ~Environment() { if (attached) downloadVM->DetachCurrentThread(); }
  JNIEnv *env = nullptr;
private:
  bool attached = false;
};
std::string stringValue(JNIEnv *env, jstring value) {
  if (!value) return {};
  const char *data = env->GetStringUTFChars(value, nullptr);
  if (!data) return {};
  std::string result(data); env->ReleaseStringUTFChars(value, data); return result;
}
bool clearException(JNIEnv *env) {
  if (!env->ExceptionCheck()) return false;
  env->ExceptionClear(); return true;
}
void bridgeFailure(JNIEnv *env) {
  if (env->ExceptionCheck()) return;
  const auto cls = env->FindClass("java/lang/IllegalStateException");
  if (cls) { env->ThrowNew(cls, "Download state could not be saved."); env->DeleteLocalRef(cls); }
}
}
namespace downloads::platform {
std::filesystem::path privateRoot() { return downloadRoot; }
void initialize() {
  Environment bridge; auto *env = bridge.env;
  if (!env || !downloadClass) throw std::runtime_error("Android downloads are not initialized.");
  auto method = env->GetStaticMethodID(downloadClass, "reconcile", "()V");
  if (method) env->CallStaticVoidMethod(downloadClass, method);
  if (clearException(env)) throw std::runtime_error("Could not restore Android downloads.");
}
std::string startTransfer(const Job &job, const std::filesystem::path &) {
  Environment bridge; auto *env = bridge.env;
  if (!env || !downloadClass) return "Android download host is unavailable.";
  auto method = env->GetStaticMethodID(downloadClass, "schedule", "(Ljava/lang/String;J)Ljava/lang/String;");
  if (!method) { clearException(env); return "Android download host is unavailable."; }
  auto id = env->NewStringUTF(job.id.c_str());
  auto result = static_cast<jstring>(env->CallStaticObjectMethod(downloadClass, method, id, static_cast<jlong>(job.attempt)));
  env->DeleteLocalRef(id);
  if (clearException(env)) return "Could not schedule this download.";
  const auto error = stringValue(env, result);
  if (result) env->DeleteLocalRef(result);
  return error;
}
void cancelTransfer(const Job &job) {
  Environment bridge; auto *env = bridge.env;
  if (!env || !downloadClass) return;
  auto method = env->GetStaticMethodID(downloadClass, "cancelTransfer", "(Ljava/lang/String;)V");
  auto id = env->NewStringUTF(job.id.c_str());
  if (method) env->CallStaticVoidMethod(downloadClass, method, id);
  env->DeleteLocalRef(id); clearException(env);
}
void beginImport(const Job &) {}
void finishActivity(const Job &, bool) {}
void reportProgress(const Job &) {}
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeInitialize(JNIEnv *env, jclass cls, jstring root) {
  try {
    if (!downloadClass) {
      env->GetJavaVM(&downloadVM);
      downloadClass = static_cast<jclass>(env->NewGlobalRef(cls));
      downloadRoot = stringValue(env, root);
    }
    downloads::service();
  } catch (...) { bridgeFailure(env); }
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeCurrent(JNIEnv *env, jclass, jstring id, jlong attempt) {
  try { return downloads::service()->isCurrentTransfer(stringValue(env, id), attempt); }
  catch (...) { return false; }
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeRequest(JNIEnv *env, jclass, jstring id, jlong attempt) {
  try {
    auto owner = downloads::service(); const auto key = stringValue(env, id);
    auto job = owner->get(key);
    if (job && job->state == downloads::State::Transferring && job->attempt == static_cast<std::uint64_t>(attempt)) {
      const auto request = downloads::transferRequest(*job);
      nlohmann::json value = {{"url", request.url}, {"title", job->request.title},
          {"headers", request.headers}, {"part", (owner->jobPath(key) / "transfer.part").string()}};
      return env->NewStringUTF(value.dump(-1, ' ', true).c_str());
    }
  } catch (...) { bridgeFailure(env); }
  return env->NewStringUTF("");
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeProgress(JNIEnv *env, jclass, jstring id, jlong attempt, jlong bytes, jlong total) {
  try { downloads::service()->transferProgress(stringValue(env, id), attempt, bytes, total); }
  catch (...) { bridgeFailure(env); }
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeComplete(JNIEnv *env, jclass, jstring id, jlong attempt, jstring path) {
  try {
    auto owner = downloads::service(); const auto key = stringValue(env, id);
    const auto expected = owner->jobPath(key) / "transfer.part";
    if (stringValue(env, path) == expected.string()) owner->acceptFile(key, attempt, expected);
  } catch (...) { bridgeFailure(env); }
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeFailed(JNIEnv *env, jclass, jstring id, jlong attempt, jstring message, jint httpStatus) {
  try { downloads::service()->transferFailed(stringValue(env, id), attempt, stringValue(env, message), httpStatus); }
  catch (...) { bridgeFailure(env); }
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeCancel(JNIEnv *env, jclass, jstring id, jlong attempt) {
  try {
    auto owner = downloads::service(); const auto key = stringValue(env, id);
    if (owner->isCurrentTransfer(key, attempt)) owner->cancel(key);
  } catch (...) { bridgeFailure(env); }
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeReconcile(JNIEnv *env, jclass, jobjectArray scheduled, jobjectArray active) {
  try {
    std::set<std::string> live;
    for (auto array : {scheduled, active}) {
      for (jsize i = 0; array && i < env->GetArrayLength(array); ++i) {
        auto item = static_cast<jstring>(env->GetObjectArrayElement(array, i));
        live.insert(stringValue(env, item)); env->DeleteLocalRef(item);
      }
    }
    auto owner = downloads::service();
    for (const auto &job : owner->jobs()) {
      if (job.state == downloads::State::Transferring && !live.contains(job.id)) {
        auto method = env->GetStaticMethodID(downloadClass, "recover", "(Ljava/lang/String;J)Z");
        auto id = env->NewStringUTF(job.id.c_str());
        bool recovered = method && env->CallStaticBooleanMethod(downloadClass, method, id, static_cast<jlong>(job.attempt));
        env->DeleteLocalRef(id);
        if (clearException(env)) recovered = false;
        if (!recovered) owner->transferFailed(job.id, job.attempt, "Download interrupted. Open Downloads to continue.");
      }
    }
  } catch (...) { bridgeFailure(env); }
}
#endif
