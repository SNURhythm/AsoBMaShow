

#include "MacNatives.h"
#include <Foundation/Foundation.h>
#if TARGET_OS_OSX
#include <AppKit/AppKit.h>
#endif

void setSmoothScrolling(bool smoothScrolling) {
  [[NSUserDefaults standardUserDefaults]
      setBool:smoothScrolling ? YES : NO
       forKey:@"AppleMomentumScrollSupported"];
}

#if TARGET_OS_OSX
bool RevealPathInFinder(const std::string &path, std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    NSString *pathString = [[NSString alloc] initWithBytes:path.data()
                                                    length:path.size()
                                                  encoding:NSUTF8StringEncoding];
    if (pathString == nil || pathString.length == 0) {
      errorMessage = "Invalid file path";
      return false;
    }

    NSURL *url = [NSURL fileURLWithPath:pathString];
    if (url == nil) {
      errorMessage = "Invalid file URL";
      return false;
    }

    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[ url ]];
    return true;
  }
}
#endif
