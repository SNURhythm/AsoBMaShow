#include "downloads/DownloadPublication.h"
#include "downloads/DownloadPublicationPaths.h"
#include "downloads/DownloadPathCodec.h"
#include "AtomicFile.h"
#include <cassert>
#include <fstream>
#include <random>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace downloads;

void testIOSPublicationRelocation(const std::filesystem::path &root) {
  for (const bool legacy : {false, true}) for (const bool installed : {false, true}) {
    const auto containers = root / (legacy ? "legacy" : "relative") /
        (installed ? "installed" : "backup") / "Containers/Data/Application";
    const auto oldContainer = containers / "11111111-1111-4111-8111-111111111111";
    const auto newContainer = containers / "22222222-2222-4222-8222-222222222222";
    const std::filesystem::path storeSuffix = "Library/Application Support/GingerRushDownloadsV1";
    const std::filesystem::path journalSuffix = storeSuffix / "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/publication.json";
    const std::filesystem::path destinationSuffix = "Documents/BMS/BMSSEARCH/package";
    const auto transactionSuffix = destinationSuffix.parent_path() /
        ".asobmashow-transactions/41b74c17-434d-423a-a40f-9399b2f0382c";
    const auto transaction = oldContainer / transactionSuffix;
    const auto destination = oldContainer / destinationSuffix;
    std::filesystem::create_directories(transaction);
    std::filesystem::create_directories((oldContainer / journalSuffix).parent_path());
    std::ofstream(destination) << "old";
    std::ofstream(transaction / "commit") << "new";
    {
      PublicationJournal journal(oldContainer / journalSuffix, destination);
      assert(journal.beginCopy(transaction));
      std::error_code error;
      journal.rename(destination, transaction / "backup", error);
      assert(!error);
      if (installed) {
        journal.rename(transaction / "commit", destination, error);
        assert(!error);
        std::string message;
        assert(journal.recordRemovals({destination.parent_path() / "old.zip"}, message));
      }
    }
    {
      std::ifstream file(oldContainer / journalSuffix);
      auto record = nlohmann::json::parse(file);
      assert(record.at("version") == 2 && record.dump().find(oldContainer.string()) == std::string::npos);
      if (legacy) {
        const DownloadPathCodec codec(oldContainer / storeSuffix);
        for (const auto *key : {"destination", "transaction"}) record.at(key) = codec.decode(record.at(key));
        if (record.contains("removedPaths")) for (auto &path : record.at("removedPaths")) path = codec.decode(path);
        record["version"] = 1;
        std::ofstream(oldContainer / journalSuffix) << record.dump();
      }
    }
    std::filesystem::rename(oldContainer, newContainer);
    PublicationJournal recovered(newContainer / journalSuffix, newContainer / destinationSuffix);
    assert(recovered.recover() == (installed ? PublicationRecovery::Installed : PublicationRecovery::Retry));
    std::string content;
    std::ifstream(newContainer / destinationSuffix) >> content;
    assert(content == (installed ? "new" : "old"));
    assert(!std::filesystem::exists(newContainer / transactionSuffix));
    if (installed) assert(recovered.removedPaths() ==
        std::vector<std::filesystem::path>{newContainer / destinationSuffix.parent_path() / "old.zip"});
    assert(!std::filesystem::exists(oldContainer));
  }
}

void testAndroidPrivatePublication(const std::filesystem::path &root) {
  const auto external = root / "external/files";
  const auto internal = root / "internal/files";
  std::filesystem::create_directories(external / "Documents/BMS/BMSSEARCH");
  std::filesystem::create_directories(internal / "Documents/BMS/BMSSEARCH");
  const std::array roots{external, internal};
  for (const auto &appRoot : roots) {
    const auto canonicalRoot = std::filesystem::canonical(appRoot);
    const auto library = canonicalRoot / "Documents/BMS/BMSSEARCH";
    const auto privateRoot = androidPublicationTransactionRoot(library, roots);
    assert(privateRoot == canonicalRoot / ".asobmashow-download-transactions");
    for (const bool installed : {false, true}) {
      const auto destination = library / (installed ? "installed" : "rollback");
      const auto journalPath = canonicalRoot / "publication.json";
      const auto transaction = privateRoot / "41b74c17-434d-423a-a40f-9399b2f0382c";
      std::filesystem::create_directories(transaction);
      std::ofstream(destination) << "old";
      std::ofstream(transaction / "commit") << "new";
      PublicationJournal journal(journalPath, destination, {}, privateRoot);
      assert(!journal.beginCopy(library / ".asobmashow-transactions" / transaction.filename()));
      assert(journal.beginCopy(transaction));
      std::error_code error;
      journal.rename(destination, transaction / "backup", error);
      assert(!error);
      if (installed) {
        journal.rename(transaction / "commit", destination, error);
        assert(!error);
      }
      // A persisted path cannot grant recovery authority over another root.
      PublicationJournal wrongRoot(journalPath, destination, {}, canonicalRoot / "elsewhere");
      assert(wrongRoot.recover() == PublicationRecovery::Conflict);
      assert(std::filesystem::exists(transaction / "backup"));
      PublicationJournal restarted(journalPath, destination, {}, privateRoot);
      assert(restarted.recover() == (installed ? PublicationRecovery::Installed : PublicationRecovery::Retry));
      std::string content;
      std::ifstream(destination) >> content;
      assert(content == (installed ? "new" : "old"));
      assert(!std::filesystem::exists(transaction));
    }
  }
  assert(androidPublicationTransactionRoot(root / "unowned/BMSSEARCH", roots).empty());
  assert(androidPublicationTransactionRoot(external / "Documents/../outside/BMSSEARCH", roots).empty());
#ifndef _WIN32
  const auto privateRoot = std::filesystem::canonical(external) / ".asobmashow-download-transactions";
  std::filesystem::remove_all(privateRoot);
  std::filesystem::create_directory_symlink(external / "Documents", privateRoot);
  assert(androidPublicationTransactionRoot(external / "Documents/BMS/BMSSEARCH", roots).empty());
#endif
}

int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("asobms-publication-" + std::to_string(std::random_device{}()));
  testAndroidPrivatePublication(root / "android");
  testIOSPublicationRelocation(root / "ios");
  {
    const auto base = root / "durable-cleanup";
    const auto destination = base / "BMSSEARCH/package";
    const auto transaction = base / "BMSSEARCH/.asobmashow-transactions/41b74c17-434d-423a-a40f-9399b2f0382c";
    std::filesystem::create_directories(transaction / "commit");
    std::ofstream(destination) << "old";
    std::ofstream(transaction / "commit/payload") << "unfinished copy";
    bool fail = true, removalSynced = false;
    PublicationJournal journal(base / "publication.json", destination, [&](const auto &directory, auto &error) {
      if (directory == transaction.parent_path() && !std::filesystem::exists(transaction)) {
        if (fail) { error = "injected cleanup fsync failure"; return false; }
        removalSynced = true;
      }
      return atomic_file::syncDirectory(directory, error);
    });
    assert(journal.beginCopy(transaction));
    assert(journal.recover() == PublicationRecovery::Conflict);
    assert(std::filesystem::exists(base / "publication.json"));
    fail = false;
    assert(journal.recover() == PublicationRecovery::Retry && removalSynced);
    assert(!std::filesystem::exists(base / "publication.json"));
    assert(std::filesystem::exists(destination));
  }
  for (const bool archive : {false, true}) {
    const auto existing = root / (archive ? "first-archive" : "first-directory");
    std::filesystem::create_directories(existing);
    const auto library = std::filesystem::canonical(existing) / "BMSSEARCH";
    const auto destination = archive ? library / "_archives/package.zip" : library / "package";
    const auto transaction = library / ".asobmashow-transactions/41b74c17-434d-423a-a40f-9399b2f0382c";
    std::filesystem::create_directories(transaction);
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream(transaction / "commit") << "new";
    bool parentSyncedBeforeInstall = false;
    PublicationJournal journal(existing / "publication.json", destination, [&](const auto &directory, auto &error) {
      if (directory == library.parent_path() && !std::filesystem::exists(destination)) parentSyncedBeforeInstall = true;
      return atomic_file::syncDirectory(directory, error);
    });
    assert(journal.beginCopy(transaction));
    std::error_code error;
    journal.rename(transaction / "commit", destination, error);
    assert(!error);
    assert(parentSyncedBeforeInstall && "persist the newly created BMSSEARCH entry before installing");
  }
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
