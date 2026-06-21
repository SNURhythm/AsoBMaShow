#include "iOSNatives.hpp"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "RAII.h"
#include <AudioToolbox/AudioToolbox.h>
#include <AVFoundation/AVFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>
#include <Photos/Photos.h>
#include <QuartzCore/CAMetalLayer.h>
#include <UIKit/UIKit.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace {
constexpr int kIOSReplaySampleRate = 44100;
constexpr int kIOSReplayChannels = 2;
constexpr NSTimeInterval kIOSReplayInputWaitTimeoutSeconds = 2.0;
constexpr NSTimeInterval kIOSReplayFinishWaitTimeoutSeconds = 30.0;
constexpr double kIOSReplayAudioLeadSeconds = 0.5;
UIDocumentInteractionController *gIOSRevealFileController = nil;

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

  if (!IsPhotoAuthorizationAllowed(status)) {
    errorMessage = PhotoAuthorizationStatusMessage(status);
    return false;
  }
  return true;
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

@interface AsoNativeTextEditorView : UIView <UITextFieldDelegate> {
@private
  UITextField *_textField;
  __unsafe_unretained UIView *_rootView;
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
- (void)showInView:(UIView *)rootView;
- (void)hideWithNotifyFinished:(BOOL)notifyFinished;
- (void)keyboardFrameChanged:(NSNotification *)notification;
- (void)keyboardWillHide:(NSNotification *)notification;
- (UIViewAnimationOptions)animationOptionsForKeyboardNotification:
    (NSNotification *)notification;
- (void)updateFrameAnimated:(BOOL)animated
                   duration:(NSTimeInterval)duration
                    options:(UIViewAnimationOptions)options;
- (void)textFieldEditingChanged:(UITextField *)textField;
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
  _textField.text = NSStringFromUtf8(config.text);
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

- (void)showInView:(UIView *)rootView {
  if (rootView == nil) {
    return;
  }
  _rootView = rootView;
  [rootView addSubview:self];
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
  UIView *rootView = _rootView;
  if (rootView == nil) {
    return;
  }

  UIEdgeInsets safeInsets = UIEdgeInsetsZero;
  if (@available(iOS 11.0, *)) {
    safeInsets = rootView.safeAreaInsets;
  }

  CGRect bounds = rootView.bounds;
  CGFloat keyboardTop = bounds.size.height - safeInsets.bottom;
  if (_keyboardVisible && !CGRectIsEmpty(_lastKeyboardFrame)) {
    CGRect keyboardFrame = [rootView convertRect:_lastKeyboardFrame fromView:nil];
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
  const std::string text = NSStringToString(_textField.text);
  _callback(_context, event, text);
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
      UIView *rootView = window.rootViewController.view;
      if (rootView == nil) {
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
      [gNativeTextEditor showInView:rootView];
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

        const CGRect sourceRect =
            CGRectMake(CGRectGetMidX(presentingView.bounds),
                       CGRectGetMidY(presentingView.bounds), 1.0, 1.0);
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
