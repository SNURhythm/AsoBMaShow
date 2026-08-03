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
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return SkinRejectedLinkKind::NonRegular;
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return SkinRejectedLinkKind::WindowsReparsePoint;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return SkinRejectedLinkKind::None;
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
