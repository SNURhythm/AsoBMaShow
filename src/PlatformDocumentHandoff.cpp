#include "PlatformDocumentHandoff.h"
#include "skin/package/SkinPathPolicy.h"

#if TARGET_OS_ANDROID
#include "AndroidNatives.h"
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "iOSNatives.hpp"
#else
#include "tinyfiledialogs.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace platform_document_handoff {
namespace detail {
class TemporaryDocumentOwnership {
public:
  using Cleanup = std::function<bool(const std::filesystem::path &,
                                     PlatformTemporaryPathKind)>;

  TemporaryDocumentOwnership(std::filesystem::path path,
                             PlatformTemporaryPathKind kind, Cleanup cleanup)
      : path_(std::move(path)), kind_(kind), cleanup_(std::move(cleanup)) {}
  ~TemporaryDocumentOwnership() = default;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  [[nodiscard]] PlatformTemporaryPathKind kind() const noexcept {
    return kind_;
  }

  bool cleanup() noexcept {
    std::lock_guard lock(mutex_);
    if (cleaned_) {
      return true;
    }
    try {
      if (!cleanup_ || !cleanup_(path_, kind_)) {
        return false;
      }
      cleaned_ = true;
      return true;
    } catch (...) {
      return false;
    }
  }

private:
  std::mutex mutex_;
  std::filesystem::path path_;
  PlatformTemporaryPathKind kind_;
  Cleanup cleanup_;
  bool cleaned_ = false;
};

class OperationState {
public:
  explicit OperationState(PlatformTemporaryPathCleanupServiceHandle cleanupService)
      : cleanupService_(cleanupService != nullptr
                            ? std::move(cleanupService)
                            : DefaultPlatformTemporaryPathCleanupService()) {}

  ~OperationState() {
    std::optional<PlatformDocumentHandoffResult> discarded;
    {
      std::lock_guard lock(mutex_);
      if (result_.has_value()) {
        discarded = std::move(result_);
        result_.reset();
      }
    }
    if (discarded.has_value()) {
      scheduleDiscarded(*discarded);
    }
  }

  [[nodiscard]] const std::atomic_bool &cancellationRequested() const {
    return cancellationRequested_;
  }

  bool installCancelHandler(std::function<void()> cancelNative) {
    std::lock_guard lock(mutex_);
    if (terminal_ || abandoned_) {
      return false;
    }
    cancelNative_ = std::move(cancelNative);
    return true;
  }

  void complete(PlatformDocumentHandoffResult result) {
    std::optional<PlatformDocumentHandoffResult> discarded;
    {
      std::lock_guard lock(mutex_);
      if (terminal_ || abandoned_) {
        discarded = std::move(result);
      } else {
        terminal_ = true;
        cancelNative_ = {};
        result_ = std::move(result);
      }
    }
    if (discarded.has_value()) {
      scheduleDiscarded(*discarded);
    }
  }

  bool runCommit(const std::function<bool()> &commitAction) {
    {
      std::lock_guard lock(mutex_);
      if (terminal_ || abandoned_ || commitWon_ ||
          cancellationRequested_.load(std::memory_order_acquire)) {
        return false;
      }
      // The commit decision is the linearization point. Native teardown must
      // never wait for the filesystem action that follows it.
      commitWon_ = true;
    }
    return commitAction();
  }

  [[nodiscard]] bool ready() const noexcept {
    std::lock_guard lock(mutex_);
    return terminal_ && !abandoned_;
  }

  std::optional<PlatformDocumentHandoffResult> takeResult() {
    std::lock_guard lock(mutex_);
    if (!terminal_ || abandoned_ || !result_.has_value()) {
      return std::nullopt;
    }
    auto result = std::move(result_);
    result_.reset();
    return result;
  }

  void cancel() noexcept {
    std::function<void()> cancelNative;
    {
      std::lock_guard lock(mutex_);
      if (terminal_ || abandoned_ || commitWon_) {
        return;
      }
      cancellationRequested_.store(true, std::memory_order_release);
      terminal_ = true;
      result_ = PlatformDocumentHandoffResult{
          .status = PlatformDocumentHandoffStatus::Cancelled};
      cancelNative = std::move(cancelNative_);
      cancelNative_ = {};
    }
    invokeCancellation(cancelNative);
  }

  void abandon() noexcept {
    std::function<void()> cancelNative;
    std::optional<PlatformDocumentHandoffResult> discarded;
    {
      std::lock_guard lock(mutex_);
      if (abandoned_) {
        return;
      }
      abandoned_ = true;
      terminal_ = true;
      if (commitWon_) {
        cancelNative_ = {};
      } else {
        cancellationRequested_.store(true, std::memory_order_release);
        cancelNative = std::move(cancelNative_);
        cancelNative_ = {};
      }
      if (result_.has_value()) {
        discarded = std::move(result_);
        result_.reset();
      }
    }
    invokeCancellation(cancelNative);
    if (discarded.has_value()) {
      scheduleDiscarded(*discarded);
    }
  }

private:
  void scheduleDiscarded(PlatformDocumentHandoffResult &result) noexcept {
    if (cleanupService_ != nullptr) {
      (void)cleanupService_->schedule(result);
    }
  }

  static void invokeCancellation(std::function<void()> &cancelNative) noexcept {
    if (!cancelNative) {
      return;
    }
    try {
      cancelNative();
    } catch (...) {
      // Cancellation is best-effort and teardown must remain noexcept.
    }
  }

  mutable std::mutex mutex_;
  std::atomic_bool cancellationRequested_ = false;
  bool terminal_ = false;
  bool abandoned_ = false;
  bool commitWon_ = false;
  std::optional<PlatformDocumentHandoffResult> result_;
  std::function<void()> cancelNative_;
  PlatformTemporaryPathCleanupServiceHandle cleanupService_;
};

NativeCancellationRegistration::NativeCancellationRegistration(
    std::function<void()> cancelNative)
    : cancelNative_(std::move(cancelNative)) {}

bool NativeCancellationRegistration::activate() noexcept {
  std::lock_guard lock(mutex_);
  if (cancellationRequested_) {
    return false;
  }
  active_ = true;
  return true;
}

void NativeCancellationRegistration::deactivate() noexcept {
  std::lock_guard lock(mutex_);
  active_ = false;
}

void NativeCancellationRegistration::cancel() noexcept {
  std::function<void()> cancelNative;
  {
    std::lock_guard lock(mutex_);
    if (cancellationRequested_) {
      return;
    }
    cancellationRequested_ = true;
    if (active_ && !cancellationDelivered_) {
      cancellationDelivered_ = true;
      cancelNative = cancelNative_;
    }
  }
  if (cancelNative) {
    try {
      cancelNative();
    } catch (...) {
      // Teardown and cancellation remain noexcept.
    }
  }
}
} // namespace detail

namespace {
constexpr std::string_view kCancelled = "__CANCELLED__";
constexpr std::string_view kErrorPrefix = "__ERROR__:";
constexpr std::string_view kSuccess = "__OK__";

std::timed_mutex &documentHandoffMutex() {
  // A native desktop picker cannot be forcibly dismissed. Intentionally keep
  // serialization alive until process exit so a detached picker worker can
  // never resume into a destroyed static mutex during shutdown.
  static auto *mutex = new std::timed_mutex();
  return *mutex;
}

std::atomic_uint64_t &nextDocumentHandoffToken() {
  static auto *counter = new std::atomic_uint64_t(1);
  return *counter;
}

PlatformDocumentHandoffResult success() {
  return {.status = PlatformDocumentHandoffStatus::Succeeded};
}

PlatformDocumentHandoffResult failure(std::string message) {
  return {.status = PlatformDocumentHandoffStatus::Failed,
          .message = std::move(message)};
}

PlatformDocumentHandoffResult cancellation() {
  return {.status = PlatformDocumentHandoffStatus::Cancelled};
}

class NativeActivityScope {
public:
  explicit NativeActivityScope(
      detail::NativeCancellationRegistration &registration)
      : registration_(registration), active_(registration_.activate()) {}
  ~NativeActivityScope() {
    if (active_) {
      registration_.deactivate();
    }
  }
  [[nodiscard]] bool active() const noexcept { return active_; }

private:
  detail::NativeCancellationRegistration &registration_;
  bool active_;
};

bool validMimeType(const std::string &mimeType) {
  if (mimeType.empty() || mimeType.find('/') == std::string::npos) {
    return false;
  }
  return std::ranges::none_of(mimeType, [](unsigned char value) {
    return std::isspace(value) != 0 || std::iscntrl(value) != 0;
  });
}

bool validSuggestedName(const std::string &name) {
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    return false;
  }
  return std::ranges::none_of(
      name, [](unsigned char value) { return std::iscntrl(value) != 0; });
}

std::string randomHex() {
  std::random_device random;
  constexpr char digits[] = "0123456789abcdef";
  std::string value(32, '0');
  for (char &digit : value) {
    digit = digits[random() & 0x0fU];
  }
  return value;
}

enum class ExclusiveCreateResult {
  Opened,
  AlreadyExists,
  Failed,
};

class ExclusiveOutputFile {
public:
  ExclusiveOutputFile() = default;
  ExclusiveOutputFile(const ExclusiveOutputFile &) = delete;
  ExclusiveOutputFile &operator=(const ExclusiveOutputFile &) = delete;
  ~ExclusiveOutputFile() { closeIgnoringErrors(); }

  ExclusiveCreateResult open(const std::filesystem::path &path,
                             std::string &errorMessage) {
    closeIgnoringErrors();
#if defined(_WIN32)
    handle_ = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
        return ExclusiveCreateResult::AlreadyExists;
      }
      errorMessage = "Unable to create a temporary export file (Windows "
                     "error " +
                     std::to_string(error) + ").";
      return ExclusiveCreateResult::Failed;
    }
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    descriptor_ = ::open(path.c_str(), flags, 0600);
    if (descriptor_ < 0) {
      const int error = errno;
      if (error == EEXIST || error == ELOOP) {
        return ExclusiveCreateResult::AlreadyExists;
      }
      errorMessage = "Unable to create a temporary export file: " +
                     std::error_code(error, std::generic_category()).message();
      return ExclusiveCreateResult::Failed;
    }
#endif
    return ExclusiveCreateResult::Opened;
  }

  bool write(const char *data, std::size_t size, std::string &errorMessage) {
    std::size_t offset = 0;
    while (offset < size) {
#if defined(_WIN32)
      const auto remaining = size - offset;
      const DWORD requested = static_cast<DWORD>(
          std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max()));
      DWORD written = 0;
      if (!WriteFile(handle_, data + offset, requested, &written, nullptr) ||
          written == 0) {
        const DWORD error = GetLastError();
        errorMessage = "Writing the document failed (Windows error " +
                       std::to_string(error) + ").";
        return false;
      }
      offset += written;
#else
      const ssize_t written =
          ::write(descriptor_, data + offset, size - offset);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        const int error = written < 0 ? errno : EIO;
        errorMessage =
            "Writing the document failed: " +
            std::error_code(error, std::generic_category()).message();
        return false;
      }
      offset += static_cast<std::size_t>(written);
#endif
    }
    return true;
  }

  bool finish(std::string &errorMessage) {
#if defined(_WIN32)
    if (!FlushFileBuffers(handle_)) {
      const DWORD error = GetLastError();
      closeIgnoringErrors();
      errorMessage = "Unable to finish writing the exported document "
                     "(Windows error " +
                     std::to_string(error) + ").";
      return false;
    }
    if (!CloseHandle(handle_)) {
      const DWORD error = GetLastError();
      handle_ = INVALID_HANDLE_VALUE;
      errorMessage = "Unable to close the exported document safely (Windows "
                     "error " +
                     std::to_string(error) + ").";
      return false;
    }
    handle_ = INVALID_HANDLE_VALUE;
#else
    int syncResult = -1;
    do {
      syncResult = ::fsync(descriptor_);
    } while (syncResult < 0 && errno == EINTR);
    if (syncResult < 0) {
      const int error = errno;
      closeIgnoringErrors();
      errorMessage = "Unable to finish writing the exported document: " +
                     std::error_code(error, std::generic_category()).message();
      return false;
    }
    const int descriptor = descriptor_;
    descriptor_ = -1;
    if (::close(descriptor) < 0) {
      const int error = errno;
      errorMessage = "Unable to close the exported document safely: " +
                     std::error_code(error, std::generic_category()).message();
      return false;
    }
#endif
    return true;
  }

  void abort() noexcept { closeIgnoringErrors(); }

private:
  void closeIgnoringErrors() noexcept {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
#else
    if (descriptor_ >= 0) {
      ::close(descriptor_);
      descriptor_ = -1;
    }
#endif
  }

#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int descriptor_ = -1;
#endif
};

class StagedTextDocumentOwnership {
public:
  StagedTextDocumentOwnership(std::filesystem::path path,
                              std::filesystem::path directory)
      : path_(std::move(path)), directory_(std::move(directory)) {}
  ~StagedTextDocumentOwnership() {
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
    ignored.clear();
    std::filesystem::remove(directory_, ignored);
  }

private:
  std::filesystem::path path_;
  std::filesystem::path directory_;
};

bool copyStreamToExclusiveFile(
    std::istream &input, ExclusiveOutputFile &output, std::uint64_t maxBytes,
    std::uint64_t &bytesCopied, std::string &errorMessage,
    const std::atomic_bool *cancellationRequested,
    const std::function<void(std::uint64_t)> &progress) {
  bytesCopied = 0;
  if (maxBytes == 0) {
    errorMessage = "A non-zero maximum document size is required.";
    return false;
  }
  std::array<char, 64 * 1024> buffer{};
  while (true) {
    if (cancellationRequested != nullptr &&
        cancellationRequested->load(std::memory_order_acquire)) {
      errorMessage = "The document operation was cancelled.";
      return false;
    }
    const auto remaining = maxBytes - bytesCopied;
    const auto requestedBytes =
        remaining < buffer.size() ? remaining + 1 : buffer.size();
    input.read(buffer.data(), static_cast<std::streamsize>(requestedBytes));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      const auto unsignedCount = static_cast<std::uint64_t>(count);
      if (unsignedCount > remaining) {
        errorMessage = "The document exceeds the maximum document size.";
        return false;
      }
      if (!output.write(buffer.data(), static_cast<std::size_t>(count),
                        errorMessage)) {
        return false;
      }
      bytesCopied += unsignedCount;
      if (progress) {
        progress(bytesCopied);
      }
    }
    if (cancellationRequested != nullptr &&
        cancellationRequested->load(std::memory_order_acquire)) {
      errorMessage = "The document operation was cancelled.";
      return false;
    }
    if (input.bad()) {
      errorMessage = "Reading the document failed.";
      return false;
    }
    if (input.eof()) {
      return true;
    }
    if (count == 0) {
      errorMessage = "Reading the document made no progress.";
      return false;
    }
  }
}

bool pathIsWithin(const std::filesystem::path &path,
                  const std::filesystem::path &directory, bool allowEqual) {
  auto pathPart = path.begin();
  for (auto directoryPart = directory.begin(); directoryPart != directory.end();
       ++directoryPart, ++pathPart) {
    if (pathPart == path.end() || *pathPart != *directoryPart) {
      return false;
    }
  }
  return allowEqual || pathPart != path.end();
}

bool validPrivateDirectoryName(const std::filesystem::path &path) {
  const auto name = path.filename().string();
  return name.size() == 32 &&
         std::ranges::all_of(name, [](unsigned char value) {
           return std::isdigit(value) != 0 ||
                  (value >= static_cast<unsigned char>('a') &&
                   value <= static_cast<unsigned char>('f'));
         });
}

#if defined(_WIN32)
bool currentWindowsUserSid(std::vector<unsigned char> &tokenBuffer,
                           PSID &userSid, std::string &errorMessage) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    errorMessage = "Unable to inspect the current Windows user (error " +
                   std::to_string(GetLastError()) + ").";
    return false;
  }
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  const DWORD sizeError = GetLastError();
  if (size == 0 || sizeError != ERROR_INSUFFICIENT_BUFFER) {
    CloseHandle(token);
    errorMessage = "Unable to inspect the current Windows user token (error " +
                   std::to_string(sizeError) + ").";
    return false;
  }
  tokenBuffer.resize(size);
  if (!GetTokenInformation(token, TokenUser, tokenBuffer.data(), size, &size)) {
    const DWORD error = GetLastError();
    CloseHandle(token);
    errorMessage = "Unable to read the current Windows user token (error " +
                   std::to_string(error) + ").";
    return false;
  }
  CloseHandle(token);
  userSid = reinterpret_cast<TOKEN_USER *>(tokenBuffer.data())->User.Sid;
  if (!IsValidSid(userSid)) {
    errorMessage = "The current Windows user SID is invalid.";
    return false;
  }
  return true;
}

bool verifyOwnerPrivatePath(const std::filesystem::path &path, bool directory,
                            std::string &errorMessage) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      (((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory)) {
    errorMessage = "Private document storage has an unsafe Windows file type.";
    return false;
  }

  std::vector<unsigned char> tokenBuffer;
  PSID userSid = nullptr;
  if (!currentWindowsUserSid(tokenBuffer, userSid, errorMessage)) {
    return false;
  }
  PSID ownerSid = nullptr;
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
  const DWORD securityError = GetNamedSecurityInfoW(
      const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
      OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &ownerSid,
      nullptr, &dacl, nullptr, &securityDescriptor);
  if (securityError != ERROR_SUCCESS) {
    errorMessage = "Unable to inspect private Windows permissions (error " +
                   std::to_string(securityError) + ").";
    return false;
  }

  bool privateAcl = ownerSid != nullptr && EqualSid(ownerSid, userSid) &&
                    dacl != nullptr && dacl->AceCount != 0;
  for (DWORD index = 0; privateAcl && index < dacl->AceCount; ++index) {
    void *rawAce = nullptr;
    if (!GetAce(dacl, index, &rawAce)) {
      privateAcl = false;
      break;
    }
    const auto *header = static_cast<const ACE_HEADER *>(rawAce);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
      privateAcl = false;
      break;
    }
    const auto *ace = static_cast<const ACCESS_ALLOWED_ACE *>(rawAce);
    const bool grantsFullControl =
        (ace->Mask & GENERIC_ALL) == GENERIC_ALL ||
        (ace->Mask & FILE_ALL_ACCESS) == FILE_ALL_ACCESS;
    privateAcl = EqualSid(const_cast<DWORD *>(&ace->SidStart), userSid) &&
                 grantsFullControl;
  }
  LocalFree(securityDescriptor);
  if (!privateAcl) {
    errorMessage =
        "Private Windows document storage is not restricted to its owner.";
    return false;
  }
  return true;
}

bool secureOwnerPrivatePath(const std::filesystem::path &path, bool directory,
                            std::string &errorMessage) {
  std::vector<unsigned char> tokenBuffer;
  PSID userSid = nullptr;
  if (!currentWindowsUserSid(tokenBuffer, userSid, errorMessage)) {
    return false;
  }

  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = GENERIC_ALL;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance =
      directory ? SUB_CONTAINERS_AND_OBJECTS_INHERIT : NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = static_cast<LPWSTR>(userSid);
  PACL dacl = nullptr;
  DWORD securityError = SetEntriesInAclW(1, &access, nullptr, &dacl);
  if (securityError == ERROR_SUCCESS) {
    securityError = SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
            PROTECTED_DACL_SECURITY_INFORMATION,
        userSid, nullptr, dacl, nullptr);
  }
  if (dacl != nullptr) {
    LocalFree(dacl);
  }
  if (securityError != ERROR_SUCCESS) {
    errorMessage = "Unable to secure private Windows storage (error " +
                   std::to_string(securityError) + ").";
    return false;
  }
  return verifyOwnerPrivatePath(path, directory, errorMessage);
}
#else
bool verifyOwnerPrivatePath(const std::filesystem::path &path, bool directory,
                            std::string &errorMessage) {
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
    const int error = errno;
    errorMessage = "Unable to inspect private document permissions: " +
                   std::error_code(error, std::generic_category()).message();
    return false;
  }
  const bool expectedType =
      directory ? S_ISDIR(status.st_mode) : S_ISREG(status.st_mode);
  const mode_t expectedMode = directory ? 0700 : 0600;
  if (!expectedType || status.st_uid != geteuid() ||
      (status.st_mode & 0777) != expectedMode) {
    errorMessage = "Private document storage is not restricted to its owner.";
    return false;
  }
  return true;
}

bool secureOwnerPrivatePath(const std::filesystem::path &path, bool directory,
                            std::string &errorMessage) {
  std::error_code error;
  std::filesystem::permissions(path,
                               directory
                                   ? std::filesystem::perms::owner_all
                                   : std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, error);
  if (error) {
    errorMessage =
        "Unable to secure private document storage: " + error.message();
    return false;
  }
  return verifyOwnerPrivatePath(path, directory, errorMessage);
}
#endif

std::filesystem::path desktopDocumentHandoffBase(std::error_code &error) {
  auto temporary = std::filesystem::temp_directory_path(error);
  if (error) {
    return {};
  }
  return (temporary / "AsoBMaShow" / "document-handoff").lexically_normal();
}

bool validateDesktopTemporaryPath(const std::filesystem::path &path,
                                  PlatformTemporaryPathKind kind,
                                  bool allowMissing, bool allowFinalSymlink,
                                  std::string &errorMessage) {
  std::error_code error;
  const auto base = desktopDocumentHandoffBase(error);
  const auto candidate = path.lexically_normal();
  if (error || !candidate.is_absolute() ||
      !pathIsWithin(candidate, base, false)) {
    errorMessage = "Temporary document is outside the private handoff root.";
    return false;
  }
  const auto issuedRoot = kind == PlatformTemporaryPathKind::File
                              ? candidate.parent_path()
                              : candidate;
  const bool validShape = kind == PlatformTemporaryPathKind::File
                              ? candidate.filename() == "imported-document.zip" &&
                                    issuedRoot.parent_path() == base
                              : issuedRoot.parent_path() == base;
  if (!validShape || !validPrivateDirectoryName(issuedRoot)) {
    errorMessage = "Temporary document does not match an issued handoff path.";
    return false;
  }
  const auto baseStatus = std::filesystem::symlink_status(base, error);
  if (error || std::filesystem::is_symlink(baseStatus)) {
    errorMessage = "The private handoff root is not trustworthy.";
    return false;
  }
  const auto applicationRootStatus =
      std::filesystem::symlink_status(base.parent_path(), error);
  if (error || std::filesystem::is_symlink(applicationRootStatus)) {
    errorMessage = "Application temporary storage is not trustworthy.";
    return false;
  }
  if (!verifyOwnerPrivatePath(base.parent_path(), true, errorMessage) ||
      !verifyOwnerPrivatePath(base, true, errorMessage)) {
    return false;
  }
  for (auto ancestor = issuedRoot; ancestor != base;
       ancestor = ancestor.parent_path()) {
    if (ancestor.empty()) {
      errorMessage = "Temporary document ancestry is invalid.";
      return false;
    }
    const auto status = std::filesystem::symlink_status(ancestor, error);
    if (allowMissing && ancestor == candidate &&
        (error == std::errc::no_such_file_or_directory ||
         (!error && status.type() == std::filesystem::file_type::not_found))) {
      errorMessage.clear();
      return true;
    }
    if (allowFinalSymlink && ancestor == candidate &&
        std::filesystem::is_symlink(status)) {
      break;
    }
    if (error || std::filesystem::is_symlink(status)) {
      errorMessage = "Temporary document has a symbolic-link ancestor.";
      return false;
    }
    if (!verifyOwnerPrivatePath(ancestor, true, errorMessage)) {
      return false;
    }
  }

  const auto canonicalBase = std::filesystem::canonical(base, error);
  if (error) {
    errorMessage = "The private handoff root cannot be resolved.";
    return false;
  }
  const auto canonicalParent = std::filesystem::canonical(
      kind == PlatformTemporaryPathKind::File ? candidate.parent_path()
                                               : issuedRoot.parent_path(),
      error);
  if (error || !pathIsWithin(canonicalParent, canonicalBase, true)) {
    errorMessage = "Temporary document escaped the private handoff root.";
    return false;
  }

  const auto status = std::filesystem::symlink_status(candidate, error);
  if (allowMissing &&
      (error == std::errc::no_such_file_or_directory ||
       (!error && status.type() == std::filesystem::file_type::not_found))) {
    errorMessage.clear();
    return true;
  }
  const bool expectedType = kind == PlatformTemporaryPathKind::File
                                ? std::filesystem::is_regular_file(status)
                                : std::filesystem::is_directory(status);
  if (error || (!expectedType &&
                !(allowFinalSymlink && std::filesystem::is_symlink(status)))) {
    errorMessage = "Temporary path has an unexpected file type.";
    return false;
  }
  if (!std::filesystem::is_symlink(status) &&
      !verifyOwnerPrivatePath(candidate,
                              kind == PlatformTemporaryPathKind::Directory,
                              errorMessage)) {
    return false;
  }
  errorMessage.clear();
  return true;
}

bool validateDesktopTemporaryDocument(const std::filesystem::path &path,
                                      bool allowMissing, bool allowFinalSymlink,
                                      std::string &errorMessage) {
  return validateDesktopTemporaryPath(path, PlatformTemporaryPathKind::File,
                                      allowMissing, allowFinalSymlink,
                                      errorMessage);
}

bool cleanupDesktopTemporaryDocument(const std::filesystem::path &path,
                                     std::string &errorMessage) {
  if (!validateDesktopTemporaryDocument(path, true, true, errorMessage)) {
    return false;
  }
  std::error_code error;
  if (!std::filesystem::remove(path, error) &&
      error != std::errc::no_such_file_or_directory) {
    errorMessage = "Temporary document cleanup failed: " + error.message();
    return false;
  }

  const auto base = desktopDocumentHandoffBase(error);
  const auto parent = path.parent_path().lexically_normal();
  if (!error && parent.parent_path() == base) {
    std::error_code ignored;
    std::filesystem::remove(parent, ignored);
  }
  errorMessage.clear();
  return true;
}

bool cleanupDesktopTemporaryDirectory(const std::filesystem::path &path,
                                      std::string &errorMessage) {
  if (!validateDesktopTemporaryPath(path, PlatformTemporaryPathKind::Directory,
                                    true, true, errorMessage)) {
    return false;
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
  } else if (error) {
    errorMessage = "Temporary directory cleanup failed: " + error.message();
    return false;
  } else if (std::filesystem::is_symlink(status)) {
    std::filesystem::remove(path, error);
  } else {
    std::filesystem::remove_all(path, error);
  }
  if (error) {
    errorMessage = "Temporary directory cleanup failed: " + error.message();
    return false;
  }
  errorMessage.clear();
  return true;
}

bool validatePlatformTemporaryPath(const std::filesystem::path &path,
                                   PlatformTemporaryPathKind kind,
                                   std::string &errorMessage) {
#if TARGET_OS_ANDROID
  return kind == PlatformTemporaryPathKind::File &&
         ValidateAndroidTemporaryDocument(path, errorMessage);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return kind == PlatformTemporaryPathKind::File
             ? ValidateIOSTemporaryDocument(path, errorMessage)
             : ValidateIOSTemporaryDirectory(path, errorMessage);
#else
  return validateDesktopTemporaryPath(path, kind, false, false, errorMessage);
#endif
}

bool cleanupPlatformTemporaryPath(const std::filesystem::path &path,
                                  PlatformTemporaryPathKind kind) {
  std::string errorMessage;
#if TARGET_OS_ANDROID
  return kind == PlatformTemporaryPathKind::File &&
         CleanupAndroidTemporaryDocument(path, errorMessage);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return kind == PlatformTemporaryPathKind::File
             ? CleanupIOSTemporaryDocument(path, errorMessage)
             : CleanupIOSTemporaryDirectory(path, errorMessage);
#else
  return kind == PlatformTemporaryPathKind::File
             ? cleanupDesktopTemporaryDocument(path, errorMessage)
             : cleanupDesktopTemporaryDirectory(path, errorMessage);
#endif
}

struct TemporaryPathIdentity {
  std::uint64_t device = 0;
  std::uint64_t object = 0;
};

std::optional<TemporaryPathIdentity>
captureTemporaryPathIdentity(const std::filesystem::path &path,
                             PlatformTemporaryPathKind kind) {
#if defined(_WIN32)
  HANDLE handle = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
      nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }
  BY_HANDLE_FILE_INFORMATION information{};
  const bool inspected = GetFileInformationByHandle(handle, &information);
  CloseHandle(handle);
  if (!inspected ||
      (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) !=
          (kind == PlatformTemporaryPathKind::Directory)) {
    return std::nullopt;
  }
  return TemporaryPathIdentity{
      .device = information.dwVolumeSerialNumber,
      .object = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
                information.nFileIndexLow};
#else
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0 ||
      (kind == PlatformTemporaryPathKind::Directory
           ? !S_ISDIR(status.st_mode)
           : !S_ISREG(status.st_mode))) {
    return std::nullopt;
  }
  return TemporaryPathIdentity{
      .device = static_cast<std::uint64_t>(status.st_dev),
      .object = static_cast<std::uint64_t>(status.st_ino)};
#endif
}

enum class TemporaryPathIdentityMatch {
  Missing,
  Exact,
  SafeFinalLink,
  Different,
};

TemporaryPathIdentityMatch matchTemporaryPathIdentity(
    const std::filesystem::path &path, PlatformTemporaryPathKind kind,
    TemporaryPathIdentity expected) {
#if defined(_WIN32)
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? TemporaryPathIdentityMatch::Missing
               : TemporaryPathIdentityMatch::Different;
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return TemporaryPathIdentityMatch::SafeFinalLink;
  }
#else
  struct stat status {};
  if (::lstat(path.c_str(), &status) != 0) {
    return errno == ENOENT ? TemporaryPathIdentityMatch::Missing
                           : TemporaryPathIdentityMatch::Different;
  }
  if (S_ISLNK(status.st_mode)) {
    return TemporaryPathIdentityMatch::SafeFinalLink;
  }
#endif
  const auto actual = captureTemporaryPathIdentity(path, kind);
  return actual && actual->device == expected.device &&
                 actual->object == expected.object
             ? TemporaryPathIdentityMatch::Exact
             : TemporaryPathIdentityMatch::Different;
}

std::shared_ptr<detail::TemporaryDocumentOwnership>
adoptTemporaryPath(const std::filesystem::path &path,
                   PlatformTemporaryPathKind kind) {
  std::string errorMessage;
  if (!validatePlatformTemporaryPath(path, kind, errorMessage)) {
    return {};
  }
  const auto identity = captureTemporaryPathIdentity(path, kind);
  if (!identity) {
    return {};
  }
  return std::make_shared<detail::TemporaryDocumentOwnership>(
      path.lexically_normal(), kind,
      [identity = *identity](const std::filesystem::path &ownedPath,
                             PlatformTemporaryPathKind ownedKind) {
        const auto match =
            matchTemporaryPathIdentity(ownedPath, ownedKind, identity);
        if (match == TemporaryPathIdentityMatch::Different) {
          return false;
        }
#if defined(_WIN32)
        if (match == TemporaryPathIdentityMatch::SafeFinalLink) {
          const DWORD attributes = GetFileAttributesW(ownedPath.c_str());
          return attributes != INVALID_FILE_ATTRIBUTES &&
                 ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0
                      ? RemoveDirectoryW(ownedPath.c_str()) != 0
                      : DeleteFileW(ownedPath.c_str()) != 0);
        }
#endif
        return cleanupPlatformTemporaryPath(ownedPath, ownedKind);
      });
}

std::filesystem::path
makePrivateImportDirectoryUnder(const std::filesystem::path &temporary,
                                std::string &errorMessage) {
  errorMessage.clear();
  std::error_code error;
  const auto applicationRoot = temporary / "AsoBMaShow";
  const auto base = applicationRoot / "document-handoff";
  std::filesystem::create_directory(applicationRoot, error);
  if (error && error != std::errc::file_exists) {
    errorMessage = "Unable to create temporary storage: " + error.message();
    return {};
  }
  error.clear();
  auto status = std::filesystem::symlink_status(applicationRoot, error);
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    errorMessage = "Application temporary storage is not trustworthy.";
    return {};
  }
  std::filesystem::create_directory(base, error);
  if (error && error != std::errc::file_exists) {
    errorMessage = "Unable to create temporary storage: " + error.message();
    return {};
  }
  error.clear();
  status = std::filesystem::symlink_status(base, error);
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    errorMessage = "Private temporary storage is not trustworthy.";
    return {};
  }
  for (const auto &directory : {applicationRoot, base}) {
    if (!secureOwnerPrivatePath(directory, true, errorMessage)) {
      return {};
    }
  }
  for (int attempt = 0; attempt < 64; ++attempt) {
    const auto candidate = base / randomHex();
    if (std::filesystem::create_directory(candidate, error)) {
      if (!secureOwnerPrivatePath(candidate, true, errorMessage)) {
        std::error_code ignored;
        std::filesystem::remove_all(candidate, ignored);
        return {};
      }
      return candidate;
    }
    if (error && error != std::errc::file_exists) {
      errorMessage = "Unable to create temporary storage: " + error.message();
      return {};
    }
    error.clear();
  }
  errorMessage = "Unable to allocate unique temporary storage.";
  return {};
}

std::filesystem::path makePrivateImportDirectory(std::string &errorMessage) {
  std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  if (error) {
    errorMessage = "Unable to locate temporary storage: " + error.message();
    return {};
  }
  return makePrivateImportDirectoryUnder(temporary, errorMessage);
}

std::string stageDesktopImport(const std::filesystem::path &source,
                               std::uint64_t maxBytes,
                               const std::atomic_bool &cancellationRequested) {
  std::string errorMessage;
  const auto directory = makePrivateImportDirectory(errorMessage);
  if (directory.empty()) {
    return std::string(kErrorPrefix) + errorMessage;
  }
  const auto destination = directory / "imported-document.zip";
  const auto cleanup = [&] {
    std::string ignored;
    (void)cleanupDesktopTemporaryDocument(destination, ignored);
  };
  const auto fail = [&](std::string message) {
    cleanup();
    return std::string(kErrorPrefix) + std::move(message);
  };
  const auto cancelled = [&] {
    cleanup();
    return std::string(kCancelled);
  };
  ExclusiveOutputFile output;
  if (output.open(destination, errorMessage) !=
      ExclusiveCreateResult::Opened) {
    return fail(errorMessage.empty()
                    ? "Unable to create the private document copy."
                    : std::move(errorMessage));
  }
  std::uint64_t bytesCopied = 0;
  std::array<char, 64 * 1024> buffer{};
#if defined(_WIN32)
  HANDLE input = CreateFileW(
      source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
          FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  FILE_ATTRIBUTE_TAG_INFO tagInfo{};
  FILE_STANDARD_INFO standardInfo{};
  if (input == INVALID_HANDLE_VALUE ||
      !GetFileInformationByHandleEx(input, FileAttributeTagInfo, &tagInfo,
                                    sizeof(tagInfo)) ||
      !GetFileInformationByHandleEx(input, FileStandardInfo, &standardInfo,
                                    sizeof(standardInfo)) ||
      (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      standardInfo.Directory) {
    if (input != INVALID_HANDLE_VALUE) {
      CloseHandle(input);
    }
    output.abort();
    return fail("The selected document is not a no-follow regular file.");
  }
  while (true) {
    if (cancellationRequested.load(std::memory_order_acquire)) {
      CloseHandle(input);
      output.abort();
      return cancelled();
    }
    DWORD count = 0;
    if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &count, nullptr)) {
      CloseHandle(input);
      output.abort();
      return fail("Unable to read the selected document.");
    }
    if (count == 0) {
      break;
    }
    const auto read = static_cast<std::uint64_t>(count);
    if (bytesCopied > maxBytes || read > maxBytes - bytesCopied ||
        !output.write(buffer.data(), static_cast<std::size_t>(count),
                      errorMessage)) {
      CloseHandle(input);
      output.abort();
      return fail(errorMessage.empty()
                      ? "The selected document exceeds the maximum size."
                      : std::move(errorMessage));
    }
    bytesCopied += read;
  }
  CloseHandle(input);
#else
  const int input =
      ::open(source.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  struct stat before {};
  if (input < 0 || ::fstat(input, &before) != 0 ||
      !S_ISREG(before.st_mode)) {
    if (input >= 0) {
      ::close(input);
    }
    output.abort();
    return fail("The selected document is not a no-follow regular file.");
  }
  while (true) {
    if (cancellationRequested.load(std::memory_order_acquire)) {
      ::close(input);
      output.abort();
      return cancelled();
    }
    const ssize_t count = ::read(input, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      ::close(input);
      output.abort();
      return fail("Unable to read the selected document.");
    }
    if (count == 0) {
      break;
    }
    const auto read = static_cast<std::uint64_t>(count);
    if (bytesCopied > maxBytes || read > maxBytes - bytesCopied ||
        !output.write(buffer.data(), static_cast<std::size_t>(count),
                      errorMessage)) {
      ::close(input);
      output.abort();
      return fail(errorMessage.empty()
                      ? "The selected document exceeds the maximum size."
                      : std::move(errorMessage));
    }
    bytesCopied += read;
  }
  struct stat after {};
  const bool unchanged = ::fstat(input, &after) == 0 &&
                         before.st_dev == after.st_dev &&
                         before.st_ino == after.st_ino &&
                         before.st_size == after.st_size;
  ::close(input);
  if (!unchanged) {
    output.abort();
    return fail("The selected document changed while being copied.");
  }
#endif
  if (cancellationRequested.load(std::memory_order_acquire)) {
    output.abort();
    return cancelled();
  }
  if (!output.finish(errorMessage) ||
      !secureOwnerPrivatePath(destination, false, errorMessage)) {
    return fail(errorMessage.empty()
                    ? "Unable to finish the private import copy."
                    : std::move(errorMessage));
  }
  return detail::PathToUtf8(
      std::filesystem::absolute(destination).lexically_normal());
}

std::string exportDesktopDocument(const PlatformDocumentExportRequest &request,
                                  const std::filesystem::path &destination,
                                  const std::atomic_bool &cancellationRequested,
                                  const detail::CommitGate &commitGate) {
  std::string errorMessage;
  if (!detail::CopyFileForExport(request.localPath, destination,
                                 request.maxBytes, errorMessage,
                                 &cancellationRequested, commitGate)) {
    return std::string(kErrorPrefix) + errorMessage;
  }
  return std::string(kSuccess);
}

PlatformDocumentHandoffResult
importDocument(std::uint64_t operationToken,
               const PlatformDocumentImportRequest &request,
               const std::atomic_bool &cancellationRequested,
               detail::NativeCancellationRegistration &nativeCancellation) {
  if (auto validation = detail::Validate(request); !validation.ok()) {
    return validation;
  }
  if (cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  auto &handoffMutex = documentHandoffMutex();
  if (!detail::LockInterruptibly(handoffMutex, cancellationRequested)) {
    return cancellation();
  }
  std::unique_lock<std::timed_mutex> operationLock(handoffMutex,
                                                   std::adopt_lock);
  if (cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  NativeActivityScope nativeActivity(nativeCancellation);
  if (!nativeActivity.active() ||
      cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  std::string bridgeResult;
  std::string originalSourceName;
#if TARGET_OS_ANDROID
  bridgeResult =
      ImportAndroidDocument(operationToken, request.mimeType, request.maxBytes);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
  bridgeResult = ImportIOSDocument(operationToken, request.mimeType,
                                   request.maxBytes, &cancellationRequested,
                                   &originalSourceName);
#else
  const char *filters[] = {"*.asobprofile", "*.zip"};
  const char *selected = tinyfd_openFileDialog(
      "Import player profile", "", 2, filters, "Player profile archive", 0);
  if (selected == nullptr) {
    bridgeResult = std::string(kCancelled);
  } else if (cancellationRequested.load(std::memory_order_acquire)) {
    bridgeResult = std::string(kCancelled);
  } else {
    const auto source = detail::PathFromUtf8(selected);
    originalSourceName = detail::PathToUtf8(source.filename());
    bridgeResult =
        stageDesktopImport(source, request.maxBytes, cancellationRequested);
  }
#endif
  return detail::ParseBridgeResult(bridgeResult, true, true,
                                   PlatformTemporaryPathKind::File,
                                   std::move(originalSourceName));
}

PlatformDocumentHandoffResult
importDirectory(std::uint64_t operationToken,
                const PlatformDirectoryImportRequest &request,
                const std::atomic_bool &cancellationRequested,
                detail::NativeCancellationRegistration &nativeCancellation) {
  if (auto validation = detail::Validate(request); !validation.ok()) {
    return validation;
  }
  if (cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  auto &handoffMutex = documentHandoffMutex();
  if (!detail::LockInterruptibly(handoffMutex, cancellationRequested)) {
    return cancellation();
  }
  std::unique_lock<std::timed_mutex> operationLock(handoffMutex,
                                                   std::adopt_lock);
  if (cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  NativeActivityScope nativeActivity(nativeCancellation);
  if (!nativeActivity.active() ||
      cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
#if TARGET_OS_ANDROID
  (void)operationToken;
  return failure("Directory import is not supported on Android in this "
                 "release.");
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
  std::string originalSourceName;
  const std::string bridgeResult = ImportIOSDirectory(
      operationToken, request.maxBytes, request.maxFiles, request.maxDepth,
      request.maxPathBytes, &cancellationRequested, &originalSourceName);
  return detail::ParseBridgeResult(bridgeResult, true, true,
                                   PlatformTemporaryPathKind::Directory,
                                   std::move(originalSourceName));
#else
  const char *selected =
      tinyfd_selectFolderDialog("Import gameplay skin folder", "");
  if (selected == nullptr ||
      cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  std::error_code error;
  const auto temporaryRoot = std::filesystem::temp_directory_path(error);
  if (error) {
    return failure("Unable to locate temporary storage: " + error.message());
  }
  return detail::CopyDirectoryForImport(detail::PathFromUtf8(selected), request,
                                        temporaryRoot,
                                        &cancellationRequested);
#endif
}

PlatformDocumentHandoffResult
exportDocument(std::uint64_t operationToken,
               const PlatformDocumentExportRequest &request,
               const std::atomic_bool &cancellationRequested,
               detail::NativeCancellationRegistration &nativeCancellation,
               const detail::CommitGate &commitGate) {
  if (auto validation = detail::Validate(request); !validation.ok()) {
    return validation;
  }
  if (cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  auto &handoffMutex = documentHandoffMutex();
  if (!detail::LockInterruptibly(handoffMutex, cancellationRequested)) {
    return cancellation();
  }
  std::unique_lock<std::timed_mutex> operationLock(handoffMutex,
                                                   std::adopt_lock);
  if (cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  NativeActivityScope nativeActivity(nativeCancellation);
  if (!nativeActivity.active() ||
      cancellationRequested.load(std::memory_order_acquire)) {
    return cancellation();
  }
  std::string bridgeResult;
#if TARGET_OS_ANDROID
  if (!RegisterAndroidDocumentCommit(operationToken, [commitGate] {
        return commitGate && commitGate([] { return true; });
      })) {
    return failure("Android document export commit registration failed.");
  }
  struct AndroidCommitRegistrationReset {
    std::uint64_t operationToken;
    ~AndroidCommitRegistrationReset() {
      UnregisterAndroidDocumentCommit(operationToken);
    }
  } androidCommitRegistrationReset{operationToken};
  bridgeResult =
      ExportAndroidDocument(operationToken, request.localPath, request.mimeType,
                            request.suggestedName, request.maxBytes);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
  bridgeResult = ExportIOSDocument(
      operationToken, request.localPath, request.mimeType,
      request.suggestedName, request.maxBytes, &cancellationRequested,
      [commitGate] { return commitGate && commitGate([] { return true; }); });
#else
  const bool textDocument = request.mimeType == "text/plain";
  const char *profileFilters[] = {"*.asobprofile", "*.zip"};
  const char *textFilters[] = {"*.txt", "*.log"};
  const std::string suggestedName =
      textDocument ? request.suggestedName
                   : detail::PreferredProfileExportName(request.suggestedName);
  const char *selected = tinyfd_saveFileDialog(
      textDocument ? "Export performance log" : "Export player profile",
      suggestedName.c_str(), 2, textDocument ? textFilters : profileFilters,
      textDocument ? "Text document" : "Player profile archive");
  if (selected == nullptr) {
    bridgeResult = std::string(kCancelled);
  } else if (cancellationRequested.load(std::memory_order_acquire)) {
    bridgeResult = std::string(kCancelled);
  } else {
    bridgeResult =
        exportDesktopDocument(request, detail::PathFromUtf8(selected),
                              cancellationRequested, commitGate);
  }
#endif
  return detail::ParseBridgeResult(bridgeResult, false, false);
}
} // namespace

namespace detail {
bool LockInterruptibly(std::timed_mutex &mutex,
                       const std::atomic_bool &cancellationRequested) {
  while (!cancellationRequested.load(std::memory_order_acquire)) {
    if (mutex.try_lock_for(std::chrono::milliseconds(25))) {
      return true;
    }
  }
  return false;
}

std::filesystem::path
CreatePrivateImportDirectoryUnder(const std::filesystem::path &temporaryRoot,
                                  std::string &errorMessage) {
  if (temporaryRoot.empty() || !temporaryRoot.is_absolute()) {
    errorMessage = "Temporary storage root must be absolute.";
    return {};
  }
  return makePrivateImportDirectoryUnder(temporaryRoot.lexically_normal(),
                                         errorMessage);
}

bool SecurePrivateDocumentPath(const std::filesystem::path &path,
                               bool directory, std::string &errorMessage) {
  return secureOwnerPrivatePath(path, directory, errorMessage);
}

PreparedTextDocumentExport
PrepareTextDocumentExportUnder(const PlatformTextDocumentExportRequest &request,
                               const std::filesystem::path &temporaryRoot) {
  PreparedTextDocumentExport prepared;
  if (auto validation = Validate(PlatformDocumentImportRequest{
          .mimeType = "text/plain", .maxBytes = request.maxBytes});
      !validation.ok()) {
    prepared.errorMessage = std::move(validation.message);
    return prepared;
  }
  if (!validSuggestedName(request.suggestedName)) {
    prepared.errorMessage =
        "The suggested export file name must be a single file name.";
    return prepared;
  }
  if (request.text.size() > request.maxBytes) {
    prepared.errorMessage =
        "The text document exceeds the maximum document size.";
    return prepared;
  }

  std::string errorMessage;
  const auto directory =
      CreatePrivateImportDirectoryUnder(temporaryRoot, errorMessage);
  if (directory.empty()) {
    prepared.errorMessage = std::move(errorMessage);
    return prepared;
  }
  const auto path = directory / "export-document.txt";
  auto ownership =
      std::make_shared<StagedTextDocumentOwnership>(path, directory);
  ExclusiveOutputFile output;
  if (output.open(path, errorMessage) != ExclusiveCreateResult::Opened ||
      !output.write(request.text.data(), request.text.size(), errorMessage) ||
      !output.finish(errorMessage) ||
      !secureOwnerPrivatePath(path, false, errorMessage)) {
    prepared.errorMessage = errorMessage.empty()
                                ? "Unable to stage the text document."
                                : std::move(errorMessage);
    return prepared;
  }

  prepared.request = {
      .localPath = path,
      .mimeType = "text/plain",
      .suggestedName = request.suggestedName,
      .maxBytes = request.maxBytes,
      .sourceLifetime = std::move(ownership),
  };
  return prepared;
}

std::string PreferredProfileExportName(const std::string &suggestedName) {
  const std::filesystem::path name = PathFromUtf8(suggestedName);
  return name.has_extension() ? suggestedName : suggestedName + ".asobprofile";
}

std::filesystem::path PathFromUtf8(std::string_view value) {
  std::u8string utf8;
  utf8.reserve(value.size());
  for (const unsigned char byte : value) {
    utf8.push_back(static_cast<char8_t>(byte));
  }
  return std::filesystem::path(utf8);
}

std::string PathToUtf8(const std::filesystem::path &path) {
  const auto value = path.u8string();
  std::string utf8;
  utf8.reserve(value.size());
  for (const char8_t byte : value) {
    utf8.push_back(static_cast<char>(byte));
  }
  return utf8;
}

PlatformDocumentHandoffResult
Validate(const PlatformDocumentImportRequest &request) {
  if (!validMimeType(request.mimeType)) {
    return failure("A valid document MIME type is required.");
  }
  if (request.maxBytes == 0 ||
      request.maxBytes > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
    return failure("A supported non-zero maximum document size is required.");
  }
  return success();
}

PlatformDocumentHandoffResult
Validate(const PlatformDirectoryImportRequest &request) {
  if (request.maxBytes == 0 || request.maxFiles == 0 || request.maxDepth == 0 ||
      request.maxPathBytes == 0 ||
      request.maxBytes > static_cast<std::uint64_t>(
                             std::numeric_limits<std::int64_t>::max())) {
    return failure("Directory import requires non-zero supported byte, file, "
                   "depth, and path limits.");
  }
  return success();
}

PlatformDocumentHandoffResult CopyDirectoryForImport(
    const std::filesystem::path &source,
    const PlatformDirectoryImportRequest &request,
    const std::filesystem::path &temporaryRoot,
    const std::atomic_bool *cancellationRequested,
    const std::function<void(std::uint64_t)> &progress) {
  if (auto validation = Validate(request); !validation.ok()) {
    return validation;
  }
  const auto cancelled = [&] {
    return cancellationRequested != nullptr &&
           cancellationRequested->load(std::memory_order_acquire);
  };
  if (cancelled()) {
    return cancellation();
  }

  std::error_code error;
  const auto sourceStatus = std::filesystem::symlink_status(source, error);
  if (error || std::filesystem::is_symlink(sourceStatus) ||
      !std::filesystem::is_directory(sourceStatus)) {
    return failure("The selected folder is not a regular directory.");
  }

  std::string errorMessage;
  const auto destinationRoot =
      CreatePrivateImportDirectoryUnder(temporaryRoot, errorMessage);
  if (destinationRoot.empty()) {
    return failure(errorMessage.empty() ? "Unable to allocate private folder "
                                        "storage."
                                        : std::move(errorMessage));
  }
  auto destinationOwnership =
      adoptTemporaryPath(destinationRoot, PlatformTemporaryPathKind::Directory);
  if (destinationOwnership == nullptr) {
    return failure("Unable to issue private folder cleanup ownership.");
  }
  const auto discard = [&] {
    (void)destinationOwnership->cleanup();
  };
  const auto failCopy = [&](std::string message) {
    discard();
    return failure(std::move(message));
  };
  const auto cancelledCopy = [&] {
    discard();
    return cancellation();
  };

  std::uint64_t copiedBytes = 0;
  std::uint64_t copiedFiles = 0;
  std::filesystem::recursive_directory_iterator iterator(
      source, std::filesystem::directory_options::none, error);
  const std::filesystem::recursive_directory_iterator end;
  while (!error && iterator != end) {
    if (cancelled()) {
      return cancelledCopy();
    }
    const auto entryPath = iterator->path();
    const auto relative = entryPath.lexically_relative(source);
    if (relative.empty() || relative.is_absolute()) {
      return failCopy("The selected folder contains an invalid path.");
    }
    const auto relativeUtf8 = PathToUtf8(relative);
    if (relativeUtf8.empty() ||
        relativeUtf8.size() > request.maxPathBytes ||
        relativeUtf8.find('\0') != std::string::npos) {
      return failCopy("The selected folder contains a path beyond its limit.");
    }
    std::uint32_t depth = 0;
    for (const auto &component : relative) {
      if (component != ".") {
        ++depth;
      }
    }
    if (depth == 0 || depth > request.maxDepth) {
      return failCopy("The selected folder exceeds its depth limit.");
    }

    const auto status = std::filesystem::symlink_status(entryPath, error);
    if (error || std::filesystem::is_symlink(status)) {
      return failCopy("The selected folder contains a symbolic link.");
    }
#if defined(_WIN32)
    const DWORD entryAttributes = GetFileAttributesW(entryPath.c_str());
    if (entryAttributes == INVALID_FILE_ATTRIBUTES ||
        (entryAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      return failCopy("The selected folder contains a reparse-point entry.");
    }
#endif
    const auto destination = destinationRoot / relative;
    if (std::filesystem::is_directory(status)) {
      std::filesystem::create_directory(destination, error);
      if (error || !secureOwnerPrivatePath(destination, true, errorMessage)) {
        return failCopy(errorMessage.empty()
                            ? "Unable to create private folder storage."
                            : std::move(errorMessage));
      }
    } else if (std::filesystem::is_regular_file(status)) {
      if (++copiedFiles > request.maxFiles) {
        return failCopy("The selected folder exceeds its file limit.");
      }
      ExclusiveOutputFile output;
      const auto outputOpen = output.open(destination, errorMessage);
      if (outputOpen != ExclusiveCreateResult::Opened) {
        return failCopy(errorMessage.empty()
                            ? "Unable to create a private folder copy."
                            : std::move(errorMessage));
      }

#if defined(_WIN32)
      HANDLE input = CreateFileW(
          entryPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
          OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
              FILE_FLAG_SEQUENTIAL_SCAN,
          nullptr);
      FILE_ATTRIBUTE_TAG_INFO tagInfo{};
      FILE_STANDARD_INFO standardInfo{};
      if (input == INVALID_HANDLE_VALUE ||
          !GetFileInformationByHandleEx(input, FileAttributeTagInfo, &tagInfo,
                                        sizeof(tagInfo)) ||
          !GetFileInformationByHandleEx(input, FileStandardInfo,
                                        &standardInfo,
                                        sizeof(standardInfo)) ||
          (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
          standardInfo.Directory) {
        if (input != INVALID_HANDLE_VALUE) {
          CloseHandle(input);
        }
        output.abort();
        return failCopy("The selected folder contains a reparse-point file.");
      }
      std::array<char, 64 * 1024> buffer{};
      while (true) {
        if (cancelled()) {
          CloseHandle(input);
          output.abort();
          return cancelledCopy();
        }
        DWORD count = 0;
        if (!ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &count, nullptr)) {
          CloseHandle(input);
          output.abort();
          return failCopy("Unable to read a selected folder file.");
        }
        if (count == 0) {
          break;
        }
        const auto bytes = static_cast<std::uint64_t>(count);
        if (bytes > request.maxBytes - copiedBytes ||
            !output.write(buffer.data(), static_cast<std::size_t>(count),
                          errorMessage)) {
          CloseHandle(input);
          output.abort();
          return failCopy(errorMessage.empty()
                              ? "The selected folder exceeds its byte limit."
                              : std::move(errorMessage));
        }
        copiedBytes += bytes;
        if (progress) {
          progress(copiedBytes);
        }
      }
      CloseHandle(input);
#else
      int descriptor = ::open(entryPath.c_str(), O_RDONLY | O_NONBLOCK |
                                                     O_NOFOLLOW | O_CLOEXEC);
      struct stat before {};
      if (descriptor < 0 || ::fstat(descriptor, &before) != 0 ||
          !S_ISREG(before.st_mode)) {
        if (descriptor >= 0) {
          ::close(descriptor);
        }
        output.abort();
        return failCopy("The selected folder contains a non-regular file.");
      }
      std::array<char, 64 * 1024> buffer{};
      while (true) {
        if (cancelled()) {
          ::close(descriptor);
          output.abort();
          return cancelledCopy();
        }
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) {
          continue;
        }
        if (count < 0) {
          ::close(descriptor);
          output.abort();
          return failCopy("Unable to read a selected folder file.");
        }
        if (count == 0) {
          break;
        }
        const auto bytes = static_cast<std::uint64_t>(count);
        if (bytes > request.maxBytes - copiedBytes ||
            !output.write(buffer.data(), static_cast<std::size_t>(count),
                          errorMessage)) {
          ::close(descriptor);
          output.abort();
          return failCopy(errorMessage.empty()
                              ? "The selected folder exceeds its byte limit."
                              : std::move(errorMessage));
        }
        copiedBytes += bytes;
        if (progress) {
          progress(copiedBytes);
        }
      }
      struct stat after {};
      const bool unchanged = ::fstat(descriptor, &after) == 0 &&
                             before.st_dev == after.st_dev &&
                             before.st_ino == after.st_ino;
      ::close(descriptor);
      if (!unchanged) {
        output.abort();
        return failCopy("A selected folder file changed while being copied.");
      }
#endif
      if (cancelled()) {
        output.abort();
        return cancelledCopy();
      }
      if (!output.finish(errorMessage) ||
          !secureOwnerPrivatePath(destination, false, errorMessage)) {
        return failCopy(errorMessage.empty()
                            ? "Unable to finish a private folder copy."
                            : std::move(errorMessage));
      }
    } else {
      return failCopy("The selected folder contains a non-regular entry.");
    }
    iterator.increment(error);
  }
  if (error) {
    return failCopy("Unable to enumerate the selected folder: " +
                    error.message());
  }
  if (cancelled()) {
    return cancelledCopy();
  }
  return ParseBridgeResult(PathToUtf8(destinationRoot), true, true,
                           PlatformTemporaryPathKind::Directory,
                           PathToUtf8(source.filename()));
}

PlatformDocumentHandoffResult
Validate(const PlatformDocumentExportRequest &request) {
  PlatformDocumentImportRequest common{.mimeType = request.mimeType,
                                       .maxBytes = request.maxBytes};
  if (auto validation = Validate(common); !validation.ok()) {
    return validation;
  }
  if (!validSuggestedName(request.suggestedName)) {
    return failure(
        "The suggested export file name must be a single file name.");
  }
  if (request.localPath.empty() || !request.localPath.is_absolute()) {
    return failure("The export source must be an absolute local path.");
  }
  std::error_code error;
  if (!std::filesystem::is_regular_file(request.localPath, error) || error) {
    return failure("The export source is not a readable regular file.");
  }
  const auto size = std::filesystem::file_size(request.localPath, error);
  if (error) {
    return failure("Unable to determine the export source size: " +
                   error.message());
  }
  if (size > request.maxBytes) {
    return failure("The export source exceeds the maximum document size.");
  }
  return success();
}

PlatformDocumentHandoffResult ParseBridgeResult(const std::string &value,
                                                bool expectsLocalPath,
                                                bool temporaryLocalFile,
                                                PlatformTemporaryPathKind
                                                    temporaryPathKind,
                                                std::string originalSourceName,
                                                SourceNameNormalizer
                                                    sourceNameNormalizer) {
  if (value == kCancelled) {
    return {.status = PlatformDocumentHandoffStatus::Cancelled};
  }
  if (value.starts_with(kErrorPrefix)) {
    std::string message(value.substr(kErrorPrefix.size()));
    if (message.empty()) {
      message = "The platform document operation failed.";
    }
    return failure(std::move(message));
  }
  if (expectsLocalPath) {
    const std::filesystem::path path = PathFromUtf8(value);
    if (value.empty() || !path.is_absolute()) {
      return failure("The platform returned an invalid local document path.");
    }
    auto result = PlatformDocumentHandoffResult{
        .status = PlatformDocumentHandoffStatus::Succeeded,
        .localPath = path.lexically_normal(),
        .originalSourceName = {},
        .temporaryLocalFile = temporaryLocalFile &&
                              temporaryPathKind ==
                                  PlatformTemporaryPathKind::File,
        .temporaryPathKind = temporaryPathKind};
    if (temporaryLocalFile) {
      result.temporaryOwnership =
          adoptTemporaryPath(result.localPath, temporaryPathKind);
      if (result.temporaryOwnership == nullptr) {
        return failure("The platform returned a document outside private "
                       "handoff storage.");
      }
    }
    if (!originalSourceName.empty()) {
      try {
        if (!sourceNameNormalizer) {
          sourceNameNormalizer = [](std::string_view sourceName)
              -> std::optional<std::string> {
            auto normalized = skin::normalizeSkinSourceNameNfc(sourceName);
            return std::move(normalized.value);
          };
        }
        auto normalized = sourceNameNormalizer(originalSourceName);
        if (!normalized) {
          if (result.temporaryOwnership != nullptr) {
            (void)result.temporaryOwnership->cleanup();
          }
          return failure("The selected source name is invalid.");
        }
        result.originalSourceName = std::move(*normalized);
      } catch (...) {
        if (result.temporaryOwnership != nullptr) {
          (void)result.temporaryOwnership->cleanup();
        }
        return failure("The selected source name is invalid.");
      }
    }
    return result;
  }
  if (value != kSuccess) {
    return failure("The platform returned an invalid document result.");
  }
  return success();
}

bool CopyStreamBounded(std::istream &input, std::ostream &output,
                       std::uint64_t maxBytes, std::uint64_t &bytesCopied,
                       std::string &errorMessage,
                       const std::atomic_bool *cancellationRequested,
                       const std::function<void(std::uint64_t)> &progress) {
  bytesCopied = 0;
  errorMessage.clear();
  if (maxBytes == 0) {
    errorMessage = "A non-zero maximum document size is required.";
    return false;
  }
  std::array<char, 64 * 1024> buffer{};
  while (true) {
    if (cancellationRequested != nullptr &&
        cancellationRequested->load(std::memory_order_acquire)) {
      errorMessage = "The document operation was cancelled.";
      return false;
    }
    const auto remaining = maxBytes - bytesCopied;
    const auto requestedBytes =
        remaining < buffer.size() ? remaining + 1 : buffer.size();
    const auto requested = static_cast<std::streamsize>(requestedBytes);
    input.read(buffer.data(), requested);
    const std::streamsize count = input.gcount();
    if (count > 0) {
      const auto unsignedCount = static_cast<std::uint64_t>(count);
      if (unsignedCount > remaining) {
        errorMessage = "The document exceeds the maximum document size.";
        return false;
      }
      output.write(buffer.data(), count);
      if (!output) {
        errorMessage = "Writing the document failed.";
        return false;
      }
      bytesCopied += unsignedCount;
      if (progress) {
        progress(bytesCopied);
      }
    }
    if (cancellationRequested != nullptr &&
        cancellationRequested->load(std::memory_order_acquire)) {
      errorMessage = "The document operation was cancelled.";
      return false;
    }
    if (input.bad()) {
      errorMessage = "Reading the document failed.";
      return false;
    }
    if (input.eof()) {
      return true;
    }
    if (count == 0) {
      errorMessage = "Reading the document made no progress.";
      return false;
    }
  }
}

bool CopyFileForExport(const std::filesystem::path &source,
                       const std::filesystem::path &destination,
                       std::uint64_t maxBytes, std::string &errorMessage,
                       const std::atomic_bool *cancellationRequested,
                       const CommitGate &commitGate,
                       const std::function<void(std::uint64_t)> &progress,
                       const std::filesystem::path &firstTemporaryCandidate) {
  errorMessage.clear();
  std::error_code equivalentError;
  if (std::filesystem::equivalent(source, destination, equivalentError) &&
      !equivalentError) {
    return true;
  }

  std::ifstream input(source, std::ios::binary);
  if (!input.is_open()) {
    errorMessage = "Unable to open the export source.";
    return false;
  }

  const auto parent = destination.has_parent_path()
                          ? destination.parent_path()
                          : std::filesystem::current_path();
  std::filesystem::path temporary;
  ExclusiveOutputFile output;
  for (int attempt = 0; attempt < 64; ++attempt) {
    std::filesystem::path candidate;
    if (attempt == 0 && !firstTemporaryCandidate.empty() &&
        firstTemporaryCandidate.parent_path().lexically_normal() ==
            parent.lexically_normal()) {
      candidate = firstTemporaryCandidate;
    } else {
      auto temporaryName = destination.filename().native();
      const std::string suffix = ".asobmashow-" + randomHex() + ".tmp";
      temporaryName.insert(temporaryName.begin(),
                           static_cast<std::filesystem::path::value_type>('.'));
      for (const char value : suffix) {
        temporaryName.push_back(
            static_cast<std::filesystem::path::value_type>(value));
      }
      candidate = parent / std::filesystem::path(temporaryName);
    }
    const auto createResult = output.open(candidate, errorMessage);
    if (createResult == ExclusiveCreateResult::AlreadyExists) {
      continue;
    }
    if (createResult == ExclusiveCreateResult::Failed) {
      return false;
    }
    temporary = candidate;
    break;
  }
  if (temporary.empty()) {
    errorMessage = "Unable to create a temporary export file.";
    return false;
  }

  const auto removeTemporary = [&temporary] {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
  };
  std::uint64_t bytesCopied = 0;
  if (!copyStreamToExclusiveFile(input, output, maxBytes, bytesCopied,
                                 errorMessage, cancellationRequested,
                                 progress)) {
    input.close();
    output.abort();
    removeTemporary();
    return false;
  }
  if (!output.finish(errorMessage)) {
    input.close();
    removeTemporary();
    return false;
  }

  if (cancellationRequested != nullptr &&
      cancellationRequested->load(std::memory_order_acquire)) {
    removeTemporary();
    errorMessage = "The document operation was cancelled.";
    return false;
  }

  const auto publish = [&]() {
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      const auto code = static_cast<int>(GetLastError());
      errorMessage = "Unable to publish the exported document (Windows error " +
                     std::to_string(code) + ").";
      return false;
    }
#else
    std::error_code publishError;
    std::filesystem::rename(temporary, destination, publishError);
    if (publishError) {
      errorMessage =
          "Unable to publish the exported document: " + publishError.message();
      return false;
    }
#endif
    return true;
  };

  const bool published = commitGate ? commitGate(publish) : publish();
  if (!published) {
    removeTemporary();
    if (errorMessage.empty()) {
      errorMessage = "The document operation was cancelled before publishing.";
    }
    return false;
  }
  return true;
}
} // namespace detail

PlatformDocumentHandoffOperation::PlatformDocumentHandoffOperation(
    std::shared_ptr<detail::OperationState> state)
    : state_(std::move(state)) {}

PlatformDocumentHandoffOperation::PlatformDocumentHandoffOperation(
    PlatformDocumentHandoffOperation &&other) noexcept
    : state_(std::move(other.state_)) {}

PlatformDocumentHandoffOperation &PlatformDocumentHandoffOperation::operator=(
    PlatformDocumentHandoffOperation &&other) noexcept {
  if (this != &other) {
    close();
    state_ = std::move(other.state_);
  }
  return *this;
}

PlatformDocumentHandoffOperation::~PlatformDocumentHandoffOperation() {
  close();
}

bool PlatformDocumentHandoffOperation::ready() const noexcept {
  return state_ != nullptr && state_->ready();
}

std::optional<PlatformDocumentHandoffResult>
PlatformDocumentHandoffOperation::takeResult() {
  return state_ != nullptr ? state_->takeResult() : std::nullopt;
}

void PlatformDocumentHandoffOperation::cancel() noexcept {
  if (state_ != nullptr) {
    state_->cancel();
  }
}

void PlatformDocumentHandoffOperation::abandon() noexcept {
  if (state_ != nullptr) {
    state_->abandon();
    state_.reset();
  }
}

void PlatformDocumentHandoffOperation::close() noexcept {
  abandon();
}

namespace {
bool cleanupTemporaryPathNow(PlatformDocumentHandoffResult &result) noexcept {
  if (result.localPath.empty() && result.temporaryOwnership == nullptr) {
    return true;
  }
  if (result.localPath.empty() || result.temporaryOwnership == nullptr ||
      result.temporaryOwnership->path() !=
          result.localPath.lexically_normal() ||
      result.temporaryOwnership->kind() != result.temporaryPathKind) {
    return false;
  }
  if (!result.temporaryOwnership->cleanup()) {
    return false;
  }
  result.temporaryOwnership.reset();
  result.temporaryLocalFile = false;
  result.localPath.clear();
  return true;
}

} // namespace

struct PlatformTemporaryPathCleanupService::State {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<PlatformDocumentHandoffResult> queued;
  std::vector<PlatformDocumentHandoffResult> unprocessed;
  CleanupTask cleanupTask;
  bool accepting = true;
  bool stopping = false;
};

PlatformTemporaryPathCleanupService::PlatformTemporaryPathCleanupService(
    CleanupTask cleanupTask)
    : state_(std::make_shared<State>()) {
  state_->cleanupTask = cleanupTask ? std::move(cleanupTask)
                                    : CleanupTask{cleanupTemporaryPathNow};
  worker_ = std::thread([state = state_] {
    while (true) {
      PlatformDocumentHandoffResult result;
      {
        std::unique_lock lock(state->mutex);
        state->ready.wait(lock,
                          [&] { return state->stopping || !state->queued.empty(); });
        if (state->queued.empty()) {
          if (state->stopping) {
            return;
          }
          continue;
        }
        result = std::move(state->queued.front());
        state->queued.pop_front();
      }
      bool cleaned = false;
      try {
        cleaned = state->cleanupTask(result);
      } catch (...) {
        cleaned = false;
      }
      if (!cleaned) {
        std::lock_guard lock(state->mutex);
        state->unprocessed.push_back(std::move(result));
      }
    }
  });
}

PlatformTemporaryPathCleanupService::~PlatformTemporaryPathCleanupService() {
  shutdown();
}

bool PlatformTemporaryPathCleanupService::schedule(
    PlatformDocumentHandoffResult &result) noexcept {
  try {
    {
      std::lock_guard lock(state_->mutex);
      if (!state_->accepting) {
        state_->unprocessed.push_back(std::move(result));
        result = {};
        return false;
      }
      state_->queued.push_back(std::move(result));
    }
    result = {};
    state_->ready.notify_one();
    return true;
  } catch (...) {
    return false;
  }
}

void PlatformTemporaryPathCleanupService::shutdown() noexcept {
  std::lock_guard shutdownLock(shutdownMutex_);
  if (state_ == nullptr) {
    return;
  }
  {
    std::lock_guard lock(state_->mutex);
    state_->accepting = false;
    state_->stopping = true;
  }
  state_->ready.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
}

std::vector<PlatformDocumentHandoffResult>
PlatformTemporaryPathCleanupService::takeUnprocessed() {
  std::lock_guard lock(state_->mutex);
  return std::move(state_->unprocessed);
}

PlatformTemporaryPathCleanupServiceHandle
CreatePlatformTemporaryPathCleanupService(
    PlatformTemporaryPathCleanupService::CleanupTask cleanupTask) {
  return std::make_shared<PlatformTemporaryPathCleanupService>(
      std::move(cleanupTask));
}

PlatformTemporaryPathCleanupServiceHandle
DefaultPlatformTemporaryPathCleanupService() {
  static const auto service = CreatePlatformTemporaryPathCleanupService();
  return service;
}

bool CleanupTemporaryPath(PlatformDocumentHandoffResult &result) noexcept {
  return cleanupTemporaryPathNow(result);
}

bool CleanupTemporaryDocument(PlatformDocumentHandoffResult &result) noexcept {
  return result.temporaryPathKind == PlatformTemporaryPathKind::File &&
         CleanupTemporaryPath(result);
}

namespace detail {
std::uint64_t NextOperationToken() {
  return NextOperationToken(nextDocumentHandoffToken());
}

std::uint64_t NextOperationToken(std::atomic_uint64_t &counter) {
  auto current = counter.load(std::memory_order_relaxed);
  while (true) {
    if (current == 0) {
      throw std::overflow_error("Document handoff operation tokens exhausted.");
    }
    const auto next =
        current == std::numeric_limits<std::uint64_t>::max() ? 0 : current + 1;
    if (counter.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return current;
    }
  }
}

PlatformDocumentHandoffOperation
StartOperation(OperationWork work, std::function<void()> cancelNative,
               PlatformTemporaryPathCleanupServiceHandle cleanupService) {
  return StartOperationWithCommit(
      [work = std::move(work)](const std::atomic_bool &cancellationRequested,
                               const CommitGate &) {
        return work(cancellationRequested);
      },
      std::move(cancelNative), {}, std::move(cleanupService));
}

PlatformDocumentHandoffOperation
StartOperationWithCommit(CommitOperationWork work,
                         std::function<void()> cancelNative,
                         std::shared_ptr<void> workerLifetime,
                         PlatformTemporaryPathCleanupServiceHandle
                             cleanupService) {
  auto state = std::make_shared<OperationState>(std::move(cleanupService));
  PlatformDocumentHandoffOperation operation(state);
  std::thread([state, work = std::move(work),
               cancelNative = std::move(cancelNative),
               workerLifetime = std::move(workerLifetime)]() mutable {
    // This opaque owner intentionally outlives terminal result publication.
    // cancel()/close() are nonblocking, so only actual worker exit proves the
    // export source can no longer be read.
    (void)workerLifetime;
    if (!state->installCancelHandler(std::move(cancelNative))) {
      return;
    }
    try {
      const CommitGate commitGate =
          [state](const std::function<bool()> &action) {
            return state->runCommit(action);
          };
      state->complete(work(state->cancellationRequested(), commitGate));
    } catch (const std::exception &error) {
      state->complete(
          failure(std::string("Document handoff failed: ") + error.what()));
    } catch (...) {
      state->complete(failure("Document handoff failed unexpectedly."));
    }
  }).detach();
  return operation;
}
} // namespace detail

namespace {
bool registerNativeDocumentToken(std::uint64_t operationToken,
                                 std::string &errorMessage) {
#if TARGET_OS_ANDROID
  return RegisterAndroidDocumentHandoff(operationToken, errorMessage);
#else
  (void)operationToken;
  errorMessage.clear();
  return true;
#endif
}

class NativeDocumentTokenLease {
public:
  explicit NativeDocumentTokenLease(std::uint64_t operationToken)
      : operationToken_(operationToken) {}
  ~NativeDocumentTokenLease() {
#if TARGET_OS_ANDROID
    RetireAndroidDocumentHandoff(operationToken_);
#endif
  }

private:
  std::uint64_t operationToken_;
};

void cancelNativeDocumentHandoff(std::uint64_t operationToken) {
#if TARGET_OS_ANDROID
  CancelAndroidDocument(operationToken);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
  CancelIOSDocument(operationToken);
#else
  (void)operationToken;
#endif
}

} // namespace

PlatformDocumentHandoffOperation
ImportDocumentAsync(PlatformDocumentImportRequest request) {
  return ImportDocumentAsync(std::move(request), {});
}

PlatformDocumentHandoffOperation
ImportDocumentAsync(PlatformDocumentImportRequest request,
                    PlatformTemporaryPathCleanupServiceHandle cleanupService) {
  const auto operationToken = detail::NextOperationToken();
  std::string registrationError;
  if (!registerNativeDocumentToken(operationToken, registrationError)) {
    return detail::StartOperation(
        [message = std::move(registrationError)](const std::atomic_bool &) {
          return failure(message.empty()
                             ? "Document handoff registration failed."
                             : message);
        },
        [] {}, std::move(cleanupService));
  }
  auto tokenLease = std::make_shared<NativeDocumentTokenLease>(operationToken);
  auto nativeCancellation =
      std::make_shared<detail::NativeCancellationRegistration>(
          [operationToken] { cancelNativeDocumentHandoff(operationToken); });
  return detail::StartOperation(
      [operationToken, request = std::move(request), nativeCancellation,
       tokenLease](const std::atomic_bool &cancellationRequested) {
        (void)tokenLease;
        try {
          return importDocument(operationToken, request, cancellationRequested,
                                *nativeCancellation);
        } catch (const std::exception &error) {
          return failure(std::string("Document import failed: ") +
                         error.what());
        } catch (...) {
          return failure("Document import failed unexpectedly.");
        }
      },
      [nativeCancellation] { nativeCancellation->cancel(); },
      std::move(cleanupService));
}

PlatformDocumentHandoffOperation
ImportDirectoryAsync(PlatformDirectoryImportRequest request,
                     PlatformTemporaryPathCleanupServiceHandle cleanupService) {
  const auto operationToken = detail::NextOperationToken();
  std::string registrationError;
  if (!registerNativeDocumentToken(operationToken, registrationError)) {
    return detail::StartOperation(
        [message = std::move(registrationError)](const std::atomic_bool &) {
          return failure(message.empty()
                             ? "Document handoff registration failed."
                             : message);
        },
        [] {}, std::move(cleanupService));
  }
  auto tokenLease = std::make_shared<NativeDocumentTokenLease>(operationToken);
  auto nativeCancellation =
      std::make_shared<detail::NativeCancellationRegistration>(
          [operationToken] { cancelNativeDocumentHandoff(operationToken); });
  return detail::StartOperation(
      [operationToken, request = std::move(request), nativeCancellation,
       tokenLease](const std::atomic_bool &cancellationRequested) {
        (void)tokenLease;
        try {
          return importDirectory(operationToken, request, cancellationRequested,
                                 *nativeCancellation);
        } catch (const std::exception &error) {
          return failure(std::string("Directory import failed: ") +
                         error.what());
        } catch (...) {
          return failure("Directory import failed unexpectedly.");
        }
      },
      [nativeCancellation] { nativeCancellation->cancel(); },
      std::move(cleanupService));
}

PlatformDocumentHandoffOperation
ExportDocumentAsync(PlatformDocumentExportRequest request) {
  const auto operationToken = detail::NextOperationToken();
  auto sourceLifetime = std::move(request.sourceLifetime);
  std::string registrationError;
  if (!registerNativeDocumentToken(operationToken, registrationError)) {
    return detail::StartOperationWithCommit(
        [message = std::move(registrationError)](const std::atomic_bool &,
                                                 const detail::CommitGate &) {
          return failure(message.empty()
                             ? "Document handoff registration failed."
                             : message);
        },
        [] {}, std::move(sourceLifetime));
  }
  auto tokenLease = std::make_shared<NativeDocumentTokenLease>(operationToken);
  auto nativeCancellation =
      std::make_shared<detail::NativeCancellationRegistration>(
          [operationToken] { cancelNativeDocumentHandoff(operationToken); });
  return detail::StartOperationWithCommit(
      [operationToken, request = std::move(request), nativeCancellation,
       tokenLease](const std::atomic_bool &cancellationRequested,
                   const detail::CommitGate &commitGate) {
        (void)tokenLease;
        try {
          return exportDocument(operationToken, request, cancellationRequested,
                                *nativeCancellation, commitGate);
        } catch (const std::exception &error) {
          return failure(std::string("Document export failed: ") +
                         error.what());
        } catch (...) {
          return failure("Document export failed unexpectedly.");
        }
      },
      [nativeCancellation] { nativeCancellation->cancel(); },
      std::move(sourceLifetime));
}

PlatformDocumentHandoffOperation
ExportTextDocumentAsync(PlatformTextDocumentExportRequest request) {
  std::filesystem::path temporaryRoot;
  std::string errorMessage;
#if TARGET_OS_ANDROID
  const auto cacheDirectory = GetAndroidCacheDir();
  if (cacheDirectory.empty()) {
    errorMessage = "Unable to locate application temporary storage.";
  } else {
    temporaryRoot = detail::PathFromUtf8(cacheDirectory);
  }
#else
  std::error_code error;
  temporaryRoot = std::filesystem::temp_directory_path(error);
  if (error) {
    errorMessage = "Unable to locate temporary storage: " + error.message();
  }
#endif
  detail::PreparedTextDocumentExport prepared;
  if (errorMessage.empty()) {
    prepared = detail::PrepareTextDocumentExportUnder(request, temporaryRoot);
    errorMessage = std::move(prepared.errorMessage);
  }
  if (!errorMessage.empty()) {
    return detail::StartOperation(
        [message = std::move(errorMessage)](const std::atomic_bool &) {
          return failure(message);
        },
        [] {});
  }
  return ExportDocumentAsync(std::move(prepared.request));
}
} // namespace platform_document_handoff
