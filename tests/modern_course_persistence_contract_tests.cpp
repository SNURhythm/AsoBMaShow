#include "repositories/ReplayRepository.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

#ifndef ASOBMASHOW_SOURCE_DIR
#error "ASOBMASHOW_SOURCE_DIR must identify the repository root"
#endif

namespace {

void require(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

std::string read(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

void requireToken(const std::filesystem::path &path, std::string_view token,
                  std::string_view message) {
  const std::string source = read(path);
  require(!source.empty(), "required Slice 5 source file is missing");
  require(source.contains(token), message);
}

void testModernCourseSchemaAndExclusiveOwnershipBoundary() {
  require(ReplayRepository::kCurrentSchemaVersion == 15,
          "pending course-score recovery requires schema version 15");
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const auto schema =
      root / "src/repositories/ReplayRepositorySchema.cpp";
  for (const std::string_view table : {"modern_course_results",
                                       "modern_course_stages",
                                       "modern_course_entries"}) {
    requireToken(schema, table, "strict modern course schema is missing");
  }
  requireToken(schema, "modern_course_result_id",
               "course replay ownership column is missing");
  requireToken(schema,
               "CHECK((modern_chart_result_id IS NOT NULL) != "
               "(modern_course_result_id IS ",
               "chart-or-course replay ownership is not exclusive");
}

void testCourseResultAndReplayStayIndependentAtRuntime() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  requireToken(root / "src/repositories/ReplayRepositoryModernResults.cpp",
               "StageModernCourseResult",
               "modern course result transaction is missing");
  requireToken(root / "src/replay/CourseReplayPersistence.cpp",
               "ReplayFileAssociationCoordinator",
               "course persistence bypasses shared file ownership");
  requireToken(root / "src/ModernResultRecallBuilder.cpp",
               "BuildCourseResult",
               "course result recall authority is missing");

  const std::string recall =
      read(root / "src/ModernResultRecallBuilder.cpp");
  require(!recall.contains("CourseReplayContext") &&
              !recall.contains("ReplayFileStore") &&
              !recall.contains("BeatorajaReplayCodec"),
          "modern course result recall must not open a replay file");
}

void testLiveCourseUsesOneModernResultFirstRoute() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const auto gameplay = root / "src/scene/play/GamePlayScene.cpp";
  const auto resultScene = root / "src/scene/ResultScene.cpp";
  const auto context = root / "src/context.h";
  const auto scores = root / "src/repositories/ScoreRepositoryQueries.cpp";

  requireToken(gameplay, "ModernCourseFile",
               "live course gameplay does not select modern persistence");
  requireToken(gameplay, "recordModernCourseStage",
               "live course gameplay does not retain canonical stage capture");
  requireToken(resultScene, "persistModernCourseResult",
               "final course result does not use one modern persistence path");
  requireToken(context, "CourseResultPersistence",
               "application context does not own the modern course boundary");
  requireToken(
      context, "courseRecovery = coursePersistence.recoverAll()",
      "startup does not retry course score projection from modern results");
  requireToken(scores, "SaveProjectedCourseScore",
               "course-score history is not projected from modern results");

  const std::string gameplaySource = read(gameplay);
  require(!gameplaySource.contains("CompletedAttemptPersistenceRoute::\n"
                                   "                                         "
                                   "LegacyCourse"),
          "live course gameplay still enters the legacy result route");
  const std::string resultSource = read(resultScene);
  require(!resultSource.contains("saveCourseReplay") &&
              !resultSource.contains("SaveCourseReplay("),
          "final course result still has a legacy replay fallback");
  const std::string repository =
      read(root / "src/repositories/ReplayRepository.h");
  require(!repository.contains("SaveCourseReplay("),
          "legacy course persistence API is still public");
}

void testModernCourseRecordsUseResultAndVerifiedReplayAuthorities() {
  const std::filesystem::path root = ASOBMASHOW_SOURCE_DIR;
  const auto summary = root / "src/ResultRecordSummary.h";
  const auto menu = root / "src/scene/MainMenuScene.cpp";
  const auto resultScene = root / "src/scene/ResultScene.cpp";

  requireToken(summary, "ModernCourseRecordId",
               "modern course Records have no durable tagged identity");
  requireToken(summary, "makeModernCourseResultRecord",
               "strict modern course rows are not projected into Records");
  requireToken(menu, "ListModernCourseResults",
               "course Records do not query strict history by course key");
  requireToken(menu, "startModernCourseReplayPlayback",
               "course Watch is not separated from the legacy adapter");
  requireToken(menu, "startModernCourseReplayResultRecall",
               "course View Result is not separated from legacy replay recall");
  requireToken(menu, "startModernCourseReplayVideoExport",
               "course video is not separated from the legacy adapter");
  requireToken(menu, "makeRuntimeCourseReplayConsumer",
               "modern course replay actions bypass the sole consumer");
  requireToken(root / "src/ReplayVideoExporter.cpp", "materializedStages",
               "course video does not consume verified continuation stages");
  requireToken(resultScene, "modernCourseResultBrowsing",
               "course result browsing still requires replay detail");

  const std::string modernRecall = read(menu);
  const auto begin = modernRecall.find(
      "void MainMenuScene::startModernCourseReplayResultRecall");
  require(begin != std::string::npos,
          "modern course result recall entry point is missing");
  const auto end = modernRecall.find(
      "void MainMenuScene::startRemoteResultRecall", begin);
  require(end != std::string::npos,
          "modern course result recall boundary is malformed");
  const std::string_view recallBody(modernRecall.data() + begin, end - begin);
  require(recallBody.contains("currentSelection") &&
              recallBody.contains("BuildCourseResult") &&
              !recallBody.contains("CourseReplayConsumer") &&
              !recallBody.contains("LoadCourseReplay"),
          "modern course View Result must resolve current chart locations and "
          "use strict result rows without BRD or legacy replay loading");

  const auto playbackBegin = modernRecall.find(
      "void MainMenuScene::startModernCourseReplayPlayback");
  const auto playbackEnd = modernRecall.find(
      "void MainMenuScene::startCourseReplayDirect", playbackBegin);
  const auto videoBegin = modernRecall.find(
      "void MainMenuScene::startModernCourseReplayVideoExport");
  const auto videoEnd = modernRecall.find(
      "std::optional<std::string>", videoBegin);
  require(playbackBegin != std::string::npos &&
              playbackEnd != std::string::npos &&
              videoBegin != std::string::npos && videoEnd != std::string::npos,
          "modern course consumer boundaries are malformed");
  const std::string_view playbackBody(modernRecall.data() + playbackBegin,
                                      playbackEnd - playbackBegin);
  const std::string_view videoBody(modernRecall.data() + videoBegin,
                                   videoEnd - videoBegin);
  require(playbackBody.contains("currentCourseSelectionFor") &&
              videoBody.contains("currentCourseSelectionFor") &&
              !playbackBody.contains("stage.score.chartPath") &&
              !videoBody.contains("stage.score.chartPath"),
          "course replay consumers must share current location resolution");

  const auto listBegin = modernRecall.find(
      "void MainMenuScene::reloadReplayRecordModels");
  const auto listEnd = modernRecall.find(
      "void MainMenuScene::showReplayListModal", listBegin);
  require(listBegin != std::string::npos && listEnd != std::string::npos,
          "modern Records list boundary is malformed");
  const std::string_view listBody(modernRecall.data() + listBegin,
                                  listEnd - listBegin);
  require(listBody.contains("ReplayFileActionService") &&
              listBody.contains("replayStateForFileAction") &&
              !listBody.contains("ChartReplayContext") &&
              !listBody.contains("makeParsedChartReplayFacts") &&
              !listBody.contains("makeRuntimeCourseReplayConsumer") &&
              !listBody.contains("consumer.load"),
          "Records must inspect owned file state without decoding or "
          "materializing chart or course replay data");
}

} // namespace

int main() {
  testModernCourseSchemaAndExclusiveOwnershipBoundary();
  testCourseResultAndReplayStayIndependentAtRuntime();
  testLiveCourseUsesOneModernResultFirstRoute();
  testModernCourseRecordsUseResultAndVerifiedReplayAuthorities();
  return 0;
}
