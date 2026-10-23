#include "view/Button.h"
#include "view/ToggleButtonContent.h"
#include "view/ClearLampColors.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "view/DropdownView.h"
#include "view/OverlayPortal.h"
#undef private
#include "view/ChoiceControlView.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "view/PlayOptionsPanelView.h"
#include "view/ScrollView.h"
#include "view/StoragePolicyView.h"
#include "view/OverlayPortal.h"
#include "view/SnappedSlider.h"
#include "view/ColorPickerView.h"
#include "view/ColorPickerPopup.h"
#include "view/TextView.h"
#include "view/TextInputBox.h"
#include "i18n/Localization.h"
#include "rendering/UniformCache.h"
#include "rendering/ResponsiveUi.h"
#include "scene/SettingsSceneInputLayout.h"
#include "scene/SettingsSceneShared.h"
#include "scene/ChartFilterSortPanelView.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>

namespace storage_management {
std::promise<StorageVideoCapabilities> storageCapabilityPromise;
const auto storageCapabilityFuture = storageCapabilityPromise.get_future().share();
std::shared_future<StorageVideoCapabilities> RequestStorageVideoCapabilities(std::function<void()>) {
  return storageCapabilityFuture;
}
} // namespace storage_management

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
  down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  down.button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.x = button.getX() + button.getWidth() / 2;
  down.button.y = button.getY() + button.getHeight() / 2;
  SDL_Event up = down;
  up.type = SDL_EVENT_MOUSE_BUTTON_UP;
  up.button.type = SDL_EVENT_MOUSE_BUTTON_UP;
  button.handleEvents(down);
  button.handleEvents(up);
}

void testStoragePolicyDirectSelection() {
  using namespace storage_management;
  StoragePolicy selected;
  int changes = 0;
  OverlayPortal portal;
  StoragePolicyView editor(selected, [&](StoragePolicy policy) {
    selected = policy;
    ++changes;
  }, &portal);
  editor.setWidth(440);
  editor.applyYogaLayout();
  TextView *codecStatus = nullptr;
  Button *codecButton = nullptr;
  Button *efficientButton = nullptr;
  ToggleButtonContent *efficientContent = nullptr;
  std::vector<DropdownView *> fields;
  for (auto *row : editor.getChildren()) {
    if (auto *text = dynamic_cast<TextView *>(row)) codecStatus = text;
    if (auto *button = dynamic_cast<Button *>(row)) {
      efficientButton = button;
      efficientContent = dynamic_cast<ToggleButtonContent *>(button->getContentView());
    }
    for (auto *child : row->getChildren()) {
      if (auto *dropdown = dynamic_cast<DropdownView *>(child)) fields.push_back(dropdown);
      if (auto *button = dynamic_cast<Button *>(child)) codecButton = button;
    }
  }
  require(codecButton && efficientButton && efficientContent && !efficientButton->isEnabled() &&
              !efficientContent->checked(), "efficient-video opt-in defaults off and requires compression");
  require(fields.size() == 5, "storage policy values offer direct dropdown selection");
  require(codecStatus && !codecStatus->getVisible(),
          "inactive codec status must not render at the origin after layout hides it");
  auto choose = [&](int field, const std::string &id) {
    click(*fields[field]->triggerButton);
    for (auto &option : fields[field]->optionButtons) {
      if (option.id == id) { click(*option.button); return; }
    }
    require(false, "storage dropdown exposes the requested choice");
  };
  require(!fields[2]->current.enabled && !fields[3]->current.enabled &&
              !fields[4]->current.enabled,
          "video details stay disabled until compression is selected");
  choose(0, "2");
  require(selected.audio == AudioAction::LossyVorbis && changes == 1,
          "choosing Vorbis skips intermediate policies and saves once");
  choose(0, "2");
  require(changes == 1, "reselecting the current policy does not save or invalidate review");
  choose(1, "1");
  require(codecStatus->getVisible(), "compression reveals the codec status");
  click(*efficientButton);
  require(selected.reencodeEfficientVideo && efficientContent->checked(),
          "efficient-video cycle saves the explicit opt-in");
  click(*codecButton);
  require(portal.presented.size() == 1, "codec selection opens a root-level modal");
  auto *modal = portal.presented.front();
  auto *panel = modal->getChildren().front();
  ScrollView *codecScroll = nullptr;
  for (auto *child : panel->getChildren())
    if (auto *scroll = dynamic_cast<ScrollView *>(child)) codecScroll = scroll;
  require(codecScroll, "codec explanations remain scrollable on short displays");
  const auto &cards = codecScroll->getContentView()->getChildren();
  require(cards.size() == 4 && YGNodeStyleGetDisplay(cards[1]->getNode()) == YGDisplayNone && YGNodeStyleGetDisplay(cards[2]->getNode()) == YGDisplayNone &&
              YGNodeStyleGetDisplay(cards[3]->getNode()) == YGDisplayNone, "pending capability checks hide codec options");
  require(cards[0]->getVisible() && !cards[1]->getVisible() &&
              !cards[2]->getVisible() && !cards[3]->getVisible(),
          "pending codec cards and their text are excluded from rendering and input");
  StorageVideoCapabilities capabilities{};
  capabilities[0] = {.codec = VideoCodec::H264, .available = true, .hardware = false,
      .playbackAvailable = true};
  capabilities[1] = {.codec = VideoCodec::Hevc, .available = false, .hardware = true,
      .playbackAvailable = true};
  capabilities[2] = {.codec = VideoCodec::Av1, .available = true, .hardware = true,
      .playbackAvailable = true};
  storageCapabilityPromise.set_value(capabilities);
  SDL_Event wake{}; wake.type = SDL_EVENT_USER;
  modal->handleEvents(wake);
  require(YGNodeStyleGetDisplay(cards[1]->getNode()) == YGDisplayFlex && YGNodeStyleGetDisplay(cards[2]->getNode()) == YGDisplayNone && YGNodeStyleGetDisplay(cards[3]->getNode()) == YGDisplayFlex,
          "codec modal only offers verified encoding and playback options");
  require(!cards[0]->getVisible() && cards[1]->getVisible() &&
              !cards[2]->getVisible() && cards[3]->getVisible(),
          "ready modal hides the status and unavailable card from rendering and input");
  auto *h264Encoding = dynamic_cast<TextView *>(cards[1]->getChildren()[2]);
  auto *av1Encoding = dynamic_cast<TextView *>(cards[3]->getChildren()[2]);
  auto *av1Playback = dynamic_cast<TextView *>(cards[3]->getChildren()[3]);
  require(h264Encoding->getText() != av1Encoding->getText() &&
              av1Playback->getText() == i18n::tr("storage.ui.codec_playback_software_help"),
          "modal explains actual hardware encoding and software playback independently");
  auto saved = selected;
  saved.videoCodec = VideoCodec::Hevc;
  const int changesBeforeRefresh = changes;
  editor.SetPolicy(saved);
  require(portal.presented.empty() && changes == changesBeforeRefresh &&
              dynamic_cast<TextView *>(codecButton->getContentView())->getText().find("HEVC") != std::string::npos,
          "unsupported saved codec remains visible without silently saving a replacement");
  click(*codecButton);
  click(*dynamic_cast<Button *>(cards[3]->getChildren().back()));
  require(selected.videoCodec == VideoCodec::Av1 && changes == changesBeforeRefresh + 1 &&
              portal.presented.empty(), "choosing a supported codec saves once and dismisses the modal");
  choose(3, "4");
  choose(4, "24");
  require(selected.bga == BgaAction::CompressH264 &&
              selected.resolution == ResolutionCap::Low && selected.maxFps == 24,
          "resolution and FPS apply the selected values directly");
  click(*fields[2]->triggerButton);
  click(*fields[4]->triggerButton);
  require(!fields[2]->current.open && fields[4]->current.open &&
              !portal.isPresented(fields[2]->menuScroll) && portal.isPresented(fields[4]->menuScroll),
          "only the active storage menu appears in the unclipped overlay portal");
  editor.SetEnabled(false);
  require(!fields[4]->current.open && !fields[0]->current.enabled &&
              !portal.isPresented(fields[4]->menuScroll),
          "starting a storage task closes menus and disables editing");
  editor.SetEnabled(true);
  choose(1, "2");
  require(selected.bga == BgaAction::Delete && selected.maxFps == 24 &&
              !fields[4]->current.enabled && !efficientButton->isEnabled() &&
              efficientContent->checked(),
          "deletion disables video details while retaining their saved values");
  require(!codecStatus->getVisible(), "leaving compression hides the codec status again");
  const int previousChanges = changes;
  editor.SetPolicy({});
  require(changes == previousChanges && fields[0]->current.selectedId == "0",
          "external policy refresh updates selection without saving again");
}

void testColorPickerColorSpace() {
  for (const auto rgb : {0x000000U, 0xFFFFFFU, 0x808080U, 0xFF0000U, 0x00FF00U,
                         0x0000FFU, 0x3399CCU, 0x12ABEFU, 0xFEDCBAU})
    require(color_picker::toRgb(color_picker::fromRgb(rgb)) == rgb,
            "picker RGB/HSV conversion retains preset and custom colors");
  auto gray = color_picker::fromRgb(0x808080, {.hue = 0.66F, .saturation = 0.8F, .value = 1});
  require(gray.hue == 0.66F && gray.saturation == 0,
          "gray retains the user's hue for the next saturation adjustment");
  const auto black = color_picker::fromRgb(0, {.hue = 0.66F, .saturation = 0.8F, .value = 1});
  require(black.hue == 0.66F && black.saturation == 0.8F && black.value == 0,
          "black retains hue and saturation for the next brightness adjustment");
  require(color_picker::toRgb({1, 1, 1}) == 0xFF0000,
          "both ends of the hue strip meet at red");
}

void testColorPickerDragAndRelease() {
  for (bool touch : {false, true}) {
    int changes = 0;
    int commits = 0;
    ColorPickerView picker({0, 1, 1}, [&](auto, bool finished) {
      if (finished) ++commits;
      else ++changes;
    });
    picker.setSize(220, 240);
    const auto pointer = [&](Uint32 type, int x, int y, SDL_FingerID id = 7) {
      SDL_Event event{};
      event.type = type;
      if (touch) {
        event.tfinger.touchID = 1;
        event.tfinger.fingerID = id;
        event.tfinger.x = float(x) / rendering::window_width;
        event.tfinger.y = float(y) / rendering::window_height;
      } else if (type == SDL_EVENT_MOUSE_MOTION) {
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
    auto down = pointer(touch ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_MOUSE_BUTTON_DOWN, 110, 80);
    require(!picker.handleEvents(down) && changes == 1 && commits == 0,
            "picker previews the initial mouse or touch press without committing");
    auto move = pointer(touch ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_MOUSE_MOTION, 400, 0);
    require(!picker.handleEvents(move) && color_picker::toRgb(picker.value()) == 0xFF0000,
            "dragging outside the square clamps to a saturated bright color");
    auto unrelated = pointer(touch ? SDL_EVENT_FINGER_UP : SDL_EVENT_MOUSE_BUTTON_UP, 110, 80, 8);
    if (!touch) unrelated.button.button = SDL_BUTTON_RIGHT;
    picker.notifyPointerEventConsumed(unrelated);
    require(commits == 0, "unrelated releases cannot end the picker gesture");
    auto up = pointer(touch ? SDL_EVENT_FINGER_UP : SDL_EVENT_MOUSE_BUTTON_UP, 110, 80);
    picker.notifyPointerEventConsumed(up);
    require(commits == 1 && color_picker::toRgb(picker.value()) == 0xFF0000,
            "a release consumed by another view commits the last previewed color");
    require(picker.handleEvents(move) && commits == 1,
            "a consumed release cannot leave the picker dragging");
    down = pointer(touch ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_MOUSE_BUTTON_DOWN, 110, 215);
    require(!picker.handleEvents(down) && std::abs(picker.value().hue - 0.5F) < 0.001F,
            "the hue strip selects cyan halfway across");
    up = pointer(touch ? SDL_EVENT_FINGER_UP : SDL_EVENT_MOUSE_BUTTON_UP, 110, 215);
    require(!picker.handleEvents(up) && commits == 2,
            "ordinary pointer release commits exactly once");
    require(!picker.handleEvents(down), "a new picker gesture starts after release");
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    picker.handleEvents(lost);
    require(commits == 3 && picker.handleEvents(move),
            "focus loss commits the last previewed value and clears dragging");
  }
}

void testColorPickerPopup() {
  for (const auto [width, height] : {std::pair{1280, 720}, {390, 844}, {640, 360}}) {
    std::vector<ColorPickerPopup::Sample> samples;
    for (int i = 0; i < 48; ++i) samples.push_back({.label = std::to_string(i + 1)});
    int previewChanges = 0;
    std::uint32_t previewColor = 0xFF0000;
    ColorPickerPopup popup({0, 1, 1}, samples, [&](std::uint32_t rgb) {
      ++previewChanges;
      previewColor = rgb;
    });
    popup.fitToViewport(width, height, 12, 20, 12, 16);
    ColorPickerView *picker = nullptr;
    std::vector<Button *> buttons;
    const auto inspect = [&](auto &&self, View *view) -> void {
      require(dynamic_cast<ScrollView *>(view) == nullptr, "popup has no competing scroll gestures");
      require(view->getX() >= 0 && view->getY() >= 0 &&
              view->getX() + view->getWidth() <= width &&
              view->getY() + view->getHeight() <= height,
              "popup controls and all 48 samples fit portrait and landscape viewports");
      if (auto *p = dynamic_cast<ColorPickerView *>(view)) picker = p;
      if (auto *b = dynamic_cast<Button *>(view)) buttons.push_back(b);
      for (auto *child : view->getChildren()) self(self, child);
    };
    inspect(inspect, &popup);
    require(picker && buttons.size() == 2 && picker->getHeight() >= 136,
            "popup provides a usable picker and two actions");
    auto *panel = popup.getChildren().front();
    for (int i = 0; i < 48; ++i) {
      auto *sample = panel->getChildren()[i + 1];
      require(sample->getY() + sample->getHeight() <= picker->getY(),
              "every note preview is above the picker");
      auto *note = sample->getChildren()[1];
      require(note->getWidth() > 0 && note->getHeight() > 0,
              "all 48 note previews retain visible dimensions on short screens");
    }
    for (auto *button : buttons)
      require(button->getY() >= picker->getY() + picker->getHeight(),
              "confirmation actions remain below the picker");
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    require(!popup.handleEvents(wheel), "modal consumes scrolling over the background");
    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.x = picker->getX() + picker->getWidth() / 2;
    down.button.y = picker->getY() + 15;
    popup.handleEvents(down);
    require(previewChanges > 0 && previewColor == color_picker::toRgb(picker->value()) &&
                previewColor != 0xFF0000 && !popup.result(),
            "picker forwards live draft colors before release or confirmation");
    SDL_Event up = down;
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    popup.handleEvents(up);
    require(!popup.result() && color_picker::toRgb(picker->value()) != 0xFF0000,
            "dragging and releasing edits only the draft until confirmed");
    click(*buttons.back());
    require(popup.result() && popup.result()->confirmed &&
                color_picker::toRgb(popup.result()->color) == color_picker::toRgb(picker->value()),
            "Confirm returns precisely the previewed draft");
  }
  for (const bool escape : {false, true}) {
    ColorPickerPopup popup({0.5F, 1, 1}, {{.label = "S", .style = ColorPickerPopup::SampleStyle::Scratch}});
    popup.fitToViewport(800, 600);
    if (escape) {
      SDL_Event event{};
      event.type = SDL_EVENT_KEY_DOWN;
      event.key.key = SDLK_ESCAPE;
      require(!popup.handleEvents(event), "Escape is consumed by the modal");
    } else {
      for (auto *child : popup.getChildren().front()->getChildren()) {
        if (auto *button = dynamic_cast<Button *>(child)) { click(*button); break; }
      }
    }
    require(popup.result() && !popup.result()->confirmed,
            "Cancel and Escape discard the draft without applying a color");
  }
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
        event.tfinger.touchID = 1;
        event.tfinger.fingerID = id;
        event.tfinger.x = static_cast<float>(x) / rendering::window_width;
        event.tfinger.y = static_cast<float>(y) / rendering::window_height;
      } else if (type == SDL_EVENT_MOUSE_MOTION) {
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
    auto down = pointer(touch ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_MOUSE_BUTTON_DOWN, 60, 25);
    require(!root.handleEvents(down), "slider begins the gesture");
    auto unrelated = pointer(touch ? SDL_EVENT_FINGER_UP : SDL_EVENT_MOUSE_BUTTON_UP, 220, 80, 8);
    if (!touch) unrelated.button.button = SDL_BUTTON_RIGHT;
    require(!root.handleEvents(unrelated), "disabled sibling consumes unrelated release");
    auto move = pointer(touch ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_MOUSE_MOTION, 120, 25);
    require(!root.handleEvents(move) && slider->value() == 50,
            "unrelated release preserves the active slider gesture");
    const int beforeRelease = changes;
    auto up = pointer(touch ? SDL_EVENT_FINGER_UP : SDL_EVENT_MOUSE_BUTTON_UP, 220, 80);
    require(!root.handleEvents(up), "disabled sibling consumes slider release");
    require(slider->value() == 50 && changes == beforeRelease,
            "covered release does not change the value or invoke its callback");
    move = pointer(touch ? SDL_EVENT_FINGER_MOTION : SDL_EVENT_MOUSE_MOTION, 220, 25);
    require(root.handleEvents(move) && slider->value() == 50 &&
                changes == beforeRelease,
            "released slider cannot keep changing on later pointer motion");
    down = pointer(touch ? SDL_EVENT_FINGER_DOWN : SDL_EVENT_MOUSE_BUTTON_DOWN, 220, 25, 9);
    require(!root.handleEvents(down), "slider accepts a fresh pointer after the covered release");
    up = pointer(touch ? SDL_EVENT_FINGER_UP : SDL_EVENT_MOUSE_BUTTON_UP, 220, 25, 9);
    root.handleEvents(up);
    require(slider->value() > 50, "fresh slider tap commits on release");
  }
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
  auto *clubContent = dynamic_cast<ToggleButtonContent *>(
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
          "cycle label translates without changing selection");
  i18n::setLanguage(i18n::Language::English);
}

void testPhoneOptionsReflowWithoutLosingSelection() {
  rendering::compactMenuCanvasActive = true;
  {
    GameplayRuleset selected = GameplayRuleset::LR2;
    ScrollView scroll(0, 0, 420, 240);
    auto *panel = new PlayOptionsPanelView(
        {.onRulesetSelected = [&](GameplayRuleset value) { selected = value; }},
        {.width = 420, .playOptionColumns = 2}, nullptr);
    scroll.setContentView(panel);
    panel->refresh({.ruleset = selected});
    for (const int width : {420, 700, 420}) {
      scroll.setWidth(width);
      panel->setWidth(width);
      scroll.refreshContentLayout();
      scroll.applyYogaLayout();
      auto *choice = panel->findViewByName("ruleset-choice");
      auto *button = dynamic_cast<Button *>(choice->findViewByName("choice:beatoraja"));
      require(button && button->getVisible() &&
                  button->getHeight() >= rendering::compactTouchTarget,
              "phone ruleset choices retain the minimum touch height after rotation");
      require(button->getX() >= panel->getX() &&
                  button->getX() + button->getWidth() <= panel->getX() + width,
              "phone option choices stay inside the resized panel");
    }
    auto *choice = panel->findViewByName("ruleset-choice");
    click(*static_cast<Button *>(choice->findViewByName("choice:beatoraja")));
    require(selected == GameplayRuleset::Beatoraja,
            "phone options remain interactive after rotating twice");
    scroll.scrollToBottom();
    require(scroll.getScrollOffset() > 0, "short phone option panes remain scrollable");
  }
  rendering::compactMenuCanvasActive = false;
}

void testLaneCoverControlsAreAvailableInPlayOptions() {
  lane_cover::State selected;
  int changes = 0;
  PlayOptionsPanelView panel(
      {.onLaneCoversChanged = [&](const lane_cover::State &state) {
        selected = state;
        ++changes;
      }}, {.width = 340.0F}, nullptr);
  panel.refresh({.laneCovers = {.laneCoverPercent = 20.1F}});
  panel.applyYogaLayout();
  for (const auto name : {"lane-cover-sudden", "lane-cover-hidden", "lane-cover-lift"}) {
    auto *button = dynamic_cast<Button *>(panel.findViewByName(name));
    require(button != nullptr && button->getHeight() == 48,
            "play options expose all cover toggles at usable heights");
    click(*button);
  }
  require(changes == 3 && selected.laneCoverEnabled && selected.hiddenEnabled &&
              selected.liftEnabled && selected.laneCoverPercent == 20.1F,
          "cover toggles are independent and preserve fine amounts");
  auto *slider = dynamic_cast<SnappedSlider *>(panel.findViewByName("lane-cover-hidden-amount"));
  require(slider != nullptr, "hidden cover exposes a fine amount slider");
  SDL_Event down{};
  down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.x = slider->getX() + slider->getWidth() / 2;
  down.button.y = slider->getY() + slider->getHeight() / 2;
  slider->handleEvents(down);
  SDL_Event up = down;
  up.type = SDL_EVENT_MOUSE_BUTTON_UP;
  slider->handleEvents(up);
  require(changes > 3 && std::abs(selected.hiddenRatio - 0.5F) < 0.02F &&
              selected.liftRatio == 0.1F && selected.laneCoverPercent == 20.1F,
          "dragging hidden amount updates only the selected cover");
  std::map<std::string, int> toggleWidths;
  for (const char *name : {"lane-cover-sudden", "lane-cover-hidden", "lane-cover-lift"})
    toggleWidths[name] = panel.findViewByName(name)->getWidth();
  for (const int width : {240, 340, 760}) {
    panel.setWidth(width);
    panel.applyYogaLayout();
    for (const char *name : {"lane-cover-sudden", "lane-cover-hidden", "lane-cover-lift"}) {
      auto *button = dynamic_cast<Button *>(panel.findViewByName(name));
      auto *content = dynamic_cast<ToggleButtonContent *>(button->getContentView());
      require(button->getWidth() == toggleWidths[name],
              "lane cover toggles keep a compact width as the panel grows or wraps");
      require(content && content->checked() && button->getX() >= panel.getX() &&
                  button->getX() + button->getWidth() <= panel.getX() + width,
              "cover cycles retain their states and fit narrow panels");
    }
    require(slider->getX() >= panel.getX() &&
                slider->getX() + slider->getWidth() <= panel.getX() + width,
            "cover amounts reflow inside narrow panels");
  }
}

ChoiceControlView &panelChoice(PlayOptionsPanelView &panel, const char *name) {
  auto *choice = dynamic_cast<ChoiceControlView *>(panel.findViewByName(name));
  require(choice != nullptr, "panel exposes the requested settings choice");
  return *choice;
}

DropdownView &choiceDropdown(ChoiceControlView &choice) {
  auto *dropdown = dynamic_cast<DropdownView *>(choice.findViewByName("choice-dropdown"));
  require(dropdown != nullptr, "settings choice has a dropdown presentation");
  return *dropdown;
}

void choosePanelOption(PlayOptionsPanelView &panel, const char *name, const std::string &id) {
  auto &choice = panelChoice(panel, name);
  if (choice.findViewByName("choice-buttons")->getVisible()) {
    auto *button = dynamic_cast<Button *>(choice.findViewByName("choice:" + id));
    require(button != nullptr, "visible choice row contains the requested option");
    click(*button);
  } else {
    auto &dropdown = choiceDropdown(choice);
    require(dropdown.getVisible(), "larger choice sets use a dropdown");
    click(*dropdown.triggerButton);
    if (!dropdown.current.enabled) {
      require(!dropdown.current.open, "locked dropdown cannot open");
      return;
    }
    bool found = false;
    for (const auto &option : dropdown.optionButtons) {
      if (option.id != id) continue;
      click(*option.button);
      found = true;
      break;
    }
    require(found, "dropdown contains the requested option");
  }
  View::dispatchDeferredEventCallbacks();
  panel.applyYogaLayout();
}

void testPlayOptionsChoiceCallbacksLocksAndPopupClosure() {
  OverlayPortal portal;
  PlayOptionsPanelState selected;
  int changes = 0;
  PlayOptionsPanelView *owner = nullptr;
  auto changed = [&] { ++changes; owner->refresh(selected); };
  PlayOptionsPanelView panel(
      {.onGaugeSelected = [&](GaugeType gauge, GaugeAutoShiftMode mode) {
         selected.gaugeType = gauge;
         selected.gaugeAutoShift = mode;
         changed();
       },
       .onGaugeLowerBoundSelected = [&](GaugeType gauge) {
         selected.gaugeAutoShiftLowerBound = gauge;
         changed();
       },
       .onPlayOptionSelected = [&](const std::string &id) { selected.playOption = id; changed(); },
       .isPlayOptionAllowed = [](const std::string &id) { return id != "RANDOM"; },
       .onLongNoteModeSelected = [&](const std::string &id) { selected.longNoteMode = id; changed(); },
       .onAssistOptionSelected = [&](const std::string &id) { selected.assistOption = id; changed(); },
       .onPlaybackModeSelected = [&](const std::string &) { changed(); },
       .onClubModeToggled = [&] { selected.clubMode = !selected.clubMode; changed(); },
       .onPacemakerSelected = [&](const std::string &id) { selected.pacemakerTarget = id; changed(); }},
      {.width = 620}, &portal);
  owner = &panel;
  selected.gaugeType = GaugeType::Grade;
  panel.refresh(selected);
  panel.applyYogaLayout();
  auto &savedGauge = choiceDropdown(panelChoice(panel, "gauge-choice"));
  require(savedGauge.current.selectedId == "GRADE" && !savedGauge.current.options.back().available && changes == 0,
          "an unsupported saved gauge remains visible without substituting Normal");
  choosePanelOption(panel, "gauge-choice", "HARD");
  require(selected.gaugeType == GaugeType::Hard && selected.gaugeAutoShift == GaugeAutoShiftMode::None,
          "gauge selection preserves the auto-shift mode");
  panel.refresh(selected);
  choosePanelOption(panel, "gauge-auto-shift-choice", "Best Clear");
  require(selected.gaugeType == GaugeType::Hard && selected.gaugeAutoShift == GaugeAutoShiftMode::BestClear,
          "auto-shift selection preserves the starting gauge");
  panel.refresh(selected);
  panel.applyYogaLayout();
  choosePanelOption(panel, "gauge-lower-bound-choice", "EASY");
  require(selected.gaugeAutoShiftLowerBound == GaugeType::Easy,
          "lower-bound selection invokes its dedicated callback");
  choosePanelOption(panel, "play-option-choice", "RANDOM");
  require(selected.playOption == "NORMAL", "disallowed play shuffle remains unselectable");
  panel.closeDropdowns();
  choosePanelOption(panel, "play-option-choice", "MIRROR");
  require(selected.playOption == "MIRROR", "play shuffle applies directly through its dropdown");
  selected.playOption = "RANDOM";
  panel.refresh(selected);
  auto &shuffle = choiceDropdown(panelChoice(panel, "play-option-choice"));
  const auto random = std::ranges::find_if(shuffle.current.options,
      [](const auto &option) { return option.id == "RANDOM"; });
  require(shuffle.current.selectedId == "RANDOM" && random != shuffle.current.options.end() &&
              !random->available,
          "an unavailable saved shuffle stays selected and visible");
  choosePanelOption(panel, "play-option-choice", "MIRROR");
  choosePanelOption(panel, "long-note-mode-choice", "HCN");
  choosePanelOption(panel, "assist-option-choice", "BPM-GUIDE");
  choosePanelOption(panel, "pacemaker-choice", "AAA");
  require(selected.longNoteMode == "HCN" && selected.assistOption == "BPM-GUIDE" &&
              selected.pacemakerTarget == "AAA", "remaining groups preserve their option callbacks");
  const int beforeReselect = changes;
  choosePanelOption(panel, "pacemaker-choice", "AAA");
  require(changes == beforeReselect, "reselecting the current panel option avoids another save");
  auto *club = dynamic_cast<Button *>(panel.findViewByName("club-mode"));
  auto *clubContent = dynamic_cast<ToggleButtonContent *>(club->getContentView());
  click(*club);
  require(selected.clubMode && clubContent->checked(), "Club Beat cycle saves and displays on");
  click(*club);
  require(!selected.clubMode && !clubContent->checked(), "Club Beat cycles back off on its next activation");
  selected.longNoteModeLocked = true;
  selected.assistOptionLocked = true;
  selected.playbackLocked = true;
  panel.refresh(selected);
  panel.applyYogaLayout();
  const int before = changes;
  choosePanelOption(panel, "long-note-mode-choice", "LN");
  choosePanelOption(panel, "assist-option-choice", "OFF");
  require(changes == before && panelChoice(panel, "long-note-mode-choice").selectedId() == "HCN" &&
              panelChoice(panel, "assist-option-choice").selectedId() == "BPM-GUIDE",
          "locked choices retain their saved selections without callbacks");
  auto *playback = dynamic_cast<DropdownView *>(panel.findViewByName("playback-mode-dropdown"));
  require(playback && !playback->current.enabled && playback->current.selectedId == "pitch-shift" &&
              playback->current.options.size() == 2 && !playback->current.options[1].available,
          "playback lock preserves the visible unsupported time-stretch choice");
  selected.playbackLocked = false;
  panel.refresh(selected);
  panel.applyYogaLayout();
  click(*playback->triggerButton);
  const int beforeUnsupported = changes;
  click(*playback->optionButtons[1].button);
  require(playback->current.selectedId == "pitch-shift" && changes == beforeUnsupported, "unsupported playback mode cannot replace pitch shift");
  panel.closeDropdowns();
  for (const char *name : {"gauge-choice", "gauge-auto-shift-choice", "gauge-lower-bound-choice",
                           "play-option-choice", "pacemaker-choice"}) {
    auto &dropdown = choiceDropdown(panelChoice(panel, name));
    click(*dropdown.triggerButton);
    require(dropdown.current.open && portal.isPresented(dropdown.menuScroll),
            "panel dropdown uses the supplied overlay portal");
    panel.closeDropdowns();
    require(!dropdown.current.open && !portal.isPresented(dropdown.menuScroll),
            "closing the panel dismisses each settings menu");
  }
  selected.gaugeAutoShift = GaugeAutoShiftMode::None;
  panel.refresh(selected);
  panel.applyYogaLayout();
  require(!panel.findViewByName("gauge-auto-shift-bounds")->getVisible(),
          "leaving bounded auto-shift hides lower-bound controls");
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
  normal.type = SDL_EVENT_MOUSE_WHEEL;
  normal.wheel.type = SDL_EVENT_MOUSE_WHEEL;
  normal.wheel.y = 0;
  normal.wheel.y = 0.25F;
  normal.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  scroll.setScrollOffset(100.0F);
  scroll.handleEvents(normal);
  require(std::abs(scroll.getScrollOffset() - 88.0F) < 0.001F,
          "scroll view uses a fractional normal wheel delta");

  SDL_Event natural = normal;
  natural.wheel.y = -0.25F;
  natural.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
  scroll.setScrollOffset(100.0F);
  scroll.handleEvents(natural);
  require(std::abs(scroll.getScrollOffset() - 112.0F) < 0.001F,
          "scroll view preserves the iPad natural-scroll direction");
}

void testScrollViewTargetsWheelEventPosition() {
  ScrollView scroll(100, 100, 360, 200);
  auto *content = new View();
  content->setWidth(360)->setHeight(800);
  scroll.setContentView(content);
  scroll.applyYogaLayout();
  scroll.setScrollOffset(100.0F);

  SDL_Event wheel{};
  wheel.type = SDL_EVENT_MOUSE_WHEEL;
  wheel.wheel.y = -0.5F;
  wheel.wheel.mouse_x = scroll.getContentX() + 10.5F;
  wheel.wheel.mouse_y = scroll.getContentY() + 10.5F;
  scroll.handleEvents(wheel);
  require(std::abs(scroll.getScrollOffset() - 124.0F) < 0.001F,
          "wheel targets its event position, not the later polled pointer");
  wheel.wheel.mouse_x = 0;
  wheel.wheel.mouse_y = 0;
  scroll.handleEvents(wheel);
  require(std::abs(scroll.getScrollOffset() - 124.0F) < 0.001F,
          "a wheel event outside the view must not scroll it");
}

void testDropdownPreservesAllocatedWidthAcrossRefreshes() {
  for (int sizing : {0, 1, 2}) {
    View root(0, 0, 440, 700);
    root.setFlexDirection(FlexDirection::Row);
    auto *dropdown = new DropdownView({});
    if (sizing == 0) dropdown->setWidth(220);
    else if (sizing == 1) dropdown->setWidthPercent(100);
    else {
      auto *sibling = new View();
      sibling->setWidth(220)->setFlexShrink(0);
      root.addView(sibling);
      dropdown->setFlex(1);
    }
    root.addView(dropdown);
    for (int length : {5, 100, 5}) {
      dropdown->refresh({.options = {{.id = "one", .label = std::string(length, 'W')}},
                          .open = true});
      root.applyYogaLayout();
      require(dropdown->getWidth() == (sizing == 1 ? 440 : 220),
              "dropdown refresh must preserve its assigned row width");
      require(dropdown->triggerButton->getWidth() == dropdown->getWidth(),
              "dropdown trigger fills its allocated width");
      require(dropdown->menuScroll->getWidth() >= dropdown->getWidth(),
              "dropdown popup is at least as wide as its trigger when space allows");
    }
  }
}

void testNaturalDropdownsRemainVisibleAcrossParentLayouts() {
  bool allVisible = true;
  for (const bool compact : {false, true}) {
    rendering::compactMenuCanvasActive = compact;
    for (int layout = 0; layout < 4; ++layout) {
      // General settings: start-aligned column. Skin choices/safety: wrapping
      // label rows. Also cover stretched columns and non-wrapping filter rows.
      OverlayPortal portal;
      View root(0, 0, layout == 3 ? 240 : 440, 700);
      root.setFlexDirection(layout < 2 ? FlexDirection::Column : FlexDirection::Row);
      root.setAlignItems(layout == 0 ? YGAlignFlexStart : layout == 1 ? YGAlignStretch : YGAlignCenter);
      if (layout >= 2) {
        root.setFlexWrap(layout == 3 ? YGWrapWrap : YGWrapNoWrap)->setGap(10);
        auto *label = new View(0, 0, 120, 30);
        label->setFlexShrink(0);
        root.addView(label);
      }
      auto *dropdown = new DropdownView({}, layout == 2 ? nullptr : &portal);
      dropdown->refresh({.selectedId = "system",
                         .options = {{.id = "system", .label = "System default"},
                                     {.id = "en", .label = "English"}}});
      root.addView(dropdown);
      root.applyYogaLayout();
      const int height = compact ? rendering::compactTouchTarget : 42;
      const bool visible = dropdown->getWidth() >= 160 &&
          dropdown->triggerButton->getWidth() == dropdown->getWidth() &&
          dropdown->triggerButton->getHeight() == height;
      if (!visible) {
        std::cerr << "collapsed dropdown: layout=" << layout << " compact=" << compact
                  << " outer=" << dropdown->getWidth()
                  << " trigger=" << dropdown->triggerButton->getWidth() << '\n';
        allVisible = false;
        continue;
      }
      click(*dropdown->triggerButton);
      require(dropdown->current.open && dropdown->menuScroll->getVisible(),
              "natural dropdowns open from their visible trigger");
      click(*dropdown->optionButtons[1].button);
      require(dropdown->current.selectedId == "en" && !dropdown->current.open,
              "natural dropdown choices remain selectable");
      View::dispatchDeferredEventCallbacks();
    }
  }
  rendering::compactMenuCanvasActive = false;
  require(allVisible, "natural dropdown triggers fill their controls in every parent layout");
}

void testSettingsChoicePresentationAndSelection() {
  View root(0, 0, 640, 500);
  OverlayPortal portal;
  int changes = 0;
  bool accept = true;
  auto *choice = new ChoiceControlView([&](const std::string &) {
    ++changes;
    return accept;
  }, &portal, 18, 0, true);
  root.addView(choice);
  DropdownView::State state{.selectedId = "a",
      .options = {{.id = "a", .label = "Off"}, {.id = "b", .label = "On"}}};
  choice->refresh(state);
  root.applyYogaLayout();
  auto *cycle = dynamic_cast<Button *>(choice->findViewByName("choice-cycle"));
  require(cycle && cycle->getVisible(), "opposing states use a cycling button");
  click(*cycle);
  require(choice->selectedId() == "b" && changes == 1, "toggle applies the other state once");
  accept = false;
  click(*cycle);
  require(choice->selectedId() == "b", "failed toggle retains the previous state");
  accept = true;
  state.options.push_back({.id = "c", .label = "Auto"});
  choice->refresh(state);
  root.applyYogaLayout();
  auto *first = dynamic_cast<Button *>(choice->findViewByName("choice:a"));
  auto *third = dynamic_cast<Button *>(choice->findViewByName("choice:c"));
  require(choice->findViewByName("choice-buttons")->getVisible(),
          "three short choices use a single button row when it fits");
  const int before = changes;
  click(*first);
  require(changes == before, "reselecting the current choice does not write settings");
  click(*third);
  require(choice->selectedId() == "c" && changes == before + 1,
          "choice buttons select directly without applying intermediate states");
  root.setWidth(180);
  root.applyYogaLayout();
  auto *dropdown = dynamic_cast<DropdownView *>(choice->findViewByName("choice-dropdown"));
  require(dropdown && dropdown->getVisible() && choice->selectedId() == "c",
          "narrow choice rows become dropdowns without changing selection");
  click(*dropdown->triggerButton);
  require(dropdown->current.open, "adaptive dropdown opens its menu");
  root.setWidth(640);
  root.applyYogaLayout();
  require(!dropdown->current.open && !dropdown->menuScroll->getVisible(),
          "changing presentation dismisses the old popup");
  state.options[0].label = std::string(100, 'W');
  choice->refresh(state);
  require(dropdown->getVisible(), "long labels use a dropdown instead of shrinking buttons");
  state.selectedId = "saved-unknown";
  state.options[0].label = "Off";
  choice->refresh(state);
  require(dropdown->getVisible(), "unknown saved choices retain a visible dropdown value");
  state.selectedId = "a";
  state.options.push_back({.id = "d", .label = "Other"});
  state.options.push_back({.id = "e", .label = "Last"});
  state.enabled = false;
  choice->refresh(state);
  require(dropdown->getVisible() && !dropdown->current.enabled,
          "larger or disabled sets preserve their dropdown state");
  View::dispatchDeferredEventCallbacks();
}

void testLocalizedFlickTapModesUseDirectSelection() {
  for (const auto language : {i18n::Language::English, i18n::Language::Japanese, i18n::Language::Korean}) {
    i18n::setLanguage(language);
    View root(0, 0, 640, 500);
    std::string saved = "off";
    int changes = 0;
    auto *choice = new ChoiceControlView([&](const std::string &id) { saved = id; ++changes; return true; });
    root.addView(choice);
    choice->refresh({.selectedId = saved,
        .options = {{.id = "off", .label = i18n::message("settings.input.tap_scratch_off.label")},
                    {.id = "on", .label = i18n::message("settings.input.tap_scratch_on.label")}}});
    root.applyYogaLayout();
    require(choice->findViewByName("choice-buttons")->getVisible() &&
                !choice->findViewByName("choice-cycle")->getVisible(),
            "localized Flick/Tap modes use separate buttons when they fit");
    auto *flick = dynamic_cast<Button *>(choice->findViewByName("choice:off"));
    auto *tap = dynamic_cast<Button *>(choice->findViewByName("choice:on"));
    require(static_cast<TextView *>(flick->getContentView())->getText() == i18n::tr("settings.input.tap_scratch_off.label") &&
                static_cast<TextView *>(tap->getContentView())->getText() == i18n::tr("settings.input.tap_scratch_on.label"),
            "touch mode buttons retain the actual localized Flick/Tap captions");
    click(*tap);
    require(saved == "on" && changes == 1, "Tap directly selects the enabled tap-to-scratch setting");
    root.setWidth(180);
    root.applyYogaLayout();
    auto &dropdown = choiceDropdown(*choice);
    require(dropdown.getVisible() && !choice->findViewByName("choice-cycle")->getVisible(),
            "localized Flick/Tap modes use a dropdown when their buttons do not fit");
    click(*dropdown.triggerButton);
    click(*dropdown.optionButtons.front().button);
    require(saved == "off" && changes == 2, "narrow Flick selection directly disables tap-to-scratch");
    View::dispatchDeferredEventCallbacks();
  }
  i18n::setLanguage(i18n::Language::English);
}

std::vector<uint32_t> renderedChoiceColors(Button &button) {
  struct Backend final : rendering::UiBatchBackend {
    std::vector<uint32_t> colors;
    bool submit(const rendering::UiBatchSubmission &submission) noexcept override {
      for (const auto &vertex : submission.colorVertices) colors.push_back(vertex.abgr);
      return true;
    }
  } backend;
  rendering::UiBatchRenderer renderer(backend);
  RenderContext context(renderer);
  const auto originalDirectory = std::filesystem::current_path();
  std::filesystem::current_path(std::filesystem::path(__FILE__).parent_path().parent_path());
  renderer.begin();
  button.render(context);
  renderer.end();
  std::filesystem::current_path(originalDirectory);
  require(!backend.colors.empty(), "choice button renders its surface and border");
  return backend.colors;
}

void requireToggleForeground(View *view, const Color &expected) {
  if (auto *text = dynamic_cast<TextView *>(view)) {
    const auto actual = text->currentColor();
    require(actual.r == expected.r && actual.g == expected.g && actual.b == expected.b,
            "toggle caption, state, and icon share a contrasting foreground");
  }
  for (auto *child : view->getChildren()) requireToggleForeground(child, expected);
}

void testToggleWidthsFollowContentAcrossLayouts() {
  const bool originalCompact = rendering::compactMenuCanvasActive;
  for (const bool compact : {false, true}) {
    rendering::compactMenuCanvasActive = compact;
    View root(0, 0, 760, 3000);
    auto *panel = new PlayOptionsPanelView({}, {.width = 760}, nullptr);
    panel->refresh({});
    root.addView(panel);
    auto *choice = new ChoiceControlView({}, nullptr, 18, 0, true);
    choice->refresh({.selectedId = "on", .options = {
        {.id = "off", .label = i18n::message("settings.skins.judgement_hud.off.label")},
        {.id = "on", .label = i18n::message("settings.skins.judgement_hud.on.label")}}});
    root.addView(choice);
    auto *caption = settings_scene::makeText(i18n::message("settings.skins.feedback.regular.label"),
        18, app_theme::textPrimary());
    auto *legacy = settings_scene::makeControlButton(760, compact ? 66 : 48, caption, true);
    root.addView(legacy);
    const std::array buttons{
        dynamic_cast<Button *>(panel->findViewByName("club-mode")),
        dynamic_cast<Button *>(panel->findViewByName("lane-cover-sudden")),
        dynamic_cast<Button *>(choice->findViewByName("choice-cycle")), legacy};
    for (const auto language : {i18n::Language::English, i18n::Language::Japanese, i18n::Language::Korean}) {
      i18n::setLanguage(language);
      root.propagateLanguageChange();
      root.setWidth(760);
      panel->setWidth(760);
      root.applyYogaLayout();
      std::array<int, 4> compactWidths{};
      for (size_t index = 0; index < buttons.size(); ++index) {
        auto *button = buttons[index];
        compactWidths[index] = button->getWidth();
        require(button->getWidth() >= 66 && button->getWidth() < 380,
                "named, current-value, and choice toggles stay compact in wide parents");
        require(button->getHeight() >= (compact ? 66 : 48), "compact toggle sizing preserves touch height");
      }
      root.setWidth(640);
      panel->setWidth(640);
      root.applyYogaLayout();
      for (size_t index = 0; index < buttons.size(); ++index)
        require(buttons[index]->getWidth() == compactWidths[index],
                "spare parent width does not stretch any toggle construction path");
      root.setWidth(180);
      panel->setWidth(180);
      root.applyYogaLayout();
      for (auto *button : buttons)
        require(button->getWidth() <= 180 && button->getX() >= root.getX(),
                "compact toggles still respect narrow parent bounds");
      require(caption->getWidth() >= caption->textureWidth(),
              "localized short toggle labels retain their natural text width");
    }
  }
  rendering::compactMenuCanvasActive = originalCompact;
  i18n::setLanguage(i18n::Language::English);
}

void testToggleSemanticColorsAndGaugeBars() {
  const auto originalTheme = app_theme::activeMode();
  for (const auto theme : {app_theme::ThemeMode::Dark, app_theme::ThemeMode::Light}) {
    app_theme::setActiveMode(theme);
    View root(0, 0, 640, 2600);
    auto *panel = new PlayOptionsPanelView({}, {.width = 640}, nullptr);
    root.addView(panel);
    auto *choice = new ChoiceControlView({}, nullptr, 18, 0, true);
    root.addView(choice);
    for (const bool active : {false, true, false}) {
      PlayOptionsPanelState state;
      state.clubMode = active;
      state.gaugeAutoShift = GaugeAutoShiftMode::BestClear;
      state.laneCovers.laneCoverEnabled = active;
      panel->refresh(state);
      choice->refresh({.selectedId = active ? "on" : "off",
          .options = {{.id = "off", .label = "Off"}, {.id = "on", .label = "On"}}});
      root.applyYogaLayout();
      for (const char *name : {"club-mode", "lane-cover-sudden", "choice-cycle"}) {
        auto *button = dynamic_cast<Button *>((std::string_view(name) == "choice-cycle" ? choice : static_cast<View *>(panel))->findViewByName(name));
        require(button && button->isSelected() == active,
                "all toggles mark the active state consistently with Club Beat");
        require(button->getHeight() >= 48, "toggle remains a usable target in a vertical panel");
        const auto surface = active ? app_theme::primaryAction() : app_theme::control();
        const auto colors = renderedChoiceColors(*button);
        require(std::ranges::find(colors, surface.toABGR()) != colors.end(),
                "all toggles use the Club Beat active or inactive surface");
        requireToggleForeground(button->getContentView(), active
            ? app_theme::textOn(app_theme::primaryAction()) : app_theme::textPrimary());
      }
    }
    for (const char *name : {"gauge-choice", "gauge-lower-bound-choice"}) {
      auto *control = panel->findViewByName(name);
      auto *dropdown = dynamic_cast<DropdownView *>(control->findViewByName("choice-dropdown"));
      for (const auto &option : dropdown->current.options)
        require(option.leadingColor.has_value(), "every gauge-related option supplies a color bar");
      require(dropdown->triggerIndicator->getVisible(), "selected gauge color appears on the trigger");
      dropdown->setOpen(true);
      for (const auto &option : dropdown->optionButtons)
        require(option.indicator->getVisible(), "gauge popup rows retain their color bars");
      dropdown->setOpen(false);
    }
    auto *autoShift = dynamic_cast<DropdownView *>(panel->findViewByName("gauge-auto-shift-choice")->findViewByName("choice-dropdown"));
    require(autoShift->triggerIndicator->getWidth() == 0, "auto-shift modes do not represent a clear lamp");
    for (const auto &option : autoShift->current.options)
      require(!option.leadingColor, "auto-shift choices have no lamp color bars");
    auto *gauge = dynamic_cast<DropdownView *>(panel->findViewByName("gauge-choice")->findViewByName("choice-dropdown"));
    for (const auto type : {GaugeType::AssistedEasy, GaugeType::Easy, GaugeType::Normal,
                           GaugeType::Hard, GaugeType::ExHard, GaugeType::Hazard}) {
      panel->refresh({.gaugeType = type});
      const auto selected = std::ranges::find_if(gauge->current.options, [&](const auto &option) {
        return option.id == gauge->current.selectedId;
      });
      require(selected != gauge->current.options.end() && selected->leadingColor &&
                  selected->leadingColor->toABGR() == clearLampColorForRank(gaugeTypeToClearRank(type)).toABGR(),
              "gauge bars reuse the established clear-lamp palette");
    }
  }
  app_theme::setActiveMode(originalTheme);
}

void testTabsStayDistinctFromSettingChoices() {
  const auto originalTheme = app_theme::activeMode();
  for (const auto theme : {app_theme::ThemeMode::Dark, app_theme::ThemeMode::Light}) {
    app_theme::setActiveMode(theme);
    for (const bool fitContent : {false, true}) {
      View root(0, 0, 400, 160);
      auto *label = new TextView(app_theme::font, 18);
      label->setText("Settings tab");
      auto *button = settings_scene::makeAccentButton(fitContent ? settings_scene::kFitContentWidth : 240,
          66, label, app_theme::cyan());
      button->setWidth(240);
      root.addView(button);
      int activations = 0;
      button->setOnClickListener([&] { ++activations; });
      for (const int step : {0, 1, 2, 3}) {
        const bool active = step % 2 == 1;
        // Reuse the same button across a responsive rail/underline transition.
        const bool vertical = step < 2;
        tab_button::style(button, active, vertical);
        root.applyYogaLayout();
        auto *indicator = button->findViewByName("tab-active-indicator");
        require(indicator && indicator->getVisible() == active,
                "tab indicator follows selection without rebuilding content");
        require(button->getChildren().size() == (fitContent ? 2 : 1),
                "refreshing a tab reuses its single selection indicator");
        require(indicator->getX() >= button->getX() && indicator->getY() >= button->getY() &&
                    indicator->getX() + indicator->getWidth() <= button->getX() + button->getWidth() &&
                    indicator->getY() + indicator->getHeight() <= button->getY() + button->getHeight(),
                "tab indicator stays inside the control");
        require(vertical ? indicator->getWidth() == 3 && indicator->getHeight() == 50
                         : indicator->getHeight() == 3 && indicator->getWidth() == 224,
                "vertical tabs have a rail and horizontal tabs have an underline");
        requireToggleForeground(label, active
            ? app_theme::textPrimary() : app_theme::textSecondary());
        if (active) {
          const auto colors = renderedChoiceColors(*button);
          require(std::ranges::find(colors, app_theme::accentBorderStrong().toABGR()) != colors.end(),
                  "the active navigation indicator is rendered");
          require(std::ranges::find(colors, app_theme::primaryAction().toABGR()) == colors.end(),
                  "tabs do not use the filled primary surface of setting choices");
        }
      }
      click(*button);
      require(activations == 1, "tab decorations preserve activation");
    }
  }
  app_theme::setActiveMode(originalTheme);
}

void testDisabledChoicesKeepMutedSelectionVisible() {
  const auto originalMode = ui_theme::activeMode();
  for (const auto mode : {ui_theme::ThemeMode::Dark, ui_theme::ThemeMode::Light}) {
    ui_theme::setActiveMode(mode);
    View root(0, 0, 640, 500);
    int changes = 0;
    auto *choice = new ChoiceControlView([&](const std::string &) { ++changes; return true; });
    root.addView(choice);
    DropdownView::State state{.selectedId = "scratch",
        .options = {{.id = "edge", .label = "Edge lane"},
                    {.id = "scratch", .label = "Scratch", .available = false},
                    {.id = "ignore", .label = "Ignore"}}};
    choice->refresh(state);
    root.applyYogaLayout();
    auto *scratch = dynamic_cast<Button *>(choice->findViewByName("choice:scratch"));
    auto *edge = dynamic_cast<Button *>(choice->findViewByName("choice:edge"));
    auto *scratchText = dynamic_cast<TextView *>(scratch->getContentView());
    const auto color = scratchText->currentColor();
    const auto muted = app_theme::textMuted();
    require(!scratch->isEnabled() && scratch->isSelected() &&
                color.r == muted.r && color.g == muted.g && color.b == muted.b,
            "unavailable saved Scratch is muted while retaining its selection marker");
    click(*scratch);
    require(changes == 0 && choice->selectedId() == "scratch",
            "unavailable Scratch does not invoke its selection callback");
    state.options[1].available = true;
    choice->refresh(state);
    const auto enabledColor = scratchText->currentColor();
    require(enabledColor.r != color.r || enabledColor.g != color.g || enabledColor.b != color.b,
            "making a saved choice available restores its enabled text styling");
    state.enabled = false;
    choice->refresh(state);
    require(!scratch->isEnabled() && !edge->isEnabled() && scratch->isSelected() && !edge->isSelected(),
            "locking the group preserves only the current selection marker");
    const auto lockedColor = static_cast<TextView *>(edge->getContentView())->currentColor();
    require(lockedColor.r == muted.r && lockedColor.g == muted.g && lockedColor.b == muted.b,
            "locked unselected choices are visibly muted");
    require(renderedChoiceColors(*scratch) != renderedChoiceColors(*edge),
            "the current locked choice retains a visible surface or border distinction");
    click(*edge);
    require(changes == 0 && choice->selectedId() == "scratch", "locked choices do not invoke callbacks");
  }
  ui_theme::setActiveMode(originalMode);
}

void testSavedBgaNoExpandCanSelectFit() {
  using Mode = AppSettings::BgaDisplayMode;
  for (const auto language : {i18n::Language::English, i18n::Language::Japanese, i18n::Language::Korean}) {
    i18n::setLanguage(language);
    for (const int width : {1200, 180}) {
      View root(0, 0, width, 500);
      Mode saved = Mode::NoExpand;
      int changes = 0;
      auto *choice = settings_scene::makeEnumChoice(settings_scene::LayoutMetrics{}, nullptr, saved,
          {Mode::Fit, Mode::Fill, Mode::Stretch}, settings_scene::formatBgaDisplayModeLabel,
          [&](Mode value) { saved = value; ++changes; });
      root.addView(choice);
      root.applyYogaLayout();
      auto *legacy = dynamic_cast<Button *>(choice->findViewByName("choice:3"));
      require(legacy && legacy->isSelected() && !legacy->isEnabled() &&
                  static_cast<TextView *>(legacy->getContentView())->getText() ==
                      i18n::tr("settings.options.no_expansion.label") && changes == 0,
              "saved No Expansion remains visibly selected without becoming an editable option");
      if (choice->findViewByName("choice-buttons")->getVisible()) {
        click(*dynamic_cast<Button *>(choice->findViewByName("choice:0")));
      } else {
        auto &dropdown = choiceDropdown(*choice);
        require(dropdown.triggerText->getText() == i18n::tr("settings.options.no_expansion.label"),
                "narrow BGA dropdown shows the saved No Expansion label");
        click(*dropdown.triggerButton);
        click(*dropdown.optionButtons.front().button);
      }
      require(saved == Mode::Fit && changes == 1 && choice->selectedId() == "0",
              "explicit Fit replaces saved No Expansion through exactly one settings callback");
      View::dispatchDeferredEventCallbacks();
    }
  }
  i18n::setLanguage(i18n::Language::English);
}

void testDropdownFitsContentAndRevealsSelection() {
  const int previousHeight = rendering::window_height;
  rendering::window_height = 900;
  for (bool compact : {false, true}) {
    rendering::compactMenuCanvasActive = compact;
    for (const int count : {2, 8, 40}) {
      OverlayPortal portal;
      DropdownView dropdown({}, count == 40 ? &portal : nullptr);
      dropdown.setPosition(100, 80, YGPositionTypeAbsolute);
      DropdownView::State state;
      for (int i = 0; i < count; ++i) {
        state.options.push_back({.id = std::to_string(i), .label = "Option"});
      }
      state.selectedId = std::to_string(count - 1);
      {
        View::LayoutBatchScope batch;
        dropdown.refresh(state);
        dropdown.setOpen(true);
      }
      View::dispatchDeferredEventCallbacks();
      for (const auto &item : dropdown.optionButtons)
        require(item.button->getHeight() >= rendering::compactTouchTarget,
                "dropdown entries have comfortable default touch targets");
      auto *menu = dropdown.menuScroll;
      auto *last = dropdown.optionButtons.back().button;
      require(menu->getY() >= 10 && menu->getY() + menu->getHeight() <= 890,
              "dropdown stays inside the viewport margin");
      require(last->getY() >= menu->getY() + 5 &&
                  last->getY() + last->getHeight() <= menu->getY() + menu->getHeight() - 5,
              "opening a constrained dropdown reveals the selected option");
      if (count <= 8) {
        menu->scrollToBottom();
        require(menu->getScrollOffset() == 0,
                "short dropdowns fit all rows, gaps, padding, and borders without scrolling");
      } else {
        require(menu->getHeight() > 600,
                "long dropdowns use available space instead of a fixed row cap");
      }
    }
  }
  rendering::compactMenuCanvasActive = false;
  rendering::window_height = previousHeight;
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

void testRetainedDropdownChangesDensity() {
  rendering::compactMenuCanvasActive = false;
  View root(0, 0, 600, 900);
  auto *dropdown = new DropdownView({});
  root.addView(dropdown);
  for (const bool compact : {false, true, false, true}) {
    rendering::compactMenuCanvasActive = compact;
    root.setWidth(compact ? 585 : 600);
    root.applyYogaLayout();
    const int expected = compact ? rendering::compactTouchTarget : 42;
    require(dropdown->getHeight() == expected &&
                dropdown->triggerButton->getHeight() == expected,
            "retained dropdown updates its trigger height across density changes");
  }
  rendering::compactMenuCanvasActive = false;
}

void testChartFilterRowsContainCompactDropdowns() {
  for (const bool initiallyCompact : {true, false}) {
    rendering::compactMenuCanvasActive = initiallyCompact;
    View root(0, 0, 472, 900);
    auto *panel = new ChartFilterPanelView({});
    root.addView(panel);
    ChartFilterPanelView::State state;
    for (const bool clearMarkVisible : {true, false}) {
      state.clearMarkFilterVisible = clearMarkVisible;
      for (const bool difficultyEnabled : {false, true}) {
        state.difficultyRangeEnabled = difficultyEnabled;
        for (const bool hasLevels : {false, true}) {
          state.difficultyLevels = hasLevels
              ? std::vector<DifficultyLevelInfo>{{.level = "1"}, {.level = "2"}}
              : std::vector<DifficultyLevelInfo>{};
          panel->refresh(state, true);
          for (const bool compact : {initiallyCompact, !initiallyCompact, initiallyCompact}) {
            // Retained panels must reflow on layout without a state refresh.
            rendering::compactMenuCanvasActive = compact;
            root.setWidth(compact ? 320 : 472);
            root.applyYogaLayout();
            const int rowHeight = compact ? 66 : 42;
            const auto &groups = panel->getChildren();
            const bool showDifficulty = difficultyEnabled && hasLevels;
            for (const auto index : {1U, 3U, 6U}) {
              if ((index == 1 && !clearMarkVisible) || (index == 6 && !showDifficulty)) continue;
              View *row = index == 6 ? groups[index]->getChildren()[1] : groups[index];
              for (View *control : row->getChildren()) {
                require(control->getY() >= row->getY() &&
                            control->getY() + control->getHeight() <= row->getY() + row->getHeight(),
                        "chart filter dropdown and range buttons must fit inside their Yoga row");
                require(control->getHeight() == rowHeight,
                        "chart filter dropdown and range buttons follow the active density");
                require(control->getWidth() > 0 && control->getX() >= row->getX() &&
                            control->getX() + control->getWidth() <= row->getX() + row->getWidth(),
                        "narrow chart filter rows retain controls within the available width");
                if (auto *dropdown = dynamic_cast<DropdownView *>(control)) {
                  require(dropdown->triggerButton->getWidth() == dropdown->getWidth() &&
                              dropdown->triggerButton->getHeight() == rowHeight,
                          "chart filter triggers fill their visible dropdown bounds");
                }
              }
            }
            int bottom = panel->getY();
            for (View *group : groups) {
              if (!group->getVisible()) continue;
              require(group->getY() >= bottom, "chart filter sibling groups must not overlap");
              bottom = group->getY() + group->getHeight();
              for (View *child : group->getChildren()) {
                require(child->getY() + child->getHeight() <= bottom,
                        "chart filter difficulty section must contain its dropdown row");
              }
            }
            require(bottom <= panel->getY() + panel->getHeight(),
                    "chart filter preferred height must contain all visible groups");
            const int expectedHeight = (clearMarkVisible ? 276 : 186) +
                (showDifficulty ? 78 : 0) +
                (compact ? (clearMarkVisible ? 48 : 24) + (showDifficulty ? 24 : 0) : 0);
            require(panel->getHeight() == expectedHeight,
                    "chart filter preferred height preserves desktop geometry and grows for compact rows");
          }
          panel->refresh(state, false);
          root.applyYogaLayout();
          require(!panel->getVisible() && panel->getHeight() == 0,
                  "closed chart filter panels remain collapsed");
        }
      }
    }
  }
  rendering::compactMenuCanvasActive = false;
}

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for panel resources");

  testDropdownPreservesAllocatedWidthAcrossRefreshes();
  testNaturalDropdownsRemainVisibleAcrossParentLayouts();
  testSettingsChoicePresentationAndSelection();
  testLocalizedFlickTapModesUseDirectSelection();
  testToggleWidthsFollowContentAcrossLayouts();
  testToggleSemanticColorsAndGaugeBars();
  testTabsStayDistinctFromSettingChoices();
  testDisabledChoicesKeepMutedSelectionVisible();
  testSavedBgaNoExpandCanSelectFit();
  testDropdownFitsContentAndRevealsSelection();
  testColorPickerPopup();
  testColorPickerColorSpace();
  testColorPickerDragAndRelease();
  testSliderReleaseConsumedByDisabledSiblingEndsOnlyItsGesture();
  testLanguageRefreshPreservesPlayOptionsEditingAndScroll();
  testPhoneOptionsReflowWithoutLosingSelection();
  testLaneCoverControlsAreAvailableInPlayOptions();
  testLaneOrderDraftTracksAuthoritativeSelectionAndProfile();
  testPlayOptionsChoiceCallbacksLocksAndPopupClosure();
  testScrollViewUsesPreciseWheelDeltaAndNaturalDirection();
  testScrollViewTargetsWheelEventPosition();
  testDropdownDefersOptionViewsUntilOpen();
  testInputSelectorWidthsStayStableAcrossRefreshes();
  testRetainedDropdownChangesDensity();
  testChartFilterRowsContainCompactDropdowns();
  testDropdownSelectionDefersTeardownUntilItsCallbackReturns();
  testStoragePolicyDirectSelection();

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
    auto *ruleset = dynamic_cast<ChoiceControlView *>(panel->findViewByName("ruleset-choice"));
    auto *lr2 = dynamic_cast<Button *>(ruleset->findViewByName("choice:lr2"));
    auto *beatoraja = dynamic_cast<Button *>(ruleset->findViewByName("choice:beatoraja"));
    require(lr2 && beatoraja && ruleset->findViewByName("choice-buttons")->getVisible(),
            "distinct rulesets offer two choice buttons");
    require(ruleset->selectedId() == "lr2" &&
                static_cast<TextView *>(lr2->getContentView())->getText() == "LR2",
            "ruleset shows the approved LR2 label and current selection");

    auto *clubMode =
        dynamic_cast<Button *>(panel->findViewByName("club-mode"));
    auto *clubContent = dynamic_cast<ToggleButtonContent *>(
        clubMode == nullptr ? nullptr : clubMode->getContentView());
    require(clubContent != nullptr && !clubContent->checked(),
            "Club Beat starts with its off state");
    panel->refresh({.ruleset = GameplayRuleset::LR2, .clubMode = true});
    require(clubContent->checked(),
            "Club Beat refresh switches to its on state");

    click(*beatoraja);
    require(selected == GameplayRuleset::Beatoraja,
            "ruleset choice invokes the Beatoraja enum callback");
    panel->refresh({.ruleset = GameplayRuleset::Beatoraja});
    require(ruleset->selectedId() == "beatoraja" &&
                static_cast<TextView *>(beatoraja->getContentView())->getText() == "Beatoraja",
            "ruleset shows the approved Beatoraja label after refresh");
    click(*lr2);
    require(selected == GameplayRuleset::LR2, "LR2 can be selected directly");

    auto *rulesetLabel = panel->findViewByName("ruleset-section-label");
    auto *gaugeLabel = panel->findViewByName("gauge-section-label");
    require(rulesetLabel != nullptr && gaugeLabel != nullptr &&
                rulesetLabel->getY() < gaugeLabel->getY(),
            "Ruleset appears before Gauge in scroll content");
    require(ruleset->getWidth() > 0 && ruleset->getWidth() <= panel->getWidth(),
            "compact layout keeps the ruleset choices inside its panel");
    require(scroll.getHeight() == 400,
            "adding the ruleset row does not grow the modal viewport");
    scroll.scrollToBottom();
    require(scroll.getScrollOffset() > 0.0f,
            "the compact modal content remains scrollable");
  }
  rendering::UniformCache::getInstance().destroyAll();
  rendering::ShaderManager::getInstance().release();
  bgfx::shutdown();
  return 0;
}
