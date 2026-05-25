#include "iOSNatives.hpp"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#import <FirebaseCore/FirebaseCore.h>
#include <dispatch/dispatch.h>
#include <vector>
#include <string>

extern "C" void AsoBMaShowInitializeFirebase() {
  @autoreleasepool {
    if ([FIRApp defaultApp] == nil) {
      [FIRApp configure];
    }
  }
}

namespace {
UIWindow *FindActiveWindow() {
  if (@available(iOS 13.0, *)) {
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
      if (![scene isKindOfClass:[UIWindowScene class]]) {
        continue;
      }
      UIWindowScene *windowScene = (UIWindowScene *)scene;
      if (windowScene.activationState !=
          UISceneActivationStateForegroundActive) {
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

bool DownloadURLTextIOS(const std::string &url, std::string &body,
                        std::string &errorMessage) {
  @autoreleasepool {
    NSString *urlString = [NSString stringWithUTF8String:url.c_str()];
    NSURL *nsUrl = [NSURL URLWithString:urlString];
    if (nsUrl == nil) {
      errorMessage = "Invalid URL: " + url;
      return false;
    }

    NSMutableURLRequest *request = [NSMutableURLRequest
         requestWithURL:nsUrl
            cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
        timeoutInterval:25.0];
    [request setValue:@"AsoBMaShow" forHTTPHeaderField:@"User-Agent"];

    __block NSData *responseData = nil;
    __block NSURLResponse *urlResponse = nil;
    __block NSError *requestError = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    NSURLSessionDataTask *task = [[NSURLSession sharedSession]
        dataTaskWithRequest:request
          completionHandler:^(NSData *data, NSURLResponse *response,
                              NSError *error) {
            responseData = data;
            urlResponse = response;
            requestError = error;
            dispatch_semaphore_signal(semaphore);
          }];
    [task resume];
    const long waitResult = dispatch_semaphore_wait(
        semaphore, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
    if (waitResult != 0) {
      [task cancel];
      errorMessage = "Timed out while downloading " + url;
      return false;
    }

    if (requestError != nil) {
      errorMessage =
          std::string([[requestError localizedDescription] UTF8String]);
      return false;
    }

    NSHTTPURLResponse *httpResponse =
        [urlResponse isKindOfClass:[NSHTTPURLResponse class]]
            ? (NSHTTPURLResponse *)urlResponse
            : nil;
    if (httpResponse != nil && httpResponse.statusCode >= 400) {
      errorMessage = "HTTP " + std::to_string(httpResponse.statusCode) +
                     " while downloading " + url;
      return false;
    }

    if (responseData == nil) {
      errorMessage = "No response body while downloading " + url;
      return false;
    }

    NSString *text = [[NSString alloc] initWithData:responseData
                                           encoding:NSUTF8StringEncoding];
    if (text == nil) {
      errorMessage = "Downloaded response is not UTF-8: " + url;
      return false;
    }

    body = std::string([text UTF8String]);
    return true;
  }
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
