#pragma once
#include "targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include <SDL2/SDL.h>
#include <cstddef>
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
  SelectionChanged,
  Submitted,
  Finished,
};

struct IOSNativeTextEditorState {
  std::string text;
  std::size_t selectionStart = 0;
  std::size_t selectionEnd = 0;
};

struct IOSNativeTextEditorConfig {
  std::string text;
  std::string placeholder;
  std::size_t selectionStart = 0;
  std::size_t selectionEnd = 0;
  int fontSize = 17;
};

using IOSNativeTextEditorCallback =
    void (*)(void *context, IOSNativeTextEditorEvent event,
             const IOSNativeTextEditorState &state);
using IOSDownloadProgressCallback =
    void (*)(void *context, std::uint64_t downloadedBytes,
             std::uint64_t totalBytes);

// get Documents path
std::string GetIOSDocumentsPath();
void *GetIOSWindowHandle(void *uiwindow);
void RegisterTouchEvent();
void RestoreIOSViewportAfterKeyboardFocus();
int GetIOSNativeTextEditorHeight();
std::vector<std::string> ListDocumentFilesRecursively();
bool PickIOSFolder(std::string &path, std::string &bookmark,
                   std::string &errorMessage);
void *StartIOSSecurityScopedResource(const std::string &path,
                                      const std::string &bookmark,
                                      std::string &resolvedPath,
                                      std::string &errorMessage);
void StopIOSSecurityScopedResource(void *resource);
IOSNormalizedSafeAreaInsets GetIOSSafeAreaInsetsNormalized();
bool GetIOSPreferredFullscreenDrawableSize(int currentWidth, int currentHeight,
                                           int logicalWidth, int logicalHeight,
                                           int &preferredWidth,
                                           int &preferredHeight);
bool SetIOSMetalLayerDrawableSize(void *metalLayer, int width, int height);
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
bool RequestIOSPhotoAddAuthorization(std::string &errorMessage);
bool SaveVideoToIOSPhotos(const std::string &filePath,
                          std::string &errorMessage);
bool SaveImageToIOSPhotos(const std::string &filePath,
                          std::string &errorMessage);
bool GetIOSFileExcludedFromBackup(const std::string &filePath, bool &excluded,
                                  std::string &errorMessage);
bool SetIOSFileExcludedFromBackup(const std::string &filePath, bool excluded,
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
void SetIOSNativeTextEditorSelection(void *context,
                                     std::size_t selectionStart,
                                     std::size_t selectionEnd);
#endif
