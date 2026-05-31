#include "iOSNatives.hpp"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include <AudioToolbox/AudioToolbox.h>
#include <AVFoundation/AVFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <CoreVideo/CoreVideo.h>
#include <Foundation/Foundation.h>
#include <Photos/Photos.h>
#include <UIKit/UIKit.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>

namespace {
constexpr int kIOSReplaySampleRate = 44100;
constexpr int kIOSReplayChannels = 2;

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
  if (description == nil) {
    return fallback != nullptr ? std::string(fallback) : std::string();
  }
  return std::string([description UTF8String]);
}

std::string AVWriterErrorMessage(AVAssetWriter *writer,
                                 const char *fallback) {
  if (writer != nil && writer.error != nil) {
    return NSErrorMessage(writer.error, fallback);
  }
  return fallback != nullptr ? std::string(fallback) : std::string();
}

bool WaitForWriterInput(AVAssetWriter *writer, AVAssetWriterInput *input,
                        NSTimeInterval timeoutSeconds,
                        std::string &errorMessage) {
  NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeoutSeconds];
  while (![input isReadyForMoreMediaData]) {
    if (writer.status == AVAssetWriterStatusFailed) {
      errorMessage = AVWriterErrorMessage(writer, "Replay video writer failed");
      return false;
    }
    if (writer.status == AVAssetWriterStatusCancelled) {
      errorMessage = "Replay video writer was cancelled";
      return false;
    }
    if (writer.status == AVAssetWriterStatusCompleted) {
      errorMessage = "Replay video writer already finished";
      return false;
    }
    if ([deadline timeIntervalSinceNow] <= 0.0) {
      errorMessage = "Timed out waiting for replay video writer";
      return false;
    }
    [NSThread sleepForTimeInterval:0.001];
  }
  return true;
}

class IOSReplayVideoWriter {
public:
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
        AVVideoProfileLevelKey : AVVideoProfileLevelH264HighAutoLevel,
      };
      NSDictionary *videoSettings = @{
        AVVideoCodecKey : AVVideoCodecTypeH264,
        AVVideoWidthKey : @(width),
        AVVideoHeightKey : @(height),
        AVVideoCompressionPropertiesKey : compressionProperties,
      };
      videoInput = [AVAssetWriterInput
          assetWriterInputWithMediaType:AVMediaTypeVideo
                         outputSettings:videoSettings];
      videoInput.expectsMediaDataInRealTime = NO;
      videoInput.mediaTimeScale = std::max(fps, 1);
      if (![writer canAddInput:videoInput]) {
        errorMessage = "Replay video writer could not add video input";
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

      NSDictionary *audioSettings = @{
        AVFormatIDKey : @(kAudioFormatMPEG4AAC),
        AVSampleRateKey : @(kIOSReplaySampleRate),
        AVNumberOfChannelsKey : @(kIOSReplayChannels),
        AVEncoderBitRateKey : @(192000),
      };
      audioInput = [AVAssetWriterInput
          assetWriterInputWithMediaType:AVMediaTypeAudio
                         outputSettings:audioSettings];
      audioInput.expectsMediaDataInRealTime = NO;
      audioInput.mediaTimeScale = kIOSReplaySampleRate;
      if (![writer canAddInput:audioInput]) {
        errorMessage = "Replay video writer could not add audio input";
        return false;
      }
      [writer addInput:audioInput];

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
        return false;
      }
      if (!WaitForWriterInput(writer, videoInput, 300.0, errorMessage)) {
        return false;
      }
      if (adaptor.pixelBufferPool == nullptr) {
        errorMessage = "Replay video writer pixel buffer pool is unavailable";
        return false;
      }

      CVPixelBufferRef pixelBuffer = nullptr;
      CVReturn ret = CVPixelBufferPoolCreatePixelBuffer(
          kCFAllocatorDefault, adaptor.pixelBufferPool, &pixelBuffer);
      if (ret != kCVReturnSuccess || pixelBuffer == nullptr) {
        errorMessage = "Failed to allocate replay video pixel buffer";
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
      if (!appendAudio(errorMessage)) {
        [writer cancelWriting];
        return false;
      }
      [audioInput markAsFinished];

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
          semaphore, dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_SEC));
      profile.finishMicros += ElapsedMicros(finishStart);
      if (waitResult != 0) {
        [writer cancelWriting];
        errorMessage = "Timed out finishing replay video writer";
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
  }

  IOSReplayVideoWriterProfile profile;

private:
  bool appendAudio(std::string &errorMessage) {
    const auto audioStart = std::chrono::steady_clock::now();
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
      profile.audioAppendMicros += ElapsedMicros(audioStart);
      return true;
    }

    NSError *error = nil;
    AVAssetReader *reader = [[AVAssetReader alloc] initWithAsset:asset
                                                           error:&error];
    if (reader == nil) {
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
    AVAssetReaderTrackOutput *readerOutput =
        [[AVAssetReaderTrackOutput alloc] initWithTrack:tracks.firstObject
                                         outputSettings:readerSettings];
    readerOutput.alwaysCopiesSampleData = NO;
    if (![reader canAddOutput:readerOutput]) {
      errorMessage = "Replay audio reader could not add output";
      return false;
    }
    [reader addOutput:readerOutput];
    if (![reader startReading]) {
      errorMessage =
          NSErrorMessage(reader.error, "Failed to start replay audio reader");
      return false;
    }

    while (true) {
      CMSampleBufferRef sampleBuffer = [readerOutput copyNextSampleBuffer];
      if (sampleBuffer == nullptr) {
        break;
      }
      const bool ready =
          WaitForWriterInput(writer, audioInput, 120.0, errorMessage);
      BOOL appended = NO;
      if (ready) {
        appended = [audioInput appendSampleBuffer:sampleBuffer];
      }
      CFRelease(sampleBuffer);
      if (!ready) {
        return false;
      }
      if (!appended) {
        errorMessage =
            AVWriterErrorMessage(writer, "Failed to append replay audio");
        return false;
      }
    }

    if (reader.status == AVAssetReaderStatusFailed) {
      errorMessage =
          NSErrorMessage(reader.error, "Failed to finish replay audio reader");
      return false;
    }
    if (reader.status == AVAssetReaderStatusCancelled) {
      errorMessage = "Replay audio reader was cancelled";
      return false;
    }
    profile.audioAppendMicros += ElapsedMicros(audioStart);
    return true;
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
  bool videoFinished = false;
};
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

void *CreateIOSReplayVideoWriter(const std::string &wavPath,
                                 const std::string &outputPath, int width,
                                 int height, int fps, int64_t bitRate,
                                 std::string &errorMessage) {
  auto *writer = new IOSReplayVideoWriter();
  if (!writer->open(wavPath, outputPath, width, height, fps, bitRate,
                    errorMessage)) {
    delete writer;
    return nullptr;
  }
  return writer;
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
