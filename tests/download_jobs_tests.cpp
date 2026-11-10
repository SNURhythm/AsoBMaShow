#include "downloads/DownloadJobs.h"
#include "downloads/DownloadUrlPolicy.h"
#include <cassert>
#include <fstream>
#include <iostream>
#include <random>
#ifndef _WIN32
#include <sys/stat.h>
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
