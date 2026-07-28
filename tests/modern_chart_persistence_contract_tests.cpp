#include "repositories/ReplayRepository.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::string read(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void requireToken(const std::filesystem::path &path,
                  std::string_view token) {
  const std::string source = read(path);
  require(!source.empty(), "required Slice 4 source file is missing");
  require(source.find(token) != std::string::npos,
          "required modern chart persistence authority is missing");
}

void testAdditiveModernChartSchemaBoundary() {
  require(ReplayRepository::kCurrentSchemaVersion >= 11,
          "modern chart persistence requires replay schema version 11+");
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const auto modern =
      root / "src/repositories/ReplayRepositoryModernResults.cpp";
  for (const std::string_view table : {"modern_chart_results",
                                       "ir_submission_snapshots",
                                       "modern_replay_files"}) {
    requireToken(modern, table);
  }
  requireToken(modern, "BEGIN IMMEDIATE TRANSACTION");
}

void testChartRuntimeUsesModernPersistenceBoundary() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/scene/play/GamePlayScene.cpp",
               "persistModernChart");
  requireToken(root / "src/repositories/ReplayRepositoryModernResults.cpp",
               "validateModernChartResult");
  requireToken(root / "src/repositories/ReplayRepositoryModernResults.cpp",
               "captureIrSubmissionSnapshot");
}

void testLegacyChartPersistenceSurfaceIsGone() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const std::string context = read(root / "src/context.h");
  const std::string repository =
      read(root / "src/repositories/ReplayRepository.h");
  const std::string records =
      read(root / "src/repositories/ReplayRepositoryRecords.cpp");
  const std::string resultScene = read(root / "src/scene/ResultScene.cpp");

  require(!context.contains("resultPersistence"),
          "application context still owns the legacy persistence coordinator");
  require(!repository.contains("SaveReplay(") &&
              !repository.contains("StageChartResult(") &&
              !repository.contains("LoadPendingChartScore(") &&
              !repository.contains("ListPendingChartScores(") &&
              !repository.contains("AcknowledgePendingChartScoreAndActivateIr(") &&
              !repository.contains("RecordPendingChartScoreRecoveryAttempt("),
          "legacy chart persistence API is still public");
  require(!records.contains("INSERT INTO replays") &&
              !records.contains("INSERT INTO replay_events") &&
              !records.contains("INSERT INTO replay_touch_samples") &&
              !records.contains("INSERT INTO replay_lane_cover_events") &&
              !records.contains("pending_chart_score_writes"),
          "legacy chart row or pending-score SQL is still compiled");
  require(!resultScene.contains("context.resultPersistence"),
          "ResultScene still retries through legacy chart persistence");
}

} // namespace

int main() {
  testAdditiveModernChartSchemaBoundary();
  testChartRuntimeUsesModernPersistenceBoundary();
  testLegacyChartPersistenceSurfaceIsGone();
  return 0;
}
