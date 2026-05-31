#pragma once

#include "ReplayData.h"
#include "context.h"

#include <filesystem>
#include <string>

struct ReplayVideoExportOptions {
  int width = 0;
  int height = 0;
  int fps = 0;
};

struct ReplayVideoExportResult {
  bool success = false;
  std::filesystem::path outputPath;
  std::string message;
};

class ReplayVideoExporter {
public:
  static ReplayVideoExportResult
  Export(ApplicationContext &context, bms_parser::Chart *chart,
         const ReplayData &replay,
         const ReplayVideoExportOptions &options = {});
};
