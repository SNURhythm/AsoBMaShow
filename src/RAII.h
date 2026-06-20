#pragma once

#include <memory>
#include <utility>

template <auto Cleanup> struct FunctionDeleter {
  template <typename T> void operator()(T *ptr) const noexcept {
    if (ptr != nullptr) {
      (void)Cleanup(ptr);
    }
  }
};

template <typename T, auto Cleanup>
using UniqueResource = std::unique_ptr<T, FunctionDeleter<Cleanup>>;

template <typename T, auto Cleanup>
UniqueResource<T, Cleanup> makeUniqueResource(T *ptr) {
  return UniqueResource<T, Cleanup>(ptr);
}

template <auto Cleanup> struct MethodCleanupAndDeleteDeleter {
  template <typename T> void operator()(T *ptr) const noexcept {
    if (ptr == nullptr) {
      return;
    }
    (void)(ptr->*Cleanup)();
    delete ptr;
  }
};

template <typename T, auto Cleanup>
using UniqueCleanupObject =
    std::unique_ptr<T, MethodCleanupAndDeleteDeleter<Cleanup>>;

template <typename T, auto Cleanup, typename... Args>
UniqueCleanupObject<T, Cleanup> makeUniqueCleanupObject(Args &&...args) {
  return UniqueCleanupObject<T, Cleanup>(
      new T(std::forward<Args>(args)...));
}

template <typename Func> class ScopeExit {
public:
  explicit ScopeExit(Func func) : func_(std::move(func)) {}
  ScopeExit(const ScopeExit &) = delete;
  ScopeExit &operator=(const ScopeExit &) = delete;

  ScopeExit(ScopeExit &&other) noexcept
      : func_(std::move(other.func_)), active_(other.active_) {
    other.active_ = false;
  }

  ~ScopeExit() {
    if (active_) {
      func_();
    }
  }

  void dismiss() { active_ = false; }

  void runNow() {
    if (!active_) {
      return;
    }
    active_ = false;
    func_();
  }

private:
  Func func_;
  bool active_ = true;
};

template <typename Func> ScopeExit<Func> makeScopeExit(Func func) {
  return ScopeExit<Func>(std::move(func));
}
