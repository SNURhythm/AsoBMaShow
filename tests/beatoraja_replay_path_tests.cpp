#include "replay/BeatorajaReplayPath.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

constexpr std::string_view kShaA =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kShaB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

std::optional<replay::ReplayPathIdentity>
chartPath(std::string_view sha256, int longNoteMode, bool hasUndefinedLongNotes,
          std::int64_t historyIndex, std::string &diagnostic) {
  const auto stem = replay::chartStem(sha256, longNoteMode,
                                      hasUndefinedLongNotes, diagnostic);
  if (!stem) {
    return std::nullopt;
  }
  return replay::pathForStem(*stem, historyIndex, diagnostic);
}

void expectPath(const std::optional<replay::ReplayPathIdentity> &actual,
                std::string_view expectedStem, std::int64_t expectedIndex,
                const std::filesystem::path &expectedPath,
                std::string_view message) {
  expect(actual.has_value(), message);
  if (!actual) {
    return;
  }
  expect(actual->stem == expectedStem, "path identity preserves exact stem");
  expect(actual->historyIndex == expectedIndex,
         "path identity preserves history index");
  expect(actual->relativePath == expectedPath,
         "path identity uses Beatoraja replay layout");
}

void testChartPathsMatchBeatorajaGrammar() {
  std::string diagnostic;
  expectPath(chartPath(kShaA, 0, false, 0, diagnostic), kShaA, 0,
             std::filesystem::path("replay") /
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaa.brd",
             "ordinary chart path is produced");

  diagnostic.clear();
  expectPath(
      chartPath(kShaA, 1, true, 1, diagnostic),
      "Caaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 1,
      std::filesystem::path("replay") / "Caaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaa_1.brd",
      "undefined-LN CN path is produced");

  diagnostic.clear();
  expectPath(
      chartPath(kShaA, 2, true, 27, diagnostic),
      "Haaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", 27,
      std::filesystem::path("replay") / "Haaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                                        "aaaaaaaaaaaaaaaaaaaaaaaaaaa_27.brd",
      "undefined-LN HCN path is produced");

  diagnostic.clear();
  expectPath(chartPath(kShaA, 2, false, 4, diagnostic), kShaA, 4,
             std::filesystem::path("replay") /
                 "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                 "aaa_4.brd",
             "non-undefined chart ignores LN interpretation prefix");
}

void testCoursePathMatchesBeatorajaGrammar() {
  replay::CoursePathInput input{
      .stageSha256 = {std::string(kShaA), std::string(kShaB)},
      .longNoteMode = 1,
      .hasUndefinedLongNotes = true,
      .beatorajaConstraintIds = {1, 4, 7, 13, 2, 3},
  };
  std::string diagnostic;
  const auto stem = replay::courseStem(input, diagnostic);
  expect(stem == "Caaaaaaaaaabbbbbbbbbb_040713",
         "course stem joins ten-character hashes, filters grade markers, "
         "and preserves two-digit constraint order");
  if (!stem) {
    return;
  }
  expectPath(replay::pathForStem(*stem, 4, diagnostic), *stem, 4,
             std::filesystem::path("replay") /
                 "Caaaaaaaaaabbbbbbbbbb_040713_4.brd",
             "course path uses the same positive history suffix");

  input.hasUndefinedLongNotes = false;
  input.beatorajaConstraintIds.clear();
  diagnostic.clear();
  expect(replay::courseStem(input, diagnostic) == "aaaaaaaaaabbbbbbbbbb",
         "course without undefined LN or constraints has no separators");
}

void testMalformedIdentityCannotProducePath() {
  std::string diagnostic;
  expect(!replay::chartStem("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                            "AAAAAAAAAAAAAAAA",
                            0, false, diagnostic),
         "uppercase chart SHA-256 is rejected");
  expect(!diagnostic.empty(), "invalid chart hash reports a diagnostic");

  diagnostic.clear();
  expect(!replay::chartStem("abc", 0, false, diagnostic),
         "short chart SHA-256 is rejected");
  diagnostic.clear();
  expect(!replay::chartStem(kShaA, 3, true, diagnostic),
         "undefined-LN chart rejects unsupported LN mode");

  replay::CoursePathInput empty;
  diagnostic.clear();
  expect(!replay::courseStem(empty, diagnostic), "empty course is rejected");

  replay::CoursePathInput badConstraint{
      .stageSha256 = {std::string(kShaA)},
      .beatorajaConstraintIds = {15},
  };
  diagnostic.clear();
  expect(!replay::courseStem(badConstraint, diagnostic),
         "unknown Beatoraja constraint ID is rejected");

  replay::CoursePathInput tooManyStages;
  tooManyStages.stageSha256.assign(257, std::string(kShaA));
  diagnostic.clear();
  expect(!replay::courseStem(tooManyStages, diagnostic),
         "course stage count is bounded");

  for (const std::string_view unsafe :
       {"", ".", "..", "a/b", "a\\b", "name.brd", "_1"}) {
    diagnostic.clear();
    expect(!replay::pathForStem(unsafe, 0, diagnostic),
           "unsafe or non-canonical stem is rejected");
  }

  diagnostic.clear();
  expect(!replay::pathForStem(kShaA, -1, diagnostic),
         "negative history index is rejected");
  diagnostic.clear();
  expect(replay::pathForStem(kShaA, std::numeric_limits<std::int64_t>::max(),
                             diagnostic)
             .has_value(),
         "maximum signed history index formats without overflow");
}

void testReplayFilenameComponentsStayWithinFilesystemLimit() {
  replay::CoursePathInput largestSafeCourse;
  largestSafeCourse.stageSha256.assign(23, std::string(kShaA));
  std::string diagnostic;
  const auto largestSafeStem =
      replay::courseStem(largestSafeCourse, diagnostic);
  expect(largestSafeStem.has_value() &&
             replay::pathForStem(*largestSafeStem,
                                 std::numeric_limits<std::int64_t>::max(),
                                 diagnostic)
                 .has_value(),
         "23-stage course leaves room for the maximum history suffix");

  replay::CoursePathInput tooManyForFilename;
  tooManyForFilename.stageSha256.assign(24, std::string(kShaA));
  diagnostic.clear();
  expect(!replay::courseStem(tooManyForFilename, diagnostic),
         "24-stage course is rejected before producing an overlong filename");

  replay::CoursePathInput constrained;
  constrained.stageSha256.assign(22, std::string(kShaA));
  constrained.longNoteMode = 1;
  constrained.hasUndefinedLongNotes = true;
  constrained.beatorajaConstraintIds = {4, 5, 6, 7, 8};
  diagnostic.clear();
  expect(!replay::courseStem(constrained, diagnostic),
         "course filename budget includes LN prefix and constraints");

  diagnostic.clear();
  expect(!replay::pathForStem(std::string(252, 'a'), 0, diagnostic),
         "formatted replay path rejects a filename over 255 bytes");
}

} // namespace

int main() {
  testChartPathsMatchBeatorajaGrammar();
  testCoursePathMatchesBeatorajaGrammar();
  testMalformedIdentityCannotProducePath();
  testReplayFilenameComponentsStayWithinFilesystemLimit();
  if (failures != 0) {
    std::cerr << failures << " Beatoraja replay path test(s) failed\n";
    return 1;
  }
  std::cout << "Beatoraja replay path tests passed\n";
  return 0;
}
