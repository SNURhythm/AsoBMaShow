#include "StorageOptimizer.h"
#include "StorageReportDetail.h"
#include "AudioTranscoder.h"
#include "BgaTranscoder.h"
#include "StoragePathIndex.h"
#include "StorageArchiveTransaction.h"
#include "../Uuid.h"

#include <algorithm>
#include <atomic>
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
          .validateExisting = [](const AssetCandidate &asset, Checkpoint check) {
            return asset.operation == AssetOperation::CompressH264 ? ValidateExistingBga(asset.destination, std::move(check)) :
                ValidateExistingAudio(asset.destination, asset.operation == AssetOperation::LosslessFlac ?
                    AudioAction::LosslessFlac : AudioAction::LossyVorbis, std::move(check));
          },
          .transactions = [](const StoragePlatformPaths &paths, StorageCommitPrecondition beforeCommit) {
            FileTransactionDependencies dependencies; dependencies.beforeCommit = std::move(beforeCommit);
            auto transaction = std::make_shared<StorageFileTransaction>(paths, std::move(dependencies));
            return OptimizerTransactions{
                .replace = [transaction](const AssetCandidate &asset, const EncodeResult &encoded, Checkpoint check) {
                  return transaction->Replace(asset, encoded, std::move(check));
                }, .erase = [transaction](const AssetCandidate &asset, Checkpoint check) { return transaction->Delete(asset, std::move(check)); },
                .reuse = [transaction](const AssetCandidate &asset, Checkpoint check) { return transaction->ReuseExisting(asset, std::move(check)); },
                .recover = [transaction](Checkpoint check, const StorageRecoveryObserver &observer) {
                  return transaction->Recover(std::move(check), observer);
                }};
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
    if (a.groupId != b.groupId || a.path != b.path || a.writableLocal != b.writableLocal || a.chartPathFallback != b.chartPathFallback || a.sourceKind != b.sourceKind) return false;
  }
  return true;
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
    result.reclaimedBytes = result.addedBytes = 0; result.bytesAfter.reset();
  }
  AccumulateStorageBytes(report.reclaimedBytes, report.addedBytes, result.reclaimedBytes, result.addedBytes);
  report.recoveryRequired |= result.reason.code == StorageReason::RecoveryRequired;
  report.cleanupPending |= result.reason.code == StorageReason::CleanupFailed;
  RetainStorageReportDetail(report, std::move(result));
}
void mergeReport(StorageReport &report, StorageReport other) {
  report.converted += other.converted; report.deleted += other.deleted;
  report.skipped += other.skipped; report.failed += other.failed;
  AccumulateStorageBytes(report.reclaimedBytes, report.addedBytes, other.reclaimedBytes, other.addedBytes);
  report.cancelled |= other.cancelled; report.recoveryRequired |= other.recoveryRequired; report.cleanupPending |= other.cleanupPending;
  for (auto &file : other.files) RetainStorageReportDetail(report, std::move(file));
  report.changedRoots.insert(report.changedRoots.end(), other.changedRoots.begin(), other.changedRoots.end());
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


}

StorageReport StorageOptimizer::Run(OptimizationRequest request, Checkpoint check, ProgressCallback progress) {
  StorageReport report;
  if (!IsValid(request.policy)) {
    append(report, {.outcome = FileOutcome::Skipped, .reason = {StorageReason::UnsupportedFormat, "Invalid storage policy"}});
    return report;
  }
  if (IsNoOp(request.policy)) return report;
  std::atomic<bool> cancelled{false};
  Checkpoint checkpoint = [&] { if (!cancelled && check && !check()) cancelled = true; return !cancelled; };
  StorageProgress state;
  state.phase = StoragePhase::Prepare;
  if (request.preflight) state.filesTotal = request.preflight->eligibleCount;
  bool notified = false;
  StoragePhase lastPhase = state.phase;
  std::chrono::steady_clock::time_point lastNotification;
  auto emit = [&](bool force = false) {
    if (!progress) return;
    const auto now = dependencies_.now();
    const bool preparing = state.phase == StoragePhase::Prepare || state.phase == StoragePhase::ArchivePrepare;
    if (force || !notified || (preparing && state.phase != lastPhase) || now - lastNotification >= std::chrono::milliseconds(100)) {
      notified = true; lastPhase = state.phase; lastNotification = now; progress(state);
    }
  };
  auto complete = [&] { report.cancelled |= cancelled; state.reclaimedBytes = report.reclaimedBytes; state.addedBytes = report.addedBytes; state.phase = StoragePhase::Complete; emit(true); return report; };
  bool recoveryActive = false;
  try {
    emit(true);
    auto lease = dependencies_.permit->Acquire(checkpoint);
    if (!lease) { report.cancelled = true; return complete(); }
    recoveryActive = true;
    report = RecoverStorageArchives(dependencies_.paths, checkpoint);
    recoveryActive = false;
    if (report.recoveryRequired || report.cancelled) return complete();
    auto transactions = dependencies_.transactions(dependencies_.paths, {});
    std::unique_ptr<StorageDirectoryEvidence> recoveryEvidence;
    StorageDirectorySummary *recoverySummary = nullptr;
    StorageRecoveryObserver observer{
      .before = [&](const fs::path &directory) {
        recoveryEvidence.reset(); recoverySummary = nullptr;
        if (!request.preflight) return;
        const auto found = request.preflight->directories.find(directory);
        if (found == request.preflight->directories.end()) return;
        recoverySummary = &found->second;
        recoveryEvidence = std::make_unique<StorageDirectoryEvidence>(*recoverySummary, *request.preflight,
            StorageDirectoryLimits{.scratchRoot = dependencies_.paths.scanRoot});
        std::string error;
        if (!recoveryEvidence->Load(checkpoint, error)) {
          recoverySummary->reason = {StorageReason::StaleInput, error};
          recoveryEvidence.reset();
        }
      },
      .after = [&](const fs::path &, const std::vector<StorageFileChange> &changes) {
        if (!recoveryEvidence) return;
        std::string error;
        // Keep unmodified reviewed entries exactly as loaded before recovery;
        // never bless unrelated changes by taking a new filesystem snapshot.
        const bool cleanupOnly = std::ranges::all_of(changes, [](const auto &change) {
          const auto extension = change.path.extension();
          return !change.after && (extension == ".backup" || extension == ".install");
        });
        // A rollback can restore media or remove a collision, changing which
        // files are eligible. Those candidates need a new explicit review.
        if (!cleanupOnly) recoverySummary->reason = {StorageReason::StaleInput, "Recovered media changed after preflight"};
        else if (!changes.empty() && (!recoveryEvidence->AdvanceRecovery(changes, checkpoint, error) ||
                                !recoveryEvidence->Finish(checkpoint, error)))
          recoverySummary->reason = {StorageReason::StaleInput, error};
        recoveryEvidence.reset();
      },
    };
    recoveryActive = true;
    const auto recovery = transactions.recover(checkpoint, observer);
    mergeReport(report, recovery);
    recoveryActive = false;
    if (report.files.size() > 100) report.files.resize(100);
    if (report.recoveryRequired || report.cancelled) return complete();
    InventoryRequest inventoryRequest = request.inventory; inventoryRequest.policy = request.policy;
    inventoryRequest.pathAccessRoots = dependencies_.paths.pathAccessRoots;
    emit();
    if (request.preflight && !sameRequest(request.preflight->request, inventoryRequest)) {
      append(report, {.reason = {StorageReason::StaleInput, "Selected folders or policy changed after preflight"}}); return complete();
    }
    StorageScanDependencies scan;
    scan.limits.scratchRoot = dependencies_.paths.scanRoot;
    if (!request.preflight) { state.phase = StoragePhase::Inventory; emit(true); }
    auto inventory = request.preflight ? std::move(*request.preflight) : BuildStoragePreflight(inventoryRequest, checkpoint, nullptr, scan);
    inventory.request.pathAccessRoots = inventoryRequest.pathAccessRoots;
    if (inventory.cancelled || !checkpoint()) return complete();
    if (!inventory.complete) {
      append(report, {.outcome = FileOutcome::Failed,
                      .reason = {StorageReason::IncompleteInventory, "Preflight was interrupted; review the selected folders again"}});
      return complete();
    }
    std::string error;
    for (const auto &group : inventory.groups) {
      if (!group.totalBytes) append(report, {.source = group.root, .reason = group.reason});
      if (group.totalBytes && (IsWithinFolder(dependencies_.paths.temporaryRoot, group.root) ||
                               IsWithinFolder(dependencies_.paths.journalRoot, group.root))) {
        append(report, {.source = group.root, .outcome = FileOutcome::Failed,
                        .reason = {StorageReason::UnsafePath, "Private transaction storage cannot be inside selected folders"}});
        return complete();
      }
    }
    report.skipped += inventory.skippedCount;
    for (auto &skipped : inventory.skips) RetainStorageReportDetail(report, std::move(skipped));
    state.filesTotal = inventory.eligibleCount;
    if (!inventory.eligibleCount) return complete();
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
    std::vector<fs::path> allowedRoots = dependencies_.paths.allowedRoots;
    const StoragePathIndex authority(allowedRoots), access(inventoryRequest.pathAccessRoots);
    std::vector<GroupStorageMeasurement> baselines = std::move(request.measurementBaselines);
    std::set<std::string> baselineIds;
    for (const auto &row : baselines) baselineIds.insert(row.groupId);
    for (const auto &row : inventory.groups) if (!baselineIds.contains(row.groupId)) baselines.push_back(row);
    std::vector<fs::path> groupRoots;
    for (const auto &row : baselines) groupRoots.push_back(row.root);
    const StoragePathIndex groups(groupRoots);
    std::set<std::size_t> selectedAncestors;
    for (const auto &row : inventory.groups)
      for (const auto position : groups.Ancestors(row.root)) selectedAncestors.insert(position);
    std::map<std::size_t, StorageDirectoryStamp> cachedAncestorStamps;
    std::map<fs::path, StorageScanTotals> totals;
    for (auto it = inventory.directories.rbegin(); it != inventory.directories.rend(); ++it) {
      auto &sum = totals[it->first];
      const auto &own = it->second.totals;
      sum.bytes += own.bytes; sum.audio += own.audio; sum.video += own.video; sum.image += own.image;
      if (inventory.directories.contains(it->first.parent_path())) {
        auto &parent = totals[it->first.parent_path()];
        parent.bytes += sum.bytes; parent.audio += sum.audio; parent.video += sum.video; parent.image += sum.image;
      }
    }
    std::vector<bool> baselineValid;
    for (const auto &row : baselines) {
      const auto found = inventory.directories.find(row.root);
      const auto measured = totals.find(row.root);
      baselineValid.push_back(found != inventory.directories.end() && measured != totals.end() &&
          found->second.reason.code == StorageReason::None && row.totalBytes == measured->second.bytes &&
          row.audioBytes == measured->second.audio && row.videoBytes == measured->second.video &&
          row.imageBytes == measured->second.image && row.sourceIdentity == StorageDirectorySourceIdentity(found->second.stamp));
      const auto position = baselineValid.size() - 1;
      if (found == inventory.directories.end() && selectedAncestors.contains(position) &&
          row.sourceKind == StorageSourceKind::Local && row.totalBytes && !row.sourceIdentity.empty()) {
        const auto matches = access.Ancestors(row.root);
        const auto boundary = matches.empty() ? fs::path{} : inventoryRequest.pathAccessRoots[matches.back()];
        StorageDirectoryStamp stamp;
        std::string stampError;
        // This preserves a cached display estimate, not recursive filesystem
        // freshness or mutation authority. Only exact committed deltas change it.
        if (ReadStorageDirectoryStamp(row.root, boundary, stamp, stampError) &&
            row.sourceIdentity == StorageDirectorySourceIdentity(stamp)) {
          baselineValid.back() = true;
          cachedAncestorStamps.emplace(position, stamp);
        }
      }
    }
    // A selected baseline known to be stale also invalidates its cached rollups.
    for (std::size_t i = 0; i < baselines.size(); ++i) if (!baselineValid[i] && inventory.directories.contains(baselines[i].root))
      for (const auto ancestor : groups.Ancestors(baselines[i].root))
        if (cachedAncestorStamps.contains(ancestor)) baselineValid[ancestor] = false;
    // Recovery advances mutation evidence, but these display totals still
    // include its old backup/install files. Do not apply incomplete deltas.
    for (const auto &changed : recovery.changedRoots)
      for (const auto position : groups.Ancestors(changed)) baselineValid[position] = false;
    totals.clear();
    std::map<std::size_t, GroupStorageDelta> deltas;
    std::vector<fs::path> changedDirectories;
    std::set<std::size_t> uncertainGroups;
    std::mutex aggregation;
    auto work = [&](const StorageDirectorySummary &selected, StorageWorkBudget &budget, Checkpoint checkpoint, ProgressCallback progress) {
      // The scheduler gives each mapped summary one owner and publishes parent
      // completion before descendants can read that parent's updated evidence.
      const auto directoryPath = selected.directory;
      auto &summary = inventory.directories.at(directoryPath);
      StorageReport report;
      StorageProgress state;
      std::string error;
      std::error_code ec;
      StorageEvidenceMetrics metrics;
      bool transactionUnresolved = false;
      auto emit = [&] { state.reclaimedBytes = report.reclaimedBytes; state.addedBytes = report.addedBytes; if (progress) progress(state); };
      auto finishDirectory = [&] {
        if (dependencies_.evidenceMetrics) {
          std::lock_guard lock(aggregation);
          dependencies_.evidenceMetrics->directoryChecks += metrics.directoryChecks;
          dependencies_.evidenceMetrics->chartChecks += metrics.chartChecks;
          dependencies_.evidenceMetrics->membershipEnumerations += metrics.membershipEnumerations;
          dependencies_.evidenceMetrics->membershipEntries += metrics.membershipEntries;
        }
        emit(); return report;
      };
      try {
        StorageDirectoryEvidence evidence(summary, inventory, {.scratchRoot = dependencies_.paths.scanRoot}, &metrics);
        auto skipDirectory = [&] {
          report.skipped += summary.eligibleCount;
          RetainStorageReportDetail(report, {.source = directoryPath,
              .reason = {StorageReason::StaleInput, error}});
          state.filesDone += summary.eligibleCount;
          // A failed baseline cannot authorize descendants later in the job.
          summary.reason = {StorageReason::StaleInput, error};
          std::lock_guard lock(aggregation);
          changedDirectories.push_back(directoryPath);
          for (const auto position : groups.Ancestors(directoryPath)) uncertainGroups.insert(position);
        };
        if (!evidence.Load(checkpoint, error)) { if (checkpoint()) skipDirectory(); return finishDirectory(); }
        const auto selectedAuthority = authority.Ancestors(directoryPath);
        if (selectedAuthority.empty()) { error = "Directory is outside authorized roots"; skipDirectory(); return finishDirectory(); }
        StoragePlatformPaths localPaths{.journalRoot = dependencies_.paths.journalRoot,
            .temporaryRoot = dependencies_.paths.temporaryRoot,
            .allowedRoots = {allowedRoots[selectedAuthority.back()]}, .scanRoot = dependencies_.paths.scanRoot,
            .journalSyncRoot = dependencies_.paths.journalSyncRoot};
        // Transaction private paths and source have separate sandbox boundaries.
        for (const auto &path : {directoryPath, localPaths.temporaryRoot, localPaths.journalRoot}) {
          const auto matches = access.Ancestors(path);
          if (!matches.empty()) localPaths.pathAccessRoots.push_back(inventoryRequest.pathAccessRoots[matches.back()]);
        }
        auto localTransactions = dependencies_.transactions(localPaths,
            [&](const AssetCandidate &asset, const std::vector<StorageFileChange> &owned, Checkpoint check, std::string &error) {
              return evidence.BeforeCommit(asset, owned, std::move(check), error);
            });
        const auto affectedGroups = groups.Ancestors(directoryPath);
        GroupStorageDelta directoryDelta;
        bool changedDirectory = false, staleScope = false;
        const auto workerId = uuid::generateV4();
        fs::path cursor;
        while (auto entry = evidence.Candidates().Next(cursor, error)) {
          cursor = entry->name;
          StorageDiagnostic candidateReason;
          auto next = StorageCandidateForEntry(*entry, evidence.Directory(), request.policy, {}, candidateReason);
          if (!next) { ++report.skipped; ++state.filesDone; continue; }
          auto asset = std::move(*next);
          if (!checkpoint() || report.recoveryRequired) break;
          state.currentPath = asset.source;
          FileResult result{.source = asset.source, .destination = asset.destination, .bytesBefore = asset.identity.bytes};
          bool transactionStarted = false;
          bool transactionFinished = false;
          const auto output = jobPath / ("candidate-" + workerId + asset.destination.extension().string());
          std::optional<StorageWorkBudget::Lease> reservation;
          try {
            if (staleScope || !evidence.Candidate(asset, checkpoint, error)) {
              result.reason = {StorageReason::StaleInput, error};
            } else if (asset.reuseDestination) {
              state.phase = StoragePhase::Verify; emit();
              const auto beforeHash = HashStorageFile(asset.destination, checkpoint, error);
              if (!beforeHash) result.reason = {StorageReason::IoError, error};
              else if (!dependencies_.validateExisting || !localTransactions.reuse)
                result.reason = {StorageReason::CodecUnavailable, "Existing destination validation is unavailable"};
              else {
                result.reason = dependencies_.validateExisting(asset, checkpoint);
                if (result.reason.code == StorageReason::ValidationFailed) result.outcome = FileOutcome::Failed;
                if (result.reason.code == StorageReason::None) {
                  const auto afterHash = HashStorageFile(asset.destination, checkpoint, error);
                  if (!afterHash || *afterHash != *beforeHash || !evidence.Candidate(asset, checkpoint, error))
                    result.reason = {StorageReason::StaleInput, "Existing destination changed during validation"};
                  else if (checkpoint()) {
                    asset.existingDestinationHash = *afterHash;
                    state.phase = StoragePhase::Commit; emit(); transactionStarted = true;
                    transactionUnresolved = true;
                    result = localTransactions.reuse(asset, checkpoint);
                    transactionFinished = true;
                    transactionUnresolved = result.reason.code == StorageReason::RecoveryRequired;
                  }
                }
              }
            } else if (const auto missing = missingCodec(asset.operation, caps); missing.code != StorageReason::None) {
              result.reason = missing;
            } else {
              bool canEncode = true;
              if (asset.operation != AssetOperation::Delete) {
                constexpr std::uint64_t reserve = 1024 * 1024;
                if (asset.identity.bytes <= std::numeric_limits<std::uint64_t>::max() - reserve) {
                  auto acquired = budget.Acquire(asset.kind, asset.identity.bytes + reserve, checkpoint);
                  if (acquired) reservation.emplace(std::move(*acquired));
                }
                if (!reservation) {
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
                    if (!evidence.Candidate(asset, checkpoint, error)) {
                      result.outcome = FileOutcome::Skipped; result.reason = {StorageReason::StaleInput, error};
                    } else {
                      state.phase = StoragePhase::Commit; emit(); transactionStarted = true;
                      transactionUnresolved = true;
                      result = asset.operation == AssetOperation::Delete ? localTransactions.erase(asset, checkpoint) : localTransactions.replace(asset, encoded, checkpoint);
                      transactionFinished = true;
                      transactionUnresolved = result.reason.code == StorageReason::RecoveryRequired;
                    }
                  }
                }
              }
            }
          } catch (const std::exception &exception) {
            result.outcome = FileOutcome::Failed;
            result.reason = {transactionStarted && !transactionFinished ? StorageReason::RecoveryRequired : StorageReason::IoError, exception.what()};
          }
          if (cancelled && !transactionStarted) { result.outcome = FileOutcome::Cancelled; result.reason = {StorageReason::Cancelled, "Cancelled before commit"}; }
          if (result.outcome == FileOutcome::Converted || result.outcome == FileOutcome::Deleted) {
            changedDirectory = true;
            // Exact physical changes include retained backups, independently of the detail cap.
            for (const auto &change : result.filesystemChanges) {
              auto before = evidence.Directory().Find(change.path.filename(), error);
              const auto oldBytes = before ? before->bytes : 0;
              const auto newBytes = change.after ? change.after->bytes : 0;
              directoryDelta.bytesBefore += oldBytes; directoryDelta.bytesAfter += newBytes;
              const auto kind = asobmshow::chart_assets::ClassifyMediaPath(change.path);
              using asobmshow::chart_assets::MediaKind;
              if (kind == MediaKind::Audio) { directoryDelta.audioBefore += oldBytes; directoryDelta.audioAfter += newBytes; }
              if (kind == MediaKind::Video) { directoryDelta.videoBefore += oldBytes; directoryDelta.videoAfter += newBytes; }
              if (kind == MediaKind::Image) { directoryDelta.imageBefore += oldBytes; directoryDelta.imageAfter += newBytes; }
            }
          }
          if (result.reason.code == StorageReason::StaleInput) staleScope = true;
          const bool recover = result.reason.code == StorageReason::RecoveryRequired;
          if (recover) budget.StopForRecovery();
          if (transactionFinished && !recover && !cancelled && !evidence.Advance(asset, result, checkpoint, error)) staleScope = true;
          append(report, std::move(result));
          fs::remove(output, ec); ec.clear();
          reservation.reset();
          ++state.filesDone; emit();
          if (report.cancelled || recover) break;
        }
        const bool uncertain = !error.empty() || staleScope || !checkpoint() || report.recoveryRequired || !evidence.Finish(checkpoint, error);
        std::lock_guard lock(aggregation);
        if (uncertain) {
          summary.reason = {StorageReason::StaleInput, error}; directoryDelta.needsRefresh = true;
          changedDirectories.push_back(directoryPath);
          for (const auto position : affectedGroups) uncertainGroups.insert(position);
        }
        if (changedDirectory) {
          changedDirectories.push_back(directoryPath);
          for (const auto position : affectedGroups) {
            auto &delta = deltas[position];
            const auto &baseline = baselines[position];
            delta.groupId = baseline.groupId; delta.baselineBytes = baseline.totalBytes;
            delta.sourceIdentity = baseline.sourceIdentity; delta.baselineGeneration = baseline.generation;
            delta.bytesBefore += directoryDelta.bytesBefore; delta.bytesAfter += directoryDelta.bytesAfter;
            delta.audioBefore += directoryDelta.audioBefore; delta.audioAfter += directoryDelta.audioAfter;
            delta.videoBefore += directoryDelta.videoBefore; delta.videoAfter += directoryDelta.videoAfter;
            delta.imageBefore += directoryDelta.imageBefore; delta.imageAfter += directoryDelta.imageAfter;
            delta.needsRefresh |= directoryDelta.needsRefresh || !baselineValid[position];
          }
        }
      } catch (const std::exception &exception) {
        const bool needsRecovery = transactionUnresolved || report.recoveryRequired;
        if (needsRecovery) budget.StopForRecovery();
        append(report, {.source = directoryPath, .outcome = FileOutcome::Failed,
            .reason = {needsRecovery ? StorageReason::RecoveryRequired : StorageReason::IoError, exception.what()}});
        summary.reason = {StorageReason::StaleInput, exception.what()};
        std::lock_guard lock(aggregation);
        changedDirectories.push_back(directoryPath);
        for (const auto position : groups.Ancestors(directoryPath)) uncertainGroups.insert(position);
      }
      return finishDirectory();
    };
    const auto recoveryBytes = report.reclaimedBytes, recoveryAdded = report.addedBytes;
    auto worked = RunStorageDirectories(inventory, dependencies_.executionLimits,
        [&] { return dependencies_.availableSpace(jobPath); }, work, checkpoint,
        [&](const StorageProgress &value) { state = value; AccumulateStorageBytes(state.reclaimedBytes, state.addedBytes, recoveryBytes, recoveryAdded); emit(); });
    report.converted += worked.converted; report.deleted += worked.deleted;
    report.skipped += worked.skipped; report.failed += worked.failed;
    AccumulateStorageBytes(report.reclaimedBytes, report.addedBytes, worked.reclaimedBytes, worked.addedBytes);
    report.cancelled |= worked.cancelled; report.recoveryRequired |= worked.recoveryRequired;
    report.cleanupPending |= worked.cleanupPending;
    for (auto &file : worked.files) RetainStorageReportDetail(report, std::move(file));
    for (const auto &archive : inventory.archives) {
      if (!archive.eligibleCount || report.recoveryRequired || !checkpoint()) continue;
      const auto beforeFiles = state.filesDone, beforeBytes = report.reclaimedBytes, beforeAdded = report.addedBytes;
      StorageArchiveDependencies archiveDependencies{
        .encode = [&](const AssetCandidate &asset, const StoragePolicy &policy, const fs::path &output, Checkpoint check, ProgressCallback progress) {
          const auto missing = missingCodec(asset.operation, caps);
          if (missing.code != StorageReason::None) return EncodeResult{.reason = missing};
          const auto &encoder = asset.operation == AssetOperation::CompressH264 ? dependencies_.encodeVideo : dependencies_.encodeAudio;
          return encoder(asset, policy, output, std::move(check), std::move(progress));
        }, .validateExisting = dependencies_.validateExisting, .availableSpace = dependencies_.availableSpace};
      mergeReport(report, UpdateStorageArchive(archive, request.policy, dependencies_.paths, archiveDependencies, checkpoint,
          [&](const StorageProgress &value) {
            state = value; state.filesDone += beforeFiles; state.filesTotal = inventory.eligibleCount;
            AccumulateStorageBytes(state.reclaimedBytes, state.addedBytes, beforeBytes, beforeAdded); emit();
          }));
    }
    // The changed subtree union is computed once, not once per asset/group pair.
    report.changedRoots.insert(report.changedRoots.end(), changedDirectories.begin(), changedDirectories.end());
    report.changedRoots = StoragePathIndex(std::move(report.changedRoots)).MinimalRoots();
    const StoragePathIndex changedIndex(report.changedRoots);
    for (std::size_t position = 0; position < baselines.size(); ++position) {
      if (deltas.contains(position) || !baselineValid[position] || changedIndex.Ancestors(baselines[position].root).empty()) continue;
      const auto &row = baselines[position];
      deltas[position] = {.groupId = row.groupId, .baselineBytes = row.totalBytes,
          .sourceIdentity = row.sourceIdentity, .baselineGeneration = row.generation};
    }
    for (auto &[position, delta] : deltas) {
      delta.needsRefresh |= uncertainGroups.contains(position);
      const auto found = inventory.directories.find(baselines[position].root);
      if (found == inventory.directories.end() && cachedAncestorStamps.contains(position)) {
        const auto &row = baselines[position];
        const auto matches = access.Ancestors(row.root);
        const auto boundary = matches.empty() ? fs::path{} : inventoryRequest.pathAccessRoots[matches.back()];
        StorageDirectoryStamp stamp;
        std::string stampError;
        if (!ReadStorageDirectoryStamp(row.root, boundary, stamp, stampError) || stamp != cachedAncestorStamps.at(position))
          delta.needsRefresh = true;
        else delta.nextSourceIdentity = row.sourceIdentity;
      } else if (found == inventory.directories.end() || found->second.reason.code != StorageReason::None) delta.needsRefresh = true;
      else delta.nextSourceIdentity = StorageDirectorySourceIdentity(found->second.stamp);
      report.groupDeltas.push_back(std::move(delta));
    }

  } catch (const std::exception &exception) {
    append(report, {.outcome = FileOutcome::Failed,
        .reason = {recoveryActive ? StorageReason::RecoveryRequired : StorageReason::IoError, exception.what()}});
  }
  return complete();
}
} // namespace storage_management
