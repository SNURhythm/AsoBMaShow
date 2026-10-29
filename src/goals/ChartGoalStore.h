#pragma once
#include "ChartGoal.h"
struct sqlite3;
namespace chart_goals {
struct Page { std::vector<Goal> goals; bool hasMore = false; };
Index loadIndex(sqlite3 *);
std::vector<Goal> loadForChart(sqlite3 *, std::string_view sha256);
Page loadPage(sqlite3 *, int offset);
// A nonzero ID selects that goal across mode edits. Without an ID, a mode
// must contain at most one goal; ambiguous reads/removals fail.
std::optional<Goal> load(sqlite3 *, std::string_view sha256, int mode, std::int64_t id = 0);
std::int64_t save(sqlite3 *, const Goal &);
void remove(sqlite3 *, std::string_view sha256, int mode, std::int64_t id = 0);
} // namespace chart_goals
