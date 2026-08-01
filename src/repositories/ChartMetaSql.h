#pragma once

#include "../bms_parser.hpp"
#include "../sqlite3.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace asobmshow::chart_sql {

inline std::string chartMetaColumnString(sqlite3_stmt *stmt, int column) {
  if (sqlite3_column_type(stmt, column) == SQLITE_NULL) {
    return {};
  }
  const auto *text =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, column));
  if (text == nullptr) {
    return {};
  }
  const int byteCount = sqlite3_column_bytes(stmt, column);
  return std::string(text, static_cast<std::size_t>(byteCount));
}

template <typename AbsolutePathFromColumn, typename RelativePathFromColumn>
inline bms_parser::ChartMeta
readChartMeta(sqlite3_stmt *stmt, AbsolutePathFromColumn absolutePathFromColumn,
              RelativePathFromColumn relativePathFromColumn) {
  int idx = 0;
  bms_parser::ChartMeta chartMeta;
  chartMeta.BmsPath = absolutePathFromColumn(stmt, idx++);
  chartMeta.MD5 = chartMetaColumnString(stmt, idx++);
  chartMeta.SHA256 = chartMetaColumnString(stmt, idx++);
  chartMeta.Title = chartMetaColumnString(stmt, idx++);
  chartMeta.SubTitle = chartMetaColumnString(stmt, idx++);
  chartMeta.Genre = chartMetaColumnString(stmt, idx++);
  chartMeta.Artist = chartMetaColumnString(stmt, idx++);
  chartMeta.SubArtist = chartMetaColumnString(stmt, idx++);
  chartMeta.Folder = absolutePathFromColumn(stmt, idx++);
  chartMeta.StageFile = relativePathFromColumn(stmt, idx++);
  chartMeta.Banner = relativePathFromColumn(stmt, idx++);
  chartMeta.BackBmp = relativePathFromColumn(stmt, idx++);
  chartMeta.Preview = relativePathFromColumn(stmt, idx++);

  chartMeta.PlayLevel = sqlite3_column_double(stmt, idx++);
  chartMeta.Difficulty = sqlite3_column_int(stmt, idx++);
  chartMeta.Total = sqlite3_column_double(stmt, idx++);
  chartMeta.HasTotal = sqlite3_column_int(stmt, idx++) != 0;
  chartMeta.Bpm = sqlite3_column_double(stmt, idx++);
  chartMeta.MaxBpm = sqlite3_column_double(stmt, idx++);
  chartMeta.MinBpm = sqlite3_column_double(stmt, idx++);
  chartMeta.PlayLength = sqlite3_column_int64(stmt, idx++);
  chartMeta.Rank = sqlite3_column_int(stmt, idx++);
  chartMeta.Player = sqlite3_column_int(stmt, idx++);
  chartMeta.KeyMode = sqlite3_column_int(stmt, idx++);
  chartMeta.TotalNotes = sqlite3_column_int(stmt, idx++);
  chartMeta.TotalLongNotes = sqlite3_column_int(stmt, idx++);
  chartMeta.TotalScratchNotes = sqlite3_column_int(stmt, idx++);
  chartMeta.TotalBackSpinNotes = sqlite3_column_int(stmt, idx++);
  chartMeta.LnMode = sqlite3_column_int(stmt, idx++);

  return chartMeta;
}

} // namespace asobmshow::chart_sql
