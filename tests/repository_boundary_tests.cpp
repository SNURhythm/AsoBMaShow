#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

std::string readText(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

bool isSource(const fs::path &path) {
  const std::string extension = path.extension().string();
  return extension == ".h" || extension == ".hpp" || extension == ".c" ||
         extension == ".cpp";
}

bool isPublicRepositoryHeader(const fs::path &relative) {
  const std::string value = relative.generic_string();
  return value == "repositories/ChartRepository.h" ||
         value == "repositories/ScoreRepository.h" ||
         value == "repositories/ReplayRepository.h" ||
         value == "repositories/MusicPlaylistRepository.h";
}

bool mayOwnSqlite(const fs::path &relative) {
  const std::string value = relative.generic_string();
  return value.starts_with("repositories/") ||
         value == "ProfileDatabaseTools.cpp" ||
         value == "ProfileDatabaseTools.h" || value == "sqlite3.c" ||
         value == "sqlite3.h";
}

int main() {
  const fs::path sourceRoot = fs::path(ASOBMASHOW_SOURCE_DIR) / "src";
  const std::array<std::string_view, 4> sqliteTokens{
      "sqlite3", "SqliteConnectionHandle", "SqliteStatementHandle",
      "SqliteTransactionHandle"};
  const std::array<std::string_view, 9> repositoryWorkflowTokens{
      "ArchiveFile", "bms_parser::Parser", "readArchive",
      "std::filesystem::recursive_directory_iterator", "CURL", "curl_",
      "Download", "nlohmann::json", "ifstream"};
  constexpr std::string_view privateHeaderToken =
      "Repository" "Internal.h";
  std::vector<std::string> failures;

  for (const fs::directory_entry &entry :
       fs::recursive_directory_iterator(sourceRoot)) {
    if (!entry.is_regular_file() || !isSource(entry.path())) {
      continue;
    }
    const fs::path relative = fs::relative(entry.path(), sourceRoot);
    const std::string text = readText(entry.path());
    if (relative.generic_string().starts_with("repositories/") &&
        (relative.extension() == ".h" || relative.extension() == ".hpp") &&
        text.find("GetInstance") != std::string::npos) {
      failures.push_back(relative.generic_string() + ": repository singleton");
    }
    const bool repositoryImplementation =
        relative.generic_string().starts_with("repositories/") &&
        relative.extension() == ".cpp";
    if (!repositoryImplementation &&
        text.find(privateHeaderToken) != std::string::npos) {
      failures.push_back(relative.generic_string() +
                         ": private repository header include");
    }
    if (!mayOwnSqlite(relative) || isPublicRepositoryHeader(relative)) {
      for (std::string_view token : sqliteTokens) {
        if (text.find(token) != std::string::npos) {
          failures.push_back(relative.generic_string() + ": raw SQLite token " +
                             std::string(token));
        }
      }
    }
    if (relative.generic_string().starts_with("repositories/ChartRepository") &&
        relative.extension() == ".cpp") {
      for (std::string_view token : repositoryWorkflowTokens) {
        if (text.find(token) != std::string::npos) {
          failures.push_back(relative.generic_string() +
                             ": non-persistence workflow token " +
                             std::string(token));
        }
      }
    }
  }

  const fs::path testRoot = fs::path(ASOBMASHOW_SOURCE_DIR) / "tests";
  for (const fs::directory_entry &entry :
       fs::recursive_directory_iterator(testRoot)) {
    if (!entry.is_regular_file() || !isSource(entry.path())) {
      continue;
    }
    if (readText(entry.path()).find(privateHeaderToken) != std::string::npos) {
      failures.push_back(
          "tests/" + fs::relative(entry.path(), testRoot).generic_string() +
          ": private repository header include");
    }
  }

  for (const std::string &failure : failures) {
    std::cerr << failure << '\n';
  }
  return failures.empty() ? 0 : 1;
}
