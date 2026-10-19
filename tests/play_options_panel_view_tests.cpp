#include "view/Button.h"
#include "view/CheckboxButtonContent.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#endif
#define private public
#include "view/DropdownView.h"
#include "view/OverlayPortal.h"
#undef private
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
#include "scene/ChartFilterSortPanelView.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>
#include <iostream>

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
  CheckboxButtonContent *efficientContent = nullptr;
  std::vector<DropdownView *> fields;
  for (auto *row : editor.getChildren()) {
    if (auto *text = dynamic_cast<TextView *>(row)) codecStatus = text;
    if (auto *button = dynamic_cast<Button *>(row)) {
      efficientButton = button;
      efficientContent = dynamic_cast<CheckboxButtonContent *>(button->getContentView());
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
          "efficient-video checkbox saves the explicit opt-in");
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
      for (const char *name : {"ruleset-lr2", "ruleset-beatoraja"}) {
        auto *button = dynamic_cast<Button *>(panel->findViewByName(name));
        require(button && button->getHeight() >= rendering::compactTouchTarget,
                "phone ruleset buttons retain 44-point touch height after rotation");
        require(button->getX() >= panel->getX() &&
                    button->getX() + button->getWidth() <= panel->getX() + width,
                "phone option buttons stay inside the resized panel");
      }
    }
    click(*static_cast<Button *>(panel->findViewByName("ruleset-beatoraja")));
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

  testColorPickerPopup();
  testColorPickerColorSpace();
  testColorPickerDragAndRelease();
  testSliderReleaseConsumedByDisabledSiblingEndsOnlyItsGesture();
  testLanguageRefreshPreservesPlayOptionsEditingAndScroll();
  testPhoneOptionsReflowWithoutLosingSelection();
  testLaneCoverControlsAreAvailableInPlayOptions();
  testLaneOrderDraftTracksAuthoritativeSelectionAndProfile();
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
