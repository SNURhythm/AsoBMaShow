#include "MainMenuScene.h"
#include "../StartupTiming.h"
#include "MainMenuLibrary.h"
#include "../ArchiveFile.h"
#include "../BmsChartFile.h"
#include "../CourseConstraintUtils.h"
#include "../LongNoteModeUtils.h"
#include "../audio/MusicPlaylist.h"
#include "../tinyfiledialogs.h"
#include <fstream>
#include <algorithm>
#include "../repositories/ReplayRepository.h"
#include "../ReplayAutoPlay.h"
#include "../ReplayResultStateBuilder.h"
#include "../ReplayVideoExporter.h"
#include "../ModernResultRecallBuilder.h"
#include "../PlayOptionUtils.h"
#include "../ResultContracts.h"
#include "../ProfileDatabaseActivity.h"
#include "../PlatformDocumentHandoff.h"
#include "../PlatformOpen.h"
#include "../RAII.h"
#include "../repositories/ScoreCacheQueries.h"
#include "../repositories/SqliteRAII.h"
#include "../ir/tachi/TachiBatchManual.h"
#include "../ir/IrProfileSettings.h"
#include "../ir/IrSavedResultUpload.h"
#include "../ir/IrSubmissionService.h"
#include "../path.h"
#include "../replay/ChartReplayConsumer.h"
#include "../replay/CourseReplayConsumer.h"
#include "../replay/ReplayFileActionSelection.h"
#include "../replay/ReplayFileActionService.h"
#include "../view/ChartListItemView.h"
#include "../view/IconText.h"
#include "../view/LibraryFolderItemView.h"
#include "../view/OverlayPortal.h"
#include "ChartPreloadWorker.h"
#include "DecideLoadingOverlay.h"
#include "../view/PlayOptionsPanelView.h"
#include "../view/TextView.h"
#include "../view/TextInputBox.h"
#include "../Utils.h"
#include "../targets.h"
#include "../view/Button.h"
#include "../view/BlockingOverlayView.h"
#include "ChartViewerScene.h"
#include "CourseGameplaySessionBuilder.h"
#include "FindBmsDialogPolicy.h"
#include "FindBmsProgressPresentation.h"
#include "IrUploadsScene.h"
#include "MusicPlayerScene.h"
#include "RemoteResultRecallController.h"
#include "ResultScene.h"
#include "SettingsScene.h"
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
#include <unordered_map>
#include <unordered_set>
#include <vector>
#ifdef _WIN32
#include <windows.h>

#elif __APPLE__

#include "TargetConditionals.h"
#if TARGET_OS_IPHONE
#include "../iOSNatives.hpp"
// define something for iphone
#include <dirent.h>
#include <sys/stat.h>
#else
// define something for OSX
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
#include <limits>
#include <sstream>
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
constexpr uint32_t kIconShare = 0xf1e0;
constexpr uint32_t kIconTrash = 0xf1f8;

std::string replayDiagnosticOr(std::string_view diagnostic,
                               std::string_view fallback) {
  std::string result = ir::sanitizeDiagnostic(diagnostic);
  if (result.empty()) {
    result = ir::sanitizeDiagnostic(fallback);
  }
  return result;
}

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

std::string
findBmsProgressEventDisplayText(const BmsSearchDownloadProgress &progress,
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

using main_menu_library::folderKeyForCourse;
using main_menu_library::folderKeyForCourseGroup;
using main_menu_library::folderKeyForCourseTable;
using main_menu_library::folderKeyForLevel;
using main_menu_library::folderKeyForTable;

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

  button->setEnabled(enabled);
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
  case ChartScanProgressStage::IndexingArchives:
    return "Indexing archives";
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

EventHandleResult MainMenuScene::handleEvents(SDL_Event &event) {
  // While a chart is launching, the decide overlay blocks all input so the
  // user cannot start another chart or mutate the library mid-launch.
  if (willStart.load(std::memory_order_acquire) && decideOverlay_ != nullptr &&
      decideOverlay_->getVisible()) {
    (void)decideOverlay_->handleEvents(event);
    return {};
  }
  if (recordsModal_ != nullptr && recordsModal_->isVisible()) {
    (void)recordsModal_->handleEvents(event);
    return {};
  }
  return Scene::handleEvents(event);
}

void MainMenuScene::init() {
  // Initialize the scene
  chartSession =
      context.chartRepository.OpenSession(&context.scoreRepository);
  auto profileOperationBlocker = [this]() -> std::optional<std::string> {
    if (replayExportInProgress.load(std::memory_order_acquire)) {
      return "A replay export is active.";
    }
    if (unzipInProgress.load(std::memory_order_acquire) ||
        findBmsJobRunning.load(std::memory_order_acquire)) {
      return "A chart archive operation is active.";
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
  initView(context);
  SDL_Log("Main Menu Scene Initialized");
}

void MainMenuScene::onPause() {
  if (revealContextMenu != nullptr) {
    revealContextMenu->dismiss();
  }
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
  if (revealContextMenu != nullptr && !revealContextMenu->isOpen()) {
    revealContextMenu->propagateThemeChange();
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
  if (recordsModal_ != nullptr) recordsModal_->refresh();
  refreshFindBmsModal();
  refreshMusicModal();
  if (rootLayout != nullptr) {
    rootLayout->applyYogaLayout();
  }
}

void MainMenuScene::enqueueLibraryRefreshTask(
    const std::string &title, const std::filesystem::path &folderToAdd,
    const std::string &iosBookmark, bool rebuildLibraryMetadata) {
  if (!context.chartLibraryTasks) {
    return;
  }
  context.chartLibraryTasks->enqueue({
      .kind = chart_library_tasks::TaskKind::RefreshLibrary,
      .title = title,
      .folderToAdd = folderToAdd,
      .iosBookmark = iosBookmark,
      .rebuildLibraryMetadata = rebuildLibraryMetadata,
  });
}

void MainMenuScene::enqueueDownloadedPathIndexTask(
    const std::filesystem::path &path,
    const main_menu_library::FindBmsChartIdentity &targetIdentity,
    std::uint64_t selectionGeneration,
    std::vector<std::filesystem::path> removedPaths) {
  if (path.empty() || !context.chartLibraryTasks) {
    return;
  }
  context.chartLibraryTasks->enqueue({
      .kind = chart_library_tasks::TaskKind::IndexDownloadedPath,
      .title = "Index Downloaded BMS",
      .downloadedPath = path,
      .downloadedRemovedPaths = std::move(removedPaths),
      .downloadedTargetIdentity = targetIdentity,
      .downloadedSelectionGeneration = selectionGeneration,
  });
}

MainMenuScene::LibraryTaskProgressSnapshot
MainMenuScene::readLibraryTaskProgress() const {
  return context.chartLibraryTasks
             ? context.chartLibraryTasks->snapshot().progress
             : LibraryTaskProgressSnapshot{};
}

int MainMenuScene::activeLibraryTaskCount() {
  return context.chartLibraryTasks
             ? context.chartLibraryTasks->snapshot().activeCount
             : 0;
}

void MainMenuScene::requestLibraryScanFlush() {
  if (activeLibraryTaskCount() > 0) {
    context.chartLibraryScanFlushRequested.fetch_add(
        1, std::memory_order_release);
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

void MainMenuScene::initView(ApplicationContext &context) {
  // Initialize the view
  revealContextMenu.reset();
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  rootLayout = nullptr;
  overlayPortal = nullptr;
  revealButton = nullptr;
  temporaryChartFolder.reset();
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
  recordsModal_.reset();
  startButtonText = nullptr;
  playOptionsModalRoot = nullptr;
  playOptionsPanel = nullptr;
  playOptionsModal.reset();
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
  parseLogExportStatusText = nullptr;
  parseLogExportButton = nullptr;
  parseLogExportButtonText = nullptr;
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
  pendingReplayExportResult.reset();
  pendingReplayExportProgress.reset();
  pendingUnzipResult.reset();
  pendingUnzipProgress.reset();
  pendingSelectChartPath.reset();
  {
    std::lock_guard<std::mutex> lock(findBmsSelectionHandoffMutex);
    pendingFindBmsSelectionHandoff.reset();
  }
  suppressPreviewForChartPath.reset();
  unzipDeleteCandidatePath.reset();
  unzipEstimatedUncompressedSize = 0;
  pendingFindBmsProgressEvents.clear();
  pendingFindBmsResult.reset();
  chartSelectionGeneration = 0;
  findBmsSelectionGenerationAtDownloadStart = 0;
  replayExportInProgress = false;
  replayResultRecallInProgress = false;
  replayIrUploadInProgress = false;
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
  publishedResultRecordDiagnostic.clear();

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
  chart_library_platform::refreshFolderAccess(
      chartSession->SelectEffectiveEntries());

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
    chartListItemView->setMeta(item, prioritizeVisibleArtworkBindings);
    chartListItemView->setClearRank(clearRankForChart(item));
    if (!item.courseStart && !item.solidArchive && !item.unavailable &&
        !item.meta.BmsPath.empty()) {
      const auto bestScore = scoreBestScores.bestFor(
          item.meta,
          long_note_mode::valueFromId(profileSelections.longNoteMode));
      if (bestScore.has_value()) {
        const int fallbackMaxScore =
            result_contract::maximumScoreForNotes(item.meta.TotalNotes)
                .value_or(0);
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
    chartSelectionGeneration =
        main_menu_library::chartSelectionGenerationAfter(
            chartSelectionGeneration, selectedChartRecord, item);
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
      if (previewWorker_ != nullptr) {
        previewWorker_->cancel();
      }
      {
        std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
        pendingStopAndClearSelectedChartAfterPreview = true;
      }
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
    if (previewWorker_ != nullptr) {
      previewWorker_->cancel();
    }
    {
      std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
      pendingStopAndClearSelectedChartAfterPreview = true;
    }
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
    {
      std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
      pendingStopAndClearSelectedChartAfterPreview = false;
    }
    if (previewWorker_ != nullptr) {
      ChartMetaRecord previewRecord;
      previewRecord.meta = std::move(meta);
      previewWorker_->request(std::move(previewRecord));
    }
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
  decideOverlay_ = new DecideLoadingOverlay(
      0, 0, rendering::window_width, rendering::window_height, {});
  decideOverlay_->setVisible(false);
  overlayPortal->present(decideOverlay_);
  previewWorker_ = new ChartPreloadWorker(kPreviewDebounceDelay);
  previewWorker_->setOnIdle([this]() {
    bool shouldStopPreviewAudio = false;
    {
      std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
      if (pendingStopAndClearSelectedChartAfterPreview) {
        pendingStopAndClearSelectedChartAfterPreview = false;
        shouldStopPreviewAudio = true;
      }
    }
    if (shouldStopPreviewAudio) {
      stopAndClearSelectedChart();
    }
  });
  previewWorker_->configure(
      [this](const ChartMetaRecord &request, std::atomic_bool &cancelled) {
        const auto &meta = request.meta;
        const auto isCancelled = [&cancelled, this, &meta]() {
          return cancelled.load(std::memory_order_relaxed) ||
                 previewWorker_->superseded(
                     fspath_to_utf8(meta.BmsPath));
        };
        SDL_Log("Previewing %s", fspath_to_utf8(meta.BmsPath).c_str());
        {
          std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
          if (isCancelled()) {
            return;
          }
          this->context.jukebox.stop();
        }
        SDL_Log("Parsing %s", fspath_to_utf8(meta.BmsPath).c_str());
        std::unique_ptr<bms_parser::Chart> chart;
        try {
          chart = play_options::parseChart(meta.BmsPath, cancelled, "preview");
        } catch (const std::exception &e) {
          SDL_Log("Preview parse failed %s: %s",
                  fspath_to_utf8(meta.BmsPath).c_str(), e.what());
          archive_file::appendDebugLogLine(
              "Preview parse exception: " + fspath_to_utf8(meta.BmsPath) +
              ": " + e.what());
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
          if (this->context.jukebox.hasLoadedResources()) {
            this->context.jukebox.reloadChartResources(*chart, true, cancelled);
          } else {
            this->context.jukebox.loadChart(*chart, true, cancelled);
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
            this->context.jukebox.play();
          }
        }
      });

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
    addFolderButton->setOnClickListener([this]() {
      if (this->context.requestAddChartFolderFromFiles) {
        this->context.requestAddChartFolderFromFiles();
      }
    });
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
      [this]() {
        if (this->context.chartLibraryFolderActions) {
          this->context.chartLibraryFolderActions->requestImportArchive();
        }
      });
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
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      {
        std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
        pendingStopAndClearSelectedChartAfterPreview = false;
      }
      stopAndClearSelectedChart();
      context.sceneManager->changeScene(
          std::make_unique<MusicPlayerScene>(
              context, SceneReturnTarget::Retained(this)),
          true);
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
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      {
        std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
        pendingStopAndClearSelectedChartAfterPreview = false;
      }
      stopAndClearSelectedChart();
      context.sceneManager->changeScene(
          std::make_unique<IrUploadsScene>(
              context, SceneReturnTarget::Retained(this)),
          true);
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
  searchBox->setEditingText(searchText);
  searchBox->setClearable(true);
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

    openReplayRecordsForSelection();
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

  revealButton = new Button(0, 0, 105, 58);
  revealButton->setFlex(1);
  auto *revealButtonText = new TextView("assets/fonts/notosanscjkjp.ttf", 24);
  revealButtonText->setText("Reveal");
  revealButtonText->setAlign(TextView::CENTER);
  revealButtonText->setVAlign(TextView::MIDDLE);
  revealButton->setContentView(revealButtonText);
  styleThemedActionButton(revealButton, revealButtonText, true,
                          ui_theme::infoAction, ui_theme::infoActionHover,
                          ui_theme::infoActionPressed, ui_theme::accentBorder);
  revealButton->setOnClickListener([this]() { toggleRevealContextMenu(); });
  chartActionsRow->addView(revealButton);
  rightContent->addView(chartActionsRow);

  ContextMenuView::Callbacks revealMenuCallbacks;
  revealMenuCallbacks.onActionSelected = [this](const std::string &actionId) {
    if (actionId == "show-same-folder") {
      showSelectedChartFolder();
    } else if (actionId == "reveal-file") {
      revealSelectedChartInFileManager();
    }
  };
  revealContextMenu = std::make_unique<ContextMenuView>(
      overlayPortal, std::move(revealMenuCallbacks));

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
    if (previewWorker_ != nullptr) {
      previewWorker_->stop();
    }
    {
      std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
      pendingStopAndClearSelectedChartAfterPreview = false;
    }
    stopAndClearSelectedChart();
    context.sceneManager->changeScene(
        std::make_unique<SettingsScene>(
            context, SettingsDestination::Profile,
            SceneReturnTarget::Retained(this)),
        true);
  });
  rightScroll->setContentView(rightContent);
  right->addView(rightScroll);
  right->addView(settingsButton);
  rootLayout->addView(right);
  buildPlayOptionsModal();
  recordsModal_ = ReplayRecordsModal::Create(rootLayout, makeRecordsModalCallbacks());
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
  const bool sidebarFolderSelected = !temporaryChartFolder.has_value();
  folderRecyclerView->selectedIndex = sidebarFolderSelected ? activeIndex : -1;
  if (preserveViewState) {
    const float maxOffset =
        std::max(0.0f, static_cast<float>(std::max(1, folderCount) *
                                              folderRecyclerView->itemHeight -
                                          folderRecyclerView->getHeight()));
    folderRecyclerView->scrollOffset =
        std::clamp(previousScrollOffset, 0.0f, maxOffset);
    folderRecyclerView->rebindVisibleItems();
  }
  if (sidebarFolderSelected) {
    auto selectedView = folderRecyclerView->getViewByIndex(activeIndex);
    if (selectedView != nullptr) {
      selectedView->onSelected();
    }
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
  const bool sidebarFolderSelected = !temporaryChartFolder.has_value();
  folderRecyclerView->selectedIndex = sidebarFolderSelected ? activeIndex : -1;
  const float maxOffset =
      std::max(0.0f, static_cast<float>(std::max(1, folderCount) *
                                            folderRecyclerView->itemHeight -
                                        folderRecyclerView->getHeight()));
  folderRecyclerView->scrollOffset =
      std::clamp(previousScrollOffset, 0.0f, maxOffset);
  folderRecyclerView->rebindVisibleItems();
  if (sidebarFolderSelected) {
    auto selectedView = folderRecyclerView->getViewByIndex(activeIndex);
    if (selectedView != nullptr) {
      selectedView->onSelected();
    }
  }
}

ChartMetaQuery MainMenuScene::chartQueryForActiveFolder() const {
  const int selectedLongNoteMode =
      long_note_mode::valueFromId(profileSelections.longNoteMode);
  if (temporaryChartFolder.has_value()) {
    return main_menu_library::chartQueryForSameFolder(
        *temporaryChartFolder, searchText, chartRecordFilters,
        selectedLongNoteMode);
  }

  ChartMetaQuery query;
  query.keyword = searchText;
  query.selectedLongNoteMode = selectedLongNoteMode;

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
  if (temporaryChartFolder.has_value()) {
    return false;
  }
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
  const bool sameFolderScope = temporaryChartFolder.has_value();
  const bool difficultyRangeEnabled = chartDifficultyRangeEnabled();
  const auto levels = chartFilterDifficultyLevels();
  const std::optional<int> folderClearMarkRank =
      !sameFolderScope && activeFolder.clearMarkFolder
          ? std::optional<int>(activeFolder.clearMarkRank)
          : std::nullopt;
  if (!sameFolderScope && activeFolder.clearMarkFolder) {
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
    if (!sameFolderScope &&
        chartRecordFilters.sort.criterion ==
            ChartRecordSortCriterion::Difficulty) {
      chartRecordFilters.sort = {};
    }
  }

  if (chartFilterPanel != nullptr) {
    chartFilterPanel->refresh({
        .filters = chartRecordFilters,
        .bpmMinText = chartBpmMinText,
        .bpmMaxText = chartBpmMaxText,
        .clearMarkFilterVisible =
            sameFolderScope || !activeFolder.clearMarkFolder,
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
        .difficultySortEnabled = difficultyRangeEnabled || sameFolderScope,
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
  if (!temporaryChartFolder.has_value() && activeFolder.clearMarkFolder) {
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
          : (!temporaryChartFolder.has_value() && activeFolder.clearMarkFolder
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
  if (!temporaryChartFolder.has_value() && activeFolder.clearMarkFolder) {
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
          : (!temporaryChartFolder.has_value() && activeFolder.clearMarkFolder
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
      !chartDifficultyRangeEnabled() && !temporaryChartFolder.has_value()) {
    return;
  }
  chartRecordFilters.sort =
      chart_record_filters::nextSortState(chartRecordFilters.sort, criterion);
  reloadChartList();
  refreshChartFilterPanel();
}

void MainMenuScene::reloadChartListForFolderSelection() {
  prioritizeVisibleArtworkBindings = true;
  reloadChartList();
  prioritizeVisibleArtworkBindings = false;
}

void MainMenuScene::reloadChartList(bool preserveViewState) {
  if (recyclerView == nullptr || !chartSession.has_value()) {
    return;
  }

  const float previousScrollOffset =
      preserveViewState ? recyclerView->scrollOffset : 0.0f;
  const int previousSelectedIndex =
      preserveViewState ? recyclerView->selectedIndex : -1;
  std::filesystem::path visibleSelectedPath;
  if (preserveViewState && previousSelectedIndex >= 0 &&
      previousSelectedIndex < recyclerView->size()) {
    visibleSelectedPath =
        recyclerView->get(previousSelectedIndex).meta.BmsPath;
  }
  const path_t previousSelectedPath =
      preserveViewState
          ? fspath_to_path_t(main_menu_library::chartSelectionPathForReload(
                visibleSelectedPath, selectedChartRecord))
          : path_t{};
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
  if (!temporaryChartFolder.has_value() &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
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
  if (!temporaryChartFolder.has_value() && !selectedChartRecord.has_value() &&
      !preserveViewState &&
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
  if (recordsModal_ != nullptr && recordsModal_->isVisible()) {
    recordsModal_->reloadRecords(true);
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

  ImageView::dropAllCache();
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
    context.chartLibraryFoldersReloadRequested = true;
  }
  context.chartLibraryListReloadRequested = true;
}

void MainMenuScene::applyPendingUiUpdates() {
  if (context.chartLibraryTasks) {
    for (auto &completion :
         context.chartLibraryTasks->takeDownloadedIndexCompletions()) {
      std::lock_guard<std::mutex> lock(findBmsSelectionHandoffMutex);
      pendingFindBmsSelectionHandoff = PendingFindBmsSelectionHandoff{
          .chartPath = std::move(completion.chartPath),
          .targetIdentity = std::move(completion.targetIdentity),
          .selectionGeneration = completion.selectionGeneration,
      };
    }
  }
  const bool shouldOpenTasksModal = tasksModalOpenRequested.exchange(false);
  const bool shouldReloadFolders =
      context.chartLibraryFoldersReloadRequested.exchange(false);
  const bool shouldReloadCharts =
      context.chartLibraryListReloadRequested.exchange(false);
  std::optional<PendingFindBmsSelectionHandoff> findBmsHandoff;
  if (shouldReloadFolders || shouldReloadCharts) {
    std::lock_guard<std::mutex> lock(findBmsSelectionHandoffMutex);
    findBmsHandoff = std::move(pendingFindBmsSelectionHandoff);
    pendingFindBmsSelectionHandoff.reset();
  }
  std::optional<ChartMetaRecord> findBmsSelectionBeforeReload;
  if (findBmsHandoff.has_value()) {
    findBmsSelectionBeforeReload = selectedRecordSnapshot();
  }
  if (shouldOpenTasksModal) {
    showTasksModal();
  }
  if (shouldReloadFolders) {
    reloadScoreClearRanks();
    reloadFolderItems(true);
  }
  if (shouldReloadFolders || shouldReloadCharts) {
    ImageView::dropAllCache();
    reloadChartList(true);
    libraryRevision = context.chartRepository.GetLibraryRevision();
  }
  if ((shouldReloadFolders || shouldReloadCharts) &&
      pendingSelectChartPath.has_value()) {
    const std::filesystem::path path = *pendingSelectChartPath;
    pendingSelectChartPath.reset();
    selectChartByPathAfterReload(path, AutoSelectionPreview::Suppress);
  }
  if (findBmsHandoff.has_value()) {
    if (findBmsSelectionBeforeReload.has_value() &&
        main_menu_library::findBmsSelectionHandoffAllowed(
            findBmsHandoff->selectionGeneration, chartSelectionGeneration,
            findBmsHandoff->targetIdentity,
            *findBmsSelectionBeforeReload)) {
      selectChartByPathAfterReload(findBmsHandoff->chartPath,
                                   AutoSelectionPreview::Load);
    } else {
      archive_file::appendDebugLogLine(
          "Skipped Find BMS preview handoff because chart selection changed.");
    }
  }
}

void MainMenuScene::selectChartByPathAfterReload(
    const std::filesystem::path &path, AutoSelectionPreview preview) {
  if (recyclerView == nullptr || path.empty() || !chartSession.has_value()) {
    return;
  }
  const path_t target = fspath_to_path_t(path);
  const ChartMetaQuery query = chartQueryForActiveFolder();
  int index = chartSession->FindChartMetaIndex(query, path);
  if (index >= 0 && !temporaryChartFolder.has_value() &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
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
      recyclerView->scrollOffset =
          main_menu_library::centeredScrollOffsetForItem(
              index, recyclerView->size(), recyclerView->itemHeight,
              recyclerView->getHeight());
      recyclerView->rebindVisibleItems();
      if (preview == AutoSelectionPreview::Suppress) {
        suppressPreviewForChartPath = record.meta.BmsPath;
      } else if (suppressPreviewForChartPath.has_value() &&
                 fspath_to_path_t(*suppressPreviewForChartPath) ==
                     fspath_to_path_t(record.meta.BmsPath)) {
        suppressPreviewForChartPath.reset();
      }
      if (recyclerView->onSelected) {
        recyclerView->onSelected(record, index);
      }
      archive_file::appendDebugLogLine(
          std::string(preview == AutoSelectionPreview::Suppress
                          ? "Selected unzipped chart: "
                          : "Selected indexed Find BMS chart: ") +
          fspath_to_utf8(record.meta.BmsPath));
      return;
    }
  }

  if (preview == AutoSelectionPreview::Load) {
    const std::array requestedPaths{path};
    const auto lookup = chartSession->SelectChartMetaByPaths(requestedPaths);
    const auto record =
        main_menu_library::findBmsUnfilteredHandoffRecord(lookup, path);
    if (record.has_value() && recyclerView->onSelected) {
      const int previous = recyclerView->selectedIndex;
      if (previous >= 0 && previous < recyclerView->size() &&
          recyclerView->onUnselected) {
        recyclerView->onUnselected(recyclerView->get(previous), previous);
      }
      recyclerView->selectedIndex = -1;
      recyclerView->rebindVisibleItems();
      if (suppressPreviewForChartPath.has_value() &&
          fspath_to_path_t(*suppressPreviewForChartPath) == target) {
        suppressPreviewForChartPath.reset();
      }
      recyclerView->onSelected(*record, -1);
      archive_file::appendDebugLogLine(
          "Selected indexed Find BMS chart outside active filters: " +
          fspath_to_utf8(record->meta.BmsPath));
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
    selectChartByPathAfterReload(path, preview);
  }
}

void MainMenuScene::selectFolder(LibraryFolderItem item) {
  const bool clearedSameFolderScope = clearSameFolderScope();
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
    if (clearedSameFolderScope || activeFolder.key != previousActiveKey) {
      reloadChartListForFolderSelection();
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
  if (item.expandable && chartQueryUnchanged && !clearedSameFolderScope) {
    return;
  }
  reloadChartListForFolderSelection();
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
                        : result_contract::maximumScoreForNotes(
                              record->meta.TotalNotes)
                              .value_or(0),
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

std::optional<MainMenuScene::CurrentCourseSelection>
MainMenuScene::currentCourseSelectionFor(
    const result_persistence::ModernCourseResult &result) {
  if (activeFolder.type != LibraryFolderItem::Type::Course ||
      activeFolder.courseKey != result.courseKey || result.totalCharts <= 0) {
    return std::nullopt;
  }
  const auto &validation = courseValidationForActiveFolder();
  if (result.stages.size() > validation.records.size() ||
      std::ranges::any_of(
          validation.records.begin(),
          validation.records.begin() +
              static_cast<std::ptrdiff_t>(result.stages.size()),
          [](const auto &record) {
            return record.solidArchive || record.unavailable ||
                   record.meta.BmsPath.empty();
          })) {
    return std::nullopt;
  }
  CurrentCourseSelection selection{
      .records = validation.records,
      .completeCourse =
          validation.records.size() ==
              static_cast<std::size_t>(result.totalCharts) &&
          validation.firstMissingIndex < 0,
  };
  selection.completedChartPaths.reserve(result.stages.size());
  for (std::size_t index = 0; index < result.stages.size(); ++index) {
    selection.completedChartPaths.push_back(
        selection.records[index].meta.BmsPath);
  }
  return selection;
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
      pendingSelectChartPath.has_value() ||
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
      recyclerView == nullptr ||
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

  auto session = buildCourseGameplaySession(
      {.courseId = activeFolder.courseId,
       .courseKey = activeFolder.courseKey,
       .courseName = activeFolder.courseGroupName.empty()
                         ? activeFolder.label
                         : activeFolder.courseGroupName + " " +
                               activeFolder.label,
       .courseGroupName = activeFolder.courseGroupName,
       .constraintJson = activeFolder.courseConstraintJson,
       .records = records,
       .selections = profileSelections,
       .player2PlayOption = std::string(
           replay::beatorajaReplayOptionName(
               context.settings.skinPlayer2RandomOption)
               .value_or("NORMAL")),
       .doublePlayFlip = context.settings.skinDoublePlayOption == 1,
       .inputKeysoundEnabled = context.settings.inputKeysoundEnabled});
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
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
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
        if (previewWorker_ != nullptr) {
          previewWorker_->stop();
        }
        {
          std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
          pendingStopAndClearSelectedChartAfterPreview = false;
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
            play_options::applySelectedPlayOptions(
                *preparedChart, session->requestedPlayOption,
                session->requestedPlayOption2);
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
        options.doublePlayFlip = session->doublePlayFlip;
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
      pendingSelectChartPath.has_value() ||
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
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
  startChartDirect(*record);
}

void MainMenuScene::startChartDirect(const ChartMetaRecord &record) {
  if (willStart.exchange(true)) {
    return;
  }
  StartupTiming::instance().beginSession();
  StartupTiming::instance().mark("main menu start press");

  if (record.solidArchive || record.unavailable ||
      record.meta.BmsPath.empty()) {
    resetStartLoadingUi();
    return;
  }

  if (startButtonText != nullptr) {
    startButtonText->setText("Loading...");
  }
  if (decideOverlay_ != nullptr) {
    decideOverlay_->setChart(record);
    decideOverlay_->setVisible(true);
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
  std::string tableName;
  std::string tableLevel;
  if ((activeFolder.type == LibraryFolderItem::Type::DifficultyTable ||
       activeFolder.type == LibraryFolderItem::Type::DifficultyLevel ||
       activeFolder.type == LibraryFolderItem::Type::DifficultyClearMark) &&
      activeFolder.tableId > 0) {
    const auto table = std::ranges::find_if(
        folderMetadataCache.tables, [this](const DifficultyTableInfo &item) {
          return item.id == activeFolder.tableId;
        });
    if (table != folderMetadataCache.tables.end()) {
      tableName = table->name;
      if (!activeFolder.tableLevel.empty()) {
        // TableDataAccessor builds HashBar titles as TableData.tag + level.
        tableLevel = table->symbol + activeFolder.tableLevel;
      }
    }
  }

  defer(
      [this, record, gaugeType, gaugeAutoShift, gaugeAutoShiftLowerBound,
       ruleset, autoKeySound, playOption, selectedLongNoteMode, assistOption,
       pacemakerTarget, playback,
       canReusePreviewForStart, chartRandomInfo, tableName = std::move(tableName),
       tableLevel = std::move(tableLevel)]() {
        auto finishStart = [this]() {
          resetStartLoadingUi();
          return true;
        };
        if (!canReusePreviewForStart) {
          if (previewWorker_ != nullptr) {
            previewWorker_->cancel();
          }
        }
        if (previewWorker_ != nullptr) {
          previewWorker_->stop();
        }
        {
          std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
          pendingStopAndClearSelectedChartAfterPreview = false;
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
                                    .gaugeAutoShiftLowerBound =
                                        gaugeAutoShiftLowerBound,
                                    .longNoteMode = selectedLongNoteMode,
                                    .assistOption = assistOption,
                                    .pacemakerTarget = pacemakerTarget,
                                    .tableName = tableName,
                                    .tableLevel = tableLevel,
                                    .playback = playback,
                                    .ruleset = ruleset,
                                });
          return finishStart();
        }

        if (previewWorker_ != nullptr) {
          previewWorker_->cancel();
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
                                      .gaugeAutoShiftLowerBound =
                                          gaugeAutoShiftLowerBound,
                                      .playOption = playInfo.option,
                                      .playOptionSeed = playInfo.seed,
                                      .playOption2 = playInfo.option2,
                                      .playOption2Seed = playInfo.seed2,
                                      .longNoteMode = selectedLongNoteMode,
                                      .assistOption = assistOption,
                                      .pacemakerTarget = pacemakerTarget,
                                      .tableName = tableName,
                                      .tableLevel = tableLevel,
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
                                         .tableName = tableName,
                                         .tableLevel = tableLevel,
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
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
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

  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
  }
  archive_file::appendDebugLogLine(
      "Open chart viewer: " + fspath_to_utf8(record.meta.BmsPath));
  context.jukebox.stop();
  context.sceneManager->changeScene(
      std::make_unique<ChartViewerScene>(context, record, chartRandomInfo.seed,
                                         chartRandomInfo.prng,
                                         chartRandomInfo.values),
      true);
}

void MainMenuScene::toggleRevealContextMenu() {
  if (revealContextMenu == nullptr || revealButton == nullptr) {
    return;
  }
  if (revealContextMenu->isOpen()) {
    revealContextMenu->dismiss();
    return;
  }
  if (willStart.load() || replayExportInProgress.load() ||
      recyclerView == nullptr) {
    return;
  }

  const auto record = selectedRecordSnapshot();
  if (!record.has_value() || record->courseStart || record->solidArchive ||
      record->unavailable || record->meta.BmsPath.empty()) {
    return;
  }

  const bool canShowSameFolder =
      main_menu_library::sameFolderForChart(*record).has_value();
  revealContextMenu->setViewportSize(rendering::window_width,
                                     rendering::window_height);
  revealContextMenu->show(
      {.x = revealButton->getX(),
       .y = revealButton->getY(),
       .width = revealButton->getWidth(),
       .height = revealButton->getHeight()},
      {{.id = "show-same-folder",
        .label = "Show Same Folder",
        .enabled = canShowSameFolder},
       {.id = "reveal-file", .label = "Reveal File"}},
      220);
}

void MainMenuScene::showSelectedChartFolder() {
  if (willStart.load() || replayExportInProgress.load() ||
      recyclerView == nullptr) {
    return;
  }
  const auto record = selectedRecordSnapshot();
  if (!record.has_value() || record->courseStart || record->solidArchive ||
      record->unavailable || record->meta.BmsPath.empty()) {
    return;
  }
  const auto folder = main_menu_library::sameFolderForChart(*record);
  if (!folder.has_value()) {
    return;
  }

  temporaryChartFolder = *folder;
  searchText.clear();
  if (searchBox != nullptr) {
    searchBox->setEditingText("");
  }
  chartRecordFilters =
      main_menu_library::filtersForSameFolder(chartRecordFilters);
  chartBpmMinText.clear();
  chartBpmMaxText.clear();
  chartClearMarkDropdownOpen = false;
  chartScoreRankDropdownOpen = false;
  chartDifficultyMinDropdownOpen = false;
  chartDifficultyMaxDropdownOpen = false;
  chartDifficultyRangeTableId.reset();

  if (folderRecyclerView != nullptr) {
    const int selectedIndex = folderRecyclerView->selectedIndex;
    if (selectedIndex >= 0 && selectedIndex < folderRecyclerView->size() &&
        folderRecyclerView->onUnselected) {
      folderRecyclerView->onUnselected(folderRecyclerView->get(selectedIndex),
                                       selectedIndex);
    }
    folderRecyclerView->selectedIndex = -1;
    folderRecyclerView->rebindVisibleItems();
  }

  refreshChartFilterPanel();
  reloadChartList(true);
  if (recyclerView->selectedIndex >= 0) {
    recyclerView->scrollOffset =
        main_menu_library::centeredScrollOffsetForItem(
            recyclerView->selectedIndex, recyclerView->size(),
            recyclerView->itemHeight, recyclerView->getHeight());
    recyclerView->rebindVisibleItems();
  }
}

bool MainMenuScene::clearSameFolderScope() {
  if (!temporaryChartFolder.has_value()) {
    return false;
  }
  temporaryChartFolder.reset();
  return true;
}

void MainMenuScene::revealSelectedChartInFileManager() {
  if (willStart.load() || replayExportInProgress.load() ||
      recyclerView == nullptr || revealButton == nullptr) {
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
  const OverlayAnchor sourceAnchor{
      .x = revealButton->getX(),
      .y = revealButton->getY(),
      .width = revealButton->getWidth(),
      .height = revealButton->getHeight(),
  };
  const auto normalized = normalizeOverlayAnchor(
      sourceAnchor, rendering::window_width, rendering::window_height);
  if (!platform_open::revealPathInFileManager(
          record->meta.BmsPath,
          {.x = normalized.x,
           .y = normalized.y,
           .width = normalized.width,
           .height = normalized.height},
          errorMessage)) {
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
    const auto courseRecords = context.replayRepository.ListLegacyCourseSummaries(
        {.courseKey = activeFolder.courseKey,
         .legacyCourseId = activeFolder.courseId});
    bool hasModernRecords = false;
    if (!activeFolder.courseKey.empty()) {
      const auto modern = context.replayRepository.ListModernCourseResults(
          activeFolder.courseKey, 1);
      hasModernRecords =
          modern.status == ModernCourseHistoryReadStatus::Loaded &&
          !modern.records.empty();
    }
    setReplayButtonVisible(!courseRecords.empty() || hasModernRecords);
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
    if (!showChartActions && revealContextMenu != nullptr) {
      revealContextMenu->dismiss();
    }
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
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() ||
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
      pendingSelectChartPath.has_value() ||
      context.chartLibraryListReloadRequested.load() ||
      context.chartLibraryFoldersReloadRequested.load() || record.unavailable ||
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
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
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
  const auto fallback = ChartRepository::DefaultBmsFolderPath();
  if (!chartSession.has_value()) {
    ensureDirectoryExistsLogged(fallback, "BMS download root");
    return fallback;
  }

  const auto selected = chartSession->SelectPrimaryStorageEntry();
  if (!selected.has_value()) {
    ensureDirectoryExistsLogged(fallback, "BMS download root");
    return fallback;
  }

  return chart_library_platform::resolveFolderEntryPath(*selected);
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

  parseLogExportStatusText =
      new TextView("assets/fonts/notosanscjkjp.ttf", 16);
  parseLogExportStatusText->setFlex(1);
  parseLogExportStatusText->setVAlign(TextView::MIDDLE);
  parseLogExportStatusText->setOverflow(TextView::TextOverflow::Hidden);
  parseLogExportStatusText->setThemedColor(ui_theme::textSecondary);
  footer->addView(parseLogExportStatusText);

  parseLogExportButton =
      makeModalButton("Export Log", 20, &parseLogExportButtonText);
  parseLogExportButton->setWidth(160);
  parseLogExportButton->setOnClickListener(
      [this]() { startParseLogExport(); });
  footer->addView(parseLogExportButton);

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
  refreshParseLogExportControls();
  refreshParseLogModal(true);
}

void MainMenuScene::showParseLogModal() {
  if (parseLogModalRoot == nullptr) {
    return;
  }
  parseLogModalRoot->setSize(rendering::window_width, rendering::window_height);
  parseLogModalRoot->setVisible(true);
  parseLogDisplayedRevision = 0;
  refreshParseLogExportControls();
  refreshParseLogModal(true);
}

void MainMenuScene::hideParseLogModal() {
  if (parseLogModalRoot != nullptr) {
    parseLogModalRoot->setVisible(false);
  }
}

void MainMenuScene::startParseLogExport() {
  if (parseLogDocumentHandoff) {
    return;
  }
  if (parseLogExportStatusText != nullptr) {
    parseLogExportStatusText->setText("Preparing performance log...");
  }
  std::string logText = archive_file::debugLogText();
  const std::uint64_t exportLimit = std::max<std::uint64_t>(
      1, static_cast<std::uint64_t>(logText.size()));
  parseLogDocumentHandoff =
      platform_document_handoff::ExportTextDocumentAsync({
          .text = std::move(logText),
          .suggestedName = "AsoBMaShow-performance-log.txt",
          .maxBytes = exportLimit,
      });
  refreshParseLogExportControls();
}

void MainMenuScene::applyParseLogDocumentHandoff() {
  if (!parseLogDocumentHandoff || !parseLogDocumentHandoff.ready()) {
    return;
  }
  auto result = parseLogDocumentHandoff.takeResult();
  parseLogDocumentHandoff.close();
  if (parseLogExportStatusText != nullptr && result) {
    if (result->ok()) {
      parseLogExportStatusText->setText("Performance log exported.");
    } else if (result->cancelled()) {
      parseLogExportStatusText->setText("Log export cancelled.");
    } else {
      const std::string message =
          result->message.empty() ? "Log export failed." : result->message;
      parseLogExportStatusText->setText(message);
      SDL_Log("Performance log export failed: %s", message.c_str());
    }
  }
  refreshParseLogExportControls();
}

void MainMenuScene::refreshParseLogExportControls() {
  if (parseLogExportButton == nullptr || parseLogExportButtonText == nullptr) {
    return;
  }
  const bool enabled = !static_cast<bool>(parseLogDocumentHandoff);
  parseLogExportButtonText->setText(enabled ? "Export Log" : "Exporting...");
  styleThemedActionButton(
      parseLogExportButton, parseLogExportButtonText, enabled,
      ui_theme::primaryAction, ui_theme::primaryActionHover,
      ui_theme::primaryActionPressed, ui_theme::accentBorderStrong);
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

  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
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
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
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
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
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
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
  }
  context.jukebox.stop();

  std::string errorMessage;
  context.musicPlayer.PlayNextAsync(errorMessage, "Playing next track.");
  musicStatusMessage = errorMessage;
  refreshMusicModal();
}

void MainMenuScene::playPreviousMusicTrack() {
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
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

  const auto snapshot = context.chartLibraryTasks
                            ? context.chartLibraryTasks->snapshot()
                            : chart_library_tasks::Snapshot{};
  if (snapshot.revision == displayedLibraryTasksRevision &&
      snapshot.progress.revision == displayedLibraryProgressRevision) {
    return;
  }
  displayedLibraryTasksRevision = snapshot.revision;
  displayedLibraryProgressRevision = snapshot.progress.revision;
  tasksText->setText(tasksModalTextSnapshot());
}

std::string MainMenuScene::tasksModalTextSnapshot() {
  const auto snapshot = context.chartLibraryTasks
                            ? context.chartLibraryTasks->snapshot()
                            : chart_library_tasks::Snapshot{};
  const LibraryTaskProgressSnapshot &progressSnapshot = snapshot.progress;
  std::vector<LibraryTaskInfo> activeTasks;
  std::vector<LibraryTaskInfo> recentTasks;
  activeTasks.reserve(snapshot.tasks.size());
  recentTasks.reserve(snapshot.tasks.size());
  for (const auto &task : snapshot.tasks) {
    if (task.status == LibraryTaskStatus::Queued ||
        task.status == LibraryTaskStatus::Running ||
        task.status == LibraryTaskStatus::Paused) {
      activeTasks.push_back(task);
    } else {
      recentTasks.push_back(task);
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
    const bool wasRunning = findBmsJobRunning.load();
    applyFindBmsUpdates();
    const bool running = wasRunning || findBmsJobRunning.load();
    if (!findBmsDialogPolicy(running, findBmsResult).showCloseOrCancel) {
      return;
    }
    if (running) {
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
  findBmsSelectionGenerationAtDownloadStart = chartSelectionGeneration;
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
  findBmsSelectionGenerationAtDownloadStart = chartSelectionGeneration;
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
  const bool wasRunning = findBmsJobRunning.load();
  applyFindBmsUpdates();
  const bool running = wasRunning || findBmsJobRunning.load();
  if (findBmsModalRoot == nullptr ||
      !findBmsDialogPolicy(running, findBmsResult).canDismiss) {
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
                                              findBmsProgressTotal, true);
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
    detail += "Adding downloaded charts to the library.";
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
    detail += findBmsDownloadFailureDetail(findBmsResult);
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
    appendLogLine(findBmsProgressEventDisplayText(progress, true));
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
    if (findBmsResult.status == BmsSearchResult::Status::Downloaded) {
      enqueueDownloadedPathIndexTask(
          findBmsResult.outputPath,
          main_menu_library::findBmsChartIdentity(findBmsModalChart.meta),
          findBmsSelectionGenerationAtDownloadStart,
          findBmsResult.removedPaths);
    } else if (keptMismatchedFiles) {
      enqueueDownloadedPathIndexTask(findBmsResult.outputPath, {}, 0,
                                     findBmsResult.removedPaths);
    }
    shouldRefresh = true;
  }
  if (shouldRefresh) {
    refreshFindBmsModal();
  }
}

void MainMenuScene::openFindBmsResultUrl(const std::string &url) {
  std::string errorMessage;
  if (!platform_open::openExternalUrl(url, errorMessage)) {
    SDL_Log("Failed to open URL %s: %s", url.c_str(), errorMessage.c_str());
  }
}

void MainMenuScene::buildPlayOptionsModal() {
  if (rootLayout == nullptr) {
    return;
  }
  playOptionsModal = MainMenuPlayOptionsModal::Create(
      rootLayout,
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
       }}, overlayPortal);
  if (!playOptionsModal) return;
  playOptionsModalRoot = playOptionsModal->root();
  playOptionsPanel = playOptionsModal->panel();
  refreshGaugeSelectionButtons();
  refreshPlayOptionButtons();
  refreshLongNoteModeButtons();
  refreshAssistOptionButtons();
  refreshPlaybackSelectionControls();
  refreshPacemakerTargetButtons();
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
  playOptionsModal->show();
}

void MainMenuScene::hidePlayOptionsModal() {
  if (playOptionsModalRoot == nullptr) {
    return;
  }
  playOptionsModal->hide();
}

void MainMenuScene::openReplayRecordsForSelection() {
  if (recordsModal_ == nullptr || recyclerView == nullptr) {
    return;
  }
  const int selected = recyclerView->selectedIndex;
  if (selected < 0 || selected >= recyclerView->size()) {
    return;
  }
  const auto *selectedMeta = &recyclerView->get(selected);
  const bool courseStartReplay =
      selectedMeta->courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      activeFolder.courseId > 0;
  if (selectedMeta->solidArchive || selectedMeta->unavailable ||
      (!courseStartReplay && selectedMeta->meta.BmsPath.empty())) {
    return;
  }
  replayIrObservedRevisions.clear();
  recordsModal_->setTouchVisualizationEnabled(
      context.settings.touchVisualizationEnabled);
  recordsModal_->showChart(*selectedMeta);
  setReplayButtonVisible(true);
}

std::vector<ResultRecordSummary>
MainMenuScene::loadRecordsForModal(const ChartMetaRecord &record) {
  const bool courseReplayList =
      record.courseStart &&
      activeFolder.type == LibraryFolderItem::Type::Course &&
      (!activeFolder.courseKey.empty() || activeFolder.courseId > 0);
  std::vector<ReplaySummary> syntheticRecords;
  std::vector<ResultRecordSummary> modernSummaries;
  if (courseReplayList) {
    const auto legacy = context.replayRepository.ListLegacyCourseSummaries(
        {.courseKey = activeFolder.courseKey,
         .legacyCourseId = activeFolder.courseId},
        kMaximumLegacyResultSummaryRows);
    modernSummaries.reserve(legacy.size());
    for (const LegacyCourseResultSummary &summary : legacy) {
      modernSummaries.push_back(makeLegacyCourseResultRecord(summary));
    }
  } else {
    syntheticRecords.push_back(autoPlayReplaySummary(record));
    const auto legacy = context.replayRepository.ListLegacyChartSummaries(
        record.meta, kMaximumLegacyResultSummaryRows);
    modernSummaries.reserve(legacy.size());
    for (const LegacyChartResultSummary &summary : legacy) {
      modernSummaries.push_back(makeLegacyChartResultRecord(summary));
    }
  }

  const std::optional<std::string> irServerOrigin =
      activeReplayIrServerOrigin();
  std::unordered_map<std::string, ir::IrUploadRecord> irRecordsByAttempt;
  bool modernIrReadSucceeded = courseReplayList;
  if (!courseReplayList && irServerOrigin.has_value() &&
      !record.meta.SHA256.empty()) {
    auto records = context.replayRepository.ListIrUploadRecordsForChart(
        ir::kTachiProviderId, *irServerOrigin, record.meta.SHA256,
        kMaximumModernChartHistoryRows);
    std::string irReadDiagnostic;
    if (records.status != ir::IrUploadRecordReadStatus::Loaded) {
      irReadDiagnostic =
          records.diagnostic.empty()
              ? "Modern IR Records unavailable: state could not be read"
              : std::string("Modern IR Records unavailable: ") +
                    records.diagnostic;
    } else {
      irRecordsByAttempt.reserve(records.records.size());
      for (auto &irRecord : records.records) {
        irRecordsByAttempt.emplace(irRecord.attemptId, std::move(irRecord));
      }
      irReadDiagnostic = std::move(records.diagnostic);
    }
    modernIrReadSucceeded = irReadDiagnostic.empty();
    if (!irReadDiagnostic.empty()) {
      const std::string diagnostic =
          ir::sanitizeDiagnostic(irReadDiagnostic);
      if (diagnostic != publishedResultRecordDiagnostic) {
        publishedResultRecordDiagnostic = diagnostic;
        SDL_Log("%s", diagnostic.c_str());
        archive_file::appendDebugLogLine(diagnostic);
      }
    }
  } else if (!courseReplayList && !irServerOrigin.has_value()) {
    const std::string diagnostic =
        "Modern IR Records unavailable: provider origin is invalid";
    if (diagnostic != publishedResultRecordDiagnostic) {
      publishedResultRecordDiagnostic = diagnostic;
      SDL_Log("%s", diagnostic.c_str());
      archive_file::appendDebugLogLine(diagnostic);
    }
  } else if (!courseReplayList) {
    modernIrReadSucceeded = true;
  }

  bool modernHistoryReadSucceeded = true;
  if (courseReplayList && !activeFolder.courseKey.empty()) {
    const auto history = context.replayRepository.ListModernCourseResults(
        activeFolder.courseKey, kMaximumModernCourseHistoryRows);
    if (history.status == ModernCourseHistoryReadStatus::Loaded) {
      modernSummaries.reserve(modernSummaries.size() + history.records.size());
      replay::ReplayFileActionService replayActions(context.replayRepository);
      for (const ModernCourseResultRecord &modern : history.records) {
        const auto inspected = replayActions.probe(modern.replayFile);
        modernSummaries.push_back(
            makeModernCourseResultRecord(
                modern, replay::replayStateForFileAction(inspected.state)));
      }
    } else {
      modernHistoryReadSucceeded = false;
      const std::string diagnostic = ir::sanitizeDiagnostic(
          history.diagnostic.empty()
              ? "Modern Course Records unavailable: history could not be read"
              : std::string("Modern Course Records unavailable: ") +
                    history.diagnostic);
      if (diagnostic != publishedResultRecordDiagnostic) {
        publishedResultRecordDiagnostic = diagnostic;
        SDL_Log("%s", diagnostic.c_str());
        archive_file::appendDebugLogLine(diagnostic);
      }
    }
  } else if (!courseReplayList && !record.meta.SHA256.empty()) {
    const auto history = context.replayRepository.ListModernChartResults(
        record.meta.SHA256, kMaximumModernChartHistoryRows);
    if (history.status == ModernChartHistoryReadStatus::Loaded) {
      modernSummaries.reserve(modernSummaries.size() + history.records.size());
      replay::ReplayFileActionService replayActions(context.replayRepository);
      for (const ModernChartResultRecord &modern : history.records) {
        const auto inspected = replayActions.probe(modern.replayFile);
        ir::IrRecordState irState = ir::IrRecordState::Hidden;
        std::optional<IrRemoteRecordId> linkedRemote;
        const auto storedIr = irRecordsByAttempt.find(modern.result.attemptId);
        if (storedIr != irRecordsByAttempt.end()) {
          const auto serviceStatus =
              context.irSubmissionService != nullptr
                  ? context.irSubmissionService->status(
                        ir::kTachiProviderId, modern.result.attemptId)
                  : ir::IrAttemptStatusSnapshot{};
          irState = storedIr->second.resolvedState(
              recordActivityFor(serviceStatus.activeRequest));
          if (storedIr->second.receiptRemoteScoreId && irServerOrigin) {
            linkedRemote = IrRemoteRecordId{
                .providerId = std::string(ir::kTachiProviderId),
                .serverOrigin = *irServerOrigin,
                .remoteScoreId = *storedIr->second.receiptRemoteScoreId,
            };
          }
        }
        modernSummaries.push_back(makeModernChartResultRecord(
            modern, replay::replayStateForFileAction(inspected.state),
            irState, std::move(linkedRemote)));
      }
    } else {
      modernHistoryReadSucceeded = false;
      const std::string diagnostic = ir::sanitizeDiagnostic(
          history.diagnostic.empty()
              ? "Modern Records unavailable: history could not be read"
              : std::string("Modern Records unavailable: ") +
                    history.diagnostic);
      if (diagnostic != publishedResultRecordDiagnostic) {
        publishedResultRecordDiagnostic = diagnostic;
        SDL_Log("%s", diagnostic.c_str());
        archive_file::appendDebugLogLine(diagnostic);
      }
    }
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
    } else if (irServerOrigin.has_value()) {
      mergeOrigin = *irServerOrigin;
      auto loaded = context.replayRepository.ListIrRemoteScoresForChart(
          ir::kTachiProviderId, mergeOrigin, record.meta.MD5,
          record.meta.SHA256);
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
    auto merged = mergeResultRecords(
        syntheticRecords, modernSummaries,
        remoteReadSucceeded
            ? std::span<const ir::IrRemoteScore>(remoteScores)
            : std::span<const ir::IrRemoteScore>{},
        ir::kTachiProviderId, mergeOrigin);
    if (remoteReadSucceeded && modernHistoryReadSucceeded &&
        modernIrReadSucceeded) {
      publishedResultRecordDiagnostic.clear();
    }
    return merged;
  } catch (...) {
    auto merged = mergeResultRecords(
        syntheticRecords, modernSummaries,
        std::span<const ir::IrRemoteScore>{}, ir::kTachiProviderId,
        std::string_view{});
    const std::string diagnostic =
        "IR Records unavailable: remote score projection is invalid";
    if (diagnostic != publishedResultRecordDiagnostic) {
      publishedResultRecordDiagnostic = diagnostic;
      SDL_Log("%s", diagnostic.c_str());
      archive_file::appendDebugLogLine(diagnostic);
    }
    return merged;
  }
}

void MainMenuScene::shareReplayFile(
    const replay::ReplayFileActionRequest &request) {
  if (replayFileDocumentHandoff || replayExportInProgress.load() ||
      replayResultRecallInProgress || replayIrUploadInProgress) {
    return;
  }

  replay::ReplayFileActionService actions(context.replayRepository);
  auto prepared = actions.prepareShare(request);
  if (prepared.state != replay::ReplayFileActionState::Verified ||
      !prepared.share) {
    const std::string diagnostic = prepared.diagnostic.empty()
                                       ? "Replay file is unavailable to share."
                                       : prepared.diagnostic;
    SDL_Log("Replay share unavailable: %s", diagnostic.c_str());
    if (recordsModal_ != nullptr) {
      recordsModal_->setStatus(diagnostic);
      recordsModal_->reloadRecords(true);
    }
    return;
  }

  PlatformDocumentExportRequest exportRequest{
      .localPath = prepared.share->sourcePath,
      .mimeType = "application/gzip",
      .suggestedName = prepared.share->suggestedFilename,
      .maxBytes = replay::kReplayLimits.maxCompressedBytes,
      .sourceLifetime = std::move(prepared.share->sourceLifetime)};
  replayFileDocumentHandoff =
      platform_document_handoff::ExportDocumentAsync(std::move(exportRequest));
  if (recordsModal_ != nullptr) {
    recordsModal_->setDocumentHandoffActive(true);
    if (!replayFileDocumentHandoff) {
      recordsModal_->setStatus("Unable to open replay sharing.");
    } else {
      recordsModal_->setStatus("Choose where to share the BRD replay.");
    }
  }
}

void MainMenuScene::removeReplayFile(
    const replay::ReplayFileActionRequest &request) {
  if (replayFileDocumentHandoff || replayExportInProgress.load() ||
      replayResultRecallInProgress || replayIrUploadInProgress) {
    return;
  }

  replay::ReplayFileActionService actions(context.replayRepository);
  const auto removed = actions.remove(request);
  if (removed.state == replay::ReplayFileActionState::UserDeleted) {
    if (recordsModal_ != nullptr) {
      recordsModal_->setStatus(
          removed.cleanupPending
              ? "Replay hidden; file cleanup will retry at startup."
              : "Replay file deleted. Result history was kept.");
      recordsModal_->reloadRecords(true);
    }
  } else {
    const std::string diagnostic = removed.diagnostic.empty()
                                       ? "Replay file could not be deleted."
                                       : removed.diagnostic;
    SDL_Log("Replay delete failed: %s", diagnostic.c_str());
    if (recordsModal_ != nullptr) {
      recordsModal_->setStatus(diagnostic);
    }
  }
}

void MainMenuScene::applyReplayFileDocumentHandoff() {
  if (!replayFileDocumentHandoff || !replayFileDocumentHandoff.ready()) {
    return;
  }
  auto result = replayFileDocumentHandoff.takeResult();
  replayFileDocumentHandoff.close();
  if (recordsModal_ != nullptr) {
    recordsModal_->setDocumentHandoffActive(false);
    if (result) {
      if (result->ok()) {
        recordsModal_->setStatus("Replay BRD shared.");
      } else if (result->cancelled()) {
        recordsModal_->setStatus("Replay sharing cancelled.");
      } else {
        recordsModal_->setStatus(result->message.empty()
                                     ? "Replay sharing failed."
                                     : result->message);
      }
    }
  }
}

ReplayRecordsModalCallbacks MainMenuScene::makeRecordsModalCallbacks() {
  ReplayRecordsModalCallbacks callbacks;
  callbacks.loadRecords = [this](const ChartMetaRecord &record) {
    return loadRecordsForModal(record);
  };
  callbacks.watchModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
        startModernReplayPlayback(record, modern);
      };
  callbacks.watchModernCourse =
      [this](const ChartMetaRecord &record,
             const ModernCourseResultRecord &modern) {
        startModernCourseReplayPlayback(record, modern);
      };
  callbacks.watchAutoPlay = [this](const ChartMetaRecord &record) {
    startAutoPlayPlayback(record);
  };
  callbacks.gbattle =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
        startModernGBattlePlayback(record, modern);
      };
  callbacks.recallModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern) {
        startModernReplayResultRecall(record, modern);
      };
  callbacks.recallModernCourse = [this](const ModernCourseResultRecord &modern,
                                        bool retrySame) {
    startModernCourseReplayResultRecall(modern, retrySame);
  };
  callbacks.recallRemote = [this](const IrRemoteRecordId &identity,
                                  const std::string &selectedStableKey) {
    startRemoteResultRecall(identity, selectedStableKey);
  };
  callbacks.exportModernChart =
      [this](const ChartMetaRecord &record, const ModernChartResultRecord &modern,
             ReplayVideoExportOptions options) {
        options.pacemakerTarget =
            pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
        startModernReplayVideoExport(record, modern, options);
      };
  callbacks.exportModernCourse =
      [this](const ModernCourseResultRecord &modern,
             ReplayVideoExportOptions options) {
        options.pacemakerTarget =
            pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
        startModernCourseReplayVideoExport(modern, options);
      };
  callbacks.exportAutoPlay =
      [this](const ChartMetaRecord &record, ReplayVideoExportOptions options) {
        options.pacemakerTarget = pacemaker::kTargetOff;
        startAutoPlayVideoExport(record, options);
      };
  callbacks.share = [this](const replay::ReplayFileActionRequest &request) {
    shareReplayFile(request);
  };
  callbacks.remove = [this](const replay::ReplayFileActionRequest &request) {
    removeReplayFile(request);
  };
  callbacks.irUpload = [this](const ModernChartResultRecord &modern) {
    startModernReplayIrUpload(modern);
  };
  callbacks.irStatusFeedback = [this](ir::IrRecordState state) {
    publishReplayIrStatusFeedback(state);
  };
  return callbacks;
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

void MainMenuScene::startAutoPlayPlayback(const ChartMetaRecord &record) {
  if (record.courseStart || willStart.load()) {
    return;
  }

  willStart.store(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(true);
  const audio::PlaybackRate autoPlayPlayback{
      .percent = context.settings.selectedPlaybackRatePercent,
      .mode = context.settings.selectedPlaybackMode,
  };
  const GameplayRuleset autoPlayRuleset = profileSelections.ruleset;
  defer(
      [this, record, autoPlayPlayback, autoPlayRuleset]() {
        auto failReplayLoad = [this]() {
          resetReplayWatchLoadingUi();
          return true;
        };
        if (previewWorker_ != nullptr) {
          previewWorker_->stop();
        }
        {
          std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
          pendingStopAndClearSelectedChartAfterPreview = false;
        }

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

        if (recordsModal_ != nullptr) recordsModal_->hide();
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
      },
      0, true);
}

void MainMenuScene::startModernReplayPlayback(
    const ChartMetaRecord &record, ModernChartResultRecord modern) {
  if (record.courseStart || willStart.load()) {
    return;
  }

  willStart.store(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(true);
  const std::string pacemakerTarget =
      pacemaker::normalizeTargetId(profileSelections.pacemakerTarget);
  const bool renderTouchPoints =
      recordsModal_ != nullptr ? recordsModal_->renderTouchPoints() : false;
  const bool renderGhosts =
      recordsModal_ != nullptr ? recordsModal_->renderReplayGhosts() : true;
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = true;
  }
  startReplayLoadWorker(
      [this, record, modern = std::move(modern), pacemakerTarget,
       renderTouchPoints,
       renderGhosts](std::shared_ptr<std::atomic_bool> cancelled) mutable {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            pendingStopAndClearSelectedChartAfterPreview = false;
          }
          auto consumer = replay::makeRuntimeChartReplayConsumer(
              context.replayRepository);
          auto loaded = consumer.load(modern, record.meta.BmsPath, *cancelled);
          if (!loaded.ready()) {
            const std::string diagnostic = std::move(loaded.diagnostic);
            queueReplayLoadCompletion([this, diagnostic]() {
              (void)finishReplayLoadFailure(
                  "Watch failed", diagnostic,
                  "Replay playback could not be prepared.");
            });
            return;
          }
          if (cancelled->load()) {
            return;
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            context.jukebox.stop();
            context.jukebox.loadChart(*loaded.chart, true, *cancelled);
          }
          if (cancelled->load()) {
            return;
          }

          struct Completion {
            replay::ChartReplayConsumerOutcome loaded;
          };
          auto completion = std::make_shared<Completion>(
              Completion{.loaded = std::move(loaded)});
          queueReplayLoadCompletion(
              [this, completion, pacemakerTarget, renderTouchPoints,
               renderGhosts]() mutable {
                auto &loaded = completion->loaded;
                if (!loaded.diagnostic.empty()) {
                  publishReplayLoadDiagnostic("Watch warning",
                                              loaded.diagnostic);
                }
                auto *chart =
                    setSelectedChart(std::move(loaded.chart), true, false);
                if (chart == nullptr) {
                  (void)finishReplayLoadFailure(
                      "Watch failed", {},
                      "Prepared replay chart is unavailable.");
                  return;
                }
                StartOptions replayOptions{
                    .startPosition = 0,
                    .autoKeySound = false,
                    .autoPlay = false,
                    .gaugeType = loaded.replayData->initialGaugeType,
                    .gaugeAutoShift = loaded.replayData->gaugeAutoShift,
                    .replayData = loaded.replayData,
                    .pacemakerTarget = pacemakerTarget,
                    .touchVisualizationEnabled = renderTouchPoints,
                    .replayGhostRenderingEnabled = renderGhosts,
                };
                applyReplayProvenanceToStartOptions(replayOptions,
                                                    *loaded.replayData);
                context.jukebox.stop();
                if (recordsModal_ != nullptr) recordsModal_->hide();
                changeToGameplayScene(chart, std::move(replayOptions));
                willStart.store(false);
              });
        } catch (...) {
          queueReplayLoadCompletion([this]() {
            (void)finishReplayLoadFailure(
                "Watch failed", {}, "Replay playback could not be prepared.");
          });
        }
      });
}

void MainMenuScene::startModernGBattlePlayback(
    const ChartMetaRecord &record, ModernChartResultRecord modern) {
  if (record.courseStart || willStart.load()) {
    return;
  }

  willStart.store(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(true);
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

  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = true;
  }
  startReplayLoadWorker(
      [this, record, modern = std::move(modern), gaugeType, gaugeAutoShift,
       gaugeAutoShiftLowerBound, autoKeySound, ruleset,
       playback](std::shared_ptr<std::atomic_bool> cancelled) mutable {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            pendingStopAndClearSelectedChartAfterPreview = false;
          }
          auto consumer = replay::makeRuntimeChartReplayConsumer(
              context.replayRepository);
          auto loaded = consumer.load(modern, record.meta.BmsPath, *cancelled);
          if (!loaded.ready()) {
            const std::string diagnostic = std::move(loaded.diagnostic);
            queueReplayLoadCompletion([this, diagnostic]() {
              (void)finishReplayLoadFailure(
                  "G-Battle failed", diagnostic,
                  "G-Battle replay could not be prepared.");
            });
            return;
          }
          if (cancelled->load()) {
            return;
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            context.jukebox.stop();
            context.jukebox.loadChart(*loaded.chart, true, *cancelled);
          }
          if (cancelled->load()) {
            return;
          }

          struct Completion {
            replay::ChartReplayConsumerOutcome loaded;
            result_persistence::ChartScoreWrite targetScore;
          };
          auto completion = std::make_shared<Completion>(
              Completion{.loaded = std::move(loaded),
                         .targetScore = modern.result.score});
          queueReplayLoadCompletion(
              [this, completion, gaugeType, gaugeAutoShift,
               gaugeAutoShiftLowerBound, autoKeySound, ruleset,
               playback]() mutable {
                auto &loaded = completion->loaded;
                if (!loaded.diagnostic.empty()) {
                  publishReplayLoadDiagnostic("G-Battle warning",
                                              loaded.diagnostic);
                }
                auto *chart =
                    setSelectedChart(std::move(loaded.chart), true, false);
                if (chart == nullptr) {
                  (void)finishReplayLoadFailure(
                      "G-Battle failed", {},
                      "Prepared replay chart is unavailable.");
                  return;
                }
                auto recordData = loaded.replayData;
                context.jukebox.stop();
                if (recordsModal_ != nullptr) recordsModal_->hide();
                changeToGameplayScene(
                    chart, {
                               .startPosition = 0,
                               .autoKeySound = autoKeySound,
                               .autoPlay = false,
                               .gaugeType = gaugeType,
                               .gaugeAutoShift = gaugeAutoShift,
                               .gaugeAutoShiftLowerBound =
                                   gaugeAutoShiftLowerBound,
                               .gbattleRecordData = recordData,
                               .targetScore = completion->targetScore,
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
              });
        } catch (...) {
          queueReplayLoadCompletion([this]() {
            (void)finishReplayLoadFailure(
                "G-Battle failed", {},
                "G-Battle replay could not be prepared.");
          });
        }
      });
}

void MainMenuScene::startModernCourseReplayPlayback(
    const ChartMetaRecord &record, ModernCourseResultRecord modern) {
  if (!record.courseStart || willStart.load()) {
    return;
  }
  auto currentSelection = currentCourseSelectionFor(modern.result);
  if (!currentSelection) {
    return;
  }
  auto chartPaths = std::move(currentSelection->completedChartPaths);

  willStart.store(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(true);
  const bool renderTouchPoints =
      recordsModal_ != nullptr ? recordsModal_->renderTouchPoints() : false;
  const bool renderGhosts =
      recordsModal_ != nullptr ? recordsModal_->renderReplayGhosts() : true;
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = true;
  }
  startReplayLoadWorker(
      [this, modern = std::move(modern), chartPaths = std::move(chartPaths),
       renderTouchPoints,
       renderGhosts](std::shared_ptr<std::atomic_bool> cancelled) mutable {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            pendingStopAndClearSelectedChartAfterPreview = false;
          }

          auto consumer =
              replay::makeRuntimeCourseReplayConsumer(context.replayRepository);
          auto loaded = consumer.load(modern, chartPaths, *cancelled);
          if (!loaded.ready()) {
            const std::string diagnostic = std::move(loaded.diagnostic);
            queueReplayLoadCompletion([this, diagnostic]() {
              (void)finishReplayLoadFailure(
                  "course Watch failed", diagnostic,
                  "Course replay playback could not be prepared.");
            });
            return;
          }
          if (cancelled->load()) {
            return;
          }
          const std::string warning = loaded.diagnostic;
          auto session = replay::makeCourseReplayLaunchSession(
              std::move(loaded), replay::CourseReplayLaunchMode::Watch,
              renderTouchPoints, renderGhosts);
          if (session == nullptr) {
            queueReplayLoadCompletion([this]() {
              (void)finishReplayLoadFailure(
                  "course Watch failed", {},
                  "Prepared course replay session is unavailable.");
            });
            return;
          }
          queueReplayLoadCompletion(
              [this, session = std::move(session), warning]() mutable {
                if (!warning.empty()) {
                  publishReplayLoadDiagnostic("course Watch warning", warning);
                }
                if (recordsModal_ != nullptr) recordsModal_->hide();
                startCourseReplayDirect(std::move(session));
                willStart.store(false);
              });
        } catch (...) {
          queueReplayLoadCompletion([this]() {
            (void)finishReplayLoadFailure(
                "course Watch failed", {},
                "Course replay playback could not be prepared.");
          });
        }
      });
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
  auto replayChart = session->takePreparedCourseChart(session->currentIndex);
  if (replayChart == nullptr) {
    replayChart = play_options::prepareReplayChart(
        stageReplay->chartMeta.BmsPath, *stageReplay, parseCancelled);
  }
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
  if (decideOverlay_ != nullptr) {
    decideOverlay_->setVisible(false);
  }
  refreshStartButtonForActiveFolder();
}

void MainMenuScene::resetReplayWatchLoadingUi() {
  willStart.store(false);
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(false);
}

void MainMenuScene::publishReplayLoadDiagnostic(
    const char *action, const std::string &diagnostic) const {
  const std::string safeDiagnostic = ir::sanitizeDiagnostic(diagnostic);
  if (safeDiagnostic.empty()) {
    return;
  }
  SDL_Log("Replay %s: %s", action, safeDiagnostic.c_str());
  archive_file::appendDebugLogLine("Replay " + std::string(action) + ": " +
                                   safeDiagnostic);
}

bool MainMenuScene::finishReplayLoadFailure(const char *action,
                                            std::string diagnostic,
                                            const char *fallback) {
  const std::string safeDiagnostic = replayDiagnosticOr(diagnostic, fallback);
  publishReplayLoadDiagnostic(action, safeDiagnostic);
  resetReplayWatchLoadingUi();
  if (recordsModal_ != nullptr) {
    recordsModal_->setStatus(safeDiagnostic);
    recordsModal_->reloadRecords(true);
  }
  if (replayStatusText != nullptr) {
    replayStatusText->setText(safeDiagnostic);
  }
  return true;
}

void MainMenuScene::startReplayLoadWorker(
    std::function<void(std::shared_ptr<std::atomic_bool>)> work) {
  if (replayLoadThread.joinable()) {
    replayLoadThread.join();
  }
  {
    std::lock_guard<std::mutex> lock(replayLoadCompletionMutex);
    pendingReplayLoadCompletion = {};
  }
  auto cancelled = std::make_shared<std::atomic_bool>(false);
  replayLoadCancelToken = cancelled;
  replayLoadInProgress.store(true, std::memory_order_release);
  replayLoadThread = std::jthread(
      [work = std::move(work), cancelled](std::stop_token stopToken) mutable {
        if (stopToken.stop_requested() || cancelled->load()) {
          return;
        }
        work(std::move(cancelled));
      });
}

void MainMenuScene::queueReplayLoadCompletion(
    std::function<void()> completion) {
  std::lock_guard<std::mutex> lock(replayLoadCompletionMutex);
  if (replayLoadCancelToken == nullptr || replayLoadCancelToken->load()) {
    return;
  }
  pendingReplayLoadCompletion = std::move(completion);
}

void MainMenuScene::applyReplayLoadCompletion() {
  std::function<void()> completion;
  {
    std::lock_guard<std::mutex> lock(replayLoadCompletionMutex);
    completion = std::move(pendingReplayLoadCompletion);
    pendingReplayLoadCompletion = {};
  }
  if (!completion) {
    return;
  }
  if (replayLoadThread.joinable()) {
    replayLoadThread.join();
  }
  replayLoadInProgress.store(false, std::memory_order_release);
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(false);
  completion();
}

void MainMenuScene::stopReplayLoadWorker() {
  if (replayLoadCancelToken != nullptr) {
    replayLoadCancelToken->store(true, std::memory_order_release);
  }
  if (replayLoadThread.joinable()) {
    replayLoadThread.request_stop();
    replayLoadThread.join();
  }
  replayLoadInProgress.store(false, std::memory_order_release);
  if (recordsModal_ != nullptr) recordsModal_->setLoadInProgress(false);
  std::lock_guard<std::mutex> lock(replayLoadCompletionMutex);
  pendingReplayLoadCompletion = {};
}

void MainMenuScene::changeToGameplayScene(bms_parser::Chart *chart,
                                          StartOptions options) {
  StartupTiming::instance().mark("main menu parse + jukebox ready, changing scene");
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
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  {
    std::lock_guard<std::mutex> lock(replayExportProgressMutex);
    pendingReplayExportProgress.reset();
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->showExportProgress(progressTitle, progressMessage);
    recordsModal_->setStatus(statusMessage);
  }
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

void MainMenuScene::startAutoPlayVideoExport(
    const ChartMetaRecord &record, ReplayVideoExportOptions options) {
  if (!beginReplayExport("Exporting Replay", "Preparing export",
                         "Exporting...")) {
    return;
  }

#if TARGET_OS_ANDROID
  options.progressCallback = [this](const ReplayVideoExportProgress &progress) {
    if (recordsModal_ != nullptr) {
      recordsModal_->updateExportProgress(progress.fraction, progress.message);
    }
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

  auto runExport = [this, record, options, complete,
                    autoPlayGaugeType, autoPlayGaugeAutoShift,
                    autoPlayGaugeAutoShiftLowerBound,
                    autoPlayAssistOption, autoPlayOption, autoPlayPlayback,
                    autoPlayClubMode, autoPlayRuleset, autoPlayLongNoteMode,
                    autoPlayRandomInfo](const std::stop_token *stopToken) {
    try {
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      {
        std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
        pendingStopAndClearSelectedChartAfterPreview = false;
      }
      context.jukebox.stop();
      if (stopToken != nullptr && stopToken->stop_requested()) {
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }

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
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }

      play_options::PlayOptionReplayInfo playInfo =
          play_options::applySelectedPlayOptions(*chart, autoPlayOption);
      applyEffectiveLongNoteModeToChart(*chart, autoPlayLongNoteMode);
      ReplayData replay = replay_autoplay::BuildReplayData(
          *chart, autoPlayGaugeType, autoPlayGaugeAutoShift, autoPlayPlayback,
          playInfo.option, playInfo.seed, playInfo.option2, playInfo.seed2,
          autoPlayAssistOption, autoPlayClubMode,
          autoPlayGaugeAutoShiftLowerBound, autoPlayRuleset);
      ReplayVideoExportOptions exportOptions = options;
      if (stopToken != nullptr) {
        exportOptions.stop = *stopToken;
      }
      exportOptions.renderTouchPoints = false;
      exportOptions.renderReplayGhosts = false;
      exportOptions.pacemakerTarget = pacemaker::kTargetOff;
      complete(ReplayVideoExporter::Export(context, chart.get(), replay,
                                           exportOptions));
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

void MainMenuScene::startModernReplayVideoExport(
    const ChartMetaRecord &record, ModernChartResultRecord modern,
    ReplayVideoExportOptions options) {
  if (!beginReplayExport("Exporting Replay", "Preparing export",
                         "Exporting...")) {
    return;
  }

#if TARGET_OS_ANDROID
  options.progressCallback = [this](const ReplayVideoExportProgress &progress) {
    if (recordsModal_ != nullptr) {
      recordsModal_->updateExportProgress(progress.fraction, progress.message);
    }
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
  auto runExport = [this, record, modern = std::move(modern), options,
                    complete](const std::stop_token *stopToken) mutable {
    try {
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      {
        std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
        pendingStopAndClearSelectedChartAfterPreview = false;
      }
      context.jukebox.stop();
      if (stopToken != nullptr && stopToken->stop_requested()) {
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }

      std::atomic_bool parseCancelled = false;
      auto consumer = replay::makeRuntimeChartReplayConsumer(
          context.replayRepository);
      auto loaded = consumer.load(modern, record.meta.BmsPath,
                                  parseCancelled);
      if (parseCancelled) {
        complete({.success = false,
                  .message = "Replay export preparation was cancelled."});
        return;
      }
      if (!loaded.ready()) {
        complete({.success = false,
                  .message = replayDiagnosticOr(
                      loaded.diagnostic,
                      "Replay export playback could not be prepared.")});
        return;
      }
      if (!loaded.diagnostic.empty()) {
        publishReplayLoadDiagnostic("video export warning",
                                    loaded.diagnostic);
      }
      if (stopToken != nullptr && stopToken->stop_requested()) {
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }

      ReplayVideoExportOptions exportOptions = options;
      if (stopToken != nullptr) {
        exportOptions.stop = *stopToken;
      }
      complete(ReplayVideoExporter::Export(
          context, loaded.chart.get(), *loaded.replayData, exportOptions));
    } catch (const std::exception &e) {
      complete({.success = false, .message = e.what()});
    } catch (...) {
      complete({.success = false,
                .message = "Unexpected replay export failure"});
    }
  };

#if TARGET_OS_ANDROID
  runExport(nullptr);
  applyReplayExportResult();
#else
  replayExportThread = std::jthread(
      [runExport = std::move(runExport)](const std::stop_token &stopToken) mutable {
        runExport(&stopToken);
      });
#endif
}

void MainMenuScene::startModernCourseReplayVideoExport(
    ModernCourseResultRecord modern, ReplayVideoExportOptions options) {
  auto currentSelection = currentCourseSelectionFor(modern.result);
  if (!currentSelection) {
    return;
  }
  auto chartPaths = std::move(currentSelection->completedChartPaths);
  if (!beginReplayExport("Exporting Course Replay", "Preparing export",
                         "Exporting...")) {
    return;
  }

#if TARGET_OS_ANDROID
  options.progressCallback = [this](const ReplayVideoExportProgress &progress) {
    if (recordsModal_ != nullptr) {
      recordsModal_->updateExportProgress(progress.fraction, progress.message);
    }
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
  auto runExport = [this, modern = std::move(modern), options, complete,
                    chartPaths = std::move(chartPaths)](
                       const std::stop_token *stopToken) mutable {
    try {
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      {
        std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
        pendingStopAndClearSelectedChartAfterPreview = false;
      }
      context.jukebox.stop();
      if (stopToken != nullptr && stopToken->stop_requested()) {
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }

      std::atomic_bool cancelled = false;
      auto consumer = replay::makeRuntimeCourseReplayConsumer(
          context.replayRepository);
      auto loaded = consumer.load(modern, chartPaths, cancelled);
      if (cancelled) {
        complete({.success = false,
                  .message =
                      "Course replay export preparation was cancelled."});
        return;
      }
      if (!loaded.ready()) {
        complete({.success = false,
                  .message = replayDiagnosticOr(
                      loaded.diagnostic,
                      "Course replay export playback could not be prepared.")});
        return;
      }
      if (!loaded.diagnostic.empty()) {
        publishReplayLoadDiagnostic("course video export warning",
                                    loaded.diagnostic);
      }
      if (stopToken != nullptr && stopToken->stop_requested()) {
        complete({.success = false, .message = "Replay export cancelled"});
        return;
      }
      ReplayVideoExportOptions exportOptions = options;
      if (stopToken != nullptr) {
        exportOptions.stop = *stopToken;
      }
      complete(ReplayVideoExporter::ExportCourseReplay(
          context, std::move(loaded), exportOptions));
    } catch (const std::exception &e) {
      complete({.success = false, .message = e.what()});
    } catch (...) {
      complete({.success = false,
                .message = "Unexpected course replay export failure"});
    }
  };

#if TARGET_OS_ANDROID
  runExport(nullptr);
  applyReplayExportResult();
#else
  replayExportThread = std::jthread(
      [runExport = std::move(runExport)](
          const std::stop_token &stopToken) mutable { runExport(&stopToken); });
#endif
}

std::optional<std::string>
MainMenuScene::activeReplayIrServerOrigin() const {
  const auto settings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (settings == context.settings.irProviders.end()) {
    return std::string(ir::kDefaultTachiServerOrigin);
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

  if (recordsModal_ != nullptr) {
    recordsModal_->showIrFeedback(message);
  }
}

void MainMenuScene::observeReplayIrServiceRevisions() {
  if (recordsModal_ == nullptr || !recordsModal_->isVisible() ||
      recordsModal_->records().empty() ||
      context.irSubmissionService == nullptr) {
    return;
  }

  bool reload = false;
  for (const ResultRecordSummary &summary : recordsModal_->records()) {
    if (!summary.modern.has_value()) {
      continue;
    }
    const std::string &attemptId = summary.modern->result.attemptId;
    const auto status = context.irSubmissionService->status(
        ir::kTachiProviderId, attemptId);
    const auto observed = replayIrObservedRevisions.find(attemptId);
    if (observed != replayIrObservedRevisions.end() &&
        observed->second == status.revision) {
      continue;
    }
    replayIrObservedRevisions[attemptId] = status.revision;
    reload = true;
  }
  if (reload) {
    recordsModal_->reloadRecords(true);
  }
}

void MainMenuScene::startModernReplayIrUpload(
    ModernChartResultRecord modern) {
  if (replayIrUploadInProgress || replayResultRecallInProgress ||
      replayExportInProgress.load()) {
    return;
  }

  const auto providerSettings = context.settings.irProviders.find(
      std::string(ir::kTachiProviderId));
  if (providerSettings == context.settings.irProviders.end() ||
      !providerSettings->second.enabled) {
    finishReplayIrUpload(
        modern.result.attemptId,
        "Enable Bokutachi in Settings > IR before uploading.");
    return;
  }
  const auto driver = context.irDrivers.find(ir::kTachiProviderId);
  if (driver == nullptr) {
    finishReplayIrUpload(modern.result.attemptId,
                         "Bokutachi IR is unavailable.");
    return;
  }
  const ir::IrDriverCapabilities capabilities = driver->capabilities();
  if (capabilities.readOnly || !capabilities.scoreSubmission ||
      context.irSubmissionService == nullptr) {
    finishReplayIrUpload(modern.result.attemptId,
                         "Bokutachi score submission is unavailable.");
    return;
  }

  replayIrUploadInProgress = true;
  if (recordsModal_ != nullptr) {
    recordsModal_->setIrUploadInProgress(true);
    recordsModal_->showIrFeedback("Preparing IR...");
  }
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }

  defer(
      [this, modern = std::move(modern)]() {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            pendingStopAndClearSelectedChartAfterPreview = false;
          }
          const auto snapshot =
              context.replayRepository.LoadModernIrSubmissionSnapshot(
                  modern.result.attemptId);
          if (snapshot.status != ModernIrSnapshotReadStatus::Loaded ||
              !snapshot.snapshot.has_value()) {
            finishReplayIrUpload(
                modern.result.attemptId,
                "This saved result has no verified IR snapshot.");
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
              ir::kTachiProviderId, snapshot.snapshot->submission,
              dependencies);
          finishReplayIrUpload(modern.result.attemptId, action.message);
        } catch (...) {
          finishReplayIrUpload(modern.result.attemptId,
                               "IR upload could not be prepared.");
        }
        return true;
      },
      1, true);
}

void MainMenuScene::finishReplayIrUpload(std::string attemptId,
                                         std::string message) {
  replayIrUploadInProgress = false;
  std::string safeMessage = ir::sanitizeDiagnostic(message);
  if (safeMessage.empty()) {
    safeMessage = "IR upload could not be queued.";
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->setIrUploadInProgress(false);
    if (!attemptId.empty()) {
      recordsModal_->reloadRecords(true);
    }
    recordsModal_->showIrFeedback(safeMessage);
  }
}

void MainMenuScene::startModernReplayResultRecall(
    const ChartMetaRecord &record, ModernChartResultRecord modern) {
  if (record.courseStart || replayResultRecallInProgress ||
      replayExportInProgress.load() || replayIrUploadInProgress) {
    return;
  }

  replayResultRecallInProgress = true;
  if (recordsModal_ != nullptr) recordsModal_->setResultRecallInProgress(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = true;
  }
  startReplayLoadWorker(
      [this, record, modern = std::move(modern)](
          std::shared_ptr<std::atomic_bool> cancelled) mutable {
        try {
          if (previewWorker_ != nullptr) {
            previewWorker_->stop();
          }
          {
            std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
            pendingStopAndClearSelectedChartAfterPreview = false;
          }
          const auto exact = context.replayRepository
                                 .LoadModernChartResultByAttempt(
                                     modern.result.attemptId);
          if (exact.status != ModernChartResultReadStatus::Loaded ||
              !exact.record.has_value()) {
            const std::string diagnostic =
                exact.diagnostic.empty() ? "saved result was not found"
                                         : exact.diagnostic;
            queueReplayLoadCompletion([this, diagnostic]() {
              finishReplayResultRecallFailure(diagnostic);
            });
            return;
          }

          auto consumer = replay::makeRuntimeChartReplayConsumer(
              context.replayRepository);
          auto replayLoad = consumer.load(*exact.record, record.meta.BmsPath,
                                          *cancelled);
          if (cancelled->load()) {
            return;
          }

          std::shared_ptr<ReplayData> retryData;
          result_recall::ModernChartLoader preparedChartLoader;
          if (replayLoad.ready()) {
            retryData = std::move(replayLoad.replayData);
            auto preparedChart =
                std::make_shared<std::unique_ptr<bms_parser::Chart>>(
                    std::move(replayLoad.chart));
            preparedChartLoader =
                [preparedChart](const std::filesystem::path &,
                                std::atomic_bool &) mutable {
                  return std::move(*preparedChart);
                };
          }

          const std::filesystem::path currentChartPath = record.meta.BmsPath;
          auto recalled = result_recall::BuildChartResult(
              exact.record->result, *cancelled, currentChartPath,
              std::move(preparedChartLoader));
          if (!recalled.value.has_value()) {
            const std::string diagnostic =
                recalled.diagnostic.empty() ? "saved result was not found"
                                             : recalled.diagnostic;
            queueReplayLoadCompletion([this, diagnostic]() {
              finishReplayResultRecallFailure(diagnostic);
            });
            return;
          }
          if (cancelled->load()) {
            return;
          }

          struct Completion {
            result_recall::ModernChartResultView view;
            std::shared_ptr<ReplayData> retryData;
          };
          auto completion = std::make_shared<Completion>(Completion{
              .view = std::move(*recalled.value),
              .retryData = std::move(retryData),
          });
          queueReplayLoadCompletion([this, completion]() mutable {
            auto &result = completion->view;
            auto chart = std::move(result.chart);
            const bms_parser::ChartMeta meta = chart->Meta;
            const std::string attemptId = result.result.attemptId;
            const ScoreProvenance provenance = result.result.score.provenance;
            const SkinGameplayGraphState gameplayGraph =
                completion->retryData != nullptr
                    ? replay_result::BuildSkinGameplayGraphState(
                          *chart, *completion->retryData, result.state)
                    : replay_result::BuildSkinGameplayChartGraphState(
                          *chart, result.state);
            replayResultRecallInProgress = false;
            context.sceneManager->changeScene(
                std::make_unique<ResultScene>(
                    context, meta, result.state, provenance, nullptr,
                    ResultPersistenceOptions{}, completion->retryData.get(),
                    ResultPracticeOptions{}, false, ResultCourseOptions{},
                    profileSelections.pacemakerTarget, std::move(chart),
                    nullptr, std::nullopt, completion->retryData.get(),
                    attemptId, completion->retryData != nullptr,
                    ResultTableContext{}, gameplayGraph,
                    result.result.playedAtUnixMillis),
                true);
          });
        } catch (...) {
          queueReplayLoadCompletion([this]() {
            finishReplayResultRecallFailure(
                "saved chart result could not be recalled");
          });
        }
      });
}

void MainMenuScene::startModernCourseReplayResultRecall(
    ModernCourseResultRecord modern, bool retrySameAllowed) {
  if (replayResultRecallInProgress || replayExportInProgress.load() ||
      replayIrUploadInProgress) {
    return;
  }

  auto currentSelection = currentCourseSelectionFor(modern.result);

  replayResultRecallInProgress = true;
  if (recordsModal_ != nullptr) recordsModal_->setResultRecallInProgress(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = true;
  }
  startReplayLoadWorker([this, modern = std::move(modern), retrySameAllowed,
                         currentSelection = std::move(currentSelection)](
                            std::shared_ptr<std::atomic_bool>
                                cancelled) mutable {
    try {
      if (previewWorker_ != nullptr) {
        previewWorker_->stop();
      }
      {
        std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
        pendingStopAndClearSelectedChartAfterPreview = false;
      }
      const auto exact =
          context.replayRepository.LoadModernCourseResultByAttempt(
              modern.result.attemptId);
      if (exact.status != ModernCourseResultReadStatus::Loaded ||
          !exact.record.has_value()) {
        const std::string diagnostic = exact.diagnostic.empty()
                                           ? "saved course result was not found"
                                           : exact.diagnostic;
        queueReplayLoadCompletion([this, diagnostic]() {
          finishReplayResultRecallFailure(diagnostic);
        });
        return;
      }
      if (!currentSelection || currentSelection->completedChartPaths.size() !=
                                   exact.record->result.stages.size()) {
        queueReplayLoadCompletion([this]() {
          finishReplayResultRecallFailure(
              "current course charts are unavailable");
        });
        return;
      }
      auto recalled = result_recall::BuildCourseResult(
          exact.record->result, *cancelled,
          currentSelection->completedChartPaths);
      if (cancelled->load()) {
        return;
      }
      if (!recalled.value.has_value() ||
          recalled.value->completedStages.empty()) {
        const std::string diagnostic = recalled.diagnostic.empty()
                                           ? "saved course result was not found"
                                           : recalled.diagnostic;
        queueReplayLoadCompletion([this, diagnostic]() {
          finishReplayResultRecallFailure(diagnostic);
        });
        return;
      }

      auto view = std::move(*recalled.value);
      std::shared_ptr<CourseReplayData> resultBrowseReplayData;
      std::vector<std::shared_ptr<bms_parser::Chart>> resultBrowseReplayCharts;
      if (exact.record->replayFile) {
        auto consumer =
            replay::makeRuntimeCourseReplayConsumer(context.replayRepository);
        auto replay = consumer.load(*exact.record,
                                    currentSelection->completedChartPaths,
                                    *cancelled);
        if (replay.ready() && replay.replayData != nullptr &&
            replay.replayData->stages.size() == view.completedStages.size() &&
            replay.charts.size() == view.completedStages.size()) {
          resultBrowseReplayData = std::move(replay.replayData);
          resultBrowseReplayCharts.reserve(replay.charts.size());
          for (auto &chart : replay.charts) {
            resultBrowseReplayCharts.emplace_back(std::move(chart));
          }
        }
      }
      if (cancelled->load()) {
        return;
      }
      auto session = std::make_shared<CoursePlaySession>();
      session->courseId = view.result.legacyCourseId;
      session->courseKey = view.result.courseKey;
      session->courseName = view.result.courseName;
      session->courseGroupName = view.result.courseGroupName;
      session->constraintJson = view.result.constraintJson;
      session->entries.resize(
          static_cast<std::size_t>(view.result.totalCharts));
      for (std::size_t index = 0; index < session->entries.size(); ++index) {
        session->entries[index].meta.TotalNotes =
            view.result.entryFacts[index].totalNotes;
        session->entries[index].meta.PlayLength =
            view.result.entryFacts[index].playLengthMicros;
      }
      for (std::size_t index = 0; index < currentSelection->records.size() &&
                                  index < session->entries.size();
           ++index) {
        session->entries[index].meta = currentSelection->records[index].meta;
      }
      session->stageProvenance.resize(view.completedStages.size());
      session->completedResults.reserve(view.completedStages.size());
      session->ownedResultBrowseCharts.reserve(view.completedStages.size());
      session->ownedResultBrowseReplayCharts =
          std::move(resultBrowseReplayCharts);
      session->resultBrowseReplayData = std::move(resultBrowseReplayData);
      session->modernCourseChartPaths.reserve(view.completedStages.size());
      for (std::size_t index = 0; index < view.completedStages.size();
           ++index) {
        auto &stage = view.completedStages[index];
        session->entries[index].meta = stage.chart->Meta;
        const ReplayData *stageReplay = session->resultBrowseStageReplay(index);
        bms_parser::Chart *replayChart =
            session->resultBrowseReplayChart(index);
        session->completedResults.emplace_back(
            stage.chart->Meta, stage.state,
            stageReplay != nullptr && replayChart != nullptr
                ? replay_result::BuildSkinGameplayGraphState(
                      *replayChart, *stageReplay, stage.state)
                : replay_result::BuildSkinGameplayChartGraphState(
                      *stage.chart, stage.state));
        session->ownedResultBrowseCharts.push_back(stage.chart);
        session->stageProvenance[index] = stage.result.score.provenance;
        session->modernCourseChartPaths.push_back(stage.chart->Meta.BmsPath);
      }
      session->modernCourseAttemptId = view.result.attemptId;
      session->modernCoursePlayedAtUnixMillis = view.result.playedAtUnixMillis;
      session->modernCourseResultBrowsing = true;
      session->restoreFinalClearTypeForResult(view.result.clearType);
      session->modernCourseRetrySameAllowed =
          retrySameAllowed && currentSelection->completeCourse;
      session->gaugeType = view.result.initialGaugeType;
      session->gaugeProfile = view.result.gaugeProfile;
      session->gaugeAutoShift = view.result.gaugeAutoShift;
      session->gaugeAutoShiftLowerBound = view.result.gaugeAutoShiftLowerBound;
      session->longNoteMode = view.result.longNoteMode;
      session->requestedPlayOption = view.result.requestedPlayOption;
      session->assistOption = view.result.assistOption;
      session->constraints =
          courseConstraintSettingsFromJson(view.result.constraintJson).rules;
      session->maxCombo = view.result.maxCombo;
      session->carriedGauge =
          session->completedResults.back().state.gaugeSnapshot();
      if (const auto ruleset =
              gameplayRulesetFromId(view.result.provenance.ruleset.id)) {
        session->ruleset = *ruleset;
        session->rulesetDescriptor = view.result.provenance.ruleset;
      }

      queueReplayLoadCompletion([this, session = std::move(session)]() {
        context.jukebox.stop();
        const auto &first = session->completedResults.front();
        const ScoreProvenance firstProvenance =
            *session->stageProvenance.front();
        const ReplayData *firstReplay = session->resultBrowseStageReplay(0);
        bms_parser::Chart *firstReplayChart =
            session->resultBrowseReplayChart(0);
        if (firstReplay != nullptr) {
          session->applyReplayStagePlayOptions(*firstReplay);
        }
        replayResultRecallInProgress = false;
        context.sceneManager->changeScene(
            std::make_unique<ResultScene>(
                context, first.meta, first.state, firstProvenance, firstReplay,
                ResultPersistenceOptions{}, nullptr, ResultPracticeOptions{},
                false,
                ResultCourseOptions{.mode = ResultCourseMode::Stage,
                                    .session = session,
                                    .savedResultBrowsing = true},
                profileSelections.pacemakerTarget,
                std::unique_ptr<bms_parser::Chart>{}, firstReplayChart,
                std::nullopt, firstReplay, std::nullopt, true,
                ResultTableContext{}, first.gameplayGraph,
                session->modernCoursePlayedAtUnixMillis),
                true);
      });
    } catch (...) {
      queueReplayLoadCompletion([this]() {
        finishReplayResultRecallFailure(
            "saved course result could not be recalled");
      });
    }
  });
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
  if (recordsModal_ != nullptr) recordsModal_->setResultRecallInProgress(true);
  if (previewWorker_ != nullptr) {
    previewWorker_->cancel();
  }
  defer(
      [this, identity = std::move(identity),
       selectedStableKey = std::move(selectedStableKey)]() {
        if (previewWorker_ != nullptr) {
          previewWorker_->stop();
        }
        {
          std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
          pendingStopAndClearSelectedChartAfterPreview = false;
        }

        RemoteResultRecallRequest request{
            .identity = std::move(identity),
            .selectedStableKey = std::move(selectedStableKey),
        };
        RemoteResultRecallCallbacks callbacks{
            .selectionStillMatches = [this](const RemoteResultRecallRequest &request) {
              return remoteResultRecallSelectionMatches(
                  recordsModal_ != nullptr ? recordsModal_->selection()
                                           : std::optional<ResultRecordSummary>{},
                  request);
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

void MainMenuScene::finishReplayResultRecallFailure(std::string diagnostic) {
  const std::string safeDiagnostic = ir::sanitizeDiagnostic(diagnostic);
  SDL_Log("Saved result recall failed: %s",
          safeDiagnostic.empty() ? "result unavailable"
                                 : safeDiagnostic.c_str());
  if (recordsModal_ != nullptr) {
    recordsModal_->setResultRecallInProgress(false);
    if (!safeDiagnostic.empty()) {
      recordsModal_->setStatus(safeDiagnostic);
    }
  }
  replayResultRecallInProgress = false;
}

void MainMenuScene::finishRemoteResultRecallFailure(std::string diagnostic) {
  if (recordsModal_ != nullptr) {
    recordsModal_->reloadRecords(true);
  }
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

  if (recordsModal_ != nullptr) {
    recordsModal_->updateExportProgress(progress->fraction, progress->message);
  }
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
    } else if (result->message == "No Chart") {
      replayStatusText->setText("No Chart");
    } else {
      replayStatusText->setText(replayDiagnosticOr(
          result->message, "Replay export failed."));
    }
  }
  if (recordsModal_ != nullptr) {
    recordsModal_->setExportInProgress(false);
    recordsModal_->returnToList(
        result->success
            ? (result->message == "Saved to Photos" ? "Saved" : "Exported")
            : (result->message == "No Chart"
                   ? "No Chart"
                   : replayDiagnosticOr(result->message,
                                        "Replay export failed.")));
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
  refreshScoreClearRanksIfNeeded();
  refreshIrRecordListIfNeeded();
  refreshTasksButton();
  applyPendingUiUpdates();
  applyFindBmsUpdates();
  applyUnzipProgress();
  applyUnzipResult();
  applyReplayLoadCompletion();
  applyReplayExportProgress();
  applyReplayExportResult();
  applyReplayFileDocumentHandoff();
  applyParseLogDocumentHandoff();
  observeReplayIrServiceRevisions();
  if (recordsModal_ != nullptr) {
    recordsModal_->update();
  }
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
  if (recordsModal_ != nullptr) {
    recordsModal_->resize(rendering::window_width, rendering::window_height);
  }
  if (playOptionsModalRoot != nullptr) {
    playOptionsModalRoot->setSize(rendering::window_width,
                                  rendering::window_height);
  }
  if (overlayPortal != nullptr) {
    overlayPortal->setSize(rendering::window_width, rendering::window_height);
  }
  if (revealContextMenu != nullptr) {
    revealContextMenu->setViewportSize(rendering::window_width,
                                       rendering::window_height);
    if (layoutChanged && revealContextMenu->isOpen()) {
      revealContextMenu->dismiss();
    }
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
  revealContextMenu.reset();
  rankingsModal.reset();
  playOptionsModal.reset();
  replayFileDocumentHandoff.close();
  parseLogDocumentHandoff.close();
  if (previewWorker_ != nullptr) {
    previewWorker_->stop();
  }
  {
    std::lock_guard<std::mutex> lock(previewJukeboxLoadMutex);
    pendingStopAndClearSelectedChartAfterPreview = false;
  }
  stopReplayLoadWorker();
  context.profileSwitchBlockers.scene = nullptr;
  context.profileSwitchBlockers.background = nullptr;
  context.refreshProfileCaches = nullptr;
  if (replayExportThread.joinable()) {
    SDL_Log("Joining replayExportThread");
    replayExportThread.request_stop();
    replayExportThread.join();
  }
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
  stopAndClearSelectedChart();
  selectedChartRecord.reset();
  chartListCache.clear();
  chartListCache.session = nullptr;
  chartSession.reset();
  recyclerView = nullptr;
  folderRecyclerView = nullptr;
  temporaryChartFolder.reset();
  rootLayout = nullptr;
  if (decideOverlay_ != nullptr) {
    overlayPortal->dismiss(decideOverlay_);
    delete decideOverlay_;
    decideOverlay_ = nullptr;
  }
  if (previewWorker_ != nullptr) {
    delete previewWorker_;
    previewWorker_ = nullptr;
  }
  overlayPortal = nullptr;
  jacketView = nullptr;
  searchBox = nullptr;
  startButton = nullptr;
  rankingsButton = nullptr;
  rankingsButtonText = nullptr;
  chartActionsRow = nullptr;
  revealButton = nullptr;
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
  recordsModal_.reset();
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
  parseLogExportStatusText = nullptr;
  parseLogExportButton = nullptr;
  parseLogExportButtonText = nullptr;
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
  pendingReplayExportResult.reset();
  pendingReplayExportProgress.reset();
  pendingUnzipResult.reset();
  pendingUnzipProgress.reset();
  pendingSelectChartPath.reset();
  {
    std::lock_guard<std::mutex> lock(findBmsSelectionHandoffMutex);
    pendingFindBmsSelectionHandoff.reset();
  }
  suppressPreviewForChartPath.reset();
  unzipDeleteCandidatePath.reset();
  unzipEstimatedUncompressedSize = 0;
  pendingFindBmsProgressEvents.clear();
  pendingFindBmsResult.reset();
  chartSelectionGeneration = 0;
  findBmsSelectionGenerationAtDownloadStart = 0;
  replayExportInProgress = false;
  replayResultRecallInProgress = false;
  replayIrUploadInProgress = false;
  replayIrObservedRevisions.clear();
  unzipInProgress = false;
  findBmsJobRunning = false;
  findBmsCancelled = false;
  findBmsResult = {};
  findBmsProgressMessage.clear();
  findBmsProgressCurrent = 0;
  findBmsProgressTotal = 0;
  findBmsProgressFraction = 0.0;
  findBmsProgressLog.clear();
  displayedLibraryTasksRevision = 0;
  displayedLibraryProgressRevision = 0;
  displayedLibraryTasksButtonText.clear();
  selectedChartMediaReady.store(false);
  selectedChartReusableForStart.store(false);
  publishedResultRecordDiagnostic.clear();
  playOptionsPanel = nullptr;
  lastLayoutWidth = -1;
  lastLayoutHeight = -1;
  lastSafeTop = -1;
  lastSafeLeft = -1;
  lastSafeBottom = -1;
  lastSafeRight = -1;
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
