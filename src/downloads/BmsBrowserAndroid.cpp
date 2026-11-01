#include "../targets.h"
#if TARGET_OS_ANDROID
#include "BmsBrowser.h"
#include "BrowserPolicy.h"
#include "DownloadService.h"
#include <SDL3/SDL.h>
#include <jni.h>

namespace {
std::string utf8(JNIEnv *env, jstring text) {
  if (!text) return {};
  jclass type = env->GetObjectClass(text);
  auto method = env->GetMethodID(type, "getBytes", "(Ljava/lang/String;)[B");
  auto encoding = env->NewStringUTF("UTF-8");
  auto bytes = static_cast<jbyteArray>(env->CallObjectMethod(text, method, encoding));
  env->DeleteLocalRef(type); env->DeleteLocalRef(encoding);
  if (!bytes || env->ExceptionCheck()) return {};
  std::string result(static_cast<std::size_t>(env->GetArrayLength(bytes)), '\0');
  env->GetByteArrayRegion(bytes, 0, result.size(), reinterpret_cast<jbyte *>(result.data()));
  env->DeleteLocalRef(bytes); return result;
}
}
namespace downloads {
void openBrowser() {
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return;
  auto cls = env->GetObjectClass(activity);
  auto method = env->GetMethodID(cls, "openBmsBrowser", "()V");
  if (method) env->CallVoidMethod(activity, method);
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->DeleteLocalRef(cls); env->DeleteLocalRef(activity);
}
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_BmsBrowserActivity_nativeCanImportTable(JNIEnv *env, jclass, jstring url) {
  return downloads::gingerTableUrl(utf8(env, url)).has_value();
}
extern "C" JNIEXPORT jboolean JNICALL
Java_com_snurhythm_asobmashow_BmsBrowserActivity_nativeImportTable(JNIEnv *env, jclass, jstring url) {
  return downloads::requestBrowserTableImport(utf8(env, url));
}
extern "C" JNIEXPORT jstring JNICALL
Java_com_snurhythm_asobmashow_BmsBrowserActivity_nativeDownload(JNIEnv *env, jclass, jstring url, jstring name,
    jstring agent, jstring cookie, jstring referrer) {
  try {
    auto error = downloads::acceptBrowserDownload("GET", utf8(env, url), utf8(env, name),
        utf8(env, agent), utf8(env, cookie), utf8(env, referrer));
    return env->NewStringUTF(error.c_str());
  } catch (...) { return env->NewStringUTF("Could not start this download."); }
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsBrowserActivity_nativeForeground(JNIEnv *, jclass, jboolean foreground) {
  try { downloads::setBrowserForeground(foreground); } catch (...) {}
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsBrowserActivity_nativeShowDownloads(JNIEnv *, jclass) {
  downloads::requestDownloadsPresentation();
}
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_BmsDownloads_nativeShowDownloads(JNIEnv *, jclass) {
  downloads::requestDownloadsPresentation();
}
#endif
