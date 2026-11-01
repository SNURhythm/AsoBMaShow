#pragma once

#include "../BmsSearchService.h"
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace downloads {

enum class State { Queued, Transferring, Downloaded, Importing, NeedsAttention,
                   Complete, Cancelled, Failed };

// Values captured at the user's action. Never retain a scene or callback here.
struct Request {
  std::string url, displayUrl, title, archiveName, expectedHash, storageIdentity;
  std::filesystem::path libraryRoot;
  std::vector<std::pair<std::string, std::string>> headers;
  std::vector<std::string> fallbackUrls;
  bool skipUnarchiving = false;
  storage_management::StoragePolicy storagePolicy;
};

struct Job {
  std::string id;
  std::uint64_t revision = 0, order = 0, attempt = 0;
  State state = State::Queued;
  Request request;
  std::uint64_t downloadedBytes = 0, totalBytes = 0;
  std::string message;
  std::uint32_t fallbackIndex = 0;
  bool confirmationUsed = false;
  std::string transferUrl; // Validated provider continuation; original selection remains immutable.
  std::optional<BmsSearchResult> result;
  bool libraryAcknowledged = false;
  std::string libraryError;
  bool publicationPrepared = false;
  std::optional<BmsSearchPendingArtifactDecision> decision;
};

Request transferRequest(const Job &job);
std::optional<std::filesystem::path> importedLibraryPath(const Job &job);
bool validRequest(const Request &request);
bool terminal(State state);
// Outstanding download work or an error/decision that still needs attention.
bool needsNotification(const Job &job);
const char *stateName(State state);

// One store per process. All operations are serialized; updates use revisions
// so late native callbacks cannot overwrite cancellation or a newer attempt.
// Failed/corrupt loads throw instead of silently erasing recoverable jobs.
class JobStore final {
public:
  explicit JobStore(std::filesystem::path root);
  Job submit(Request request);
  std::vector<Job> jobs() const;
  std::optional<Job> get(const std::string &id) const;
  bool update(Job job);
  bool forget(const std::string &id);
  std::filesystem::path jobPath(const std::string &id) const;
  std::filesystem::path archivePath(const std::string &id, std::uint64_t attempt = 0) const;
  const std::filesystem::path &root() const { return root_; }
private:
  void save(const std::vector<Job> &jobs) const;
  std::filesystem::path root_;
  mutable std::mutex mutex_;
  std::vector<Job> jobs_;
};

} // namespace downloads
