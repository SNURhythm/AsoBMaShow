#pragma once
#include "targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include <SDL2/SDL.h>
#include <cstdint>
#include <string>
#include <vector>

struct IOSNormalizedSafeAreaInsets {
  float top = 0.0f;
  float left = 0.0f;
  float bottom = 0.0f;
  float right = 0.0f;
};

struct IOSSystemTextMetrics {
  int ascent = 0;
  int descent = 0;
  int height = 0;
};

struct IOSReplayVideoWriterProfile {
  long long videoPixelBufferCopyMicros = 0;
  long long videoAppendMicros = 0;
  long long audioAppendMicros = 0;
  long long finishMicros = 0;
};

enum class IOSNativeTextEditorEvent {
  Changed,
  Submitted,
  Finished,
};

struct IOSNativeTextEditorConfig {
  std::string text;
  std::string placeholder;
  int fontSize = 17;
};

using IOSNativeTextEditorCallback =
    void (*)(void *context, IOSNativeTextEditorEvent event,
             const std::string &text);
using IOSDownloadProgressCallback =
    void (*)(void *context, std::uint64_t downloadedBytes,
             std::uint64_t totalBytes);

// get Documents path
std::string GetIOSDocumentsPath();
void *GetIOSWindowHandle(void *uiwindow);
void RegisterTouchEvent();
std::vector<std::string> ListDocumentFilesRecursively();
bool PickIOSFolder(std::string &path, std::string &bookmark,
                   std::string &errorMessage);
void *StartIOSSecurityScopedResource(const std::string &path,
                                      const std::string &bookmark,
                                      std::string &resolvedPath,
                                      std::string &errorMessage);
void StopIOSSecurityScopedResource(void *resource);
IOSNormalizedSafeAreaInsets GetIOSSafeAreaInsetsNormalized();
bool DownloadURLTextIOS(const std::string &url, std::string &body,
                        std::string &errorMessage);
bool PostURLTextIOS(const std::string &url, std::string &body,
                    std::string &errorMessage);
bool DownloadURLBinaryIOS(const std::string &url,
                          std::vector<unsigned char> &body,
                          std::string &errorMessage,
                          IOSDownloadProgressCallback progressCallback =
                              nullptr,
                          void *progressContext = nullptr);
bool OpenURLInIOSBrowser(const std::string &url, std::string &errorMessage);
bool RevealIOSFileInFiles(const std::string &filePath,
                          std::string &errorMessage);
bool SaveVideoToIOSPhotos(const std::string &filePath,
                          std::string &errorMessage);
void *CreateIOSReplayVideoWriter(const std::string &wavPath,
                                 const std::string &outputPath, int width,
                                 int height, int fps, int64_t bitRate,
                                 std::string &errorMessage);
bool AppendIOSReplayVideoFrame(void *writer, const uint8_t *bgraFrame,
                               size_t frameIndex, std::string &errorMessage);
bool FinishIOSReplayVideoWriter(void *writer,
                                IOSReplayVideoWriterProfile &profile,
                                std::string &errorMessage);
void CancelIOSReplayVideoWriter(void *writer);
IOSSystemTextMetrics GetIOSSystemTextMetrics(int fontSize);
int MeasureIOSSystemTextWidth(const std::string &utf8, int fontSize);
SDL_Surface *RenderIOSSystemTextSurface(const std::string &utf8, int fontSize,
                                        SDL_Color color);
void ShowIOSNativeTextEditor(const IOSNativeTextEditorConfig &config,
                             void *context,
                             IOSNativeTextEditorCallback callback);
void HideIOSNativeTextEditor(void *context, bool notifyFinished);
#endif
