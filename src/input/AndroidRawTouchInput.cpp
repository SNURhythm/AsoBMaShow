#include "AndroidRawTouchInput.h"

#include <mutex>

namespace input::android {
namespace {
struct Registry {
  std::mutex mutex;
  RawTouchRegistration *active = nullptr;
};
Registry &registry() {
  static Registry state;
  return state;
}
} // namespace

RawTouchRegistration::RawTouchRegistration(Callback callback, void *context)
    : callback_(callback), context_(context) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  state.active = this;
}

RawTouchRegistration::~RawTouchRegistration() {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  if (state.active == this) state.active = nullptr;
}

void RawTouchRegistration::dispatch(const RawTouchEvent &event) {
  auto &state = registry();
  std::lock_guard lock(state.mutex);
  if (state.active != nullptr && state.active->callback_ != nullptr) {
    state.active->callback_(event, state.active->context_);
  }
}

} // namespace input::android
