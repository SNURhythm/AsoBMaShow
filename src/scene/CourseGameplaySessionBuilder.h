#pragma once

#include "MainMenuProfileSelections.h"
#include "../CoursePlaySession.h"
#include "../repositories/ChartRepository.h"

#include <memory>
#include <string>
#include <vector>

struct CourseGameplaySessionRequest {
  int courseId = 0;
  std::string courseKey;
  std::string courseName;
  std::string courseGroupName;
  std::string constraintJson;
  std::vector<ChartMetaRecord> records;
  main_menu_profile::Selections selections;
  bool inputKeysoundEnabled = true;
};

[[nodiscard]] std::shared_ptr<CoursePlaySession>
buildCourseGameplaySession(CourseGameplaySessionRequest);
