#include "iOSNatives.hpp"
#if TARGET_OS_IOS
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#include <vector>
#include <string>

namespace {
UIWindow *FindActiveWindow() {
  if (@available(iOS 13.0, *)) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
      if (![scene isKindOfClass:[UIWindowScene class]]) {
        continue;
      }
      UIWindowScene *windowScene = (UIWindowScene *)scene;
      if (windowScene.activationState != UISceneActivationStateForegroundActive) {
        continue;
      }
      for (UIWindow *window in windowScene.windows) {
        if (window.isKeyWindow) {
          return window;
        }
      }
      if (windowScene.windows.count > 0) {
        return windowScene.windows.firstObject;
      }
    }
  }
  return UIApplication.sharedApplication.keyWindow;
}
} // namespace

std::string GetIOSDocumentsPath() {
  return std::string([[NSSearchPathForDirectoriesInDomains(
      NSDocumentDirectory, NSUserDomainMask, YES) objectAtIndex:0] UTF8String]);
}

// get nwh
void *GetIOSWindowHandle(void *uiwindow) {
  // get rootviewcontroller.view.layer;

  return (__bridge void *)(((__bridge UIWindow *)uiwindow)
                               .rootViewController.view.layer);
}

// list files recursively
std::vector<std::string> ListDocumentFilesRecursively() {
  // get file manager
  NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                       NSUserDomainMask, YES);
  NSString *documentsDirectory = [paths objectAtIndex:0];
  NSArray *filePathsArray = [[NSFileManager defaultManager]
      subpathsOfDirectoryAtPath:documentsDirectory
                          error:nil];

  // convert to vector
  std::vector<std::string> filesVec;
  for (NSString *file in filePathsArray) {
    filesVec.push_back([file UTF8String]);
  }
  // return
  return filesVec;
}

IOSNormalizedSafeAreaInsets GetIOSSafeAreaInsetsNormalized() {
  IOSNormalizedSafeAreaInsets insets;
  UIWindow *window = FindActiveWindow();
  if (window == nil) {
    return insets;
  }

  const UIEdgeInsets safeInsets = window.safeAreaInsets;
  const CGRect bounds = window.bounds;
  if (bounds.size.width <= 0.0 || bounds.size.height <= 0.0) {
    return insets;
  }

  insets.top = safeInsets.top / bounds.size.height;
  insets.left = safeInsets.left / bounds.size.width;
  insets.bottom = safeInsets.bottom / bounds.size.height;
  insets.right = safeInsets.right / bounds.size.width;
  return insets;
}

// register touch event
// void RegisterTouchEvent() {
//   // get root view controller
//   UIViewController *rootViewController =
//       [UIApplication sharedApplication].keyWindow.rootViewController;
//   // get view
//   UIView *view = rootViewController.view;
//   // get touch event
//   UITapGestureRecognizer *tapGesture = [[UITapGestureRecognizer alloc]
//       initWithTarget:rootViewController
//               action:@selector(handleTapGesture:)];
//   // add touch event
//   [view addGestureRecognizer:tapGesture];
// }
#endif
