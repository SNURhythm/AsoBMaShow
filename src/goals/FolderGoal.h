#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace folder_goals {
enum class SourceKind { Directory, Table, Level };
enum class Type { Lamp, AverageEx, ExCoverage };
inline constexpr std::array<int,9> scoreRanks{6,9,12,15,18,21,24,26,27};
struct Source {
  SourceKind kind = SourceKind::Directory;
  std::string identity, level, name;
  bool operator==(const Source &) const = default;
};
struct Target {
  Type type = Type::Lamp;
  int lamp = 200;
  int percent = 10000; // Hundredths of one percent for lamp/custom targets.
  int scoreRank = 0; // EX targets: numerator over 27; zero uses a custom rate.
  int exPercent = 10000; // Coverage minimum EX rate, in hundredths of one percent.
  bool operator==(const Target &) const = default;
};
struct Goal {
  Source source;
  int longNoteMode = 1;
  Target target;
  std::int64_t revision = 1, setAt = 0, achievedAt = 0;
  std::string id; // Empty for a new draft; legacy rows retain their original key.
};
struct Member {
  std::string identity;
  bool available = true;
  int lamp = -1, score = 0, maxScore = 0;
};
struct Progress {
  std::int64_t available = 0, missing = 0, qualifying = 0, required = 0, unknownScore = 0;
  double percent = 0;
  bool achieved = false, sourceAvailable = true;
};
struct GoalProgress { Goal goal; Progress progress; };
double scoreTargetPercent(const Target &);
double targetPercent(const Target &);
void validate(const Target &);
void validate(const Goal &);
std::string sourceKey(const Source &);
std::string scopeKey(const Goal &);
std::string key(const Goal &);
Progress evaluate(const Target &, std::span<const Member>);
std::string percentText(double percent);
} // namespace folder_goals
