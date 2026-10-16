#pragma once

#include <atomic>
#include <cstdint>
#include <optional>

namespace ui {

struct Revisions {
  std::uint64_t visual = 0;
  std::uint64_t completion = 0;
  bool operator==(const Revisions &) const = default;
};

namespace detail {
// Export workers own separate UI trees. Their local style/layout mutations must
// not keep the foreground UI rendering. Completions explicitly cross threads.
inline thread_local std::uint64_t visualRevision = 1;
inline std::atomic<std::uint64_t> completionRevision{1};
}

inline void invalidate() noexcept { ++detail::visualRevision; }
inline void notifyCompletion() noexcept {
  detail::completionRevision.fetch_add(1, std::memory_order_release);
}
[[nodiscard]] inline Revisions revisions() noexcept {
  return {detail::visualRevision,
          detail::completionRevision.load(std::memory_order_acquire)};
}

struct FrameDemand {
  bool animation = false;
  std::optional<std::uint64_t> redrawAt;
};

namespace detail {
inline thread_local FrameDemand *frameDemand = nullptr;
}

// Draw requests belong to the active presentation attempt. Nested/offscreen
// drawing restores the owner's collector instead of leaking its animation rate.
class FrameDemandScope {
public:
  explicit FrameDemandScope(FrameDemand &demand) noexcept
      : previous_(detail::frameDemand) {
    detail::frameDemand = &demand;
  }
  ~FrameDemandScope() { detail::frameDemand = previous_; }
  FrameDemandScope(const FrameDemandScope &) = delete;
  FrameDemandScope &operator=(const FrameDemandScope &) = delete;
private:
  FrameDemand *previous_;
};

inline void requestAnimationFrame() noexcept {
  if (detail::frameDemand) detail::frameDemand->animation = true;
}
inline void requestRedrawAt(std::uint64_t ticks) noexcept {
  if (auto *demand = detail::frameDemand;
      demand && (!demand->redrawAt || ticks < *demand->redrawAt)) {
    demand->redrawAt = ticks;
  }
}

class PresentationSchedule {
public:
  // Milliseconds on SDL_GetTicks' monotonic clock. Animation is paced by the
  // existing application/display pacer; this class never sleeps or polls input.
  [[nodiscard]] bool shouldRender(std::uint64_t now,
                                  bool continuous = false) const noexcept {
    return !presented_ || continuous || revisions() != presentedRevisions_ ||
           demand_.animation || (demand_.redrawAt && now >= *demand_.redrawAt) ||
           now - presentedAt_ >= kResourceMaintenanceInterval;
  }
  void invalidate() noexcept { presented_ = false; }
  void didPresent(Revisions beforeDrawing, FrameDemand demand,
                  std::uint64_t now) noexcept {
    presentedRevisions_ = beforeDrawing;
    demand_ = demand;
    presentedAt_ = now;
    presented_ = true;
  }
private:
  static constexpr std::uint64_t kResourceMaintenanceInterval = 1'000;
  Revisions presentedRevisions_;
  FrameDemand demand_;
  std::uint64_t presentedAt_ = 0;
  bool presented_ = false;
};

} // namespace ui
