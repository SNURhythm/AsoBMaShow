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

} // namespace

int main() {
  testAdditiveModernChartSchemaBoundary();
  testChartRuntimeUsesModernPersistenceBoundary();
  return 0;
}
