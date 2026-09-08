#include "CourseGameplaySessionBuilder.h"

#include "../AssistOptionUtils.h"
#include "../CourseConstraintUtils.h"

std::shared_ptr<CoursePlaySession>
buildCourseGameplaySession(CourseGameplaySessionRequest request) {
  auto session = std::make_shared<CoursePlaySession>();
  session->courseId = request.courseId;
  session->courseKey = std::move(request.courseKey);
  session->courseName = std::move(request.courseName);
  session->courseGroupName = std::move(request.courseGroupName);
  session->constraintJson = std::move(request.constraintJson);
  session->entries.reserve(request.records.size());
  for (const auto &record : request.records) {
    session->entries.push_back(CoursePlayEntry{.meta = record.meta});
  }

  const CourseConstraintSettings constraints =
      courseConstraintSettingsFromJson(session->constraintJson);
  int longNoteMode =
      long_note_mode::valueFromId(request.selections.longNoteMode);
  if (constraints.rules.longNoteMode != CourseLongNoteMode::Unspecified) {
    longNoteMode = courseLongNoteModeToChartMetaValue(
        constraints.rules.longNoteMode);
  }
  session->currentIndex = 0;
  session->ruleset = request.selections.ruleset;
  session->rulesetDescriptor = RulesetDescriptor::For(session->ruleset);
  session->gaugeType = request.selections.gaugeType;
  session->gaugeProfile = constraints.gaugeProfile;
  session->gaugeAutoShift = request.selections.gaugeAutoShift;
  session->gaugeAutoShiftLowerBound =
      request.selections.gaugeAutoShiftLowerBound;
  session->longNoteMode = longNoteMode;
  session->constraints = constraints.rules;
  const auto playOptions = coursePlayOptionsForConstraints(
      request.selections.playOption, request.player2PlayOption,
      request.doublePlayFlip, constraints);
  session->requestedPlayOption = playOptions.player1;
  session->requestedPlayOption2 = playOptions.player2;
  session->doublePlayFlip = playOptions.doublePlayFlip;
  session->assistOption = assist_options::kOff;
  session->autoKeySound = !request.inputKeysoundEnabled;
  return session;
}
