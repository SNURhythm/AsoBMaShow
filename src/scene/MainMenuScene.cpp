#include "MainMenuScene.h"
#include "MainMenuLibrary.h"
#include "../ArchiveFile.h"
#include "../BmsChartFile.h"
#include "../DifficultyTableImporter.h"
#include "../CourseConstraintUtils.h"
#include "../LongNoteModeUtils.h"
#include "../audio/MusicPlaylist.h"
#include "../tinyfiledialogs.h"
#include <fstream>
#include <algorithm>
#include "../repositories/ReplayRepository.h"
#include "../ReplayAutoPlay.h"
#include "../ReplayVideoExporter.h"
#include "../ResultRecallBuilder.h"
#include "../PlayOptionUtils.h"
#include "../ProfileDatabaseActivity.h"
#include "../RAII.h"
#include "../repositories/ScoreCacheQueries.h"
#include "../repositories/SqliteRAII.h"
#include "../ir/tachi/TachiBatchManual.h"
#include "../ir/IrProfileSettings.h"
#include "../ir/IrReplayRecordState.h"
#include "../ir/IrSavedResultUpload.h"
#include "../ir/IrSubmissionService.h"
#include "../path.h"
#include "../view/ChartListItemView.h"
#include "../view/IconText.h"
#include "../view/LibraryFolderItemView.h"
#include "../view/OverlayPortal.h"
#include "../view/PlayOptionsPanelView.h"
#include "../view/TextView.h"
#include "../view/TextInputBox.h"
#include "../Utils.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/BlockingOverlayView.h"
#include "ChartViewerScene.h"
#include "FindBmsDialogPolicy.h"
#include "IrUploadsScene.h"
#include "MusicPlayerScene.h"
#include "RemoteResultRecallController.h"
#include "ResultScene.h"
#include "play/GamePlayScene.h"
#include "play/GameplayGaugeRules.h"
#include "play/Pacemaker.h"
#include "../view/ClearLampColors.h"
#include "../view/DropdownView.h"
#include "../view/ResultRecordListView.h"
#include "../view/ScrollView.h"
#include "../view/UiTheme.h"
#include <array>
#include <chrono>
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
constexpr auto kPreviewDebounceDelay = std::chrono::milliseconds(100);
constexpr size_t kFindBmsMaxLogLines = 120;
constexpr size_t kFindBmsMaxPendingProgressEvents = 160;
// Keep this below the modal's nominal width because row padding and the
// scrollbar gutter reduce the usable text area.
constexpr size_t kParseLogRowMaxColumns = 88;
constexpr int kParseLogRowHeight = 48;
constexpr uint32_t kIconXmark = 0xf00d;
constexpr uint32_t kIconFilter = 0xf0b0;
constexpr uint32_t kIconSort = 0xf0dc;
constexpr uint32_t kIconFileLines = 0xf15c;
constexpr uint32_t kIconCalculator = 0xf1ec;

ir::IrRecordActivity
recordActivityFor(ir::IrActiveRequestKind activeRequest) noexcept {
  switch (activeRequest) {
  case ir::IrActiveRequestKind::None:
    return ir::IrRecordActivity::None;
  case ir::IrActiveRequestKind::Submit:
    return ir::IrRecordActivity::Submitting;
  case ir::IrActiveRequestKind::Poll:
    return ir::IrRecordActivity::Polling;
  }
  return ir::IrRecordActivity::None;
}

constexpr const char *kDefaultDifficultyTableUrls[] = {
    "https://rattoto10.jounin.jp/table.html",
    "https://rattoto10.jounin.jp/table_insane.html",
    "https://stellabms.xyz/sl/table.html",
    "https://stellabms.xyz/st/table.html",
};

std::string formatGaugeTotal(const bms_parser::ChartMeta &meta,
                             GameplayRuleset ruleset) {
  const double total = resolveEffectiveGaugeTotal(ruleset, meta);
  std::ostringstream stream;
  stream << std::fixed << std::setprecision(2) << total;
  std::string value = stream.str();
  while (!value.empty() && value.back() == '0') value.pop_back();
  if (!value.empty() && value.back() == '.') value.pop_back();
  return value;
}

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
    {"LIGHT ASSIST", kClearTypeLightAssistedEasyClearRank},
    {"A-CLEAR", kClearTypeAssistedEasyClearRank},
    {"FAILED", kClearTypeFailedRank},
    {"NO PLAY", kNoClearTypeRank},
};

std::string trimAsciiWhitespace(std::string text) {
  const auto first = std::find_if_not(
      text.begin(), text.end(),
      [](unsigned char ch) { return std::isspace(ch) != 0; });
  const auto last = std::find_if_not(
      text.rbegin(), text.rend(),
      [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
  if (first >= last) {
    return "";
  }
  return std::string(first, last);
}

std::optional<double> parseOptionalBpmFilter(const std::string &text) {
  const std::string trimmed = trimAsciiWhitespace(text);
  if (trimmed.empty()) {
    return std::nullopt;
  }

  char *end = nullptr;
  const double value = std::strtod(trimmed.c_str(), &end);
  if (end == trimmed.c_str() || end == nullptr || *end != '\0' ||
      !std::isfinite(value)) {
    return std::nullopt;
  }
  return std::max(0.0, value);
}

std::optional<size_t>
difficultyLevelIndex(const std::vector<DifficultyLevelInfo> &levels,
                     const std::optional<std::string> &level) {
  return chart_record_filters::difficultyLevelIndex(levels, level);
}

void normalizeDifficultyFilterRange(
    ChartRecordFilters &filters,
    const std::vector<DifficultyLevelInfo> &levels) {
  chart_record_filters::normalizeDifficultyRange(filters, levels);
}

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

class ParseLogRowView : public View {
public:
  ParseLogRowView() : View() {
    setFlexDirection(FlexDirection::Column);
    setJustifyContent(YGJustifyCenter);
    setPadding(Edge::Left, 10);
    setPadding(Edge::Right, 14);

    label = new TextView("assets/fonts/notosanscjkjp.ttf", 15);
    label->setThemedColor(ui_theme::textSecondary);
    label->setWrap(true);
    label->setOverflow(TextView::TextOverflow::Hidden);
    label->setVAlign(TextView::MIDDLE);
    label->setFlex(1);
    addView(label);
  }

  void setRow(const MainMenuParseLogRow &row) {
    if (label != nullptr) {
      label->setText(row.text);
    }
  }

private:
  TextView *label = nullptr;
};

size_t utf8CodepointLengthAt(const std::string &text, size_t offset,
                             char32_t *codepoint = nullptr) {
  if (offset >= text.size()) {
    return 0;
  }
  const unsigned char ch = static_cast<unsigned char>(text[offset]);
  size_t length = 1;
  char32_t decoded = ch;
  if ((ch & 0x80) == 0x00) {
    length = 1;
  } else if ((ch & 0xE0) == 0xC0) {
    length = 2;
    decoded = static_cast<char32_t>(ch & 0x1F);
  } else if ((ch & 0xF0) == 0xE0) {
    length = 3;
    decoded = static_cast<char32_t>(ch & 0x0F);
  } else if ((ch & 0xF8) == 0xF0) {
    length = 4;
    decoded = static_cast<char32_t>(ch & 0x07);
  }
  if (offset + length > text.size()) {
    if (codepoint != nullptr) {
      *codepoint = ch;
    }
    return 1;
  }
  for (size_t i = 1; i < length; ++i) {
    const unsigned char continuation =
        static_cast<unsigned char>(text[offset + i]);
    if ((continuation & 0xC0) != 0x80) {
      if (codepoint != nullptr) {
        *codepoint = ch;
      }
      return 1;
    }
    decoded = (decoded << 6) | static_cast<char32_t>(continuation & 0x3F);
  }
  if (codepoint != nullptr) {
    *codepoint = decoded;
  }
  return length;
}

bool isWideLogCodepoint(char32_t codepoint) {
  return (codepoint >= 0x1100 && codepoint <= 0x115F) ||
         (codepoint >= 0x2329 && codepoint <= 0x232A) ||
         (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
         (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
         (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
         (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
         (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||
         (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
         (codepoint >= 0xFFE0 && codepoint <= 0xFFE6);
}

size_t parseLogCodepointColumns(char32_t codepoint) {
  if (codepoint == '\t') {
    return 4;
  }
  if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint < 0xA0)) {
    return 1;
  }
  return isWideLogCodepoint(codepoint) ? 2 : 1;
}

bool isLogLineBreak(char ch) { return ch == '\n' || ch == '\r'; }

void appendParseLogRowsForLine(std::vector<MainMenuParseLogRow> &rows,
                               const std::string &line, std::uint64_t &rowId) {
  if (line.empty()) {
    rows.push_back({rowId++, " "});
    return;
  }

  size_t offset = 0;
  bool continuation = false;
  while (offset < line.size()) {
    if (isLogLineBreak(line[offset])) {
      if (line[offset] == '\r' && offset + 1 < line.size() &&
          line[offset + 1] == '\n') {
        offset += 2;
      } else {
        ++offset;
      }
      continuation = false;
      continue;
    }

    const size_t chunkStart = offset;
    size_t columns = 0;
    const size_t columnLimit =
        std::max<size_t>(1, kParseLogRowMaxColumns - (continuation ? 2 : 0));
    while (offset < line.size() && !isLogLineBreak(line[offset])) {
      char32_t codepoint = 0;
      const size_t length = utf8CodepointLengthAt(line, offset, &codepoint);
      if (length == 0) {
        break;
      }
      const size_t codepointColumns = parseLogCodepointColumns(codepoint);
      if (columns > 0 && columns + codepointColumns > columnLimit) {
        break;
      }
      offset += length;
      columns += codepointColumns;
    }

    std::string rowText = line.substr(chunkStart, offset - chunkStart);
    if (rowText.empty()) {
      rowText = " ";
    } else if (continuation) {
      rowText.insert(0, "  ");
    }
    rows.push_back({rowId++, std::move(rowText)});
    continuation = offset < line.size() && !isLogLineBreak(line[offset]);
  }
}

std::vector<MainMenuParseLogRow>
parseLogRowsFromLines(const std::vector<std::string> &lines) {
  std::vector<MainMenuParseLogRow> rows;
  if (lines.empty()) {
    rows.push_back({0, "No parsing logs yet."});
    return rows;
  }

  std::uint64_t rowId = 0;
  for (const std::string &line : lines) {
    appendParseLogRowsForLine(rows, line, rowId);
  }
  return rows;
}

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
  case GaugeType::Hazard:
    return kClearTypeFullComboRank;
  case GaugeType::Normal:
  default:
    return kClearTypeNormalClearRank;
  }
}

std::string gaugeButtonLabel(GaugeType gaugeType,
                             GaugeAutoShiftMode autoShift) {
  if (gaugeAutoShiftEnabled(autoShift)) {
    return gaugeAutoShiftMenuLabel(autoShift);
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
  case GaugeType::Hazard:
    return "HAZARD";
  default:
    return "NORMAL";
  }
}

SDL_Color readyGaugeTextColor(GaugeType gaugeType,
                              GaugeAutoShiftMode autoShift) {
  if (gaugeAutoShiftEnabled(autoShift)) {
    return SDL_Color{255, 205, 37, 255};
  }

  const Color color = clearLampColorForRank(clearRankForGaugeType(gaugeType));
  return SDL_Color{color.r, color.g, color.b, 255};
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

Button *makeModalIconButton(uint32_t iconCodepoint, int fontSize,
                            TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, 54, 54);
  auto *text = new TextView(ui_icons::kFontAwesomeSolidPath, fontSize);
  text->setText(ui_icons::textForCodepoint(iconCodepoint));
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  text->setOverflow(TextView::TextOverflow::Hidden);
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

void MainMenuScene::ChartListPageCache::reset(
    ChartRepository::Session &chartSession, const ChartMetaQuery &chartQuery,
    int count, std::optional<ChartMetaRecord> leading) {
  session = &chartSession;
  query = chartQuery;
  query.limit = 0;
  query.offset = 0;
  leadingRecord = std::move(leading);
  totalCount = std::max(0, count) + (leadingRecord.has_value() ? 1 : 0);
  pages.clear();
  pageOrder.clear();
}

void MainMenuScene::ChartListPageCache::releasePages() {
  pages.clear();
  pageOrder.clear();
}

void MainMenuScene::ChartListPageCache::clear() {
  session = nullptr;
  totalCount = 0;
  leadingRecord.reset();
  pages.clear();
  pageOrder.clear();
}

const ChartMetaRecord &MainMenuScene::ChartListPageCache::get(int index) const {
  if (index < 0 || index >= totalCount) {
    return fallbackRecord;
  }
  if (leadingRecord.has_value()) {
    if (index == 0) {
      return *leadingRecord;
    }
    index--;
  }
  if (session == nullptr) {
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
    session->QueryChartMeta(pageQuery, records);
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
  chartSession =
      context.chartRepository.OpenSession(&context.scoreRepository);
  auto profileOperationBlocker = [this]() -> std::optional<std::string> {
    if (libraryActiveTaskCount.load(std::memory_order_acquire) > 0 ||
        androidArchiveImportCopyPending.load(std::memory_order_acquire)) {
      return "A chart library scan or import is active.";
    }
    if (replayExportInProgress.load(std::memory_order_acquire)) {
      return "A replay export is active.";
    }
    if (unzipInProgress.load(std::memory_order_acquire) ||
        findBmsJobRunning.load(std::memory_order_acquire)) {
      return "A chart archive operation is active.";
    }
    if (addFolderPickerInProgress.load(std::memory_order_acquire) ||
        archiveImportPickerInProgress.load(std::memory_order_acquire)) {
      return "A chart import picker is active.";
    }
    if (willStart.load(std::memory_order_acquire)) {
      return "A chart or replay transition is active.";
    }
    return std::nullopt;
  };
  context.profileSwitchBlockers.background = profileOperationBlocker;
  context.profileSwitchBlockers.scene = std::move(profileOperationBlocker);
  context.refreshProfileCaches = [this]() {
    // Profile switching happens while Main Menu is paused. Defer score DB
    // attachment and view work until onResume(), when the active profile is
    // fully committed and the scene owns its database dependencies again.
    scoreClearRanks = {};
    scoreBestScores = {};
    folderClearData = {};
    scoreClearRanksRevision = 0;
    observedIrReconciliationRevision = 0;
    observedIrAccountEvidenceRevision = 0;
    replayIrObservedRevisions.clear();
  };
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
  context.requestRebuildChartLibrary = [this]() {
    startLibraryRebuild();
  };
  context.notifyBackgroundTaskPauseStateChanged = [this]() {
    syncLibraryTaskPauseStateWithForegroundScene();
  };
  initView(context);
  SDL_Log("Main Menu Scene Initialized");
  syncLibraryTaskPauseStateWithForegroundScene();
  startLibraryTaskWorker();
  enqueueLibraryRefreshTask("Refresh Library");
}

void MainMenuScene::onPause() {
  if (rankingsModal) {
    rankingsModal->close();
  }
  chartListCache.releasePages();
}

void MainMenuScene::onResume() {
  context.profileSwitchBlockers.scene =
      context.profileSwitchBlockers.background;
  replayIrObservedRevisions.clear();
  applyThemeChange();
  const bool scoreQueryReady = !prepareScoreQueryDatabase().has_value();
  // Gameplay selections belong to the committed profile even when its score
  // attachment is temporarily unavailable.
  reloadProfileSelectionsFromSettings();
  syncLibraryTaskPauseStateWithForegroundScene();
  startLibraryTaskWorker();
  if (scoreQueryReady) {
    if (refreshScoreClearRankViews().has_value()) {
      scoreClearRanks = {};
      scoreBestScores = {};
      folderClearData = {};
      scoreClearRanksRevision = 0;
      refreshLongNoteModeClearRankViews();
    }
  } else {
    // Never render the previous profile's score-derived state while attachment
    // retries continue from update().
    scoreClearRanks = {};
    scoreBestScores = {};
    folderClearData = {};
    scoreClearRanksRevision = 0;
    refreshLongNoteModeClearRankViews();
  }
  refreshLibraryIfNeeded();
  reselectCurrentChart();
}

void MainMenuScene::reloadProfileSelectionsFromSettings() {
  const std::string previousLongNoteMode = profileSelections.longNoteMode;
  const bool refreshLongNoteQueries =
      profileSelectionsInitialized &&
      previousLongNoteMode != context.settings.selectedLnMode;
  profileSelections.reload(context.settings);
  profileSelectionsInitialized = true;

  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshPacemakerTargetButtons();
  refreshSelectedChartActionState();
  if (refreshLongNoteQueries) {
    refreshLongNoteModeClearRankViews();
  }
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
  refreshChartFilterButtons();
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
  const bool workerPaused = libraryTaskWorkerPaused.load();
  if (workerPaused) {
    return;
  }
  if (checkEntriesThread.joinable()) {
    return;
  }
  if (!workerPaused) {
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

void MainMenuScene::syncLibraryTaskPauseStateWithForegroundScene() {
  if (context.backgroundTasksPausedForForegroundScene.load()) {
    pauseLibraryTaskWorker();
    return;
  }

  resumeLibraryTaskWorker();
  startLibraryTaskWorker();
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
    const std::string &iosBookmark, bool rebuildLibraryMetadata) {
  const std::uint64_t id = nextLibraryTaskId.fetch_add(1);
  {
    std::lock_guard<std::mutex> lock(libraryTaskMutex);
    libraryTaskQueue.push_back(LibraryTaskRequest{
        .id = id,
        .kind = LibraryTaskKind::RefreshLibrary,
        .title = title,
        .folderToAdd = folderToAdd,
        .iosBookmark = iosBookmark,
        .rebuildLibraryMetadata = rebuildLibraryMetadata,
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
  startLibraryTaskWorker();
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
        if (stopToken.stop_requested()) {
          return true;
        }
        return !libraryTaskWorkerPaused.load() && !libraryTaskQueue.empty();
      });
      if (stopToken.stop_requested()) {
        break;
      }
      auto taskIt = libraryTaskQueue.begin();
      if (libraryTaskWorkerPaused.load()) {
        continue;
      }
      if (taskIt == libraryTaskQueue.end()) {
        continue;
      }
      task = *taskIt;
      libraryTaskQueue.erase(taskIt);
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
    ChartRepository::Session &chartSession, std::uint64_t taskId,
    const std::stop_token &stopToken) {
  if (context.settings.defaultDifficultyTablesSeeded ||
      stopToken.stop_requested()) {
    return;
  }

  constexpr int totalTables =
      static_cast<int>(sizeof(kDefaultDifficultyTableUrls) /
                       sizeof(kDefaultDifficultyTableUrls[0]));
  int successfulTables = 0;
  bool allSucceeded = true;
  DifficultyTableImporter importer;

  for (int i = 0; i < totalTables; ++i) {
    if (stopToken.stop_requested()) {
      return;
    }

    const char *url = kDefaultDifficultyTableUrls[i];
    setLibraryTaskState(taskId, LibraryTaskStatus::Running, 0.02, i,
                        totalTables, "Adding default difficulty tables");

    std::string errorMessage;
    const bool ok = importer.ImportFromUrl(
        chartSession, url, &errorMessage,
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
    if (!context.saveSettings()) {
      SDL_Log("Failed to save default difficulty table seed setting");
    }
  }
  if (successfulTables > 0) {
    requestLibraryReload(true);
  }
}

void MainMenuScene::runLibraryRefreshTask(const LibraryTaskRequest &task,
                                          const std::stop_token &stopToken) {
  auto taskSession = context.chartRepository.OpenSession();
  if (!taskSession.has_value()) {
    throw std::runtime_error("Failed to open chart database");
  }
  taskSession->EnsureSchema();

  auto pauseTask = [&]() {
    return waitForLibraryTaskResume(task.id, stopToken);
  };
  if (!pauseTask()) {
    return;
  }

  std::vector<ChartEntry> entries;
  if (!task.folderToAdd.empty()) {
    setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.01, 0, 0,
                        "Adding folder");
    if (!taskSession->InsertEntry(task.folderToAdd, task.iosBookmark)) {
      throw std::runtime_error("Failed to add folder");
    }
    requestLibraryReload(true);
    entries.push_back({
        .path = fspath_to_path_t(task.folderToAdd),
        .iosBookmark = task.iosBookmark,
    });
  }

  setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.02, 0, 0,
                      "Importing difficulty tables");
  if (!pauseTask()) {
    return;
  }
  seedDefaultDifficultyTablesIfNeeded(*taskSession, task.id, stopToken);
  if (stopToken.stop_requested() || !pauseTask()) {
    return;
  }
  DifficultyTableImporter importer;
  const int importedTables = importer.ImportFromDirectory(
      *taskSession, Utils::GetDocumentsPath("tables"));
  if (importedTables > 0 && !stopToken.stop_requested()) {
    requestLibraryReload(true);
  }
  if (entries.empty()) {
    entries = taskSession->SelectEffectiveEntries();
  }

  if (stopToken.stop_requested()) {
    return;
  }

  if (entries.empty()) {
    setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.04, 0, 0,
                        "Waiting for library folder");
    if (!pauseTask()) {
      return;
    }
    constexpr auto bootstrapMode =
        main_menu_library::emptyLibraryBootstrapMode(TARGET_PLATFORM);
    if constexpr (bootstrapMode ==
                  main_menu_library::EmptyLibraryBootstrapMode::DefaultFolder) {
      const auto path = ChartRepository::DefaultBmsFolderPath();
      ensureLibraryFolderExists(path);
      if (!taskSession->InsertEntry(path)) {
        throw std::runtime_error("Failed to add default library folder");
      }
      entries = taskSession->SelectEffectiveEntries();
      if (entries.empty()) {
        throw std::runtime_error(
            "Default library folder was not available after insertion");
      }
    } else {
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
            std::cout
                << "Could not expand ~ because no home directory is set.\n";
            folder.clear();
            continue;
          }
          std::ifstream test(folder);
          if (!test) {
            folder.clear();
          }
        }

        if (folder.empty()) {
          return;
        }
      } else {
        folder = folder_c;
      }
      const std::filesystem::path path(folder);
      if (!taskSession->InsertEntry(path)) {
        throw std::runtime_error("Failed to add selected library folder");
      }
      entries = taskSession->SelectEffectiveEntries();
    }
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
  if (task.rebuildLibraryMetadata) {
    setLibraryTaskState(task.id, LibraryTaskStatus::Running, 0.08, 0, 0,
                        "Clearing library caches");
    if (!pauseTask()) {
      return;
    }
    archive_file::appendDebugLogLine(
        "Manual library rebuild requested; clearing chart metadata caches.");
    if (!taskSession->ClearChartMeta()) {
      throw std::runtime_error("Failed to clear chart metadata cache");
    }
    requestLibraryReload(true);
  }
  LoadCharts(
      *taskSession, entries, *this, stopToken,
      [this, taskId = task.id](const ChartScanProgress &progress) {
        updateLibraryTaskProgress(taskId, progress);
      },
      [this, taskId = task.id, &stopToken]() {
        return waitForLibraryTaskResume(taskId, stopToken);
      });
}

bool MainMenuScene::insertChartFolderEntryImmediately(
    const std::filesystem::path &folderPath, const std::string &iosBookmark) {
  if (folderPath.empty()) {
    return false;
  }

  auto entrySession = context.chartRepository.OpenSession();
  if (!entrySession.has_value()) {
    SDL_Log("Failed to open chart database while adding folder %s",
            fspath_to_utf8(folderPath).c_str());
    return false;
  }

  if (!entrySession->InsertEntry(folderPath, iosBookmark)) {
    SDL_Log("Failed to add chart folder entry %s",
            fspath_to_utf8(folderPath).c_str());
    return false;
  }

  requestLibraryReload(true);
  return true;
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
        insertChartFolderEntryImmediately(folderPath, bookmark);
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
        insertChartFolderEntryImmediately(folderPath, treeUri);
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
  const std::filesystem::path outputRoot = ChartRepository::DefaultBmsFolderPath();

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

  auto importSession = context.chartRepository.OpenSession();
  if (!importSession.has_value()) {
    throw std::runtime_error("Imported " + importType +
                             ". Failed to refresh library.");
  }

  importSession->EnsureSchema();
  importSession->InsertEntry(outputRoot);

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
  ChartLibraryScanner scanner;
  const int changedCount = scanner.Scan(
      *importSession, roots, &stopToken, scanProgress, pauseTask);
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
  overlayPortal = nullptr;
  jacketView = nullptr;
  searchBox = nullptr;
  chartFilterPanel = nullptr;
  chartSortPanel = nullptr;
  selectedChartRecord.reset();
  chartFilterButton = nullptr;
  chartFilterButtonText = nullptr;
  chartSortButton = nullptr;
  chartSortButtonText = nullptr;
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
  irUploadsButton = nullptr;
  irUploadsButtonText = nullptr;
  tasksButton = nullptr;
  tasksButtonText = nullptr;
  replayButtonText = nullptr;
  replayStatusText = nullptr;
  replayModalRoot = nullptr;
  replayModalContentFrame = nullptr;
  replayListContent = nullptr;
  replayFilterSortContent = nullptr;
  replayWatchOptionsContent = nullptr;
  replayExportOptionsContent = nullptr;
  replayExportProgressContent = nullptr;
  replayExportProgressTrack = nullptr;
  replayExportProgressFill = nullptr;
  replayModalTitleText = nullptr;
  replayExportProgressMessageText = nullptr;
  replayExportProgressPercentText = nullptr;
  startButtonText = nullptr;
  playOptionsModalRoot = nullptr;
  playOptionsPanel = nullptr;
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
  parseLogRecyclerView = nullptr;
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
  findBmsCloseButton = nullptr;
  findBmsKeepFilesButton = nullptr;
  findBmsDeleteFilesButton = nullptr;
  findBmsOpenButton = nullptr;
  findBmsGoogleButton = nullptr;
  findBmsRefreshButton = nullptr;
  findBmsCloseButtonText = nullptr;
  findBmsKeepFilesButtonText = nullptr;
  findBmsDeleteFilesButtonText = nullptr;
  findBmsOpenButtonText = nullptr;
  findBmsGoogleButtonText = nullptr;
  findBmsRefreshButtonText = nullptr;
  readyGaugeText = nullptr;
  readyTotalRow = nullptr;
  readyTotalIconText = nullptr;
  readyTotalText = nullptr;
  readyPlayOptionText = nullptr;
  readyAssistOptionText = nullptr;
  readyPacemakerText = nullptr;
  readyPlayOptionsButton = nullptr;
  playOptionsCloseButton = nullptr;
  playOptionsCloseButtonText = nullptr;
  replayListView = nullptr;
  replayWatchButton = nullptr;
  replayGBattleButton = nullptr;
  replayModalResultButton = nullptr;
  replayModalExportButton = nullptr;
  replayModalFilterButton = nullptr;
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
  replayGBattleButtonText = nullptr;
  replayModalResultButtonText = nullptr;
  replayModalExportButtonText = nullptr;
  replayModalFilterButtonText = nullptr;
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
  replayResultRecallInProgress = false;
  replayIrUploadInProgress = false;
  replayIrUploadFeedbackRevision = 0;
  replayIrObservedRevisions.clear();
  unzipInProgress = false;
  tasksModalOpenRequested = false;
  findBmsJobRunning = false;
  findBmsCancelled = false;
  findBmsResult = {};
  findBmsPendingDecision.reset();
  findBmsProgressMessage.clear();
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.0;
  findBmsProgressLog.clear();
  musicStatusMessage.clear();
  chartRecordFilters = {};
  chartFilterPanelVisible = false;
  chartSortPanelVisible = false;
  chartBpmMinText.clear();
  chartBpmMaxText.clear();
  chartClearMarkDropdownOpen = false;
  chartScoreRankDropdownOpen = false;
  chartDifficultyMinDropdownOpen = false;
  chartDifficultyMaxDropdownOpen = false;
  replaySummaries.clear();
  visibleResultRecordSummaries.clear();
  resultRecordSummaries.clear();
  selectedResultRecordStableKey.reset();
  publishedResultRecordDiagnostic.clear();
  replayRecordFilters = {};
  selectedReplayIndex = -1;
  selectedReplaySummary.reset();
  selectedResultRecordSummary.reset();
  replayExportSelection.reset();
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  selectedExportIncludeResultScreen = true;
  selectedReplayRenderTouchPoints = true;
  selectedReplayRenderGhosts = true;
  replayExportProgressFraction = 0.0;
  replayClearFilterButtons.clear();
  replayPlayOptionFilterButtons.clear();
  replayScoreRankFilterButtons.clear();
  replaySortButtons.clear();

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
  if (!chartSession.has_value()) {
    SDL_Log("Chart repository session is unavailable");
    return;
  }
  chartSession->EnsureSchema();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  RefreshIOSFolderAccess(chartSession->SelectEffectiveEntries());
#elif TARGET_OS_ANDROID
  (void)chartSession->SelectEffectiveEntries();
#endif

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
    if (!item.courseStart && !item.solidArchive && !item.unavailable &&
        !item.meta.BmsPath.empty()) {
      const auto bestScore = scoreBestScores.bestFor(
          item.meta,
          long_note_mode::valueFromId(profileSelections.longNoteMode));
      if (bestScore.has_value()) {
        const int fallbackMaxScore = std::max(0, item.meta.TotalNotes) * 2;
        chartListItemView->setBestScoreRank(
            bestScore->score,
            bestScore->maxScore > 0 ? bestScore->maxScore : fallbackMaxScore);
      } else {
        chartListItemView->setBestScoreRank(0, 0);
      }
    } else {
      chartListItemView->setBestScoreRank(0, 0);
    }
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
    selectedChartRecord = item;
    refreshRankingsButton();
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
      retirePreviewLoadThread(true);
      clearSelectedChart();
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
    retirePreviewLoadThread(true);
    clearSelectedChart();
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
    schedulePreviewLoad(meta);
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

  overlayPortal = new OverlayPortal(0, 0, rendering::window_width,
                                    rendering::window_height);
  overlayPortal->setPositionType(YGPositionTypeAbsolute);
  overlayPortal->setPosition(Edge::Left, 0);
  overlayPortal->setPosition(Edge::Top, 0);
  overlayPortal->setZIndex(2000);

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

  irUploadsButton =
      makeModalButton("IR Uploads", 20, &irUploadsButtonText);
  irUploadsButton->setWidth(154);
  irUploadsButton->setHeight(50);
  irUploadsButton->setOnClickListener([this, &context]() {
    if (context.sceneManager != nullptr) {
      cancelPreviewLoading(true);
      context.sceneManager->changeScene(
          std::make_unique<IrUploadsScene>(context), true);
    }
  });
  styleThemedActionButton(irUploadsButton, irUploadsButtonText, true,
                          ui_theme::control, ui_theme::controlHover,
                          ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  libraryHeader->addView(irUploadsButton);

  tasksButton = makeModalButton("0 Tasks", 20, &tasksButtonText);
  tasksButton->setWidth(142);
  tasksButton->setHeight(50);
  tasksButton->setOnClickListener([this]() { showTasksModal(); });
  styleThemedActionButton(tasksButton, tasksButtonText, true, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  libraryHeader->addView(tasksButton);
  left->addView(libraryHeader);

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

  chartFilterButton =
      makeModalIconButton(kIconFilter, 20, &chartFilterButtonText);
  chartFilterButton->setWidth(56);
  chartFilterButton->setHeight(56);
  chartFilterButton->setFlexShrink(0.0f);
  chartFilterButton->setOnClickListener([this]() {
    setChartFilterPanelVisible(!chartFilterPanelVisible);
  });
  filterRow->addView(chartFilterButton);

  chartSortButton =
      makeModalIconButton(kIconSort, 20, &chartSortButtonText);
  chartSortButton->setWidth(56);
  chartSortButton->setHeight(56);
  chartSortButton->setFlexShrink(0.0f);
  chartSortButton->setOnClickListener([this]() {
    setChartSortPanelVisible(!chartSortPanelVisible);
  });
  filterRow->addView(chartSortButton);

  auto *filterLabel = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  filterLabel->setText("Search");
  filterLabel->setThemedColor(ui_theme::textSecondary);
  left->addView(filterLabel);
  left->addView(filterRow);

  chartFilterPanel = new ChartFilterPanelView({
      .onClearMarkChanged =
          [this](std::optional<int> rank) { setChartClearFilter(rank); },
      .onScoreRankChanged = [this](std::optional<std::string> rank) {
        setChartScoreRankFilter(std::move(rank));
      },
      .onBpmMinChanged =
          [this](const std::string &text) { setChartBpmMinFilter(text); },
      .onBpmMaxChanged =
          [this](const std::string &text) { setChartBpmMaxFilter(text); },
      .onDifficultyMinChanged = [this](std::optional<std::string> level) {
        setChartDifficultyMinFilter(std::move(level));
      },
      .onDifficultyMaxChanged = [this](std::optional<std::string> level) {
        setChartDifficultyMaxFilter(std::move(level));
      },
      .onClearMarkDropdownChanged = [this](bool open) {
        setChartClearMarkDropdownOpen(open);
      },
      .onScoreRankDropdownChanged = [this](bool open) {
        setChartScoreRankDropdownOpen(open);
      },
      .onClearMarkRangeChanged = [this](bool orAbove, bool orBelow) {
        setChartClearMarkRange(orAbove, orBelow);
      },
      .onScoreRankRangeChanged = [this](bool orAbove, bool orBelow) {
        setChartScoreRankRange(orAbove, orBelow);
      },
      .onDifficultyDropdownChanged = [this](bool minLevel, bool open) {
        setChartDifficultyDropdownOpen(minLevel, open);
      },
  });

  chartSortPanel = new ChartSortPanelView({
      .onSortChanged = [this](ChartRecordSortCriterion criterion) {
        setChartSortCriterion(criterion);
      },
  });

  left->addView(chartFilterPanel);
  left->addView(chartSortPanel);
  refreshChartFilterPanel();

  recyclerView->setFlex(1);
  recyclerView->clearBackgroundColor();
  recyclerView->setBorderWidth(0);
  recyclerView->setCornerRadius(ui_theme::controlRadius());
  left->addView(recyclerView);
  rootLayout->addView(left);

  auto right = new View();
  right->setFlexDirection(FlexDirection::Column);
  right->setAlignItems(YGAlignCenter);
  right->setWidth(300);
  right->setThemedBackgroundColor(ui_theme::mainMenuPanel);
  right->setCornerRadius(ui_theme::panelRadius());
  right->setThemedShadow(ui_theme::shadow, ui_theme::kPanelShadow);
  right->setThemedBorderColor(ui_theme::hairline);
  right->setBorderWidth(1);
  right->setGap(12);
  right->setPadding(Edge::Bottom, 16);

  auto *rightScroll = new ScrollView();
  rightScroll->setWidth(280);
  rightScroll->setFlex(1);
  rightScroll->setFlexShrink(1);
  rightScroll->clearBackgroundColor();
  auto *rightContent = new View();
  rightContent->setWidth(278);
  rightContent->setFlexDirection(FlexDirection::Column);
  rightContent->setAlignItems(YGAlignCenter);
  rightContent->setPadding(Edge::Top, 16);
  rightContent->setPadding(Edge::Bottom, 16);
  rightContent->setPadding(Edge::Left, 9);
  rightContent->setPadding(Edge::Right, 9);
  rightContent->setGap(12);

  auto *readySettings = new View();
  readySettings->setFlexDirection(FlexDirection::Column);
  readySettings->setAlignItems(YGAlignStretch);
  readySettings->setPadding(Edge::Top, 10);
  readySettings->setPadding(Edge::Bottom, 10);
  readySettings->setPadding(Edge::Left, 12);
  readySettings->setPadding(Edge::Right, 12);
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
  readyTotalRow = new View();
  readyTotalRow->setFlexDirection(FlexDirection::Row);
  readyTotalRow->setAlignItems(YGAlignCenter);
  readyTotalRow->setGap(8);
  readyTotalRow->setWidth(260);
  readyTotalRow->setHeight(28);
  readyTotalRow->setPadding(Edge::Left, 12);
  readyTotalRow->setPadding(Edge::Right, 12);
  readyTotalIconText =
      new TextView(ui_icons::kFontAwesomeSolidPath, 15);
  readyTotalIconText->setWidth(18);
  readyTotalIconText->setHeight(28);
  readyTotalIconText->setAlign(TextView::CENTER);
  readyTotalIconText->setVAlign(TextView::MIDDLE);
  readyTotalIconText->setOverflow(TextView::TextOverflow::Hidden);
  readyTotalIconText->setThemedColor(ui_theme::cyan);
  readyTotalText = makeReadyStatusText();
  readyTotalText->setFlex(1);
  readyTotalText->setThemedColor(ui_theme::cyan);
  readyTotalRow->addView(readyTotalIconText);
  readyTotalRow->addView(readyTotalText);
  readyAssistOptionText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  readyAssistOptionText->setHeight(28);
  readyAssistOptionText->setThemedColor(ui_theme::textPrimary);
  readyAssistOptionText->setOverflow(TextView::TextOverflow::Hidden);
  readyPacemakerText = makeReadyStatusText();
  readySettings->addView(readyGaugeRow);
  readySettings->addView(readyPlayOptionText);
  readySettings->addView(readyAssistOptionText);
  readySettings->addView(readyPacemakerText);

  readyPlayOptionsButton = new Button(0, 0, 260, 150);
  readyPlayOptionsButton->setWidth(260);
  readyPlayOptionsButton->setHeight(150);
  readyPlayOptionsButton->setFlexShrink(0);
  readyPlayOptionsButton->setCornerRadius(ui_theme::controlRadius());
  readyPlayOptionsButton->setStyledBorderWidth(1);
  readyPlayOptionsButton->setThemedBackgroundColors(
      ui_theme::control, ui_theme::controlHover, ui_theme::controlPressed);
  readyPlayOptionsButton->setThemedBorderColors(
      ui_theme::hairlineStrong, ui_theme::accentBorderStrong,
      ui_theme::accentBorderStrong);
  readyPlayOptionsButton->setContentView(readySettings);
  readyPlayOptionsButton->setOnClickListener(
      [this]() { showPlayOptionsModal(); });
  rightContent->addView(readyTotalRow);
  rightContent->addView(readyPlayOptionsButton);
  refreshPlaybackSelectionControls();

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
    const auto selectedRecord = selectedRecordSnapshot();
    if (activeFolder.type == LibraryFolderItem::Type::Course &&
        (!selectedRecord.has_value() || selectedRecord->courseStart)) {
      startSelectedCourse();
      return;
    }
    if (!selectedRecord.has_value() || selectedRecord->solidArchive ||
        selectedRecord->unavailable || selectedRecord->meta.BmsPath.empty()) {
      return;
    }
    startSelectedChart();
  });
  replayButtonSlot = new View();
  replayButtonSlot->setWidth(220)->setHeight(0);
  replayButtonSlot->setVisible(false);
  replayButtonSlot->setAlignItems(YGAlignStretch);

  replayButton = new Button(0, 0, 220, 58);
  replayButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  replayButtonText->setText("Records");
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
    const auto selectedMeta = selectedRecordSnapshot();
    if (!selectedMeta.has_value()) {
      return;
    }
    const bool courseStartReplay =
        selectedMeta->courseStart &&
        activeFolder.type == LibraryFolderItem::Type::Course &&
        activeFolder.courseId > 0;
    if (selectedMeta->solidArchive || selectedMeta->unavailable ||
        (!courseStartReplay && selectedMeta->meta.BmsPath.empty())) {
      return;
    }

    showReplayListModal(*selectedMeta);
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
  rightContent->addView(jacketCard);
  rightContent->addView(startButton);

  rankingsButton = new Button(0, 0, 220, 58);
  rankingsButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 26);
  rankingsButtonText->setText("Rankings");
  rankingsButtonText->setAlign(TextView::CENTER);
  rankingsButtonText->setVAlign(TextView::MIDDLE);
  rankingsButton->setContentView(rankingsButtonText);
  styleThemedActionButton(rankingsButton, rankingsButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed,
                          ui_theme::accentBorder);
  rankingsButton->setOnClickListener(
      [this]() { openRankingsForSelection(); });
  rankingsButton->setEnabled(false);
  rightContent->addView(rankingsButton);

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
  rightContent->addView(chartActionsRow);

  rightContent->addView(replayButtonSlot);
  rightContent->addView(unzipButtonSlot);
  rightContent->addView(findBmsButtonSlot);
  rightContent->addView(replayStatusText);

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
  rightScroll->setContentView(rightContent);
  right->addView(rightScroll);
  right->addView(settingsButton);
  rootLayout->addView(right);
  buildPlayOptionsModal();
  buildReplayModal();
  buildParseLogModal();
  buildTasksModal();
  buildFindBmsModal();
  buildUnzipProgressModal();
  rootLayout->addView(overlayPortal);
  reloadProfileSelectionsFromSettings();
  reloadScoreClearRanks();
  reloadFolderItems();
  reloadChartList();
  libraryRevision = context.chartRepository.GetLibraryRevision();
  rootLayout->applyYogaLayout();
}

void MainMenuScene::reloadFolderItems(bool preserveViewState) {
  if (folderRecyclerView == nullptr || !chartSession.has_value()) {
    return;
  }

  const float previousScrollOffset =
      preserveViewState ? folderRecyclerView->scrollOffset : 0.0f;
  const std::uint64_t currentLibraryRevision =
      context.chartRepository.GetLibraryRevision();
  if (!folderMetadataCache.valid ||
      folderMetadataCache.libraryRevision != currentLibraryRevision) {
    folderMetadataCache = LibraryFolderMetadataCache{};
    folderMetadataCache.libraryRevision = currentLibraryRevision;
    folderMetadataCache.allSongCount = chartSession->CountAllChartMeta();
    folderMetadataCache.favoriteCount = chartSession->CountFavoriteCharts();
    folderMetadataCache.solidArchiveCount = chartSession->CountSolidArchives();
    folderMetadataCache.tables = chartSession->SelectDifficultyTables();
    folderMetadataCache.courseTables =
        chartSession->SelectDifficultyCourseTables();
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
              .emplace(table.id,
                       chartSession->SelectDifficultyLevels(table.id))
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
    const LibraryFolderItem coursesRootItem{
        .key = "courses",
        .label = "Courses",
        .type = LibraryFolderItem::Type::CoursesRoot,
        .depth = 0,
        .count = -1,
        .expandable = true,
        .expanded = isExpanded("courses"),
    };
    folders.push_back(coursesRootItem);
    if (coursesRootItem.expanded) {
      const auto makeCourseItem =
          [](int courseId, const std::string &courseKey, int tableId,
             const std::string &groupName, const std::string &level,
             const std::string &name, const std::string &constraintJson,
             int depth) {
        const std::string courseLabel = level.empty() ? name : level;
        return LibraryFolderItem{
            .key = folderKeyForCourse(courseId),
            .label = courseLabel,
            .type = LibraryFolderItem::Type::Course,
            .depth = depth,
            .count = -1,
            .courseId = courseId,
            .courseKey = courseKey,
            .courseTableId = tableId,
            .courseGroupName = groupName,
            .courseConstraintJson = constraintJson,
        };
      };
      const auto makeCourseInfoItem = [&](const DifficultyCourseInfo &course,
                                          int depth) {
        return makeCourseItem(course.id, course.courseKey, course.tableId,
                              course.groupName, course.level, course.name,
                              course.constraintJson, depth);
      };

      for (const auto &table : courseTables) {
        const std::string tableKey = folderKeyForCourseTable(table.tableId);
        const LibraryFolderItem tableItem{
            .key = tableKey,
            .label = table.tableName,
            .type = LibraryFolderItem::Type::CourseTable,
            .depth = 1,
            .count = -1,
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
                              chartSession->SelectDifficultyCourseGroups(
                                  table.tableId))
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
                group.singletonCourseId, group.singletonCourseKey,
                group.tableId, group.groupName, group.singletonCourseLevel,
                group.singletonCourseName,
                group.singletonCourseConstraintJson, 2));
            continue;
          }

          const LibraryFolderItem groupItem{
              .key = groupKey,
              .label = label,
              .type = LibraryFolderItem::Type::CourseGroup,
              .depth = 2,
              .count = -1,
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
                                     chartSession->SelectDifficultyCourses(
                                         group.tableId, group.groupName))
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
  refreshChartFilterPanel();

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
  if (folderRecyclerView == nullptr || !chartSession.has_value()) {
    return;
  }

  std::vector<LibraryFolderItem> folders = folderRecyclerView->getItems();
  if (folders.empty()) {
    reloadFolderItems(true);
    return;
  }

  const int favoriteCount = chartSession->CountFavoriteCharts();
  if (folderMetadataCache.valid) {
    folderMetadataCache.favoriteCount = favoriteCount;
    folderMetadataCache.libraryRevision =
        context.chartRepository.GetLibraryRevision();
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
  query.selectedLongNoteMode =
      long_note_mode::valueFromId(profileSelections.longNoteMode);

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
  chart_record_filters::applyToQuery(query, chartRecordFilters,
                                     chartDifficultyRangeEnabled());
  return query;
}

bool MainMenuScene::chartDifficultyRangeEnabled() const {
  return main_menu_library::difficultyRangeEnabledForFolder(
      activeFolder.type == LibraryFolderItem::Type::DifficultyTable,
      activeFolder.clearMarkFolder, activeFolder.tableId,
      activeFolder.tableLevel);
}

std::vector<DifficultyLevelInfo>
MainMenuScene::chartFilterDifficultyLevels() const {
  if (!chartDifficultyRangeEnabled()) {
    return {};
  }
  const auto tableIt = folderMetadataCache.levelsByTable.find(
      activeFolder.tableId);
  if (tableIt == folderMetadataCache.levelsByTable.end()) {
    return {};
  }
  return tableIt->second;
}

void MainMenuScene::setChartFilterPanelVisible(bool visible) {
  chartFilterPanelVisible = visible;
  if (visible) {
    chartSortPanelVisible = false;
  } else {
    chartClearMarkDropdownOpen = false;
    chartScoreRankDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  refreshChartFilterPanel();
}

void MainMenuScene::setChartSortPanelVisible(bool visible) {
  chartSortPanelVisible = visible;
  if (visible) {
    chartFilterPanelVisible = false;
    chartClearMarkDropdownOpen = false;
    chartScoreRankDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  refreshChartFilterPanel();
}

void MainMenuScene::refreshChartFilterPanel() {
  const bool difficultyRangeEnabled = chartDifficultyRangeEnabled();
  const auto levels = chartFilterDifficultyLevels();
  const std::optional<int> folderClearMarkRank =
      activeFolder.clearMarkFolder
          ? std::optional<int>(activeFolder.clearMarkRank)
          : std::nullopt;
  if (activeFolder.clearMarkFolder) {
    chartRecordFilters.clearMarkRank.reset();
    chartRecordFilters.clearMarkOrAbove = false;
    chartRecordFilters.clearMarkOrBelow = false;
    chartClearMarkDropdownOpen = false;
  }
  chart_record_filters::normalizeSelection(chartRecordFilters,
                                           folderClearMarkRank);
  const std::optional<int> effectiveClearMarkRank =
      chartRecordFilters.clearMarkRank.has_value()
          ? chartRecordFilters.clearMarkRank
          : folderClearMarkRank;
  if (!chart_record_filters::scoreRankFilterEnabled(effectiveClearMarkRank)) {
    chartScoreRankDropdownOpen = false;
  }
  if (!chartFilterPanelVisible) {
    chartClearMarkDropdownOpen = false;
    chartScoreRankDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  if (difficultyRangeEnabled) {
    const bool difficultyTableChanged =
        chart_record_filters::resetDifficultyRangeOnTableChange(
            chartRecordFilters, chartDifficultyRangeTableId,
            activeFolder.tableId);
    if (difficultyTableChanged) {
      chartDifficultyMinDropdownOpen = false;
      chartDifficultyMaxDropdownOpen = false;
    }
    normalizeDifficultyFilterRange(chartRecordFilters, levels);
  } else {
    chartDifficultyRangeTableId.reset();
    chartRecordFilters.difficultyMinLevel.reset();
    chartRecordFilters.difficultyMaxLevel.reset();
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
    if (chartRecordFilters.sort.criterion == ChartRecordSortCriterion::Difficulty) {
      chartRecordFilters.sort = {};
    }
  }

  if (chartFilterPanel != nullptr) {
    chartFilterPanel->refresh({
        .filters = chartRecordFilters,
        .bpmMinText = chartBpmMinText,
        .bpmMaxText = chartBpmMaxText,
        .clearMarkFilterVisible = !activeFolder.clearMarkFolder,
        .effectiveClearMarkRank = effectiveClearMarkRank,
        .clearMarkDropdownOpen = chartClearMarkDropdownOpen,
        .scoreRankDropdownOpen = chartScoreRankDropdownOpen,
        .difficultyRangeEnabled = difficultyRangeEnabled,
        .difficultyMinDropdownOpen = chartDifficultyMinDropdownOpen,
        .difficultyMaxDropdownOpen = chartDifficultyMaxDropdownOpen,
        .difficultyLevels = levels,
    }, chartFilterPanelVisible);
  }
  if (chartSortPanel != nullptr) {
    chartSortPanel->refresh({
        .sort = chartRecordFilters.sort,
        .difficultySortEnabled = difficultyRangeEnabled,
    }, chartSortPanelVisible);
  }
  refreshChartFilterButtons();
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::refreshChartFilterButtons() {
  const bool filterActive =
      chartFilterPanelVisible || chartRecordFilters.clearMarkRank.has_value() ||
      chartRecordFilters.scoreRank.has_value() ||
      chartRecordFilters.bpmMin.has_value() ||
      chartRecordFilters.bpmMax.has_value() ||
      chartRecordFilters.difficultyMinLevel.has_value() ||
      chartRecordFilters.difficultyMaxLevel.has_value();
  styleThemedActionButton(
      chartFilterButton, chartFilterButtonText, true,
      filterActive ? ui_theme::primaryAction : ui_theme::control,
      filterActive ? ui_theme::primaryActionHover : ui_theme::controlHover,
      filterActive ? ui_theme::primaryActionPressed : ui_theme::controlPressed,
      filterActive ? ui_theme::accentBorderStrong : ui_theme::hairlineStrong);

  const bool sortActive =
      chartSortPanelVisible ||
      chartRecordFilters.sort.criterion != ChartRecordSortCriterion::Default;
  styleThemedActionButton(
      chartSortButton, chartSortButtonText, true,
      sortActive ? ui_theme::primaryAction : ui_theme::control,
      sortActive ? ui_theme::primaryActionHover : ui_theme::controlHover,
      sortActive ? ui_theme::primaryActionPressed : ui_theme::controlPressed,
      sortActive ? ui_theme::accentBorderStrong : ui_theme::hairlineStrong);
}

void MainMenuScene::setChartClearFilter(std::optional<int> rank) {
  if (activeFolder.clearMarkFolder) {
    rank.reset();
  }
  chartRecordFilters.clearMarkRank = rank;
  if (!rank.has_value()) {
    chartRecordFilters.clearMarkOrAbove = false;
    chartRecordFilters.clearMarkOrBelow = false;
  }
  chart_record_filters::normalizeSelection(chartRecordFilters);
  chartClearMarkDropdownOpen = false;
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartScoreRankFilter(std::optional<std::string> rank) {
  const std::optional<int> effectiveClearMarkRank =
      chartRecordFilters.clearMarkRank.has_value()
          ? chartRecordFilters.clearMarkRank
          : (activeFolder.clearMarkFolder
                 ? std::optional<int>(activeFolder.clearMarkRank)
                 : std::nullopt);
  if (!chart_record_filters::scoreRankFilterEnabled(effectiveClearMarkRank)) {
    rank.reset();
  }
  chartRecordFilters.scoreRank = rank;
  if (!rank.has_value()) {
    chartRecordFilters.scoreRankOrAbove = false;
    chartRecordFilters.scoreRankOrBelow = false;
  }
  chartScoreRankDropdownOpen = false;
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartBpmMinFilter(const std::string &text) {
  chartBpmMinText = text;
  chartRecordFilters.bpmMin = parseOptionalBpmFilter(text);
  chart_record_filters::normalizeBpmRange(chartRecordFilters, chartBpmMinText,
                                          chartBpmMaxText);
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartBpmMaxFilter(const std::string &text) {
  chartBpmMaxText = text;
  chartRecordFilters.bpmMax = parseOptionalBpmFilter(text);
  chart_record_filters::normalizeBpmRange(chartRecordFilters, chartBpmMinText,
                                          chartBpmMaxText);
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartDifficultyMinFilter(
    std::optional<std::string> level) {
  chart_record_filters::setDifficultyMinLevel(chartRecordFilters,
                                              chartFilterDifficultyLevels(),
                                              std::move(level));
  chartDifficultyMinDropdownOpen = false;
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartDifficultyMaxFilter(
    std::optional<std::string> level) {
  chart_record_filters::setDifficultyMaxLevel(chartRecordFilters,
                                              chartFilterDifficultyLevels(),
                                              std::move(level));
  chartDifficultyMaxDropdownOpen = false;
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartClearMarkDropdownOpen(bool open) {
  if (activeFolder.clearMarkFolder) {
    open = false;
  }
  chartClearMarkDropdownOpen = open;
  if (open) {
    chartScoreRankDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  refreshChartFilterPanel();
}

void MainMenuScene::setChartScoreRankDropdownOpen(bool open) {
  const std::optional<int> effectiveClearMarkRank =
      chartRecordFilters.clearMarkRank.has_value()
          ? chartRecordFilters.clearMarkRank
          : (activeFolder.clearMarkFolder
                 ? std::optional<int>(activeFolder.clearMarkRank)
                 : std::nullopt);
  if (!chart_record_filters::scoreRankFilterEnabled(effectiveClearMarkRank)) {
    open = false;
  }
  chartScoreRankDropdownOpen = open;
  if (open) {
    chartClearMarkDropdownOpen = false;
    chartDifficultyMinDropdownOpen = false;
    chartDifficultyMaxDropdownOpen = false;
  }
  refreshChartFilterPanel();
}

void MainMenuScene::setChartClearMarkRange(bool orAbove, bool orBelow) {
  if (!chartRecordFilters.clearMarkRank.has_value()) {
    chartRecordFilters.clearMarkOrAbove = false;
    chartRecordFilters.clearMarkOrBelow = false;
  } else {
    chartRecordFilters.clearMarkOrAbove = orAbove;
    chartRecordFilters.clearMarkOrBelow = !orAbove && orBelow;
  }
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartScoreRankRange(bool orAbove, bool orBelow) {
  if (!chartRecordFilters.scoreRank.has_value()) {
    chartRecordFilters.scoreRankOrAbove = false;
    chartRecordFilters.scoreRankOrBelow = false;
  } else {
    chartRecordFilters.scoreRankOrAbove = orAbove;
    chartRecordFilters.scoreRankOrBelow = !orAbove && orBelow;
  }
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::setChartDifficultyDropdownOpen(bool minLevel, bool open) {
  if (open) {
    chartClearMarkDropdownOpen = false;
    chartScoreRankDropdownOpen = false;
  }
  if (minLevel) {
    chartDifficultyMinDropdownOpen = open;
    if (open) {
      chartDifficultyMaxDropdownOpen = false;
    }
  } else {
    chartDifficultyMaxDropdownOpen = open;
    if (open) {
      chartDifficultyMinDropdownOpen = false;
    }
  }
  refreshChartFilterPanel();
}

void MainMenuScene::setChartSortCriterion(
    ChartRecordSortCriterion criterion) {
  if (criterion == ChartRecordSortCriterion::Difficulty &&
      !chartDifficultyRangeEnabled()) {
    return;
  }
  chartRecordFilters.sort =
      chart_record_filters::nextSortState(chartRecordFilters.sort, criterion);
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::reloadChartList(bool preserveViewState) {
  if (recyclerView == nullptr || !chartSession.has_value()) {
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

  const int databaseCount = chartSession->CountChartMeta(query);
  const int count = databaseCount + (leadingRecord.has_value() ? 1 : 0);
  const int leadingOffset = leadingRecord.has_value() ? 1 : 0;
  chartListCache.reset(*chartSession, query, databaseCount,
                       std::move(leadingRecord));
  recyclerView->setItemProvider(
      count, [this](int index) -> const ChartMetaRecord & {
        return chartListCache.get(index);
      });
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshSelectedChartActionState();
  if (!selectedChartRecord.has_value() && !preserveViewState &&
      activeFolder.type == LibraryFolderItem::Type::Course && count > 0) {
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
    const int databaseIndex = chartSession->FindChartMetaIndex(
        query, std::filesystem::path(path));
    if (databaseIndex < 0) {
      return -1;
    }
    const int index = databaseIndex + leadingOffset;
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
  refreshSelectedChartActionState();
  recyclerView->rebindVisibleItems();
}

std::optional<std::string> MainMenuScene::reloadScoreClearRanks() {
  if (!chartSession.has_value()) {
    return "chart database is unavailable";
  }

  profile_database_activity::WriteGuard profileDatabaseOperation;
  const auto definitions = chartSession->SelectDifficultyCourseDefinitions();
  const CourseScoreRecoveryResult scoreRecovery =
      context.scoreRepository.RecoverCourseRecords(definitions);
  if (!scoreRecovery.ok()) {
    SDL_Log("SQL error while recovering course scores: %s",
            scoreRecovery.errorMessage.c_str());
  }

  std::string replayRecoveryError;
  if (!context.replayRepository.RecoverCourseRecords(
          definitions, scoreRecovery.evidence, replayRecoveryError)) {
    SDL_Log("SQL error while recovering course replays: %s",
            replayRecoveryError.c_str());
  }

  auto prepared =
      context.scoreRepository.PrepareScoreQueryDatabase(*chartSession);
  if (const auto &error = prepared.error()) {
    SDL_Log("SQL error while preparing score query database: %s",
            error->c_str());
    return error;
  }
  scoreClearRanks = context.scoreRepository.LoadBestClearRanks(
      *chartSession, score_cache_queries::kScoreDatabaseSchema);
  scoreBestScores = context.scoreRepository.LoadBestScores(
      *chartSession, score_cache_queries::kScoreDatabaseSchema);
  const ScoreClearRankCache localClearRanks =
      context.scoreRepository.LoadLocalBestClearRanks(
          *chartSession, score_cache_queries::kScoreDatabaseSchema);
  folderClearData = chartSession->LoadFolderClearDataByLongNoteMode(
      scoreClearRanks, localClearRanks);
  scoreClearRanksRevision = context.scoreRepository.GetRevision();
  return std::nullopt;
}

std::optional<std::string> MainMenuScene::prepareScoreQueryDatabase() {
  if (!chartSession.has_value()) {
    return "chart database is unavailable";
  }

  auto prepared =
      context.scoreRepository.PrepareScoreQueryDatabase(*chartSession);
  if (const auto &error = prepared.error()) {
    SDL_Log("SQL error while preparing score query database: %s",
            error->c_str());
    return error;
  }
  return std::nullopt;
}

std::optional<std::string> MainMenuScene::refreshScoreClearRankViews() {
  if (const auto error = reloadScoreClearRanks()) {
    return error;
  }
  refreshLongNoteModeClearRankViews();
  return std::nullopt;
}

void MainMenuScene::refreshLongNoteModeClearRankViews() {
  if (folderRecyclerView != nullptr) {
    reloadFolderItems(true);
  }
  if (recyclerView != nullptr) {
    const bool chartListDependsOnScores =
        activeFolder.type == LibraryFolderItem::Type::DifficultyClearMark ||
        chartRecordFilters.clearMarkRank.has_value() ||
        chartRecordFilters.scoreRank.has_value() ||
        chartRecordFilters.sort.criterion == ChartRecordSortCriterion::ClearMark ||
        chartRecordFilters.sort.criterion == ChartRecordSortCriterion::Score;
    if (chartListDependsOnScores) {
      reloadChartList(true);
    } else {
      recyclerView->rebindVisibleItems();
    }
  }
}

void MainMenuScene::refreshScoreClearRanksIfNeeded() {
  const std::uint64_t revision = context.scoreRepository.GetRevision();
  if (revision == scoreClearRanksRevision) {
    return;
  }

  if (refreshScoreClearRankViews().has_value()) {
    const bool hadVisibleScoreState = scoreClearRanksRevision != 0;
    scoreClearRanks = {};
    scoreBestScores = {};
    folderClearData = {};
    scoreClearRanksRevision = 0;
    if (hadVisibleScoreState) {
      refreshLongNoteModeClearRankViews();
    }
  }
}

void MainMenuScene::refreshIrRecordListIfNeeded() {
  bool recordsNeedRefresh = false;
  const std::uint64_t accountEvidenceRevision =
      context.irAccountEvidenceRevision.load(std::memory_order_acquire);
  if (accountEvidenceRevision != observedIrAccountEvidenceRevision) {
    observedIrAccountEvidenceRevision = accountEvidenceRevision;
    recordsNeedRefresh = true;
  }

  if (context.irSubmissionService != nullptr) {
    const auto status = context.irSubmissionService->reconciliationStatus(
        ir::kTachiProviderId);
    if (status.phase == ir::IrReconciliationPhase::Succeeded &&
        status.revision != 0 &&
        status.revision != observedIrReconciliationRevision) {
      observedIrReconciliationRevision = status.revision;
      recordsNeedRefresh = true;
    }
  }
  if (!recordsNeedRefresh) {
    return;
  }
  if (replayModalRoot != nullptr && replayModalRoot->getVisible()) {
    reloadReplayRecordModels(true);
  }
}

void MainMenuScene::refreshLibraryIfNeeded() {
  const std::uint64_t revision =
      context.chartRepository.GetLibraryRevision();
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
      record.meta, long_note_mode::valueFromId(profileSelections.longNoteMode));
}

int MainMenuScene::clearRankForFolder(const std::string &key) const {
  const int mode = long_note_mode::valueFromId(profileSelections.longNoteMode);
  const auto &clearRanks = folderClearData.clearRanks[static_cast<size_t>(mode)];
  const auto it = clearRanks.find(key);
  return it == clearRanks.end() ? kNoClearTypeRank : it->second;
}

int MainMenuScene::clearMarkCountForFolder(const std::string &key,
                                           int clearMarkRank) const {
  const int mode = long_note_mode::valueFromId(profileSelections.longNoteMode);
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
    libraryRevision = context.chartRepository.GetLibraryRevision();
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
  if (recyclerView == nullptr || path.empty() || !chartSession.has_value()) {
    return;
  }
  const path_t target = fspath_to_path_t(path);
  const ChartMetaQuery query = chartQueryForActiveFolder();
  int index = chartSession->FindChartMetaIndex(query, path);
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

void MainMenuScene::selectFolder(LibraryFolderItem item) {
  auto toggleExpandedFolder = [this](const std::string &key) {
    const auto it = expandedLibraryFolders.find(key);
    if (it == expandedLibraryFolders.end()) {
      expandedLibraryFolders.insert(key);
    } else {
      expandedLibraryFolders.erase(it);
    }
  };

  if (item.type == LibraryFolderItem::Type::CoursesRoot) {
    const std::string previousActiveKey = activeFolder.key;
    toggleExpandedFolder(item.key);
    reloadFolderItems(true);
    if (activeFolder.key != previousActiveKey) {
      reloadChartList();
    }
    return;
  }

  const bool chartQueryUnchanged = activeFolder.key == item.key;
  activeFolder = item;
  refreshChartFilterPanel();
  if (item.expandable) {
    toggleExpandedFolder(item.key);
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

  if (!chartSession.has_value() ||
      !chartSession->SetFavorite(record.meta, favorite)) {
    return false;
  }

  refreshFavoriteFolderCount();
  if (activeFolder.type == LibraryFolderItem::Type::Favorites && !favorite) {
    reloadChartList(true);
  } else if (recyclerView != nullptr) {
    reloadChartList(true);
  }
  libraryRevision = context.chartRepository.GetLibraryRevision();
  return true;
}

std::optional<ChartMetaRecord> MainMenuScene::selectedRecordSnapshot() const {
  if (selectedChartRecord.has_value()) {
    return selectedChartRecord;
  }
  if (recyclerView == nullptr || recyclerView->selectedIndex < 0 ||
      recyclerView->selectedIndex >= recyclerView->size()) {
    return std::nullopt;
  }
  return recyclerView->get(recyclerView->selectedIndex);
}

void MainMenuScene::refreshSelectedChartActionState() {
  refreshRankingsButton();
  const auto record = selectedRecordSnapshot();
  if (!record.has_value()) {
    refreshReplayAvailability(nullptr);
    setPlayableChartActionsVisible(false);
    setUnzipButtonVisible(false);
    setFindBmsButtonVisible(false);
    refreshStartButtonForActiveFolder();
    return;
  }

  refreshReplayAvailability(&*record);
  if (record->courseStart) {
    const bool currentCourseStart =
        activeFolder.type == LibraryFolderItem::Type::Course &&
        activeFolder.courseId > 0;
    setPlayableChartActionsVisible(currentCourseStart, false);
    refreshUnzipButtonForSelection(nullptr);
    setFindBmsButtonVisible(false);
    refreshStartButtonForActiveFolder();
    return;
  }

  setPlayableChartActionsVisible(!record->unavailable &&
                                 !record->solidArchive &&
                                 !record->meta.BmsPath.empty());
  refreshUnzipButtonForSelection(&*record);
  setFindBmsButtonVisible(
      record->unavailable && !record->solidArchive &&
      (!record->meta.SHA256.empty() || !record->meta.MD5.empty() ||
       !record->meta.Title.empty()));
  refreshStartButtonForActiveFolder();
}

void MainMenuScene::refreshRankingsButton() {
  if (rankingsButton == nullptr) {
    return;
  }
  const auto record = selectedRecordSnapshot();
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  const bool enabled =
      context.irRankingService != nullptr && record.has_value() &&
      !record->courseStart && driver != nullptr &&
      driver->capabilities().chartRankings &&
      settings != context.settings.irProviders.end() &&
      settings->second.enabled &&
      ir::makeBokutachiRankingQuery(record->meta).value.has_value();
  rankingsButton->setEnabled(enabled);
}

void MainMenuScene::openRankingsForSelection() {
  if (rankingsButton == nullptr || !rankingsButton->isEnabled() ||
      context.irRankingService == nullptr || overlayPortal == nullptr) {
    return;
  }
  const auto record = selectedRecordSnapshot();
  if (!record || record->courseStart) {
    return;
  }
  const auto query = ir::makeBokutachiRankingQuery(record->meta);
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (!query.value || settings == context.settings.irProviders.end() ||
      !settings->second.enabled) {
    refreshRankingsButton();
    return;
  }

  std::optional<ir::IrLocalComparison> comparison;
  const int selectedLongNoteMode =
      long_note_mode::valueFromId(profileSelections.longNoteMode);
  const auto best = context.scoreRepository.LoadBestScoreForRuleset(
      record->meta, RulesetDescriptor::For(GameplayRuleset::LR2),
      selectedLongNoteMode);
  if (best) {
    comparison = ir::IrLocalComparison{
        .label = "Local PB",
        .score = best->score,
        .maxScore = best->maxScore > 0
                        ? best->maxScore
                        : std::max(0, record->meta.TotalNotes) * 2,
        .clearType = best->clearType,
        .badPoints = best->badPoints,
        .maxCombo = best->maxCombo,
    };
  }
  if (!rankingsModal) {
    rankingsModal = std::make_unique<ir::IrRankingModal>(
        *overlayPortal, *context.irRankingService);
  }
  rankingsModal->open(
      {.profileId = context.profileManager.activeProfile().id,
       .providerId = std::string(ir::kTachiProviderId),
       .serverOrigin = settings->second.serverOrigin,
       .chart = *query.value,
       .localComparison = std::move(comparison)},
      record->meta.Title.empty() ? "Selected chart" : record->meta.Title);
}

MainMenuScene::EffectivePlayOptionSelection
MainMenuScene::currentEffectivePlayOptionSelection() const {
  EffectivePlayOptionSelection selection;
  selection.playOption =
      play_options::normalizePlayOption(profileSelections.playOption);
  selection.longNoteMode = long_note_mode::parseId(
      profileSelections.longNoteMode, AppSettings::kDefaultLnMode);
  selection.assistOption =
      assist_options::normalize(profileSelections.assistOption);

  const auto record = selectedRecordSnapshot();
  const bool selectedCourseStart =
      record.has_value() && record->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0;
  if (selectedCourseStart) {
    const CourseConstraintSettings constraintSettings =
        courseConstraintSettingsFromJson(activeFolder.courseConstraintJson);
    if (coursePlayOptionLocksSelection(constraintSettings)) {
      selection.playOption = coursePlayOptionForConstraints(
          profileSelections.playOption, constraintSettings);
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

void MainMenuScene::setGameplayRulesetSelection(GameplayRuleset ruleset) {
  const main_menu_profile::Selections previousSelections = profileSelections;
  const AppSettings previousSettings = context.settings;
  profileSelections.ruleset = ruleset;
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  std::string errorMessage;
  if (!context.saveSettings(&errorMessage)) {
    profileSelections = previousSelections;
    context.settings = previousSettings;
    SDL_Log("Failed to save gameplay ruleset selection: %s",
            errorMessage.empty() ? "unknown error" : errorMessage.c_str());
  }
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setGaugeSelection(GaugeType gaugeType,
                                      GaugeAutoShiftMode autoShift) {
  profileSelections.gaugeType = gaugeType;
  profileSelections.gaugeAutoShift = autoShift;
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save gauge selection");
  }
  refreshGaugeSelectionButtons();
}

void MainMenuScene::setGaugeAutoShiftLowerBound(GaugeType gaugeType) {
  profileSelections.gaugeAutoShiftLowerBound = gaugeType;
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save gauge auto shift lower bound");
  }
  refreshGaugeSelectionButtons();
}

void MainMenuScene::refreshGaugeSelectionButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setPlayOptionSelection(const std::string &option) {
  if (!currentPlayOptionSelectionAllowed(option)) {
    return;
  }
  profileSelections.playOption = play_options::normalizePlayOption(option);
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save play option selection");
  }
  refreshPlayOptionButtons();
}

void MainMenuScene::refreshPlayOptionButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setLongNoteModeSelection(const std::string &mode) {
  if (!currentLongNoteModeSelectionAllowed(mode)) {
    return;
  }
  const std::string previousMode = profileSelections.longNoteMode;
  profileSelections.longNoteMode =
      long_note_mode::parseId(mode, AppSettings::kDefaultLnMode);
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save long note mode selection");
  }
  refreshLongNoteModeButtons();
  if (profileSelections.longNoteMode != previousMode) {
    refreshLongNoteModeClearRankViews();
  }
}

void MainMenuScene::refreshLongNoteModeButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setAssistOptionSelection(const std::string &option) {
  if (!currentAssistOptionSelectionAllowed(option)) {
    return;
  }
  profileSelections.assistOption = assist_options::normalize(option);
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save assist option selection");
  }
  refreshAssistOptionButtons();
}

void MainMenuScene::refreshAssistOptionButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setPacemakerTargetSelection(const std::string &target) {
  profileSelections.pacemakerTarget = pacemaker::normalizeTargetId(target);
  profileSelections.applyTo(context.settings);
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save pacemaker target selection");
  }
  refreshPacemakerTargetButtons();
}

void MainMenuScene::refreshPacemakerTargetButtons() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::setPlaybackRateSelection(int percent) {
  if (playbackSelectionLockedForCourse()) {
    return;
  }
  context.settings.selectedPlaybackRatePercent = percent;
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save playback rate selection");
  }
  refreshPlaybackSelectionControls();
}

void MainMenuScene::setPlaybackModeSelection(const std::string &mode) {
  if (playbackSelectionLockedForCourse() || mode != "pitch-shift") {
    return;
  }
  context.settings.selectedPlaybackMode = audio::PlaybackMode::PitchShift;
  context.settings.sanitize();
  if (!context.saveSettings()) {
    SDL_Log("Failed to save playback mode selection");
  }
  refreshPlaybackSelectionControls();
}

void MainMenuScene::toggleGameplayClubMode() {
  context.settings.gameplayClubModeEnabled =
      !context.settings.gameplayClubModeEnabled;
  if (!context.saveSettings()) {
    SDL_Log("Failed to save gameplay Club mode selection");
  }
  refreshPlaybackSelectionControls();
}

void MainMenuScene::refreshPlaybackSelectionControls() {
  refreshPlayOptionsPanel();
  refreshReadySettingsSummary();
}

void MainMenuScene::refreshPlayOptionsPanel() {
  if (playOptionsPanel == nullptr) {
    return;
  }
  const EffectivePlayOptionSelection effective =
      currentEffectivePlayOptionSelection();
  const bool playbackLocked = playbackSelectionLockedForCourse();
  const int playbackRate =
      playbackLocked ? course_rules::kRequiredPlaybackRate.percent
                     : context.settings.selectedPlaybackRatePercent;
  playOptionsPanel->refresh(
      {.ruleset = profileSelections.ruleset,
       .gaugeType = profileSelections.gaugeType,
       .gaugeAutoShift = profileSelections.gaugeAutoShift,
       .gaugeAutoShiftLowerBound =
           profileSelections.gaugeAutoShiftLowerBound,
       .playOption = effective.playOption,
       .defaultLaneOrder = {},
       .laneOrderEnabled = false,
       .longNoteMode = effective.longNoteMode,
       .longNoteModeLocked = effective.longNoteModeLocked,
       .assistOption = effective.assistOption,
       .assistOptionLocked = effective.assistOptionLocked,
       .playbackRatePercent = playbackRate,
       .playbackLocked = playbackLocked,
       .clubMode = context.settings.gameplayClubModeEnabled,
       .pacemakerTarget = profileSelections.pacemakerTarget});
}

bool MainMenuScene::playbackSelectionLockedForCourse() const {
  const auto record = selectedRecordSnapshot();
  return record.has_value() && record->courseStart &&
         activeFolder.type == LibraryFolderItem::Type::Course &&
         activeFolder.courseId > 0;
}

void MainMenuScene::refreshReadySettingsSummary() {
  const EffectivePlayOptionSelection effective =
      currentEffectivePlayOptionSelection();
  const auto record = selectedRecordSnapshot();
  const bool showTotal = record.has_value() && !record->courseStart &&
                         !record->solidArchive && !record->unavailable;
  if (readyGaugeText != nullptr) {
    readyGaugeText->setText(gaugeButtonLabel(profileSelections.gaugeType,
                                             profileSelections.gaugeAutoShift));
    readyGaugeText->setColor(readyGaugeTextColor(
        profileSelections.gaugeType, profileSelections.gaugeAutoShift));
  }
  if (readyPlayOptionText != nullptr) {
    readyPlayOptionText->setText(
        std::string(gameplayRulesetLabel(profileSelections.ruleset)) + " · " +
        effective.playOption + " · " + effective.longNoteMode);
  }
  if (readyTotalText != nullptr) {
    if (showTotal) {
      const bool chartAuthored =
          record->meta.HasTotal && record->meta.Total > 0.0;
      if (readyTotalIconText != nullptr) {
        readyTotalIconText->setText(ui_icons::textForCodepoint(
            chartAuthored ? kIconFileLines : kIconCalculator));
      }
      readyTotalText->setText(
          "TOTAL: " +
          formatGaugeTotal(record->meta, profileSelections.ruleset));
      readyTotalRow->setDisplay(YGDisplayFlex);
      readyTotalRow->setVisible(true);
    } else {
      readyTotalText->setText("");
      if (readyTotalIconText != nullptr) {
        readyTotalIconText->setText("");
      }
      readyTotalRow->setDisplay(YGDisplayNone);
      readyTotalRow->setVisible(false);
    }
  }
  if (readyAssistOptionText != nullptr) {
    const int percent =
        playbackSelectionLockedForCourse()
            ? course_rules::kRequiredPlaybackRate.percent
            : context.settings.selectedPlaybackRatePercent;
    const bool optionEnabled =
        assist_options::isEnabled(effective.assistOption);
    if (!optionEnabled && percent == 100) {
      readyAssistOptionText->setText("Assist off");
    } else {
      std::string reasons;
      if (optionEnabled) {
        reasons = effective.assistOption;
      }
      if (percent != 100) {
        if (!reasons.empty()) {
          reasons += "/";
        }
        reasons += std::to_string(percent) + "%";
      }
      readyAssistOptionText->setText("Assist · " + reasons);
    }
  }
  if (readyPacemakerText != nullptr) {
    readyPacemakerText->setText(
        "Target · " +
        pacemaker::displayTargetLabel(profileSelections.pacemakerTarget));
  }
}

const MainMenuScene::CourseValidationCache &
MainMenuScene::courseValidationForActiveFolder() {
  const std::uint64_t currentLibraryRevision =
      context.chartRepository.GetLibraryRevision();
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
      activeFolder.courseId <= 0 || !chartSession.has_value()) {
    return courseValidationCache;
  }

  ChartMetaQuery query;
  query.courseId = activeFolder.courseId;
  chartSession->QueryChartMeta(query, courseValidationCache.records);
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
  const auto selectedRecord = selectedRecordSnapshot();
  if (selectedRecord.has_value() && !selectedRecord->courseStart) {
    startButtonText->setText("Start");
    return;
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
      const int databaseIndex = chartSession.has_value()
                                    ? chartSession->FindChartMetaIndex(
                                          chartQueryForActiveFolder(),
                                          missingRecord.meta.BmsPath)
                                    : -1;
      if (databaseIndex >= 0) {
        visibleMissingIndex = databaseIndex + 1;
      }
    } else if (searchText.empty()) {
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
  session->courseKey = activeFolder.courseKey;
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
  int courseLongNoteMode =
      long_note_mode::valueFromId(profileSelections.longNoteMode);
  if (constraintSettings.rules.longNoteMode !=
      CourseLongNoteMode::Unspecified) {
    courseLongNoteMode = courseLongNoteModeToChartMetaValue(
        constraintSettings.rules.longNoteMode);
  }
  session->currentIndex = 0;
  session->ruleset = profileSelections.ruleset;
  session->rulesetDescriptor = RulesetDescriptor::For(session->ruleset);
  session->gaugeType = profileSelections.gaugeType;
  session->gaugeProfile = constraintSettings.gaugeProfile;
  session->gaugeAutoShift = profileSelections.gaugeAutoShift;
  session->gaugeAutoShiftLowerBound =
      profileSelections.gaugeAutoShiftLowerBound;
  session->longNoteMode = courseLongNoteMode;
  session->constraints = constraintSettings.rules;
  session->requestedPlayOption = coursePlayOptionForConstraints(
      profileSelections.playOption, constraintSettings);
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
  cancelActivePreviewLoading();
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  const int selectedLongNoteMode =
      normalizeChartLongNoteModeValue(session->longNoteMode) > 0
          ? normalizeChartLongNoteModeValue(session->longNoteMode)
          : long_note_mode::valueFromId(profileSelections.longNoteMode);
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
        joinRetiredPreviewLoadThreads();
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
        options.gaugeAutoShiftLowerBound =
            session->gaugeAutoShiftLowerBound;
        options.playOption = playInfo.option;
        options.playOptionSeed = playInfo.seed;
        options.playOption2 = playInfo.option2;
        options.playOption2Seed = playInfo.seed2;
        options.longNoteMode = selectedLongNoteMode;
        options.assistOption = session->assistOption;
        options.playback = course_rules::kRequiredPlaybackRate;
        options.clubMode = context.settings.gameplayClubModeEnabled;
        options.courseSession = session;
        options.courseConstraints = session->constraints;
        options.ruleset = session->ruleset;
        options.requiredRulesetDescriptor = session->rulesetDescriptor;
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

  const auto record = selectedRecordSnapshot();
  if (!record.has_value()) {
    return;
  }
  if (record->solidArchive || record->unavailable ||
      record->meta.BmsPath.empty()) {
    return;
  }
  startChartDirect(*record);
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

  const GaugeType gaugeType = profileSelections.gaugeType;
  const GaugeAutoShiftMode gaugeAutoShift = profileSelections.gaugeAutoShift;
  const GaugeType gaugeAutoShiftLowerBound =
      profileSelections.gaugeAutoShiftLowerBound;
  const GameplayRuleset ruleset = profileSelections.ruleset;
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;
  const std::string playOption = profileSelections.playOption;
  int selectedLongNoteMode = normalizeChartLongNoteModeValue(record.meta.LnMode);
  if (selectedLongNoteMode == 0) {
    selectedLongNoteMode =
        long_note_mode::valueFromId(profileSelections.longNoteMode);
  }
  const std::string assistOption = profileSelections.assistOption;
  const std::string pacemakerTarget =
      pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode,
  };
  const std::string normalizedPlayOption =
      play_options::normalizePlayOption(playOption);
  const bool canReusePreviewForStart =
      normalizedPlayOption.empty() || normalizedPlayOption == "NORMAL";
  const SelectedChartRandomInfo chartRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  defer(
      [this, record, gaugeType, gaugeAutoShift, gaugeAutoShiftLowerBound,
       ruleset, autoKeySound, playOption, selectedLongNoteMode, assistOption,
       pacemakerTarget, playback,
       canReusePreviewForStart, chartRandomInfo]() {
        auto finishStart = [this]() {
          resetStartLoadingUi();
          return true;
        };
        if (!canReusePreviewForStart) {
          cancelActivePreviewLoading();
        }
        if (loadThread.joinable()) {
          loadThread.join();
        }
        joinRetiredPreviewLoadThreads();

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
                                    .gaugeAutoShiftLowerBound =
                                        gaugeAutoShiftLowerBound,
                                    .longNoteMode = selectedLongNoteMode,
                                    .assistOption = assistOption,
                                    .pacemakerTarget = pacemakerTarget,
                                    .playback = playback,
                                    .ruleset = ruleset,
                                });
          return finishStart();
        }

        cancelActivePreviewLoading();
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
                                      .gaugeAutoShiftLowerBound =
                                          gaugeAutoShiftLowerBound,
                                      .playOption = playInfo.option,
                                      .playOptionSeed = playInfo.seed,
                                      .playOption2 = playInfo.option2,
                                      .playOption2Seed = playInfo.seed2,
                                      .longNoteMode = selectedLongNoteMode,
                                      .assistOption = assistOption,
                                      .pacemakerTarget = pacemakerTarget,
                                      .playback = playback,
                                      .ruleset = ruleset,
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
                                         .gaugeAutoShiftLowerBound =
                                             gaugeAutoShiftLowerBound,
                                         .longNoteMode = selectedLongNoteMode,
                                         .assistOption = assistOption,
                                         .pacemakerTarget = pacemakerTarget,
                                         .playback = playback,
                                         .ruleset = ruleset,
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

  const auto record = selectedRecordSnapshot();
  if (!record.has_value()) {
    return;
  }

  if (record->solidArchive || record->unavailable ||
      record->meta.BmsPath.empty()) {
    return;
  }
  openChartViewerDirect(*record);
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

  const auto record = selectedRecordSnapshot();
  if (!record.has_value()) {
    return;
  }

  if (record->unavailable || record->meta.BmsPath.empty()) {
    return;
  }

  std::string errorMessage;
  if (!revealPathInFileManager(record->meta.BmsPath, errorMessage)) {
    SDL_Log("Failed to reveal chart file %s: %s",
            fspath_to_utf8(record->meta.BmsPath).c_str(),
            errorMessage.c_str());
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
  if (record != nullptr && record->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      (!activeFolder.courseKey.empty() || activeFolder.courseId > 0)) {
    const auto courseReplays = context.replayRepository.ListCourseReplays(
        {.courseKey = activeFolder.courseKey,
         .legacyCourseId = activeFolder.courseId});
    setReplayButtonVisible(!courseReplays.empty());
    return;
  }

  if (record == nullptr || record->solidArchive || record->unavailable ||
      record->meta.BmsPath.empty()) {
    setReplayButtonVisible(false);
    return;
  }

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
  cancelPreviewLoading(true);
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
      auto unzipSession = context.chartRepository.OpenSession();
      if (!unzipSession.has_value()) {
        result.success = false;
        result.message = "Unzipped archive. Failed to refresh library.";
      } else {
        unzipSession->EnsureSchema();
        std::vector<std::filesystem::path> roots{scanRoot};
        postProgress(archive_file::UnzipProgress{
            .fraction = 0.98, .message = "Refreshing library"});
        ChartLibraryScanner scanner;
        const int changedCount = scanner.Scan(*unzipSession, roots,
                                              &stopToken);
        if (!stopToken.stop_requested()) {
          std::vector<bms_parser::ChartMeta> chartMetas;
          unzipSession->SelectAllChartMeta(chartMetas);
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

  auto deleteSession = context.chartRepository.OpenSession();
  if (deleteSession.has_value()) {
    deleteSession->DeleteArchiveRecords(archivePath);
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

void MainMenuScene::startLibraryRebuild() {
  if (willStart.load() || replayExportInProgress.load()) {
    return;
  }
  enqueueLibraryRefreshTask("Rebuild Library", std::filesystem::path(), "",
                            true);
  tasksModalOpenRequested.store(true);
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
  if (!chartSession.has_value()) {
    return ChartRepository::DefaultBmsFolderPath();
  }
  auto entries = chartSession->SelectEffectiveEntries();
  if (entries.empty()) {
    const auto path = ChartRepository::DefaultBmsFolderPath();
    const bool pathReady =
        ensureDirectoryExistsLogged(path, "BMS download root");
#if !(TARGET_OS_ANDROID)
    if (pathReady) {
      chartSession->InsertEntry(path);
    }
#endif
    return path;
  }

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  return ResolveIOSFolderEntryPath(entries.front());
#elif TARGET_OS_ANDROID
  const auto path = ChartRepository::DefaultBmsFolderPath();
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

  parseLogRecyclerView = new RecyclerView<MainMenuParseLogRow>(
      [](const MainMenuParseLogRow &a, const MainMenuParseLogRow &b) {
        return a.id == b.id;
      });
  parseLogRecyclerView->itemHeight = kParseLogRowHeight;
  parseLogRecyclerView->reserveScrollbarGutter = true;
  parseLogRecyclerView->setWidth(kModalContentWidth);
  parseLogRecyclerView->setFlex(1);
  parseLogRecyclerView->setThemedBackgroundColor(ui_theme::insetSurface);
  parseLogRecyclerView->setCornerRadius(ui_theme::controlRadius());
  parseLogRecyclerView->setThemedBorderColor(ui_theme::hairline);
  parseLogRecyclerView->setBorderWidth(1);
  parseLogRecyclerView->onCreateView = [](const MainMenuParseLogRow &) {
    return new ParseLogRowView();
  };
  parseLogRecyclerView->onBind = [](View *view,
                                    const MainMenuParseLogRow &row, int,
                                    bool) {
    auto *rowView = dynamic_cast<ParseLogRowView *>(view);
    if (rowView != nullptr) {
      rowView->setRow(row);
    }
  };
  panel->addView(parseLogRecyclerView);

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
  refreshParseLogModal(true);
}

void MainMenuScene::showParseLogModal() {
  if (parseLogModalRoot == nullptr) {
    return;
  }
  parseLogModalRoot->setSize(rendering::window_width, rendering::window_height);
  parseLogModalRoot->setVisible(true);
  parseLogDisplayedRevision = 0;
  refreshParseLogModal(true);
}

void MainMenuScene::hideParseLogModal() {
  if (parseLogModalRoot != nullptr) {
    parseLogModalRoot->setVisible(false);
  }
}

void MainMenuScene::refreshParseLogModal(bool forceScrollToBottom) {
  if (parseLogModalRoot == nullptr || parseLogRecyclerView == nullptr) {
    return;
  }

  const std::uint64_t revision = archive_file::debugLogRevision();
  if (revision == parseLogDisplayedRevision) {
    if (forceScrollToBottom) {
      scrollParseLogModalToBottom();
    }
    return;
  }
  const bool shouldScrollToBottom =
      forceScrollToBottom || parseLogDisplayedRevision == 0 ||
      isParseLogScrolledNearBottom();
  parseLogDisplayedRevision = revision;
  parseLogRecyclerView->setItems(
      parseLogRowsFromLines(archive_file::debugLogLines()));
  if (shouldScrollToBottom) {
    scrollParseLogModalToBottom();
  }
}

bool MainMenuScene::isParseLogScrolledNearBottom() const {
  if (parseLogRecyclerView == nullptr) {
    return true;
  }
  const float maxOffset = std::max(
      0.0f, static_cast<float>(parseLogRecyclerView->size() *
                                   parseLogRecyclerView->itemHeight -
                               parseLogRecyclerView->getHeight()));
  return maxOffset - parseLogRecyclerView->scrollOffset <=
         static_cast<float>(parseLogRecyclerView->itemHeight * 2);
}

void MainMenuScene::scrollParseLogModalToBottom() {
  if (parseLogRecyclerView == nullptr) {
    return;
  }
  const float maxOffset = std::max(
      0.0f, static_cast<float>(parseLogRecyclerView->size() *
                                   parseLogRecyclerView->itemHeight -
                               parseLogRecyclerView->getHeight()));
  parseLogRecyclerView->scrollOffset = maxOffset;
  parseLogRecyclerView->rebindVisibleItems();
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

  cancelPreviewLoading(false);
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
  cancelPreviewLoading(false);
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
  cancelPreviewLoading(false);
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
  cancelPreviewLoading(false);
  context.jukebox.stop();

  std::string errorMessage;
  context.musicPlayer.PlayNextAsync(errorMessage, "Playing next track.");
  musicStatusMessage = errorMessage;
  refreshMusicModal();
}

void MainMenuScene::playPreviousMusicTrack() {
  cancelPreviewLoading(false);
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
      ->setHeight(460)
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
  tasksScrollView->setContentPadding(Edge::All, 12);

  tasksContent = new View();
  tasksContent->setFlexDirection(FlexDirection::Column);
  tasksContent->setAlignItems(YGAlignStretch);

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
  findBmsKeepFilesButton =
      makeModalButton("Keep Files", 18, &findBmsKeepFilesButtonText);
  findBmsDeleteFilesButton =
      makeModalButton("Delete Files", 18, &findBmsDeleteFilesButtonText);
  findBmsOpenButton = makeModalButton("Source", 18, &findBmsOpenButtonText);
  findBmsGoogleButton = makeModalButton("Search", 18, &findBmsGoogleButtonText);
  findBmsRefreshButton =
      makeModalButton("Refresh", 18, &findBmsRefreshButtonText);

  findBmsCloseButton->setWidth(130);
  findBmsKeepFilesButton->setWidth(150);
  findBmsDeleteFilesButton->setWidth(150);
  findBmsOpenButton->setWidth(180);
  findBmsGoogleButton->setWidth(150);
  findBmsRefreshButton->setWidth(150);
  findBmsCloseButton->setOnClickListener([this]() {
    if (!findBmsDialogPolicy(findBmsJobRunning.load(), findBmsResult)
             .showCloseOrCancel) {
      return;
    }
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
  findBmsKeepFilesButton->setOnClickListener([this]() {
    startFindBmsPendingArtifactResolution(
        BmsSearchPendingArtifactDecision::Keep);
  });
  findBmsDeleteFilesButton->setOnClickListener([this]() {
    startFindBmsPendingArtifactResolution(
        BmsSearchPendingArtifactDecision::Delete);
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
  footer->addView(findBmsKeepFilesButton);
  footer->addView(findBmsDeleteFilesButton);
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
  findBmsPendingDecision.reset();
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
  const BmsSearchDownloadOptions downloadOptions{
      .skipUnarchivingForNonSolidArchives =
          context.settings.findBmsSkipUnarchivingForNonSolidArchives};
  findBmsJobRunning = true;
  findBmsModalRoot->setSize(rendering::window_width, rendering::window_height);
  findBmsModalRoot->setVisible(true);
  refreshFindBmsModal();

  findBmsThread = std::jthread([this, record, downloadRoot, downloadOptions](
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
        progressCallback, record.meta.Title, record.meta.Artist,
        downloadOptions);
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
  const BmsSearchDownloadOptions downloadOptions{
      .skipUnarchivingForNonSolidArchives =
          context.settings.findBmsSkipUnarchivingForNonSolidArchives};
  findBmsResult = {};
  findBmsResult.candidates = {candidate};
  findBmsPendingDecision.reset();
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

  findBmsThread = std::jthread(
      [this, candidate, record, downloadRoot, downloadOptions](
          const std::stop_token &stopToken) {
        BmsSearchService service;
        auto progressCallback =
            [this](const BmsSearchDownloadProgress &progress) {
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
            findBmsCancelled, progressCallback, downloadOptions);
        {
          std::lock_guard<std::mutex> lock(findBmsUpdateMutex);
          pendingFindBmsResult = std::move(result);
          findBmsJobRunning = false;
        }
      });
}

void MainMenuScene::startFindBmsPendingArtifactResolution(
    BmsSearchPendingArtifactDecision decision) {
  if (findBmsJobRunning.load() || !findBmsResult.pendingArtifact) {
    return;
  }
  if (findBmsThread.joinable()) {
    findBmsThread.join();
  }

  BmsSearchResult result = findBmsResult;
  findBmsPendingDecision = decision;
  findBmsProgressMessage =
      decision == BmsSearchPendingArtifactDecision::Keep ? "Keeping files"
                                                        : "Deleting files";
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.95;
  findBmsProgressLog.push_back(findBmsProgressMessage);
  pendingFindBmsProgressEvents.clear();
  pendingFindBmsResult.reset();
  findBmsJobRunning = true;
  refreshFindBmsModal();

  findBmsThread = std::jthread(
      [this, result = std::move(result), decision](
          const std::stop_token &) mutable {
        BmsSearchService service;
        auto resolved =
            service.resolvePendingArtifact(std::move(result), decision);
        std::lock_guard<std::mutex> lock(findBmsUpdateMutex);
        pendingFindBmsResult = std::move(resolved);
        findBmsJobRunning = false;
      });
}

void MainMenuScene::hideFindBmsModal() {
  if (findBmsModalRoot == nullptr ||
      !findBmsDialogPolicy(findBmsJobRunning.load(), findBmsResult)
           .canDismiss) {
    return;
  }
  findBmsModalRoot->setVisible(false);
}

void MainMenuScene::refreshFindBmsModal() {
  if (findBmsModalRoot == nullptr) {
    return;
  }

  const bool running = findBmsJobRunning.load();
  const auto policy =
      findBmsDialogPolicy(findBmsJobRunning.load(), findBmsResult);
  if (findBmsModalTitleText != nullptr) {
    findBmsModalTitleText->setText("Find BMS");
  }

  std::string statusText;
  if (running) {
    if (findBmsPendingDecision) {
      statusText = *findBmsPendingDecision ==
                           BmsSearchPendingArtifactDecision::Keep
                       ? "Keeping files"
                       : "Deleting files";
    } else {
      statusText = findBmsProgressDisplayText(findBmsProgressMessage,
                                              findBmsProgressCurrent,
                                              findBmsProgressTotal, false);
    }
  } else {
    switch (findBmsResult.status) {
    case BmsSearchResult::Status::Downloaded:
      statusText = "Download complete";
      break;
    case BmsSearchResult::Status::NoDownloadLink:
    case BmsSearchResult::Status::UnsupportedLink:
      statusText = "Manual download needed";
      break;
    case BmsSearchResult::Status::NotFound:
      statusText = "Not found";
      break;
    case BmsSearchResult::Status::AmbiguousCandidates:
      statusText = "Choose a match";
      break;
    case BmsSearchResult::Status::HashMismatch:
      statusText = findBmsResult.pendingArtifact ? "Chart mismatch"
                                                 : "Decision complete";
      break;
    case BmsSearchResult::Status::DownloadFailed:
      statusText = "Download failed";
      break;
    }
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
      !running && policy.showNormalResultActions &&
      findBmsResult.status == BmsSearchResult::Status::AmbiguousCandidates &&
      !findBmsResult.candidates.empty();

  std::string detail;
  if (!findBmsModalChart.meta.Title.empty()) {
    detail += findBmsModalChart.meta.Title + "\n";
  }
  if (running && findBmsPendingDecision) {
    detail += "Resolving the downloaded files. This dialog cannot close yet.";
  } else if (!running && findBmsResult.pendingArtifact) {
    detail += findBmsResult.message.empty()
                  ? "Choose Keep Files or Delete Files to continue."
                  : findBmsResult.message;
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::Downloaded) {
    detail += "Refresh the library to use it.";
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::NoDownloadLink) {
    detail += "Download from the source, then refresh.";
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::UnsupportedLink) {
    detail += "Download from the source, then refresh.";
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::NotFound) {
    detail += "Try searching by title.";
  } else if (!running && findBmsResult.status ==
                             BmsSearchResult::Status::AmbiguousCandidates) {
    detail += "Choose an archive below.";
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::HashMismatch) {
    detail += findBmsResult.message.empty()
                  ? "The downloaded archive does not match this chart."
                  : findBmsResult.message;
  } else if (!running &&
             findBmsResult.status == BmsSearchResult::Status::DownloadFailed) {
    detail += "Open the source or try again.";
  } else {
    detail += "Searching available sources...";
  }
  if (findBmsDetailText != nullptr) {
    findBmsDetailText->setText(detail);
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
      policy.showNormalResultActions && !manualSourceUrl.empty() &&
      findBmsResult.status != BmsSearchResult::Status::Downloaded &&
      findBmsResult.status != BmsSearchResult::Status::NotFound;
  const bool hasSearchAction =
      policy.showNormalResultActions && !downloaded &&
      (!findBmsModalChart.meta.SHA256.empty() ||
       !findBmsModalChart.meta.MD5.empty() ||
       !findBmsModalChart.meta.Title.empty() ||
       !findBmsModalChart.meta.Artist.empty());
  const bool hasRefreshAction =
      policy.showNormalResultActions && !running && !downloaded;
  if (findBmsCloseButtonText != nullptr) {
    findBmsCloseButtonText->setText(running ? "Cancel" : "Close");
  }
  if (findBmsCloseButton != nullptr) {
    findBmsCloseButton->setVisible(policy.showCloseOrCancel);
    findBmsCloseButton->setWidth(policy.showCloseOrCancel ? 130.0f : 0.0f);
  }
  if (findBmsKeepFilesButton != nullptr) {
    findBmsKeepFilesButton->setVisible(policy.showPendingActions);
    findBmsKeepFilesButton->setWidth(policy.showPendingActions ? 150.0f
                                                              : 0.0f);
  }
  if (findBmsDeleteFilesButton != nullptr) {
    findBmsDeleteFilesButton->setVisible(policy.showPendingActions);
    findBmsDeleteFilesButton->setWidth(policy.showPendingActions ? 150.0f
                                                                : 0.0f);
  }
  if (findBmsOpenButtonText != nullptr) {
    const bool downloadSource =
        (findBmsResult.status == BmsSearchResult::Status::DownloadFailed ||
         findBmsResult.status == BmsSearchResult::Status::HashMismatch) &&
        findBmsResult.fallbackUrl.empty() && !findBmsResult.downloadUrl.empty();
    const bool bmsSearchSource =
        manualSourceUrl.find("bmssearch.net") != std::string::npos;
    findBmsOpenButtonText->setText(
        downloadSource ? "Download"
                       : (bmsSearchSource ? "BMS Search" : "Source"));
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
  styleThemedActionButton(
      findBmsKeepFilesButton, findBmsKeepFilesButtonText,
      policy.showPendingActions, ui_theme::successAction,
      ui_theme::successActionHover, ui_theme::successActionPressed,
      ui_theme::accentBorder);
  styleThemedActionButton(
      findBmsDeleteFilesButton, findBmsDeleteFilesButtonText,
      policy.showPendingActions, ui_theme::dangerAction,
      ui_theme::dangerActionHover, ui_theme::dangerActionPressed,
      ui_theme::accentBorder);
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
    findBmsPendingDecision.reset();
    const bool keptMismatchedFiles =
        findBmsResult.status == BmsSearchResult::Status::HashMismatch &&
        !findBmsResult.pendingArtifact && !findBmsResult.outputPath.empty();
    if (findBmsResult.status == BmsSearchResult::Status::Downloaded ||
        keptMismatchedFiles) {
      findBmsProgressFraction = 1.0;
    }
    if (!findBmsResult.message.empty() &&
        (findBmsProgressLog.empty() ||
         findBmsProgressLog.back() != findBmsResult.message)) {
      appendLogLine(findBmsResult.message);
    }
    if (findBmsResult.status == BmsSearchResult::Status::Downloaded ||
        keptMismatchedFiles) {
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

  const float availablePanelWidth =
      std::max(300.0f, static_cast<float>(rendering::window_width) - 48.0f);
  const float kModalPanelWidth = std::min(760.0f, availablePanelWidth);
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalScrollRightPadding = 18.0f;
  const float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;
  const float kOptionContentWidth =
      std::max(0.0f, kModalContentWidth - kModalScrollRightPadding);
  const float availablePanelHeight =
      std::max(240.0f, static_cast<float>(rendering::window_height) - 72.0f);
  const float kModalPanelHeight = std::min(820.0f, availablePanelHeight);

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
      ->setHeight(kModalPanelHeight)
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

  auto *scrollView = new ScrollView(0, 0, static_cast<int>(kModalContentWidth),
                                    1);
  scrollView->setWidth(kModalContentWidth);
  scrollView->setFlex(1.0f);
  scrollView->setContentPadding(Edge::Right, kModalScrollRightPadding);

  auto *optionsContent = new View();
  optionsContent->setFlexDirection(FlexDirection::Column);
  optionsContent->setAlignItems(YGAlignStretch);
  optionsContent->setGap(12);
  optionsContent->setWidth(kOptionContentWidth);

  const size_t playOptionColumns = kOptionContentWidth >= 620.0f ? 4U : 2U;
  playOptionsPanel = new PlayOptionsPanelView(
      {.onRulesetSelected = [this](GameplayRuleset ruleset) {
         setGameplayRulesetSelection(ruleset);
       },
       .onGaugeSelected = [this](GaugeType type,
                                 GaugeAutoShiftMode autoShift) {
         setGaugeSelection(type, autoShift);
       },
       .onGaugeLowerBoundSelected = [this](GaugeType type) {
         setGaugeAutoShiftLowerBound(type);
       },
       .onPlayOptionSelected = [this](const std::string &option) {
         setPlayOptionSelection(option);
       },
       .isPlayOptionAllowed = [this](const std::string &option) {
         return currentPlayOptionSelectionAllowed(option);
       },
       .onLongNoteModeSelected = [this](const std::string &mode) {
         setLongNoteModeSelection(mode);
       },
       .onAssistOptionSelected = [this](const std::string &option) {
         setAssistOptionSelection(option);
       },
       .onPlaybackRateSelected = [this](int percent) {
         setPlaybackRateSelection(percent);
       },
       .onPlaybackModeSelected = [this](const std::string &mode) {
         setPlaybackModeSelection(mode);
       },
       .onClubModeToggled = [this]() { toggleGameplayClubMode(); },
       .onPacemakerSelected = [this](const std::string &target) {
         setPacemakerTargetSelection(target);
       }},
      {.width = kOptionContentWidth,
       .playOptionColumns = static_cast<int>(playOptionColumns),
       .showGauge = true,
       .showLaneOrder = false,
       .showPacemaker = true},
      overlayPortal);
  optionsContent->addView(playOptionsPanel);

  scrollView->setContentView(optionsContent);
  panel->addView(scrollView);

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
  refreshPlaybackSelectionControls();
  refreshPacemakerTargetButtons();
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
  refreshPlaybackSelectionControls();
  refreshPacemakerTargetButtons();
  playOptionsModalRoot->setSize(rendering::window_width,
                                rendering::window_height);
  playOptionsModalRoot->setVisible(true);
  playOptionsModalRoot->applyYogaLayout();
}

void MainMenuScene::hidePlayOptionsModal() {
  if (playOptionsModalRoot == nullptr) {
    return;
  }
  if (playOptionsPanel != nullptr) {
    playOptionsPanel->closeDropdowns();
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

  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(10)
      ->setHeight(54);
  replayModalTitleText = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  replayModalTitleText->setText("Records");
  replayModalTitleText->setThemedColor(ui_theme::textPrimary);
  replayModalTitleText->setHeight(54);
  replayModalTitleText->setFlexGrow(1.0f);
  replayModalTitleText->setFlexBasis(0.0f);
  replayModalTitleText->setMinWidth(0.0f);
  replayModalFilterButton =
      makeModalIconButton(kIconFilter, 20, &replayModalFilterButtonText);
  replayModalFilterButton->setWidth(54);
  replayModalFilterButton->setHeight(54);
  replayModalFilterButton->setFlexShrink(0.0f);
  replayModalCloseButton =
      makeModalIconButton(kIconXmark, 22, &replayModalCloseButtonText);
  replayModalCloseButton->setWidth(54);
  replayModalCloseButton->setHeight(54);
  replayModalCloseButton->setFlexShrink(0.0f);
  header->addView(replayModalTitleText);
  header->addView(replayModalFilterButton);
  header->addView(replayModalCloseButton);
  panel->addView(header);

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
  replayListView = new ResultRecordListView();
  replayListView->onSelectionChanged = [this](int idx) {
    selectReplayModalIndex(idx);
    if (selectedReplayIsAutoPlay()) {
      selectedReplayRenderTouchPoints = false;
      selectedReplayRenderGhosts = false;
      refreshReplayExportOptionButtons();
    }
    refreshReplayModalActions();
  };
  replayListView->onIrUploadRequested =
      [this](const ResultRecordSummary &record) {
        if (!record.capabilities.irUpload || !record.local.has_value()) {
          return;
        }
        startReplayIrUpload(replayModalChart, *record.local);
      };
  replayListView->onIrStatusFeedbackRequested =
      [this](const ResultRecordSummary &record) {
        publishReplayIrStatusFeedback(record.irState);
      };
  replayListView->setFlex(1);
  replayListView->clearBackgroundColor();
  replayListView->setThemedBorderColor(ui_theme::hairline);
  replayListView->setBorderWidth(1);
  replayListContent->addView(replayListView);
  replayModalContentFrame->addView(replayListContent);

  replayFilterSortContent = new View();
  replayFilterSortContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight);
  replayFilterSortContent->setVisible(false);

  constexpr float kFilterScrollRightPadding = 12.0f;
  constexpr float kFilterContentWidth =
      kModalContentWidth - kFilterScrollRightPadding;
  auto *filterScroll = new ScrollView(0, 0, static_cast<int>(kModalContentWidth),
                                      static_cast<int>(kModalContentHeight));
  filterScroll->setWidth(kModalContentWidth);
  filterScroll->setHeight(kModalContentHeight);
  filterScroll->setContentPadding(Edge::Right, kFilterScrollRightPadding);

  auto *filterContent = new View();
  filterContent->setFlexDirection(FlexDirection::Column);
  filterContent->setAlignItems(YGAlignStretch);
  filterContent->setGap(10);
  filterContent->setWidth(kFilterContentWidth);

  auto makeFilterButton = [](const std::string &label, int fontSize,
                             TextView **textOut) {
    auto *button = makeModalButton(label, fontSize, textOut);
    button->setHeight(46);
    button->setFlexGrow(1.0f);
    button->setFlexBasis(0.0f);
    button->setFlexShrink(1.0f);
    return button;
  };
  auto addFilterButton = [&](View *&row, size_t &index, size_t columns,
                             Button *button) {
    if (index % columns == 0) {
      row = makeModalOptionRow(46);
      filterContent->addView(row);
    }
    if (row != nullptr) {
      row->addView(button);
    }
    ++index;
  };

  filterContent->addView(makeModalLabel("Clear Mark"));
  View *filterRow = nullptr;
  size_t filterIndex = 0;
  auto makeClearFilterButton = [this, &makeFilterButton](
                                   const std::string &label,
                                   std::optional<int> rank) {
    TextView *text = nullptr;
    auto *button = makeFilterButton(label, 15, &text);
    button->setOnClickListener(
        [this, rank]() { setReplayClearFilter(rank); });
    replayClearFilterButtons.push_back({
        .button = button,
        .text = text,
        .rank = rank,
    });
    return button;
  };
  addFilterButton(filterRow, filterIndex, 3,
                  makeClearFilterButton("All", std::nullopt));
  for (const auto &filter : kDifficultyClearMarkFilters) {
    if (filter.rank == kNoClearTypeRank) {
      continue;
    }
    addFilterButton(filterRow, filterIndex, 3,
                    makeClearFilterButton(filter.label, filter.rank));
  }

  filterContent->addView(makeModalLabel("Play Option"));
  filterRow = nullptr;
  filterIndex = 0;
  auto makePlayOptionFilterButton = [this, &makeFilterButton](
                                        const std::string &label,
                                        std::optional<std::string> option) {
    TextView *text = nullptr;
    auto *button = makeFilterButton(label, 14, &text);
    button->setOnClickListener(
        [this, option]() { setReplayPlayOptionFilter(option); });
    replayPlayOptionFilterButtons.push_back({
        .button = button,
        .text = text,
        .option = option,
    });
    return button;
  };
  addFilterButton(filterRow, filterIndex, 4,
                  makePlayOptionFilterButton("All", std::nullopt));
  for (const char *option : play_options::kPlayOptions) {
    addFilterButton(filterRow, filterIndex, 4,
                    makePlayOptionFilterButton(option, std::string(option)));
  }

  filterContent->addView(makeModalLabel("Score Rank"));
  filterRow = nullptr;
  filterIndex = 0;
  auto makeScoreRankFilterButton = [this, &makeFilterButton](
                                       const std::string &label,
                                       std::optional<std::string> rank) {
    TextView *text = nullptr;
    auto *button = makeFilterButton(label, 16, &text);
    button->setOnClickListener(
        [this, rank]() { setReplayScoreRankFilter(rank); });
    replayScoreRankFilterButtons.push_back({
        .button = button,
        .text = text,
        .rank = rank,
    });
    return button;
  };
  addFilterButton(filterRow, filterIndex, 4,
                  makeScoreRankFilterButton("All", std::nullopt));
  constexpr std::array<const char *, 10> kScoreRankFilterLabels = {
      "MAX", "MAX -", "AAA", "AA", "A", "B", "C", "D", "E", "F"};
  for (const char *rank : kScoreRankFilterLabels) {
    addFilterButton(filterRow, filterIndex, 4,
                    makeScoreRankFilterButton(rank, std::string(rank)));
  }

  filterContent->addView(makeModalLabel("Sort"));
  filterRow = nullptr;
  filterIndex = 0;
  auto makeSortButton = [this, &makeFilterButton](
                            const std::string &label,
                            ReplayRecordSortCriterion criterion) {
    TextView *text = nullptr;
    auto *button = makeFilterButton(label, 16, &text);
    button->setOnClickListener(
        [this, criterion]() { setReplaySortCriterion(criterion); });
    replaySortButtons.push_back({
        .button = button,
        .text = text,
        .criterion = criterion,
    });
    return button;
  };
  addFilterButton(filterRow, filterIndex, 2,
                  makeSortButton("Newest", ReplayRecordSortCriterion::Newest));
  addFilterButton(
      filterRow, filterIndex, 2,
      makeSortButton("Clear Mark", ReplayRecordSortCriterion::ClearMark));
  addFilterButton(filterRow, filterIndex, 2,
                  makeSortButton("Score", ReplayRecordSortCriterion::Score));
  addFilterButton(
      filterRow, filterIndex, 2,
      makeSortButton("Max Combo", ReplayRecordSortCriterion::MaxCombo));

  filterScroll->setContentView(filterContent);
  replayFilterSortContent->addView(filterScroll);
  replayModalContentFrame->addView(replayFilterSortContent);

  replayWatchOptionsContent = new View();
  replayWatchOptionsContent->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setPositionType(YGPositionTypeAbsolute)
      ->setPosition(Edge::Left, 0)
      ->setPosition(Edge::Top, 0)
      ->setWidth(kModalContentWidth)
      ->setHeight(kModalContentHeight)
      ->setJustifyContent(YGJustifyCenter)
      ->setGap(12);
  replayWatchOptionsContent->setVisible(false);

  replayWatchOptionsContent->addView(makeModalLabel("Watch Visualization"));
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
  replayWatchOptionsContent->addView(replayTouchRow);
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
  replayWatchOptionsContent->addView(replayGhostRow);
  replayModalContentFrame->addView(replayWatchOptionsContent);

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
  footer->setGap(8);
  footer->setHeight(58);

  replayWatchButton = makeModalButton("Watch", 20, &replayWatchButtonText);
  replayGBattleButton =
      makeModalButton("G-BATTLE", 18, &replayGBattleButtonText);
  replayModalResultButton =
      makeModalButton("View Result", 18, &replayModalResultButtonText);
  replayModalExportButton =
      makeModalButton("Export Video", 18, &replayModalExportButtonText);
  replayModalCloseButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || replayResultRecallInProgress ||
        replayIrUploadInProgress) {
      return;
    }
    hideReplayModal();
  });
  replayModalFilterButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || replayResultRecallInProgress ||
        replayIrUploadInProgress) {
      return;
    }
    if (replayFilterSortContent != nullptr &&
        replayFilterSortContent->getVisible()) {
      replayModalTitleText->setText("Records");
      replayFilterSortContent->setVisible(false);
      replayListContent->setVisible(true);
      replayListView->restoreSelection(selectedReplayIndex);
      refreshReplayModalActions();
      return;
    }
    showReplayFilterSortOptions();
  });
  replayWatchButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || replayResultRecallInProgress ||
        replayIrUploadInProgress) {
      return;
    }
    if (!selectedResultRecordSummary.has_value() ||
        !selectedResultRecordSummary->capabilities.watch ||
        !selectedReplaySummary.has_value()) {
      return;
    }
    if (replayWatchOptionsContent != nullptr &&
        replayWatchOptionsContent->getVisible()) {
      startReplayPlayback(replayModalChart, selectedReplaySummary->id);
      return;
    }
    replayModalTitleText->setText("Watch Options");
    replayListContent->setVisible(false);
    replayFilterSortContent->setVisible(false);
    replayWatchOptionsContent->setVisible(true);
    replayExportOptionsContent->setVisible(false);
    replayExportProgressContent->setVisible(false);
    refreshReplayExportOptionButtons();
    refreshReplayModalActions();
    replayModalRoot->applyYogaLayoutFromRoot();
  });
  replayGBattleButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || replayResultRecallInProgress ||
        replayIrUploadInProgress) {
      return;
    }
    if (!selectedResultRecordSummary.has_value() ||
        !selectedResultRecordSummary->capabilities.gBattle ||
        !selectedReplaySummary.has_value()) {
      return;
    }
    startGBattlePlayback(replayModalChart, selectedReplaySummary->id);
  });
  replayModalResultButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || replayResultRecallInProgress ||
        replayIrUploadInProgress ||
        !selectedResultRecordSummary.has_value() ||
        !selectedResultRecordSummary->capabilities.resultRecall) {
      return;
    }
    if (selectedReplaySummary.has_value()) {
      startReplayResultRecall(replayModalChart, selectedReplaySummary->id);
      return;
    }
    const auto *remoteIdentity = std::get_if<IrRemoteRecordId>(
        &selectedResultRecordSummary->identity);
    if (remoteIdentity == nullptr || !selectedResultRecordStableKey) {
      return;
    }
    startRemoteResultRecall(*remoteIdentity, *selectedResultRecordStableKey);
  });
  replayModalExportButton->setOnClickListener([this]() {
    if (replayExportInProgress.load() || replayResultRecallInProgress ||
        replayIrUploadInProgress) {
      return;
    }
    if (replayWatchOptionsContent != nullptr &&
        replayWatchOptionsContent->getVisible()) {
      return;
    }
    if (replayFilterSortContent != nullptr &&
        replayFilterSortContent->getVisible()) {
      return;
    }
    if (replayExportOptionsContent != nullptr &&
        replayExportOptionsContent->getVisible()) {
      if (!replayExportSelection.has_value()) {
        return;
      }
      const ReplaySummary exportSelection = replayExportSelection.value();
      const bool exportAutoPlay = exportSelection.autoPlay;
      ReplayVideoExportOptions options;
      options.fps = selectedExportFps;
      options.includeResultScreen = selectedExportIncludeResultScreen;
      options.renderTouchPoints =
          exportAutoPlay ? false : selectedReplayRenderTouchPoints;
      options.renderReplayGhosts =
          exportAutoPlay ? false : selectedReplayRenderGhosts;
      options.pacemakerTarget =
          exportAutoPlay
              ? pacemaker::kTargetOff
              : pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
      if (!selectedExportFullResolution) {
        options.height = 1080;
      }
      startReplayVideoExport(replayExportChart, exportSelection.id, options);
      return;
    }
    if (!selectedResultRecordSummary.has_value() ||
        !selectedResultRecordSummary->capabilities.videoExport ||
        !selectedReplaySummary.has_value()) {
      return;
    }
    showReplayExportOptions();
  });
  footer->addView(replayWatchButton);
  footer->addView(replayGBattleButton);
  footer->addView(replayModalResultButton);
  footer->addView(replayModalExportButton);
  panel->addView(footer);

  replayModalRoot->addView(panel);
  rootLayout->addView(replayModalRoot);
  refreshReplayExportOptionButtons();
  refreshReplayModalActions();
}

void MainMenuScene::reloadReplayRecordModels(bool preserveViewState) {
  std::optional<std::string> preferredStableKey =
      preserveViewState ? selectedResultRecordStableKey : std::nullopt;
  if (!preferredStableKey && preserveViewState && selectedReplaySummary) {
    preferredStableKey =
        makeLocalResultRecord(*selectedReplaySummary).stableKey();
  }
  const float previousScrollOffset =
      preserveViewState && replayListView != nullptr
          ? replayListView->scrollOffset
          : 0.0F;

  const bool courseReplayList =
      replayModalChart.courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      (!activeFolder.courseKey.empty() || activeFolder.courseId > 0);
  if (courseReplayList) {
    replaySummaries = context.replayRepository.ListCourseReplays(
        {.courseKey = activeFolder.courseKey,
         .legacyCourseId = activeFolder.courseId},
        0);
  } else {
    const std::string irServerOrigin =
        activeReplayIrServerOrigin().value_or(std::string{});
    replaySummaries = context.replayRepository.ListReplays(
        replayModalChart.meta, 0, ir::kTachiProviderId, irServerOrigin);
    for (ReplaySummary &summary : replaySummaries) {
      ir::resolveReplayIrRecordState(summary);
    }
    replaySummaries.insert(replaySummaries.begin(),
                           autoPlayReplaySummary(replayModalChart));
  }

  std::vector<ir::IrRemoteScore> remoteScores;
  std::string mergeOrigin;
  bool remoteReadSucceeded = false;
  if (!courseReplayList) {
    const auto providerSettings = context.settings.irProviders.find(
        std::string(ir::kTachiProviderId));
    if (providerSettings == context.settings.irProviders.end() ||
        !providerSettings->second.enabled) {
      remoteReadSucceeded = true;
    } else if (const auto normalizedOrigin = ir::normalizeServerOrigin(
                   providerSettings->second.serverOrigin)) {
      mergeOrigin = *normalizedOrigin;
      auto loaded = context.replayRepository.ListIrRemoteScoresForChart(
          ir::kTachiProviderId, mergeOrigin, replayModalChart.meta.MD5,
          replayModalChart.meta.SHA256);
      if (loaded.status == ir::IrRemoteScoreReadOutcome::Status::Loaded) {
        remoteScores = std::move(loaded.scores);
        remoteReadSucceeded = true;
      } else {
        std::string diagnostic = ir::sanitizeDiagnostic(
            std::string("IR Records unavailable: ") +
            (loaded.diagnostic.empty()
                 ? "remote score history could not be read"
                 : loaded.diagnostic));
        if (diagnostic != publishedResultRecordDiagnostic) {
          publishedResultRecordDiagnostic = diagnostic;
          SDL_Log("%s", diagnostic.c_str());
          archive_file::appendDebugLogLine(diagnostic);
        }
      }
    } else {
      const std::string diagnostic =
          "IR Records unavailable: provider origin is invalid";
      if (diagnostic != publishedResultRecordDiagnostic) {
        publishedResultRecordDiagnostic = diagnostic;
        SDL_Log("%s", diagnostic.c_str());
        archive_file::appendDebugLogLine(diagnostic);
      }
    }
  } else {
    remoteReadSucceeded = true;
  }

  try {
    resultRecordSummaries = mergeResultRecords(
        replaySummaries,
        remoteReadSucceeded
            ? std::span<const ir::IrRemoteScore>(remoteScores)
            : std::span<const ir::IrRemoteScore>{},
        ir::kTachiProviderId, mergeOrigin);
    if (remoteReadSucceeded) {
      publishedResultRecordDiagnostic.clear();
    }
  } catch (...) {
    resultRecordSummaries = mergeResultRecords(
        replaySummaries, std::span<const ir::IrRemoteScore>{},
        ir::kTachiProviderId, std::string_view{});
    const std::string diagnostic =
        "IR Records unavailable: remote score projection is invalid";
    if (diagnostic != publishedResultRecordDiagnostic) {
      publishedResultRecordDiagnostic = diagnostic;
      SDL_Log("%s", diagnostic.c_str());
      archive_file::appendDebugLogLine(diagnostic);
    }
  }

  applyReplayRecordFilters(std::move(preferredStableKey));
  if (preserveViewState && replayListView != nullptr) {
    replayListView->scrollOffset = previousScrollOffset;
    replayListView->rebindVisibleItems();
  }
}

void MainMenuScene::showReplayListModal(const ChartMetaRecord &record) {
  if (replayModalRoot == nullptr || replayListView == nullptr) {
    return;
  }

  replayModalChart = record;
  replayExportSelection.reset();
  replayIrUploadInProgress = false;
  replayIrObservedRevisions.clear();
  ++replayIrUploadFeedbackRevision;

  clearReplayModalSelection();
  selectedReplayRenderTouchPoints = context.settings.touchVisualizationEnabled;
  selectedReplayRenderGhosts = true;
  replayRecordFilters = {};
  reloadReplayRecordModels(false);
  setReplayButtonVisible(true);
  replayModalTitleText->setText("Records");
  replayListContent->setVisible(true);
  replayFilterSortContent->setVisible(false);
  replayWatchOptionsContent->setVisible(false);
  replayExportOptionsContent->setVisible(false);
  replayExportProgressContent->setVisible(false);
  replayModalRoot->setSize(rendering::window_width, rendering::window_height);
  replayModalRoot->setVisible(true);
  refreshReplayFilterSortButtons();
  refreshReplayExportOptionButtons();
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayoutFromRoot();
}

void MainMenuScene::showReplayFilterSortOptions() {
  if (replayModalRoot == nullptr || replayFilterSortContent == nullptr) {
    return;
  }
  replayModalTitleText->setText("Filter / Sort");
  replayListContent->setVisible(false);
  replayFilterSortContent->setVisible(true);
  replayWatchOptionsContent->setVisible(false);
  replayExportOptionsContent->setVisible(false);
  replayExportProgressContent->setVisible(false);
  replayExportSelection.reset();
  refreshReplayFilterSortButtons();
  refreshReplayModalActions();
  replayModalRoot->applyYogaLayoutFromRoot();
}

void MainMenuScene::showReplayExportOptions() {
  if (replayModalRoot == nullptr ||
      !selectedResultRecordSummary.has_value() ||
      !selectedResultRecordSummary->capabilities.videoExport ||
      !selectedReplaySummary.has_value()) {
    return;
  }

  replayExportSelection = selectedReplaySummary;
  replayExportChart = replayModalChart;
  replayModalTitleText->setText("Export Options");
  replayListContent->setVisible(false);
  replayFilterSortContent->setVisible(false);
  replayWatchOptionsContent->setVisible(false);
  replayExportOptionsContent->setVisible(true);
  replayExportProgressContent->setVisible(false);
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  selectedExportIncludeResultScreen = true;
  if (replayExportSelection->autoPlay) {
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
  replayFilterSortContent->setVisible(false);
  replayWatchOptionsContent->setVisible(false);
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
  if (replayExportInProgress.load() || replayResultRecallInProgress ||
      replayIrUploadInProgress) {
    return;
  }
  replayModalRoot->setVisible(false);
  replayIrObservedRevisions.clear();
  ++replayIrUploadFeedbackRevision;
  clearReplayModalSelection();
  replayExportSelection.reset();
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Watch");
  }
  if (replayGBattleButtonText != nullptr) {
    replayGBattleButtonText->setText("G-BATTLE");
  }
  if (replayModalResultButtonText != nullptr) {
    replayModalResultButtonText->setText("View Result");
  }
  if (replayModalExportButtonText != nullptr) {
    replayModalExportButtonText->setText("Export Video");
  }
}

void MainMenuScene::refreshReplayModalActions() {
  const bool filterSortMode = replayFilterSortContent != nullptr &&
                              replayFilterSortContent->getVisible();
  const bool watchOptionsMode = replayWatchOptionsContent != nullptr &&
                                replayWatchOptionsContent->getVisible();
  const bool optionsMode = replayExportOptionsContent != nullptr &&
                           replayExportOptionsContent->getVisible();
  const bool progressMode = replayExportProgressContent != nullptr &&
                            replayExportProgressContent->getVisible();
  const ResultRecordCapabilities capabilities =
      selectedResultRecordSummary.has_value()
          ? selectedResultRecordSummary->capabilities
          : ResultRecordCapabilities{};
  const bool exportInProgress = replayExportInProgress.load();
  const bool resultRecallInProgress = replayResultRecallInProgress;
  const bool irUploadInProgress = replayIrUploadInProgress;
  const bool modalOperationInProgress =
      exportInProgress || resultRecallInProgress || irUploadInProgress;
  const bool watchVisible = capabilities.watch && !filterSortMode &&
                            !optionsMode && !progressMode;
  const bool gbattleVisible = capabilities.gBattle && !filterSortMode &&
                              !watchOptionsMode && !optionsMode &&
                              !progressMode;
  const ResultRecordRecallActionState resultAction =
      resultRecordRecallActionState(
          selectedResultRecordSummary,
          !filterSortMode && !watchOptionsMode && !optionsMode &&
              !progressMode,
          modalOperationInProgress);
  const bool resultVisible = resultAction.visible;
  const bool exportVisible = capabilities.videoExport && !filterSortMode &&
                             !watchOptionsMode && !progressMode;

  if (replayModalCloseButtonText != nullptr) {
    replayModalCloseButtonText->setText(ui_icons::textForCodepoint(kIconXmark));
  }
  if (replayModalFilterButtonText != nullptr) {
    replayModalFilterButtonText->setText(
        ui_icons::textForCodepoint(kIconFilter));
  }
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Watch");
  }
  if (replayGBattleButtonText != nullptr) {
    replayGBattleButtonText->setText("G-BATTLE");
  }
  if (replayModalResultButtonText != nullptr) {
    replayModalResultButtonText->setText(
        resultRecallInProgress ? "Loading..." : "View Result");
  }
  if (replayModalExportButtonText != nullptr) {
    replayModalExportButtonText->setText(exportInProgress ? "Exporting"
                                                          : "Export Video");
  }

  if (replayModalFilterButton != nullptr) {
    const bool filterVisible =
        !watchOptionsMode && !optionsMode && !progressMode;
    replayModalFilterButton->setVisible(filterVisible);
    replayModalFilterButton->setWidth(filterVisible ? 54.0f : 0.0f);
  }
  if (replayWatchButton != nullptr) {
    replayWatchButton->setVisible(watchVisible);
    replayWatchButton->setWidth(
        watchVisible ? (watchOptionsMode ? 160.0F : 124.0F) : 0.0F);
  }
  if (replayGBattleButton != nullptr) {
    replayGBattleButton->setVisible(gbattleVisible);
    replayGBattleButton->setWidth(gbattleVisible ? 144.0F : 0.0F);
  }
  if (replayModalResultButton != nullptr) {
    replayModalResultButton->setVisible(resultVisible);
    replayModalResultButton->setWidth(resultVisible ? 142.0F : 0.0F);
  }
  if (replayModalExportButton != nullptr) {
    replayModalExportButton->setVisible(exportVisible);
    replayModalExportButton->setWidth(
        exportVisible ? (optionsMode ? 160.0F : 142.0F) : 0.0F);
  }

  styleThemedActionButton(replayModalCloseButton, replayModalCloseButtonText,
                          !modalOperationInProgress, ui_theme::control,
                          ui_theme::controlHover, ui_theme::controlPressed,
                          ui_theme::hairlineStrong);
  if (replay_record_filters::hasActiveCriteria(replayRecordFilters) ||
      filterSortMode) {
    styleThemedActionButton(
        replayModalFilterButton, replayModalFilterButtonText,
        !watchOptionsMode && !optionsMode && !progressMode &&
            !modalOperationInProgress,
        ui_theme::primaryAction, ui_theme::primaryActionHover,
        ui_theme::primaryActionPressed, ui_theme::accentBorderStrong);
  } else {
    styleThemedActionButton(
        replayModalFilterButton, replayModalFilterButtonText,
        !watchOptionsMode && !optionsMode && !progressMode &&
            !modalOperationInProgress,
        ui_theme::control, ui_theme::controlHover, ui_theme::controlPressed,
        ui_theme::hairlineStrong);
  }
  styleThemedActionButton(replayWatchButton, replayWatchButtonText,
                          watchVisible && !modalOperationInProgress,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  styleThemedActionButton(replayGBattleButton, replayGBattleButtonText,
                          gbattleVisible && !modalOperationInProgress,
                          ui_theme::warningAction,
                          ui_theme::warningActionHover,
                          ui_theme::warningActionPressed, ui_theme::amber);
  styleThemedActionButton(replayModalResultButton, replayModalResultButtonText,
                          resultAction.enabled,
                          ui_theme::successAction,
                          ui_theme::successActionHover,
                          ui_theme::successActionPressed, ui_theme::lime);
  styleThemedActionButton(replayModalExportButton, replayModalExportButtonText,
                          exportVisible && !modalOperationInProgress,
                          ui_theme::violetAction, ui_theme::violetActionHover,
                          ui_theme::violetActionPressed,
                          ui_theme::violetActionHover);

  if (replayModalRoot != nullptr) {
    replayModalRoot->applyYogaLayoutFromRoot();
  }
}

void MainMenuScene::refreshReplayFilterSortButtons() {
  for (const ReplayClearFilterButton &item : replayClearFilterButtons) {
    styleOptionButton(item.button, item.text,
                      item.rank == replayRecordFilters.clearMarkRank);
  }
  for (const ReplayOptionFilterButton &item : replayPlayOptionFilterButtons) {
    const std::optional<std::string> normalized =
        item.option.has_value()
            ? std::optional<std::string>(
                  play_options::normalizePlayOption(*item.option))
            : std::nullopt;
    styleOptionButton(item.button, item.text,
                      normalized == replayRecordFilters.playOption);
  }
  for (const ReplayScoreRankFilterButton &item :
       replayScoreRankFilterButtons) {
    if (item.rank.has_value() && !replayScoreRankFilterAvailable()) {
      styleThemedActionButton(item.button, item.text, false,
                              ui_theme::control, ui_theme::controlHover,
                              ui_theme::controlPressed,
                              ui_theme::hairlineStrong);
    } else {
      styleOptionButton(item.button, item.text,
                        item.rank == replayRecordFilters.scoreRank);
    }
  }
  for (const ReplaySortButton &item : replaySortButtons) {
    styleOptionButton(item.button, item.text,
                      item.criterion == replayRecordFilters.sort);
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

void MainMenuScene::clearReplayModalSelection() {
  selectedReplayIndex = -1;
  selectedReplaySummary.reset();
  selectedResultRecordSummary.reset();
  selectedResultRecordStableKey.reset();
}

bool MainMenuScene::selectReplayModalIndex(int index) {
  clearReplayModalSelection();
  if (index < 0 ||
      index >= static_cast<int>(visibleResultRecordSummaries.size())) {
    return false;
  }

  selectedReplayIndex = index;
  selectedResultRecordSummary =
      visibleResultRecordSummaries[static_cast<std::size_t>(index)];
  selectedReplaySummary = selectedResultRecordSummary->local;
  selectedResultRecordStableKey = selectedResultRecordSummary->stableKey();
  return true;
}

void MainMenuScene::applyReplayRecordFilters(
    std::optional<std::string> preferredStableKey) {
  if (!preferredStableKey.has_value()) {
    preferredStableKey = selectedResultRecordStableKey;
  }

  visibleResultRecordSummaries = replay_record_filters::apply(
      resultRecordSummaries, replayRecordFilters);
  if (replayListView != nullptr) {
    replayListView->setResultRecords(visibleResultRecordSummaries);
  }

  clearReplayModalSelection();
  int restoreIndex = -1;
  if (preferredStableKey.has_value()) {
    for (size_t i = 0; i < visibleResultRecordSummaries.size(); ++i) {
      if (visibleResultRecordSummaries[i].stableKey() ==
          *preferredStableKey) {
        restoreIndex = static_cast<int>(i);
        break;
      }
    }
  }
  if (restoreIndex >= 0) {
    selectReplayModalIndex(restoreIndex);
    if (replayListView != nullptr) {
      replayListView->restoreSelection(restoreIndex);
    }
  }
  refreshReplayModalActions();
}

void MainMenuScene::setReplayClearFilter(std::optional<int> rank) {
  replayRecordFilters.clearMarkRank = rank;
  applyReplayRecordFilters();
  refreshReplayFilterSortButtons();
}

void MainMenuScene::setReplayPlayOptionFilter(
    std::optional<std::string> option) {
  replayRecordFilters.playOption =
      option.has_value() ? std::optional<std::string>(
                               play_options::normalizePlayOption(*option))
                         : std::nullopt;
  applyReplayRecordFilters();
  refreshReplayFilterSortButtons();
}

void MainMenuScene::setReplayScoreRankFilter(
    std::optional<std::string> rank) {
  if (rank.has_value() && !replayScoreRankFilterAvailable()) {
    return;
  }
  replayRecordFilters.scoreRank = rank;
  applyReplayRecordFilters();
  refreshReplayFilterSortButtons();
}

void MainMenuScene::setReplaySortCriterion(
    ReplayRecordSortCriterion criterion) {
  replayRecordFilters.sort = criterion;
  applyReplayRecordFilters();
  refreshReplayFilterSortButtons();
}

bool MainMenuScene::replayScoreRankFilterAvailable() const {
  return replay_record_filters::supportsScoreRankFilter(
      resultRecordSummaries);
}

bool MainMenuScene::selectedReplayIsAutoPlay() const {
  if (replayExportOptionsContent != nullptr &&
      replayExportOptionsContent->getVisible() &&
      replayExportSelection.has_value()) {
    return replayExportSelection->autoPlay;
  }
  return selectedResultRecordSummary.has_value() &&
         selectedResultRecordSummary->autoPlay;
}

bool MainMenuScene::selectedReplayIsCourseReplay() const {
  if (replayExportOptionsContent != nullptr &&
      replayExportOptionsContent->getVisible() &&
      replayExportSelection.has_value()) {
    return replayExportSelection->courseReplay;
  }
  return selectedResultRecordSummary.has_value() &&
         selectedResultRecordSummary->course;
}

bms_parser::ChartMeta
MainMenuScene::replayLoadMetaForRecord(const ChartMetaRecord &record) const {
  bms_parser::ChartMeta meta = record.meta;
  if (normalizeChartLongNoteModeValue(meta.LnMode) == 0) {
    meta.LnMode = long_note_mode::valueFromId(profileSelections.longNoteMode);
  }
  return meta;
}

ReplaySummary
MainMenuScene::autoPlayReplaySummary(const ChartMetaRecord &record) const {
  std::optional<std::string> playOption;
  if (!play_options::isNormalPlayOption(profileSelections.playOption)) {
    playOption = profileSelections.playOption;
  }
  return replay_autoplay::BuildSummary(
      replayLoadMetaForRecord(record), profileSelections.gaugeType,
      profileSelections.gaugeAutoShift, playOption, std::nullopt, std::nullopt,
      std::nullopt, profileSelections.assistOption,
      {.percent = context.settings.selectedPlaybackRatePercent,
       .mode = context.settings.selectedPlaybackMode},
      profileSelections.ruleset);
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

  playInfo = play_options::applySelectedPlayOptions(
      *preparedChart, profileSelections.playOption);
  applyEffectiveLongNoteModeToChart(
      *preparedChart,
      long_note_mode::valueFromId(profileSelections.longNoteMode));
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
  cancelActivePreviewLoading();
  if (replayWatchButtonText != nullptr) {
    replayWatchButtonText->setText("Loading...");
  }
  const std::string pacemakerTarget =
      pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
  const audio::PlaybackRate autoPlayPlayback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode,
  };
  const GameplayRuleset autoPlayRuleset = profileSelections.ruleset;
  defer(
      [this, record, replayId, pacemakerTarget, autoPlayPlayback,
       autoPlayRuleset]() {
        auto failReplayLoad = [this]() {
          resetReplayWatchLoadingUi();
          return true;
        };
        if (loadThread.joinable()) {
          loadThread.join();
        }
        joinRetiredPreviewLoadThreads();

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
          changeToGameplayScene(
              chart, {
                         .startPosition = 0,
                         .autoKeySound = true,
                         .autoPlay = true,
                         .gaugeType = profileSelections.gaugeType,
                         .gaugeAutoShift = profileSelections.gaugeAutoShift,
                         .gaugeAutoShiftLowerBound =
                             profileSelections.gaugeAutoShiftLowerBound,
                         .playOption = playInfo.option,
                         .playOptionSeed = playInfo.seed,
                         .playOption2 = playInfo.option2,
                         .playOption2Seed = playInfo.seed2,
                         .longNoteMode = long_note_mode::valueFromId(
                             profileSelections.longNoteMode),
                         .assistOption = profileSelections.assistOption,
                         .pacemakerTarget = pacemaker::kTargetOff,
                         .playback = autoPlayPlayback,
                         .touchVisualizationEnabled = false,
                         .replayGhostRenderingEnabled = false,
                         .ruleset = autoPlayRuleset,
                     });
          willStart.store(false);
          return true;
        }

        auto replay = context.replayRepository.LoadReplay(
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
        StartOptions replayOptions{
            .startPosition = 0,
            .autoKeySound = false,
            .autoPlay = false,
            .gaugeType = replayData->initialGaugeType,
            .gaugeAutoShift = replayData->gaugeAutoShift,
            .replayData = replayData,
            .pacemakerTarget = pacemakerTarget,
            .touchVisualizationEnabled = selectedReplayRenderTouchPoints,
            .replayGhostRenderingEnabled = selectedReplayRenderGhosts,
        };
        applyReplayProvenanceToStartOptions(replayOptions, *replayData);
        context.jukebox.stop();
        hideReplayModal();
        changeToGameplayScene(chart, std::move(replayOptions));
        willStart.store(false);
        return true;
      },
      0, true);
}

void MainMenuScene::startGBattlePlayback(const ChartMetaRecord &record,
                                         int replayId) {
  if (record.courseStart || replay_autoplay::isAutoPlayReplayId(replayId) ||
      willStart.load()) {
    return;
  }

  willStart.store(true);
  cancelActivePreviewLoading();
  if (replayGBattleButtonText != nullptr) {
    replayGBattleButtonText->setText("Loading...");
  }

  const GaugeType gaugeType = profileSelections.gaugeType;
  const GaugeAutoShiftMode gaugeAutoShift = profileSelections.gaugeAutoShift;
  const GaugeType gaugeAutoShiftLowerBound =
      profileSelections.gaugeAutoShiftLowerBound;
  const bool autoKeySound = !context.settings.inputKeysoundEnabled;
  const GameplayRuleset ruleset = profileSelections.ruleset;
  const audio::PlaybackRate playback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode,
  };

  defer(
      [this, record, replayId, gaugeType, gaugeAutoShift,
       gaugeAutoShiftLowerBound, autoKeySound, ruleset, playback]() {
        auto failGBattleLoad = [this]() {
          resetReplayWatchLoadingUi();
          return true;
        };
        if (loadThread.joinable()) {
          loadThread.join();
        }
        joinRetiredPreviewLoadThreads();

        auto replay = context.replayRepository.LoadReplay(
            replayId, replayLoadMetaForRecord(record));
        if (!replay.has_value() || replay->autoPlay) {
          resetReplayWatchLoadingUi();
          refreshReplayAvailability(&record);
          return true;
        }

        std::atomic_bool parseCancelled = false;
        auto preparedChart = play_options::prepareReplayChart(
            record.meta.BmsPath, replay.value(), parseCancelled);
        if (preparedChart == nullptr || parseCancelled) {
          return failGBattleLoad();
        }

        context.jukebox.stop();
        context.jukebox.loadChart(*preparedChart, true, parseCancelled);
        if (parseCancelled) {
          return failGBattleLoad();
        }

        auto *chart = setSelectedChart(std::move(preparedChart), true, false);
        if (chart == nullptr) {
          return failGBattleLoad();
        }

        auto recordData =
            std::make_shared<ReplayData>(std::move(replay.value()));
        context.jukebox.stop();
        hideReplayModal();
        changeToGameplayScene(
            chart, {
                       .startPosition = 0,
                       .autoKeySound = autoKeySound,
                       .autoPlay = false,
                       .gaugeType = gaugeType,
                       .gaugeAutoShift = gaugeAutoShift,
                       .gaugeAutoShiftLowerBound = gaugeAutoShiftLowerBound,
                       .gbattleRecordData = recordData,
                       .playOption = recordData->playOption,
                       .playOptionSeed = recordData->playOptionSeed,
                       .playOption2 = recordData->playOption2,
                       .playOption2Seed = recordData->playOption2Seed,
                       .longNoteMode = normalizeChartLongNoteModeValue(
                           recordData->chartMeta.LnMode),
                       .assistOption = recordData->assistOption,
                       .pacemakerTarget = pacemaker::kTargetOff,
                       .playback = playback,
                       .replayGhostRenderingEnabled = false,
                       .ruleset = ruleset,
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
  cancelActivePreviewLoading();
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
        joinRetiredPreviewLoadThreads();

        auto replay = context.replayRepository.LoadCourseReplay(replayId);
        if (!replay.has_value() || replay->stages.empty()) {
          return failReplayLoad();
        }

        auto replayData =
            std::make_shared<CourseReplayData>(std::move(*replay));
        auto session = std::make_shared<CoursePlaySession>();
        session->courseId = replayData->courseId;
        session->courseKey = replayData->courseKey;
        session->courseName = replayData->courseName;
        session->courseGroupName = replayData->courseGroupName;
        session->constraintJson = replayData->constraintJson;
        session->entries.reserve(replayData->stages.size());
        for (const auto &stage : replayData->stages) {
          session->entries.push_back(CoursePlayEntry{.meta = stage.replay.chartMeta});
        }
        session->snapshotRulesetFromReplay(replayData->stages.front().replay);
        const CourseConstraintSettings constraintSettings =
            courseConstraintSettingsFromJson(replayData->constraintJson);
        session->currentIndex = 0;
        session->gaugeType = replayData->initialGaugeType;
        session->gaugeProfile = replayData->gaugeProfile;
        session->gaugeAutoShift = replayData->gaugeAutoShift;
        session->gaugeAutoShiftLowerBound =
            replayData->gaugeAutoShiftLowerBound;
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

void MainMenuScene::schedulePreviewLoad(bms_parser::ChartMeta meta) {
  {
    std::lock_guard<std::mutex> lock(retiredPreviewLoadThreadsMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
  }
  previewLoadDebouncer.schedule(
      kPreviewDebounceDelay,
      [this, meta = std::move(meta)](const DebounceToken &previewToken) {
        if (willStart.load() || previewToken.cancelled()) {
          return;
        }
        startPreviewLoadThread(meta, previewToken);
      });
}

void MainMenuScene::startPreviewLoadThread(
    bms_parser::ChartMeta meta, DebounceToken previewToken) {
  if (loadThread.joinable()) {
    return;
  }

  auto cancelToken = std::make_shared<std::atomic_bool>(false);
  auto finishedToken = std::make_shared<std::atomic_bool>(false);
  previewLoadCancelToken = cancelToken;
  previewLoadFinishedToken = finishedToken;
  loadThread = std::thread([this, meta = std::move(meta), cancelToken,
                            finishedToken, previewToken]() {
    auto markFinished = makeScopeExit([finishedToken]() {
      finishedToken->store(true, std::memory_order_release);
    });
    auto isCancelled = [cancelToken, previewToken]() {
      return cancelToken->load(std::memory_order_relaxed) ||
             previewToken.cancelled();
    };
    SDL_Log("Previewing %s", fspath_to_utf8(meta.BmsPath).c_str());

    {
      std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
      if (isCancelled()) {
        return;
      }
      context.jukebox.stop();
    }
    SDL_Log("Parsing %s", fspath_to_utf8(meta.BmsPath).c_str());
    std::unique_ptr<bms_parser::Chart> chart;
    try {
      chart = play_options::parseChart(meta.BmsPath, *cancelToken, "preview");
    } catch (const std::exception &e) {
      SDL_Log("Preview parse failed %s: %s",
              fspath_to_utf8(meta.BmsPath).c_str(), e.what());
      archive_file::appendDebugLogLine(
          "Preview parse exception: " + fspath_to_utf8(meta.BmsPath) + ": " +
          e.what());
      return;
    }
    if (isCancelled()) {
      return;
    }
    SDL_Log("Parsed %s", fspath_to_utf8(meta.BmsPath).c_str());
    if (chart == nullptr) {
      SDL_Log("Chart is null");
      archive_file::appendDebugLogLine("Preview chart is null: " +
                                       fspath_to_utf8(meta.BmsPath));
      return;
    }

    {
      std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
      if (isCancelled()) {
        return;
      }
      if (context.jukebox.hasLoadedResources()) {
        context.jukebox.reloadChartResources(*chart, true, *cancelToken);
      } else {
        context.jukebox.loadChart(*chart, true, *cancelToken);
      }
      if (isCancelled()) {
        return;
      }
      setSelectedChart(std::move(chart), true);
      if (isCancelled()) {
        clearSelectedChart();
        return;
      }
      if (!willStart.load()) {
        context.jukebox.play();
      }
    }
  });
}

void MainMenuScene::cancelActivePreviewLoading() {
  previewLoadDebouncer.cancel();
  if (previewLoadCancelToken != nullptr) {
    previewLoadCancelToken->store(true, std::memory_order_release);
  }
}

void MainMenuScene::retirePreviewLoadThread(bool stopPreviewAudioWhenDone) {
  cancelActivePreviewLoading();
  {
    std::lock_guard<std::mutex> lock(retiredPreviewLoadThreadsMutex);
    if (stopPreviewAudioWhenDone) {
      pendingStopAndClearSelectedChartAfterPreview = true;
    }
    if (loadThread.joinable()) {
      SDL_Log("Retiring preview thread");
      retiredPreviewLoadThreads.push_back(RetiredPreviewLoadThread{
          .thread = std::move(loadThread),
          .finished = previewLoadFinishedToken != nullptr
                          ? previewLoadFinishedToken
                          : std::make_shared<std::atomic_bool>(true),
      });
    }
  }
  previewLoadCancelToken = std::make_shared<std::atomic_bool>(true);
  previewLoadFinishedToken = std::make_shared<std::atomic_bool>(true);
  reapRetiredPreviewLoadThreads();
}

void MainMenuScene::reapRetiredPreviewLoadThreads() {
  std::vector<RetiredPreviewLoadThread> finishedThreads;
  bool shouldStopPreviewAudio = false;
  {
    std::lock_guard<std::mutex> lock(retiredPreviewLoadThreadsMutex);
    auto it = retiredPreviewLoadThreads.begin();
    while (it != retiredPreviewLoadThreads.end()) {
      const bool finished =
          it->finished == nullptr ||
          it->finished->load(std::memory_order_acquire);
      if (!finished) {
        ++it;
        continue;
      }
      finishedThreads.push_back(std::move(*it));
      it = retiredPreviewLoadThreads.erase(it);
    }

    if (pendingStopAndClearSelectedChartAfterPreview &&
        retiredPreviewLoadThreads.empty()) {
      pendingStopAndClearSelectedChartAfterPreview = false;
      shouldStopPreviewAudio = true;
    }
  }

  for (auto &retiredThread : finishedThreads) {
    if (retiredThread.thread.joinable()) {
      SDL_Log("Joining finished preview thread");
      retiredThread.thread.join();
    }
  }
  if (shouldStopPreviewAudio) {
    stopAndClearSelectedChart();
  }
}

void MainMenuScene::joinRetiredPreviewLoadThreads() {
  std::vector<RetiredPreviewLoadThread> retiredThreads;
  bool shouldStopPreviewAudio = false;
  {
    std::lock_guard<std::mutex> lock(retiredPreviewLoadThreadsMutex);
    retiredThreads.swap(retiredPreviewLoadThreads);
    if (pendingStopAndClearSelectedChartAfterPreview) {
      pendingStopAndClearSelectedChartAfterPreview = false;
      shouldStopPreviewAudio = true;
    }
  }

  for (auto &retiredThread : retiredThreads) {
    if (retiredThread.thread.joinable()) {
      SDL_Log("Joining retired preview thread");
      retiredThread.thread.join();
    }
  }
  if (shouldStopPreviewAudio) {
    stopAndClearSelectedChart();
  }
}

void MainMenuScene::cancelPreviewLoading(bool stopPreviewAudio) {
  cancelActivePreviewLoading();
  if (loadThread.joinable()) {
    SDL_Log("Joining preview thread");
    loadThread.join();
  }
  joinRetiredPreviewLoadThreads();
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
  if (replayGBattleButtonText != nullptr) {
    replayGBattleButtonText->setText("G-BATTLE");
  }
}

void MainMenuScene::changeToGameplayScene(bms_parser::Chart *chart,
                                          StartOptions options) {
  if (options.replayData == nullptr) {
    options.clubMode = context.settings.gameplayClubModeEnabled;
  }
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
  cancelActivePreviewLoading();
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
  const GaugeType autoPlayGaugeType = profileSelections.gaugeType;
  const GaugeAutoShiftMode autoPlayGaugeAutoShift =
      profileSelections.gaugeAutoShift;
  const GaugeType autoPlayGaugeAutoShiftLowerBound =
      profileSelections.gaugeAutoShiftLowerBound;
  const std::string autoPlayAssistOption = profileSelections.assistOption;
  const std::string autoPlayOption = profileSelections.playOption;
  const audio::PlaybackRate autoPlayPlayback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode,
  };
  const bool autoPlayClubMode = context.settings.gameplayClubModeEnabled;
  const GameplayRuleset autoPlayRuleset = profileSelections.ruleset;
  const int autoPlayLongNoteMode =
      long_note_mode::valueFromId(profileSelections.longNoteMode);
  const SelectedChartRandomInfo autoPlayRandomInfo =
      selectedChartRandomInfoForPath(record.meta.BmsPath);

  auto runExport = [this, record, replayId, options, complete,
                    autoPlayGaugeType, autoPlayGaugeAutoShift,
                    autoPlayGaugeAutoShiftLowerBound,
                    autoPlayAssistOption, autoPlayOption, autoPlayPlayback,
                    autoPlayClubMode, autoPlayRuleset, autoPlayLongNoteMode,
                    autoPlayRandomInfo](const std::stop_token *stopToken) {
    try {
      if (loadThread.joinable()) {
        loadThread.join();
      }
      joinRetiredPreviewLoadThreads();
      context.jukebox.stop();
      if (stopToken != nullptr && stopToken->stop_requested()) {
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }

      if (record.courseStart) {
        auto replay = context.replayRepository.LoadCourseReplay(replayId);
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
            autoPlayPlayback, playInfo.option, playInfo.seed, playInfo.option2,
            playInfo.seed2, autoPlayAssistOption, autoPlayClubMode,
            autoPlayGaugeAutoShiftLowerBound, autoPlayRuleset);
        ReplayVideoExportOptions exportOptions = options;
        exportOptions.renderTouchPoints = false;
        exportOptions.renderReplayGhosts = false;
        exportOptions.pacemakerTarget = pacemaker::kTargetOff;
        complete(ReplayVideoExporter::Export(context, chart.get(), replay,
                                             exportOptions));
        return;
      }

      auto replay = context.replayRepository.LoadReplay(
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

std::optional<std::string>
MainMenuScene::activeReplayIrServerOrigin() const {
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (settings == context.settings.irProviders.end()) {
    return std::nullopt;
  }
  return ir::normalizeServerOrigin(settings->second.serverOrigin);
}

void MainMenuScene::publishReplayIrStatusFeedback(
    ir::IrRecordState state) {
  const char *message = nullptr;
  switch (state) {
  case ir::IrRecordState::Queued:
    message = "IR upload is queued.";
    break;
  case ir::IrRecordState::Uploading:
    message = "IR upload is in progress.";
    break;
  case ir::IrRecordState::AwaitingRemote:
    message = "IR is awaiting the remote result.";
    break;
  case ir::IrRecordState::Blocked:
    message = "IR upload is blocked. Check Settings > IR.";
    break;
  case ir::IrRecordState::Uploaded:
    message = "IR upload is complete.";
    break;
  case ir::IrRecordState::Hidden:
  case ir::IrRecordState::Eligible:
  case ir::IrRecordState::Failed:
    return;
  }

  if (replayModalTitleText != nullptr) {
    replayModalTitleText->setText(message);
  }
  const std::uint64_t feedbackRevision = ++replayIrUploadFeedbackRevision;
  defer(
      [this, feedbackRevision]() {
        if (feedbackRevision != replayIrUploadFeedbackRevision ||
            replayIrUploadInProgress) {
          return true;
        }
        if (replayModalRoot != nullptr && replayModalRoot->getVisible() &&
            replayListContent != nullptr && replayListContent->getVisible() &&
            replayModalTitleText != nullptr) {
          replayModalTitleText->setText("Records");
          replayModalRoot->applyYogaLayoutFromRoot();
        }
        return true;
      },
      1400, true);
}

void MainMenuScene::observeReplayIrServiceRevisions() {
  if (replayModalRoot == nullptr || !replayModalRoot->getVisible() ||
      replayListContent == nullptr || !replayListContent->getVisible() ||
      context.irSubmissionService == nullptr) {
    return;
  }

  for (const ReplaySummary &summary : replaySummaries) {
    if (summary.autoPlay || summary.courseReplay ||
        !summary.attemptId.has_value()) {
      continue;
    }
    const auto status = context.irSubmissionService->status(
        ir::kTachiProviderId, *summary.attemptId);
    const auto observed =
        replayIrObservedRevisions.find(*summary.attemptId);
    if (observed != replayIrObservedRevisions.end() &&
        observed->second == status.revision) {
      continue;
    }
    replayIrObservedRevisions[*summary.attemptId] = status.revision;
    refreshReplayIrMarker(summary.id,
                          recordActivityFor(status.activeRequest));
  }
}

void MainMenuScene::startReplayIrUpload(const ChartMetaRecord &record,
                                        ReplaySummary summary) {
  const bool actionableState =
      summary.irRecordState == ir::IrRecordState::Eligible ||
      summary.irRecordState == ir::IrRecordState::Failed;
  if (replayIrUploadInProgress || replayResultRecallInProgress ||
      replayExportInProgress.load() || record.courseStart ||
      summary.autoPlay || summary.courseReplay || !actionableState) {
    return;
  }

  const auto providerSettings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (providerSettings == context.settings.irProviders.end() ||
      !providerSettings->second.enabled) {
    finishReplayIrUpload(
        summary.id, "Enable Bokutachi in Settings > IR before uploading.");
    return;
  }
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  if (driver == nullptr) {
    finishReplayIrUpload(summary.id, "Bokutachi IR is unavailable.");
    return;
  }
  const ir::IrDriverCapabilities capabilities = driver->capabilities();
  if (capabilities.readOnly || !capabilities.scoreSubmission) {
    finishReplayIrUpload(summary.id,
                         "Bokutachi score submission is unavailable.");
    return;
  }
  if (context.irSubmissionService == nullptr) {
    finishReplayIrUpload(summary.id,
                         "The IR submission service is unavailable.");
    return;
  }

  replayIrUploadInProgress = true;
  ++replayIrUploadFeedbackRevision;
  if (replayModalTitleText != nullptr) {
    replayModalTitleText->setText("Preparing IR...");
  }
  refreshReplayModalActions();
  cancelActivePreviewLoading();

  defer(
      [this, record, summary = std::move(summary)]() {
        try {
          if (loadThread.joinable()) {
            loadThread.join();
          }
          joinRetiredPreviewLoadThreads();

          auto stored = context.replayRepository.LoadReplayResult(
              summary.id, replayLoadMetaForRecord(record));
          std::atomic_bool cancelled = false;
          auto recalled = stored.has_value()
                              ? result_recall::BuildChartResult(
                                    std::move(*stored), cancelled)
                              : result_recall::ChartBuildOutcome{};
          if (!recalled.value.has_value() ||
              !recalled.value->historicalIr.has_value() ||
              recalled.value->historicalIr->submission == nullptr) {
            finishReplayIrUpload(
                summary.id,
                "This saved result could not be verified for IR.");
            return true;
          }

          const ir::IrSavedResultUploadDependencies dependencies{
              .loadOutbox =
                  [this](std::string_view provider,
                         std::string_view attempt) {
                    return context.replayRepository.LoadIrOutbox(provider,
                                                                 attempt);
                  },
              .buildDraft = [this](const ir::IrSubmission &submission) {
                return context.irDrivers.buildDraft(ir::kTachiProviderId,
                                                    submission);
              },
              .enqueue = [this](const ir::IrOutboxDraft &draft) {
                return context.irSubmissionService->enqueueManual(draft);
              },
              .retry = [this](std::int64_t rowId) {
                return context.irSubmissionService->retry(rowId);
              },
          };
          const auto action = ir::executeIrSavedResultUpload(
              ir::kTachiProviderId,
              *recalled.value->historicalIr->submission, dependencies);
          finishReplayIrUpload(summary.id, action.message);
        } catch (const std::exception &) {
          finishReplayIrUpload(summary.id,
                               "IR upload could not be prepared.");
        } catch (...) {
          finishReplayIrUpload(summary.id,
                               "IR upload could not be prepared.");
        }
        return true;
      },
      1, true);
}

void MainMenuScene::finishReplayIrUpload(int replayId, std::string message) {
  replayIrUploadInProgress = false;
  refreshReplayIrMarker(replayId);

  std::string safeMessage = ir::sanitizeDiagnostic(message);
  if (safeMessage.empty()) {
    safeMessage = "IR upload could not be queued.";
  }
  if (replayModalTitleText != nullptr) {
    replayModalTitleText->setText(safeMessage);
  }
  refreshReplayModalActions();

  const std::uint64_t feedbackRevision = ++replayIrUploadFeedbackRevision;
  defer(
      [this, feedbackRevision]() {
        if (feedbackRevision != replayIrUploadFeedbackRevision ||
            replayIrUploadInProgress) {
          return true;
        }
        if (replayModalRoot != nullptr && replayModalRoot->getVisible() &&
            replayListContent != nullptr && replayListContent->getVisible() &&
            replayModalTitleText != nullptr) {
          replayModalTitleText->setText("Records");
          replayModalRoot->applyYogaLayoutFromRoot();
        }
        return true;
      },
      1400, true);
}

void MainMenuScene::refreshReplayIrMarker(
    int replayId, ir::IrRecordActivity activity) {
  auto summary = std::find_if(
      replaySummaries.begin(), replaySummaries.end(),
      [replayId](const ReplaySummary &candidate) {
        return candidate.id == replayId;
      });
  if (summary == replaySummaries.end() || replayModalChart.courseStart) {
    return;
  }

  const std::string irServerOrigin =
      activeReplayIrServerOrigin().value_or(std::string{});
  auto latestSummaries = context.replayRepository.ListReplays(
      replayModalChart.meta, 0, ir::kTachiProviderId, irServerOrigin);
  auto latest = std::find_if(
      latestSummaries.begin(), latestSummaries.end(),
      [replayId](const ReplaySummary &candidate) {
        return candidate.id == replayId;
      });
  if (latest == latestSummaries.end()) {
    return;
  }

  const std::optional<std::string> preferredStableKey =
      selectedResultRecordStableKey;
  const float previousScrollOffset =
      replayListView != nullptr ? replayListView->scrollOffset : 0.0F;
  ir::resolveReplayIrRecordState(*latest, activity);
  *summary = std::move(*latest);
  auto resultRecord = std::ranges::find_if(
      resultRecordSummaries, [replayId](const ResultRecordSummary &candidate) {
        return candidate.localReplayId() == replayId;
      });
  if (resultRecord != resultRecordSummaries.end()) {
    *resultRecord = makeLocalResultRecord(*summary);
  }
  applyReplayRecordFilters(preferredStableKey);
  if (replayListView != nullptr) {
    replayListView->scrollOffset = previousScrollOffset;
    replayListView->rebindVisibleItems();
  }
}

void MainMenuScene::startReplayResultRecall(const ChartMetaRecord &record,
                                            int replayId) {
  if (replayResultRecallInProgress || replayExportInProgress.load() ||
      replayIrUploadInProgress ||
      replay_autoplay::isAutoPlayReplayId(replayId)) {
    return;
  }

  replayResultRecallInProgress = true;
  refreshReplayModalActions();
  cancelActivePreviewLoading();
  defer(
      [this, record, replayId]() {
        if (loadThread.joinable()) {
          loadThread.join();
        }
        joinRetiredPreviewLoadThreads();
        context.jukebox.stop();

        if (record.courseStart) {
          startCourseReplayResultRecall(replayId);
          return true;
        }

        auto stored = context.replayRepository.LoadReplayResult(
            replayId, replayLoadMetaForRecord(record));
        std::atomic_bool cancelled = false;
        auto recalled = stored.has_value()
                            ? result_recall::BuildChartResult(
                                  std::move(*stored), cancelled)
                            : result_recall::ChartBuildOutcome{};
        if (!recalled.value.has_value()) {
          finishReplayResultRecallFailure(
              recalled.diagnostic.empty() ? "saved replay was not found"
                                          : recalled.diagnostic);
          return true;
        }

        auto result = std::move(*recalled.value);
        ResultPersistenceOptions persistence;
        if (result.historicalIr.has_value()) {
          persistence.attempt = result.historicalIr->attempt;
          persistence.irSubmission = result.historicalIr->submission;
          persistence.outcome = result.historicalIr->saveOutcome;
        }
        const ReplayData replay = result.replay;
        auto chart = std::move(result.chart);
        const bms_parser::ChartMeta meta = chart->Meta;
        replayResultRecallInProgress = false;
        context.sceneManager->changeScene(
            std::make_unique<ResultScene>(
                context, meta, result.state, replay.provenance, nullptr,
                std::move(persistence), &replay, ResultPracticeOptions{},
                false, ResultCourseOptions{},
                profileSelections.pacemakerTarget, std::move(chart)),
            true);
        return true;
      },
      1, true);
}

void MainMenuScene::startRemoteResultRecall(IrRemoteRecordId identity,
                                            std::string selectedStableKey) {
  if (replayResultRecallInProgress || replayExportInProgress.load() ||
      replayIrUploadInProgress || identity.providerId.empty() ||
      identity.serverOrigin.empty() || identity.remoteScoreId.empty() ||
      selectedStableKey.empty()) {
    return;
  }
  replayResultRecallInProgress = true;
  refreshReplayModalActions();
  cancelActivePreviewLoading();
  defer(
      [this, identity = std::move(identity),
       selectedStableKey = std::move(selectedStableKey)]() {
        if (loadThread.joinable()) {
          loadThread.join();
        }
        joinRetiredPreviewLoadThreads();

        RemoteResultRecallRequest request{
            .identity = std::move(identity),
            .selectedStableKey = std::move(selectedStableKey),
        };
        RemoteResultRecallCallbacks callbacks{
            .selectionStillMatches = [this](const auto &candidate) {
              return remoteResultRecallSelectionMatches(
                  selectedResultRecordSummary, candidate);
            },
            .loadExact = [this](const IrRemoteRecordId &candidate) {
              return context.replayRepository.LoadIrRemoteScore(
                  candidate.providerId, candidate.serverOrigin,
                  candidate.remoteScoreId);
            },
            .transition = [this](ResultRemoteOptions remote,
                                 bool retainCurrentScene) {
              auto next =
                  std::make_unique<ResultScene>(context, std::move(remote));
              replayResultRecallInProgress = false;
              context.jukebox.stop();
              context.sceneManager->changeScene(std::move(next),
                                                retainCurrentScene);
              return true;
            },
            .failAndReload = [this](std::string diagnostic) {
              finishRemoteResultRecallFailure(std::move(diagnostic));
            },
        };
        (void)executeRemoteResultRecall(request, callbacks);
        return true;
      },
      1, true);
}

void MainMenuScene::startCourseReplayResultRecall(int replayId) {
  auto stored = context.replayRepository.LoadCourseReplay(replayId);
  std::atomic_bool cancelled = false;
  auto recalled = stored.has_value()
                      ? result_recall::BuildCourseResult(std::move(*stored),
                                                        cancelled)
                      : result_recall::CourseBuildOutcome{};
  if (!recalled.value.has_value() || recalled.value->session == nullptr ||
      recalled.value->session->completedResults.empty() ||
      recalled.value->session->courseReplayData == nullptr) {
    finishReplayResultRecallFailure(
        recalled.diagnostic.empty() ? "saved course replay was not found"
                                    : recalled.diagnostic);
    return;
  }

  auto session = std::move(recalled.value->session);
  session->currentIndex = 0;
  const auto &result = session->completedResults.front();
  const auto &replay = session->courseReplayData->stages.front().replay;
  session->applyReplayStagePlayOptions(replay);
  replayResultRecallInProgress = false;
  context.sceneManager->changeScene(
      std::make_unique<ResultScene>(
          context, result.meta, result.state, replay.provenance, nullptr,
          ResultPersistenceOptions{}, nullptr, ResultPracticeOptions{}, false,
          ResultCourseOptions{.mode = ResultCourseMode::Stage,
                              .session = session,
                              .savedResultBrowsing = true}),
      true);
}

void MainMenuScene::finishReplayResultRecallFailure(std::string diagnostic) {
  const std::string safeDiagnostic = ir::sanitizeDiagnostic(diagnostic);
  SDL_Log("Saved result recall failed: %s",
          safeDiagnostic.empty() ? "result unavailable"
                                 : safeDiagnostic.c_str());
  if (replayModalResultButtonText != nullptr) {
    replayModalResultButtonText->setText("Result Unavailable");
  }
  if (replayModalRoot != nullptr) {
    replayModalRoot->applyYogaLayoutFromRoot();
  }
  defer(
      [this]() {
        replayResultRecallInProgress = false;
        refreshReplayModalActions();
        return true;
      },
      1400, true);
}

void MainMenuScene::finishRemoteResultRecallFailure(std::string diagnostic) {
  reloadReplayRecordModels(true);
  finishReplayResultRecallFailure(std::move(diagnostic));
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
    replayModalTitleText->setText("Records");
    replayExportProgressContent->setVisible(false);
    replayExportOptionsContent->setVisible(false);
    replayListContent->setVisible(true);
    replayExportSelection.reset();
    if (replayListView != nullptr) {
      replayListView->restoreSelection(selectedReplayIndex);
    }
    refreshReplayModalActions();
  }

  if (result->success) {
    SDL_Log("Replay video exported: %s (%s)",
            fspath_to_utf8(result->outputPath).c_str(),
            result->message.c_str());
  } else {
    SDL_Log("Replay video export failed: %s (%s)", result->message.c_str(),
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
  previewLoadDebouncer.update();
  reapRetiredPreviewLoadThreads();
  refreshScoreClearRanksIfNeeded();
  refreshIrRecordListIfNeeded();
  refreshTasksButton();
  applyPendingUiUpdates();
  applyFindBmsUpdates();
  applyUnzipProgress();
  applyUnzipResult();
  applyReplayExportProgress();
  applyReplayExportResult();
  observeReplayIrServiceRevisions();
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
  if (rankingsModal) {
    rankingsModal->update();
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
  if (overlayPortal != nullptr) {
    overlayPortal->setSize(rendering::window_width, rendering::window_height);
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
  rankingsModal.reset();
  cancelActivePreviewLoading();
  context.profileSwitchBlockers.scene = nullptr;
  context.profileSwitchBlockers.background = nullptr;
  context.refreshProfileCaches = nullptr;
  context.requestAddChartFolderFromFiles = nullptr;
  context.requestRebuildChartLibrary = nullptr;
  context.notifyBackgroundTaskPauseStateChanged = nullptr;
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
  joinRetiredPreviewLoadThreads();
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  ClearIOSFolderAccess();
#endif
  stopAndClearSelectedChart();
  selectedChartRecord.reset();
  chartListCache.clear();
  chartListCache.session = nullptr;
  chartSession.reset();
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  rootLayout = nullptr;
  overlayPortal = nullptr;
  jacketView = nullptr;
  searchBox = nullptr;
  startButton = nullptr;
  rankingsButton = nullptr;
  rankingsButtonText = nullptr;
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
  irUploadsButton = nullptr;
  irUploadsButtonText = nullptr;
  tasksButton = nullptr;
  tasksButtonText = nullptr;
  replayButtonText = nullptr;
  replayStatusText = nullptr;
  replayModalRoot = nullptr;
  replayModalContentFrame = nullptr;
  replayListContent = nullptr;
  replayFilterSortContent = nullptr;
  replayWatchOptionsContent = nullptr;
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
  parseLogRecyclerView = nullptr;
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
  findBmsCloseButton = nullptr;
  findBmsOpenButton = nullptr;
  findBmsGoogleButton = nullptr;
  findBmsRefreshButton = nullptr;
  findBmsCloseButtonText = nullptr;
  findBmsOpenButtonText = nullptr;
  findBmsGoogleButtonText = nullptr;
  findBmsRefreshButtonText = nullptr;
  readyGaugeText = nullptr;
  readyTotalRow = nullptr;
  readyTotalIconText = nullptr;
  readyTotalText = nullptr;
  readyPlayOptionText = nullptr;
  readyAssistOptionText = nullptr;
  readyPacemakerText = nullptr;
  readyPlayOptionsButton = nullptr;
  playOptionsCloseButton = nullptr;
  playOptionsCloseButtonText = nullptr;
  replayListView = nullptr;
  replayWatchButton = nullptr;
  replayGBattleButton = nullptr;
  replayModalResultButton = nullptr;
  replayModalExportButton = nullptr;
  replayModalFilterButton = nullptr;
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
  replayGBattleButtonText = nullptr;
  replayModalResultButtonText = nullptr;
  replayModalExportButtonText = nullptr;
  replayModalFilterButtonText = nullptr;
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
  replayResultRecallInProgress = false;
  replayIrUploadInProgress = false;
  replayIrObservedRevisions.clear();
  ++replayIrUploadFeedbackRevision;
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
  visibleResultRecordSummaries.clear();
  resultRecordSummaries.clear();
  selectedResultRecordStableKey.reset();
  publishedResultRecordDiagnostic.clear();
  replayRecordFilters = {};
  selectedReplayIndex = -1;
  selectedReplaySummary.reset();
  selectedResultRecordSummary.reset();
  replayExportSelection.reset();
  selectedExportFps = 120;
  selectedExportFullResolution = true;
  selectedExportIncludeResultScreen = true;
  selectedReplayRenderTouchPoints = true;
  selectedReplayRenderGhosts = true;
  replayExportProgressFraction = 0.0;
  playOptionsPanel = nullptr;
  replayClearFilterButtons.clear();
  replayPlayOptionFilterButtons.clear();
  replayScoreRankFilterButtons.clear();
  replaySortButtons.clear();
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
}

void MainMenuScene::LoadCharts(ChartRepository::Session &chartSession,
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
  ChartLibraryScanner scanner;
  const int changedCount = scanner.Scan(
      chartSession, roots, &stop_token, progressCallback, pauseCallback,
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
