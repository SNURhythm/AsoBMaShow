#include "SkinAliasDetector.h"

#if defined(__APPLE__)
#import <Foundation/Foundation.h>
#include <sys/stat.h>

namespace skin {
namespace {

class AppleSkinAliasDetector final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind
  inspectNoFollow(const std::filesystem::path &path) const override {
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
      return SkinRejectedLinkKind::NonRegular;
    }
    if (S_ISLNK(status.st_mode)) {
      return SkinRejectedLinkKind::SymbolicLink;
    }
    if (!S_ISREG(status.st_mode) && !S_ISDIR(status.st_mode)) {
      return SkinRejectedLinkKind::NonRegular;
    }
    if (S_ISREG(status.st_mode) && status.st_nlink != 1) {
      return SkinRejectedLinkKind::HardLink;
    }

    @autoreleasepool {
      NSString *nativePath = [[NSFileManager defaultManager]
          stringWithFileSystemRepresentation:path.c_str()
                                      length:path.native().size()];
      if (nativePath == nil) {
        return SkinRejectedLinkKind::NonRegular;
      }
      NSURL *url = [NSURL fileURLWithPath:nativePath
                              isDirectory:S_ISDIR(status.st_mode)];
      NSNumber *isAlias = nil;
      NSError *error = nil;
      if (![url getResourceValue:&isAlias
                          forKey:NSURLIsAliasFileKey
                           error:&error]) {
        return SkinRejectedLinkKind::NonRegular;
      }
      if (isAlias.boolValue) {
        return SkinRejectedLinkKind::AppleFinderAlias;
      }
    }
    return SkinRejectedLinkKind::None;
  }
};

} // namespace

std::unique_ptr<SkinAliasDetector> createPlatformSkinAliasDetector() {
  return std::make_unique<AppleSkinAliasDetector>();
}

} // namespace skin
#endif
