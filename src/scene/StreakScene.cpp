#include "StreakScene.h"
#include "SceneManager.h"
#include "../Uuid.h"
#include "../rendering/common.h"
#include "../view/ActionButtonStyle.h"
#include "../view/AppTheme.h"
#include "../view/Button.h"
#include "../view/DropdownView.h"
#include "../view/OverlayPortal.h"
#include "../view/ScrollView.h"
#include "../view/TextInputBox.h"
#include "../view/TextView.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

namespace {
TextView *label(View *parent, const std::string &value, int size = 21) {
  auto *text = new TextView(app_theme::font, size);
  text->setText(value);
  text->setWrap(true);
  text->setMinHeight(size + 12)->setFlexShrink(0);
  text->setThemedColor(app_theme::textPrimary);
  parent->addView(text);
  return text;
}
Button *action(View *parent, const std::string &value, std::function<void()> click,
               app_theme::ActionCategory category = app_theme::ActionCategory::Neutral) {
  auto *button = new Button();
  button->setHeight(52)->setMinWidth(0)->setFlexShrink(0);
  auto *text = new TextView(app_theme::font, 21);
  text->setText(value);
  text->setAlign(TextView::CENTER);
  text->setVAlign(TextView::MIDDLE);
  button->setContentView(text);
  action_button::applyStyle(button, text, category);
  button->setOnClickListener(std::move(click));
  parent->addView(button);
  return button;
}
void field(View *parent, const std::string &caption, std::string &value) {
  label(parent, caption, 18);
  auto *input = new TextInputBox(app_theme::font, 22);
  input->setHeight(52)->setFlexShrink(0)->setPadding(Edge::All, 10);
  input->setThemedBackgroundColor(app_theme::insetSurface);
  input->setThemedColor(app_theme::textPrimary);
  input->setEditingText(value);
  input->onTextChanged([&value](const std::string &next) { value = next; });
  parent->addView(input);
}
void select(View *parent, OverlayPortal *portal, const std::string &caption,
            std::vector<DropdownView::Option> options, std::string selected,
            std::function<void(const std::string &)> change) {
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
  dropdown->setHeight(52)->setFlexShrink(0);
  dropdown->setTriggerWidth(300);
  dropdown->refresh(*state);
  parent->addView(dropdown);
}
int integer(const std::string &value) {
  int number = 0;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
  if (error != std::errc{} || end != value.data() + value.size())
    throw std::invalid_argument("Enter whole numbers for goals, chart counts and points.");
  return number;
}
std::string levelsText(const streak::Assignment &a) {
  std::string text;
  for (const auto &level : a.levels) {
    if (!text.empty()) text += ", ";
    text += level.label.empty() ? level.level : level.label;
  }
  return text;
}
std::string requirements(const streak::Assignment &a) {
  return (a.targetLamp < 0 ? "Any lamp" : streak::lampLabel(a.targetLamp) + " or above") +
         "  ·  " + streak::scoreLabel(a);
}
} // namespace

StreakScene::StreakScene(ApplicationContext &context, SceneReturnTarget target)
    : Scene(context), returnTarget_(std::move(target)) {}
void StreakScene::init() { reload(); build(); }
void StreakScene::reload() {
  if (profileId_ != context.profileManager.activeProfile().id || checkedDay_ != streak::today())
    showOptional_ = false;
  profileId_ = context.profileManager.activeProfile().id;
  std::string error;
  auto loaded = context.scoreRepository.LoadStreak(error);
  loadFailed_ = !loaded;
  if (!loaded) { message_ = error; return; }
  state_ = std::move(*loaded);
  draft_ = state_.configurations.empty() ? streak::Config{} : state_.configurations.rbegin()->second;
  daily_ = std::to_string(draft_.dailyGoal);
  weekly_ = std::to_string(draft_.weeklyGoal);
  monthly_ = std::to_string(draft_.monthlyGoal);
  checkedDay_ = streak::today();
}
void StreakScene::later(std::function<void()> action) {
  defer([this, action = std::move(action)] { action(); build(); return true; }, 0, true);
}
void StreakScene::build() {
  for (auto *view : views) delete view;
  views.clear();
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
  action(header, i18n::tr("streak.ui.back"), [this] { back(); })->setWidth(110);
  root_->addView(header);
  scroll_ = new ScrollView();
  scroll_->setFlex(1)->setMinHeight(0);
  root_->addView(scroll_);
  auto *body = new View();
  body->setFlexDirection(FlexDirection::Column)->setAlignItems(YGAlignStretch)
      ->setGap(10)->setPadding(Edge::Bottom, 24);
  scroll_->setContentView(body);
  if (!message_.empty()) label(body, message_, 19);
  if (loadFailed_) {
    action(body, i18n::tr("streak.ui.retry"), [this] { later([this] { reload(); }); });
  } else if (page_ == Page::Dashboard) dashboard(body);
  else if (page_ == Page::Routine) routine(body);
  else editor(body);
  addView(portal_);
  layout();
}
void StreakScene::dashboard(View *body) {
  if (state_.days.empty()) {
    label(body, i18n::tr("streak.ui.welcome"), 26);
    label(body, i18n::tr("streak.ui.rules"));
    action(body, i18n::tr("streak.ui.create"), [this] { later([this] { page_ = Page::Routine; }); }, app_theme::ActionCategory::Primary);
    return;
  }
  const auto &day = state_.days.rbegin()->second;
  const auto totals = streak::summary(state_);
  const bool complete = day.points >= day.config.dailyGoal;
  const int bonus = streak::bonusPoints(day);
  const int bonusLimit = streak::bonusLimit(day);
  const bool bonusAvailable = bonus < bonusLimit;
  auto *summary = body;
  if (complete) {
    summary = new View();
    summary->setFlexDirection(FlexDirection::Column)->setGap(10)->setPadding(Edge::All, 20)
        ->setFlexShrink(0)->setThemedBackgroundColor(app_theme::panelStrong);
    summary->setName("streakDayComplete");
    label(summary, i18n::tr("streak.ui.complete"), 32)->setThemedColor(app_theme::lime);
    body->addView(summary);
  }
  label(summary, std::to_string(state_.currentStreak) + " day streak", 38);
  label(summary, "Best: " + std::to_string(state_.bestStreak) + "  ·  " + streak::dateLabel(totals.day), 18);
  label(summary, std::to_string(std::min<std::int64_t>(day.points, day.config.dailyGoal)) +
      " / " + std::to_string(day.config.dailyGoal) + " points today", 28);
  label(summary, complete ? i18n::tr("streak.ui.complete_help") :
              day.played ? i18n::tr("streak.ui.protected") : i18n::tr("streak.ui.pending"), 20);
  if (complete)
    action(summary, i18n::tr("streak.ui.done"), [this] { back(); }, app_theme::ActionCategory::Confirmation);
  label(body, "This week: " + std::to_string(totals.weeklyPoints) + " / " + std::to_string(day.config.weeklyGoal) +
              "   ·   This month: " + std::to_string(totals.monthlyPoints) + " / " + std::to_string(day.config.monthlyGoal), 20);
  label(body, "Savings: " + std::to_string(state_.savings) + " points   ·   Freezes: " + std::to_string(state_.freezes) + " / 2", 23);
  action(body, "Buy a freeze · " + std::to_string(streak::freezePrice(state_)) + " points", [this] {
    later([this] {
      std::string error;
      message_ = context.scoreRepository.BuyStreakFreeze(error) ? i18n::tr("streak.ui.freeze_bought") : error;
      reload();
    });
  });
  label(body, i18n::tr("streak.ui.freeze_price_help"), 18);
  action(body, i18n::tr("streak.ui.edit"), [this] { later([this] { message_.clear(); page_ = Page::Routine; }); });
  if (state_.configurations.rbegin()->first > totals.day)
    label(body, "Routine changes scheduled for " + streak::dateLabel(state_.configurations.rbegin()->first), 19);
  if (complete) {
    label(body, std::string(i18n::tr("streak.ui.shared_bonus")) + " " + std::to_string(bonus) +
                " / " + std::to_string(bonusLimit), 20);
    label(body, i18n::tr(bonusAvailable ? "streak.ui.optional_help" : "streak.ui.bonus_complete"), 18);
    if (bonusAvailable)
      action(body, i18n::tr(showOptional_ ? "streak.ui.hide_optional" : "streak.ui.show_optional"),
          [this] { later([this] { showOptional_ = !showOptional_; }); });
  } else label(body, i18n::tr("streak.ui.assignments"), 27);
  if (!complete && day.assignments.empty()) label(body, i18n::tr("streak.ui.no_assignments"));
  for (const auto &p : day.assignments) {
    if (complete && (!showOptional_ || !bonusAvailable)) break;
    auto *card = new View();
    card->setFlexDirection(FlexDirection::Column)->setGap(6)->setPadding(Edge::All, 14)
        ->setFlexShrink(0)->setThemedBackgroundColor(app_theme::insetSurface);
    const auto reward = complete ? 1 :
        std::min<std::int64_t>(p.requirement.points, day.config.dailyGoal - day.points);
    label(card, p.requirement.name + "  ·  +" + std::to_string(reward) + " points", 24);
    if (complete)
      label(card, std::string(i18n::tr("streak.ui.bonus_today")) + " " + std::to_string(p.bonusRounds), 18);
    label(card, std::to_string(complete ? 0 : p.charts) + " / " +
                std::to_string(complete ? 1 : p.requirement.chartCount) +
                " distinct charts  ·  Round " + std::to_string(p.completedRounds + 1), 22);
    label(card, levelsText(p.requirement), 18);
    label(card, requirements(p.requirement), 19);
    label(card, "Previous lamp: " + streak::lampLabel(p.requirement.eligibleLamp) +
                (p.requirement.eligibleOrBelow ? " or below" : ""), 18);
    body->addView(card);
  }
  label(body, i18n::tr("streak.ui.milestones"), 27);
  std::string badges;
  for (int n : {7, 14, 30, 50, 100, 365}) {
    if (!badges.empty()) badges += "  ·  ";
    badges += std::to_string(n) + "d" + (state_.milestones.contains("streak:" + std::to_string(n)) ? " [earned]" : "");
  }
  label(body, badges);
  int weeks = 0, months = 0;
  for (const auto &id : state_.milestones) {
    weeks += id.starts_with("week:"); months += id.starts_with("month:");
  }
  label(body, "Weekly goals earned: " + std::to_string(weeks) + "  ·  Monthly goals earned: " + std::to_string(months), 19);
  label(body, i18n::tr("streak.ui.rules"), 18);
}
void StreakScene::routine(View *body) {
  label(body, i18n::tr("streak.ui.routine"), 28);
  label(body, i18n::tr("streak.ui.edit_help"), 18);
  field(body, i18n::tr("streak.ui.daily"), daily_);
  field(body, i18n::tr("streak.ui.weekly"), weekly_);
  field(body, i18n::tr("streak.ui.monthly"), monthly_);
  for (std::size_t i = 0; i < draft_.assignments.size(); ++i) {
    const auto &a = draft_.assignments[i];
    label(body, a.name + "  ·  " + levelsText(a), 22);
    label(body, std::to_string(a.chartCount) + " charts · " + requirements(a) + " · +" + std::to_string(a.points) + " points", 18);
    auto *row = new View();
    row->setFlexDirection(FlexDirection::Row)->setGap(10)->setFlexShrink(0);
    action(row, i18n::tr("streak.ui.edit_assignment"), [this, i] { later([this, i] { editAssignment(static_cast<int>(i)); }); })->setFlex(1);
    action(row, i18n::tr("streak.ui.remove"), [this, i] { later([this, i] { draft_.assignments.erase(draft_.assignments.begin() + i); }); })->setFlex(1);
    body->addView(row);
  }
  action(body, i18n::tr("streak.ui.add"), [this] { later([this] { editAssignment(-1); }); });
  action(body, i18n::tr("streak.ui.save"), [this] { later([this] { saveRoutine(); }); }, app_theme::ActionCategory::Primary);
}
void StreakScene::editAssignment(int index) {
  editingIndex_ = index;
  assignment_ = index < 0 ? streak::Assignment{} : draft_.assignments[index];
  if (index < 0) { assignment_.id = uuid::generateV4(); assignment_.name = "Daily practice"; }
  name_ = assignment_.name;
  count_ = std::to_string(assignment_.chartCount);
  points_ = std::to_string(assignment_.points);
  rate_ = std::to_string(100.0 * assignment_.scoreNumerator / assignment_.scoreDenominator);
  page_ = Page::Assignment;
  message_.clear();
}
void StreakScene::editor(View *body) {
  field(body, i18n::tr("streak.ui.name"), name_);
  auto session = context.chartRepository.OpenSession();
  const auto tables = session ? session->SelectDifficultyTables() : std::vector<DifficultyTableInfo>{};
  if (std::none_of(tables.begin(), tables.end(), [this](const auto &t) { return t.id == tableId_; }))
    tableId_ = tables.empty() ? 0 : tables.front().id;
  std::vector<DropdownView::Option> tableOptions;
  for (const auto &table : tables) tableOptions.push_back({std::to_string(table.id), table.name});
  select(body, portal_, i18n::tr("streak.ui.table"), std::move(tableOptions), std::to_string(tableId_), [this](const std::string &id) {
    later([this, id] { tableId_ = integer(id); });
  });
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
        if (!duplicate) assignment_.levels.push_back({source, level.level, level.tableName + " · " + level.tableSymbol + level.level});
      });
    });
  } else label(body, i18n::tr("streak.ui.no_tables"));
  for (std::size_t i = 0; i < assignment_.levels.size(); ++i)
    action(body, assignment_.levels[i].label + "  ×", [this, i] { later([this, i] { assignment_.levels.erase(assignment_.levels.begin() + i); }); });
  std::vector<DropdownView::Option> eligible{{"-2", "Any previous lamp"}};
  for (int rank : {-1, 0, 100, 150, 200, 300, 400, 500, 600}) {
    eligible.push_back({std::to_string(rank), streak::lampLabel(rank)});
    if (rank >= 0) eligible.push_back({std::to_string(rank) + "below", streak::lampLabel(rank) + " or below"});
  }
  select(body, portal_, i18n::tr("streak.ui.eligible"), std::move(eligible), std::to_string(assignment_.eligibleLamp) + (assignment_.eligibleOrBelow ? "below" : ""), [this](const std::string &id) {
    assignment_.eligibleOrBelow = id.ends_with("below");
    assignment_.eligibleLamp = integer(assignment_.eligibleOrBelow ? id.substr(0, id.size() - 5) : id);
  });
  std::vector<DropdownView::Option> lamps{{"-1", "Any lamp"}};
  for (int rank : {0, 100, 150, 200, 300, 400, 500, 600})
    lamps.push_back({std::to_string(rank), streak::lampLabel(rank) + " or above"});
  select(body, portal_, i18n::tr("streak.ui.lamp"), std::move(lamps), std::to_string(assignment_.targetLamp), [this](const std::string &id) { assignment_.targetLamp = integer(id); });
  std::vector<DropdownView::Option> scores{{"0", "Any score"}, {"6", "E"}, {"9", "D"}, {"12", "C"}, {"15", "B"}, {"18", "A"}, {"21", "AA"}, {"24", "AAA"}, {"26", "MAX -"}, {"27", "MAX"}, {"custom", "Custom EX rate"}};
  select(body, portal_, i18n::tr("streak.ui.score"), std::move(scores), assignment_.customRate ? "custom" : std::to_string(assignment_.scoreNumerator), [this](const std::string &id) {
    assignment_.customRate = id == "custom";
    if (!assignment_.customRate) { assignment_.scoreNumerator = integer(id); assignment_.scoreDenominator = 27; }
  });
  field(body, i18n::tr("streak.ui.custom_rate"), rate_);
  field(body, i18n::tr("streak.ui.count"), count_);
  field(body, i18n::tr("streak.ui.points"), points_);
  label(body, i18n::tr("streak.ui.repeat_help"), 18);
  action(body, i18n::tr("streak.ui.keep"), [this] { later([this] { acceptAssignment(); }); }, app_theme::ActionCategory::Primary);
}
void StreakScene::acceptAssignment() {
  try {
    assignment_.name = name_;
    assignment_.chartCount = integer(count_);
    assignment_.points = integer(points_);
    if (assignment_.customRate) {
      std::size_t end = 0;
      const double rate = std::stod(rate_, &end);
      if (end != rate_.size() || !std::isfinite(rate) || rate <= 0 || rate > 100)
        throw std::invalid_argument("Enter an EX rate above 0 and at most 100%.");
      assignment_.scoreNumerator = static_cast<int>(std::lround(rate * 100));
      assignment_.scoreDenominator = 10000;
      if (assignment_.scoreNumerator == 0) throw std::invalid_argument("Minimum EX rate is 0.01%.");
    }
    auto check = draft_;
    check.assignments = {assignment_};
    if (auto error = streak::validate(check); !error.empty()) throw std::invalid_argument(error);
    if (editingIndex_ < 0) draft_.assignments.push_back(assignment_);
    else draft_.assignments.at(editingIndex_) = assignment_;
    page_ = Page::Routine;
    message_ = i18n::tr("streak.ui.unsaved");
  } catch (const std::exception &error) { message_ = error.what(); }
}
void StreakScene::saveRoutine() {
  try {
    draft_.dailyGoal = integer(daily_);
    draft_.weeklyGoal = integer(weekly_);
    draft_.monthlyGoal = integer(monthly_);
    streak::DayId effective = 0;
    std::string error;
    if (!context.scoreRepository.ConfigureStreak(draft_, effective, error)) throw std::runtime_error(error);
    message_ = "Routine saved. Effective " + streak::dateLabel(effective) + ".";
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
  root_->applyYogaLayout();
  scroll_->refreshContentLayout();
}
void StreakScene::back() {
  if (page_ == Page::Assignment) { later([this] { page_ = Page::Routine; message_.clear(); }); return; }
  if (page_ == Page::Routine) { later([this] { page_ = Page::Dashboard; message_.clear(); reload(); }); return; }
  defer([this] { (void)returnToScene(*context.sceneManager, returnTarget_); return false; }, 0, true);
}
void StreakScene::update(float dt) {
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
  for (auto it = views.rbegin(); it != views.rend(); ++it)
    if (!(*it)->handleEvents(event)) return {};
  if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) back();
  return {};
}
void StreakScene::cleanupScene() { root_ = nullptr; scroll_ = nullptr; portal_ = nullptr; }
