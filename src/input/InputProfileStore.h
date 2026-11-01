#pragma once

#include "InputProfile.h"

#include <filesystem>
#include <string>
#include <vector>

namespace atomic_file {
struct Operations;
}

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
                         std::string &errorMessage,
                         const atomic_file::Operations *operations = nullptr);
};
