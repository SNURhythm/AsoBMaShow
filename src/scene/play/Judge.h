// ReSharper disable StringLiteralTypo
// ReSharper disable IdentifierTypo
#pragma once
#include <string>
#include <map>
#include "../../bms_parser.hpp"
enum Judgement { PGreat, Great, Good, Bad, Kpoor, Poor, None, JudgementCount };

enum class CourseJudgementConstraint { None, NoGood, NoGreat };

class JudgeResult {
public:
  JudgeResult(Judgement Judgement, long long Diff)
      : judgement(Judgement), Diff(Diff) {}

  Judgement judgement = None;
  long long Diff;

  [[nodiscard]] bool isComboBreak() const {
    return judgement == Bad || judgement == Poor;
  }

  [[nodiscard]] bool isNotePlayed() const {
    return judgement != Kpoor && judgement != None;
  }

  [[nodiscard]] std::string toString() const {
    switch (judgement) {
    case PGreat:
      return "PGREAT";
    case Great:
      return "GREAT";
    case Good:
      return "GOOD";
    case Bad:
      return "BAD";
    case Kpoor:
      return "KPOOR";
    case Poor:
      return "POOR";
    case None:
      return "NONE";
    default:
      return "NONE";
    }
  }
};

class Judge {
private:
  // dictionary for timing windows. JudgeRank -> {Judgement -> (early, late)}
  inline static const std::map<Judgement, std::pair<long long, long long>>
      TimingWindowsByRank[4] = {
          std::map<Judgement, std::pair<long long, long long>>{
              {PGreat, std::pair<long long, long long>(-5000, 5000)},
              {Great, std::pair<long long, long long>(-15000, 15000)},
              {Good, std::pair<long long, long long>(-37500, 37500)},
              {Bad, std::pair<long long, long long>(-385000, 490000)},
              {Kpoor, std::pair<long long, long long>(-500000, 150000)}},
          std::map<Judgement, std::pair<long long, long long>>{
              {PGreat, std::pair<long long, long long>(-10000, 10000)},
              {Great, std::pair<long long, long long>(-30000, 30000)},
              {Good, std::pair<long long, long long>(-75000, 75000)},
              {Bad, std::pair<long long, long long>(-330000, 420000)},
              {Kpoor, std::pair<long long, long long>(-500000, 150000)}},
          std::map<Judgement, std::pair<long long, long long>>{
              {PGreat, std::pair<long long, long long>(-15000, 15000)},
              {Great, std::pair<long long, long long>(-45000, 45000)},
              {Good, std::pair<long long, long long>(-112500, 112500)},
              {Bad, std::pair<long long, long long>(-275000, 350000)},
              {Kpoor, std::pair<long long, long long>(-500000, 150000)}},
          std::map<Judgement, std::pair<long long, long long>>{
              {PGreat, std::pair<long long, long long>(-20000, 20000)},
              {Great, std::pair<long long, long long>(-60000, 60000)},
              {Good, std::pair<long long, long long>(-150000, 150000)},
              {Bad, std::pair<long long, long long>(-220000, 280000)},
              {Kpoor, std::pair<long long, long long>(-500000, 150000)}}};

public:
  std::map<Judgement, std::pair<long long, long long>> timingWindows;
  explicit Judge(int Rank);
  void applyCourseJudgementConstraint(CourseJudgementConstraint constraint);
  void applyWindowScale(int playbackRatePercent, int judgeScalePercent);
  static bool checkRange(long long Diff, long long Early, long long Late);
  JudgeResult judgeNow(const bms_parser::Note *Note, long long InputTime);
  static int clampRank(int rank);
  static std::string getRankDescription(int Rank);
};
