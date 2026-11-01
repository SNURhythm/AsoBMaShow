#include "downloads/DownloadPublication.h"
#include <cassert>
#include <fstream>
#include <random>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace downloads;
int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("asobms-publication-" + std::to_string(std::random_device{}()));
  const auto target = root / "BMSSEARCH/package";
  const auto transaction = root / "BMSSEARCH/.asobmashow-transactions/41b74c17-434d-423a-a40f-9399b2f0382c";
  std::filesystem::create_directories(transaction);
  std::ofstream(target) << "old";
  std::ofstream(transaction / "commit") << "new";
  {
    PublicationJournal journal(root / "publication.json", target);
    std::error_code error;
    journal.rename(target, transaction / "backup", error);
    assert(!error);
  }
  {
    PublicationJournal restarted(root / "publication.json", target);
    assert(restarted.recover() == PublicationRecovery::Retry);
    std::string text;
    std::ifstream(target) >> text;
    assert(text == "old"); // Death between backup and install restores old files.
  }
  std::filesystem::create_directories(transaction);
  std::ofstream(transaction / "commit") << "new";
  {
    PublicationJournal journal(root / "publication.json", target);
    std::error_code error;
    journal.rename(target, transaction / "backup", error);
    assert(!error);
    journal.rename(transaction / "commit", target, error);
    assert(!error);
  }
  {
    PublicationJournal restarted(root / "publication.json", target);
    assert(restarted.recover() == PublicationRecovery::Installed);
    std::string text;
    std::ifstream(target) >> text;
    assert(text == "new"); // No payload reread/republication on callback recovery.
    std::filesystem::rename(target, root / "moved");
    std::ofstream(target) << "unrelated";
    assert(restarted.recover() == PublicationRecovery::Conflict);
    assert(std::filesystem::exists(target)); // A replacement by the player is untouched.
  }
#ifndef _WIN32
  if (geteuid() != 0) { // Root can sync mode-000 files, so cannot exercise this failure.
    const auto directory = root / "directory/BMSSEARCH/package";
    const auto transaction = root / "directory/BMSSEARCH/.asobmashow-transactions/41b74c17-434d-423a-a40f-9399b2f0382c";
    std::filesystem::create_directories(directory);
    std::filesystem::create_directories(transaction / "commit/nested");
    std::ofstream(directory / "old") << "old";
    const auto payload = transaction / "commit/nested/payload";
    std::ofstream(payload) << "new";
    PublicationJournal journal(root / "directory/publication.json", directory);
    assert(journal.beginCopy(transaction));
    std::error_code error;
    journal.rename(directory, transaction / "backup", error);
    assert(!error);
    std::filesystem::permissions(payload, std::filesystem::perms::none);
    journal.rename(transaction / "commit", directory, error);
    // Restore permissions even when the old implementation incorrectly installs.
    std::filesystem::permissions(std::filesystem::exists(payload) ? payload : directory / "nested/payload",
        std::filesystem::perms::owner_all);
    assert(error && "an unsyncable nested payload must prevent the install journal and rename");
    assert(journal.recover() == PublicationRecovery::Retry);
    assert(std::filesystem::exists(directory / "old"));
  }
#endif
  std::filesystem::remove_all(root);
}
