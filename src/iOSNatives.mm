#include "iOSNatives.hpp"
#if TARGET_OS_IOS
#include <Foundation/Foundation.h>
#include <UIKit/UIKit.h>
#include <vector>
#include <string>
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
