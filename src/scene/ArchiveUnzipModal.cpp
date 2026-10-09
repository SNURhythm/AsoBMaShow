#include "../i18n/Localization.h"
#include "ArchiveUnzipModal.h"
#include "ArchiveUnzipPresentation.h"

#include "FindBmsProgressPresentation.h"
#include "../rendering/common.h"
#include "../view/BlockingOverlayView.h"
#include "../view/Button.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <utility>

namespace {

Button *makeModalButton(const i18n::Text &label, int fontSize,
                        TextView **textOut = nullptr) {
  auto *button = new Button(0, 0, 160, 58);
  auto *text = new TextView("assets/fonts/notosanscjkjp.ttf", fontSize);
  text->setLocalizedText(label);
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

}

std::unique_ptr<ArchiveUnzipModal> ArchiveUnzipModal::Create(
    View *parent, ChartRepository &repository,
    ArchiveUnzipModalCallbacks callbacks) {
  if (parent == nullptr) {
    return nullptr;
  }
  auto modal = std::unique_ptr<ArchiveUnzipModal>(
      new ArchiveUnzipModal(repository, std::move(callbacks)));
  modal->build(parent);
  return modal;
}

ArchiveUnzipModal::ArchiveUnzipModal(
    ChartRepository &repository, ArchiveUnzipModalCallbacks callbacks)
    : operation_(repository), callbacks_(std::move(callbacks)) {}

ArchiveUnzipModal::~ArchiveUnzipModal() {
  cancelAndWait();
  if (cancelButton_ != nullptr) {
    cancelButton_->setOnClickListener(nullptr);
  }
  if (deleteButton_ != nullptr) {
    deleteButton_->setOnClickListener(nullptr);
  }
  if (keepButton_ != nullptr) {
    keepButton_->setOnClickListener(nullptr);
  }
}

void ArchiveUnzipModal::build(View *parent) {
  constexpr float kModalPanelWidth = 700.0f;
  constexpr float kModalPanelPadding = 22.0f;
  constexpr float kModalContentWidth =
      kModalPanelWidth - kModalPanelPadding * 2.0f;

  root_ = new BlockingOverlayView(0, 0, rendering::window_width,
                                  rendering::window_height);
  root_->setPositionType(YGPositionTypeAbsolute);
  root_->setPosition(Edge::Left, 0);
  root_->setPosition(Edge::Top, 0);
  root_->setZIndex(1000);
  root_->setVisible(false);
  root_->setFlexDirection(FlexDirection::Column);
  root_->setAlignItems(YGAlignCenter);
  root_->setJustifyContent(YGJustifyCenter);
  root_->setThemedBackgroundColor(ui_theme::scrim);
  parent->addView(root_);

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
  root_->addView(panel);

  title_ = new TextView("assets/fonts/notosanscjkjp.ttf", 30);
  title_->setLocalizedText(i18n::message("library.archive.unzip.label"));
  title_->setThemedColor(ui_theme::textPrimary);
  title_->setHeight(42);
  panel->addView(title_);

  message_ = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  message_->setThemedColor(ui_theme::textSecondary);
  message_->setMinHeight(32);
  message_->setWidthPercent(100);
  message_->setWrap(true);
  panel->addView(message_);

  track_ = new View();
  track_->setWidth(kModalContentWidth)
      ->setHeight(24)
      ->setThemedBackgroundColor(ui_theme::progressTrack)
      ->setCornerRadius(ui_theme::controlRadius())
      ->setThemedBorderColor(ui_theme::hairline)
      ->setBorderWidth(1);
  fill_ = new View();
  fill_->setWidth(0)->setHeight(20)->setBackgroundColor(ui_theme::progressFill());
  track_->addView(fill_);
  panel->addView(track_);

  percent_ = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  percent_->setThemedColor(ui_theme::textSecondary);
  percent_->setHeight(28);
  panel->addView(percent_);

  detail_ = new TextView("assets/fonts/notosanscjkjp.ttf", 18);
  detail_->setThemedColor(ui_theme::textMuted);
  detail_->setMinHeight(54);
  detail_->setWidthPercent(100);
  detail_->setWrap(true);
  panel->addView(detail_);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  keepButton_ = makeModalButton(i18n::message("library.archive.keep_archives.label"), 18);
  keepButton_->setVisible(false);
  keepButton_->setWidth(0)->setHeight(0);
  keepButton_->setOnClickListener([this]() { beginAll(false); });
  footer->addView(keepButton_);

  deleteButton_ = makeModalButton(i18n::message("library.archive.delete_archive.label"), 18, &deleteText_);
  deleteButton_->setVisible(false);
  deleteButton_->setWidth(0)->setHeight(0);
  deleteButton_->setOnClickListener([this]() {
    if (choosingAll_) {
      beginAll(true);
    } else {
      deleteArchive();
    }
  });
  footer->addView(deleteButton_);

  cancelButton_ = makeModalButton(i18n::message("library.archive.cancel.label"), 20, &cancelText_);
  cancelButton_->setWidth(130);
  cancelButton_->setOnClickListener([this]() { cancelOrClose(); });
  footer->addView(cancelButton_);
  panel->addView(footer);
}

bool ArchiveUnzipModal::start(const ChartMetaRecord &record) {
  if (choosingAll_ || !operation_.start(record)) {
    return false;
  }
  estimatedSize_ = record.archiveUncompressedSize;
  cancelling_ = false;
  batchMode_ = false;
  indexing_ = false;
  cancelButton_->setEnabled(true);
  setAllChoiceVisible(false);
  message_->setMinHeight(32);
  resize(rendering::window_width, rendering::window_height);
  root_->setVisible(true);
  title_->setLocalizedText(i18n::message("library.archive.unzip.label"));
  cancelText_->setLocalizedText(i18n::message("library.archive.cancel.label"));
  setDeleteVisible(false);
  updateProgress(0.0, i18n::message("library.archive.preparing_unzip.label"));
  return true;
}

bool ArchiveUnzipModal::startAll() {
  if (inProgress()) {
    return false;
  }
  operation_.keepArchive();
  choosingAll_ = true;
  batchMode_ = true;
  indexing_ = false;
  cancelButton_->setEnabled(true);
  cancelling_ = false;
  estimatedSize_ = 0;
  resize(rendering::window_width, rendering::window_height);
  root_->setVisible(true);
  title_->setLocalizedText(i18n::message("library.archive.unzip_all.label"));
  message_->setLocalizedText(i18n::message("library.archive.originals.disposition_help"));
  message_->setMinHeight(64);
  detail_->setLocalizedText(i18n::message("library.archive.delete_originals.warning"));
  cancelText_->setLocalizedText(i18n::message("library.archive.cancel.label"));
  setAllChoiceVisible(true);
  root_->applyYogaLayout();
  return true;
}

void ArchiveUnzipModal::beginAll(bool deleteAfterUnzip) {
  if (!choosingAll_) {
    return;
  }
  if (!operation_.startAll(deleteAfterUnzip)) {
    message_->setLocalizedText(i18n::message("library.archive.could_not_start_unzip_all_original_archives_kept.message"));
    return;
  }
  choosingAll_ = false;
  setAllChoiceVisible(false);
  updateProgress(0.0, i18n::message("library.archive.finding_solid_archives.label"));
}

bool ArchiveUnzipModal::inProgress() const {
  return choosingAll_ || operation_.inProgress();
}

bool ArchiveUnzipModal::isVisible() const {
  return root_ != nullptr && root_->getVisible();
}

View *ArchiveUnzipModal::root() const { return root_; }

void ArchiveUnzipModal::update() {
  if (const auto progress = operation_.takeProgress();
      progress && (!cancelling_ || (batchMode_ && progress->indexing))) {
    if (batchMode_ && progress->indexing) {
      indexing_ = true;
      cancelButton_->setEnabled(false);
      cancelText_->setLocalizedText(i18n::message("library.archive.indexing.progress"));
    }
    const auto progressMessage = archive_unzip_presentation::progressMessage(
        *progress, batchMode_);
    updateProgress(progress->fraction, progressMessage,
                   progress->current, progress->total);
  }
  const auto result = operation_.takeResult();
  if (result) {
    cancelling_ = false;
    indexing_ = false;
    cancelButton_->setEnabled(true);
    title_->setLocalizedText(result->success ? i18n::message("library.archive.unzip_complete.label")
                      : result->cancelled ? i18n::message("library.archive.unzip_cancelled.label") : i18n::message("library.archive.unzip_failed.label"));
    if (result->batch) {
      title_->setLocalizedText(result->success ? i18n::message("library.archive.unzip_all_complete.label")
                        : result->cancelled ? i18n::message("library.archive.unzip_all_cancelled.label")
                                            : i18n::message("library.archive.unzip_all_finished_errors.label"));
      message_->setMinHeight(128);
    }
    updateProgress(result->success ? 1.0 : 0.0, result->message);
    const bool canDelete = operation_.canDeleteArchive();
    setDeleteVisible(canDelete);
    cancelText_->setLocalizedText(canDelete ? i18n::message("library.archive.keep_archive.label") : i18n::message("library.archive.close.label"));
    if (canDelete) {
      detail_->setLocalizedText(i18n::message("library.archive.choose_whether_keep_delete_original_archive.message"));
    } else if (result->batch) {
      detail_->setLocalizedText(i18n::message(
          "library.archive.archives_not_completed.message",
          {{"count", std::to_string(result->archiveCount - result->completedCount)}}));
    }
    root_->applyYogaLayout();
  }
  if (const auto deletion = operation_.takeDeleteResult()) {
    deleting_ = false;
    cancelButton_->setEnabled(true);
    title_->setLocalizedText(deletion->deleted ? i18n::message("library.archive.archive_deleted.label") : i18n::message("library.archive.delete_failed.label"));
    setDeleteVisible(deletion->canRetry);
    cancelText_->setLocalizedText(deletion->canRetry ? i18n::message("library.archive.keep_archive.label") : i18n::message("library.archive.close.label"));
    updateProgress(1.0, deletion->message);
  }
  const bool operationChanged =
      !operation_.inProgress() && operation_.takeLibraryChanged();
  const bool changed = operationChanged ||
                       (result && !result->batch && result->success);
  const auto callbacks = callbacks_;
  if (changed && callbacks.libraryChanged) {
    callbacks.libraryChanged();
  }
  if (result && callbacks.finished) {
    callbacks.finished(*result);
  }
}

void ArchiveUnzipModal::resize(int width, int height) {
  if (root_ != nullptr) {
    root_->setSize(width, height);
  }
}

void ArchiveUnzipModal::cancelAndWait() {
  operation_.cancelAndWait();
  cancelling_ = false;
  hide();
}

void ArchiveUnzipModal::hide() {
  if (operation_.inProgress()) {
    return;
  }
  operation_.keepArchive();
  choosingAll_ = false;
  batchMode_ = false;
  deleting_ = false;
  estimatedSize_ = 0;
  if (root_ != nullptr) {
    root_->setVisible(false);
  }
}

bool ArchiveUnzipModal::handleEvents(SDL_Event &event) {
  if (event.type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
      event.type == SDL_EVENT_DID_ENTER_BACKGROUND) {
    cancelAndWait();
    return true;
  }
  if (!isVisible()) {
    return true;
  }
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
    if (event.key.repeat == 0) {
      cancelOrClose();
    }
    return false;
  }
  (void)root_->handleEvents(event);
  return false;
}

void ArchiveUnzipModal::cancelOrClose() {
  if (indexing_ || deleting_) return;
  if (operation_.inProgress()) {
    cancelling_ = true;
    operation_.requestCancel();
    updateProgress(0.0, batchMode_ ? i18n::message("library.archive.stopping_extraction_then_indexing_completed_folders.progress")
                                    : i18n::message("library.archive.cancelling.progress"));
  } else {
    hide();
  }
}

void ArchiveUnzipModal::deleteArchive() {
  if (!operation_.startDeleteArchive()) {
    return;
  }
  deleting_ = true;
  setDeleteVisible(false);
  cancelButton_->setEnabled(false);
  cancelText_->setLocalizedText(i18n::message("library.archive.deleting.progress"));
  title_->setLocalizedText(i18n::message("library.archive.deleting_archive.label"));
  updateProgress(1.0, i18n::message("library.archive.deleting_original_archive_refreshing_library.progress"));
}

void ArchiveUnzipModal::setDeleteVisible(bool visible) {
  deleteButton_->setVisible(visible);
  deleteButton_->setWidth(visible ? 210.0f : 0.0f);
  deleteButton_->setHeight(visible ? 58.0f : 0.0f);
}

void ArchiveUnzipModal::setAllChoiceVisible(bool visible) {
  detail_->setMinHeight(visible ? 80.0f : 54.0f);
  keepButton_->setVisible(visible);
  keepButton_->setWidth(visible ? 180.0f : 0.0f);
  keepButton_->setHeight(visible ? 58.0f : 0.0f);
  deleteText_->setLocalizedText(visible ? i18n::message("library.archive.delete_after_unzip.label") : i18n::message("library.archive.delete_archive.label"));
  setDeleteVisible(visible);
  track_->setVisible(!visible);
  track_->setHeight(visible ? 0.0f : 24.0f);
  percent_->setVisible(!visible);
  percent_->setHeight(visible ? 0.0f : 28.0f);
}

void ArchiveUnzipModal::updateProgress(double fraction,
                                      const i18n::Text &message,
                                      std::uint64_t current,
                                      std::uint64_t total) {
  fraction = std::clamp(fraction, 0.0, 1.0);
  message_->setLocalizedText(message);
  fill_->setWidth(std::max(0.0f, (track_->getWidth() - 4.0f) *
                                   static_cast<float>(fraction)));
  std::ostringstream text;
  text << std::fixed << std::setprecision(0) << (fraction * 100.0) << "%";
  percent_->setLocalizedText(archive_unzip_presentation::progressCount(
      text.str(), current, total, batchMode_ && !indexing_));
  i18n::Text detail = batchMode_ ? i18n::message("library.archive.processing_archives_concurrently_within_device_budget.message")
                                 : total > 0 ? i18n::message("library.archive.processing_files.label") : i18n::message("library.archive.working_on_archive.label");
  if (indexing_) {
    detail = i18n::message("library.archive.indexing.cancellation_notice");
  }
  if (estimatedSize_ > 0) {
    detail = i18n::message("library.archive.progress.detail_with_size",
                           {{"detail", detail},
                            {"size", formatFindBmsBytes(estimatedSize_)}});
  }
  detail_->setLocalizedText(detail);
  if (isVisible()) {
    root_->applyYogaLayout();
  }
}
