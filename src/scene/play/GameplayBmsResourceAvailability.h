#pragma once

#include "../../ArchiveFile.h"
#include "../../bms_parser.hpp"

#include <filesystem>

namespace gameplay {

[[nodiscard]] inline bool bmsResourceImagePresent(
    const bms_parser::ChartMeta &meta,
    const std::filesystem::path &declaredPath) noexcept {
  if (declaredPath.empty() || meta.BmsPath.empty()) {
    return false;
  }
  try {
    return archive_file::exists(meta.BmsPath.parent_path() / declaredPath);
  } catch (...) {
    return false;
  }
}

} // namespace gameplay
