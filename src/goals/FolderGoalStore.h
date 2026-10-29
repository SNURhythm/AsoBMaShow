#pragma once
#include "FolderGoal.h"
#include <array>
#include <map>
#include <optional>
#include <unordered_map>
struct sqlite3;
namespace folder_goals {
std::vector<Goal> load(sqlite3 *);
void save(sqlite3 *, Goal &);
void remove(sqlite3 *, const Goal &);
struct EvidenceRow { int lamp = -1, score = 0, maxScore = 0; bool hasScore = false; };
using Evidence = std::unordered_map<std::string, std::array<EvidenceRow, 4>>;
Evidence loadEvidence(sqlite3 *);
class Projector {
public:
  explicit Projector(sqlite3 *charts) : charts_(charts) {}
  void refresh();
  Progress project(const Goal &, const Evidence &);
  bool contains(const Source &, std::string_view hash);
private:
  struct Chart { std::string hash; bool available = false; int mode = 0, longNotes = 0, notes = 0; };
  struct Membership { bool exists = false; std::vector<Chart> charts; };
  const Membership &members(const Source &);
  sqlite3 *charts_;
  int dataVersion_ = -1;
  std::map<std::string, Membership> cache_;
};
} // namespace folder_goals
