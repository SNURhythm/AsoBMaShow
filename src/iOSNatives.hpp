#pragma once
#include "targets.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include <string>
#include <vector>

struct IOSNormalizedSafeAreaInsets {
  float top = 0.0f;
  float left = 0.0f;
  float bottom = 0.0f;
  float right = 0.0f;
};

// get Documents path
std::string GetIOSDocumentsPath();
void *GetIOSWindowHandle(void *uiwindow);
void RegisterTouchEvent();
std::vector<std::string> ListDocumentFilesRecursively();
IOSNormalizedSafeAreaInsets GetIOSSafeAreaInsetsNormalized();
#endif
