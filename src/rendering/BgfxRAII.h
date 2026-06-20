#pragma once

#include <bgfx/bgfx.h>

namespace rendering {
template <typename Handle> class BgfxHandleGuard {
public:
  explicit BgfxHandleGuard(Handle handle = BGFX_INVALID_HANDLE)
      : handle_(handle) {}
  BgfxHandleGuard(const BgfxHandleGuard &) = delete;
  BgfxHandleGuard &operator=(const BgfxHandleGuard &) = delete;

  ~BgfxHandleGuard() { reset(); }

  Handle get() const { return handle_; }

  Handle release() {
    const Handle handle = handle_;
    handle_ = BGFX_INVALID_HANDLE;
    return handle;
  }

  void reset(Handle handle = BGFX_INVALID_HANDLE) {
    if (bgfx::isValid(handle_)) {
      bgfx::destroy(handle_);
    }
    handle_ = handle;
  }

private:
  Handle handle_ = BGFX_INVALID_HANDLE;
};
} // namespace rendering
