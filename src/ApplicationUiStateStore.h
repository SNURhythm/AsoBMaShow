#pragma once

#include "ApplicationUiState.h"

#include <filesystem>
#include <string>
#include <vector>

enum class ApplicationUiStateLoadStatus {
  Loaded,
  Missing,
  Invalid,
  FutureVersion,
};

struct ApplicationUiStateLoadResult {
  ApplicationUiState state;
  ApplicationUiStateLoadStatus status =
      ApplicationUiStateLoadStatus::Missing;
  std::vector<std::string> diagnostics;
};

[[nodiscard]] std::filesystem::path
applicationUiStatePath(const std::filesystem::path &applicationDataRoot);

class ApplicationUiStateStore {
public:
  static ApplicationUiStateLoadResult
  Load(const std::filesystem::path &path);
  static bool SaveAtomic(const std::filesystem::path &path,
                         const ApplicationUiState &state,
                         std::string &diagnostic);
};
