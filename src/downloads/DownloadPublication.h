#pragma once
#include <filesystem>
#include <system_error>

namespace downloads {
enum class PublicationRecovery { Retry, Installed, Conflict };
// Records the small rename boundary of the existing archive publisher. File
// identity (not payload hashes) distinguishes a completed install from a crash
// before rename, and refuses to overwrite a user's subsequent replacement.
class PublicationJournal {
public:
  PublicationJournal(std::filesystem::path journal, std::filesystem::path destination);
  bool beginCopy(const std::filesystem::path &transaction);
  void rename(const std::filesystem::path &from, const std::filesystem::path &to,
              std::error_code &error);
  PublicationRecovery recover();
private:
  std::filesystem::path journal_, destination_;
};
} // namespace downloads
