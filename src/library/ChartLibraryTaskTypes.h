#pragma once

#include "../ChartLibraryScanner.h"
#include "../scene/MainMenuLibrary.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace chart_library_tasks {

enum class TaskStatus { Queued, Running, Complete, Failed, Paused };

enum class TaskKind {
  RefreshLibrary,
  RefreshPath,
  IndexDownloadedPath,
  AndroidImport
};

struct TaskRequest {
  std::uint64_t id = 0;
  TaskKind kind = TaskKind::RefreshLibrary;
  std::string title;
  std::filesystem::path folderToAdd;
  std::string iosBookmark;
  std::filesystem::path refreshPath;
  std::filesystem::path downloadedPath;
  std::vector<std::filesystem::path> downloadedRemovedPaths;
  main_menu_library::FindBmsChartIdentity downloadedTargetIdentity;
  std::uint64_t downloadedSelectionGeneration = 0;
  std::filesystem::path androidImportPath;
  bool androidImportFolder = false;
  bool rebuildLibraryMetadata = false;
};

struct TaskInfo {
  std::uint64_t id = 0;
  std::string title;
  TaskStatus status = TaskStatus::Queued;
  double fraction = 0.0;
  int current = 0;
  int total = 0;
  std::string detail;
};

struct ProgressSnapshot {
  bool valid = false;
  std::uint64_t revision = 0;
  std::uint64_t taskId = 0;
  int current = 0;
  int total = 0;
  int basisPoints = 0;
  ChartScanProgressStage stage = ChartScanProgressStage::Preparing;
};

struct Snapshot {
  std::uint64_t revision = 0;
  int activeCount = 0;
  std::vector<TaskInfo> tasks;
  ProgressSnapshot progress;
};

struct DownloadedIndexCompletion {
  std::filesystem::path chartPath;
  main_menu_library::FindBmsChartIdentity targetIdentity;
  std::uint64_t selectionGeneration = 0;
};

enum class TaskRunDisposition { Complete, Paused, Failed };

struct TaskRunResult {
  TaskRunDisposition disposition = TaskRunDisposition::Complete;
  std::string detail = "Complete";
  std::optional<DownloadedIndexCompletion> downloadedIndex;
};

} // namespace chart_library_tasks
