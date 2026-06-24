#pragma once
#ifdef _WIN32
#include <windows.h>
#define TARGET_OS_WINDOWS 1
#elif __APPLE__

#include "TargetConditionals.h"
#if TARGET_OS_IPHONE && TARGET_IPHONE_SIMULATOR
// define something for simulator
#elif TARGET_OS_IPHONE
#else
#define TARGET_OS_OSX 1
#endif
#elif __ANDROID__
#define TARGET_OS_ANDROID 1
#elif __linux
#define TARGET_OS_LINUX 1
#elif __unix // all unices not caught above
// Unix
#elif __posix
// POSIX
#endif

#ifndef TARGET_OS_WINDOWS
#define TARGET_OS_WINDOWS 0
#endif
#ifndef TARGET_OS_OSX
#define TARGET_OS_OSX 0
#endif
#ifndef TARGET_OS_IOS
#define TARGET_OS_IOS 0
#endif
#ifndef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 0
#endif
#ifndef TARGET_OS_SIMULATOR
#define TARGET_OS_SIMULATOR 0
#endif
#ifndef TARGET_OS_ANDROID
#define TARGET_OS_ANDROID 0
#endif
#ifndef TARGET_OS_LINUX
#define TARGET_OS_LINUX 0
#endif

#define TARGET_OS_DESKTOP (TARGET_OS_OSX || TARGET_OS_WINDOWS)

enum TargetPlatform { Windows, MacOS, Linux, iOS, Android };

#if TARGET_OS_OSX
constexpr TargetPlatform TARGET_PLATFORM = MacOS;
#elif TARGET_OS_IOS
constexpr TargetPlatform TARGET_PLATFORM = iOS;
#elif TARGET_OS_IPHONE
constexpr TargetPlatform TARGET_PLATFORM = iOS;
#elif TARGET_OS_ANDROID
constexpr TargetPlatform TARGET_PLATFORM = Android;
#elif TARGET_OS_LINUX
constexpr TargetPlatform TARGET_PLATFORM = Linux;
#else
constexpr TargetPlatform TARGET_PLATFORM = Windows;
#endif
