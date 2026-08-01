#pragma once

#include "InputProfile.h"

#include <filesystem>
#include <string>
#include <vector>

enum class InputProfileLoadStatus {
  Loaded,
  MissingDefaults,
  InvalidDocument,
  FutureVersion
};

struct InputProfileLoadResult {
  InputProfileLoadStatus status = InputProfileLoadStatus::MissingDefaults;
  InputProfile profile;
  std::vector<std::string> diagnostics;
};

class InputProfileStore {
public:
  static InputProfileLoadResult load(const std::filesystem::path &path);
  static bool saveAtomic(const std::filesystem::path &path,
                         const InputProfile &profile,
                         std::string &errorMessage);

#ifdef INPUT_PROFILE_STORE_TESTING
  static void setForceFinalRenameFailureForTesting(bool forceFailure);
#endif
};
