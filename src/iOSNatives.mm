#include "iOSNatives.hpp"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "RAII.h"
#include "audio/NativeMusicPlayer.h"
#include "ir/IrHttpClientIOS.h"
#include "platform/PhotoAuthorizationPolicy.h"
#include <AudioToolbox/AudioToolbox.h>
#include <AVFoundation/AVFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>
#include <MediaPlayer/MediaPlayer.h>
#include <Photos/Photos.h>
#include <QuartzCore/CAMetalLayer.h>
#include <UIKit/UIKit.h>
#include <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr int kIOSReplaySampleRate = 44100;
constexpr int kIOSReplayChannels = 2;
constexpr NSTimeInterval kIOSReplayInputWaitTimeoutSeconds = 2.0;
constexpr NSTimeInterval kIOSReplayFinishWaitTimeoutSeconds = 30.0;
constexpr double kIOSReplayAudioLeadSeconds = 0.5;
UIDocumentInteractionController *gIOSRevealFileController = nil;
AVPlayer *gIOSNativeMusicPlayer = nil;
id gIOSNativeMusicFinishedObserver = nil;
IOSNativeMusicMetadata gIOSNativeMusicMetadata;
IOSNativeMusicQueue gIOSNativeMusicQueue;
bool gIOSNativeMusicRemoteCommandsConfigured = false;
float gIOSNativeMusicPlaybackRate = 1.0f;
bool gIOSNativeMusicTimeStretch = false;

NSString *NSStringFromUtf8(const std::string &utf8) {
  if (utf8.empty()) {
    return @"";
  }
  return [[NSString alloc] initWithBytes:utf8.data()
                                  length:utf8.size()
                                encoding:NSUTF8StringEncoding];
}

std::string NSStringToString(NSString *value) {
  if (value == nil) {
    return {};
  }
  const char *utf8 = [value UTF8String];
  return utf8 != nullptr ? std::string(utf8) : std::string();
}

bool IsUtf8ContinuationByte(unsigned char value) {
  return (value & 0b11000000) == 0b10000000;
}

std::size_t ClampUtf8ByteOffset(const std::string &utf8,
                                std::size_t byteOffset) {
  byteOffset = std::min(byteOffset, utf8.size());
  while (byteOffset < utf8.size() &&
         IsUtf8ContinuationByte(static_cast<unsigned char>(utf8[byteOffset]))) {
    ++byteOffset;
  }
  return byteOffset;
}

std::size_t Utf8ByteOffsetFromUtf16Offset(NSString *value,
                                          NSInteger utf16Offset) {
  if (value == nil) {
    return 0;
  }
  const NSInteger clampedOffset =
      std::max<NSInteger>(0, std::min<NSInteger>(utf16Offset, value.length));
  if (clampedOffset <= 0) {
    return 0;
  }
  NSString *prefix = [value substringToIndex:clampedOffset];
  return static_cast<std::size_t>(
      [prefix lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
}

NSInteger Utf16OffsetFromUtf8ByteOffset(NSString *value,
                                        std::size_t byteOffset) {
  if (value == nil) {
    return 0;
  }
  const std::string utf8 = NSStringToString(value);
  byteOffset = ClampUtf8ByteOffset(utf8, byteOffset);
  if (byteOffset <= 0) {
    return 0;
  }
  if (byteOffset >= utf8.size()) {
    return value.length;
  }
  NSString *prefix = [[NSString alloc] initWithBytes:utf8.data()
                                              length:byteOffset
                                            encoding:NSUTF8StringEncoding];
  return prefix != nil ? prefix.length : value.length;
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
  // Safe-area queries run every rendered frame. Reuse the app window while its
  // scene remains active and the window stays key and visible instead of
  // rebuilding connectedScenes collections on the hot path. The weak
  // reference cannot extend the UIKit window lifetime.
  static __weak UIWindow *cachedActiveWindow = nil;
  UIWindow *cachedWindow = cachedActiveWindow;
  if (@available(iOS 13.0, *)) {
    if (cachedWindow != nil && cachedWindow.windowScene != nil &&
        cachedWindow.windowScene.activationState ==
            UISceneActivationStateForegroundActive &&
        cachedWindow.isKeyWindow && !cachedWindow.hidden &&
        cachedWindow.alpha > 0.0) {
      return cachedWindow;
    }

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
          cachedActiveWindow = window;
          return window;
        }
      }
      if (windowScene.windows.count > 0) {
        UIWindow *window = windowScene.windows.firstObject;
        cachedActiveWindow = window;
        return window;
      }
    }
  }
  UIWindow *window = UIApplication.sharedApplication.keyWindow;
  cachedActiveWindow = window;
  return window;
}

void RestoreIOSViewportAfterKeyboardFocusOnce() {
  @autoreleasepool {
    UIWindow *window = FindActiveWindow();
    if (window == nil) {
      return;
    }

    UIViewController *rootController = window.rootViewController;
    UIView *rootView = rootController.view;
    if (rootController == nil || rootView == nil) {
      return;
    }

    @try {
      if ([rootController
              respondsToSelector:NSSelectorFromString(@"setKeyboardHeight:")]) {
        [rootController setValue:@0 forKey:@"keyboardHeight"];
      }
      if ([rootController
              respondsToSelector:NSSelectorFromString(@"setKeyboardVisible:")]) {
        [rootController setValue:@NO forKey:@"keyboardVisible"];
      }
    } @catch (NSException *exception) {
      (void)exception;
    }

    const CGRect windowBounds = window.bounds;
    if (!CGRectIsEmpty(windowBounds) &&
        !CGRectEqualToRect(rootView.frame, windowBounds)) {
      rootView.frame = windowBounds;
    }
    [rootView setNeedsLayout];
    [rootView layoutIfNeeded];
  }
}

int RoundedCGFloat(CGFloat value) {
  return static_cast<int>(std::lround(static_cast<double>(value)));
}

void OrientSizeToMatch(int &width, int &height, int referenceWidth,
                       int referenceHeight) {
  const bool candidateLandscape = width >= height;
  const bool referenceLandscape = referenceWidth >= referenceHeight;
  if (candidateLandscape != referenceLandscape) {
    std::swap(width, height);
  }
}

bool SimilarAspect(int width, int height, int referenceWidth,
                   int referenceHeight) {
  if (width <= 0 || height <= 0 || referenceWidth <= 0 || referenceHeight <= 0) {
    return false;
  }
  const double aspect = static_cast<double>(width) / static_cast<double>(height);
  const double referenceAspect =
      static_cast<double>(referenceWidth) / static_cast<double>(referenceHeight);
  return std::abs(aspect - referenceAspect) <= referenceAspect * 0.02;
}

void ConsiderDrawableCandidate(int candidateWidth, int candidateHeight,
                               int referenceWidth, int referenceHeight,
                               int &bestWidth, int &bestHeight) {
  OrientSizeToMatch(candidateWidth, candidateHeight, referenceWidth,
                    referenceHeight);
  if (!SimilarAspect(candidateWidth, candidateHeight, referenceWidth,
                     referenceHeight)) {
    return;
  }
  if (candidateWidth <= bestWidth + 8 || candidateHeight <= bestHeight + 8) {
    return;
  }
  bestWidth = candidateWidth;
  bestHeight = candidateHeight;
}

UIViewController *TopViewController(UIViewController *viewController) {
  UIViewController *top = viewController;
  while (top.presentedViewController != nil) {
    top = top.presentedViewController;
  }
  return top;
}

platform::PhotoAuthorizationStatus
ToPhotoAuthorizationStatus(PHAuthorizationStatus status) {
  switch (status) {
  case PHAuthorizationStatusNotDetermined:
    return platform::PhotoAuthorizationStatus::NotDetermined;
  case PHAuthorizationStatusRestricted:
    return platform::PhotoAuthorizationStatus::Restricted;
  case PHAuthorizationStatusDenied:
    return platform::PhotoAuthorizationStatus::Denied;
  case PHAuthorizationStatusAuthorized:
    return platform::PhotoAuthorizationStatus::Authorized;
  default:
    if (@available(iOS 14.0, *)) {
      if (status == PHAuthorizationStatusLimited) {
        return platform::PhotoAuthorizationStatus::Limited;
      }
    }
    return platform::PhotoAuthorizationStatus::Restricted;
  }
}

void OpenApplicationSettings() {
  void (^openBlock)(void) = ^{
    NSURL *settingsURL =
        [NSURL URLWithString:UIApplicationOpenSettingsURLString];
    if (settingsURL != nil &&
        [[UIApplication sharedApplication] canOpenURL:settingsURL]) {
      [[UIApplication sharedApplication] openURL:settingsURL
                                         options:@{}
                               completionHandler:nil];
    }
  };

  if ([NSThread isMainThread]) {
    openBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), openBlock);
  }
}

bool CreateFullFrameRatePlaybackVideoForPhotos(NSString *sourcePath,
                                               NSString **preparedPath,
                                               std::string &errorMessage) {
  *preparedPath = nil;

#if defined(__IPHONE_OS_VERSION_MAX_ALLOWED) &&                                \
    __IPHONE_OS_VERSION_MAX_ALLOWED >= 180000
  if (@available(iOS 18.0, *)) {
    // Photos needs this as typed QuickTime metadata, not FFmpeg string mdta.
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

  switch (platform::photoAuthorizationAction(
      ToPhotoAuthorizationStatus(status))) {
  case platform::PhotoAuthorizationAction::Proceed:
    return true;
  case platform::PhotoAuthorizationAction::OpenSettings:
    errorMessage =
        "Photos access is off. Enable Photos access in Settings to export";
    OpenApplicationSettings();
    return false;
  case platform::PhotoAuthorizationAction::ExplainRestriction:
    errorMessage = "Photos access is restricted and cannot be changed here";
    return false;
  case platform::PhotoAuthorizationAction::Request:
    errorMessage = "Photos permission was not granted";
    return false;
  }
  errorMessage = "Photos permission was not granted";
  return false;
}

long long ElapsedMicros(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - start)
      .count();
}

std::string NSErrorMessage(NSError *error, const char *fallback) {
  if (error == nil) {
    return fallback != nullptr ? std::string(fallback) : std::string();
  }
  NSString *description = [error localizedDescription];
  const std::string message = NSStringToString(description);
  if (!message.empty()) {
    return message;
  }
  return fallback != nullptr ? std::string(fallback) : std::string();
}

std::string NSExceptionMessage(NSException *exception, const char *fallback) {
  if (exception == nil) {
    return fallback != nullptr ? std::string(fallback) : std::string();
  }
  std::string message = NSStringToString(exception.name);
  const std::string reason = NSStringToString(exception.reason);
  if (!reason.empty()) {
    if (!message.empty()) {
      message += ": ";
    }
    message += reason;
  }
  if (!message.empty()) {
    return message;
  }
  return fallback != nullptr ? std::string(fallback) : std::string();
}

std::string AVWriterErrorMessage(AVAssetWriter *writer,
                                 const char *fallback) {
  if (writer != nil && writer.error != nil) {
    return NSErrorMessage(writer.error, fallback);
  }
  return fallback != nullptr ? std::string(fallback) : std::string();
}

std::string AVWriterStatusName(AVAssetWriterStatus status) {
  switch (status) {
  case AVAssetWriterStatusUnknown:
    return "unknown";
  case AVAssetWriterStatusWriting:
    return "writing";
  case AVAssetWriterStatusCompleted:
    return "completed";
  case AVAssetWriterStatusFailed:
    return "failed";
  case AVAssetWriterStatusCancelled:
    return "cancelled";
  }
  return "invalid";
}

NSObject *IOSNativeMusicLock() {
  static NSObject *lock = [NSObject new];
  return lock;
}

long long IOSNativeMusicDurationMicrosLocked() {
  if (gIOSNativeMusicPlayer == nil) {
    return 0;
  }
  if (gIOSNativeMusicMetadata.durationMicros > 0) {
    return gIOSNativeMusicMetadata.durationMicros;
  }
  const NSTimeInterval duration =
      CMTimeGetSeconds(gIOSNativeMusicPlayer.currentItem.duration);
  return std::isfinite(duration)
             ? static_cast<long long>(
                   std::max<NSTimeInterval>(0.0, duration) * 1000000.0)
             : 0;
}

bool IOSNativeMusicIsPlayingLocked() {
  return gIOSNativeMusicPlayer != nil && gIOSNativeMusicPlayer.rate != 0.0f;
}

NSTimeInterval IOSNativeMusicCurrentTimeLocked() {
  if (gIOSNativeMusicPlayer == nil) {
    return 0.0;
  }
  const NSTimeInterval currentTime =
      CMTimeGetSeconds(gIOSNativeMusicPlayer.currentTime);
  return std::isfinite(currentTime)
             ? std::max<NSTimeInterval>(0.0, currentTime)
             : 0.0;
}

void ApplyIOSNativeMusicPlaybackModeLocked() {
  if (gIOSNativeMusicPlayer.currentItem == nil) {
    return;
  }
  gIOSNativeMusicPlayer.currentItem.audioTimePitchAlgorithm =
      gIOSNativeMusicTimeStretch ? AVAudioTimePitchAlgorithmSpectral
                                 : AVAudioTimePitchAlgorithmVarispeed;
}

void UpdateIOSNativeMusicNowPlayingInfoLocked() {
  @autoreleasepool {
    MPNowPlayingInfoCenter *center = [MPNowPlayingInfoCenter defaultCenter];
    if (gIOSNativeMusicPlayer == nil) {
      center.nowPlayingInfo = nil;
      if (@available(iOS 13.0, *)) {
        center.playbackState = MPNowPlayingPlaybackStateStopped;
      }
      return;
    }

    NSMutableDictionary *info = [NSMutableDictionary dictionary];
    NSString *title = NSStringFromUtf8(gIOSNativeMusicMetadata.title);
    NSString *artist = NSStringFromUtf8(gIOSNativeMusicMetadata.artist);
    NSString *album = NSStringFromUtf8(gIOSNativeMusicMetadata.album);
    NSString *artworkPath = NSStringFromUtf8(gIOSNativeMusicMetadata.artworkPath);
    if (title != nil && title.length > 0) {
      info[MPMediaItemPropertyTitle] = title;
    } else {
      info[MPMediaItemPropertyTitle] = @"AsoBMaShow";
    }
    if (artist != nil && artist.length > 0) {
      info[MPMediaItemPropertyArtist] = artist;
    }
    if (album != nil && album.length > 0) {
      info[MPMediaItemPropertyAlbumTitle] = album;
    }
    if (artworkPath != nil && artworkPath.length > 0) {
      UIImage *artworkImage = [UIImage imageWithContentsOfFile:artworkPath];
      if (artworkImage != nil) {
        MPMediaItemArtwork *artwork =
            [[MPMediaItemArtwork alloc]
                initWithBoundsSize:artworkImage.size
                     requestHandler:^UIImage *(CGSize size) {
                       (void)size;
                       return artworkImage;
                     }];
        info[MPMediaItemPropertyArtwork] = artwork;
      }
    }

    const long long durationMicros = IOSNativeMusicDurationMicrosLocked();
    if (durationMicros > 0) {
      info[MPMediaItemPropertyPlaybackDuration] =
          @(static_cast<double>(durationMicros) / 1000000.0);
    }
    info[MPNowPlayingInfoPropertyElapsedPlaybackTime] =
        @(IOSNativeMusicCurrentTimeLocked());
    info[MPNowPlayingInfoPropertyPlaybackRate] =
        @(IOSNativeMusicIsPlayingLocked() ? gIOSNativeMusicPlaybackRate
                                          : 0.0f);
    const NSUInteger queueCount =
        static_cast<NSUInteger>(gIOSNativeMusicQueue.items.size());
    if (queueCount > 0) {
      info[MPNowPlayingInfoPropertyPlaybackQueueCount] = @(queueCount);
      if (gIOSNativeMusicQueue.currentIndex >= 0 &&
          static_cast<NSUInteger>(gIOSNativeMusicQueue.currentIndex) <
              queueCount) {
        info[MPNowPlayingInfoPropertyPlaybackQueueIndex] =
            @(gIOSNativeMusicQueue.currentIndex);
      }
    }
    center.nowPlayingInfo = info;
    if (@available(iOS 13.0, *)) {
      center.playbackState = IOSNativeMusicIsPlayingLocked()
                                 ? MPNowPlayingPlaybackStatePlaying
                                 : MPNowPlayingPlaybackStatePaused;
    }
  }
}

bool ActivateIOSNativeMusicAudioSession(std::string &errorMessage) {
  AVAudioSession *session = [AVAudioSession sharedInstance];
  NSError *error = nil;
  if (![session setCategory:AVAudioSessionCategoryPlayback error:&error]) {
    errorMessage = NSErrorMessage(error, "Could not configure audio session");
    return false;
  }
  error = nil;
  if (![session setActive:YES error:&error]) {
    errorMessage = NSErrorMessage(error, "Could not activate audio session");
    return false;
  }
  return true;
}

bool RestoreIOSForegroundAudioSession(std::string &errorMessage) {
  AVAudioSession *session = [AVAudioSession sharedInstance];
  NSError *error = nil;
  if (![session setCategory:AVAudioSessionCategoryAmbient
                withOptions:AVAudioSessionCategoryOptionMixWithOthers
                      error:&error]) {
    errorMessage =
        NSErrorMessage(error, "Could not restore foreground audio session");
    return false;
  }
  error = nil;
  if (![session setActive:YES error:&error]) {
    errorMessage = NSErrorMessage(error, "Could not restore audio session");
    return false;
  }
  return true;
}

void ConfigureIOSNativeMusicRemoteCommands() {
  if (gIOSNativeMusicRemoteCommandsConfigured) {
    return;
  }
  gIOSNativeMusicRemoteCommandsConfigured = true;

  MPRemoteCommandCenter *center = [MPRemoteCommandCenter sharedCommandCenter];
  center.playCommand.enabled = YES;
  [center.playCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
                              MPRemoteCommandEvent *) {
    std::string errorMessage;
    return PlayIOSNativeMusic(errorMessage)
               ? MPRemoteCommandHandlerStatusSuccess
               : MPRemoteCommandHandlerStatusCommandFailed;
  }];

  center.pauseCommand.enabled = YES;
  [center.pauseCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
                               MPRemoteCommandEvent *) {
    std::string errorMessage;
    return PauseIOSNativeMusic(errorMessage)
               ? MPRemoteCommandHandlerStatusSuccess
               : MPRemoteCommandHandlerStatusCommandFailed;
  }];

  center.togglePlayPauseCommand.enabled = YES;
  [center.togglePlayPauseCommand
      addTargetWithHandler:^MPRemoteCommandHandlerStatus(
          MPRemoteCommandEvent *) {
        @synchronized(IOSNativeMusicLock()) {
          std::string errorMessage;
          if (IOSNativeMusicIsPlayingLocked()) {
            return PauseIOSNativeMusic(errorMessage)
                       ? MPRemoteCommandHandlerStatusSuccess
                       : MPRemoteCommandHandlerStatusCommandFailed;
          }
          return PlayIOSNativeMusic(errorMessage)
                     ? MPRemoteCommandHandlerStatusSuccess
                     : MPRemoteCommandHandlerStatusCommandFailed;
        }
      }];

  center.stopCommand.enabled = YES;
  [center.stopCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(
                              MPRemoteCommandEvent *) {
    std::string errorMessage;
    return StopIOSNativeMusic(errorMessage)
               ? MPRemoteCommandHandlerStatusSuccess
               : MPRemoteCommandHandlerStatusCommandFailed;
  }];

  center.previousTrackCommand.enabled = YES;
  [center.previousTrackCommand
      addTargetWithHandler:^MPRemoteCommandHandlerStatus(
          MPRemoteCommandEvent *) {
        native_music_player::NotifyControlEvent(
            native_music_player::ControlEvent::Previous);
        return MPRemoteCommandHandlerStatusSuccess;
      }];

  center.nextTrackCommand.enabled = YES;
  [center.nextTrackCommand
      addTargetWithHandler:^MPRemoteCommandHandlerStatus(
          MPRemoteCommandEvent *) {
        native_music_player::NotifyControlEvent(
            native_music_player::ControlEvent::Next);
        return MPRemoteCommandHandlerStatusSuccess;
      }];

  center.changePlaybackPositionCommand.enabled = YES;
  [center.changePlaybackPositionCommand
      addTargetWithHandler:^MPRemoteCommandHandlerStatus(
          MPRemoteCommandEvent *event) {
        if (![event
                isKindOfClass:[MPChangePlaybackPositionCommandEvent class]]) {
          return MPRemoteCommandHandlerStatusCommandFailed;
        }
        MPChangePlaybackPositionCommandEvent *positionEvent =
            (MPChangePlaybackPositionCommandEvent *)event;
        std::string errorMessage;
        const long long positionMicros = static_cast<long long>(
            std::max<NSTimeInterval>(0.0, positionEvent.positionTime) *
            1000000.0);
        return SeekIOSNativeMusic(positionMicros, errorMessage)
                   ? MPRemoteCommandHandlerStatusSuccess
                   : MPRemoteCommandHandlerStatusCommandFailed;
      }];
}

bool WaitForWriterInput(AVAssetWriter *writer, AVAssetWriterInput *input,
                        const char *inputName, NSTimeInterval timeoutSeconds,
                        std::string &errorMessage) {
  const std::string inputLabel =
      inputName != nullptr ? std::string(inputName) : std::string("unknown");
  if (writer == nil) {
    errorMessage = "Replay video writer is unavailable while waiting for " +
                   inputLabel + " input";
    return false;
  }
  if (input == nil) {
    errorMessage = "Replay video writer " + inputLabel +
                   " input is unavailable";
    return false;
  }
  NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSeconds];
  while (![input isReadyForMoreMediaData]) {
    if (writer.status == AVAssetWriterStatusFailed) {
      const std::string fallback =
          "Replay video writer failed while waiting for " + inputLabel +
          " input";
      errorMessage = AVWriterErrorMessage(writer, fallback.c_str());
      return false;
    }
    if (writer.status == AVAssetWriterStatusCancelled) {
      errorMessage =
          "Replay video writer was cancelled while waiting for " +
          inputLabel + " input";
      return false;
    }
    if (writer.status == AVAssetWriterStatusCompleted) {
      errorMessage =
          "Replay video writer already finished while waiting for " +
          inputLabel + " input";
      return false;
    }
    if ([deadline timeIntervalSinceNow] <= 0.0) {
      errorMessage =
          "Timed out waiting for replay video writer " + inputLabel +
          " input; status=" + AVWriterStatusName(writer.status);
      return false;
    }
    [NSThread sleepForTimeInterval:0.001];
  }
  return true;
}

void AppendReplayFrameContext(std::string &errorMessage, size_t frameIndex) {
  errorMessage += " at frame " + std::to_string(frameIndex);
}

class IOSReplayVideoWriter {
public:
  ~IOSReplayVideoWriter() { releasePendingAudioSample(); }

  bool open(const std::string &wavPath, const std::string &outputPath,
            int width, int height, int fps, int64_t bitRate,
            std::string &errorMessage) {
    @autoreleasepool {
      this->wavPath = wavPath;
      this->width = width;
      this->height = height;
      this->fps = fps;

      NSString *outputPathString = NSStringFromUtf8(outputPath);
      if (outputPathString == nil || outputPathString.length == 0) {
        errorMessage = "Replay video output path is empty";
        return false;
      }
      outputURL = [NSURL fileURLWithPath:outputPathString];
      [[NSFileManager defaultManager] removeItemAtURL:outputURL error:nil];

      NSError *error = nil;
      writer = [AVAssetWriter assetWriterWithURL:outputURL
                                        fileType:AVFileTypeMPEG4
                                           error:&error];
      if (writer == nil) {
        errorMessage =
            NSErrorMessage(error, "Failed to create replay video writer");
        return false;
      }

      NSDictionary *compressionProperties = @{
        AVVideoAverageBitRateKey : @(std::max<int64_t>(bitRate, 1)),
        AVVideoExpectedSourceFrameRateKey : @(std::max(fps, 1)),
        AVVideoMaxKeyFrameIntervalKey : @(std::max(fps * 2, 1)),
        AVVideoAllowFrameReorderingKey : @NO,
      };
      NSDictionary *videoSettings = @{
        AVVideoCodecKey : AVVideoCodecTypeHEVC,
        AVVideoWidthKey : @(width),
        AVVideoHeightKey : @(height),
        AVVideoCompressionPropertiesKey : compressionProperties,
      };
      videoInput = [AVAssetWriterInput
          assetWriterInputWithMediaType:AVMediaTypeVideo
                         outputSettings:videoSettings];
      if (videoInput == nil) {
        errorMessage = "Replay video writer could not create video input";
        return false;
      }
      videoInput.expectsMediaDataInRealTime = NO;
      videoInput.mediaTimeScale = std::max(fps, 1);
      if (![writer canAddInput:videoInput]) {
        errorMessage =
            "Replay video writer could not add video input (" +
            std::to_string(width) + "x" + std::to_string(height) + " @ " +
            std::to_string(fps) + "fps, bitrate " + std::to_string(bitRate) +
            ")";
        return false;
      }
      [writer addInput:videoInput];

      NSDictionary *pixelBufferAttributes = @{
        (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey :
            @(kCVPixelFormatType_32BGRA),
        (__bridge NSString *)kCVPixelBufferWidthKey : @(width),
        (__bridge NSString *)kCVPixelBufferHeightKey : @(height),
        (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey : @{},
      };
      adaptor = [AVAssetWriterInputPixelBufferAdaptor
          assetWriterInputPixelBufferAdaptorWithAssetWriterInput:videoInput
                                     sourcePixelBufferAttributes:
                                         pixelBufferAttributes];
      if (adaptor == nil) {
        errorMessage = "Replay video writer could not create pixel adaptor";
        return false;
      }

      if (!openAudioReader(errorMessage)) {
        return false;
      }
      if (audioReaderOutput != nil) {
        NSDictionary *audioSettings = @{
          AVFormatIDKey : @(kAudioFormatMPEG4AAC),
          AVSampleRateKey : @(kIOSReplaySampleRate),
          AVNumberOfChannelsKey : @(kIOSReplayChannels),
          AVEncoderBitRateKey : @(192000),
        };
        audioInput = [AVAssetWriterInput
            assetWriterInputWithMediaType:AVMediaTypeAudio
                           outputSettings:audioSettings];
        if (audioInput == nil) {
          errorMessage = "Replay video writer could not create audio input";
          return false;
        }
        audioInput.expectsMediaDataInRealTime = NO;
        if (![writer canAddInput:audioInput]) {
          errorMessage = "Replay video writer could not add audio input";
          return false;
        }
        [writer addInput:audioInput];
      }

      if (![writer startWriting]) {
        errorMessage =
            AVWriterErrorMessage(writer, "Failed to start replay video writer");
        return false;
      }
      [writer startSessionAtSourceTime:kCMTimeZero];
      return true;
    }
  }

  bool appendFrame(const uint8_t *bgraFrame, size_t frameIndex,
                   std::string &errorMessage) {
    @autoreleasepool {
      if (bgraFrame == nullptr) {
        errorMessage = "Replay video frame is empty";
        AppendReplayFrameContext(errorMessage, frameIndex);
        return false;
      }
      if (!WaitForWriterInput(writer, videoInput, "video",
                              kIOSReplayInputWaitTimeoutSeconds,
                              errorMessage)) {
        AppendReplayFrameContext(errorMessage, frameIndex);
        return false;
      }
      if (adaptor.pixelBufferPool == nullptr) {
        errorMessage = "Replay video writer pixel buffer pool is unavailable";
        AppendReplayFrameContext(errorMessage, frameIndex);
        return false;
      }

      CVPixelBufferRef pixelBuffer = nullptr;
      CVReturn ret = CVPixelBufferPoolCreatePixelBuffer(
          kCFAllocatorDefault, adaptor.pixelBufferPool, &pixelBuffer);
      if (ret != kCVReturnSuccess || pixelBuffer == nullptr) {
        errorMessage = "Failed to allocate replay video pixel buffer";
        AppendReplayFrameContext(errorMessage, frameIndex);
        return false;
      }

      const auto copyStart = std::chrono::steady_clock::now();
      CVPixelBufferLockBaseAddress(pixelBuffer, 0);
      auto *destination =
          static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(pixelBuffer));
      const size_t destinationStride = CVPixelBufferGetBytesPerRow(pixelBuffer);
      const size_t sourceStride = static_cast<size_t>(width) * 4ULL;
      for (int y = 0; y < height; ++y) {
        std::memcpy(destination + static_cast<size_t>(y) * destinationStride,
                    bgraFrame + static_cast<size_t>(y) * sourceStride,
                    sourceStride);
      }
      CVPixelBufferUnlockBaseAddress(pixelBuffer, 0);
      profile.videoPixelBufferCopyMicros += ElapsedMicros(copyStart);

      const auto appendStart = std::chrono::steady_clock::now();
      const CMTime presentationTime =
          CMTimeMake(static_cast<int64_t>(frameIndex), std::max(fps, 1));
      const BOOL appended =
          [adaptor appendPixelBuffer:pixelBuffer
                 withPresentationTime:presentationTime];
      CVPixelBufferRelease(pixelBuffer);
      profile.videoAppendMicros += ElapsedMicros(appendStart);
      if (!appended) {
        errorMessage =
            AVWriterErrorMessage(writer, "Failed to append replay video frame");
        AppendReplayFrameContext(errorMessage, frameIndex);
        return false;
      }

      const CMTime audioLeadTarget = CMTimeAdd(
          presentationTime,
          CMTimeMakeWithSeconds(kIOSReplayAudioLeadSeconds, std::max(fps, 1)));
      if (!appendAudioThrough(audioLeadTarget, errorMessage)) {
        errorMessage += " while interleaving replay audio";
        AppendReplayFrameContext(errorMessage, frameIndex);
        return false;
      }
      return true;
    }
  }

  bool finish(std::string &errorMessage) {
    @autoreleasepool {
      if (!videoFinished) {
        [videoInput markAsFinished];
        videoFinished = true;
      }
      if (!appendRemainingAudio(errorMessage)) {
        [writer cancelWriting];
        return false;
      }
      if (audioInput != nil && !audioFinished) {
        [audioInput markAsFinished];
        audioFinished = true;
      }

      __block BOOL finished = NO;
      __block AVAssetWriterStatus finishStatus = AVAssetWriterStatusUnknown;
      __block NSError *finishError = nil;
      dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
      const auto finishStart = std::chrono::steady_clock::now();
      [writer finishWritingWithCompletionHandler:^{
        finishStatus = writer.status;
        finishError = writer.error;
        finished = YES;
        dispatch_semaphore_signal(semaphore);
      }];
      const long waitResult = dispatch_semaphore_wait(
          semaphore,
          dispatch_time(DISPATCH_TIME_NOW,
                        static_cast<int64_t>(
                            kIOSReplayFinishWaitTimeoutSeconds * NSEC_PER_SEC)));
      profile.finishMicros += ElapsedMicros(finishStart);
      if (waitResult != 0) {
        [writer cancelWriting];
        errorMessage = "Timed out finishing replay video writer; status=" +
                       AVWriterStatusName(writer.status);
        return false;
      }
      if (!finished || finishStatus != AVAssetWriterStatusCompleted) {
        errorMessage =
            NSErrorMessage(finishError, "Failed to finish replay video writer");
        return false;
      }
      return true;
    }
  }

  void cancel() {
    if (writer != nil && writer.status == AVAssetWriterStatusWriting) {
      [writer cancelWriting];
    }
    if (audioReader != nil && audioReader.status == AVAssetReaderStatusReading) {
      [audioReader cancelReading];
    }
    releasePendingAudioSample();
  }

  IOSReplayVideoWriterProfile profile;

private:
  bool openAudioReader(std::string &errorMessage) {
    NSString *path = NSStringFromUtf8(wavPath);
    if (path == nil || path.length == 0) {
      errorMessage = "Replay audio path is empty";
      return false;
    }
    NSURL *url = [NSURL fileURLWithPath:path];
    AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
    NSArray<AVAssetTrack *> *tracks =
        [asset tracksWithMediaType:AVMediaTypeAudio];
    if (tracks.count == 0) {
      return true;
    }

    NSError *error = nil;
    audioReader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
    if (audioReader == nil) {
      errorMessage = NSErrorMessage(error, "Failed to read replay audio");
      return false;
    }

    NSDictionary *readerSettings = @{
      AVFormatIDKey : @(kAudioFormatLinearPCM),
      AVLinearPCMBitDepthKey : @(16),
      AVLinearPCMIsBigEndianKey : @NO,
      AVLinearPCMIsFloatKey : @NO,
      AVLinearPCMIsNonInterleaved : @NO,
    };
    audioReaderOutput =
        [[AVAssetReaderTrackOutput alloc] initWithTrack:tracks.firstObject
                                         outputSettings:readerSettings];
    audioReaderOutput.alwaysCopiesSampleData = NO;
    if (![audioReader canAddOutput:audioReaderOutput]) {
      errorMessage = "Replay audio reader could not add output";
      return false;
    }
    [audioReader addOutput:audioReaderOutput];
    if (![audioReader startReading]) {
      errorMessage = NSErrorMessage(audioReader.error,
                                    "Failed to start replay audio reader");
      return false;
    }
    return true;
  }

  bool appendAudioThrough(CMTime targetTime, std::string &errorMessage) {
    if (audioInput == nil || audioReaderOutput == nil || audioFinished) {
      return true;
    }
    if (!CMTIME_IS_VALID(targetTime)) {
      errorMessage = "Replay audio target time is invalid";
      return false;
    }
    if (CMTIME_IS_VALID(audioQueuedThroughTime) &&
        CMTimeCompare(audioQueuedThroughTime, targetTime) >= 0) {
      return true;
    }

    const auto audioStart = std::chrono::steady_clock::now();
    while (!CMTIME_IS_VALID(audioQueuedThroughTime) ||
           CMTimeCompare(audioQueuedThroughTime, targetTime) < 0) {
      CMSampleBufferRef sampleBuffer = pendingAudioSample;
      pendingAudioSample = nullptr;
      if (sampleBuffer == nullptr) {
        sampleBuffer = [audioReaderOutput copyNextSampleBuffer];
      }
      if (sampleBuffer == nullptr) {
        profile.audioAppendMicros += ElapsedMicros(audioStart);
        return handleAudioReaderEnd(errorMessage);
      }

      if (!appendAudioSample(sampleBuffer, errorMessage)) {
        CFRelease(sampleBuffer);
        return false;
      }
      updateAudioQueuedThroughTime(sampleBuffer);
      CFRelease(sampleBuffer);
    }

    profile.audioAppendMicros += ElapsedMicros(audioStart);
    return true;
  }

  bool appendRemainingAudio(std::string &errorMessage) {
    if (audioInput == nil || audioReaderOutput == nil || audioFinished) {
      return true;
    }

    const auto audioStart = std::chrono::steady_clock::now();
    while (!audioFinished) {
      CMSampleBufferRef sampleBuffer = pendingAudioSample;
      pendingAudioSample = nullptr;
      if (sampleBuffer == nullptr) {
        sampleBuffer = [audioReaderOutput copyNextSampleBuffer];
      }
      if (sampleBuffer == nullptr) {
        profile.audioAppendMicros += ElapsedMicros(audioStart);
        return handleAudioReaderEnd(errorMessage);
      }

      if (!appendAudioSample(sampleBuffer, errorMessage)) {
        CFRelease(sampleBuffer);
        return false;
      }
      updateAudioQueuedThroughTime(sampleBuffer);
      CFRelease(sampleBuffer);
    }

    profile.audioAppendMicros += ElapsedMicros(audioStart);
    return true;
  }

  bool appendAudioSample(CMSampleBufferRef sampleBuffer,
                         std::string &errorMessage) {
    const bool ready =
        WaitForWriterInput(writer, audioInput, "audio",
                           kIOSReplayInputWaitTimeoutSeconds, errorMessage);
    BOOL appended = NO;
    if (ready) {
      appended = [audioInput appendSampleBuffer:sampleBuffer];
    }
    if (!ready) {
      return false;
    }
    if (!appended) {
      errorMessage =
          AVWriterErrorMessage(writer, "Failed to append replay audio");
      return false;
    }
    return true;
  }

  bool handleAudioReaderEnd(std::string &errorMessage) {
    if (audioReader.status == AVAssetReaderStatusFailed) {
      errorMessage = NSErrorMessage(audioReader.error,
                                    "Failed to finish replay audio reader");
      return false;
    }
    if (audioReader.status == AVAssetReaderStatusCancelled) {
      errorMessage = "Replay audio reader was cancelled";
      return false;
    }
    if (audioInput != nil && !audioFinished) {
      [audioInput markAsFinished];
    }
    audioFinished = true;
    return true;
  }

  void updateAudioQueuedThroughTime(CMSampleBufferRef sampleBuffer) {
    const CMItemCount sampleCount = CMSampleBufferGetNumSamples(sampleBuffer);
    if (sampleCount > 0) {
      audioQueuedSampleCount += static_cast<int64_t>(sampleCount);
      audioQueuedThroughTime =
          CMTimeMake(audioQueuedSampleCount, kIOSReplaySampleRate);
      return;
    }

    CMTime sampleEnd = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    const CMTime duration = CMSampleBufferGetDuration(sampleBuffer);
    if (CMTIME_IS_VALID(sampleEnd) && CMTIME_IS_VALID(duration)) {
      sampleEnd = CMTimeAdd(sampleEnd, duration);
    }
    if (CMTIME_IS_VALID(sampleEnd) &&
        (!CMTIME_IS_VALID(audioQueuedThroughTime) ||
         CMTimeCompare(sampleEnd, audioQueuedThroughTime) > 0)) {
      audioQueuedThroughTime = sampleEnd;
    }
  }

  void releasePendingAudioSample() {
    if (pendingAudioSample != nullptr) {
      CFRelease(pendingAudioSample);
      pendingAudioSample = nullptr;
    }
  }

  std::string wavPath;
  int width = 0;
  int height = 0;
  int fps = 0;
  NSURL *outputURL = nil;
  AVAssetWriter *writer = nil;
  AVAssetWriterInput *videoInput = nil;
  AVAssetWriterInput *audioInput = nil;
  AVAssetWriterInputPixelBufferAdaptor *adaptor = nil;
  AVAssetReader *audioReader = nil;
  AVAssetReaderTrackOutput *audioReaderOutput = nil;
  CMSampleBufferRef pendingAudioSample = nullptr;
  CMTime audioQueuedThroughTime = kCMTimeInvalid;
  int64_t audioQueuedSampleCount = 0;
  bool videoFinished = false;
  bool audioFinished = false;
};
} // namespace

bool RequestIOSPhotoAddAuthorization(std::string &errorMessage) {
  return RequestPhotoAddAuthorization(errorMessage);
}

@interface AsoNativeTextEditorView : UIView <UITextFieldDelegate> {
@private
  UITextField *_textField;
  __unsafe_unretained UIView *_containerView;
  std::size_t _initialSelectionStart;
  std::size_t _initialSelectionEnd;
  void *_context;
  IOSNativeTextEditorCallback _callback;
  CGRect _lastKeyboardFrame;
  BOOL _keyboardVisible;
  BOOL _hiding;
}
- (instancetype)initWithConfig:(const IOSNativeTextEditorConfig &)config
                       context:(void *)context
                      callback:(IOSNativeTextEditorCallback)callback;
- (void *)context;
- (void)showInView:(UIView *)containerView;
- (void)hideWithNotifyFinished:(BOOL)notifyFinished;
- (void)keyboardFrameChanged:(NSNotification *)notification;
- (void)keyboardWillHide:(NSNotification *)notification;
- (void)setSelectionStart:(std::size_t)selectionStart
                      end:(std::size_t)selectionEnd;
- (void)setState:(const IOSNativeTextEditorState &)state;
- (UIViewAnimationOptions)animationOptionsForKeyboardNotification:
    (NSNotification *)notification;
- (void)updateFrameAnimated:(BOOL)animated
                   duration:(NSTimeInterval)duration
                    options:(UIViewAnimationOptions)options;
- (void)textFieldEditingChanged:(UITextField *)textField;
- (void)textFieldDidChangeSelection:(UITextField *)textField;
- (void)emitEvent:(IOSNativeTextEditorEvent)event;
@end

static AsoNativeTextEditorView *gNativeTextEditor = nil;
static constexpr CGFloat kNativeTextEditorHeight = 54.0;
static constexpr CGFloat kNativeTextEditorHorizontalPadding = 8.0;
static constexpr CGFloat kNativeTextEditorVerticalPadding = 6.0;

@implementation AsoNativeTextEditorView
- (instancetype)initWithConfig:(const IOSNativeTextEditorConfig &)config
                       context:(void *)context
                      callback:(IOSNativeTextEditorCallback)callback {
  self = [super initWithFrame:CGRectZero];
  if (self == nil) {
    return nil;
  }

  _context = context;
  _callback = callback;
  _initialSelectionStart = config.selectionStart;
  _initialSelectionEnd = config.selectionEnd;
  _lastKeyboardFrame = CGRectZero;
  _keyboardVisible = NO;
  _hiding = NO;

  self.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleTopMargin;
  self.backgroundColor =
      [UIColor colorWithWhite:0.08 alpha:0.96];

  _textField = [[UITextField alloc] initWithFrame:CGRectZero];
  _textField.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  _textField.borderStyle = UITextBorderStyleRoundedRect;
  if (@available(iOS 13.0, *)) {
    _textField.backgroundColor = UIColor.systemBackgroundColor;
    _textField.textColor = UIColor.labelColor;
  } else {
    _textField.backgroundColor = UIColor.whiteColor;
    _textField.textColor = UIColor.blackColor;
  }
  _textField.tintColor = UIColor.systemBlueColor;
  _textField.font =
      [UIFont systemFontOfSize:std::max(12, config.fontSize)];
  _textField.clearButtonMode = UITextFieldViewModeWhileEditing;
  _textField.autocorrectionType = UITextAutocorrectionTypeNo;
  _textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
  _textField.spellCheckingType = UITextSpellCheckingTypeNo;
  _textField.smartDashesType = UITextSmartDashesTypeNo;
  _textField.smartQuotesType = UITextSmartQuotesTypeNo;
  _textField.returnKeyType = UIReturnKeyDone;
  _textField.enablesReturnKeyAutomatically = NO;
  _textField.delegate = self;
  NSString *text = NSStringFromUtf8(config.text);
  _textField.text = text != nil ? text : @"";
  _textField.placeholder = NSStringFromUtf8(config.placeholder);
  [_textField addTarget:self
                 action:@selector(textFieldEditingChanged:)
       forControlEvents:UIControlEventEditingChanged];
  [self addSubview:_textField];

  return self;
}

- (void *)context {
  return _context;
}

- (void)showInView:(UIView *)containerView {
  if (containerView == nil) {
    return;
  }
  _containerView = containerView;
  [containerView addSubview:self];
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(keyboardFrameChanged:)
             name:UIKeyboardWillChangeFrameNotification
           object:nil];
  [[NSNotificationCenter defaultCenter]
      addObserver:self
         selector:@selector(keyboardWillHide:)
             name:UIKeyboardWillHideNotification
           object:nil];
  [self updateFrameAnimated:NO duration:0.0 options:0];
  [_textField becomeFirstResponder];
  [self setSelectionStart:_initialSelectionStart end:_initialSelectionEnd];
}

- (void)dealloc {
  [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)layoutSubviews {
  [super layoutSubviews];
  const CGFloat fieldX = kNativeTextEditorHorizontalPadding;
  const CGFloat fieldY = kNativeTextEditorVerticalPadding;
  const CGFloat fieldWidth =
      std::max<CGFloat>(1.0, self.bounds.size.width -
                                 2.0 * kNativeTextEditorHorizontalPadding);
  const CGFloat fieldHeight =
      std::max<CGFloat>(1.0, self.bounds.size.height -
                                 2.0 * kNativeTextEditorVerticalPadding);
  _textField.frame = CGRectMake(fieldX, fieldY, fieldWidth, fieldHeight);
}

- (void)keyboardFrameChanged:(NSNotification *)notification {
  NSValue *frameValue =
      notification.userInfo[UIKeyboardFrameEndUserInfoKey];
  if (frameValue != nil) {
    _lastKeyboardFrame = [frameValue CGRectValue];
    _keyboardVisible = YES;
  }
  NSTimeInterval duration =
      [notification.userInfo[UIKeyboardAnimationDurationUserInfoKey]
          doubleValue];
  UIViewAnimationOptions options =
      [self animationOptionsForKeyboardNotification:notification];
  [self updateFrameAnimated:YES duration:duration options:options];
}

- (void)keyboardWillHide:(NSNotification *)notification {
  _keyboardVisible = NO;
  NSTimeInterval duration =
      [notification.userInfo[UIKeyboardAnimationDurationUserInfoKey]
          doubleValue];
  UIViewAnimationOptions options =
      [self animationOptionsForKeyboardNotification:notification];
  [self updateFrameAnimated:YES duration:duration options:options];
}

- (void)setSelectionStart:(std::size_t)selectionStart
                      end:(std::size_t)selectionEnd {
  NSString *text = _textField.text != nil ? _textField.text : @"";
  selectionStart = ClampUtf8ByteOffset(NSStringToString(text), selectionStart);
  selectionEnd = ClampUtf8ByteOffset(NSStringToString(text), selectionEnd);
  if (selectionEnd < selectionStart) {
    std::swap(selectionStart, selectionEnd);
  }

  const NSInteger start = Utf16OffsetFromUtf8ByteOffset(text, selectionStart);
  const NSInteger end = Utf16OffsetFromUtf8ByteOffset(text, selectionEnd);
  UITextPosition *beginning = _textField.beginningOfDocument;
  UITextPosition *startPosition =
      [_textField positionFromPosition:beginning offset:start];
  UITextPosition *endPosition =
      [_textField positionFromPosition:beginning offset:end];
  if (startPosition == nil || endPosition == nil) {
    return;
  }
  UITextRange *range =
      [_textField textRangeFromPosition:startPosition toPosition:endPosition];
  if (range != nil) {
    _textField.selectedTextRange = range;
  }
}

- (void)setState:(const IOSNativeTextEditorState &)state {
  NSString *text = NSStringFromUtf8(state.text);
  _textField.text = text != nil ? text : @"";
  [self setSelectionStart:state.selectionStart end:state.selectionEnd];
}

- (UIViewAnimationOptions)animationOptionsForKeyboardNotification:
    (NSNotification *)notification {
  NSNumber *curveValue =
      notification.userInfo[UIKeyboardAnimationCurveUserInfoKey];
  UIViewAnimationCurve curve =
      curveValue != nil ? static_cast<UIViewAnimationCurve>(curveValue.integerValue)
                        : UIViewAnimationCurveEaseInOut;
  return static_cast<UIViewAnimationOptions>(curve << 16);
}

- (void)updateFrameAnimated:(BOOL)animated
                   duration:(NSTimeInterval)duration
                    options:(UIViewAnimationOptions)options {
  UIView *containerView = _containerView;
  if (containerView == nil) {
    return;
  }

  UIEdgeInsets safeInsets = UIEdgeInsetsZero;
  if (@available(iOS 11.0, *)) {
    safeInsets = containerView.safeAreaInsets;
  }

  CGRect bounds = containerView.bounds;
  CGFloat keyboardTop = bounds.size.height - safeInsets.bottom;
  if (_keyboardVisible && !CGRectIsEmpty(_lastKeyboardFrame)) {
    CGRect keyboardFrame =
        [containerView convertRect:_lastKeyboardFrame fromView:nil];
    if (CGRectIntersectsRect(bounds, keyboardFrame)) {
      keyboardTop = std::max<CGFloat>(
          0.0, std::min<CGFloat>(keyboardTop, CGRectGetMinY(keyboardFrame)));
    }
  }

  const CGFloat x = safeInsets.left;
  const CGFloat width =
      std::max<CGFloat>(1.0, bounds.size.width - safeInsets.left -
                                 safeInsets.right);
  const CGFloat y = std::max<CGFloat>(safeInsets.top,
                                      keyboardTop - kNativeTextEditorHeight);
  CGRect nextFrame = CGRectMake(x, y, width, kNativeTextEditorHeight);

  auto applyFrame = ^{
    self.frame = nextFrame;
    [self setNeedsLayout];
    [self layoutIfNeeded];
  };

  if (animated && duration > 0.0) {
    [UIView animateWithDuration:duration
                          delay:0.0
                        options:options
                     animations:applyFrame
                     completion:nil];
  } else {
    applyFrame();
  }
}

- (void)textFieldEditingChanged:(UITextField *)textField {
  (void)textField;
  [self emitEvent:IOSNativeTextEditorEvent::Changed];
}

- (void)textFieldDidChangeSelection:(UITextField *)textField {
  (void)textField;
  [self emitEvent:IOSNativeTextEditorEvent::SelectionChanged];
}

- (BOOL)textFieldShouldReturn:(UITextField *)textField {
  (void)textField;
  [self emitEvent:IOSNativeTextEditorEvent::Submitted];
  if (!_hiding) {
    [self hideWithNotifyFinished:NO];
  }
  return NO;
}

- (void)textFieldDidEndEditing:(UITextField *)textField {
  (void)textField;
  if (!_hiding) {
    [self hideWithNotifyFinished:YES];
  }
}

- (void)emitEvent:(IOSNativeTextEditorEvent)event {
  if (_callback == nullptr) {
    return;
  }
  NSString *nativeText = _textField.text != nil ? _textField.text : @"";
  IOSNativeTextEditorState state;
  state.text = NSStringToString(nativeText);
  state.selectionStart = state.text.size();
  state.selectionEnd = state.text.size();

  UITextRange *selectedRange = _textField.selectedTextRange;
  UITextPosition *beginning = _textField.beginningOfDocument;
  if (selectedRange != nil && beginning != nil) {
    const NSInteger selectionStart =
        [_textField offsetFromPosition:beginning toPosition:selectedRange.start];
    const NSInteger selectionEnd =
        [_textField offsetFromPosition:beginning toPosition:selectedRange.end];
    state.selectionStart =
        Utf8ByteOffsetFromUtf16Offset(nativeText, selectionStart);
    state.selectionEnd = Utf8ByteOffsetFromUtf16Offset(nativeText, selectionEnd);
  }

  _callback(_context, event, state);
}

- (void)hideWithNotifyFinished:(BOOL)notifyFinished {
  if (_hiding) {
    return;
  }
  _hiding = YES;
  [[NSNotificationCenter defaultCenter] removeObserver:self];
  if (notifyFinished) {
    [self emitEvent:IOSNativeTextEditorEvent::Finished];
  }
  [_textField resignFirstResponder];
  [self removeFromSuperview];
  if (gNativeTextEditor == self) {
    gNativeTextEditor = nil;
  }
}
@end

void ShowIOSNativeTextEditor(const IOSNativeTextEditorConfig &config,
                             void *context,
                             IOSNativeTextEditorCallback callback) {
  const IOSNativeTextEditorConfig editorConfig = config;
  void *editorContext = context;
  auto editorCallback = callback;
  auto showBlock = ^{
    @autoreleasepool {
      if (editorCallback == nullptr) {
        return;
      }
      UIWindow *window = FindActiveWindow();
      if (window == nil) {
        return;
      }
      if (gNativeTextEditor != nil) {
        if ([gNativeTextEditor context] == editorContext) {
          return;
        }
        [gNativeTextEditor hideWithNotifyFinished:YES];
      }
      gNativeTextEditor =
          [[AsoNativeTextEditorView alloc] initWithConfig:editorConfig
                                                  context:editorContext
                                                 callback:editorCallback];
      [gNativeTextEditor showInView:window];
    }
  };

  if ([NSThread isMainThread]) {
    showBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), showBlock);
  }
}

void HideIOSNativeTextEditor(void *context, bool notifyFinished) {
  void *editorContext = context;
  auto hideBlock = ^{
    @autoreleasepool {
      if (gNativeTextEditor == nil) {
        return;
      }
      if (editorContext != nullptr &&
          [gNativeTextEditor context] != editorContext) {
        return;
      }
      [gNativeTextEditor hideWithNotifyFinished:notifyFinished ? YES : NO];
    }
  };

  if ([NSThread isMainThread]) {
    hideBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), hideBlock);
  }
}

void SetIOSNativeTextEditorSelection(void *context,
                                     std::size_t selectionStart,
                                     std::size_t selectionEnd) {
  void *editorContext = context;
  auto selectionBlock = ^{
    @autoreleasepool {
      if (gNativeTextEditor == nil) {
        return;
      }
      if (editorContext != nullptr &&
          [gNativeTextEditor context] != editorContext) {
        return;
      }
      [gNativeTextEditor setSelectionStart:selectionStart end:selectionEnd];
    }
  };

  if ([NSThread isMainThread]) {
    selectionBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), selectionBlock);
  }
}

void SetIOSNativeTextEditorState(
    void *context, const IOSNativeTextEditorState &state) {
  void *editorContext = context;
  const IOSNativeTextEditorState editorState = state;
  auto stateBlock = ^{
    @autoreleasepool {
      if (gNativeTextEditor == nil) {
        return;
      }
      if (editorContext != nullptr &&
          [gNativeTextEditor context] != editorContext) {
        return;
      }
      [gNativeTextEditor setState:editorState];
    }
  };

  if ([NSThread isMainThread]) {
    stateBlock();
  } else {
    dispatch_async(dispatch_get_main_queue(), stateBlock);
  }
}

int GetIOSNativeTextEditorHeight() {
  return RoundedCGFloat(kNativeTextEditorHeight);
}

void RestoreIOSViewportAfterKeyboardFocus() {
  auto restoreIfNoEditor = ^{
    if (gNativeTextEditor != nil) {
      return;
    }
    RestoreIOSViewportAfterKeyboardFocusOnce();
  };
  auto scheduleRestore = ^(NSTimeInterval delaySeconds) {
    const dispatch_time_t when =
        dispatch_time(DISPATCH_TIME_NOW,
                      static_cast<int64_t>(delaySeconds * NSEC_PER_SEC));
    dispatch_after(when, dispatch_get_main_queue(), restoreIfNoEditor);
  };
  dispatch_async(dispatch_get_main_queue(), restoreIfNoEditor);
  scheduleRestore(0.12);
  scheduleRestore(0.35);
}

@interface AsoFolderPickerDelegate : NSObject <UIDocumentPickerDelegate> {
@private
  dispatch_semaphore_t _semaphore;
}
@property(nonatomic, copy) NSString *selectedPath;
@property(nonatomic, copy) NSString *bookmarkBase64;
@property(nonatomic, copy) NSString *errorMessage;
@property(nonatomic, assign) BOOL picked;
- (instancetype)initWithSemaphore:(dispatch_semaphore_t)semaphore;
- (void)signalFinished;
@end

@implementation AsoFolderPickerDelegate
- (instancetype)initWithSemaphore:(dispatch_semaphore_t)semaphore {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  _semaphore = semaphore;
  _selectedPath = @"";
  _bookmarkBase64 = @"";
  _errorMessage = @"";
  _picked = NO;
  return self;
}

- (void)signalFinished {
  if (_semaphore != nullptr) {
    dispatch_semaphore_signal(_semaphore);
  }
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
  (void)controller;
  NSURL *url = urls.firstObject;
  if (url == nil) {
    self.errorMessage = @"No folder was selected";
    [self signalFinished];
    return;
  }

  BOOL accessing = [url startAccessingSecurityScopedResource];
  NSError *bookmarkError = nil;
  // iOS does not expose security-scoped bookmark options; persist a regular
  // bookmark and call startAccessingSecurityScopedResource after resolving it.
  NSData *bookmarkData =
      [url bookmarkDataWithOptions:0
     includingResourceValuesForKeys:nil
                      relativeToURL:nil
                              error:&bookmarkError];

  if (bookmarkData == nil) {
    self.errorMessage =
        bookmarkError.localizedDescription ?: @"Failed to create folder access";
  } else {
    self.selectedPath = url.path ?: @"";
    self.bookmarkBase64 =
        [bookmarkData base64EncodedStringWithOptions:0] ?: @"";
    self.picked = YES;
  }

  if (accessing) {
    [url stopAccessingSecurityScopedResource];
  }
  [self signalFinished];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
  (void)controller;
  [self signalFinished];
}
@end

@class AsoDocumentHandoffDelegate;

class IOSExclusiveOutputFile {
public:
  enum class OpenResult { Opened, AlreadyExists, Cancelled, Failed };

  IOSExclusiveOutputFile() = default;
  IOSExclusiveOutputFile(const IOSExclusiveOutputFile &) = delete;
  IOSExclusiveOutputFile &operator=(const IOSExclusiveOutputFile &) = delete;
  ~IOSExclusiveOutputFile() { abort(); }

  OpenResult open(NSString *path, NSString **errorMessage) {
    std::lock_guard lock(mutex_);
    if (cancelled_) {
      return OpenResult::Cancelled;
    }
    if (descriptor_ >= 0 || ownsPath_ || path.length == 0) {
      setError(errorMessage, @"Invalid private document destination.");
      return OpenResult::Failed;
    }

    const char *pathBytes = path.fileSystemRepresentation;
    if (pathBytes == nullptr) {
      setError(errorMessage, @"The private document path is invalid.");
      return OpenResult::Failed;
    }
    const int descriptor =
        ::open(pathBytes,
               O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0) {
      const int openError = errno;
      if (openError == EEXIST || openError == ELOOP) {
        return OpenResult::AlreadyExists;
      }
      setPOSIXError(errorMessage, openError,
                    @"Could not create the private document copy.");
      return OpenResult::Failed;
    }

    struct stat status {};
    if (::fstat(descriptor, &status) != 0) {
      const int statusError = errno;
      ::close(descriptor);
      setPOSIXError(errorMessage, statusError,
                    @"Could not inspect the private document copy.");
      return OpenResult::Failed;
    }
    const dev_t device = status.st_dev;
    const ino_t inode = status.st_ino;
    int securityError = 0;
    if (::fchmod(descriptor, 0600) != 0) {
      securityError = errno;
    } else if (::fstat(descriptor, &status) != 0) {
      securityError = errno;
    } else if (!S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
               (status.st_mode & 0777) != 0600) {
      securityError = EACCES;
    }
    if (securityError != 0) {
      ::close(descriptor);
      removePathIfIdentityMatches(path, device, inode);
      setPOSIXError(errorMessage, securityError,
                    @"Could not secure the private document copy.");
      return OpenResult::Failed;
    }

    descriptor_ = descriptor;
    path_ = [path copy];
    device_ = device;
    inode_ = inode;
    ownsPath_ = true;
    return OpenResult::Opened;
  }

  bool write(const std::uint8_t *data, std::size_t size,
             NSString **errorMessage) {
    std::lock_guard lock(mutex_);
    if (cancelled_) {
      return false;
    }
    if (descriptor_ < 0) {
      setError(errorMessage, @"The private document copy is not open.");
      return false;
    }
    std::size_t offset = 0;
    while (offset < size) {
      const ssize_t count =
          ::write(descriptor_, data + offset, size - offset);
      if (count < 0 && errno == EINTR) {
        continue;
      }
      if (count <= 0) {
        setPOSIXError(errorMessage, count < 0 ? errno : EIO,
                      @"Writing the private document copy failed.");
        return false;
      }
      offset += static_cast<std::size_t>(count);
    }
    return true;
  }

  bool finish(NSString **errorMessage) {
    std::lock_guard lock(mutex_);
    if (cancelled_) {
      return false;
    }
    if (descriptor_ < 0) {
      setError(errorMessage, @"The private document copy is not open.");
      return false;
    }

    int syncResult = -1;
    do {
      syncResult = ::fsync(descriptor_);
    } while (syncResult < 0 && errno == EINTR);
    if (syncResult < 0) {
      setPOSIXError(errorMessage, errno,
                    @"Could not finish the private document copy.");
      closeLocked();
      return false;
    }
    if (!descriptorAndPathStillMatchLocked()) {
      setError(errorMessage,
               @"The private document destination changed during copying.");
      closeLocked();
      return false;
    }
    const int descriptor = descriptor_;
    descriptor_ = -1;
    if (::close(descriptor) != 0) {
      setPOSIXError(errorMessage, errno,
                    @"Could not close the private document copy safely.");
      return false;
    }
    return true;
  }

  void releaseOwnership() noexcept {
    std::lock_guard lock(mutex_);
    ownsPath_ = false;
    path_ = nil;
  }

  void cancel() noexcept {
    std::lock_guard lock(mutex_);
    cancelled_ = true;
    closeLocked();
    removeOwnedPathLocked();
  }

  void abort() noexcept {
    std::lock_guard lock(mutex_);
    closeLocked();
    removeOwnedPathLocked();
  }

  [[nodiscard]] bool cancelled() const noexcept {
    std::lock_guard lock(mutex_);
    return cancelled_;
  }

private:
  static void setError(NSString **errorMessage, NSString *message) {
    if (errorMessage != nullptr) {
      *errorMessage = message ?: @"Private document I/O failed.";
    }
  }

  static void setPOSIXError(NSString **errorMessage, int code,
                            NSString *fallback) {
    if (errorMessage == nullptr) {
      return;
    }
    NSError *error = [NSError errorWithDomain:NSPOSIXErrorDomain
                                         code:code
                                     userInfo:nil];
    *errorMessage = error.localizedDescription ?: fallback;
  }

  static void removePathIfIdentityMatches(NSString *path, dev_t device,
                                          ino_t inode) noexcept {
    const char *pathBytes = path.fileSystemRepresentation;
    struct stat status {};
    if (pathBytes != nullptr && ::lstat(pathBytes, &status) == 0 &&
        S_ISREG(status.st_mode) && status.st_dev == device &&
        status.st_ino == inode) {
      ::unlink(pathBytes);
    }
  }

  bool descriptorAndPathStillMatchLocked() const noexcept {
    if (descriptor_ < 0 || !ownsPath_ || path_.length == 0) {
      return false;
    }
    struct stat descriptorStatus {};
    struct stat pathStatus {};
    const char *pathBytes = path_.fileSystemRepresentation;
    return pathBytes != nullptr &&
           ::fstat(descriptor_, &descriptorStatus) == 0 &&
           ::lstat(pathBytes, &pathStatus) == 0 &&
           S_ISREG(descriptorStatus.st_mode) && S_ISREG(pathStatus.st_mode) &&
           descriptorStatus.st_dev == device_ &&
           descriptorStatus.st_ino == inode_ && pathStatus.st_dev == device_ &&
           pathStatus.st_ino == inode_ &&
           descriptorStatus.st_uid == geteuid() &&
           pathStatus.st_uid == geteuid() &&
           (descriptorStatus.st_mode & 0777) == 0600 &&
           (pathStatus.st_mode & 0777) == 0600;
  }

  void closeLocked() noexcept {
    if (descriptor_ >= 0) {
      const int descriptor = descriptor_;
      descriptor_ = -1;
      ::close(descriptor);
    }
  }

  void removeOwnedPathLocked() noexcept {
    if (ownsPath_) {
      removePathIfIdentityMatches(path_, device_, inode_);
    }
    ownsPath_ = false;
    path_ = nil;
  }

  mutable std::mutex mutex_;
  int descriptor_ = -1;
  NSString *path_ = nil;
  dev_t device_ = 0;
  ino_t inode_ = 0;
  bool ownsPath_ = false;
  bool cancelled_ = false;
};

static AsoDocumentHandoffDelegate *gActiveIOSDocumentHandoffDelegate = nil;
static std::mutex gIOSDocumentIOMutex;
static unsigned long long gIOSDocumentIOToken = 0;
static bool gIOSDocumentIOCancellationRequested = false;
static NSFileCoordinator *gIOSDocumentCoordinator = nil;
static NSInputStream *gIOSDocumentInput = nil;
static std::shared_ptr<IOSExclusiveOutputFile> gIOSDocumentOutput;

static void RegisterIOSDocumentCoordinator(
    unsigned long long operationToken, NSFileCoordinator *coordinator) {
  std::lock_guard lock(gIOSDocumentIOMutex);
  gIOSDocumentIOToken = operationToken;
  gIOSDocumentIOCancellationRequested = false;
  gIOSDocumentCoordinator = coordinator;
  gIOSDocumentInput = nil;
  gIOSDocumentOutput.reset();
}

static bool RegisterIOSDocumentStreams(unsigned long long operationToken,
                                       NSInputStream *input,
                                       const std::shared_ptr<
                                           IOSExclusiveOutputFile> &output) {
  std::lock_guard lock(gIOSDocumentIOMutex);
  if (gIOSDocumentIOToken != operationToken ||
      gIOSDocumentIOCancellationRequested) {
    return false;
  }
  gIOSDocumentInput = input;
  gIOSDocumentOutput = output;
  return true;
}

static void ClearIOSDocumentStreams(unsigned long long operationToken) {
  std::lock_guard lock(gIOSDocumentIOMutex);
  if (gIOSDocumentIOToken != operationToken) {
    return;
  }
  gIOSDocumentInput = nil;
  gIOSDocumentOutput.reset();
}

static void UnregisterIOSDocumentIO(unsigned long long operationToken) {
  std::lock_guard lock(gIOSDocumentIOMutex);
  if (gIOSDocumentIOToken != operationToken) {
    return;
  }
  gIOSDocumentIOToken = 0;
  gIOSDocumentIOCancellationRequested = false;
  gIOSDocumentCoordinator = nil;
  gIOSDocumentInput = nil;
  gIOSDocumentOutput.reset();
}

static void CancelIOSDocumentIO(unsigned long long operationToken) {
  NSFileCoordinator *coordinator = nil;
  NSInputStream *input = nil;
  std::shared_ptr<IOSExclusiveOutputFile> output;
  {
    std::lock_guard lock(gIOSDocumentIOMutex);
    if (gIOSDocumentIOToken != operationToken) {
      return;
    }
    gIOSDocumentIOCancellationRequested = true;
    coordinator = gIOSDocumentCoordinator;
    input = gIOSDocumentInput;
    output = gIOSDocumentOutput;
  }
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    [coordinator cancel];
    [input close];
    if (output != nullptr) {
      output->cancel();
    }
  });
}

@interface AsoDocumentHandoffDelegate
    : NSObject <UIDocumentPickerDelegate, UIAdaptivePresentationControllerDelegate> {
@private
  dispatch_semaphore_t _semaphore;
  BOOL _finished;
}
@property(nonatomic, strong) NSURL *selectedURL;
@property(nonatomic, strong) UIDocumentPickerViewController *picker;
@property(nonatomic, copy) NSString *errorMessage;
@property(nonatomic, assign) BOOL cancelled;
@property(nonatomic, assign) unsigned long long operationToken;
@property(nonatomic, assign) BOOL programmaticCancellationPending;
@property(nonatomic, assign) BOOL dismissalOutcomePending;
@property(nonatomic, strong) NSURL *pendingURL;
@property(nonatomic, copy) NSString *pendingErrorMessage;
@property(nonatomic, assign) BOOL pendingCancelled;
@property(nonatomic, copy) BOOL (^commitHandler)(void);
- (instancetype)initWithSemaphore:(dispatch_semaphore_t)semaphore
                    operationToken:(unsigned long long)operationToken;
- (void)finishWithURL:(NSURL *)url
                error:(NSString *)errorMessage
            cancelled:(BOOL)cancelled;
- (void)finishAfterDismissingPicker:(UIDocumentPickerViewController *)picker
                                URL:(NSURL *)url
                              error:(NSString *)errorMessage
                          cancelled:(BOOL)cancelled;
@end

@implementation AsoDocumentHandoffDelegate
- (instancetype)initWithSemaphore:(dispatch_semaphore_t)semaphore
                    operationToken:(unsigned long long)operationToken {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  _semaphore = semaphore;
  _finished = NO;
  _selectedURL = nil;
  _picker = nil;
  _errorMessage = @"";
  _cancelled = NO;
  _operationToken = operationToken;
  _programmaticCancellationPending = NO;
  _dismissalOutcomePending = NO;
  _pendingURL = nil;
  _pendingErrorMessage = @"";
  _pendingCancelled = NO;
  _commitHandler = nil;
  return self;
}

- (void)finishAfterDismissingPicker:(UIDocumentPickerViewController *)picker
                                URL:(NSURL *)url
                              error:(NSString *)errorMessage
                          cancelled:(BOOL)cancelled {
  self.dismissalOutcomePending = YES;
  self.pendingURL = url;
  self.pendingErrorMessage = errorMessage ?: @"";
  self.pendingCancelled = cancelled;
  UIViewController *presenting = picker.presentingViewController;
  if (presenting == nil) {
    [self finishWithURL:url error:errorMessage cancelled:cancelled];
    return;
  }
  [presenting dismissViewControllerAnimated:YES
                                  completion:^{
    [self finishWithURL:self.pendingURL
                  error:self.pendingErrorMessage
              cancelled:self.pendingCancelled];
  }];
}

- (void)finishWithURL:(NSURL *)url
                error:(NSString *)errorMessage
            cancelled:(BOOL)cancelled {
  @synchronized(self) {
    if (_finished) {
      return;
    }
    _finished = YES;
    self.selectedURL = url;
    self.errorMessage = errorMessage ?: @"";
    self.cancelled = cancelled;
    if (gActiveIOSDocumentHandoffDelegate == self) {
      gActiveIOSDocumentHandoffDelegate = nil;
    }
    if (_semaphore != nullptr) {
      dispatch_semaphore_signal(_semaphore);
    }
  }
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller
    didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
  if (self.dismissalOutcomePending) {
    return;
  }
  NSURL *url = urls.firstObject;
  if (url == nil) {
    [self finishAfterDismissingPicker:controller
                                  URL:nil
                                error:@"The document picker returned no document."
                            cancelled:NO];
    return;
  }
  if (self.commitHandler != nil && !self.commitHandler()) {
    self.commitHandler = nil;
    // The system picker may already have replaced a user-selected destination.
    // Never delete provider-owned content here; cancellation only controls the
    // app result once the irreversible picker callback has arrived.
    [self finishAfterDismissingPicker:controller
                                  URL:nil
                                error:@""
                            cancelled:YES];
    return;
  }
  self.commitHandler = nil;
  [self finishAfterDismissingPicker:controller
                                URL:url
                              error:@""
                          cancelled:NO];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)controller {
  if (self.programmaticCancellationPending || self.dismissalOutcomePending) {
    return;
  }
  [self finishAfterDismissingPicker:controller
                                URL:nil
                              error:@""
                          cancelled:YES];
}

- (void)presentationControllerDidDismiss:
    (UIPresentationController *)presentationController {
  (void)presentationController;
  if (self.dismissalOutcomePending) {
    [self finishWithURL:self.pendingURL
                  error:self.pendingErrorMessage
              cancelled:self.pendingCancelled];
  } else {
    [self finishWithURL:nil error:@"" cancelled:YES];
  }
}
@end

@interface AsoSecurityScopedResource : NSObject
@property(nonatomic, strong) NSURL *url;
@property(nonatomic, assign) BOOL accessing;
- (instancetype)initWithURL:(NSURL *)url accessing:(BOOL)accessing;
- (void)stopAccess;
@end

@implementation AsoSecurityScopedResource
- (instancetype)initWithURL:(NSURL *)url accessing:(BOOL)accessing {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  _url = url;
  _accessing = accessing;
  return self;
}

- (void)stopAccess {
  if (_accessing) {
    [_url stopAccessingSecurityScopedResource];
    _accessing = NO;
  }
}

- (void)dealloc {
  [self stopAccess];
}
@end

@interface AsoBinaryDownloadDelegate : NSObject <NSURLSessionDownloadDelegate> {
 @public
  NSData *responseData;
  NSURLResponse *urlResponse;
  NSError *requestError;
  dispatch_semaphore_t semaphore;
  IOSDownloadProgressCallback progressCallback;
  void *progressContext;
}
@end

@implementation AsoBinaryDownloadDelegate
- (void)URLSession:(NSURLSession *)session
                 downloadTask:(NSURLSessionDownloadTask *)downloadTask
                 didWriteData:(int64_t)bytesWritten
            totalBytesWritten:(int64_t)totalBytesWritten
    totalBytesExpectedToWrite:(int64_t)totalBytesExpectedToWrite {
  (void)session;
  (void)downloadTask;
  (void)bytesWritten;
  if (progressCallback == nullptr) {
    return;
  }

  const std::uint64_t downloadedBytes =
      totalBytesWritten > 0 ? static_cast<std::uint64_t>(totalBytesWritten) : 0;
  const std::uint64_t totalBytes =
      totalBytesExpectedToWrite > 0
          ? static_cast<std::uint64_t>(totalBytesExpectedToWrite)
          : 0;
  progressCallback(progressContext, downloadedBytes, totalBytes);
}

- (void)URLSession:(NSURLSession *)session
                 downloadTask:(NSURLSessionDownloadTask *)downloadTask
    didFinishDownloadingToURL:(NSURL *)location {
  (void)session;
  if (urlResponse == nil) {
    urlResponse = downloadTask.response;
  }

  NSError *readError = nil;
  NSData *data = [NSData dataWithContentsOfURL:location
                                       options:0
                                         error:&readError];
  if (data == nil) {
    if (requestError == nil) {
      requestError = readError;
    }
    return;
  }
  responseData = data;
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
  (void)session;
  if (urlResponse == nil) {
    urlResponse = task.response;
  }
  if (error != nil) {
    requestError = error;
  }
  dispatch_semaphore_signal(semaphore);
}
@end

@interface AsoIrHttpDelegate
    : NSObject <NSURLSessionDataDelegate, NSURLSessionTaskDelegate> {
 @public
  NSMutableData *responseData;
  NSURLResponse *urlResponse;
  NSError *requestError;
  dispatch_semaphore_t semaphore;
  NSUInteger maximumResponseBytes;
  BOOL responseTooLarge;
}
@end

@implementation AsoIrHttpDelegate
- (instancetype)init {
  self = [super init];
  if (self == nil) {
    return nil;
  }
  responseData = [[NSMutableData alloc] init];
  urlResponse = nil;
  requestError = nil;
  semaphore = dispatch_semaphore_create(0);
  maximumResponseBytes = 0;
  responseTooLarge = NO;
  return self;
}

- (void)URLSession:(NSURLSession *)session
              dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveResponse:(NSURLResponse *)response
     completionHandler:
         (void (^)(NSURLSessionResponseDisposition disposition))
             completionHandler {
  (void)session;
  (void)dataTask;
  urlResponse = response;
  const long long expected = response.expectedContentLength;
  if (expected > 0 &&
      static_cast<unsigned long long>(expected) > maximumResponseBytes) {
    responseTooLarge = YES;
    completionHandler(NSURLSessionResponseCancel);
    return;
  }
  completionHandler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession *)session
          dataTask:(NSURLSessionDataTask *)dataTask
    didReceiveData:(NSData *)data {
  (void)session;
  if (responseTooLarge) {
    return;
  }
  if (responseData.length > maximumResponseBytes ||
      data.length > maximumResponseBytes - responseData.length) {
    responseTooLarge = YES;
    [dataTask cancel];
    return;
  }
  [responseData appendData:data];
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    willPerformHTTPRedirection:(NSHTTPURLResponse *)response
                    newRequest:(NSURLRequest *)request
             completionHandler:
                 (void (^)(NSURLRequest *_Nullable))completionHandler {
  (void)session;
  (void)task;
  (void)request;
  urlResponse = response;
  completionHandler(nil);
}

- (void)URLSession:(NSURLSession *)session
                    task:(NSURLSessionTask *)task
    didCompleteWithError:(NSError *)error {
  (void)session;
  if (urlResponse == nil) {
    urlResponse = task.response;
  }
  requestError = error;
  dispatch_semaphore_signal(semaphore);
}
@end

namespace {

ir::IrTransportError IOSIrTransportError(NSError *error) {
  if (error == nil) {
    return ir::IrTransportError::None;
  }
  if (![error.domain isEqualToString:NSURLErrorDomain]) {
    return ir::IrTransportError::Other;
  }
  switch (error.code) {
  case NSURLErrorCancelled:
    return ir::IrTransportError::Cancelled;
  case NSURLErrorNotConnectedToInternet:
  case NSURLErrorNetworkConnectionLost:
  case NSURLErrorDataNotAllowed:
  case NSURLErrorInternationalRoamingOff:
    return ir::IrTransportError::Offline;
  case NSURLErrorCannotFindHost:
  case NSURLErrorDNSLookupFailed:
    return ir::IrTransportError::Dns;
  case NSURLErrorCannotConnectToHost:
    return ir::IrTransportError::Connect;
  case NSURLErrorSecureConnectionFailed:
  case NSURLErrorServerCertificateHasBadDate:
  case NSURLErrorServerCertificateUntrusted:
  case NSURLErrorServerCertificateHasUnknownRoot:
  case NSURLErrorServerCertificateNotYetValid:
  case NSURLErrorClientCertificateRejected:
  case NSURLErrorClientCertificateRequired:
    return ir::IrTransportError::Tls;
  case NSURLErrorTimedOut:
    return ir::IrTransportError::Timeout;
  default:
    return ir::IrTransportError::Other;
  }
}

std::optional<std::string> IOSSafeRetryAfter(NSHTTPURLResponse *response) {
  if (response == nil) {
    return std::nullopt;
  }
  NSString *value = [response valueForHTTPHeaderField:@"Retry-After"];
  if (value == nil) {
    return std::nullopt;
  }
  std::string utf8 = NSStringToString(value);
  if (utf8.empty() || utf8.size() > 2 * 1024 ||
      !std::ranges::all_of(utf8, [](unsigned char character) {
        return character == '\t' ||
               (character >= 0x20U && character < 0x7fU);
      })) {
    return std::nullopt;
  }
  const auto first = utf8.find_first_not_of(" \t");
  if (first == std::string::npos) {
    return std::nullopt;
  }
  const auto last = utf8.find_last_not_of(" \t");
  return utf8.substr(first, last - first + 1);
}

} // namespace

ir::IrHttpResponse
ir::PerformIrHttpRequestIOS(const IrHttpRequest &request,
                            std::stop_token stopToken) noexcept {
  @autoreleasepool {
    if ([NSThread isMainThread]) {
      return {.transportError = IrTransportError::Other,
              .diagnostic =
                  "IR HTTP requests must run away from the main thread"};
    }
    if (stopToken.stop_requested()) {
      return {.transportError = IrTransportError::Cancelled,
              .diagnostic = "IR HTTP request was cancelled"};
    }
    NSString *urlText = NSStringFromUtf8(request.url);
    NSURL *url = [NSURL URLWithString:urlText];
    if (url == nil) {
      return {.transportError = IrTransportError::Other,
              .diagnostic = "IR HTTP URL is invalid"};
    }
    NSMutableURLRequest *nativeRequest = [NSMutableURLRequest
         requestWithURL:url
            cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
        timeoutInterval:request.totalTimeout.count()];
    nativeRequest.HTTPMethod = request.method == IrHttpMethod::Post ? @"POST"
                                                                    : @"GET";
    [nativeRequest setValue:@"AsoBMaShow" forHTTPHeaderField:@"User-Agent"];
    for (const auto &[name, value] : request.headers) {
      [nativeRequest setValue:NSStringFromUtf8(value)
           forHTTPHeaderField:NSStringFromUtf8(name)];
    }
    if (request.method == IrHttpMethod::Post) {
      nativeRequest.HTTPBody =
          [NSData dataWithBytes:request.body.data() length:request.body.size()];
    }

    AsoIrHttpDelegate *delegate = [[AsoIrHttpDelegate alloc] init];
    delegate->maximumResponseBytes = request.maximumResponseBytes;
    NSURLSessionConfiguration *configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    configuration.timeoutIntervalForRequest = request.connectTimeout.count();
    configuration.timeoutIntervalForResource = request.totalTimeout.count();
    NSOperationQueue *delegateQueue = [[NSOperationQueue alloc] init];
    delegateQueue.maxConcurrentOperationCount = 1;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration
                                                          delegate:delegate
                                                     delegateQueue:delegateQueue];
    NSURLSessionDataTask *task = [session dataTaskWithRequest:nativeRequest];
    [task resume];

    const auto deadline = std::chrono::steady_clock::now() +
                          request.totalTimeout + std::chrono::seconds(2);
    bool cancelled = false;
    bool timedOut = false;
    while (dispatch_semaphore_wait(
               delegate->semaphore,
               dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) != 0) {
      if (stopToken.stop_requested()) {
        cancelled = true;
        [task cancel];
      } else if (std::chrono::steady_clock::now() >= deadline) {
        timedOut = true;
        [task cancel];
      }
      if (cancelled || timedOut) {
        (void)dispatch_semaphore_wait(
            delegate->semaphore,
            dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
        break;
      }
    }
    [session finishTasksAndInvalidate];

    if (cancelled || stopToken.stop_requested()) {
      return {.transportError = IrTransportError::Cancelled,
              .diagnostic = "IR HTTP request was cancelled"};
    }
    if (delegate->responseTooLarge) {
      return {.transportError = IrTransportError::ResponseTooLarge,
              .diagnostic = "IR HTTP response exceeded its size limit"};
    }
    if (timedOut) {
      return {.transportError = IrTransportError::Timeout,
              .diagnostic = "IR HTTP request timed out"};
    }
    const IrTransportError transport =
        IOSIrTransportError(delegate->requestError);
    if (transport != IrTransportError::None) {
      return {.transportError = transport,
              .diagnostic = transport == IrTransportError::Cancelled
                                ? "IR HTTP request was cancelled"
                                : "IR HTTP request failed"};
    }
    NSHTTPURLResponse *httpResponse =
        [delegate->urlResponse isKindOfClass:[NSHTTPURLResponse class]]
            ? (NSHTTPURLResponse *)delegate->urlResponse
            : nil;
    if (httpResponse == nil || httpResponse.statusCode < 100 ||
        httpResponse.statusCode > 599) {
      return {.transportError = IrTransportError::Other,
              .diagnostic = "IR HTTP response status is invalid"};
    }
    std::string body;
    if (delegate->responseData.length != 0) {
      body.assign(static_cast<const char *>(delegate->responseData.bytes),
                  delegate->responseData.length);
    }
    return {.statusCode = httpResponse.statusCode,
            .body = std::move(body),
            .retryAfter = IOSSafeRetryAfter(httpResponse)};
  }
}

bool PickIOSFolder(std::string &path, std::string &bookmark,
                   std::string &errorMessage) {
  path.clear();
  bookmark.clear();
  errorMessage.clear();

  if ([NSThread isMainThread]) {
    errorMessage = "PickIOSFolder must be called off the main thread";
    return false;
  }

  dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
  __block AsoFolderPickerDelegate *delegate =
      [[AsoFolderPickerDelegate alloc] initWithSemaphore:semaphore];

  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      UIWindow *window = FindActiveWindow();
      UIViewController *presenting =
          window != nil ? TopViewController(window.rootViewController) : nil;
      if (presenting == nil) {
        delegate.errorMessage = @"No active view controller";
        [delegate signalFinished];
        return;
      }

      UIDocumentPickerViewController *picker =
          [[UIDocumentPickerViewController alloc]
              initWithDocumentTypes:@[ @"public.folder" ]
                              inMode:UIDocumentPickerModeOpen];
      picker.delegate = delegate;
      picker.allowsMultipleSelection = NO;
      picker.modalPresentationStyle = UIModalPresentationFormSheet;
      [presenting presentViewController:picker animated:YES completion:nil];
    }
  });

  dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
  const bool picked = delegate.picked == YES;
  path = NSStringToString(delegate.selectedPath);
  bookmark = NSStringToString(delegate.bookmarkBase64);
  errorMessage = NSStringToString(delegate.errorMessage);
  delegate = nil;
  return picked;
}

namespace {
constexpr std::string_view kIOSDocumentCancelled = "__CANCELLED__";
constexpr std::string_view kIOSDocumentErrorPrefix = "__ERROR__:";
constexpr std::string_view kIOSDocumentSuccess = "__OK__";

bool IOSDocumentCancellationRequested(
    const std::atomic_bool *cancellationRequested) {
  return cancellationRequested != nullptr &&
         cancellationRequested->load(std::memory_order_acquire);
}

std::string IOSPathToUtf8(const std::filesystem::path &path) {
  const auto value = path.u8string();
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

bool IOSPathWithin(const std::filesystem::path &path,
                   const std::filesystem::path &directory,
                   bool allowEqual) {
  auto pathPart = path.begin();
  for (auto directoryPart = directory.begin();
       directoryPart != directory.end(); ++directoryPart, ++pathPart) {
    if (pathPart == path.end() || *pathPart != *directoryPart) {
      return false;
    }
  }
  return allowEqual || pathPart != path.end();
}

bool IOSIssuedImportPath(const std::filesystem::path &candidate,
                         const std::filesystem::path &base,
                         bool directory) {
  if (candidate.parent_path() != base) {
    return false;
  }
  const std::string name = candidate.filename().string();
  const std::size_t uuidLength = 36;
  if (name.size() != uuidLength + (directory ? 0 : 4) ||
      (!directory && !name.ends_with(".zip"))) {
    return false;
  }
  for (std::size_t index = 0; index < uuidLength; ++index) {
    const unsigned char value = static_cast<unsigned char>(name[index]);
    const bool hyphen = index == 8 || index == 13 || index == 18 || index == 23;
    if (hyphen ? value != static_cast<unsigned char>('-')
               : std::isxdigit(value) == 0) {
      return false;
    }
  }
  return true;
}

NSString *IOSDocumentHandoffBaseDirectory(NSString **errorMessage) {
  NSFileManager *fileManager = NSFileManager.defaultManager;
  NSURL *cacheURL =
      [fileManager URLsForDirectory:NSCachesDirectory
                         inDomains:NSUserDomainMask].firstObject;
  if (cacheURL == nil) {
    if (errorMessage != nullptr) {
      *errorMessage = @"Application cache storage is unavailable.";
    }
    return nil;
  }
  NSString *directory =
      [cacheURL.path stringByAppendingPathComponent:@"document-handoff"];
  std::error_code pathError;
  const auto cachePath =
      std::filesystem::path(NSStringToString(cacheURL.path)).lexically_normal();
  const auto directoryPath =
      std::filesystem::path(NSStringToString(directory)).lexically_normal();
  auto directoryStatus =
      std::filesystem::symlink_status(directoryPath, pathError);
  const bool directoryMissing =
      pathError == std::errc::no_such_file_or_directory ||
      (!pathError && directoryStatus.type() ==
                         std::filesystem::file_type::not_found);
  if (!directoryMissing &&
      (pathError || std::filesystem::is_symlink(directoryStatus) ||
       !std::filesystem::is_directory(directoryStatus))) {
    if (errorMessage != nullptr) {
      *errorMessage = @"Private document storage is not trustworthy.";
    }
    return nil;
  }
  NSError *createError = nil;
  NSDictionary *attributes = @{NSFilePosixPermissions : @0700};
  if (directoryMissing &&
      ![fileManager createDirectoryAtPath:directory
              withIntermediateDirectories:NO
                               attributes:attributes
                                    error:&createError]) {
    if (errorMessage != nullptr) {
      *errorMessage = createError.localizedDescription ?:
          @"Could not create private document storage.";
    }
    return nil;
  }
  pathError.clear();
  directoryStatus = std::filesystem::symlink_status(directoryPath, pathError);
  const auto canonicalCache = std::filesystem::canonical(cachePath, pathError);
  const auto canonicalParent =
      std::filesystem::canonical(directoryPath.parent_path(), pathError);
  if (pathError || std::filesystem::is_symlink(directoryStatus) ||
      !std::filesystem::is_directory(directoryStatus) ||
      canonicalParent != canonicalCache) {
    if (errorMessage != nullptr) {
      *errorMessage = @"Private document storage escaped application caches.";
    }
    return nil;
  }
  NSError *attributeError = nil;
  [fileManager setAttributes:attributes
                ofItemAtPath:directory
                       error:&attributeError];
  if (attributeError != nil) {
    if (errorMessage != nullptr) {
      *errorMessage = attributeError.localizedDescription ?:
          @"Could not secure private document storage.";
    }
    return nil;
  }
  struct stat privateStatus {};
  if (::lstat(directoryPath.c_str(), &privateStatus) != 0 ||
      !S_ISDIR(privateStatus.st_mode) || privateStatus.st_uid != geteuid() ||
      (privateStatus.st_mode & 0777) != 0700) {
    if (errorMessage != nullptr) {
      *errorMessage = @"Private document storage permissions are unsafe.";
    }
    return nil;
  }
  return directory;
}

bool ValidateIOSTemporaryPath(const std::filesystem::path &localPath,
                              bool directory, bool allowMissing,
                              bool allowFinalSymlink,
                              std::string &errorMessage) {
  errorMessage.clear();
  NSString *storageError = nil;
  NSString *baseText = IOSDocumentHandoffBaseDirectory(&storageError);
  if (baseText == nil) {
    errorMessage = NSStringToString(storageError);
    return false;
  }

  std::error_code error;
  const auto base = std::filesystem::path(NSStringToString(baseText));
  const auto candidate = localPath.lexically_normal();
  if (!candidate.is_absolute() ||
      !IOSPathWithin(candidate, base.lexically_normal(), false)) {
    errorMessage = "Temporary document is outside private iOS storage.";
    return false;
  }
  if (!IOSIssuedImportPath(candidate, base.lexically_normal(), directory)) {
    errorMessage = "Temporary path does not match an issued iOS import.";
    return false;
  }
  const auto baseStatus = std::filesystem::symlink_status(base, error);
  if (error || std::filesystem::is_symlink(baseStatus)) {
    errorMessage = "Private iOS document storage is not trustworthy.";
    return false;
  }
  for (auto ancestor = candidate.parent_path(); ancestor != base;
       ancestor = ancestor.parent_path()) {
    if (ancestor.empty()) {
      errorMessage = "Temporary document ancestry is invalid.";
      return false;
    }
    const auto status = std::filesystem::symlink_status(ancestor, error);
    if (error || std::filesystem::is_symlink(status)) {
      errorMessage = "Temporary document has a symbolic-link ancestor.";
      return false;
    }
  }

  const auto canonicalBase = std::filesystem::canonical(base, error);
  if (error) {
    errorMessage = "Private iOS document storage cannot be resolved.";
    return false;
  }
  const auto canonicalParent =
      std::filesystem::canonical(candidate.parent_path(), error);
  if (error || !IOSPathWithin(canonicalParent, canonicalBase, true)) {
    errorMessage = "Temporary document escaped private iOS storage.";
    return false;
  }

  const auto candidateStatus =
      std::filesystem::symlink_status(candidate, error);
  if (allowMissing &&
      (error == std::errc::no_such_file_or_directory ||
       (!error && candidateStatus.type() ==
                      std::filesystem::file_type::not_found))) {
    return true;
  }
  const bool expectedType = directory
                                ? std::filesystem::is_directory(candidateStatus)
                                : std::filesystem::is_regular_file(candidateStatus);
  if (error || (!expectedType &&
                !(allowFinalSymlink &&
                  std::filesystem::is_symlink(candidateStatus)))) {
    errorMessage = directory
                       ? "Temporary iOS import is not a directory."
                       : "Temporary iOS document is not a regular file.";
    return false;
  }
  if (!std::filesystem::is_symlink(candidateStatus)) {
    struct stat privateStatus {};
    if (::lstat(candidate.c_str(), &privateStatus) != 0 ||
        (directory ? !S_ISDIR(privateStatus.st_mode)
                   : !S_ISREG(privateStatus.st_mode)) ||
        privateStatus.st_uid != geteuid() ||
        (privateStatus.st_mode & 0777) != (directory ? 0700 : 0600)) {
      errorMessage = "Temporary iOS document permissions are unsafe.";
      return false;
    }
  }
  return true;
}

bool ValidateIOSTemporaryDocumentPath(
    const std::filesystem::path &localPath, bool allowMissing,
    bool allowFinalSymlink, std::string &errorMessage) {
  return ValidateIOSTemporaryPath(localPath, false, allowMissing,
                                  allowFinalSymlink, errorMessage);
}

bool CopyIOSDocumentURLBounded(NSURL *sourceURL, NSString *destinationPath,
                               std::uint64_t operationToken,
                               std::uint64_t maxBytes,
                               const std::atomic_bool *cancellationRequested,
                               bool &cancelled,
                               NSString **errorMessage) {
  cancelled = false;
  if (sourceURL == nil || destinationPath.length == 0 || maxBytes == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = @"Invalid private document copy request.";
    }
    return false;
  }
  if (IOSDocumentCancellationRequested(cancellationRequested)) {
    cancelled = true;
    return false;
  }

  __block BOOL copied = NO;
  __block BOOL copyCancelled = NO;
  __block NSString *copyErrorMessage = @"";
  auto output = std::make_shared<IOSExclusiveOutputFile>();
  NSFileCoordinator *coordinator = [[NSFileCoordinator alloc] initWithFilePresenter:nil];
  RegisterIOSDocumentCoordinator(operationToken, coordinator);
  if (IOSDocumentCancellationRequested(cancellationRequested)) {
    [coordinator cancel];
    UnregisterIOSDocumentIO(operationToken);
    cancelled = true;
    return false;
  }
  NSError *coordinationError = nil;
  [coordinator coordinateReadingItemAtURL:sourceURL
                                  options:0
                                    error:&coordinationError
                               byAccessor:^(NSURL *coordinatedURL) {
    if (IOSDocumentCancellationRequested(cancellationRequested)) {
      copyCancelled = YES;
      return;
    }
    NSNumber *declaredSize = nil;
    NSError *sizeError = nil;
    [coordinatedURL getResourceValue:&declaredSize
                              forKey:NSURLFileSizeKey
                               error:&sizeError];
    if (declaredSize != nil && declaredSize.unsignedLongLongValue > maxBytes) {
      copyErrorMessage = @"The selected document exceeds the maximum size.";
      return;
    }

    NSInputStream *input = [NSInputStream inputStreamWithURL:coordinatedURL];
    if (!RegisterIOSDocumentStreams(operationToken, input, output)) {
      copyCancelled = YES;
      return;
    }
    [input open];
    NSString *openErrorMessage = nil;
    const auto openResult = output->open(destinationPath, &openErrorMessage);
    if (openResult == IOSExclusiveOutputFile::OpenResult::Cancelled) {
      copyCancelled = YES;
      [input close];
      return;
    }
    if (openResult == IOSExclusiveOutputFile::OpenResult::AlreadyExists) {
      copyErrorMessage = @"The private document destination already exists.";
      [input close];
      return;
    }
    if (openResult != IOSExclusiveOutputFile::OpenResult::Opened) {
      copyErrorMessage = openErrorMessage ?:
          @"Could not create the private document copy.";
      [input close];
      return;
    }
    std::uint64_t total = 0;
    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (true) {
      if (IOSDocumentCancellationRequested(cancellationRequested)) {
        copyCancelled = YES;
        break;
      }
      const NSInteger count = [input read:buffer.data()
                                  maxLength:buffer.size()];
      if (count < 0) {
        copyErrorMessage = input.streamError.localizedDescription ?:
            @"Reading the selected document failed.";
        break;
      }
      if (count == 0) {
        copied = YES;
        break;
      }
      const auto unsignedCount = static_cast<std::uint64_t>(count);
      if (unsignedCount > maxBytes - total) {
        copyErrorMessage = @"The selected document exceeds the maximum size.";
        break;
      }
      NSInteger written = 0;
      while (written < count) {
        if (IOSDocumentCancellationRequested(cancellationRequested)) {
          copyCancelled = YES;
          break;
        }
        NSString *writeErrorMessage = nil;
        const auto remaining = static_cast<std::size_t>(count - written);
        if (!output->write(buffer.data() + written, remaining,
                           &writeErrorMessage)) {
          if (output->cancelled() ||
              IOSDocumentCancellationRequested(cancellationRequested)) {
            copyCancelled = YES;
          }
          copyErrorMessage = writeErrorMessage ?:
              @"Writing the private document copy failed.";
          break;
        }
        written = count;
      }
      if (written != count) {
        break;
      }
      total += unsignedCount;
    }
    [input close];
    if (copied) {
      NSString *finishErrorMessage = nil;
      if (!output->finish(&finishErrorMessage)) {
        copied = NO;
        if (output->cancelled() ||
            IOSDocumentCancellationRequested(cancellationRequested)) {
          copyCancelled = YES;
        } else {
          copyErrorMessage = finishErrorMessage ?:
              @"Could not finish the private document copy.";
        }
      }
    }
  }];
  UnregisterIOSDocumentIO(operationToken);

  if (IOSDocumentCancellationRequested(cancellationRequested)) {
    copyCancelled = YES;
    copied = NO;
  }

  if (!copied) {
    output->abort();
    if (copyCancelled ||
        IOSDocumentCancellationRequested(cancellationRequested)) {
      cancelled = true;
      return false;
    }
    if (copyErrorMessage.length == 0 && coordinationError != nil) {
      copyErrorMessage = coordinationError.localizedDescription;
    }
    if (copyErrorMessage.length == 0) {
      copyErrorMessage = @"Could not copy the selected document.";
    }
    if (errorMessage != nullptr) {
      *errorMessage = copyErrorMessage;
    }
    return false;
  }
  output->releaseOwnership();
  return true;
}

NSString *CreateIOSPrivateImportDirectory(NSString *baseDirectory,
                                          NSString **errorMessage) {
  if (baseDirectory.length == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = @"Private document storage is unavailable.";
    }
    return nil;
  }
  for (int attempt = 0; attempt < 64; ++attempt) {
    NSString *candidate = [baseDirectory
        stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    const char *path = candidate.fileSystemRepresentation;
    if (path == nullptr) {
      continue;
    }
    if (::mkdir(path, 0700) != 0) {
      if (errno == EEXIST) {
        continue;
      }
      if (errorMessage != nullptr) {
        *errorMessage = [NSError errorWithDomain:NSPOSIXErrorDomain
                                             code:errno
                                         userInfo:nil].localizedDescription;
      }
      return nil;
    }
    struct stat status {};
    if (::chmod(path, 0700) != 0 || ::lstat(path, &status) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
        (status.st_mode & 0777) != 0700) {
      [NSFileManager.defaultManager removeItemAtPath:candidate error:nil];
      if (errorMessage != nullptr) {
        *errorMessage = @"Could not secure the private folder copy.";
      }
      return nil;
    }
    return candidate;
  }
  if (errorMessage != nullptr) {
    *errorMessage = @"Could not allocate private folder storage.";
  }
  return nil;
}

void CleanupIOSIssuedDirectoryAfterFailure(NSString *path) noexcept {
  try {
    const auto directory =
        std::filesystem::path(NSStringToString(path)).lexically_normal();
    std::string validationError;
    if (!ValidateIOSTemporaryPath(directory, true, true, true,
                                  validationError)) {
      return;
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(directory, error);
    if (error == std::errc::no_such_file_or_directory) {
      return;
    }
    if (error) {
      return;
    }
    if (std::filesystem::is_symlink(status)) {
      std::filesystem::remove(directory, error);
    } else {
      std::filesystem::remove_all(directory, error);
    }
  } catch (...) {
  }
}

bool SecureIOSPrivateDirectory(NSString *path, NSString **errorMessage) {
  const char *bytes = path.fileSystemRepresentation;
  if (bytes == nullptr || ::mkdir(bytes, 0700) != 0) {
    if (errorMessage != nullptr) {
      *errorMessage = [NSError errorWithDomain:NSPOSIXErrorDomain
                                           code:bytes == nullptr ? EINVAL : errno
                                       userInfo:nil].localizedDescription;
    }
    return false;
  }
  struct stat status {};
  if (::chmod(bytes, 0700) != 0 || ::lstat(bytes, &status) != 0 ||
      !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & 0777) != 0700) {
    if (errorMessage != nullptr) {
      *errorMessage = @"Could not secure a private folder directory.";
    }
    return false;
  }
  return true;
}

bool CopyIOSDirectoryFileBounded(
    NSURL *sourceURL, NSString *destinationPath,
    std::uint64_t operationToken, std::uint64_t maxBytes,
    std::uint64_t maxRegularFileBytes,
    std::uint64_t &totalBytes,
    const std::atomic_bool *cancellationRequested, bool &cancelled,
    NSString **errorMessage) {
  auto output = std::make_shared<IOSExclusiveOutputFile>();
  const char *sourcePath = sourceURL.path.fileSystemRepresentation;
  int descriptor = sourcePath != nullptr
                       ? ::open(sourcePath,
                                O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)
                       : -1;
  struct stat sourceStatus {};
  if (descriptor < 0 || ::fstat(descriptor, &sourceStatus) != 0 ||
      !S_ISREG(sourceStatus.st_mode)) {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    if (errorMessage != nullptr) {
      *errorMessage = @"The selected folder contains a non-regular file.";
    }
    return false;
  }
  if (sourceStatus.st_size < 0 ||
      static_cast<std::uint64_t>(sourceStatus.st_size) > maxRegularFileBytes) {
    ::close(descriptor);
    if (errorMessage != nullptr) {
      *errorMessage = @"The selected folder contains a file beyond its limit.";
    }
    return false;
  }
  if (!RegisterIOSDocumentStreams(operationToken, nil, output)) {
    ::close(descriptor);
    cancelled = true;
    return false;
  }
  NSString *openError = nil;
  const auto openResult = output->open(destinationPath, &openError);
  if (openResult != IOSExclusiveOutputFile::OpenResult::Opened) {
    ::close(descriptor);
    ClearIOSDocumentStreams(operationToken);
    if (openResult == IOSExclusiveOutputFile::OpenResult::Cancelled ||
        IOSDocumentCancellationRequested(cancellationRequested)) {
      cancelled = true;
    } else if (errorMessage != nullptr) {
      *errorMessage = openError ?:
          @"Could not create a private folder file.";
    }
    return false;
  }

  bool complete = false;
  std::uint64_t copiedFileBytes = 0;
  std::array<std::uint8_t, 64 * 1024> buffer{};
  while (true) {
    if (IOSDocumentCancellationRequested(cancellationRequested)) {
      cancelled = true;
      break;
    }
    const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      if (errorMessage != nullptr) {
        *errorMessage = @"Reading a selected folder file failed.";
      }
      break;
    }
    if (count == 0) {
      complete = true;
      break;
    }
    const auto read = static_cast<std::uint64_t>(count);
    if (totalBytes > maxBytes || read > maxBytes - totalBytes) {
      if (errorMessage != nullptr) {
        *errorMessage = @"The selected folder exceeds the maximum size.";
      }
      break;
    }
    if (copiedFileBytes > maxRegularFileBytes ||
        read > maxRegularFileBytes - copiedFileBytes) {
      if (errorMessage != nullptr) {
        *errorMessage = @"The selected folder contains a file beyond its limit.";
      }
      break;
    }
    NSString *writeError = nil;
    if (!output->write(buffer.data(), static_cast<std::size_t>(count),
                       &writeError)) {
      if (output->cancelled() ||
          IOSDocumentCancellationRequested(cancellationRequested)) {
        cancelled = true;
      } else if (errorMessage != nullptr) {
        *errorMessage = writeError ?:
            @"Writing a private folder file failed.";
      }
      break;
    }
    totalBytes += read;
    copiedFileBytes += read;
  }
  struct stat finalSourceStatus {};
  if (complete &&
      (::fstat(descriptor, &finalSourceStatus) != 0 ||
       finalSourceStatus.st_dev != sourceStatus.st_dev ||
       finalSourceStatus.st_ino != sourceStatus.st_ino ||
       finalSourceStatus.st_size != sourceStatus.st_size)) {
    complete = false;
    if (errorMessage != nullptr) {
      *errorMessage = @"A selected folder file changed while being copied.";
    }
  }
  ::close(descriptor);
  if (complete) {
    NSString *finishError = nil;
    if (!output->finish(&finishError)) {
      complete = false;
      if (output->cancelled() ||
          IOSDocumentCancellationRequested(cancellationRequested)) {
        cancelled = true;
      } else if (errorMessage != nullptr) {
        *errorMessage = finishError ?:
            @"Could not finish a private folder file.";
      }
    }
  }
  ClearIOSDocumentStreams(operationToken);
  if (!complete) {
    output->abort();
    return false;
  }
  output->releaseOwnership();
  return true;
}

bool CopyIOSDirectoryURLBounded(
    NSURL *sourceURL, NSString *destinationRoot,
    std::uint64_t operationToken, std::uint64_t maxBytes,
    std::uint64_t maxFiles, std::uint32_t maxDepth,
    std::uint32_t maxPathBytes, std::uint64_t maxRegularFileBytes,
    const std::atomic_bool *cancellationRequested, bool &cancelled,
    NSString **errorMessage) {
  cancelled = false;
  if (sourceURL == nil || destinationRoot.length == 0 || maxBytes == 0 ||
      maxFiles == 0 || maxDepth == 0 || maxPathBytes == 0 ||
      maxRegularFileBytes == 0) {
    if (errorMessage != nullptr) {
      *errorMessage = @"Invalid private folder copy request.";
    }
    return false;
  }
  if (IOSDocumentCancellationRequested(cancellationRequested)) {
    cancelled = true;
    return false;
  }

  __block BOOL copied = NO;
  __block BOOL copyCancelled = NO;
  __block NSString *copyError = @"";
  __block std::uint64_t totalBytes = 0;
  __block std::uint64_t entryCount = 0;
  NSFileCoordinator *coordinator =
      [[NSFileCoordinator alloc] initWithFilePresenter:nil];
  RegisterIOSDocumentCoordinator(operationToken, coordinator);
  NSError *coordinationError = nil;
  [coordinator coordinateReadingItemAtURL:sourceURL
                                  options:0
                                    error:&coordinationError
                               byAccessor:^(NSURL *coordinatedURL) {
    NSNumber *rootDirectory = nil;
    NSNumber *rootSymlink = nil;
    NSNumber *rootAlias = nil;
    NSError *rootError = nil;
    const bool rootValues =
        [coordinatedURL getResourceValue:&rootDirectory
                                  forKey:NSURLIsDirectoryKey
                                   error:&rootError] &&
        [coordinatedURL getResourceValue:&rootSymlink
                                  forKey:NSURLIsSymbolicLinkKey
                                   error:&rootError] &&
        [coordinatedURL getResourceValue:&rootAlias
                                  forKey:NSURLIsAliasFileKey
                                   error:&rootError];
    if (!rootValues || !rootDirectory.boolValue || rootSymlink.boolValue ||
        rootAlias.boolValue) {
      copyError = rootError.localizedDescription ?:
          @"The selected item is not a safe folder.";
      return;
    }

    NSString *rootPath = coordinatedURL.path.stringByStandardizingPath;
    NSString *rootPrefix = [rootPath stringByAppendingString:@"/"];
    NSArray<NSURLResourceKey> *resourceKeys = @[
      NSURLIsDirectoryKey, NSURLIsRegularFileKey, NSURLIsSymbolicLinkKey,
      NSURLIsAliasFileKey, NSURLFileSizeKey
    ];
    NSDirectoryEnumerator<NSURL *> *enumerator =
        [NSFileManager.defaultManager
            enumeratorAtURL:coordinatedURL
 includingPropertiesForKeys:resourceKeys
                    options:0
               errorHandler:^BOOL(NSURL *url, NSError *error) {
      (void)url;
      copyError = error.localizedDescription ?:
          @"Enumerating the selected folder failed.";
      return NO;
    }];
    for (NSURL *entryURL in enumerator) {
      if (IOSDocumentCancellationRequested(cancellationRequested)) {
        copyCancelled = YES;
        break;
      }
      if (++entryCount > maxFiles) {
        copyError = @"The selected folder contains too many entries.";
        break;
      }
      NSString *entryPath = entryURL.path.stringByStandardizingPath;
      if (![entryPath hasPrefix:rootPrefix]) {
        copyError = @"A selected folder entry escaped its package root.";
        break;
      }
      NSString *relative = [entryPath substringFromIndex:rootPrefix.length];
      const std::string relativeUtf8 = NSStringToString(relative);
      if (relativeUtf8.empty() || relativeUtf8.size() > maxPathBytes) {
        copyError = @"A selected folder path exceeds the supported limit.";
        break;
      }
      const auto relativePath = std::filesystem::path(relativeUtf8);
      std::uint32_t depth = 0;
      bool unsafeComponent = false;
      for (const auto &component : relativePath) {
        const auto value = component.string();
        if (value.empty() || value == "." || value == "..") {
          unsafeComponent = true;
          break;
        }
        ++depth;
      }
      if (unsafeComponent || depth == 0 || depth > maxDepth ||
          relativePath.is_absolute()) {
        copyError = @"A selected folder path is unsafe or too deep.";
        break;
      }

      NSNumber *isDirectory = nil;
      NSNumber *isRegular = nil;
      NSNumber *isSymlink = nil;
      NSNumber *isAlias = nil;
      NSError *resourceError = nil;
      const bool values =
          [entryURL getResourceValue:&isDirectory
                              forKey:NSURLIsDirectoryKey
                               error:&resourceError] &&
          [entryURL getResourceValue:&isRegular
                              forKey:NSURLIsRegularFileKey
                               error:&resourceError] &&
          [entryURL getResourceValue:&isSymlink
                              forKey:NSURLIsSymbolicLinkKey
                               error:&resourceError] &&
          [entryURL getResourceValue:&isAlias
                              forKey:NSURLIsAliasFileKey
                               error:&resourceError];
      if (!values || isSymlink.boolValue || isAlias.boolValue ||
          (isDirectory.boolValue == isRegular.boolValue)) {
        copyError = resourceError.localizedDescription ?:
            @"The selected folder contains an unsupported entry.";
        break;
      }

      NSString *destination =
          [destinationRoot stringByAppendingPathComponent:relative];
      if (isDirectory.boolValue) {
        NSString *directoryError = nil;
        if (!SecureIOSPrivateDirectory(destination, &directoryError)) {
          copyError = directoryError ?:
              @"Could not create a private folder directory.";
          break;
        }
        continue;
      }
      NSNumber *declaredSize = nil;
      if (![entryURL getResourceValue:&declaredSize
                               forKey:NSURLFileSizeKey
                                error:&resourceError]) {
        copyError = resourceError.localizedDescription ?:
            @"Could not inspect a selected folder file.";
        break;
      }
      const std::uint64_t declared = declaredSize.unsignedLongLongValue;
      if (totalBytes > maxBytes || declared > maxBytes - totalBytes) {
        copyError = @"The selected folder exceeds the maximum size.";
        break;
      }
      if (declared > maxRegularFileBytes) {
        copyError = @"The selected folder contains a file beyond its limit.";
        break;
      }
      NSString *fileError = nil;
      bool fileCancelled = false;
      if (!CopyIOSDirectoryFileBounded(
              entryURL, destination, operationToken, maxBytes,
              maxRegularFileBytes, totalBytes, cancellationRequested,
              fileCancelled, &fileError)) {
        copyCancelled = fileCancelled;
        copyError = fileError ?:
            @"Could not copy a selected folder file.";
        break;
      }
    }
    copied = !copyCancelled && copyError.length == 0;
  }];
  UnregisterIOSDocumentIO(operationToken);

  if (IOSDocumentCancellationRequested(cancellationRequested)) {
    copyCancelled = YES;
    copied = NO;
  }
  if (!copied) {
    cancelled = copyCancelled == YES;
    if (!cancelled && copyError.length == 0 && coordinationError != nil) {
      copyError = coordinationError.localizedDescription;
    }
    if (!cancelled && errorMessage != nullptr) {
      *errorMessage = copyError.length > 0 ? copyError :
          @"Could not copy the selected folder.";
    }
    return false;
  }
  return true;
}

bool WaitForIOSDocumentPicker(std::uint64_t operationToken,
                              NSString *mimeType, bool directoryImport,
                              NSURL *exportURL,
                              BOOL (^commitHandler)(void),
                              NSURL *__strong *selectedURL,
                              bool &cancelled, std::string &errorMessage,
                              const std::atomic_bool *cancellationRequested) {
  if ([NSThread isMainThread]) {
    errorMessage = "Document handoff must run off the main thread.";
    return false;
  }
  cancelled = false;
  errorMessage.clear();
  if (selectedURL != nullptr) {
    *selectedURL = nil;
  }
  if (IOSDocumentCancellationRequested(cancellationRequested)) {
    cancelled = true;
    return false;
  }

  dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
  __block AsoDocumentHandoffDelegate *delegate =
      [[AsoDocumentHandoffDelegate alloc]
          initWithSemaphore:semaphore
             operationToken:operationToken];
  delegate.commitHandler = commitHandler;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      if (IOSDocumentCancellationRequested(cancellationRequested)) {
        [delegate finishWithURL:nil error:@"" cancelled:YES];
        return;
      }
      gActiveIOSDocumentHandoffDelegate = delegate;
      UIWindow *window = FindActiveWindow();
      UIViewController *presenting =
          window != nil ? TopViewController(window.rootViewController) : nil;
      if (presenting == nil) {
        [delegate finishWithURL:nil
                          error:@"No active view controller is available."
                      cancelled:NO];
        return;
      }

      @try {
        UIDocumentPickerViewController *picker = nil;
        if (exportURL != nil) {
          picker = [[UIDocumentPickerViewController alloc]
              initForExportingURLs:@[ exportURL ]
                            asCopy:YES];
        } else {
          UTType *contentType =
              directoryImport ? UTTypeFolder : [UTType typeWithMIMEType:mimeType];
          if (contentType == nil) {
            [delegate finishWithURL:nil
                              error:@"The requested document type is unsupported."
                          cancelled:NO];
            return;
          }
          picker = [[UIDocumentPickerViewController alloc]
              initForOpeningContentTypes:@[ contentType ]
                                  asCopy:NO];
        }
        picker.delegate = delegate;
        delegate.picker = picker;
        picker.allowsMultipleSelection = NO;
        picker.modalPresentationStyle = UIModalPresentationFormSheet;
        picker.presentationController.delegate = delegate;
        if (IOSDocumentCancellationRequested(cancellationRequested)) {
          [delegate finishWithURL:nil error:@"" cancelled:YES];
          return;
        }
        [presenting presentViewController:picker animated:YES completion:nil];
      } @catch (NSException *exception) {
        [delegate finishWithURL:nil
                          error:exception.reason ?:
                              @"Could not present the document picker."
                      cancelled:NO];
      }
    }
  });

  dispatch_semaphore_wait(semaphore, DISPATCH_TIME_FOREVER);
  cancelled = delegate.cancelled == YES;
  errorMessage = NSStringToString(delegate.errorMessage);
  if (selectedURL != nullptr) {
    *selectedURL = delegate.selectedURL;
  }
  delegate = nil;
  return !cancelled && errorMessage.empty();
}

std::string IOSErrorResult(NSString *message, std::string_view fallback) {
  std::string value = NSStringToString(message);
  if (value.empty()) {
    value = std::string(fallback);
  }
  return std::string(kIOSDocumentErrorPrefix) + value;
}
} // namespace

void CancelIOSDocument(std::uint64_t operationToken) {
  CancelIOSDocumentIO(operationToken);
  dispatch_async(dispatch_get_main_queue(), ^{
    AsoDocumentHandoffDelegate *delegate =
        gActiveIOSDocumentHandoffDelegate;
    if (delegate == nil || delegate.operationToken != operationToken) {
      return;
    }
    if (delegate.dismissalOutcomePending) {
      return;
    }
    UIDocumentPickerViewController *picker = delegate.picker;
    delegate.programmaticCancellationPending = YES;
    [delegate finishAfterDismissingPicker:picker
                                      URL:nil
                                    error:@""
                                cancelled:YES];
  });
}

bool ValidateIOSTemporaryDocument(const std::filesystem::path &localPath,
                                  std::string &errorMessage) {
  @autoreleasepool {
    return ValidateIOSTemporaryDocumentPath(localPath, false, false,
                                            errorMessage);
  }
}

bool CleanupIOSTemporaryDocument(const std::filesystem::path &localPath,
                                 std::string &errorMessage) {
  @autoreleasepool {
    if (!ValidateIOSTemporaryDocumentPath(localPath, true, true,
                                          errorMessage)) {
      return false;
    }
    std::error_code error;
    if (!std::filesystem::remove(localPath, error) &&
        error != std::errc::no_such_file_or_directory) {
      errorMessage = "Temporary iOS document cleanup failed: " +
                     error.message();
      return false;
    }
    errorMessage.clear();
    return true;
  }
}

bool ValidateIOSTemporaryDirectory(const std::filesystem::path &localPath,
                                   std::string &errorMessage) {
  @autoreleasepool {
    return ValidateIOSTemporaryPath(localPath, true, false, false,
                                    errorMessage);
  }
}

bool CleanupIOSTemporaryDirectory(const std::filesystem::path &localPath,
                                  std::string &errorMessage) {
  @autoreleasepool {
    if (!ValidateIOSTemporaryPath(localPath, true, true, true,
                                  errorMessage)) {
      return false;
    }
    std::error_code error;
    const auto status = std::filesystem::symlink_status(localPath, error);
    if (error == std::errc::no_such_file_or_directory) {
      error.clear();
    } else if (error) {
      errorMessage = "Temporary iOS directory cleanup failed: " +
                     error.message();
      return false;
    } else if (std::filesystem::is_symlink(status)) {
      std::filesystem::remove(localPath, error);
    } else {
      std::filesystem::remove_all(localPath, error);
    }
    if (error) {
      errorMessage = "Temporary iOS directory cleanup failed: " +
                     error.message();
      return false;
    }
    errorMessage.clear();
    return true;
  }
}

std::string ImportIOSDocument(std::uint64_t operationToken,
                              const std::string &mimeType,
                              std::uint64_t maxBytes,
                              const std::atomic_bool *cancellationRequested,
                              std::string *originalSourceName) {
  @autoreleasepool {
    if (mimeType.empty() || maxBytes == 0) {
      return std::string(kIOSDocumentErrorPrefix) +
             "Invalid document import request.";
    }
    if (IOSDocumentCancellationRequested(cancellationRequested)) {
      return std::string(kIOSDocumentCancelled);
    }

    NSURL *selectedURL = nil;
    bool cancelled = false;
    std::string pickerError;
    if (!WaitForIOSDocumentPicker(operationToken, NSStringFromUtf8(mimeType),
                                  false, nil, nil, &selectedURL, cancelled,
                                  pickerError, cancellationRequested)) {
      if (cancelled) {
        return std::string(kIOSDocumentCancelled);
      }
      return std::string(kIOSDocumentErrorPrefix) + pickerError;
    }
    if (originalSourceName != nullptr) {
      *originalSourceName = NSStringToString(selectedURL.lastPathComponent);
    }

    BOOL accessing = [selectedURL startAccessingSecurityScopedResource];
    NSString *storageError = nil;
    NSString *baseDirectory = IOSDocumentHandoffBaseDirectory(&storageError);
    if (baseDirectory == nil) {
      if (accessing) {
        [selectedURL stopAccessingSecurityScopedResource];
      }
      return IOSErrorResult(storageError, "Private document storage failed.");
    }
    NSString *fileName = [NSString
        stringWithFormat:@"%@.zip", NSUUID.UUID.UUIDString];
    NSString *destination =
        [baseDirectory stringByAppendingPathComponent:fileName];
    NSString *copyError = nil;
    bool copyCancelled = false;
    const bool copied = CopyIOSDocumentURLBounded(
        selectedURL, destination, operationToken, maxBytes, cancellationRequested,
        copyCancelled, &copyError);
    if (accessing) {
      [selectedURL stopAccessingSecurityScopedResource];
    }
    if (!copied) {
      if (copyCancelled) {
        return std::string(kIOSDocumentCancelled);
      }
      return IOSErrorResult(copyError, "Private document copy failed.");
    }
    return NSStringToString(destination);
  }
}

std::string ImportIOSDirectory(
    std::uint64_t operationToken, std::uint64_t maxBytes,
    std::uint64_t maxFiles, std::uint32_t maxDepth,
    std::uint32_t maxPathBytes, std::uint64_t maxRegularFileBytes,
    const std::atomic_bool *cancellationRequested,
    std::string *originalSourceName) {
  @autoreleasepool {
    if (maxBytes == 0 || maxFiles == 0 || maxDepth == 0 ||
        maxPathBytes == 0 || maxRegularFileBytes == 0) {
      return std::string(kIOSDocumentErrorPrefix) +
             "Invalid folder import request.";
    }
    if (IOSDocumentCancellationRequested(cancellationRequested)) {
      return std::string(kIOSDocumentCancelled);
    }

    NSURL *selectedURL = nil;
    bool cancelled = false;
    std::string pickerError;
    if (!WaitForIOSDocumentPicker(operationToken, @"", true, nil, nil,
                                  &selectedURL, cancelled, pickerError,
                                  cancellationRequested)) {
      if (cancelled) {
        return std::string(kIOSDocumentCancelled);
      }
      return std::string(kIOSDocumentErrorPrefix) + pickerError;
    }
    if (originalSourceName != nullptr) {
      *originalSourceName = NSStringToString(selectedURL.lastPathComponent);
    }

    BOOL accessing = [selectedURL startAccessingSecurityScopedResource];
    NSString *storageError = nil;
    NSString *baseDirectory = IOSDocumentHandoffBaseDirectory(&storageError);
    if (baseDirectory == nil) {
      if (accessing) {
        [selectedURL stopAccessingSecurityScopedResource];
      }
      return IOSErrorResult(storageError, "Private folder storage failed.");
    }
    NSString *destination =
        CreateIOSPrivateImportDirectory(baseDirectory, &storageError);
    if (destination == nil) {
      if (accessing) {
        [selectedURL stopAccessingSecurityScopedResource];
      }
      return IOSErrorResult(storageError, "Private folder storage failed.");
    }

    NSString *copyError = nil;
    bool copyCancelled = false;
    const bool copied = CopyIOSDirectoryURLBounded(
        selectedURL, destination, operationToken, maxBytes, maxFiles,
        maxDepth, maxPathBytes, maxRegularFileBytes, cancellationRequested,
        copyCancelled, &copyError);
    if (accessing) {
      [selectedURL stopAccessingSecurityScopedResource];
    }
    if (!copied || IOSDocumentCancellationRequested(cancellationRequested)) {
      CleanupIOSIssuedDirectoryAfterFailure(destination);
      if (copyCancelled ||
          IOSDocumentCancellationRequested(cancellationRequested)) {
        return std::string(kIOSDocumentCancelled);
      }
      return IOSErrorResult(copyError, "Private folder copy failed.");
    }
    return NSStringToString(destination);
  }
}

std::string ExportIOSDocument(std::uint64_t operationToken,
                              const std::filesystem::path &localPath,
                              const std::string &mimeType,
                              const std::string &suggestedName,
                              std::uint64_t maxBytes,
                              const std::atomic_bool *cancellationRequested,
                              std::function<bool()> commitHandler) {
  @autoreleasepool {
    if (localPath.empty() || mimeType.empty() || suggestedName.empty() ||
        suggestedName == "." || suggestedName == ".." ||
        suggestedName.find('/') != std::string::npos ||
        suggestedName.find('\\') != std::string::npos || maxBytes == 0) {
      return std::string(kIOSDocumentErrorPrefix) +
             "Invalid document export request.";
    }
    if (IOSDocumentCancellationRequested(cancellationRequested)) {
      return std::string(kIOSDocumentCancelled);
    }

    NSString *storageError = nil;
    NSString *baseDirectory = IOSDocumentHandoffBaseDirectory(&storageError);
    if (baseDirectory == nil) {
      return IOSErrorResult(storageError, "Private document storage failed.");
    }
    NSString *exportDirectory = [baseDirectory
        stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSError *directoryError = nil;
    if (![NSFileManager.defaultManager
            createDirectoryAtPath:exportDirectory
      withIntermediateDirectories:NO
                       attributes:@{NSFilePosixPermissions : @0700}
                            error:&directoryError]) {
      return IOSErrorResult(directoryError.localizedDescription,
                            "Private export staging failed.");
    }
    NSString *stagedPath = [exportDirectory
        stringByAppendingPathComponent:NSStringFromUtf8(suggestedName)];
    NSURL *sourceURL =
        [NSURL fileURLWithPath:NSStringFromUtf8(IOSPathToUtf8(localPath))];
    NSString *copyError = nil;
    bool copyCancelled = false;
    if (!CopyIOSDocumentURLBounded(sourceURL, stagedPath, operationToken, maxBytes,
                                   cancellationRequested, copyCancelled,
                                   &copyError)) {
      [NSFileManager.defaultManager removeItemAtPath:exportDirectory error:nil];
      if (copyCancelled) {
        return std::string(kIOSDocumentCancelled);
      }
      return IOSErrorResult(copyError, "Private export staging failed.");
    }

    bool cancelled = false;
    std::string pickerError;
    BOOL (^nativeCommitHandler)(void) = ^BOOL {
      try {
        return commitHandler && commitHandler() ? YES : NO;
      } catch (...) {
        return NO;
      }
    };
    const bool picked = WaitForIOSDocumentPicker(
        operationToken, NSStringFromUtf8(mimeType), false,
        [NSURL fileURLWithPath:stagedPath], nativeCommitHandler,
        nullptr, cancelled, pickerError, cancellationRequested);
    [NSFileManager.defaultManager removeItemAtPath:exportDirectory error:nil];
    if (!picked) {
      if (cancelled) {
        return std::string(kIOSDocumentCancelled);
      }
      return std::string(kIOSDocumentErrorPrefix) + pickerError;
    }
    return std::string(kIOSDocumentSuccess);
  }
}

void *StartIOSSecurityScopedResource(const std::string &path,
                                      const std::string &bookmark,
                                      std::string &resolvedPath,
                                      std::string &errorMessage) {
  resolvedPath = path;
  errorMessage.clear();
  if (bookmark.empty()) {
    return nullptr;
  }

  @autoreleasepool {
    NSString *bookmarkString = NSStringFromUtf8(bookmark);
    NSData *bookmarkData =
        [[NSData alloc] initWithBase64EncodedString:bookmarkString options:0];
    if (bookmarkData == nil) {
      errorMessage = "Invalid folder access bookmark";
      return nullptr;
    }

    NSError *resolveError = nil;
    BOOL stale = NO;
    NSURL *url = [NSURL
        URLByResolvingBookmarkData:bookmarkData
                            options:0
                      relativeToURL:nil
                bookmarkDataIsStale:&stale
                              error:&resolveError];
    if (url == nil) {
      errorMessage =
          resolveError.localizedDescription != nil
              ? NSStringToString(resolveError.localizedDescription)
              : "Failed to resolve folder access";
      return nullptr;
    }

    resolvedPath = NSStringToString(url.path);
    BOOL accessing = [url startAccessingSecurityScopedResource];
    if (!accessing) {
      errorMessage = "Failed to start folder access";
    }
    if (stale) {
      SDL_Log("Folder access bookmark is stale for %s",
              resolvedPath.c_str());
    }

    AsoSecurityScopedResource *resource =
        [[AsoSecurityScopedResource alloc] initWithURL:url accessing:accessing];
    return (__bridge_retained void *)resource;
  }
}

void StopIOSSecurityScopedResource(void *resource) {
  if (resource == nullptr) {
    return;
  }
  AsoSecurityScopedResource *scoped =
      (__bridge_transfer AsoSecurityScopedResource *)resource;
  [scoped stopAccess];
}

std::string GetIOSDocumentsPath() {
  @autoreleasepool {
    NSFileManager *manager = NSFileManager.defaultManager;
    NSArray<NSString *> *paths = NSSearchPathForDirectoriesInDomains(
        NSDocumentDirectory, NSUserDomainMask, YES);
    if (paths.count == 0) {
      return {};
    }
    NSURL *directory =
        [NSURL fileURLWithPath:paths.firstObject isDirectory:YES];
    NSURL *resolvedDirectory = directory.URLByResolvingSymlinksInPath;
    if (resolvedDirectory == nil) {
      return {};
    }
    NSError *error = nil;
    if (![manager createDirectoryAtURL:resolvedDirectory
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:&error]) {
      return {};
    }
    return std::string(resolvedDirectory.fileSystemRepresentation);
  }
}

std::string GetIOSApplicationSupportPath() {
  @autoreleasepool {
    NSFileManager *manager = NSFileManager.defaultManager;
    NSURL *base = [manager URLForDirectory:NSApplicationSupportDirectory
                                  inDomain:NSUserDomainMask
                         appropriateForURL:nil
                                    create:YES
                                     error:nil];
    if (base == nil) {
      return {};
    }
    NSURL *resolvedBase = base.URLByResolvingSymlinksInPath;
    if (resolvedBase == nil) {
      return {};
    }
    NSURL *directory =
        [resolvedBase URLByAppendingPathComponent:@"AsoBMaShow"
                                      isDirectory:YES];
    NSError *error = nil;
    if (![manager createDirectoryAtURL:directory
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:&error]) {
      return {};
    }
    if (![directory setResourceValue:@YES
                              forKey:NSURLIsExcludedFromBackupKey
                               error:&error]) {
      // The directory is still usable if iOS rejects this advisory cache
      // attribute. Do not prevent the user-visible skin root from starting.
      NSLog(@"Could not exclude private skin storage from backup: %@", error);
    }
    return std::string(directory.fileSystemRepresentation);
  }
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

bool PostURLTextIOS(const std::string &url, std::string &body,
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
    request.HTTPMethod = @"POST";
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
      errorMessage = "Timed out while posting " + url;
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
                     " while posting " + url;
      return false;
    }

    if (responseData == nil) {
      errorMessage = "No response body while posting " + url;
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

bool DownloadURLBinaryIOS(const std::string &url,
                          std::vector<unsigned char> &body,
                          std::string &errorMessage,
                          IOSDownloadProgressCallback progressCallback,
                          void *progressContext) {
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
        timeoutInterval:180.0];
    [request setValue:@"AsoBMaShow" forHTTPHeaderField:@"User-Agent"];

    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    AsoBinaryDownloadDelegate *delegate =
        [[AsoBinaryDownloadDelegate alloc] init];
    delegate->semaphore = semaphore;
    delegate->progressCallback = progressCallback;
    delegate->progressContext = progressContext;
    NSURLSessionConfiguration *configuration =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    configuration.requestCachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    NSURLSession *session = [NSURLSession sessionWithConfiguration:configuration
                                                          delegate:delegate
                                                     delegateQueue:nil];
    NSURLSessionDownloadTask *task = [session downloadTaskWithRequest:request];
    [task resume];
    const long waitResult = dispatch_semaphore_wait(
        semaphore, dispatch_time(DISPATCH_TIME_NOW, 190 * NSEC_PER_SEC));
    if (waitResult != 0) {
      [task cancel];
      [session invalidateAndCancel];
      errorMessage = "Timed out while downloading " + url;
      return false;
    }
    [session finishTasksAndInvalidate];

    if (delegate->requestError != nil) {
      errorMessage =
          std::string([[delegate->requestError localizedDescription] UTF8String]);
      return false;
    }

    NSHTTPURLResponse *httpResponse =
        [delegate->urlResponse isKindOfClass:[NSHTTPURLResponse class]]
            ? (NSHTTPURLResponse *)delegate->urlResponse
            : nil;
    if (httpResponse != nil && httpResponse.statusCode >= 400) {
      errorMessage = "HTTP " + std::to_string(httpResponse.statusCode) +
                     " while downloading " + url;
      return false;
    }

    if (delegate->responseData == nil) {
      errorMessage = "No response body while downloading " + url;
      return false;
    }

    const auto *bytes =
        static_cast<const unsigned char *>(delegate->responseData.bytes);
    body.assign(bytes, bytes + delegate->responseData.length);
    return true;
  }
}

bool OpenURLInIOSBrowser(const std::string &url, std::string &errorMessage) {
  @autoreleasepool {
    NSString *urlString = [NSString stringWithUTF8String:url.c_str()];
    NSURL *nsUrl = [NSURL URLWithString:urlString];
    if (nsUrl == nil) {
      errorMessage = "Invalid URL: " + url;
      return false;
    }

    if ([NSThread isMainThread]) {
      [UIApplication.sharedApplication openURL:nsUrl
                                       options:@{}
                             completionHandler:nil];
      return true;
    }

    __block BOOL opened = NO;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
      [UIApplication.sharedApplication
                    openURL:nsUrl
                    options:@{}
          completionHandler:^(BOOL success) {
            opened = success;
            dispatch_semaphore_signal(semaphore);
          }];
    });
    const long waitResult = dispatch_semaphore_wait(
        semaphore, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (waitResult != 0) {
      errorMessage = "Timed out while opening URL";
      return false;
    }
    if (!opened) {
      errorMessage = "Could not open URL";
      return false;
    }
    return true;
  }
}

bool RevealIOSFileInFiles(const std::string &filePath,
                          const IOSNormalizedRect &sourceAnchor,
                          std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    NSString *path = NSStringFromUtf8(filePath);
    if (path == nil || path.length == 0) {
      errorMessage = "Chart file path is empty";
      return false;
    }
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
      errorMessage = "Chart file does not exist";
      return false;
    }

    NSURL *fileURL = [NSURL fileURLWithPath:path];
    if (fileURL == nil) {
      errorMessage = "Invalid chart file URL";
      return false;
    }

    const IOSNormalizedRect normalizedSourceAnchor = sourceAnchor;
    BOOL (^presentBlock)(void) = ^BOOL {
      @autoreleasepool {
        UIWindow *window = FindActiveWindow();
        UIViewController *presenting =
            window != nil ? TopViewController(window.rootViewController) : nil;
        UIView *presentingView = presenting.view;
        if (presenting == nil || presentingView == nil) {
          return NO;
        }

        gIOSRevealFileController =
            [UIDocumentInteractionController interactionControllerWithURL:fileURL];
        if (gIOSRevealFileController == nil) {
          return NO;
        }

        const CGRect bounds = presentingView.bounds;
        const CGFloat boundsWidth = CGRectGetWidth(bounds);
        const CGFloat boundsHeight = CGRectGetHeight(bounds);
        CGRect sourceRect = CGRectMake(
            CGRectGetMinX(bounds) +
                static_cast<CGFloat>(normalizedSourceAnchor.x) * boundsWidth,
            CGRectGetMinY(bounds) +
                static_cast<CGFloat>(normalizedSourceAnchor.y) * boundsHeight,
            static_cast<CGFloat>(normalizedSourceAnchor.width) * boundsWidth,
            static_cast<CGFloat>(normalizedSourceAnchor.height) *
                boundsHeight);
        if (CGRectGetWidth(sourceRect) <= 0.0 ||
            CGRectGetHeight(sourceRect) <= 0.0) {
          sourceRect = CGRectMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds),
                                  1.0, 1.0);
        }
        return [gIOSRevealFileController presentOptionsMenuFromRect:sourceRect
                                                            inView:presentingView
                                                          animated:YES];
      }
    };

    if ([NSThread isMainThread]) {
      if (!presentBlock()) {
        errorMessage = "Could not open the Files options menu";
        return false;
      }
      return true;
    }

    dispatch_async(dispatch_get_main_queue(), ^{
      if (!presentBlock()) {
        SDL_Log("Could not open the Files options menu for %s",
                filePath.c_str());
      }
    });
    return true;
  }
}

bool SaveVideoToIOSPhotos(const std::string &filePath,
                          std::string &errorMessage) {
  @autoreleasepool {
    if (!RequestIOSPhotoAddAuthorization(errorMessage)) {
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
              [PHAssetCreationRequest creationRequestForAsset];
          PHAssetResourceCreationOptions *options =
              [[PHAssetResourceCreationOptions alloc] init];
          options.shouldMoveFile = YES;
          [request addResourceWithType:PHAssetResourceTypeVideo
                               fileURL:fileUrl
                               options:options];
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

    if (preparedPath != nil && ![preparedPath isEqualToString:path]) {
      [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    } else if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
      [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    }
    return true;
  }
  return false;
}

bool SaveImageToIOSPhotos(const std::string &filePath,
                          std::string &errorMessage) {
  @autoreleasepool {
    if (!RequestIOSPhotoAddAuthorization(errorMessage)) {
      return false;
    }

    NSString *path = NSStringFromUtf8(filePath);
    if (path == nil || path.length == 0) {
      errorMessage = "Image export path is empty";
      return false;
    }
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
      errorMessage = "Image export file does not exist";
      return false;
    }

    NSURL *fileUrl = [NSURL fileURLWithPath:path];
    __block BOOL saveSucceeded = NO;
    __block BOOL requestCreated = NO;
    __block NSError *saveError = nil;
    dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

    [[PHPhotoLibrary sharedPhotoLibrary]
        performChanges:^{
          PHAssetCreationRequest *request =
              [PHAssetCreationRequest creationRequestForAsset];
          PHAssetResourceCreationOptions *options =
              [[PHAssetResourceCreationOptions alloc] init];
          options.shouldMoveFile = YES;
          [request addResourceWithType:PHAssetResourceTypePhoto
                               fileURL:fileUrl
                               options:options];
          requestCreated = request != nil;
        }
        completionHandler:^(BOOL success, NSError *error) {
          saveSucceeded = success;
          saveError = error;
          dispatch_semaphore_signal(semaphore);
        }];

    const long waitResult = dispatch_semaphore_wait(
        semaphore, dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_SEC));
    if (waitResult != 0) {
      errorMessage = "Timed out saving image to Photos";
      return false;
    }

    if (!saveSucceeded || !requestCreated) {
      if (saveError != nil) {
        errorMessage =
            std::string([[saveError localizedDescription] UTF8String]);
      } else {
        errorMessage = "Failed to save image to Photos";
      }
      return false;
    }

    if ([[NSFileManager defaultManager] fileExistsAtPath:path]) {
      [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    }
    return true;
  }
  return false;
}

bool GetIOSFileExcludedFromBackup(const std::string &filePath, bool &excluded,
                                  std::string &errorMessage) {
  excluded = false;
  errorMessage.clear();
  @autoreleasepool {
    NSString *path = NSStringFromUtf8(filePath);
    if (path == nil || path.length == 0) {
      errorMessage = "File path is empty";
      return false;
    }

    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    NSError *error = nil;
    NSDictionary<NSURLResourceKey, id> *values =
        [url resourceValuesForKeys:@[ NSURLIsExcludedFromBackupKey ]
                              error:&error];
    if (values == nil) {
      errorMessage =
          NSErrorMessage(error, "Failed to read iCloud Backup setting");
      return false;
    }

    NSNumber *value = values[NSURLIsExcludedFromBackupKey];
    excluded = value != nil && value.boolValue;
    return true;
  }
}

bool SetIOSFileExcludedFromBackup(const std::string &filePath, bool excluded,
                                  std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    NSString *path = NSStringFromUtf8(filePath);
    if (path == nil || path.length == 0) {
      errorMessage = "File path is empty";
      return false;
    }

    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    NSError *error = nil;
    NSNumber *value = @(excluded);
    if (![url setResourceValue:value
                        forKey:NSURLIsExcludedFromBackupKey
                         error:&error]) {
      errorMessage =
          NSErrorMessage(error, "Failed to update iCloud Backup setting");
      return false;
    }
    return true;
  }
}

bool LoadIOSNativeMusicFile(const std::string &filePath,
                            const IOSNativeMusicMetadata &metadata,
                            std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      NSString *path = NSStringFromUtf8(filePath);
      if (path == nil || path.length == 0) {
        errorMessage = "Music file path is empty";
        return false;
      }
      if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        errorMessage = "Music file does not exist";
        return false;
      }
      if (!ActivateIOSNativeMusicAudioSession(errorMessage)) {
        return false;
      }

      NSURL *url = [NSURL fileURLWithPath:path];
      if (gIOSNativeMusicFinishedObserver != nil) {
        [[NSNotificationCenter defaultCenter]
            removeObserver:gIOSNativeMusicFinishedObserver];
        gIOSNativeMusicFinishedObserver = nil;
      }
      [gIOSNativeMusicPlayer pause];

      AVPlayerItem *item = [AVPlayerItem playerItemWithURL:url];
      if (item == nil) {
        errorMessage = "Could not load music";
        return false;
      }
      AVPlayer *player = [AVPlayer playerWithPlayerItem:item];
      player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
      player.automaticallyWaitsToMinimizeStalling = NO;

      gIOSNativeMusicPlayer = player;
      ApplyIOSNativeMusicPlaybackModeLocked();
      gIOSNativeMusicFinishedObserver =
          [[NSNotificationCenter defaultCenter]
              addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                          object:item
                           queue:nil
                      usingBlock:^(NSNotification *) {
                        bool finishedCurrentItem = false;
                        @synchronized(IOSNativeMusicLock()) {
                          finishedCurrentItem =
                              gIOSNativeMusicPlayer.currentItem == item;
                          if (finishedCurrentItem) {
                            UpdateIOSNativeMusicNowPlayingInfoLocked();
                          }
                        }
                        if (finishedCurrentItem) {
                          native_music_player::NotifyControlEvent(
                              native_music_player::ControlEvent::Finished);
                        }
                      }];
      gIOSNativeMusicMetadata = metadata;
      if (gIOSNativeMusicMetadata.durationMicros <= 0) {
        gIOSNativeMusicMetadata.durationMicros =
            IOSNativeMusicDurationMicrosLocked();
      }
      ConfigureIOSNativeMusicRemoteCommands();
      UpdateIOSNativeMusicNowPlayingInfoLocked();
      return true;
    }
  }
}

bool UpdateIOSNativeMusicMetadata(const IOSNativeMusicMetadata &metadata,
                                  std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      if (gIOSNativeMusicPlayer == nil) {
        return true;
      }
      const long long currentDurationMicros =
          IOSNativeMusicDurationMicrosLocked();
      gIOSNativeMusicMetadata = metadata;
      if (gIOSNativeMusicMetadata.durationMicros <= 0) {
        gIOSNativeMusicMetadata.durationMicros = currentDurationMicros;
      }
      UpdateIOSNativeMusicNowPlayingInfoLocked();
      return true;
    }
  }
}

bool UpdateIOSNativeMusicQueue(const IOSNativeMusicQueue &queue,
                               std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      gIOSNativeMusicQueue = queue;
      UpdateIOSNativeMusicNowPlayingInfoLocked();
      return true;
    }
  }
}

bool PlayIOSNativeMusic(std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      if (gIOSNativeMusicPlayer == nil) {
        errorMessage = "No music is loaded";
        return false;
      }
      if (!ActivateIOSNativeMusicAudioSession(errorMessage)) {
        return false;
      }
      [gIOSNativeMusicPlayer playImmediatelyAtRate:gIOSNativeMusicPlaybackRate];
      UpdateIOSNativeMusicNowPlayingInfoLocked();
      return true;
    }
  }
}

bool PauseIOSNativeMusic(std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      if (gIOSNativeMusicPlayer == nil) {
        errorMessage = "No music is loaded";
        return false;
      }
      [gIOSNativeMusicPlayer pause];
      UpdateIOSNativeMusicNowPlayingInfoLocked();
      return true;
    }
  }
}

bool StopIOSNativeMusic(std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      if (gIOSNativeMusicPlayer == nil) {
        errorMessage = "No music is loaded";
        return false;
      }
      [gIOSNativeMusicPlayer pause];
      [gIOSNativeMusicPlayer seekToTime:kCMTimeZero
                        toleranceBefore:kCMTimeZero
                         toleranceAfter:kCMTimeZero];
      UpdateIOSNativeMusicNowPlayingInfoLocked();
      MPNowPlayingInfoCenter *center = [MPNowPlayingInfoCenter defaultCenter];
      center.nowPlayingInfo = nil;
      if (@available(iOS 13.0, *)) {
        center.playbackState = MPNowPlayingPlaybackStateStopped;
      }

      return RestoreIOSForegroundAudioSession(errorMessage);
    }
  }
}

bool SeekIOSNativeMusic(long long positionMicros, std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      if (gIOSNativeMusicPlayer == nil) {
        errorMessage = "No music is loaded";
        return false;
      }
      const NSTimeInterval duration = static_cast<NSTimeInterval>(
          IOSNativeMusicDurationMicrosLocked()) / 1000000.0;
      const NSTimeInterval target = std::clamp<NSTimeInterval>(
          static_cast<NSTimeInterval>(std::max(0LL, positionMicros)) /
              1000000.0,
          0.0, duration > 0.0 ? duration : DBL_MAX);
      const bool wasPlaying = IOSNativeMusicIsPlayingLocked();
      const CMTime targetTime = CMTimeMakeWithSeconds(target, NSEC_PER_SEC);
      [gIOSNativeMusicPlayer seekToTime:targetTime
                        toleranceBefore:kCMTimeZero
                         toleranceAfter:kCMTimeZero];
      if (wasPlaying) {
        [gIOSNativeMusicPlayer
            playImmediatelyAtRate:gIOSNativeMusicPlaybackRate];
      }
      UpdateIOSNativeMusicNowPlayingInfoLocked();
      return true;
    }
  }
}

bool SetIOSNativeMusicPlaybackRate(int percent, bool timeStretch,
                                   std::string &errorMessage) {
  errorMessage.clear();
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      gIOSNativeMusicPlaybackRate =
          std::clamp(static_cast<float>(percent) / 100.0f, 0.5f, 2.0f);
      gIOSNativeMusicTimeStretch = timeStretch;
      if (gIOSNativeMusicPlayer != nil) {
        const bool wasPlaying = IOSNativeMusicIsPlayingLocked();
        ApplyIOSNativeMusicPlaybackModeLocked();
        if (wasPlaying) {
          [gIOSNativeMusicPlayer
              playImmediatelyAtRate:gIOSNativeMusicPlaybackRate];
        }
        UpdateIOSNativeMusicNowPlayingInfoLocked();
      }
      return true;
    }
  }
}

IOSNativeMusicState GetIOSNativeMusicState() {
  @autoreleasepool {
    @synchronized(IOSNativeMusicLock()) {
      IOSNativeMusicState state;
      state.loaded = gIOSNativeMusicPlayer != nil;
      if (!state.loaded) {
        return state;
      }
      state.playing = IOSNativeMusicIsPlayingLocked();
      state.positionMicros = static_cast<long long>(
          IOSNativeMusicCurrentTimeLocked() * 1000000.0);
      state.durationMicros = IOSNativeMusicDurationMicrosLocked();
      return state;
    }
  }
}

void *CreateIOSReplayVideoWriter(const std::string &wavPath,
                                 const std::string &outputPath, int width,
                                 int height, int fps, int64_t bitRate,
                                 std::string &errorMessage) {
  UniqueCleanupObject<IOSReplayVideoWriter, &IOSReplayVideoWriter::cancel>
      writer;
  try {
    @try {
      writer = makeUniqueCleanupObject<IOSReplayVideoWriter,
                                       &IOSReplayVideoWriter::cancel>();
      if (!writer->open(wavPath, outputPath, width, height, fps, bitRate,
                        errorMessage)) {
        writer.reset();
        return nullptr;
      }
      return writer.release();
    } @catch (NSException *exception) {
      errorMessage =
          "Replay video writer setup exception: " +
          NSExceptionMessage(exception, "Objective-C exception");
      return nullptr;
    }
  } catch (const std::exception &exception) {
    errorMessage =
        std::string("Replay video writer setup exception: ") +
        exception.what();
    return nullptr;
  } catch (...) {
    errorMessage = "Replay video writer setup exception";
    return nullptr;
  }
}

bool AppendIOSReplayVideoFrame(void *writer, const uint8_t *bgraFrame,
                               size_t frameIndex, std::string &errorMessage) {
  if (writer == nullptr) {
    errorMessage = "Replay video writer is unavailable";
    return false;
  }
  return static_cast<IOSReplayVideoWriter *>(writer)->appendFrame(
      bgraFrame, frameIndex, errorMessage);
}

bool FinishIOSReplayVideoWriter(void *writer,
                                IOSReplayVideoWriterProfile &profile,
                                std::string &errorMessage) {
  if (writer == nullptr) {
    errorMessage = "Replay video writer is unavailable";
    return false;
  }
  auto *iosWriter = static_cast<IOSReplayVideoWriter *>(writer);
  const bool success = iosWriter->finish(errorMessage);
  profile = iosWriter->profile;
  if (!success) {
    iosWriter->cancel();
  }
  delete iosWriter;
  return success;
}

void CancelIOSReplayVideoWriter(void *writer) {
  if (writer == nullptr) {
    return;
  }
  auto *iosWriter = static_cast<IOSReplayVideoWriter *>(writer);
  iosWriter->cancel();
  delete iosWriter;
}

bool GetIOSPreferredFullscreenDrawableSize(int currentWidth, int currentHeight,
                                           int logicalWidth, int logicalHeight,
                                           int &preferredWidth,
                                           int &preferredHeight) {
  @autoreleasepool {
    preferredWidth = 0;
    preferredHeight = 0;
    UIWindow *window = FindActiveWindow();
    if (window == nil) {
      return false;
    }
    UIScreen *screen = window.screen;
    if (screen == nil) {
      screen = UIScreen.mainScreen;
    }
    if (screen == nil) {
      return false;
    }

    const CGRect windowBounds = window.bounds;
    const CGRect screenBounds = screen.bounds;
    const int windowPointW = RoundedCGFloat(windowBounds.size.width);
    const int windowPointH = RoundedCGFloat(windowBounds.size.height);
    int screenPointW = RoundedCGFloat(screenBounds.size.width);
    int screenPointH = RoundedCGFloat(screenBounds.size.height);
    OrientSizeToMatch(screenPointW, screenPointH, windowPointW, windowPointH);
    if (std::abs(windowPointW - screenPointW) > 4 ||
        std::abs(windowPointH - screenPointH) > 4) {
      return false;
    }
    if (!SimilarAspect(windowPointW, windowPointH, screenPointW, screenPointH)) {
      return false;
    }

    int referenceWidth = currentWidth > 0 ? currentWidth : logicalWidth;
    int referenceHeight = currentHeight > 0 ? currentHeight : logicalHeight;
    if (referenceWidth <= 0 || referenceHeight <= 0) {
      return false;
    }

    int bestWidth = currentWidth;
    int bestHeight = currentHeight;
    if (bestWidth <= 0 || bestHeight <= 0) {
      bestWidth = logicalWidth;
      bestHeight = logicalHeight;
    }

    if (screen.currentMode != nil) {
      const CGSize modeSize = screen.currentMode.size;
      ConsiderDrawableCandidate(RoundedCGFloat(modeSize.width),
                                RoundedCGFloat(modeSize.height),
                                referenceWidth, referenceHeight, bestWidth,
                                bestHeight);
    }

    const CGRect nativeBounds = screen.nativeBounds;
    ConsiderDrawableCandidate(RoundedCGFloat(nativeBounds.size.width),
                              RoundedCGFloat(nativeBounds.size.height),
                              referenceWidth, referenceHeight, bestWidth,
                              bestHeight);
    ConsiderDrawableCandidate(
        RoundedCGFloat(screenBounds.size.width * screen.scale),
        RoundedCGFloat(screenBounds.size.height * screen.scale), referenceWidth,
        referenceHeight, bestWidth, bestHeight);
    ConsiderDrawableCandidate(
        RoundedCGFloat(screenBounds.size.width * screen.nativeScale),
        RoundedCGFloat(screenBounds.size.height * screen.nativeScale),
        referenceWidth, referenceHeight, bestWidth, bestHeight);

    if (bestWidth <= currentWidth + 8 || bestHeight <= currentHeight + 8) {
      return false;
    }

    preferredWidth = bestWidth;
    preferredHeight = bestHeight;
    return true;
  }
}

bool SetIOSMetalLayerDrawableSize(void *metalLayer, int width, int height) {
  @autoreleasepool {
    if (metalLayer == nullptr || width <= 0 || height <= 0) {
      return false;
    }
    id layerObject = (__bridge id)metalLayer;
    if (![layerObject isKindOfClass:[CAMetalLayer class]]) {
      return false;
    }

    CAMetalLayer *layer = (CAMetalLayer *)layerObject;
    const CGSize boundsSize = layer.bounds.size;
    if (boundsSize.width > 0.0 && boundsSize.height > 0.0) {
      const CGFloat scaleX = static_cast<CGFloat>(width) / boundsSize.width;
      const CGFloat scaleY = static_cast<CGFloat>(height) / boundsSize.height;
      const CGFloat maxScale = std::max(scaleX, scaleY);
      if (maxScale > 0.0 &&
          std::abs(scaleX - scaleY) <= maxScale * static_cast<CGFloat>(0.02)) {
        layer.contentsScale = (scaleX + scaleY) * static_cast<CGFloat>(0.5);
      }
    }
    layer.drawableSize = CGSizeMake(width, height);
    return true;
  }
}

void WaitIOSMainRunLoopForMicros(long long waitMicros) {
  @autoreleasepool {
    if (waitMicros <= 0) {
      return;
    }
    if (![NSThread isMainThread]) {
      [NSThread sleepForTimeInterval:
                    static_cast<NSTimeInterval>(waitMicros) / 1000000.0];
      return;
    }

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::microseconds(waitMicros);
    const auto serviceMode = [deadline](CFStringRef mode) {
      constexpr auto kRunLoopSlice = std::chrono::milliseconds(1);
      const auto remaining = deadline - Clock::now();
      if (remaining <= Clock::duration::zero()) {
        return;
      }
      const auto slice = std::min(remaining, Clock::duration(kRunLoopSlice));
      CFRunLoopRunInMode(
          mode, std::chrono::duration<double>(slice).count(), true);
    };

    while (Clock::now() < deadline) {
      serviceMode(kCFRunLoopDefaultMode);
      serviceMode((__bridge CFStringRef)UITrackingRunLoopMode);
    }
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
