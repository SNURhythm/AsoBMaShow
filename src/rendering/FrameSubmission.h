#pragma once
#include <cstdint>

namespace rendering {
namespace detail {
extern "C" bool asobmashow_bgfx_frame_draws_accepted();
// Only the serialized bgfx owner thread may access this counter.
inline std::uint64_t frameSubmissionFailures = 0;
}

// Inspect on the bgfx API/owner thread after all draws and BEFORE bgfx::frame().
// The result covers bgfx's current encoder-0 draw/dispatch drop counter only;
// callers must also propagate their own allocation/preflight failures. It is
// CPU submission acceptance, not GPU completion or successful presentation.
//
// The app submits exclusively through encoder 0. Additional encoders make this
// return false, even after bgfx::end(); their handles survive until frame().
// No encoder may begin/end/submit concurrently with this inspection, and the
// result is invalidated by further submissions. frame() starts fresh accounting.
// Returns false outside the initialized lifetime. Does not wait for the GPU.
inline bool frameDrawsAccepted() {
  return detail::asobmashow_bgfx_frame_draws_accepted();
}

inline void rejectFrameSubmission() {
  ++detail::frameSubmissionFailures;
}

// Capture before rendering, inspect before frame(). A failure stays observable
// across batch begin/end calls and later successes. A fresh scope can recover
// without resetting a global latch or hiding failures from an outer scope.
class FrameSubmission {
public:
  [[nodiscard]] bool accepted() const {
    return failuresBefore_ == detail::frameSubmissionFailures &&
           frameDrawsAccepted();
  }
private:
  std::uint64_t failuresBefore_ = detail::frameSubmissionFailures;
};
} // namespace rendering
