// Compile upstream's implementation in this translation unit so the small
// application hook can inspect private current-frame state without modifying
// the pinned submodule or relying on the delayed/lifetime public statistics.
#include "bgfx.cpp"
#include "../src/rendering/FrameSubmission.h"

namespace rendering::detail {
extern "C" bool asobmashow_bgfx_frame_draws_accepted() {
  using namespace bgfx;
  if (s_ctx == nullptr) return false;
  BGFX_CHECK_API_THREAD();
  // begin(true) retains its handle until frame(), including after end(). This
  // rejects both active and completed extra encoders without reading counters
  // that may be uninitialized or stale from a previous frame.
  return s_ctx->m_encoderHandle->getNumHandles() == 1 &&
         s_ctx->m_encoder[0].m_numDropped == 0;
}
} // namespace rendering::detail
