#include "downloads/DownloadCoordinator.h"
#include <cassert>
#include <condition_variable>
#include <fstream>
#include <future>
#include <random>

using namespace downloads;
using namespace std::chrono_literals;
static Request request(const std::filesystem::path &root) {
  Request r;
  r.url = r.displayUrl = "https://example.com/song.zip";
  r.archiveName = "song.zip";
  r.libraryRoot = root / "library";
  return r;
}
int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("asobms-download-coordinator-" + std::to_string(std::random_device{}()));
  std::vector<std::string> started, stopped;
  CoordinatorHooks hooks;
  hooks.startTransfer = [&](const Job &job, const auto &) {
    started.push_back(job.id);
    return std::string{};
  };
  hooks.cancelTransfer = [&](const Job &job) { stopped.push_back(job.id); };
  {
    DownloadCoordinator service(root, hooks);
    const auto a = service.submit(request(root));
    const auto b = service.submit(request(root));
    assert(started.empty()); // A background launch cannot admit a new user job.
    service.setForeground(true);
    assert(started == std::vector<std::string>{a.id});
    assert(service.cancel(a.id));
    assert(stopped == std::vector<std::string>{a.id});
    assert(started == (std::vector<std::string>{a.id, b.id}));
    const auto file = root / "system-download";
    std::ofstream(file) << "archive";
    assert(!service.acceptFile(a.id, a.attempt, file));
    assert(service.get(a.id)->state == State::Cancelled);
    assert(service.acceptFile(b.id, b.attempt, file));
    assert(service.get(b.id)->state == State::Downloaded);
    assert(std::filesystem::file_size(service.archivePath(b.id)) == 7);
  }
  {
    DownloadCoordinator service(root, hooks);
    const auto jobs = service.jobs();
    assert(jobs[0].state == State::Cancelled);
    assert(jobs[1].state == State::Downloaded);
    assert(started.size() == 2); // Reopening must not redownload a completed file.
    hooks.importArchive = [&](const Job &, const auto &path, std::atomic_bool &cancelled,
                               auto, auto) {
      assert(!cancelled);
      assert(std::filesystem::file_size(path) == 7);
      return BmsSearchResult{.status = BmsSearchResult::Status::Downloaded,
                             .outputPath = root / "library/song"};
    };
  }
  {
    DownloadCoordinator service(root, hooks);
    service.setForeground(true);
    service.enableImports(true);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (service.jobs()[1].state != State::Complete && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(service.jobs()[1].state == State::Complete);
    assert(service.readyForLibrary().size() == 1);
    const auto id = service.readyForLibrary()[0].id;
    assert(service.acknowledgeLibrary(id));
    assert(service.readyForLibrary().empty());
    assert(!service.cancel(id));
  }
  std::filesystem::remove_all(root);
}
