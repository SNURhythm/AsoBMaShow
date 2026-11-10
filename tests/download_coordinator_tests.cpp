#include "downloads/DownloadCoordinator.h"
#include "downloads/DownloadLibraryDelivery.h"
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
    int enqueued = 0;
    LibraryDeliveryState status = LibraryDeliveryState::Pending;
    DownloadLibraryDelivery delivery([&](const Job &) { return ++enqueued; },
        [&](std::uint64_t) { return status; });
    delivery.poll(service); delivery.poll(service);
    assert(enqueued == 1 && service.readyForLibrary().size() == 1);
    status = LibraryDeliveryState::Failed;
    delivery.poll(service); delivery.poll(service);
    assert(enqueued == 1 && !service.get(id)->libraryError.empty());
    assert(service.retry(id));
    status = LibraryDeliveryState::Pending;
    delivery.poll(service);
    assert(enqueued == 2);
    status = LibraryDeliveryState::Complete;
    delivery.poll(service);
    assert(service.readyForLibrary().empty());
    assert(!service.cancel(id));
  }
  {
    DownloadCoordinator service(root, hooks);
    service.setForeground(true);
    const auto job = service.submit(request(root));
    const auto queued = service.submit(request(root));
    assert(service.get(queued.id)->state == State::Queued);
    service.interrupt(job.id, "Runtime expired");
    assert(service.get(job.id)->state == State::Failed);
    assert(!service.isCurrentTransfer(job.id, job.attempt));
    assert(service.get(queued.id)->state == State::Transferring);
    service.transferFailed(job.id, job.attempt, "Late cancellation callback");
    assert(service.get(queued.id)->state == State::Transferring);
    service.cancel(queued.id);
    assert(service.retry(job.id));
    assert(service.get(job.id)->attempt == job.attempt + 1);
    service.cancel(job.id);
  }
  {
    std::promise<void> entered, released;
    auto wait = released.get_future();
    hooks.importArchive = [&](const Job &, const auto &, std::atomic_bool &cancelled, auto, auto) {
      entered.set_value();
      wait.wait();
      assert(cancelled);
      return BmsSearchResult{.status = BmsSearchResult::Status::DownloadFailed};
    };
    DownloadCoordinator service(root, hooks);
    service.setForeground(true);
    const auto job = service.submit(request(root));
    const auto file = root / "system-completed";
    std::ofstream(file) << "archive";
    service.acceptFile(job.id, job.attempt, file);
    service.enableImports(true);
    assert(entered.get_future().wait_for(5s) == std::future_status::ready);
    service.interrupt(job.id, "Runtime expired");
    released.set_value();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (service.get(job.id)->state == State::Importing && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(service.get(job.id)->state == State::NeedsAttention);
    assert(std::filesystem::is_regular_file(service.archivePath(job.id)));
  }
  {
    CoordinatorHooks follow;
    follow.startTransfer = [](const Job &, const auto &) { return std::string{}; };
    follow.continueTransfer = [](const Job &, const auto &) { return std::string("https://files.example/confirmed.zip"); };
    follow.importArchive = [](const Job &, const auto &, auto &, auto, auto) {
      assert(false && "confirmation response must not enter the archive importer");
      return BmsSearchResult{};
    };
    DownloadCoordinator service(root / "continuation", follow);
    service.setForeground(true);
    auto input = request(root);
    input.headers = {{"Cookie", "session=private"}};
    const auto job = service.submit(input);
    const auto file = root / "confirmation";
    std::ofstream(file) << "confirmation page";
    assert(service.acceptFile(job.id, 0, file));
    service.enableImports(true);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (service.get(job.id)->attempt == 0 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    const auto continued = service.get(job.id);
    assert(continued->attempt == 1);
    assert(continued->transferUrl == "https://files.example/confirmed.zip");
    assert(transferRequest(*continued).headers.empty());
    assert(continued->request.url == input.url);
    assert(!service.acceptFile(job.id, 0, file));
    service.cancel(job.id);
  }
  {
    CoordinatorHooks transport;
    int starts = 0;
    transport.startTransfer = [&](const Job &, const auto &) { ++starts; return std::string{}; };
    DownloadCoordinator service(root / "rejected", transport);
    service.setForeground(true);
    auto input = request(root);
    input.url = "https://pixeldra.in/api/file/abcd1234?download";
    const auto job = service.submit(input);
    service.transferFailed(job.id, 0, "HTTP rejected", 403);
    assert(service.get(job.id)->state == State::Failed);
    assert(service.get(job.id)->attempt == 0 && starts == 1);
    assert(transferRequest(*service.get(job.id)).url == input.url);

  }
  {
    CoordinatorHooks handoff;
    int starts = 0;
    handoff.startTransfer = [&](const Job &, const auto &) { ++starts; return std::string{}; };
    auto directory = root / "handoff-failure";
    DownloadCoordinator service(directory, handoff);
    service.setForeground(true);
    const auto job = service.submit(request(root));
    auto manifest = directory / "jobs.json";
    std::filesystem::rename(manifest, directory / "saved.json");
    std::filesystem::create_directory(manifest); // force atomic manifest replacement to fail
    auto file = root / "handoff-complete";
    std::ofstream(file) << "archive";
    try { service.acceptFile(job.id, 0, file); } catch (const std::exception &) {}
    assert(service.get(job.id)->state == State::Failed);
    assert(!service.isCurrentTransfer(job.id, 0));
    assert(std::filesystem::exists(service.archivePath(job.id)));
    std::filesystem::remove(manifest);
    std::filesystem::rename(directory / "saved.json", manifest);
    assert(service.retry(job.id));
    assert(service.get(job.id)->state == State::Downloaded);
    assert(starts == 1 && service.error().empty());
  }
  {
    CoordinatorHooks mismatch;
    bool cleaned = false;
    mismatch.startTransfer = [](const Job &, const auto &) { return std::string{}; };
    mismatch.cleanup = [&](const Job &) { cleaned = true; };
    mismatch.importArchive = [](const Job &, const auto &, auto &, auto, auto) {
      return BmsSearchResult{.status = BmsSearchResult::Status::HashMismatch,
          .pendingArtifact = BmsSearchPendingArtifact{}};
    };
    DownloadCoordinator service(root / "cancel-artifact", mismatch);
    service.setForeground(true);
    const auto job = service.submit(request(root));
    auto file = root / "mismatch-file"; std::ofstream(file) << "archive";
    service.acceptFile(job.id, 0, file); service.enableImports(true);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (service.get(job.id)->state != State::NeedsAttention && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(service.get(job.id)->state == State::NeedsAttention);
    assert(service.cancel(job.id));
    assert(service.forget(job.id) && cleaned);
    assert(!service.get(job.id));
  }
  {
    std::promise<void> entered, release;
    auto released = release.get_future();
    std::atomic<int> imports = 0;
    std::promise<void> nextImport;
    CoordinatorHooks maintenance;
    maintenance.startTransfer = [](const Job &, const auto &) { return std::string{}; };
    maintenance.importArchive = [&](const Job &, const auto &, auto &, auto, auto prepare) {
      if (++imports == 1) {
        assert(prepare(BmsSearchPendingArtifact{}));
        entered.set_value();
        released.wait();
        return BmsSearchResult{.status = BmsSearchResult::Status::DownloadFailed};
      }
      nextImport.set_value();
      return BmsSearchResult{.status = BmsSearchResult::Status::Downloaded, .outputPath = root / "installed"};
    };
    maintenance.resolveArtifact = [&](const auto &, auto, auto &, auto) {
      return BmsSearchResult{.status = BmsSearchResult::Status::Downloaded, .outputPath = root / "installed"};
    };
    DownloadCoordinator service(root / "maintenance", maintenance);
    service.setForeground(true);
    service.enableImports(true);
    const auto start = [&] {
      const auto job = service.submit(request(root));
      const auto file = root / "maintenance-completed";
      std::ofstream(file) << "archive";
      assert(service.acceptFile(job.id, job.attempt, file));
      return job;
    };
    const auto first = start();
    assert(entered.get_future().wait_for(5s) == std::future_status::ready);
    assert(!service.tryReserveMaintenance() && "active publication must finish before maintenance can own library paths");
    release.set_value();
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (service.get(first.id)->state == State::Importing && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(service.get(first.id)->publicationPrepared);
    assert(!service.tryReserveMaintenance() && "an unfinished publication must be recovered before maintenance");
    while (!service.retry(first.id) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    while (service.get(first.id)->state != State::Complete && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(service.get(first.id)->state == State::Complete);
    assert(service.tryReserveMaintenance());
    assert(!service.tryReserveMaintenance());
    const auto second = start();
    service.enableImports(true); // The main loop cannot undo the reservation.
    auto next = nextImport.get_future();
    assert(next.wait_for(20ms) == std::future_status::timeout);
    assert(service.get(second.id)->state == State::Downloaded && imports == 1);
    service.releaseMaintenance();
    assert(next.wait_for(5s) == std::future_status::ready);
  }
  for (const bool keep : {false, true}) {
    std::atomic<int> admissions = 0, imports = 0, transfers = 0;
    std::promise<void> finished;
    CoordinatorHooks runtime;
    runtime.startTransfer = [&](const Job &, const auto &) { ++transfers; return std::string{}; };
    runtime.beginImport = [&](const Job &) { ++admissions; };
    runtime.importArchive = [&](const Job &, const auto &, auto &, auto, auto) {
      const int count = ++imports;
      assert(admissions == count);
      if (count == 1) {
        BmsSearchResult result{.status = BmsSearchResult::Status::HashMismatch};
        if (keep) result.pendingArtifact = BmsSearchPendingArtifact{};
        return result;
      }
      return BmsSearchResult{.status = BmsSearchResult::Status::Downloaded, .outputPath = root / "installed"};
    };
    runtime.resolveArtifact = [&](const Job &, auto decision, auto &, auto) {
      assert(decision == BmsSearchPendingArtifactDecision::Keep && admissions == 2);
      return BmsSearchResult{.status = BmsSearchResult::Status::Downloaded, .outputPath = root / "installed"};
    };
    runtime.finishActivity = [&](const Job &, bool success) { if (success) finished.set_value(); };
    DownloadCoordinator service(root / (keep ? "runtime-keep" : "runtime-retry"), runtime);
    service.setForeground(true); service.enableImports(true);
    const auto job = service.submit(request(root));
    const auto file = root / "runtime-completed";
    std::ofstream(file) << "archive";
    assert(service.acceptFile(job.id, job.attempt, file));
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (service.get(job.id)->state != State::NeedsAttention && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    bool admitted = false;
    while (!(admitted = keep ? service.decide(job.id, BmsSearchPendingArtifactDecision::Keep) : service.retry(job.id)) &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(admitted);
    assert(finished.get_future().wait_for(5s) == std::future_status::ready);
    assert(admissions == 2 && transfers == 1);
  }
  {
    const auto directory = root / "interrupted-delete";
    std::string id;
    {
      JobStore store(directory);
      id = store.submit(request(root)).id;
      for (const auto state : {State::Transferring, State::Downloaded, State::Importing}) {
        auto job = *store.get(id);
        job.state = state;
        job.decision = BmsSearchPendingArtifactDecision::Delete;
        job.result = BmsSearchResult{.status = BmsSearchResult::Status::HashMismatch,
            .pendingArtifact = BmsSearchPendingArtifact{}};
        assert(store.update(job));
      }
      // The delete intent is durable, but the process died after removing the
      // retained payload and before saving the completion record.
    }
    std::atomic<int> resolutions = 0, transfers = 0;
    std::promise<void> completed;
    CoordinatorHooks deletion;
    deletion.startTransfer = [&](const auto &, const auto &) { ++transfers; return std::string{}; };
    deletion.importArchive = [](const auto &, const auto &, auto &, auto, auto) {
      assert(false && "a saved deletion must never re-import or re-download");
      return BmsSearchResult{};
    };
    deletion.resolveArtifact = [&](const Job &job, auto decision, auto &, auto) {
      assert(job.result->pendingArtifact && decision == BmsSearchPendingArtifactDecision::Delete);
      if (++resolutions == 1) return BmsSearchResult{.status = BmsSearchResult::Status::DownloadFailed};
      return BmsSearchResult{.status = BmsSearchResult::Status::HashMismatch, .message = "Downloaded files deleted."};
    };
    deletion.finishActivity = [&](const auto &, bool success) { if (success) completed.set_value(); };
    DownloadCoordinator service(directory, deletion);
    service.setForeground(true); service.enableImports(true);
    assert(service.retry(id));
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (service.get(id)->state != State::NeedsAttention && std::chrono::steady_clock::now() < deadline)
      std::this_thread::yield();
    assert(service.get(id)->decision == BmsSearchPendingArtifactDecision::Delete);
    assert(service.get(id)->result->pendingArtifact);
    while (!service.retry(id) && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    assert(completed.get_future().wait_for(5s) == std::future_status::ready);
    assert(service.get(id)->state == State::Complete && service.get(id)->libraryAcknowledged);
    assert(transfers == 0 && resolutions == 2);
  }
  std::filesystem::remove_all(root);
}
