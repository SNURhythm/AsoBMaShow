#pragma once
#include <string>
#include "../ClearLampOrder.h"

namespace chart_goals {
// One pass over best records, never play history. The correlated library lookup
// uses the SHA256 index, avoids duplicate chart installations, and checks LN scope.
inline std::string recommendationCandidatesSql() {
  return R"sql(WITH ranked AS MATERIALIZED (
    SELECT chart_sha256 AS sha256, ln_mode, score, max_score,
      CASE WHEN score*27 < max_score*6 THEN 6
           WHEN score*27 < max_score*9 THEN 9
           WHEN score*27 < max_score*12 THEN 12
           WHEN score*27 < max_score*15 THEN 15
           WHEN score*27 < max_score*18 THEN 18
           WHEN score*27 < max_score*21 THEN 21
           WHEN score*27 < max_score*24 THEN 24
           WHEN score*27 < max_score*26 THEN 26 ELSE 27 END AS target
    FROM score_db.score_sha256_best_score_cache
    WHERE max_score>0 AND score>=0 AND score<max_score
  ), candidates AS (
    SELECT r.*, (max_score*target+26)/27-score AS remaining
    FROM ranked r
    WHERE (max_score*target-score*27)*100 <= max_score*54
      AND EXISTS (SELECT 1 FROM chart_meta c WHERE c.sha256=r.sha256
        AND r.ln_mode=CASE WHEN COALESCE(c.total_long_notes,0)+COALESCE(c.total_backspin_notes,0)=0 THEN 0
          WHEN c.ln_mode BETWEEN 1 AND 3 THEN c.ln_mode ELSE r.ln_mode END
        AND (COALESCE(c.total_long_notes,0)+COALESCE(c.total_backspin_notes,0)=0 OR r.ln_mode BETWEEN 1 AND 3))
  ))sql";
}
inline std::string recommendationPageSql() {
  return recommendationCandidatesSql() +
      " SELECT sha256,ln_mode,target,score,max_score,remaining FROM ("
      "SELECT *,ROW_NUMBER() OVER(PARTITION BY sha256 ORDER BY remaining,1.0*remaining/max_score,ln_mode) AS mode_choice "
      "FROM candidates WHERE target=?) WHERE mode_choice=1"
      " ORDER BY remaining, 1.0*remaining/max_score, sha256 LIMIT 20 OFFSET ?";
}
inline std::string gaugeRecommendationCandidatesSql() {
  return R"sql(WITH lamp_bests AS (
    SELECT g.*, NULL AS combo_break FROM score_db.goal_gauge_best g
    WHERE target<600 AND gap <= CASE WHEN target<=300 THEN 5 ELSE 0.1 END
    UNION ALL
    SELECT chart_sha256,ln_mode,600,0,0,0,0,combo_break,combo_break FROM score_db.goal_combo_best
    WHERE combo_break BETWEEN 1 AND 5
  ), candidates AS (
    SELECT g.* FROM lamp_bests g
    WHERE NOT EXISTS (SELECT 1 FROM score_db.score_sha256_clear_rank_cache b
        WHERE b.chart_sha256=g.chart_sha256 AND b.ln_mode=g.ln_mode AND )sql" + clearLampOrderSql("b.rank") + ">=" + clearLampOrderSql("g.target") + R"sql()
      AND EXISTS (SELECT 1 FROM chart_meta c WHERE c.sha256=g.chart_sha256
        AND g.ln_mode=CASE WHEN COALESCE(c.total_long_notes,0)+COALESCE(c.total_backspin_notes,0)=0 THEN 0
          WHEN c.ln_mode BETWEEN 1 AND 3 THEN c.ln_mode ELSE g.ln_mode END
        AND (COALESCE(c.total_long_notes,0)+COALESCE(c.total_backspin_notes,0)=0 OR g.ln_mode BETWEEN 1 AND 3))
  ))sql";
}
inline std::string gaugeRecommendationPageSql() {
  return gaugeRecommendationCandidatesSql() +
      " SELECT chart_sha256,ln_mode,target,gauge_value,border,death_notes,total_notes,gap,combo_break FROM ("
      "SELECT *,ROW_NUMBER() OVER(PARTITION BY chart_sha256 ORDER BY gap,death_notes,ln_mode) AS mode_choice "
      "FROM candidates WHERE target=?) WHERE mode_choice=1"
      " ORDER BY gap,death_notes,chart_sha256 LIMIT 20 OFFSET ?";
}

}
