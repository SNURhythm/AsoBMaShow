#pragma once

#include "CoursePlaySession.h"
#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

struct CourseConstraintSettings {
  GaugeProfile gaugeProfile = GaugeProfile::Standard;
  CourseConstraintRules rules;
  std::optional<std::string> gradeConstraint;
};

inline std::string normalizeCourseConstraintName(std::string name) {
  name.erase(name.begin(),
             std::find_if(name.begin(), name.end(), [](unsigned char c) {
               return std::isspace(c) == 0;
             }));
  name.erase(std::find_if(name.rbegin(), name.rend(), [](unsigned char c) {
               return std::isspace(c) == 0;
             }).base(),
             name.end());
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    if (c == '-') {
      return '_';
    }
    return static_cast<char>(std::tolower(c));
  });
  return name;
}

inline void collectCourseConstraintNames(const nlohmann::json &value,
                                         std::vector<std::string> &names) {
  if (value.is_string()) {
    names.push_back(normalizeCourseConstraintName(value.get<std::string>()));
    return;
  }
  if (value.is_array()) {
    for (const auto &item : value) {
      collectCourseConstraintNames(item, names);
    }
  }
}

inline int courseConstraintType(const std::string &constraint) {
  if (constraint == "grade" || constraint == "grade_mirror" ||
      constraint == "grade_random") {
    return 0;
  }
  if (constraint == "no_speed") {
    return 1;
  }
  if (constraint == "no_good" || constraint == "no_great") {
    return 2;
  }
  if (constraint == "gauge_lr2" || constraint == "gauge_5k" ||
      constraint == "gauge_7k" || constraint == "gauge_9k" ||
      constraint == "gauge_24k") {
    return 3;
  }
  if (constraint == "ln" || constraint == "cn" || constraint == "hcn") {
    return 4;
  }
  return -1;
}

inline CourseConstraintSettings
courseConstraintSettingsFromJsonShared(const std::string &constraintJson) {
  CourseConstraintSettings settings;
  if (constraintJson.empty()) {
    return settings;
  }
  const auto parsed = nlohmann::json::parse(constraintJson, nullptr, false);
  if (parsed.is_discarded()) {
    return settings;
  }

  std::vector<std::string> constraints;
  collectCourseConstraintNames(parsed, constraints);
  std::array<bool, 5> seenTypes{};
  for (const auto &constraint : constraints) {
    const int type = courseConstraintType(constraint);
    if (type < 0 || seenTypes[static_cast<size_t>(type)]) {
      continue;
    }
    seenTypes[static_cast<size_t>(type)] = true;

    if (type == 0) {
      settings.gradeConstraint = constraint;
      continue;
    }
    if (type == 1) {
      settings.rules.noSpeed = true;
      continue;
    }
    if (type == 2) {
      if (constraint == "no_good") {
        settings.rules.judgement = CourseJudgementConstraint::NoGood;
      } else if (constraint == "no_great") {
        settings.rules.judgement = CourseJudgementConstraint::NoGreat;
      }
      continue;
    }
    if (type == 3) {
      if (constraint == "gauge_5k") {
        settings.gaugeProfile = GaugeProfile::Course5Keys;
      } else if (constraint == "gauge_7k") {
        settings.gaugeProfile = GaugeProfile::Course7Keys;
      } else if (constraint == "gauge_9k") {
        settings.gaugeProfile = GaugeProfile::Course9Keys;
      } else if (constraint == "gauge_24k") {
        settings.gaugeProfile = GaugeProfile::Course24Keys;
      } else if (constraint == "gauge_lr2") {
        settings.gaugeProfile = GaugeProfile::CourseLR2;
      }
      continue;
    }
    if (type == 4) {
      if (constraint == "ln") {
        settings.rules.longNoteMode = CourseLongNoteMode::LN;
      } else if (constraint == "cn") {
        settings.rules.longNoteMode = CourseLongNoteMode::CN;
      } else if (constraint == "hcn") {
        settings.rules.longNoteMode = CourseLongNoteMode::HCN;
      }
    }
  }

  if (settings.gaugeProfile == GaugeProfile::Standard) {
    settings.gaugeProfile = GaugeProfile::CourseDefault;
  }
  return settings;
}
