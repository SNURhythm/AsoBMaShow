#include "view/Button.h"
#include "view/CheckboxButtonContent.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "view/DropdownView.h"
#undef private
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "view/PlayOptionsPanelView.h"
#include "view/ScrollView.h"
#include "view/SnappedSlider.h"
#include "view/TextView.h"
#include "view/TextInputBox.h"
#include "i18n/Localization.h"
#include "rendering/UniformCache.h"
#include "scene/SettingsSceneInputLayout.h"

#include <SDL2/SDL.h>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0f;
float heightScale = 1.0f;
float ui_scale_x = 1.0f;
float ui_scale_y = 1.0f;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void click(Button &button) {
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.x = button.getX() + button.getWidth() / 2;
  down.button.y = button.getY() + button.getHeight() / 2;
  SDL_Event up = down;
  up.type = SDL_MOUSEBUTTONUP;
  up.button.type = SDL_MOUSEBUTTONUP;
  button.handleEvents(down);
  button.handleEvents(up);
}

void testSliderReleaseConsumedByDisabledSiblingEndsOnlyItsGesture() {
  for (bool touch : {false, true}) {
    View root(0, 0, 240, 120);
    int changes = 0;
    auto *slider = new SnappedSlider([&](int) { ++changes; });
    slider->setSize(240, 50);
    slider->setPosition(0, 0, YGPositionTypeAbsolute);
    root.addView(slider);
    auto *disabled = new Button(0, 60, 240, 50);
    disabled->setPosition(0, 60, YGPositionTypeAbsolute);
    disabled->setEnabled(false);
    root.addView(disabled);
    root.applyYogaLayout();
    const auto pointer = [&](Uint32 type, int x, int y, SDL_FingerID id = 7) {
      SDL_Event event{};
      event.type = type;
      if (touch) {
        event.tfinger.touchId = 1;
        event.tfinger.fingerId = id;
        event.tfinger.x = static_cast<float>(x) / rendering::window_width;
        event.tfinger.y = static_cast<float>(y) / rendering::window_height;
      } else if (type == SDL_MOUSEMOTION) {
        event.motion.which = 1;
        event.motion.x = x;
        event.motion.y = y;
      } else {
        event.button.which = 1;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = x;
        event.button.y = y;
      }
      return event;
    };
    auto down = pointer(touch ? SDL_FINGERDOWN : SDL_MOUSEBUTTONDOWN, 60, 25);
    require(!root.handleEvents(down), "slider begins the gesture");
    auto unrelated = pointer(touch ? SDL_FINGERUP : SDL_MOUSEBUTTONUP, 220, 80, 8);
    if (!touch) unrelated.button.button = SDL_BUTTON_RIGHT;
    require(!root.handleEvents(unrelated), "disabled sibling consumes unrelated release");
    auto move = pointer(touch ? SDL_FINGERMOTION : SDL_MOUSEMOTION, 120, 25);
    require(!root.handleEvents(move) && slider->value() == 50,
            "unrelated release preserves the active slider gesture");
    const int beforeRelease = changes;
    auto up = pointer(touch ? SDL_FINGERUP : SDL_MOUSEBUTTONUP, 220, 80);
    require(!root.handleEvents(up), "disabled sibling consumes slider release");
    require(slider->value() == 50 && changes == beforeRelease,
            "covered release does not change the value or invoke its callback");
    move = pointer(touch ? SDL_FINGERMOTION : SDL_MOUSEMOTION, 220, 25);
    require(root.handleEvents(move) && slider->value() == 50 &&
                changes == beforeRelease,
            "released slider cannot keep changing on later pointer motion");
    down = pointer(touch ? SDL_FINGERDOWN : SDL_MOUSEBUTTONDOWN, 220, 25, 9);
    require(!root.handleEvents(down) && slider->value() > 50,
            "slider accepts a fresh pointer after the covered release");
  }
}

const TextView *buttonText(const Button &button) {
  return dynamic_cast<const TextView *>(button.getContentView());
}

TextInputBox *findInput(View &view) {
  if (auto *input = dynamic_cast<TextInputBox *>(&view)) return input;
  for (auto *child : view.getChildren()) {
    if (auto *input = findInput(*child)) return input;
  }
  return nullptr;
}

void testLanguageRefreshPreservesPlayOptionsEditingAndScroll() {
  i18n::setLanguage(i18n::Language::English);
  ScrollView scroll(0, 0, 360, 240);
  auto *panel = new PlayOptionsPanelView(
      {}, {.width = 340.0f, .showLaneOrder = true}, nullptr);
  scroll.setContentView(panel);
  panel->refresh({.ruleset = GameplayRuleset::Beatoraja, .clubMode = true});
  scroll.applyYogaLayout();
  auto *input = findInput(*panel);
  require(input != nullptr, "play options exposes a lane-order input");
  input->setEditingText("7654321 unsaved");
  auto *heading = dynamic_cast<TextView *>(
      panel->findViewByName("ruleset-section-label"));
  require(heading != nullptr, "ruleset heading is inspectable");
  auto *club = dynamic_cast<Button *>(panel->findViewByName("club-mode"));
  auto *clubContent = dynamic_cast<CheckboxButtonContent *>(
      club == nullptr ? nullptr : club->getContentView());
  scroll.setScrollOffset(75.0f);
  const float offset = scroll.getScrollOffset();
  const auto english = heading->getText();

  i18n::setLanguage(i18n::Language::Korean);
  scroll.propagateLanguageChange();
  require(heading->getText() == i18n::tr("play_options.ruleset.label") &&
              heading->getText() != english,
          "retained panel resolves its original localized heading");
  require(input->getText() == "7654321 unsaved",
          "language refresh preserves unsaved lane-order edits");
  require(std::abs(scroll.getScrollOffset() - offset) < 0.001f,
          "language refresh preserves modal scroll");
  require(clubContent != nullptr && clubContent->checked() &&
              clubContent->labelView()->getText() ==
                  i18n::tr("play_options.club_beat.label"),
          "checkbox label translates without changing selection");
  i18n::setLanguage(i18n::Language::English);
}

void testLaneOrderDraftTracksAuthoritativeSelectionAndProfile() {
  PlayOptionsPanelView panel(
      {}, {.width = 340.0f, .showLaneOrder = true}, nullptr);
  PlayOptionsPanelState state{
      .playOption = "NORMAL",
      .defaultLaneOrder = "1234567",
      .laneOrderEnabled = true,
      .profileId = "first-profile"};
  panel.refresh(state);
  auto *input = findInput(panel);
  require(input != nullptr && input->getText() == "1234567",
          "initial authoritative lane order populates the input");
  input->setEditingText("7654321 draft");
  panel.refresh(state);
  require(input->getText() == "7654321 draft",
          "unchanged authoritative refresh preserves lane-order draft");
  state.clubMode = true;
  panel.refresh(state);
  require(input->getText() == "7654321 draft",
          "unrelated option refresh preserves lane-order draft");

  state.defaultLaneOrder = "123456789";
  panel.refresh(state);
  require(input->getText() == "123456789",
          "changed authoritative lane configuration replaces old draft");
  input->setEditingText("another draft");
  state.playOption = "MIRROR";
  panel.refresh(state);
  require(input->getText() == "123456789",
          "changed selected mode replaces old draft");
  input->setEditingText("previous profile draft");
  state.profileId = "second-profile";
  panel.refresh(state);
  require(input->getText() == "123456789",
          "new profile clears draft even when lane configuration matches");
}

void testScrollViewUsesPreciseWheelDeltaAndNaturalDirection() {
  ScrollView scroll(0, 0, 360, 200);
  auto *content = new View();
  content->setWidth(360)->setHeight(800);
  scroll.setContentView(content);
  scroll.applyYogaLayout();

  SDL_Event normal{};
  normal.type = SDL_MOUSEWHEEL;
  normal.wheel.type = SDL_MOUSEWHEEL;
  normal.wheel.y = 0;
  normal.wheel.preciseY = 0.25F;
  normal.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  scroll.setScrollOffset(100.0F);
  scroll.handleEvents(normal);
  require(std::abs(scroll.getScrollOffset() - 88.0F) < 0.001F,
          "scroll view uses a fractional normal wheel delta");

  SDL_Event natural = normal;
  natural.wheel.preciseY = -0.25F;
  natural.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
  scroll.setScrollOffset(100.0F);
  scroll.handleEvents(natural);
  require(std::abs(scroll.getScrollOffset() - 112.0F) < 0.001F,
          "scroll view preserves the iPad natural-scroll direction");
}

void testDropdownDefersOptionViewsUntilOpen() {
  DropdownView dropdown({}, nullptr);
  DropdownView::State state;
  state.options.reserve(261);
  for (int index = 0; index < 261; ++index) {
    state.options.push_back({.id = std::to_string(index),
                             .label = "Option " + std::to_string(index)});
  }

  dropdown.refresh(state);
  require(dropdown.optionButtons.empty(),
          "closed dropdown does not create hidden option views");

  dropdown.setOpen(true);
  require(dropdown.optionButtons.size() == state.options.size(),
          "opening dropdown creates its current option views");

  dropdown.setOpen(false);
  View::dispatchDeferredEventCallbacks();
  require(dropdown.optionButtons.empty(),
          "closing dropdown releases hidden option views after the event");
}

void testInputSelectorWidthsStayStableAcrossRefreshes() {
  for (const int width : {1200, 520}) {
    const auto layout = settings_scene::resolveInputSettingsLayout(width, width < 720);
    View row(0, 0, width, 300);
    row.setFlexDirection(layout.stackSelectors ? FlexDirection::Column : FlexDirection::Row);
    row.setGap(layout.selectorGap);
    std::vector<DropdownView *> selectors;
    for (int index = 0; index < 3; ++index) {
      auto *dropdown = new DropdownView({});
      dropdown->setTriggerWidth(layout.selectorWidth);
      dropdown->setFlexGrow(layout.stackSelectors ? 0.0F : 1.0F);
      row.addView(dropdown);
      selectors.push_back(dropdown);
    }
    for (int iteration = 0; iteration < 8; ++iteration) {
      for (auto *dropdown : selectors) {
        DropdownView::State state;
        state.label = i18n::message("settings.input.device.label");
        state.options = {{.id = "short", .label = "Keyboard"},
                         {.id = "long", .label = std::string(100 + iteration * 5, 'W')}};
        state.selectedId = iteration % 2 == 0 ? "short" : "long";
        state.open = iteration % 2 != 0;
        dropdown->refresh(state);
      }
      i18n::setLanguage(iteration % 2 == 0 ? i18n::Language::English : i18n::Language::Korean);
      row.propagateLanguageChange();
      row.applyYogaLayout();
      for (std::size_t index = 0; index < selectors.size(); ++index) {
        auto *dropdown = selectors[index];
        require(std::abs(dropdown->getWidth() - layout.selectorWidth) <= 1,
                "input selector width stays allocated across refresh, selection, and language changes");
        require(dropdown->getX() == (layout.stackSelectors ? 0 :
                    static_cast<int>(index) * (layout.selectorWidth + layout.selectorGap)),
                "input selector siblings do not shift as device labels change");
        if (dropdown->current.open) {
          require(dropdown->menuScroll->getWidth() >= dropdown->getWidth() &&
                      dropdown->menuScroll->getWidth() <= rendering::window_width,
                  "long device labels can use a wider bounded popup without widening the trigger");
        }
      }
    }
  }
  i18n::setLanguage(i18n::Language::English);
}

void testDropdownSelectionDefersTeardownUntilItsCallbackReturns() {
  DropdownView *dropdownRef = nullptr;
  DropdownView dropdown(
      {.onOptionSelected = [&](const std::string &selectedId) {
        DropdownView::State refreshed;
        refreshed.selectedId = selectedId;
        refreshed.options = {{.id = "first", .label = "First"},
                             {.id = "second", .label = "Second"}};
        dropdownRef->refresh(refreshed);
      }},
      nullptr);
  dropdownRef = &dropdown;

  DropdownView::State state;
  state.options = {{.id = "first", .label = "First"},
                   {.id = "second", .label = "Second"}};
  dropdown.refresh(state);
  dropdown.setOpen(true);

  click(*dropdown.optionButtons.front().button);
  require(!dropdown.current.open && !dropdown.optionButtons.empty(),
          "option selection keeps its view alive until the click callback ends");

  View::dispatchDeferredEventCallbacks();
  require(dropdown.optionButtons.empty(),
          "option selection releases hidden views after its callback returns");
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for panel resources");

  testSliderReleaseConsumedByDisabledSiblingEndsOnlyItsGesture();
  testLanguageRefreshPreservesPlayOptionsEditingAndScroll();
  testLaneOrderDraftTracksAuthoritativeSelectionAndProfile();
  testScrollViewUsesPreciseWheelDeltaAndNaturalDirection();
  testDropdownDefersOptionViewsUntilOpen();
  testInputSelectorWidthsStayStableAcrossRefreshes();
  testDropdownSelectionDefersTeardownUntilItsCallbackReturns();

  {
    GameplayRuleset selected = GameplayRuleset::LR2;
    ScrollView scroll(0, 0, 360, 400);
    scroll.setWidth(360)->setHeight(400);
    auto *content = new View();
    content->setWidth(340)
        ->setFlexDirection(FlexDirection::Column)
        ->setAlignItems(YGAlignStretch);
    auto *panel = new PlayOptionsPanelView(
        {.onRulesetSelected =
             [&](GameplayRuleset ruleset) { selected = ruleset; }},
        {.width = 340.0f,
         .playOptionColumns = 2,
         .showGauge = true,
         .showLaneOrder = false,
         .showPacemaker = true},
        nullptr);
    content->addView(panel);
    scroll.setContentView(content);
    scroll.applyYogaLayout();

    panel->refresh({.ruleset = GameplayRuleset::LR2});
    auto *lr2 = dynamic_cast<Button *>(panel->findViewByName("ruleset-lr2"));
    auto *beatoraja =
        dynamic_cast<Button *>(panel->findViewByName("ruleset-beatoraja"));
    require(lr2 != nullptr && beatoraja != nullptr,
            "ruleset buttons have stable names");
    require(buttonText(*lr2) != nullptr && buttonText(*beatoraja) != nullptr &&
                buttonText(*lr2)->getText() == "LR2" &&
                buttonText(*beatoraja)->getText() == "Beatoraja",
            "ruleset buttons have the approved labels");
    require(lr2->isSelected() && !beatoraja->isSelected(),
            "LR2 selected styling follows panel state");

    auto *clubMode =
        dynamic_cast<Button *>(panel->findViewByName("club-mode"));
    auto *clubContent = dynamic_cast<CheckboxButtonContent *>(
        clubMode == nullptr ? nullptr : clubMode->getContentView());
    require(clubContent != nullptr && !clubContent->checked(),
            "Club Beat starts with the FontAwesome unchecked icon");
    panel->refresh({.ruleset = GameplayRuleset::LR2, .clubMode = true});
    require(clubContent->checked(),
            "Club Beat refresh switches to the FontAwesome checked icon");

    click(*beatoraja);
    require(selected == GameplayRuleset::Beatoraja,
            "Beatoraja button invokes the enum callback");
    panel->refresh({.ruleset = GameplayRuleset::Beatoraja});
    require(!lr2->isSelected() && beatoraja->isSelected(),
            "Beatoraja selected styling follows panel state");

    auto *rulesetLabel = panel->findViewByName("ruleset-section-label");
    auto *gaugeLabel = panel->findViewByName("gauge-section-label");
    require(rulesetLabel != nullptr && gaugeLabel != nullptr &&
                rulesetLabel->getY() < gaugeLabel->getY(),
            "Ruleset appears before Gauge in scroll content");
    require(lr2->getWidth() == beatoraja->getWidth() && lr2->getWidth() > 0,
            "compact layout keeps equal-width ruleset buttons");
    require(scroll.getHeight() == 400,
            "adding the ruleset row does not grow the modal viewport");
    scroll.scrollToBottom();
    require(scroll.getScrollOffset() > 0.0f,
            "the compact modal content remains scrollable");
  }
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
