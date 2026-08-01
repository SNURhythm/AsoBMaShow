// ReSharper disable StringLiteralTypo
// ReSharper disable IdentifierTypo
#pragma once
#include "../../bms_parser.hpp"
#include "Judgement.h"
#include "NoteTimeRange.h"
#include <map>
#include <optional>
#include <string>

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
  std::optional<NoteTimeRange> allowedNoteRange;

public:
  std::map<Judgement, std::pair<long long, long long>> timingWindows;
  explicit Judge(int Rank);
  void applyCourseJudgementConstraint(CourseJudgementConstraint constraint);
  void applyWindowScale(int playbackRatePercent, int judgeScalePercent);
  void setAllowedNoteRange(std::optional<NoteTimeRange> range);
  [[nodiscard]] bool allowsNote(const bms_parser::Note *note) const;
  static bool checkRange(long long Diff, long long Early, long long Late);
  JudgeResult judgeNow(const bms_parser::Note *Note, long long InputTime);
  static int clampRank(int rank);
  static std::string getRankDescription(int Rank);
};
