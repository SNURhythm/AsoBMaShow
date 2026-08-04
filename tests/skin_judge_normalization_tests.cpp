#include "skin/beatoraja/SkinJudgeNormalization.h"

#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

SkinDestinationBody destination(int source, std::size_t frameCount = 1) {
  SkinDestinationBody body;
  body.authoredOrdinal = static_cast<std::uint32_t>(source);
  body.frames.reserve(frameCount);
  for (std::size_t frame = 0; frame < frameCount; ++frame) {
    body.frames.push_back(
        {.timeMillis = static_cast<int>(frame),
         .x = static_cast<double>(source),
         .y = 2.0,
         .width = 3.0,
         .height = 4.0});
  }
  return body;
}

SkinJudgeInlineImageChild imageChild(std::string id, std::size_t index,
                                     int source,
                                     std::size_t frameCount = 1) {
  SkinJudgeInlineImageChild child;
  child.authoredId = std::move(id);
  child.authoredIndex = index;
  child.image.orderedStates = {{.resource =
                                    static_cast<SkinResourceId>(1000 + source),
                                .frames = {{.x = source,
                                            .y = 0,
                                            .w = 10,
                                            .h = 20}}}};
  child.destination = destination(source, frameCount);
  return child;
}

SkinJudgeInlineNumberChild numberChild(std::string id, std::size_t index,
                                       int source,
                                       std::size_t frameCount = 1) {
  SkinJudgeInlineNumberChild child;
  child.authoredId = std::move(id);
  child.authoredIndex = index;
  child.presentation.number.value = SkinIntegerPropertyId{77};
  child.presentation.number.digitCount = 3;
  child.presentation.number.digits.positive = {
      .resource = static_cast<SkinResourceId>(2000 + source),
      .frames = {{.x = source, .y = 0, .w = 10, .h = 20}},
  };
  child.presentation.destination = destination(source, frameCount);
  return child;
}

SkinJudgeNormalizationInput completeInput(std::size_t gradeCount = 3) {
  SkinJudgeNormalizationInput input;
  input.player = 2;
  input.shiftImageByHalfDetailWidth = true;
  for (std::size_t grade = 0; grade < gradeCount; ++grade) {
    input.images.push_back(imageChild("image-" + std::to_string(grade), grade,
                                      10 + static_cast<int>(grade)));
    input.numbers.push_back(numberChild("number-" + std::to_string(grade),
                                        grade, 20 + static_cast<int>(grade)));
  }
  return input;
}

void testSparseGradesPreserveIndependentChildrenAndOuterFields() {
  auto input = completeInput(4);
  input.images[1] = std::nullopt;
  input.numbers[2] = std::nullopt;

  const auto result = normalizeSkinJudge(input);
  expect(result.judge.has_value(), "sparse Judge grades normalize");
  expect(result.error == SkinJudgeNormalizationError::None,
         "sparse Judge grades report no error");
  if (!result.judge) {
    return;
  }

  const auto &judge = *result.judge;
  expect(judge.player == 2 && judge.shiftImageByHalfDetailWidth,
         "Judge preserves player and shift outer fields");
  expect(judge.grades.size() == SkinJudgeNormalizationPolicy::runtimeGradeSlots,
         "Judge retains the runtime's seven grade slots");
  expect(judge.grades[0].image && judge.grades[0].detailNumber &&
             judge.grades[0].image->authoredId == "image-0" &&
             judge.grades[0].image->authoredIndex == 0 &&
             judge.grades[0].image->image.orderedStates.front().resource ==
                 1010 &&
             judge.grades[0].detailNumber->authoredId == "number-0" &&
             judge.grades[0].detailNumber->authoredIndex == 0 &&
             judge.grades[0].detailNumber->presentation.number.digitCount == 3,
         "first grade retains inline payloads and authored IDs/indexes");
  expect(!judge.grades[1].image && judge.grades[1].detailNumber &&
             judge.grades[1].detailNumber->authoredId == "number-1",
         "missing image does not discard its independently resolved number");
  expect(judge.grades[2].image && !judge.grades[2].detailNumber &&
             judge.grades[2].image->authoredId == "image-2",
         "missing number does not discard its independently resolved image");
  expect(!judge.grades[4].image && !judge.grades[4].detailNumber,
         "unwritten runtime grades remain sparse holes");
  expect(judge.grades[3].image &&
             judge.grades[3].image->destination.authoredOrdinal == 13 &&
             judge.grades[3].detailNumber &&
             judge.grades[3].detailNumber->presentation.destination
                     .authoredOrdinal ==
                 23,
         "child destinations retain their authored presentation metadata");
}

void testRuntimeMetadataCapturesPinnedFallbackAndCountVisibility() {
  const auto result = normalizeSkinJudge(completeInput(7));
  expect(result.judge.has_value(), "seven authored Judge grades normalize");
  if (!result.judge) {
    return;
  }

  const auto &runtime = result.judge->runtime;
  expect(runtime.maxGaugePreferredGrade == 6 &&
             runtime.maxGaugeFallbackGrade == 0,
         "max gauge metadata chooses grade six before grade zero");
  expect(runtime.detailNumberVisibleGradeCount == 3,
         "detail number metadata limits normal visibility to grades zero through two");
}

void testEighthGradeIsIgnoredWithoutReplacingRuntimeSlots() {
  auto input = completeInput(8);
  const auto result = normalizeSkinJudge(input);
  expect(result.judge.has_value(), "eight authored Judge grades normalize");
  expect(result.ignoredAuthoredGrades == 1,
         "the eighth grade is reported as ignored by SkinJudge's seven slots");
  if (!result.judge) {
    return;
  }
  expect(result.judge->grades[6].image &&
             result.judge->grades[6].image->authoredId == "image-6" &&
             result.judge->grades[6].detailNumber &&
             result.judge->grades[6].detailNumber->authoredId == "number-6",
         "the seventh authored children remain in runtime slot six");
}

void testUnsafeArraysAndNestedDestinationsFailClosed() {
  {
    auto input = completeInput(2);
    input.numbers.pop_back();
    const auto result = normalizeSkinJudge(input);
    expect(!result.judge &&
               result.error == SkinJudgeNormalizationError::UnsafeCardinality,
           "numbers shorter than images fail closed before grade indexing");
  }
  {
    auto input = completeInput(
        SkinJudgeNormalizationPolicy::maxAuthoredGrades + 1);
    const auto result = normalizeSkinJudge(input);
    expect(!result.judge &&
               result.error == SkinJudgeNormalizationError::GradeLimitExceeded,
           "over-budget authored grade arrays fail closed");
  }
  {
    auto input = completeInput();
    input.images.front() = imageChild(
        "image-0", 0, 10,
        SkinJudgeNormalizationPolicy::maxDestinationFramesPerChild + 1);
    const auto result = normalizeSkinJudge(input);
    expect(!result.judge &&
               result.error == SkinJudgeNormalizationError::FrameLimitExceeded,
           "over-budget image destination frames fail closed");
  }
  {
    auto input = completeInput();
    input.numbers.front()->presentation.destination.frames.front().width =
        std::numeric_limits<double>::infinity();
    const auto result = normalizeSkinJudge(input);
    expect(!result.judge &&
               result.error == SkinJudgeNormalizationError::NonFiniteGeometry,
           "non-finite number destination geometry fails closed");
  }
}

void testNumbersBeyondImageArrayAreNeverResolved() {
  auto input = completeInput(1);
  input.numbers.push_back(numberChild(
      "unused-number", 1, 21,
      SkinJudgeNormalizationPolicy::maxDestinationFramesPerChild + 1));

  const auto result = normalizeSkinJudge(input);
  expect(result.judge.has_value() &&
             result.error == SkinJudgeNormalizationError::None,
         "numbers beyond images.length are ignored instead of normalized");
  if (!result.judge) {
    return;
  }
  expect(result.judge->grades[0].detailNumber &&
             result.judge->grades[0].detailNumber->authoredId == "number-0" &&
             !result.judge->grades[1].detailNumber,
         "only the image-indexed number child reaches a runtime grade slot");
}

} // namespace

int main() {
  testSparseGradesPreserveIndependentChildrenAndOuterFields();
  testRuntimeMetadataCapturesPinnedFallbackAndCountVisibility();
  testEighthGradeIsIgnoredWithoutReplacingRuntimeSlots();
  testUnsafeArraysAndNestedDestinationsFailClosed();
  testNumbersBeyondImageArrayAreNeverResolved();
  return failures == 0 ? 0 : 1;
}
