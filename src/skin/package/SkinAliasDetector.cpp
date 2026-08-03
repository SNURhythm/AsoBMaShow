#include "SkinAliasDetector.h"

#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace skin {
namespace {

SkinRejectedLinkKind inspectPortable(const std::filesystem::path &path) {
#if defined(_WIN32)
  const HANDLE handle = CreateFileW(
      path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return SkinRejectedLinkKind::NonRegular;
  }
  FILE_ATTRIBUTE_TAG_INFO tagInfo{};
  BY_HANDLE_FILE_INFORMATION information{};
  const bool inspected =
      GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tagInfo,
                                   sizeof(tagInfo)) &&
      GetFileInformationByHandle(handle, &information);
  const DWORD fileType = GetFileType(handle);
  CloseHandle(handle);
  if (!inspected || fileType != FILE_TYPE_DISK) {
    return SkinRejectedLinkKind::NonRegular;
  }
  if ((tagInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return SkinRejectedLinkKind::WindowsReparsePoint;
  }
  if ((tagInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return SkinRejectedLinkKind::None;
  }
  if (information.nNumberOfLinks != 1) {
    return SkinRejectedLinkKind::HardLink;
  }
  return SkinRejectedLinkKind::None;
#else
  struct stat status{};
  if (::lstat(path.c_str(), &status) != 0) {
    return SkinRejectedLinkKind::NonRegular;
  }
  if (S_ISLNK(status.st_mode)) {
    return SkinRejectedLinkKind::SymbolicLink;
  }
  if (S_ISDIR(status.st_mode)) {
    return SkinRejectedLinkKind::None;
  }
  if (!S_ISREG(status.st_mode)) {
    return SkinRejectedLinkKind::NonRegular;
  }
  if (status.st_nlink != 1) {
    return SkinRejectedLinkKind::HardLink;
  }
  return SkinRejectedLinkKind::None;
#endif
}

class PortableSkinAliasDetector final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind
  inspectNoFollow(const std::filesystem::path &path) const override {
    return inspectPortable(path);
  }
};

} // namespace

#if !defined(__APPLE__)
std::unique_ptr<SkinAliasDetector> createPlatformSkinAliasDetector() {
  return std::make_unique<PortableSkinAliasDetector>();
}
#endif

} // namespace skin
