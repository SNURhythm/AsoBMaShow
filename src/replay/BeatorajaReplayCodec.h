#pragma once

#include "ReplayPlayback.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace replay {

struct ReplayChartDocument {
  ReplayPlaybackData playback;
  ReplayTimeBounds timeBounds;

  bool operator==(const ReplayChartDocument &) const = default;
};

struct ReplayCourseDocument {
  CourseReplayPlaybackData playback;
  std::vector<ReplayTimeBounds> timeBounds;

  bool operator==(const ReplayCourseDocument &) const = default;
};

struct ReplayDecodeContext {
  std::vector<int> stageKeyModes;
  std::vector<ReplayTimeBounds> stageTimeBounds;
};

enum class ReplayStageDecodeSource : std::uint8_t {
  Stock,
  AsoExtension,
};

struct ReplayDecodeOutcome {
  std::optional<ReplayChartDocument> chart;
  std::optional<ReplayCourseDocument> course;
  std::vector<ReplayStageDecodeSource> stageSources;
  bool stockOnly = false;
  bool unsupportedAsoExtension = false;
  std::string diagnostic;

  [[nodiscard]] std::optional<bool>
  replayPathHasUndefinedLongNotes() const noexcept;
};

class BeatorajaReplayCodec {
public:
  static constexpr int kCodecVersion = 3;

  explicit BeatorajaReplayCodec(ReplayLimits limits = kReplayLimits);

  [[nodiscard]] std::optional<std::vector<std::byte>>
  encodeChart(const ReplayChartDocument &replay,
              std::int64_t playedAtUnixMillis, std::string &diagnostic) const;

  [[nodiscard]] std::optional<std::vector<std::byte>>
  encodeCourse(const ReplayCourseDocument &replay,
               std::int64_t playedAtUnixMillis, std::string &diagnostic) const;

  [[nodiscard]] ReplayDecodeOutcome
  decode(std::span<const std::byte> encoded,
         const ReplayDecodeContext &context) const;

  [[nodiscard]] static std::optional<int>
  beatorajaKeyCode(const LogicalControl &control, int keyMode) noexcept;

  [[nodiscard]] static std::optional<LogicalControl>
  logicalControl(int keyCode, int keyMode) noexcept;

private:
  ReplayLimits limits_;
};

} // namespace replay
