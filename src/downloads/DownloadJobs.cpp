#include "DownloadJobs.h"
#include "BrowserPolicy.h"
#include "../AtomicFile.h"
#include "../ArchiveFile.h"
#include "../../yoga/lib/nlohmann/json.hpp"
#include <algorithm>
#include <fstream>
#include <limits>
#include <random>
#include <stdexcept>

namespace downloads {
namespace {
using json = nlohmann::json;
constexpr std::uintmax_t maxManifestBytes = 4 * 1024 * 1024;
constexpr std::size_t maxJobs = 256;
bool validId(const std::string &id) {
  return id.size() == 32 && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
std::string newId() {
  std::random_device random;
  std::string id;
  for (int i = 0; i < 32; ++i) id += "0123456789abcdef"[random() & 15];
  return id;
}
std::string utf8(const std::filesystem::path &path) {
  const auto bytes = path.u8string();
  return {bytes.begin(), bytes.end()};
}
std::filesystem::path pathAt(const json &value, const char *key) {
  const auto text = value.at(key).get<std::string>();
  return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
json policyJson(const storage_management::StoragePolicy &p) {
  return {{"audio", p.audio}, {"bga", p.bga}, {"videoQuality", p.videoQuality},
          {"resolution", p.resolution}, {"maxFps", p.maxFps},
          {"videoCodec", p.videoCodec}, {"reencodeEfficientVideo", p.reencodeEfficientVideo}};
}
storage_management::StoragePolicy readPolicy(const json &j) {
  return {j.at("audio").get<storage_management::AudioAction>(),
          j.at("bga").get<storage_management::BgaAction>(),
          j.at("videoQuality").get<storage_management::VideoQuality>(),
          j.at("resolution").get<storage_management::ResolutionCap>(),
          j.at("maxFps").get<int>(), j.at("videoCodec").get<storage_management::VideoCodec>(),
          j.at("reencodeEfficientVideo").get<bool>()};
}
json requestJson(const Request &r) {
  return {{"url", r.url}, {"displayUrl", r.displayUrl}, {"title", r.title},
          {"archiveName", r.archiveName}, {"expectedHash", r.expectedHash},
          {"storageIdentity", r.storageIdentity}, {"libraryRoot", utf8(r.libraryRoot)},
          {"fallbackUrls", r.fallbackUrls}, {"headers", r.headers}, {"skipUnarchiving", r.skipUnarchiving},
          {"storagePolicy", policyJson(r.storagePolicy)}};
}
Request readRequest(const json &j) {
  Request r;
  j.at("url").get_to(r.url); j.at("displayUrl").get_to(r.displayUrl);
  j.at("title").get_to(r.title); j.at("archiveName").get_to(r.archiveName);
  j.at("expectedHash").get_to(r.expectedHash); j.at("storageIdentity").get_to(r.storageIdentity);
  r.libraryRoot = pathAt(j, "libraryRoot");
  j.at("headers").get_to(r.headers); j.at("skipUnarchiving").get_to(r.skipUnarchiving);
  if (j.contains("fallbackUrls")) j.at("fallbackUrls").get_to(r.fallbackUrls);
  r.storagePolicy = readPolicy(j.at("storagePolicy"));
  if (!validRequest(r)) throw std::runtime_error("Invalid saved download request.");
  return r;
}
json artifactJson(const BmsSearchPendingArtifact &a) {
  return {{"kind", a.kind}, {"stagingRoot", utf8(a.stagingRoot)},
          {"sourcePath", utf8(a.sourcePath)}, {"downloadRoot", utf8(a.downloadRoot)},
          {"destinationPath", utf8(a.destinationPath)}, {"archiveName", a.archiveName},
          {"storageKey", a.storageKey}, {"alternateDestinationPath", utf8(a.alternateDestinationPath)},
          {"originalFilesIndexPath", utf8(a.originalFilesIndexPath)}};
}
BmsSearchPendingArtifact readArtifact(const json &j) {
  BmsSearchPendingArtifact a;
  a.kind = j.at("kind").get<BmsSearchPendingArtifactKind>();
  if (a.kind != BmsSearchPendingArtifactKind::Archive && a.kind != BmsSearchPendingArtifactKind::ExtractedDirectory)
    throw std::runtime_error("Invalid saved download artifact.");
  a.stagingRoot = pathAt(j, "stagingRoot"); a.sourcePath = pathAt(j, "sourcePath");
  a.downloadRoot = pathAt(j, "downloadRoot"); a.destinationPath = pathAt(j, "destinationPath");
  j.at("archiveName").get_to(a.archiveName); j.at("storageKey").get_to(a.storageKey);
  a.alternateDestinationPath = pathAt(j, "alternateDestinationPath");
  a.originalFilesIndexPath = pathAt(j, "originalFilesIndexPath");
  return a;
}
json resultJson(const BmsSearchResult &r) {
  json removed = json::array();
  for (const auto &p : r.removedPaths) removed.push_back(utf8(p));
  return {{"status", r.status}, {"message", r.message}, {"outputPath", utf8(r.outputPath)},
          {"removedPaths", removed}, {"httpStatus", r.downloadHttpStatus},
          {"artifact", r.pendingArtifact ? artifactJson(*r.pendingArtifact) : json(nullptr)}};
}
BmsSearchResult readResult(const json &j) {
  BmsSearchResult r;
  r.status = j.at("status").get<BmsSearchResult::Status>();
  if (r.status < BmsSearchResult::Status::NotFound || r.status > BmsSearchResult::Status::DownloadFailed)
    throw std::runtime_error("Invalid saved download result.");
  j.at("message").get_to(r.message); r.outputPath = pathAt(j, "outputPath");
  j.at("httpStatus").get_to(r.downloadHttpStatus);
  for (const auto &p : j.at("removedPaths")) {
    const auto text = p.get<std::string>();
    r.removedPaths.emplace_back(std::u8string(text.begin(), text.end()));
  }
  if (!j.at("artifact").is_null()) r.pendingArtifact = readArtifact(j.at("artifact"));
  return r;
}
json jobJson(const Job &j) {
  return {{"id", j.id}, {"revision", j.revision}, {"order", j.order},
          {"attempt", j.attempt}, {"state", j.state}, {"request", requestJson(j.request)},
          {"downloadedBytes", j.downloadedBytes}, {"totalBytes", j.totalBytes},
          {"message", j.message}, {"transferUrl", j.transferUrl}, {"fallbackIndex", j.fallbackIndex}, {"confirmationUsed", j.confirmationUsed}, {"libraryAcknowledged", j.libraryAcknowledged}, {"libraryError", j.libraryError},
          {"publicationPrepared", j.publicationPrepared},
          {"decision", j.decision ? json(*j.decision) : json(nullptr)},
          {"result", j.result ? resultJson(*j.result) : json(nullptr)}};
}
Job readJob(const json &j) {
  Job result;
  j.at("id").get_to(result.id); j.at("revision").get_to(result.revision);
  j.at("order").get_to(result.order); j.at("attempt").get_to(result.attempt);
  j.at("state").get_to(result.state); result.request = readRequest(j.at("request"));
  j.at("downloadedBytes").get_to(result.downloadedBytes); j.at("totalBytes").get_to(result.totalBytes);
  j.at("message").get_to(result.message); j.at("libraryAcknowledged").get_to(result.libraryAcknowledged);
  result.fallbackIndex = j.value("fallbackIndex", 0u);
  result.confirmationUsed = j.value("confirmationUsed", false);
  if (result.fallbackIndex > result.request.fallbackUrls.size()) throw std::runtime_error("Invalid fallback index.");
  result.transferUrl = j.value("transferUrl", "");
  if (!result.transferUrl.empty() && !allowedRedirect(result.request.url, result.transferUrl))
    throw std::runtime_error("Invalid saved download continuation.");
  result.libraryError = j.value("libraryError", "");
  result.publicationPrepared = j.value("publicationPrepared", false);
  if (j.contains("decision") && !j.at("decision").is_null()) {
    result.decision = j.at("decision").get<BmsSearchPendingArtifactDecision>();
    if (*result.decision != BmsSearchPendingArtifactDecision::Keep &&
        *result.decision != BmsSearchPendingArtifactDecision::Delete)
      throw std::runtime_error("Invalid saved download decision.");
  }
  if (!j.at("result").is_null()) result.result = readResult(j.at("result"));
  if (!validId(result.id) || result.state < State::Queued || result.state > State::Failed ||
      result.revision == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("Invalid saved download state.");
  return result;
}
bool transition(const Job &old, const Job &next) {
  if (old.request.url != next.request.url || requestJson(old.request) != requestJson(next.request) ||
      old.order != next.order || next.attempt < old.attempt) return false;
  if (old.transferUrl != next.transferUrl &&
      !(((old.state == State::Importing && !old.confirmationUsed && next.confirmationUsed &&
           next.fallbackIndex == old.fallbackIndex) ||
          (old.state == State::Transferring && next.fallbackIndex == old.fallbackIndex + 1 &&
           next.fallbackIndex <= old.request.fallbackUrls.size() &&
           next.transferUrl == old.request.fallbackUrls[next.fallbackIndex - 1])) &&
        next.state == State::Queued && next.attempt == old.attempt + 1 &&
        allowedRedirect(transferRequest(old).url, next.transferUrl))) return false;
  if (old.state == State::Cancelled) return next.state == State::Cancelled;
  if (old.state == State::Complete) return next.state == State::Complete;
  if (next.state == State::Cancelled) return true;
  if (next.state == old.state) return next.attempt == old.attempt;
  switch (old.state) {
  case State::Queued: return next.state == State::Transferring || next.state == State::Failed || next.state == State::NeedsAttention;
  case State::Transferring: return (next.state == State::Queued && next.attempt == old.attempt + 1) || next.state == State::Downloaded || next.state == State::Failed || next.state == State::NeedsAttention;
  case State::Downloaded: return next.state == State::Importing || next.state == State::Failed || next.state == State::NeedsAttention;
  case State::Importing: return (next.state == State::Queued && next.attempt == old.attempt + 1) || next.state == State::Complete || next.state == State::NeedsAttention || next.state == State::Failed || next.state == State::Downloaded;
  case State::NeedsAttention: case State::Failed:
    return (next.state == State::Queued && next.attempt == old.attempt + 1) ||
           next.state == State::Importing || next.state == State::Downloaded || next.state == State::Complete;
  default: return false;
  }
}
}

std::optional<std::filesystem::path> importedLibraryPath(const Job &job) {
  if (job.state != State::Complete || !job.libraryAcknowledged ||
      !job.result || job.result->outputPath.empty()) return std::nullopt;
  return job.result->outputPath;
}

Request transferRequest(const Job &job) {
  auto request = job.request;
  if (!job.transferUrl.empty()) {
    request.url = job.transferUrl;
    if (httpOrigin(request.url) != httpOrigin(job.request.url))
      std::erase_if(request.headers, [](const auto &header) { return header.first != "User-Agent"; });
  }
  return request;
}
bool validRequest(const Request &r) {
  if (!httpOrigin(r.url) || (!r.displayUrl.empty() && !httpOrigin(r.displayUrl)) ||
      r.title.size() > 1024 || r.storageIdentity.size() > 16384 ||
      r.archiveName != safeArchiveName(r.archiveName) || r.archiveName.size() > 240 ||
      (!r.libraryRoot.empty() && !r.libraryRoot.is_absolute()) ||
      !storage_management::IsValid(r.storagePolicy) || r.headers.size() > 3) return false;
  if (!r.expectedHash.empty() &&
      ((r.expectedHash.size() != 32 && r.expectedHash.size() != 64) ||
       !std::all_of(r.expectedHash.begin(), r.expectedHash.end(), [](unsigned char c) { return std::isxdigit(c); }))) return false;
  if (r.fallbackUrls.size() > 2 || (!r.headers.empty() && !r.fallbackUrls.empty())) return false;
  for (const auto &url : r.fallbackUrls) if (!allowedRedirect(r.url, url)) return false;
  std::vector<std::string> seen;
  for (const auto &[name, value] : r.headers) {
    if ((name != "Cookie" && name != "Referer" && name != "User-Agent") || value.size() > 32768 ||
        std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
        std::find(seen.begin(), seen.end(), name) != seen.end()) return false;
    if (name == "Referer" && !httpOrigin(value)) return false;
    seen.push_back(name);
  }
  return true;
}
bool needsNotification(const Job &job) {
  return job.state != State::Cancelled &&
      (job.state != State::Complete || !job.libraryAcknowledged);
}
bool terminal(State state) { return state == State::Complete || state == State::Cancelled || state == State::Failed; }
const char *stateName(State state) {
  switch (state) {
  case State::Queued: return "Queued";
  case State::Transferring: return "Downloading";
  case State::Downloaded: return "Ready to import";
  case State::Importing: return "Importing";
  case State::NeedsAttention: return "Needs attention";
  case State::Complete: return "Complete";
  case State::Cancelled: return "Cancelled";
  case State::Failed: return "Failed";
  }
  return "Unknown";
}
JobStore::JobStore(std::filesystem::path root) : root_(std::move(root)) {
  if (!root_.is_absolute()) throw std::runtime_error("Download storage must be an absolute private path.");
  std::filesystem::create_directories(root_);
  if (std::filesystem::is_symlink(std::filesystem::symlink_status(root_)))
    throw std::runtime_error("Download storage cannot be a symlink.");
  std::filesystem::permissions(root_, std::filesystem::perms::owner_all);
  const auto manifest = root_ / "jobs.json";
  if (!std::filesystem::exists(manifest)) return;
  if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(manifest)) ||
      std::filesystem::file_size(manifest) > maxManifestBytes)
    throw std::runtime_error("Download manifest is invalid or too large.");
  std::ifstream input(manifest, std::ios::binary);
  const json document = json::parse(input);
  if (document.at("version") != 1 || !document.at("jobs").is_array() || document.at("jobs").size() > maxJobs)
    throw std::runtime_error("Unsupported download manifest.");
  for (const auto &record : document.at("jobs")) {
    auto job = readJob(record);
    if (std::any_of(jobs_.begin(), jobs_.end(), [&](const auto &other) { return job.id == other.id || job.order == other.order; }))
      throw std::runtime_error("Duplicate saved download identity.");
    jobs_.push_back(std::move(job));
  }
  std::sort(jobs_.begin(), jobs_.end(), [](const auto &a, const auto &b) { return a.order < b.order; });
  for (const auto &job : jobs_) {
    const auto old = jobPath(job.id) / (job.attempt == 0 ? "archive" : "archive." + std::to_string(job.attempt));
    const auto current = archivePath(job.id, job.attempt);
    std::error_code error;
    if (old == current || !std::filesystem::is_regular_file(std::filesystem::symlink_status(old, error))) continue;
    std::string message;
    if (std::filesystem::exists(current) || !atomic_file::renameDurably(old, current, message) ||
        !atomic_file::syncDirectory(current.parent_path(), message))
      throw std::runtime_error("Could not recover the retained archive name.");
  }
}
void JobStore::save(const std::vector<Job> &jobs) const {
  json records = json::array();
  for (const auto &j : jobs) records.push_back(jobJson(j));
  const auto text = json{{"version", 1}, {"jobs", records}}.dump();
  if (text.size() > maxManifestBytes) throw std::runtime_error("Download history is full; remove completed entries.");
  const auto operations = atomic_file::privateFileOperations();
  std::string error;
  if (!atomic_file::writeWithoutBackup(root_ / "jobs.json",
          std::as_bytes(std::span(text.data(), text.size())), error, &operations))
    throw std::runtime_error("Could not save download state: " + error);
}
Job JobStore::submit(Request request) {
  if (!validRequest(request)) throw std::runtime_error("Invalid download request.");
  std::lock_guard lock(mutex_);
  if (jobs_.size() >= maxJobs) throw std::runtime_error("Download history is full; remove completed entries.");
  Job job;
  do { job.id = newId(); } while (std::any_of(jobs_.begin(), jobs_.end(), [&](const auto &j) { return j.id == job.id; }));
  if (!jobs_.empty() && jobs_.back().order == std::numeric_limits<std::uint64_t>::max())
    throw std::runtime_error("Download sequence exhausted.");
  job.order = jobs_.empty() ? 1 : jobs_.back().order + 1;
  job.request = std::move(request);
  std::filesystem::create_directory(jobPath(job.id));
  std::filesystem::permissions(jobPath(job.id), std::filesystem::perms::owner_all);
  auto next = jobs_;
  next.push_back(job);
  save(next);
  jobs_.swap(next);
  return job;
}
std::vector<Job> JobStore::jobs() const { std::lock_guard lock(mutex_); return jobs_; }
std::optional<Job> JobStore::get(const std::string &id) const {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto &j) { return j.id == id; });
  return found == jobs_.end() ? std::nullopt : std::optional(*found);
}
bool JobStore::update(Job job) {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto &j) { return j.id == job.id; });
  if (found == jobs_.end() || found->revision != job.revision || !transition(*found, job) ||
      job.message.size() > 16384 || job.revision == std::numeric_limits<std::uint64_t>::max()) return false;
  ++job.revision;
  auto next = jobs_;
  next[found - jobs_.begin()] = std::move(job);
  save(next);
  jobs_.swap(next);
  return true;
}
bool JobStore::forget(const std::string &id) {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(jobs_.begin(), jobs_.end(), [&](const auto &j) { return j.id == id; });
  if (found == jobs_.end() || !terminal(found->state) ||
      (found->result && found->result->pendingArtifact) ||
      (found->state == State::Complete && !found->libraryAcknowledged)) return false;
  auto next = jobs_;
  next.erase(next.begin() + (found - jobs_.begin()));
  save(next);
  jobs_.swap(next);
  // The manifest is authoritative before discarding private remnants.
  std::error_code ignored;
  std::filesystem::remove_all(jobPath(id), ignored);
  return true;
}
std::filesystem::path JobStore::jobPath(const std::string &id) const {
  if (!validId(id)) throw std::runtime_error("Invalid download identity.");
  return root_ / id;
}
std::filesystem::path JobStore::archivePath(const std::string &id, std::uint64_t attempt) const {
  const auto root = jobPath(id);
  const auto job = get(id);
  if (!job) throw std::runtime_error("Download is no longer available.");
  return root / ((attempt == 0 ? "archive" : "archive." + std::to_string(attempt)) +
      archive_file::archiveExtensionFromName(job->request.archiveName));
}
} // namespace downloads
