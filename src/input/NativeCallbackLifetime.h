#pragma once

#include <cstdint>
#include <memory>

class NativeCallbackLifetime {
  struct State;
  struct Registry;

public:
  class Lease {
  public:
    Lease() = default;
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept;
    Lease &operator=(Lease &&other) noexcept;
    ~Lease();

    [[nodiscard]] explicit operator bool() const noexcept {
      return owner_ != nullptr;
    }

    template <typename Owner> [[nodiscard]] Owner *ownerAs() const noexcept {
      return static_cast<Owner *>(owner_);
    }

  private:
    friend class NativeCallbackLifetime;
    Lease(std::shared_ptr<State> state, void *owner) noexcept;
    void release() noexcept;

    std::shared_ptr<State> state_;
    void *owner_ = nullptr;
  };

  explicit NativeCallbackLifetime(void *owner);
  NativeCallbackLifetime(const NativeCallbackLifetime &) = delete;
  NativeCallbackLifetime &operator=(const NativeCallbackLifetime &) = delete;
  NativeCallbackLifetime(NativeCallbackLifetime &&) = delete;
  NativeCallbackLifetime &operator=(NativeCallbackLifetime &&) = delete;
  ~NativeCallbackLifetime();

  [[nodiscard]] void *token() const noexcept;
  void closeAndWait() noexcept;

  [[nodiscard]] static Lease acquire(void *token) noexcept;

private:
  [[nodiscard]] static Registry &registry();

  std::shared_ptr<State> state_;
  std::uintptr_t token_ = 0;
};
