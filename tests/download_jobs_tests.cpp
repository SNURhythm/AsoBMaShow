#include "downloads/DownloadJobs.h"
#include "downloads/DownloadUrlPolicy.h"
#include "downloads/DownloadPathCodec.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <random>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace downloads;

static Request request() {
  Request r;
  r.url = "https://example.com/archive.zip";
  r.displayUrl = r.url;
  r.title = "Test package";
  r.archiveName = "archive.zip";
  r.libraryRoot = "/tmp/bms-library";
  r.expectedHash = std::string(64, 'a');
  return r;
}

static void testContainerRelocation(const std::filesystem::path &root) {
  for (const bool legacy : {false, true}) {
    const auto containers = root / (legacy ? "legacy" : "relative") / "Containers/Data/Application";
    const auto oldContainer = containers / "11111111-1111-4111-8111-111111111111";
    const auto newContainer = containers / "22222222-2222-4222-8222-222222222222";
    const std::filesystem::path suffix = "Library/Application Support/GingerRushDownloadsV1";
    const auto oldRoot = oldContainer / suffix;
    std::string id;
    {
      JobStore store(oldRoot);
      auto input = request();
      input.libraryRoot = oldContainer / "Documents/BMS";
      id = store.submit(input).id;
      for (const auto state : {State::Transferring, State::Downloaded, State::Importing}) {
        auto job = *store.get(id);
        job.state = state;
        assert(store.update(job));
      }
      const auto staging = oldRoot / "imports/11111111-1111-4111-8111-111111111111";
      const auto library = input.libraryRoot / "BMSSEARCH";
      auto job = *store.get(id);
      job.publicationPrepared = true;
      job.decision = BmsSearchPendingArtifactDecision::Keep;
      job.result = BmsSearchResult{.status = BmsSearchResult::Status::HashMismatch,
          .outputPath = library / "package", .removedPaths = {library / "old.zip", root / "external.zip"},
          .pendingArtifact = BmsSearchPendingArtifact{.kind = BmsSearchPendingArtifactKind::Archive,
              .stagingRoot = staging, .sourcePath = store.archivePath(id), .downloadRoot = library,
              .destinationPath = library / "_archives/package.zip", .archiveName = "package.zip",
              .storageKey = "package", .alternateDestinationPath = library / "package",
              .originalFilesIndexPath = staging / "original-files.sqlite"}};
      assert(store.update(job));
      std::ofstream(store.archivePath(id)) << "retained";
    }
    {
      std::ifstream file(oldRoot / "jobs.json");
      auto manifest = nlohmann::json::parse(file);
      assert(manifest.at("version") == 2);
      assert(manifest.dump().find(oldContainer.string()) == std::string::npos);
      if (legacy) {
        const auto absolute = [&](auto &self, nlohmann::json &value) -> void {
          if (value.is_object() && value.contains("containerRelative"))
            value = (oldContainer / value.at("containerRelative").get<std::string>()).string();
          else if (value.is_structured()) for (auto &child : value) self(self, child);
        };
        absolute(absolute, manifest);
        manifest["version"] = 1;
        std::ofstream(oldRoot / "jobs.json") << manifest.dump();
      }
    }
    std::filesystem::rename(oldContainer, newContainer);
    JobStore moved(newContainer / suffix);
    const auto job = *moved.get(id);
    const auto &artifact = *job.result->pendingArtifact;
    const auto library = newContainer / "Documents/BMS/BMSSEARCH";
    assert(job.publicationPrepared && job.decision == BmsSearchPendingArtifactDecision::Keep);
    assert(job.request.libraryRoot == library.parent_path());
    assert(artifact.stagingRoot == newContainer / suffix / "imports/11111111-1111-4111-8111-111111111111");
    assert(artifact.sourcePath == moved.archivePath(id) && std::filesystem::file_size(artifact.sourcePath) == 8);
    assert(artifact.downloadRoot == library && artifact.destinationPath == library / "_archives/package.zip");
    assert(artifact.alternateDestinationPath == library / "package");
    assert(artifact.originalFilesIndexPath == artifact.stagingRoot / "original-files.sqlite");
    assert(job.result->outputPath == library / "package");
    assert(job.result->removedPaths == (std::vector<std::filesystem::path>{library / "old.zip", root / "external.zip"}));
    assert(!std::filesystem::exists(oldContainer));
    const DownloadPathCodec codec(newContainer / suffix);
    for (const auto *unsafe : {"../Documents/escape", "Documents/../Library/private", "Library/OtherApp", "/Documents/escape"}) {
      bool rejected = false;
      try { codec.decode(nlohmann::json{{"containerRelative", unsafe}}); } catch (...) { rejected = true; }
      assert(rejected);
    }
  }
}

int main() {
  {
    Job job;
    for (const auto state : {State::Queued, State::Transferring, State::Downloaded,
                            State::Importing, State::NeedsAttention, State::Failed}) {
      job.state = state;
      assert(needsNotification(job));
    }
    job.state = State::Complete;
    assert(needsNotification(job)); // Library indexing is still pending.
    job.libraryAcknowledged = true;
    assert(!needsNotification(job));
    job.state = State::Cancelled;
    assert(!needsNotification(job));
  }

  const auto root = std::filesystem::temp_directory_path() /
      ("asobms-download-jobs-" + std::to_string(std::random_device{}()));
  testContainerRelocation(root / "relocation");
  {
    const auto removalRoot = root / "removal";
    JobStore store(removalRoot);
    auto job = store.submit(request());
    job.state = State::Cancelled;
    assert(store.update(job));
    const auto directory = store.jobPath(job.id);
    std::ofstream(directory / "archive.zip") << "retained payload";
#ifndef _WIN32
    if (geteuid() != 0) {
      std::filesystem::permissions(directory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
      bool forgotten = false;
      try { forgotten = store.forget(job.id); } catch (...) {}
      std::filesystem::permissions(directory, std::filesystem::perms::owner_all);
      assert(!forgotten && store.get(job.id));
      JobStore restarted(removalRoot);
      assert(restarted.get(job.id) && std::filesystem::exists(directory / "archive.zip"));
    }
#endif
    // A crash after physical deletion but before the manifest commit must
    // leave a visible, idempotently removable history row.
    std::filesystem::remove_all(directory);
    JobStore restarted(removalRoot);
    assert(restarted.get(job.id));
    assert(restarted.forget(job.id));
    assert(!restarted.get(job.id));
    assert(!JobStore(removalRoot).get(job.id));
  }
  {
    JobStore store(root);
    auto retired = request();
    retired.browserTransferId = std::string(32, 'b');
    assert(!validRequest(retired));
    retired = request(); retired.fallbackUrls = {"https://other.example/song.zip"};
    assert(!validRequest(retired));
    const auto first = store.submit(request());
    auto second = store.submit(request());
    assert(first.id != second.id);
    assert(store.jobs().at(0).id == first.id);
    auto cancelled = *store.get(first.id);
    cancelled.state = State::Cancelled;
    assert(store.update(cancelled));
    auto stale = first;
    stale.state = State::Transferring;
    assert(!store.update(stale)); // A late callback cannot resurrect cancellation.
    auto current = *store.get(first.id);
    current.state = State::Downloaded;
    assert(!store.update(current)); // Even a fresh callback obeys terminal state.
    second.state = State::Transferring;
    assert(store.update(second));
    second = *store.get(second.id);
    second.state = State::Downloaded;
    second.downloadedBytes = 123;
    assert(store.update(second));
    { std::ofstream legacy(root / second.id / "archive"); legacy << "retained"; }
    JobStore reopened(root);
    assert(reopened.get(first.id)->request.browserTransferId.empty());
    assert(reopened.get(second.id)->request.browserTransferId.empty());
    assert(reopened.get(first.id)->state == State::Cancelled);
    assert(reopened.get(second.id)->downloadedBytes == 123);
    assert(reopened.archivePath(second.id) == root / second.id / "archive.zip");
    assert(std::filesystem::file_size(reopened.archivePath(second.id)) == 8);
    assert(!std::filesystem::exists(root / second.id / "archive"));
    bool rejected = false;
    try { (void)reopened.archivePath("../../escape"); } catch (...) { rejected = true; }
    assert(rejected);
#ifndef _WIN32
    struct stat info{};
    assert(stat((root / "jobs.json").c_str(), &info) == 0);
    assert((info.st_mode & 077) == 0);
#endif
    auto importing = *reopened.get(second.id);
    importing.state = State::Importing;
    assert(reopened.update(importing));
    auto completed = *reopened.get(second.id);
    completed.state = State::Complete;
    completed.result = BmsSearchResult{.status = BmsSearchResult::Status::Downloaded,
                                      .outputPath = "/tmp/bms-library/BMSSEARCH/song"};
    assert(reopened.update(completed));
    assert(!reopened.forget(second.id)); // Index delivery must survive closing UI.
    JobStore recovered(root);
    const auto saved = *recovered.get(second.id);
    assert(saved.state == State::Complete);
    assert(saved.result->outputPath == "/tmp/bms-library/BMSSEARCH/song");
    assert(!importedLibraryPath(saved)); // Locate waits for library indexing.
    auto acknowledged = saved;
    acknowledged.libraryAcknowledged = true;
    assert(recovered.update(acknowledged));
    JobStore indexed(root);
    assert(importedLibraryPath(*indexed.get(second.id)) == saved.result->outputPath);
    auto packed = *indexed.get(second.id);
    packed.result->outputPath = "/tmp/bms-library/BMSSEARCH/song.zip";
    assert(importedLibraryPath(packed) == packed.result->outputPath);
    packed.state = State::NeedsAttention;
    assert(!importedLibraryPath(packed));
    packed.state = State::Cancelled;
    assert(!importedLibraryPath(packed));
    packed.state = State::Complete;
    packed.result.reset();
    assert(!importedLibraryPath(packed));
    assert(recovered.forget(second.id));
  }
  {
    auto invalid = request();
    invalid.url = "file:///etc/passwd";
    assert(!validRequest(invalid));
    invalid = request(); invalid.headers = {{"Cookie", "a=b\r\nX-Evil: y"}};
    assert(!validRequest(invalid));
    invalid = request(); invalid.archiveName = "../../outside.zip";
    assert(!validRequest(invalid));
    invalid = request(); invalid.url = "https://user:secret@example.com/a";
    assert(!validRequest(invalid));
    invalid = request(); invalid.expectedHash = "not-a-hash";
    assert(!validRequest(invalid));
    assert(allowedRedirect("https://example.com/a", "https://other.example/b"));
    assert(!allowedRedirect("https://example.com/a", "http://example.com/b"));
    assert(!allowedRedirect("https://example.com/a", "file:///tmp/b"));
    assert(!sameOrigin("https://example.com/a", "https://example.com.evil/b"));
    assert(sameOrigin("https://EXAMPLE.com:443/a", "https://example.com/b"));

  }
  {
    std::ofstream(root / "jobs.json", std::ios::trunc) << "{broken";
    bool rejected = false;
    try { JobStore store(root); } catch (...) { rejected = true; }
    assert(rejected); // Corruption must not be replaced by an empty queue.
  }
  {
    std::ofstream huge(root / "jobs.json", std::ios::binary | std::ios::trunc);
    huge.seekp(8 * 1024 * 1024);
    huge.put(' ');
    huge.close();
    bool rejected = false;
    try { JobStore store(root); } catch (...) { rejected = true; }
    assert(rejected); // Reject size before reading/parsing a malformed manifest.
  }
  std::filesystem::remove_all(root);
  std::cout << "download jobs passed\n";
}
