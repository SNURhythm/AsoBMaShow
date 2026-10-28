#pragma once
#include "ChartGoal.h"
struct sqlite3;
namespace chart_goals {
struct Page { std::vector<Goal> goals; bool hasMore = false; };
Index loadIndex(sqlite3 *);
std::vector<Goal> loadForChart(sqlite3 *, std::string_view sha256);
Page loadPage(sqlite3 *, int offset);
std::optional<Goal> load(sqlite3 *, std::string_view sha256, int mode);
void save(sqlite3 *, const Goal &);
void remove(sqlite3 *, std::string_view sha256, int mode);
} // namespace chart_goals
