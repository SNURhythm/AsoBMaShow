#pragma once
#include <string>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
namespace reminders {
enum class Permission { Unknown, NotDetermined, Denied, Granted, Unsupported };
#if defined(__ANDROID__) || TARGET_OS_IPHONE
inline constexpr bool supported = true;
// These calls are safe on the application owner thread; adapters serialize OS
// operations. JSON contains at most 56 dated requests, all owned by Combo.
void refreshPermission();
Permission permission();
void requestPermission();
void openSettings();
void replacePlan(const std::string &json);
std::string takeTapProfile();
#else
inline constexpr bool supported = false;
inline void refreshPermission() {}
inline Permission permission() { return Permission::Unsupported; }
inline void requestPermission() {}
inline void openSettings() {}
inline void replacePlan(const std::string &) {}
inline std::string takeTapProfile() { return {}; }
#endif
}
