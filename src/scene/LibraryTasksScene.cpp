#include "../i18n/Localization.h"
#include "LibraryTasksScene.h"

#include "SceneManager.h"
#include "../library/ChartLibraryTaskService.h"
#include "../rendering/common.h"
#include "../view/Button.h"
#include "../view/ScrollView.h"
#include "../view/TextView.h"
#include "../view/AppTheme.h"

#include <iomanip>
#include <sstream>
#include <utility>

namespace {
constexpr const char *kFontPath = app_theme::font;

std::string statusName(chart_library_tasks::TaskStatus status) {
  using chart_library_tasks::TaskStatus;
  switch (status) {
  case TaskStatus::Queued: return i18n::tr("library.tasks.queued.label");
  case TaskStatus::Running: return i18n::tr("library.tasks.running.label");
  case TaskStatus::Complete: return i18n::tr("library.tasks.complete.label");
  case TaskStatus::Failed: return i18n::tr("library.tasks.failed.label");
  case TaskStatus::Paused: return i18n::tr("library.tasks.paused.label");
  }
  return {};
}

TextView *makeText(std::string value, int size,
                   View::ThemeColorProvider color) {
  auto *result = new TextView(size >= 26 ? app_theme::boldFont : kFontPath, size);
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
      ->setGap(0)
      ->setPadding(Edge::All, 0)
      ->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(rootLayout_);

  auto *header = new View();
  header->setHeight(80)
      ->setPadding(Edge::Left, 24)
      ->setPadding(Edge::Right, 24)
      ->setFlexDirection(FlexDirection::Row)
      ->setAlignItems(YGAlignCenter)
      ->setGap(14);
  auto *back = new Button();
  back->setWidth(116)->setHeight(52)->setCornerRadius(app_theme::controlRadius());
  back->setThemedBackgroundColors(app_theme::control, app_theme::controlHover,
                                  app_theme::controlPressed);
  back->setContentView(makeText(i18n::tr("library.tasks.back.label"), 20, app_theme::textPrimary));
  back->setOnClickListener([this] { goBack(); });
  auto *title = makeText(i18n::tr("library.tasks.library_tasks.label"), 34, app_theme::textPrimary);
  title->setFlex(1)->setHeight(52);
  header->addView(title);
  header->addView(back);
  rootLayout_->addView(header);
  auto *headerRule = new View();
  headerRule->setHeight(1)->setFlexShrink(0)->setThemedBackgroundColor(app_theme::hairline);
  rootLayout_->addView(headerRule);

  taskScroll_ = new ScrollView();
  taskScroll_->setFlex(1)->setMinHeight(0);
  taskList_ = new View();
  taskList_
      ->setFlexDirection(FlexDirection::Column)
      ->setAlignItems(YGAlignStretch)
      ->setGap(0)
      ->setPadding(Edge::Left, 24)
      ->setPadding(Edge::Right, 24);
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
    auto *empty = makeText(i18n::tr("library.tasks.no_library_tasks.message"), 20,
                           app_theme::textSecondary);
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
      if (!task.detail.empty()) detail << " · " << task.detail.resolve();
      auto *row = new View();
      row->setMinHeight(100)
          ->setFlexDirection(FlexDirection::Column)
          ->setGap(4)
          ->setPadding(Edge::Top, 18)
          ->setPadding(Edge::Bottom, 18);
      auto *taskTitle = makeText(task.title.resolve(), 28, app_theme::textPrimary);
      taskTitle->setMinHeight(36);
      row->addView(taskTitle);
      auto *taskDetail = makeText(detail.str(), 20, app_theme::textSecondary);
      taskDetail->setMinHeight(28);
      row->addView(taskDetail);
      taskList_->addView(row);
      auto *rule = new View();
      rule->setHeight(1)->setFlexShrink(0)->setThemedBackgroundColor(app_theme::hairline);
      taskList_->addView(rule);
    }
  }
  if (rootLayout_ != nullptr) rootLayout_->applyYogaLayout();
  if (taskScroll_ != nullptr) taskScroll_->scrollToBottom();
}

void LibraryTasksScene::goBack() {
  (void)returnToScene(*context.sceneManager, returnTarget_);
}

EventHandleResult LibraryTasksScene::handleEvents(SDL_Event &event) {
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
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
