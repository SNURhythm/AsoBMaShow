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

inline std::string normalizedSqlHash(std::string_view expression) {
  return "lower(trim(" + std::string(expression) + "))";
}

inline std::string sqlTextHasValue(std::string_view expression) {
  return "NULLIF(TRIM(" + std::string(expression) + "), '') IS NOT NULL";
}

inline std::string legacyBmsRelativePathExpr(std::string_view expression) {
  const std::string text(expression);
  return "CASE WHEN " + text + " LIKE 'Documents/BMS/%' THEN substr(" + text +
         ", length('Documents/BMS/') + 1) ELSE " + text + " END";
}

inline std::string storedOrLegacyBmsPathMatchCondition(
    std::string_view storedPathExpression,
    std::string_view chartPathExpression) {
  const std::string storedPath(storedPathExpression);
  const std::string chartPath(chartPathExpression);
  return storedPath + " != '' AND (" + chartPath + " = " + storedPath +
         " OR " + chartPath + " = 'Documents/BMS/' || " + storedPath + ")";
}

inline std::string chartIdentityHashCondition(std::string_view itemAlias,
                                              std::string_view itemColumn,
                                              std::string_view chartAlias,
                                              std::string_view chartColumn) {
  const std::string itemText(itemAlias);
  const std::string itemColumnText(itemColumn);
  const std::string chartText(chartAlias);
  const std::string chartColumnText(chartColumn);
  const std::string itemExpr = itemText + "." + itemColumnText;
  return sqlTextHasValue(itemExpr) + " AND " + chartText + "." +
         chartColumnText + " = " + normalizedSqlHash(itemExpr);
}

inline std::string chartIdentitySha256Condition(std::string_view itemAlias,
                                                std::string_view chartAlias) {
  return chartIdentityHashCondition(itemAlias, "chart_sha256", chartAlias,
                                    "sha256");
}

inline std::string chartIdentityMd5Condition(std::string_view itemAlias,
                                             std::string_view chartAlias) {
  return chartIdentityHashCondition(itemAlias, "chart_md5", chartAlias, "md5");
}

inline std::string chartIdentityPathCondition(std::string_view itemAlias,
                                              std::string_view chartAlias) {
  const std::string itemText(itemAlias);
  const std::string chartText(chartAlias);
  return storedOrLegacyBmsPathMatchCondition(itemText + ".chart_path",
                                             chartText + ".path");
}

inline std::string chartIdentityMatchPredicate(std::string_view itemAlias,
                                               std::string_view chartAlias) {
  return "((" + chartIdentitySha256Condition(itemAlias, chartAlias) +
         ") OR (" + chartIdentityMd5Condition(itemAlias, chartAlias) +
         ") OR (" + chartIdentityPathCondition(itemAlias, chartAlias) + "))";
}

inline std::string chartIdentityPreferenceOrderBy(std::string_view itemAlias,
                                                  std::string_view chartAlias) {
  return "CASE WHEN " + chartIdentitySha256Condition(itemAlias, chartAlias) +
         " THEN 0 WHEN " + chartIdentityMd5Condition(itemAlias, chartAlias) +
         " THEN 1 WHEN " + chartIdentityPathCondition(itemAlias, chartAlias) +
         " THEN 2 ELSE 3 END";
}

inline std::string matchedChartPathSubquery(
    std::string_view sourceAlias, bool includeTitleTieBreaker = false,
    std::string_view matchAlias = "cm_match") {
  const std::string sourceText(sourceAlias);
  const std::string matchText(matchAlias);
  const std::string sourceSha256 = sourceText + ".sha256";
  const std::string sourceMd5 = sourceText + ".md5";
  std::string query = "(SELECT " + matchText + ".path FROM " +
                      kChartMetaTable + " " + matchText + " WHERE ((" +
                      sqlTextHasValue(sourceSha256) + " AND " + matchText +
                      ".sha256 = " + normalizedSqlHash(sourceSha256) + ") OR (" +
                      sqlTextHasValue(sourceMd5) + " AND " + matchText +
                      ".md5 = " + normalizedSqlHash(sourceMd5) +
                      ")) ORDER BY " +
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
