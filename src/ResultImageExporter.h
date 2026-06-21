#pragma once

#include "ReplayData.h"
#include "bms_parser.hpp"
#include "context.h"
#include "scene/play/RhythmState.h"
#include "skin/SkinTypes.h"

#include <filesystem>
#include <optional>
#include <string>

struct ResultImageExportResult {
  bool success = false;
  std::filesystem::path outputPath;
  std::string message;
};

class ResultImageExporter {
public:
  static ResultImageExportResult Export(ApplicationContext &context,
                                        const bms_parser::ChartMeta &meta,
                                        const RhythmState &state,
                                        const std::string &playModeLabel = {},
                                        const std::string &laneOrderLabel = {},
                                        const std::string &difficultyLabel = {},
                                        const std::optional<
                                            ResultPreviousBestData>
                                            &previousBest = std::nullopt);
  static ResultImageExportResult ExportReplay(ApplicationContext &context,
                                              bms_parser::Chart &chart,
                                              const ReplayData &replay);
};
