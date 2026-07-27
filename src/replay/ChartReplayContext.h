#pragma once

#include "BeatorajaReplayCodec.h"
#include "ReplayCapabilities.h"
#include "ReplayFileStore.h"

#include "../ModernResult.h"
#include "../repositories/ReplayRepository.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace replay {

struct ParsedChartReplayFacts {
  ReplayChartIdentity chart;
  int longNoteMode = 0;
  ReplayTimeBounds timeBounds;

  bool operator==(const ParsedChartReplayFacts &) const = default;
};

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
                     std::filesystem::path profileRoot,
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
