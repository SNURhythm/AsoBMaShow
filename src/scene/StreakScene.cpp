#include "StreakScene.h"
#include "SceneManager.h"
#include "../Uuid.h"
#include "../LongNoteModeUtils.h"
#include "../music_select/MusicSelectAssignmentFolders.h"
#include "../rendering/common.h"
#include "../rendering/ResponsiveUi.h"
#include "../view/ActionButtonStyle.h"
#include "../view/ActionButtonContent.h"
#include "../view/AppTheme.h"
#include "../view/Button.h"
#include "../view/BlockingOverlayView.h"
#include "../view/ClearLampColors.h"
#include "../view/DropdownView.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include "../view/ProgressMotionView.h"
#include "../view/StreakFlameView.h"
#include "../view/AssignmentProgressView.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

namespace {
View *card(View *parent) {
  auto *view = new View();
  view->setFlexDirection(FlexDirection::Column)->setGap(10)->setPadding(Edge::All, 20)
      ->setFlexShrink(0)->setCornerRadius(24)->setThemedBackgroundColor(app_theme::panel);
  parent->addView(view);
  return view;
}
void sizeResetChallengeModal(View *overlay) {
  auto *panel = overlay->findViewByName("streakResetPanel");
  if (!panel) return;
  const auto &children = panel->getChildren();
  auto *scroll = dynamic_cast<ScrollView *>(children[1]);
  auto *details = scroll ? scroll->getContentView() : nullptr;
  if (!details) return;
  float contentHeight = 0;
  for (auto *child : details->getChildren()) contentHeight += child->getHeight();
  if (!details->getChildren().empty()) contentHeight += 12 * (details->getChildren().size() - 1);
  const float desired = contentHeight + children.front()->getHeight() +
      children.back()->getHeight() + 60; // Card padding and two gaps.
  panel->setHeight(std::min(620.0F, desired));
}
std::string numberText(const char *key, std::int64_t value) {
  return i18n::format(key, {{"count", std::to_string(value)}});
}
void meter(View *parent, std::int64_t value, std::int64_t goal,
           ui::motion::Track &track, float &previousTarget, bool complete = false) {
  const float target = std::clamp(goal > 0 ? float(value) / goal : 0, 0.0F, 1.0F);
  if (previousTarget < 0)
    track.start(0, target, SDL_GetTicks(), 850, 80, ui::motion::Ease::Smooth);
  else if (previousTarget != target)
    track.retarget(target, SDL_GetTicks(), 850);
  previousTarget = target;
  parent->addView(new ProgressMotionView(track, complete));
}
TextView *label(View *parent, const std::string &value, int size = 21) {
  auto *text = new TextView(app_theme::font, size);
  text->setText(value);
  text->setWrap(true);
  text->setMinHeight(size + 12)->setFlexShrink(0);
  text->setThemedColor(app_theme::textPrimary);
  parent->addView(text);
  return text;
}
void lampLabel(View *parent, const std::string &text, int rank, int size = 19) {
  auto *row = new View();
  row->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(8)->setFlexShrink(0);
  if (hasClearLampColor(rank)) {
    auto *bar = new View();
    bar->setWidth(4)->setHeight(22)->setCornerRadius(2)->setFlexShrink(0);
    bar->setBackgroundColor(clearLampColorForRank(rank));
    row->addView(bar);
  }
  label(row, text, size)->setFlex(1)->setMinWidth(0);
  parent->addView(row);
}
Button *action(View *parent, const std::string &value, std::function<void()> click,
               app_theme::ActionCategory category = app_theme::ActionCategory::Neutral,
               uint32_t glyph = 0) {
  auto *button = new Button();
  button->setHeight(rendering::compactMenuCanvasActive ? 66 : 52)->setMinWidth(0)->setFlexShrink(0);
  auto *text = new TextView(app_theme::font, 21);
  text->setText(value);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  if (glyph) {
    auto *content = action_button::makeInlineContent(text, glyph);
    content->setPadding(Edge::Left, 14)->setPadding(Edge::Right, 14);
    button->setContentView(content);
  } else button->setContentView(text);
  action_button::applyStyle(button, text, category);
  if (glyph) action_button::setIconColor(button, [category] { return app_theme::actionPalette(category).label; });
  button->setCornerRadius(16);
  button->setOnClickListener(std::move(click));
  parent->addView(button);
  return button;
}
TextView *fieldError(View *parent, View *control, const std::string &error) {
  if (error.empty()) return nullptr;
  control->setName("streakInvalidField");
  control->setBorderWidth(2)->setThemedBorderColor(app_theme::coral);
  auto *message = label(parent, i18n::tr(error), 17);
  message->setThemedColor(app_theme::coral);
  return message;
}
void field(View *parent, const std::string &caption, std::string &value,
           std::string *error = nullptr) {
  label(parent, caption, 18);
  auto *input = new TextInputBox(app_theme::font, 22);
  input->setHeight(rendering::compactMenuCanvasActive ? 66 : 52)->setFlexShrink(0)->setPadding(Edge::All, 10)->setCornerRadius(14);
  input->setThemedBackgroundColor(app_theme::insetSurface);
  input->setThemedColor(app_theme::textPrimary);
  input->setEditingText(value);
  parent->addView(input);
  auto *explanation = fieldError(parent, input, error ? *error : std::string{});
  input->onTextChanged([&value, error, input, explanation](const std::string &next) {
    value = next;
    if (error) error->clear();
    if (explanation) {
      input->setBorderWidth(0)->setName("");
      explanation->setDisplay(YGDisplayNone);
      explanation->setVisible(false);
    }
  });
}
void select(View *parent, OverlayPortal *portal, const std::string &caption,
            std::vector<DropdownView::Option> options, std::string selected,
            std::function<void(const std::string &)> change, const std::string &error = {}) {
  label(parent, caption, 18);
  auto state = std::make_shared<DropdownView::State>();
  state->selectedId = std::move(selected);
  state->options = std::move(options);
  state->maxVisibleItems = 7;
  auto ptr = std::make_shared<DropdownView *>(nullptr);
  auto *dropdown = new DropdownView({.onOptionSelected = [state, ptr, change](const std::string &id) {
    state->selectedId = id;
    (*ptr)->refresh(*state);
    change(id);
  }}, portal);
  *ptr = dropdown;
  dropdown->setHeight(rendering::compactMenuCanvasActive ? 66 : 52)->setFlexShrink(0);
  dropdown->setTriggerWidth(300);
  dropdown->refresh(*state);
  parent->addView(dropdown);
  fieldError(parent, dropdown, error);
}
int integer(const std::string &value) {
  int number = 0;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument(i18n::tr("streak.ui.whole_numbers"));
  return number;
}
std::string levelsText(const streak::Assignment &a) {
  struct Group { std::string source, table, levels; };
  std::vector<Group> groups;
  const std::string separator = " · ";
  for (const auto &level : a.levels) {
    const auto split = level.label.rfind(separator);
    const auto table = split == std::string::npos ? std::string{} : level.label.substr(0, split);
    const auto name = split == std::string::npos
        ? (level.label.empty() ? level.level : level.label) : level.label.substr(split + separator.size());
    auto group = std::find_if(groups.begin(), groups.end(), [&](const auto &g) {
      return g.source == level.source && g.table == table;
    });
    if (group == groups.end()) groups.push_back({level.source, table, name});
    else group->levels += ", " + name;
  }
  std::string text;
  for (const auto &group : groups) {
    if (!text.empty()) text += " / ";
    if (!group.table.empty()) text += group.table + separator;
    text += group.levels;
  }
  return text;
}
std::string lampText(int rank) {
  if (rank == -2) return i18n::tr("streak.ui.any_previous_lamp");
  return streak::lampLabel(rank);
}
std::string comparison(const char *key, const std::string &value) {
  return i18n::format(key, {{"value", value}});
}
void assignmentConditions(View *parent, const streak::Assignment &a, int size = 18) {
  const auto row = [parent, size](const std::string &previous, const std::string &target,
                                 int previousLamp = -2, int targetLamp = -2) {
    auto *line = new View();
    line->setFlexDirection(FlexDirection::Row)->setAlignItems(YGAlignCenter)->setGap(10)->setFlexShrink(0);
    auto *left = new View();
    // Definite widths prevent intrinsic text measurements from shifting the arrow column.
    left->setFlex(1)->setFlexBasis(0)->setWidth(0)->setMinWidth(0);
    lampLabel(left, previous, previousLamp, size);
    line->addView(left);
    auto *arrow = new TextView(ui_icons::kFontAwesomeSolidPath, size);
    arrow->setText(ui_icons::textForCodepoint(ui_icons::kArrowRight));
    arrow->setAlign(TextView::CENTER);
    arrow->setVAlign(TextView::MIDDLE);
    arrow->setThemedColor(app_theme::textSecondary);
    arrow->setWidth(24)->setHeight(size + 12)->setFlexShrink(0);
    line->addView(arrow);
    auto *right = new View();
    right->setFlex(1)->setFlexBasis(0)->setWidth(0)->setMinWidth(0);
    lampLabel(right, target, targetLamp, size);
    line->addView(right);
    parent->addView(line);
  };
  const auto previousLamp = a.eligibleLamp == -2 ? i18n::tr("streak.ui.any_lamp") :
      a.eligibleOrBelow ? comparison("streak.ui.at_most", lampText(a.eligibleLamp)) : lampText(a.eligibleLamp);
  row(a.eligibleLamp == -2 ? previousLamp : comparison("streak.ui.condition_lamp", previousLamp),
      a.targetLamp < 0 ? i18n::tr("streak.ui.any_lamp") :
          comparison("streak.ui.at_least", lampText(a.targetLamp)), a.eligibleLamp, a.targetLamp);
  auto previousScore = a.eligibleScoreRank == -2 ? i18n::tr("streak.ui.any_score") :
      streak::scoreRankLabel(a.eligibleScoreRank);
  if (a.eligibleScoreRank != -2 && a.eligibleScoreOrBelow)
    previousScore = comparison("streak.ui.at_most", previousScore);
  row(a.eligibleScoreRank == -2 ? previousScore : comparison("streak.ui.condition_score", previousScore),
      a.scoreNumerator == 0 ? i18n::tr("streak.ui.any_score") :
          comparison("streak.ui.at_least", streak::scoreValueLabel(a)));
}
} // namespace

StreakScene::StreakScene(ApplicationContext &context, SceneReturnTarget target)
    : Scene(context), returnTarget_(std::move(target)) {}
void StreakScene::configureTab(float topInset, std::function<void()> onClose) {
  embeddedTab_ = true;
  tabClose_ = std::move(onClose);
  setTabTopInset(topInset);
}
void StreakScene::setTabTopInset(float topInset) {
  tabTopInset_ = std::max(0.0F, topInset);
  layout();
}
void StreakScene::onPause() {
  pendingResetAssignment_.clear();
  roundCue_ = {};
  goalIgnitionSoundPending_ = false;
  const auto endEditing = [](View *view, const auto &self) -> void {
    if (auto *input = dynamic_cast<TextInputBox *>(view)) input->endEditing();
    for (auto *child : view->getChildren()) self(child, self);
  };
  for (auto *view : views) { view->cancelPointerInput(); endEditing(view, endEditing); }
}
void StreakScene::onResume() {
  if (profileId_ != context.profileManager.activeProfile().id || checkedDay_ != streak::today())
    page_ = Page::Dashboard;
  if (page_ == Page::Dashboard) reload();
  build();
}
void StreakScene::init() { reload(); build(); }
void StreakScene::reload() {
  fieldErrors_.clear();
  pendingResetAssignment_.clear();
  if (profileId_ != context.profileManager.activeProfile().id || checkedDay_ != streak::today()) {
    showOptional_ = false;
    scrollOffsets_.fill(0);
    meterTargets_.fill(-1);
    goalIgnitionStartedAt_.reset();
    if (scroll_) scroll_->setScrollOffset(0);
    streakProgress_.reset();
    streakProgressStartedAt_ = 0;
    roundCue_ = {};
    goalIgnitionSoundPending_ = false;
  }
  profileId_ = context.profileManager.activeProfile().id;
  std::string error;
  auto loaded = context.scoreRepository.LoadStreak(error);
  loadFailed_ = !loaded;
  if (!loaded) { message_ = error; return; }
  state_ = std::move(*loaded);
  if (auto progress = context.scoreRepository.TakeStreakProgress()) {
    streakProgress_ = std::move(progress);
    streakProgressStartedAt_ = 0;
    roundCue_ = {};
    goalIgnitionSoundPending_ = false;
  }
  draft_ = state_.configurations.empty() ? streak::Config{} : state_.configurations.rbegin()->second;
  daily_ = std::to_string(draft_.dailyGoal);
  weekly_ = std::to_string(draft_.weeklyGoal);
  monthly_ = std::to_string(draft_.monthlyGoal);
  checkedDay_ = streak::today();
}
std::set<std::string> StreakScene::unavailableAssignments(const streak::Day &day) {
  auto session = context.chartRepository.OpenSession();
  if (!session) throw std::runtime_error("Chart storage is unavailable.");
  MusicSelectRepositoryMetadata metadata;
  for (const auto &table : session->SelectDifficultyTables())
    metadata.tables.push_back({.info=table});
  const int mode = long_note_mode::valueFromId(context.settings.selectedLnMode);
  const auto charts = loadMusicSelectAssignmentCharts(*session, metadata, day, mode);
  const auto ranks = context.scoreRepository.LoadBestClearRanks();
  const auto scores = context.scoreRepository.LoadBestScores();
  std::set<std::string> unavailable;
  for (const auto &progress : day.assignments)
    if (musicSelectAssignmentNeedsAdjustment(day, progress, charts,
        {.scoreFor=[&scores](const bms_parser::ChartMeta &meta, int mode) {
           return scores.bestFor(meta, mode);
         }, .clearFor=[&ranks](const bms_parser::ChartMeta &meta, int mode) {
           return ranks.bestRankFor(meta, mode);
         }, .selectedLongNoteMode=mode}))
      unavailable.insert(progress.requirement.id);
  return unavailable;
}
void StreakScene::later(std::function<void()> action) {
  defer([this, action = std::move(action)] { action(); build(); return true; }, 0, true);
}
void StreakScene::build() {
  if (scroll_) scrollOffsets_[static_cast<std::size_t>(renderedPage_)] = scroll_->getScrollOffset();
  if (page_ != Page::Dashboard) { roundCue_ = {}; goalIgnitionSoundPending_ = false; }
  for (auto *view : views) delete view;
  views.clear();
  resetOverlay_ = nullptr;
  root_ = new View(0, 0, rendering::window_width, rendering::window_height);
  root_->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)
      ->setPadding(Edge::All, 18)->setGap(12)->setThemedBackgroundColor(app_theme::mainMenuBackdrop);
  addView(root_);
  portal_ = new OverlayPortal(0, 0, rendering::window_width, rendering::window_height);
  portal_->setPositionType(YGPositionTypeAbsolute);
  portal_->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0)->setZIndex(900);
  auto *header = new View();
  header->setFlexDirection(FlexDirection::Row)->setGap(12)->setAlignItems(YGAlignCenter)->setFlexShrink(0);
  label(header, i18n::tr("streak.ui.title"), 32)->setFlex(1);
  if (!embeddedTab_ || page_ != Page::Dashboard)
    action(header, i18n::tr("streak.ui.back"), [this] { back(); }, app_theme::ActionCategory::Neutral, ui_icons::kCollapse)->setWidth(140);
  root_->addView(header);
  scroll_ = new ScrollView();
  scroll_->setFlex(1)->setMinHeight(0);
  root_->addView(scroll_);
  auto *body = new View();
  body->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)
      ->setGap(16)->setPadding(Edge::Bottom, 24);
  auto *content = new View();
  content->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignCenter);
  body->setWidthPercent(100);
  YGNodeStyleSetMaxWidth(body->getNode(), 960);
  content->addView(body);
  scroll_->setContentView(content);
  if (loadFailed_) {
    action(body, i18n::tr("streak.ui.retry"), [this] { later([this] { reload(); }); }, app_theme::ActionCategory::Neutral, ui_icons::kRotate);
  } else if (page_ == Page::Dashboard) dashboard(body);
  else if (page_ == Page::Routine) routine(body);
  else editor(body);
  if (!message_.empty()) label(body, message_, 19);
  addView(portal_);
  resetChallengeModal();
  layout();
  renderedPage_ = page_;
  scroll_->setScrollOffset(scrollOffsets_[static_cast<std::size_t>(page_)]);
  if (auto *invalid = body->findViewByName("streakInvalidField"))
    scroll_->setScrollOffset(std::max(0.0F, scroll_->getScrollOffset() + invalid->getY() - scroll_->getY() - 48));
}
void StreakScene::dashboard(View *body) {
  if (state_.days.empty()) {
    body = card(body);
    body->addView(new StreakFlameView());
    label(body, i18n::tr("streak.ui.welcome"), 26);
    label(body, i18n::tr("streak.ui.rules"));
    action(body, i18n::tr("streak.ui.create"), [this] { later([this] { page_ = Page::Routine; }); }, app_theme::ActionCategory::Primary, ui_icons::kPlus);
    return;
  }
  const auto &day = state_.days.rbegin()->second;
  const auto totals = streak::summary(state_);
  const bool complete = day.points >= day.config.dailyGoal;
  const int bonus = streak::bonusPoints(day);
  const int bonusLimit = streak::bonusLimit(day);
  const bool bonusAvailable = bonus < bonusLimit;
  if (streakProgress_ && streakProgressStartedAt_ == 0) {
    streakProgressStartedAt_ = SDL_GetTicks();
    goalIgnitionSoundPending_ = streakProgress_->reachedDailyGoal;
    if (streakProgress_->reachedDailyGoal) goalIgnitionStartedAt_ = streakProgressStartedAt_;
    roundCue_.start(*streakProgress_, streakProgressStartedAt_);
  }
  auto *summary = card(body);
  summary->setName(complete ? "streakDayComplete" : "streakDailyHero");
  summary->addView(new StreakFlameView(day.played, complete, false, goalIgnitionStartedAt_));
  label(summary, i18n::tr(complete ? "streak.ui.complete" : "streak.ui.keep_going"), 30);
  label(summary, numberText("streak.ui.streak_count", state_.currentStreak), 44);
  label(summary, i18n::plural("streak.ui.best_count", state_.bestStreak).resolve(), 18)->setThemedColor(app_theme::textSecondary);
  label(summary, std::to_string(std::min<std::int64_t>(day.points, day.config.dailyGoal)) +
      " / " + std::to_string(day.config.dailyGoal), 34);
  meter(summary, day.points, day.config.dailyGoal, meterTracks_[0], meterTargets_[0], complete);
  label(summary, i18n::tr(complete ? "streak.ui.complete_help" :
              day.played ? "streak.ui.protected" : "streak.ui.pending"), 20);
  if (complete)
    action(summary, i18n::tr("streak.ui.done"), [this] { back(); }, app_theme::ActionCategory::Confirmation, ui_icons::kCheck);
  if (complete) {
    label(body, std::string(i18n::tr("streak.ui.shared_bonus")) + " " + std::to_string(bonus) +
                " / " + std::to_string(bonusLimit), 20);
    label(body, i18n::tr(bonusAvailable ? "streak.ui.optional_help" : "streak.ui.bonus_complete"), 18);
    if (bonusAvailable)
      action(body, i18n::tr(showOptional_ ? "streak.ui.hide_optional" : "streak.ui.show_optional"),
          [this] { later([this] {
            showOptional_ = !showOptional_;
            if (!showOptional_) { streakProgress_.reset(); roundCue_ = {}; goalIgnitionSoundPending_ = false; }
          }); },
          app_theme::ActionCategory::Neutral, showOptional_ ? ui_icons::kHide : ui_icons::kPlus);
  } else label(body, i18n::tr("streak.ui.assignments"), 27);
  if (!complete && day.assignments.empty()) label(body, i18n::tr("streak.ui.no_assignments"));
  std::set<std::string> unavailable;
  try {
    if (!complete || (showOptional_ && bonusAvailable) || streakProgress_)
      unavailable = unavailableAssignments(day);
  }
  catch (const std::exception &error) { label(body, error.what(), 18); }
  for (const auto &p : day.assignments) {
    const auto change = streak::progressChange(streakProgress_, p);
    if (!streak::dashboardAssignmentVisible(complete, showOptional_, bonusAvailable, change)) continue;
    auto *assignmentCard = card(body);
    const auto reward = complete ? 1 :
        std::min<std::int64_t>(p.requirement.points, day.config.dailyGoal - day.points);
    label(assignmentCard, p.requirement.name + "  ·  " + i18n::plural("streak.ui.reward", reward).resolve(), 24);
    if (complete)
      label(assignmentCard, std::string(i18n::tr("streak.ui.bonus_today")) + " " + std::to_string(p.bonusRounds), 18);
    label(assignmentCard, levelsText(p.requirement), 18);
    assignmentConditions(assignmentCard, p.requirement);
    assignmentCard->addView(new AssignmentProgressView(p,
        change, streakProgressStartedAt_));
    if (complete && bonusAvailable && streak::canResetExtraChallenge(p))
      action(assignmentCard, i18n::tr("streak.ui.too_hard"), [this, id = p.requirement.id] {
        later([this, id] { pendingResetAssignment_ = id; pendingResetDay_ = checkedDay_; });
      }, app_theme::ActionCategory::Neutral, ui_icons::kRotate);
    if (complete && p.bonusChartCount == 2)
      label(assignmentCard, i18n::tr("streak.ui.two_chart_rounds"), 18);
    if (unavailable.contains(p.requirement.id))
      action(assignmentCard, i18n::tr("streak.ui.adjust_today"), [this, requirement = p.requirement] {
        later([this, requirement] {
          editAssignment(-1);
          adjustingToday_ = true;
          assignment_ = requirement;
          name_ = requirement.name;
          count_ = std::to_string(requirement.chartCount);
          points_ = std::to_string(requirement.points);
          rate_ = std::to_string(100.0 * requirement.scoreNumerator / requirement.scoreDenominator);
        });
      }, app_theme::ActionCategory::Neutral, ui_icons::kPlayOptions);
  }
  auto *periods = card(body);
  label(periods, i18n::tr("streak.ui.week_progress"), 20);
  label(periods, std::to_string(totals.weeklyPoints) + " / " + std::to_string(day.config.weeklyGoal), 26);
  meter(periods, totals.weeklyPoints, day.config.weeklyGoal, meterTracks_[1], meterTargets_[1]);
  label(periods, i18n::tr("streak.ui.month_progress"), 20);
  label(periods, std::to_string(totals.monthlyPoints) + " / " + std::to_string(day.config.monthlyGoal), 26);
  meter(periods, totals.monthlyPoints, day.config.monthlyGoal, meterTracks_[2], meterTargets_[2]);
  auto *wallet = card(body);
  label(wallet, i18n::plural("streak.ui.savings_count", state_.savings).resolve(), 26);
  label(wallet, numberText("streak.ui.freezes_count", state_.freezes), 21)->setThemedColor(app_theme::cyan);
  action(wallet, i18n::plural("streak.ui.buy_freeze", streak::freezePrice(state_)).resolve(), [this] {
    later([this] {
      std::string error;
      const bool bought = context.scoreRepository.BuyStreakFreeze(error);
      message_ = bought ? i18n::tr("streak.ui.freeze_bought") : error;
      if (bought) streakSounds_.play(context.jukebox.audioRuntime(), streak::Sound::Freeze);
      reload();
    });
  }, app_theme::ActionCategory::Information, ui_icons::kSnowflake)->setEnabled(streak::canBuyFreeze(state_));
  action(body, i18n::tr("streak.ui.edit"), [this] { later([this] { message_.clear(); fieldErrors_.clear(); page_ = Page::Routine; }); }, app_theme::ActionCategory::Neutral, ui_icons::kClock);
  if (state_.configurations.rbegin()->first > totals.day)
    label(body, comparison("streak.ui.scheduled", streak::dateLabel(state_.configurations.rbegin()->first)), 19);
  label(body, i18n::tr("streak.ui.milestones"), 27);
  auto *badges = card(body);
  auto *badgeRow = new View();
  badgeRow->setFlexDirection(FlexDirection::Row)->setFlexWrap(YGWrapWrap)->setGap(10);
  badges->addView(badgeRow);
  for (int n : {7, 14, 30, 50, 100, 365}) {
    const bool earned = state_.milestones.contains("streak:" + std::to_string(n));
    auto *badge = label(badgeRow, std::to_string(n), 24);
    badge->setWidth(76)->setHeight(64)->setPadding(Edge::All, 12)->setCornerRadius(18);
    badge->setAlign(TextView::CENTER);
    badge->setThemedBackgroundColor(earned ? app_theme::lime : app_theme::insetSurface);
    badge->setThemedColor(earned ? app_theme::panel : app_theme::textMuted);
  }

}
void StreakScene::resetChallengeModal() {
  if (pendingResetAssignment_.empty() || page_ != Page::Dashboard) return;
  const auto day = state_.days.find(pendingResetDay_);
  if (day == state_.days.end()) { pendingResetAssignment_.clear(); return; }
  const auto progress = std::find_if(day->second.assignments.begin(), day->second.assignments.end(),
      [this](const auto &p) { return p.requirement.id == pendingResetAssignment_; });
  if (progress == day->second.assignments.end()) { pendingResetAssignment_.clear(); return; }
  const auto *original = &progress->originalRequirement;
  resetOverlay_ = new BlockingOverlayView();
  resetOverlay_->setName("streakResetChallengeModal");
  resetOverlay_->setPositionType(YGPositionTypeAbsolute);
  resetOverlay_->setPosition(Edge::Left, 0)->setPosition(Edge::Top, 0)->setZIndex(1000);
  resetOverlay_->setFlexDirection(FlexDirection::Column)->setJustifyContent(YGJustifyCenter)
      ->setAlignItems(YGAlignCenter)->setPadding(Edge::All, 24)->setThemedBackgroundColor(app_theme::scrim);
  auto *panel = card(resetOverlay_);
  panel->setName("streakResetPanel");
  panel->setWidthPercent(100)->setFlexShrink(1);
  YGNodeStyleSetHeightPercent(panel->getNode(), 100);
  YGNodeStyleSetMaxWidth(panel->getNode(), 640);
  YGNodeStyleSetMaxHeight(panel->getNode(), 620);
  label(panel, i18n::tr("streak.ui.reset_challenge_title"), 28);
  auto *scroll = new ScrollView();
  scroll->setFlex(1)->setMinHeight(0);
  auto *details = new View();
  details->setName("streakResetDetails");
  details->setFlexDirection(FlexDirection::Column)->setGap(12);
  label(details, original->name, 23);
  label(details, i18n::tr("streak.ui.reset_challenge_help"), 20);
  label(details, levelsText(*original), 18);
  assignmentConditions(details, *original);
  scroll->setContentView(details);
  panel->addView(scroll);
  auto *buttons = new View();
  buttons->setFlexDirection(FlexDirection::Row)->setGap(12)->setFlexShrink(0);
  action(buttons, i18n::tr("streak.ui.cancel"), [this] {
    later([this] { pendingResetAssignment_.clear(); });
  }, app_theme::ActionCategory::Neutral)->setFlex(1)->setName("streakResetCancel");
  action(buttons, i18n::tr("streak.ui.reset_challenge_confirm"), [this] {
    later([this] {
      const auto id = pendingResetAssignment_;
      pendingResetAssignment_.clear();
      if (profileId_ != context.profileManager.activeProfile().id) { reload(); return; }
      std::string error;
      if (context.scoreRepository.ResetStreakExtraChallenge(id, pendingResetDay_, error)) {
        streakProgress_.reset(); roundCue_ = {}; goalIgnitionSoundPending_ = false;
        reload();
        message_ = i18n::tr("streak.ui.reset_challenge_done");
      } else message_ = error;
    });
  }, app_theme::ActionCategory::Confirmation)->setFlex(1)->setName("streakResetConfirm");
  panel->addView(buttons);
  root_->addView(resetOverlay_);
}
void StreakScene::routine(View *body) {
  body = card(body);
  label(body, i18n::tr("streak.ui.routine"), 28);
  label(body, i18n::tr("streak.ui.edit_help"), 18);
  field(body, i18n::tr("streak.ui.daily"), daily_, &fieldErrors_["daily"]);
  field(body, i18n::tr("streak.ui.weekly"), weekly_, &fieldErrors_["weekly"]);
  field(body, i18n::tr("streak.ui.monthly"), monthly_, &fieldErrors_["monthly"]);
  auto *assignments = new View();
  assignments->setFlexDirection(FlexDirection::Column)->setGap(12)->setFlexShrink(0);
  body->addView(assignments);
  for (std::size_t i = 0; i < draft_.assignments.size(); ++i) {
    const auto &a = draft_.assignments[i];
    label(assignments, a.name + "  ·  " + levelsText(a), 22);
    label(assignments, i18n::plural("streak.ui.chart_count", a.chartCount).resolve() + " · " + i18n::plural("streak.ui.reward", a.points).resolve(), 18);
    assignmentConditions(assignments, a);
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row)->setGap(10)->setFlexShrink(0);
    action(row, i18n::tr("streak.ui.edit_assignment"), [this, i] { later([this, i] { editAssignment(static_cast<int>(i)); }); }, app_theme::ActionCategory::Neutral, ui_icons::kPen)->setFlex(1);
    action(row, i18n::tr("streak.ui.remove"), [this, i] { later([this, i] { draft_.assignments.erase(draft_.assignments.begin() + i); }); }, app_theme::ActionCategory::Destructive, ui_icons::kTrash)->setFlex(1);
    assignments->addView(row);
  }
  action(assignments, i18n::tr("streak.ui.add"), [this] { later([this] { editAssignment(-1); }); }, app_theme::ActionCategory::Neutral, ui_icons::kPlus)->setEnabled(draft_.assignments.size() < 100);
  fieldError(body, assignments, fieldErrors_["assignments"]);
  action(body, i18n::tr("streak.ui.save"), [this] { later([this] { saveRoutine(); }); }, app_theme::ActionCategory::Primary, ui_icons::kSave);
}
void StreakScene::editAssignment(int index) {
  scrollOffsets_[static_cast<std::size_t>(Page::Assignment)] = 0;
  adjustingToday_ = false;
  fieldErrors_.clear();
  editingIndex_ = index;
  assignment_ = index < 0 ? streak::Assignment{} : draft_.assignments[index];
  if (index < 0) { assignment_.id = uuid::generateV4(); assignment_.name = i18n::tr("streak.ui.default_assignment"); }
  name_ = assignment_.name;
  count_ = std::to_string(assignment_.chartCount);
  points_ = std::to_string(assignment_.points);
  rate_ = std::to_string(100.0 * assignment_.scoreNumerator / assignment_.scoreDenominator);
  page_ = Page::Assignment;
  message_.clear();
}
void StreakScene::editor(View *body) {
  body = card(body);
  if (adjustingToday_) label(body, i18n::tr("streak.ui.adjust_help"), 18);
  field(body, i18n::tr("streak.ui.name"), name_, &fieldErrors_["name"]);
  auto session = context.chartRepository.OpenSession();
  const auto tables = session ? session->SelectDifficultyTables() : std::vector<DifficultyTableInfo>{};
  if (std::none_of(tables.begin(), tables.end(), [this](const auto &t) { return t.id == tableId_; }))
    tableId_ = tables.empty() ? 0 : tables.front().id;
  std::vector<DropdownView::Option> tableOptions;
  for (const auto &table : tables) tableOptions.push_back({std::to_string(table.id), table.name});
  select(body, portal_, i18n::tr("streak.ui.table"), std::move(tableOptions), std::to_string(tableId_), [this](const std::string &id) {
    later([this, id] { tableId_ = integer(id); });
  }, tables.empty() ? fieldErrors_["levels"] : std::string{});
  auto table = std::find_if(tables.begin(), tables.end(), [this](const auto &t) { return t.id == tableId_; });
  if (table != tables.end()) {
    auto levels = session->SelectDifficultyLevels(tableId_);
    std::vector<DropdownView::Option> options{{"", i18n::tr("streak.ui.choose_level")}};
    for (std::size_t i = 0; i < levels.size(); ++i)
      options.push_back({std::to_string(i), levels[i].tableSymbol + levels[i].level});
    select(body, portal_, i18n::tr("streak.ui.level"), std::move(options), "", [this, levels, source = table->sourceUrl](const std::string &id) {
      if (id.empty()) return;
      later([this, id, levels, source] {
        const auto &level = levels.at(integer(id));
        const auto duplicate = std::any_of(assignment_.levels.begin(), assignment_.levels.end(), [&](const auto &l) { return l.source == source && l.level == level.level; });
        fieldErrors_.erase("levels");
        if (!duplicate) assignment_.levels.push_back({source, level.level, level.tableName + " · " + level.tableSymbol + level.level});
      });
    }, fieldErrors_["levels"]);
  } else label(body, i18n::tr("streak.ui.no_tables"));
  for (std::size_t i = 0; i < assignment_.levels.size(); ++i) {
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row)->setGap(12)->setAlignItems(YGAlignCenter)->setFlexShrink(0);
    label(row, assignment_.levels[i].label, 20)->setFlex(1)->setMinWidth(0);
    action(row, i18n::tr("streak.ui.remove_level"), [this, i] {
      later([this, i] { assignment_.levels.erase(assignment_.levels.begin() + i); });
    }, app_theme::ActionCategory::Destructive, ui_icons::kTrash)->setWidth(140);
    body->addView(row);
  }
  std::vector<DropdownView::Option> eligible{{"-2", i18n::tr("streak.ui.any_previous_lamp")}};
  for (int rank : {-1, 0, 100, 150, 200, 300, 400, 500, 600}) {
    const auto color = hasClearLampColor(rank) ? std::optional(clearLampColorForRank(rank)) : std::nullopt;
    eligible.push_back({.id=std::to_string(rank), .label=lampText(rank), .leadingColor=color});
    if (rank >= 0) eligible.push_back({.id=std::to_string(rank) + "below",
        .label=comparison("streak.ui.at_most", lampText(rank)), .leadingColor=color});
  }
  select(body, portal_, i18n::tr("streak.ui.eligible"), std::move(eligible), std::to_string(assignment_.eligibleLamp) + (assignment_.eligibleOrBelow ? "below" : ""), [this](const std::string &id) {
    assignment_.eligibleOrBelow = id.ends_with("below");
    assignment_.eligibleLamp = integer(assignment_.eligibleOrBelow ? id.substr(0, id.size() - 5) : id);
  });
  std::vector<DropdownView::Option> previousScores{{"-2", i18n::tr("streak.ui.any_previous_score")}};
  for (int rank : {-1, 0, 6, 9, 12, 15, 18, 21, 24, 26, 27}) {
    previousScores.push_back({std::to_string(rank), streak::scoreRankLabel(rank)});
    if (rank >= 0) previousScores.push_back({std::to_string(rank) + "below",
        comparison("streak.ui.at_most", streak::scoreRankLabel(rank))});
  }
  select(body, portal_, i18n::tr("streak.ui.eligible_score"), std::move(previousScores),
      std::to_string(assignment_.eligibleScoreRank) + (assignment_.eligibleScoreOrBelow ? "below" : ""),
      [this](const std::string &id) {
        assignment_.eligibleScoreOrBelow = id.ends_with("below");
        assignment_.eligibleScoreRank = integer(assignment_.eligibleScoreOrBelow ? id.substr(0, id.size() - 5) : id);
      });
  std::vector<DropdownView::Option> lamps{{"-1", i18n::tr("streak.ui.any_lamp")}};
  for (int rank : {0, 100, 150, 200, 300, 400, 500, 600})
    lamps.push_back({.id=std::to_string(rank), .label=comparison("streak.ui.at_least", lampText(rank)),
                     .leadingColor=clearLampColorForRank(rank)});
  select(body, portal_, i18n::tr("streak.ui.lamp"), std::move(lamps), std::to_string(assignment_.targetLamp), [this](const std::string &id) { assignment_.targetLamp = integer(id);
    later([this] { fieldErrors_.erase("lamp"); fieldErrors_.erase("score"); });
  }, fieldErrors_["lamp"]);
  std::vector<DropdownView::Option> scores{{"0", i18n::tr("streak.ui.any_score")}, {"6", "E"}, {"9", "D"}, {"12", "C"}, {"15", "B"}, {"18", "A"}, {"21", "AA"}, {"24", "AAA"}, {"26", "MAX -"}, {"27", "MAX"}, {"custom", i18n::tr("streak.ui.custom_option")}};
  auto *customRate = new View();
  customRate->setFlexDirection(FlexDirection::Column)->setGap(10)->setFlexShrink(0);
  field(customRate, i18n::tr("streak.ui.custom_rate"), rate_, &fieldErrors_["rate"]);
  const auto showCustomRate = [customRate](bool visible) {
    customRate->setDisplay(visible ? YGDisplayFlex : YGDisplayNone);
    customRate->setVisible(visible);
  };
  showCustomRate(assignment_.customRate);
  select(body, portal_, i18n::tr("streak.ui.score"), std::move(scores), assignment_.customRate ? "custom" : std::to_string(assignment_.scoreNumerator), [this, showCustomRate](const std::string &id) {
    assignment_.customRate = id == "custom";
    showCustomRate(assignment_.customRate);
    layout();
    later([this] { fieldErrors_.erase("lamp"); fieldErrors_.erase("score"); fieldErrors_.erase("rate"); });
    if (!assignment_.customRate) { assignment_.scoreNumerator = integer(id); assignment_.scoreDenominator = 27; }
  }, fieldErrors_["score"]);
  body->addView(customRate);
  field(body, i18n::tr("streak.ui.count"), count_, &fieldErrors_["count"]);
  if (!adjustingToday_) field(body, i18n::tr("streak.ui.points"), points_, &fieldErrors_["points"]);
  action(body, i18n::tr("streak.ui.keep"), [this] { later([this] { acceptAssignment(); }); }, app_theme::ActionCategory::Primary, ui_icons::kCheck);
}
void StreakScene::acceptAssignment() {
  message_.clear();
  auto candidate = assignment_;
  fieldErrors_ = streak::readAssignmentForm(candidate, name_, count_, points_, rate_);
  if (!fieldErrors_.empty()) return;
  assignment_ = std::move(candidate);
  try {
    auto check = draft_;
    check.assignments = {assignment_};
    if (auto error = streak::validate(check); !error.empty()) throw std::invalid_argument(error);
    if (adjustingToday_) {
      if (checkedDay_ != streak::today())
        throw std::runtime_error(i18n::tr("streak.ui.day_changed"));
      std::string error;
      const auto current = context.scoreRepository.LoadStreak(error);
      if (!current) throw std::runtime_error(error);
      const auto day = current->days.find(checkedDay_);
      if (day == current->days.end() ||
          !unavailableAssignments(day->second).contains(assignment_.id))
        throw std::runtime_error(i18n::tr("streak.ui.adjust_unavailable"));
      if (!context.scoreRepository.AdjustStreakAssignment(assignment_, checkedDay_, error))
        throw std::runtime_error(error);
      adjustingToday_ = false;
      page_ = Page::Dashboard;
      reload();
      message_ = i18n::tr("streak.ui.adjusted");
      return;
    }
    if (editingIndex_ < 0) draft_.assignments.push_back(assignment_);
    else draft_.assignments.at(editingIndex_) = assignment_;
    page_ = Page::Routine;
    message_ = i18n::tr("streak.ui.unsaved");
  } catch (const std::exception &error) { message_ = error.what(); }
}
void StreakScene::saveRoutine() {
  message_.clear();
  auto candidate = draft_;
  fieldErrors_ = streak::readRoutineForm(candidate, daily_, weekly_, monthly_);
  if (!fieldErrors_.empty()) return;
  draft_ = std::move(candidate);
  try {
    streak::DayId effective = 0;
    std::string error;
    if (!context.scoreRepository.ConfigureStreak(draft_, effective, error)) throw std::runtime_error(error);
    message_ = comparison("streak.ui.saved", streak::dateLabel(effective));
    page_ = Page::Dashboard;
    reload();
  } catch (const std::exception &error) { message_ = error.what(); }
}
void StreakScene::layout() {
  if (!root_) return;
  root_->setSize(rendering::window_width, rendering::window_height);
  portal_->setSize(rendering::window_width, rendering::window_height);
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  const auto safe = GetIOSSafeAreaInsetsNormalized();
  root_->setPadding(Edge::Top, 18 + safe.top * rendering::window_height)
      ->setPadding(Edge::Bottom, 18 + safe.bottom * rendering::window_height)
      ->setPadding(Edge::Left, 18 + safe.left * rendering::window_width)
      ->setPadding(Edge::Right, 18 + safe.right * rendering::window_width);
#endif
  if (embeddedTab_) root_->setPadding(Edge::Top, 18 + tabTopInset_);
  if (resetOverlay_) {
    resetOverlay_->setSize(rendering::window_width, rendering::window_height);
    resetOverlay_->setPadding(Edge::Top, 24 + (embeddedTab_ ? tabTopInset_ : 0));
  }
  root_->applyYogaLayout();
  if (resetOverlay_) { sizeResetChallengeModal(resetOverlay_); root_->applyYogaLayout(); }
  scroll_->refreshContentLayout();
}
void StreakScene::back() {
  if (page_ == Page::Assignment) { later([this] { page_ = adjustingToday_ ? Page::Dashboard : Page::Routine; adjustingToday_ = false; fieldErrors_.clear(); message_.clear(); }); return; }
  if (page_ == Page::Routine) { later([this] { page_ = Page::Dashboard; message_.clear(); reload(); }); return; }
  if (embeddedTab_) { later([this] { if (tabClose_) tabClose_(); }); return; }
  defer([this] { (void)returnToScene(*context.sceneManager, returnTarget_); return false; }, 0, true);
}
void StreakScene::update(float dt) {
  if (page_ == Page::Dashboard && goalIgnitionSoundPending_) {
    goalIgnitionSoundPending_ = false;
    if (SDL_GetTicks() - streakProgressStartedAt_ < 750)
      streakSounds_.play(context.jukebox.audioRuntime(), streak::Sound::Complete);
  }
  if (page_ == Page::Dashboard && roundCue_.consume(SDL_GetTicks()))
    streakSounds_.play(context.jukebox.audioRuntime(), streak::Sound::RoundAdvance);
  elapsed_ += dt;
  if (profileId_ != context.profileManager.activeProfile().id) {
    page_ = Page::Dashboard; message_.clear(); reload(); build();
  } else if (elapsed_ >= 30) {
    elapsed_ = 0;
    if (checkedDay_ != streak::today() && page_ == Page::Dashboard) { reload(); build(); }
  }
}
void StreakScene::onPresentationOrientationChanged() { layout(); }
void StreakScene::onLanguageChanged() { build(); }
EventHandleResult StreakScene::handleEvents(SDL_Event &event) {
  if (!pendingResetAssignment_.empty() && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
    later([this] { pendingResetAssignment_.clear(); });
    return {};
  }
  for (auto it = views.rbegin(); it != views.rend(); ++it)
    if (!(*it)->handleEvents(event)) return {};
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) back();
  return {};
}
void StreakScene::cleanupScene() { root_ = nullptr; scroll_ = nullptr; portal_ = nullptr; resetOverlay_ = nullptr; }
