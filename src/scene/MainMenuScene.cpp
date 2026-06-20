#include "MainMenuScene.h"
#include "MainMenuLibrary.h"
#include "../ArchiveFile.h"
#include "../tinyfiledialogs.h"
#include <fstream>
#include <algorithm>
#include "../ReplayDBHelper.h"
#include "../ReplayVideoExporter.h"
#include "../PlayOptionUtils.h"
#include "../view/ChartListItemView.h"
#include "../view/LibraryFolderItemView.h"
#include "../view/TextView.h"
#include "../view/TextInputBox.h"
#include "../Utils.h"
#include "../targets.h"
#include "../video/transcode.h"
#include "../view/Button.h"
#include "../view/BlockingOverlayView.h"
#include "ChartViewerScene.h"
#include "play/GamePlayScene.h"
#include "../view/ClearLampColors.h"
#include "../view/ReplaySummaryListView.h"
#include "../view/ScrollView.h"
#include <cctype>
#include <cstring>
#include <memory>
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
constexpr size_t kFindBmsMaxLogLines = 120;
constexpr size_t kFindBmsMaxPendingProgressEvents = 160;

struct SafeAreaInsets {
  int top = 0;
  int left = 0;
  int bottom = 0;
  int right = 0;
};

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
    setBorderWidth(2);

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
    setBackgroundColor(Color(40, 96, 156, 236));
    setBorderColor(Color(105, 162, 222, 255));
    if (label != nullptr) {
      label->setColor({242, 247, 255, 255});
    }
  }

  void onUnselected() override {
    setBackgroundColor(Color(22, 49, 77, 224));
    setBorderColor(Color(92, 128, 170, 230));
    if (label != nullptr) {
      label->setColor({226, 237, 249, 255});
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
  const int percent = static_cast<int>(
      std::lround(std::clamp(ratio, 0.0, 1.0) * 100.0));
  return std::to_string(percent) + "%";
}

std::string findBmsProgressDisplayText(const std::string &message,
                                       std::uint64_t downloadedBytes,
                                       std::uint64_t totalBytes,
                                       bool includeBytes) {
  if (message == "Downloading archive" && totalBytes > 0) {
    const double ratio =
        std::clamp(static_cast<double>(downloadedBytes) /
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
    const double ratio =
        std::clamp(static_cast<double>(downloadedBytes) /
                       static_cast<double>(totalBytes),
                   0.0, 1.0);
    return "Download complete - " + progressPercentText(ratio);
  }
  return message;
}

std::string findBmsProgressDisplayText(
    const BmsSearchDownloadProgress &progress, bool includeBytes) {
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
    return std::max(previous, progress.totalBytes > 0 ? 0.10 + ratio * 0.80
                                                      : 0.90);
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

  if (!std::filesystem::exists(targetPath, errorCode) || errorCode) {
    errorMessage = "Chart file does not exist: " +
                   path_t_to_utf8(fspath_to_path_t(targetPath));
    return false;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return RevealIOSFileInFiles(path_t_to_utf8(fspath_to_path_t(targetPath)),
                              errorMessage);
#elif TARGET_OS_OSX
  return RevealPathInFinder(path_t_to_utf8(fspath_to_path_t(targetPath)),
                            errorMessage);
#elif defined(_WIN32)
  const std::wstring nativePath = targetPath.wstring();
  const HRESULT coInit =
      CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
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

  const std::string directoryText = directoryPath.string();
  pid_t pid = 0;
  char *argv[] = {const_cast<char *>(openerPath.c_str()),
                  const_cast<char *>(directoryText.c_str()), nullptr};
  const int result =
      posix_spawn(&pid, openerPath.c_str(), nullptr, nullptr, argv, environ);
  if (result != 0) {
    errorMessage = std::string("Could not open file manager: ") +
                   std::strerror(result);
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
#elif TARGET_OS_OSX
  return OpenURLInDefaultBrowser(url, errorMessage);
#elif defined(_WIN32)
  const HINSTANCE result =
      ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    errorMessage = std::string("Could not open browser: ") +
                   std::strerror(result);
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
using main_menu_library::folderKeyForLevel;
using main_menu_library::folderKeyForTable;

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
std::mutex gIOSFolderAccessMutex;
std::vector<void *> gIOSFolderAccessHandles;
std::unordered_map<path_t, path_t> gIOSResolvedFolderPaths;

void RefreshIOSFolderAccess(const std::vector<ChartEntry> &entries) {
  std::lock_guard<std::mutex> lock(gIOSFolderAccessMutex);
  for (void *handle : gIOSFolderAccessHandles) {
    StopIOSSecurityScopedResource(handle);
  }
  gIOSFolderAccessHandles.clear();
  gIOSResolvedFolderPaths.clear();

  for (const auto &entry : entries) {
    if (entry.iosBookmark.empty()) {
      continue;
    }
    std::string resolvedPath;
    std::string errorMessage;
    void *handle = StartIOSSecurityScopedResource(
        path_t_to_utf8(entry.path), entry.iosBookmark, resolvedPath,
        errorMessage);
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

void styleActionButton(Button *button, TextView *text, bool enabled,
                       const Color &normal, const Color &hover,
                       const Color &pressed, const Color &border) {
  if (button == nullptr || text == nullptr) {
    return;
  }

  if (enabled) {
    button->setBackgroundColors(normal, hover, pressed);
    button->setBorderColors(border, Color(border.r, border.g, border.b, 255),
                            Color(235, 246, 255, 255));
    text->setColor({242, 247, 255, 255});
  } else {
    button->setBackgroundColors(Color(25, 31, 39, 154), Color(25, 31, 39, 154),
                                Color(25, 31, 39, 154));
    button->setBorderColors(Color(76, 88, 102, 120), Color(76, 88, 102, 120),
                            Color(76, 88, 102, 120));
    text->setColor({129, 143, 160, 255});
  }
}

void styleOptionButton(Button *button, TextView *text, bool selected) {
  if (selected) {
    styleActionButton(button, text, true, Color(38, 97, 87, 232),
                      Color(50, 121, 109, 242), Color(65, 146, 130, 250),
                      Color(112, 212, 191, 255));
  } else {
    styleActionButton(button, text, true, Color(22, 34, 51, 220),
                      Color(32, 48, 70, 232), Color(44, 65, 94, 242),
                      Color(83, 109, 140, 220));
  }
}

TextView *makeModalLabel(const std::string &text) {
  auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  label->setText(text);
  label->setColor({173, 193, 216, 255});
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
  button->setStyledBorderWidth(2);
  if (textOut != nullptr) {
    *textOut = text;
  }
  return button;
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

} // namespace

void MainMenuScene::ChartListPageCache::reset(sqlite3 *database,
                                              const ChartMetaQuery &chartQuery,
                                              int count) {
  db = database;
  query = chartQuery;
  query.limit = 0;
  query.offset = 0;
  totalCount = std::max(0, count);
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
  initView(context);
  SDL_Log("Main Menu Scene Initialized");
  startLibraryTaskWorker();
  enqueueLibraryRefreshTask("Refresh Library");
}

void MainMenuScene::onPause() {
  pauseLibraryTaskWorker();
}

void MainMenuScene::onResume() {
  resumeLibraryTaskWorker();
  startLibraryTaskWorker();
  refreshScoreClearRanksIfNeeded();
  refreshLibraryIfNeeded();
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
      const auto queuedIt =
          std::find_if(libraryTaskQueue.begin(), libraryTaskQueue.end(),
                       [&task](const auto &queuedTask) {
                         return queuedTask.id == task.id;
                       });
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
      const auto queuedIt =
          std::find_if(libraryTaskQueue.begin(), libraryTaskQueue.end(),
                       [&task](const auto &queuedTask) {
                         return queuedTask.id == task.id;
                       });
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

bool MainMenuScene::waitForLibraryTaskResume(
    std::uint64_t id, const std::stop_token &stopToken) {
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
  const int total = snapshot.valid && snapshot.taskId == id ? snapshot.total : 0;
  setLibraryTaskState(id, LibraryTaskStatus::Paused, fraction, current, total,
                      "Paused");

  std::unique_lock<std::mutex> lock(libraryTaskPauseMutex);
  libraryTaskPauseCv.wait(lock, stopToken, [this]() {
    return !libraryTaskWorkerPaused.load();
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
  startLibraryTaskWorker();
  const std::uint64_t id = nextLibraryTaskId.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    libraryTaskQueue.push_back(LibraryTaskRequest{
        .id = id,
        .title = title,
        .folderToAdd = folderToAdd,
        .iosBookmark = iosBookmark,
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
                                        double fraction, int current,
                                        int total,
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
      libraryTasks.begin(), libraryTasks.end(), [](const auto &task) {
        return isActiveLibraryTaskStatus(task.status);
      }));
  libraryActiveTaskCount.store(activeCount, std::memory_order_release);
}

void MainMenuScene::updateLibraryTaskProgress(
    std::uint64_t id, const ChartScanProgress &progress) {
  const int total = std::max(0, progress.total);
  const int current =
      total > 0 ? std::clamp(progress.current, 0, total)
                : std::max(0, progress.current);
  const int basisPoints =
      total > 0 ? static_cast<int>((static_cast<std::int64_t>(current) *
                                    10000) /
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
      libraryTaskCv.wait(lock, stopToken, [this]() {
        return !libraryTaskWorkerPaused.load() && !libraryTaskQueue.empty();
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
      runLibraryRefreshTask(task, stopToken);
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

void MainMenuScene::runLibraryRefreshTask(
    const LibraryTaskRequest &task, const std::stop_token &stopToken) {
  auto &dbHelper = ChartDBHelper::GetInstance();
  auto taskDb = dbHelper.Connect();
  dbHelper.CreateChartMetaTable(taskDb);
  dbHelper.CreateSolidArchiveTable(taskDb);
  dbHelper.CreateEntriesTable(taskDb);
  dbHelper.CreateDifficultyTableTables(taskDb);

  auto pauseTask = [&]() {
    return waitForLibraryTaskResume(task.id, stopToken);
  };
  if (!pauseTask()) {
    dbHelper.Close(taskDb);
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
    dbHelper.Close(taskDb);
    return;
  }
  const int importedTables = dbHelper.ImportDifficultyTablesFromDirectory(
      taskDb, Utils::GetDocumentsPath("tables"));
  if (importedTables > 0 && !stopToken.stop_requested()) {
    requestLibraryReload(true);
  }
  auto entries = dbHelper.SelectAllEntries(taskDb);

  if (stopToken.stop_requested()) {
    dbHelper.Close(taskDb);
    return;
  }

  if (entries.empty()) {
    setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.04, 0, 0,
                        "Waiting for library folder");
    if (!pauseTask()) {
      dbHelper.Close(taskDb);
      return;
    }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
    std::string folder;
    std::string bookmark;
    std::string errorMessage;
    if (PickIOSFolder(folder, bookmark, errorMessage)) {
      dbHelper.InsertEntry(taskDb, std::filesystem::path(folder), bookmark);
      entries = dbHelper.SelectAllEntries(taskDb);
    } else {
      if (!errorMessage.empty()) {
        SDL_Log("Failed to pick iOS library folder: %s",
                errorMessage.c_str());
      }
      auto path = Utils::GetDocumentsPath("BMS");
      std::filesystem::create_directories(path);
      entries.push_back({
          .path = fspath_to_path_t(path),
          .iosBookmark = "",
      });
    }
#else
    char *folder_c = tinyfd_selectFolderDialog("Select Folder", nullptr);
    std::string folder;
    if (folder_c == nullptr) {
      std::cerr << "tinyfd_selectFolderDialog error: " << strerror(errno)
                << std::endl;
      std::cout << "Failed to open folder select dialog.\n";

      while (folder.empty()) {
        if (stopToken.stop_requested()) {
          dbHelper.Close(taskDb);
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

        if (folder[0] == '~') {
          folder.replace(0, 1, getenv("HOME"));
        }
        std::ifstream test(folder);
        if (!test)
          folder = "";
      }

      if (folder.empty()) {
        dbHelper.Close(taskDb);
        return;
      }
    } else {
      folder = folder_c;
    }
    std::filesystem::path path(folder);
    dbHelper.InsertEntry(taskDb, path);
    entries = dbHelper.SelectAllEntries(taskDb);
#endif
  }

  if (stopToken.stop_requested()) {
    dbHelper.Close(taskDb);
    return;
  }

  setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.06, 0, 0,
                      "Refreshing folder access");
  if (!pauseTask()) {
    dbHelper.Close(taskDb);
    return;
  }
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  RefreshIOSFolderAccess(entries);
#endif
  LoadCharts(dbHelper, taskDb, entries, *this, stopToken,
             [this, taskId = task.id](const ChartScanProgress &progress) {
               updateLibraryTaskProgress(taskId, progress);
             },
             [this, taskId = task.id, &stopToken]() {
               return waitForLibraryTaskResume(taskId, stopToken);
             });
  dbHelper.Close(taskDb);
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

  addFolderPickerThread = std::jthread(
      [this](const std::stop_token &stopToken) {
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
                ? path_t_to_utf8(fspath_to_path_t(folderPath.filename()))
                : path_t_to_utf8(fspath_to_path_t(folderPath));
        if (folderName.empty()) {
          folderName = "Folder";
        }
        enqueueLibraryRefreshTask("Add Folder: " + folderName, folderPath,
                                  bookmark);
      });
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
  playOptionsCloseButton = nullptr;
  playOptionsCloseButtonText = nullptr;
  replayListView = nullptr;
  replayWatchButton = nullptr;
  replayModalExportButton = nullptr;
  replayModalCloseButton = nullptr;
  replayFps60Button = nullptr;
  replayFps120Button = nullptr;
  replayResolution1080Button = nullptr;
  replayResolutionFullButton = nullptr;
  replayWatchButtonText = nullptr;
  replayModalExportButtonText = nullptr;
  replayModalCloseButtonText = nullptr;
  replayFps60ButtonText = nullptr;
  replayFps120ButtonText = nullptr;
  replayResolution1080ButtonText = nullptr;
  replayResolutionFullButtonText = nullptr;
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
  replaySummaries.clear();
  selectedReplayIndex = -1;
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  replayExportProgressFraction = 0.0;
  gaugeSelectionButtons.clear();
  playOptionButtons.clear();

  const Color kBackdropTint(10, 18, 30, 112);
  const Color kPanelFill(17, 27, 42, 196);
  const Color kSurfaceFill(11, 18, 30, 168);
  const Color kPrimaryButtonNormal(29, 73, 120, 216);
  const Color kPrimaryButtonHover(40, 96, 156, 228);
  const Color kPrimaryButtonPressed(58, 129, 204, 236);
  const Color kSecondaryButtonNormal(76, 49, 36, 208);
  const Color kSecondaryButtonHover(101, 65, 47, 220);
  const Color kSecondaryButtonPressed(133, 87, 63, 232);

  recyclerView = new RecyclerView<ChartMetaRecord>(
      [](const ChartMetaRecord &a, const ChartMetaRecord &b) {
        return a.meta.SHA256 == b.meta.SHA256 && a.meta.MD5 == b.meta.MD5 &&
               a.meta.BmsPath == b.meta.BmsPath &&
               a.difficultyTableLabels == b.difficultyTableLabels &&
               a.unavailable == b.unavailable &&
               a.solidArchive == b.solidArchive &&
               a.archiveSize == b.archiveSize &&
               a.archiveUncompressedSize == b.archiveUncompressedSize &&
               a.archiveFileCount == b.archiveFileCount;
      });
  folderRecyclerView = new RecyclerView<LibraryFolderItem>(
      [](const LibraryFolderItem &a, const LibraryFolderItem &b) {
        return a.key == b.key;
      });
  auto dbHelper = ChartDBHelper::GetInstance();
  dbHelper.CreateChartMetaTable(db);
  dbHelper.CreateSolidArchiveTable(db);
  dbHelper.CreateDifficultyTableTables(db);

  recyclerView->onCreateView = [this](const ChartMetaRecord &item) {
    return new ChartListItemView(0, 0, rendering::window_width, 100, item);
  };
  recyclerView->itemHeight = 100;
  recyclerView->onBind = [this](View *view, const ChartMetaRecord &item,
                                int idx, bool isSelected) {
    auto *chartListItemView = dynamic_cast<ChartListItemView *>(view);
    chartListItemView->setMeta(item);
    chartListItemView->setClearRank(clearRankForChart(item));
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
    if (!replayExportInProgress.load() && replayStatusText != nullptr) {
      replayStatusText->setText("");
    }
    setPlayableChartActionsVisible(!item.unavailable &&
                                   !item.solidArchive &&
                                   !meta.BmsPath.empty());
    refreshUnzipButtonForSelection(&item);
    setFindBmsButtonVisible(item.unavailable &&
                            !item.solidArchive &&
                            (!meta.SHA256.empty() || !meta.MD5.empty() ||
                             !meta.Title.empty()));
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
          path_t_to_utf8(fspath_to_path_t(meta.BmsPath)) +
          " files=" + std::to_string(item.archiveFileCount) +
          " estimatedUnpacked=" +
          std::to_string(item.archiveUncompressedSize));
      return;
    }
    if (archive_file::isVirtualPath(meta.BmsPath) &&
        !context.settings.archiveChartPreviewEnabled) {
      jacketView->freeImage();
      if (!replayExportInProgress.load() && replayStatusText != nullptr) {
        replayStatusText->setText("Archive preview disabled");
      }
      archive_file::appendDebugLogLine(
          "Preview skipped by archive chart preview setting: " +
          path_t_to_utf8(fspath_to_path_t(meta.BmsPath)));
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
      jacketView->setImageAsync(meta.Folder / meta.StageFile);
    } else {
      jacketView->freeImage();
    }
    if (suppressPreview) {
      if (!replayExportInProgress.load() && replayStatusText != nullptr) {
        replayStatusText->setText("Unzipped chart selected");
      }
      archive_file::appendDebugLogLine(
          "Preview suppressed for auto-selected unzipped chart: " +
          path_t_to_utf8(fspath_to_path_t(meta.BmsPath)));
      return;
    }
    previewLoadCancelled = false;
    loadThread = std::thread([this, meta, &context]() {
      SDL_Log("Previewing %s", path_t_to_utf8(meta.BmsPath).c_str());

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
      SDL_Log("Parsing %s", path_t_to_utf8(meta.BmsPath).c_str());
      std::unique_ptr<bms_parser::Chart> chart;
      try {
        chart = play_options::parseChart(meta.BmsPath, previewLoadCancelled,
                                         "preview");
      } catch (const std::exception &e) {
        SDL_Log("Preview parse failed %s: %s",
                path_t_to_utf8(meta.BmsPath).c_str(), e.what());
        archive_file::appendDebugLogLine(
            "Preview parse exception: " +
            path_t_to_utf8(fspath_to_path_t(meta.BmsPath)) + ": " + e.what());
        return;
      }
      SDL_Log("Parsed %s", path_t_to_utf8(meta.BmsPath).c_str());
      if (chart == nullptr) {
        SDL_Log("Chart is null");
        archive_file::appendDebugLogLine(
            "Preview chart is null: " +
            path_t_to_utf8(fspath_to_path_t(meta.BmsPath)));
        return;
      }

      context.jukebox.loadChart(*chart, true, previewLoadCancelled);
      if (previewLoadCancelled) {
        return;
      }
      setSelectedChart(std::move(chart), true);
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

  folderRecyclerView->onCreateView = [](const LibraryFolderItem &item) {
    return new LibraryFolderItemView(0, 0, 260, 44);
  };
  folderRecyclerView->itemHeight = 44;
  folderRecyclerView->onBind = [this](View *view, const LibraryFolderItem &item,
                                      int idx, bool isSelected) {
    auto *folderView = dynamic_cast<LibraryFolderItemView *>(view);
    if (folderView != nullptr) {
      folderView->setItem(item.label, item.depth, item.count, isSelected,
                          clearRankForFolder(item.key));
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
  rootLayout->setBackgroundColor(kBackdropTint);

  auto nav = new View();
  nav->setFlexDirection(FlexDirection::Column);
  nav->setAlignItems(YGAlignStretch);
  nav->setWidth(280);
  nav->setGap(12);
  nav->setPadding(Edge::All, 14);
  nav->setBackgroundColor(kPanelFill);
  nav->setBorderColor(Color(70, 95, 124, 255));
  nav->setBorderWidth(2);

  auto *navTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  navTitle->setText("Library");
  navTitle->setColor({243, 247, 255, 255});
  nav->addView(navTitle);

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  auto *addFolderButton = new Button(0, 0, 252, 50);
  auto *addFolderText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  addFolderText->setText("Add Folder");
  addFolderText->setAlign(TextView::CENTER);
  addFolderText->setVAlign(TextView::MIDDLE);
  addFolderButton->setContentView(addFolderText);
  addFolderButton->setBackgroundColors(
      Color(30, 63, 75, 216), Color(42, 83, 97, 228),
      Color(55, 106, 123, 236));
  addFolderButton->setBorderColors(Color(96, 169, 181, 255),
                                   Color(121, 199, 211, 255),
                                   Color(151, 224, 235, 255));
  addFolderButton->setStyledBorderWidth(2);
  addFolderButton->setOnClickListener(
      [this]() { addIOSFolderEntryFromFiles(); });
  nav->addView(addFolderButton);
#endif

  folderRecyclerView->setFlex(1);
  folderRecyclerView->clearBackgroundColor();
  folderRecyclerView->setBorderColor(Color(63, 86, 113, 255));
  folderRecyclerView->setBorderWidth(2);
  nav->addView(folderRecyclerView);
  rootLayout->addView(nav);

  auto left = new View();
  left->setFlexDirection(FlexDirection::Column);
  left->setAlignItems(YGAlignStretch);
  left->setFlex(1);
  left->setGap(14);
  left->setPadding(Edge::All, 16);
  left->setBackgroundColor(kPanelFill);
  left->setBorderColor(Color(70, 95, 124, 255));
  left->setBorderWidth(2);

  auto *libraryHeader = new View();
  libraryHeader->setFlexDirection(FlexDirection::Row);
  libraryHeader->setAlignItems(YGAlignCenter);
  libraryHeader->setGap(12);
  libraryHeader->setHeight(58);

  auto *libraryTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 44);
  libraryTitle->setText("Song Select");
  libraryTitle->setColor({243, 247, 255, 255});
  libraryTitle->setVAlign(TextView::MIDDLE);
  libraryTitle->setFlex(1);
  libraryHeader->addView(libraryTitle);

  parseLogButton = makeModalButton("Log", 20, &parseLogButtonText);
  parseLogButton->setWidth(112);
  parseLogButton->setHeight(50);
  parseLogButton->setOnClickListener([this]() { showParseLogModal(); });
  styleActionButton(parseLogButton, parseLogButtonText, true,
                    Color(22, 34, 51, 220), Color(32, 48, 70, 232),
                    Color(44, 65, 94, 242), Color(83, 109, 140, 220));
  libraryHeader->addView(parseLogButton);

  tasksButton = makeModalButton("0 Tasks", 20, &tasksButtonText);
  tasksButton->setWidth(142);
  tasksButton->setHeight(50);
  tasksButton->setOnClickListener([this]() { showTasksModal(); });
  styleActionButton(tasksButton, tasksButtonText, true,
                    Color(22, 34, 51, 220), Color(32, 48, 70, 232),
                    Color(44, 65, 94, 242), Color(83, 109, 140, 220));
  libraryHeader->addView(tasksButton);
  left->addView(libraryHeader);

  auto *librarySubtitle = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  librarySubtitle->setText(
      "Search your library and preview charts before starting.");
  librarySubtitle->setColor({157, 177, 200, 255});
  left->addView(librarySubtitle);

  auto *filterRow = new View();
  filterRow->setFlexDirection(FlexDirection::Row);
  filterRow->setAlignItems(YGAlignStretch);
  filterRow->setGap(10);

  searchBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 30);
  searchBox->setText("");
  searchBox->setHeight(56);
  searchBox->setFlex(1);
  searchBox->setBackgroundColor(kSurfaceFill);
  searchBox->setBorderColor(Color(88, 115, 149, 255));
  searchBox->setBorderWidth(2);
  searchBox->setVAlign(TextView::MIDDLE);
  searchBox->setColor({239, 244, 251, 255});
  auto onSearchChanged = [this](const std::string &text) {
    searchText = text;
    reloadChartList();
  };
  searchBox->onTextChanged(onSearchChanged);
  searchBox->onSubmit(onSearchChanged);
  filterRow->addView(searchBox);

  difficultyFilterBox = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 30);
  difficultyFilterBox->setText("");
  difficultyFilterBox->setHeight(56);
  difficultyFilterBox->setWidth(180);
  difficultyFilterBox->setBackgroundColor(kSurfaceFill);
  difficultyFilterBox->setBorderColor(Color(88, 115, 149, 255));
  difficultyFilterBox->setBorderWidth(2);
  difficultyFilterBox->setVAlign(TextView::MIDDLE);
  difficultyFilterBox->setColor({239, 244, 251, 255});
  auto onDifficultyChanged = [this](const std::string &text) {
    difficultyText = text;
    reloadChartList();
  };
  difficultyFilterBox->onTextChanged(onDifficultyChanged);
  difficultyFilterBox->onSubmit(onDifficultyChanged);
  filterRow->addView(difficultyFilterBox);

  auto *filterLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  filterLabel->setText("Search / Difficulty");
  filterLabel->setColor({157, 177, 200, 255});
  left->addView(filterLabel);
  left->addView(filterRow);

  recyclerView->setFlex(1);
  recyclerView->clearBackgroundColor();
  recyclerView->setBorderColor(Color(63, 86, 113, 255));
  recyclerView->setBorderWidth(2);
  left->addView(recyclerView);
  rootLayout->addView(left);

  auto right = new View();
  right->setFlexDirection(FlexDirection::Column);
  right->setAlignItems(YGAlignCenter);
  right->setPadding(Edge::All, 20);
  right->setGap(12);
  right->setWidth(300);
  right->setBackgroundColor(kPanelFill);
  right->setBorderColor(Color(70, 95, 124, 255));
  right->setBorderWidth(2);

  auto *rightTitle = new TextView("assets/fonts/notosanscjkjp.ttf", 34);
  rightTitle->setText("Ready");
  rightTitle->setColor({243, 247, 255, 255});
  rightTitle->setAlign(TextView::CENTER);
  rightTitle->setHeight(42);
  right->addView(rightTitle);

  auto *rightSubtitle = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  rightSubtitle->setText("Preview, tweak, and start.");
  rightSubtitle->setColor({157, 177, 200, 255});
  rightSubtitle->setAlign(TextView::CENTER);
  rightSubtitle->setHeight(28);
  right->addView(rightSubtitle);

  const GaugeSelection savedGaugeSelection =
      gaugeSelectionFromSettingId(context.settings.selectedGaugeType);
  selectedGaugeType = savedGaugeSelection.type;
  selectedGaugeAutoShift = savedGaugeSelection.autoShift;
  selectedPlayOption =
      play_options::normalizePlayOption(context.settings.selectedPlayOption);

  auto *readySettings = new View();
  readySettings->setFlexDirection(FlexDirection::Column);
  readySettings->setAlignItems(YGAlignStretch);
  readySettings->setWidth(220);
  readySettings->setGap(6);

  auto makeReadyStatusText = []() {
    auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
    text->setHeight(28);
    text->setColor({222, 234, 247, 255});
    return text;
  };
  auto *readyGaugeRow = new View();
  readyGaugeRow->setFlexDirection(FlexDirection::Row);
  readyGaugeRow->setAlignItems(YGAlignCenter);
  readyGaugeRow->setGap(6);
  readyGaugeRow->setHeight(28);
  auto *readyGaugeLabelText = makeReadyStatusText();
  readyGaugeLabelText->setText("Gauge:");
  readyGaugeLabelText->setColor({157, 177, 200, 255});
  readyGaugeLabelText->setWidth(70);
  readyGaugeText = makeReadyStatusText();
  readyGaugeText->setFlex(1);
  readyGaugeRow->addView(readyGaugeLabelText);
  readyGaugeRow->addView(readyGaugeText);
  readyPlayOptionText = makeReadyStatusText();
  readySettings->addView(readyGaugeRow);
  readySettings->addView(readyPlayOptionText);

  auto *playOptionsButton = new Button(0, 0, 220, 54);
  auto *playOptionsButtonText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  playOptionsButtonText->setText("Options");
  playOptionsButtonText->setAlign(TextView::CENTER);
  playOptionsButtonText->setVAlign(TextView::MIDDLE);
  playOptionsButton->setContentView(playOptionsButtonText);
  playOptionsButton->setBackgroundColors(
      Color(30, 63, 75, 216), Color(42, 83, 97, 228), Color(55, 106, 123, 236));
  playOptionsButton->setBorderColors(Color(96, 169, 181, 255),
                                     Color(121, 199, 211, 255),
                                     Color(151, 224, 235, 255));
  playOptionsButton->setStyledBorderWidth(2);
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
  startButton->setBackgroundColors(kPrimaryButtonNormal, kPrimaryButtonHover,
                                   kPrimaryButtonPressed);
  startButton->setBorderColors(Color(105, 162, 222, 255),
                               Color(133, 190, 244, 255),
                               Color(162, 212, 255, 255));
  startButton->setStyledBorderWidth(2);
  startButton->setOnClickListener([this]() {
    if (willStart.load()) {
      return;
    }
    auto selected = recyclerView->selectedIndex;
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
  replayButton->setBackgroundColors(
      Color(25, 58, 65, 216), Color(35, 82, 92, 228), Color(48, 111, 124, 236));
  replayButton->setBorderColors(Color(91, 174, 184, 255),
                                Color(116, 204, 214, 255),
                                Color(145, 232, 241, 255));
  replayButton->setStyledBorderWidth(2);
  replayButton->setOnClickListener([this]() {
    if (willStart.load() || replayExportInProgress.load()) {
      return;
    }
    auto selected = recyclerView->selectedIndex;
    if (selected < 0 || selected >= recyclerView->size()) {
      return;
    }
    const auto &selectedMeta = recyclerView->get(selected);
    if (selectedMeta.solidArchive || selectedMeta.unavailable ||
        selectedMeta.meta.BmsPath.empty()) {
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
  findBmsButton->setBackgroundColors(
      Color(32, 74, 62, 216), Color(45, 99, 83, 228),
      Color(61, 132, 109, 236));
  findBmsButton->setBorderColors(Color(94, 181, 153, 255),
                                 Color(119, 210, 180, 255),
                                 Color(148, 235, 204, 255));
  findBmsButton->setStyledBorderWidth(2);
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
  unzipButton->setBackgroundColors(
      Color(76, 61, 30, 216), Color(103, 82, 40, 228),
      Color(132, 106, 53, 236));
  unzipButton->setBorderColors(Color(188, 157, 87, 255),
                               Color(220, 188, 112, 255),
                               Color(244, 214, 143, 255));
  unzipButton->setStyledBorderWidth(2);
  unzipButton->setOnClickListener(
      [this]() { startUnzipSelectedArchiveFolder(); });
  unzipButtonSlot->addView(unzipButton);

  replayStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 17);
  replayStatusText->setText("");
  replayStatusText->setColor({157, 177, 200, 255});
  replayStatusText->setAlign(TextView::CENTER);
  replayStatusText->setHeight(20);

  auto *jacketCard = new View();
  jacketCard->setWidth(200);
  jacketCard->setHeight(200);
  jacketCard->setAlignItems(YGAlignCenter);
  jacketCard->setJustifyContent(YGJustifyCenter);
  jacketCard->setBackgroundColor(kSurfaceFill);
  jacketCard->setBorderColor(Color(88, 115, 149, 255));
  jacketCard->setBorderWidth(2);
  jacketView->setWidth(200)->setHeight(200);
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
  viewerButton->setBackgroundColors(
      Color(31, 51, 74, 216), Color(43, 70, 100, 228),
      Color(58, 93, 132, 236));
  viewerButton->setBorderColors(Color(106, 153, 205, 255),
                                Color(135, 181, 229, 255),
                                Color(167, 209, 248, 255));
  viewerButton->setStyledBorderWidth(2);
  viewerButton->setOnClickListener(
      [this]() { openChartViewerForSelection(); });
  chartActionsRow->addView(viewerButton);

  auto *revealButton = new Button(0, 0, 105, 58);
  revealButton->setFlex(1);
  auto *revealButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  revealButtonText->setText("Reveal");
  revealButtonText->setAlign(TextView::CENTER);
  revealButtonText->setVAlign(TextView::MIDDLE);
  revealButton->setContentView(revealButtonText);
  revealButton->setBackgroundColors(
      Color(31, 51, 74, 216), Color(43, 70, 100, 228),
      Color(58, 93, 132, 236));
  revealButton->setBorderColors(Color(106, 153, 205, 255),
                                Color(135, 181, 229, 255),
                                Color(167, 209, 248, 255));
  revealButton->setStyledBorderWidth(2);
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
  settingsButton->setBackgroundColors(
      kSecondaryButtonNormal, kSecondaryButtonHover, kSecondaryButtonPressed);
  settingsButton->setBorderColors(Color(174, 124, 91, 255),
                                  Color(207, 146, 105, 255),
                                  Color(232, 169, 122, 255));
  settingsButton->setStyledBorderWidth(2);
  settingsButton->setOnClickListener([this, &context]() {
    if (willStart.load() || replayExportInProgress.load()) {
      return;
    }
    previewLoadCancelled = true;
    context.jukebox.stop();
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
  addView(rootLayout);
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

  const float previousScrollOffset = preserveViewState
                                         ? folderRecyclerView->scrollOffset
                                         : 0.0f;
  auto dbHelper = ChartDBHelper::GetInstance();
  std::vector<LibraryFolderItem> folders;

  int allSongCount = 0;
  allSongCount = dbHelper.CountAllChartMeta(db);

  folders.push_back({
      .key = "all",
      .label = "All songs",
      .type = LibraryFolderItem::Type::AllSongs,
      .depth = 0,
      .count = allSongCount,
  });

  const int solidArchiveCount = dbHelper.CountSolidArchives(db);
  if (solidArchiveCount > 0) {
    folders.push_back({
        .key = "solid-archives",
        .label = "Solid Archive",
        .type = LibraryFolderItem::Type::SolidArchives,
        .depth = 0,
        .count = solidArchiveCount,
    });
  }

  const auto tables = dbHelper.SelectDifficultyTables(db);
  for (const auto &table : tables) {
    folders.push_back({
        .key = folderKeyForTable(table.id),
        .label = table.name,
        .type = LibraryFolderItem::Type::DifficultyTable,
        .depth = 0,
        .count = table.chartCount,
        .tableId = table.id,
    });

    const auto levels = dbHelper.SelectDifficultyLevels(db, table.id);
    for (const auto &level : levels) {
      folders.push_back({
          .key = folderKeyForLevel(level.tableId, level.level),
          .label = level.tableSymbol + level.level,
          .type = LibraryFolderItem::Type::DifficultyLevel,
          .depth = 1,
          .count = level.chartCount,
          .tableId = level.tableId,
          .tableLevel = level.level,
      });
    }
  }

  const auto courseGroups = dbHelper.SelectDifficultyCourseGroups(db);
  if (!courseGroups.empty()) {
    int coursesCount = 0;
    for (const auto &group : courseGroups) {
      coursesCount += group.matchedChartCount;
    }
    folders.push_back({
        .key = "courses",
        .label = "Courses",
        .type = LibraryFolderItem::Type::CoursesRoot,
        .depth = 0,
        .count = coursesCount,
    });

    for (const auto &group : courseGroups) {
      const std::string label = group.groupName.empty()
                                    ? group.tableName + " Courses"
                                    : group.groupName;
      folders.push_back({
          .key = folderKeyForCourseGroup(group.tableId, group.groupName),
          .label = label,
          .type = LibraryFolderItem::Type::CourseGroup,
          .depth = 1,
          .count = group.matchedChartCount,
          .courseTableId = group.tableId,
          .courseGroupName = group.groupName,
      });

      const auto courses =
          dbHelper.SelectDifficultyCourses(db, group.tableId, group.groupName);
      for (const auto &course : courses) {
        const std::string courseLabel =
            course.level.empty() ? course.name : course.level;
        folders.push_back({
            .key = folderKeyForCourse(course.id),
            .label = courseLabel,
            .type = LibraryFolderItem::Type::Course,
            .depth = 2,
            .count = course.matchedChartCount,
            .courseId = course.id,
            .courseTableId = course.tableId,
            .courseGroupName = course.groupName,
        });
      }
    }
  }

  for (auto &folder : folders) {
    folder.clearRank = clearRankForFolder(folder.key);
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
    const float maxOffset = std::max(
        0.0f, static_cast<float>(
                  std::max(1, folderCount) * folderRecyclerView->itemHeight -
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

void MainMenuScene::reloadChartList(bool preserveViewState) {
  if (recyclerView == nullptr) {
    return;
  }

  const float previousScrollOffset = preserveViewState
                                         ? recyclerView->scrollOffset
                                         : 0.0f;
  const int previousSelectedIndex =
      preserveViewState ? recyclerView->selectedIndex : -1;
  path_t previousSelectedPath;
  if (preserveViewState && previousSelectedIndex >= 0 &&
      previousSelectedIndex < recyclerView->size()) {
    previousSelectedPath =
        fspath_to_path_t(recyclerView->get(previousSelectedIndex).meta.BmsPath);
  }

  ChartMetaQuery query;
  query.keyword = searchText;
  query.difficultyText = difficultyText;

  switch (activeFolder.type) {
  case LibraryFolderItem::Type::SolidArchives:
    query.solidArchivesOnly = true;
    break;
  case LibraryFolderItem::Type::DifficultyTable:
    query.tableId = activeFolder.tableId;
    break;
  case LibraryFolderItem::Type::DifficultyLevel:
    query.tableId = activeFolder.tableId;
    query.tableLevel = activeFolder.tableLevel;
    break;
  case LibraryFolderItem::Type::CoursesRoot:
    query.coursesOnly = true;
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

  const int count = ChartDBHelper::GetInstance().CountChartMeta(db, query);
  chartListCache.reset(db, query, count);
  if (!preserveViewState || previousSelectedPath.empty()) {
    refreshReplayAvailability(nullptr);
  }
  recyclerView->setItemProvider(count,
                                [this](int index) -> const ChartMetaRecord & {
                                  return chartListCache.get(index);
                                });
  if (!preserveViewState) {
    return;
  }

  const float maxOffset = std::max(
      0.0f, static_cast<float>(std::max(1, count) * recyclerView->itemHeight -
                                recyclerView->getHeight()));
  recyclerView->scrollOffset =
      std::clamp(previousScrollOffset, 0.0f, maxOffset);

  int restoredSelectedIndex = -1;
  auto pathMatchesPreviousSelection = [&](int index) {
    if (index < 0 || index >= count || previousSelectedPath.empty()) {
      return false;
    }
    return fspath_to_path_t(chartListCache.get(index).meta.BmsPath) ==
           previousSelectedPath;
  };

  if (pathMatchesPreviousSelection(previousSelectedIndex)) {
    restoredSelectedIndex = previousSelectedIndex;
  } else if (!previousSelectedPath.empty()) {
    const int visibleStart =
        std::max(0, static_cast<int>(previousScrollOffset /
                                     std::max(1, recyclerView->itemHeight)) -
                        chartListCache.pageSize);
    const int visibleEnd =
        std::min(count, visibleStart + chartListCache.pageSize * 3);
    for (int i = visibleStart; i < visibleEnd; ++i) {
      if (pathMatchesPreviousSelection(i)) {
        restoredSelectedIndex = i;
        break;
      }
    }
  }

  recyclerView->selectedIndex = restoredSelectedIndex;
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
  folderClearRanks =
      main_menu_library::LoadFolderClearRanks(db, scoreClearRanks);
}

void MainMenuScene::refreshScoreClearRankViews() {
  reloadScoreClearRanks();
  if (folderRecyclerView != nullptr) {
    folderRecyclerView->rebindVisibleItems();
  }
  if (recyclerView != nullptr) {
    recyclerView->rebindVisibleItems();
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
  if (record.solidArchive) {
    return kNoClearTypeRank;
  }
  return scoreClearRanks.bestRankFor(record.meta);
}

int MainMenuScene::clearRankForFolder(const std::string &key) const {
  const auto it = folderClearRanks.find(key);
  return it == folderClearRanks.end() ? kNoClearTypeRank : it->second;
}

void MainMenuScene::requestLibraryReload(bool includeFolders) {
  if (includeFolders) {
    folderItemsReloadRequested = true;
  }
  chartListReloadRequested = true;
}

void MainMenuScene::applyPendingUiUpdates() {
  const bool shouldReloadFolders = folderItemsReloadRequested.exchange(false);
  const bool shouldReloadCharts = chartListReloadRequested.exchange(false);
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
  for (int i = 0; i < recyclerView->size(); ++i) {
    const ChartMetaRecord &record = chartListCache.get(i);
    if (fspath_to_path_t(record.meta.BmsPath) != target) {
      continue;
    }

    const int previous = recyclerView->selectedIndex;
    if (previous >= 0 && previous < recyclerView->size() &&
        previous != i && recyclerView->onUnselected) {
      recyclerView->onUnselected(recyclerView->get(previous), previous);
    }
    recyclerView->selectedIndex = i;
    const float selectedY = static_cast<float>(i * recyclerView->itemHeight);
    const float viewportHeight = static_cast<float>(recyclerView->getHeight());
    const float itemHeight = static_cast<float>(recyclerView->itemHeight);
    const float centeredOffset =
        selectedY - std::max(0.0f, viewportHeight - itemHeight) / 2.0f;
    const float maxOffset = std::max(
        0.0f, static_cast<float>(
                  std::max(1, recyclerView->size()) * recyclerView->itemHeight -
                  recyclerView->getHeight()));
    recyclerView->scrollOffset = std::clamp(centeredOffset, 0.0f, maxOffset);
    recyclerView->rebindVisibleItems();
    suppressPreviewForChartPath = record.meta.BmsPath;
    if (recyclerView->onSelected) {
      recyclerView->onSelected(record, i);
    }
    archive_file::appendDebugLogLine(
        "Selected unzipped chart: " +
        path_t_to_utf8(fspath_to_path_t(record.meta.BmsPath)));
    return;
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
  activeFolder = item;
  reloadChartList();
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
      const bool darkText = item.autoShift || item.type == GaugeType::Hard ||
                            item.type == GaugeType::ExHard;
      item.text->setColor(darkText ? SDL_Color{14, 20, 28, 255}
                                   : SDL_Color{255, 255, 255, 255});
    } else {
      item.button->setBackgroundColors(Color(20, 31, 47, 214),
                                       Color(31, 48, 72, 226),
                                       Color(44, 67, 99, 236));
      item.button->setBorderColors(Color(76, 101, 130, 190),
                                   Color(106, 134, 166, 220),
                                   Color(134, 164, 198, 240));
      item.text->setColor({216, 227, 241, 255});
    }
  }
  refreshReadySettingsSummary();
}

void MainMenuScene::setPlayOptionSelection(const std::string &option) {
  selectedPlayOption = play_options::normalizePlayOption(option);
  context.settings.selectedPlayOption = selectedPlayOption;
  context.settings.sanitize();
  if (!context.settings.save()) {
    SDL_Log("Failed to save play option selection");
  }
  refreshPlayOptionButtons();
}

void MainMenuScene::refreshPlayOptionButtons() {
  for (auto &item : playOptionButtons) {
    if (item.button == nullptr || item.text == nullptr) {
      continue;
    }

    item.text->setText(item.option);
    styleOptionButton(item.button, item.text,
                      play_options::normalizePlayOption(item.option) ==
                          selectedPlayOption);
  }
  refreshReadySettingsSummary();
}

void MainMenuScene::refreshReadySettingsSummary() {
  if (readyGaugeText != nullptr) {
    readyGaugeText->setText(
        gaugeButtonLabel(selectedGaugeType, selectedGaugeAutoShift));
    readyGaugeText->setColor(
        readyGaugeTextColor(selectedGaugeType, selectedGaugeAutoShift));
  }
  if (readyPlayOptionText != nullptr) {
    readyPlayOptionText->setText("Option: " + selectedPlayOption);
  }
}

void MainMenuScene::startSelectedChart() {
  if (willStart.load() || unzipInProgress.load() ||
      pendingSelectChartPath.has_value() ||
      chartListReloadRequested.load() || folderItemsReloadRequested.load() ||
      recyclerView == nullptr) {
    return;
  }

  int selected = recyclerView != nullptr ? recyclerView->selectedIndex : -1;
  if (recyclerView == nullptr || selected < 0 ||
      selected >= recyclerView->size()) {
    return;
  }
  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable || record.meta.BmsPath.empty()) {
    return;
  }
  startChartDirect(record);
}

void MainMenuScene::startChartDirect(const ChartMetaRecord &record) {
  if (willStart.exchange(true)) {
    return;
  }

  if (record.solidArchive || record.unavailable || record.meta.BmsPath.empty()) {
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
  const std::string normalizedPlayOption =
      play_options::normalizePlayOption(playOption);
  const bool canReusePreviewForStart =
      normalizedPlayOption.empty() || normalizedPlayOption == "NORMAL";
  const SelectedChartRandomInfo chartRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  defer(
      [this, record, gaugeType, gaugeAutoShift, autoKeySound, playOption,
       canReusePreviewForStart, chartRandomInfo]() {
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
              path_t_to_utf8(fspath_to_path_t(record.meta.BmsPath)));
          context.jukebox.stop();
          changeToGameplayScene(readyChart,
                                {
                                    .startPosition = 0,
                                    .autoKeySound = autoKeySound,
                                    .autoPlay = false,
                                    .gaugeType = gaugeType,
                                    .gaugeAutoShift = gaugeAutoShift,
                                });
          return finishStart();
        }

        selectedChartMediaReady.store(false);
        std::atomic_bool parseCancelled = false;
        std::unique_ptr<bms_parser::Chart> preparedChart;
        try {
          preparedChart =
              play_options::parseChart(record.meta.BmsPath,
                                       chartRandomInfo.seed,
                                       chartRandomInfo.prng,
                                       chartRandomInfo.values, parseCancelled);
        } catch (const std::exception &e) {
          SDL_Log("Error parsing %s for start: %s",
                  path_t_to_utf8(record.meta.BmsPath).c_str(), e.what());
          archive_file::appendDebugLogLine(
              "Start parse exception: " +
              path_t_to_utf8(fspath_to_path_t(record.meta.BmsPath)) + ": " +
              e.what());
        }
        if (preparedChart != nullptr && !parseCancelled) {
          play_options::PlayOptionReplayInfo playInfo =
              play_options::applySelectedPlayOptions(*preparedChart,
                                                     playOption);
          context.jukebox.stop();
          context.jukebox.loadChart(*preparedChart, true, parseCancelled);
          bms_parser::Chart *loadedChart = nullptr;
          if (!parseCancelled) {
            loadedChart = setSelectedChart(std::move(preparedChart), true);
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
                                  });
            return finishStart();
          }
        }

        auto *chart = loadedSelectedChartForPath(record.meta.BmsPath);
        if (chart == nullptr) {
          return finishStart();
        }

        context.jukebox.stop();
        changeToGameplayScene(chart,
                              {
                                  .startPosition = 0,
                                  .autoKeySound = autoKeySound,
                                  .autoPlay = false,
                                  .gaugeType = gaugeType,
                                  .gaugeAutoShift = gaugeAutoShift,
                              });
        return finishStart();
      },
      0, true);
}

void MainMenuScene::openChartViewerForSelection() {
  if (willStart.load() || replayExportInProgress.load() ||
      unzipInProgress.load() || pendingSelectChartPath.has_value() ||
      chartListReloadRequested.load() ||
      folderItemsReloadRequested.load() || recyclerView == nullptr) {
    return;
  }

  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }

  const ChartMetaRecord record = recyclerView->get(selected);
  if (record.solidArchive || record.unavailable || record.meta.BmsPath.empty()) {
    return;
  }
  openChartViewerDirect(record);
}

void MainMenuScene::openChartViewerDirect(const ChartMetaRecord &record) {
  if (willStart.load() || replayExportInProgress.load() ||
      record.solidArchive || record.unavailable || record.meta.BmsPath.empty()) {
    return;
  }

  const SelectedChartRandomInfo chartRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  previewLoadCancelled = true;
  if (loadThread.joinable()) {
    loadThread.join();
  }
  archive_file::appendDebugLogLine(
      "Open chart viewer: " +
      path_t_to_utf8(fspath_to_path_t(record.meta.BmsPath)));
  context.jukebox.stop();
  context.sceneManager->changeScene(
      new ChartViewerScene(context, record, chartRandomInfo.seed,
                           chartRandomInfo.prng, chartRandomInfo.values),
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
            path_t_to_utf8(fspath_to_path_t(record.meta.BmsPath)).c_str(),
            errorMessage.c_str());
  }
}

void MainMenuScene::refreshReplayAvailability(const ChartMetaRecord *record) {
  replaySummaries.clear();
  selectedReplayIndex = -1;
  if (record == nullptr || record->solidArchive || record->unavailable ||
      record->meta.BmsPath.empty()) {
    setReplayButtonVisible(false);
    return;
  }

  replaySummaries = ReplayDBHelper::GetInstance().ListReplays(record->meta);
  setReplayButtonVisible(!replaySummaries.empty());
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
  if (startButton != nullptr) {
    startButton->setVisible(visible);
    startButton->setHeight(visible ? 86.0f : 0.0f);
  }
  if (chartActionsRow != nullptr) {
    chartActionsRow->setVisible(visible);
    chartActionsRow->setHeight(visible ? 58.0f : 0.0f);
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
      chartListReloadRequested.load() ||
      folderItemsReloadRequested.load() || recyclerView == nullptr) {
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
      "Unzip requested: " +
      path_t_to_utf8(fspath_to_path_t(record.meta.BmsPath)) + " outputRoot=" +
      path_t_to_utf8(fspath_to_path_t(outputRoot)) + " mode=full-archive");

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
      result.message = stopToken.stop_requested()
                           ? "Unzip cancelled"
                           : (errorMessage.empty()
                                  ? "Unzip failed"
                                  : "Unzip failed: " + errorMessage);
    } else if (stopToken.stop_requested()) {
      result.success = false;
      result.message = "Unzip cancelled";
    } else {
      auto dbHelper = ChartDBHelper::GetInstance();
      auto unzipDb = dbHelper.Connect();
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
      dbHelper.Close(unzipDb);

      result.success = true;
      result.message =
          changedCount > 0
              ? "Unzipped archive. Library refreshed."
              : "Unzipped archive. Library already current.";
      if (!stopToken.stop_requested()) {
        requestLibraryReload(true);
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

  unzipModalRoot =
      new BlockingOverlayView(0, 0, rendering::window_width,
                              rendering::window_height);
  unzipModalRoot->setPositionType(YGPositionTypeAbsolute);
  unzipModalRoot->setPosition(Edge::Left, 0);
  unzipModalRoot->setPosition(Edge::Top, 0);
  unzipModalRoot->setZIndex(1000);
  unzipModalRoot->setVisible(false);
  unzipModalRoot->setFlexDirection(FlexDirection::Column);
  unzipModalRoot->setAlignItems(YGAlignCenter);
  unzipModalRoot->setJustifyContent(YGJustifyCenter);
  unzipModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setFlexDirection(FlexDirection::Column)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setBackgroundColor(Color(16, 25, 39, 244))
      ->setBorderColor(Color(93, 123, 160, 255))
      ->setBorderWidth(2);

  unzipModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  unzipModalTitleText->setText("Unzip");
  unzipModalTitleText->setColor({245, 249, 255, 255});
  unzipModalTitleText->setHeight(42);
  panel->addView(unzipModalTitleText);

  unzipProgressMessageText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  unzipProgressMessageText->setColor({222, 234, 247, 255});
  unzipProgressMessageText->setHeight(32);
  panel->addView(unzipProgressMessageText);

  unzipProgressTrack = new View();
  unzipProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setBackgroundColor(Color(8, 14, 23, 230))
      ->setBorderColor(Color(74, 101, 132, 255))
      ->setBorderWidth(2);
  unzipProgressFill = new View();
  unzipProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      Color(74, 157, 224, 240));
  unzipProgressTrack->addView(unzipProgressFill);
  panel->addView(unzipProgressTrack);

  unzipProgressPercentText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  unzipProgressPercentText->setColor({173, 193, 216, 255});
  unzipProgressPercentText->setHeight(28);
  panel->addView(unzipProgressPercentText);

  unzipProgressDetailText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  unzipProgressDetailText->setColor({143, 161, 184, 255});
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
    std::string detail =
        total > 0 ? "Processing files" : "Working on archive";
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
  if (!std::filesystem::is_regular_file(archivePath, error) || error) {
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
        "Failed to delete source archive: " +
        path_t_to_utf8(fspath_to_path_t(archivePath)) +
        (error ? ": " + error.message() : ""));
    return;
  }

  auto dbHelper = ChartDBHelper::GetInstance();
  sqlite3 *deleteDb = dbHelper.Connect();
  if (deleteDb != nullptr) {
    dbHelper.DeleteArchiveRecords(deleteDb, archivePath);
    dbHelper.Close(deleteDb);
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
      "Deleted source archive after unzip: " +
      path_t_to_utf8(fspath_to_path_t(archivePath)));
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
  const bool canDeleteArchive =
      result->success && result->canDeleteArchive &&
      !result->archivePath.empty() &&
      std::filesystem::is_regular_file(result->archivePath, archiveStateError) &&
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
      (result->chartPath.empty()
           ? ""
           : ": " + path_t_to_utf8(fspath_to_path_t(result->chartPath))));

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
  auto entries = dbHelper.SelectAllEntries(db);
  if (entries.empty()) {
    const auto path = Utils::GetDocumentsPath("BMS");
    std::error_code errorCode;
    std::filesystem::create_directories(path, errorCode);
    dbHelper.InsertEntry(db, path);
    return path;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return ResolveIOSFolderEntryPath(entries.front());
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
  parseLogModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(640)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setBackgroundColor(Color(13, 22, 35, 242))
      ->setBorderColor(Color(86, 118, 153, 255))
      ->setBorderWidth(2);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Parsing Logs");
  title->setColor({245, 249, 255, 255});
  title->setHeight(42);
  panel->addView(title);

  parseLogScrollView =
      new ScrollView(0, 0, static_cast<int>(kModalContentWidth), 480);
  parseLogScrollView->setWidth(kModalContentWidth);
  parseLogScrollView->setFlex(1);
  parseLogScrollView->setBackgroundColor(Color(7, 14, 24, 230));
  parseLogScrollView->setBorderColor(Color(76, 105, 139, 255));
  parseLogScrollView->setBorderWidth(2);

  parseLogContent = new View();
  parseLogContent->setFlexDirection(FlexDirection::Column);
  parseLogContent->setAlignItems(YGAlignStretch);
  parseLogContent->setPadding(Edge::All, 10);

  parseLogText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  parseLogText->setText(archive_file::debugLogText());
  parseLogText->setColor({181, 203, 225, 255});
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

  parseLogCloseButton =
      makeModalButton("Close", 20, &parseLogCloseButtonText);
  parseLogCloseButton->setWidth(130);
  parseLogCloseButton->setOnClickListener([this]() { hideParseLogModal(); });
  styleActionButton(parseLogCloseButton, parseLogCloseButtonText, true,
                    Color(38, 64, 95, 232), Color(50, 84, 123, 242),
                    Color(64, 103, 148, 250), Color(116, 161, 210, 255));

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
  tasksModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(560)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setBackgroundColor(Color(13, 22, 35, 242))
      ->setBorderColor(Color(86, 118, 153, 255))
      ->setBorderWidth(2);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Tasks");
  title->setColor({245, 249, 255, 255});
  title->setHeight(42);
  panel->addView(title);

  tasksScrollView =
      new ScrollView(0, 0, static_cast<int>(kModalContentWidth), 400);
  tasksScrollView->setWidth(kModalContentWidth);
  tasksScrollView->setFlex(1);
  tasksScrollView->setBackgroundColor(Color(7, 14, 24, 230));
  tasksScrollView->setBorderColor(Color(76, 105, 139, 255));
  tasksScrollView->setBorderWidth(2);

  tasksContent = new View();
  tasksContent->setFlexDirection(FlexDirection::Column);
  tasksContent->setAlignItems(YGAlignStretch);
  tasksContent->setPadding(Edge::All, 12);

  tasksText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  tasksText->setText(tasksModalTextSnapshot());
  tasksText->setColor({205, 222, 241, 255});
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
  styleActionButton(tasksRefreshButton, tasksRefreshButtonText, true,
                    Color(38, 97, 87, 232), Color(50, 121, 109, 242),
                    Color(65, 146, 130, 250), Color(112, 212, 191, 255));

  tasksCloseButton = makeModalButton("Close", 20, &tasksCloseButtonText);
  tasksCloseButton->setWidth(130);
  tasksCloseButton->setOnClickListener([this]() { hideTasksModal(); });
  styleActionButton(tasksCloseButton, tasksCloseButtonText, true,
                    Color(38, 64, 95, 232), Color(50, 84, 123, 242),
                    Color(64, 103, 148, 250), Color(116, 161, 210, 255));

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
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    activeTasks.reserve(libraryTasks.size());
    for (const auto &task : libraryTasks) {
      if (task.status == LibraryTaskStatus::Queued ||
          task.status == LibraryTaskStatus::Running ||
          task.status == LibraryTaskStatus::Paused) {
        activeTasks.push_back(task);
      }
    }
  }

  if (activeTasks.empty()) {
    return "No parsing tasks.";
  }

  std::ostringstream text;
  text << activeTasks.size()
       << (activeTasks.size() == 1 ? " task" : " tasks") << "\n\n";
  for (const auto &task : activeTasks) {
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
    } else if (!task.detail.empty()) {
      text << "\n" << task.detail;
    }
    text << "\n\n";
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
  findBmsModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(560)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, kModalPanelPadding)
      ->setBackgroundColor(Color(13, 22, 35, 242))
      ->setBorderColor(Color(86, 118, 153, 255))
      ->setBorderWidth(2);

  findBmsModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  findBmsModalTitleText->setText("Find BMS");
  findBmsModalTitleText->setColor({245, 249, 255, 255});
  findBmsModalTitleText->setHeight(42);
  panel->addView(findBmsModalTitleText);

  findBmsStatusText = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  findBmsStatusText->setText("Preparing lookup");
  findBmsStatusText->setColor({235, 243, 252, 255});
  findBmsStatusText->setWrap(true);
  findBmsStatusText->setOverflow(TextView::TextOverflow::Hidden);
  findBmsStatusText->setHeight(58);
  panel->addView(findBmsStatusText);

  findBmsDetailText = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  findBmsDetailText->setText("");
  findBmsDetailText->setColor({173, 193, 216, 255});
  findBmsDetailText->setWrap(true);
  findBmsDetailText->setOverflow(TextView::TextOverflow::Hidden);
  findBmsDetailText->setFlex(1);
  panel->addView(findBmsDetailText);

  findBmsLogScrollView =
      new ScrollView(0, 0, static_cast<int>(kModalContentWidth), 112);
  findBmsLogScrollView->setWidth(kModalContentWidth);
  findBmsLogScrollView->setHeight(112);
  findBmsLogScrollView->setBackgroundColor(Color(7, 14, 24, 210));
  findBmsLogScrollView->setBorderColor(Color(76, 105, 139, 255));
  findBmsLogScrollView->setBorderWidth(2);

  findBmsLogContent = new View();
  findBmsLogContent->setFlexDirection(FlexDirection::Column);
  findBmsLogContent->setAlignItems(YGAlignStretch);
  findBmsLogContent->setPadding(Edge::All, 8);

  findBmsLogText = new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  findBmsLogText->setText("Preparing lookup");
  findBmsLogText->setColor({150, 173, 197, 255});
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
  findBmsCandidateRecyclerView->setBackgroundColor(Color(7, 14, 24, 210));
  findBmsCandidateRecyclerView->setBorderColor(Color(76, 105, 139, 255));
  findBmsCandidateRecyclerView->setBorderWidth(2);
  findBmsCandidateRecyclerView->setVisible(false);
  findBmsCandidateRecyclerView->onCreateView = [](const BmsSearchCandidate &) {
    return new FindBmsCandidateItemView();
  };
  findBmsCandidateRecyclerView->onBind =
      [](View *view, const BmsSearchCandidate &candidate, int idx,
         bool isSelected) {
        auto *itemView = dynamic_cast<FindBmsCandidateItemView *>(view);
        if (itemView != nullptr) {
          itemView->setCandidate(candidate, static_cast<size_t>(idx),
                                 isSelected);
        }
      };
  findBmsCandidateRecyclerView->onSelected =
      [this](const BmsSearchCandidate &, int idx) {
        startFindBmsCandidateDownload(static_cast<size_t>(idx));
      };
  panel->addView(findBmsCandidateRecyclerView);

  findBmsProgressTrack = new View();
  findBmsProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setBackgroundColor(Color(8, 14, 23, 230))
      ->setBorderColor(Color(74, 101, 132, 255))
      ->setBorderWidth(2);
  findBmsProgressFill = new View();
  findBmsProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      Color(62, 168, 145, 240));
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
    openFindBmsResultUrl(
        BmsSearchService::searchUrlForText(findBmsTitleSearchQuery(
            findBmsModalChart)));
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
    auto result = service.downloadCandidate(
        candidate, record.meta.SHA256, record.meta.MD5, downloadRoot,
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
    findBmsStatusText->setColor(failed ? SDL_Color{255, 177, 170, 255}
                                       : SDL_Color{235, 243, 252, 255});
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
    detail += "SHA256: " + compactHashForModal(findBmsModalChart.meta.SHA256) +
              "\n";
  }
  if (!findBmsModalChart.meta.MD5.empty()) {
    detail += "MD5: " + compactHashForModal(findBmsModalChart.meta.MD5) +
              "\n";
  }
  if (!running && findBmsResult.status == BmsSearchResult::Status::Downloaded) {
    detail += "Saved to " +
              path_t_to_utf8(fspath_to_path_t(findBmsResult.outputPath)) +
              "\nRefreshing the library will make newly found charts playable.";
    if (!findBmsResult.debugPath.empty()) {
      detail += "\nDebug files: " +
                path_t_to_utf8(fspath_to_path_t(findBmsResult.debugPath));
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
    detail += hasSha
                  ? "No matching BMS Search page was available. Search by title."
                  : "Horie did not find a matching song. Search by title.";
  } else if (!running &&
             findBmsResult.status ==
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
      detail += "\nKept at " +
                path_t_to_utf8(fspath_to_path_t(findBmsResult.outputPath));
    }
    if (!findBmsResult.debugPath.empty()) {
      detail += "\nDebug files: " +
                path_t_to_utf8(fspath_to_path_t(findBmsResult.debugPath));
    }
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::DownloadFailed) {
    detail += "Automatic download failed. Open the source page or refresh "
              "after downloading.";
    if (!findBmsResult.debugPath.empty()) {
      detail += "\nDebug files: " +
                path_t_to_utf8(fspath_to_path_t(findBmsResult.debugPath));
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
      !downloaded &&
      (!findBmsModalChart.meta.SHA256.empty() ||
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
                       : (bmsSearchSource ? "Open BMS Search"
                                          : "Open Source"));
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

  styleActionButton(findBmsCloseButton, findBmsCloseButtonText, true,
                    Color(47, 54, 70, 220), Color(62, 72, 92, 232),
                    Color(78, 90, 114, 242), Color(118, 137, 160, 220));
  styleActionButton(findBmsOpenButton, findBmsOpenButtonText,
                    !running && hasSource, Color(29, 73, 120, 224),
                    Color(40, 96, 156, 236), Color(58, 129, 204, 246),
                    Color(105, 162, 222, 255));
  styleActionButton(findBmsGoogleButton, findBmsGoogleButtonText,
                    !running && hasSearchAction, Color(47, 54, 88, 224),
                    Color(65, 75, 119, 236), Color(82, 94, 148, 246),
                    Color(126, 141, 219, 255));
  styleActionButton(findBmsRefreshButton, findBmsRefreshButtonText,
                    hasRefreshAction, Color(38, 97, 87, 232),
                    Color(50, 121, 109, 242), Color(65, 146, 130, 250),
                    Color(112, 212, 191, 255));
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
  playOptionsModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(640)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12)
      ->setPadding(Edge::All, 22)
      ->setBackgroundColor(Color(13, 22, 35, 242))
      ->setBorderColor(Color(86, 118, 153, 255))
      ->setBorderWidth(2);

  auto *title = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title->setText("Play Options");
  title->setColor({245, 249, 255, 255});
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
  styleActionButton(playOptionsCloseButton, playOptionsCloseButtonText, true,
                    Color(47, 54, 70, 220), Color(62, 72, 92, 232),
                    Color(78, 90, 114, 242), Color(118, 137, 160, 220));
}

void MainMenuScene::showPlayOptionsModal() {
  if (playOptionsModalRoot == nullptr) {
    return;
  }

  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
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
  replayModalRoot->setBackgroundColor(Color(0, 0, 0, 164));

  auto *panel = new View();
  panel->setWidth(kModalPanelWidth)
      ->setHeight(620)
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(14)
      ->setPadding(Edge::All, 22)
      ->setBackgroundColor(Color(13, 22, 35, 242))
      ->setBorderColor(Color(86, 118, 153, 255))
      ->setBorderWidth(2);

  replayModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  replayModalTitleText->setText("Replay");
  replayModalTitleText->setColor({245, 249, 255, 255});
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
    refreshReplayModalActions();
  };
  replayListView->setFlex(1);
  replayListView->clearBackgroundColor();
  replayListView->setBorderColor(Color(55, 76, 102, 255));
  replayListView->setBorderWidth(2);
  replayListContent->addView(replayListView);
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
      ->setGap(18);
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
  replayExportProgressMessageText->setColor({235, 243, 252, 255});
  replayExportProgressMessageText->setHeight(38);
  replayExportProgressContent->addView(replayExportProgressMessageText);

  replayExportProgressTrack = new View();
  replayExportProgressTrack->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setBackgroundColor(Color(8, 14, 23, 230))
      ->setBorderColor(Color(74, 101, 132, 255))
      ->setBorderWidth(2);
  replayExportProgressFill = new View();
  replayExportProgressFill->setWidth(0)->setHeight(20)->setBackgroundColor(
      Color(62, 168, 145, 240));
  replayExportProgressTrack->addView(replayExportProgressFill);
  replayExportProgressContent->addView(replayExportProgressTrack);

  replayExportProgressPercentText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  replayExportProgressPercentText->setText("0%");
  replayExportProgressPercentText->setColor({173, 193, 216, 255});
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
  replayModalExportButton =
      makeModalButton("Export", 20, &replayModalExportButtonText);
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
  replaySummaries = ReplayDBHelper::GetInstance().ListReplays(record.meta);
  setReplayButtonVisible(!replaySummaries.empty());
  if (replaySummaries.empty()) {
    return;
  }

  selectedReplayIndex = -1;
  replayModalTitleText->setText("Replay");
  replayListContent->setVisible(true);
  replayExportOptionsContent->setVisible(false);
  replayExportProgressContent->setVisible(false);
  replayListView->setReplaySummaries(replaySummaries);
  replayModalRoot->setSize(rendering::window_width, rendering::window_height);
  replayModalRoot->setVisible(true);
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayout();
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
  refreshReplayExportOptionButtons();
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayout();
}

void MainMenuScene::showReplayExportProgress() {
  if (replayModalRoot == nullptr) {
    return;
  }

  replayModalTitleText->setText("Exporting Replay");
  replayListContent->setVisible(false);
  replayExportOptionsContent->setVisible(false);
  replayExportProgressContent->setVisible(true);
  updateReplayExportProgressUi(0.0, "Preparing export");
  replayModalRoot->setSize(rendering::window_width, rendering::window_height);
  replayModalRoot->setVisible(true);
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayout();
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
  if (replayModalExportButtonText != nullptr) {
    replayModalExportButtonText->setText("Export");
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

  if (replayModalCloseButtonText != nullptr) {
    replayModalCloseButtonText->setText(optionsMode ? "Back" : "Close");
  }
  if (replayModalExportButtonText != nullptr) {
    replayModalExportButtonText->setText(
        exportInProgress ? "Exporting"
                         : (optionsMode ? "Start Export" : "Export"));
  }

  if (replayWatchButton != nullptr) {
    replayWatchButton->setVisible(!optionsMode && !progressMode);
    replayWatchButton->setWidth((optionsMode || progressMode) ? 0.0f : 160.0f);
  }
  if (replayModalExportButton != nullptr) {
    replayModalExportButton->setVisible(!progressMode);
    replayModalExportButton->setWidth(progressMode ? 0.0f : 160.0f);
  }

  styleActionButton(replayModalCloseButton, replayModalCloseButtonText,
                    !exportInProgress, Color(47, 54, 70, 220),
                    Color(62, 72, 92, 232), Color(78, 90, 114, 242),
                    Color(118, 137, 160, 220));
  styleActionButton(replayWatchButton, replayWatchButtonText,
                    hasSelection && !optionsMode && !progressMode &&
                        !exportInProgress,
                    Color(29, 73, 120, 224), Color(40, 96, 156, 236),
                    Color(58, 129, 204, 246), Color(105, 162, 222, 255));
  styleActionButton(replayModalExportButton, replayModalExportButtonText,
                    hasSelection && !progressMode && !exportInProgress,
                    Color(47, 54, 88, 224), Color(65, 75, 119, 236),
                    Color(82, 94, 148, 246), Color(126, 141, 219, 255));

  if (replayModalRoot != nullptr) {
    replayModalRoot->applyYogaLayout();
  }
}

void MainMenuScene::refreshReplayExportOptionButtons() {
  styleOptionButton(replayFps60Button, replayFps60ButtonText,
                    selectedExportFps == 60);
  styleOptionButton(replayFps120Button, replayFps120ButtonText,
                    selectedExportFps == 120);
  styleOptionButton(replayResolution1080Button, replayResolution1080ButtonText,
                    !selectedExportFullResolution);
  styleOptionButton(replayResolutionFullButton, replayResolutionFullButtonText,
                    selectedExportFullResolution);
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
    replayModalRoot->applyYogaLayout();
  }
}

void MainMenuScene::startReplayPlayback(const ChartMetaRecord &record,
                                        int replayId) {
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

        auto replay =
            ReplayDBHelper::GetInstance().LoadReplay(replayId, record.meta);
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

        auto *chart = setSelectedChart(std::move(replayChart), true);
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
                              });
        willStart.store(false);
        return true;
      },
      0, true);
}

bms_parser::Chart *
MainMenuScene::setSelectedChart(std::unique_ptr<bms_parser::Chart> chart,
                                bool mediaReady) {
  bms_parser::Chart *raw = chart.get();
  std::unique_ptr<bms_parser::Chart> previous;
  {
    std::lock_guard<std::mutex> lock(selectedChartMutex);
    previous = std::move(selectedChart);
    selectedChart = std::move(chart);
    selectedChartMediaReady.store(mediaReady);
  }
  return raw;
}

void MainMenuScene::clearSelectedChart() {
  std::unique_ptr<bms_parser::Chart> previous;
  {
    std::lock_guard<std::mutex> lock(selectedChartMutex);
    previous = std::move(selectedChart);
    selectedChartMediaReady.store(false);
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
  if (selectedChart == nullptr ||
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
  if (!selectedChartMediaReady.load() || selectedChart == nullptr ||
      fspath_to_path_t(selectedChart->Meta.BmsPath) != fspath_to_path_t(path)) {
    return nullptr;
  }
  return selectedChart.get();
}

void MainMenuScene::resetStartLoadingUi() {
  willStart.store(false);
  if (startButtonText != nullptr) {
    startButtonText->setText("Start");
  }
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
      new GamePlayScene(context, chart, std::move(options)), true);
}

void MainMenuScene::startReplayVideoExport(const ChartMetaRecord &record,
                                           int replayId,
                                           ReplayVideoExportOptions options) {
  if (replayExportInProgress.exchange(true)) {
    return;
  }
  if (replayExportThread.joinable()) {
    replayExportThread.join();
  }

  willStart.store(true);
  previewLoadCancelled = true;
  selectedChartMediaReady.store(false);
  {
    std::lock_guard<std::mutex> lock(replayExportProgressMutex);
    pendingReplayExportProgress.reset();
  }
  showReplayExportProgress();
  if (replayStatusText != nullptr) {
    replayStatusText->setText("Exporting...");
  }

  options.progressCallback = [this](const ReplayVideoExportProgress &progress) {
    std::lock_guard<std::mutex> lock(replayExportProgressMutex);
    pendingReplayExportProgress = PendingReplayExportProgress{
        .fraction = progress.fraction,
        .message = progress.message,
    };
  };

  replayExportThread = std::jthread(
      [this, record, replayId, options](const std::stop_token &stopToken) {
        auto complete = [this](const ReplayVideoExportResult &result) {
          std::lock_guard<std::mutex> lock(replayExportResultMutex);
          pendingReplayExportResult = PendingReplayExportResult{
              .success = result.success,
              .outputPath = result.outputPath,
              .message = result.message,
          };
        };

        try {
          if (loadThread.joinable()) {
            loadThread.join();
          }
          context.jukebox.stop();
          if (stopToken.stop_requested()) {
            complete({.success = false, .message = "Replay export cancelled"});
            return;
          }

          auto replay =
              ReplayDBHelper::GetInstance().LoadReplay(replayId, record.meta);
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
          if (stopToken.stop_requested()) {
            complete({.success = false, .message = "Replay export cancelled"});
            return;
          }

          complete(ReplayVideoExporter::Export(context, chart.get(),
                                               replay.value(), options));
        } catch (const std::exception &e) {
          complete({.success = false, .message = e.what()});
        } catch (...) {
          complete({.success = false,
                    .message = "Unexpected replay export failure"});
        }
      });
}

void MainMenuScene::applyReplayVideoExportProgress() {
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

void MainMenuScene::applyReplayVideoExportResult() {
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
    if (result->success) {
      updateReplayExportProgressUi(1.0, result->message == "Saved to Photos"
                                            ? "Saved to Photos"
                                            : "Export complete");
    } else {
      const std::string failureMessage =
          (result->message == "No Replay" || result->message == "No Chart")
              ? result->message
              : "Export failed";
      updateReplayExportProgressUi(replayExportProgressFraction,
                                   failureMessage);
    }
    replayModalTitleText->setText(result->success ? "Export Complete"
                                                  : "Export Failed");
    refreshReplayModalActions();
  }

  if (result->success) {
    SDL_Log("Replay video exported: %s (%s)",
            result->outputPath.string().c_str(), result->message.c_str());
  } else {
    SDL_Log("Replay video export failed: %s (%s)", result->message.c_str(),
            result->outputPath.string().c_str());
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
  applyReplayVideoExportProgress();
  applyReplayVideoExportResult();
  if (parseLogModalRoot != nullptr && parseLogModalRoot->getVisible()) {
    refreshParseLogModal();
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
  playOptionsCloseButton = nullptr;
  playOptionsCloseButtonText = nullptr;
  replayListView = nullptr;
  replayWatchButton = nullptr;
  replayModalExportButton = nullptr;
  replayModalCloseButton = nullptr;
  replayFps60Button = nullptr;
  replayFps120Button = nullptr;
  replayResolution1080Button = nullptr;
  replayResolutionFullButton = nullptr;
  replayWatchButtonText = nullptr;
  replayModalExportButtonText = nullptr;
  replayModalCloseButtonText = nullptr;
  replayFps60ButtonText = nullptr;
  replayFps120ButtonText = nullptr;
  replayResolution1080ButtonText = nullptr;
  replayResolutionFullButtonText = nullptr;
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
  libraryProgressStage.store(static_cast<int>(ChartScanProgressStage::Preparing),
                             std::memory_order_relaxed);
  displayedLibraryTasksRevision = 0;
  displayedLibraryProgressRevision = 0;
  displayedLibraryTasksButtonText.clear();
  selectedChartMediaReady.store(false);
  replaySummaries.clear();
  selectedReplayIndex = -1;
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  replayExportProgressFraction = 0.0;
  gaugeSelectionButtons.clear();
  playOptionButtons.clear();
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
#else
    roots.emplace_back(entry.path);
#endif
  }

  if (stop_token.stop_requested()) {
    return;
  }

  SDL_Log("Refreshing chart library");
  const int changedCount =
      dbHelper.ScanChartRoots(db, roots, &stop_token, progressCallback,
                              pauseCallback,
                              [&scene]() {
                                return scene.pendingLibraryScanFlushRequest();
                              },
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

        if (filename.size() > 4) {
          path_t ext = filename.substr(filename.size() - 4);
          std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
          if (ext == L".bms" || ext == L".bme" || ext == L".bml") {
            path_t dirPath;

            path_t fullPath = path.wstring() + L"\\" + filename;
            if (oldFilesWs.find(fullPath) == oldFilesWs.end()) {
              diffs.push_back({fullPath, Added});
            }
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
#elif TARGET_OS_OSX || TARGET_OS_LINUX
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
  DIR *dir = opendir(directoryPath.c_str());
  if (dir) {
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
      if (stop_token.stop_requested()) {
        closedir(dir);
        break;
      }
      resolveDType(directoryPath, entry);
      if (entry->d_type == DT_REG) {
        std::string filename = entry->d_name;
        if (filename.size() > 4) {
          std::string ext = filename.substr(filename.size() - 4);
          std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
          if (ext == ".bms" || ext == ".bme" || ext == ".bml") {
            std::filesystem::path fullPath = directoryPath / filename;
            if (oldFiles.find(fspath_to_path_t(fullPath)) == oldFiles.end()) {
              diffs.push_back({fullPath, Added});
            }
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
    closedir(dir);
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
            directoryPath.string().c_str(), error.message().c_str());
    return;
  }

  for (const auto end = std::filesystem::directory_iterator();
       iterator != end; iterator.increment(error)) {
    if (stop_token.stop_requested()) {
      break;
    }
    if (error) {
      SDL_Log("Failed while reading iOS directory: %s (%s)",
              directoryPath.string().c_str(), error.message().c_str());
      error.clear();
      continue;
    }

    const std::filesystem::directory_entry &entry = *iterator;
    std::error_code typeError;
    if (entry.is_regular_file(typeError)) {
      std::string ext = entry.path().extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(),
                     [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                     });
      if (ext == ".bms" || ext == ".bme" || ext == ".bml") {
        if (oldFilesWs.find(fspath_to_path_t(entry.path())) ==
            oldFilesWs.end()) {
          diffs.push_back({entry.path(), Added});
        }
      }
    } else if (!typeError && entry.is_directory(typeError)) {
      directoriesToVisit.push_back(entry.path());
    } else if (typeError) {
      SDL_Log("Failed to inspect iOS path: %s (%s)",
              entry.path().string().c_str(), typeError.message().c_str());
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
#elif TARGET_OS_OSX || TARGET_OS_LINUX
    FindFilesUnix(currentDir, diffs, oldFilesWs, directoriesToVisit,
                  stop_token);
#elif TARGET_OS_IOS || TARGET_OS_SIMULATOR
    FindFilesIOS(currentDir, diffs, oldFilesWs, directoriesToVisit, stop_token);
#endif
  }
}
