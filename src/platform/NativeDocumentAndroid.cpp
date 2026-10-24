#include "NativeDocument.h"
#include "../targets.h"

#if TARGET_OS_ANDROID
#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace native_document {
namespace {
std::atomic<jlong> nextId{1};
std::mutex statesMutex;
std::unordered_map<jlong, std::weak_ptr<State>> states;

// Java holds a token, never a pointer to a Surface or scene. Retiring the token
// before removing the view makes even an in-flight UI callback harmless.
void dismiss(jlong id) {
  std::shared_ptr<State> state;
  {
    std::lock_guard<std::mutex> lock(statesMutex);
    const auto it = states.find(id);
    if (it != states.end()) state = it->second.lock();
  }
  if (state) state->requestDismiss();
}

struct JniScope {
  JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
  bool pushed = false;
  JniScope() {
    if (env) {
      pushed = env->PushLocalFrame(8) == 0;
      if (!pushed) { env->ExceptionClear(); env = nullptr; }
    }
  }
  ~JniScope() {
    if (env) {
      if (env->ExceptionCheck()) {
        SDL_Log("Native document: Android JNI operation failed");
        env->ExceptionClear();
      }
      if (pushed) env->PopLocalFrame(nullptr);
    }
  }
};

class AndroidSurface final : public Surface {
  const jlong id_ = nextId.fetch_add(1);
  const std::string text_;
  jobject view_ = nullptr; // Global reference to a lifetime gate, not an Android View.

  template <class Function>
  bool invoke(const char *name, const char *signature, Function function) {
    JniScope scope;
    auto *env = scope.env;
    if (!env || !view_) return false;
    const auto type = env->GetObjectClass(view_);
    const auto method = type ? env->GetMethodID(type, name, signature) : nullptr;
    if (!method || env->ExceptionCheck()) return false;
    function(env, method);
    return !env->ExceptionCheck();
  }
public:
  explicit AndroidSurface(const std::string &text) : text_(text) {
    std::lock_guard<std::mutex> lock(statesMutex);
    states.emplace(id_, state_);
  }
  bool open(Frame frame, Appearance appearance) {
    if (text_.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) return false;
    JniScope scope;
    auto *env = scope.env;
    if (!env) return false;
    const auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    const auto type = activity ? env->GetObjectClass(activity) : nullptr;
    const auto method = type ? env->GetMethodID(type, "openNativeDocument",
        "(J[BFFFFIIIF)Lcom/snurhythm/asobmashow/NativeDocumentView;") : nullptr;
    if (!method || env->ExceptionCheck()) return false;
    // NewStringUTF uses modified UTF-8 (breaking supplementary characters and
    // embedded NULs). Decode ordinary UTF-8 explicitly on the Java side instead.
    const auto bytes = env->NewByteArray(static_cast<jsize>(text_.size()));
    if (!bytes || env->ExceptionCheck()) return false;
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(text_.size()),
                           reinterpret_cast<const jbyte *>(text_.data()));
    if (env->ExceptionCheck()) return false;
    const auto local = env->CallObjectMethod(activity, method, id_, bytes,
        frame.x, frame.y, frame.width, frame.height,
        static_cast<jint>(appearance.foreground), static_cast<jint>(appearance.background),
        static_cast<jint>(appearance.highlight), appearance.fontSize);
    if (!local || env->ExceptionCheck()) return false;
    view_ = env->NewGlobalRef(local);
    if (!view_) {
      // The UI create may already be queued. Cancel it even if retaining its
      // Java owner failed, otherwise no native destructor could reach it.
      env->ExceptionClear();
      const auto localType = env->GetObjectClass(local);
      const auto close = localType ? env->GetMethodID(localType, "close", "()V") : nullptr;
      if (close) env->CallVoidMethod(local, close);
    }
    return view_ != nullptr && !env->ExceptionCheck();
  }
  ~AndroidSurface() override {
    {
      std::lock_guard<std::mutex> lock(statesMutex);
      states.erase(id_);
    }
    invoke("close", "()V", [&](JNIEnv *env, jmethodID method) {
      env->CallVoidMethod(view_, method);
    });
    JniScope scope;
    if (scope.env && view_) scope.env->DeleteGlobalRef(view_);
  }
  void setFrame(Frame frame) override {
    if (!invoke("setFrame", "(FFFF)V", [&](JNIEnv *env, jmethodID method) {
      env->CallVoidMethod(view_, method, frame.x, frame.y, frame.width, frame.height);
    })) state_->requestDismiss();
  }
  void setAppearance(Appearance appearance) override {
    if (!invoke("setAppearance", "(IIIF)V", [&](JNIEnv *env, jmethodID method) {
      env->CallVoidMethod(view_, method, static_cast<jint>(appearance.foreground),
          static_cast<jint>(appearance.background), static_cast<jint>(appearance.highlight),
          appearance.fontSize);
    })) state_->requestDismiss();
  }
  void highlight(const std::vector<text_range::Range> &ranges, bool focus) override {
    const auto converted = utf16Ranges(text_, ranges);
    std::vector<jint> endpoints;
    if (converted.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max() / 2)) return;
    endpoints.reserve(converted.size() * 2);
    for (const auto &range : converted) {
      endpoints.push_back(static_cast<jint>(range.begin));
      endpoints.push_back(static_cast<jint>(range.end));
    }
    if (!invoke("highlight", "([IZ)V", [&](JNIEnv *env, jmethodID method) {
      const auto array = env->NewIntArray(static_cast<jsize>(endpoints.size()));
      if (!array || env->ExceptionCheck()) return;
      if (!endpoints.empty())
        env->SetIntArrayRegion(array, 0, static_cast<jsize>(endpoints.size()), endpoints.data());
      if (!env->ExceptionCheck()) env->CallVoidMethod(view_, method, array, static_cast<jboolean>(focus));
    })) state_->requestDismiss();
  }
  void scroll(Scroll action) override {
    if (!invoke("scroll", "(I)V", [&](JNIEnv *env, jmethodID method) {
      env->CallVoidMethod(view_, method, static_cast<jint>(action));
    })) state_->requestDismiss();
  }
};
}
namespace detail {
std::unique_ptr<Surface> createAndroid(const std::string &text, Frame frame, Appearance appearance) {
  auto surface = std::make_unique<AndroidSurface>(text);
  if (!surface->open(frame, appearance)) return nullptr;
  return surface;
}
}
} // namespace native_document

extern "C" JNIEXPORT void JNICALL
Java_com_snurhythm_asobmashow_NativeDocumentView_requestDismiss(JNIEnv *, jclass, jlong id) {
  native_document::dismiss(id);
}
#endif
