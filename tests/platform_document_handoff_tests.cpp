#include "../src/PlatformDocumentHandoff.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace {
int failures = 0;

using namespace std::chrono_literals;

void expect(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

template <typename Predicate>
bool waitUntil(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

struct BlockingWork {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool released = false;
  std::atomic_bool finished = false;

  void waitForRelease() {
    std::unique_lock lock(mutex);
    entered = true;
    condition.notify_all();
    condition.wait(lock, [this] { return released; });
    finished = true;
  }

  bool waitUntilEntered() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, 2s, [this] { return entered; });
  }

  void release() {
    std::lock_guard lock(mutex);
    released = true;
    condition.notify_all();
  }
};

struct LifetimeProbe {
  explicit LifetimeProbe(std::atomic_bool &destroyed) : destroyed(destroyed) {}
  ~LifetimeProbe() { destroyed = true; }

  std::atomic_bool &destroyed;
};

std::filesystem::path makePrivateTestDocument(const std::string &testName,
                                              const std::string &contents) {
  (void)testName;
  static std::atomic_uint64_t sequence = 1;
  const std::string suffix =
      std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
  const std::string directoryName =
      std::string(32 - suffix.size(), '0') + suffix;
  const auto directory = std::filesystem::temp_directory_path() / "AsoBMaShow" /
                         "document-handoff" / directoryName;
  std::error_code error;
  std::filesystem::remove_all(directory, error);
  std::filesystem::create_directories(directory, error);
  for (const auto &privateDirectory : {directory.parent_path().parent_path(),
                                       directory.parent_path(), directory}) {
    std::string securityError;
    platform_document_handoff::detail::SecurePrivateDocumentPath(
        privateDirectory, true, securityError);
  }
  const auto path = directory / "imported-document.zip";
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << contents;
  output.close();
  std::string securityError;
  platform_document_handoff::detail::SecurePrivateDocumentPath(path, false,
                                                               securityError);
  return path;
}

void testBridgeCancellationIsDistinct() {
  const PlatformDocumentHandoffResult result =
      platform_document_handoff::detail::ParseBridgeResult("__CANCELLED__",
                                                           true, true);
  expect(result.status == PlatformDocumentHandoffStatus::Cancelled,
         "bridge cancellation has a dedicated status");
  expect(result.cancelled() && !result.ok() && result.message.empty() &&
             result.localPath.empty(),
         "bridge cancellation carries neither an error nor a local path");
}

void testBridgeErrorPreservesMessage() {
  const PlatformDocumentHandoffResult result =
      platform_document_handoff::detail::ParseBridgeResult(
          "__ERROR__:provider unavailable", true, true);
  expect(result.status == PlatformDocumentHandoffStatus::Failed &&
             result.message == "provider unavailable" && !result.cancelled(),
         "bridge error preserves its diagnostic");
}

void testBridgeImportRequiresAbsoluteLocalPath() {
  const auto imported =
      makePrivateTestDocument("bridge-import", "private import");
  const auto valid = platform_document_handoff::detail::ParseBridgeResult(
      imported.string(), true, true);
  expect(valid.ok() && valid.localPath.is_absolute() &&
             valid.temporaryLocalFile && valid.temporaryOwnership != nullptr,
         "bridge import returns an owned absolute private path");

  const auto relative = platform_document_handoff::detail::ParseBridgeResult(
      "relative/import.document", true, true);
  expect(relative.status == PlatformDocumentHandoffStatus::Failed &&
             relative.localPath.empty(),
         "bridge import rejects a relative local path");

  auto cleanup = valid;
  platform_document_handoff::CleanupTemporaryDocument(cleanup);
}

void testDefaultBridgeNormalizesPickedSourceNamesAndRejectsInvalidUtf8() {
  const auto decomposed = makePrivateTestDocument("decomposed-source", "copy");
  const auto normalized = platform_document_handoff::detail::ParseBridgeResult(
      decomposed.string(), true, true, PlatformTemporaryPathKind::File,
      std::string("Cafe\xCC\x81"));
  expect(normalized.ok() && normalized.originalSourceName == "Caf\xC3\xA9",
         "default bridge normalization converts a decomposed picked filename to NFC");
  auto normalizedCleanup = normalized;
  (void)platform_document_handoff::CleanupTemporaryDocument(normalizedCleanup);

  const auto invalidFile = makePrivateTestDocument("invalid-source-file", "copy");
  const auto invalidFileRoot = invalidFile.parent_path();
  const auto invalidFileResult =
      platform_document_handoff::detail::ParseBridgeResult(
          invalidFile.string(), true, true, PlatformTemporaryPathKind::File,
          std::string("\xFF", 1));
  expect(!invalidFileResult.ok() && !std::filesystem::exists(invalidFile),
         "invalid UTF-8 rejects and cleans an issued temporary file");
  std::error_code error;
  std::filesystem::remove_all(invalidFileRoot, error);

  std::string allocationError;
  const auto invalidDirectory =
      platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
          std::filesystem::temp_directory_path(), allocationError);
  std::ofstream(invalidDirectory / "skin.lua", std::ios::binary) << "skin";
  const auto invalidDirectoryResult =
      platform_document_handoff::detail::ParseBridgeResult(
          invalidDirectory.string(), true, true,
          PlatformTemporaryPathKind::Directory, std::string("\xFF", 1));
  expect(!invalidDirectoryResult.ok() &&
             !std::filesystem::exists(invalidDirectory),
         "invalid UTF-8 rejects and cleans an issued temporary directory");
  std::filesystem::remove_all(invalidDirectory, error);
}

void testBridgeExportSuccessHasNoLocalPath() {
  const auto result = platform_document_handoff::detail::ParseBridgeResult(
      "__OK__", false, false);
  expect(result.ok() && result.localPath.empty() && !result.temporaryLocalFile,
         "bridge export success does not invent a destination path");
}

void testImportRequestRequiresMimeTypeAndExplicitLimit() {
  PlatformDocumentImportRequest request;
  request.mimeType = "";
  request.maxBytes = 1024;
  auto result = platform_document_handoff::detail::Validate(request);
  expect(result.status == PlatformDocumentHandoffStatus::Failed &&
             result.message.find("MIME") != std::string::npos,
         "import rejects an empty MIME type");

  request.mimeType = "application/zip";
  request.maxBytes = 0;
  result = platform_document_handoff::detail::Validate(request);
  expect(result.status == PlatformDocumentHandoffStatus::Failed &&
             result.message.find("maximum") != std::string::npos,
         "import requires an explicit non-zero byte limit");
}

void testExportRequestRequiresLeafNameAndBoundedRegularSource() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      "asobmashow-platform-document-handoff-tests";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  const std::filesystem::path source = root / "profile.zip";
  {
    std::ofstream stream(source, std::ios::binary | std::ios::trunc);
    stream << "12345";
  }

  PlatformDocumentExportRequest request;
  request.localPath = source;
  request.mimeType = "application/zip";
  request.suggestedName = "nested/profile.zip";
  request.maxBytes = 5;
  auto result = platform_document_handoff::detail::Validate(request);
  expect(result.status == PlatformDocumentHandoffStatus::Failed &&
             result.message.find("file name") != std::string::npos,
         "export rejects a suggested name containing path components");

  request.suggestedName = "profile.zip";
  request.maxBytes = 4;
  result = platform_document_handoff::detail::Validate(request);
  expect(result.status == PlatformDocumentHandoffStatus::Failed &&
             result.message.find("maximum") != std::string::npos,
         "export rejects a source that exceeds its explicit byte limit");

  request.maxBytes = 5;
  result = platform_document_handoff::detail::Validate(request);
  expect(result.ok(), "export accepts a bounded regular source");
  std::filesystem::remove_all(root, error);
}

void testBoundedStreamCopyStopsAtLimit() {
  {
    std::istringstream input("12345");
    std::ostringstream output;
    std::uint64_t copied = 0;
    std::string message;
    const bool ok = platform_document_handoff::detail::CopyStreamBounded(
        input, output, 5, copied, message);
    expect(ok && copied == 5 && output.str() == "12345" && message.empty(),
           "bounded stream copy accepts a payload exactly at the limit");
  }

  {
    std::istringstream input("123456");
    std::ostringstream output;
    std::uint64_t copied = 0;
    std::string message;
    const bool ok = platform_document_handoff::detail::CopyStreamBounded(
        input, output, 5, copied, message);
    expect(!ok && copied <= 5 && message.find("maximum") != std::string::npos,
           "bounded stream copy detects overflow without reporting success");
  }
}

void testFailedDesktopExportPreservesExistingDestination() {
  const auto root = std::filesystem::temp_directory_path() /
                    "asobmashow-platform-safe-export-test";
  const auto source = root / "source.zip";
  const auto destination = root / "destination.zip";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  {
    std::ofstream output(source, std::ios::binary);
    output << "123456";
  }
  {
    std::ofstream output(destination, std::ios::binary);
    output << "existing";
  }

  std::string message;
  expect(!platform_document_handoff::detail::CopyFileForExport(
             source, destination, 5, message) &&
             message.find("maximum") != std::string::npos,
         "bounded export fails before replacing its destination");
  std::ifstream preserved(destination, std::ios::binary);
  std::stringstream contents;
  contents << preserved.rdbuf();
  expect(contents.str() == "existing",
         "failed export preserves the user's existing destination");

  std::atomic_bool cancelled = true;
  message.clear();
  expect(!platform_document_handoff::detail::CopyFileForExport(
             source, destination, 6, message, &cancelled) &&
             message.find("cancelled") != std::string::npos,
         "desktop export copy observes lifecycle cancellation");
  std::ifstream cancellationPreserved(destination, std::ios::binary);
  contents.str("");
  contents.clear();
  contents << cancellationPreserved.rdbuf();
  expect(contents.str() == "existing",
         "cancelled export leaves an existing destination intact");

  cancelled = false;
  message.clear();
  expect(!platform_document_handoff::detail::CopyFileForExport(
             source, destination, 6, message, &cancelled, {},
             [&cancelled](std::uint64_t copied) {
               if (copied > 0) {
                 cancelled = true;
               }
             }) &&
             message.find("cancelled") != std::string::npos,
         "desktop export observes cancellation that arrives during copying");
  std::ifstream midCopyPreserved(destination, std::ios::binary);
  contents.str("");
  contents.clear();
  contents << midCopyPreserved.rdbuf();
  expect(contents.str() == "existing",
         "mid-copy cancellation cannot publish over the destination");

  message.clear();
  expect(platform_document_handoff::detail::CopyFileForExport(
             source, destination, 6, message) &&
             message.empty(),
         "successful bounded export atomically replaces its destination");
  std::ifstream replaced(destination, std::ios::binary);
  contents.str("");
  contents.clear();
  contents << replaced.rdbuf();
  expect(contents.str() == "123456",
         "successful export publishes the complete source");

  std::filesystem::remove_all(root, error);
}

void testDesktopExportCreatesItsStagingFileExclusively() {
  const auto root = std::filesystem::temp_directory_path() /
                    "asobmashow-platform-exclusive-export-test";
  const auto source = root / "source.zip";
  const auto destination = root / "destination.zip";
  const auto occupiedCandidate = root / ".occupied.tmp";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  {
    std::ofstream output(source, std::ios::binary);
    output << "new-profile";
  }
  {
    std::ofstream output(destination, std::ios::binary);
    output << "old-profile";
  }
  {
    std::ofstream output(occupiedCandidate, std::ios::binary);
    output << "attacker-sentinel";
  }

  std::string message;
  expect(
      platform_document_handoff::detail::CopyFileForExport(
          source, destination, 11, message, nullptr, {}, {}, occupiedCandidate),
      "an occupied forced staging name is skipped without failing export");
  std::ifstream occupied(occupiedCandidate, std::ios::binary);
  std::stringstream occupiedContents;
  occupiedContents << occupied.rdbuf();
  expect(occupiedContents.str() == "attacker-sentinel",
         "exclusive staging never truncates a pre-existing file");
  std::ifstream exported(destination, std::ios::binary);
  std::stringstream exportedContents;
  exportedContents << exported.rdbuf();
  expect(exportedContents.str() == "new-profile",
         "export retries with a fresh exclusive staging name");

  const auto outsideFile = root / "outside-keep.zip";
  const auto linkedCandidate = root / ".linked.tmp";
  const auto linkedDestination = root / "linked-destination.zip";
  {
    std::ofstream output(outsideFile, std::ios::binary);
    output << "outside-keep";
  }
  std::filesystem::create_symlink(outsideFile, linkedCandidate, error);
  if (!error) {
    message.clear();
    expect(platform_document_handoff::detail::CopyFileForExport(
               source, linkedDestination, 11, message, nullptr, {}, {},
               linkedCandidate),
           "a symlink occupying the forced staging name is skipped");
    std::ifstream outside(outsideFile, std::ios::binary);
    std::stringstream outsideContents;
    outsideContents << outside.rdbuf();
    expect(outsideContents.str() == "outside-keep" &&
               std::filesystem::is_symlink(
                   std::filesystem::symlink_status(linkedCandidate)),
           "exclusive no-follow staging preserves a symlink target and link");
  }

  std::filesystem::remove_all(root, error);
}

void testAsyncHandoffValidatesBeforeOpeningPicker() {
  PlatformDocumentImportRequest request;
  request.mimeType = "";
  request.maxBytes = 1024;
  auto operation = platform_document_handoff::ImportDocumentAsync(request);
  expect(waitUntil([&] { return operation.ready(); }) && operation.poll(),
         "invalid request completes without starting a native picker");
  const auto result = operation.takeResult();
  expect(result.has_value() &&
             result->status == PlatformDocumentHandoffStatus::Failed &&
             result->message.find("MIME") != std::string::npos,
         "async handoff validates before opening a native picker");
}

void testCancelPendingCompletesPromptlyAndExactlyOnce() {
  auto work = std::make_shared<BlockingWork>();
  std::atomic_int cancellationCalls = 0;
  auto operation = platform_document_handoff::detail::StartOperation(
      [work](const std::atomic_bool &) {
        work->waitForRelease();
        return PlatformDocumentHandoffResult{
            .status = PlatformDocumentHandoffStatus::Failed,
            .message = "late failure"};
      },
      [&cancellationCalls] { ++cancellationCalls; });
  expect(work->waitUntilEntered(), "test operation starts its native work");

  operation.cancel();
  operation.cancel();
  expect(operation.ready() && operation.poll(),
         "cancelling a pending operation makes it immediately ready");
  const auto result = operation.takeResult();
  expect(result.has_value() && result->cancelled(),
         "cancelling a pending operation produces cancellation");
  expect(!operation.takeResult().has_value(),
         "an operation result can only be taken once");
  expect(cancellationCalls == 1,
         "native cancellation is requested exactly once");

  work->release();
  expect(waitUntil([&] { return work->finished.load(); }),
         "late native work exits after cancellation");
}

void testNativeCancellationRegistrationClosesActivationGap() {
  std::atomic_int cancellationCalls = 0;
  platform_document_handoff::detail::NativeCancellationRegistration
      cancelledBeforeActivation([&cancellationCalls] { ++cancellationCalls; });
  cancelledBeforeActivation.cancel();
  expect(
      !cancelledBeforeActivation.activate() && cancellationCalls == 0,
      "cancel between the final check and native registration prevents launch");

  platform_document_handoff::detail::NativeCancellationRegistration
      cancelledAfterActivation([&cancellationCalls] { ++cancellationCalls; });
  expect(cancelledAfterActivation.activate(),
         "uncancelled native registration becomes active");
  cancelledAfterActivation.cancel();
  cancelledAfterActivation.cancel();
  cancelledAfterActivation.deactivate();
  expect(cancellationCalls == 1,
         "active native cancellation is delivered exactly once");
}

void testCancelledWaiterLeavesSerializedNativeGatePromptly() {
  std::timed_mutex gate;
  gate.lock();
  std::atomic_bool cancelled = false;
  std::atomic_bool waiterFinished = false;
  std::atomic_bool acquired = true;
  std::thread waiter([&] {
    acquired =
        platform_document_handoff::detail::LockInterruptibly(gate, cancelled);
    if (acquired) {
      gate.unlock();
    }
    waiterFinished = true;
  });
  std::this_thread::sleep_for(25ms);
  cancelled = true;
  expect(waitUntil([&] { return waiterFinished.load(); }) && !acquired,
         "a cancelled second handoff exits while the first still owns the "
         "native gate");
  gate.unlock();
  waiter.join();
}

void testOperationTokensNeverWrapOrCancelANewerOperation() {
  std::atomic_uint64_t nearExhaustion =
      std::numeric_limits<std::uint64_t>::max() - 1;
  expect(platform_document_handoff::detail::NextOperationToken(
             nearExhaustion) == std::numeric_limits<std::uint64_t>::max() - 1 &&
             platform_document_handoff::detail::NextOperationToken(
                 nearExhaustion) == std::numeric_limits<std::uint64_t>::max(),
         "the last two never-reused operation tokens are issued once");
  bool exhausted = false;
  try {
    (void)platform_document_handoff::detail::NextOperationToken(nearExhaustion);
  } catch (const std::overflow_error &) {
    exhausted = true;
  }
  expect(exhausted && nearExhaustion == 0,
         "token exhaustion is permanent instead of wrapping to a reused token");

  struct FakeTokenNative {
    std::mutex mutex;
    std::condition_variable condition;
    std::uint64_t activeToken = 11;
    bool oldCancelEntered = false;
    bool releaseOldCancel = false;
    int newPickerCancellationCount = 0;
  } native;
  platform_document_handoff::detail::NativeCancellationRegistration old(
      [&native] {
        std::unique_lock lock(native.mutex);
        native.oldCancelEntered = true;
        native.condition.notify_all();
        native.condition.wait(lock,
                              [&native] { return native.releaseOldCancel; });
        if (native.activeToken == 11) {
          ++native.newPickerCancellationCount;
        }
      });
  expect(old.activate(), "old token becomes active for stale-cancel test");
  std::thread delayedCancel([&old] { old.cancel(); });
  {
    std::unique_lock lock(native.mutex);
    native.condition.wait_for(lock, 2s,
                              [&native] { return native.oldCancelEntered; });
    old.deactivate();
    native.activeToken = 12;
    native.releaseOldCancel = true;
    native.condition.notify_all();
  }
  delayedCancel.join();
  expect(native.newPickerCancellationCount == 0,
         "a delayed old-token cancellation is ignored by a newer picker");
}

void testCommitDecisionKeepsTeardownNonblocking() {
  auto action = std::make_shared<BlockingWork>();
  std::atomic_int committedCancellationCalls = 0;
  auto operation = platform_document_handoff::detail::StartOperationWithCommit(
      [action](const std::atomic_bool &,
               const platform_document_handoff::detail::CommitGate &commit) {
        const bool committed = commit([action] {
          action->waitForRelease();
          return true;
        });
        return PlatformDocumentHandoffResult{
            .status = committed ? PlatformDocumentHandoffStatus::Succeeded
                                : PlatformDocumentHandoffStatus::Failed};
      },
      [&committedCancellationCalls] { ++committedCancellationCalls; });
  expect(action->waitUntilEntered(),
         "commit action reaches blocking test hook");
  auto started = std::chrono::steady_clock::now();
  operation.cancel();
  expect(std::chrono::steady_clock::now() - started < 250ms,
         "cancel does not wait for an in-progress publish action");
  started = std::chrono::steady_clock::now();
  operation.close();
  expect(std::chrono::steady_clock::now() - started < 250ms,
         "destruction does not wait for an in-progress publish action");
  expect(committedCancellationCalls == 0,
         "closing after commit does not cancel the committed native export");
  action->release();
  expect(waitUntil([&] { return action->finished.load(); }),
         "detached publish action exits after handle teardown");

  auto beforeCommit = std::make_shared<BlockingWork>();
  std::atomic_int commitActions = 0;
  std::atomic_bool preCommitWorkerFinished = false;
  auto cancelled = platform_document_handoff::detail::StartOperationWithCommit(
      [beforeCommit, &commitActions, &preCommitWorkerFinished](
          const std::atomic_bool &,
          const platform_document_handoff::detail::CommitGate &commit) {
        beforeCommit->waitForRelease();
        const bool committed = commit([&commitActions] {
          ++commitActions;
          return true;
        });
        PlatformDocumentHandoffResult result{
            .status = committed ? PlatformDocumentHandoffStatus::Succeeded
                                : PlatformDocumentHandoffStatus::Failed};
        preCommitWorkerFinished = true;
        return result;
      },
      [] {});
  expect(beforeCommit->waitUntilEntered(),
         "pre-commit cancellation test reaches the decision gap");
  cancelled.cancel();
  beforeCommit->release();
  expect(waitUntil([&] { return preCommitWorkerFinished.load(); }) &&
             commitActions == 0,
         "cancel before commit prevents the publish action from running");
}

void testCancelledIrreversibleNativeCommitKeepsProviderContentUntouched() {
  auto beforeNativeAcknowledgement = std::make_shared<BlockingWork>();
  std::atomic_bool providerCommitted = false;
  std::atomic_bool acknowledgementAccepted = true;
  std::atomic_bool workerFinished = false;
  auto operation = platform_document_handoff::detail::StartOperationWithCommit(
      [beforeNativeAcknowledgement, &providerCommitted,
       &acknowledgementAccepted, &workerFinished](
          const std::atomic_bool &,
          const platform_document_handoff::detail::CommitGate &commit) {
        beforeNativeAcknowledgement->waitForRelease();
        providerCommitted = true;
        const bool accepted = commit([] { return true; });
        acknowledgementAccepted = accepted;
        PlatformDocumentHandoffResult result{
            .status = accepted ? PlatformDocumentHandoffStatus::Succeeded
                               : PlatformDocumentHandoffStatus::Cancelled};
        workerFinished = true;
        return result;
      },
      [] {});
  expect(beforeNativeAcknowledgement->waitUntilEntered(),
         "irreversible native commit test reaches the callback gap");
  operation.cancel();
  const auto cancelled = operation.takeResult();
  expect(cancelled.has_value() && cancelled->cancelled(),
         "lifecycle cancellation can win before native commit acknowledgement");
  beforeNativeAcknowledgement->release();
  expect(waitUntil([&] { return workerFinished.load(); }) &&
             providerCommitted && !acknowledgementAccepted,
         "a rejected late native acknowledgement never deletes provider-owned "
         "content");
}

void testDestroyPendingDoesNotBlockOrUseDestroyedHandle() {
  auto work = std::make_shared<BlockingWork>();
  std::atomic_int cancellationCalls = 0;
  const auto started = std::chrono::steady_clock::now();
  {
    auto operation = platform_document_handoff::detail::StartOperation(
        [work](const std::atomic_bool &) {
          work->waitForRelease();
          return PlatformDocumentHandoffResult{
              .status = PlatformDocumentHandoffStatus::Succeeded};
        },
        [&cancellationCalls] { ++cancellationCalls; });
    expect(work->waitUntilEntered(),
           "pending operation starts before teardown");
  }
  const auto teardownTime = std::chrono::steady_clock::now() - started;
  expect(teardownTime < 250ms,
         "destroying a pending operation does not wait for native UI");
  expect(cancellationCalls == 1,
         "destroying a pending operation requests native cancellation");

  work->release();
  expect(waitUntil([&] { return work->finished.load(); }),
         "detached native completion remains safe after handle destruction");
}

void testExportSourceLifetimeOutlivesNonblockingTeardown() {
  const auto runCase = [](bool closeOperation) {
    auto work = std::make_shared<BlockingWork>();
    std::atomic_bool lifetimeDestroyed = false;
    PlatformDocumentExportRequest request;
    request.sourceLifetime = std::make_shared<LifetimeProbe>(lifetimeDestroyed);

    auto operation =
        platform_document_handoff::detail::StartOperationWithCommit(
            [work](const std::atomic_bool &,
                   const platform_document_handoff::detail::CommitGate &) {
              work->waitForRelease();
              return PlatformDocumentHandoffResult{
                  .status = PlatformDocumentHandoffStatus::Cancelled};
            },
            [] {}, std::move(request.sourceLifetime));

    expect(work->waitUntilEntered(),
           "export worker starts before lifetime teardown is tested");
    if (closeOperation) {
      operation.close();
      expect(!operation,
             "close releases the export operation handle immediately");
    } else {
      operation.cancel();
      expect(operation.ready(),
             "cancel publishes a terminal result before worker exit");
    }
    expect(!lifetimeDestroyed.load(std::memory_order_acquire),
           "nonblocking teardown retains the export source owner");

    work->release();
    expect(waitUntil([&] {
             return lifetimeDestroyed.load(std::memory_order_acquire);
           }),
           "export source owner releases only after detached worker exit");
  };

  runCase(false);
  runCase(true);
}

void testLateImportCompletionIsIgnoredAndCleaned() {
  const auto temporaryImport =
      makePrivateTestDocument("late-import", "temporary");
  const auto root = temporaryImport.parent_path();
  std::error_code error;

  auto work = std::make_shared<BlockingWork>();
  auto operation = platform_document_handoff::detail::StartOperation(
      [work, temporaryImport](const std::atomic_bool &) {
        work->waitForRelease();
        return platform_document_handoff::detail::ParseBridgeResult(
            temporaryImport.string(), true, true);
      },
      [] {});
  expect(work->waitUntilEntered(), "late import test reaches native work");
  operation.cancel();
  const auto cancelled = operation.takeResult();
  expect(cancelled.has_value() && cancelled->cancelled(),
         "cancelled result wins over a late native completion");
  work->release();
  expect(waitUntil([&] { return !std::filesystem::exists(temporaryImport); }),
         "late temporary import is removed instead of being leaked");
  std::filesystem::remove_all(root, error);
}

void testUnconsumedTemporaryImportIsCleanedOnClose() {
  const auto temporaryImport =
      makePrivateTestDocument("unconsumed-import", "temporary");
  const auto root = temporaryImport.parent_path();
  std::error_code error;

  auto operation = platform_document_handoff::detail::StartOperation(
      [temporaryImport](const std::atomic_bool &) {
        return platform_document_handoff::detail::ParseBridgeResult(
            temporaryImport.string(), true, true);
      },
      [] {});
  expect(waitUntil([&] { return operation.ready(); }),
         "temporary import operation completes");
  operation.close();
  expect(waitUntil([&] { return !std::filesystem::exists(temporaryImport); }),
         "closing without taking a temporary import cleans it up");

  std::filesystem::remove_all(root, error);
}

void testTakenTemporaryImportHasExplicitCleanup() {
  const auto temporaryImport =
      makePrivateTestDocument("taken-import", "temporary");
  const auto root = temporaryImport.parent_path();
  std::error_code error;

  auto result = platform_document_handoff::detail::ParseBridgeResult(
      temporaryImport.string(), true, true);
  expect(result.ok(), "private bridge result carries cleanup ownership");
  expect(platform_document_handoff::CleanupTemporaryDocument(result),
         "taken temporary import exposes deterministic cleanup");
  expect(!std::filesystem::exists(temporaryImport) &&
             result.localPath.empty() && !result.temporaryLocalFile,
         "explicit cleanup consumes ownership and removes the file");
  expect(platform_document_handoff::CleanupTemporaryDocument(result),
         "temporary cleanup is idempotent");

  std::filesystem::remove_all(root, error);
}

void testDirectoryCleanupRequiresItsExactIssuedRoot() {
  std::string errorMessage;
  const auto root =
      platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
          std::filesystem::temp_directory_path(), errorMessage);
  expect(!root.empty() && errorMessage.empty(),
         "directory import test allocates a private issued root");
  const auto child = root / "chart";
  std::ofstream(child, std::ios::binary) << "chart";

  auto result = platform_document_handoff::detail::ParseBridgeResult(
      root.string(), true, true, PlatformTemporaryPathKind::Directory);
  expect(result.ok() && !result.temporaryLocalFile &&
             result.temporaryPathKind == PlatformTemporaryPathKind::Directory,
         "directory result carries an owned directory cleanup capability");
  expect(!platform_document_handoff::CleanupTemporaryDocument(result) &&
             std::filesystem::exists(result.localPath),
         "legacy file cleanup refuses an owned directory");

  const auto issuedRoot = result.localPath;
  result.localPath = child;
  expect(!platform_document_handoff::CleanupTemporaryPath(result) &&
             std::filesystem::exists(issuedRoot),
         "directory cleanup refuses a child of its issued root");
  result.localPath = issuedRoot.parent_path();
  expect(!platform_document_handoff::CleanupTemporaryPath(result) &&
             std::filesystem::exists(issuedRoot),
         "directory cleanup refuses a parent of its issued root");
  result.localPath = std::filesystem::temp_directory_path();
  expect(!platform_document_handoff::CleanupTemporaryPath(result) &&
             std::filesystem::exists(issuedRoot),
         "directory cleanup refuses a substituted path");
  result.localPath = issuedRoot;
  expect(platform_document_handoff::CleanupTemporaryPath(result) &&
             !std::filesystem::exists(issuedRoot),
         "directory cleanup removes exactly its issued root");
  expect(platform_document_handoff::CleanupTemporaryPath(result),
         "directory cleanup is idempotent");
}

void testDirectoryCleanupRejectsIdentitySwapAndUnlinksFinalSymlink() {
  std::string allocationError;
  const auto root =
      platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
          std::filesystem::temp_directory_path(), allocationError);
  auto result = platform_document_handoff::detail::ParseBridgeResult(
      root.string(), true, true, PlatformTemporaryPathKind::Directory);
  expect(result.ok(), "issued directory receives an exact cleanup capability");

  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directory(root, error);
  std::string securityError;
  (void)platform_document_handoff::detail::SecurePrivateDocumentPath(
      root, true, securityError);
  const auto replacementMarker = root / "replacement-marker";
  std::ofstream(replacementMarker, std::ios::binary) << "keep";
  expect(!platform_document_handoff::CleanupTemporaryPath(result) &&
             std::filesystem::exists(replacementMarker),
         "directory cleanup refuses a post-adoption identity swap");
  std::filesystem::remove_all(root, error);

  const auto symlinkRoot =
      platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
          std::filesystem::temp_directory_path(), allocationError);
  auto symlinkResult = platform_document_handoff::detail::ParseBridgeResult(
      symlinkRoot.string(), true, true, PlatformTemporaryPathKind::Directory);
  const auto outside = std::filesystem::temp_directory_path() /
                       "asobmashow-directory-cleanup-outside";
  std::filesystem::remove_all(outside, error);
  std::filesystem::create_directories(outside, error);
  const auto outsideMarker = outside / "keep";
  std::ofstream(outsideMarker, std::ios::binary) << "keep";
  std::filesystem::remove_all(symlinkRoot, error);
  std::filesystem::create_directory_symlink(outside, symlinkRoot, error);
  if (!error) {
    expect(platform_document_handoff::CleanupTemporaryPath(symlinkResult) &&
               !std::filesystem::exists(symlinkRoot) &&
               std::filesystem::exists(outsideMarker),
           "directory cleanup unlinks a final symlink without traversing it");
  }
  std::filesystem::remove_all(outside, error);
}

void testDirectoryImportUsesThePickedFolderAsItsPrivateRoot() {
  const auto sourceRoot = std::filesystem::temp_directory_path() /
                          "asobmashow-directory-import-source" / "Sample Skin";
  const auto temporaryRoot = std::filesystem::temp_directory_path();
  std::error_code error;
  std::filesystem::remove_all(sourceRoot.parent_path(), error);
  std::filesystem::create_directories(sourceRoot / "nested", error);
  {
    std::ofstream(sourceRoot / "skin.lua", std::ios::binary) << "skin";
    std::ofstream(sourceRoot / "nested" / "readme.txt", std::ios::binary)
        << "nested";
  }

  const PlatformDirectoryImportRequest request{
      .maxBytes = 1024,
      .maxFiles = 3,
      .maxRegularFileBytes = 1024,
      .maxDepth = 2,
      .maxPathBytes = 128};
  const auto imported =
      platform_document_handoff::detail::CopyDirectoryForImport(
          sourceRoot, request, temporaryRoot);
  expect(imported.ok() &&
             imported.temporaryPathKind == PlatformTemporaryPathKind::Directory &&
             imported.originalSourceName == "Sample Skin",
         "directory import retains the selected folder name and cleanup root");
  expect(std::filesystem::is_regular_file(imported.localPath / "skin.lua") &&
             std::filesystem::is_regular_file(imported.localPath / "nested" /
                                              "readme.txt") &&
             !std::filesystem::exists(imported.localPath / "Sample Skin"),
         "directory import copies the picked folder contents directly at its root");

  auto cleanup = imported;
  expect(platform_document_handoff::CleanupTemporaryPath(cleanup),
         "directory import result exposes exact-root cleanup");

  PlatformDirectoryImportRequest invalid = request;
  invalid.maxFiles = 0;
  expect(!platform_document_handoff::detail::Validate(invalid).ok(),
         "directory import requires a non-zero file limit");
  invalid = request;
  invalid.maxDepth = 0;
  expect(!platform_document_handoff::detail::Validate(invalid).ok(),
         "directory import requires a non-zero depth limit");
  invalid = request;
  invalid.maxPathBytes = 0;
  expect(!platform_document_handoff::detail::Validate(invalid).ok(),
         "directory import requires a non-zero path limit");

  invalid = request;
  invalid.maxFiles = 2;
  expect(!platform_document_handoff::detail::CopyDirectoryForImport(
              sourceRoot, invalid, temporaryRoot)
              .ok(),
         "directory import counts directories toward the selected folder entry "
         "limit");
  invalid = request;
  invalid.maxBytes = 3;
  expect(!platform_document_handoff::detail::CopyDirectoryForImport(
              sourceRoot, invalid, temporaryRoot)
              .ok(),
         "directory import enforces the selected folder byte limit");
  invalid = request;
  invalid.maxDepth = 1;
  expect(!platform_document_handoff::detail::CopyDirectoryForImport(
              sourceRoot, invalid, temporaryRoot)
              .ok(),
         "directory import enforces the selected folder depth limit");
  invalid = request;
  invalid.maxPathBytes = 7;
  expect(!platform_document_handoff::detail::CopyDirectoryForImport(
              sourceRoot, invalid, temporaryRoot)
              .ok(),
         "directory import enforces the selected folder path-byte limit");
  invalid = request;
  invalid.maxRegularFileBytes = 3;
  expect(!platform_document_handoff::detail::CopyDirectoryForImport(
              sourceRoot, invalid, temporaryRoot)
              .ok(),
         "directory import rejects a source file before copying beyond the per-file limit");

  std::atomic_bool cancelledBeforePicker = true;
  expect(platform_document_handoff::detail::CopyDirectoryForImport(
             sourceRoot, request, temporaryRoot, &cancelledBeforePicker)
             .cancelled(),
         "directory import observes cancellation before private copy begins");

  std::atomic_bool cancelled = false;
  const auto cancelledImport =
      platform_document_handoff::detail::CopyDirectoryForImport(
          sourceRoot, request, temporaryRoot, &cancelled,
          [&cancelled](std::uint64_t copied) {
            if (copied != 0) {
              cancelled = true;
            }
          });
  expect(cancelledImport.cancelled(),
         "directory import observes cancellation during copy before commit");

#if !defined(_WIN32)
  const auto mutableSourceRoot = sourceRoot.parent_path() / "Mutable Skin";
  std::filesystem::create_directories(mutableSourceRoot, error);
  const auto mutableSourceFile = mutableSourceRoot / "skin.lua";
  std::ofstream(mutableSourceFile, std::ios::binary) << "before";
  bool rewroteSource = false;
  const auto changedDuringCopy =
      platform_document_handoff::detail::CopyDirectoryForImport(
          mutableSourceRoot, request, temporaryRoot, nullptr,
          [&mutableSourceFile, &rewroteSource](std::uint64_t copied) {
            if (copied == 0 || rewroteSource) {
              return;
            }
            std::ofstream(mutableSourceFile,
                          std::ios::binary | std::ios::trunc)
                << "rewritten while the importer owns its descriptor";
            rewroteSource = true;
          });
  expect(rewroteSource && !changedDuringCopy.ok() &&
             changedDuringCopy.message.find("changed while being copied") !=
                 std::string::npos,
         "directory import rejects an in-place source edit during descriptor copy");
  std::filesystem::remove_all(mutableSourceRoot, error);
#endif

  const auto link = sourceRoot / "linked.txt";
  std::filesystem::create_symlink(sourceRoot / "skin.lua", link, error);
  if (!error) {
    const auto linked = platform_document_handoff::detail::CopyDirectoryForImport(
        sourceRoot, request, temporaryRoot);
    expect(!linked.ok() && !linked.cancelled(),
           "directory import rejects symbolic-link source entries");
    std::filesystem::remove(link, error);
    error.clear();
  }
  const auto sourceRootLink = sourceRoot.parent_path() / "Sample Skin link";
  std::filesystem::create_directory_symlink(sourceRoot, sourceRootLink, error);
  if (!error) {
    const auto linkedRoot =
        platform_document_handoff::detail::CopyDirectoryForImport(
            sourceRootLink, request, temporaryRoot);
    expect(!linkedRoot.ok() && !linkedRoot.cancelled(),
           "directory import rejects a selected source-root symlink");
    std::filesystem::remove(sourceRootLink, error);
    error.clear();
  }
#if !defined(_WIN32)
  const auto fifo = sourceRoot / "special-node";
  if (::mkfifo(fifo.c_str(), 0600) == 0) {
    const auto special = platform_document_handoff::detail::CopyDirectoryForImport(
        sourceRoot, request, temporaryRoot);
    expect(!special.ok() && !special.cancelled(),
           "directory import rejects non-regular source entries");
  }
#endif
  std::filesystem::remove_all(sourceRoot.parent_path(), error);
}

void testPublicAbandonAndSourceNameNormalizationHook() {
  const auto temporaryImport =
      makePrivateTestDocument("public-abandon", "temporary");
  auto operation = platform_document_handoff::detail::StartOperation(
      [temporaryImport](const std::atomic_bool &) {
        return platform_document_handoff::detail::ParseBridgeResult(
            temporaryImport.string(), true, true, PlatformTemporaryPathKind::File,
            "raw-source-name",
            [](std::string_view value) -> std::optional<std::string> {
              return std::string("normalized-") + std::string(value);
            });
      },
      [] {});
  expect(waitUntil([&] { return operation.ready(); }),
         "source-name hook test receives a ready import result");
  auto result = operation.takeResult();
  expect(result && result->originalSourceName == "normalized-raw-source-name",
         "bridge import preserves source name through the injected normalizer");
  operation.abandon();
  expect(!operation,
         "public abandon releases the operation handle like the close alias");
  if (result) {
    expect(platform_document_handoff::CleanupTemporaryDocument(*result),
           "taken source-name result retains legacy file cleanup");
  }

  const auto invalidTemporaryImport =
      makePrivateTestDocument("invalid-source-name", "temporary");
  const auto rejected = platform_document_handoff::detail::ParseBridgeResult(
      invalidTemporaryImport.string(), true, true,
      PlatformTemporaryPathKind::File,
      "invalid-source-name",
      [](std::string_view) -> std::optional<std::string> { return std::nullopt; });
  expect(!rejected.ok() && !std::filesystem::exists(invalidTemporaryImport) &&
             rejected.message.find("source name") != std::string::npos,
         "source-name normalization hook rejects invalid UTF-8 or unsafe names");
}

void testScheduledTemporaryCleanupAndReadyAbandonAreNonblocking() {
  const PlatformDocumentHandoffResult defaults;
  const PlatformDirectoryImportRequest directoryDefaults;
  expect(defaults.temporaryPathKind == PlatformTemporaryPathKind::None &&
             directoryDefaults.maxBytes == 0 && directoryDefaults.maxFiles == 0 &&
             directoryDefaults.maxRegularFileBytes == 0 &&
             directoryDefaults.maxDepth == 0 &&
             directoryDefaults.maxPathBytes == 0,
         "temporary path and directory import value defaults are explicit");

  const auto temporaryImport = makePrivateTestDocument(
      "scheduled-cleanup", "temporary");
  const auto firstRoot = temporaryImport.parent_path();
  std::error_code error;
  auto result = platform_document_handoff::detail::ParseBridgeResult(
      temporaryImport.string(), true, true);

  auto blockingCleanup = std::make_shared<BlockingWork>();
  const auto service =
      platform_document_handoff::CreatePlatformTemporaryPathCleanupService(
          [blockingCleanup](PlatformDocumentHandoffResult &) {
            blockingCleanup->waitForRelease();
            return false;
          });
  const auto scheduledAt = std::chrono::steady_clock::now();
  expect(service->schedule(result) &&
             std::chrono::steady_clock::now() - scheduledAt < 250ms &&
             blockingCleanup->waitUntilEntered(),
         "injected cleanup service schedules without waiting for its cleaner");

  const auto racedImport =
      makePrivateTestDocument("ready-abandon", "temporary");
  auto operation = platform_document_handoff::detail::StartOperation(
      [racedImport](const std::atomic_bool &) {
        return platform_document_handoff::detail::ParseBridgeResult(
            racedImport.string(), true, true);
      },
      [] {}, service);
  expect(waitUntil([&] { return operation.ready(); }),
         "ready-abandon test receives an unconsumed import result");
  const auto abandonedAt = std::chrono::steady_clock::now();
  operation.close();
  expect(std::chrono::steady_clock::now() - abandonedAt < 250ms,
         "close transfers a ready import without waiting for the cleaner");

  std::atomic_bool shutdownReturned = false;
  std::thread shutdownThread([&] {
    service->shutdown();
    shutdownReturned = true;
  });
  std::this_thread::sleep_for(25ms);
  expect(!shutdownReturned,
         "shutdown drains queued cleanup instead of abandoning a capability");
  blockingCleanup->release();
  shutdownThread.join();
  service->shutdown();
  const auto unprocessed = service->takeUnprocessed();
  expect(unprocessed.size() == 2,
         "failed injected cleanup remains reportable after shutdown");

  auto rejected = platform_document_handoff::detail::ParseBridgeResult(
      makePrivateTestDocument("rejected-cleanup", "temporary").string(), true,
      true);
  expect(!service->schedule(rejected) && rejected.temporaryOwnership == nullptr,
         "rejected scheduling transfers ownership into the service report");
  const auto rejectedResults = service->takeUnprocessed();
  expect(rejectedResults.size() == 1,
         "shutdown service reports capabilities rejected after shutdown");

  std::filesystem::remove_all(firstRoot, error);
  std::filesystem::remove_all(racedImport.parent_path(), error);
  for (const auto &pending : rejectedResults) {
    std::filesystem::remove_all(pending.localPath.parent_path(), error);
  }
}

void testReadyTakeResultAndAbandonRaceHasOneNonblockingDisposition() {
  std::atomic_uint32_t cleanupCalls = 0;
  const auto service =
      platform_document_handoff::CreatePlatformTemporaryPathCleanupService(
          [&cleanupCalls](PlatformDocumentHandoffResult &result) {
            ++cleanupCalls;
            return platform_document_handoff::CleanupTemporaryPath(result);
          });

  for (int iteration = 0; iteration < 64; ++iteration) {
    const auto temporaryImport = makePrivateTestDocument(
        "ready-take-abandon-race", "temporary");
    auto work = std::make_shared<BlockingWork>();
    auto operation = platform_document_handoff::detail::StartOperation(
        [work, temporaryImport](const std::atomic_bool &) {
          work->waitForRelease();
          return platform_document_handoff::detail::ParseBridgeResult(
              temporaryImport.string(), true, true);
        },
        [] {}, service);
    expect(work->waitUntilEntered() && !operation.ready(),
           "race iteration reaches the ready/takeResult boundary");
    const auto cleanupBefore = cleanupCalls.load(std::memory_order_acquire);
    work->release();
    if ((iteration % 2) == 0) {
      std::this_thread::yield();
    }
    auto result = operation.takeResult();
    const auto abandonAt = std::chrono::steady_clock::now();
    operation.abandon();
    expect(std::chrono::steady_clock::now() - abandonAt < 250ms,
           "abandon remains nonblocking while completion races takeResult");
    if (result) {
      expect(platform_document_handoff::CleanupTemporaryPath(*result),
             "a taken race result has exactly one caller-owned cleanup path");
    } else {
      expect(waitUntil([&] {
               return !std::filesystem::exists(temporaryImport);
             }) && cleanupCalls.load(std::memory_order_acquire) ==
                       cleanupBefore + 1,
             "an untaken race result is disposed exactly once by the worker service");
    }
  }
  service->shutdown();
}

void testTemporaryCleanupRejectsForgedAndSymlinkEscapes() {
  const auto outsideRoot = std::filesystem::temp_directory_path() /
                           "asobmashow-platform-cleanup-outside";
  const auto outsideFile = outsideRoot / "keep.zip";
  std::error_code error;
  std::filesystem::remove_all(outsideRoot, error);
  std::filesystem::create_directories(outsideRoot, error);
  {
    std::ofstream output(outsideFile, std::ios::binary);
    output << "keep";
  }

  PlatformDocumentHandoffResult forged{
      .status = PlatformDocumentHandoffStatus::Succeeded,
      .localPath = outsideFile,
      .temporaryLocalFile = true};
  expect(!platform_document_handoff::CleanupTemporaryDocument(forged) &&
             std::filesystem::exists(outsideFile),
         "a caller-controlled temporary flag cannot authorize deletion");

  const auto fakeRoot = outsideRoot / "attacker" / "document-handoff";
  std::filesystem::create_directories(fakeRoot, error);
  const auto fakeOwnedPath = fakeRoot / "victim.zip";
  {
    std::ofstream output(fakeOwnedPath, std::ios::binary);
    output << "keep";
  }
  const auto fakeOwned = platform_document_handoff::detail::ParseBridgeResult(
      fakeOwnedPath.string(), true, true);
  expect(!fakeOwned.ok() && std::filesystem::exists(fakeOwnedPath),
         "an attacker directory merely named document-handoff cannot mint "
         "ownership");

  const auto legitimate =
      makePrivateTestDocument("mutated-token", "legitimate");
  auto owned = platform_document_handoff::detail::ParseBridgeResult(
      legitimate.string(), true, true);
  owned.localPath = outsideFile;
  expect(!platform_document_handoff::CleanupTemporaryDocument(owned) &&
             std::filesystem::exists(outsideFile) &&
             std::filesystem::exists(legitimate),
         "an opaque cleanup token cannot be moved onto another path");
  owned.localPath = legitimate;
  expect(platform_document_handoff::CleanupTemporaryDocument(owned),
         "the original owned path remains cleanable");

  const auto handoffBase = std::filesystem::temp_directory_path() /
                           "AsoBMaShow" / "document-handoff";
  const auto finalLinkDirectory = handoffBase / "final-link-rejection";
  const auto finalLink = finalLinkDirectory / "imported-document.zip";
  std::filesystem::remove_all(finalLinkDirectory, error);
  std::filesystem::create_directories(finalLinkDirectory, error);
  std::filesystem::create_symlink(outsideFile, finalLink, error);
  if (!error) {
    const auto linked = platform_document_handoff::detail::ParseBridgeResult(
        finalLink.string(), true, true);
    expect(!linked.ok() && std::filesystem::exists(outsideFile),
           "ownership is never minted for a final-component symlink");
    std::filesystem::remove_all(finalLinkDirectory, error);
  }

  const auto swappedPath =
      makePrivateTestDocument("final-link-swap", "owned before swap");
  auto swapped = platform_document_handoff::detail::ParseBridgeResult(
      swappedPath.string(), true, true);
  expect(swapped.ok(), "regular private file receives cleanup ownership");
  std::filesystem::remove(swappedPath, error);
  std::filesystem::create_symlink(outsideFile, swappedPath, error);
  if (!error) {
    expect(
        platform_document_handoff::CleanupTemporaryDocument(swapped) &&
            !std::filesystem::exists(swappedPath) &&
            std::filesystem::exists(outsideFile),
        "cleanup removes a post-adoption final symlink without following it");
  }

  const auto escapeLink = handoffBase / "symlink-escape";
  std::filesystem::remove(escapeLink, error);
  std::filesystem::create_directories(handoffBase, error);
  std::filesystem::create_directory_symlink(outsideRoot, escapeLink, error);
  if (!error) {
    const auto escaped = platform_document_handoff::detail::ParseBridgeResult(
        (escapeLink / outsideFile.filename()).string(), true, true);
    expect(!escaped.ok() && std::filesystem::exists(outsideFile),
           "cleanup ownership rejects a symlink ancestor escaping private "
           "storage");
    std::filesystem::remove(escapeLink, error);
  }

  std::filesystem::remove_all(outsideRoot, error);
}

void testTemporaryOwnershipRequiresExactPrivateShapeAndModes() {
  std::error_code error;
  const auto wrongLeaf = makePrivateTestDocument("wrong-leaf", "wrong leaf");
  const auto renamedLeaf = wrongLeaf.parent_path() / "another-name.zip";
  std::filesystem::rename(wrongLeaf, renamedLeaf, error);
  const auto wrongLeafResult =
      platform_document_handoff::detail::ParseBridgeResult(renamedLeaf.string(),
                                                           true, true);
  expect(!wrongLeafResult.ok() && std::filesystem::exists(renamedLeaf),
         "desktop ownership requires the issued import file name");
  std::filesystem::remove_all(renamedLeaf.parent_path(), error);

  const auto nestedRoot =
      makePrivateTestDocument("nested-shape", "nested source").parent_path();
  const auto nestedDirectory = nestedRoot / "nested";
  std::filesystem::create_directory(nestedDirectory, error);
  std::filesystem::permissions(nestedDirectory,
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  const auto nestedPath = nestedDirectory / "imported-document.zip";
  {
    std::ofstream output(nestedPath, std::ios::binary);
    output << "nested";
  }
  std::filesystem::permissions(nestedPath,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, error);
  const auto nestedResult =
      platform_document_handoff::detail::ParseBridgeResult(nestedPath.string(),
                                                           true, true);
  expect(!nestedResult.ok() && std::filesystem::exists(nestedPath),
         "desktop ownership rejects nested files below an issued directory");
  std::filesystem::remove_all(nestedRoot, error);

#if !defined(_WIN32)
  const auto looseMode =
      makePrivateTestDocument("loose-mode", "loose permissions");
  std::filesystem::permissions(looseMode,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write |
                                   std::filesystem::perms::group_read,
                               std::filesystem::perm_options::replace, error);
  const auto looseModeResult =
      platform_document_handoff::detail::ParseBridgeResult(looseMode.string(),
                                                           true, true);
  expect(!looseModeResult.ok() && std::filesystem::exists(looseMode),
         "desktop ownership rejects a private copy readable by another user");
  std::filesystem::remove_all(looseMode.parent_path(), error);
#endif
}

void testPrivateImportRootRejectsPreplantedLinks() {
  const auto testRoot = std::filesystem::temp_directory_path() /
                        "asobmashow-private-root-hardening-test";
  const auto outsideRoot = std::filesystem::temp_directory_path() /
                           "asobmashow-private-root-hardening-outside";
  std::error_code error;
  std::filesystem::remove_all(testRoot, error);
  std::filesystem::remove_all(outsideRoot, error);
  std::filesystem::create_directories(testRoot, error);
  std::filesystem::create_directories(outsideRoot, error);
  const auto marker = outsideRoot / "keep.txt";
  {
    std::ofstream output(marker);
    output << "keep";
  }

  const auto applicationRoot = testRoot / "AsoBMaShow";
  std::filesystem::create_directory_symlink(outsideRoot, applicationRoot,
                                            error);
  if (!error) {
    std::string message;
    expect(platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
               testRoot, message)
                   .empty() &&
               std::filesystem::exists(marker),
           "a preplanted application-root symlink is rejected");
    std::filesystem::remove(applicationRoot, error);
  }

  std::filesystem::create_directory(applicationRoot, error);
  std::string securityError;
  platform_document_handoff::detail::SecurePrivateDocumentPath(
      applicationRoot, true, securityError);
  const auto handoffRoot = applicationRoot / "document-handoff";
  std::filesystem::create_directory_symlink(outsideRoot, handoffRoot, error);
  if (!error) {
    std::string message;
    expect(platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
               testRoot, message)
                   .empty() &&
               std::filesystem::exists(marker),
           "a preplanted handoff-root symlink is rejected");
    std::filesystem::remove(handoffRoot, error);
  }

  std::string message;
  const auto created =
      platform_document_handoff::detail::CreatePrivateImportDirectoryUnder(
          testRoot, message);
  expect(!created.empty() && message.empty() &&
             created.parent_path() == handoffRoot &&
             created.filename().string().size() == 32,
         "private import root creation yields a secured issued directory");

  std::filesystem::remove_all(testRoot, error);
  std::filesystem::remove_all(outsideRoot, error);
}

void testPreferredProfileExportNameUsesProfileExtension() {
  expect(platform_document_handoff::detail::PreferredProfileExportName(
             "player-profile") == "player-profile.asobprofile",
         "desktop profile export adds the profile archive extension");
  expect(platform_document_handoff::detail::PreferredProfileExportName(
             "player-profile.asobprofile") == "player-profile.asobprofile" &&
             platform_document_handoff::detail::PreferredProfileExportName(
                 "legacy.zip") == "legacy.zip",
         "desktop profile export preserves explicit profile and legacy "
         "extensions");
}

void testTextDocumentExportStagesExactOwnedFile() {
  const auto root = std::filesystem::temp_directory_path() /
                    "asobmashow-text-document-export-test";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);

  PlatformTextDocumentExportRequest request;
  request.text =
      "parse started\narchive: \xed\x8c\x8c\xec\x9d\xbc.zip\nparse finished\n";
  request.suggestedName = "AsoBMaShow-performance-log.txt";
  request.maxBytes = 4096;
  auto prepared =
      platform_document_handoff::detail::PrepareTextDocumentExportUnder(request,
                                                                        root);

  expect(prepared.ok(), "text export staging succeeds for a bounded log");
  const auto stagedPath = prepared.request.localPath;
  const auto stagedDirectory = stagedPath.parent_path();
  std::ifstream input(stagedPath, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  expect(contents.str() == request.text,
         "text export staging preserves the exact UTF-8 log bytes");
  expect(prepared.request.mimeType == "text/plain" &&
             prepared.request.suggestedName == request.suggestedName &&
             prepared.request.maxBytes == request.maxBytes &&
             prepared.request.sourceLifetime != nullptr,
         "text export staging prepares a bounded owned handoff request");

  input.close();
  prepared.request.sourceLifetime.reset();
  expect(!std::filesystem::exists(stagedPath) &&
             !std::filesystem::exists(stagedDirectory),
         "text export staging removes its private source when ownership ends");
  std::filesystem::remove_all(root, error);
}

void testTextDocumentExportRejectsUnsafeOrOversizedRequests() {
  const auto root = std::filesystem::temp_directory_path() /
                    "asobmashow-invalid-text-document-export-test";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);

  PlatformTextDocumentExportRequest request;
  request.text = "too large";
  request.suggestedName = "performance-log.txt";
  request.maxBytes = 3;
  auto prepared =
      platform_document_handoff::detail::PrepareTextDocumentExportUnder(request,
                                                                        root);
  expect(!prepared.ok() && !prepared.errorMessage.empty(),
         "text export staging rejects content above its explicit limit");

  request.maxBytes = 4096;
  request.suggestedName = "../performance-log.txt";
  prepared = platform_document_handoff::detail::PrepareTextDocumentExportUnder(
      request, root);
  expect(!prepared.ok() && !prepared.errorMessage.empty(),
         "text export staging rejects a non-leaf suggested name");
  std::filesystem::remove_all(root, error);
}

void testUnicodeDesktopPathsRoundTripAsUtf8() {
  const std::string unicodeUtf8 =
      "\xed\x94\x84\xeb\xa1\x9c\xed\x95\x84-\xf0\x9f\x8e\xb5.asobprofile";
  const auto unicodePath =
      platform_document_handoff::detail::PathFromUtf8(unicodeUtf8);
  expect(platform_document_handoff::detail::PathToUtf8(unicodePath) ==
             unicodeUtf8,
         "desktop bridge paths preserve Korean and emoji UTF-8 bytes");

  const auto root =
      std::filesystem::temp_directory_path() / "asobmashow-unicode-export-test";
  const auto source = root / "source.zip";
  const auto destination = root / unicodePath;
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root, error);
  {
    std::ofstream output(source, std::ios::binary);
    output << "unicode-profile";
  }
  std::string message;
  expect(platform_document_handoff::detail::CopyFileForExport(
             source, destination, 15, message) &&
             std::filesystem::exists(destination),
         "desktop export publishes to a Unicode destination path");
  std::filesystem::remove_all(root, error);
}
} // namespace

int main() {
  testBridgeCancellationIsDistinct();
  testBridgeErrorPreservesMessage();
  testBridgeImportRequiresAbsoluteLocalPath();
  testDefaultBridgeNormalizesPickedSourceNamesAndRejectsInvalidUtf8();
  testBridgeExportSuccessHasNoLocalPath();
  testImportRequestRequiresMimeTypeAndExplicitLimit();
  testExportRequestRequiresLeafNameAndBoundedRegularSource();
  testBoundedStreamCopyStopsAtLimit();
  testFailedDesktopExportPreservesExistingDestination();
  testDesktopExportCreatesItsStagingFileExclusively();
  testAsyncHandoffValidatesBeforeOpeningPicker();
  testCancelPendingCompletesPromptlyAndExactlyOnce();
  testNativeCancellationRegistrationClosesActivationGap();
  testCancelledWaiterLeavesSerializedNativeGatePromptly();
  testOperationTokensNeverWrapOrCancelANewerOperation();
  testCommitDecisionKeepsTeardownNonblocking();
  testCancelledIrreversibleNativeCommitKeepsProviderContentUntouched();
  testDestroyPendingDoesNotBlockOrUseDestroyedHandle();
  testExportSourceLifetimeOutlivesNonblockingTeardown();
  testLateImportCompletionIsIgnoredAndCleaned();
  testUnconsumedTemporaryImportIsCleanedOnClose();
  testTakenTemporaryImportHasExplicitCleanup();
  testDirectoryCleanupRequiresItsExactIssuedRoot();
  testDirectoryCleanupRejectsIdentitySwapAndUnlinksFinalSymlink();
  testDirectoryImportUsesThePickedFolderAsItsPrivateRoot();
  testPublicAbandonAndSourceNameNormalizationHook();
  testScheduledTemporaryCleanupAndReadyAbandonAreNonblocking();
  testReadyTakeResultAndAbandonRaceHasOneNonblockingDisposition();
  testTemporaryCleanupRejectsForgedAndSymlinkEscapes();
  testTemporaryOwnershipRequiresExactPrivateShapeAndModes();
  testPrivateImportRootRejectsPreplantedLinks();
  testPreferredProfileExportNameUsesProfileExtension();
  testTextDocumentExportStagesExactOwnedFile();
  testTextDocumentExportRejectsUnsafeOrOversizedRequests();
  testUnicodeDesktopPathsRoundTripAsUtf8();

  if (failures != 0) {
    std::cerr << failures << " platform document handoff test(s) failed\n";
    return 1;
  }
  std::cout << "Platform document handoff tests passed\n";
  return 0;
}
