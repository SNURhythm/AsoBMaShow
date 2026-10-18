#include "StorageArchiveIO.h"
#include <algorithm>
#include <cerrno>
#include <limits>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace storage_management {
namespace {
void require(bool value, const char *message) {
  if (!value) throw StorageArchiveError(StorageReason::IoError, message);
}
void requireSystem(bool value, const char *message) {
  if (value) return;
#ifdef _WIN32
  const auto error = std::error_code(GetLastError(), std::system_category());
#else
  const auto error = std::error_code(errno, std::generic_category());
#endif
  throw StorageArchiveError(StorageReason::IoError, std::string(message) + ": " + error.message());
}
void range(std::uint64_t offset, std::uint64_t length) {
  constexpr auto maximum = std::uint64_t(std::numeric_limits<std::int64_t>::max());
  require(offset <= maximum && length <= maximum - offset, "Archive offset overflow");
}
}
StorageArchiveIO::StorageArchiveIO(const std::filesystem::path &path,
    const std::vector<std::filesystem::path> &roots, bool write) : path(path), accessRoots_(roots) {
  std::string error;
  const auto initial = ReadFileIdentity(path, error, roots);
  if (!initial) throw StorageArchiveError(StorageReason::UnsafePath, error);
  original_ = *initial;
#ifdef _WIN32
  handle_ = CreateFileW(path.c_str(), GENERIC_READ | (write ? GENERIC_WRITE : 0), FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  requireSystem(handle_ != INVALID_HANDLE_VALUE, "Cannot lock archive for storage maintenance");
#else
  fd_ = open(path.c_str(), (write ? O_RDWR : O_RDONLY) | O_NOFOLLOW | O_CLOEXEC);
  requireSystem(fd_ >= 0, write ? "Cannot open writable archive" : "Cannot open archive for reading");
  if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    const auto error = std::error_code(errno, std::generic_category());
    close(fd_); fd_ = -1;
    throw StorageArchiveError(StorageReason::IoError, "Cannot lock archive: " + error.message());
  }
#endif
  try { originalToken_ = ChangeToken(); CheckPath(); }
  catch (...) {
#ifdef _WIN32
    CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE;
#else
    close(fd_); fd_ = -1;
#endif
    throw;
  }
}
StorageArchiveIO::~StorageArchiveIO() {
#ifdef _WIN32
  if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
  if (fd_ >= 0) close(fd_);
#endif
}
FileIdentity StorageArchiveIO::Identity() const {
#ifdef _WIN32
  BY_HANDLE_FILE_INFORMATION info{};
  require(GetFileInformationByHandle(handle_, &info) && info.nNumberOfLinks == 1 &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)), "Unsafe archive handle");
  return { .bytes = (std::uint64_t(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
      .device = info.dwVolumeSerialNumber, .fileId = (std::uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow };
#else
  struct stat info{};
  require(fstat(fd_, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size >= 0,
      "Unsafe archive handle");
  return { .bytes = static_cast<std::uint64_t>(info.st_size), .device = static_cast<std::uint64_t>(info.st_dev),
      .fileId = static_cast<std::uint64_t>(info.st_ino) };
#endif
}
std::string StorageArchiveIO::ChangeToken() const {
#ifdef _WIN32
  FILE_BASIC_INFO info{};
  require(GetFileInformationByHandleEx(handle_, FileBasicInfo, &info, sizeof(info)), "Cannot inspect archive change time");
  return std::to_string(info.LastWriteTime.QuadPart) + ":" + std::to_string(info.ChangeTime.QuadPart);
#else
  struct stat info{};
  require(fstat(fd_, &info) == 0, "Cannot inspect archive change time");
#ifdef __APPLE__
  const auto modified = info.st_mtimespec, changed = info.st_ctimespec;
#else
  const auto modified = info.st_mtim, changed = info.st_ctim;
#endif
  return std::to_string(modified.tv_sec) + ":" + std::to_string(modified.tv_nsec) + ":" +
      std::to_string(changed.tv_sec) + ":" + std::to_string(changed.tv_nsec);
#endif
}
void StorageArchiveIO::CheckPath() const {
  std::string error;
  const auto current = ReadFileIdentity(path, error, accessRoots_);
  const auto pinned = Identity();
  if (!current || current->device != original_.device || current->fileId != original_.fileId ||
      current->device != pinned.device || current->fileId != pinned.fileId || current->bytes != pinned.bytes ||
      current->modified != original_.modified || ChangeToken() != originalToken_)
    throw StorageArchiveError(StorageReason::StaleInput, "Archive path or identity changed");
}
void StorageArchiveIO::CaptureWrite() {
  std::string error; const auto current = ReadFileIdentity(path, error, accessRoots_);
  if (!current || current->device != original_.device || current->fileId != original_.fileId)
    throw StorageArchiveError(StorageReason::StaleInput, "Archive path changed during write");
  original_ = *current;
  originalToken_ = ChangeToken();
}
void StorageArchiveIO::Read(std::uint64_t offset, std::span<char> buffer) const {
  range(offset, buffer.size());
  bytesRead_ += buffer.size();
  while (!buffer.empty()) {
    const auto amount = std::min<std::size_t>(buffer.size(), 1024 * 1024);
#ifdef _WIN32
    LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(offset);
    require(SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN), "Cannot seek archive");
    DWORD got = 0;
    require(ReadFile(handle_, buffer.data(), static_cast<DWORD>(amount), &got, nullptr) && got, "Cannot read archive");
#else
    const auto got = pread(fd_, buffer.data(), amount, static_cast<off_t>(offset));
    if (got < 0 && errno == EINTR) continue;
    requireSystem(got >= 0, "Cannot read archive");
    require(got > 0, "Unexpected end of archive");
#endif
    offset += got; buffer = buffer.subspan(got);
  }
}
std::string StorageArchiveIO::Read(std::uint64_t offset, std::size_t length) const {
  std::string result(length, '\0'); Read(offset, std::span<char>(result)); return result;
}
void StorageArchiveIO::Write(std::uint64_t offset, std::string_view bytes) {
  range(offset, bytes.size()); CheckPath();
  while (!bytes.empty()) {
    const auto amount = std::min<std::size_t>(bytes.size(), 1024 * 1024);
#ifdef _WIN32
    LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(offset);
    require(SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN), "Cannot seek archive");
    DWORD put = 0;
    require(WriteFile(handle_, bytes.data(), static_cast<DWORD>(amount), &put, nullptr) && put, "Cannot write archive");
#else
    const auto put = pwrite(fd_, bytes.data(), amount, static_cast<off_t>(offset));
    if (put < 0 && errno == EINTR) continue;
    requireSystem(put >= 0, "Cannot write archive");
    require(put > 0, "Archive write made no progress");
#endif
    offset += put; bytes.remove_prefix(put);
  }
  CaptureWrite();
}
void StorageArchiveIO::Truncate(std::uint64_t length) {
  range(length, 0); CheckPath();
#ifdef _WIN32
  LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(length);
  require(SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN) && SetEndOfFile(handle_), "Cannot truncate archive");
#else
  requireSystem(ftruncate(fd_, static_cast<off_t>(length)) == 0, "Cannot truncate archive");
#endif
  CaptureWrite();
}
void StorageArchiveIO::Sync() {
#ifdef _WIN32
  requireSystem(FlushFileBuffers(handle_), "Cannot sync archive");
#else
  requireSystem(fsync(fd_) == 0, "Cannot sync archive");
#endif
}
}
