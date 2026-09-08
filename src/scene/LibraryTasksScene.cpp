#include "LibraryTasksScene.h"

#include "SceneManager.h"
#include "../library/ChartLibraryTaskService.h"
#include "../rendering/common.h"
#include "../view/Button.h"
#include "../view/ScrollView.h"
#include "../view/TextView.h"
#include "../view/UiTheme.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace {
constexpr const char *kFontPath = "assets/fonts/notosanscjkjp.ttf";

std::string statusName(chart_library_tasks::TaskStatus status) {
  using chart_library_tasks::TaskStatus;
  switch (status) {
  case TaskStatus::Queued: return "Queued";
  case TaskStatus::Running: return "Running";
  case TaskStatus::Complete: return "Complete";
  case TaskStatus::Failed: return "Failed";
  case TaskStatus::Paused: return "Paused";
  }
  return {};
}

TextView *makeText(std::string value, int size,
                   View::ThemeColorProvider color) {
  auto *result = new TextView(kFontPath, size);
  result->setText(std::move(value));
  result->setThemedColor(std::move(color));
  result->setVAlign(TextView::MIDDLE);
  result->setWrap(true);
  return result;
}
} // namespace

void LibraryTasksScene::init() { buildView(); }

void LibraryTasksScene::buildView() {
  rootLayout_ =
      new View(0, 0, rendering::window_width, rendering::window_height);
  rootLayout_->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12)
      ->setPadding(Edge::All, 24)
      ->setThemedBackgroundColor(ui_theme::mainMenuBackdrop);
  addView(rootLayout_);

  auto *header = new View();
  header->setHeight(64)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(14);
  auto *back = new Button();
  back->setWidth(116)->setHeight(52)->setCornerRadius(ui_theme::controlRadius());
  back->setThemedBackgroundColors(ui_theme::control, ui_theme::controlHover,
                                  ui_theme::controlPressed);
  back->setContentView(makeText("Back", 20, ui_theme::textPrimary));
  back->setOnClickListener([this] { goBack(); });
  header->addView(back);
  auto *title = makeText("Library Tasks", 34, ui_theme::textPrimary);
  title->setFlex(1)->setHeight(52);
  header->addView(title);
  rootLayout_->addView(header);

  taskScroll_ = new ScrollView();
  taskScroll_->setFlex(1)->setMinHeight(0);
  taskList_ = new View();
  taskList_
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(12);
  taskScroll_->setContentView(taskList_);
  rootLayout_->addView(taskScroll_);
  refreshTasks();
  rootLayout_->applyYogaLayout();
  layoutWidth_ = rendering::window_width;
  layoutHeight_ = rendering::window_height;
}

void LibraryTasksScene::refreshTasks() {
  const auto snapshot = context.chartLibraryTasks
                            ? context.chartLibraryTasks->snapshot()
                            : chart_library_tasks::Snapshot{};
  taskRevision_ = snapshot.revision;
  taskList_->clearChildren();

  if (snapshot.tasks.empty()) {
    auto *empty = makeText("No library tasks.", 20,
                           ui_theme::textSecondary);
    empty->setHeight(54);
    taskList_->addView(empty);
  } else {
    for (const auto &task : snapshot.tasks) {
      std::ostringstream detail;
      detail << statusName(task.status);
      if (task.total > 0) {
        detail << " · " << task.current << '/' << task.total;
      } else if (task.fraction > 0.0) {
        detail << " · " << std::fixed << std::setprecision(0)
               << task.fraction * 100.0 << '%';
      }
      if (!task.detail.empty()) detail << " · " << task.detail;
      auto *row = new View();
      row->setMinHeight(74)
          ->setFlexDirection(FlexDirection::Column)
          ->setGap(2)
          ->setPadding(Edge::All, 12)
          ->setThemedBackgroundColor(ui_theme::panelStrong)
          ->setThemedBorderColor(ui_theme::hairline)
          ->setBorderWidth(1)
          ->setCornerRadius(ui_theme::panelRadius());
      auto *taskTitle = makeText(task.title, 21, ui_theme::textPrimary);
      taskTitle->setHeight(30);
      row->addView(taskTitle);
      auto *taskDetail =
          makeText(detail.str(), 17, ui_theme::textSecondary);
      taskDetail->setHeight(26);
      row->addView(taskDetail);
      taskList_->addView(row);
    }
  }
  if (rootLayout_ != nullptr) rootLayout_->applyYogaLayout();
  if (taskScroll_ != nullptr) taskScroll_->scrollToBottom();
}

void LibraryTasksScene::goBack() {
  (void)returnToScene(*context.sceneManager, returnTarget_);
}

EventHandleResult LibraryTasksScene::handleEvents(SDL_Event &event) {
  if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE) {
    goBack();
    return {};
  }
  return Scene::handleEvents(event);
}

void LibraryTasksScene::update(float) {
  const auto revision = context.chartLibraryTasks
                            ? context.chartLibraryTasks->snapshot().revision
                            : 0;
  if (revision != taskRevision_) {
    refreshTasks();
  }
  if (rootLayout_ != nullptr &&
      (layoutWidth_ != rendering::window_width ||
       layoutHeight_ != rendering::window_height)) {
    layoutWidth_ = rendering::window_width;
    layoutHeight_ = rendering::window_height;
    rootLayout_->setSize(layoutWidth_, layoutHeight_);
    rootLayout_->applyYogaLayout();
    if (taskScroll_ != nullptr) taskScroll_->refreshContentLayout();
  }
}

void LibraryTasksScene::renderScene() {}

void LibraryTasksScene::cleanupScene() {
  rootLayout_ = nullptr;
  taskScroll_ = nullptr;
  taskList_ = nullptr;
}
