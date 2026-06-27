#include "MainMenuScene.h"
#include "MainMenuLibrary.h"
#include "../ArchiveFile.h"
#include "../BmsChartFile.h"
#include "../CourseConstraintUtils.h"
#include "../LongNoteModeUtils.h"
#include "../audio/MusicPlaylist.h"
#include "../tinyfiledialogs.h"
#include <fstream>
#include <algorithm>
#include "../ReplayDBHelper.h"
#include "../ReplayAutoPlay.h"
#include "../ReplayVideoExporter.h"
#include "../ResultImageExporter.h"
#include "../PlayOptionUtils.h"
#include "../RAII.h"
#include "../SqliteRAII.h"
#include "../path.h"
#include "../view/ChartListItemView.h"
#include "../view/LibraryFolderItemView.h"
#include "../view/TextView.h"
#include "../view/TextInputBox.h"
#include "../Utils.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/BlockingOverlayView.h"
#include "ChartViewerScene.h"
#include "MusicPlayerScene.h"
#include "play/GamePlayScene.h"
#include "../view/ClearLampColors.h"
#include "../view/ReplaySummaryListView.h"
#include "../view/ScrollView.h"
#include "../view/UiTheme.h"
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>

#elif __APPLE__

#include "TargetConditionals.h"
#if TARGET_OS_IPHONE
#include "../iOSNatives.hpp"
// define something for iphone
#include <dirent.h>
#include <sys/stat.h>
#else
// define something for OSX
#include "../MacNatives.h"
#include <dirent.h>
#include <sys/stat.h>
#endif
#elif defined(__ANDROID__)
#include "../AndroidNatives.h"
#include <dirent.h>
#include <sys/stat.h>
#elif __linux
// linux
#include <dirent.h>
#include <sys/stat.h>
#elif __unix // all unices not caught above
// Unix
#elif __posix
// POSIX
#endif
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#if TARGET_OS_LINUX
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace {

constexpr int kRootPadding = 28;
constexpr int kLibraryPanelWidth = 360;
constexpr int kLibraryPanelPadding = 14;
constexpr int kLibraryControlWidth =
    kLibraryPanelWidth - (kLibraryPanelPadding * 2);
constexpr size_t kFindBmsMaxLogLines = 120;
constexpr size_t kFindBmsMaxPendingProgressEvents = 160;
constexpr const char *kDefaultDifficultyTableUrls[] = {
    "https://rattoto10.jounin.jp/table.html",
    "https://rattoto10.jounin.jp/table_insane.html",
    "https://stellabms.xyz/sl/table.html",
    "https://stellabms.xyz/st/table.html",
};

void ensureLibraryFolderExists(const std::filesystem::path &path) {
  std::error_code error;
  if (Utils::EnsureDirectoryExists(path, error)) {
    return;
  }

  throw std::runtime_error("Could not create library folder '" +
                           fspath_to_utf8(path) + "': " + error.message());
}

bool ensureDirectoryExistsLogged(const std::filesystem::path &path,
                                 const char *description) {
  std::error_code error;
  if (Utils::EnsureDirectoryExists(path, error)) {
    return true;
  }

  SDL_Log("Failed to create %s %s: %s", description,
          fspath_to_utf8(path).c_str(), error.message().c_str());
  return false;
}

const char *homeDirectoryEnvValue() {
  if (const char *home = std::getenv("HOME");
      home != nullptr && home[0] != '\0') {
    return home;
  }
#ifdef _WIN32
  if (const char *profile = std::getenv("USERPROFILE");
      profile != nullptr && profile[0] != '\0') {
    return profile;
  }
#endif
  return nullptr;
}

bool expandCurrentUserHomeShortcut(std::string &path) {
  if (path.empty() || path.front() != '~') {
    return true;
  }
  if (path.size() > 1 && path[1] != '/' && path[1] != '\\') {
    return true;
  }

  const char *home = homeDirectoryEnvValue();
  if (home == nullptr) {
    return false;
  }
  path.replace(0, 1, home);
  return true;
}

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

struct ClearMarkFilterDefinition {
  const char *label;
  int rank;
};

constexpr ClearMarkFilterDefinition kDifficultyClearMarkFilters[] = {
    {"FULL COMBO", kClearTypeFullComboRank},
    {"EXH-CLEAR", kClearTypeExHardClearRank},
    {"H-CLEAR", kClearTypeHardClearRank},
    {"CLEAR", kClearTypeNormalClearRank},
    {"E-CLEAR", kClearTypeEasyClearRank},
    {"A-CLEAR", kClearTypeAssistedEasyClearRank},
    {"FAILED", kClearTypeFailedRank},
    {"NO PLAY", kNoClearTypeRank},
};

std::string longNoteModeOptionFromCourseConstraint(CourseLongNoteMode mode) {
  return long_note_mode::idFromValue(courseLongNoteModeToChartMetaValue(mode),
                                     AppSettings::kDefaultLnMode);
}

std::string clearMarkFolderKey(const std::string &parentKey, int clearRank) {
  return parentKey + ":clear:" + std::to_string(clearRank);
}

std::string findBmsManualSourceUrl(const BmsSearchResult &result) {
  if (!result.fallbackUrl.empty()) {
    return result.fallbackUrl;
  }
  if ((result.status == BmsSearchResult::Status::DownloadFailed ||
       result.status == BmsSearchResult::Status::HashMismatch) &&
      !result.downloadUrl.empty()) {
    return result.downloadUrl;
  }
  return result.patternUrl;
}

std::string findBmsTitleSearchQuery(const ChartMetaRecord &record) {
  std::string query = record.meta.Title;
  if (!query.empty() && !record.meta.Artist.empty()) {
    query += " " + record.meta.Artist;
  }
  if (query.empty()) {
    query = !record.meta.MD5.empty() ? record.meta.MD5 : record.meta.SHA256;
  }
  return query;
}

std::string compactHashForModal(const std::string &value) {
  if (value.size() <= 24) {
    return value;
  }
  return value.substr(0, 10) + "..." + value.substr(value.size() - 10);
}

std::string findBmsCandidateLabel(const BmsSearchCandidate &candidate,
                                  size_t index) {
  std::string label = std::to_string(index + 1) + ". Download ";
  if (!candidate.artist.empty() || !candidate.title.empty()) {
    if (!candidate.artist.empty()) {
      label += "[" + candidate.artist + "] ";
    }
    label += candidate.title.empty() ? candidate.name : candidate.title;
  } else {
    label += candidate.name.empty() ? "Horie archive" : candidate.name;
  }
  return label;
}

class FindBmsCandidateItemView : public View {
public:
  FindBmsCandidateItemView() : View() {
    setFlexDirection(FlexDirection::Column);
    setJustifyContent(YGJustifyCenter);
    setPadding(Edge::Left, 14);
    setPadding(Edge::Right, 14);
    setCornerRadius(ui_theme::controlRadius());
    setBorderWidth(1);

    label = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
    label->setWrap(true);
    label->setOverflow(TextView::TextOverflow::Hidden);
    label->setVAlign(TextView::MIDDLE);
    label->setFlex(1);
    addView(label);
    onUnselected();
  }

  void setCandidate(const BmsSearchCandidate &candidate, size_t index,
                    bool selected) {
    if (label != nullptr) {
      label->setText(findBmsCandidateLabel(candidate, index));
    }
    if (selected) {
      onSelected();
    } else {
      onUnselected();
    }
  }

  void onSelected() override {
    setThemedBackgroundColor(ui_theme::infoActionHover);
    setThemedBorderColor(
        [] { return ui_theme::withAlpha(ui_theme::infoActionPressed(), 210); });
    if (label != nullptr) {
      label->setThemedColor(
          [] { return ui_theme::textOn(ui_theme::infoActionHover()); });
    }
  }

  void onUnselected() override {
    setThemedBackgroundColor(ui_theme::control);
    setThemedBorderColor(ui_theme::hairlineStrong);
    if (label != nullptr) {
      label->setThemedColor(ui_theme::textPrimary);
    }
  }

private:
  TextView *label = nullptr;
};

bool messageStartsWith(const std::string &message, const std::string &prefix) {
  return message.rfind(prefix, 0) == 0;
}

std::string formatFindBmsBytes(std::uint64_t bytes) {
  constexpr double kKib = 1024.0;
  constexpr double kMib = kKib * 1024.0;
  constexpr double kGib = kMib * 1024.0;
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(bytes >= 10 * 1024 ? 1 : 0);
  if (bytes >= static_cast<std::uint64_t>(kGib)) {
    stream << static_cast<double>(bytes) / kGib << " GB";
  } else if (bytes >= static_cast<std::uint64_t>(kMib)) {
    stream << static_cast<double>(bytes) / kMib << " MB";
  } else if (bytes >= static_cast<std::uint64_t>(kKib)) {
    stream << static_cast<double>(bytes) / kKib << " KB";
  } else {
    stream.str("");
    stream.clear();
    stream << bytes << " B";
  }
  return stream.str();
}

bool pathIsInsideDirectoryForMenu(const std::filesystem::path &path,
                                  const std::filesystem::path &directory) {
  if (path.empty() || directory.empty()) {
    return false;
  }
  const std::filesystem::path normalizedPath = path.lexically_normal();
  const std::filesystem::path normalizedDirectory =
      directory.lexically_normal();
  if (normalizedPath == normalizedDirectory) {
    return false;
  }
  const std::filesystem::path relative =
      normalizedPath.lexically_relative(normalizedDirectory);
  if (relative.empty() || relative.is_absolute()) {
    return false;
  }
  const auto first = relative.begin();
  return first != relative.end() && *first != std::filesystem::path("..") &&
         *first != std::filesystem::path(".");
}

double progressRatio(const BmsSearchDownloadProgress &progress) {
  if (progress.totalBytes == 0) {
    return 0.0;
  }
  return std::clamp(static_cast<double>(progress.downloadedBytes) /
                        static_cast<double>(progress.totalBytes),
                    0.0, 1.0);
}

std::string progressPercentText(double ratio) {
  const int percent =
      static_cast<int>(std::lround(std::clamp(ratio, 0.0, 1.0) * 100.0));
  return std::to_string(percent) + "%";
}

std::string findBmsProgressDisplayText(const std::string &message,
                                       std::uint64_t downloadedBytes,
                                       std::uint64_t totalBytes,
                                       bool includeBytes) {
  if (message == "Downloading archive" && totalBytes > 0) {
    const double ratio = std::clamp(static_cast<double>(downloadedBytes) /
                                        static_cast<double>(totalBytes),
                                    0.0, 1.0);
    std::string text = "Downloading archive - " + progressPercentText(ratio);
    if (includeBytes) {
      text += " (" + formatFindBmsBytes(downloadedBytes) + " / " +
              formatFindBmsBytes(totalBytes) + ")";
    }
    return text;
  }
  if (message == "Downloading archive" && downloadedBytes > 0) {
    return "Downloading archive (" + formatFindBmsBytes(downloadedBytes) + ")";
  }
  if (message == "Download complete" && totalBytes > 0) {
    const double ratio = std::clamp(static_cast<double>(downloadedBytes) /
                                        static_cast<double>(totalBytes),
                                    0.0, 1.0);
    return "Download complete - " + progressPercentText(ratio);
  }
  return message;
}

std::string
findBmsProgressDisplayText(const BmsSearchDownloadProgress &progress,
                           bool includeBytes) {
  return findBmsProgressDisplayText(progress.message, progress.downloadedBytes,
                                    progress.totalBytes, includeBytes);
}

bool shouldReplaceFindBmsLogLine(const std::string &previous,
                                 const std::string &next) {
  for (const char *prefix : {"Downloading archive", "Extracting "}) {
    if (messageStartsWith(previous, prefix) &&
        messageStartsWith(next, prefix)) {
      return true;
    }
  }
  return false;
}

double findBmsProgressFractionFor(const BmsSearchDownloadProgress &progress,
                                  double previous) {
  const std::string &message = progress.message;
  if (message == "Preparing lookup") {
    return std::max(previous, 0.02);
  }
  if (message == "Opening BMS Search pattern page") {
    return std::max(previous, 0.04);
  }
  if (message == "Opening BMS Search details page") {
    return std::max(previous, 0.07);
  }
  if (messageStartsWith(message, "Searching ") &&
      message.find(" package source") != std::string::npos) {
    return std::max(previous, 0.08);
  }
  if (messageStartsWith(message, "Preparing ") &&
      message.find(" package download") != std::string::npos) {
    return std::max(previous, 0.09);
  }
  if (message == "Searching Horie archive") {
    return std::max(previous, 0.08);
  }
  if (message == "Preparing Horie archive download") {
    return std::max(previous, 0.09);
  }
  if (message == "Downloading archive") {
    const double ratio = progressRatio(progress);
    if (progress.totalBytes > 0) {
      return std::max(previous, 0.10 + ratio * 0.80);
    }
    return std::min(0.90, std::max(previous + 0.003, 0.10));
  }
  if (message == "Download complete") {
    const double ratio = progressRatio(progress);
    return std::max(previous,
                    progress.totalBytes > 0 ? 0.10 + ratio * 0.80 : 0.90);
  }
  if (message == "Confirming Google Drive download") {
    return 0.10;
  }
  if (message == "Extracting archive") {
    return std::max(previous, 0.92);
  }
  if (messageStartsWith(message, "Extracting ")) {
    const double ratio = progressRatio(progress);
    if (progress.totalBytes > 0) {
      return std::max(previous, 0.92 + ratio * 0.06);
    }
    return std::min(0.98, std::max(previous + 0.005, 0.93));
  }
  return std::max(previous, 0.05);
}

SafeAreaInsets getSafeAreaInsetsUi() {
  SafeAreaInsets insets;
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const IOSNormalizedSafeAreaInsets normalized =
      GetIOSSafeAreaInsetsNormalized();
  insets.top = static_cast<int>(std::lround(
      normalized.top * static_cast<float>(rendering::window_height)));
  insets.left = static_cast<int>(std::lround(
      normalized.left * static_cast<float>(rendering::window_width)));
  insets.right = static_cast<int>(std::lround(
      normalized.right * static_cast<float>(rendering::window_width)));
#endif
  return insets;
}

bool revealPathInFileManager(const std::filesystem::path &path,
                             std::string &errorMessage) {
  errorMessage.clear();
  if (path.empty()) {
    errorMessage = "Chart file path is empty";
    return false;
  }

  std::error_code errorCode;
  std::filesystem::path targetPath = path;
  if (!targetPath.is_absolute()) {
    const auto absolutePath = std::filesystem::absolute(targetPath, errorCode);
    if (!errorCode) {
      targetPath = absolutePath;
    }
    errorCode.clear();
  }
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (archive_file::splitVirtualPath(targetPath, archivePath, innerPath)) {
    targetPath = archivePath;
  }

  const std::string targetPathText = fspath_to_utf8(targetPath);
  const bool targetExists = std::filesystem::exists(targetPath, errorCode);
  if (errorCode) {
    errorMessage =
        "Could not check chart file: " + targetPathText + " (" +
        errorCode.message() + ")";
    return false;
  }
  if (!targetExists) {
    errorMessage = "Chart file does not exist: " +
                   targetPathText;
    return false;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return RevealIOSFileInFiles(targetPathText, errorMessage);
#elif TARGET_OS_ANDROID
  errorMessage = "Reveal is not supported on Android yet";
  return false;
#elif TARGET_OS_OSX
  return RevealPathInFinder(targetPathText, errorMessage);
#elif defined(_WIN32)
  const std::wstring nativePath = targetPath.wstring();
  const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                     COINIT_DISABLE_OLE1DDE);
  const bool didCoInitialize = SUCCEEDED(coInit);
  PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(nativePath.c_str());
  if (pidl == nullptr) {
    if (didCoInitialize) {
      CoUninitialize();
    }
    errorMessage = "Could not resolve chart file for Explorer";
    return false;
  }

  const HRESULT result = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
  ILFree(pidl);
  if (didCoInitialize) {
    CoUninitialize();
  }
  if (FAILED(result)) {
    std::ostringstream stream;
    stream << "Could not open Explorer: HRESULT 0x" << std::hex
           << static_cast<unsigned long>(result);
    errorMessage = stream.str();
    return false;
  }
  return true;
#elif TARGET_OS_LINUX
  const bool isDirectory = std::filesystem::is_directory(targetPath, errorCode);
  if (errorCode) {
    errorMessage =
        "Could not check chart file type: " + targetPathText + " (" +
        errorCode.message() + ")";
    return false;
  }
  std::filesystem::path directoryPath =
      isDirectory ? targetPath : targetPath.parent_path();
  if (directoryPath.empty()) {
    directoryPath = ".";
  }

  std::string openerPath;
  for (const char *candidate :
       {"/usr/bin/xdg-open", "/bin/xdg-open", "/usr/local/bin/xdg-open"}) {
    if (std::filesystem::exists(candidate, errorCode)) {
      openerPath = candidate;
      break;
    }
    errorCode.clear();
  }
  if (openerPath.empty()) {
    errorMessage = "Could not find xdg-open";
    return false;
  }

  const std::string directoryText = fspath_to_utf8(directoryPath);
  pid_t pid = 0;
  char *argv[] = {const_cast<char *>(openerPath.c_str()),
                  const_cast<char *>(directoryText.c_str()), nullptr};
  const int result =
      posix_spawn(&pid, openerPath.c_str(), nullptr, nullptr, argv, environ);
  if (result != 0) {
    errorMessage =
        std::string("Could not open file manager: ") + std::strerror(result);
    return false;
  }
  std::thread([pid]() {
    int status = 0;
    waitpid(pid, &status, 0);
  }).detach();
  return true;
#else
  errorMessage = "Reveal is not supported on this platform";
  return false;
#endif
}

bool openExternalUrl(const std::string &url, std::string &errorMessage) {
  errorMessage.clear();
  if (url.empty()) {
    errorMessage = "URL is empty";
    return false;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return OpenURLInIOSBrowser(url, errorMessage);
#elif TARGET_OS_ANDROID
  return OpenURLInAndroidBrowser(url, errorMessage);
#elif TARGET_OS_OSX
  return OpenURLInDefaultBrowser(url, errorMessage);
#elif defined(_WIN32)
  const HINSTANCE result = ShellExecuteA(nullptr, "open", url.c_str(), nullptr,
                                         nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<intptr_t>(result) <= 32) {
    errorMessage = "Could not open browser";
    return false;
  }
  return true;
#elif TARGET_OS_LINUX
  std::error_code errorCode;
  std::string openerPath;
  for (const char *candidate :
       {"/usr/bin/xdg-open", "/bin/xdg-open", "/usr/local/bin/xdg-open"}) {
    if (std::filesystem::exists(candidate, errorCode)) {
      openerPath = candidate;
      break;
    }
    errorCode.clear();
  }
  if (openerPath.empty()) {
    errorMessage = "Could not find xdg-open";
    return false;
  }

  pid_t pid = 0;
  char *argv[] = {const_cast<char *>(openerPath.c_str()),
                  const_cast<char *>(url.c_str()), nullptr};
  const int result =
      posix_spawn(&pid, openerPath.c_str(), nullptr, nullptr, argv, environ);
  if (result != 0) {
    errorMessage =
        std::string("Could not open browser: ") + std::strerror(result);
    return false;
  }
  std::thread([pid]() {
    int status = 0;
    waitpid(pid, &status, 0);
  }).detach();
  return true;
#else
  errorMessage = "Opening URLs is not supported on this platform";
  return false;
#endif
}

using main_menu_library::folderKeyForCourse;
using main_menu_library::folderKeyForCourseGroup;
using main_menu_library::folderKeyForCourseTable;
using main_menu_library::folderKeyForLevel;
using main_menu_library::folderKeyForTable;

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
std::mutex gIOSFolderAccessMutex;
std::vector<void *> gIOSFolderAccessHandles;
std::unordered_map<path_t, path_t> gIOSResolvedFolderPaths;

void ClearIOSFolderAccessLocked() {
  for (void *handle : gIOSFolderAccessHandles) {
    StopIOSSecurityScopedResource(handle);
  }
  gIOSFolderAccessHandles.clear();
  gIOSResolvedFolderPaths.clear();
}

void ClearIOSFolderAccess() {
  std::lock_guard<std::mutex> lock(gIOSFolderAccessMutex);
  ClearIOSFolderAccessLocked();
}

void RefreshIOSFolderAccess(const std::vector<ChartEntry> &entries) {
  std::lock_guard<std::mutex> lock(gIOSFolderAccessMutex);
  ClearIOSFolderAccessLocked();

  for (const auto &entry : entries) {
    if (entry.iosBookmark.empty()) {
      continue;
    }
    std::string resolvedPath;
    std::string errorMessage;
    void *handle = StartIOSSecurityScopedResource(path_t_to_utf8(entry.path),
                                                  entry.iosBookmark,
                                                  resolvedPath, errorMessage);
    if (!errorMessage.empty()) {
      SDL_Log("Failed to open folder access for %s: %s",
              path_t_to_utf8(entry.path).c_str(), errorMessage.c_str());
    }
    if (!resolvedPath.empty()) {
      gIOSResolvedFolderPaths[entry.path] = utf8_to_path_t(resolvedPath);
    }
    if (handle != nullptr) {
      gIOSFolderAccessHandles.push_back(handle);
    }
  }
}

std::filesystem::path ResolveIOSFolderEntryPath(const ChartEntry &entry) {
  std::lock_guard<std::mutex> lock(gIOSFolderAccessMutex);
  const auto it = gIOSResolvedFolderPaths.find(entry.path);
  if (it != gIOSResolvedFolderPaths.end()) {
    return std::filesystem::path(it->second);
  }
  return std::filesystem::path(entry.path);
}
#endif

int clearRankForGaugeType(GaugeType gaugeType) {
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return kClearTypeAssistedEasyClearRank;
  case GaugeType::Easy:
    return kClearTypeEasyClearRank;
  case GaugeType::Hard:
    return kClearTypeHardClearRank;
  case GaugeType::ExHard:
    return kClearTypeExHardClearRank;
  case GaugeType::Normal:
  default:
    return kClearTypeNormalClearRank;
  }
}

std::string gaugeButtonLabel(GaugeType gaugeType, bool autoShift) {
  if (autoShift) {
    return "GAS";
  }
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return "A-EASY";
  case GaugeType::Easy:
    return "EASY";
  case GaugeType::Normal:
    return "NORMAL";
  case GaugeType::Hard:
    return "HARD";
  case GaugeType::ExHard:
    return "EX-HARD";
  default:
    return "NORMAL";
  }
}

SDL_Color readyGaugeTextColor(GaugeType gaugeType, bool autoShift) {
  if (autoShift) {
    return SDL_Color{255, 205, 37, 255};
  }

  const Color color = clearLampColorForRank(clearRankForGaugeType(gaugeType));
  return SDL_Color{color.r, color.g, color.b, 255};
}

const char *gaugeSettingId(GaugeType gaugeType, bool autoShift) {
  if (autoShift) {
    return "gas";
  }
  switch (gaugeType) {
  case GaugeType::AssistedEasy:
    return "assisted_easy";
  case GaugeType::Easy:
    return "easy";
  case GaugeType::Normal:
    return "normal";
  case GaugeType::Hard:
    return "hard";
  case GaugeType::ExHard:
    return "exhard";
  default:
    return "normal";
  }
}

struct GaugeSelection {
  GaugeType type = GaugeType::Normal;
  bool autoShift = false;
};

GaugeSelection gaugeSelectionFromSettingId(const std::string &id) {
  if (id == "gas") {
    return {.type = GaugeType::ExHard, .autoShift = true};
  }
  if (id == "assisted_easy") {
    return {.type = GaugeType::AssistedEasy};
  }
  if (id == "easy") {
    return {.type = GaugeType::Easy};
  }
  if (id == "hard") {
    return {.type = GaugeType::Hard};
  }
  if (id == "exhard") {
    return {.type = GaugeType::ExHard};
  }
  return {.type = GaugeType::Normal};
}

void styleThemedActionButton(Button *button, TextView *text, bool enabled,
                             View::ThemeColorProvider normal,
                             View::ThemeColorProvider hover,
                             View::ThemeColorProvider pressed,
                             View::ThemeColorProvider border) {
  if (button == nullptr || text == nullptr) {
    return;
  }

  button->setCornerRadius(ui_theme::controlRadius());
  if (enabled) {
    button->setThemedBackgroundColors(normal, hover, pressed);
    button->setThemedBorderColors(
        [border] { return ui_theme::withAlpha(border(), 150); },
        [border] { return ui_theme::withAlpha(border(), 190); },
        [border] { return ui_theme::withAlpha(border(), 220); });
    text->setThemedColor([normal] { return ui_theme::textOn(normal()); });
  } else {
    button->setThemedBackgroundColors(
        ui_theme::panelSubtle, ui_theme::panelSubtle, ui_theme::panelSubtle);
    button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle,
                                  ui_theme::hairlineSubtle);
    text->setThemedColor(ui_theme::textMuted);
  }
}

void styleOptionButton(Button *button, TextView *text, bool selected) {
  if (selected) {
    styleThemedActionButton(button, text, true, ui_theme::primaryAction,
                            ui_theme::primaryActionHover,
                            ui_theme::primaryActionPressed,
                            ui_theme::accentBorderStrong);
  } else {
    styleThemedActionButton(button, text, true, ui_theme::control,
                            ui_theme::controlHover, ui_theme::controlPressed,
                            ui_theme::hairlineStrong);
  }
}

void styleLockedOptionButton(Button *button, TextView *text, bool selected) {
  if (button == nullptr || text == nullptr) {
    return;
  }

  if (selected) {
    styleThemedActionButton(button, text, true, ui_theme::primaryAction,
                            ui_theme::primaryAction, ui_theme::primaryAction,
                            ui_theme::accentBorderStrong);
    return;
  }

  styleThemedActionButton(button, text, false, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
}

TextView *makeModalLabel(const std::string &text) {
  auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  label->setText(text);
  label->setThemedColor(ui_theme::textSecondary);
  label->setHeight(28);
  return label;
}

View *makeModalOptionRow(float height = 58.0f) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row);
  row->setAlignItems(YGAlignStretch);
  row->setGap(12);
  row->setHeight(height);
  return row;
}

Button *makeModalButton(const std::string &label, int fontSize,
                        TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, 160, 58);
  auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", fontSize);
  text->setText(label);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  button->setContentView(text);
  button->setStyledBorderWidth(1);
  button->setCornerRadius(ui_theme::controlRadius());
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
}

Color modalPanelBorder() {
  return ui_theme::activeMode() == ui_theme::ThemeMode::Light
             ? ui_theme::hairlineStrong()
             : Color(86, 118, 153, 210);
}

const char *chartScanProgressStageText(ChartScanProgressStage stage) {
  switch (stage) {
  case ChartScanProgressStage::Preparing:
    return "Preparing library scan";
  case ChartScanProgressStage::ScanningRoots:
    return "Scanning folders";
  case ChartScanProgressStage::PreparingUpdates:
    return "Preparing chart updates";
  case ChartScanProgressStage::RemovingDeleted:
    return "Removing deleted charts";
  case ChartScanProgressStage::ParsingCharts:
    return "Parsing charts";
  case ChartScanProgressStage::ReadingArchive:
    return "Reading archive entries";
  }
  return "Refreshing library";
}

std::string formatMusicTime(long long micros) {
  if (micros < 0) {
    return "--:--";
  }
  const long long totalSeconds = micros / 1000000LL;
  const long long minutes = totalSeconds / 60LL;
  const long long seconds = totalSeconds % 60LL;
  std::ostringstream stream;
  stream << minutes << ":" << std::setw(2) << std::setfill('0') << seconds;
  return stream.str();
}

std::string musicTrackDisplayName(const music_playlist::MusicTrack *track) {
  if (track == nullptr) {
    return "No track selected";
  }
  std::string title = track->title.empty() ? "Untitled" : track->title;
  if (!track->artist.empty()) {
    title += " / " + track->artist;
  }
  return title;
}

std::string musicPlaylistTextSnapshot(
    const std::vector<music_playlist::MusicTrack> &tracks) {
  if (tracks.empty()) {
    return "My Playlist\nEmpty";
  }

  constexpr std::size_t kVisibleTrackCount = 5;
  std::ostringstream text;
  text << "My Playlist";
  const std::size_t visibleCount =
      std::min(kVisibleTrackCount, tracks.size());
  for (std::size_t i = 0; i < visibleCount; ++i) {
    text << "\n" << (i + 1) << ". " << musicTrackDisplayName(&tracks[i]);
  }
  if (tracks.size() > visibleCount) {
    text << "\n+" << (tracks.size() - visibleCount) << " more";
  }
  return text.str();
}

} // namespace

void MainMenuScene::ChartListPageCache::reset(sqlite3 *database,
                                              const ChartMetaQuery &chartQuery,
                                              int count,
                                              std::optional<ChartMetaRecord>
                                                  leading) {
  db = database;
  query = chartQuery;
  query.limit = 0;
  query.offset = 0;
  leadingRecord = std::move(leading);
  totalCount = std::max(0, count) + (leadingRecord.has_value() ? 1 : 0);
  clear();
}

void MainMenuScene::ChartListPageCache::clear() {
  pages.clear();
  pageOrder.clear();
}

const ChartMetaRecord &MainMenuScene::ChartListPageCache::get(int index) const {
  if (db == nullptr || index < 0 || index >= totalCount) {
    return fallbackRecord;
  }
  if (leadingRecord.has_value()) {
    if (index == 0) {
      return *leadingRecord;
    }
    index--;
  }

  const int pageIndex = index / pageSize;
  auto pageIt = pages.find(pageIndex);
  if (pageIt == pages.end()) {
    ChartMetaQuery pageQuery = query;
    pageQuery.limit = pageSize;
    pageQuery.offset = pageIndex * pageSize;

    std::vector<ChartMetaRecord> records;
    records.reserve(pageSize);
    ChartDBHelper::GetInstance().QueryChartMeta(db, pageQuery, records);
    pageIt = pages.emplace(pageIndex, std::move(records)).first;
  }
  touchPage(pageIndex);

  const int localIndex = index - (pageIndex * pageSize);
  if (localIndex < 0 || localIndex >= static_cast<int>(pageIt->second.size())) {
    return fallbackRecord;
  }
  return pageIt->second[localIndex];
}

void MainMenuScene::ChartListPageCache::touchPage(int pageIndex) const {
  pageOrder.erase(std::remove(pageOrder.begin(), pageOrder.end(), pageIndex),
                  pageOrder.end());
  pageOrder.push_back(pageIndex);

  while (static_cast<int>(pages.size()) > maxPages && !pageOrder.empty()) {
    const int victim = pageOrder.front();
    pageOrder.pop_front();
    if (victim == pageIndex && pages.size() == 1) {
      pageOrder.push_back(victim);
      break;
    }
    pages.erase(victim);
  }
}

void MainMenuScene::init() {
  // Initialize the scene
  db = ChartDBHelper::GetInstance().Connect();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  context.requestAddChartFolderFromFiles = [this]() {
    addIOSFolderEntryFromFiles();
  };
#elif TARGET_OS_ANDROID
  if (AndroidBuildHasManageExternalStorage()) {
    context.requestAddChartFolderFromFiles = [this]() {
      addAndroidFolderEntryFromPicker();
    };
  } else {
    context.requestAddChartFolderFromFiles = [this]() {
      importAndroidFolderFromPicker();
    };
  }
#endif
  initView(context);
  SDL_Log("Main Menu Scene Initialized");
  startLibraryTaskWorker();
  enqueueLibraryRefreshTask("Refresh Library");
}

void MainMenuScene::onPause() { pauseLibraryTaskWorker(); }

void MainMenuScene::onResume() {
  applyThemeChange();
  resumeLibraryTaskWorker();
  startLibraryTaskWorker();
  refreshScoreClearRanksIfNeeded();
  refreshLibraryIfNeeded();
  reselectCurrentChart();
}

void MainMenuScene::applyThemeChange() {
  const ui_theme::ThemeMode activeMode = ui_theme::activeMode();
  if (appliedUiThemeMode == activeMode) {
    return;
  }

  appliedUiThemeMode = activeMode;
  for (auto *view : views) {
    if (view != nullptr) {
      view->propagateThemeChange();
    }
  }
  if (folderRecyclerView != nullptr) {
    folderRecyclerView->rebindVisibleItems();
  }
  if (recyclerView != nullptr) {
    recyclerView->rebindVisibleItems();
  }
  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshAssistOptionButtons();
  refreshReplayModalActions();
  refreshReplayExportOptionButtons();
  refreshFindBmsModal();
  refreshMusicModal();
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::startLibraryTaskWorker() {
  std::lock_guard<std::mutex> workerLock(libraryTaskWorkerMutex);
  if (libraryTaskWorkerPaused.load()) {
    return;
  }
  if (checkEntriesThread.joinable()) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    bool changed = false;
    for (auto &task : libraryTasks) {
      if (task.status != LibraryTaskStatus::Paused) {
        continue;
      }
      const auto queuedIt = std::find_if(
          libraryTaskQueue.begin(), libraryTaskQueue.end(),
          [&task](const auto &queuedTask) { return queuedTask.id == task.id; });
      if (queuedIt != libraryTaskQueue.end()) {
        task.status = LibraryTaskStatus::Queued;
        task.detail = "Waiting";
        changed = true;
      }
    }
    if (changed) {
      bumpLibraryTasksRevisionLocked();
    }
  }
  checkEntriesThread = std::jthread(
      [this](const std::stop_token &stopToken) { libraryTaskLoop(stopToken); });
}

void MainMenuScene::stopLibraryTaskWorker() {
  std::lock_guard<std::mutex> workerLock(libraryTaskWorkerMutex);
  if (!checkEntriesThread.joinable()) {
    return;
  }
  SDL_Log("Joining library task worker");
  checkEntriesThread.request_stop();
  libraryTaskWorkerPaused = false;
  libraryTaskPauseCv.notify_all();
  libraryTaskCv.notify_all();
  checkEntriesThread.join();
}

void MainMenuScene::pauseLibraryTaskWorker() {
  libraryTaskWorkerPaused = true;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    for (auto &task : libraryTasks) {
      if (isPauseableLibraryTaskStatus(task.status)) {
        task.status = LibraryTaskStatus::Paused;
        task.detail = "Paused";
        changed = true;
      }
    }
    if (changed) {
      bumpLibraryTasksRevisionLocked();
    }
  }
  libraryTaskCv.notify_all();
}

void MainMenuScene::resumeLibraryTaskWorker() {
  libraryTaskWorkerPaused = false;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    for (auto &task : libraryTasks) {
      if (task.status != LibraryTaskStatus::Paused) {
        continue;
      }
      const auto queuedIt = std::find_if(
          libraryTaskQueue.begin(), libraryTaskQueue.end(),
          [&task](const auto &queuedTask) { return queuedTask.id == task.id; });
      task.status = queuedIt != libraryTaskQueue.end()
                        ? LibraryTaskStatus::Queued
                        : LibraryTaskStatus::Running;
      task.detail = queuedIt != libraryTaskQueue.end() ? "Waiting" : "Resuming";
      changed = true;
    }
    if (changed) {
      bumpLibraryTasksRevisionLocked();
    }
  }
  libraryTaskPauseCv.notify_all();
  libraryTaskCv.notify_all();
}

bool MainMenuScene::waitForLibraryTaskResume(std::uint64_t id,
                                             const std::stop_token &stopToken) {
  if (!libraryTaskWorkerPaused.load()) {
    return !stopToken.stop_requested();
  }

  const LibraryTaskProgressSnapshot snapshot = readLibraryTaskProgress();
  const double fraction =
      snapshot.valid && snapshot.taskId == id
          ? static_cast<double>(snapshot.basisPoints) / 10000.0
          : 0.0;
  const int current =
      snapshot.valid && snapshot.taskId == id ? snapshot.current : 0;
  const int total =
      snapshot.valid && snapshot.taskId == id ? snapshot.total : 0;
  setLibraryTaskState(id, LibraryTaskStatus::Paused, fraction, current, total,
                      "Paused");

  std::unique_lock<std::mutex> lock(libraryTaskPauseMutex);
  libraryTaskPauseCv.wait(lock, [this, &stopToken]() {
    return stopToken.stop_requested() || !libraryTaskWorkerPaused.load();
  });
  if (stopToken.stop_requested()) {
    return false;
  }
  setLibraryTaskState(id, LibraryTaskStatus::Running, fraction, current, total,
                      "Resuming");
  return true;
}

void MainMenuScene::enqueueLibraryRefreshTask(
    const std::string &title, const std::filesystem::path &folderToAdd,
    const std::string &iosBookmark) {
  std::filesystem::path taskFolderToAdd = folderToAdd;
  std::string taskIOSBookmark = iosBookmark;
  if (!taskFolderToAdd.empty()) {
    auto &dbHelper = ChartDBHelper::GetInstance();
    SqliteConnectionHandle taskDbHandle(dbHelper.Connect());
    sqlite3 *taskDb = taskDbHandle.get();
    if (taskDb != nullptr) {
      dbHelper.CreateEntriesTable(taskDb);
      if (dbHelper.InsertEntry(taskDb, taskFolderToAdd, taskIOSBookmark)) {
        taskFolderToAdd.clear();
        taskIOSBookmark.clear();
        requestLibraryReload(true);
      }
    }
  }

  startLibraryTaskWorker();
  const std::uint64_t id = nextLibraryTaskId.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    libraryTaskQueue.push_back(LibraryTaskRequest{
        .id = id,
        .kind = LibraryTaskKind::RefreshLibrary,
        .title = title,
        .folderToAdd = taskFolderToAdd,
        .iosBookmark = taskIOSBookmark,
    });
    libraryTasks.push_back(LibraryTaskInfo{
        .id = id,
        .title = title,
        .status = LibraryTaskStatus::Queued,
        .fraction = 0.0,
        .current = 0,
        .total = 0,
        .detail = "Waiting",
    });
    constexpr std::size_t kMaxTaskHistory = 24;
    while (libraryTasks.size() > kMaxTaskHistory) {
      const auto removable = std::find_if(
          libraryTasks.begin(), libraryTasks.end(), [](const auto &task) {
            return task.status == LibraryTaskStatus::Complete ||
                   task.status == LibraryTaskStatus::Failed ||
                   task.status == LibraryTaskStatus::Paused;
          });
      if (removable == libraryTasks.end()) {
        break;
      }
      libraryTasks.erase(removable);
    }
    bumpLibraryTasksRevisionLocked();
  }
  libraryTaskCv.notify_one();
}

#if TARGET_OS_ANDROID
void MainMenuScene::createPendingAndroidImportTask(bool folderImport) {
  startLibraryTaskWorker();
  const std::uint64_t id = nextLibraryTaskId.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    libraryTasks.push_back(LibraryTaskInfo{
        .id = id,
        .title = folderImport ? "Import Folder" : "Import Archive",
        .status = LibraryTaskStatus::Running,
        .fraction = 0.0,
        .current = 0,
        .total = 0,
        .detail = folderImport ? "Copying selected folder"
                               : "Copying selected archive",
    });
    constexpr std::size_t kMaxTaskHistory = 24;
    while (libraryTasks.size() > kMaxTaskHistory) {
      const auto removable = std::find_if(
          libraryTasks.begin(), libraryTasks.end(), [](const auto &task) {
            return task.status == LibraryTaskStatus::Complete ||
                   task.status == LibraryTaskStatus::Failed ||
                   task.status == LibraryTaskStatus::Paused;
          });
      if (removable == libraryTasks.end()) {
        break;
      }
      libraryTasks.erase(removable);
    }
    bumpLibraryTasksRevisionLocked();
  }
  {
    std::lock_guard<std::mutex> lock(androidArchiveImportMutex);
    pendingAndroidArchiveImportTasks.emplace_back(id, folderImport);
  }
  androidArchiveImportCopyPending.store(true);
  tasksModalOpenRequested.store(true);
}

void MainMenuScene::enqueueAndroidImportTask(
    std::uint64_t id, const std::filesystem::path &importPath,
    bool folderImport) {
  if (importPath.empty()) {
    setLibraryTaskState(id, LibraryTaskStatus::Failed, 0.0, 0, 0,
                        "Import failed: selected path is empty.");
    return;
  }

  startLibraryTaskWorker();
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    libraryTaskQueue.push_back(LibraryTaskRequest{
        .id = id,
        .kind = LibraryTaskKind::AndroidImport,
        .title = folderImport ? "Import Folder" : "Import Archive",
        .androidImportPath = importPath,
        .androidImportFolder = folderImport,
    });
    auto taskIt = std::find_if(libraryTasks.begin(), libraryTasks.end(),
                               [id](const auto &task) {
                                 return task.id == id;
                               });
    if (taskIt == libraryTasks.end()) {
      libraryTasks.push_back(LibraryTaskInfo{
          .id = id,
          .title = folderImport ? "Import Folder" : "Import Archive",
          .status = LibraryTaskStatus::Queued,
          .fraction = 0.0,
          .current = 0,
          .total = 0,
          .detail = "Waiting",
      });
    } else {
      taskIt->title = folderImport ? "Import Folder" : "Import Archive";
      taskIt->status = LibraryTaskStatus::Queued;
      taskIt->fraction = 0.0;
      taskIt->current = 0;
      taskIt->total = 0;
      taskIt->detail = "Waiting";
    }
    bumpLibraryTasksRevisionLocked();
  }
  tasksModalOpenRequested.store(true);
  libraryTaskCv.notify_one();
}
#endif

bool MainMenuScene::isPauseableLibraryTaskStatus(LibraryTaskStatus status) {
  return status == LibraryTaskStatus::Queued ||
         status == LibraryTaskStatus::Running;
}

bool MainMenuScene::isActiveLibraryTaskStatus(LibraryTaskStatus status) {
  return isPauseableLibraryTaskStatus(status) ||
         status == LibraryTaskStatus::Paused;
}

void MainMenuScene::setLibraryTaskState(std::uint64_t id,
                                        LibraryTaskStatus status,
                                        double fraction, int current, int total,
                                        const std::string &detail) {
  std::lock_guard<std::mutex> lock(libraryTaskMutex);
  auto taskIt = std::find_if(libraryTasks.begin(), libraryTasks.end(),
                             [id](const auto &task) { return task.id == id; });
  if (taskIt == libraryTasks.end()) {
    return;
  }
  taskIt->status = status;
  taskIt->fraction = std::clamp(fraction, 0.0, 1.0);
  taskIt->current = std::max(0, current);
  taskIt->total = std::max(0, total);
  taskIt->detail = detail;
  bumpLibraryTasksRevisionLocked();
}

void MainMenuScene::bumpLibraryTasksRevisionLocked() {
  ++libraryTasksRevision;
  const int activeCount = static_cast<int>(std::count_if(
      libraryTasks.begin(), libraryTasks.end(),
      [](const auto &task) { return isActiveLibraryTaskStatus(task.status); }));
  libraryActiveTaskCount.store(activeCount, std::memory_order_release);
}

void MainMenuScene::updateLibraryTaskProgress(
    std::uint64_t id, const ChartScanProgress &progress) {
  const int total = std::max(0, progress.total);
  const int current = total > 0 ? std::clamp(progress.current, 0, total)
                                : std::max(0, progress.current);
  const int basisPoints =
      total > 0
          ? static_cast<int>((static_cast<std::int64_t>(current) * 10000) /
                             std::max(1, total))
          : 0;
  std::uint64_t revision =
      libraryProgressRevision.load(std::memory_order_relaxed);
  if ((revision & 1U) != 0) {
    ++revision;
  }
  libraryProgressRevision.store(revision + 1, std::memory_order_release);
  libraryProgressTaskId.store(id, std::memory_order_relaxed);
  libraryProgressCurrent.store(current, std::memory_order_relaxed);
  libraryProgressTotal.store(total, std::memory_order_relaxed);
  libraryProgressBasisPoints.store(std::clamp(basisPoints, 0, 10000),
                                   std::memory_order_relaxed);
  libraryProgressStage.store(static_cast<int>(progress.stage),
                             std::memory_order_relaxed);
  libraryProgressRevision.store(revision + 2, std::memory_order_release);
}

MainMenuScene::LibraryTaskProgressSnapshot
MainMenuScene::readLibraryTaskProgress() const {
  for (int attempt = 0; attempt < 3; ++attempt) {
    const std::uint64_t before =
        libraryProgressRevision.load(std::memory_order_acquire);
    if ((before & 1U) != 0) {
      continue;
    }
    LibraryTaskProgressSnapshot snapshot;
    snapshot.valid = before != 0;
    snapshot.revision = before;
    snapshot.taskId = libraryProgressTaskId.load(std::memory_order_relaxed);
    snapshot.current = libraryProgressCurrent.load(std::memory_order_relaxed);
    snapshot.total = libraryProgressTotal.load(std::memory_order_relaxed);
    snapshot.basisPoints =
        libraryProgressBasisPoints.load(std::memory_order_relaxed);
    snapshot.stage = static_cast<ChartScanProgressStage>(
        libraryProgressStage.load(std::memory_order_relaxed));
    const std::uint64_t after =
        libraryProgressRevision.load(std::memory_order_acquire);
    if (before == after && (after & 1U) == 0) {
      return snapshot;
    }
  }
  return {};
}

int MainMenuScene::activeLibraryTaskCount() {
  return libraryActiveTaskCount.load(std::memory_order_acquire);
}

void MainMenuScene::requestLibraryScanFlush() {
  if (activeLibraryTaskCount() > 0) {
    libraryScanFlushRequested.fetch_add(1, std::memory_order_release);
  }
  requestLibraryReload(true);
}

std::uint64_t MainMenuScene::pendingLibraryScanFlushRequest() const {
  const std::uint64_t requested =
      libraryScanFlushRequested.load(std::memory_order_acquire);
  const std::uint64_t completed =
      libraryScanFlushCompleted.load(std::memory_order_acquire);
  return requested > completed ? requested : 0;
}

void MainMenuScene::completeLibraryScanFlush(std::uint64_t request) {
  if (request == 0) {
    return;
  }
  std::uint64_t completed =
      libraryScanFlushCompleted.load(std::memory_order_relaxed);
  while (completed < request &&
         !libraryScanFlushCompleted.compare_exchange_weak(
             completed, request, std::memory_order_release,
             std::memory_order_relaxed)) {
  }
  requestLibraryReload(true);
}

void MainMenuScene::refreshTasksButton() {
  if (tasksButtonText == nullptr) {
    return;
  }
  const int count = activeLibraryTaskCount();
  std::string label = std::to_string(count);
  label += count == 1 ? " Task" : " Tasks";
  if (label == displayedLibraryTasksButtonText) {
    return;
  }
  displayedLibraryTasksButtonText = label;
  tasksButtonText->setText(label);
}

void MainMenuScene::libraryTaskLoop(const std::stop_token &stopToken) {
  std::optional<LibraryTaskRequest> pausedTask;
  while (!stopToken.stop_requested()) {
    LibraryTaskRequest task;
    {
      std::unique_lock<std::mutex> lock(libraryTaskMutex);
      libraryTaskCv.wait(lock, [this, &stopToken]() {
        return stopToken.stop_requested() ||
               (!libraryTaskWorkerPaused.load() && !libraryTaskQueue.empty());
      });
      if (stopToken.stop_requested()) {
        break;
      }
      task = libraryTaskQueue.front();
      libraryTaskQueue.pop_front();
    }

    if (!waitForLibraryTaskResume(task.id, stopToken)) {
      pausedTask = task;
      break;
    }
    setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.0, 0, 0,
                        "Starting");
    try {
      switch (task.kind) {
      case LibraryTaskKind::RefreshLibrary:
        runLibraryRefreshTask(task, stopToken);
        break;
      case LibraryTaskKind::AndroidImport:
#if TARGET_OS_ANDROID
        runAndroidImportTask(task, stopToken);
#else
        throw std::runtime_error("Android import task is unavailable.");
#endif
        break;
      }
      if (stopToken.stop_requested()) {
        setLibraryTaskState(task.id, LibraryTaskStatus::Paused, 0.0, 0, 0,
                            "Paused");
        pausedTask = task;
      } else {
        setLibraryTaskState(task.id, LibraryTaskStatus::Complete, 1.0, 1, 1,
                            "Complete");
      }
    } catch (const std::exception &e) {
      setLibraryTaskState(task.id, LibraryTaskStatus::Failed, 0.0, 0, 0,
                          e.what());
      archive_file::appendDebugLogLine("Library task failed: " + task.title +
                                       ": " + e.what());
    }
  }

  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    if (pausedTask.has_value()) {
      const auto queuedIt =
          std::find_if(libraryTaskQueue.begin(), libraryTaskQueue.end(),
                       [&pausedTask](const auto &task) {
                         return task.id == pausedTask->id;
                       });
      if (queuedIt == libraryTaskQueue.end()) {
        libraryTaskQueue.push_front(*pausedTask);
      }
    }
    for (auto &task : libraryTasks) {
      if (isPauseableLibraryTaskStatus(task.status)) {
        task.status = LibraryTaskStatus::Paused;
        task.detail = "Paused";
      }
    }
    bumpLibraryTasksRevisionLocked();
  }
}

void MainMenuScene::seedDefaultDifficultyTablesIfNeeded(
    sqlite3 *taskDb, std::uint64_t taskId,
    const std::stop_token &stopToken) {
  if (context.settings.defaultDifficultyTablesSeeded ||
      stopToken.stop_requested()) {
    return;
  }

  auto &dbHelper = ChartDBHelper::GetInstance();
  constexpr int totalTables =
      static_cast<int>(sizeof(kDefaultDifficultyTableUrls) /
                       sizeof(kDefaultDifficultyTableUrls[0]));
  int successfulTables = 0;
  bool allSucceeded = true;

  for (int i = 0; i < totalTables; ++i) {
    if (stopToken.stop_requested()) {
      return;
    }

    const char *url = kDefaultDifficultyTableUrls[i];
    setLibraryTaskState(taskId, LibraryTaskStatus::Running, 0.02, i,
                        totalTables, "Adding default difficulty tables");

    std::string errorMessage;
    const bool ok = dbHelper.ImportDifficultyTableFromUrl(
        taskDb, url, &errorMessage,
        [this, taskId, i, totalTables,
         url](const DifficultyTableImportProgress &progress) {
          std::string detail = progress.tableName.empty()
                                   ? std::string(url)
                                   : progress.tableName;
          setLibraryTaskState(taskId, LibraryTaskStatus::Running, 0.02,
                              i + (progress.current > 0 ? 1 : 0), totalTables,
                              "Adding default table: " + detail);
        });
    if (ok) {
      ++successfulTables;
    } else {
      allSucceeded = false;
      SDL_Log("Failed to import default difficulty table %s: %s", url,
              errorMessage.empty() ? "unknown error" : errorMessage.c_str());
    }
  }

  if (stopToken.stop_requested()) {
    return;
  }

  if (allSucceeded) {
    context.settings.defaultDifficultyTablesSeeded = true;
    if (!context.settings.save()) {
      SDL_Log("Failed to save default difficulty table seed setting");
    }
  }
  if (successfulTables > 0) {
    requestLibraryReload(true);
  }
}

void MainMenuScene::runLibraryRefreshTask(const LibraryTaskRequest &task,
                                          const std::stop_token &stopToken) {
  auto &dbHelper = ChartDBHelper::GetInstance();
  SqliteConnectionHandle taskDbHandle(dbHelper.Connect());
  sqlite3 *taskDb = taskDbHandle.get();
  if (taskDb == nullptr) {
    throw std::runtime_error("Failed to open chart database");
  }
  dbHelper.CreateChartMetaTable(taskDb);
  dbHelper.CreateSolidArchiveTable(taskDb);
  dbHelper.CreateEntriesTable(taskDb);
  dbHelper.CreateDifficultyTableTables(taskDb);

  auto pauseTask = [&]() {
    return waitForLibraryTaskResume(task.id, stopToken);
  };
  if (!pauseTask()) {
    return;
  }

  if (!task.folderToAdd.empty()) {
    setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.01, 0, 0,
                        "Adding folder");
    dbHelper.InsertEntry(taskDb, task.folderToAdd, task.iosBookmark);
  }

  setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.02, 0, 0,
                      "Importing difficulty tables");
  if (!pauseTask()) {
    return;
  }
  seedDefaultDifficultyTablesIfNeeded(taskDb, task.id, stopToken);
  if (stopToken.stop_requested() || !pauseTask()) {
    return;
  }
  const int importedTables = dbHelper.ImportDifficultyTablesFromDirectory(
      taskDb, Utils::GetDocumentsPath("tables"));
  if (importedTables > 0 && !stopToken.stop_requested()) {
    requestLibraryReload(true);
  }
  auto entries = dbHelper.SelectEffectiveEntries(taskDb);

  if (stopToken.stop_requested()) {
    return;
  }

  if (entries.empty()) {
    setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.04, 0, 0,
                        "Waiting for library folder");
    if (!pauseTask()) {
      return;
    }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
    std::string folder;
    std::string bookmark;
    std::string errorMessage;
    if (PickIOSFolder(folder, bookmark, errorMessage)) {
      dbHelper.InsertEntry(taskDb, std::filesystem::path(folder), bookmark);
      entries = dbHelper.SelectEffectiveEntries(taskDb);
    } else {
      if (!errorMessage.empty()) {
        SDL_Log("Failed to pick iOS library folder: %s", errorMessage.c_str());
      }
      auto path = ChartDBHelper::DefaultBmsFolderPath();
      ensureLibraryFolderExists(path);
      entries.push_back({
          .path = fspath_to_path_t(path),
          .iosBookmark = "",
      });
    }
#elif TARGET_OS_ANDROID
    auto path = ChartDBHelper::DefaultBmsFolderPath();
    ensureLibraryFolderExists(path);
    dbHelper.InsertEntry(taskDb, path);
    entries = dbHelper.SelectEffectiveEntries(taskDb);
#else
    char *folder_c = tinyfd_selectFolderDialog("Select Folder", nullptr);
    std::string folder;
    if (folder_c == nullptr) {
      std::cerr << "tinyfd_selectFolderDialog error: " << strerror(errno)
                << std::endl;
      std::cout << "Failed to open folder select dialog.\n";

      while (folder.empty()) {
        if (stopToken.stop_requested()) {
          return;
        }

        std::cout << "Enter bms folder path: ";
        std::cin >> folder;
        if (std::cin.eof() || std::cin.fail()) {
          break;
        }
        if (folder.empty()) {
          continue;
        }

        if (!expandCurrentUserHomeShortcut(folder)) {
          std::cout << "Could not expand ~ because no home directory is set.\n";
          folder.clear();
          continue;
        }
        std::ifstream test(folder);
        if (!test)
          folder = "";
      }

      if (folder.empty()) {
        return;
      }
    } else {
      folder = folder_c;
    }
    std::filesystem::path path(folder);
    dbHelper.InsertEntry(taskDb, path);
    entries = dbHelper.SelectEffectiveEntries(taskDb);
#endif
  }

  if (stopToken.stop_requested()) {
    return;
  }

  setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.06, 0, 0,
                      "Refreshing folder access");
  if (!pauseTask()) {
    return;
  }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  RefreshIOSFolderAccess(entries);
#endif
  LoadCharts(
      dbHelper, taskDb, entries, *this, stopToken,
      [this, taskId = task.id](const ChartScanProgress &progress) {
        updateLibraryTaskProgress(taskId, progress);
      },
      [this, taskId = task.id, &stopToken]() {
        return waitForLibraryTaskResume(taskId, stopToken);
      });
}

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
void MainMenuScene::addIOSFolderEntryFromFiles() {
  if (willStart.load() || replayExportInProgress.load() ||
      addFolderPickerInProgress.load()) {
    return;
  }
  if (addFolderPickerThread.joinable()) {
    addFolderPickerThread.join();
  }
  if (addFolderPickerInProgress.exchange(true)) {
    return;
  }

  addFolderPickerThread =
      std::jthread([this](const std::stop_token &stopToken) {
        struct PickerFlagReset {
          std::atomic_bool &flag;
          ~PickerFlagReset() { flag.store(false); }
        } reset{addFolderPickerInProgress};

        std::string folder;
        std::string bookmark;
        std::string errorMessage;
        if (!PickIOSFolder(folder, bookmark, errorMessage)) {
          if (!errorMessage.empty()) {
            SDL_Log("Failed to pick iOS library folder: %s",
                    errorMessage.c_str());
          }
          return;
        }
        if (stopToken.stop_requested()) {
          return;
        }

        std::filesystem::path folderPath(folder);
        std::string folderName =
            !folderPath.filename().empty()
                ? fspath_to_utf8(folderPath.filename())
                : fspath_to_utf8(folderPath);
        if (folderName.empty()) {
          folderName = "Folder";
        }
        enqueueLibraryRefreshTask("Add Folder: " + folderName, folderPath,
                                  bookmark);
      });
}
#endif

#if TARGET_OS_ANDROID
void MainMenuScene::addAndroidFolderEntryFromPicker() {
  if (willStart.load() || replayExportInProgress.load() ||
      addFolderPickerInProgress.load()) {
    return;
  }
  if (addFolderPickerThread.joinable()) {
    addFolderPickerThread.join();
  }
  if (addFolderPickerInProgress.exchange(true)) {
    return;
  }

  addFolderPickerThread =
      std::jthread([this](const std::stop_token &stopToken) {
        struct PickerFlagReset {
          std::atomic_bool &flag;
          ~PickerFlagReset() { flag.store(false); }
        } reset{addFolderPickerInProgress};

        std::filesystem::path folder;
        std::string treeUri;
        std::string errorMessage;
        if (!PickAndroidChartFolder(folder, treeUri, errorMessage)) {
          if (!errorMessage.empty()) {
            SDL_Log("Failed to pick Android library folder: %s",
                    errorMessage.c_str());
          }
          return;
        }
        if (stopToken.stop_requested()) {
          return;
        }

        std::filesystem::path folderPath(folder);
        std::string folderName =
            !folderPath.filename().empty()
                ? fspath_to_utf8(folderPath.filename())
                : fspath_to_utf8(folderPath);
        if (folderName.empty()) {
          folderName = "Folder";
        }
        enqueueLibraryRefreshTask("Add Folder: " + folderName, folderPath,
                                  treeUri);
      });
}

void MainMenuScene::importAndroidArchiveFromPicker() {
  importAndroidPathFromPicker(false);
}

void MainMenuScene::importAndroidFolderFromPicker() {
  importAndroidPathFromPicker(true);
}

void MainMenuScene::importAndroidPathFromPicker(bool folderImport) {
  if (willStart.load() || replayExportInProgress.load() ||
      archiveImportPickerInProgress.load()) {
    return;
  }
  if (archiveImportPickerThread.joinable()) {
    archiveImportPickerThread.join();
  }
  if (archiveImportPickerInProgress.exchange(true)) {
    return;
  }
  if (replayStatusText != nullptr) {
    replayStatusText->setText(folderImport ? "Choose a folder..."
                                           : "Choose an archive...");
  }

  archiveImportPickerThread =
      std::jthread([this, folderImport](const std::stop_token &stopToken) {
        struct PickerFlagReset {
          std::atomic_bool &flag;
          ~PickerFlagReset() { flag.store(false); }
        } reset{archiveImportPickerInProgress};

        std::filesystem::path importPath;
        std::string errorMessage;
        const bool picked =
            folderImport ? PickAndroidFolderForImport(importPath, errorMessage)
                         : PickAndroidArchiveForImport(importPath,
                                                       errorMessage);
        if (!picked) {
          std::lock_guard<std::mutex> lock(androidArchiveImportMutex);
          pendingAndroidArchiveImportError =
              errorMessage.empty()
                  ? (folderImport ? "Folder import cancelled."
                                  : "Archive import cancelled.")
                  : errorMessage;
          return;
        }
        if (stopToken.stop_requested()) {
          return;
        }

        if (importPath.empty()) {
          createPendingAndroidImportTask(folderImport);
        } else {
          const std::uint64_t id = nextLibraryTaskId.fetch_add(1);
          enqueueAndroidImportTask(id, importPath, folderImport);
        }
      });
}

void MainMenuScene::pollPendingAndroidArchiveImport() {
  const std::uint64_t now = SDL_GetTicks64();
  if (now < nextAndroidArchiveImportPollMs) {
    return;
  }
  nextAndroidArchiveImportPollMs = now + 1000;

  std::string errorMessage;
  const auto archivePath = ConsumePendingAndroidArchiveImport(errorMessage);
  if (!archivePath.has_value() && errorMessage.empty()) {
    return;
  }

  std::uint64_t taskId = 0;
  bool folderImport = false;
  {
    std::lock_guard<std::mutex> lock(androidArchiveImportMutex);
    if (!pendingAndroidArchiveImportTasks.empty()) {
      taskId = pendingAndroidArchiveImportTasks.front().first;
      folderImport = pendingAndroidArchiveImportTasks.front().second;
      pendingAndroidArchiveImportTasks.pop_front();
    }
    androidArchiveImportCopyPending.store(
        !pendingAndroidArchiveImportTasks.empty());
  }
  if (archivePath.has_value()) {
    if (taskId == 0) {
      taskId = nextLibraryTaskId.fetch_add(1);
    }
    enqueueAndroidImportTask(taskId, *archivePath, folderImport);
  } else {
    if (taskId != 0) {
      setLibraryTaskState(taskId, LibraryTaskStatus::Failed, 0.0, 0, 0,
                          errorMessage.empty() ? "Import failed"
                                               : errorMessage);
    }
    {
      std::lock_guard<std::mutex> lock(androidArchiveImportMutex);
      pendingAndroidArchiveImportError = errorMessage;
    }
  }
}

void MainMenuScene::applyPendingAndroidArchiveImport() {
  std::optional<std::string> errorMessage;
  {
    std::lock_guard<std::mutex> lock(androidArchiveImportMutex);
    if (pendingAndroidArchiveImportError.has_value()) {
      errorMessage = std::move(pendingAndroidArchiveImportError);
      pendingAndroidArchiveImportError.reset();
    }
  }

  if (errorMessage.has_value()) {
    SDL_Log("Android archive import failed: %s", errorMessage->c_str());
    if (replayStatusText != nullptr) {
      replayStatusText->setText("Archive import failed");
    }
  }
}

void MainMenuScene::runAndroidImportTask(const LibraryTaskRequest &task,
                                         const std::stop_token &stopToken) {
  const std::filesystem::path importPath = task.androidImportPath;
  if (importPath.empty()) {
    throw std::runtime_error("Import failed: selected path is empty.");
  }

  std::error_code importPathError;
  const bool importingFolder =
      task.androidImportFolder ||
      std::filesystem::is_directory(importPath, importPathError);
  const std::string importType = importingFolder ? "folder" : "archive";
  const std::filesystem::path outputRoot = ChartDBHelper::DefaultBmsFolderPath();

  setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.0, 0, 0,
                      "Preparing import");
  archive_file::appendDebugLogLine(
      "Android import task requested: " + fspath_to_utf8(importPath) +
      " outputRoot=" + fspath_to_utf8(outputRoot));

  auto postImportProgress = [this, &task,
                             importingFolder](double fraction,
                                              const std::string &message) {
    setLibraryTaskState(task.id, LibraryTaskStatus::Running,
                        std::clamp(fraction, 0.0, 1.0), 0, 0,
                        message.empty()
                            ? (importingFolder ? "Importing folder"
                                               : "Importing archive")
                            : message);
  };

  std::string errorMessage;
  std::error_code fsError;
  if (!Utils::EnsureDirectoryExists(outputRoot, fsError)) {
    throw std::runtime_error("Import failed: could not create BMS import "
                             "folder: " +
                             fsError.message());
  }

  std::filesystem::path outputFolder;
  if (importingFolder) {
    outputFolder = importPath;
    postImportProgress(0.90, "Refreshing library");
  } else {
    auto postUnzipProgress =
        [&](const archive_file::UnzipProgress &progress) {
          postImportProgress(progress.fraction * 0.90, progress.message);
        };
    const auto unzippedArchive = archive_file::unzipArchiveFully(
        importPath, outputRoot, &errorMessage, &stopToken, postUnzipProgress);
    if (unzippedArchive.has_value()) {
      outputFolder = unzippedArchive->outputFolder;
    }
  }

  if (outputFolder.empty()) {
    throw std::runtime_error(
        stopToken.stop_requested()
            ? "Import cancelled"
            : (errorMessage.empty() ? "Import failed"
                                    : "Import failed: " + errorMessage));
  }
  if (stopToken.stop_requested()) {
    throw std::runtime_error("Import cancelled");
  }

  auto &dbHelper = ChartDBHelper::GetInstance();
  SqliteConnectionHandle importDbHandle(dbHelper.Connect());
  sqlite3 *importDb = importDbHandle.get();
  if (importDb == nullptr) {
    throw std::runtime_error("Imported " + importType +
                             ". Failed to refresh library.");
  }

  dbHelper.CreateEntriesTable(importDb);
  dbHelper.CreateChartMetaTable(importDb);
  dbHelper.CreateSolidArchiveTable(importDb);
  dbHelper.CreateDifficultyTableTables(importDb);
  dbHelper.InsertEntry(importDb, outputRoot);

  std::vector<std::filesystem::path> roots{outputFolder};
  postImportProgress(0.92, "Refreshing library");
  auto scanProgress = [this, &task](const ChartScanProgress &progress) {
    const int total = std::max(0, progress.total);
    const int current = total > 0 ? std::clamp(progress.current, 0, total)
                                  : std::max(0, progress.current);
    const double scanFraction =
        total > 0 ? static_cast<double>(current) / std::max(1, total) : 0.0;
    setLibraryTaskState(task.id, LibraryTaskStatus::Running,
                        0.92 + scanFraction * 0.08, current, total,
                        chartScanProgressStageText(progress.stage));
  };
  auto pauseTask = [this, &task, &stopToken]() {
    return waitForLibraryTaskResume(task.id, stopToken);
  };
  const int changedCount =
      dbHelper.ScanChartRoots(importDb, roots, &stopToken, scanProgress,
                              pauseTask);
  if (stopToken.stop_requested()) {
    throw std::runtime_error("Import cancelled");
  }

  if (!importingFolder) {
    std::filesystem::remove(importPath, fsError);
  }
  requestLibraryReload(true);
  const std::string message =
      changedCount > 0 ? "Imported " + importType + ". Library refreshed."
                       : "Imported " + importType +
                             ". Library already current.";
  SDL_Log("Android import task result: %s", message.c_str());
  archive_file::appendDebugLogLine(message);
  setLibraryTaskState(task.id, LibraryTaskStatus::Running, 1.0, 1, 1,
                      message);
}
#endif

void MainMenuScene::initView(ApplicationContext &context) {
  // Initialize the view
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  rootLayout = nullptr;
  jacketView = nullptr;
  searchBox = nullptr;
  difficultyFilterBox = nullptr;
  startButton = nullptr;
  chartActionsRow = nullptr;
  replayButtonSlot = nullptr;
  replayButton = nullptr;
  findBmsButtonSlot = nullptr;
  findBmsButton = nullptr;
  findBmsButtonText = nullptr;
  unzipButtonSlot = nullptr;
  unzipButton = nullptr;
  unzipButtonText = nullptr;
  parseLogButton = nullptr;
  parseLogButtonText = nullptr;
  musicButton = nullptr;
  musicButtonText = nullptr;
  tasksButton = nullptr;
  tasksButtonText = nullptr;
  replayButtonText = nullptr;
  replayStatusText = nullptr;
  replayModalRoot = nullptr;
  replayModalContentFrame = nullptr;
  replayListContent = nullptr;
  replayExportOptionsContent = nullptr;
  replayExportProgressContent = nullptr;
  replayExportProgressTrack = nullptr;
  replayExportProgressFill = nullptr;
  replayModalTitleText = nullptr;
  replayExportProgressMessageText = nullptr;
  replayExportProgressPercentText = nullptr;
  startButtonText = nullptr;
  playOptionsModalRoot = nullptr;
  musicModalRoot = nullptr;
  unzipModalRoot = nullptr;
  unzipProgressTrack = nullptr;
  unzipProgressFill = nullptr;
  unzipModalTitleText = nullptr;
  unzipProgressMessageText = nullptr;
  unzipProgressPercentText = nullptr;
  unzipProgressDetailText = nullptr;
  unzipDeleteArchiveButton = nullptr;
  unzipCancelButton = nullptr;
  unzipDeleteArchiveButtonText = nullptr;
  unzipCancelButtonText = nullptr;
  parseLogModalRoot = nullptr;
  tasksModalRoot = nullptr;
  parseLogScrollView = nullptr;
  parseLogContent = nullptr;
  parseLogText = nullptr;
  parseLogCloseButton = nullptr;
  parseLogCloseButtonText = nullptr;
  musicTrackText = nullptr;
  musicStatusText = nullptr;
  musicPlaylistText = nullptr;
  musicSelectedButton = nullptr;
  musicAddSelectedButton = nullptr;
  musicRemoveSelectedButton = nullptr;
  musicPlaylistButton = nullptr;
  musicClearPlaylistButton = nullptr;
  musicRandomButton = nullptr;
  musicPreviousButton = nullptr;
  musicSeekBackwardButton = nullptr;
  musicPlayPauseButton = nullptr;
  musicSeekForwardButton = nullptr;
  musicNextButton = nullptr;
  musicStopButton = nullptr;
  musicCloseButton = nullptr;
  musicSelectedButtonText = nullptr;
  musicAddSelectedButtonText = nullptr;
  musicRemoveSelectedButtonText = nullptr;
  musicPlaylistButtonText = nullptr;
  musicClearPlaylistButtonText = nullptr;
  musicRandomButtonText = nullptr;
  musicPreviousButtonText = nullptr;
  musicSeekBackwardButtonText = nullptr;
  musicPlayPauseButtonText = nullptr;
  musicSeekForwardButtonText = nullptr;
  musicNextButtonText = nullptr;
  musicStopButtonText = nullptr;
  musicCloseButtonText = nullptr;
  tasksScrollView = nullptr;
  tasksContent = nullptr;
  tasksText = nullptr;
  tasksRefreshButton = nullptr;
  tasksRefreshButtonText = nullptr;
  tasksCloseButton = nullptr;
  tasksCloseButtonText = nullptr;
  findBmsModalRoot = nullptr;
  findBmsProgressTrack = nullptr;
  findBmsProgressFill = nullptr;
  findBmsModalTitleText = nullptr;
  findBmsStatusText = nullptr;
  findBmsDetailText = nullptr;
  findBmsLogScrollView = nullptr;
  findBmsLogContent = nullptr;
  findBmsLogText = nullptr;
  findBmsCloseButton = nullptr;
  findBmsOpenButton = nullptr;
  findBmsGoogleButton = nullptr;
  findBmsRefreshButton = nullptr;
  findBmsCloseButtonText = nullptr;
  findBmsOpenButtonText = nullptr;
  findBmsGoogleButtonText = nullptr;
  findBmsRefreshButtonText = nullptr;
  readyGaugeText = nullptr;
  readyPlayOptionText = nullptr;
  readyAssistOptionText = nullptr;
  playOptionsCloseButton = nullptr;
  playOptionsCloseButtonText = nullptr;
  replayListView = nullptr;
  replayWatchButton = nullptr;
  replayModalPhotoButton = nullptr;
  replayModalExportButton = nullptr;
  replayModalCloseButton = nullptr;
  replayFps60Button = nullptr;
  replayFps120Button = nullptr;
  replayResolution1080Button = nullptr;
  replayResolutionFullButton = nullptr;
  replayResultIncludeButton = nullptr;
  replayResultSkipButton = nullptr;
  replayTouchShowButton = nullptr;
  replayTouchHideButton = nullptr;
  replayGhostShowButton = nullptr;
  replayGhostHideButton = nullptr;
  replayExportTouchShowButton = nullptr;
  replayExportTouchHideButton = nullptr;
  replayExportGhostShowButton = nullptr;
  replayExportGhostHideButton = nullptr;
  replayWatchButtonText = nullptr;
  replayModalPhotoButtonText = nullptr;
  replayModalExportButtonText = nullptr;
  replayModalCloseButtonText = nullptr;
  replayFps60ButtonText = nullptr;
  replayFps120ButtonText = nullptr;
  replayResolution1080ButtonText = nullptr;
  replayResolutionFullButtonText = nullptr;
  replayResultIncludeButtonText = nullptr;
  replayResultSkipButtonText = nullptr;
  replayTouchShowButtonText = nullptr;
  replayTouchHideButtonText = nullptr;
  replayGhostShowButtonText = nullptr;
  replayGhostHideButtonText = nullptr;
  replayExportTouchShowButtonText = nullptr;
  replayExportTouchHideButtonText = nullptr;
  replayExportGhostShowButtonText = nullptr;
  replayExportGhostHideButtonText = nullptr;
  pendingReplayExportResult.reset();
  pendingReplayExportProgress.reset();
  pendingUnzipResult.reset();
  pendingUnzipProgress.reset();
  pendingSelectChartPath.reset();
  suppressPreviewForChartPath.reset();
  unzipDeleteCandidatePath.reset();
  unzipEstimatedUncompressedSize = 0;
#if TARGET_OS_ANDROID
  {
    std::lock_guard<std::mutex> lock(androidArchiveImportMutex);
    pendingAndroidArchiveImportError.reset();
    pendingAndroidArchiveImportTasks.clear();
  }
  androidArchiveImportCopyPending = false;
#endif
  pendingFindBmsProgressEvents.clear();
  pendingFindBmsResult.reset();
  replayExportInProgress = false;
  unzipInProgress = false;
  tasksModalOpenRequested = false;
  findBmsJobRunning = false;
  findBmsCancelled = false;
  findBmsResult = {};
  findBmsProgressMessage.clear();
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.0;
  findBmsProgressLog.clear();
  musicStatusMessage.clear();
  replaySummaries.clear();
  selectedReplayIndex = -1;
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  selectedExportIncludeResultScreen = true;
  selectedReplayRenderTouchPoints = true;
  selectedReplayRenderGhosts = true;
  replayExportProgressFraction = 0.0;
  gaugeSelectionButtons.clear();
  playOptionButtons.clear();
  longNoteModeButtons.clear();
  assistOptionButtons.clear();

  appliedUiThemeMode = ui_theme::activeMode();

  recyclerView = new RecyclerView<ChartMetaRecord>(
      [](const ChartMetaRecord &a, const ChartMetaRecord &b) {
        return a.meta.SHA256 == b.meta.SHA256 && a.meta.MD5 == b.meta.MD5 &&
               a.meta.BmsPath == b.meta.BmsPath &&
               a.difficultyTableLabels == b.difficultyTableLabels &&
               a.courseStart == b.courseStart &&
               a.unavailable == b.unavailable &&
               a.solidArchive == b.solidArchive &&
               a.archiveSize == b.archiveSize &&
               a.archiveUncompressedSize == b.archiveUncompressedSize &&
               a.archiveFileCount == b.archiveFileCount &&
               a.favorite == b.favorite;
      });
  folderRecyclerView = new RecyclerView<LibraryFolderItem>(
      [](const LibraryFolderItem &a, const LibraryFolderItem &b) {
        return a.key == b.key;
      });
  auto dbHelper = ChartDBHelper::GetInstance();
  dbHelper.CreateChartMetaTable(db);
  dbHelper.CreateSolidArchiveTable(db);
  dbHelper.CreateFavoritesTable(db);
  dbHelper.CreateEntriesTable(db);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  RefreshIOSFolderAccess(dbHelper.SelectEffectiveEntries(db));
#elif TARGET_OS_ANDROID
  (void)dbHelper.SelectEffectiveEntries(db);
#endif
  dbHelper.CreateDifficultyTableTables(db);

  static constexpr int kChartListItemHeight = 108;
  recyclerView->onCreateView = [this](const ChartMetaRecord &item) {
    return new ChartListItemView(0, 0, rendering::window_width,
                                 kChartListItemHeight, item);
  };
  recyclerView->itemHeight = kChartListItemHeight;
  recyclerView->topMargin = 8;
  recyclerView->bottomMargin = 8;
  recyclerView->onBind = [this](View *view, const ChartMetaRecord &item,
                                int idx, bool isSelected) {
    auto *chartListItemView = dynamic_cast<ChartListItemView *>(view);
    chartListItemView->setMeta(item);
    chartListItemView->setClearRank(clearRankForChart(item));
    chartListItemView->setFavoriteToggleHandler(
        [this](const ChartMetaRecord &record, bool favorite) {
          return toggleChartFavorite(record, favorite);
        });
    if (isSelected) {
      chartListItemView->onSelected();
    } else {
      chartListItemView->onUnselected();
    }
  };

  jacketView = new ImageView(0, 0, 0, 0);
  recyclerView->onSelected = [this, &context](const ChartMetaRecord &item,
                                              int idx) {
    if (willStart.load())
      return;
    const auto &meta = item.meta;
    auto selectedView = recyclerView->getViewByIndex(idx);
    if (selectedView) {
      selectedView->onSelected();
    }
    refreshReplayAvailability(&item);
    refreshPlayOptionButtons();
    refreshLongNoteModeButtons();
    refreshAssistOptionButtons();
    if (!replayExportInProgress.load() && replayStatusText != nullptr) {
      replayStatusText->setText("");
    }
    if (item.courseStart) {
      setPlayableChartActionsVisible(true, false);
      refreshUnzipButtonForSelection(nullptr);
      setFindBmsButtonVisible(false);
      previewLoadCancelled = true;
      if (loadThread.joinable()) {
        SDL_Log("Joining preview thread");
        loadThread.join();
      }
      stopAndClearSelectedChart();
      jacketView->freeImage();
      refreshStartButtonForActiveFolder();
      return;
    }
    setPlayableChartActionsVisible(!item.unavailable && !item.solidArchive &&
                                   !meta.BmsPath.empty());
    refreshUnzipButtonForSelection(&item);
    setFindBmsButtonVisible(
        item.unavailable && !item.solidArchive &&
        (!meta.SHA256.empty() || !meta.MD5.empty() || !meta.Title.empty()));
    refreshStartButtonForActiveFolder();
    previewLoadCancelled = true;
    if (loadThread.joinable()) {
      SDL_Log("Joining preview thread");
      loadThread.join();
    }
    stopAndClearSelectedChart();
    if (item.unavailable || meta.BmsPath.empty()) {
      jacketView->freeImage();
      return;
    }
    if (item.solidArchive) {
      jacketView->freeImage();
      if (!replayExportInProgress.load() && replayStatusText != nullptr) {
        replayStatusText->setText(
            "Skipped solid archive. Estimated unzip: " +
            formatFindBmsBytes(item.archiveUncompressedSize));
      }
      archive_file::appendDebugLogLine(
          "Solid archive selected without chart probing: " +
          fspath_to_utf8(meta.BmsPath) +
          " files=" + std::to_string(item.archiveFileCount) +
          " estimatedUnpacked=" + std::to_string(item.archiveUncompressedSize));
      return;
    }
    bool archiveVirtualPath = archive_file::isVirtualPath(meta.BmsPath);
#if TARGET_OS_ANDROID
    if (archiveVirtualPath && IsAndroidTreePath(meta.BmsPath)) {
      archiveVirtualPath = false;
    }
#endif
    if (archiveVirtualPath && !context.settings.archiveChartPreviewEnabled) {
      jacketView->freeImage();
      if (!replayExportInProgress.load() && replayStatusText != nullptr) {
        replayStatusText->setText("Archive preview disabled");
      }
      archive_file::appendDebugLogLine(
          "Preview skipped by archive chart preview setting: " +
          fspath_to_utf8(meta.BmsPath));
      return;
    }
    bool suppressPreview = false;
    if (suppressPreviewForChartPath.has_value()) {
      const path_t suppressPath =
          fspath_to_path_t(suppressPreviewForChartPath.value());
      suppressPreview = suppressPath == fspath_to_path_t(meta.BmsPath);
    }
    if (suppressPreview) {
      suppressPreviewForChartPath.reset();
    }
    if (!meta.StageFile.empty()) {
      jacketView->setImageAsync(meta.Folder / meta.StageFile, true);
    } else {
      jacketView->freeImage();
    }
    if (suppressPreview) {
      if (!replayExportInProgress.load() && replayStatusText != nullptr) {
        replayStatusText->setText("Unzipped chart selected");
      }
      archive_file::appendDebugLogLine(
          "Preview suppressed for auto-selected unzipped chart: " +
          fspath_to_utf8(meta.BmsPath));
      return;
    }
    std::string musicStopError;
    context.musicPlayer.Stop(musicStopError);
    previewLoadCancelled = false;
    loadThread = std::thread([this, meta, &context]() {
      SDL_Log("Previewing %s", fspath_to_utf8(meta.BmsPath).c_str());

      // Debounce selection changes before doing expensive chart/media loading.
      for (int i = 0; i < 50; i++) {
        if (previewLoadCancelled) {
          return;
        }
        if (willStart.load())
          break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      context.jukebox.stop();
      SDL_Log("Parsing %s", fspath_to_utf8(meta.BmsPath).c_str());
      std::unique_ptr<bms_parser::Chart> chart;
      try {
        chart = play_options::parseChart(meta.BmsPath, previewLoadCancelled,
                                         "preview");
      } catch (const std::exception &e) {
        SDL_Log("Preview parse failed %s: %s",
                fspath_to_utf8(meta.BmsPath).c_str(), e.what());
        archive_file::appendDebugLogLine(
            "Preview parse exception: " + fspath_to_utf8(meta.BmsPath) + ": " +
            e.what());
        return;
      }
      SDL_Log("Parsed %s", fspath_to_utf8(meta.BmsPath).c_str());
      if (chart == nullptr) {
        SDL_Log("Chart is null");
        archive_file::appendDebugLogLine(
            "Preview chart is null: " + fspath_to_utf8(meta.BmsPath));
        return;
      }

      context.jukebox.loadChart(*chart, true, previewLoadCancelled);
      if (previewLoadCancelled) {
        return;
      }
      setSelectedChart(std::move(chart), true);
      if (previewLoadCancelled) {
        clearSelectedChart();
        return;
      }
      if (!willStart.load()) {
        context.jukebox.play();
      }
    });
  };
  recyclerView->onUnselected = [this](const ChartMetaRecord &item, int idx) {
    auto unselectedView = recyclerView->getViewByIndex(idx);
    if (unselectedView) {
      unselectedView->onUnselected();
    }
  };

  static constexpr int kFolderListItemHeight = 50;
  folderRecyclerView->onCreateView = [](const LibraryFolderItem &item) {
    return new LibraryFolderItemView(0, 0, kLibraryControlWidth,
                                     kFolderListItemHeight);
  };
  folderRecyclerView->itemHeight = kFolderListItemHeight;
  folderRecyclerView->onBind = [this](View *view, const LibraryFolderItem &item,
                                      int idx, bool isSelected) {
    auto *folderView = dynamic_cast<LibraryFolderItemView *>(view);
    if (folderView != nullptr) {
      folderView->setItem(item.label, item.depth, item.count, isSelected,
                          item.clearMarkFolder ? item.clearMarkRank
                                               : clearRankForFolder(item.key),
                          item.clearMarkFolder, item.expandable,
                          item.expanded);
    }
  };
  folderRecyclerView->onSelected = [this](const LibraryFolderItem &item,
                                          int idx) {
    auto selectedView = folderRecyclerView->getViewByIndex(idx);
    if (selectedView) {
      selectedView->onSelected();
    }
    selectFolder(item);
  };
  folderRecyclerView->onUnselected = [this](const LibraryFolderItem &item,
                                            int idx) {
    auto unselectedView = folderRecyclerView->getViewByIndex(idx);
    if (unselectedView) {
      unselectedView->onUnselected();
    }
  };

  rootLayout =
      new View(0, 0, rendering::window_width, rendering::window_height);
  addView(rootLayout);
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  lastLayoutWidth = rendering::window_width;
  lastLayoutHeight = rendering::window_height;
  lastSafeTop = safe.top;
  lastSafeLeft = safe.left;
  lastSafeBottom = safe.bottom;
  lastSafeRight = safe.right;
  rootLayout->setFlexDirection(FlexDirection::Row);
  rootLayout->setAlignItems(YGAlignStretch);
  rootLayout->setGap(24);
  rootLayout->setPadding(Edge::Top, safe.top + kRootPadding);
  rootLayout->setPadding(Edge::Left, safe.left + kRootPadding);
  rootLayout->setPadding(Edge::Right, safe.right + kRootPadding);
  rootLayout->setPadding(Edge::Bottom, safe.bottom + kRootPadding);
  rootLayout->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);

  auto nav = new View();
  nav->setFlexDirection(FlexDirection::Column);
  nav->setAlignItems(YGAlignStretch);
  nav->setWidth(kLibraryPanelWidth);
  nav->setGap(12);
  nav->setPadding(Edge::All, kLibraryPanelPadding);
  nav->setThemedBackgroundColor(ui_theme::mainMenuPanel);
  nav->setCornerRadius(ui_theme::panelRadius());
  nav->setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);
  nav->setThemedBorderColor(ui_theme::hairline);
  nav->setBorderWidth(1);

  auto *navTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  navTitle->setText("Library");
  navTitle->setThemedColor(ui_theme::textPrimary);
  nav->addView(navTitle);

  bool showAddFolderButton = false;
  std::string addFolderButtonLabel = "Add Folder";
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  showAddFolderButton = true;
#elif TARGET_OS_ANDROID
  const bool androidFullFileAccessBuild = AndroidBuildHasManageExternalStorage();
  showAddFolderButton = true;
  addFolderButtonLabel =
      androidFullFileAccessBuild ? "Add Folder" : "Import Folder";
#endif
  if (showAddFolderButton) {
    auto *addFolderButton = new Button(0, 0, kLibraryControlWidth, 50);
    auto *addFolderText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
    addFolderText->setText(addFolderButtonLabel);
    addFolderText->setAlign(TextView::CENTER);
    addFolderText->setVAlign(TextView::MIDDLE);
    addFolderButton->setContentView(addFolderText);
    styleThemedActionButton(addFolderButton, addFolderText, true,
                            ui_theme::primaryAction,
                            ui_theme::primaryActionHover,
                            ui_theme::primaryActionPressed,
                            ui_theme::accentBorderStrong);
    addFolderButton->setCornerRadius(ui_theme::controlRadius());
    addFolderButton->setStyledBorderWidth(1);
#if TARGET_OS_ANDROID
    addFolderButton->setOnClickListener([this]() {
      if (AndroidBuildHasManageExternalStorage()) {
        addAndroidFolderEntryFromPicker();
      } else {
        importAndroidFolderFromPicker();
      }
    });
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
    addFolderButton->setOnClickListener(
        [this]() { addIOSFolderEntryFromFiles(); });
#endif
    nav->addView(addFolderButton);
  }
#if TARGET_OS_ANDROID
  auto *importArchiveButton = new Button(0, 0, kLibraryControlWidth, 50);
  auto *importArchiveText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  importArchiveText->setText("Import Archive");
  importArchiveText->setAlign(TextView::CENTER);
  importArchiveText->setVAlign(TextView::MIDDLE);
  importArchiveButton->setContentView(importArchiveText);
  styleThemedActionButton(importArchiveButton, importArchiveText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  importArchiveButton->setCornerRadius(ui_theme::controlRadius());
  importArchiveButton->setStyledBorderWidth(1);
  importArchiveButton->setOnClickListener(
      [this]() { importAndroidArchiveFromPicker(); });
  nav->addView(importArchiveButton);
#endif

  folderRecyclerView->setFlex(1);
  folderRecyclerView->clearBackgroundColor();
  folderRecyclerView->setBorderWidth(0);
  folderRecyclerView->setCornerRadius(ui_theme::controlRadius());
  nav->addView(folderRecyclerView);
  rootLayout->addView(nav);

  auto left = new View();
  left->setFlexDirection(FlexDirection::Column);
  left->setAlignItems(YGAlignStretch);
  left->setFlex(1);
  left->setGap(14);
  left->setPadding(Edge::All, 16);
  left->setThemedBackgroundColor(ui_theme::mainMenuPanel);
  left->setCornerRadius(ui_theme::panelRadius());
  left->setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);
  left->setThemedBorderColor(ui_theme::hairline);
  left->setBorderWidth(1);

  auto *libraryHeader = new View();
  libraryHeader->setFlexDirection(FlexDirection::Row);
  libraryHeader->setAlignItems(YGAlignCenter);
  libraryHeader->setGap(12);
  libraryHeader->setHeight(58);

  auto *libraryTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 44);
  libraryTitle->setText("Song Select");
  libraryTitle->setThemedColor(ui_theme::textPrimary);
  libraryTitle->setVAlign(TextView::MIDDLE);
  libraryTitle->setFlex(1);
  libraryHeader->addView(libraryTitle);

  parseLogButton = makeModalButton("Log", 20, &parseLogButtonText);
  parseLogButton->setWidth(112);
  parseLogButton->setHeight(50);
  parseLogButton->setOnClickListener([this]() { showParseLogModal(); });
  styleThemedActionButton(parseLogButton, parseLogButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  libraryHeader->addView(parseLogButton);

  musicButton = makeModalButton("Music", 20, &musicButtonText);
  musicButton->setWidth(122);
  musicButton->setHeight(50);
  musicButton->setOnClickListener([this, &context]() {
    if (context.sceneManager != nullptr) {
      cancelPreviewLoading(true);
      context.sceneManager->changeScene(
          std::make_unique<MusicPlayerScene>(context), true);
    }
  });
  styleThemedActionButton(musicButton, musicButtonText, true, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  libraryHeader->addView(musicButton);

  tasksButton = makeModalButton("0 Tasks", 20, &tasksButtonText);
  tasksButton->setWidth(142);
  tasksButton->setHeight(50);
  tasksButton->setOnClickListener([this]() { showTasksModal(); });
  styleThemedActionButton(tasksButton, tasksButtonText, true, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  libraryHeader->addView(tasksButton);
  left->addView(libraryHeader);

  auto *librarySubtitle = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  librarySubtitle->setText(
      "Search your library and preview charts before starting.");
  librarySubtitle->setThemedColor(ui_theme::textSecondary);
  left->addView(librarySubtitle);

  auto *filterRow = new View();
  filterRow->setFlexDirection(FlexDirection::Row);
  filterRow->setAlignItems(YGAlignStretch);
  filterRow->setGap(10);

  searchBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 30);
  searchBox->setText(searchText);
  searchBox->setHeight(56);
  searchBox->setFlex(1);
  searchBox->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  searchBox->setCornerRadius(ui_theme::controlRadius());
  searchBox->setThemedBorderColor(ui_theme::hairlineSubtle);
  searchBox->setBorderWidth(1);
  searchBox->setVAlign(TextView::MIDDLE);
  searchBox->setThemedColor(ui_theme::textPrimary);
  auto onSearchChanged = [this](const std::string &text) {
    searchText = text;
    reloadChartList();
  };
  searchBox->onTextChanged(onSearchChanged);
  searchBox->onSubmit(onSearchChanged);
  filterRow->addView(searchBox);

  difficultyFilterBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 30);
  difficultyFilterBox->setText(difficultyText);
  difficultyFilterBox->setHeight(56);
  difficultyFilterBox->setWidth(180);
  difficultyFilterBox->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  difficultyFilterBox->setCornerRadius(ui_theme::controlRadius());
  difficultyFilterBox->setThemedBorderColor(ui_theme::hairlineSubtle);
  difficultyFilterBox->setBorderWidth(1);
  difficultyFilterBox->setVAlign(TextView::MIDDLE);
  difficultyFilterBox->setThemedColor(ui_theme::textPrimary);
  auto onDifficultyChanged = [this](const std::string &text) {
    difficultyText = text;
    reloadChartList();
  };
  difficultyFilterBox->onTextChanged(onDifficultyChanged);
  difficultyFilterBox->onSubmit(onDifficultyChanged);
  filterRow->addView(difficultyFilterBox);

  auto *filterLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  filterLabel->setText("Search / Difficulty");
  filterLabel->setThemedColor(ui_theme::textSecondary);
  left->addView(filterLabel);
  left->addView(filterRow);

  recyclerView->setFlex(1);
  recyclerView->clearBackgroundColor();
  recyclerView->setBorderWidth(0);
  recyclerView->setCornerRadius(ui_theme::controlRadius());
  left->addView(recyclerView);
  rootLayout->addView(left);

  auto right = new View();
  right->setFlexDirection(FlexDirection::Column);
  right->setAlignItems(YGAlignCenter);
  right->setPadding(Edge::All, 20);
  right->setGap(12);
  right->setWidth(300);
  right->setThemedBackgroundColor(ui_theme::mainMenuPanel);
  right->setCornerRadius(ui_theme::panelRadius());
  right->setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);
  right->setThemedBorderColor(ui_theme::hairline);
  right->setBorderWidth(1);

  auto *rightTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 34);
  rightTitle->setText("Ready");
  rightTitle->setThemedColor(ui_theme::textPrimary);
  rightTitle->setAlign(TextView::CENTER);
  rightTitle->setHeight(42);
  right->addView(rightTitle);

  auto *rightSubtitle = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  rightSubtitle->setText("Preview, tweak, and start.");
  rightSubtitle->setThemedColor(ui_theme::textSecondary);
  rightSubtitle->setAlign(TextView::CENTER);
  rightSubtitle->setHeight(28);
  right->addView(rightSubtitle);

  const GaugeSelection savedGaugeSelection =
      gaugeSelectionFromSettingId(context.settings.selectedGaugeType);
  selectedGaugeType = savedGaugeSelection.type;
  selectedGaugeAutoShift = savedGaugeSelection.autoShift;
  selectedPlayOption =
      play_options::normalizePlayOption(context.settings.selectedPlayOption);
  selectedLnMode =
      long_note_mode::parseId(context.settings.selectedLnMode,
                              AppSettings::kDefaultLnMode);
  selectedAssistOption =
      assist_options::normalize(context.settings.selectedAssistOption);

  auto *readySettings = new View();
  readySettings->setFlexDirection(FlexDirection::Column);
  readySettings->setAlignItems(YGAlignStretch);
  readySettings->setWidth(220);
  readySettings->setGap(6);

  auto makeReadyStatusText = []() {
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
    text->setHeight(28);
    text->setThemedColor(ui_theme::textPrimary);
    return text;
  };
  auto *readyGaugeRow = new View();
  readyGaugeRow->setFlexDirection(FlexDirection::Row);
  readyGaugeRow->setAlignItems(YGAlignCenter);
  readyGaugeRow->setGap(6);
  readyGaugeRow->setHeight(28);
  auto *readyGaugeLabelText = makeReadyStatusText();
  readyGaugeLabelText->setText("Gauge:");
  readyGaugeLabelText->setThemedColor(ui_theme::textSecondary);
  readyGaugeLabelText->setWidth(70);
  readyGaugeText = makeReadyStatusText();
  readyGaugeText->setFlex(1);
  readyGaugeRow->addView(readyGaugeLabelText);
  readyGaugeRow->addView(readyGaugeText);
  readyPlayOptionText = makeReadyStatusText();
  readyAssistOptionText = makeReadyStatusText();
  readySettings->addView(readyGaugeRow);
  readySettings->addView(readyPlayOptionText);
  readySettings->addView(readyAssistOptionText);

  auto *playOptionsButton = new Button(0, 0, 220, 54);
  auto *playOptionsButtonText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  playOptionsButtonText->setText("Options");
  playOptionsButtonText->setAlign(TextView::CENTER);
  playOptionsButtonText->setVAlign(TextView::MIDDLE);
  playOptionsButton->setContentView(playOptionsButtonText);
  styleThemedActionButton(playOptionsButton, playOptionsButtonText, true,
                          ui_theme::primaryAction, ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  playOptionsButton->setOnClickListener([this]() { showPlayOptionsModal(); });
  readySettings->addView(playOptionsButton);
  right->addView(readySettings);
  refreshReadySettingsSummary();

  startButton = new Button(0, 0, 220, 86);
  auto buttonText = new TextView("assets/fonts/notosanscjkjp.ttf", 32);
  startButtonText = buttonText;
  buttonText->setText("Start");
  buttonText->setAlign(TextView::CENTER);
  buttonText->setVAlign(TextView::MIDDLE);
  startButton->setContentView(buttonText);
  styleThemedActionButton(startButton, buttonText, true,
                          ui_theme::primaryAction, ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  startButton->setOnClickListener([this]() {
    if (willStart.load()) {
      return;
    }
    auto selected = recyclerView->selectedIndex;
    if (activeFolder.type == LibraryFolderItem::Type::Course &&
        (selected < 0 || selected >= recyclerView->size() ||
         recyclerView->get(selected).courseStart)) {
      startSelectedCourse();
      return;
    }
    if (selected >= 0 && selected < recyclerView->size()) {
      const auto &selectedMeta = recyclerView->get(selected);
      if (selectedMeta.solidArchive || selectedMeta.unavailable ||
          selectedMeta.meta.BmsPath.empty()) {
        return;
      }
      startSelectedChart();
    }
  });
  replayButtonSlot = new View();
  replayButtonSlot->setWidth(220)->setHeight(0);
  replayButtonSlot->setVisible(false);
  replayButtonSlot->setAlignItems(YGAlignStretch);

  replayButton = new Button(0, 0, 220, 58);
  replayButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  replayButtonText->setText("Replay");
  replayButtonText->setAlign(TextView::CENTER);
  replayButtonText->setVAlign(TextView::MIDDLE);
  replayButton->setContentView(replayButtonText);
  styleThemedActionButton(replayButton, replayButtonText, true,
                          ui_theme::successAction, ui_theme::successActionHover,
                          ui_theme::successActionPressed,
                          ui_theme::accentBorder);
  replayButton->setOnClickListener([this]() {
    if (willStart.load() || replayExportInProgress.load()) {
      return;
    }
    auto selected = recyclerView->selectedIndex;
    if (selected < 0 || selected >= recyclerView->size()) {
      return;
    }
    const auto &selectedMeta = recyclerView->get(selected);
    const bool courseStartReplay =
        selectedMeta.courseStart &&
        activeFolder.type == LibraryFolderItem::Type::Course &&
        activeFolder.courseId > 0;
    if (selectedMeta.solidArchive || selectedMeta.unavailable ||
        (!courseStartReplay && selectedMeta.meta.BmsPath.empty())) {
      return;
    }

    showReplayListModal(selectedMeta);
  });
  replayButtonSlot->addView(replayButton);

  findBmsButtonSlot = new View();
  findBmsButtonSlot->setWidth(220)->setHeight(0);
  findBmsButtonSlot->setVisible(false);
  findBmsButtonSlot->setAlignItems(YGAlignStretch);

  findBmsButton = new Button(0, 0, 220, 58);
  findBmsButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  findBmsButtonText->setText("Find BMS");
  findBmsButtonText->setAlign(TextView::CENTER);
  findBmsButtonText->setVAlign(TextView::MIDDLE);
  findBmsButton->setContentView(findBmsButtonText);
  styleThemedActionButton(findBmsButton, findBmsButtonText, true,
                          ui_theme::successAction, ui_theme::successActionHover,
                          ui_theme::successActionPressed,
                          ui_theme::accentBorder);
  findBmsButton->setOnClickListener([this]() { openFindBmsForSelection(); });
  findBmsButtonSlot->addView(findBmsButton);

  unzipButtonSlot = new View();
  unzipButtonSlot->setWidth(220)->setHeight(0);
  unzipButtonSlot->setVisible(false);
  unzipButtonSlot->setAlignItems(YGAlignStretch);

  unzipButton = new Button(0, 0, 220, 58);
  unzipButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  unzipButtonText->setText("Unzip");
  unzipButtonText->setAlign(TextView::CENTER);
  unzipButtonText->setVAlign(TextView::MIDDLE);
  unzipButton->setContentView(unzipButtonText);
  styleThemedActionButton(unzipButton, unzipButtonText, true,
                          ui_theme::warningAction, ui_theme::warningActionHover,
                          ui_theme::warningActionPressed,
                          ui_theme::accentBorder);
  unzipButton->setOnClickListener(
      [this]() { startUnzipSelectedArchiveFolder(); });
  unzipButtonSlot->addView(unzipButton);

  replayStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  replayStatusText->setText("");
  replayStatusText->setThemedColor(ui_theme::textSecondary);
  replayStatusText->setAlign(TextView::CENTER);
  replayStatusText->setHeight(20);

  auto *jacketCard = new View();
  jacketCard->setWidth(200);
  jacketCard->setHeight(200);
  jacketCard->setAlignItems(YGAlignCenter);
  jacketCard->setJustifyContent(YGJustifyCenter);
  jacketCard->setThemedBackgroundColor(ui_theme::mainMenuSurface);
  jacketCard->setCornerRadius(ui_theme::panelRadius());
  jacketCard->setThemedBorderColor(ui_theme::hairlineSubtle);
  jacketCard->setBorderWidth(1);
  jacketView->setWidth(198)->setHeight(198);
  jacketView->setCornerRadius(
      ui_theme::childRadiusForInset(ui_theme::panelRadius(), 1.0f, 0.0f));
  jacketCard->addView(jacketView);
  startButton->setHeight(86);
  right->addView(jacketCard);
  right->addView(startButton);

  chartActionsRow = new View();
  chartActionsRow->setFlexDirection(FlexDirection::Row);
  chartActionsRow->setAlignItems(YGAlignStretch);
  chartActionsRow->setWidth(220);
  chartActionsRow->setHeight(58);
  chartActionsRow->setGap(10);

  auto *viewerButton = new Button(0, 0, 105, 58);
  viewerButton->setFlex(1);
  auto *viewerButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  viewerButtonText->setText("Viewer");
  viewerButtonText->setAlign(TextView::CENTER);
  viewerButtonText->setVAlign(TextView::MIDDLE);
  viewerButton->setContentView(viewerButtonText);
  styleThemedActionButton(viewerButton, viewerButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  viewerButton->setOnClickListener([this]() { openChartViewerForSelection(); });
  chartActionsRow->addView(viewerButton);

  auto *revealButton = new Button(0, 0, 105, 58);
  revealButton->setFlex(1);
  auto *revealButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  revealButtonText->setText("Reveal");
  revealButtonText->setAlign(TextView::CENTER);
  revealButtonText->setVAlign(TextView::MIDDLE);
  revealButton->setContentView(revealButtonText);
  styleThemedActionButton(revealButton, revealButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  revealButton->setOnClickListener(
      [this]() { revealSelectedChartInFileManager(); });
  chartActionsRow->addView(revealButton);
  right->addView(chartActionsRow);

  right->addView(replayButtonSlot);
  right->addView(unzipButtonSlot);
  right->addView(findBmsButtonSlot);
  right->addView(replayStatusText);

  auto *settingsSpacer = new View();
  settingsSpacer->setWidth(220);
  settingsSpacer->setFlex(1);
  right->addView(settingsSpacer);

  auto *settingsButton = new Button(0, 0, 220, 64);
  auto *settingsText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  settingsText->setText("Settings");
  settingsText->setAlign(TextView::CENTER);
  settingsText->setVAlign(TextView::MIDDLE);
  settingsButton->setContentView(settingsText);
  styleThemedActionButton(settingsButton, settingsText, true,
                          ui_theme::dangerAction, ui_theme::dangerActionHover,
                          ui_theme::dangerActionPressed,
                          ui_theme::accentBorder);
  settingsButton->setOnClickListener([this, &context]() {
    if (willStart.load() || replayExportInProgress.load()) {
      return;
    }
    cancelPreviewLoading(true);
    context.sceneManager->changeScene("Settings", true);
  });
  right->addView(settingsButton);

  rootLayout->addView(right);
  buildPlayOptionsModal();
  buildReplayModal();
  buildParseLogModal();
  buildTasksModal();
  buildFindBmsModal();
  buildUnzipProgressModal();
  reloadScoreClearRanks();
  reloadFolderItems();
  reloadChartList();
  libraryRevision = ChartDBHelper::GetInstance().GetLibraryRevision();
  rootLayout->applyYogaLayout();
}

void MainMenuScene::reloadFolderItems(bool preserveViewState) {
  if (folderRecyclerView == nullptr) {
    return;
  }

  const float previousScrollOffset =
      preserveViewState ? folderRecyclerView->scrollOffset : 0.0f;
  auto &dbHelper = ChartDBHelper::GetInstance();
  const std::uint64_t currentLibraryRevision = dbHelper.GetLibraryRevision();
  if (!folderMetadataCache.valid ||
      folderMetadataCache.libraryRevision != currentLibraryRevision) {
    folderMetadataCache = LibraryFolderMetadataCache{};
    folderMetadataCache.libraryRevision = currentLibraryRevision;
    folderMetadataCache.allSongCount = dbHelper.CountAllChartMeta(db);
    folderMetadataCache.favoriteCount = dbHelper.CountFavoriteCharts(db);
    folderMetadataCache.solidArchiveCount = dbHelper.CountSolidArchives(db);
    folderMetadataCache.tables = dbHelper.SelectDifficultyTables(db);
    folderMetadataCache.courseTables = dbHelper.SelectDifficultyCourseTables(db);
    folderMetadataCache.valid = true;
  }
  std::vector<LibraryFolderItem> folders;

  const int allSongCount = folderMetadataCache.allSongCount;
  const int favoriteCount = folderMetadataCache.favoriteCount;

  auto isExpanded = [this](const std::string &key) {
    return expandedLibraryFolders.find(key) != expandedLibraryFolders.end();
  };
  auto appendClearMarkFilters = [&](const LibraryFolderItem &parent,
                                    int childDepth) {
    for (const auto &filter : kDifficultyClearMarkFilters) {
      const int count = clearMarkCountForFolder(parent.key, filter.rank);
      if (count <= 0) {
        continue;
      }
      folders.push_back({
          .key = clearMarkFolderKey(parent.key, filter.rank),
          .label = filter.label,
          .type = LibraryFolderItem::Type::DifficultyClearMark,
          .depth = childDepth,
          .count = count,
          .tableId = parent.tableId,
          .tableLevel = parent.tableLevel,
          .clearRank = filter.rank,
          .clearMarkRank = filter.rank,
          .clearMarkFolder = true,
      });
    }
  };

  const LibraryFolderItem allSongsItem{
      .key = "all",
      .label = "All songs",
      .type = LibraryFolderItem::Type::AllSongs,
      .depth = 0,
      .count = allSongCount,
      .expandable = true,
      .expanded = isExpanded("all"),
  };
  folders.push_back(allSongsItem);
  if (allSongsItem.expanded) {
    appendClearMarkFilters(allSongsItem, 1);
  }

  folders.push_back({
      .key = "favorites",
      .label = "Favorites",
      .type = LibraryFolderItem::Type::Favorites,
      .depth = 0,
      .count = favoriteCount,
  });

  const int solidArchiveCount = folderMetadataCache.solidArchiveCount;
  if (solidArchiveCount > 0) {
    folders.push_back({
        .key = "solid-archives",
        .label = "Solid Archive",
        .type = LibraryFolderItem::Type::SolidArchives,
        .depth = 0,
        .count = solidArchiveCount,
    });
  }

  for (const auto &table : folderMetadataCache.tables) {
    const std::string tableKey = folderKeyForTable(table.id);
    const LibraryFolderItem tableItem{
        .key = tableKey,
        .label = table.name,
        .type = LibraryFolderItem::Type::DifficultyTable,
        .depth = 0,
        .count = table.chartCount,
        .tableId = table.id,
        .expandable = true,
        .expanded = isExpanded(tableKey),
    };
    folders.push_back(tableItem);
    if (!tableItem.expanded) {
      continue;
    }

    appendClearMarkFilters(tableItem, 1);

    auto levelsIt = folderMetadataCache.levelsByTable.find(table.id);
    if (levelsIt == folderMetadataCache.levelsByTable.end()) {
      levelsIt =
          folderMetadataCache.levelsByTable
              .emplace(table.id, dbHelper.SelectDifficultyLevels(db, table.id))
              .first;
    }
    const auto &levels = levelsIt->second;
    for (const auto &level : levels) {
      const std::string levelKey =
          folderKeyForLevel(level.tableId, level.level);
      const LibraryFolderItem levelItem{
          .key = levelKey,
          .label = level.tableSymbol + level.level,
          .type = LibraryFolderItem::Type::DifficultyLevel,
          .depth = 1,
          .count = level.chartCount,
          .tableId = level.tableId,
          .tableLevel = level.level,
          .expandable = true,
          .expanded = isExpanded(levelKey),
      };
      folders.push_back(levelItem);
      if (levelItem.expanded) {
        appendClearMarkFilters(levelItem, 2);
      }
    }
  }

  const auto &courseTables = folderMetadataCache.courseTables;
  if (!courseTables.empty()) {
    int coursesCount = 0;
    for (const auto &table : courseTables) {
      coursesCount += table.chartCount;
    }
    const LibraryFolderItem coursesRootItem{
        .key = "courses",
        .label = "Courses",
        .type = LibraryFolderItem::Type::CoursesRoot,
        .depth = 0,
        .count = coursesCount,
        .expandable = true,
        .expanded = isExpanded("courses"),
    };
    folders.push_back(coursesRootItem);
    if (coursesRootItem.expanded) {
      const auto makeCourseItem =
          [](int courseId, int tableId, const std::string &groupName,
             const std::string &level, const std::string &name,
             const std::string &constraintJson, int depth) {
        const std::string courseLabel = level.empty() ? name : level;
        return LibraryFolderItem{
            .key = folderKeyForCourse(courseId),
            .label = courseLabel,
            .type = LibraryFolderItem::Type::Course,
            .depth = depth,
            .count = -1,
            .courseId = courseId,
            .courseTableId = tableId,
            .courseGroupName = groupName,
            .courseConstraintJson = constraintJson,
        };
      };
      const auto makeCourseInfoItem = [&](const DifficultyCourseInfo &course,
                                          int depth) {
        return makeCourseItem(course.id, course.tableId, course.groupName,
                              course.level, course.name, course.constraintJson,
                              depth);
      };

      for (const auto &table : courseTables) {
        const std::string tableKey = folderKeyForCourseTable(table.tableId);
        const LibraryFolderItem tableItem{
            .key = tableKey,
            .label = table.tableName,
            .type = LibraryFolderItem::Type::CourseTable,
            .depth = 1,
            .count = table.chartCount,
            .courseTableId = table.tableId,
            .expandable = true,
            .expanded = isExpanded(tableKey),
        };
        folders.push_back(tableItem);
        if (!tableItem.expanded) {
          continue;
        }

        auto groupsIt =
            folderMetadataCache.courseGroupsByTable.find(table.tableId);
        if (groupsIt == folderMetadataCache.courseGroupsByTable.end()) {
          groupsIt = folderMetadataCache.courseGroupsByTable
                         .emplace(table.tableId,
                                  dbHelper.SelectDifficultyCourseGroups(
                                      db, table.tableId))
                         .first;
        }
        for (const auto &group : groupsIt->second) {
          const std::string label =
              group.groupName.empty() ? "Ungrouped" : group.groupName;
          const std::string groupKey =
              folderKeyForCourseGroup(group.tableId, group.groupName);
          const bool duplicateSingletonGroup =
              group.courseCount == 1 && group.singletonCourseId > 0 &&
              (group.groupName.empty() || group.singletonCourseName == label ||
               group.singletonCourseLevel == label);
          if (duplicateSingletonGroup) {
            folders.push_back(makeCourseItem(
                group.singletonCourseId, group.tableId, group.groupName,
                group.singletonCourseLevel, group.singletonCourseName,
                group.singletonCourseConstraintJson, 2));
            continue;
          }

          const LibraryFolderItem groupItem{
              .key = groupKey,
              .label = label,
              .type = LibraryFolderItem::Type::CourseGroup,
              .depth = 2,
              .count = group.chartCount,
              .courseTableId = group.tableId,
              .courseGroupName = group.groupName,
              .expandable = true,
              .expanded = isExpanded(groupKey),
          };
          folders.push_back(groupItem);
          if (!groupItem.expanded) {
            continue;
          }

          auto coursesIt = folderMetadataCache.coursesByGroup.find(groupKey);
          if (coursesIt == folderMetadataCache.coursesByGroup.end()) {
            coursesIt = folderMetadataCache.coursesByGroup
                            .emplace(groupKey,
                                     dbHelper.SelectDifficultyCourses(
                                         db, group.tableId, group.groupName))
                            .first;
          }
          for (const auto &course : coursesIt->second) {
            folders.push_back(makeCourseInfoItem(course, 3));
          }
        }
      }
    }
  }

  for (auto &folder : folders) {
    folder.clearRank =
        folder.clearMarkFolder ? folder.clearMarkRank
                               : clearRankForFolder(folder.key);
  }

  if (activeFolder.key.empty()) {
    activeFolder = folders.front();
  }

  bool activeStillExists = false;
  int activeIndex = 0;
  for (int i = 0; i < folders.size(); i++) {
    const auto &folder = folders[i];
    if (folder.key == activeFolder.key) {
      activeFolder = folder;
      activeStillExists = true;
      activeIndex = i;
      break;
    }
  }
  if (!activeStillExists) {
    activeFolder = folders.front();
    activeIndex = 0;
  }

  const int folderCount = static_cast<int>(folders.size());
  folderRecyclerView->setItems(std::move(folders));
  folderRecyclerView->selectedIndex = activeIndex;
  if (preserveViewState) {
    const float maxOffset =
        std::max(0.0f, static_cast<float>(std::max(1, folderCount) *
                                              folderRecyclerView->itemHeight -
                                          folderRecyclerView->getHeight()));
    folderRecyclerView->scrollOffset =
        std::clamp(previousScrollOffset, 0.0f, maxOffset);
    folderRecyclerView->rebindVisibleItems();
  }
  auto selectedView = folderRecyclerView->getViewByIndex(activeIndex);
  if (selectedView != nullptr) {
    selectedView->onSelected();
  }
}

void MainMenuScene::refreshFavoriteFolderCount() {
  if (folderRecyclerView == nullptr) {
    return;
  }

  std::vector<LibraryFolderItem> folders = folderRecyclerView->getItems();
  if (folders.empty()) {
    reloadFolderItems(true);
    return;
  }

  auto &dbHelper = ChartDBHelper::GetInstance();
  const int favoriteCount = dbHelper.CountFavoriteCharts(db);
  if (folderMetadataCache.valid) {
    folderMetadataCache.favoriteCount = favoriteCount;
    folderMetadataCache.libraryRevision = dbHelper.GetLibraryRevision();
  }
  const float previousScrollOffset = folderRecyclerView->scrollOffset;
  int activeIndex = std::clamp(folderRecyclerView->selectedIndex, 0,
                               static_cast<int>(folders.size()) - 1);
  bool foundFavorites = false;

  for (int i = 0; i < static_cast<int>(folders.size()); ++i) {
    auto &folder = folders[static_cast<std::size_t>(i)];
    if (folder.key == "favorites") {
      folder.count = favoriteCount;
      foundFavorites = true;
      if (activeFolder.key == folder.key) {
        activeFolder = folder;
      }
    }
    if (folder.key == activeFolder.key) {
      activeIndex = i;
    }
  }

  if (!foundFavorites) {
    reloadFolderItems(true);
    return;
  }

  const int folderCount = static_cast<int>(folders.size());
  folderRecyclerView->setItems(std::move(folders));
  folderRecyclerView->selectedIndex = activeIndex;
  const float maxOffset =
      std::max(0.0f, static_cast<float>(std::max(1, folderCount) *
                                            folderRecyclerView->itemHeight -
                                        folderRecyclerView->getHeight()));
  folderRecyclerView->scrollOffset =
      std::clamp(previousScrollOffset, 0.0f, maxOffset);
  folderRecyclerView->rebindVisibleItems();
  auto selectedView = folderRecyclerView->getViewByIndex(activeIndex);
  if (selectedView != nullptr) {
    selectedView->onSelected();
  }
}

ChartMetaQuery MainMenuScene::chartQueryForActiveFolder() const {
  ChartMetaQuery query;
  query.keyword = searchText;
  query.difficultyText = difficultyText;
  query.selectedLongNoteMode = long_note_mode::valueFromId(selectedLnMode);

  switch (activeFolder.type) {
  case LibraryFolderItem::Type::SolidArchives:
    query.solidArchivesOnly = true;
    break;
  case LibraryFolderItem::Type::Favorites:
    query.favoritesOnly = true;
    break;
  case LibraryFolderItem::Type::DifficultyTable:
    query.tableId = activeFolder.tableId;
    break;
  case LibraryFolderItem::Type::DifficultyLevel:
    query.tableId = activeFolder.tableId;
    query.tableLevel = activeFolder.tableLevel;
    break;
  case LibraryFolderItem::Type::DifficultyClearMark:
    query.tableId = activeFolder.tableId;
    query.tableLevel = activeFolder.tableLevel;
    query.clearMarkFilter = true;
    query.clearMarkRank = activeFolder.clearMarkRank;
    break;
  case LibraryFolderItem::Type::CoursesRoot:
    query.coursesOnly = true;
    break;
  case LibraryFolderItem::Type::CourseTable:
    query.courseTableId = activeFolder.courseTableId;
    break;
  case LibraryFolderItem::Type::CourseGroup:
    query.courseTableId = activeFolder.courseTableId;
    query.courseGroupName = activeFolder.courseGroupName;
    break;
  case LibraryFolderItem::Type::Course:
    query.courseId = activeFolder.courseId;
    break;
  case LibraryFolderItem::Type::AllSongs:
  default:
    break;
  }
  return query;
}

void MainMenuScene::reloadChartList(bool preserveViewState) {
  if (recyclerView == nullptr) {
    return;
  }

  const float previousScrollOffset =
      preserveViewState ? recyclerView->scrollOffset : 0.0f;
  const int previousSelectedIndex =
      preserveViewState ? recyclerView->selectedIndex : -1;
  path_t previousSelectedPath;
  if (preserveViewState && previousSelectedIndex >= 0 &&
      previousSelectedIndex < recyclerView->size()) {
    previousSelectedPath =
        fspath_to_path_t(recyclerView->get(previousSelectedIndex).meta.BmsPath);
  }
  int previousTopIndex = -1;
  float previousTopItemOffset = 0.0f;
  path_t previousTopPath;
  if (preserveViewState && recyclerView->itemHeight > 0 &&
      recyclerView->size() > 0) {
    previousTopIndex = std::clamp(
        static_cast<int>(previousScrollOffset /
                         static_cast<float>(recyclerView->itemHeight)),
        0, recyclerView->size() - 1);
    previousTopItemOffset =
        previousScrollOffset -
        static_cast<float>(previousTopIndex * recyclerView->itemHeight);
    previousTopPath =
        fspath_to_path_t(recyclerView->get(previousTopIndex).meta.BmsPath);
  }

  ChartMetaQuery query = chartQueryForActiveFolder();

  std::optional<ChartMetaRecord> leadingRecord;
  if (activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0) {
    ChartMetaRecord courseRecord;
    courseRecord.courseStart = true;
    courseRecord.meta.Title = activeFolder.label.empty() ? "Course"
                                                         : activeFolder.label;
    courseRecord.meta.Artist = activeFolder.courseGroupName.empty()
                                   ? "Course Mode"
                                   : activeFolder.courseGroupName;
    courseRecord.difficultyTableLabels =
        activeFolder.label.empty() ? "Course" : activeFolder.label;
    leadingRecord = std::move(courseRecord);
  }

  int dbCount = 0;
  if (!preserveViewState || previousSelectedPath.empty()) {
    refreshReplayAvailability(nullptr);
    refreshPlayOptionButtons();
    refreshLongNoteModeButtons();
    refreshAssistOptionButtons();
  }
  dbCount = ChartDBHelper::GetInstance().CountChartMeta(db, query);
  const int count = dbCount + (leadingRecord.has_value() ? 1 : 0);
  const int leadingOffset = leadingRecord.has_value() ? 1 : 0;
  chartListCache.reset(db, query, dbCount, std::move(leadingRecord));
  recyclerView->setItemProvider(
      count, [this](int index) -> const ChartMetaRecord & {
        return chartListCache.get(index);
      });
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshStartButtonForActiveFolder();
  if (!preserveViewState && activeFolder.type == LibraryFolderItem::Type::Course &&
      count > 0) {
    recyclerView->selectedIndex = 0;
    if (recyclerView->onSelected) {
      recyclerView->onSelected(recyclerView->get(0), 0);
    }
  }
  if (!preserveViewState) {
    return;
  }

  const float maxOffset = std::max(
      0.0f, static_cast<float>(std::max(1, count) * recyclerView->itemHeight -
                               recyclerView->getHeight()));
  auto pathMatches = [&](int index, const path_t &path) {
    if (index < 0 || index >= count || path.empty()) {
      return false;
    }
    return fspath_to_path_t(recyclerView->get(index).meta.BmsPath) == path;
  };
  auto findPathNear = [&](const path_t &path, int preferredIndex) {
    if (path.empty() || count <= 0) {
      return -1;
    }
    if (pathMatches(preferredIndex, path)) {
      return preferredIndex;
    }
    const int dbIndex = ChartDBHelper::GetInstance().FindChartMetaIndex(
        db, query, std::filesystem::path(path));
    if (dbIndex < 0) {
      return -1;
    }
    const int index = dbIndex + leadingOffset;
    return index >= 0 && index < count ? index : -1;
  };

  float restoredScrollOffset = std::clamp(previousScrollOffset, 0.0f, maxOffset);
  const int restoredTopIndex = findPathNear(previousTopPath, previousTopIndex);
  if (restoredTopIndex >= 0) {
    restoredScrollOffset = std::clamp(
        static_cast<float>(restoredTopIndex * recyclerView->itemHeight) +
            previousTopItemOffset,
        0.0f, maxOffset);
  }
  recyclerView->scrollOffset = restoredScrollOffset;

  const int restoredSelectedIndex =
      findPathNear(previousSelectedPath, previousSelectedIndex);

  recyclerView->selectedIndex = restoredSelectedIndex;
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  if (restoredSelectedIndex < 0 && !previousSelectedPath.empty()) {
    refreshReplayAvailability(nullptr);
    setPlayableChartActionsVisible(false);
    setUnzipButtonVisible(false);
    setFindBmsButtonVisible(false);
  }
  recyclerView->rebindVisibleItems();
}

void MainMenuScene::reloadScoreClearRanks() {
  scoreClearRanks = ScoreDBHelper::GetInstance().LoadBestClearRanks();
  scoreClearRanksRevision = ScoreDBHelper::GetInstance().GetRevision();
  rebuildScoreClearRankTempTable();
  folderClearData = main_menu_library::LoadFolderClearDataByLongNoteMode(
      db, scoreClearRanks);
}

void MainMenuScene::rebuildScoreClearRankTempTable() {
  if (db == nullptr) {
    return;
  }

  auto exec = [this](const char *query, const char *context) {
    if (const auto error = executeSqlite(db, query)) {
      SDL_Log("SQL error while %s: %s", context, error->c_str());
      return false;
    }
    return true;
  };

  if (!exec("DROP TABLE IF EXISTS temp.score_clear_rank_cache",
            "dropping score clear rank cache") ||
      !exec("CREATE TEMP TABLE score_clear_rank_cache ("
            "kind INTEGER NOT NULL,"
            "key TEXT NOT NULL,"
            "ln_mode INTEGER NOT NULL,"
            "rank INTEGER NOT NULL,"
            "PRIMARY KEY(kind, key, ln_mode)"
            ") WITHOUT ROWID",
            "creating score clear rank cache") ||
      !exec("BEGIN", "starting score clear rank cache rebuild")) {
    return;
  }

  SqliteStatementHandle stmt;
  const int rc = prepareSqliteStatement(
      db,
      "INSERT INTO temp.score_clear_rank_cache(kind, key, ln_mode, rank) "
      "VALUES (?, ?, ?, ?)",
      stmt);
  if (rc != SQLITE_OK) {
    SDL_Log("SQL error while preparing score clear rank cache insert: %s",
            sqlite3_errmsg(db));
    exec("ROLLBACK", "rolling back score clear rank cache rebuild");
    return;
  }

  auto insertRanks = [&](int kind, const ScoreRankMap &ranks) {
    for (const auto &[key, rankByMode] : ranks) {
      if (key.empty()) {
        continue;
      }
      for (int lnMode = 0; lnMode < static_cast<int>(rankByMode.ranks.size());
           ++lnMode) {
        int rank = rankByMode.ranks[static_cast<size_t>(lnMode)];
        if (lnMode == 1 && rank == kNoClearTypeRank &&
            rankByMode.ranks[0] != kNoClearTypeRank) {
          rank = rankByMode.ranks[0];
        }
        if (rank < kNoClearTypeRank) {
          continue;
        }
        sqlite3_reset(stmt.get());
        sqlite3_clear_bindings(stmt.get());
        sqlite3_bind_int(stmt.get(), 1, kind);
        bindSqliteText(stmt.get(), 2, key);
        sqlite3_bind_int(stmt.get(), 3, lnMode);
        sqlite3_bind_int(stmt.get(), 4, rank);
        if (sqlite3_step(stmt.get()) != SQLITE_DONE) {
          SDL_Log("SQL error while inserting score clear rank cache row: %s",
                  sqlite3_errmsg(db));
          return false;
        }
      }
    }
    return true;
  };

  const bool ok = insertRanks(0, scoreClearRanks.rankBySha256) &&
                  insertRanks(1, scoreClearRanks.rankByMd5) &&
                  insertRanks(2, scoreClearRanks.rankByPath);
  exec(ok ? "COMMIT" : "ROLLBACK",
       ok ? "committing score clear rank cache rebuild"
          : "rolling back score clear rank cache rebuild");
}

void MainMenuScene::refreshScoreClearRankViews() {
  reloadScoreClearRanks();
  refreshLongNoteModeClearRankViews();
}

void MainMenuScene::refreshLongNoteModeClearRankViews() {
  if (folderRecyclerView != nullptr) {
    reloadFolderItems(true);
  }
  if (recyclerView != nullptr) {
    if (activeFolder.type == LibraryFolderItem::Type::DifficultyClearMark) {
      reloadChartList(true);
    } else {
      recyclerView->rebindVisibleItems();
    }
  }
}

void MainMenuScene::refreshScoreClearRanksIfNeeded() {
  const std::uint64_t revision = ScoreDBHelper::GetInstance().GetRevision();
  if (scoreClearRanksRevision == 0 || revision == scoreClearRanksRevision) {
    return;
  }

  refreshScoreClearRankViews();
}

void MainMenuScene::refreshLibraryIfNeeded() {
  const std::uint64_t revision =
      ChartDBHelper::GetInstance().GetLibraryRevision();
  if (libraryRevision == 0) {
    libraryRevision = revision;
    return;
  }
  if (revision == libraryRevision) {
    return;
  }

  reloadScoreClearRanks();
  reloadFolderItems(true);
  reloadChartList(true);
  libraryRevision = revision;
}

int MainMenuScene::clearRankForChart(const ChartMetaRecord &record) const {
  if (record.courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course) {
    return clearRankForFolder(activeFolder.key);
  }
  if (record.solidArchive) {
    return kNoClearTypeRank;
  }
  return scoreClearRanks.bestRankFor(
      record.meta, long_note_mode::valueFromId(selectedLnMode));
}

int MainMenuScene::clearRankForFolder(const std::string &key) const {
  const int mode = long_note_mode::valueFromId(selectedLnMode);
  const auto &clearRanks = folderClearData.clearRanks[static_cast<size_t>(mode)];
  const auto it = clearRanks.find(key);
  return it == clearRanks.end() ? kNoClearTypeRank : it->second;
}

int MainMenuScene::clearMarkCountForFolder(const std::string &key,
                                           int clearMarkRank) const {
  const int mode = long_note_mode::valueFromId(selectedLnMode);
  const auto &clearMarkCounts =
      folderClearData.clearMarkCounts[static_cast<size_t>(mode)];
  const auto folderIt = clearMarkCounts.find(key);
  if (folderIt == clearMarkCounts.end()) {
    return 0;
  }
  const auto countIt = folderIt->second.find(clearMarkRank);
  return countIt == folderIt->second.end() ? 0 : countIt->second;
}

void MainMenuScene::requestLibraryReload(bool includeFolders) {
  if (includeFolders) {
    folderItemsReloadRequested = true;
  }
  chartListReloadRequested = true;
}

void MainMenuScene::applyPendingUiUpdates() {
  const bool shouldOpenTasksModal = tasksModalOpenRequested.exchange(false);
  const bool shouldReloadFolders = folderItemsReloadRequested.exchange(false);
  const bool shouldReloadCharts = chartListReloadRequested.exchange(false);
  if (shouldOpenTasksModal) {
    showTasksModal();
  }
  if (shouldReloadFolders) {
    reloadScoreClearRanks();
    reloadFolderItems(true);
  }
  if (shouldReloadFolders || shouldReloadCharts) {
    reloadChartList(true);
    libraryRevision = ChartDBHelper::GetInstance().GetLibraryRevision();
  }
  if ((shouldReloadFolders || shouldReloadCharts) &&
      pendingSelectChartPath.has_value()) {
    const std::filesystem::path path = *pendingSelectChartPath;
    pendingSelectChartPath.reset();
    selectChartByPathAfterReload(path);
  }
}

void MainMenuScene::selectChartByPathAfterReload(
    const std::filesystem::path &path) {
  if (recyclerView == nullptr || path.empty()) {
    return;
  }
  const path_t target = fspath_to_path_t(path);
  const ChartMetaQuery query = chartQueryForActiveFolder();
  int index = ChartDBHelper::GetInstance().FindChartMetaIndex(db, query, path);
  if (index >= 0 && activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0) {
    index += 1;
  }
  if (index >= 0 && index < recyclerView->size()) {
    const ChartMetaRecord &record = recyclerView->get(index);
    if (fspath_to_path_t(record.meta.BmsPath) == target) {
      const int previous = recyclerView->selectedIndex;
      if (previous >= 0 && previous < recyclerView->size() &&
          previous != index && recyclerView->onUnselected) {
        recyclerView->onUnselected(recyclerView->get(previous), previous);
      }
      recyclerView->selectedIndex = index;
      const float selectedY =
          static_cast<float>(index * recyclerView->itemHeight);
      const float viewportHeight =
          static_cast<float>(recyclerView->getHeight());
      const float itemHeight = static_cast<float>(recyclerView->itemHeight);
      const float centeredOffset =
          selectedY - std::max(0.0f, viewportHeight - itemHeight) / 2.0f;
      const float maxOffset =
          std::max(0.0f, static_cast<float>(std::max(1, recyclerView->size()) *
                                                recyclerView->itemHeight -
                                            recyclerView->getHeight()));
      recyclerView->scrollOffset = std::clamp(centeredOffset, 0.0f, maxOffset);
      recyclerView->rebindVisibleItems();
      suppressPreviewForChartPath = record.meta.BmsPath;
      if (recyclerView->onSelected) {
        recyclerView->onSelected(record, index);
      }
      archive_file::appendDebugLogLine(
          "Selected unzipped chart: " + fspath_to_utf8(record.meta.BmsPath));
      return;
    }
  }

  if (activeFolder.type != LibraryFolderItem::Type::AllSongs) {
    activeFolder = {
        .key = "all",
        .label = "All songs",
        .type = LibraryFolderItem::Type::AllSongs,
    };
    reloadFolderItems();
    reloadChartList();
    selectChartByPathAfterReload(path);
  }
}

void MainMenuScene::selectFolder(const LibraryFolderItem &item) {
  const bool chartQueryUnchanged = activeFolder.key == item.key;
  activeFolder = item;
  if (item.expandable) {
    const auto it = expandedLibraryFolders.find(item.key);
    if (it == expandedLibraryFolders.end()) {
      expandedLibraryFolders.insert(item.key);
    } else {
      expandedLibraryFolders.erase(it);
    }
    reloadFolderItems(true);
  }
  if (item.expandable && chartQueryUnchanged) {
    return;
  }
  reloadChartList();
}

bool MainMenuScene::toggleChartFavorite(const ChartMetaRecord &record,
                                        bool favorite) {
  if (record.solidArchive || record.unavailable || record.meta.BmsPath.empty()) {
    return false;
  }

  auto &dbHelper = ChartDBHelper::GetInstance();
  if (!dbHelper.SetFavorite(db, record.meta, favorite)) {
    return false;
  }

  refreshFavoriteFolderCount();
  if (activeFolder.type == LibraryFolderItem::Type::Favorites && !favorite) {
    reloadChartList(true);
  } else if (recyclerView != nullptr) {
    chartListCache.clear();
    recyclerView->rebindVisibleItems();
  }
  libraryRevision = dbHelper.GetLibraryRevision();
  return true;
}

std::optional<ChartMetaRecord> MainMenuScene::selectedRecordSnapshot() const {
  if (recyclerView == nullptr || recyclerView->selectedIndex < 0 ||
      recyclerView->selectedIndex >= recyclerView->size()) {
    return std::nullopt;
  }
  return recyclerView->get(recyclerView->selectedIndex);
}

MainMenuScene::EffectivePlayOptionSelection
MainMenuScene::currentEffectivePlayOptionSelection() const {
  EffectivePlayOptionSelection selection;
  selection.playOption = play_options::normalizePlayOption(selectedPlayOption);
  selection.longNoteMode =
      long_note_mode::parseId(selectedLnMode, AppSettings::kDefaultLnMode);
  selection.assistOption = assist_options::normalize(selectedAssistOption);

  const auto record = selectedRecordSnapshot();
  const bool selectedCourseStart =
      record.has_value() && record->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0;
  if (selectedCourseStart) {
    const CourseConstraintSettings constraintSettings =
        courseConstraintSettingsFromJson(activeFolder.courseConstraintJson);
    if (coursePlayOptionLocksSelection(constraintSettings)) {
      selection.playOption =
          coursePlayOptionForConstraints(selectedPlayOption, constraintSettings);
    }
    if (constraintSettings.rules.longNoteMode !=
        CourseLongNoteMode::Unspecified) {
      selection.longNoteMode = longNoteModeOptionFromCourseConstraint(
          constraintSettings.rules.longNoteMode);
      selection.longNoteModeLocked = true;
    }
    selection.assistOption = assist_options::kOff;
    selection.assistOptionLocked = true;
    return selection;
  }

  if (record.has_value()) {
    const int chartLnMode =
        normalizeChartLongNoteModeValue(record->meta.LnMode);
    if (chartLnMode > 0) {
      selection.longNoteMode =
          long_note_mode::idFromValue(chartLnMode, AppSettings::kDefaultLnMode);
      selection.longNoteModeLocked = true;
    }
  }

  return selection;
}

bool MainMenuScene::currentPlayOptionSelectionAllowed(
    const std::string &option) const {
  const auto record = selectedRecordSnapshot();
  const bool selectedCourseStart =
      record.has_value() && record->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0;
  if (!selectedCourseStart) {
    return true;
  }

  const CourseConstraintSettings constraintSettings =
      courseConstraintSettingsFromJson(activeFolder.courseConstraintJson);
  return coursePlayOptionAllowedByConstraints(option, constraintSettings);
}

bool MainMenuScene::currentLongNoteModeSelectionAllowed(
    const std::string &mode) const {
  const EffectivePlayOptionSelection selection =
      currentEffectivePlayOptionSelection();
  if (!selection.longNoteModeLocked) {
    return true;
  }
  return long_note_mode::parseId(mode, AppSettings::kDefaultLnMode) ==
         selection.longNoteMode;
}

bool MainMenuScene::currentAssistOptionSelectionAllowed(
    const std::string &option) const {
  const EffectivePlayOptionSelection selection =
      currentEffectivePlayOptionSelection();
  if (!selection.assistOptionLocked) {
    return true;
  }
  return assist_options::normalize(option) == selection.assistOption;
}

void MainMenuScene::setGaugeSelection(GaugeType gaugeType, bool autoShift) {
  selectedGaugeType = gaugeType;
  selectedGaugeAutoShift = autoShift;
  context.settings.selectedGaugeType = gaugeSettingId(gaugeType, autoShift);
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save gauge selection");
  }
  refreshGaugeSelectionButtons();
}

void MainMenuScene::refreshGaugeSelectionButtons() {
  for (auto &item : gaugeSelectionButtons) {
    if (item.button == nullptr || item.text == nullptr) {
      continue;
    }

    const bool selected = item.autoShift == selectedGaugeAutoShift &&
                          (item.autoShift || item.type == selectedGaugeType);
    if (selected) {
      const Color accent =
          item.autoShift
              ? Color(255, 205, 37, 242)
              : clearLampColorForRank(clearRankForGaugeType(item.type));
      item.button->setBackgroundColors(
          accent, accent, Color(accent.r, accent.g, accent.b, 255));
      item.button->setBorderColors(Color(255, 255, 255, 220),
                                   Color(255, 255, 255, 240),
                                   Color(255, 255, 255, 255));
      item.text->setColor(ui_theme::sdl(ui_theme::textOn(accent)));
    } else {
      item.button->setThemedBackgroundColors(
          ui_theme::control, ui_theme::controlHover, ui_theme::controlPressed);
      item.button->setThemedBorderColors(ui_theme::hairlineSubtle,
                                         ui_theme::hairlineStrong,
                                         ui_theme::accentBorder);
      item.text->setThemedColor(ui_theme::textPrimary);
    }
  }
  refreshReadySettingsSummary();
}

void MainMenuScene::setPlayOptionSelection(const std::string &option) {
  if (!currentPlayOptionSelectionAllowed(option)) {
    return;
  }
  selectedPlayOption = play_options::normalizePlayOption(option);
  context.settings.selectedPlayOption = selectedPlayOption;
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save play option selection");
  }
  refreshPlayOptionButtons();
}

void MainMenuScene::refreshPlayOptionButtons() {
  const EffectivePlayOptionSelection effective =
      currentEffectivePlayOptionSelection();
  for (auto &item : playOptionButtons) {
    if (item.button == nullptr || item.text == nullptr) {
      continue;
    }

    const std::string normalized =
        play_options::normalizePlayOption(item.option);
    const bool allowed = currentPlayOptionSelectionAllowed(item.option);
    item.text->setText(item.option);
    if (allowed) {
      item.button->setOnClickListener(
          [this, option = item.option]() { setPlayOptionSelection(option); });
    } else {
      item.button->setOnClickListener(std::function<void()>{});
    }
    const bool selected = normalized == effective.playOption;
    if (!allowed) {
      styleLockedOptionButton(item.button, item.text, selected);
    } else {
      styleOptionButton(item.button, item.text, selected);
    }
  }
  refreshReadySettingsSummary();
}

void MainMenuScene::setLongNoteModeSelection(const std::string &mode) {
  if (!currentLongNoteModeSelectionAllowed(mode)) {
    return;
  }
  const std::string previousMode = selectedLnMode;
  selectedLnMode = long_note_mode::parseId(mode, AppSettings::kDefaultLnMode);
  context.settings.selectedLnMode = selectedLnMode;
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save long note mode selection");
  }
  refreshLongNoteModeButtons();
  if (selectedLnMode != previousMode) {
    refreshLongNoteModeClearRankViews();
  }
}

void MainMenuScene::refreshLongNoteModeButtons() {
  const EffectivePlayOptionSelection effective =
      currentEffectivePlayOptionSelection();
  for (auto &item : longNoteModeButtons) {
    if (item.button == nullptr || item.text == nullptr) {
      continue;
    }

    const std::string normalized =
        long_note_mode::parseId(item.mode, AppSettings::kDefaultLnMode);
    const bool allowed = currentLongNoteModeSelectionAllowed(item.mode);
    item.text->setText(item.mode);
    if (allowed && !effective.longNoteModeLocked) {
      item.button->setOnClickListener(
          [this, mode = item.mode]() { setLongNoteModeSelection(mode); });
    } else {
      item.button->setOnClickListener(std::function<void()>{});
    }
    const bool selected = normalized == effective.longNoteMode;
    if (effective.longNoteModeLocked || !allowed) {
      styleLockedOptionButton(item.button, item.text, selected);
    } else {
      styleOptionButton(item.button, item.text, selected);
    }
  }
  refreshReadySettingsSummary();
}

void MainMenuScene::setAssistOptionSelection(const std::string &option) {
  if (!currentAssistOptionSelectionAllowed(option)) {
    return;
  }
  selectedAssistOption = assist_options::normalize(option);
  context.settings.selectedAssistOption = selectedAssistOption;
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save assist option selection");
  }
  refreshAssistOptionButtons();
}

void MainMenuScene::refreshAssistOptionButtons() {
  const EffectivePlayOptionSelection effective =
      currentEffectivePlayOptionSelection();
  for (auto &item : assistOptionButtons) {
    if (item.button == nullptr || item.text == nullptr) {
      continue;
    }

    const bool allowed = currentAssistOptionSelectionAllowed(item.option);
    item.text->setText(item.option);
    if (allowed && !effective.assistOptionLocked) {
      item.button->setOnClickListener(
          [this, option = item.option]() { setAssistOptionSelection(option); });
    } else {
      item.button->setOnClickListener(std::function<void()>{});
    }

    const bool selected =
        assist_options::normalize(item.option) == effective.assistOption;
    if (effective.assistOptionLocked || !allowed) {
      styleLockedOptionButton(item.button, item.text, selected);
    } else {
      styleOptionButton(item.button, item.text, selected);
    }
  }
  refreshReadySettingsSummary();
}

void MainMenuScene::refreshReadySettingsSummary() {
  const EffectivePlayOptionSelection effective =
      currentEffectivePlayOptionSelection();
  if (readyGaugeText != nullptr) {
    readyGaugeText->setText(
        gaugeButtonLabel(selectedGaugeType, selectedGaugeAutoShift));
    readyGaugeText->setColor(
        readyGaugeTextColor(selectedGaugeType, selectedGaugeAutoShift));
  }
  if (readyPlayOptionText != nullptr) {
    readyPlayOptionText->setText("Option: " + effective.playOption + " / " +
                                 effective.longNoteMode);
  }
  if (readyAssistOptionText != nullptr) {
    readyAssistOptionText->setText("Assist: " + effective.assistOption);
  }
}

const MainMenuScene::CourseValidationCache &
MainMenuScene::courseValidationForActiveFolder() {
  const std::uint64_t currentLibraryRevision =
      ChartDBHelper::GetInstance().GetLibraryRevision();
  if (courseValidationCache.valid &&
      courseValidationCache.libraryRevision == currentLibraryRevision &&
      courseValidationCache.courseId == activeFolder.courseId) {
    return courseValidationCache;
  }

  courseValidationCache = CourseValidationCache{};
  courseValidationCache.valid = true;
  courseValidationCache.libraryRevision = currentLibraryRevision;
  courseValidationCache.courseId = activeFolder.courseId;

  if (activeFolder.type != LibraryFolderItem::Type::Course ||
      activeFolder.courseId <= 0) {
    return courseValidationCache;
  }

  ChartMetaQuery query;
  query.courseId = activeFolder.courseId;
  ChartDBHelper::GetInstance().QueryChartMeta(db, query,
                                              courseValidationCache.records);
  courseValidationCache.empty = courseValidationCache.records.empty();
  for (int i = 0;
       i < static_cast<int>(courseValidationCache.records.size()); ++i) {
    const auto &record =
        courseValidationCache.records[static_cast<std::size_t>(i)];
    if (record.solidArchive || record.unavailable ||
        record.meta.BmsPath.empty()) {
      courseValidationCache.firstMissingIndex = i;
      break;
    }
  }
  return courseValidationCache;
}

void MainMenuScene::refreshStartButtonForActiveFolder() {
  if (startButtonText == nullptr || willStart.load()) {
    return;
  }
  if (activeFolder.type != LibraryFolderItem::Type::Course ||
      activeFolder.courseId <= 0) {
    startButtonText->setText("Start");
    return;
  }
  if (recyclerView != nullptr && recyclerView->selectedIndex >= 0 &&
      recyclerView->selectedIndex < recyclerView->size()) {
    const auto &selectedRecord = recyclerView->get(recyclerView->selectedIndex);
    if (!selectedRecord.courseStart) {
      startButtonText->setText("Start");
      return;
    }
  }

  const CourseValidationCache &validation = courseValidationForActiveFolder();
  if (validation.empty) {
    startButtonText->setText("No Course");
    return;
  }

  startButtonText->setText(validation.firstMissingIndex >= 0 ? "Missing"
                                                             : "Start Course");
}

void MainMenuScene::startSelectedCourse() {
  if (willStart.load() || unzipInProgress.load() ||
      pendingSelectChartPath.has_value() || chartListReloadRequested.load() ||
      folderItemsReloadRequested.load() || recyclerView == nullptr ||
      activeFolder.type != LibraryFolderItem::Type::Course ||
      activeFolder.courseId <= 0) {
    return;
  }

  const CourseValidationCache &validation = courseValidationForActiveFolder();
  if (validation.empty) {
    if (replayStatusText != nullptr) {
      replayStatusText->setText("No course charts");
    }
    refreshStartButtonForActiveFolder();
    return;
  }

  const auto &records = validation.records;
  const int firstMissingIndex = validation.firstMissingIndex;
  if (firstMissingIndex >= 0) {
    if (replayStatusText != nullptr) {
      replayStatusText->setText("Course has missing charts");
    }
    int visibleMissingIndex = -1;
    const auto &missingRecord =
        records[static_cast<std::size_t>(firstMissingIndex)];
    if (!missingRecord.meta.BmsPath.empty()) {
      const int dbIndex = ChartDBHelper::GetInstance().FindChartMetaIndex(
          db, chartQueryForActiveFolder(), missingRecord.meta.BmsPath);
      if (dbIndex >= 0) {
        visibleMissingIndex = dbIndex + 1;
      }
    } else if (searchText.empty() && difficultyText.empty()) {
      visibleMissingIndex = firstMissingIndex + 1;
    }
    if (visibleMissingIndex >= 0) {
      const int previous = recyclerView->selectedIndex;
      if (previous >= 0 && previous < recyclerView->size() &&
          previous != visibleMissingIndex && recyclerView->onUnselected) {
        recyclerView->onUnselected(recyclerView->get(previous), previous);
      }
      recyclerView->selectedIndex = visibleMissingIndex;
      recyclerView->rebindVisibleItems();
      if (recyclerView->onSelected) {
        recyclerView->onSelected(recyclerView->get(visibleMissingIndex),
                                 visibleMissingIndex);
      }
    }
    refreshStartButtonForActiveFolder();
    return;
  }

  auto session = std::make_shared<CoursePlaySession>();
  session->courseId = activeFolder.courseId;
  session->courseName =
      activeFolder.courseGroupName.empty()
          ? activeFolder.label
          : activeFolder.courseGroupName + " " + activeFolder.label;
  session->courseGroupName = activeFolder.courseGroupName;
  session->constraintJson = activeFolder.courseConstraintJson;
  session->entries.reserve(records.size());
  for (const auto &record : records) {
    session->entries.push_back(CoursePlayEntry{.meta = record.meta});
  }
  const CourseConstraintSettings constraintSettings =
      courseConstraintSettingsFromJson(activeFolder.courseConstraintJson);
  int courseLongNoteMode = long_note_mode::valueFromId(selectedLnMode);
  if (constraintSettings.rules.longNoteMode !=
      CourseLongNoteMode::Unspecified) {
    courseLongNoteMode = courseLongNoteModeToChartMetaValue(
        constraintSettings.rules.longNoteMode);
  }
  session->currentIndex = 0;
  session->gaugeType = selectedGaugeType;
  session->gaugeProfile = constraintSettings.gaugeProfile;
  session->gaugeAutoShift = selectedGaugeAutoShift;
  session->longNoteMode = courseLongNoteMode;
  session->constraints = constraintSettings.rules;
  session->requestedPlayOption =
      coursePlayOptionForConstraints(selectedPlayOption, constraintSettings);
  session->assistOption = assist_options::kOff;
  session->autoKeySound = !context.settings.inputKeysoundEnabled;
  startCourseDirect(std::move(session));
}

void MainMenuScene::startCourseDirect(
    std::shared_ptr<CoursePlaySession> session) {
  if (session == nullptr || session->entries.empty() ||
      willStart.exchange(true)) {
    return;
  }

  if (startButtonText != nullptr) {
    startButtonText->setText("Loading...");
  }
  ImageView::dropAllCache();
  previewLoadCancelled = true;
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  const int selectedLongNoteMode =
      normalizeChartLongNoteModeValue(session->longNoteMode) > 0
          ? normalizeChartLongNoteModeValue(session->longNoteMode)
          : long_note_mode::valueFromId(selectedLnMode);
  session->longNoteMode = selectedLongNoteMode;
  if (!session->courseReplayPlayback) {
    session->assistOption = assist_options::kOff;
  }

  defer(
      [this, session, selectedLongNoteMode]() {
        auto finishStart = [this]() {
          resetStartLoadingUi();
          return true;
        };
        if (loadThread.joinable()) {
          loadThread.join();
        }
        clearSelectedChart();

        const bms_parser::ChartMeta *firstMeta = session->currentMeta();
        if (firstMeta == nullptr || firstMeta->BmsPath.empty()) {
          return finishStart();
        }

        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> preparedChart;
        try {
          preparedChart = play_options::parseChart(firstMeta->BmsPath,
                                                   parseCancelled, "course");
        } catch (const std::exception &e) {
          SDL_Log("Error parsing %s for course start: %s",
                  fspath_to_utf8(firstMeta->BmsPath).c_str(), e.what());
          archive_file::appendDebugLogLine(
              "Course start parse exception: " +
              fspath_to_utf8(firstMeta->BmsPath) + ": " + e.what());
        }
        if (preparedChart == nullptr || parseCancelled) {
          if (replayStatusText != nullptr) {
            replayStatusText->setText("Course start failed");
          }
          return finishStart();
        }
        applyCourseConstraintsToChart(*preparedChart, session->constraints);

        play_options::PlayOptionReplayInfo playInfo =
            play_options::applySelectedPlayOptions(*preparedChart,
                                                   session->requestedPlayOption);
        applyEffectiveLongNoteModeToChart(*preparedChart,
                                          selectedLongNoteMode);
        session->playOption = playInfo.option;
        session->playOptionSeed = playInfo.seed;
        session->playOption2 = playInfo.option2;
        session->playOption2Seed = playInfo.seed2;

        context.jukebox.stop();
        context.jukebox.loadChart(*preparedChart, true, parseCancelled);
        if (parseCancelled) {
          return finishStart();
        }

        StartOptions options;
        options.startPosition = 0;
        options.autoKeySound = session->autoKeySound;
        options.autoPlay = false;
        options.gaugeType = session->gaugeType;
        options.gaugeProfile = session->gaugeProfile;
        options.gaugeAutoShift = session->gaugeAutoShift;
        options.playOption = playInfo.option;
        options.playOptionSeed = playInfo.seed;
        options.playOption2 = playInfo.option2;
        options.playOption2Seed = playInfo.seed2;
        options.longNoteMode = selectedLongNoteMode;
        options.assistOption = session->assistOption;
        options.courseSession = session;
        options.courseConstraints = session->constraints;
        options.ownsChart = true;

        context.sceneManager->changeScene(
            std::make_unique<GamePlayScene>(context, std::move(preparedChart),
                                            std::move(options)),
            true);
        return finishStart();
      },
      0, true);
}

void MainMenuScene::startSelectedChart() {
  if (willStart.load() || unzipInProgress.load() ||
      pendingSelectChartPath.has_value() || chartListReloadRequested.load() ||
      folderItemsReloadRequested.load() || recyclerView == nullptr) {
    return;
  }

  int selected = recyclerView != nullptr ? recyclerView->selectedIndex : -1;
  if (recyclerView == nullptr || selected < 0 ||
      selected >= recyclerView->size()) {
    return;
  }
  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    return;
  }
  startChartDirect(record);
}

void MainMenuScene::startChartDirect(const ChartMetaRecord &record) {
  if (willStart.exchange(true)) {
    return;
  }

  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    resetStartLoadingUi();
    return;
  }

  if (startButtonText != nullptr) {
    startButtonText->setText("Loading...");
  }
  ImageView::dropAllCache();

  const GaugeType gaugeType = selectedGaugeType;
  const bool gaugeAutoShift = selectedGaugeAutoShift;
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;
  const std::string playOption = selectedPlayOption;
  int selectedLongNoteMode = normalizeChartLongNoteModeValue(record.meta.LnMode);
  if (selectedLongNoteMode == 0) {
    selectedLongNoteMode = long_note_mode::valueFromId(selectedLnMode);
  }
  const std::string assistOption = selectedAssistOption;
  const std::string normalizedPlayOption =
      play_options::normalizePlayOption(playOption);
  const bool canReusePreviewForStart =
      normalizedPlayOption.empty() || normalizedPlayOption == "NORMAL";
  const SelectedChartRandomInfo chartRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  defer(
      [this, record, gaugeType, gaugeAutoShift, autoKeySound, playOption,
       selectedLongNoteMode, assistOption, canReusePreviewForStart,
       chartRandomInfo]() {
        auto finishStart = [this]() {
          resetStartLoadingUi();
          return true;
        };
        if (!canReusePreviewForStart) {
          previewLoadCancelled = true;
        }
        if (loadThread.joinable()) {
          loadThread.join();
        }

        bms_parser::Chart *readyChart = nullptr;
        if (canReusePreviewForStart) {
          readyChart = loadedSelectedChartForPath(record.meta.BmsPath);
        }
        if (readyChart != nullptr) {
          archive_file::appendDebugLogLine(
              "Start reusing loaded preview chart: " +
              fspath_to_utf8(record.meta.BmsPath));
          applyEffectiveLongNoteModeToChart(*readyChart,
                                            selectedLongNoteMode);
          context.jukebox.stop();
          changeToGameplayScene(readyChart,
                                {
                                    .startPosition = 0,
                                    .autoKeySound = autoKeySound,
                                    .autoPlay = false,
                                    .gaugeType = gaugeType,
                                    .gaugeAutoShift = gaugeAutoShift,
                                    .longNoteMode = selectedLongNoteMode,
                                    .assistOption = assistOption,
                                });
          return finishStart();
        }

        selectedChartMediaReady.store(false);
        selectedChartReusableForStart.store(false);
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> preparedChart;
        try {
          preparedChart = play_options::parseChart(
              record.meta.BmsPath, chartRandomInfo.seed, chartRandomInfo.prng,
              chartRandomInfo.values, parseCancelled);
        } catch (const std::exception &e) {
          SDL_Log("Error parsing %s for start: %s",
                  fspath_to_utf8(record.meta.BmsPath).c_str(), e.what());
          archive_file::appendDebugLogLine(
              "Start parse exception: " +
              fspath_to_utf8(record.meta.BmsPath) + ": " + e.what());
        }
        if (preparedChart != nullptr && !parseCancelled) {
          play_options::PlayOptionReplayInfo playInfo =
              play_options::applySelectedPlayOptions(*preparedChart,
                                                     playOption);
          applyEffectiveLongNoteModeToChart(*preparedChart,
                                            selectedLongNoteMode);
          context.jukebox.stop();
          context.jukebox.loadChart(*preparedChart, true, parseCancelled);
          bms_parser::Chart *loadedChart = nullptr;
          if (!parseCancelled) {
            loadedChart = setSelectedChart(
                std::move(preparedChart), true,
                play_options::isNormalPlayOption(playOption));
          }
          if (parseCancelled) {
            preparedChart.reset();
          } else {
            changeToGameplayScene(loadedChart,
                                  {
                                      .startPosition = 0,
                                      .autoKeySound = autoKeySound,
                                      .autoPlay = false,
                                      .gaugeType = gaugeType,
                                      .gaugeAutoShift = gaugeAutoShift,
                                      .playOption = playInfo.option,
                                      .playOptionSeed = playInfo.seed,
                                      .playOption2 = playInfo.option2,
                                      .playOption2Seed = playInfo.seed2,
                                      .longNoteMode = selectedLongNoteMode,
                                      .assistOption = assistOption,
                                  });
            return finishStart();
          }
        }

        auto *chart = loadedSelectedChartForPath(record.meta.BmsPath);
        if (chart == nullptr) {
          return finishStart();
        }

        context.jukebox.stop();
        applyEffectiveLongNoteModeToChart(*chart, selectedLongNoteMode);
        changeToGameplayScene(chart, {
                                         .startPosition = 0,
                                         .autoKeySound = autoKeySound,
                                         .autoPlay = false,
                                         .gaugeType = gaugeType,
                                         .gaugeAutoShift = gaugeAutoShift,
                                         .longNoteMode = selectedLongNoteMode,
                                         .assistOption = assistOption,
                                     });
        return finishStart();
      },
      0, true);
}

void MainMenuScene::openChartViewerForSelection() {
  if (willStart.load() || replayExportInProgress.load() ||
      unzipInProgress.load() || pendingSelectChartPath.has_value() ||
      chartListReloadRequested.load() || folderItemsReloadRequested.load() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    return;
  }
  openChartViewerDirect(record);
}

void MainMenuScene::openChartViewerDirect(const ChartMetaRecord &record) {
  if (willStart.load() || replayExportInProgress.load() ||
      record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    return;
  }

  const SelectedChartRandomInfo chartRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  cancelPreviewLoading(false);
  archive_file::appendDebugLogLine(
      "Open chart viewer: " + fspath_to_utf8(record.meta.BmsPath));
  context.jukebox.stop();
  context.sceneManager->changeScene(
      std::make_unique<ChartViewerScene>(context, record, chartRandomInfo.seed,
                                         chartRandomInfo.prng,
                                         chartRandomInfo.values),
      true);
}

void MainMenuScene::revealSelectedChartInFileManager() {
  if (willStart.load() || replayExportInProgress.load() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.unavailable || record.meta.BmsPath.empty()) {
    return;
  }

  std::string errorMessage;
  if (!revealPathInFileManager(record.meta.BmsPath, errorMessage)) {
    SDL_Log("Failed to reveal chart file %s: %s",
            fspath_to_utf8(record.meta.BmsPath).c_str(), errorMessage.c_str());
  }
}

void MainMenuScene::reselectCurrentChart() {
  if (recyclerView == nullptr || !recyclerView->onSelected) {
    return;
  }
  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  recyclerView->onSelected(record, selected);
}

void MainMenuScene::refreshReplayAvailability(const ChartMetaRecord *record) {
  replaySummaries.clear();
  selectedReplayIndex = -1;
  if (record != nullptr && record->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0) {
    replaySummaries =
        ReplayDBHelper::GetInstance().ListCourseReplays(activeFolder.courseId);
    setReplayButtonVisible(!replaySummaries.empty());
    return;
  }

  if (record == nullptr || record->solidArchive || record->unavailable ||
      record->meta.BmsPath.empty()) {
    setReplayButtonVisible(false);
    return;
  }

  replaySummaries = ReplayDBHelper::GetInstance().ListReplays(record->meta);
  setReplayButtonVisible(true);
}

void MainMenuScene::setReplayButtonVisible(bool visible) {
  if (replayButtonSlot == nullptr) {
    return;
  }

  replayButtonSlot->setVisible(visible);
  replayButtonSlot->setHeight(visible ? 58.0f : 0.0f);
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::setPlayableChartActionsVisible(bool visible) {
  setPlayableChartActionsVisible(visible, visible);
}

void MainMenuScene::setPlayableChartActionsVisible(bool visible,
                                                   bool chartActionsVisible) {
  if (startButton != nullptr) {
    startButton->setVisible(visible);
    startButton->setHeight(visible ? 86.0f : 0.0f);
  }
  if (chartActionsRow != nullptr) {
    const bool showChartActions = visible && chartActionsVisible;
    chartActionsRow->setVisible(showChartActions);
    chartActionsRow->setHeight(showChartActions ? 58.0f : 0.0f);
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::setUnzipButtonVisible(bool visible) {
  if (unzipButtonSlot == nullptr) {
    return;
  }

  const bool show = visible || unzipInProgress.load();
  unzipButtonSlot->setVisible(show);
  unzipButtonSlot->setHeight(show ? 58.0f : 0.0f);
  if (unzipButtonText != nullptr && unzipInProgress.load()) {
    unzipButtonText->setText("Unzipping...");
  }
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::refreshUnzipButtonForSelection(
    const ChartMetaRecord *record) {
  bool visible = false;
  if (record != nullptr && !record->unavailable &&
      !record->meta.BmsPath.empty()) {
    visible = record->solidArchive;
  }
  if (unzipButtonText != nullptr && !unzipInProgress.load()) {
    unzipButtonText->setText("Unzip");
  }
  setUnzipButtonVisible(visible);
}

void MainMenuScene::startUnzipSelectedArchiveFolder() {
  if (willStart.load() || replayExportInProgress.load() ||
      unzipInProgress.load() || pendingSelectChartPath.has_value() ||
      chartListReloadRequested.load() || folderItemsReloadRequested.load() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  startUnzipArchiveFolder(record);
}

void MainMenuScene::startUnzipArchiveFolder(const ChartMetaRecord &record) {
  const bool fullArchiveUnzip =
      record.solidArchive && !archive_file::isVirtualPath(record.meta.BmsPath);
  if (willStart.load() || replayExportInProgress.load() ||
      pendingSelectChartPath.has_value() || chartListReloadRequested.load() ||
      folderItemsReloadRequested.load() || record.unavailable ||
      record.meta.BmsPath.empty() || !fullArchiveUnzip) {
    return;
  }
  if (unzipInProgress.exchange(true)) {
    return;
  }

  if (unzipThread.joinable()) {
    unzipThread.join();
  }
  {
    std::lock_guard<std::mutex> lock(unzipResultMutex);
    pendingUnzipResult.reset();
  }
  {
    std::lock_guard<std::mutex> lock(unzipProgressMutex);
    pendingUnzipProgress.reset();
  }
  previewLoadCancelled = true;
  if (loadThread.joinable()) {
    loadThread.join();
  }
  stopAndClearSelectedChart();
  unzipEstimatedUncompressedSize =
      fullArchiveUnzip ? record.archiveUncompressedSize : 0;

  if (unzipButtonText != nullptr) {
    unzipButtonText->setText("Unzipping...");
  }
  if (replayStatusText != nullptr) {
    replayStatusText->setText("Unzipping full archive...");
  }
  setUnzipButtonVisible(true);
  showUnzipProgressModal();

  const std::filesystem::path sourceArchivePath = record.meta.BmsPath;

  std::filesystem::path outputRoot = sourceArchivePath.parent_path();
  if (outputRoot.empty()) {
    outputRoot = ".";
  }
  archive_file::appendDebugLogLine(
      "Unzip requested: " + fspath_to_utf8(record.meta.BmsPath) +
      " outputRoot=" + fspath_to_utf8(outputRoot) + " mode=full-archive");

  unzipThread = std::jthread([this, record, outputRoot, fullArchiveUnzip,
                              sourceArchivePath](
                                 const std::stop_token &stopToken) {
    PendingUnzipResult result;
    result.rootPath = outputRoot;
    result.archivePath = sourceArchivePath;
    result.canDeleteArchive = fullArchiveUnzip;
    auto postProgress = [this](const archive_file::UnzipProgress &progress) {
      std::lock_guard<std::mutex> lock(unzipProgressMutex);
      pendingUnzipProgress = PendingUnzipProgress{
          .fraction = progress.fraction,
          .current = progress.current,
          .total = progress.total,
          .message = progress.message,
      };
    };

    std::string errorMessage;
    std::filesystem::path scanRoot = outputRoot;
    const auto unzippedArchive = archive_file::unzipArchiveFully(
        record.meta.BmsPath, outputRoot, &errorMessage, &stopToken,
        postProgress);
    if (unzippedArchive.has_value()) {
      result.outputFolder = unzippedArchive->outputFolder;
      scanRoot = unzippedArchive->outputFolder;
    }

    if (result.outputFolder.empty()) {
      result.success = false;
      result.message =
          stopToken.stop_requested()
              ? "Unzip cancelled"
              : (errorMessage.empty() ? "Unzip failed"
                                      : "Unzip failed: " + errorMessage);
    } else if (stopToken.stop_requested()) {
      result.success = false;
      result.message = "Unzip cancelled";
    } else {
      auto &dbHelper = ChartDBHelper::GetInstance();
      SqliteConnectionHandle unzipDbHandle(dbHelper.Connect());
      sqlite3 *unzipDb = unzipDbHandle.get();
      if (unzipDb == nullptr) {
        result.success = false;
        result.message = "Unzipped archive. Failed to refresh library.";
      } else {
        dbHelper.CreateChartMetaTable(unzipDb);
        dbHelper.CreateSolidArchiveTable(unzipDb);
        dbHelper.CreateDifficultyTableTables(unzipDb);
        std::vector<std::filesystem::path> roots{scanRoot};
        postProgress(archive_file::UnzipProgress{
            .fraction = 0.98, .message = "Refreshing library"});
        const int changedCount =
            dbHelper.ScanChartRoots(unzipDb, roots, &stopToken);
        if (!stopToken.stop_requested()) {
          std::vector<bms_parser::ChartMeta> chartMetas;
          dbHelper.SelectAllChartMeta(unzipDb, chartMetas);
          for (const auto &meta : chartMetas) {
            if (pathIsInsideDirectoryForMenu(meta.BmsPath, scanRoot)) {
              result.chartPath = meta.BmsPath;
              break;
            }
          }
        }

        result.success = true;
        result.message = changedCount > 0
                             ? "Unzipped archive. Library refreshed."
                             : "Unzipped archive. Library already current.";
        if (!stopToken.stop_requested()) {
          requestLibraryReload(true);
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(unzipResultMutex);
      pendingUnzipResult = std::move(result);
    }
  });
}

void MainMenuScene::buildUnzipProgressModal() {
  constexpr float kModalPanelWidth = 700.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;

  unzipModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                           rendering::window_height);
  unzipModalRoot->setPositionType(YGPositionTypeAbsolute);
  unzipModalRoot->setPosition(Edge::Left, 0);
  unzipModalRoot->setPosition(Edge::Top, 0);
  unzipModalRoot->setZIndex(1000);
  unzipModalRoot->setVisible(false);
  unzipModalRoot->setFlexDirection(FlexDirection::Column);
  unzipModalRoot->setAlignItems(YGAlignCenter);
  unzipModalRoot->setJustifyContent(YGJustifyCenter);
  unzipModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setFlexDirection(FlexDirection::Column)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  unzipModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  unzipModalTitleText->setText("Unzip");
  unzipModalTitleText->setThemedColor(ui_theme::textPrimary);
  unzipModalTitleText->setHeight(42);
  panel->addView(unzipModalTitleText);

  unzipProgressMessageText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  unzipProgressMessageText->setThemedColor(ui_theme::textSecondary);
  unzipProgressMessageText->setHeight(32);
  panel->addView(unzipProgressMessageText);

  unzipProgressTrack = new View();
  unzipProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setThemedBackgroundColor(ui_theme::progressTrack)
      ->setCornerRadius(ui_theme::controlRadius())
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);
  unzipProgressFill = new View();
  unzipProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      ui_theme::progressFill());
  unzipProgressTrack->addView(unzipProgressFill);
  panel->addView(unzipProgressTrack);

  unzipProgressPercentText = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  unzipProgressPercentText->setThemedColor(ui_theme::textSecondary);
  unzipProgressPercentText->setHeight(28);
  panel->addView(unzipProgressPercentText);

  unzipProgressDetailText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  unzipProgressDetailText->setThemedColor(ui_theme::textMuted);
  unzipProgressDetailText->setHeight(54);
  panel->addView(unzipProgressDetailText);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  unzipDeleteArchiveButton =
      makeModalButton("Delete Archive", 18, &unzipDeleteArchiveButtonText);
  unzipDeleteArchiveButton->setVisible(false);
  unzipDeleteArchiveButton->setWidth(0)->setHeight(0);
  unzipDeleteArchiveButton->setOnClickListener(
      [this]() { deleteUnzippedSourceArchive(); });
  footer->addView(unzipDeleteArchiveButton);

  unzipCancelButton = makeModalButton("Cancel", 20, &unzipCancelButtonText);
  unzipCancelButton->setWidth(130);
  unzipCancelButton->setOnClickListener([this]() {
    if (unzipInProgress.load()) {
      if (unzipThread.joinable()) {
        unzipThread.request_stop();
      }
      updateUnzipProgressUi(0.0, "Cancelling...", 0, 0);
      return;
    }
    hideUnzipProgressModal();
  });
  footer->addView(unzipCancelButton);
  panel->addView(footer);

  unzipModalRoot->addView(panel);
  rootLayout->addView(unzipModalRoot);
}

void MainMenuScene::showUnzipProgressModal() {
  if (unzipModalRoot == nullptr) {
    return;
  }
  unzipModalRoot->setSize(rendering::window_width, rendering::window_height);
  unzipModalRoot->setVisible(true);
  if (unzipModalTitleText != nullptr) {
    unzipModalTitleText->setText("Unzip");
  }
  if (unzipCancelButtonText != nullptr) {
    unzipCancelButtonText->setText("Cancel");
  }
  setUnzipDeleteArchiveButtonVisible(false);
  unzipDeleteCandidatePath.reset();
  updateUnzipProgressUi(0.0, "Preparing unzip", 0, 0);
  unzipModalRoot->applyYogaLayout();
}

void MainMenuScene::hideUnzipProgressModal() {
  if (unzipModalRoot != nullptr) {
    unzipModalRoot->setVisible(false);
  }
  if (!unzipInProgress.load()) {
    unzipEstimatedUncompressedSize = 0;
    unzipDeleteCandidatePath.reset();
    setUnzipDeleteArchiveButtonVisible(false);
  }
}

void MainMenuScene::updateUnzipProgressUi(double fraction,
                                          const std::string &message,
                                          std::uint64_t current,
                                          std::uint64_t total) {
  fraction = std::clamp(fraction, 0.0, 1.0);
  if (unzipProgressMessageText != nullptr) {
    unzipProgressMessageText->setText(message);
  }
  if (unzipProgressFill != nullptr && unzipProgressTrack != nullptr) {
    unzipProgressFill->setWidth(
        std::max(0.0f, (unzipProgressTrack->getWidth() - 4.0f) *
                           static_cast<float>(fraction)));
  }
  if (unzipProgressPercentText != nullptr) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(0) << (fraction * 100.0) << "%";
    if (total > 0) {
      text << " (" << current << "/" << total << ")";
    }
    unzipProgressPercentText->setText(text.str());
  }
  if (unzipProgressDetailText != nullptr) {
    std::string detail = total > 0 ? "Processing files" : "Working on archive";
    if (unzipEstimatedUncompressedSize > 0) {
      detail += "\nEstimated unzipped size: " +
                formatFindBmsBytes(unzipEstimatedUncompressedSize);
    }
    unzipProgressDetailText->setText(detail);
  }
  if (unzipModalRoot != nullptr && unzipModalRoot->getVisible()) {
    unzipModalRoot->applyYogaLayout();
  }
}

void MainMenuScene::setUnzipDeleteArchiveButtonVisible(bool visible) {
  if (unzipDeleteArchiveButton == nullptr) {
    return;
  }
  unzipDeleteArchiveButton->setVisible(visible);
  unzipDeleteArchiveButton->setWidth(visible ? 210.0f : 0.0f);
  unzipDeleteArchiveButton->setHeight(visible ? 58.0f : 0.0f);
  if (unzipDeleteArchiveButtonText != nullptr) {
    unzipDeleteArchiveButtonText->setText("Delete Archive");
  }
  if (unzipModalRoot != nullptr && unzipModalRoot->getVisible()) {
    unzipModalRoot->applyYogaLayout();
  }
}

void MainMenuScene::deleteUnzippedSourceArchive() {
  if (unzipInProgress.load() || !unzipDeleteCandidatePath.has_value()) {
    return;
  }

  const std::filesystem::path archivePath = *unzipDeleteCandidatePath;
  std::error_code error;
  const bool archiveExists =
      std::filesystem::is_regular_file(archivePath, error);
  if (error) {
    updateUnzipProgressUi(1.0,
                          "Could not check archive: " + error.message(), 0, 0);
    archive_file::appendDebugLogLine(
        "Failed to check source archive before delete: " +
        fspath_to_utf8(archivePath) + ": " + error.message());
    return;
  }
  if (!archiveExists) {
    updateUnzipProgressUi(1.0, "Archive is already unavailable", 0, 0);
    setUnzipDeleteArchiveButtonVisible(false);
    unzipDeleteCandidatePath.reset();
    return;
  }

  const bool removed = std::filesystem::remove(archivePath, error);
  if (error || !removed) {
    updateUnzipProgressUi(
        1.0,
        "Could not delete archive" +
            (error ? std::string(": ") + error.message() : std::string()),
        0, 0);
    archive_file::appendDebugLogLine(
        "Failed to delete source archive: " + fspath_to_utf8(archivePath) +
        (error ? ": " + error.message() : ""));
    return;
  }

  auto &dbHelper = ChartDBHelper::GetInstance();
  SqliteConnectionHandle deleteDbHandle(dbHelper.Connect());
  sqlite3 *deleteDb = deleteDbHandle.get();
  if (deleteDb != nullptr) {
    dbHelper.DeleteArchiveRecords(deleteDb, archivePath);
  }
  requestLibraryReload(true);
  unzipDeleteCandidatePath.reset();
  setUnzipDeleteArchiveButtonVisible(false);
  if (unzipModalTitleText != nullptr) {
    unzipModalTitleText->setText("Archive Deleted");
  }
  if (unzipCancelButtonText != nullptr) {
    unzipCancelButtonText->setText("Close");
  }
  updateUnzipProgressUi(1.0, "Original archive deleted", 0, 0);
  archive_file::appendDebugLogLine(
      "Deleted source archive after unzip: " + fspath_to_utf8(archivePath));
}

void MainMenuScene::applyUnzipProgress() {
  std::optional<PendingUnzipProgress> progress;
  {
    std::lock_guard<std::mutex> lock(unzipProgressMutex);
    if (!pendingUnzipProgress.has_value()) {
      return;
    }
    progress = std::move(pendingUnzipProgress);
    pendingUnzipProgress.reset();
  }
  updateUnzipProgressUi(progress->fraction, progress->message,
                        progress->current, progress->total);
}

void MainMenuScene::applyUnzipResult() {
  std::optional<PendingUnzipResult> result;
  {
    std::lock_guard<std::mutex> lock(unzipResultMutex);
    if (!pendingUnzipResult.has_value()) {
      return;
    }
    result = std::move(pendingUnzipResult);
    pendingUnzipResult.reset();
  }

  if (unzipThread.joinable()) {
    unzipThread.join();
  }
  unzipInProgress.store(false);
  if (unzipButtonText != nullptr) {
    unzipButtonText->setText(result->success ? "Unzipped" : "Unzip");
  }
  if (unzipModalTitleText != nullptr) {
    unzipModalTitleText->setText(result->success ? "Unzip Complete"
                                                 : "Unzip Failed");
  }
  updateUnzipProgressUi(result->success ? 1.0 : 0.0, result->message, 0, 0);
  std::error_code archiveStateError;
  const bool canDeleteArchive = result->success && result->canDeleteArchive &&
                                !result->archivePath.empty() &&
                                std::filesystem::is_regular_file(
                                    result->archivePath, archiveStateError) &&
                                !archiveStateError;
  if (canDeleteArchive) {
    unzipDeleteCandidatePath = result->archivePath;
    setUnzipDeleteArchiveButtonVisible(true);
    if (unzipCancelButtonText != nullptr) {
      unzipCancelButtonText->setText("Keep Archive");
    }
    if (unzipProgressDetailText != nullptr) {
      unzipProgressDetailText->setText(
          "Choose whether to keep or delete the original archive.");
    }
  } else {
    unzipDeleteCandidatePath.reset();
    setUnzipDeleteArchiveButtonVisible(false);
    if (unzipCancelButtonText != nullptr) {
      unzipCancelButtonText->setText("Close");
    }
  }
  if (result->success && !result->chartPath.empty()) {
    pendingSelectChartPath = result->chartPath;
    requestLibraryReload(true);
  }
  if (replayStatusText != nullptr) {
    replayStatusText->setText(result->message);
  }
  archive_file::appendDebugLogLine(
      result->message +
      (result->chartPath.empty() ? ""
                                 : ": " + fspath_to_utf8(result->chartPath)));

  defer(
      [this, hideModal = result->success && !canDeleteArchive]() {
        if (!unzipInProgress.load() && replayStatusText != nullptr) {
          replayStatusText->setText("");
        }
        if (!unzipInProgress.load() && unzipButtonText != nullptr) {
          unzipButtonText->setText("Unzip");
        }
        if (hideModal && !unzipInProgress.load() && unzipModalRoot != nullptr &&
            unzipModalRoot->getVisible()) {
          hideUnzipProgressModal();
        }
        return true;
      },
      result->success ? 900 : 1800, true);
}

void MainMenuScene::startLibraryRefresh() {
  if (willStart.load() || replayExportInProgress.load()) {
    return;
  }
  enqueueLibraryRefreshTask("Refresh Library");
}

void MainMenuScene::setFindBmsButtonVisible(bool visible) {
  if (findBmsButtonSlot == nullptr) {
    return;
  }

  findBmsButtonSlot->setVisible(visible);
  findBmsButtonSlot->setHeight(visible ? 58.0f : 0.0f);
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::openFindBmsForSelection() {
  if (willStart.load() || replayExportInProgress.load() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || !record.unavailable ||
      (record.meta.SHA256.empty() && record.meta.MD5.empty() &&
       record.meta.Title.empty())) {
    return;
  }
  showFindBmsModal(record);
}

std::filesystem::path MainMenuScene::preferredBmsDownloadRoot() {
  auto &dbHelper = ChartDBHelper::GetInstance();
  dbHelper.CreateEntriesTable(db);
  auto entries = dbHelper.SelectEffectiveEntries(db);
  if (entries.empty()) {
    const auto path = ChartDBHelper::DefaultBmsFolderPath();
    const bool pathReady =
        ensureDirectoryExistsLogged(path, "BMS download root");
#if !(TARGET_OS_ANDROID)
    if (pathReady) {
      dbHelper.InsertEntry(db, path);
    }
#endif
    return path;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return ResolveIOSFolderEntryPath(entries.front());
#elif TARGET_OS_ANDROID
  const auto path = ChartDBHelper::DefaultBmsFolderPath();
  ensureDirectoryExistsLogged(path, "BMS download root");
  return path;
#else
  return std::filesystem::path(entries.front().path);
#endif
}

void MainMenuScene::buildParseLogModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 900.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;

  parseLogModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                              rendering::window_height);
  parseLogModalRoot->setPositionType(YGPositionTypeAbsolute);
  parseLogModalRoot->setPosition(Edge::Left, 0);
  parseLogModalRoot->setPosition(Edge::Top, 0);
  parseLogModalRoot->setZIndex(1000);
  parseLogModalRoot->setVisible(false);
  parseLogModalRoot->setFlexDirection(FlexDirection::Column);
  parseLogModalRoot->setAlignItems(YGAlignCenter);
  parseLogModalRoot->setJustifyContent(YGJustifyCenter);
  parseLogModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(640)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Parsing Logs");
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(42);
  panel->addView(title);

  parseLogScrollView =
      new ScrollView(0, 0, static_cast<int>(kModalContentWidth), 480);
  parseLogScrollView->setWidth(kModalContentWidth);
  parseLogScrollView->setFlex(1);
  parseLogScrollView->setThemedBackgroundColor(ui_theme::insetSurface);
  parseLogScrollView->setCornerRadius(ui_theme::controlRadius());
  parseLogScrollView->setThemedBorderColor(ui_theme::hairline);
  parseLogScrollView->setBorderWidth(1);

  parseLogContent = new View();
  parseLogContent->setFlexDirection(FlexDirection::Column);
  parseLogContent->setAlignItems(YGAlignStretch);
  parseLogContent->setPadding(Edge::All, 10);

  parseLogText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  parseLogText->setText(archive_file::debugLogText());
  parseLogText->setThemedColor(ui_theme::textSecondary);
  parseLogText->setWrap(true);
  parseLogText->setOverflow(TextView::TextOverflow::Visible);
  parseLogContent->addView(parseLogText);
  parseLogScrollView->setContentView(parseLogContent);
  panel->addView(parseLogScrollView);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  parseLogCloseButton = makeModalButton("Close", 20, &parseLogCloseButtonText);
  parseLogCloseButton->setWidth(130);
  parseLogCloseButton->setOnClickListener([this]() { hideParseLogModal(); });
  styleThemedActionButton(parseLogCloseButton, parseLogCloseButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);

  footer->addView(parseLogCloseButton);
  panel->addView(footer);

  parseLogModalRoot->addView(panel);
  rootLayout->addView(parseLogModalRoot);
  parseLogDisplayedRevision = 0;
  refreshParseLogModal();
}

void MainMenuScene::showParseLogModal() {
  if (parseLogModalRoot == nullptr) {
    return;
  }
  parseLogModalRoot->setSize(rendering::window_width, rendering::window_height);
  parseLogModalRoot->setVisible(true);
  parseLogDisplayedRevision = 0;
  refreshParseLogModal();
  if (parseLogScrollView != nullptr) {
    parseLogScrollView->scrollToBottom();
  }
}

void MainMenuScene::hideParseLogModal() {
  if (parseLogModalRoot != nullptr) {
    parseLogModalRoot->setVisible(false);
  }
}

void MainMenuScene::refreshParseLogModal() {
  if (parseLogModalRoot == nullptr || parseLogText == nullptr) {
    return;
  }

  const std::uint64_t revision = archive_file::debugLogRevision();
  if (revision == parseLogDisplayedRevision) {
    return;
  }
  parseLogDisplayedRevision = revision;
  parseLogText->setText(archive_file::debugLogText());
  if (parseLogScrollView != nullptr) {
    parseLogScrollView->scrollToBottom();
  }
}

void MainMenuScene::buildMusicModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;

  musicModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                           rendering::window_height);
  musicModalRoot->setPositionType(YGPositionTypeAbsolute);
  musicModalRoot->setPosition(Edge::Left, 0);
  musicModalRoot->setPosition(Edge::Top, 0);
  musicModalRoot->setZIndex(1000);
  musicModalRoot->setVisible(false);
  musicModalRoot->setFlexDirection(FlexDirection::Column);
  musicModalRoot->setAlignItems(YGAlignCenter);
  musicModalRoot->setJustifyContent(YGJustifyCenter);
  musicModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(650)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Music Player");
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(42);
  panel->addView(title);

  musicTrackText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  musicTrackText->setHeight(62);
  musicTrackText->setWrap(true);
  musicTrackText->setOverflow(TextView::TextOverflow::Hidden);
  musicTrackText->setThemedColor(ui_theme::textPrimary);
  panel->addView(musicTrackText);

  musicStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  musicStatusText->setHeight(70);
  musicStatusText->setWrap(true);
  musicStatusText->setThemedColor(ui_theme::textSecondary);
  panel->addView(musicStatusText);

  musicPlaylistText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  musicPlaylistText->setHeight(100);
  musicPlaylistText->setWrap(true);
  musicPlaylistText->setOverflow(TextView::TextOverflow::Hidden);
  musicPlaylistText->setThemedColor(ui_theme::textSecondary);
  panel->addView(musicPlaylistText);

  auto *sourceRow = makeModalOptionRow();
  musicSelectedButton =
      makeModalButton("Play Selected", 18, &musicSelectedButtonText);
  musicSelectedButton->setFlex(1);
  musicSelectedButton->setOnClickListener(
      [this]() { playSelectedChartAsMusic(); });
  musicRandomButton = makeModalButton("Random All", 20, &musicRandomButtonText);
  musicRandomButton->setFlex(1);
  musicRandomButton->setOnClickListener([this]() { playRandomMusicLibrary(); });
  sourceRow->addView(musicSelectedButton);
  sourceRow->addView(musicRandomButton);
  panel->addView(sourceRow);

  auto *playlistRow = makeModalOptionRow();
  musicAddSelectedButton =
      makeModalButton("Add Selected", 18, &musicAddSelectedButtonText);
  musicAddSelectedButton->setFlex(1);
  musicAddSelectedButton->setOnClickListener(
      [this]() { addSelectedChartToMusicPlaylist(); });
  musicRemoveSelectedButton =
      makeModalButton("Remove Selected", 16, &musicRemoveSelectedButtonText);
  musicRemoveSelectedButton->setFlex(1);
  musicRemoveSelectedButton->setOnClickListener(
      [this]() { removeSelectedChartFromMusicPlaylist(); });
  musicPlaylistButton =
      makeModalButton("Play Playlist", 18, &musicPlaylistButtonText);
  musicPlaylistButton->setFlex(1);
  musicPlaylistButton->setOnClickListener(
      [this]() { playSavedMusicPlaylist(); });
  musicClearPlaylistButton =
      makeModalButton("Clear", 18, &musicClearPlaylistButtonText);
  musicClearPlaylistButton->setFlex(1);
  musicClearPlaylistButton->setOnClickListener(
      [this]() { clearSavedMusicPlaylist(); });
  playlistRow->addView(musicAddSelectedButton);
  playlistRow->addView(musicRemoveSelectedButton);
  playlistRow->addView(musicPlaylistButton);
  playlistRow->addView(musicClearPlaylistButton);
  panel->addView(playlistRow);

  auto *transportRow = makeModalOptionRow();
  musicPreviousButton =
      makeModalButton("Previous", 16, &musicPreviousButtonText);
  musicPreviousButton->setFlex(1);
  musicPreviousButton->setOnClickListener(
      [this]() { playPreviousMusicTrack(); });
  musicSeekBackwardButton =
      makeModalButton("-10s", 18, &musicSeekBackwardButtonText);
  musicSeekBackwardButton->setFlex(1);
  musicSeekBackwardButton->setOnClickListener(
      [this]() { seekMusicRelative(-10000000LL); });
  musicPlayPauseButton = makeModalButton("Play", 20, &musicPlayPauseButtonText);
  musicPlayPauseButton->setFlex(1);
  musicPlayPauseButton->setOnClickListener([this]() { toggleMusicPlayback(); });
  musicSeekForwardButton =
      makeModalButton("+10s", 18, &musicSeekForwardButtonText);
  musicSeekForwardButton->setFlex(1);
  musicSeekForwardButton->setOnClickListener(
      [this]() { seekMusicRelative(10000000LL); });
  musicNextButton = makeModalButton("Next", 20, &musicNextButtonText);
  musicNextButton->setFlex(1);
  musicNextButton->setOnClickListener([this]() { playNextMusicTrack(); });
  musicStopButton = makeModalButton("Stop", 18, &musicStopButtonText);
  musicStopButton->setFlex(1);
  musicStopButton->setOnClickListener([this]() { stopMusicPlayback(); });
  transportRow->addView(musicPreviousButton);
  transportRow->addView(musicSeekBackwardButton);
  transportRow->addView(musicPlayPauseButton);
  transportRow->addView(musicSeekForwardButton);
  transportRow->addView(musicNextButton);
  transportRow->addView(musicStopButton);
  panel->addView(transportRow);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  musicCloseButton = makeModalButton("Close", 20, &musicCloseButtonText);
  musicCloseButton->setWidth(130);
  musicCloseButton->setOnClickListener([this]() { hideMusicModal(); });
  footer->addView(musicCloseButton);
  panel->addView(footer);

  musicModalRoot->addView(panel);
  rootLayout->addView(musicModalRoot);
  refreshMusicModal();
}

void MainMenuScene::showMusicModal() {
  if (musicModalRoot == nullptr) {
    return;
  }
  musicModalRoot->setSize(rendering::window_width, rendering::window_height);
  musicModalRoot->setVisible(true);
  std::string errorMessage;
  if (!context.musicPlayer.ReloadPlaylists(errorMessage) &&
      !errorMessage.empty()) {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::hideMusicModal() {
  if (musicModalRoot != nullptr) {
    musicModalRoot->setVisible(false);
  }
}

void MainMenuScene::refreshMusicModal() {
  if (musicModalRoot == nullptr || musicTrackText == nullptr ||
      musicStatusText == nullptr || musicPlaylistText == nullptr) {
    return;
  }

  const auto track = context.musicPlayer.CurrentTrackSnapshot();
  const auto playback = context.musicPlayer.PlaybackState();

  musicTrackText->setText(
      musicTrackDisplayName(track ? &track.value() : nullptr));

  std::string status;
  if (!musicStatusMessage.empty()) {
    status += musicStatusMessage + "\n";
  }
  if (!playback.supported) {
    status += "Native music playback is unavailable on this platform.";
  } else if (!playback.loaded) {
    status += "Choose Play Selected, Play Playlist, or Random All.";
  } else {
    status += playback.playing ? "Playing " : "Paused ";
    status += formatMusicTime(playback.positionMicros) + " / " +
              formatMusicTime(playback.durationMicros);
  }
  if (const auto playlist = context.musicPlayer.DefaultPlaylistSnapshot()) {
    status += "  " + playlist->name + ": " +
              std::to_string(playlist->trackCount);
  }
  const std::size_t libraryTrackCount = context.musicPlayer.LibraryTrackCount();
  if (libraryTrackCount > 0) {
    status += "  Library tracks: " + std::to_string(libraryTrackCount);
  }
  musicStatusText->setText(status);
  musicPlaylistText->setText(
      musicPlaylistTextSnapshot(
          context.musicPlayer.DefaultPlaylistTracksSnapshot()));

  if (musicPlayPauseButtonText != nullptr) {
    musicPlayPauseButtonText->setText(
        playback.playing ? "Pause" : (playback.loaded ? "Resume" : "Play"));
  }

  styleThemedActionButton(musicSelectedButton, musicSelectedButtonText, true,
                          ui_theme::primaryAction, ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  styleThemedActionButton(musicAddSelectedButton, musicAddSelectedButtonText,
                          true, ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicRemoveSelectedButton,
                          musicRemoveSelectedButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicPlaylistButton, musicPlaylistButtonText, true,
                          ui_theme::primaryAction, ui_theme::primaryActionHover,
                          ui_theme::primaryActionPressed,
                          ui_theme::accentBorderStrong);
  styleThemedActionButton(musicClearPlaylistButton,
                          musicClearPlaylistButtonText, true,
                          ui_theme::warningAction,
                          ui_theme::warningActionHover,
                          ui_theme::warningActionPressed,
                          ui_theme::accentBorder);
  styleThemedActionButton(musicRandomButton, musicRandomButtonText, true,
                          ui_theme::successAction, ui_theme::successActionHover,
                          ui_theme::successActionPressed,
                          ui_theme::accentBorder);
  styleThemedActionButton(musicPreviousButton, musicPreviousButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicSeekBackwardButton,
                          musicSeekBackwardButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicPlayPauseButton, musicPlayPauseButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(musicSeekForwardButton, musicSeekForwardButtonText,
                          true, ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicNextButton, musicNextButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(musicStopButton, musicStopButtonText, true,
                          ui_theme::warningAction, ui_theme::warningActionHover,
                          ui_theme::warningActionPressed,
                          ui_theme::accentBorder);
  styleThemedActionButton(musicCloseButton, musicCloseButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
}

void MainMenuScene::playSelectedChartAsMusic() {
  if (willStart.load() || replayExportInProgress.load() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    musicStatusMessage = "Select a chart first.";
    refreshMusicModal();
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    musicStatusMessage = "Selected chart cannot be played as music.";
    refreshMusicModal();
    return;
  }

  previewLoadCancelled = true;
  if (loadThread.joinable()) {
    loadThread.join();
  }
  context.jukebox.stop();

  MusicTrackRecord musicRecord{.representativeChart = record.meta,
                               .chartCount = 1};
  context.musicPlayer.SetNowPlaying({music_playlist::MakeTrack(musicRecord)});

  std::string statusMessage;
  context.musicPlayer.PlayCurrentAsync(statusMessage, "Playing selected chart.");
  musicStatusMessage = statusMessage;
  refreshMusicModal();
}

void MainMenuScene::addSelectedChartToMusicPlaylist() {
  if (willStart.load() || replayExportInProgress.load() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    musicStatusMessage = "Select a chart first.";
    refreshMusicModal();
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    musicStatusMessage = "Selected chart cannot be added to a playlist.";
    refreshMusicModal();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.AddChartToDefaultPlaylist(record.meta,
                                                    errorMessage)) {
    musicStatusMessage = "Added selected chart to My Playlist.";
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::removeSelectedChartFromMusicPlaylist() {
  if (willStart.load() || replayExportInProgress.load() ||
      recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    musicStatusMessage = "Select a chart first.";
    refreshMusicModal();
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    musicStatusMessage = "Selected chart cannot be removed from a playlist.";
    refreshMusicModal();
    return;
  }

  std::string errorMessage;
  if (context.musicPlayer.RemoveChartFromDefaultPlaylist(record.meta,
                                                         errorMessage)) {
    musicStatusMessage = "Removed selected chart from My Playlist.";
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::playSavedMusicPlaylist() {
  previewLoadCancelled = true;
  if (loadThread.joinable()) {
    loadThread.join();
  }
  context.jukebox.stop();

  std::string errorMessage;
  if (!context.musicPlayer.StartDefaultPlaylist(errorMessage)) {
    musicStatusMessage = errorMessage;
  } else {
    context.musicPlayer.PlayCurrentAsync(errorMessage, "Playing My Playlist.");
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::clearSavedMusicPlaylist() {
  std::string errorMessage;
  if (context.musicPlayer.ClearDefaultPlaylist(errorMessage)) {
    musicStatusMessage = "Cleared My Playlist.";
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::playRandomMusicLibrary() {
  previewLoadCancelled = true;
  if (loadThread.joinable()) {
    loadThread.join();
  }
  context.jukebox.stop();

  std::string errorMessage;
  if (!context.musicPlayer.ReloadLibrary(errorMessage) ||
      !context.musicPlayer.StartRandomLibrary(errorMessage)) {
    musicStatusMessage = errorMessage;
  } else {
    context.musicPlayer.PlayCurrentAsync(errorMessage,
                                         "Playing Now Playing.");
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::toggleMusicPlayback() {
  std::string errorMessage;
  const auto playback = context.musicPlayer.PlaybackState();
  bool ok = false;
  if (playback.playing) {
    ok = context.musicPlayer.Pause(errorMessage);
  } else if (playback.loaded) {
    ok = context.musicPlayer.Resume(errorMessage);
  } else {
    ok = context.musicPlayer.PlayCurrentAsync(errorMessage,
                                              "Playing current track.");
  }
  musicStatusMessage = errorMessage;
  refreshMusicModal();
}

void MainMenuScene::seekMusicRelative(long long deltaMicros) {
  const auto playback = context.musicPlayer.PlaybackState();
  if (!playback.supported || !playback.loaded) {
    musicStatusMessage = "No music is loaded.";
    refreshMusicModal();
    return;
  }

  long long targetMicros = std::max(0LL, playback.positionMicros + deltaMicros);
  if (playback.durationMicros > 0) {
    targetMicros = std::min(targetMicros, playback.durationMicros);
  }

  std::string errorMessage;
  if (context.musicPlayer.Seek(targetMicros, errorMessage)) {
    musicStatusMessage = "Seeked to " + formatMusicTime(targetMicros) + ".";
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::playNextMusicTrack() {
  previewLoadCancelled = true;
  if (loadThread.joinable()) {
    loadThread.join();
  }
  context.jukebox.stop();

  std::string errorMessage;
  context.musicPlayer.PlayNextAsync(errorMessage, "Playing next track.");
  musicStatusMessage = errorMessage;
  refreshMusicModal();
}

void MainMenuScene::playPreviousMusicTrack() {
  previewLoadCancelled = true;
  if (loadThread.joinable()) {
    loadThread.join();
  }
  context.jukebox.stop();

  std::string errorMessage;
  context.musicPlayer.PlayPreviousAsync(errorMessage,
                                        "Playing previous track.");
  musicStatusMessage = errorMessage;
  refreshMusicModal();
}

void MainMenuScene::stopMusicPlayback() {
  std::string errorMessage;
  if (context.musicPlayer.Stop(errorMessage)) {
    musicStatusMessage = "Stopped.";
  } else {
    musicStatusMessage = errorMessage;
  }
  refreshMusicModal();
}

void MainMenuScene::buildTasksModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;

  tasksModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                           rendering::window_height);
  tasksModalRoot->setPositionType(YGPositionTypeAbsolute);
  tasksModalRoot->setPosition(Edge::Left, 0);
  tasksModalRoot->setPosition(Edge::Top, 0);
  tasksModalRoot->setZIndex(1000);
  tasksModalRoot->setVisible(false);
  tasksModalRoot->setFlexDirection(FlexDirection::Column);
  tasksModalRoot->setAlignItems(YGAlignCenter);
  tasksModalRoot->setJustifyContent(YGJustifyCenter);
  tasksModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(560)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Tasks");
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(42);
  panel->addView(title);

  tasksScrollView =
      new ScrollView(0, 0, static_cast<int>(kModalContentWidth), 400);
  tasksScrollView->setWidth(kModalContentWidth);
  tasksScrollView->setFlex(1);
  tasksScrollView->setThemedBackgroundColor(ui_theme::insetSurface);
  tasksScrollView->setCornerRadius(ui_theme::controlRadius());
  tasksScrollView->setThemedBorderColor(ui_theme::hairline);
  tasksScrollView->setBorderWidth(1);

  tasksContent = new View();
  tasksContent->setFlexDirection(FlexDirection::Column);
  tasksContent->setAlignItems(YGAlignStretch);
  tasksContent->setPadding(Edge::All, 12);

  tasksText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  tasksText->setText(tasksModalTextSnapshot());
  tasksText->setThemedColor(ui_theme::textSecondary);
  tasksText->setWrap(true);
  tasksText->setOverflow(TextView::TextOverflow::Visible);
  tasksContent->addView(tasksText);
  tasksScrollView->setContentView(tasksContent);
  panel->addView(tasksScrollView);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  tasksRefreshButton =
      makeModalButton("Refresh List", 18, &tasksRefreshButtonText);
  tasksRefreshButton->setWidth(150);
  tasksRefreshButton->setOnClickListener([this]() {
    requestLibraryScanFlush();
    applyPendingUiUpdates();
    hideTasksModal();
  });
  styleThemedActionButton(tasksRefreshButton, tasksRefreshButtonText, true,
                          ui_theme::successAction, ui_theme::successActionHover,
                          ui_theme::successActionPressed,
                          ui_theme::accentBorder);

  tasksCloseButton = makeModalButton("Close", 20, &tasksCloseButtonText);
  tasksCloseButton->setWidth(130);
  tasksCloseButton->setOnClickListener([this]() { hideTasksModal(); });
  styleThemedActionButton(tasksCloseButton, tasksCloseButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);

  footer->addView(tasksRefreshButton);
  footer->addView(tasksCloseButton);
  panel->addView(footer);

  tasksModalRoot->addView(panel);
  rootLayout->addView(tasksModalRoot);
  displayedLibraryTasksRevision = 0;
  displayedLibraryProgressRevision = 0;
  refreshTasksModal();
}

void MainMenuScene::showTasksModal() {
  if (tasksModalRoot == nullptr) {
    return;
  }
  tasksModalRoot->setSize(rendering::window_width, rendering::window_height);
  tasksModalRoot->setVisible(true);
  displayedLibraryTasksRevision = 0;
  displayedLibraryProgressRevision = 0;
  refreshTasksModal();
}

void MainMenuScene::hideTasksModal() {
  if (tasksModalRoot != nullptr) {
    tasksModalRoot->setVisible(false);
  }
}

void MainMenuScene::refreshTasksModal() {
  if (tasksModalRoot == nullptr || tasksText == nullptr) {
    return;
  }

  std::uint64_t tasksRevision = 0;
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    tasksRevision = libraryTasksRevision;
  }
  const LibraryTaskProgressSnapshot progressSnapshot =
      readLibraryTaskProgress();
  if (tasksRevision == displayedLibraryTasksRevision &&
      progressSnapshot.revision == displayedLibraryProgressRevision) {
    return;
  }
  displayedLibraryTasksRevision = tasksRevision;
  displayedLibraryProgressRevision = progressSnapshot.revision;
  tasksText->setText(tasksModalTextSnapshot());
}

std::string MainMenuScene::tasksModalTextSnapshot() {
  const LibraryTaskProgressSnapshot progressSnapshot =
      readLibraryTaskProgress();
  std::vector<LibraryTaskInfo> activeTasks;
  std::vector<LibraryTaskInfo> recentTasks;
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    activeTasks.reserve(libraryTasks.size());
    recentTasks.reserve(libraryTasks.size());
    for (const auto &task : libraryTasks) {
      if (task.status == LibraryTaskStatus::Queued ||
          task.status == LibraryTaskStatus::Running ||
          task.status == LibraryTaskStatus::Paused) {
        activeTasks.push_back(task);
      } else {
        recentTasks.push_back(task);
      }
    }
  }

  if (activeTasks.empty() && recentTasks.empty()) {
    return "No parsing tasks.";
  }

  std::ostringstream text;
  if (activeTasks.empty()) {
    text << "No active tasks.\n\nRecent tasks\n\n";
  } else {
    text << activeTasks.size()
         << (activeTasks.size() == 1 ? " active task" : " active tasks")
         << "\n\n";
  }

  auto appendTask = [&text, &progressSnapshot](const LibraryTaskInfo &task) {
    std::string statusText;
    switch (task.status) {
    case LibraryTaskStatus::Queued:
      statusText = "Queued";
      break;
    case LibraryTaskStatus::Running:
      statusText = "Running";
      break;
    case LibraryTaskStatus::Complete:
      statusText = "Complete";
      break;
    case LibraryTaskStatus::Failed:
      statusText = "Failed";
      break;
    case LibraryTaskStatus::Paused:
      statusText = "Paused";
      break;
    }

    text << task.title << "\n";
    text << statusText;
    if (task.status == LibraryTaskStatus::Running) {
      if (progressSnapshot.valid && progressSnapshot.taskId == task.id) {
        text << " - " << (progressSnapshot.basisPoints / 100) << "%";
        if (progressSnapshot.total > 0) {
          text << " (" << progressSnapshot.current << " / "
               << progressSnapshot.total << ")";
        }
        text << "\n" << chartScanProgressStageText(progressSnapshot.stage);
      } else {
        text << " - " << static_cast<int>(std::round(task.fraction * 100.0))
             << "%";
        if (task.total > 0) {
          text << " (" << task.current << " / " << task.total << ")";
        }
        if (!task.detail.empty()) {
          text << "\n" << task.detail;
        }
      }
    } else if (!task.detail.empty() && task.detail != statusText) {
      text << "\n" << task.detail;
    }
    text << "\n\n";
  };

  for (const auto &task : activeTasks) {
    appendTask(task);
  }

  if (!activeTasks.empty() && !recentTasks.empty()) {
    text << "Recent tasks\n\n";
  }

  constexpr std::size_t kMaxRecentTasksShown = 8;
  std::size_t shownRecentTasks = 0;
  for (auto it = recentTasks.rbegin();
       it != recentTasks.rend() && shownRecentTasks < kMaxRecentTasksShown;
       ++it, ++shownRecentTasks) {
    appendTask(*it);
  }

  return text.str();
}

void MainMenuScene::buildFindBmsModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;

  findBmsModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                             rendering::window_height);
  findBmsModalRoot->setPositionType(YGPositionTypeAbsolute);
  findBmsModalRoot->setPosition(Edge::Left, 0);
  findBmsModalRoot->setPosition(Edge::Top, 0);
  findBmsModalRoot->setZIndex(1000);
  findBmsModalRoot->setVisible(false);
  findBmsModalRoot->setFlexDirection(FlexDirection::Column);
  findBmsModalRoot->setAlignItems(YGAlignCenter);
  findBmsModalRoot->setJustifyContent(YGJustifyCenter);
  findBmsModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(560)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  findBmsModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  findBmsModalTitleText->setText("Find BMS");
  findBmsModalTitleText->setThemedColor(ui_theme::textPrimary);
  findBmsModalTitleText->setHeight(42);
  panel->addView(findBmsModalTitleText);

  findBmsStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  findBmsStatusText->setText("Preparing lookup");
  findBmsStatusText->setThemedColor(ui_theme::textPrimary);
  findBmsStatusText->setWrap(true);
  findBmsStatusText->setOverflow(TextView::TextOverflow::Hidden);
  findBmsStatusText->setHeight(58);
  panel->addView(findBmsStatusText);

  findBmsDetailText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  findBmsDetailText->setText("");
  findBmsDetailText->setThemedColor(ui_theme::textSecondary);
  findBmsDetailText->setWrap(true);
  findBmsDetailText->setOverflow(TextView::TextOverflow::Hidden);
  findBmsDetailText->setFlex(1);
  panel->addView(findBmsDetailText);

  findBmsLogScrollView =
      new ScrollView(0, 0, static_cast<int>(kModalContentWidth), 112);
  findBmsLogScrollView->setWidth(kModalContentWidth);
  findBmsLogScrollView->setHeight(112);
  findBmsLogScrollView->setThemedBackgroundColor(ui_theme::insetSurface);
  findBmsLogScrollView->setCornerRadius(ui_theme::controlRadius());
  findBmsLogScrollView->setThemedBorderColor(ui_theme::hairline);
  findBmsLogScrollView->setBorderWidth(1);

  findBmsLogContent = new View();
  findBmsLogContent->setFlexDirection(FlexDirection::Column);
  findBmsLogContent->setAlignItems(YGAlignStretch);
  findBmsLogContent->setPadding(Edge::All, 8);

  findBmsLogText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  findBmsLogText->setText("Preparing lookup");
  findBmsLogText->setThemedColor(ui_theme::textSecondary);
  findBmsLogText->setWrap(true);
  findBmsLogText->setOverflow(TextView::TextOverflow::Visible);
  findBmsLogContent->addView(findBmsLogText);
  findBmsLogScrollView->setContentView(findBmsLogContent);
  panel->addView(findBmsLogScrollView);

  findBmsCandidateRecyclerView = new RecyclerView<BmsSearchCandidate>(
      [](const BmsSearchCandidate &a, const BmsSearchCandidate &b) {
        return a.source == b.source && a.id == b.id && a.name == b.name;
      });
  findBmsCandidateRecyclerView->itemHeight = 52;
  findBmsCandidateRecyclerView->reserveScrollbarGutter = true;
  findBmsCandidateRecyclerView->setWidth(kModalContentWidth);
  findBmsCandidateRecyclerView->setHeight(0);
  findBmsCandidateRecyclerView->setThemedBackgroundColor(
      ui_theme::insetSurface);
  findBmsCandidateRecyclerView->setCornerRadius(ui_theme::controlRadius());
  findBmsCandidateRecyclerView->setThemedBorderColor(ui_theme::hairline);
  findBmsCandidateRecyclerView->setBorderWidth(1);
  findBmsCandidateRecyclerView->setVisible(false);
  findBmsCandidateRecyclerView->onCreateView = [](const BmsSearchCandidate &) {
    return new FindBmsCandidateItemView();
  };
  findBmsCandidateRecyclerView->onBind = [](View *view,
                                            const BmsSearchCandidate &candidate,
                                            int idx, bool isSelected) {
    auto *itemView = dynamic_cast<FindBmsCandidateItemView *>(view);
    if (itemView != nullptr) {
      itemView->setCandidate(candidate, static_cast<size_t>(idx), isSelected);
    }
  };
  findBmsCandidateRecyclerView->onSelected = [this](const BmsSearchCandidate &,
                                                    int idx) {
    startFindBmsCandidateDownload(static_cast<size_t>(idx));
  };
  panel->addView(findBmsCandidateRecyclerView);

  findBmsProgressTrack = new View();
  findBmsProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setThemedBackgroundColor(ui_theme::progressTrack)
      ->setCornerRadius(ui_theme::controlRadius())
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);
  findBmsProgressFill = new View();
  findBmsProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      ui_theme::progressFill());
  findBmsProgressTrack->addView(findBmsProgressFill);
  panel->addView(findBmsProgressTrack);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  findBmsCloseButton = makeModalButton("Cancel", 20, &findBmsCloseButtonText);
  findBmsOpenButton =
      makeModalButton("Open BMS Search", 18, &findBmsOpenButtonText);
  findBmsGoogleButton =
      makeModalButton("Search BMS", 18, &findBmsGoogleButtonText);
  findBmsRefreshButton =
      makeModalButton("Refresh List", 18, &findBmsRefreshButtonText);

  findBmsCloseButton->setWidth(130);
  findBmsOpenButton->setWidth(180);
  findBmsGoogleButton->setWidth(150);
  findBmsRefreshButton->setWidth(150);
  findBmsCloseButton->setOnClickListener([this]() {
    if (findBmsJobRunning.load()) {
      findBmsCancelled = true;
      if (findBmsThread.joinable()) {
        findBmsThread.request_stop();
      }
      refreshFindBmsModal();
      return;
    }
    hideFindBmsModal();
  });
  findBmsOpenButton->setOnClickListener([this]() {
    const std::string url = findBmsManualSourceUrl(findBmsResult);
    openFindBmsResultUrl(url);
  });
  findBmsGoogleButton->setOnClickListener([this]() {
    openFindBmsResultUrl(BmsSearchService::searchUrlForText(
        findBmsTitleSearchQuery(findBmsModalChart)));
  });
  findBmsRefreshButton->setOnClickListener([this]() {
    startLibraryRefresh();
    hideFindBmsModal();
  });

  footer->addView(findBmsCloseButton);
  footer->addView(findBmsOpenButton);
  footer->addView(findBmsGoogleButton);
  footer->addView(findBmsRefreshButton);
  panel->addView(footer);

  findBmsModalRoot->addView(panel);
  rootLayout->addView(findBmsModalRoot);
  refreshFindBmsModal();
}

void MainMenuScene::showFindBmsModal(const ChartMetaRecord &record) {
  if (findBmsModalRoot == nullptr) {
    return;
  }
  if (findBmsThread.joinable()) {
    findBmsCancelled = true;
    findBmsThread.request_stop();
    findBmsThread.join();
  }

  findBmsModalChart = record;
  findBmsResult = {};
  if (!record.meta.SHA256.empty()) {
    findBmsResult.patternUrl =
        BmsSearchService::patternUrlForSha256(record.meta.SHA256);
    findBmsResult.fallbackUrl = findBmsResult.patternUrl;
  } else {
    findBmsResult.fallbackUrl =
        BmsSearchService::searchUrlForText(findBmsTitleSearchQuery(record));
  }
  findBmsProgressMessage = "Preparing lookup";
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.02;
  findBmsProgressLog.clear();
  findBmsProgressLog.push_back("Preparing lookup");
  findBmsCancelled = false;
  pendingFindBmsProgressEvents.clear();
  pendingFindBmsResult.reset();

  const std::filesystem::path downloadRoot = preferredBmsDownloadRoot();
  findBmsJobRunning = true;
  findBmsModalRoot->setSize(rendering::window_width, rendering::window_height);
  findBmsModalRoot->setVisible(true);
  refreshFindBmsModal();

  findBmsThread = std::jthread([this, record, downloadRoot](
                                   const std::stop_token &stopToken) {
    BmsSearchService service;
    auto progressCallback = [this](const BmsSearchDownloadProgress &progress) {
      std::lock_guard<std::mutex> lock(findBmsUpdateMutex);
      pendingFindBmsProgressEvents.push_back(progress);
      while (pendingFindBmsProgressEvents.size() >
             kFindBmsMaxPendingProgressEvents) {
        pendingFindBmsProgressEvents.pop_front();
      }
    };
    if (stopToken.stop_requested()) {
      findBmsCancelled = true;
    }
    auto result = service.findAndDownload(
        record.meta.SHA256, record.meta.MD5, downloadRoot, findBmsCancelled,
        progressCallback, record.meta.Title, record.meta.Artist);
    {
      std::lock_guard<std::mutex> lock(findBmsUpdateMutex);
      pendingFindBmsResult = std::move(result);
      findBmsJobRunning = false;
    }
  });
}

void MainMenuScene::startFindBmsCandidateDownload(size_t candidateIndex) {
  if (findBmsJobRunning.load() ||
      candidateIndex >= findBmsResult.candidates.size()) {
    return;
  }
  if (findBmsThread.joinable()) {
    findBmsThread.join();
  }

  const BmsSearchCandidate candidate = findBmsResult.candidates[candidateIndex];
  const ChartMetaRecord record = findBmsModalChart;
  const std::filesystem::path downloadRoot = preferredBmsDownloadRoot();
  findBmsResult = {};
  findBmsResult.candidates = {candidate};
  findBmsProgressMessage = "Preparing Horie archive download";
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.09;
  findBmsProgressLog.clear();
  findBmsProgressLog.push_back("Preparing Horie archive download");
  findBmsCancelled = false;
  pendingFindBmsProgressEvents.clear();
  pendingFindBmsResult.reset();
  findBmsJobRunning = true;
  refreshFindBmsModal();

  findBmsThread = std::jthread([this, candidate, record, downloadRoot](
                                   const std::stop_token &stopToken) {
    BmsSearchService service;
    auto progressCallback = [this](const BmsSearchDownloadProgress &progress) {
      std::lock_guard<std::mutex> lock(findBmsUpdateMutex);
      pendingFindBmsProgressEvents.push_back(progress);
      while (pendingFindBmsProgressEvents.size() >
             kFindBmsMaxPendingProgressEvents) {
        pendingFindBmsProgressEvents.pop_front();
      }
    };
    if (stopToken.stop_requested()) {
      findBmsCancelled = true;
    }
    auto result = service.downloadCandidate(candidate, record.meta.SHA256,
                                            record.meta.MD5, downloadRoot,
                                            findBmsCancelled, progressCallback);
    {
      std::lock_guard<std::mutex> lock(findBmsUpdateMutex);
      pendingFindBmsResult = std::move(result);
      findBmsJobRunning = false;
    }
  });
}

void MainMenuScene::hideFindBmsModal() {
  if (findBmsModalRoot == nullptr || findBmsJobRunning.load()) {
    return;
  }
  findBmsModalRoot->setVisible(false);
}

void MainMenuScene::refreshFindBmsModal() {
  if (findBmsModalRoot == nullptr) {
    return;
  }

  const bool running = findBmsJobRunning.load();
  if (findBmsModalTitleText != nullptr) {
    findBmsModalTitleText->setText("Find BMS");
  }

  std::string statusText =
      running ? findBmsProgressDisplayText(findBmsProgressMessage,
                                           findBmsProgressCurrent,
                                           findBmsProgressTotal, false)
              : findBmsResult.message;
  if (statusText.empty()) {
    statusText = running ? "Searching" : "Lookup finished.";
  }
  if (findBmsStatusText != nullptr) {
    findBmsStatusText->setText(statusText);
    const bool failed =
        !running &&
        (findBmsResult.status == BmsSearchResult::Status::DownloadFailed ||
         findBmsResult.status == BmsSearchResult::Status::HashMismatch ||
         findBmsResult.status == BmsSearchResult::Status::NotFound);
    findBmsStatusText->setColor(
        ui_theme::sdl(failed ? ui_theme::coral() : ui_theme::textPrimary()));
  }

  const bool showCandidateList =
      !running &&
      findBmsResult.status == BmsSearchResult::Status::AmbiguousCandidates &&
      !findBmsResult.candidates.empty();

  std::string detail;
  if (!findBmsModalChart.meta.Title.empty()) {
    detail += findBmsModalChart.meta.Title + "\n";
  }
  if (!findBmsModalChart.meta.SHA256.empty()) {
    detail +=
        "SHA256: " + compactHashForModal(findBmsModalChart.meta.SHA256) + "\n";
  }
  if (!findBmsModalChart.meta.MD5.empty()) {
    detail += "MD5: " + compactHashForModal(findBmsModalChart.meta.MD5) + "\n";
  }
  if (!running && findBmsResult.status == BmsSearchResult::Status::Downloaded) {
    detail += "Saved to " + fspath_to_utf8(findBmsResult.outputPath) +
              "\nRefreshing the library will make newly found charts playable.";
    if (!findBmsResult.debugPath.empty()) {
      detail += "\nDebug files: " + fspath_to_utf8(findBmsResult.debugPath);
    }
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::NoDownloadLink) {
    detail += "Open the BMS Search page to download manually, then refresh the "
              "list.";
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::UnsupportedLink) {
    detail += "A source exists, but this app cannot safely download and "
              "extract it yet. Download manually, then refresh the list.";
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::NotFound) {
    const bool hasSha = !findBmsModalChart.meta.SHA256.empty();
    detail +=
        hasSha ? "No matching BMS Search page was available. Search by title."
               : "Horie did not find a matching song. Search by title.";
  } else if (!running && findBmsResult.status ==
                             BmsSearchResult::Status::AmbiguousCandidates) {
    detail += "Horie found multiple matching archives. Choose one archive to "
              "download.";
    if (!findBmsResult.candidates.empty() &&
        !findBmsResult.candidates.front().query.empty()) {
      detail += "\nQuery: " + findBmsResult.candidates.front().query;
    }
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::HashMismatch) {
    detail += "The archive was extracted, but it does not contain the selected "
              "BMS chart hash.";
    if (!findBmsResult.outputPath.empty()) {
      detail += "\nKept at " + fspath_to_utf8(findBmsResult.outputPath);
    }
    if (!findBmsResult.debugPath.empty()) {
      detail += "\nDebug files: " + fspath_to_utf8(findBmsResult.debugPath);
    }
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::DownloadFailed) {
    detail += "Automatic download failed. Open the source page or refresh "
              "after downloading.";
    if (!findBmsResult.debugPath.empty()) {
      detail += "\nDebug files: " + fspath_to_utf8(findBmsResult.debugPath);
    }
  } else {
    detail +=
        "Checking package sources, BMS Search, then Horie archive if needed.";
  }
  if (findBmsDetailText != nullptr) {
    findBmsDetailText->setText(detail);
  }

  if (findBmsLogScrollView != nullptr) {
    findBmsLogScrollView->setHeight(showCandidateList ? 56.0f : 112.0f);
  }
  if (findBmsLogText != nullptr) {
    std::string logText;
    for (const auto &line : findBmsProgressLog) {
      if (!logText.empty()) {
        logText += "\n";
      }
      logText += "- " + line;
    }
    if (logText.empty()) {
      logText = "- Waiting for progress";
    }
    findBmsLogText->setText(logText);
  }
  if (findBmsLogScrollView != nullptr) {
    findBmsLogScrollView->scrollToBottom();
  }

  if (findBmsCandidateRecyclerView != nullptr) {
    findBmsCandidateRecyclerView->setVisible(showCandidateList);
    const int visibleRows =
        showCandidateList
            ? std::min<int>(static_cast<int>(findBmsResult.candidates.size()),
                            3)
            : 0;
    findBmsCandidateRecyclerView->setHeight(static_cast<float>(
        visibleRows * findBmsCandidateRecyclerView->itemHeight));
    if (showCandidateList) {
      findBmsCandidateRecyclerView->setItems(findBmsResult.candidates);
    } else {
      findBmsCandidateRecyclerView->clear();
    }
  }

  const double fraction =
      (!running && findBmsResult.status == BmsSearchResult::Status::Downloaded)
          ? 1.0
          : findBmsProgressFraction;
  if (findBmsProgressFill != nullptr) {
    findBmsProgressFill->setWidthPercent(
        static_cast<float>(std::clamp(fraction, 0.0, 1.0) * 100.0));
  }

  const std::string manualSourceUrl = findBmsManualSourceUrl(findBmsResult);
  const bool downloaded =
      !running && findBmsResult.status == BmsSearchResult::Status::Downloaded;
  const bool hasSource =
      !manualSourceUrl.empty() &&
      findBmsResult.status != BmsSearchResult::Status::Downloaded &&
      findBmsResult.status != BmsSearchResult::Status::NotFound;
  const bool hasSearchAction =
      !downloaded && (!findBmsModalChart.meta.SHA256.empty() ||
                      !findBmsModalChart.meta.MD5.empty() ||
                      !findBmsModalChart.meta.Title.empty() ||
                      !findBmsModalChart.meta.Artist.empty());
  const bool hasRefreshAction = !running && !downloaded;
  if (findBmsCloseButtonText != nullptr) {
    findBmsCloseButtonText->setText(running ? "Cancel" : "Close");
  }
  if (findBmsOpenButtonText != nullptr) {
    const bool downloadSource =
        (findBmsResult.status == BmsSearchResult::Status::DownloadFailed ||
         findBmsResult.status == BmsSearchResult::Status::HashMismatch) &&
        findBmsResult.fallbackUrl.empty() && !findBmsResult.downloadUrl.empty();
    const bool bmsSearchSource =
        manualSourceUrl.find("bmssearch.net") != std::string::npos;
    findBmsOpenButtonText->setText(
        downloadSource ? "Open Download"
                       : (bmsSearchSource ? "Open BMS Search" : "Open Source"));
  }
  if (findBmsOpenButton != nullptr) {
    findBmsOpenButton->setVisible(!running && hasSource);
    findBmsOpenButton->setWidth((!running && hasSource) ? 180.0f : 0.0f);
  }
  if (findBmsGoogleButton != nullptr) {
    findBmsGoogleButton->setVisible(!running && hasSearchAction);
    findBmsGoogleButton->setWidth((!running && hasSearchAction) ? 150.0f
                                                                : 0.0f);
  }
  if (findBmsRefreshButton != nullptr) {
    findBmsRefreshButton->setVisible(hasRefreshAction);
    findBmsRefreshButton->setWidth(hasRefreshAction ? 150.0f : 0.0f);
  }

  styleThemedActionButton(findBmsCloseButton, findBmsCloseButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
  styleThemedActionButton(findBmsOpenButton, findBmsOpenButtonText,
                          !running && hasSource, ui_theme::infoAction,
                          ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(
      findBmsGoogleButton, findBmsGoogleButtonText, !running && hasSearchAction,
      ui_theme::violetAction, ui_theme::violetActionHover,
      ui_theme::violetActionPressed, ui_theme::violetActionHover);
  styleThemedActionButton(
      findBmsRefreshButton, findBmsRefreshButtonText, hasRefreshAction,
      ui_theme::successAction, ui_theme::successActionHover,
      ui_theme::successActionPressed, ui_theme::accentBorder);
  findBmsModalRoot->applyYogaLayout();
}

void MainMenuScene::applyFindBmsUpdates() {
  std::deque<BmsSearchDownloadProgress> progressEvents;
  std::optional<BmsSearchResult> result;
  {
    std::lock_guard<std::mutex> lock(findBmsUpdateMutex);
    progressEvents = std::move(pendingFindBmsProgressEvents);
    result = std::move(pendingFindBmsResult);
    pendingFindBmsProgressEvents.clear();
    pendingFindBmsResult.reset();
  }

  auto appendLogLine = [this](const std::string &logLine) {
    if (logLine.empty()) {
      return;
    }
    if (!findBmsProgressLog.empty() &&
        shouldReplaceFindBmsLogLine(findBmsProgressLog.back(), logLine)) {
      findBmsProgressLog.back() = logLine;
    } else if (findBmsProgressLog.empty() ||
               findBmsProgressLog.back() != logLine) {
      findBmsProgressLog.push_back(logLine);
    }
    while (findBmsProgressLog.size() > kFindBmsMaxLogLines) {
      findBmsProgressLog.pop_front();
    }
  };

  bool shouldRefresh = false;
  for (const auto &progress : progressEvents) {
    findBmsProgressMessage = progress.message;
    findBmsProgressCurrent = progress.downloadedBytes;
    findBmsProgressTotal = progress.totalBytes;
    findBmsProgressFraction =
        findBmsProgressFractionFor(progress, findBmsProgressFraction);
    appendLogLine(findBmsProgressDisplayText(progress, true));
    shouldRefresh = true;
  }
  if (result) {
    findBmsJobRunning = false;
    findBmsResult = std::move(*result);
    if (findBmsResult.status == BmsSearchResult::Status::Downloaded) {
      findBmsProgressFraction = 1.0;
    }
    if (!findBmsResult.message.empty() &&
        (findBmsProgressLog.empty() ||
         findBmsProgressLog.back() != findBmsResult.message)) {
      appendLogLine(findBmsResult.message);
    }
    if (findBmsResult.status == BmsSearchResult::Status::Downloaded) {
      startLibraryRefresh();
    }
    shouldRefresh = true;
  }
  if (shouldRefresh) {
    refreshFindBmsModal();
  }
}

void MainMenuScene::openFindBmsResultUrl(const std::string &url) {
  std::string errorMessage;
  if (!openExternalUrl(url, errorMessage)) {
    SDL_Log("Failed to open URL %s: %s", url.c_str(), errorMessage.c_str());
  }
}

void MainMenuScene::buildPlayOptionsModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalGridGap = 12.0f;
  constexpr float kPlayOptionColumnWidth =
      (kModalPanelWidth - kModalPanelPadding * 2.0f - kModalGridGap * 3.0f) /
      4.0f;

  playOptionsModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                                 rendering::window_height);
  playOptionsModalRoot->setPositionType(YGPositionTypeAbsolute);
  playOptionsModalRoot->setPosition(Edge::Left, 0);
  playOptionsModalRoot->setPosition(Edge::Top, 0);
  playOptionsModalRoot->setZIndex(1000);
  playOptionsModalRoot->setVisible(false);
  playOptionsModalRoot->setFlexDirection(FlexDirection::Column);
  playOptionsModalRoot->setAlignItems(YGAlignCenter);
  playOptionsModalRoot->setJustifyContent(YGJustifyCenter);
  playOptionsModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(805)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12)
      ->setPadding(Edge::All, 22)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Play Options");
  title->setThemedColor(ui_theme::textPrimary);
  title->setHeight(42);
  panel->addView(title);

  panel->addView(makeModalLabel("Gauge"));

  auto makeGaugeButton = [this](GaugeType type, bool autoShift) {
    TextView *text = nullptr;
    auto *button =
        makeModalButton(gaugeButtonLabel(type, autoShift), 18, &text);
    button->setFlex(1);
    button->setOnClickListener(
        [this, type, autoShift]() { setGaugeSelection(type, autoShift); });
    gaugeSelectionButtons.push_back({
        .button = button,
        .text = text,
        .type = type,
        .autoShift = autoShift,
    });
    return button;
  };

  auto *gaugeRowA = makeModalOptionRow(58);
  gaugeRowA->addView(makeGaugeButton(GaugeType::AssistedEasy, false));
  gaugeRowA->addView(makeGaugeButton(GaugeType::Easy, false));
  gaugeRowA->addView(makeGaugeButton(GaugeType::Normal, false));
  auto *gaugeRowB = makeModalOptionRow(58);
  gaugeRowB->addView(makeGaugeButton(GaugeType::Hard, false));
  gaugeRowB->addView(makeGaugeButton(GaugeType::ExHard, false));
  gaugeRowB->addView(makeGaugeButton(GaugeType::ExHard, true));
  panel->addView(gaugeRowA);
  panel->addView(gaugeRowB);

  panel->addView(makeModalLabel("Play Option"));

  auto makePlayOptionButton = [this](std::string option) {
    TextView *text = nullptr;
    auto *button = makeModalButton(option, 15, &text);
    button->setWidth(kPlayOptionColumnWidth);
    button->setOnClickListener(
        [this, option]() { setPlayOptionSelection(option); });
    playOptionButtons.push_back({
        .button = button,
        .text = text,
        .option = option,
    });
    return button;
  };

  auto *playOptionRowA = makeModalOptionRow(58);
  auto *playOptionRowB = makeModalOptionRow(58);
  auto *playOptionRowC = makeModalOptionRow(58);
  for (size_t i = 0; i < play_options::kPlayOptions.size(); ++i) {
    auto *row =
        i < 4 ? playOptionRowA : (i < 8 ? playOptionRowB : playOptionRowC);
    row->addView(makePlayOptionButton(play_options::kPlayOptions[i]));
  }
  panel->addView(playOptionRowA);
  panel->addView(playOptionRowB);
  panel->addView(playOptionRowC);

  panel->addView(makeModalLabel("Long Note Mode"));

  auto makeLongNoteModeButton = [this](std::string mode) {
    TextView *text = nullptr;
    auto *button = makeModalButton(mode, 18, &text);
    button->setFlex(1);
    button->setOnClickListener(
        [this, mode]() { setLongNoteModeSelection(mode); });
    longNoteModeButtons.push_back({
        .button = button,
        .text = text,
        .mode = mode,
    });
    return button;
  };

  auto *longNoteModeRow = makeModalOptionRow(58);
  for (const char *mode : long_note_mode::kPlayableIds) {
    longNoteModeRow->addView(makeLongNoteModeButton(mode));
  }
  panel->addView(longNoteModeRow);

  panel->addView(makeModalLabel("Assist Option"));

  auto makeAssistOptionButton = [this](std::string option) {
    TextView *text = nullptr;
    auto *button = makeModalButton(option, 18, &text);
    button->setFlex(1);
    button->setOnClickListener(
        [this, option]() { setAssistOptionSelection(option); });
    assistOptionButtons.push_back({
        .button = button,
        .text = text,
        .option = option,
    });
    return button;
  };

  auto *assistOptionRow = makeModalOptionRow(58);
  assistOptionRow->addView(makeAssistOptionButton(assist_options::kOff));
  assistOptionRow->addView(makeAssistOptionButton(assist_options::kDrag));
  panel->addView(assistOptionRow);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setHeight(58);
  playOptionsCloseButton =
      makeModalButton("Close", 20, &playOptionsCloseButtonText);
  playOptionsCloseButton->setOnClickListener(
      [this]() { hidePlayOptionsModal(); });
  footer->addView(playOptionsCloseButton);
  panel->addView(footer);

  playOptionsModalRoot->addView(panel);
  rootLayout->addView(playOptionsModalRoot);
  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  styleThemedActionButton(playOptionsCloseButton, playOptionsCloseButtonText,
                          true, ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed, ui_theme::hairlineStrong);
}

void MainMenuScene::showPlayOptionsModal() {
  if (playOptionsModalRoot == nullptr) {
    return;
  }

  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  playOptionsModalRoot->setSize(rendering::window_width,
                                rendering::window_height);
  playOptionsModalRoot->setVisible(true);
  playOptionsModalRoot->applyYogaLayout();
}

void MainMenuScene::hidePlayOptionsModal() {
  if (playOptionsModalRoot == nullptr) {
    return;
  }
  playOptionsModalRoot->setVisible(false);
}

void MainMenuScene::buildReplayModal() {
  if (rootLayout == nullptr) {
    return;
  }

  constexpr float kModalPanelWidth = 760.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;
  constexpr float kModalContentHeight = 418.0f;

  replayModalRoot = new BlockingOverlayView(0, 0, rendering::window_width,
                                            rendering::window_height);
  replayModalRoot->setPositionType(YGPositionTypeAbsolute);
  replayModalRoot->setPosition(Edge::Left, 0);
  replayModalRoot->setPosition(Edge::Top, 0);
  replayModalRoot->setZIndex(1000);
  replayModalRoot->setVisible(false);
  replayModalRoot->setFlexDirection(FlexDirection::Column);
  replayModalRoot->setAlignItems(YGAlignCenter);
  replayModalRoot->setJustifyContent(YGJustifyCenter);
  replayModalRoot->setThemedBackgroundColor(ui_theme::scrim);

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(620)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, 22)
      ->setThemedBackgroundColor(ui_theme::panelStrong)
      ->setCornerRadius(ui_theme::panelRadius())
      ->setThemedShadow(ui_theme::shadow, ui_theme::kModalShadow)
      ->setThemedBorderColor(modalPanelBorder)
      ->setBorderWidth(1);

  replayModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  replayModalTitleText->setText("Replay");
  replayModalTitleText->setThemedColor(ui_theme::textPrimary);
  replayModalTitleText->setHeight(42);
  panel->addView(replayModalTitleText);

  replayModalContentFrame = new View();
  replayModalContentFrame->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setFlexShrink(0);
  panel->addView(replayModalContentFrame);

  replayListContent = new View();
  replayListContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setGap(10);
  replayListView = new ReplaySummaryListView();
  replayListView->onSelectionChanged = [this](int idx) {
    selectedReplayIndex = idx;
    if (selectedReplayIsAutoPlay()) {
      selectedReplayRenderTouchPoints = false;
      selectedReplayRenderGhosts = false;
      refreshReplayExportOptionButtons();
    }
    refreshReplayModalActions();
  };
  replayListView->setFlex(1);
  replayListView->clearBackgroundColor();
  replayListView->setThemedBorderColor(ui_theme::hairline);
  replayListView->setBorderWidth(1);
  replayListContent->addView(replayListView);
  auto *replayTouchRow = makeModalOptionRow(52.0f);
  auto *replayTouchLabel = makeModalLabel("Touch Points");
  replayTouchLabel->setWidth(180);
  replayTouchLabel->setHeight(52);
  replayTouchLabel->setVAlign(TextView::MIDDLE);
  replayTouchShowButton =
      makeModalButton("Show", 18, &replayTouchShowButtonText);
  replayTouchHideButton =
      makeModalButton("Hide", 18, &replayTouchHideButtonText);
  replayTouchShowButton->setFlex(1);
  replayTouchHideButton->setFlex(1);
  replayTouchShowButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || selectedReplayIsAutoPlay()) {
      return;
    }
    selectedReplayRenderTouchPoints = true;
    refreshReplayExportOptionButtons();
  });
  replayTouchHideButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || selectedReplayIsAutoPlay()) {
      return;
    }
    selectedReplayRenderTouchPoints = false;
    refreshReplayExportOptionButtons();
  });
  replayTouchRow->addView(replayTouchLabel);
  replayTouchRow->addView(replayTouchShowButton);
  replayTouchRow->addView(replayTouchHideButton);
  replayListContent->addView(replayTouchRow);
  auto *replayGhostRow = makeModalOptionRow(52.0f);
  auto *replayGhostLabel = makeModalLabel("Ghosts");
  replayGhostLabel->setWidth(180);
  replayGhostLabel->setHeight(52);
  replayGhostLabel->setVAlign(TextView::MIDDLE);
  replayGhostShowButton =
      makeModalButton("Show", 18, &replayGhostShowButtonText);
  replayGhostHideButton =
      makeModalButton("Hide", 18, &replayGhostHideButtonText);
  replayGhostShowButton->setFlex(1);
  replayGhostHideButton->setFlex(1);
  replayGhostShowButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || selectedReplayIsAutoPlay()) {
      return;
    }
    selectedReplayRenderGhosts = true;
    refreshReplayExportOptionButtons();
  });
  replayGhostHideButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || selectedReplayIsAutoPlay()) {
      return;
    }
    selectedReplayRenderGhosts = false;
    refreshReplayExportOptionButtons();
  });
  replayGhostRow->addView(replayGhostLabel);
  replayGhostRow->addView(replayGhostShowButton);
  replayGhostRow->addView(replayGhostHideButton);
  replayListContent->addView(replayGhostRow);
  replayModalContentFrame->addView(replayListContent);

  replayExportOptionsContent = new View();
  replayExportOptionsContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(6);
  replayExportOptionsContent->setVisible(false);

  replayExportOptionsContent->addView(makeModalLabel("Frame Rate"));
  auto *fpsRow = makeModalOptionRow();
  replayFps60Button = makeModalButton("60 fps", 20, &replayFps60ButtonText);
  replayFps120Button = makeModalButton("120 fps", 20, &replayFps120ButtonText);
  replayFps60Button->setFlex(1);
  replayFps120Button->setFlex(1);
  replayFps60Button->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    selectedExportFps = 60;
    refreshReplayExportOptionButtons();
  });
  replayFps120Button->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    selectedExportFps = 120;
    refreshReplayExportOptionButtons();
  });
  fpsRow->addView(replayFps60Button);
  fpsRow->addView(replayFps120Button);
  replayExportOptionsContent->addView(fpsRow);

  replayExportOptionsContent->addView(makeModalLabel("Resolution"));
  auto *resolutionRow = makeModalOptionRow();
  replayResolution1080Button =
      makeModalButton("1080p", 20, &replayResolution1080ButtonText);
  replayResolutionFullButton =
      makeModalButton("Full Resolution", 20, &replayResolutionFullButtonText);
  replayResolution1080Button->setFlex(1);
  replayResolutionFullButton->setFlex(1);
  replayResolution1080Button->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    selectedExportFullResolution = false;
    refreshReplayExportOptionButtons();
  });
  replayResolutionFullButton->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    selectedExportFullResolution = true;
    refreshReplayExportOptionButtons();
  });
  resolutionRow->addView(replayResolution1080Button);
  resolutionRow->addView(replayResolutionFullButton);
  replayExportOptionsContent->addView(resolutionRow);

  replayExportOptionsContent->addView(makeModalLabel("Result Screen"));
  auto *resultRow = makeModalOptionRow();
  replayResultIncludeButton =
      makeModalButton("Include", 20, &replayResultIncludeButtonText);
  replayResultSkipButton =
      makeModalButton("Skip", 20, &replayResultSkipButtonText);
  replayResultIncludeButton->setFlex(1);
  replayResultSkipButton->setFlex(1);
  replayResultIncludeButton->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    selectedExportIncludeResultScreen = true;
    refreshReplayExportOptionButtons();
  });
  replayResultSkipButton->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    selectedExportIncludeResultScreen = false;
    refreshReplayExportOptionButtons();
  });
  resultRow->addView(replayResultIncludeButton);
  resultRow->addView(replayResultSkipButton);
  replayExportOptionsContent->addView(resultRow);

  auto *exportTouchRow = makeModalOptionRow();
  auto *exportTouchLabel = makeModalLabel("Touch Points");
  exportTouchLabel->setWidth(180);
  exportTouchLabel->setHeight(58);
  exportTouchLabel->setVAlign(TextView::MIDDLE);
  replayExportTouchShowButton =
      makeModalButton("Show", 18, &replayExportTouchShowButtonText);
  replayExportTouchHideButton =
      makeModalButton("Hide", 18, &replayExportTouchHideButtonText);
  replayExportTouchShowButton->setFlex(1);
  replayExportTouchHideButton->setFlex(1);
  replayExportTouchShowButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || selectedReplayIsAutoPlay()) {
      return;
    }
    selectedReplayRenderTouchPoints = true;
    refreshReplayExportOptionButtons();
  });
  replayExportTouchHideButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || selectedReplayIsAutoPlay()) {
      return;
    }
    selectedReplayRenderTouchPoints = false;
    refreshReplayExportOptionButtons();
  });
  exportTouchRow->addView(exportTouchLabel);
  exportTouchRow->addView(replayExportTouchShowButton);
  exportTouchRow->addView(replayExportTouchHideButton);
  replayExportOptionsContent->addView(exportTouchRow);

  auto *exportGhostRow = makeModalOptionRow();
  auto *exportGhostLabel = makeModalLabel("Ghosts");
  exportGhostLabel->setWidth(180);
  exportGhostLabel->setHeight(58);
  exportGhostLabel->setVAlign(TextView::MIDDLE);
  replayExportGhostShowButton =
      makeModalButton("Show", 18, &replayExportGhostShowButtonText);
  replayExportGhostHideButton =
      makeModalButton("Hide", 18, &replayExportGhostHideButtonText);
  replayExportGhostShowButton->setFlex(1);
  replayExportGhostHideButton->setFlex(1);
  replayExportGhostShowButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || selectedReplayIsAutoPlay()) {
      return;
    }
    selectedReplayRenderGhosts = true;
    refreshReplayExportOptionButtons();
  });
  replayExportGhostHideButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || selectedReplayIsAutoPlay()) {
      return;
    }
    selectedReplayRenderGhosts = false;
    refreshReplayExportOptionButtons();
  });
  exportGhostRow->addView(exportGhostLabel);
  exportGhostRow->addView(replayExportGhostShowButton);
  exportGhostRow->addView(replayExportGhostHideButton);
  replayExportOptionsContent->addView(exportGhostRow);
  replayModalContentFrame->addView(replayExportOptionsContent);

  replayExportProgressContent = new View();
  replayExportProgressContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setJustifyContent(YGJustifyCenter)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setGap(18);
  replayExportProgressContent->setVisible(false);

  replayExportProgressMessageText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  replayExportProgressMessageText->setText("Preparing export");
  replayExportProgressMessageText->setColor(
      ui_theme::sdl(ui_theme::textPrimary()));
  replayExportProgressMessageText->setHeight(38);
  replayExportProgressContent->addView(replayExportProgressMessageText);

  replayExportProgressTrack = new View();
  replayExportProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setThemedBackgroundColor(ui_theme::progressTrack)
      ->setCornerRadius(ui_theme::controlRadius())
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);
  replayExportProgressFill = new View();
  replayExportProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      ui_theme::progressFill());
  replayExportProgressTrack->addView(replayExportProgressFill);
  replayExportProgressContent->addView(replayExportProgressTrack);

  replayExportProgressPercentText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  replayExportProgressPercentText->setText("0%");
  replayExportProgressPercentText->setColor(
      ui_theme::sdl(ui_theme::textSecondary()));
  replayExportProgressPercentText->setHeight(34);
  replayExportProgressPercentText->setAlign(TextView::RIGHT);
  replayExportProgressContent->addView(replayExportProgressPercentText);
  replayModalContentFrame->addView(replayExportProgressContent);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  replayModalCloseButton =
      makeModalButton("Close", 20, &replayModalCloseButtonText);
  replayWatchButton = makeModalButton("Watch", 20, &replayWatchButtonText);
  replayModalPhotoButton =
      makeModalButton("Export Photo", 18, &replayModalPhotoButtonText);
  replayModalExportButton =
      makeModalButton("Export Video", 18, &replayModalExportButtonText);
  replayModalCloseButton->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    if (replayExportOptionsContent != nullptr &&
        replayExportOptionsContent->getVisible()) {
      replayModalTitleText->setText("Replay");
      replayExportOptionsContent->setVisible(false);
      replayListContent->setVisible(true);
      const int previousSelection = selectedReplayIndex;
      const float previousScrollOffset = replayListView->scrollOffset;
      replayListView->setReplaySummaries(replaySummaries);
      replayListView->scrollOffset = previousScrollOffset;
      selectedReplayIndex =
          previousSelection >= 0 &&
                  previousSelection < static_cast<int>(replaySummaries.size())
              ? previousSelection
              : -1;
      replayListView->restoreSelection(selectedReplayIndex);
      refreshReplayModalActions();
      return;
    }
    hideReplayModal();
  });
  replayWatchButton->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    if (selectedReplayIndex < 0 ||
        selectedReplayIndex >= static_cast<int>(replaySummaries.size())) {
      return;
    }
    startReplayPlayback(replayModalChart,
                        replaySummaries[selectedReplayIndex].id);
  });
  replayModalPhotoButton->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    if (selectedReplayIsAutoPlay()) {
      return;
    }
    if (selectedReplayIndex < 0 ||
        selectedReplayIndex >= static_cast<int>(replaySummaries.size())) {
      return;
    }
    startReplayImageExport(replayModalChart,
                           replaySummaries[selectedReplayIndex].id);
  });
  replayModalExportButton->setOnClickListener([this]() {
    if (replayExportInProgress.load()) {
      return;
    }
    if (selectedReplayIndex < 0 ||
        selectedReplayIndex >= static_cast<int>(replaySummaries.size())) {
      return;
    }
    if (replayExportOptionsContent != nullptr &&
        replayExportOptionsContent->getVisible()) {
      ReplayVideoExportOptions options;
      options.fps = selectedExportFps;
      options.includeResultScreen = selectedExportIncludeResultScreen;
      options.renderTouchPoints =
          selectedReplayIsAutoPlay() ? false : selectedReplayRenderTouchPoints;
      options.renderReplayGhosts =
          selectedReplayIsAutoPlay() ? false : selectedReplayRenderGhosts;
      if (!selectedExportFullResolution) {
        options.height = 1080;
      }
      startReplayVideoExport(replayModalChart,
                             replaySummaries[selectedReplayIndex].id, options);
      return;
    }
    showReplayExportOptions();
  });
  footer->addView(replayModalCloseButton);
  footer->addView(replayWatchButton);
  footer->addView(replayModalPhotoButton);
  footer->addView(replayModalExportButton);
  panel->addView(footer);

  replayModalRoot->addView(panel);
  rootLayout->addView(replayModalRoot);
  refreshReplayExportOptionButtons();
  refreshReplayModalActions();
}

void MainMenuScene::showReplayListModal(const ChartMetaRecord &record) {
  if (replayModalRoot == nullptr || replayListView == nullptr) {
    return;
  }

  replayModalChart = record;
  const bool courseReplayList =
      record.courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0;
  if (courseReplayList) {
    replaySummaries =
        ReplayDBHelper::GetInstance().ListCourseReplays(activeFolder.courseId);
  } else {
    replaySummaries = ReplayDBHelper::GetInstance().ListReplays(record.meta);
    replaySummaries.insert(replaySummaries.begin(),
                           autoPlayReplaySummary(record));
  }
  setReplayButtonVisible(true);

  selectedReplayIndex = -1;
  selectedReplayRenderTouchPoints = context.settings.touchVisualizationEnabled;
  selectedReplayRenderGhosts = true;
  replayModalTitleText->setText("Replay");
  replayListContent->setVisible(true);
  replayExportOptionsContent->setVisible(false);
  replayExportProgressContent->setVisible(false);
  replayListView->setReplaySummaries(replaySummaries);
  replayModalRoot->setSize(rendering::window_width, rendering::window_height);
  replayModalRoot->setVisible(true);
  refreshReplayExportOptionButtons();
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayoutFromRoot();
}

void MainMenuScene::showReplayExportOptions() {
  if (replayModalRoot == nullptr || selectedReplayIndex < 0 ||
      selectedReplayIndex >= static_cast<int>(replaySummaries.size())) {
    return;
  }

  replayModalTitleText->setText("Export Options");
  replayListContent->setVisible(false);
  replayExportOptionsContent->setVisible(true);
  replayExportProgressContent->setVisible(false);
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  selectedExportIncludeResultScreen = true;
  if (selectedReplayIsAutoPlay()) {
    selectedReplayRenderTouchPoints = false;
    selectedReplayRenderGhosts = false;
  }
  refreshReplayExportOptionButtons();
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayoutFromRoot();
}

void MainMenuScene::showReplayExportProgress(const std::string &title,
                                             const std::string &message) {
  if (replayModalRoot == nullptr) {
    return;
  }

  replayModalTitleText->setText(title);
  replayListContent->setVisible(false);
  replayExportOptionsContent->setVisible(false);
  replayExportProgressContent->setVisible(true);
  updateReplayExportProgressUi(0.0, message);
  replayModalRoot->setSize(rendering::window_width, rendering::window_height);
  replayModalRoot->setVisible(true);
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayoutFromRoot();
}

void MainMenuScene::hideReplayModal() {
  if (replayModalRoot == nullptr) {
    return;
  }
  if (replayExportInProgress.load()) {
    return;
  }
  replayModalRoot->setVisible(false);
  selectedReplayIndex = -1;
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Watch");
  }
  if (replayModalPhotoButtonText != nullptr) {
    replayModalPhotoButtonText->setText("Export Photo");
  }
  if (replayModalExportButtonText != nullptr) {
    replayModalExportButtonText->setText("Export Video");
  }
}

void MainMenuScene::refreshReplayModalActions() {
  const bool hasSelection =
      selectedReplayIndex >= 0 &&
      selectedReplayIndex < static_cast<int>(replaySummaries.size());
  const bool optionsMode = replayExportOptionsContent != nullptr &&
                           replayExportOptionsContent->getVisible();
  const bool progressMode = replayExportProgressContent != nullptr &&
                            replayExportProgressContent->getVisible();
  const bool exportInProgress = replayExportInProgress.load();
  const bool autoPlaySelection = selectedReplayIsAutoPlay();
  const bool courseReplaySelection = selectedReplayIsCourseReplay();

  if (replayModalCloseButtonText != nullptr) {
    replayModalCloseButtonText->setText(optionsMode ? "Back" : "Close");
  }
  if (replayModalPhotoButtonText != nullptr) {
    replayModalPhotoButtonText->setText(
        autoPlaySelection ? "No Photo"
                          : (courseReplaySelection ? "Export Photos"
                                                   : "Export Photo"));
  }
  if (replayModalExportButtonText != nullptr) {
    replayModalExportButtonText->setText(exportInProgress ? "Exporting"
                                                          : "Export Video");
  }

  if (replayWatchButton != nullptr) {
    replayWatchButton->setVisible(!optionsMode && !progressMode);
    replayWatchButton->setWidth((optionsMode || progressMode) ? 0.0f : 160.0f);
  }
  if (replayModalPhotoButton != nullptr) {
    replayModalPhotoButton->setVisible(!optionsMode && !progressMode);
    replayModalPhotoButton->setWidth((optionsMode || progressMode) ? 0.0f
                                                                   : 160.0f);
  }
  if (replayModalExportButton != nullptr) {
    replayModalExportButton->setVisible(!progressMode);
    replayModalExportButton->setWidth(progressMode ? 0.0f : 160.0f);
  }

  styleThemedActionButton(replayModalCloseButton, replayModalCloseButtonText,
                          !exportInProgress, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  styleThemedActionButton(replayWatchButton, replayWatchButtonText,
                          hasSelection && !optionsMode && !progressMode &&
                              !exportInProgress,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(replayModalPhotoButton, replayModalPhotoButtonText,
                          hasSelection && !optionsMode && !progressMode &&
                              !exportInProgress && !autoPlaySelection,
                          ui_theme::successAction,
                          ui_theme::successActionHover,
                          ui_theme::successActionPressed, ui_theme::lime);
  styleThemedActionButton(replayModalExportButton, replayModalExportButtonText,
                          hasSelection && !progressMode && !exportInProgress,
                          ui_theme::violetAction, ui_theme::violetActionHover,
                          ui_theme::violetActionPressed,
                          ui_theme::violetActionHover);

  if (replayModalRoot != nullptr) {
    replayModalRoot->applyYogaLayoutFromRoot();
  }
}

void MainMenuScene::refreshReplayExportOptionButtons() {
  const bool autoPlaySelection = selectedReplayIsAutoPlay();
  if (autoPlaySelection) {
    selectedReplayRenderTouchPoints = false;
    selectedReplayRenderGhosts = false;
  }

  styleOptionButton(replayFps60Button, replayFps60ButtonText,
                    selectedExportFps == 60);
  styleOptionButton(replayFps120Button, replayFps120ButtonText,
                    selectedExportFps == 120);
  styleOptionButton(replayResolution1080Button, replayResolution1080ButtonText,
                    !selectedExportFullResolution);
  styleOptionButton(replayResolutionFullButton, replayResolutionFullButtonText,
                    selectedExportFullResolution);
  styleOptionButton(replayResultIncludeButton, replayResultIncludeButtonText,
                    selectedExportIncludeResultScreen);
  styleOptionButton(replayResultSkipButton, replayResultSkipButtonText,
                    !selectedExportIncludeResultScreen);
  if (autoPlaySelection) {
    styleThemedActionButton(replayTouchShowButton, replayTouchShowButtonText,
                            false, ui_theme::control, ui_theme::controlHover,
                            ui_theme::controlPressed,
                            ui_theme::hairlineStrong);
    styleOptionButton(replayTouchHideButton, replayTouchHideButtonText, true);
    styleThemedActionButton(replayExportTouchShowButton,
                            replayExportTouchShowButtonText, false,
                            ui_theme::control, ui_theme::controlHover,
                            ui_theme::controlPressed,
                            ui_theme::hairlineStrong);
    styleOptionButton(replayExportTouchHideButton,
                      replayExportTouchHideButtonText, true);
    styleThemedActionButton(replayGhostShowButton, replayGhostShowButtonText,
                            false, ui_theme::control, ui_theme::controlHover,
                            ui_theme::controlPressed,
                            ui_theme::hairlineStrong);
    styleOptionButton(replayGhostHideButton, replayGhostHideButtonText, true);
    styleThemedActionButton(replayExportGhostShowButton,
                            replayExportGhostShowButtonText, false,
                            ui_theme::control, ui_theme::controlHover,
                            ui_theme::controlPressed,
                            ui_theme::hairlineStrong);
    styleOptionButton(replayExportGhostHideButton,
                      replayExportGhostHideButtonText, true);
    return;
  }

  styleOptionButton(replayTouchShowButton, replayTouchShowButtonText,
                    selectedReplayRenderTouchPoints);
  styleOptionButton(replayTouchHideButton, replayTouchHideButtonText,
                    !selectedReplayRenderTouchPoints);
  styleOptionButton(replayExportTouchShowButton,
                    replayExportTouchShowButtonText,
                    selectedReplayRenderTouchPoints);
  styleOptionButton(replayExportTouchHideButton,
                    replayExportTouchHideButtonText,
                    !selectedReplayRenderTouchPoints);
  styleOptionButton(replayGhostShowButton, replayGhostShowButtonText,
                    selectedReplayRenderGhosts);
  styleOptionButton(replayGhostHideButton, replayGhostHideButtonText,
                    !selectedReplayRenderGhosts);
  styleOptionButton(replayExportGhostShowButton,
                    replayExportGhostShowButtonText,
                    selectedReplayRenderGhosts);
  styleOptionButton(replayExportGhostHideButton,
                    replayExportGhostHideButtonText,
                    !selectedReplayRenderGhosts);
}

void MainMenuScene::updateReplayExportProgressUi(double fraction,
                                                 const std::string &message) {
  replayExportProgressFraction = std::clamp(fraction, 0.0, 1.0);
  const int displayedPercent =
      static_cast<int>(std::lround(replayExportProgressFraction * 100.0));
  if (replayExportProgressMessageText != nullptr) {
    replayExportProgressMessageText->setText(message);
  }
  if (replayExportProgressPercentText != nullptr) {
    replayExportProgressPercentText->setText(std::to_string(displayedPercent) +
                                             "%");
  }
  if (replayExportProgressFill != nullptr) {
    replayExportProgressFill->setWidthPercent(
        static_cast<float>(displayedPercent));
  }
  if (replayModalRoot != nullptr) {
    replayModalRoot->applyYogaLayoutFromRoot();
  }
}

bool MainMenuScene::selectedReplayIsAutoPlay() const {
  return selectedReplayIndex >= 0 &&
         selectedReplayIndex < static_cast<int>(replaySummaries.size()) &&
         replaySummaries[selectedReplayIndex].autoPlay;
}

bool MainMenuScene::selectedReplayIsCourseReplay() const {
  return selectedReplayIndex >= 0 &&
         selectedReplayIndex < static_cast<int>(replaySummaries.size()) &&
         replaySummaries[selectedReplayIndex].courseReplay;
}

bms_parser::ChartMeta
MainMenuScene::replayLoadMetaForRecord(const ChartMetaRecord &record) const {
  bms_parser::ChartMeta meta = record.meta;
  if (normalizeChartLongNoteModeValue(meta.LnMode) == 0) {
    meta.LnMode = long_note_mode::valueFromId(selectedLnMode);
  }
  return meta;
}

ReplaySummary
MainMenuScene::autoPlayReplaySummary(const ChartMetaRecord &record) const {
  std::optional<std::string> playOption;
  if (!play_options::isNormalPlayOption(selectedPlayOption)) {
    playOption = selectedPlayOption;
  }
  return replay_autoplay::BuildSummary(
      replayLoadMetaForRecord(record), selectedGaugeType,
      selectedGaugeAutoShift, playOption, std::nullopt, std::nullopt,
      std::nullopt, selectedAssistOption);
}

bool MainMenuScene::prepareAutoPlayChartForRecord(
    const ChartMetaRecord &record,
    std::unique_ptr<bms_parser::Chart> &preparedChart,
    play_options::PlayOptionReplayInfo &playInfo,
    std::atomic_bool &parseCancelled) const {
  if (record.solidArchive || record.unavailable || record.meta.BmsPath.empty()) {
    return false;
  }

  const SelectedChartRandomInfo chartRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);
  try {
    preparedChart = play_options::parseChart(
        record.meta.BmsPath, chartRandomInfo.seed, chartRandomInfo.prng,
        chartRandomInfo.values, parseCancelled, "autoplay");
  } catch (const std::exception &e) {
    SDL_Log("Error parsing %s for autoplay: %s",
            fspath_to_utf8(record.meta.BmsPath).c_str(), e.what());
    archive_file::appendDebugLogLine(
        "Autoplay parse exception: " + fspath_to_utf8(record.meta.BmsPath) +
        ": " + e.what());
  }
  if (preparedChart == nullptr || parseCancelled) {
    return false;
  }

  playInfo =
      play_options::applySelectedPlayOptions(*preparedChart, selectedPlayOption);
  applyEffectiveLongNoteModeToChart(
      *preparedChart, long_note_mode::valueFromId(selectedLnMode));
  return true;
}

void MainMenuScene::startReplayPlayback(const ChartMetaRecord &record,
                                        int replayId) {
  if (record.courseStart) {
    startCourseReplayPlayback(record, replayId);
    return;
  }

  if (willStart.load()) {
    return;
  }

  willStart.store(true);
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Loading...");
  }

  defer(
      [this, record, replayId]() {
        auto failReplayLoad = [this]() {
          resetReplayWatchLoadingUi();
          return true;
        };
        if (loadThread.joinable()) {
          loadThread.join();
        }

        if (replay_autoplay::isAutoPlayReplayId(replayId)) {
          std::atomic_bool parseCancelled = false;
          std::unique_ptr<bms_parser::Chart> autoPlayChart;
          play_options::PlayOptionReplayInfo playInfo;
          if (!prepareAutoPlayChartForRecord(record, autoPlayChart, playInfo,
                                             parseCancelled)) {
            return failReplayLoad();
          }

          context.jukebox.stop();
          context.jukebox.loadChart(*autoPlayChart, true, parseCancelled);
          if (parseCancelled) {
            return failReplayLoad();
          }

          auto *chart = setSelectedChart(std::move(autoPlayChart), true, false);
          if (chart == nullptr) {
            return failReplayLoad();
          }

          hideReplayModal();
          changeToGameplayScene(chart,
                                {
                                    .startPosition = 0,
                                    .autoKeySound = true,
                                    .autoPlay = true,
                                    .gaugeType = selectedGaugeType,
                                    .gaugeAutoShift = selectedGaugeAutoShift,
                                    .playOption = playInfo.option,
                                    .playOptionSeed = playInfo.seed,
                                    .playOption2 = playInfo.option2,
                                    .playOption2Seed = playInfo.seed2,
                                    .longNoteMode =
                                        long_note_mode::valueFromId(selectedLnMode),
                                    .assistOption = selectedAssistOption,
                                    .touchVisualizationEnabled = false,
                                    .replayGhostRenderingEnabled = false,
                                });
          willStart.store(false);
          return true;
        }

        auto replay = ReplayDBHelper::GetInstance().LoadReplay(
            replayId, replayLoadMetaForRecord(record));
        if (!replay.has_value()) {
          resetReplayWatchLoadingUi();
          refreshReplayAvailability(&record);
          return true;
        }

        std::atomic_bool parseCancelled = false;
        auto replayChart = play_options::prepareReplayChart(
            record.meta.BmsPath, replay.value(), parseCancelled);
        if (replayChart == nullptr || parseCancelled) {
          return failReplayLoad();
        }

        context.jukebox.stop();
        context.jukebox.loadChart(*replayChart, true, parseCancelled);
        if (parseCancelled) {
          return failReplayLoad();
        }

        auto *chart = setSelectedChart(std::move(replayChart), true, false);
        if (chart == nullptr) {
          return failReplayLoad();
        }

        auto replayData =
            std::make_shared<ReplayData>(std::move(replay.value()));
        context.jukebox.stop();
        hideReplayModal();
        changeToGameplayScene(chart,
                              {
                                  .startPosition = 0,
                                  .autoKeySound = false,
                                  .autoPlay = false,
                                  .gaugeType = replayData->initialGaugeType,
                                  .gaugeAutoShift = replayData->gaugeAutoShift,
                                  .replayData = replayData,
                                  .touchVisualizationEnabled =
                                      selectedReplayRenderTouchPoints,
                                  .replayGhostRenderingEnabled =
                                      selectedReplayRenderGhosts,
                              });
        willStart.store(false);
        return true;
      },
      0, true);
}

void MainMenuScene::startCourseReplayPlayback(const ChartMetaRecord &record,
                                              int replayId) {
  (void)record;
  if (willStart.load()) {
    return;
  }

  willStart.store(true);
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Loading...");
  }

  defer(
      [this, replayId]() {
        auto failReplayLoad = [this]() {
          resetReplayWatchLoadingUi();
          return true;
        };
        if (loadThread.joinable()) {
          loadThread.join();
        }

        auto replay = ReplayDBHelper::GetInstance().LoadCourseReplay(replayId);
        if (!replay.has_value() || replay->stages.empty()) {
          return failReplayLoad();
        }

        auto replayData =
            std::make_shared<CourseReplayData>(std::move(*replay));
        auto session = std::make_shared<CoursePlaySession>();
        session->courseId = replayData->courseId;
        session->courseName = replayData->courseName;
        session->courseGroupName = replayData->courseGroupName;
        session->constraintJson = replayData->constraintJson;
        session->entries.reserve(replayData->stages.size());
        for (const auto &stage : replayData->stages) {
          session->entries.push_back(CoursePlayEntry{.meta = stage.replay.chartMeta});
        }
        const CourseConstraintSettings constraintSettings =
            courseConstraintSettingsFromJson(replayData->constraintJson);
        session->currentIndex = 0;
        session->gaugeType = replayData->initialGaugeType;
        session->gaugeProfile = replayData->gaugeProfile;
        session->gaugeAutoShift = replayData->gaugeAutoShift;
        session->longNoteMode = replayData->longNoteMode;
        session->constraints = constraintSettings.rules;
        session->requestedPlayOption = replayData->requestedPlayOption;
        session->assistOption = replayData->assistOption;
        session->autoKeySound = false;
        session->courseReplayPlayback = true;
        session->courseReplayData = std::move(replayData);
        session->replayTouchVisualizationEnabled =
            selectedReplayRenderTouchPoints;
        session->replayGhostRenderingEnabled = selectedReplayRenderGhosts;

        hideReplayModal();
        startCourseReplayDirect(std::move(session));
        return true;
      },
      0, true);
}

void MainMenuScene::startCourseReplayDirect(
    std::shared_ptr<CoursePlaySession> session) {
  if (session == nullptr ||
      !session->hasCourseReplayStage(session->currentIndex)) {
    resetReplayWatchLoadingUi();
    return;
  }

  auto stageReplay = session->currentCourseReplayStageReplay();
  if (stageReplay == nullptr) {
    resetReplayWatchLoadingUi();
    return;
  }
  session->applyReplayStagePlayOptions(*stageReplay);
  std::atomic_bool parseCancelled = false;
  auto replayChart = play_options::prepareReplayChart(
      stageReplay->chartMeta.BmsPath, *stageReplay, parseCancelled);
  if (replayChart == nullptr || parseCancelled) {
    resetReplayWatchLoadingUi();
    return;
  }

  context.jukebox.stop();
  context.jukebox.loadChart(*replayChart, true, parseCancelled);
  if (parseCancelled) {
    resetReplayWatchLoadingUi();
    return;
  }

  StartOptions options = makeCourseReplayStageStartOptions(session, stageReplay);

  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, std::move(replayChart),
                                      std::move(options)),
      true);
  willStart.store(false);
}

bms_parser::Chart *
MainMenuScene::setSelectedChart(std::unique_ptr<bms_parser::Chart> chart,
                                bool mediaReady, bool reusableForStart) {
  bms_parser::Chart *raw = chart.get();
  std::unique_ptr<bms_parser::Chart> previous;
  {
    std::lock_guard<std::mutex> lock(selectedChartMutex);
    previous = std::move(selectedChart);
    selectedChart = std::move(chart);
    selectedChartMediaReady.store(mediaReady);
    selectedChartReusableForStart.store(reusableForStart);
  }
  return raw;
}

void MainMenuScene::clearSelectedChart() {
  std::unique_ptr<bms_parser::Chart> previous;
  {
    std::lock_guard<std::mutex> lock(selectedChartMutex);
    previous = std::move(selectedChart);
    selectedChartMediaReady.store(false);
    selectedChartReusableForStart.store(false);
  }
}

void MainMenuScene::cancelPreviewLoading(bool stopPreviewAudio) {
  previewLoadCancelled = true;
  if (loadThread.joinable()) {
    SDL_Log("Joining preview thread");
    loadThread.join();
  }
  if (stopPreviewAudio) {
    stopAndClearSelectedChart();
  }
}

void MainMenuScene::stopAndClearSelectedChart() {
  context.jukebox.stop();
  clearSelectedChart();
}

MainMenuScene::SelectedChartRandomInfo
MainMenuScene::selectedChartRandomInfoForPath(
    const std::filesystem::path &path) const {
  SelectedChartRandomInfo info;
  std::lock_guard<std::mutex> lock(selectedChartMutex);
  if (!selectedChartReusableForStart.load() || selectedChart == nullptr ||
      fspath_to_path_t(selectedChart->Meta.BmsPath) != fspath_to_path_t(path)) {
    return info;
  }
  info.seed = selectedChart->Meta.RandomSeed;
  info.prng = selectedChart->Meta.RandomPrng;
  if (!selectedChart->Meta.RandomValues.empty()) {
    info.values = selectedChart->Meta.RandomValues;
  }
  return info;
}

bms_parser::Chart *MainMenuScene::loadedSelectedChartForPath(
    const std::filesystem::path &path) const {
  std::lock_guard<std::mutex> lock(selectedChartMutex);
  if (!selectedChartMediaReady.load() || !selectedChartReusableForStart.load() ||
      selectedChart == nullptr ||
      fspath_to_path_t(selectedChart->Meta.BmsPath) != fspath_to_path_t(path)) {
    return nullptr;
  }
  return selectedChart.get();
}

void MainMenuScene::resetStartLoadingUi() {
  willStart.store(false);
  refreshStartButtonForActiveFolder();
}

void MainMenuScene::resetReplayWatchLoadingUi() {
  willStart.store(false);
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Watch");
  }
}

void MainMenuScene::changeToGameplayScene(bms_parser::Chart *chart,
                                          StartOptions options) {
  context.sceneManager->changeScene(
      std::make_unique<GamePlayScene>(context, chart, std::move(options)),
      true);
}

bool MainMenuScene::beginReplayExport(const std::string &progressTitle,
                                      const std::string &progressMessage,
                                      const std::string &statusMessage) {
  if (replayExportInProgress.exchange(true)) {
    return false;
  }
  if (replayExportThread.joinable()) {
    replayExportThread.join();
  }

  willStart.store(true);
  previewLoadCancelled = true;
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  {
    std::lock_guard<std::mutex> lock(replayExportProgressMutex);
    pendingReplayExportProgress.reset();
  }
  showReplayExportProgress(progressTitle, progressMessage);
  if (replayStatusText != nullptr) {
    replayStatusText->setText(statusMessage);
  }
  return true;
}

void MainMenuScene::queueReplayExportResult(
    const ReplayVideoExportResult &result) {
  std::lock_guard<std::mutex> lock(replayExportResultMutex);
  pendingReplayExportResult = PendingReplayExportResult{
      .success = result.success,
      .photo = false,
      .outputPath = result.outputPath,
      .message = result.message,
  };
}

void MainMenuScene::queueReplayExportResult(
    const ResultImageExportResult &result) {
  std::lock_guard<std::mutex> lock(replayExportResultMutex);
  pendingReplayExportResult = PendingReplayExportResult{
      .success = result.success,
      .photo = true,
      .outputPath = result.outputPath,
      .message = result.message,
  };
}

void MainMenuScene::startReplayVideoExport(const ChartMetaRecord &record,
                                           int replayId,
                                           ReplayVideoExportOptions options) {
  if (!beginReplayExport("Exporting Replay", "Preparing export",
                         "Exporting...")) {
    return;
  }

#if TARGET_OS_ANDROID
  options.progressCallback = [this](const ReplayVideoExportProgress &progress) {
    updateReplayExportProgressUi(progress.fraction, progress.message);
  };
#else
  options.progressCallback = [this](const ReplayVideoExportProgress &progress) {
    std::lock_guard<std::mutex> lock(replayExportProgressMutex);
    pendingReplayExportProgress = PendingReplayExportProgress{
        .fraction = progress.fraction,
        .message = progress.message,
    };
  };
#endif

  auto complete = [this](const ReplayVideoExportResult &result) {
    queueReplayExportResult(result);
  };
  const GaugeType autoPlayGaugeType = selectedGaugeType;
  const bool autoPlayGaugeAutoShift = selectedGaugeAutoShift;
  const std::string autoPlayAssistOption = selectedAssistOption;
  const std::string autoPlayOption = selectedPlayOption;
  const int autoPlayLongNoteMode =
      long_note_mode::valueFromId(selectedLnMode);
  const SelectedChartRandomInfo autoPlayRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  auto runExport = [this, record, replayId, options, complete,
                    autoPlayGaugeType, autoPlayGaugeAutoShift,
                    autoPlayAssistOption, autoPlayOption,
                    autoPlayLongNoteMode,
                    autoPlayRandomInfo](const std::stop_token *stopToken) {
    try {
      if (loadThread.joinable()) {
        loadThread.join();
      }
      context.jukebox.stop();
      if (stopToken != nullptr && stopToken->stop_requested()) {
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }

      if (record.courseStart) {
        auto replay = ReplayDBHelper::GetInstance().LoadCourseReplay(replayId);
        if (!replay.has_value()) {
          complete({.success = false, .message = "No Replay"});
          return;
        }
        if (stopToken != nullptr && stopToken->stop_requested()) {
          complete({.success = false, .message = "Replay export cancelled"});
          return;
        }

        complete(ReplayVideoExporter::ExportCourseReplay(context,
                                                         replay.value(),
                                                         options));
        return;
      }

      if (replay_autoplay::isAutoPlayReplayId(replayId)) {
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> chart;
        try {
          chart = play_options::parseChart(
              record.meta.BmsPath, autoPlayRandomInfo.seed,
              autoPlayRandomInfo.prng, autoPlayRandomInfo.values,
              parseCancelled, "autoplay export");
        } catch (const std::exception &e) {
          SDL_Log("Error parsing %s for autoplay export: %s",
                  fspath_to_utf8(record.meta.BmsPath).c_str(), e.what());
          archive_file::appendDebugLogLine(
              "Autoplay export parse exception: " +
              fspath_to_utf8(record.meta.BmsPath) + ": " + e.what());
        }
        if (chart == nullptr || parseCancelled) {
          complete({.success = false, .message = "No Chart"});
          return;
        }
        if (stopToken != nullptr && stopToken->stop_requested()) {
          complete({.success = false,
                    .message = "Replay export cancelled"});
          return;
        }

        play_options::PlayOptionReplayInfo playInfo =
            play_options::applySelectedPlayOptions(*chart, autoPlayOption);
        applyEffectiveLongNoteModeToChart(*chart, autoPlayLongNoteMode);
        ReplayData replay = replay_autoplay::BuildReplayData(
            *chart, autoPlayGaugeType, autoPlayGaugeAutoShift,
            playInfo.option, playInfo.seed, playInfo.option2, playInfo.seed2,
            autoPlayAssistOption);
        ReplayVideoExportOptions exportOptions = options;
        exportOptions.renderTouchPoints = false;
        exportOptions.renderReplayGhosts = false;
        complete(ReplayVideoExporter::Export(context, chart.get(), replay,
                                             exportOptions));
        return;
      }

      auto replay = ReplayDBHelper::GetInstance().LoadReplay(
          replayId, replayLoadMetaForRecord(record));
      if (!replay.has_value()) {
        complete({.success = false, .message = "No Replay"});
        return;
      }

      std::atomic_bool parseCancelled = false;
      auto chart = play_options::prepareReplayChart(
          record.meta.BmsPath, replay.value(), parseCancelled);
      if (chart == nullptr || parseCancelled) {
        complete({.success = false, .message = "No Chart"});
        return;
      }
      if (stopToken != nullptr && stopToken->stop_requested()) {
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }

      complete(ReplayVideoExporter::Export(context, chart.get(), replay.value(),
                                           options));
    } catch (const std::exception &e) {
      complete({.success = false, .message = e.what()});
    } catch (...) {
      complete({.success = false, .message = "Unexpected replay export failure"});
    }
  };

#if TARGET_OS_ANDROID
  runExport(nullptr);
  applyReplayExportResult();
#else
  replayExportThread = std::jthread(
      [runExport = std::move(runExport)](const std::stop_token &stopToken) {
        runExport(&stopToken);
      });
#endif
}

void MainMenuScene::startReplayImageExport(const ChartMetaRecord &record,
                                           int replayId) {
  if (replay_autoplay::isAutoPlayReplayId(replayId)) {
    return;
  }
  if (!beginReplayExport("Exporting Photo", "Preparing photo",
                         "Exporting photo...")) {
    return;
  }

  auto complete = [this](const ResultImageExportResult &result) {
    queueReplayExportResult(result);
  };

  try {
    if (loadThread.joinable()) {
      loadThread.join();
    }
    context.jukebox.stop();

    if (record.courseStart) {
      updateReplayExportProgressUi(0.20, "Loading course replay");
      auto replay = ReplayDBHelper::GetInstance().LoadCourseReplay(replayId);
      if (!replay.has_value()) {
        complete({.success = false, .message = "No Replay"});
        applyReplayExportResult();
        return;
      }

      updateReplayExportProgressUi(0.65, "Rendering photos");
      complete(ResultImageExporter::ExportCourseReplay(context,
                                                       replay.value()));
      applyReplayExportResult();
      return;
    }

    auto replay = ReplayDBHelper::GetInstance().LoadReplay(
        replayId, replayLoadMetaForRecord(record));
    if (!replay.has_value()) {
      complete({.success = false, .message = "No Replay"});
      applyReplayExportResult();
      return;
    }

    updateReplayExportProgressUi(0.25, "Loading chart");
    std::atomic_bool parseCancelled = false;
    auto chart = play_options::prepareReplayChart(record.meta.BmsPath,
                                                  replay.value(),
                                                  parseCancelled);
    if (chart == nullptr || parseCancelled) {
      complete({.success = false, .message = "No Chart"});
      applyReplayExportResult();
      return;
    }

    updateReplayExportProgressUi(0.75, "Rendering photo");
    complete(ResultImageExporter::ExportReplay(context, *chart, replay.value()));
  } catch (const std::exception &e) {
    complete({.success = false, .message = e.what()});
  } catch (...) {
    complete({.success = false,
              .message = "Unexpected photo export failure"});
  }
  applyReplayExportResult();
}

void MainMenuScene::applyReplayExportProgress() {
  std::optional<PendingReplayExportProgress> progress;
  {
    std::lock_guard<std::mutex> lock(replayExportProgressMutex);
    if (!pendingReplayExportProgress.has_value()) {
      return;
    }
    progress = std::move(pendingReplayExportProgress);
    pendingReplayExportProgress.reset();
  }

  updateReplayExportProgressUi(progress->fraction, progress->message);
}

void MainMenuScene::applyReplayExportResult() {
  std::optional<PendingReplayExportResult> result;
  {
    std::lock_guard<std::mutex> lock(replayExportResultMutex);
    if (!pendingReplayExportResult.has_value()) {
      return;
    }
    result = std::move(pendingReplayExportResult);
    pendingReplayExportResult.reset();
  }

  if (replayExportThread.joinable()) {
    replayExportThread.join();
  }
  replayExportInProgress = false;
  willStart.store(false);
  {
    std::lock_guard<std::mutex> lock(replayExportProgressMutex);
    pendingReplayExportProgress.reset();
  }

  if (recyclerView != nullptr) {
    const int selected = recyclerView->selectedIndex;
    if (selected >= 0 && selected < recyclerView->size()) {
      const auto &selectedMeta = recyclerView->get(selected);
      if (!selectedMeta.unavailable && !selectedMeta.meta.BmsPath.empty() &&
          recyclerView->onSelected) {
        recyclerView->onSelected(selectedMeta, selected);
      }
    }
  }

  if (replayStatusText != nullptr) {
    if (result->success) {
      replayStatusText->setText(
          result->message == "Saved to Photos" ? "Saved" : "Exported");
    } else if (result->message == "No Replay") {
      replayStatusText->setText("No Replay");
    } else if (result->message == "No Chart") {
      replayStatusText->setText("No Chart");
    } else {
      replayStatusText->setText("Export Failed");
    }
  }
  if (replayExportProgressContent != nullptr &&
      replayExportProgressContent->getVisible()) {
    replayModalTitleText->setText("Replay");
    replayExportProgressContent->setVisible(false);
    replayExportOptionsContent->setVisible(false);
    replayListContent->setVisible(true);
    refreshReplayModalActions();
  }

  if (result->success) {
    SDL_Log("Replay %s exported: %s (%s)",
            result->photo ? "image" : "video",
            fspath_to_utf8(result->outputPath).c_str(),
            result->message.c_str());
  } else {
    SDL_Log("Replay %s export failed: %s (%s)",
            result->photo ? "image" : "video", result->message.c_str(),
            fspath_to_utf8(result->outputPath).c_str());
  }

  defer(
      [this]() {
        if (!replayExportInProgress.load() && replayStatusText != nullptr) {
          replayStatusText->setText("");
        }
        return true;
      },
      result->success ? 1800 : 1400, true);
}

void MainMenuScene::update(float dt) {
  // Update the scene logic
  // std::cout << "Updating Main Menu Scene, dt: " << dt << std::endl;
  refreshScoreClearRanksIfNeeded();
  refreshTasksButton();
  applyPendingUiUpdates();
  applyFindBmsUpdates();
  applyUnzipProgress();
  applyUnzipResult();
  applyReplayExportProgress();
  applyReplayExportResult();
#if TARGET_OS_ANDROID
  pollPendingAndroidArchiveImport();
  applyPendingAndroidArchiveImport();
#endif
  if (parseLogModalRoot != nullptr && parseLogModalRoot->getVisible()) {
    refreshParseLogModal();
  }
  std::string nativeMusicStatusMessage;
  if (context.musicPlayer.ProcessNativeControlEvents(
          nativeMusicStatusMessage)) {
    musicStatusMessage = nativeMusicStatusMessage;
  }
  if (context.musicPlayer.ConsumeNativeControlStatus(
          nativeMusicStatusMessage)) {
    musicStatusMessage = nativeMusicStatusMessage;
  }
  if (musicModalRoot != nullptr && musicModalRoot->getVisible()) {
    refreshMusicModal();
  }
  if (tasksModalRoot != nullptr && tasksModalRoot->getVisible()) {
    refreshTasksModal();
  }
}

void MainMenuScene::renderScene() {
  // Render the scene
  // SDL_Log("Rendering Main Menu Scene");
  if (rootLayout == nullptr) {
    return;
  }
  const SafeAreaInsets safe = getSafeAreaInsetsUi();
  const bool layoutChanged =
      rendering::window_width != lastLayoutWidth ||
      rendering::window_height != lastLayoutHeight || safe.top != lastSafeTop ||
      safe.left != lastSafeLeft || safe.bottom != lastSafeBottom ||
      safe.right != lastSafeRight;
  rootLayout->setSize(rendering::window_width, rendering::window_height);
  if (replayModalRoot != nullptr) {
    replayModalRoot->setSize(rendering::window_width, rendering::window_height);
  }
  if (playOptionsModalRoot != nullptr) {
    playOptionsModalRoot->setSize(rendering::window_width,
                                  rendering::window_height);
  }
  if (parseLogModalRoot != nullptr) {
    parseLogModalRoot->setSize(rendering::window_width,
                               rendering::window_height);
  }
  if (musicModalRoot != nullptr) {
    musicModalRoot->setSize(rendering::window_width, rendering::window_height);
  }
  if (tasksModalRoot != nullptr) {
    tasksModalRoot->setSize(rendering::window_width, rendering::window_height);
  }
  if (findBmsModalRoot != nullptr) {
    findBmsModalRoot->setSize(rendering::window_width,
                              rendering::window_height);
  }
  if (unzipModalRoot != nullptr) {
    unzipModalRoot->setSize(rendering::window_width, rendering::window_height);
  }
  if (layoutChanged) {
    lastLayoutWidth = rendering::window_width;
    lastLayoutHeight = rendering::window_height;
    lastSafeTop = safe.top;
    lastSafeLeft = safe.left;
    lastSafeBottom = safe.bottom;
    lastSafeRight = safe.right;
    rootLayout->setPadding(Edge::Top, safe.top + kRootPadding);
    rootLayout->setPadding(Edge::Left, safe.left + kRootPadding);
    rootLayout->setPadding(Edge::Right, safe.right + kRootPadding);
    rootLayout->setPadding(Edge::Bottom, safe.bottom + kRootPadding);
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::cleanupScene() {
  // Cleanup resources when exiting the scene
  previewLoadCancelled = true;
  context.requestAddChartFolderFromFiles = nullptr;
  libraryTaskWorkerPaused = true;
  if (replayExportThread.joinable()) {
    SDL_Log("Joining replayExportThread");
    replayExportThread.request_stop();
    replayExportThread.join();
  }
  if (addFolderPickerThread.joinable()) {
    SDL_Log("Joining addFolderPickerThread");
    addFolderPickerThread.request_stop();
    addFolderPickerThread.join();
  }
  addFolderPickerInProgress = false;
#if TARGET_OS_ANDROID
  if (archiveImportPickerThread.joinable()) {
    SDL_Log("Joining archiveImportPickerThread");
    archiveImportPickerThread.request_stop();
    archiveImportPickerThread.join();
  }
  archiveImportPickerInProgress = false;
#endif
  stopLibraryTaskWorker();
  if (findBmsThread.joinable()) {
    SDL_Log("Joining findBmsThread");
    findBmsCancelled = true;
    findBmsThread.request_stop();
    findBmsThread.join();
  }
  if (unzipThread.joinable()) {
    SDL_Log("Joining unzipThread");
    unzipThread.request_stop();
    unzipThread.join();
  }
  if (loadThread.joinable()) {
    SDL_Log("Joining loadThread");
    loadThread.join();
  }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  ClearIOSFolderAccess();
#endif
  stopAndClearSelectedChart();
  ChartDBHelper::GetInstance().Close(db);
  db = nullptr;
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  rootLayout = nullptr;
  jacketView = nullptr;
  searchBox = nullptr;
  difficultyFilterBox = nullptr;
  startButton = nullptr;
  chartActionsRow = nullptr;
  replayButtonSlot = nullptr;
  replayButton = nullptr;
  findBmsButtonSlot = nullptr;
  findBmsButton = nullptr;
  findBmsButtonText = nullptr;
  unzipButtonSlot = nullptr;
  unzipButton = nullptr;
  unzipButtonText = nullptr;
  parseLogButton = nullptr;
  parseLogButtonText = nullptr;
  musicButton = nullptr;
  musicButtonText = nullptr;
  tasksButton = nullptr;
  tasksButtonText = nullptr;
  replayButtonText = nullptr;
  replayStatusText = nullptr;
  replayModalRoot = nullptr;
  replayModalContentFrame = nullptr;
  replayListContent = nullptr;
  replayExportOptionsContent = nullptr;
  replayExportProgressContent = nullptr;
  replayExportProgressTrack = nullptr;
  replayExportProgressFill = nullptr;
  replayModalTitleText = nullptr;
  replayExportProgressMessageText = nullptr;
  replayExportProgressPercentText = nullptr;
  startButtonText = nullptr;
  playOptionsModalRoot = nullptr;
  musicModalRoot = nullptr;
  unzipModalRoot = nullptr;
  unzipProgressTrack = nullptr;
  unzipProgressFill = nullptr;
  unzipModalTitleText = nullptr;
  unzipProgressMessageText = nullptr;
  unzipProgressPercentText = nullptr;
  unzipProgressDetailText = nullptr;
  unzipDeleteArchiveButton = nullptr;
  unzipCancelButton = nullptr;
  unzipDeleteArchiveButtonText = nullptr;
  unzipCancelButtonText = nullptr;
  parseLogModalRoot = nullptr;
  tasksModalRoot = nullptr;
  parseLogScrollView = nullptr;
  parseLogContent = nullptr;
  parseLogText = nullptr;
  parseLogCloseButton = nullptr;
  parseLogCloseButtonText = nullptr;
  musicTrackText = nullptr;
  musicStatusText = nullptr;
  musicPlaylistText = nullptr;
  musicSelectedButton = nullptr;
  musicAddSelectedButton = nullptr;
  musicRemoveSelectedButton = nullptr;
  musicPlaylistButton = nullptr;
  musicClearPlaylistButton = nullptr;
  musicRandomButton = nullptr;
  musicPreviousButton = nullptr;
  musicSeekBackwardButton = nullptr;
  musicPlayPauseButton = nullptr;
  musicSeekForwardButton = nullptr;
  musicNextButton = nullptr;
  musicStopButton = nullptr;
  musicCloseButton = nullptr;
  musicSelectedButtonText = nullptr;
  musicAddSelectedButtonText = nullptr;
  musicRemoveSelectedButtonText = nullptr;
  musicPlaylistButtonText = nullptr;
  musicClearPlaylistButtonText = nullptr;
  musicRandomButtonText = nullptr;
  musicPreviousButtonText = nullptr;
  musicSeekBackwardButtonText = nullptr;
  musicPlayPauseButtonText = nullptr;
  musicSeekForwardButtonText = nullptr;
  musicNextButtonText = nullptr;
  musicStopButtonText = nullptr;
  musicCloseButtonText = nullptr;
  tasksScrollView = nullptr;
  tasksContent = nullptr;
  tasksText = nullptr;
  tasksRefreshButton = nullptr;
  tasksRefreshButtonText = nullptr;
  tasksCloseButton = nullptr;
  tasksCloseButtonText = nullptr;
  findBmsModalRoot = nullptr;
  findBmsProgressTrack = nullptr;
  findBmsProgressFill = nullptr;
  findBmsModalTitleText = nullptr;
  findBmsStatusText = nullptr;
  findBmsDetailText = nullptr;
  findBmsLogScrollView = nullptr;
  findBmsLogContent = nullptr;
  findBmsLogText = nullptr;
  findBmsCloseButton = nullptr;
  findBmsOpenButton = nullptr;
  findBmsGoogleButton = nullptr;
  findBmsRefreshButton = nullptr;
  findBmsCloseButtonText = nullptr;
  findBmsOpenButtonText = nullptr;
  findBmsGoogleButtonText = nullptr;
  findBmsRefreshButtonText = nullptr;
  readyGaugeText = nullptr;
  readyPlayOptionText = nullptr;
  readyAssistOptionText = nullptr;
  playOptionsCloseButton = nullptr;
  playOptionsCloseButtonText = nullptr;
  replayListView = nullptr;
  replayWatchButton = nullptr;
  replayModalPhotoButton = nullptr;
  replayModalExportButton = nullptr;
  replayModalCloseButton = nullptr;
  replayFps60Button = nullptr;
  replayFps120Button = nullptr;
  replayResolution1080Button = nullptr;
  replayResolutionFullButton = nullptr;
  replayResultIncludeButton = nullptr;
  replayResultSkipButton = nullptr;
  replayTouchShowButton = nullptr;
  replayTouchHideButton = nullptr;
  replayGhostShowButton = nullptr;
  replayGhostHideButton = nullptr;
  replayExportTouchShowButton = nullptr;
  replayExportTouchHideButton = nullptr;
  replayExportGhostShowButton = nullptr;
  replayExportGhostHideButton = nullptr;
  replayWatchButtonText = nullptr;
  replayModalPhotoButtonText = nullptr;
  replayModalExportButtonText = nullptr;
  replayModalCloseButtonText = nullptr;
  replayFps60ButtonText = nullptr;
  replayFps120ButtonText = nullptr;
  replayResolution1080ButtonText = nullptr;
  replayResolutionFullButtonText = nullptr;
  replayResultIncludeButtonText = nullptr;
  replayResultSkipButtonText = nullptr;
  replayTouchShowButtonText = nullptr;
  replayTouchHideButtonText = nullptr;
  replayGhostShowButtonText = nullptr;
  replayGhostHideButtonText = nullptr;
  replayExportTouchShowButtonText = nullptr;
  replayExportTouchHideButtonText = nullptr;
  replayExportGhostShowButtonText = nullptr;
  replayExportGhostHideButtonText = nullptr;
  pendingReplayExportResult.reset();
  pendingReplayExportProgress.reset();
  pendingUnzipResult.reset();
  pendingUnzipProgress.reset();
  pendingSelectChartPath.reset();
  suppressPreviewForChartPath.reset();
  unzipDeleteCandidatePath.reset();
  unzipEstimatedUncompressedSize = 0;
  pendingFindBmsProgressEvents.clear();
  pendingFindBmsResult.reset();
  replayExportInProgress = false;
  unzipInProgress = false;
  findBmsJobRunning = false;
  findBmsCancelled = false;
  findBmsResult = {};
  findBmsProgressMessage.clear();
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.0;
  findBmsProgressLog.clear();
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    libraryTaskQueue.clear();
    libraryTasks.clear();
    bumpLibraryTasksRevisionLocked();
  }
  libraryProgressRevision.store(0, std::memory_order_release);
  libraryProgressTaskId.store(0, std::memory_order_relaxed);
  libraryProgressCurrent.store(0, std::memory_order_relaxed);
  libraryProgressTotal.store(0, std::memory_order_relaxed);
  libraryProgressBasisPoints.store(0, std::memory_order_relaxed);
  libraryProgressStage.store(
      static_cast<int>(ChartScanProgressStage::Preparing),
      std::memory_order_relaxed);
  displayedLibraryTasksRevision = 0;
  displayedLibraryProgressRevision = 0;
  displayedLibraryTasksButtonText.clear();
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  replaySummaries.clear();
  selectedReplayIndex = -1;
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  selectedExportIncludeResultScreen = true;
  selectedReplayRenderTouchPoints = true;
  selectedReplayRenderGhosts = true;
  replayExportProgressFraction = 0.0;
  gaugeSelectionButtons.clear();
  playOptionButtons.clear();
  longNoteModeButtons.clear();
  assistOptionButtons.clear();
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}

void MainMenuScene::LoadCharts(ChartDBHelper &dbHelper, sqlite3 *db,
                               std::vector<ChartEntry> &entries,
                               MainMenuScene &scene,
                               const std::stop_token &stop_token,
                               ChartScanProgressCallback progressCallback,
                               ChartScanPauseCallback pauseCallback) {
  std::vector<std::filesystem::path> roots;
  roots.reserve(entries.size());
  for (auto &entry : entries) {
    if (stop_token.stop_requested()) {
      break;
    }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
    roots.push_back(ResolveIOSFolderEntryPath(entry));
#elif TARGET_OS_ANDROID
    std::filesystem::path root(entry.path);
    RegisterAndroidChartFolder(root, entry.iosBookmark);
    roots.push_back(std::move(root));
#else
    roots.emplace_back(entry.path);
#endif
  }

  if (stop_token.stop_requested()) {
    return;
  }

  SDL_Log("Refreshing chart library");
  const int changedCount = dbHelper.ScanChartRoots(
      db, roots, &stop_token, progressCallback, pauseCallback,
      [&scene]() { return scene.pendingLibraryScanFlushRequest(); },
      [&scene](std::uint64_t request) {
        scene.completeLibraryScanFlush(request);
      });
  SDL_Log("Chart library refresh changed %d entries", changedCount);
  if (changedCount > 0) {
    scene.requestLibraryReload(true);
  }
}

#ifdef _WIN32
void MainMenuScene::FindFilesWin(const std::filesystem::path &path,
                                 std::vector<Diff> &diffs,
                                 const std::unordered_set<path_t> &oldFilesWs,
                                 std::vector<path_t> &directoriesToVisit,
                                 const std::stop_token &stop_token) {
  WIN32_FIND_DATAW findFileData;
  HANDLE hFind =
      FindFirstFileW((path.wstring() + L"\\*.*").c_str(), &findFileData);

  if (hFind != INVALID_HANDLE_VALUE) {
    do {
      if (stop_token.stop_requested()) {
        break;
      }
      if (!(findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        path_t filename(findFileData.cFileName);

        if (asobmshow::bms_chart_file::isBmsChartFileName(filename)) {
          path_t dirPath;

          path_t fullPath = path.wstring() + L"\\" + filename;
          if (oldFilesWs.find(fullPath) == oldFilesWs.end()) {
            diffs.push_back({fullPath, Added});
          }
        }
      } else if (findFileData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        path_t filename(findFileData.cFileName);

        if (filename != L"." && filename != L"..") {
          directoriesToVisit.push_back(path.wstring() + L"\\" + filename);
        }
      }
    } while (FindNextFileW(hFind, &findFileData) != 0);
    FindClose(hFind);
  }
}
#elif TARGET_OS_OSX || TARGET_OS_LINUX || TARGET_OS_ANDROID
void MainMenuScene::resolveDType(const std::filesystem::path &directoryPath,
                                 struct dirent *entry) {
  if (entry->d_type == DT_UNKNOWN) {
    std::filesystem::path fullPath = directoryPath / entry->d_name;
    struct stat statbuf;
    if (stat(fullPath.c_str(), &statbuf) == 0) {
      if (S_ISREG(statbuf.st_mode)) {
        entry->d_type = DT_REG;
      } else if (S_ISDIR(statbuf.st_mode)) {
        entry->d_type = DT_DIR;
      }
    }
  }
}
// TODO: Use platform-specific method for faster traversal
void MainMenuScene::FindFilesUnix(
    const std::filesystem::path &directoryPath, std::vector<Diff> &diffs,
    const std::unordered_set<path_t> &oldFiles,
    std::vector<std::filesystem::path> &directoriesToVisit,
    const std::stop_token &stop_token) {
  UniqueResource<DIR, closedir> dir(opendir(directoryPath.c_str()));
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir.get())) != nullptr) {
      if (stop_token.stop_requested()) {
        break;
      }
      resolveDType(directoryPath, entry);
      if (entry->d_type == DT_REG) {
        std::string filename = entry->d_name;
        if (asobmshow::bms_chart_file::isBmsChartFileName(filename)) {
          std::filesystem::path fullPath = directoryPath / filename;
          if (oldFiles.find(fspath_to_path_t(fullPath)) == oldFiles.end()) {
            diffs.push_back({fullPath, Added});
          }
        }
      } else if (entry->d_type == DT_DIR) {
        std::string filename = entry->d_name;
        if (filename != "." && filename != "..") {
          directoriesToVisit.push_back(directoryPath / filename);
        }
      } else {
        SDL_Log("Unknown file type: %s", entry->d_name);
      }
    }
  } else {
    SDL_Log("Failed to open directory: %s", directoryPath.c_str());
  }
}

#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
void MainMenuScene::FindFilesIOS(
    const std::filesystem::path &directoryPath, std::vector<Diff> &diffs,
    const std::unordered_set<path_t> &oldFilesWs,
    std::vector<std::filesystem::path> &directoriesToVisit,
    const std::stop_token &stop_token) {
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      directoryPath, std::filesystem::directory_options::skip_permission_denied,
      error);
  if (error) {
    SDL_Log("Failed to open iOS directory: %s (%s)",
            fspath_to_utf8(directoryPath).c_str(), error.message().c_str());
    return;
  }

  for (const auto end = std::filesystem::directory_iterator(); iterator != end;
       iterator.increment(error)) {
    if (stop_token.stop_requested()) {
      break;
    }
    if (error) {
      SDL_Log("Failed while reading iOS directory: %s (%s)",
              fspath_to_utf8(directoryPath).c_str(), error.message().c_str());
      error.clear();
      continue;
    }

    const std::filesystem::directory_entry &entry = *iterator;
    std::error_code typeError;
    if (entry.is_regular_file(typeError)) {
      if (asobmshow::bms_chart_file::isBmsChartPath(entry.path())) {
        if (oldFilesWs.find(fspath_to_path_t(entry.path())) ==
            oldFilesWs.end()) {
          diffs.push_back({entry.path(), Added});
        }
      }
    } else if (!typeError && entry.is_directory(typeError)) {
      directoriesToVisit.push_back(entry.path());
    } else if (typeError) {
      SDL_Log("Failed to inspect iOS path: %s (%s)",
              fspath_to_utf8(entry.path()).c_str(),
              typeError.message().c_str());
    }
  }
}
#endif

void MainMenuScene::FindNewBmsFiles(
    std::vector<Diff> &diffs, const std::unordered_set<path_t> &oldFilesWs,
    const std::filesystem::path &path, const std::stop_token &stop_token) {
#ifdef _WIN32
  std::vector<path_t> directoriesToVisit;
  directoriesToVisit.push_back(path.wstring());
#else
  std::vector<std::filesystem::path> directoriesToVisit;
  directoriesToVisit.push_back(path);
#endif
  SDL_Log("Finding new bms files in %s", path_t_to_utf8(path).c_str());
  while (!directoriesToVisit.empty()) {
    if (stop_token.stop_requested()) {
      break;
    }
    std::filesystem::path currentDir = directoriesToVisit.back();
    directoriesToVisit.pop_back();

#ifdef _WIN32
    FindFilesWin(currentDir, diffs, oldFilesWs, directoriesToVisit, stop_token);
#elif TARGET_OS_OSX || TARGET_OS_LINUX || TARGET_OS_ANDROID
    FindFilesUnix(currentDir, diffs, oldFilesWs, directoriesToVisit,
                  stop_token);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
    FindFilesIOS(currentDir, diffs, oldFilesWs, directoriesToVisit, stop_token);
#endif
  }
}
