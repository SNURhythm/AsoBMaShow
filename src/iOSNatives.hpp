#pragma once
#include "targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include <SDL2/SDL.h>
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

// get Documents path
std::string GetIOSDocumentsPath();
void *GetIOSWindowHandle(void *uiwindow);
void RegisterTouchEvent();
std::vector<std::string> ListDocumentFilesRecursively();
IOSNormalizedSafeAreaInsets GetIOSSafeAreaInsetsNormalized();
bool DownloadURLTextIOS(const std::string &url, std::string &body,
                        std::string &errorMessage);
IOSSystemTextMetrics GetIOSSystemTextMetrics(int fontSize);
int MeasureIOSSystemTextWidth(const std::string &utf8, int fontSize);
SDL_Surface *RenderIOSSystemTextSurface(const std::string &utf8, int fontSize,
                                        SDL_Color color);
#endif
