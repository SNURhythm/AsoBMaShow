#include "KomboStore.h"
#include "../ClearLampOrder.h"
#include "../../yoga/lib/nlohmann/json.hpp"
#include "../repositories/SqliteRAII.h"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <limits>

namespace kombo {
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Level, source, level, label)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Assignment, id, name, levels, eligibleLamp,
    eligibleOrBelow, eligibleScoreRank, eligibleScoreOrBelow, targetLamp, scoreNumerator, scoreDenominator, customRate, chartCount, points)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Config, dailyGoal, weeklyGoal, monthlyGoal, assignments)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ChartAchievement, eligibleSince, lamp, score, maxScore, levels)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Progress, requirement, originalRequirement, completedRounds, bonusRounds, bonusDifficultyApplied, bonusChartCount, earnedPoints, charts, usedCharts, chartProgress)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(Day, config, played, frozen, points, assignments)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(State, version, configurations, days, savings, freezes,
    freezePurchases, currentKombo, bestKombo, milestones)
namespace {
void normalizeLamps(State &state) {
  const auto assignment = [](Assignment &value) {
    value.eligibleLamp = normalizeClearLamp(value.eligibleLamp);
    value.targetLamp = normalizeClearLamp(value.targetLamp);
  };
  const auto config = [&](Config &value) {
    for (auto &entry : value.assignments) assignment(entry);
  };
  for (auto &[id, value] : state.configurations) config(value);
  for (auto &[id, day] : state.days) {
    config(day.config);
    for (auto &progress : day.assignments) {
      assignment(progress.requirement);
      assignment(progress.originalRequirement);
      for (auto &[chart, achievement] : progress.chartProgress)
        achievement.lamp = normalizeClearLamp(achievement.lamp);
    }
  }
}
void require(bool condition, sqlite3 *db) {
  if (!condition) throw std::runtime_error("Kombo storage: " + sqliteDatabaseError(db));
}
void execute(sqlite3 *db, const char *sql) {
  require(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK, db);
}
}
State load(sqlite3 *db) {
  SqliteStatementHandle exists;
  require(prepareSqliteStatement(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='kombo_state'", exists) == SQLITE_OK, db);
  const int existence = sqlite3_step(exists);
  if (existence == SQLITE_DONE) return {};
  require(existence == SQLITE_ROW, db);
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "SELECT body FROM kombo_state WHERE id=1", query) == SQLITE_OK, db);
  const int result = sqlite3_step(query);
  if (result == SQLITE_DONE) return {};
  require(result == SQLITE_ROW, db);
  auto json = nlohmann::json::parse(sqliteColumnString(query, 0));
  const auto version = json.at("version").get<int>();
  if (version != 1 && version != 2 && version != 3 && version != 4 && version != 5)
    throw std::runtime_error("This kombo data needs a different app version.");
  if (version == 1) {
    // Preserve all previously earned totals and savings. The old format did
    // not track bonus rounds or purchases, so their new counters start at zero.
    json["version"] = 2;
    json["freezePurchases"] = 0;
    for (auto &entry : json.at("days")) {
      for (auto &progress : entry.at(1).at("assignments")) {
        progress["bonusRounds"] = 0;
        progress["earnedPoints"] = static_cast<std::int64_t>(progress.at("completedRounds").get<int>()) *
            progress.at("requirement").at("points").get<int>();
      }
    }
  }
  if (version < 3) {
    json["version"] = 3;
    for (auto &entry : json.at("days")) {
      for (auto &progress : entry.at(1).at("assignments")) {
        // Old state only remembers charts already counted. Do not invent
        // eligibility or achievements for earlier unsuccessful attempts.
        progress["chartProgress"] = nlohmann::json::object();
        for (const auto &chart : progress.at("usedCharts"))
          progress["chartProgress"][chart.get<std::string>()] = ChartAchievement{};
      }
    }
  }
  if (version < 4) {
    json["version"] = 4;
    const auto migrateAssignment = [](auto &assignment) {
      assignment["eligibleScoreRank"] = -2;
      assignment["eligibleScoreOrBelow"] = false;
    };
    for (auto &entry : json.at("configurations"))
      for (auto &assignment : entry.at(1).at("assignments")) migrateAssignment(assignment);
    for (auto &entry : json.at("days")) {
      for (auto &assignment : entry.at(1).at("config").at("assignments")) migrateAssignment(assignment);
      for (auto &progress : entry.at(1).at("assignments")) migrateAssignment(progress.at("requirement"));
    }
  }
  if (version < 5) {
    json["version"] = 5;
    for (auto &entry : json.at("days")) {
      auto &day = entry.at(1);
      const bool bonus = day.at("points").get<std::int64_t>() >= day.at("config").at("dailyGoal").get<int>();
      const nlohmann::json *originalConfig = &day.at("config");
      int originalDate = std::numeric_limits<int>::min();
      for (const auto &configuration : json.at("configurations")) {
        const int date = configuration.at(0).get<int>();
        if (date <= entry.at(0).get<int>() && date > originalDate) {
          originalConfig = &configuration.at(1); originalDate = date;
        }
      }
      for (auto &progress : day.at("assignments")) {
        auto original = std::find_if(originalConfig->at("assignments").begin(), originalConfig->at("assignments").end(),
            [&](const auto &a) { return a.at("id") == progress.at("requirement").at("id"); });
        progress["originalRequirement"] = original != originalConfig->at("assignments").end()
            ? *original : progress.at("requirement");
        for (auto &achievement : progress.at("chartProgress"))
          achievement["levels"] = nlohmann::json::array(); // Unknown until resolved from chart metadata.
        // Preserve existing difficulty and earnings, but never raise it again.
        progress["bonusDifficultyApplied"] = bonus;
        progress["bonusChartCount"] = 1;
        if (bonus) { progress["requirement"]["chartCount"] = 1; progress["charts"] = 0; }
      }
    }
  }
  auto state = json.get<State>();
  normalizeLamps(state);
  if (state.savings < 0 || state.freezes < 0 || state.bestKombo < 0 || state.freezePurchases < 0)
    throw std::runtime_error("Invalid kombo balances.");
  for (const auto &[id, config] : state.configurations)
    if (auto error = validate(config); !error.empty()) throw std::runtime_error(error);
  for (const auto &[id, day] : state.days) {
    if (auto error = validate(day.config); !error.empty()) throw std::runtime_error(error);
    if (day.points < 0 || (day.frozen && day.played) ||
        day.assignments.size() != day.config.assignments.size())
      throw std::runtime_error("Invalid daily kombo progress.");
    std::int64_t earned = 0;
    for (std::size_t i = 0; i < day.assignments.size(); ++i) {
      const auto &progress = day.assignments[i];
      const auto &base = day.config.assignments[i];
      Config active = day.config;
      active.assignments = {progress.requirement};
      if (auto error = validate(active); !error.empty()) throw std::runtime_error(error);
      active.assignments = {progress.originalRequirement};
      if (auto error = validate(active); !error.empty()) throw std::runtime_error(error);
      if (progress.originalRequirement.id != base.id || progress.originalRequirement.points != base.points ||
          progress.requirement.id != base.id || progress.requirement.points != base.points ||
          progress.completedRounds < 0 || progress.bonusRounds < 0 ||
          (progress.bonusChartCount != 1 && progress.bonusChartCount != 2) ||
          (progress.bonusChartCount == 2 && !progress.bonusDifficultyApplied) ||
          progress.bonusRounds > sharedDailyBonusLimit || progress.bonusRounds > progress.completedRounds ||
          progress.earnedPoints < progress.bonusRounds ||
          progress.earnedPoints > static_cast<std::int64_t>(progress.completedRounds - progress.bonusRounds) *
              progress.requirement.points + progress.bonusRounds ||
          progress.charts < 0 ||
          static_cast<std::uint64_t>(progress.charts) + progress.completedRounds > progress.usedCharts.size() ||
          progress.usedCharts.contains("") || (!day.played && !progress.usedCharts.empty()))
        throw std::runtime_error("Invalid assignment progress.");
      constexpr std::array validLamps{-1, 0, 50, 100, 200, 300, 400, 500, 600};
      for (const auto &[chart, achievement] : progress.chartProgress) {
        if (chart.empty() || !day.played ||
            std::find(validLamps.begin(), validLamps.end(), achievement.lamp) == validLamps.end() ||
            achievement.score < 0 || achievement.maxScore < 0 || achievement.score > achievement.maxScore)
          throw std::runtime_error("Invalid daily chart achievements.");
      }
      if (!std::all_of(progress.usedCharts.begin(), progress.usedCharts.end(),
                      [&](const auto &chart) { return progress.chartProgress.contains(chart); }))
        throw std::runtime_error("Counted chart is missing its daily eligibility.");
      earned += progress.earnedPoints;
    }
    if (earned != day.points || (!day.played && day.points != 0))
      throw std::runtime_error("Daily points do not match completed assignments.");
  }
  return state;
}
void save(sqlite3 *db, const State &state) {
  execute(db, "CREATE TABLE IF NOT EXISTS kombo_state (id INTEGER PRIMARY KEY CHECK(id=1), body TEXT NOT NULL)");
  SqliteStatementHandle query;
  require(prepareSqliteStatement(db, "INSERT INTO kombo_state(id,body) VALUES(1,?) ON CONFLICT(id) DO UPDATE SET body=excluded.body", query) == SQLITE_OK, db);
  auto canonical = state;
  normalizeLamps(canonical);
  require(bindSqliteText(query, 1, nlohmann::json(canonical).dump()), db);
  require(sqlite3_step(query) == SQLITE_DONE, db);
}
void recordSavedPlay(sqlite3 *db, const std::filesystem::path &chartPath,
                     Play play, const std::string &md5,
                     const std::string &createdAt, std::uint32_t randomChoice, PlayProgress *progress) {
  if (!play.manual) return;
  auto state = load(db);
  if (state.configurations.empty()) return;
  SqliteStatementHandle time;
  require(prepareSqliteStatement(db, "SELECT CAST(strftime('%s', ?1) AS INTEGER), strftime('%Y-%m-%d %H:%M:%f', ?1)", time) == SQLITE_OK, db);
  require(bindSqliteText(time, 1, createdAt), db);
  require(sqlite3_step(time) == SQLITE_ROW && sqlite3_column_type(time, 0) != SQLITE_NULL, db);
  play.day = localDay(sqlite3_column_int64(time, 0));
  play.createdAt = sqliteColumnString(time, 1);
  if (play.day < state.configurations.begin()->first) return;

  // Read only indexed metadata for this chart, never chart/archive payloads.
  const auto pathText = chartPath.u8string();
  sqlite3 *raw = nullptr;
  const int opened = sqlite3_open_v2(reinterpret_cast<const char *>(pathText.c_str()),
                                    &raw, SQLITE_OPEN_READONLY, nullptr);
  SqliteConnectionHandle charts(raw);
  if (opened != SQLITE_OK) throw std::runtime_error("Difficulty-table storage is unavailable.");
  SqliteStatementHandle query;
  require(prepareSqliteStatement(charts.get(),
      "SELECT DISTINCT t.source_url, e.level FROM difficulty_table_entries e "
      "JOIN difficulty_tables t ON t.id=e.table_id "
      "WHERE (e.sha256=? AND e.sha256<>'') OR (e.md5=? AND e.md5<>'')", query) == SQLITE_OK, charts.get());
  require(bindSqliteText(query, 1, play.chart) && bindSqliteText(query, 2, md5), charts.get());
  int result;
  while ((result = sqlite3_step(query)) == SQLITE_ROW)
    play.levels.push_back({sqliteColumnString(query, 0), sqliteColumnString(query, 1), {}});
  require(result == SQLITE_DONE, charts.get());
  advance(state, play.day);
  auto &day = state.days.at(play.day);
  std::int64_t possiblePoints = day.points;
  for (const auto &assignment : day.assignments) possiblePoints += assignment.requirement.points;
  if (day.points < day.config.dailyGoal && possiblePoints >= day.config.dailyGoal) {
    // Promotion uses the same level order as the table browser. Read only table
    // metadata, once per selected source; never scan or open chart payloads.
    std::map<std::string, std::vector<Level>> tables;
    for (const auto &assignment : day.assignments)
      for (const auto &level : assignment.requirement.levels) tables.try_emplace(level.source);
    for (auto &[source, levels] : tables) {
      SqliteStatementHandle ordered;
      require(prepareSqliteStatement(charts.get(),
          "SELECT e.level, t.symbol FROM difficulty_table_entries e "
          "JOIN difficulty_tables t ON t.id=e.table_id WHERE t.source_url=? "
          "GROUP BY e.level ORDER BY MIN(e.sort_order), e.level", ordered) == SQLITE_OK, charts.get());
      require(bindSqliteText(ordered, 1, source), charts.get());
      while ((result = sqlite3_step(ordered)) == SQLITE_ROW) {
        auto level = sqliteColumnString(ordered, 0);
        levels.push_back({source, level, sqliteColumnString(ordered, 1) + level});
      }
      require(result == SQLITE_DONE, charts.get());
      for (std::size_t i = 1; i < levels.size(); ++i)
        play.nextLevels.push_back({levels[i-1], levels[i]});
    }
    if (!play.nextLevels.empty()) {
      std::map<std::string, std::vector<Level>> memberships;
      for (auto &assignment : day.assignments) {
        for (auto &[chart, achievement] : assignment.chartProgress) {
          if (!achievement.levels.empty() || assignment.usedCharts.contains(chart)) continue;
          auto [found, inserted] = memberships.try_emplace(chart);
          if (inserted) {
            if (chart == play.chart) found->second = play.levels;
            else {
              SqliteStatementHandle membership;
              require(prepareSqliteStatement(charts.get(),
                  "SELECT DISTINCT t.source_url,e.level FROM difficulty_table_entries e "
                  "JOIN difficulty_tables t ON t.id=e.table_id "
                  "WHERE (e.sha256=?1 AND e.sha256<>'') OR (e.md5<>'' AND e.md5 IN "
                  "(SELECT md5 FROM chart_meta WHERE sha256=?1))", membership) == SQLITE_OK, charts.get());
              require(bindSqliteText(membership, 1, chart), charts.get());
              while ((result = sqlite3_step(membership)) == SQLITE_ROW)
                found->second.push_back({sqliteColumnString(membership, 0), sqliteColumnString(membership, 1), {}});
              require(result == SQLITE_DONE, charts.get());
            }
          }
          achievement.levels = found->second;
        }
      }
    }
  }
  record(state, play, randomChoice, progress);
  save(db, state);
}
} // namespace kombo
