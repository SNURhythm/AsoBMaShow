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
#include <vector>

namespace replay {

struct ParsedCourseReplayStageFacts {
  ReplayChartIdentity chart;
  int longNoteMode = 0;
  bool hasUndefinedLongNotes = false;
  std::optional<ReplayTimeBounds> timeBounds;

  bool operator==(const ParsedCourseReplayStageFacts &) const = default;
};

struct ParsedCourseReplayFacts {
  std::vector<ParsedCourseReplayStageFacts> stages;

  bool operator==(const ParsedCourseReplayFacts &) const = default;
};

[[nodiscard]] inline ParsedCourseReplayStageFacts
makeParsedCourseReplayStageFacts(
    const bms_parser::ChartMeta &parsedChart, int recordLongNoteMode,
    bool hasUndefinedLongNotes,
    std::optional<ReplayTimeBounds> timeBounds = std::nullopt) noexcept {
  const int authoredLongNoteMode =
      long_note_mode::normalizeValue(parsedChart.LnMode);
  return {
      .chart = {.md5 = parsedChart.MD5,
                .sha256 = parsedChart.SHA256,
                .keyMode = parsedChart.KeyMode},
      .longNoteMode = authoredLongNoteMode > long_note_mode::kUnknownValue
                          ? authoredLongNoteMode
                          : long_note_mode::normalizeValue(recordLongNoteMode),
      .hasUndefinedLongNotes = hasUndefinedLongNotes,
      .timeBounds = std::move(timeBounds),
  };
}

enum class CourseReplayContextState {
  Ready,
  InvalidRequest,
  ResultNotFound,
  ResultUnavailable,
  ResultInvalid,
  CourseShapeMismatch,
  StageMismatch,
  LongNoteModeMismatch,
  ReplayNotAttached,
  FileUserDeleted,
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

[[nodiscard]] inline ReplayState replayStateForCourseReplayContext(
    CourseReplayContextState state) noexcept {
  switch (state) {
  case CourseReplayContextState::Ready:
    return ReplayState::Verified;
  case CourseReplayContextState::FileCorrupt:
    return ReplayState::Corrupt;
  case CourseReplayContextState::UnsupportedExtension:
  case CourseReplayContextState::UnsupportedCodecVersion:
    return ReplayState::UnsupportedExtension;
  case CourseReplayContextState::CourseShapeMismatch:
  case CourseReplayContextState::StageMismatch:
  case CourseReplayContextState::LongNoteModeMismatch:
  case CourseReplayContextState::ReferenceMismatch:
  case CourseReplayContextState::FileUnsafe:
  case CourseReplayContextState::DecodeFailed:
  case CourseReplayContextState::ReplayInvalid:
  case CourseReplayContextState::SharedFactsMismatch:
    return ReplayState::Mismatched;
  case CourseReplayContextState::ReplayNotAttached:
  case CourseReplayContextState::FileMissing:
  case CourseReplayContextState::FileIoFailure:
    return ReplayState::Missing;
  case CourseReplayContextState::FileUserDeleted:
    return ReplayState::UserDeleted;
  case CourseReplayContextState::InvalidRequest:
  case CourseReplayContextState::ResultNotFound:
  case CourseReplayContextState::ResultUnavailable:
  case CourseReplayContextState::ResultInvalid:
    return ReplayState::NotApplicable;
  }
  return ReplayState::NotApplicable;
}

struct VerifiedCourseReplay {
  result_persistence::ModernCourseResult result;
  ModernReplayFileReference reference;
  ReplayCourseDocument document;
  std::vector<ReplayStageDecodeSource> stageSources;
  CoursePathInput pathInput;

  bool operator==(const VerifiedCourseReplay &) const = default;
};

struct CourseReplayContextOutcome {
  CourseReplayContextState state = CourseReplayContextState::InvalidRequest;
  std::optional<result_persistence::ModernCourseResult> result;
  std::optional<ModernReplayFileReference> reference;
  std::optional<VerifiedCourseReplay> verified;
  std::string diagnostic;

  [[nodiscard]] bool resultAvailable() const noexcept {
    return result.has_value();
  }
  [[nodiscard]] bool replayAvailable() const noexcept {
    return state == CourseReplayContextState::Ready && verified.has_value();
  }
  [[nodiscard]] ReplayState replayState() const noexcept {
    return replayStateForCourseReplayContext(state);
  }
};

struct CourseReplayContextDependencies {
  std::function<ModernCourseResultReadOutcome(std::string_view)> loadResult;
  std::function<ReplayFileReadOutcome(const ReplayFileMetadata &)>
      readVerifiedFile;
  std::function<ReplayDecodeOutcome(std::span<const std::byte>,
                                    const ReplayDecodeContext &)>
      decode;
};

class CourseReplayContext {
public:
  CourseReplayContext(ReplayRepository &repository,
                      ReplayLimits limits = kReplayLimits);
  explicit CourseReplayContext(CourseReplayContextDependencies dependencies,
                               ReplayLimits limits = kReplayLimits);

  [[nodiscard]] CourseReplayContextOutcome
  load(std::string_view attemptId,
       const ParsedCourseReplayFacts &parsedCourse) const noexcept;

private:
  CourseReplayContextDependencies dependencies_;
  ReplayLimits limits_;
};

} // namespace replay
