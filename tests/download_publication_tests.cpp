#include "downloads/DownloadPublication.h"
#include "AtomicFile.h"
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
  for (const bool failInstall : {false, true}) {
    const auto base = root / (failInstall ? "install-sync" : "backup-sync");
    const auto destination = base / "BMSSEARCH/package";
    const auto transaction = base / "BMSSEARCH/.asobmashow-transactions/41b74c17-434d-423a-a40f-9399b2f0382c";
    std::filesystem::create_directories(transaction);
    std::ofstream(destination) << "old";
    std::ofstream(transaction / "commit") << "new";
    bool fail = true;
    PublicationJournal journal(base / "publication.json", destination, [&](const auto &directory, auto &message) {
      const bool moved = failInstall ? !std::filesystem::exists(transaction / "commit") : !std::filesystem::exists(destination);
      if (fail && moved && directory == destination.parent_path()) { message = "injected fsync failure"; return false; }
      return atomic_file::syncDirectory(directory, message);
    });
    assert(journal.beginCopy(transaction));
    std::error_code error;
    bool interrupted = false;
    try {
      journal.rename(destination, transaction / "backup", error);
      assert(!error);
      if (failInstall) journal.rename(transaction / "commit", destination, error);
    } catch (const std::exception &) { interrupted = true; }
    assert(interrupted && "a moved-but-unsynced publication must bypass destructive legacy cleanup");
    assert(std::filesystem::exists(transaction / "backup"));
    if (failInstall) {
      assert(journal.recover() == PublicationRecovery::Conflict);
      assert(std::filesystem::exists(transaction / "backup"));
    }
    fail = false;
    assert(journal.recover() == (failInstall ? PublicationRecovery::Installed : PublicationRecovery::Retry));
    std::string content;
    std::ifstream(destination) >> content;
    assert(content == (failInstall ? "new" : "old"));
  }
#ifndef _WIN32
  if (geteuid() != 0) { // Root can sync mode-000 files, so cannot exercise this failure.
    for (const bool blockTransactionRoot : {true, false}) {
      const auto base = root / (blockTransactionRoot ? "transaction-ancestor" : "library-ancestor");
      const auto destination = base / "BMSSEARCH/package";
      const auto transaction = base / "BMSSEARCH/.asobmashow-transactions/41b74c17-434d-423a-a40f-9399b2f0382c";
      std::filesystem::create_directories(transaction);
      std::ofstream(destination) << "old";
      std::ofstream(transaction / "commit") << "new";
      PublicationJournal journal(base / "publication.json", destination);
      assert(journal.beginCopy(transaction));
      const auto blocked = blockTransactionRoot ? transaction.parent_path() : destination.parent_path();
      // Renames remain permitted, but opening this directory for fsync fails.
      std::filesystem::permissions(blocked, std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
      std::error_code error;
      journal.rename(destination, transaction / "backup", error);
      std::filesystem::permissions(blocked, std::filesystem::perms::owner_all);
      assert(error && "unsynced transaction ancestry must prevent moving the existing installation");
      assert(std::filesystem::exists(destination));
      assert(!std::filesystem::exists(transaction / "backup"));
      assert(journal.recover() == PublicationRecovery::Retry);
      std::string text;
      std::ifstream(destination) >> text;
      assert(text == "old");
    }
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
