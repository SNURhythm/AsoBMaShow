#include "replay/BeatorajaLongNoteMode.h"
#include "replay/BeatorajaReplayPath.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kShaA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kShaB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testChartPathMatchesStockGrammar() {
  std::string diagnostic;
  expect(replay::chartStem(kShaA, 1, false, diagnostic) == kShaA,
         "ordinary chart stem is its full SHA-256");
  expect(replay::chartStem(kShaA, 2, true, diagnostic) ==
             std::string("C") + std::string(kShaA),
         "undefined CN chart receives the stock C prefix");
  expect(replay::chartStem(kShaA, 3, true, diagnostic) ==
             std::string("H") + std::string(kShaA),
         "undefined HCN chart receives the stock H prefix");

  const auto first = replay::pathForStem(kShaA, 0, diagnostic);
  expect(first && first->relativePath == std::filesystem::path("replay") /
                                             (std::string(kShaA) + ".brd"),
         "slot zero uses the unsuffixed Beatoraja filename");
  const auto history = replay::pathForStem(kShaA, 17, diagnostic);
  expect(history &&
             history->relativePath == std::filesystem::path("replay") /
                                          (std::string(kShaA) + "_17.brd"),
         "history slot uses the Beatoraja numeric suffix");
}

void testCoursePathAndConstraintProjection() {
  replay::CoursePathInput course{
      .stageSha256 = {std::string(kShaA), std::string(kShaB)},
      .longNoteMode = 2,
      .hasUndefinedLongNotes = true,
      .beatorajaConstraintIds = {1, 2, 3, 4, 9, 14},
  };
  std::string diagnostic;
  const auto stem = replay::courseStem(course, diagnostic);
  expect(stem == "Caaaaaaaaaabbbbbbbbbb_040914",
         "course stem uses ten hash characters and playable constraints");
  expect(stem && replay::pathForStem(*stem, 3, diagnostic)->relativePath ==
                     std::filesystem::path("replay") /
                         "Caaaaaaaaaabbbbbbbbbb_040914_3.brd",
         "course stem and numeric slot compose under replay directory");

  course.beatorajaConstraintIds = {0};
  expect(!replay::courseStem(course, diagnostic),
         "constraint below stock range is rejected");
  course.beatorajaConstraintIds = {15};
  expect(!replay::courseStem(course, diagnostic),
         "constraint above stock range is rejected");
}

void testPlaybackStageLimitIsNotAPathLimit() {
  replay::CoursePathInput structurallyValid;
  structurallyValid.stageSha256.assign(replay::kReplayLimits.maxCourseStages,
                                       std::string(kShaA));
  std::string diagnostic;
  expect(!replay::courseStem(structurallyValid, diagnostic) &&
             diagnostic.find("filename") != std::string::npos,
         "256-stage playback is rejected only by filename eligibility");

  replay::CoursePathInput structurallyInvalid = structurallyValid;
  structurallyInvalid.stageSha256.emplace_back(kShaB);
  expect(!replay::courseStem(structurallyInvalid, diagnostic) &&
             diagnostic.find("stage count") != std::string::npos,
         "stage count above the shared playback limit is diagnosed first");

  replay::CoursePathInput largestSafe;
  largestSafe.stageSha256.assign(23, std::string(kShaA));
  const auto stem = replay::courseStem(largestSafe, diagnostic);
  expect(stem && replay::pathForStem(*stem,
                                     std::numeric_limits<std::int64_t>::max(),
                                     diagnostic),
         "23-stage course leaves room for maximum numeric history index");

  replay::CoursePathInput tooLong;
  tooLong.stageSha256.assign(24, std::string(kShaA));
  expect(!replay::courseStem(tooLong, diagnostic),
         "24-stage stock stem cannot fit the reserved filename suffix");
}

void testMalformedAndUnsafeIdentityFailsClosed() {
  std::string diagnostic;
  for (const std::string_view hash : {"", "ABCDEF", "../"}) {
    expect(!replay::chartStem(hash, 1, false, diagnostic),
           "non-canonical chart hash cannot form a stem");
  }
  expect(!replay::chartStem(kShaA, 0, true, diagnostic),
         "undefined long notes require an effective mode");
  expect(!replay::chartStem(kShaA, 4, false, diagnostic),
         "unsupported long-note mode cannot form a stem");

  for (const std::string_view unsafe :
       {"", ".", "..", "a/b", "a\\b", "name.brd", "_1"}) {
    expect(!replay::pathForStem(unsafe, 0, diagnostic),
           "unsafe or non-canonical stem is rejected");
  }
  expect(!replay::pathForStem(kShaA, -1, diagnostic),
         "negative history slot is rejected");
  expect(!replay::pathForStem(std::string(252, 'a'), 0, diagnostic),
         "formatted filename cannot exceed the shared byte limit");
}

void testRelativePathValidationSharesBuilderGrammar() {
  std::string diagnostic;
  const auto stem = replay::chartStem(kShaA, 1, false, diagnostic);
  const auto canonical = replay::pathForStem(*stem, 17, diagnostic);
  expect(canonical && replay::isCanonicalReplayRelativePath(
                          canonical->relativePath, diagnostic),
         "builder output is accepted by the shared relative-path validator");
  for (const auto &unsafe :
       {std::filesystem::path("replay/not-a-stock-stem.brd"),
        std::filesystem::path("../replay/") / (std::string(kShaA) + ".brd"),
        std::filesystem::path("replay/nested") / (std::string(kShaA) + ".brd"),
        std::filesystem::absolute(std::string(kShaA) + ".brd")}) {
    expect(!replay::isCanonicalReplayRelativePath(unsafe, diagnostic),
           "metadata path outside builder grammar is rejected");
  }
}

void testStemMatchingUsesParsedLongNoteContext() {
  std::string diagnostic;
  expect(
      replay::chartStemMatches(std::string("C") + std::string(kShaA), kShaA, 2,
                               std::nullopt, diagnostic) &&
          replay::chartStemMatches(kShaA, kShaA, 2, std::nullopt, diagnostic),
      "unknown authored-LN context accepts either stock-compatible stem");
  expect(!replay::chartStemMatches(std::string("C") + std::string(kShaA), kShaA,
                                   2, false, diagnostic) &&
             !replay::chartStemMatches(kShaA, kShaA, 2, true, diagnostic),
         "known parsed context requires exact prefix agreement");

  replay::CoursePathInput course{
      .stageSha256 = {std::string(kShaA), std::string(kShaB)},
      .longNoteMode = 3,
  };
  expect(replay::courseStemMatches("Haaaaaaaaaabbbbbbbbbb", course,
                                   std::nullopt, diagnostic) &&
             replay::courseStemMatches("aaaaaaaaaabbbbbbbbbb", course,
                                       std::nullopt, diagnostic),
         "unknown course LN context accepts both compatible stems");
  expect(!replay::courseStemMatches("Haaaaaaaaaabbbbbbbbbb", course, false,
                                    diagnostic),
         "known course LN context rejects a contradictory prefix");
}

void testUndefinedLongNoteDetectionIncludesBackspins() {
  expect(replay::hasUndefinedLongNotesForReplay(0, 0, 1),
         "undefined backspin requires a mode-specific path");
  expect(replay::hasUndefinedLongNotesForReplay(0, 1, 0),
         "undefined keyboard LN requires a mode-specific path");
  expect(!replay::hasUndefinedLongNotesForReplay(0, 0, 0),
         "chart without long notes has no undefined interpretation");
  expect(!replay::hasUndefinedLongNotesForReplay(1, 1, 1),
         "authored mode removes undefined interpretation");
}

} // namespace

int main() {
  testChartPathMatchesStockGrammar();
  testCoursePathAndConstraintProjection();
  testPlaybackStageLimitIsNotAPathLimit();
  testMalformedAndUnsafeIdentityFailsClosed();
  testRelativePathValidationSharesBuilderGrammar();
  testStemMatchingUsesParsedLongNoteContext();
  testUndefinedLongNoteDetectionIncludesBackspins();
  if (failures != 0) {
    std::cerr << failures << " Beatoraja replay path test(s) failed\n";
    return 1;
  }
  std::cout << "Beatoraja replay path tests passed\n";
  return 0;
}
