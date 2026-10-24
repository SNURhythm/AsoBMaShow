#include "../library/ReadmeTranslation.h"
#include "../targets.h"
#include "../i18n/Localization.h"
#include <nlohmann/json.hpp>
#if TARGET_OS_ANDROID
#include <SDL3/SDL_system.h>
#include <jni.h>
#elif defined(__APPLE__)
extern "C" bool ReadmeAppleTranslationSupported();
extern "C" void ReadmeAppleTranslate(std::uint64_t, const char *, const char *,
    void (*)(std::uint64_t, int, const char *));
extern "C" void ReadmeAppleTranslationStop(std::uint64_t);
#endif
namespace readme_translation {
namespace {
[[maybe_unused]] void receive(std::uint64_t id, int state, const char *text) {
  if (state < 0 || state > static_cast<int>(State::Cancelled)) return;
  deliver(id, static_cast<State>(state), text ? text : "");
}
#if TARGET_OS_ANDROID
void invoke(std::uint64_t id, const std::string *json, const std::string *target) {
  auto *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  if (!env) { deliver(id, State::Failed, ""); return; }
  if (env->PushLocalFrame(8) < 0) { env->ExceptionClear(); deliver(id, State::Failed, ""); return; }
  const auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  const auto type = activity ? env->GetObjectClass(activity) : nullptr;
  const auto method = type ? env->GetMethodID(type, json ? "translateReadme" : "cancelReadmeTranslation",
      json ? "(J[BLjava/lang/String;)V" : "(J)V") : nullptr;
  if (method && !env->ExceptionCheck()) {
    if (json) {
      const auto bytes = env->NewByteArray(static_cast<jsize>(json->size()));
      const auto language = env->NewStringUTF(target->c_str());
      if (bytes && language) {
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(json->size()),
                               reinterpret_cast<const jbyte *>(json->data()));
        if (!env->ExceptionCheck()) env->CallVoidMethod(activity, method, static_cast<jlong>(id), bytes, language);
      }
    } else env->CallVoidMethod(activity, method, static_cast<jlong>(id));
  }
  const bool failed = !method || env->ExceptionCheck();
  if (env->ExceptionCheck()) env->ExceptionClear();
  env->PopLocalFrame(nullptr);
  if (failed && json) deliver(id, State::Failed, "");
}
#endif
}
bool supported() {
#if TARGET_OS_ANDROID
  return true;
#elif defined(__APPLE__)
  return ReadmeAppleTranslationSupported();
#else
  return false;
#endif
}
void start(std::uint64_t id, const std::string &text, const std::string &target) {
  const auto parts = chunks(text);
  const auto json = nlohmann::json{
      {"parts", parts}, {"samples", languageSamples(parts)},
      {"translate", i18n::tr("readme.translation.translate")},
      {"preparing", i18n::tr("readme.translation.preparing")},
      {"cancel", i18n::tr("readme.translation.cancel")},
      {"download", i18n::tr("readme.translation.download")},
      {"download_message", i18n::tr("readme.translation.download_message")}}.dump();
#if TARGET_OS_ANDROID
  invoke(id, &json, &target);
#elif defined(__APPLE__)
  ReadmeAppleTranslate(id, json.c_str(), target.c_str(), receive);
#else
  deliver(id, State::Failed, "");
#endif
}
void stop(std::uint64_t id) {
  if (!id) return;
#if TARGET_OS_ANDROID
  invoke(id, nullptr, nullptr);
#elif defined(__APPLE__)
  ReadmeAppleTranslationStop(id);
#endif
}
}
#if TARGET_OS_ANDROID
extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_ReadmeTranslator_deliver(JNIEnv *env, jclass, jlong id, jint state, jbyteArray bytes) {
  std::string text;
  if (bytes) {
    text.resize(env->GetArrayLength(bytes));
    env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(text.size()), reinterpret_cast<jbyte *>(text.data()));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return; }
  }
  readme_translation::deliver(static_cast<std::uint64_t>(id),
      static_cast<readme_translation::State>(state), std::move(text));
}
#endif
