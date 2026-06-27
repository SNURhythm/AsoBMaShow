#pragma once

#include "CoursePlaySession.h"
#include "PlayOptionUtils.h"
#include "../yoga/lib/nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

enum class CourseConstraintType {
  Unknown = -1,
  Grade = 0,
  NoSpeed,
  Judgement,
  Gauge,
  LongNote,
};

inline constexpr std::size_t kCourseConstraintTypeCount = 5;

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

inline CourseConstraintType courseConstraintType(const std::string &constraint) {
  if (constraint == "grade" || constraint == "grade_mirror" ||
      constraint == "grade_random") {
    return CourseConstraintType::Grade;
  }
  if (constraint == "no_speed") {
    return CourseConstraintType::NoSpeed;
  }
  if (constraint == "no_good" || constraint == "no_great") {
    return CourseConstraintType::Judgement;
  }
  if (constraint == "gauge_lr2" || constraint == "gauge_5k" ||
      constraint == "gauge_7k" || constraint == "gauge_9k" ||
      constraint == "gauge_24k") {
    return CourseConstraintType::Gauge;
  }
  if (constraint == "ln" || constraint == "cn" || constraint == "hcn") {
    return CourseConstraintType::LongNote;
  }
  return CourseConstraintType::Unknown;
}

inline CourseConstraintSettings
courseConstraintSettingsFromJson(const std::string &constraintJson) {
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
  std::array<bool, kCourseConstraintTypeCount> seenTypes{};
  for (const auto &constraint : constraints) {
    const CourseConstraintType type = courseConstraintType(constraint);
    if (type == CourseConstraintType::Unknown) {
      continue;
    }
    const auto typeIndex = static_cast<std::size_t>(type);
    if (seenTypes[typeIndex]) {
      continue;
    }
    seenTypes[typeIndex] = true;

    if (type == CourseConstraintType::Grade) {
      settings.gradeConstraint = constraint;
      continue;
    }
    if (type == CourseConstraintType::NoSpeed) {
      settings.rules.noSpeed = true;
      continue;
    }
    if (type == CourseConstraintType::Judgement) {
      if (constraint == "no_good") {
        settings.rules.judgement = CourseJudgementConstraint::NoGood;
      } else if (constraint == "no_great") {
        settings.rules.judgement = CourseJudgementConstraint::NoGreat;
      }
      continue;
    }
    if (type == CourseConstraintType::Gauge) {
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
    if (type == CourseConstraintType::LongNote) {
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

inline std::string coursePlayOptionForConstraints(
    const std::string &selectedPlayOption,
    const CourseConstraintSettings &constraintSettings) {
  const std::string normalized =
      play_options::normalizePlayOption(selectedPlayOption);
  if (!constraintSettings.gradeConstraint.has_value()) {
    return normalized;
  }
  if (*constraintSettings.gradeConstraint == "grade") {
    return "NORMAL";
  }
  if (*constraintSettings.gradeConstraint == "grade_mirror" &&
      normalized != "MIRROR") {
    return "NORMAL";
  }
  return normalized;
}

inline bool coursePlayOptionLocksSelection(
    const CourseConstraintSettings &constraintSettings) {
  return constraintSettings.gradeConstraint.has_value() &&
         (*constraintSettings.gradeConstraint == "grade" ||
          *constraintSettings.gradeConstraint == "grade_mirror");
}

inline bool coursePlayOptionAllowedByConstraints(
    const std::string &option,
    const CourseConstraintSettings &constraintSettings) {
  if (!coursePlayOptionLocksSelection(constraintSettings)) {
    return true;
  }

  const std::string normalized = play_options::normalizePlayOption(option);
  if (*constraintSettings.gradeConstraint == "grade") {
    return normalized == "NORMAL";
  }
  if (*constraintSettings.gradeConstraint == "grade_mirror") {
    return normalized == "NORMAL" || normalized == "MIRROR";
  }
  return true;
}
