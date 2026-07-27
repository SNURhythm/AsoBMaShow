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
  require(ReplayRepository::kCurrentSchemaVersion == 12,
          "Slice 5 advances the additive replay schema to version 12");
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
               "(modern_chart_result_id IS NOT NULL) != "
               "(modern_course_result_id IS NOT NULL)",
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
  requireToken(scores, "SaveProjectedCourseScore",
               "course-score history is not projected from modern results");

  const std::string gameplaySource = read(gameplay);
  require(!gameplaySource.contains("CompletedAttemptPersistenceRoute::\n"
                                   "                                         "
                                   "LegacyCourse"),
          "live course gameplay still enters the legacy result route");
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
  require(recallBody.contains("BuildCourseResult") &&
              !recallBody.contains("CourseReplayConsumer") &&
              !recallBody.contains("LoadCourseReplay"),
          "modern course View Result must use strict result rows without BRD or legacy replay loading");
}

} // namespace

int main() {
  testModernCourseSchemaAndExclusiveOwnershipBoundary();
  testCourseResultAndReplayStayIndependentAtRuntime();
  testLiveCourseUsesOneModernResultFirstRoute();
  testModernCourseRecordsUseResultAndVerifiedReplayAuthorities();
  return 0;
}
