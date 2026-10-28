#include "LocalNotifications.h"
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
#include <atomic>
namespace reminders {
namespace {
std::atomic<Permission> status{Permission::Unknown};
std::string call(const char *operation, const std::string &argument = {}) {
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  if (!env) return {};
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!activity) return {};
  auto type = env->GetObjectClass(activity);
  auto method = env->GetMethodID(type, "komboNotifications", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
  auto op = env->NewStringUTF(operation);
  auto arg = env->NewStringUTF(argument.c_str());
  auto value = method ? static_cast<jstring>(env->CallObjectMethod(activity, method, op, arg)) : nullptr;
  std::string result;
  if (env->ExceptionCheck()) { env->ExceptionClear(); SDL_Log("Kombo notification JNI call failed"); }
  else if (value) {
    const char *text = env->GetStringUTFChars(value, nullptr);
    if (text) { result = text; env->ReleaseStringUTFChars(value, text); }
  }
  if (value) env->DeleteLocalRef(value);
  env->DeleteLocalRef(arg); env->DeleteLocalRef(op);
  env->DeleteLocalRef(type); env->DeleteLocalRef(activity);
  return result;
}
}
void refreshPermission() {
  const auto value = call("permission");
  status = value == "granted" ? Permission::Granted : value == "ask" ? Permission::NotDetermined : Permission::Denied;
}
Permission permission() { return status.load(); }
void requestPermission() { call("request"); }
void openSettings() { call("settings"); }
void replacePlan(const std::string &json) { call("replace", json); }
std::string takeTapProfile() { return call("tap"); }
}
#endif
