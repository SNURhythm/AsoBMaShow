#include "ProfileExportStaging.h"

#include <array>
#include <cstdio>
#include <exception>
#include <mutex>
#include <random>
#include <set>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include "PlatformDocumentHandoff.h"
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace profile_export_staging {
namespace {

void report(const WarningReporter &reporter, std::string message) noexcept {
  if (!reporter || message.empty()) {
    return;
  }
  try {
    reporter(message);
  } catch (...) {
  }
}

void reportLateCleanup(std::string_view message) noexcept {
  if (!message.empty()) {
    std::fprintf(stderr, "AsoBMaShow: %.*s\n", static_cast<int>(message.size()),
                 message.data());
  }
}

std::mutex &activeIdentitiesMutex() {
  static std::mutex mutex;
  return mutex;
}

std::set<std::string> &activeIdentities() {
  static std::set<std::string> identities;
  return identities;
}

bool registerActiveIdentity(const std::string &identity) noexcept {
  try {
    std::lock_guard lock(activeIdentitiesMutex());
    activeIdentities().insert(identity);
    return true;
  } catch (...) {
    return false;
  }
}

void unregisterActiveIdentity(const std::string &identity) noexcept {
  try {
    std::lock_guard lock(activeIdentitiesMutex());
    activeIdentities().erase(identity);
  } catch (...) {
  }
}

bool isActiveIdentity(const std::string &identity) noexcept {
  try {
    std::lock_guard lock(activeIdentitiesMutex());
    return activeIdentities().contains(identity);
  } catch (...) {
    return true;
  }
}

bool pathStartsWith(const std::filesystem::path &candidate,
                    const std::filesystem::path &root) {
  auto candidatePart = candidate.begin();
  for (auto rootPart = root.begin(); rootPart != root.end();
       ++rootPart, ++candidatePart) {
    if (candidatePart == candidate.end()) {
      return false;
    }
#if defined(_WIN32)
    const auto candidateText = candidatePart->wstring();
    const auto rootText = rootPart->wstring();
    if (CompareStringOrdinal(
            candidateText.c_str(), static_cast<int>(candidateText.size()),
            rootText.c_str(), static_cast<int>(rootText.size()),
            TRUE) != CSTR_EQUAL) {
      return false;
    }
#else
    if (*candidatePart != *rootPart) {
      return false;
    }
#endif
  }
  return true;
}

bool rootsOverlap(const std::filesystem::path &first,
                  const std::filesystem::path &second,
                  std::string &errorMessage) {
  std::error_code error;
  const auto canonicalFirst = std::filesystem::weakly_canonical(first, error);
  if (error) {
    errorMessage =
        "Unable to resolve profile export staging: " + error.message();
    return true;
  }
  const auto canonicalSecond = std::filesystem::weakly_canonical(second, error);
  if (error) {
    errorMessage =
        "Unable to resolve managed application data: " + error.message();
    return true;
  }
  if (pathStartsWith(canonicalFirst, canonicalSecond) ||
      pathStartsWith(canonicalSecond, canonicalFirst)) {
    errorMessage = "Profile export staging overlaps managed application data.";
    return true;
  }
  return false;
}

std::string randomDirectoryName() {
  std::random_device random;
  constexpr char hexadecimal[] = "0123456789abcdef";
  std::array<char, 32> name{};
  for (char &character : name) {
    character = hexadecimal[random() & 0xfU];
  }
  return std::string(name.begin(), name.end());
}

enum class OpenIssuedResult { Opened, Busy, Unsafe, Missing };

#if !defined(_WIN32)

class UniqueFd {
public:
  UniqueFd() = default;
  explicit UniqueFd(int value) : value_(value) {}
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  ~UniqueFd() { reset(); }

  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return value_ >= 0; }
  int release() noexcept { return std::exchange(value_, -1); }
  void reset(int value = -1) noexcept {
    if (value_ >= 0) {
      ::close(value_);
    }
    value_ = value;
  }

private:
  int value_ = -1;
};

struct NativeRoot {
  UniqueFd temporary;
  UniqueFd root;
};

struct NativeIssued {
  UniqueFd directory;
  UniqueFd lease;
  std::string name;
  dev_t device = 0;
  ino_t inode = 0;
  std::chrono::system_clock::time_point modified;
};

bool cleanupIssued(NativeRoot &root, NativeIssued &issued,
                   std::string &errorMessage) noexcept;

std::string posixError(std::string_view fallback, int error) {
  return std::string(fallback) + ": " + std::strerror(error);
}

bool secureDirectoryFd(int descriptor, std::string &errorMessage) {
  struct stat status{};
  if (::fchmod(descriptor, 0700) != 0 || ::fstat(descriptor, &status) != 0) {
    errorMessage = posixError("Unable to secure profile export staging", errno);
    return false;
  }
  if (!S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0777) != 0700) {
    errorMessage = "Profile export staging permissions are unsafe.";
    return false;
  }
  return true;
}

std::chrono::system_clock::time_point
modifiedFromStatus(const struct stat &status) {
#if defined(__APPLE__)
  const auto seconds = std::chrono::seconds(status.st_mtimespec.tv_sec);
  const auto nanoseconds =
      std::chrono::nanoseconds(status.st_mtimespec.tv_nsec);
#else
  const auto seconds = std::chrono::seconds(status.st_mtim.tv_sec);
  const auto nanoseconds = std::chrono::nanoseconds(status.st_mtim.tv_nsec);
#endif
  return std::chrono::system_clock::time_point(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          seconds + nanoseconds));
}

bool openRoot(const std::filesystem::path &temporaryRoot,
              NativeRoot &nativeRoot, std::string &errorMessage) {
  if (temporaryRoot.empty() || !temporaryRoot.is_absolute()) {
    errorMessage = "Private temporary storage must be an absolute path.";
    return false;
  }
  UniqueFd temporary(::open(temporaryRoot.c_str(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!temporary) {
    errorMessage =
        posixError("Private temporary storage is not trustworthy", errno);
    return false;
  }
  if (::mkdirat(temporary.get(), std::string(kRootName).c_str(), 0700) != 0 &&
      errno != EEXIST) {
    errorMessage = posixError("Unable to create profile export staging", errno);
    return false;
  }
  UniqueFd root(::openat(temporary.get(), std::string(kRootName).c_str(),
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!root || !secureDirectoryFd(root.get(), errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage =
          posixError("Profile export staging is not trustworthy", errno);
    }
    return false;
  }
  nativeRoot.temporary = std::move(temporary);
  nativeRoot.root = std::move(root);
  return true;
}

bool verifyRootPathIdentity(const std::filesystem::path &rootPath,
                            const NativeRoot &nativeRoot,
                            std::string &errorMessage) {
  struct stat pathStatus{};
  struct stat heldStatus{};
  if (::lstat(rootPath.c_str(), &pathStatus) != 0 ||
      ::fstat(nativeRoot.root.get(), &heldStatus) != 0) {
    errorMessage =
        posixError("Unable to verify profile export staging identity", errno);
    return false;
  }
  if (!S_ISDIR(pathStatus.st_mode) || !S_ISDIR(heldStatus.st_mode) ||
      pathStatus.st_dev != heldStatus.st_dev ||
      pathStatus.st_ino != heldStatus.st_ino) {
    errorMessage =
        "Profile export staging path changed while it was being prepared.";
    return false;
  }
  return true;
}

OpenIssuedResult acquireLease(int directoryFd, UniqueFd &lease, bool create,
                              std::string &errorMessage) {
  UniqueFd candidate;
  bool created = false;
  if (create) {
    candidate.reset(::openat(
        directoryFd, ".lease",
        O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600));
    if (candidate) {
      created = true;
    } else if (errno != EEXIST) {
      errorMessage = posixError("Unable to create profile export lease", errno);
      return OpenIssuedResult::Unsafe;
    }
  }
  if (!candidate) {
    candidate.reset(::openat(directoryFd, ".lease",
                             O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
  }
  if (!candidate) {
    errorMessage = posixError("Unable to open profile export lease", errno);
    return OpenIssuedResult::Unsafe;
  }
  struct stat status{};
  if (::fstat(candidate.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != geteuid() || status.st_nlink != 1) {
    errorMessage = "Profile export lease identity is unsafe.";
    return OpenIssuedResult::Unsafe;
  }
  if (created && (::fchmod(candidate.get(), 0600) != 0 ||
                  ::fstat(candidate.get(), &status) != 0)) {
    errorMessage = posixError("Unable to secure profile export lease", errno);
    return OpenIssuedResult::Unsafe;
  }
  if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
      status.st_nlink != 1 || (status.st_mode & 0777) != 0600) {
    errorMessage = "Profile export lease permissions are unsafe.";
    return OpenIssuedResult::Unsafe;
  }
  if (::flock(candidate.get(), LOCK_EX | LOCK_NB) != 0) {
    if (errno == EWOULDBLOCK || errno == EAGAIN) {
      return OpenIssuedResult::Busy;
    }
    errorMessage = posixError("Unable to lock profile export lease", errno);
    return OpenIssuedResult::Unsafe;
  }
  lease = std::move(candidate);
  return OpenIssuedResult::Opened;
}

OpenIssuedResult lockIssuedDirectory(int directoryFd,
                                     std::string &errorMessage) {
  if (::flock(directoryFd, LOCK_EX | LOCK_NB) == 0) {
    return OpenIssuedResult::Opened;
  }
  if (errno == EWOULDBLOCK || errno == EAGAIN) {
    return OpenIssuedResult::Busy;
  }
  errorMessage =
      posixError("Unable to lock issued profile export staging", errno);
  return OpenIssuedResult::Unsafe;
}

OpenIssuedResult openIssued(const NativeRoot &root, std::string_view name,
                            bool createLease, NativeIssued &issued,
                            std::string &errorMessage) {
  UniqueFd directory(::openat(root.root.get(), std::string(name).c_str(),
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
  if (!directory) {
    if (errno == ENOENT) {
      return OpenIssuedResult::Missing;
    }
    errorMessage =
        posixError("Unable to open issued profile export staging", errno);
    return OpenIssuedResult::Unsafe;
  }
  struct stat status{};
  if (::fstat(directory.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != geteuid() || (status.st_mode & 0777) != 0700) {
    errorMessage = "Issued profile export staging is unsafe.";
    return OpenIssuedResult::Unsafe;
  }
  const auto directoryLock = lockIssuedDirectory(directory.get(), errorMessage);
  if (directoryLock != OpenIssuedResult::Opened) {
    return directoryLock;
  }
  UniqueFd lease;
  const auto leaseResult =
      acquireLease(directory.get(), lease, createLease, errorMessage);
  if (leaseResult != OpenIssuedResult::Opened) {
    return leaseResult;
  }
  issued.directory = std::move(directory);
  issued.lease = std::move(lease);
  issued.name = std::string(name);
  issued.device = status.st_dev;
  issued.inode = status.st_ino;
  issued.modified = modifiedFromStatus(status);
  return OpenIssuedResult::Opened;
}

bool allocateIssued(NativeRoot &root, NativeIssued &issued,
                    std::string &errorMessage) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::string name = randomDirectoryName();
    if (::mkdirat(root.root.get(), name.c_str(), 0700) != 0) {
      if (errno == EEXIST) {
        continue;
      }
      errorMessage =
          posixError("Unable to allocate profile export staging", errno);
      return false;
    }
    UniqueFd directory(
        ::openat(root.root.get(), name.c_str(),
                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (!directory || !secureDirectoryFd(directory.get(), errorMessage)) {
      if (directory) {
        ::unlinkat(root.root.get(), name.c_str(), AT_REMOVEDIR);
      }
      return false;
    }
    struct stat status{};
    if (::fstat(directory.get(), &status) != 0) {
      errorMessage =
          posixError("Unable to inspect profile export staging", errno);
      return false;
    }
    NativeIssued candidate;
    candidate.directory = std::move(directory);
    candidate.name = name;
    candidate.device = status.st_dev;
    candidate.inode = status.st_ino;
    candidate.modified = modifiedFromStatus(status);
    if (lockIssuedDirectory(candidate.directory.get(), errorMessage) !=
        OpenIssuedResult::Opened) {
      std::string cleanupError;
      cleanupIssued(root, candidate, cleanupError);
      return false;
    }
    if (acquireLease(candidate.directory.get(), candidate.lease, true,
                     errorMessage) != OpenIssuedResult::Opened) {
      std::string cleanupError;
      cleanupIssued(root, candidate, cleanupError);
      return false;
    }
    issued = std::move(candidate);
    return true;
  }
  errorMessage = "Unable to allocate unique profile export staging.";
  return false;
}

std::string identityKey(const NativeIssued &issued) {
  return "posix:" +
         std::to_string(static_cast<unsigned long long>(issued.device)) + ":" +
         std::to_string(static_cast<unsigned long long>(issued.inode));
}

std::chrono::system_clock::time_point modifiedTime(const NativeIssued &issued) {
  return issued.modified;
}

bool clearDirectoryContents(int directoryFd, dev_t expectedDevice,
                            std::string &errorMessage) {
  UniqueFd duplicate(::dup(directoryFd));
  if (!duplicate) {
    errorMessage =
        posixError("Unable to inspect profile export staging", errno);
    return false;
  }
  DIR *rawDirectory = ::fdopendir(duplicate.release());
  if (rawDirectory == nullptr) {
    errorMessage =
        posixError("Unable to inspect profile export staging", errno);
    return false;
  }
  bool success = true;
  while (true) {
    errno = 0;
    dirent *entry = ::readdir(rawDirectory);
    if (entry == nullptr) {
      if (errno != 0) {
        errorMessage =
            posixError("Unable to enumerate profile export staging", errno);
        success = false;
      }
      break;
    }
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    struct stat status{};
    if (::fstatat(directoryFd, entry->d_name, &status, AT_SYMLINK_NOFOLLOW) !=
        0) {
      errorMessage =
          posixError("Unable to inspect profile export staging entry", errno);
      success = false;
      break;
    }
    if (status.st_dev != expectedDevice) {
      errorMessage =
          "Refusing to traverse a mounted profile export staging entry.";
      success = false;
      break;
    }
    if (S_ISDIR(status.st_mode)) {
      UniqueFd child(::openat(directoryFd, entry->d_name,
                              O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
      struct stat openedStatus{};
      if (!child || ::fstat(child.get(), &openedStatus) != 0 ||
          openedStatus.st_dev != status.st_dev ||
          openedStatus.st_ino != status.st_ino ||
          !clearDirectoryContents(child.get(), expectedDevice, errorMessage)) {
        if (errorMessage.empty()) {
          errorMessage = "Profile export staging changed during cleanup.";
        }
        success = false;
        break;
      }
      struct stat finalStatus{};
      if (::fstatat(directoryFd, entry->d_name, &finalStatus,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
          finalStatus.st_dev != openedStatus.st_dev ||
          finalStatus.st_ino != openedStatus.st_ino ||
          ::unlinkat(directoryFd, entry->d_name, AT_REMOVEDIR) != 0) {
        errorMessage =
            "Profile export staging changed during directory cleanup.";
        success = false;
        break;
      }
    } else if (S_ISREG(status.st_mode)) {
      if (::unlinkat(directoryFd, entry->d_name, 0) != 0) {
        errorMessage =
            posixError("Unable to remove profile export staging file", errno);
        success = false;
        break;
      }
    } else {
      errorMessage =
          "Refusing to remove an unsafe profile export staging entry.";
      success = false;
      break;
    }
  }
  ::closedir(rawDirectory);
  return success;
}

bool cleanupIssued(NativeRoot &root, NativeIssued &issued,
                   std::string &errorMessage) noexcept {
  try {
    struct stat heldStatus{};
    if (::fstat(issued.directory.get(), &heldStatus) != 0 ||
        heldStatus.st_dev != issued.device ||
        heldStatus.st_ino != issued.inode ||
        !clearDirectoryContents(issued.directory.get(), issued.device,
                                errorMessage)) {
      if (errorMessage.empty()) {
        errorMessage = "Unable to validate profile export staging cleanup.";
      }
      return false;
    }
    struct stat pathStatus{};
    if (::fstatat(root.root.get(), issued.name.c_str(), &pathStatus,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        return true;
      }
      errorMessage =
          posixError("Unable to validate issued profile export path", errno);
      return false;
    }
    if (!S_ISDIR(pathStatus.st_mode) || pathStatus.st_dev != issued.device ||
        pathStatus.st_ino != issued.inode ||
        ::unlinkat(root.root.get(), issued.name.c_str(), AT_REMOVEDIR) != 0) {
      errorMessage =
          "Refusing to remove a replaced profile export staging directory.";
      return false;
    }
    return true;
  } catch (...) {
    errorMessage = "Profile export staging cleanup is deferred.";
    return false;
  }
}

bool listIssuedNames(const NativeRoot &root, std::vector<std::string> &names,
                     std::string &errorMessage) {
  UniqueFd duplicate(::dup(root.root.get()));
  if (!duplicate) {
    errorMessage =
        posixError("Unable to inspect profile export staging", errno);
    return false;
  }
  DIR *directory = ::fdopendir(duplicate.release());
  if (directory == nullptr) {
    errorMessage =
        posixError("Unable to inspect profile export staging", errno);
    return false;
  }
  errno = 0;
  while (dirent *entry = ::readdir(directory)) {
    const std::string_view name(entry->d_name);
    if (IsIssuedDirectoryName(name)) {
      names.emplace_back(name);
    }
  }
  const int enumerationError = errno;
  ::closedir(directory);
  if (enumerationError != 0) {
    errorMessage = posixError("Unable to enumerate profile export staging",
                              enumerationError);
    return false;
  }
  return true;
}

#else

class UniqueHandle {
public:
  UniqueHandle() = default;
  explicit UniqueHandle(HANDLE value) : value_(value) {}
  UniqueHandle(const UniqueHandle &) = delete;
  UniqueHandle &operator=(const UniqueHandle &) = delete;
  UniqueHandle(UniqueHandle &&other) noexcept
      : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
  UniqueHandle &operator=(UniqueHandle &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
    }
    return *this;
  }
  ~UniqueHandle() { reset(); }

  [[nodiscard]] HANDLE get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
  }
  void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
    if (*this) {
      CloseHandle(value_);
    }
    value_ = value;
  }

private:
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

struct NativeRoot {
  UniqueHandle temporary;
  UniqueHandle root;
  std::filesystem::path rootPath;
};

struct NativeIssued {
  UniqueHandle directory;
  UniqueHandle lease;
  std::filesystem::path path;
  std::string name;
  DWORD volumeSerial = 0;
  DWORD fileIndexHigh = 0;
  DWORD fileIndexLow = 0;
  std::chrono::system_clock::time_point modified;
};

bool cleanupIssued(NativeRoot &root, NativeIssued &issued,
                   std::string &errorMessage) noexcept;

std::string windowsError(std::string_view fallback, DWORD code) {
  return std::string(fallback) + " (Windows error " +
         std::to_string(static_cast<unsigned long>(code)) + ")";
}

bool handleIsSafeDirectory(HANDLE handle, std::string &errorMessage) {
  FILE_ATTRIBUTE_TAG_INFO tagInfo{};
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tagInfo,
                                    sizeof(tagInfo)) ||
      !GetFileInformationByHandle(handle, &information) ||
      (tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
      (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    errorMessage = "Profile export staging is an unsafe Windows directory.";
    return false;
  }
  return true;
}

UniqueHandle openDirectoryHandle(const std::filesystem::path &path,
                                 DWORD access) {
  return UniqueHandle(CreateFileW(
      path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
}

bool openRoot(const std::filesystem::path &temporaryRoot,
              NativeRoot &nativeRoot, std::string &errorMessage) {
  if (temporaryRoot.empty() || !temporaryRoot.is_absolute()) {
    errorMessage = "Private temporary storage must be an absolute path.";
    return false;
  }
  auto temporary = openDirectoryHandle(temporaryRoot, FILE_LIST_DIRECTORY |
                                                          FILE_READ_ATTRIBUTES);
  if (!temporary || !handleIsSafeDirectory(temporary.get(), errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage = windowsError(
          "Private temporary storage is not trustworthy", GetLastError());
    }
    return false;
  }
  const auto rootPath = RootUnder(temporaryRoot);
  if (!CreateDirectoryW(rootPath.c_str(), nullptr) &&
      GetLastError() != ERROR_ALREADY_EXISTS) {
    errorMessage =
        windowsError("Unable to create profile export staging", GetLastError());
    return false;
  }
  auto root =
      openDirectoryHandle(rootPath, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES);
  if (!root || !handleIsSafeDirectory(root.get(), errorMessage) ||
      !platform_document_handoff::detail::SecurePrivateDocumentPath(
          rootPath, true, errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage = windowsError("Profile export staging is not trustworthy",
                                  GetLastError());
    }
    return false;
  }
  nativeRoot.temporary = std::move(temporary);
  nativeRoot.root = std::move(root);
  nativeRoot.rootPath = rootPath;
  return true;
}

bool verifyRootPathIdentity(const std::filesystem::path &rootPath,
                            const NativeRoot &nativeRoot,
                            std::string &errorMessage) {
  auto current = openDirectoryHandle(rootPath, FILE_READ_ATTRIBUTES);
  if (!current || !handleIsSafeDirectory(current.get(), errorMessage)) {
    if (errorMessage.empty()) {
      errorMessage = windowsError(
          "Unable to verify profile export staging identity", GetLastError());
    }
    return false;
  }
  BY_HANDLE_FILE_INFORMATION currentInformation{};
  BY_HANDLE_FILE_INFORMATION heldInformation{};
  if (!GetFileInformationByHandle(current.get(), &currentInformation) ||
      !GetFileInformationByHandle(nativeRoot.root.get(), &heldInformation)) {
    errorMessage = windowsError(
        "Unable to verify profile export staging identity", GetLastError());
    return false;
  }
  if (currentInformation.dwVolumeSerialNumber !=
          heldInformation.dwVolumeSerialNumber ||
      currentInformation.nFileIndexHigh != heldInformation.nFileIndexHigh ||
      currentInformation.nFileIndexLow != heldInformation.nFileIndexLow) {
    errorMessage =
        "Profile export staging path changed while it was being prepared.";
    return false;
  }
  return true;
}

bool readIdentity(HANDLE handle, NativeIssued &issued) {
  BY_HANDLE_FILE_INFORMATION information{};
  if (!GetFileInformationByHandle(handle, &information)) {
    return false;
  }
  issued.volumeSerial = information.dwVolumeSerialNumber;
  issued.fileIndexHigh = information.nFileIndexHigh;
  issued.fileIndexLow = information.nFileIndexLow;
  ULARGE_INTEGER ticks{};
  ticks.LowPart = information.ftLastWriteTime.dwLowDateTime;
  ticks.HighPart = information.ftLastWriteTime.dwHighDateTime;
  constexpr unsigned long long kWindowsToUnixEpoch = 116444736000000000ULL;
  if (ticks.QuadPart < kWindowsToUnixEpoch) {
    issued.modified = std::chrono::system_clock::time_point::min();
  } else {
    issued.modified = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds((ticks.QuadPart - kWindowsToUnixEpoch) *
                                     100ULL)));
  }
  return true;
}

OpenIssuedResult acquireLease(const std::filesystem::path &directory,
                              UniqueHandle &lease, bool create,
                              std::string &errorMessage) {
  const auto path = directory / L".lease";
  UniqueHandle candidate(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     nullptr,
                                     create ? OPEN_ALWAYS : OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!candidate) {
    errorMessage =
        windowsError("Unable to open profile export lease", GetLastError());
    return OpenIssuedResult::Unsafe;
  }
  FILE_ATTRIBUTE_TAG_INFO tagInfo{};
  if (!GetFileInformationByHandleEx(candidate.get(), FileAttributeTagInfo,
                                    &tagInfo, sizeof(tagInfo)) ||
      (tagInfo.FileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
    errorMessage = "Profile export lease is unsafe.";
    return OpenIssuedResult::Unsafe;
  }
  OVERLAPPED overlapped{};
  if (!LockFileEx(candidate.get(),
                  LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                  &overlapped)) {
    if (GetLastError() == ERROR_LOCK_VIOLATION) {
      return OpenIssuedResult::Busy;
    }
    errorMessage =
        windowsError("Unable to lock profile export lease", GetLastError());
    return OpenIssuedResult::Unsafe;
  }
  lease = std::move(candidate);
  return OpenIssuedResult::Opened;
}

OpenIssuedResult openIssued(const NativeRoot &root, std::string_view name,
                            bool createLease, NativeIssued &issued,
                            std::string &errorMessage) {
  const auto path = root.rootPath / std::string(name);
  auto directory = openDirectoryHandle(path, DELETE | FILE_LIST_DIRECTORY |
                                                 FILE_READ_ATTRIBUTES);
  if (!directory) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
      return OpenIssuedResult::Missing;
    }
    if (error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION) {
      return OpenIssuedResult::Busy;
    }
    errorMessage =
        windowsError("Unable to open issued profile export staging", error);
    return OpenIssuedResult::Unsafe;
  }
  if (!handleIsSafeDirectory(directory.get(), errorMessage)) {
    return OpenIssuedResult::Unsafe;
  }
  issued.path = path;
  issued.name = std::string(name);
  if (!readIdentity(directory.get(), issued)) {
    errorMessage = windowsError("Unable to identify profile export staging",
                                GetLastError());
    return OpenIssuedResult::Unsafe;
  }
  UniqueHandle lease;
  const auto leaseResult = acquireLease(path, lease, createLease, errorMessage);
  if (leaseResult != OpenIssuedResult::Opened) {
    return leaseResult;
  }
  issued.directory = std::move(directory);
  issued.lease = std::move(lease);
  return OpenIssuedResult::Opened;
}

bool allocateIssued(NativeRoot &root, NativeIssued &issued,
                    std::string &errorMessage) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    const std::string name = randomDirectoryName();
    const auto path = root.rootPath / name;
    if (!CreateDirectoryW(path.c_str(), nullptr)) {
      if (GetLastError() == ERROR_ALREADY_EXISTS) {
        continue;
      }
      errorMessage = windowsError("Unable to allocate profile export staging",
                                  GetLastError());
      return false;
    }
    auto directory = openDirectoryHandle(path, DELETE | FILE_LIST_DIRECTORY |
                                                   FILE_READ_ATTRIBUTES);
    if (!directory || !handleIsSafeDirectory(directory.get(), errorMessage) ||
        !platform_document_handoff::detail::SecurePrivateDocumentPath(
            path, true, errorMessage)) {
      return false;
    }
    NativeIssued candidate;
    candidate.directory = std::move(directory);
    candidate.path = path;
    candidate.name = name;
    if (!readIdentity(candidate.directory.get(), candidate)) {
      errorMessage = windowsError("Unable to identify profile export staging",
                                  GetLastError());
      return false;
    }
    if (acquireLease(path, candidate.lease, true, errorMessage) !=
        OpenIssuedResult::Opened) {
      std::string cleanupError;
      cleanupIssued(root, candidate, cleanupError);
      return false;
    }
    issued = std::move(candidate);
    return true;
  }
  errorMessage = "Unable to allocate unique profile export staging.";
  return false;
}

std::string identityKey(const NativeIssued &issued) {
  return "windows:" +
         std::to_string(static_cast<unsigned long>(issued.volumeSerial)) + ":" +
         std::to_string(static_cast<unsigned long>(issued.fileIndexHigh)) +
         ":" + std::to_string(static_cast<unsigned long>(issued.fileIndexLow));
}

std::chrono::system_clock::time_point modifiedTime(const NativeIssued &issued) {
  return issued.modified;
}

bool markForDeletion(HANDLE handle, std::string &errorMessage) {
  FILE_DISPOSITION_INFO disposition{};
  disposition.DeleteFile = TRUE;
  if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition,
                                  sizeof(disposition))) {
    errorMessage =
        windowsError("Unable to remove profile export staging", GetLastError());
    return false;
  }
  return true;
}

bool clearDirectoryContents(const std::filesystem::path &directory,
                            std::string &errorMessage) {
  WIN32_FIND_DATAW data{};
  const auto pattern = directory / L"*";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND) {
      return true;
    }
    errorMessage = windowsError("Unable to inspect profile export staging",
                                GetLastError());
    return false;
  }
  bool success = true;
  do {
    const std::wstring_view name(data.cFileName);
    if (name == L"." || name == L"..") {
      continue;
    }
    const auto childPath = directory / data.cFileName;
    const bool childDirectory =
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    auto child = UniqueHandle(
        CreateFileW(childPath.c_str(),
                    DELETE | FILE_READ_ATTRIBUTES |
                        (childDirectory ? FILE_LIST_DIRECTORY : 0),
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT |
                        (childDirectory ? FILE_FLAG_BACKUP_SEMANTICS : 0),
                    nullptr));
    if (!child) {
      errorMessage = windowsError("Unable to open profile export staging entry",
                                  GetLastError());
      success = false;
      break;
    }
    FILE_ATTRIBUTE_TAG_INFO tagInfo{};
    if (!GetFileInformationByHandleEx(child.get(), FileAttributeTagInfo,
                                      &tagInfo, sizeof(tagInfo)) ||
        (tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
      errorMessage =
          "Refusing to traverse a Windows reparse point in profile staging.";
      success = false;
      break;
    }
    if (childDirectory && !clearDirectoryContents(childPath, errorMessage)) {
      success = false;
      break;
    }
    if (!markForDeletion(child.get(), errorMessage)) {
      success = false;
      break;
    }
    child.reset();
  } while (FindNextFileW(find, &data));
  const DWORD enumerationError = GetLastError();
  FindClose(find);
  if (success && enumerationError != ERROR_NO_MORE_FILES) {
    errorMessage = windowsError("Unable to enumerate profile export staging",
                                enumerationError);
    return false;
  }
  return success;
}

bool cleanupIssued(NativeRoot &, NativeIssued &issued,
                   std::string &errorMessage) noexcept {
  try {
    issued.lease.reset();
    if (!clearDirectoryContents(issued.path, errorMessage) ||
        !markForDeletion(issued.directory.get(), errorMessage)) {
      return false;
    }
    return true;
  } catch (...) {
    errorMessage = "Profile export staging cleanup is deferred.";
    return false;
  }
}

bool listIssuedNames(const NativeRoot &root, std::vector<std::string> &names,
                     std::string &errorMessage) {
  WIN32_FIND_DATAW data{};
  const auto pattern = root.rootPath / L"*";
  HANDLE find = FindFirstFileW(pattern.c_str(), &data);
  if (find == INVALID_HANDLE_VALUE) {
    if (GetLastError() == ERROR_FILE_NOT_FOUND) {
      return true;
    }
    errorMessage = windowsError("Unable to inspect profile export staging",
                                GetLastError());
    return false;
  }
  do {
    const std::filesystem::path name(data.cFileName);
    const std::string text = name.string();
    if (IsIssuedDirectoryName(text)) {
      names.push_back(text);
    }
  } while (FindNextFileW(find, &data));
  const DWORD enumerationError = GetLastError();
  FindClose(find);
  if (enumerationError != ERROR_NO_MORE_FILES) {
    errorMessage = windowsError("Unable to enumerate profile export staging",
                                enumerationError);
    return false;
  }
  return true;
}

#endif

class SourceLifetime {
public:
  SourceLifetime() = default;

  void adopt(NativeRoot root, NativeIssued issued,
             std::string identity) noexcept {
    root_ = std::move(root);
    issued_ = std::move(issued);
    identity_ = std::move(identity);
    registered_ = registerActiveIdentity(identity_);
    adopted_ = true;
  }

  ~SourceLifetime() {
    if (!adopted_) {
      return;
    }
    std::string errorMessage;
    if (!cleanupIssued(root_, issued_, errorMessage)) {
      reportLateCleanup(errorMessage);
    }
    if (registered_) {
      unregisterActiveIdentity(identity_);
    }
  }

private:
  NativeRoot root_;
  NativeIssued issued_;
  std::string identity_;
  bool registered_ = false;
  bool adopted_ = false;
};

struct PreparedRoot {
  std::filesystem::path path;
  NativeRoot native;
  std::string errorMessage;
};

PreparedRoot prepareRoot(const Request &request) {
  PreparedRoot prepared;
  if (request.temporaryRoot.empty() || !request.temporaryRoot.is_absolute()) {
    prepared.errorMessage =
        "Private temporary storage must be an absolute path.";
    return prepared;
  }
  if (request.managedApplicationRoot.empty() ||
      !request.managedApplicationRoot.is_absolute()) {
    prepared.errorMessage =
        "Managed application data must be an absolute path.";
    return prepared;
  }
  if (request.staleAfter <= std::chrono::system_clock::duration::zero()) {
    prepared.errorMessage =
        "Profile export stale-cleanup age must be positive.";
    return prepared;
  }
  std::error_code canonicalError;
  const auto canonicalTemporary =
      std::filesystem::canonical(request.temporaryRoot, canonicalError);
  if (canonicalError) {
    prepared.errorMessage = "Unable to resolve private temporary storage: " +
                            canonicalError.message();
    return prepared;
  }
  prepared.path = RootUnder(canonicalTemporary);
  if (rootsOverlap(prepared.path, request.managedApplicationRoot,
                   prepared.errorMessage) ||
      !openRoot(canonicalTemporary, prepared.native, prepared.errorMessage) ||
      !verifyRootPathIdentity(prepared.path, prepared.native,
                              prepared.errorMessage)) {
    return prepared;
  }
  return prepared;
}

} // namespace

std::filesystem::path RootUnder(const std::filesystem::path &temporaryRoot) {
  return (temporaryRoot / std::string(kRootName)).lexically_normal();
}

bool IsIssuedDirectoryName(std::string_view name) noexcept {
  if (name.size() != 32) {
    return false;
  }
  for (const char character : name) {
    const bool digit = character >= '0' && character <= '9';
    const bool lowerHex = character >= 'a' && character <= 'f';
    if (!digit && !lowerHex) {
      return false;
    }
  }
  return true;
}

SweepResult Sweep(const Request &request) {
  SweepResult result;
  auto prepared = prepareRoot(request);
  if (!prepared.errorMessage.empty()) {
    result.errorMessage = std::move(prepared.errorMessage);
    return result;
  }
  std::vector<std::string> names;
  if (!listIssuedNames(prepared.native, names, result.errorMessage)) {
    return result;
  }
  for (const auto &name : names) {
    NativeIssued issued;
    std::string openError;
    const auto opened =
        openIssued(prepared.native, name, true, issued, openError);
    if (opened == OpenIssuedResult::Busy ||
        opened == OpenIssuedResult::Missing) {
      continue;
    }
    if (opened != OpenIssuedResult::Opened) {
      report(request.reportWarning,
             openError.empty()
                 ? "Skipped an unsafe issued profile export staging entry."
                 : std::move(openError));
      continue;
    }
    const auto modified = modifiedTime(issued);
    if (modified == std::chrono::system_clock::time_point::max()) {
      report(request.reportWarning,
             "Unable to determine profile export staging age.");
      continue;
    }
    const std::string identity = identityKey(issued);
    if (isActiveIdentity(identity) || modified > request.now ||
        request.now - modified < request.staleAfter) {
      continue;
    }
    std::string cleanupError;
    if (cleanupIssued(prepared.native, issued, cleanupError)) {
      ++result.staleDirectoriesRemoved;
    } else {
      report(request.reportWarning, std::move(cleanupError));
    }
  }
  return result;
}

Result Create(const Request &request) {
  Result result;
  auto prepared = prepareRoot(request);
  if (!prepared.errorMessage.empty()) {
    result.errorMessage = std::move(prepared.errorMessage);
    return result;
  }
  NativeIssued issued;
  if (!allocateIssued(prepared.native, issued, result.errorMessage)) {
    return result;
  }
  if (!verifyRootPathIdentity(prepared.path, prepared.native,
                              result.errorMessage)) {
    std::string cleanupError;
    cleanupIssued(prepared.native, issued, cleanupError);
    return result;
  }
  const std::string issuedName = issued.name;
  bool adopted = false;
  try {
    auto archivePath = prepared.path / issuedName / std::string(kArchiveName);
    auto identity = identityKey(issued);
    auto lifetime = std::make_shared<SourceLifetime>();
    lifetime->adopt(std::move(prepared.native), std::move(issued),
                    std::move(identity));
    adopted = true;
    result.archivePath = std::move(archivePath);
    result.sourceLifetime = std::move(lifetime);
  } catch (const std::exception &exception) {
    if (!adopted) {
      std::string cleanupError;
      cleanupIssued(prepared.native, issued, cleanupError);
    }
    result.errorMessage = "Unable to retain profile export staging: " +
                          std::string(exception.what());
  } catch (...) {
    if (!adopted) {
      std::string cleanupError;
      cleanupIssued(prepared.native, issued, cleanupError);
    }
    result.errorMessage = "Unable to retain profile export staging.";
  }
  return result;
}

} // namespace profile_export_staging
