#include "StorageOptimizer.h"
#include "AudioTranscoder.h"
#include "BgaTranscoder.h"
#include "../BmsChartFile.h"
#include "../Uuid.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace storage_management {
namespace fs = std::filesystem;

StorageConversionPermit::Lease::Lease(Lease &&other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
StorageConversionPermit::Lease::~Lease() {
  if (!owner_) return;
  { std::lock_guard lock(owner_->mutex_); owner_->busy_ = false; }
  owner_->changed_.notify_all();
}
std::optional<StorageConversionPermit::Lease> StorageConversionPermit::Acquire(Checkpoint check) {
  if (check && !check()) return {};
  std::unique_lock lock(mutex_);
  while (busy_) {
    lock.unlock();
    if (check && !check()) return {};
    lock.lock();
    if (!busy_) break;
    // Callers without an explicit wake signal still observe cancellation.
    changed_.wait_for(lock, std::chrono::milliseconds(50));
  }
  busy_ = true;
  return Lease(this);
}
void StorageConversionPermit::Notify() { changed_.notify_all(); }
std::shared_ptr<StorageConversionPermit> SharedStorageConversionPermit() {
  static auto permit = std::make_shared<StorageConversionPermit>(); return permit;
}
OptimizerDependencies DefaultOptimizerDependencies(StoragePlatformPaths paths) {
  return {.paths = std::move(paths), .permit = SharedStorageConversionPermit(), .probeCodecs = ProbeStorageCodecs,
          .encodeAudio = EncodeAudio, .encodeVideo = EncodeBga,
          .transactions = [](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
            FileTransactionDependencies dependencies; dependencies.beforeCommit = std::move(beforeCommit);
            auto transaction = std::make_shared<StorageFileTransaction>(paths, std::move(dependencies));
            return OptimizerTransactions{
                .replace = [transaction](const AssetCandidate &asset, const EncodeResult &encoded, Checkpoint check) {
                  return transaction->Replace(asset, encoded, std::move(check));
                }, .erase = [transaction](const AssetCandidate &asset, Checkpoint check) { return transaction->Delete(asset, std::move(check)); },
                .recover = [transaction](Checkpoint check) { return transaction->Recover(std::move(check)); }};
          },
          .availableSpace = [](const fs::path &path) -> std::optional<std::uint64_t> {
            std::error_code error; const auto space = fs::space(path, error);
            return error ? std::nullopt : std::optional<std::uint64_t>(space.available);
          }, .now = [] { return std::chrono::steady_clock::now(); }};
}

namespace {
bool sameRequest(const InventoryRequest &left, const InventoryRequest &right) {
  if (left.policy != right.policy || left.isolatedStaging != right.isolatedStaging || left.groups.size() != right.groups.size()) return false;
  for (std::size_t i = 0; i < left.groups.size(); ++i) {
    const auto &a = left.groups[i]; const auto &b = right.groups[i];
    if (a.groupId != b.groupId || a.path != b.path || a.writableLocal != b.writableLocal || a.chartPathFallback != b.chartPathFallback) return false;
  }
  return true;
}
void addRoot(std::vector<fs::path> &roots, const fs::path &root) {
  if (std::ranges::any_of(roots, [&](const auto &old) { return IsWithinFolder(root, old); })) return;
  std::erase_if(roots, [&](const auto &old) { return IsWithinFolder(old, root); });
  roots.push_back(root); std::ranges::sort(roots);
}
void append(StorageReport &report, FileResult result) {
  switch (result.outcome) {
  case FileOutcome::Converted: ++report.converted; break;
  case FileOutcome::Deleted: ++report.deleted; break;
  case FileOutcome::Skipped: ++report.skipped; break;
  case FileOutcome::Failed: ++report.failed; break;
  case FileOutcome::Cancelled: report.cancelled = true; break;
  }
  if (result.outcome != FileOutcome::Converted && result.outcome != FileOutcome::Deleted) {
    result.reclaimedBytes = 0; result.bytesAfter.reset();
  }
  report.reclaimedBytes += result.reclaimedBytes;
  report.recoveryRequired |= result.reason.code == StorageReason::RecoveryRequired;
  report.cleanupPending |= result.reason.code == StorageReason::CleanupFailed;
  if (report.files.size() < 100) report.files.push_back(std::move(result));
}
StorageDiagnostic missingCodec(AssetOperation operation, const StorageCodecCapabilities &caps) {
  const StorageCodecSupport *required = nullptr;
  switch (operation) {
  case AssetOperation::LosslessFlac: required = &caps.flac; break;
  case AssetOperation::LossyVorbis: required = &caps.vorbis; break;
  case AssetOperation::CompressH264: required = caps.x264.available ? &caps.mp4 : &caps.x264; break;
  case AssetOperation::Delete: break;
  }
  return required && !required->available ? StorageDiagnostic{StorageReason::CodecUnavailable,
      required->reason.empty() ? "Requested codec is unavailable in this build" : required->reason} : StorageDiagnostic{};
}
struct TemporaryDirectory {
  fs::path path;
  ~TemporaryDirectory() { if (!path.empty()) { std::error_code ignored; fs::remove_all(path, ignored); } }
};

// All files are checked once at admission. Between files, retain that evidence:
// inspect the next source, charts and directory timestamps, not a refreshed copy
// of every asset. Only a transaction's explicit path/identity changes advance it.
struct Evidence {
  StorageInventory inventory;
  std::map<fs::path, FileIdentity> charts;
  std::map<fs::path, std::set<fs::path>> children;
  explicit Evidence(StorageInventory value) : inventory(std::move(value)) {
    for (const auto &[path, identity] : inventory.files)
      if (asobmshow::bms_chart_file::isBmsChartPath(path)) charts.emplace(path, identity);
    for (const auto &path : inventory.entries) children[path.parent_path()].insert(path);
  }
  bool checkCharts(Checkpoint check, std::string &error) const {
    for (const auto &[path, identity] : charts) {
      if (check && !check()) { error = "Cancelled"; return false; }
      auto current = ReadFileIdentity(path, error, inventory.request.pathAccessRoots);
      if (!current || *current != identity) { error = "Chart changed since inventory"; return false; }
    }
    return true;
  }
  bool checkDirectories(Checkpoint check, std::string &error, const fs::path &except = {}) const {
    for (const auto &[path, modified] : inventory.directories) {
      if (path == except) continue;
      if (check && !check()) { error = "Cancelled"; return false; }
      if (!IsSafeLocalPath(path, error, inventory.request.pathAccessRoots)) return false;
      std::error_code ec;
      if (fs::last_write_time(path, ec) != modified || ec) { error = "Directory changed since inventory"; return false; }
    }
    return true;
  }
  bool candidate(const AssetCandidate &asset, Checkpoint check, std::string &error) const {
    if (!checkCharts(check, error) || !checkDirectories(check, error)) return false;
    const auto identity = ReadFileIdentity(asset.source, error, inventory.request.pathAccessRoots);
    if (!identity || *identity != asset.identity) { error = "Source changed since inventory"; return false; }
    return true;
  }
  bool checkEntries(const fs::path &parent, const std::set<fs::path> &expected, Checkpoint check,
                    std::string &error, fs::file_time_type &modified) const {
    std::error_code ec;
    modified = fs::last_write_time(parent, ec);
    if (ec) { error = ec.message(); return false; }
    std::set<fs::path> actual;
    fs::directory_iterator it(parent, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
      if (check && !check()) { error = "Cancelled"; return false; }
      actual.insert(it->path());
    }
    if (ec || actual != expected || fs::last_write_time(parent, ec) != modified || ec) {
      error = "Unexpected directory entry changed during transaction"; return false;
    }
    return true;
  }
  bool beforeCommit(const AssetCandidate &asset, const std::vector<StorageFileChange> &owned,
                     Checkpoint check, std::string &error) const {
    const auto parent = asset.source.parent_path();
    if (!IsSafeLocalPath(parent, error, inventory.request.pathAccessRoots) || !checkCharts(check, error) || !checkDirectories(check, error, parent)) return false;
    const auto found = children.find(parent);
    auto expected = found == children.end() ? std::set<fs::path>{} : found->second;
    bool sourceChangedByTransaction = false;
    for (const auto &change : owned) {
      if (change.path.parent_path() != parent) { error = "Unexpected transaction path"; return false; }
      sourceChangedByTransaction |= change.path == asset.source;
      if (change.after) {
        const auto identity = ReadFileIdentity(change.path, error, inventory.request.pathAccessRoots);
        if (!identity || *identity != *change.after) { error = "Transaction file identity changed before commit"; return false; }
        expected.insert(change.path);
      } else {
        std::error_code ec; const auto status = fs::symlink_status(change.path, ec);
        if (fs::exists(status) || (ec && ec != std::errc::no_such_file_or_directory)) {
          error = "Foreign file appeared at a transaction path"; return false;
        }
        expected.erase(change.path);
      }
    }
    if (!sourceChangedByTransaction) {
      const auto identity = ReadFileIdentity(asset.source, error, inventory.request.pathAccessRoots);
      if (!identity || *identity != asset.identity) { error = "Source changed before commit"; return false; }
    }
    fs::file_time_type ignored;
    return checkEntries(parent, expected, check, error, ignored);
  }
  bool advance(const AssetCandidate &asset, const FileResult &result, Checkpoint check, std::string &error) {
    const auto parent = asset.source.parent_path();
    if (!IsSafeLocalPath(parent, error, inventory.request.pathAccessRoots)) return false;
    for (const auto &change : result.filesystemChanges) {
      if (change.path.parent_path() != parent) { error = "Transaction reported an unrelated path"; return false; }
      if (change.after) {
        const auto identity = ReadFileIdentity(change.path, error, inventory.request.pathAccessRoots);
        if (!identity || *identity != *change.after) { error = "Installed file or recovery backup changed"; return false; }
        inventory.files[change.path] = *change.after;
        inventory.entries.insert(change.path); children[parent].insert(change.path);
      } else {
        std::error_code ec; const auto status = fs::symlink_status(change.path, ec);
        if (fs::exists(status) || (ec && ec != std::errc::no_such_file_or_directory)) {
          error = "A file appeared at a retired source path"; return false;
        }
        inventory.files.erase(change.path); inventory.entries.erase(change.path); children[parent].erase(change.path);
      }
    }
    if (!checkCharts(check, error) || !checkDirectories(check, error, parent)) return false;
    fs::file_time_type modified;
    if (!checkEntries(parent, children[parent], check, error, modified)) return false;
    inventory.directories[parent] = modified;
    return true;
  }
};
}

StorageReport StorageOptimizer::Run(const OptimizationRequest &request, Checkpoint check, ProgressCallback progress) {
  StorageReport report;
  if (!IsValid(request.policy)) {
    append(report, {.outcome = FileOutcome::Skipped, .reason = {StorageReason::UnsupportedFormat, "Invalid storage policy"}});
    return report;
  }
  if (IsNoOp(request.policy)) return report;
  bool cancelled = false;
  Checkpoint checkpoint = [&] { if (!cancelled && check && !check()) cancelled = true; return !cancelled; };
  StorageProgress state;
  bool notified = false;
  std::chrono::steady_clock::time_point lastNotification;
  auto emit = [&](bool force = false) {
    if (!progress) return;
    const auto now = dependencies_.now();
    if (force || !notified || now - lastNotification >= std::chrono::milliseconds(100)) {
      notified = true; lastNotification = now; state.reclaimedBytes = report.reclaimedBytes; progress(state);
    }
  };
  auto complete = [&] { report.cancelled |= cancelled; state.phase = StoragePhase::Complete; emit(true); return report; };
  try {
    auto lease = dependencies_.permit->Acquire(checkpoint);
    if (!lease) { report.cancelled = true; return complete(); }
    StorageCommitPrecondition validateBeforeCommit;
    auto transactions = dependencies_.transactions(dependencies_.paths,
        [&](const AssetCandidate &asset, const std::vector<StorageFileChange> &owned, Checkpoint check, std::string &error) {
          return validateBeforeCommit && validateBeforeCommit(asset, owned, std::move(check), error);
        });
    const auto recovery = transactions.recover(checkpoint);
    report = recovery;
    if (report.files.size() > 100) report.files.resize(100);
    if (report.recoveryRequired || report.cancelled) return complete();
    InventoryRequest inventoryRequest = request.inventory; inventoryRequest.policy = request.policy;
    inventoryRequest.pathAccessRoots = dependencies_.paths.pathAccessRoots;
    emit();
    if (request.preflight && !sameRequest(request.preflight->request, inventoryRequest)) {
      append(report, {.reason = {StorageReason::StaleInput, "Selected folders or policy changed after preflight"}}); return complete();
    }
    auto inventory = request.preflight ? *request.preflight : BuildInventory(inventoryRequest, checkpoint);
    inventory.request.pathAccessRoots = inventoryRequest.pathAccessRoots;
    if (inventory.cancelled || !checkpoint()) return complete();
    std::string error;
    if (!RevalidateInventory(inventory, checkpoint, error)) {
      if (!cancelled) append(report, {.reason = {StorageReason::StaleInput, error}});
      return complete();
    }
    for (const auto &group : inventory.groups) {
      if (!group.totalBytes) append(report, {.source = group.root, .reason = group.reason});
      if (group.totalBytes && (IsWithinFolder(dependencies_.paths.temporaryRoot, group.root) ||
                               IsWithinFolder(dependencies_.paths.journalRoot, group.root))) {
        append(report, {.source = group.root, .outcome = FileOutcome::Failed,
                        .reason = {StorageReason::UnsafePath, "Private transaction storage cannot be inside selected folders"}});
        return complete();
      }
    }
    for (auto skipped : inventory.skips) {
      if (auto found = inventory.files.find(skipped.source); found != inventory.files.end()) skipped.bytesBefore = found->second.bytes;
      append(report, std::move(skipped));
    }
    state.filesTotal = inventory.candidates.size();
    if (inventory.candidates.empty()) return complete();
    std::error_code ec;
    fs::create_directories(dependencies_.paths.temporaryRoot, ec);
    if (ec || !IsSafeLocalPath(dependencies_.paths.temporaryRoot, error, dependencies_.paths.pathAccessRoots)) {
      append(report, {.outcome = FileOutcome::Failed, .reason = {StorageReason::IoError, ec ? ec.message() : error}}); return complete();
    }
    TemporaryDirectory temporary;
    const auto jobPath = dependencies_.paths.temporaryRoot / uuid::generateV4();
    if (!fs::create_directory(jobPath, ec) || ec) {
      append(report, {.outcome = FileOutcome::Failed, .reason = {StorageReason::IoError, "Cannot create private storage job"}}); return complete();
    }
    temporary.path = jobPath;
    fs::permissions(jobPath, fs::perms::owner_all, fs::perm_options::replace, ec);
    if (ec) { append(report, {.outcome = FileOutcome::Failed, .reason = {StorageReason::IoError, ec.message()}}); return complete(); }
    const auto caps = dependencies_.probeCodecs();
    Evidence evidence(std::move(inventory));
    validateBeforeCommit = [&](const AssetCandidate &asset, const std::vector<StorageFileChange> &owned, Checkpoint check, std::string &error) {
      return evidence.beforeCommit(asset, owned, std::move(check), error);
    };
    bool staleScope = false;
    for (auto asset : evidence.inventory.candidates) {
      if (!checkpoint() || report.recoveryRequired) break;
      state.currentPath = asset.source;
      FileResult result{.source = asset.source, .destination = asset.destination, .bytesBefore = asset.identity.bytes};
      bool transactionStarted = false;
      bool transactionFinished = false;
      const auto output = jobPath / ("candidate" + asset.destination.extension().string());
      try {
        if (staleScope || !evidence.candidate(asset, checkpoint, error)) {
          result.reason = {StorageReason::StaleInput, error};
        } else if (const auto missing = missingCodec(asset.operation, caps); missing.code != StorageReason::None) {
          result.reason = missing;
        } else {
          bool canEncode = true;
          if (asset.operation != AssetOperation::Delete) {
            const auto available = dependencies_.availableSpace(jobPath);
            constexpr std::uint64_t reserve = 1024 * 1024;
            if (!available || asset.identity.bytes > std::numeric_limits<std::uint64_t>::max() - reserve || *available < asset.identity.bytes + reserve) {
              canEncode = false; result.reason = {StorageReason::InsufficientSpace, "Insufficient private space before encoding"};
            }
          }
          if (canEncode) {
            const auto hash = HashStorageFile(asset.source, checkpoint, error);
            if (!hash) { result.outcome = cancelled ? FileOutcome::Cancelled : FileOutcome::Failed; result.reason = {cancelled ? StorageReason::Cancelled : StorageReason::IoError, error}; }
            else {
              asset.sourceHash = *hash;
              EncodeResult encoded;
              if (asset.operation != AssetOperation::Delete) {
                state.phase = StoragePhase::Encode; emit();
                auto encoderProgress = [&](const StorageProgress &value) { state.phase = value.phase; emit(); };
                const auto &encoder = asset.operation == AssetOperation::CompressH264 ? dependencies_.encodeVideo : dependencies_.encodeAudio;
                encoded = encoder(asset, request.policy, output, checkpoint, encoderProgress);
                result.outcome = encoded.outcome; result.reason = encoded.reason;
                if (encoded.outcome == FileOutcome::Converted && encoded.outputPath != output) {
                  encoded.outcome = result.outcome = FileOutcome::Failed;
                  result.reason = {StorageReason::ValidationFailed, "Encoder returned an output outside this job"};
                }
              }
              if (checkpoint() && (asset.operation == AssetOperation::Delete || encoded.outcome == FileOutcome::Converted)) {
                if (!evidence.candidate(asset, checkpoint, error)) {
                  result.outcome = FileOutcome::Skipped; result.reason = {StorageReason::StaleInput, error};
                } else {
                  state.phase = StoragePhase::Commit; emit(); transactionStarted = true;
                  result = asset.operation == AssetOperation::Delete ? transactions.erase(asset, checkpoint) : transactions.replace(asset, encoded, checkpoint);
                  transactionFinished = true;
                }
              }
            }
          }
        }
      } catch (const std::exception &exception) {
        result.outcome = FileOutcome::Failed;
        result.reason = {transactionStarted && !transactionFinished ? StorageReason::RecoveryRequired : StorageReason::IoError, exception.what()};
      }
      if (cancelled && !transactionFinished) { result.outcome = FileOutcome::Cancelled; result.reason = {StorageReason::Cancelled, "Cancelled before commit"}; }
      if (result.outcome == FileOutcome::Converted || result.outcome == FileOutcome::Deleted)
        for (const auto &group : evidence.inventory.groups)
          if (group.totalBytes && IsWithinFolder(asset.source, group.root)) addRoot(report.changedRoots, group.root);
      const bool recover = result.reason.code == StorageReason::RecoveryRequired;
      if (transactionFinished && !recover && !cancelled && !evidence.advance(asset, result, checkpoint, error)) staleScope = true;
      append(report, std::move(result));
      fs::remove(output, ec); ec.clear();
      ++state.filesDone; emit();
      if (report.cancelled || recover) break;
    }
  } catch (const std::exception &exception) {
    append(report, {.outcome = FileOutcome::Failed, .reason = {StorageReason::IoError, exception.what()}});
  }
  return complete();
}
} // namespace storage_management
