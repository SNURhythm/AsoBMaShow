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

void testRecordsReplayDeletionRequiresBoundConfirmation() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const std::string menu = read(root / "src/scene/MainMenuScene.cpp");

  const auto handlerBegin =
      menu.find("replayDeleteButton->setOnClickListener");
  const auto handlerEnd =
      menu.find("replayModalFilterButton->setOnClickListener", handlerBegin);
  require(handlerBegin != std::string::npos &&
              handlerEnd != std::string::npos,
          "Records replay delete callback is missing");
  const std::string_view handler(menu.data() + handlerBegin,
                                 handlerEnd - handlerBegin);
  require(handler.contains("showReplayDeleteConfirmation") &&
              !handler.contains("confirmSelectedReplayFileDelete"),
          "the trash button must open confirmation without deleting");

  const auto confirmBegin =
      menu.find("void MainMenuScene::confirmSelectedReplayFileDelete");
  const auto confirmEnd =
      menu.find("void MainMenuScene::applyReplayFileDocumentHandoff",
                confirmBegin);
  require(confirmBegin != std::string::npos &&
              confirmEnd != std::string::npos,
          "confirmed replay delete boundary is missing");
  const std::string_view confirm(menu.data() + confirmBegin,
                                 confirmEnd - confirmBegin);
  const auto authorization =
      confirm.find("replayDeleteConfirmation.confirm()");
  const auto mutation = confirm.find("actions.remove(*request)");
  require(authorization != std::string_view::npos &&
              mutation != std::string_view::npos && authorization < mutation,
          "deletion must consume its exact pending confirmation before the "
          "file action service mutates anything");
}

} // namespace

int main() {
  testAdditiveModernChartSchemaBoundary();
  testChartRuntimeUsesModernPersistenceBoundary();
  testLegacyChartPersistenceSurfaceIsGone();
  testRecordsReplayDeletionRequiresBoundConfirmation();
  return 0;
}
