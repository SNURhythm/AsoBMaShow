#pragma once

#include "targets.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace platform_document_handoff::detail {
class TemporaryDocumentOwnership;
}

enum class PlatformTemporaryPathKind {
  None,
  File,
  Directory,
};

enum class PlatformDocumentHandoffStatus {
  Succeeded,
  Cancelled,
  Failed,
};

struct PlatformDocumentHandoffResult {
  PlatformDocumentHandoffStatus status = PlatformDocumentHandoffStatus::Failed;
  std::string message;
  std::filesystem::path localPath;
  // The selected file or folder basename, normalized by the caller-supplied
  // policy hook. Native bridges return only the raw UTF-8 name.
  std::string originalSourceName;

  // A successful import is staged in application-private storage. The caller
  // owns that file and should remove it after consuming the document.
  bool temporaryLocalFile = false;
  PlatformTemporaryPathKind temporaryPathKind = PlatformTemporaryPathKind::None;

  // Opaque deletion capability issued only for a platform-created private
  // copy. Cleanup never trusts temporaryLocalFile or localPath alone.
  std::shared_ptr<platform_document_handoff::detail::TemporaryDocumentOwnership>
      temporaryOwnership;

  [[nodiscard]] bool ok() const noexcept {
    return status == PlatformDocumentHandoffStatus::Succeeded;
  }
  [[nodiscard]] bool cancelled() const noexcept {
    return status == PlatformDocumentHandoffStatus::Cancelled;
  }
};

struct PlatformDocumentImportRequest {
  std::string mimeType;
  std::uint64_t maxBytes = 0;
};

// Reserved for the platform folder-picker implementation. It is deliberately a
// value type so portable lifecycle and cleanup code does not depend on native
// picker availability.
struct PlatformDirectoryImportRequest {
  std::uint64_t maxBytes = 0;
  std::uint64_t maxFiles = 0;
  std::uint64_t maxRegularFileBytes = 0;
  std::uint32_t maxDepth = 0;
  std::uint32_t maxPathBytes = 0;
};

struct PlatformDocumentExportRequest {
  std::filesystem::path localPath;
  std::string mimeType;
  std::string suggestedName;
  std::uint64_t maxBytes = 0;

  // Optional opaque RAII owner for a temporary localPath. The platform layer
  // retains it until detached export work has actually stopped reading the
  // source, including after nonblocking cancel() or close().
  std::shared_ptr<void> sourceLifetime;
};

struct PlatformTextDocumentExportRequest {
  std::string text;
  std::string suggestedName;
  std::uint64_t maxBytes = 0;
};

namespace platform_document_handoff {

class PlatformTemporaryPathCleanupService {
public:
  using CleanupTask =
      std::function<bool(PlatformDocumentHandoffResult &result)>;

  explicit PlatformTemporaryPathCleanupService(CleanupTask cleanupTask = {});
  ~PlatformTemporaryPathCleanupService();
  PlatformTemporaryPathCleanupService(
      const PlatformTemporaryPathCleanupService &) = delete;
  PlatformTemporaryPathCleanupService &operator=(
      const PlatformTemporaryPathCleanupService &) = delete;
  [[nodiscard]] bool schedule(PlatformDocumentHandoffResult &result) noexcept;
  void shutdown() noexcept;
  [[nodiscard]] std::vector<PlatformDocumentHandoffResult>
  takeUnprocessed();

private:
  struct State;
  std::shared_ptr<State> state_;
  std::thread worker_;
  std::mutex shutdownMutex_;
};

using PlatformTemporaryPathCleanupServiceHandle =
    std::shared_ptr<PlatformTemporaryPathCleanupService>;

PlatformTemporaryPathCleanupServiceHandle
CreatePlatformTemporaryPathCleanupService(
    PlatformTemporaryPathCleanupService::CleanupTask cleanupTask = {});
PlatformTemporaryPathCleanupServiceHandle
DefaultPlatformTemporaryPathCleanupService();

class PlatformDocumentHandoffOperation;
namespace detail {
class OperationState;
using OperationWork = std::function<PlatformDocumentHandoffResult(
    const std::atomic_bool &cancellationRequested)>;
using CommitGate =
    std::function<bool(const std::function<bool()> &commitAction)>;
using CommitOperationWork = std::function<PlatformDocumentHandoffResult(
    const std::atomic_bool &cancellationRequested,
    const CommitGate &commitGate)>;
using SourceNameNormalizer =
    std::function<std::optional<std::string>(std::string_view)>;
PlatformDocumentHandoffOperation
StartOperation(OperationWork work, std::function<void()> cancelNative,
               PlatformTemporaryPathCleanupServiceHandle cleanupService = {});
PlatformDocumentHandoffOperation
StartOperationWithCommit(CommitOperationWork work,
                         std::function<void()> cancelNative,
                         std::shared_ptr<void> workerLifetime = {},
                         PlatformTemporaryPathCleanupServiceHandle
                             cleanupService = {});
std::uint64_t NextOperationToken();
std::uint64_t NextOperationToken(std::atomic_uint64_t &counter);
std::filesystem::path
CreatePrivateImportDirectoryUnder(const std::filesystem::path &temporaryRoot,
                                  std::string &errorMessage);
bool SecurePrivateDocumentPath(const std::filesystem::path &path,
                               bool directory, std::string &errorMessage);
std::string PreferredProfileExportName(const std::string &suggestedName);
std::filesystem::path PathFromUtf8(std::string_view value);
std::string PathToUtf8(const std::filesystem::path &path);
bool LockInterruptibly(std::timed_mutex &mutex,
                       const std::atomic_bool &cancellationRequested);

struct PreparedTextDocumentExport {
  PlatformDocumentExportRequest request;
  std::string errorMessage;

  [[nodiscard]] bool ok() const noexcept {
    return errorMessage.empty() && !request.localPath.empty();
  }
};

PreparedTextDocumentExport
PrepareTextDocumentExportUnder(const PlatformTextDocumentExportRequest &request,
                               const std::filesystem::path &temporaryRoot);

// Closes the cancel-before-native-registration race without allowing an
// operation waiting behind another picker to cancel that other picker.
class NativeCancellationRegistration {
public:
  explicit NativeCancellationRegistration(std::function<void()> cancelNative);
  [[nodiscard]] bool activate() noexcept;
  void deactivate() noexcept;
  void cancel() noexcept;

private:
  std::mutex mutex_;
  std::function<void()> cancelNative_;
  bool active_ = false;
  bool cancellationRequested_ = false;
  bool cancellationDelivered_ = false;
};
} // namespace detail

// A move-only, nonblocking handle for a native document picker. The UI can
// poll it from the main thread and explicitly cancel it during scene teardown.
// Destruction is equivalent to close(): it never waits for native UI or I/O.
class PlatformDocumentHandoffOperation {
public:
  PlatformDocumentHandoffOperation() = default;
  PlatformDocumentHandoffOperation(
      PlatformDocumentHandoffOperation &&other) noexcept;
  PlatformDocumentHandoffOperation &
  operator=(PlatformDocumentHandoffOperation &&other) noexcept;
  PlatformDocumentHandoffOperation(const PlatformDocumentHandoffOperation &) =
      delete;
  PlatformDocumentHandoffOperation &
  operator=(const PlatformDocumentHandoffOperation &) = delete;
  ~PlatformDocumentHandoffOperation();

  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] bool poll() const noexcept { return ready(); }
  [[nodiscard]] std::optional<PlatformDocumentHandoffResult> takeResult();

  // cancel() leaves a Cancelled result available to takeResult(). close()
  // cancels if needed, discards any result, and releases the handle.
  void cancel() noexcept;
  // Signals and detaches only. Completion transfers an unconsumed temporary
  // result to its cleanup service without recursive work on this caller.
  void abandon() noexcept;
  void close() noexcept;
  [[nodiscard]] explicit operator bool() const noexcept {
    return state_ != nullptr;
  }

private:
  explicit PlatformDocumentHandoffOperation(
      std::shared_ptr<detail::OperationState> state);
  std::shared_ptr<detail::OperationState> state_;

  friend PlatformDocumentHandoffOperation
      detail::StartOperation(detail::OperationWork, std::function<void()>,
                             PlatformTemporaryPathCleanupServiceHandle);
  friend PlatformDocumentHandoffOperation
      detail::StartOperationWithCommit(detail::CommitOperationWork,
                                       std::function<void()>,
                                       std::shared_ptr<void>,
                                       PlatformTemporaryPathCleanupServiceHandle);
};

PlatformDocumentHandoffOperation
ImportDocumentAsync(PlatformDocumentImportRequest request);
PlatformDocumentHandoffOperation
ImportDocumentAsync(PlatformDocumentImportRequest request,
                    PlatformTemporaryPathCleanupServiceHandle cleanupService);
PlatformDocumentHandoffOperation
ImportDirectoryAsync(PlatformDirectoryImportRequest request,
                     PlatformTemporaryPathCleanupServiceHandle cleanupService);
PlatformDocumentHandoffOperation
ExportDocumentAsync(PlatformDocumentExportRequest request);
PlatformDocumentHandoffOperation
ExportTextDocumentAsync(PlatformTextDocumentExportRequest request);

// Removes a successful imported private copy after the caller has consumed it.
// Ownership is cleared even if the file was already absent.
bool CleanupTemporaryDocument(PlatformDocumentHandoffResult &result) noexcept;

// Synchronously removes an owned temporary file or directory. Cleanup is
// authorized solely by the opaque capability held in result and always targets
// its exact root. Lifecycle teardown uses PlatformTemporaryPathCleanupService.
bool CleanupTemporaryPath(PlatformDocumentHandoffResult &result) noexcept;

namespace detail {
PlatformDocumentHandoffResult
Validate(const PlatformDocumentImportRequest &request);
PlatformDocumentHandoffResult
Validate(const PlatformDirectoryImportRequest &request);
PlatformDocumentHandoffResult
Validate(const PlatformDocumentExportRequest &request);

PlatformDocumentHandoffResult ParseBridgeResult(const std::string &value,
                                                bool expectsLocalPath,
                                                bool temporaryLocalFile,
                                                PlatformTemporaryPathKind
                                                    temporaryPathKind =
                                                        PlatformTemporaryPathKind::File,
                                                std::string originalSourceName = {},
                                                SourceNameNormalizer
                                                    sourceNameNormalizer = {});

// Copies one picked source directory into one application-private root. The
// return path is that root itself, never an extra source-basename directory.
// This portable seam is also used by desktop folder selection; native bridges
// supply their own coordinated copies.
PlatformDocumentHandoffResult CopyDirectoryForImport(
    const std::filesystem::path &source,
    const PlatformDirectoryImportRequest &request,
    const std::filesystem::path &temporaryRoot,
    const std::atomic_bool *cancellationRequested = nullptr,
    const std::function<void(std::uint64_t)> &progress = {});

bool CopyStreamBounded(std::istream &input, std::ostream &output,
                       std::uint64_t maxBytes, std::uint64_t &bytesCopied,
                       std::string &errorMessage,
                       const std::atomic_bool *cancellationRequested = nullptr,
                       const std::function<void(std::uint64_t)> &progress = {});
bool CopyFileForExport(
    const std::filesystem::path &source,
    const std::filesystem::path &destination, std::uint64_t maxBytes,
    std::string &errorMessage,
    const std::atomic_bool *cancellationRequested = nullptr,
    const CommitGate &commitGate = {},
    const std::function<void(std::uint64_t)> &progress = {},
    const std::filesystem::path &firstTemporaryCandidate = {});

} // namespace detail
} // namespace platform_document_handoff
