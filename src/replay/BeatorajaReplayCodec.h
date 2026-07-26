#pragma once

#include "ReplayPlaybackData.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace replay {

struct ReplayCodecLimits {
  static constexpr std::size_t kMaximumCompressedBytes = 64U * 1024U * 1024U;

  std::size_t maxCompressedBytes = kMaximumCompressedBytes;
  std::size_t maxJsonBytes = 256U * 1024U * 1024U;
  std::size_t maxKeyInputBytes = 9U * 1'000'000U;
  std::size_t maxInputTransitions = 1'000'000U;
  std::size_t maxTouchSamples = 1'000'000U;
  std::size_t maxLaneCoverEvents = 100'000U;
  std::size_t maxJsonDepth = 64U;
};

struct ReplayDecodeOutcome {
  std::optional<ReplayPlaybackData> chart;
  std::optional<CourseReplayPlaybackData> course;
  bool stockOnly = false;
  bool unsupportedAsoExtension = false;
  std::string diagnostic;
};

class BeatorajaReplayCodec {
public:
  static constexpr int kCodecVersion = 2;

  explicit BeatorajaReplayCodec(ReplayCodecLimits limits = {});

  [[nodiscard]] std::optional<std::vector<std::byte>>
  encodeChart(const ReplayPlaybackData &replay, std::int64_t playedAtUnixMillis,
              std::string &diagnostic) const;

  [[nodiscard]] std::optional<std::vector<std::byte>>
  encodeCourse(const CourseReplayPlaybackData &replay,
               std::int64_t playedAtUnixMillis, std::string &diagnostic) const;

  [[nodiscard]] ReplayDecodeOutcome
  decode(std::span<const std::byte> encoded,
         std::optional<int> expectedKeyMode = std::nullopt) const;

  [[nodiscard]] ReplayDecodeOutcome
  decode(std::span<const std::byte> encoded,
         std::span<const int> expectedStageKeyModes) const;

  [[nodiscard]] static std::optional<int>
  beatorajaKeyCode(const LogicalControl &control, int keyMode) noexcept;

  [[nodiscard]] static std::optional<LogicalControl>
  logicalControl(int keyCode, int keyMode) noexcept;

private:
  ReplayCodecLimits limits_;
};

} // namespace replay
