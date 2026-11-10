#pragma once
#include <filesystem>
#include <system_error>
#include <string>
#include <vector>
#include <functional>

namespace downloads {
enum class PublicationRecovery { Retry, Installed, Conflict };
// Records the small rename boundary of the existing archive publisher. File
// identity (not payload hashes) distinguishes a completed install from a crash
// before rename, and refuses to overwrite a user's subsequent replacement.
class PublicationJournal {
public:
  using DirectorySync = std::function<bool(const std::filesystem::path &, std::string &)>;
  // An explicit root is application-owned recovery authority, never journal data.
  PublicationJournal(std::filesystem::path journal, std::filesystem::path destination,
                     DirectorySync syncDirectory = {}, std::filesystem::path transactionRoot = {});
  bool beginCopy(const std::filesystem::path &transaction);
  // Throws after a moved-but-unsynced rename; caller must preserve recovery material.
  void rename(const std::filesystem::path &from, const std::filesystem::path &to,
              std::error_code &error);
  PublicationRecovery recover();
  bool recordRemovals(const std::vector<std::filesystem::path> &paths, std::string &error);
  std::vector<std::filesystem::path> removedPaths() const;
private:
  std::filesystem::path journal_, destination_, transactionRoot_;
  DirectorySync syncDirectory_;
};
} // namespace downloads
