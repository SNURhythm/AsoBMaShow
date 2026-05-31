#include "iOSNatives.hpp"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include <AVFoundation/AVFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <Foundation/Foundation.h>
#include <Photos/Photos.h>
#include <UIKit/UIKit.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {
NSString *NSStringFromUtf8(const std::string &utf8) {
  if (utf8.empty()) {
    return @"";
  }
  return [[NSString alloc] initWithBytes:utf8.data()
                                  length:utf8.size()
                                encoding:NSUTF8StringEncoding];
}

CTFontRef CreateIOSSystemFont(int fontSize) {
  const CGFloat pointSize = std::max(1, fontSize);
  UIFont *font = [UIFont systemFontOfSize:pointSize];
  return CTFontCreateWithName((__bridge CFStringRef)font.fontName, pointSize,
                              nullptr);
}

CTLineRef CreateIOSSystemTextLine(const std::string &utf8, int fontSize) {
  NSString *text = NSStringFromUtf8(utf8);
  if (text == nil) {
    return nullptr;
  }

  CTFontRef font = CreateIOSSystemFont(fontSize);
  if (font == nullptr) {
    return nullptr;
  }

  NSDictionary *attributes = @{
    (__bridge id)kCTFontAttributeName : (__bridge id)font,
    (__bridge id)kCTForegroundColorFromContextAttributeName : @YES,
  };
  NSAttributedString *attributedText =
      [[NSAttributedString alloc] initWithString:text attributes:attributes];
  CTLineRef line = CTLineCreateWithAttributedString(
      (__bridge CFAttributedStringRef)attributedText);
  CFRelease(font);
  return line;
}

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

bool IsPhotoAuthorizationAllowed(PHAuthorizationStatus status) {
  if (status == PHAuthorizationStatusAuthorized) {
    return true;
  }
  if (@available(iOS 14.0, *)) {
    return status == PHAuthorizationStatusLimited;
  }
  return false;
}

std::string PhotoAuthorizationStatusMessage(PHAuthorizationStatus status) {
  switch (status) {
  case PHAuthorizationStatusDenied:
    return "Photos permission was denied";
  case PHAuthorizationStatusRestricted:
    return "Photos access is restricted";
  case PHAuthorizationStatusNotDetermined:
    return "Photos permission was not granted";
  case PHAuthorizationStatusAuthorized:
    return "";
  default:
    if (@available(iOS 14.0, *)) {
      if (status == PHAuthorizationStatusLimited) {
        return "";
      }
    }
    return "Photos permission was not granted";
  }
}

bool CreateFullFrameRatePlaybackVideoForPhotos(NSString *sourcePath,
                                               NSString **preparedPath,
                                               std::string &errorMessage) {
  *preparedPath = nil;

#if defined(__IPHONE_OS_VERSION_MAX_ALLOWED) &&                                \
    __IPHONE_OS_VERSION_MAX_ALLOWED >= 180000
  if (@available(iOS 18.0, *)) {
    NSURL *sourceURL = [NSURL fileURLWithPath:sourcePath];
    NSString *fileName =
        [NSString stringWithFormat:@"AsoBMaShowReplay-%@.mov",
                                   [[NSUUID UUID] UUIDString]];
    NSString *outputPath =
        [NSTemporaryDirectory() stringByAppendingPathComponent:fileName];
    NSURL *outputURL = [NSURL fileURLWithPath:outputPath];
    [[NSFileManager defaultManager] removeItemAtURL:outputURL error:nil];

    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:sourceURL options:nil];
    AVAssetExportSession *session =
        [[AVAssetExportSession alloc]
            initWithAsset:asset
               presetName:AVAssetExportPresetPassthrough];
    if (session == nil) {
      errorMessage = "Failed to prepare replay video for Photos";
      return false;
    }
    if (![session.supportedFileTypes
            containsObject:AVFileTypeQuickTimeMovie]) {
      errorMessage = "Photos replay video export does not support MOV output";
      return false;
    }

    AVMutableMetadataItem *playbackIntent =
        [AVMutableMetadataItem metadataItem];
    playbackIntent.identifier =
        AVMetadataIdentifierQuickTimeMetadataFullFrameRatePlaybackIntent;
    playbackIntent.value = @1;
    playbackIntent.dataType = (__bridge NSString *)kCMMetadataBaseDataType_UInt8;
    session.metadata = @[ playbackIntent ];
    session.outputURL = outputURL;
    session.outputFileType = AVFileTypeQuickTimeMovie;

    __block AVAssetExportSessionStatus exportStatus =
        AVAssetExportSessionStatusUnknown;
    __block NSError *exportError = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    [session exportAsynchronouslyWithCompletionHandler:^{
      exportStatus = session.status;
      exportError = session.error;
      dispatch_semaphore_signal(semaphore);
    }];

    const long waitResult = dispatch_semaphore_wait(
        semaphore, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC));
    if (waitResult != 0) {
      [session cancelExport];
      [[NSFileManager defaultManager] removeItemAtURL:outputURL error:nil];
      errorMessage = "Timed out preparing replay video for Photos";
      return false;
    }
    if (exportStatus != AVAssetExportSessionStatusCompleted) {
      [[NSFileManager defaultManager] removeItemAtURL:outputURL error:nil];
      if (exportError != nil) {
        errorMessage =
            std::string([[exportError localizedDescription] UTF8String]);
      } else {
        errorMessage = "Failed to prepare replay video for Photos";
      }
      return false;
    }

    *preparedPath = outputPath;
  }
#endif

  return true;
}

bool RequestPhotoAddAuthorization(std::string &errorMessage) {
  __block PHAuthorizationStatus status = PHAuthorizationStatusNotDetermined;
  dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

  void (^requestBlock)(void) = ^{
    if (@available(iOS 14.0, *)) {
      status = [PHPhotoLibrary authorizationStatusForAccessLevel:
                                   PHAccessLevelAddOnly];
      if (status == PHAuthorizationStatusNotDetermined) {
        [PHPhotoLibrary
            requestAuthorizationForAccessLevel:PHAccessLevelAddOnly
                                       handler:^(
                                           PHAuthorizationStatus newStatus) {
                                         status = newStatus;
                                         dispatch_semaphore_signal(semaphore);
                                       }];
        return;
      }
      dispatch_semaphore_signal(semaphore);
      return;
    }

    status = [PHPhotoLibrary authorizationStatus];
    if (status == PHAuthorizationStatusNotDetermined) {
      [PHPhotoLibrary requestAuthorization:^(PHAuthorizationStatus newStatus) {
        status = newStatus;
        dispatch_semaphore_signal(semaphore);
      }];
      return;
    }
    dispatch_semaphore_signal(semaphore);
  };

  if ([NSThread isMainThread]) {
    requestBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), requestBlock);
  }

  const long waitResult = dispatch_semaphore_wait(
      semaphore, dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC));
  if (waitResult != 0) {
    errorMessage = "Timed out waiting for Photos permission";
    return false;
  }

  if (!IsPhotoAuthorizationAllowed(status)) {
    errorMessage = PhotoAuthorizationStatusMessage(status);
    return false;
  }
  return true;
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

bool SaveVideoToIOSPhotos(const std::string &filePath,
                          std::string &errorMessage) {
  @autoreleasepool {
    if (!RequestPhotoAddAuthorization(errorMessage)) {
      return false;
    }

    NSString *path = NSStringFromUtf8(filePath);
    if (path == nil || path.length == 0) {
      errorMessage = "Video export path is empty";
      return false;
    }
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
      errorMessage = "Video export file does not exist";
      return false;
    }

    NSString *preparedPath = nil;
    if (!CreateFullFrameRatePlaybackVideoForPhotos(path, &preparedPath,
                                                   errorMessage)) {
      return false;
    }

    NSString *savePath = preparedPath != nil ? preparedPath : path;
    NSURL *fileUrl = [NSURL fileURLWithPath:savePath];
    __block BOOL saveSucceeded = NO;
    __block BOOL requestCreated = NO;
    __block NSError *saveError = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    [[PHPhotoLibrary sharedPhotoLibrary]
        performChanges:^{
          PHAssetCreationRequest *request =
              [PHAssetCreationRequest creationRequestForAssetFromVideoAtFileURL:
                                          fileUrl];
          requestCreated = request != nil;
        }
        completionHandler:^(BOOL success, NSError *error) {
          saveSucceeded = success;
          saveError = error;
          dispatch_semaphore_signal(semaphore);
        }];

    const long waitResult = dispatch_semaphore_wait(
        semaphore, dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC));
    if (preparedPath != nil) {
      [[NSFileManager defaultManager] removeItemAtPath:preparedPath error:nil];
    }
    if (waitResult != 0) {
      errorMessage = "Timed out saving video to Photos";
      return false;
    }

    if (!saveSucceeded || !requestCreated) {
      if (saveError != nil) {
        errorMessage =
            std::string([[saveError localizedDescription] UTF8String]);
      } else {
        errorMessage = "Failed to save video to Photos";
      }
      return false;
    }

    return true;
  }
  return false;
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

IOSSystemTextMetrics GetIOSSystemTextMetrics(int fontSize) {
  @autoreleasepool {
    IOSSystemTextMetrics metrics;
    CTFontRef font = CreateIOSSystemFont(fontSize);
    if (font == nullptr) {
      metrics.height = std::max(1, fontSize);
      metrics.ascent = metrics.height;
      return metrics;
    }

    const CGFloat ascent = CTFontGetAscent(font);
    const CGFloat descent = CTFontGetDescent(font);
    const CGFloat leading = CTFontGetLeading(font);
    metrics.ascent = static_cast<int>(std::ceil(ascent));
    metrics.descent = static_cast<int>(std::ceil(descent));
    metrics.height = static_cast<int>(std::ceil(ascent + descent + leading));
    metrics.height = std::max(metrics.height, metrics.ascent + metrics.descent);
    metrics.height = std::max(1, metrics.height);
    CFRelease(font);
    return metrics;
  }
  return {};
}

int MeasureIOSSystemTextWidth(const std::string &utf8, int fontSize) {
  @autoreleasepool {
    if (utf8.empty()) {
      return 0;
    }

    CTLineRef line = CreateIOSSystemTextLine(utf8, fontSize);
    if (line == nullptr) {
      return 0;
    }

    const double width =
        CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    CFRelease(line);
    return std::max(0, static_cast<int>(std::ceil(width)));
  }
  return 0;
}

SDL_Surface *RenderIOSSystemTextSurface(const std::string &utf8, int fontSize,
                                        SDL_Color color) {
  @autoreleasepool {
    if (utf8.empty()) {
      return nullptr;
    }

    CTLineRef line = CreateIOSSystemTextLine(utf8, fontSize);
    if (line == nullptr) {
      return nullptr;
    }

    CGFloat ascent = 0.0;
    CGFloat descent = 0.0;
    CGFloat leading = 0.0;
    const double textWidth =
        CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    const IOSSystemTextMetrics metrics = GetIOSSystemTextMetrics(fontSize);
    const int width = std::max(1, static_cast<int>(std::ceil(textWidth)));
    const int height = std::max(1, metrics.height);

    std::vector<Uint8> alpha(
        static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    CGContextRef context = CGBitmapContextCreate(
        alpha.data(), width, height, 8, width, nullptr, kCGImageAlphaOnly);
    if (context == nullptr) {
      CFRelease(line);
      return nullptr;
    }

    CGContextSetShouldAntialias(context, true);
    CGContextSetAllowsAntialiasing(context, true);
    CGContextSetGrayFillColor(context, 1.0, 1.0);
    CGContextSetTextMatrix(context, CGAffineTransformIdentity);
    const CGFloat baseline =
        static_cast<CGFloat>(std::max(0, height - metrics.ascent));
    CGContextSetTextPosition(context, 0.0, baseline);
    CTLineDraw(line, context);
    CGContextRelease(context);
    CFRelease(line);

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(
        0, width, height, 32, SDL_PIXELFORMAT_BGRA32);
    if (surface == nullptr) {
      return nullptr;
    }

    SDL_FillRect(surface, nullptr, SDL_MapRGBA(surface->format, 0, 0, 0, 0));
    if (SDL_MUSTLOCK(surface)) {
      SDL_LockSurface(surface);
    }

    for (int y = 0; y < height; ++y) {
      auto *row = static_cast<Uint8 *>(surface->pixels) + y * surface->pitch;
      for (int x = 0; x < width; ++x) {
        const Uint8 coverage = alpha[static_cast<size_t>(y) * width + x];
        const Uint8 alphaValue =
            static_cast<Uint8>((static_cast<int>(coverage) * color.a) / 255);
        Uint32 pixel =
            SDL_MapRGBA(surface->format, color.r, color.g, color.b, alphaValue);
        std::memcpy(row + x * sizeof(Uint32), &pixel, sizeof(pixel));
      }
    }

    if (SDL_MUSTLOCK(surface)) {
      SDL_UnlockSurface(surface);
    }
    return surface;
  }
  return nullptr;
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
