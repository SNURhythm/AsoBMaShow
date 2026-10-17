#pragma once

#include "../bms_parser.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace audio {
class DecodedAssetCache;
inline constexpr int kPreviewSampleRate = 44100;
struct PagedPreviewGains {
  float bgm = 1.0f;
  float keysound = 1.0f;
};
enum class PagedPreviewRenderStatus { Data, Pending, End };
struct PagedPreviewRenderResult {
  PagedPreviewRenderStatus status = PagedPreviewRenderStatus::Pending;
  std::uint32_t frames = 0;
};
struct PagedPreviewState {
  bool startupReady = false;
  bool preparationComplete = false;
  bool durationKnown = false;
  bool cancelled = false;
  std::uint64_t firstFrame44100 = 0;
  std::uint64_t durationFrames44100 = 0;
  std::uint64_t positionFrames44100 = 0;
  std::uint64_t revision = 0;
  std::string error;
};

// A single control thread owns Begin/Cancel. One backend callback owns CheckRender/Render.
// Cancel joins preparation, retaining every owner/page until destruction, which
// must follow backend quiescence. State queries/waits are safe on non-callback threads.
class PagedPreviewSource {
public:
  static std::shared_ptr<PagedPreviewSource>
  Create(const bms_parser::Chart &chart,
         std::shared_ptr<DecodedAssetCache> decodedAssets,
         std::uint64_t firstFrame44100, PagedPreviewGains gains = {});
  ~PagedPreviewSource();
  PagedPreviewSource(const PagedPreviewSource &) = delete;
  PagedPreviewSource &operator=(const PagedPreviewSource &) = delete;

  // Call once, after opening the backend, before allowing its callback to run.
  // Optional leading-silence removal prepares a new absolute starting position
  // before startupReady; later silence and the complete native cache are kept.
  bool Begin(int actualOutputRate, bool skipLeadingSilence = false);
  // Read-only full-callback preflight for output split across scratch chunks.
  // Data reports the available request clamped to known EOF; Pending/End do not
  // change mixer state or time. Callback-only, with the same realtime guarantees
  // as Render. Published readiness persists until Cancel/failure.
  [[nodiscard]] PagedPreviewRenderResult CheckRender(std::uint32_t frameCount) const noexcept;
  // Always clears the supplied stereo frames. Pending freezes all mixer state
  // and the frame clock. No allocation, locks, I/O, waits or notifications.
  PagedPreviewRenderResult Render(std::span<float> stereo,
                                  std::uint32_t frameCount) noexcept;
  [[nodiscard]] PagedPreviewState GetState() const;
  void WaitForChange(std::uint64_t observedRevision,
                     std::chrono::milliseconds timeout);
  void Cancel();

private:
  struct Impl;
  explicit PagedPreviewSource(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl;
};
} // namespace audio
