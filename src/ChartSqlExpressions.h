#pragma once

#include <string>
#include <string_view>

namespace asobmshow::chart_sql {

inline constexpr const char *kChartMetaTable = "chart_meta";
inline constexpr const char *kMaxSqlIntegerText = "9223372036854775807";

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
