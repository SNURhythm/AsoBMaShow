#include "PlatformOpen.h"

#include "ArchiveFile.h"
#include "path.h"
#include "targets.h"

#include <cerrno>
#include <cstring>
#include <thread>

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "iOSNatives.hpp"
#elif TARGET_OS_ANDROID
#include "AndroidNatives.h"
#elif TARGET_OS_OSX
#include "MacNatives.h"
#elif defined(_WIN32)
#include <shlobj.h>
#include <shellapi.h>
#elif TARGET_OS_LINUX
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace platform_open {
namespace {

#if TARGET_OS_LINUX
std::string xdgOpenPath() {
  std::error_code error;
  for (const char *candidate :
       {"/usr/bin/xdg-open", "/bin/xdg-open", "/usr/local/bin/xdg-open"}) {
    if (std::filesystem::exists(candidate, error)) return candidate;
    error.clear();
  }
  return {};
}

bool spawnXdgOpen(std::string_view target, std::string_view description,
                  std::string &error) {
  const std::string opener = xdgOpenPath();
  if (opener.empty()) {
    error = "Could not find xdg-open";
    return false;
  }
  std::string argument(target);
  pid_t pid = 0;
  char *argv[] = {const_cast<char *>(opener.c_str()), argument.data(), nullptr};
  const int result =
      posix_spawn(&pid, opener.c_str(), nullptr, nullptr, argv, environ);
  if (result != 0) {
    error = "Could not open " + std::string(description) + ": " +
            std::strerror(result);
    return false;
  }
  std::thread([pid]() {
    int status = 0;
    waitpid(pid, &status, 0);
  }).detach();
  return true;
}
#endif

} // namespace

bool desktopOpenSupported() noexcept {
#if TARGET_OS_OSX || TARGET_OS_LINUX || defined(_WIN32)
  return true;
#else
  return false;
#endif
}

bool revealPathInFileManager(const std::filesystem::path &path,
                             const RevealAnchor &anchor,
                             std::string &error) {
  (void)anchor;
  error.clear();
  if (path.empty()) {
    error = "Chart file path is empty";
    return false;
  }

  std::error_code errorCode;
  std::filesystem::path target = path;
  if (!target.is_absolute()) {
    const auto absolute = std::filesystem::absolute(target, errorCode);
    if (!errorCode) target = absolute;
    errorCode.clear();
  }
  std::filesystem::path archive;
  std::filesystem::path inner;
  if (archive_file::splitVirtualPath(target, archive, inner)) target = archive;

  const std::string targetText = fspath_to_utf8(target);
  const bool exists = std::filesystem::exists(target, errorCode);
  if (errorCode) {
    error = "Could not check chart file: " + targetText + " (" +
            errorCode.message() + ")";
    return false;
  }
  if (!exists) {
    error = "Chart file does not exist: " + targetText;
    return false;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return RevealIOSFileInFiles(
      targetText,
      {.x = anchor.x,
       .y = anchor.y,
       .width = anchor.width,
       .height = anchor.height},
      error);
#elif TARGET_OS_ANDROID
  error = "Reveal is not supported on Android yet";
  return false;
#elif TARGET_OS_OSX
  return RevealPathInFinder(targetText, error);
#elif defined(_WIN32)
  const std::wstring nativePath = target.wstring();
  const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                     COINIT_DISABLE_OLE1DDE);
  const bool didCoInitialize = SUCCEEDED(coInit);
  PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(nativePath.c_str());
  if (pidl == nullptr) {
    if (didCoInitialize) CoUninitialize();
    error = "Could not resolve chart file for Explorer";
    return false;
  }
  const HRESULT result = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
  ILFree(pidl);
  if (didCoInitialize) CoUninitialize();
  if (FAILED(result)) {
    error = "Could not open Explorer";
    return false;
  }
  return true;
#elif TARGET_OS_LINUX
  const bool isDirectory = std::filesystem::is_directory(target, errorCode);
  if (errorCode) {
    error = "Could not check chart file type: " + targetText + " (" +
            errorCode.message() + ")";
    return false;
  }
  std::filesystem::path directory =
      isDirectory ? target : target.parent_path();
  if (directory.empty()) directory = ".";
  return spawnXdgOpen(fspath_to_utf8(directory), "file manager", error);
#else
  error = "Reveal is not supported on this platform";
  return false;
#endif
}

bool openPath(const std::filesystem::path &path, std::string &error) {
  error.clear();
#if TARGET_OS_OSX
  return OpenPathWithDefaultApplication(fspath_to_utf8(path), error);
#elif defined(_WIN32)
  const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(),
                                         nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<intptr_t>(result) <= 32) {
    error = "Could not open path";
    return false;
  }
  return true;
#elif TARGET_OS_LINUX
  return spawnXdgOpen(fspath_to_utf8(path), "path", error);
#else
  (void)path;
  error = "Opening paths is not supported on this platform";
  return false;
#endif
}

bool openExternalUrl(std::string_view url, std::string &error) {
  error.clear();
  if (url.empty()) {
    error = "URL is empty";
    return false;
  }
  const std::string urlText(url);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return OpenURLInIOSBrowser(urlText, error);
#elif TARGET_OS_ANDROID
  return OpenURLInAndroidBrowser(urlText, error);
#elif TARGET_OS_OSX
  return OpenURLInDefaultBrowser(urlText, error);
#elif defined(_WIN32)
  const HINSTANCE result = ShellExecuteA(nullptr, "open", urlText.c_str(),
                                         nullptr, nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<intptr_t>(result) <= 32) {
    error = "Could not open browser";
    return false;
  }
  return true;
#elif TARGET_OS_LINUX
  return spawnXdgOpen(urlText, "browser", error);
#else
  error = "Opening URLs is not supported on this platform";
  return false;
#endif
}

} // namespace platform_open
