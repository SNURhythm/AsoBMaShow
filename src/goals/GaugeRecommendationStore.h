#pragma once
#include "../repositories/SqliteRAII.h"
#include "../ResultPersistenceModel.h"
#include "../BmsMetadataText.h"
#include "../scene/play/GameplayScoreState.h"

namespace chart_goals {
// Authoritative numeric bests, bounded to six rows per chart/LN scope. These
// are updated with the score transaction, never rebuilt from result history.
inline bool gaugeSummaryInstalled(sqlite3 *db) {
  SqliteStatementHandle query;
  if (prepareSqliteStatement(db, "SELECT chart_sha256,ln_mode,target,gauge_value,border,death_notes,total_notes,gap FROM goal_gauge_best LIMIT 0", query) != SQLITE_OK)
    return false;
  query.reset();
  return prepareSqliteStatement(db, "SELECT 1 FROM sqlite_master WHERE name='goal_gauge_proximity' AND type='index'", query) == SQLITE_OK &&
         sqlite3_step(query) == SQLITE_ROW;
}
inline bool ensureGaugeSummary(sqlite3 *db) {
  return !executeSqlite(db, "CREATE TABLE IF NOT EXISTS goal_gauge_best(chart_sha256 TEXT NOT NULL,ln_mode INTEGER NOT NULL,target INTEGER NOT NULL,gauge_value REAL NOT NULL,border REAL NOT NULL,death_notes INTEGER NOT NULL,total_notes INTEGER NOT NULL,gap REAL NOT NULL,PRIMARY KEY(chart_sha256,ln_mode,target)) WITHOUT ROWID") &&
         !executeSqlite(db, "CREATE INDEX IF NOT EXISTS goal_gauge_proximity ON goal_gauge_best(target,gap,death_notes,chart_sha256,ln_mode)") &&
         gaugeSummaryInstalled(db);
}
inline bool updateGaugeSummary(sqlite3 *db, const result_persistence::ChartScoreWrite &score,
                               const GaugeEvidence &evidence) {
  const auto &provenance = score.provenance;
  const int total = score.maxScore / 2;
  if (!evidence.valid(total)) return false;
  const auto hash = asobmshow::bms_metadata::normalizedHash(score.chartSha256);
  if (!evidence.recorded || total <= 0 || hash.size() != 64 || score.longNoteMode < 0 || score.longNoteMode > 3 ||
      provenance.eligibility != ScoreEligibility::Verified || !isSupportedRulesetDescriptor(provenance.ruleset) ||
      provenance.autoPlay || provenance.practice || provenance.clubMode || provenance.assistOption != assist_options::kOff ||
      !provenance.playback.neutral() || provenance.startingGaugePercent || gaugeProfileIsCourse(provenance.gaugeProfile)) return true;
  SqliteStatementHandle insert;
  if (prepareSqliteStatement(db,
      "INSERT INTO goal_gauge_best(chart_sha256,ln_mode,target,gauge_value,border,death_notes,total_notes,gap) VALUES(?,?,?,?,?,?,?,?) "
      "ON CONFLICT(chart_sha256,ln_mode,target) DO UPDATE SET gauge_value=excluded.gauge_value,border=excluded.border,"
      "death_notes=excluded.death_notes,total_notes=excluded.total_notes,gap=excluded.gap "
      "WHERE excluded.gap<goal_gauge_best.gap OR (excluded.gap=goal_gauge_best.gap AND excluded.death_notes=-1 AND goal_gauge_best.death_notes>=0)",
      insert) != SQLITE_OK) return false;
  for (int gauge = 0; gauge < 6; ++gauge) {
    const bool survival = gauge >= 3;
    if (bool(evidence.survivalMask & (1u << gauge)) != survival) continue;
    const int death = evidence.deathPassedNotes[gauge];
    if ((!survival || death == -1) && evidence.passedNotes != total) continue;
    const auto profile = provenance.ruleset.id == "lr2" ? GaugeProfile::Standard : provenance.gaugeProfile;
    const float border = survival ? 0 : gaugeBorderValue(static_cast<GaugeType>(gauge), profile);
    const double gap = survival ? (death == -1 ? 0 : static_cast<double>(total - death) / total)
                                : std::max(0.0f, border - evidence.values[gauge]);
    sqlite3_reset(insert);
    if (!bindSqliteText(insert, 1, hash) || sqlite3_bind_int(insert, 2, score.longNoteMode) != SQLITE_OK ||
        sqlite3_bind_int(insert, 3, (gauge + 1) * 100) != SQLITE_OK ||
        sqlite3_bind_double(insert, 4, evidence.values[gauge]) != SQLITE_OK ||
        sqlite3_bind_double(insert, 5, border) != SQLITE_OK || sqlite3_bind_int(insert, 6, death) != SQLITE_OK ||
        sqlite3_bind_int(insert, 7, total) != SQLITE_OK || sqlite3_bind_double(insert, 8, gap) != SQLITE_OK ||
        sqlite3_step(insert) != SQLITE_DONE) return false;
  }
  return true;
}
}
