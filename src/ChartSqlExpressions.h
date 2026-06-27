#pragma once

#include <string>
#include <string_view>

namespace asobmshow::chart_sql {

inline constexpr const char *kChartMetaTable = "chart_meta";
inline constexpr const char *kMaxSqlIntegerText = "9223372036854775807";
inline constexpr const char *kChartMetaSelectColumns =
    "cm.path,"
    "cm.md5,"
    "cm.sha256,"
    "cm.title,"
    "cm.subtitle,"
    "cm.genre,"
    "cm.artist,"
    "cm.sub_artist,"
    "cm.folder,"
    "cm.stage_file,"
    "cm.banner,"
    "cm.back_bmp,"
    "cm.preview,"
    "cm.level,"
    "cm.difficulty,"
    "cm.total,"
    "cm.bpm,"
    "cm.max_bpm,"
    "cm.min_bpm,"
    "cm.length,"
    "cm.rank,"
    "cm.player,"
    "cm.keys,"
    "cm.total_notes,"
    "cm.total_long_notes,"
    "cm.total_scratch_notes,"
    "cm.total_backspin_notes,"
    "cm.ln_mode";
inline constexpr int kChartMetaColumnCount = 28;

inline std::string chartSourcePriorityExpr(std::string_view alias) {
  const std::string aliasText(alias);
  return "COALESCE(" + aliasText + ".source_priority, 3)";
}

inline std::string chartSourceArchiveSizeExpr(std::string_view alias) {
  const std::string aliasText(alias);
  return "COALESCE(" + aliasText + ".source_archive_size, " +
         kMaxSqlIntegerText + ")";
}

inline std::string chartSourceOrderBy(std::string_view alias) {
  const std::string aliasText(alias);
  return chartSourcePriorityExpr(aliasText) + ", " +
         chartSourceArchiveSizeExpr(aliasText) + ", " + aliasText + ".path";
}

inline std::string chartArtworkOrderBy(std::string_view alias) {
  const std::string aliasText(alias);
  return "CASE WHEN NULLIF(TRIM(" + aliasText +
         ".stage_file), '') IS NOT NULL THEN 0 WHEN NULLIF(TRIM(" +
         aliasText + ".banner), '') IS NOT NULL THEN 1 ELSE 2 END";
}

inline std::string chartIdentityPreferenceOrderBy(std::string_view itemAlias,
                                                  std::string_view chartAlias) {
  const std::string itemText(itemAlias);
  const std::string chartText(chartAlias);
  return "CASE WHEN " + itemText + ".chart_sha256 != '' AND " + chartText +
         ".sha256 = " + itemText + ".chart_sha256 THEN 0 WHEN " + itemText +
         ".chart_md5 != '' AND " + chartText + ".md5 = " + itemText +
         ".chart_md5 THEN 1 WHEN " + itemText + ".chart_path != '' AND " +
         chartText + ".path = " + itemText +
         ".chart_path THEN 2 ELSE 3 END";
}

inline std::string matchedChartPathSubquery(
    std::string_view sourceAlias, bool includeTitleTieBreaker = false,
    std::string_view matchAlias = "cm_match") {
  const std::string sourceText(sourceAlias);
  const std::string matchText(matchAlias);
  std::string query = "(SELECT " + matchText + ".path FROM " +
                      kChartMetaTable + " " + matchText + " WHERE ((" +
                      sourceText + ".sha256 != '' AND " + matchText +
                      ".sha256 = " + sourceText + ".sha256) OR (" +
                      sourceText + ".md5 != '' AND " + matchText +
                      ".md5 = " + sourceText + ".md5)) ORDER BY " +
                      chartSourceOrderBy(matchText);
  if (includeTitleTieBreaker) {
    query += ", " + matchText + ".title COLLATE NOCASE";
  }
  query += " LIMIT 1)";
  return query;
}

inline std::string preferredChartPredicate(
    std::string_view alias, std::string_view chartMetaTable = kChartMetaTable) {
  const std::string aliasText(alias);
  const std::string tableText(chartMetaTable);
  const std::string betterPriority = chartSourcePriorityExpr("cm_better");
  const std::string currentPriority = chartSourcePriorityExpr(aliasText);
  const std::string betterArchiveSize =
      chartSourceArchiveSizeExpr("cm_better");
  const std::string currentArchiveSize = chartSourceArchiveSizeExpr(aliasText);

  return "NOT EXISTS (SELECT 1 FROM " + tableText +
         " cm_better WHERE "
         "cm_better.path != " +
         aliasText + ".path AND ((" + aliasText +
         ".sha256 != '' AND cm_better.sha256 = " + aliasText +
         ".sha256) OR (" + aliasText + ".sha256 = '' AND " + aliasText +
         ".md5 != '' AND cm_better.md5 = " + aliasText + ".md5)) AND (" +
         betterPriority + " < " + currentPriority + " OR (" +
         betterPriority + " = " + currentPriority + " AND " +
         betterArchiveSize + " < " + currentArchiveSize + ") OR (" +
         betterPriority + " = " + currentPriority + " AND " +
         betterArchiveSize + " = " + currentArchiveSize +
         " AND cm_better.path < " + aliasText + ".path)))";
}

} // namespace asobmshow::chart_sql
