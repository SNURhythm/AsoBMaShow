#pragma once

#include "BeatorajaReplayCodec.h"
#include "ReplayCapabilities.h"
#include "ReplayFileStore.h"

#include "../LongNoteModeUtils.h"
#include "../ModernResult.h"
#include "../repositories/ReplayRepository.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace replay {

struct ParsedChartReplayFacts {
  ReplayChartIdentity chart;
  int longNoteMode = 0;
  std::optional<ReplayTimeBounds> timeBounds;

  bool operator==(const ParsedChartReplayFacts &) const = default;
};

// Projects the selected parsed chart into the one identity/LN/time input used
// by replay availability checks and activated consumers. An authored LN mode
// wins; records provide the fallback only for undefined-LN charts.
[[nodiscard]] inline ParsedChartReplayFacts makeParsedChartReplayFacts(
    const bms_parser::ChartMeta &parsedChart, int recordLongNoteMode,
    std::optional<ReplayTimeBounds> timeBounds = std::nullopt) noexcept {
  const int authoredLongNoteMode =
      long_note_mode::normalizeValue(parsedChart.LnMode);
  return {
      .chart = {.md5 = parsedChart.MD5,
                .sha256 = parsedChart.SHA256,
                .keyMode = parsedChart.KeyMode},
      .longNoteMode =
          authoredLongNoteMode > long_note_mode::kUnknownValue
              ? authoredLongNoteMode
              : long_note_mode::normalizeValue(recordLongNoteMode),
      .timeBounds = std::move(timeBounds),
  };
}

enum class ChartReplayContextState {
  Ready,
  InvalidRequest,
  ResultNotFound,
  ResultUnavailable,
  ResultInvalid,
  ChartMismatch,
  LongNoteModeMismatch,
  ReplayNotAttached,
  ReferenceMismatch,
  UnsupportedCodecVersion,
  FileMissing,
  FileCorrupt,
  FileUnsafe,
  FileIoFailure,
  DecodeFailed,
  UnsupportedExtension,
  ReplayInvalid,
  SharedFactsMismatch,
};

struct VerifiedChartReplay {
  result_persistence::ModernChartResult result;
  ModernReplayFileReference reference;
  ReplayChartDocument document;
  ReplayStageDecodeSource source = ReplayStageDecodeSource::AsoExtension;

  bool operator==(const VerifiedChartReplay &) const = default;
};

struct ChartReplayContextOutcome {
  ChartReplayContextState state = ChartReplayContextState::InvalidRequest;
  std::optional<result_persistence::ModernChartResult> result;
  std::optional<ModernReplayFileReference> reference;
  std::optional<VerifiedChartReplay> verified;
  std::string diagnostic;

  [[nodiscard]] bool resultAvailable() const noexcept {
    return result.has_value();
  }
  [[nodiscard]] bool replayAvailable() const noexcept {
    return state == ChartReplayContextState::Ready && verified.has_value();
  }
  [[nodiscard]] ReplayState replayState() const noexcept;
};

struct ChartReplayContextDependencies {
  std::function<ModernChartResultReadOutcome(std::string_view)> loadResult;
  std::function<ReplayFileReadOutcome(const ReplayFileMetadata &)>
      readVerifiedFile;
  std::function<ReplayDecodeOutcome(std::span<const std::byte>,
                                    const ReplayDecodeContext &)>
      decode;
};

class ChartReplayContext {
public:
  ChartReplayContext(ReplayRepository &repository,
                     ReplayLimits limits = kReplayLimits);
  explicit ChartReplayContext(ChartReplayContextDependencies dependencies,
                              ReplayLimits limits = kReplayLimits);

  [[nodiscard]] ChartReplayContextOutcome
  load(std::string_view attemptId,
       const ParsedChartReplayFacts &parsedChart) const noexcept;

private:
  ChartReplayContextDependencies dependencies_;
  ReplayLimits limits_;
};

} // namespace replay
