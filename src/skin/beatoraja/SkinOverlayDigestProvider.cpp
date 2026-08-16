#include "SkinOverlayDigestProvider.h"

#include "../../FileChecksum.h"
#include "SkinAcceptanceRecorder.h"
#include "../package/SkinPathPolicy.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <limits>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "../package/SkinIOSFileOpenCompatibility.h"

namespace skin {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view kTreeDigestMagic = "ASOBMSKIN-TREE-V1";
constexpr std::size_t kMaximumOutstandingTickets = 4;

SkinDiagnostic digestFailure(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

SkinOverlayDigestPollResult readyFailure(std::string code,
                                         std::string message) {
  return {.state = SkinOverlayDigestPollState::Ready,
          .failure = digestFailure(std::move(code), std::move(message))};
}

void hashBytes(file_checksum::Sha256 &hash, std::string_view bytes) {
  hash.update(std::as_bytes(std::span(bytes.data(), bytes.size())));
}

void hashU32(file_checksum::Sha256 &hash, std::uint32_t value) {
  std::array<std::byte, 4> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(
        (value >> ((bytes.size() - index - 1U) * 8U)) & 0xffU);
  }
  hash.update(bytes);
}

void hashU64(file_checksum::Sha256 &hash, std::uint64_t value) {
  std::array<std::byte, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(
        (value >> ((bytes.size() - index - 1U) * 8U)) & 0xffU);
  }
  hash.update(bytes);
}

struct DigestFileMetadata {
  std::uint64_t device = 0;
  std::uint64_t inode = 0;
  std::uint64_t size = 0;
  std::uint64_t links = 0;
  std::int64_t modifiedSeconds = 0;
  std::int64_t modifiedNanoseconds = 0;
  std::int64_t changedSeconds = 0;
  std::int64_t changedNanoseconds = 0;
  std::uint32_t mode = 0;

  auto operator<=>(const DigestFileMetadata &) const = default;
};

struct DigestFileEntry {
  std::string relativePath;
  DigestFileMetadata metadata;

  auto operator<=>(const DigestFileEntry &) const = default;
};

struct DigestInventory {
  std::vector<DigestFileEntry> files;
  // Directories remain structural and are not part of SkinTreeDigestV1, but
  // retaining bounded metadata lets the worker detect tree mutations.
  std::vector<DigestFileEntry> directories;
  std::map<std::string, std::uint8_t> collisionNodes;
  std::uint64_t totalBytes = 0;
};

struct DigestComputation {
  bool cancelled = false;
  SkinOverlayDigestPollResult result;
};

#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
struct DigestStabilityHook {
  void (*callback)(void *) = nullptr;
  void *context = nullptr;

  void invoke() const {
    if (callback != nullptr) {
      callback(context);
    }
  }
};
#endif

DigestComputation cancelledComputation() { return {.cancelled = true}; }

DigestComputation failedComputation(std::string code, std::string message) {
  return {.result = readyFailure(std::move(code), std::move(message))};
}

bool safePathComponent(std::string_view component) {
  if (component.empty() || component == "." || component == "..") {
    return false;
  }
  const auto normalized = normalizeSkinSourceNameNfc(component);
  return normalized.value && *normalized.value == component;
}

bool recordInventoryNode(DigestInventory &inventory,
                         std::string_view relativePath) {
  const auto collisionKey = skinPathCollisionKey(relativePath);
  return collisionKey &&
         inventory.collisionNodes.emplace(*collisionKey, 0U).second;
}

#if !defined(_WIN32)

class UniqueDescriptor {
public:
  UniqueDescriptor() = default;
  explicit UniqueDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}
  UniqueDescriptor(const UniqueDescriptor &) = delete;
  UniqueDescriptor &operator=(const UniqueDescriptor &) = delete;
  UniqueDescriptor(UniqueDescriptor &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  UniqueDescriptor &operator=(UniqueDescriptor &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.descriptor_, -1));
    }
    return *this;
  }
  ~UniqueDescriptor() { reset(); }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return descriptor_ >= 0;
  }

private:
  void reset(int descriptor = -1) noexcept {
    if (descriptor_ >= 0) {
      ::close(descriptor_);
    }
    descriptor_ = descriptor;
  }

  int descriptor_ = -1;
};

int directoryOpenFlags() {
  int flags = O_RDONLY | O_DIRECTORY;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

int fileOpenFlags() {
  int flags = O_RDONLY | O_NONBLOCK;
#if defined(O_CLOEXEC)
  flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

DigestFileMetadata metadataFromStat(const struct stat &status) {
#if defined(__APPLE__)
  const auto modifiedSeconds = status.st_mtimespec.tv_sec;
  const auto modifiedNanoseconds = status.st_mtimespec.tv_nsec;
  const auto changedSeconds = status.st_ctimespec.tv_sec;
  const auto changedNanoseconds = status.st_ctimespec.tv_nsec;
#else
  const auto modifiedSeconds = status.st_mtim.tv_sec;
  const auto modifiedNanoseconds = status.st_mtim.tv_nsec;
  const auto changedSeconds = status.st_ctim.tv_sec;
  const auto changedNanoseconds = status.st_ctim.tv_nsec;
#endif
  DigestFileMetadata metadata{
      .device = static_cast<std::uint64_t>(status.st_dev),
      .inode = static_cast<std::uint64_t>(status.st_ino),
      .size =
          status.st_size < 0 ? 0U : static_cast<std::uint64_t>(status.st_size),
      .links = static_cast<std::uint64_t>(status.st_nlink),
      .modifiedSeconds = static_cast<std::int64_t>(modifiedSeconds),
      .modifiedNanoseconds = static_cast<std::int64_t>(modifiedNanoseconds),
      .changedSeconds = static_cast<std::int64_t>(changedSeconds),
      .changedNanoseconds = static_cast<std::int64_t>(changedNanoseconds),
      .mode = static_cast<std::uint32_t>(status.st_mode),
  };
  return metadata;
}

bool sameOpenedNode(const struct stat &inspected, const struct stat &opened) {
  return inspected.st_dev == opened.st_dev &&
         inspected.st_ino == opened.st_ino &&
         (inspected.st_mode & S_IFMT) == (opened.st_mode & S_IFMT);
}

struct OpenOverlayRootResult {
  UniqueDescriptor descriptor;
  bool missing = false;
  std::string failureCode;
};

OpenOverlayRootResult openAbsoluteDirectoryNoFollow(const fs::path &path) {
  const fs::path normalized = path.lexically_normal();
  if (path.empty() || !path.is_absolute() || normalized != path ||
      normalized.root_path().empty()) {
    return {.failureCode = "skin_overlay_digest_unsafe_root"};
  }

  UniqueDescriptor current(
      ::open(normalized.root_path().c_str(), directoryOpenFlags()));
  if (!current) {
    return {.failureCode = "skin_overlay_digest_unsafe_root"};
  }

  const fs::path relative =
      normalized.lexically_relative(normalized.root_path());
  for (const auto &componentPath : relative) {
    const auto component = componentPath.native();
    if (!safePathComponent(component)) {
      return {.failureCode = "skin_overlay_digest_unsafe_root"};
    }

    struct stat inspected{};
    if (::fstatat(current.get(), component.c_str(), &inspected,
                  AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
        return {.missing = true};
      }
      return {.failureCode = "skin_overlay_digest_unsafe_root"};
    }
    if (!S_ISDIR(inspected.st_mode)) {
      return {.failureCode = "skin_overlay_digest_unsafe_root"};
    }

    UniqueDescriptor next(
        ::openat(current.get(), component.c_str(), directoryOpenFlags()));
    struct stat opened{};
    if (!next || ::fstat(next.get(), &opened) != 0 ||
        !S_ISDIR(opened.st_mode) || !sameOpenedNode(inspected, opened)) {
      return {.failureCode = "skin_overlay_digest_unsafe_root"};
    }
    current = std::move(next);
  }
  return {.descriptor = std::move(current)};
}

bool collectDirectoryInventory(int directory, std::string_view prefix,
                               std::uint32_t depth,
                               const SkinOverlayDigestLimits &limits,
                               const std::atomic_bool &cancelled,
                               DigestInventory &inventory,
                               std::string &failureCode) {
  if (cancelled.load(std::memory_order_relaxed)) {
    return false;
  }
  const int duplicate = ::openat(directory, ".", directoryOpenFlags());
  if (duplicate < 0) {
    failureCode = "skin_overlay_digest_io_failed";
    return false;
  }
  DIR *stream = ::fdopendir(duplicate);
  if (stream == nullptr) {
    ::close(duplicate);
    failureCode = "skin_overlay_digest_io_failed";
    return false;
  }

  bool succeeded = true;
  while (true) {
    errno = 0;
    const dirent *entry = ::readdir(stream);
    if (entry == nullptr) {
      if (errno != 0) {
        failureCode = "skin_overlay_digest_io_failed";
        succeeded = false;
      }
      break;
    }
    if (cancelled.load(std::memory_order_relaxed)) {
      succeeded = false;
      break;
    }
    const std::string_view name(entry->d_name);
    if (name == "." || name == "..") {
      continue;
    }
    if (!safePathComponent(name)) {
      failureCode = "skin_overlay_digest_unsafe_node";
      succeeded = false;
      break;
    }
    if (depth >= SkinPackagePolicy::maxPathComponents) {
      failureCode = "skin_overlay_digest_limit_exceeded";
      succeeded = false;
      break;
    }

    std::string relativePath;
    relativePath.reserve(prefix.size() + (prefix.empty() ? 0U : 1U) +
                         name.size());
    relativePath.append(prefix);
    if (!relativePath.empty()) {
      relativePath.push_back('/');
    }
    relativePath.append(name);
    if (relativePath.size() > SkinPackagePolicy::maxPathBytes) {
      failureCode = "skin_overlay_digest_limit_exceeded";
      succeeded = false;
      break;
    }
    if (!recordInventoryNode(inventory, relativePath)) {
      failureCode = "skin_overlay_digest_unsafe_node";
      succeeded = false;
      break;
    }

    struct stat inspected{};
    if (::fstatat(directory, entry->d_name, &inspected, AT_SYMLINK_NOFOLLOW) !=
        0) {
      failureCode = "skin_overlay_digest_source_changed";
      succeeded = false;
      break;
    }
    if (S_ISDIR(inspected.st_mode)) {
      UniqueDescriptor child(
          ::openat(directory, entry->d_name, directoryOpenFlags()));
      struct stat opened{};
      if (!child || ::fstat(child.get(), &opened) != 0 ||
          !S_ISDIR(opened.st_mode) || !sameOpenedNode(inspected, opened)) {
        failureCode = "skin_overlay_digest_unsafe_node";
        succeeded = false;
        break;
      }
      constexpr std::uint64_t kComponentsPerFile =
          SkinPackagePolicy::maxPathComponents;
      const std::uint64_t maximumDirectories =
          limits.maximumFiles >
                  (std::numeric_limits<std::uint64_t>::max() - 1U) /
                      kComponentsPerFile
              ? std::numeric_limits<std::uint64_t>::max()
              : limits.maximumFiles * kComponentsPerFile + 1U;
      if (inventory.directories.size() >= maximumDirectories) {
        failureCode = "skin_overlay_digest_limit_exceeded";
        succeeded = false;
        break;
      }
      inventory.directories.push_back(
          {.relativePath = relativePath,
           .metadata = metadataFromStat(inspected)});
      if (!collectDirectoryInventory(child.get(), relativePath, depth + 1U,
                                     limits, cancelled, inventory,
                                     failureCode)) {
        succeeded = false;
        break;
      }
      continue;
    }
    if (!S_ISREG(inspected.st_mode) || inspected.st_nlink != 1 ||
        inspected.st_size < 0) {
      failureCode = "skin_overlay_digest_unsafe_node";
      succeeded = false;
      break;
    }

    const auto size = static_cast<std::uint64_t>(inspected.st_size);
    if (size > limits.maximumFileBytes ||
        inventory.files.size() >= limits.maximumFiles ||
        size > limits.maximumTotalBytes - inventory.totalBytes) {
      failureCode = "skin_overlay_digest_limit_exceeded";
      succeeded = false;
      break;
    }
    inventory.totalBytes += size;
    inventory.files.push_back({.relativePath = std::move(relativePath),
                               .metadata = metadataFromStat(inspected)});
  }
  ::closedir(stream);
  return succeeded;
}

UniqueDescriptor openRelativeFileNoFollow(int root,
                                          std::string_view relativePath) {
  UniqueDescriptor current(::dup(root));
  if (!current) {
    return {};
  }

  std::size_t start = 0;
  while (true) {
    const auto separator = relativePath.find('/', start);
    const std::string component(relativePath.substr(
        start, separator == std::string_view::npos ? std::string_view::npos
                                                   : separator - start));
    if (!safePathComponent(component)) {
      return {};
    }
    if (separator == std::string_view::npos) {
      return UniqueDescriptor(
          ::openat(current.get(), component.c_str(), fileOpenFlags()));
    }
    UniqueDescriptor child(
        ::openat(current.get(), component.c_str(), directoryOpenFlags()));
    if (!child) {
      return {};
    }
    current = std::move(child);
    start = separator + 1U;
  }
}

DigestComputation computeOverlayDigestPosix(
    const fs::path &root, const SkinOverlayDigestLimits &limits,
    const std::atomic_bool &cancelled
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
    , DigestStabilityHook stabilityHook
#endif
) {
  auto opened = openAbsoluteDirectoryNoFollow(root);
  if (!opened.failureCode.empty()) {
    return failedComputation(
        std::move(opened.failureCode),
        "private skin overlay root failed no-follow validation");
  }

  DigestInventory first;
  DigestFileMetadata rootMetadata;
  if (!opened.missing) {
    struct stat rootStatus{};
    if (::fstat(opened.descriptor.get(), &rootStatus) != 0 ||
        !S_ISDIR(rootStatus.st_mode)) {
      return failedComputation("skin_overlay_digest_unsafe_root",
                               "private skin overlay root is unsafe");
    }
    rootMetadata = metadataFromStat(rootStatus);
    std::string failureCode;
    if (!collectDirectoryInventory(opened.descriptor.get(), {}, 0U, limits,
                                   cancelled, first, failureCode)) {
      if (cancelled.load(std::memory_order_relaxed)) {
        return cancelledComputation();
      }
      const bool unsafe = failureCode == "skin_overlay_digest_unsafe_node";
      return failedComputation(
          std::move(failureCode),
          unsafe ? "private skin overlay contains an unsafe node"
                 : "private skin overlay could not be measured safely");
    }
    std::sort(first.files.begin(), first.files.end());
    std::sort(first.directories.begin(), first.directories.end());
  }

  if (cancelled.load(std::memory_order_relaxed)) {
    return cancelledComputation();
  }

  file_checksum::Sha256 hash;
  hashBytes(hash, kTreeDigestMagic);
  const std::byte zero{};
  hash.update(std::span(&zero, 1));
  hashU64(hash, static_cast<std::uint64_t>(first.files.size()));
  std::array<char, 64U * 1024U> buffer{};
  for (const auto &entry : first.files) {
    if (cancelled.load(std::memory_order_relaxed)) {
      return cancelledComputation();
    }
    hashU32(hash, static_cast<std::uint32_t>(entry.relativePath.size()));
    hashBytes(hash, entry.relativePath);
    hashU64(hash, entry.metadata.size);

    UniqueDescriptor file =
        openRelativeFileNoFollow(opened.descriptor.get(), entry.relativePath);
    struct stat before{};
    if (!file || ::fstat(file.get(), &before) != 0 ||
        !S_ISREG(before.st_mode) || before.st_nlink != 1 ||
        metadataFromStat(before) != entry.metadata) {
      return failedComputation("skin_overlay_digest_source_changed",
                               "private skin overlay changed before reading");
    }

    std::uint64_t readBytes = 0;
    while (true) {
      if (cancelled.load(std::memory_order_relaxed)) {
        return cancelledComputation();
      }
      const ssize_t count = ::read(file.get(), buffer.data(), buffer.size());
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count < 0) {
        return failedComputation("skin_overlay_digest_io_failed",
                                 "private skin overlay file could not be read");
      }
      if (count == 0) {
        break;
      }
      const auto countSize = static_cast<std::size_t>(count);
      hash.update(std::as_bytes(std::span(buffer.data(), countSize)));
      readBytes += static_cast<std::uint64_t>(count);
      if (readBytes > entry.metadata.size) {
        return failedComputation("skin_overlay_digest_source_changed",
                                 "private skin overlay changed while reading");
      }
    }
    struct stat after{};
    if (readBytes != entry.metadata.size || ::fstat(file.get(), &after) != 0 ||
        metadataFromStat(after) != entry.metadata) {
      return failedComputation("skin_overlay_digest_source_changed",
                               "private skin overlay changed while reading");
    }
  }

#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
  stabilityHook.invoke();
#endif
  if (cancelled.load(std::memory_order_relaxed)) {
    return cancelledComputation();
  }

  if (opened.missing) {
    const auto rechecked = openAbsoluteDirectoryNoFollow(root);
    if (!rechecked.missing || !rechecked.failureCode.empty()) {
      return failedComputation(
          "skin_overlay_digest_source_changed",
          "private skin overlay appeared during measurement");
    }
  } else {
    DigestInventory second;
    std::string failureCode;
    if (!collectDirectoryInventory(opened.descriptor.get(), {}, 0U, limits,
                                   cancelled, second, failureCode)) {
      if (cancelled.load(std::memory_order_relaxed)) {
        return cancelledComputation();
      }
      return failedComputation(
          "skin_overlay_digest_source_changed",
          "private skin overlay changed during measurement");
    }
    std::sort(second.files.begin(), second.files.end());
    std::sort(second.directories.begin(), second.directories.end());
    struct stat rootAfter{};
    auto reopened = openAbsoluteDirectoryNoFollow(root);
    struct stat reopenedStatus{};
    if (second.files != first.files) {
      return failedComputation(
          "skin_overlay_digest_source_changed",
          "private skin overlay files changed during measurement");
    }
    if (second.directories != first.directories) {
      return failedComputation(
          "skin_overlay_digest_source_changed",
          "private skin overlay directories changed during measurement");
    }
    if (::fstat(opened.descriptor.get(), &rootAfter) != 0 ||
        metadataFromStat(rootAfter) != rootMetadata) {
      return failedComputation(
          "skin_overlay_digest_source_changed",
          "private skin overlay root changed during measurement");
    }
    if (reopened.missing || !reopened.failureCode.empty() ||
        ::fstat(reopened.descriptor.get(), &reopenedStatus) != 0 ||
        metadataFromStat(reopenedStatus) != rootMetadata) {
      return failedComputation(
          "skin_overlay_digest_source_changed",
          "private skin overlay root identity changed during measurement");
    }
  }

  return {.result = {.state = SkinOverlayDigestPollState::Ready,
                     .lowercaseSha256 = hash.finalHex()}};
}

#else

DigestComputation computeOverlayDigestWindows(
    const fs::path &root, const SkinOverlayDigestLimits &limits,
    const std::atomic_bool &cancelled
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
    , DigestStabilityHook stabilityHook
#endif
) {
  // This acceptance-only service must never follow a Windows reparse point.
  // Keep Windows conservative until the native handle-relative walker is
  // available: a missing overlay has the canonical empty digest, while a
  // present tree fails closed instead of relying on path-based traversal.
  if (cancelled.load(std::memory_order_relaxed)) {
    return cancelledComputation();
  }
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
  stabilityHook.invoke();
#endif
  const DWORD attributes = ::GetFileAttributesW(root.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES &&
      ::GetLastError() == ERROR_FILE_NOT_FOUND) {
    file_checksum::Sha256 hash;
    hashBytes(hash, kTreeDigestMagic);
    const std::byte zero{};
    hash.update(std::span(&zero, 1));
    hashU64(hash, 0);
    return {.result = {.state = SkinOverlayDigestPollState::Ready,
                       .lowercaseSha256 = hash.finalHex()}};
  }
  (void)limits;
  return failedComputation(
      "skin_overlay_digest_unsafe_root",
      "private skin overlay cannot be measured with no-follow guarantees");
}

#endif

DigestComputation computeOverlayDigest(
    const SkinStorageRoots &roots, const SkinOverlayDigestLimits &limits,
    const SkinAcceptanceActivationKey &activation,
    const std::atomic_bool &cancelled
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
    , DigestStabilityHook stabilityHook
#endif
) {
  if (cancelled.load(std::memory_order_relaxed)) {
    return cancelledComputation();
  }
  const auto derived = deriveSkinPrivateOverlayRoot(roots, activation.profileId,
                                                    activation.entry);
  if (!derived.root) {
    return failedComputation("skin_overlay_identity_invalid",
                             "private skin overlay identity is invalid");
  }
#if defined(_WIN32)
  return computeOverlayDigestWindows(*derived.root, limits, cancelled
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
                                     , stabilityHook
#endif
  );
#else
  return computeOverlayDigestPosix(*derived.root, limits, cancelled
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
                                   , stabilityHook
#endif
  );
#endif
}

} // namespace

SkinOverlayDigestTicket nextSkinOverlayDigestTicket() noexcept {
  static std::atomic<std::uint64_t> next{1};
  auto candidate = next.load(std::memory_order_relaxed);
  while (candidate != std::numeric_limits<std::uint64_t>::max()) {
    if (next.compare_exchange_weak(candidate, candidate + 1,
                                   std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return {candidate};
    }
  }
  return {};
}

class SkinOverlayDigestProvider::Impl final {
public:
  Impl(SkinStorageRoots roots, SkinOverlayDigestLimits limits)
      : roots_(std::move(roots)), limits_(limits),
        worker_([this] { workerLoop(); }) {}

  ~Impl() { shutdown(); }

  SkinOverlayDigestTicket begin(const SkinAcceptanceActivationKey &activation) {
    const auto ticket = nextSkinOverlayDigestTicket();
    if (!ticket) {
      return {};
    }
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    Job job{.ticket = ticket, .activation = activation, .cancelled = cancelled};
    {
      std::lock_guard lock(mutex_);
      if (stopped_ || entries_.size() >= kMaximumOutstandingTickets) {
        return {};
      }
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
      job.stabilityHook = stabilityHook_;
#endif
      entries_.emplace(ticket.value, EntryState{.cancelled = cancelled});
      try {
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
        if (failNextQueueCommit_) {
          failNextQueueCommit_ = false;
          throw std::bad_alloc();
        }
#endif
        jobs_.push_back(std::move(job));
      } catch (...) {
        entries_.erase(ticket.value);
        throw;
      }
    }
    condition_.notify_one();
    return ticket;
  }

  SkinOverlayDigestPollResult
  poll(SkinOverlayDigestTicket ticket) const noexcept {
    try {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(ticket.value);
      if (!ticket || found == entries_.end()) {
        return {};
      }
      if (found->second.completion) {
        return *found->second.completion;
      }
      return {.state = SkinOverlayDigestPollState::Pending};
    } catch (...) {
      return {};
    }
  }

  void cancel(SkinOverlayDigestTicket ticket) noexcept {
    try {
      std::lock_guard lock(mutex_);
      const auto found = entries_.find(ticket.value);
      if (found == entries_.end()) {
        return;
      }
      found->second.cancelled->store(true, std::memory_order_relaxed);
      std::erase_if(jobs_,
                    [ticket](const Job &job) { return job.ticket == ticket; });
      entries_.erase(found);
    } catch (...) {
    }
    condition_.notify_all();
  }

  void shutdown() noexcept {
    try {
      std::lock_guard shutdownLock(shutdownMutex_);
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
      DigestStabilityHook shutdownHook;
#endif
      {
        std::lock_guard lock(mutex_);
        if (!stopped_) {
          stopped_ = true;
          for (auto &[ticket, entry] : entries_) {
            (void)ticket;
            entry.cancelled->store(true, std::memory_order_relaxed);
          }
          entries_.clear();
          jobs_.clear();
        }
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
        shutdownHook = shutdownHook_;
#endif
      }
      condition_.notify_all();
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
      shutdownHook.invoke();
#endif
      if (worker_.joinable()) {
        worker_.join();
      }
    } catch (...) {
      if (worker_.joinable()) {
        std::terminate();
      }
    }
  }

#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
  void setStabilityHook(SkinOverlayDigestProvider::StabilityHookForTesting hook,
                        void *context) noexcept {
    try {
      std::lock_guard lock(mutex_);
      stabilityHook_ = {.callback = hook, .context = context};
    } catch (...) {
    }
  }

  void setShutdownHook(SkinOverlayDigestProvider::StabilityHookForTesting hook,
                       void *context) noexcept {
    try {
      std::lock_guard lock(mutex_);
      shutdownHook_ = {.callback = hook, .context = context};
    } catch (...) {
    }
  }

  std::size_t queuedJobCount() const noexcept {
    try {
      std::lock_guard lock(mutex_);
      return jobs_.size();
    } catch (...) {
      return std::numeric_limits<std::size_t>::max();
    }
  }

  void failNextQueueCommit() noexcept {
    try {
      std::lock_guard lock(mutex_);
      failNextQueueCommit_ = true;
    } catch (...) {
    }
  }

  void failNextComputationAndFailureResult() noexcept {
    failNextComputation_.store(true, std::memory_order_relaxed);
    failNextFailureResult_.store(true, std::memory_order_relaxed);
  }
#endif

private:
  struct EntryState {
    std::shared_ptr<std::atomic_bool> cancelled;
    std::optional<SkinOverlayDigestPollResult> completion;
  };

  struct Job {
    SkinOverlayDigestTicket ticket;
    SkinAcceptanceActivationKey activation;
    std::shared_ptr<std::atomic_bool> cancelled;
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
    DigestStabilityHook stabilityHook;
#endif
  };

  void workerLoop() noexcept {
    while (true) {
      std::optional<Job> job;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopped_ || !jobs_.empty(); });
        if (stopped_) {
          return;
        }
        job.emplace(std::move(jobs_.front()));
        jobs_.pop_front();
      }

      DigestComputation computed;
      bool resultAllocationFailed = false;
      try {
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
        if (failNextComputation_.exchange(false, std::memory_order_relaxed)) {
          throw std::bad_alloc();
        }
#endif
        computed = computeOverlayDigest(roots_, limits_, job->activation,
                                        *job->cancelled
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
                                        , job->stabilityHook
#endif
        );
      } catch (...) {
        try {
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
          if (failNextFailureResult_.exchange(false,
                                              std::memory_order_relaxed)) {
            throw std::bad_alloc();
          }
#endif
          computed = failedComputation(
              "skin_overlay_digest_io_failed",
              "private skin overlay measurement failed safely");
        } catch (...) {
          job->cancelled->store(true, std::memory_order_relaxed);
          resultAllocationFailed = true;
        }
      }

      std::lock_guard lock(mutex_);
      const auto found = entries_.find(job->ticket.value);
      if (resultAllocationFailed) {
        if (found != entries_.end()) {
          entries_.erase(found);
        }
        continue;
      }
      if (!stopped_ && !computed.cancelled &&
          !job->cancelled->load(std::memory_order_relaxed) &&
          found != entries_.end()) {
        found->second.completion = std::move(computed.result);
      }
    }
  }

  SkinStorageRoots roots_;
  SkinOverlayDigestLimits limits_;
  std::mutex shutdownMutex_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool stopped_ = false;
  std::deque<Job> jobs_;
  mutable std::map<std::uint64_t, EntryState> entries_;
  std::thread worker_;
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
  DigestStabilityHook stabilityHook_;
  DigestStabilityHook shutdownHook_;
#endif
#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
  bool failNextQueueCommit_ = false;
  std::atomic_bool failNextComputation_{false};
  std::atomic_bool failNextFailureResult_{false};
#endif
};

SkinOverlayDigestProvider::SkinOverlayDigestProvider(
    SkinStorageRoots roots, SkinOverlayDigestLimits limits)
    : impl_(std::make_unique<Impl>(std::move(roots), limits)) {}

SkinOverlayDigestProvider::~SkinOverlayDigestProvider() { shutdown(); }

SkinOverlayDigestTicket SkinOverlayDigestProvider::beginDigest(
    const SkinAcceptanceActivationKey &activation) {
  return impl_ ? impl_->begin(activation) : SkinOverlayDigestTicket{};
}

SkinOverlayDigestPollResult SkinOverlayDigestProvider::pollDigest(
    SkinOverlayDigestTicket ticket) const noexcept {
  return impl_ ? impl_->poll(ticket) : SkinOverlayDigestPollResult{};
}

void SkinOverlayDigestProvider::cancelDigest(
    SkinOverlayDigestTicket ticket) noexcept {
  if (impl_) {
    impl_->cancel(ticket);
  }
}

void SkinOverlayDigestProvider::shutdown() noexcept {
  if (impl_) {
    impl_->shutdown();
  }
}

#if defined(ASOBMASHOW_SKIN_OVERLAY_DIGEST_PROVIDER_TESTING)
void SkinOverlayDigestProvider::setStabilityHookForTesting(
    StabilityHookForTesting hook, void *context) noexcept {
  if (impl_) {
    impl_->setStabilityHook(hook, context);
  }
}

void SkinOverlayDigestProvider::setShutdownHookForTesting(
    StabilityHookForTesting hook, void *context) noexcept {
  if (impl_) {
    impl_->setShutdownHook(hook, context);
  }
}

bool SkinOverlayDigestProvider::inventoryRejectsNodesForTesting(
    std::span<const InventoryNodeForTesting> nodes) noexcept {
  try {
    DigestInventory inventory;
    for (const auto &node : nodes) {
      // Files and directories intentionally share one collision namespace.
      (void)node.directory;
      if (!recordInventoryNode(inventory, node.virtualPath)) {
        return true;
      }
    }
    return false;
  } catch (...) {
    return true;
  }
}

std::size_t
SkinOverlayDigestProvider::queuedJobCountForTesting() const noexcept {
  return impl_ ? impl_->queuedJobCount() : 0U;
}

void SkinOverlayDigestProvider::failNextQueueCommitForTesting() noexcept {
  if (impl_) {
    impl_->failNextQueueCommit();
  }
}

void SkinOverlayDigestProvider::
    failNextComputationAndFailureResultForTesting() noexcept {
  if (impl_) {
    impl_->failNextComputationAndFailureResult();
  }
}
#endif

} // namespace skin
