#include "ArchiveUnzipModal.h"

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
  title_->setText("Unzip");
  title_->setThemedColor(ui_theme::textPrimary);
  title_->setHeight(42);
  panel->addView(title_);

  message_ = new TextView("assets/fonts/notosanscjkjp.ttf", 22);
  message_->setThemedColor(ui_theme::textSecondary);
  message_->setHeight(32);
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
  detail_->setHeight(54);
  panel->addView(detail_);

  auto *footer = new View();
  footer->setFlexDirection(FlexDirection::Row);
  footer->setJustifyContent(YGJustifyFlexEnd);
  footer->setAlignItems(YGAlignStretch);
  footer->setGap(12);
  footer->setHeight(58);

  deleteButton_ = makeModalButton("Delete Archive", 18);
  deleteButton_->setVisible(false);
  deleteButton_->setWidth(0)->setHeight(0);
  deleteButton_->setOnClickListener([this]() { deleteArchive(); });
  footer->addView(deleteButton_);

  cancelButton_ = makeModalButton("Cancel", 20, &cancelText_);
  cancelButton_->setWidth(130);
  cancelButton_->setOnClickListener([this]() { cancelOrClose(); });
  footer->addView(cancelButton_);
  panel->addView(footer);
}

bool ArchiveUnzipModal::start(const ChartMetaRecord &record) {
  if (!operation_.start(record)) {
    return false;
  }
  estimatedSize_ = record.archiveUncompressedSize;
  cancelling_ = false;
  resize(rendering::window_width, rendering::window_height);
  root_->setVisible(true);
  title_->setText("Unzip");
  cancelText_->setText("Cancel");
  setDeleteVisible(false);
  updateProgress(0.0, "Preparing unzip");
  return true;
}

bool ArchiveUnzipModal::inProgress() const { return operation_.inProgress(); }

bool ArchiveUnzipModal::isVisible() const {
  return root_ != nullptr && root_->getVisible();
}

View *ArchiveUnzipModal::root() const { return root_; }

void ArchiveUnzipModal::update() {
  if (const auto progress = operation_.takeProgress(); progress && !cancelling_) {
    updateProgress(progress->fraction, progress->message,
                   progress->current, progress->total);
  }
  const auto result = operation_.takeResult();
  if (result) {
    cancelling_ = false;
    title_->setText(result->success ? "Unzip Complete"
                      : result->cancelled ? "Unzip Cancelled" : "Unzip Failed");
    updateProgress(result->success ? 1.0 : 0.0, result->message);
    const bool canDelete = operation_.canDeleteArchive();
    setDeleteVisible(canDelete);
    cancelText_->setText(canDelete ? "Keep Archive" : "Close");
    if (canDelete) {
      detail_->setText("Choose whether to keep or delete the original archive.");
    }
    root_->applyYogaLayout();
  }
  const bool operationChanged =
      !operation_.inProgress() && operation_.takeLibraryChanged();
  const bool changed = std::exchange(libraryChangedPending_, false) ||
                       operationChanged ||
                       (result && result->success);
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
  if (inProgress()) {
    return;
  }
  operation_.keepArchive();
  estimatedSize_ = 0;
  if (root_ != nullptr) {
    root_->setVisible(false);
  }
}

bool ArchiveUnzipModal::handleEvents(SDL_Event &event) {
  if (event.type == SDL_APP_WILLENTERBACKGROUND ||
      event.type == SDL_APP_DIDENTERBACKGROUND) {
    cancelAndWait();
    return true;
  }
  if (!isVisible()) {
    return true;
  }
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
    if (event.key.repeat == 0) {
      cancelOrClose();
    }
    return false;
  }
  (void)root_->handleEvents(event);
  return false;
}

void ArchiveUnzipModal::cancelOrClose() {
  if (inProgress()) {
    cancelling_ = true;
    operation_.requestCancel();
    updateProgress(0.0, "Cancelling...");
  } else {
    hide();
  }
}

void ArchiveUnzipModal::deleteArchive() {
  std::string message;
  const bool deleted = operation_.deleteArchive(message);
  setDeleteVisible(operation_.canDeleteArchive());
  if (deleted) {
    title_->setText("Archive Deleted");
    cancelText_->setText("Close");
    libraryChangedPending_ = true;
  }
  updateProgress(1.0, message);
}

void ArchiveUnzipModal::setDeleteVisible(bool visible) {
  deleteButton_->setVisible(visible);
  deleteButton_->setWidth(visible ? 210.0f : 0.0f);
  deleteButton_->setHeight(visible ? 58.0f : 0.0f);
}

void ArchiveUnzipModal::updateProgress(double fraction,
                                      const std::string &message,
                                      std::uint64_t current,
                                      std::uint64_t total) {
  fraction = std::clamp(fraction, 0.0, 1.0);
  message_->setText(message);
  fill_->setWidth(std::max(0.0f, (track_->getWidth() - 4.0f) *
                                   static_cast<float>(fraction)));
  std::ostringstream text;
  text << std::fixed << std::setprecision(0) << (fraction * 100.0) << "%";
  if (total > 0) {
    text << " (" << current << "/" << total << ")";
  }
  percent_->setText(text.str());
  std::string detail = total > 0 ? "Processing files" : "Working on archive";
  if (estimatedSize_ > 0) {
    detail += "\nEstimated unzipped size: " + formatFindBmsBytes(estimatedSize_);
  }
  detail_->setText(detail);
  if (isVisible()) {
    root_->applyYogaLayout();
  }
}
