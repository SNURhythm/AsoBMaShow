#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace video::frameCpuTiming {
enum class Stage : unsigned {
  SceneUpdate, SceneRender, GameplayUpdate, Capture, Projection, SkinPrepare,
  SkinSubmit, BgaPrepare, FullscreenBga, FrameSubmit, DeferredWork,
  ResultPersistence, RealtimeStop, Count
};
inline constexpr std::array<const char *, static_cast<unsigned>(Stage::Count)>
    stageNames{"scene_update", "scene_render", "gameplay_update", "capture",
               "projection", "skin_prepare", "skin_submit", "bga_prepare",
               "fullscreen_bga", "frame_submit", "deferred_work",
               "result_persistence", "realtime_stop"};
enum class GameplayPhase : unsigned { Unknown, Preparing, Active, Paused, Ending, PostNotes };
enum class GameplayMode : unsigned { Unknown, Manual, Autoplay, Replay, Practice };
inline constexpr std::array<const char *, 6> phaseNames{
    "unknown", "preparing", "active", "paused", "ending", "post_notes"};
inline constexpr std::array<const char *, 5> modeNames{
    "unknown", "manual", "autoplay", "replay", "practice"};
struct StageData { double seconds = 0; std::uint32_t count = 0; };
struct FrameData {
  bool collected = false;
  GameplayPhase phaseFirst = GameplayPhase::Unknown;
  GameplayPhase phaseLast = GameplayPhase::Unknown;
  GameplayMode mode = GameplayMode::Unknown;
  bool skinSelected = false;
  std::uint32_t phaseMask = 0, openStages = 0, invalidStages = 0;
  std::array<StageData, static_cast<unsigned>(Stage::Count)> stages{};
};

using Clock = double (*)() noexcept;
namespace detail {
struct Collector {
  std::uint64_t token = 0, serial = 0;
  Clock clock = nullptr;
  FrameData data;
  std::array<unsigned, static_cast<unsigned>(Stage::Count)> open{};
};
// Only the application worker activates its collector. Input, audio, renderer
// and export threads do not inherit its token or invoke the diagnostic clock.
inline thread_local Collector collector;
} // namespace detail

inline bool active() noexcept { return detail::collector.token != 0; }
inline void beginFrame(std::uint64_t token, Clock clock) noexcept {
  auto &c = detail::collector;
  ++c.serial;
  c.token = clock ? token : 0;
  c.clock = clock;
  c.data = {};
  c.data.collected = c.token != 0;
  c.open = {};
}
inline FrameData endFrame(std::uint64_t token) noexcept {
  auto &c = detail::collector;
  if (!token || c.token != token) return {};
  for (unsigned i = 0; i < c.open.size(); ++i) {
    if (c.open[i]) c.data.openStages |= 1U << i;
  }
  c.token = 0;
  return c.data;
}
inline void observeGameplay(GameplayPhase phase, GameplayMode mode,
                            bool skinSelected) noexcept {
  auto &c = detail::collector;
  if (!c.token || phase == GameplayPhase::Unknown) return;
  if (c.data.phaseFirst == GameplayPhase::Unknown) c.data.phaseFirst = phase;
  c.data.phaseLast = phase;
  c.data.phaseMask |= 1U << static_cast<unsigned>(phase);
  c.data.mode = mode;
  c.data.skinSelected = skinSelected;
}

// Totals are inclusive: nested stage durations must not be added to parents.
// Scope completion is ignored after interruption or replacement of its frame.
class Scope {
public:
  explicit Scope(Stage stage) noexcept : stage_(static_cast<unsigned>(stage)) {
    auto &c = detail::collector;
    if (!c.token || stage_ >= c.open.size()) return;
    start_ = c.clock();
    if (!std::isfinite(start_) || start_ <= 0) {
      c.data.invalidStages |= 1U << stage_;
      return;
    }
    serial_ = c.serial;
    token_ = c.token;
    ++c.open[stage_];
  }
  ~Scope() { finish(); }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
  void finish() noexcept {
    auto &c = detail::collector;
    const auto token = token_;
    token_ = 0;
    if (!token || c.token != token || c.serial != serial_) return;
    const double elapsed = c.clock() - start_;
    --c.open[stage_];
    auto &stage = c.data.stages[stage_];
    if (!std::isfinite(elapsed) || elapsed < 0 ||
        !std::isfinite(stage.seconds + elapsed)) {
      c.data.invalidStages |= 1U << stage_;
      return;
    }
    stage.seconds += elapsed;
    ++stage.count;
  }
private:
  unsigned stage_;
  std::uint64_t token_ = 0, serial_ = 0;
  double start_ = 0;
};
} // namespace video::frameCpuTiming
