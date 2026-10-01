#include "rendering/UniformCache.h"
#include "view/TextInputBox.h"
#include "view/TextView.h"
#include "view/Button.h"
#include "view/OverlayPortal.h"
#include "view/DropdownView.h"
#include "view/ScrollView.h"
#include "i18n/Localization.h"
#include "view/IconText.h"
#include "view/UiTheme.h"
#include "scene/play/GuidedAccessReminder.h"
#include "scene/play/GuidedAccessButtonCue.h"

#include <SDL2/SDL.h>
#include <SDL_ttf.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {

ipad_hardware::ButtonLocation testButtonLocation;
ipad_hardware::ButtonLocation GetIOSHardwareButtonLocation() { return testButtonLocation; }
struct TestSafeInsets { float top = 0, right = 0, bottom = 0, left = 0; };
TestSafeInsets GetIOSSafeAreaInsetsNormalized() { return {}; }
constexpr uint32_t kIconLock = 0xf023;
constexpr uint32_t kIconLockOpen = 0xf3c1;
struct ReminderUIFixture {
  View root;
  gameplay::GuidedAccessReminder guidedAccessReminder;
  bool guidedAccessReminderDismissed = false;
  View *guidedAccessReminderLayout = nullptr;
  TextView *guidedAccessReminderTitle = nullptr;
  TextView *guidedAccessReminderWhy = nullptr;
  TextView *guidedAccessReminderHelp = nullptr;
  TextView *guidedAccessReminderDisableHelp = nullptr;
  TextView *guidedAccessReminderIcon = nullptr;
  View *guidedAccessButtonMarker = nullptr;
  TextView *guidedAccessButtonHint = nullptr;
  TextView *guidedAccessButtonCheck = nullptr;
  View *pauseButton = nullptr, *practiceRestartButton = nullptr, *practiceHudText = nullptr;
  View *skinResetLayoutButton = nullptr, *pauseLayout = nullptr;
  void addView(View *view) { root.addView(view); }
  void returnFromGuidedAccessReminder() {}
  void showGuidedAccessReminder();
};
#include "guided_access_reminder_ui.inc"

int clearCompositionCalls = 0;
SDL_Rect nativeInputRect{};

void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

class InspectableTextInputBox final : public TextInputBox {
public:
  using TextInputBox::TextInputBox;

  [[nodiscard]] SDL_Rect textRect() const { return resolvedTextRect(); }
};

void click(TextInputBox &input, int x, int y) {
  SDL_Event down{};
  down.type = SDL_MOUSEBUTTONDOWN;
  down.button.type = SDL_MOUSEBUTTONDOWN;
  down.button.button = SDL_BUTTON_LEFT;
  down.button.which = 1;
  down.button.x = x;
  down.button.y = y;
  SDL_Event up = down;
  up.type = SDL_MOUSEBUTTONUP;
  up.button.type = SDL_MOUSEBUTTONUP;
  input.handleEvents(down);
  input.handleEvents(up);
}

void testDefaultHorizontalPadding() {
  InspectableTextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.applyYogaLayout();
  input.setEditingText("query");

  expect(input.textRect().x == input.getX() + 12,
         "left-aligned text uses the default leading inset");
}

void testClearButtonVisibilityAndCallback() {
  TextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.applyYogaLayout();
  input.setEditingText("query");

  int notifications = 0;
  std::string lastText;
  input.onTextChanged([&](const std::string &text) {
    ++notifications;
    lastText = text;
  });
  input.setClearable(true);

  expect(input.isClearButtonVisible(),
         "configured non-empty input shows its clear button");
  clearCompositionCalls = 0;
  click(input, input.getX() + input.getWidth() - 18,
        input.getY() + input.getHeight() / 2);
  expect(input.getText().empty(), "clear button clears the editing value");
  expect(clearCompositionCalls == 1,
         "clear button cancels the platform IME composition");
  expect(notifications == 1 && lastText.empty(),
         "clear button publishes exactly one normal text change");
  expect(!input.isClearButtonVisible(),
         "clear button hides after the field becomes empty");
}

void testEmptyInputHasNoClearHitTarget() {
  TextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.applyYogaLayout();
  input.setEditingText("");
  input.setClearable(true);

  expect(!input.isClearButtonVisible(),
         "configured empty input keeps its clear button hidden");
  click(input, input.getX() + input.getWidth() - 18,
        input.getY() + input.getHeight() / 2);
  expect(input.getSelected(),
         "empty input trailing edge remains part of the text field");
  input.onUnselected();
}

void testEmptyInputKeepsItsVisibleFrame() {
  struct RecordingBackend final : rendering::UiBatchBackend {
    std::vector<rendering::PosColorVertex> vertices;

    bool submit(const rendering::UiBatchSubmission &submission) noexcept override {
      vertices.insert(vertices.end(), submission.colorVertices.begin(),
                      submission.colorVertices.end());
      return true;
    }
  } backend;
  rendering::UiBatchRenderer renderer(backend);
  RenderContext context(renderer);
  context.pushScissor(0, 0, 300, 100);

  TextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.setBackgroundColor(Color(20, 30, 40, 255));
  input.setBorderColor(Color(80, 90, 100, 255));
  input.setBorderWidth(1);
  input.applyYogaLayout();

  for (const char *value : {"", "query", ""}) {
    input.setEditingText(value);
    renderer.begin();
    backend.vertices.clear();
    input.render(context);
    renderer.end();

    bool paintedBackground = false;
    bool paintedBorder = false;
    for (const auto &vertex : backend.vertices) {
      paintedBackground |= vertex.abgr == Color(20, 30, 40, 255).toABGR();
      paintedBorder |= vertex.abgr == Color(80, 90, 100, 255).toABGR();
    }
    expect(paintedBackground && paintedBorder,
           "input paints its background and border before typing and after "
           "clearing");
    expect(input.getWidth() == 240 && input.getHeight() == 52,
           "clearing text preserves the input layout frame");
  }

  click(input, 120, 26);
  expect(input.getSelected(), "cleared input remains clickable");
  input.onUnselected();
  context.popScissor();
}

void testFocusedInputConsumesItsInitiatingTouch() {
  TextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.setPositionNoLayout(450, 735, YGPositionTypeAbsolute);
  input.applyYogaLayout();
  input.setEditingText("needle");
  input.beginEditing();

  SDL_Event down{};
  down.type = SDL_FINGERDOWN;
  down.tfinger.type = SDL_FINGERDOWN;
  down.tfinger.fingerId = 7;
  down.tfinger.x = 570.0F / static_cast<float>(rendering::design_width);
  down.tfinger.y = 757.0F / static_cast<float>(rendering::design_height);
  expect(!input.handleEvents(down),
         "a focused native text input consumes its initiating touch");

  SDL_Event up = down;
  up.type = SDL_FINGERUP;
  up.tfinger.type = SDL_FINGERUP;
  expect(!input.handleEvents(up),
         "the initiating text touch keeps its matching release");
  input.endEditing();
}

void testBeginEditingUsesTheLatestDeclaredInputFrame() {
  nativeInputRect = {};
  TextInputBox input("assets/fonts/notosanscjkjp.ttf", 18);
  input.setSize(240, 52);
  input.setPositionNoLayout(450, 735, YGPositionTypeAbsolute);
  input.beginEditing();

  expect(nativeInputRect.x >= input.getX() &&
             nativeInputRect.x < input.getX() + input.getWidth() &&
             nativeInputRect.y >= input.getY() &&
             nativeInputRect.y < input.getY() + input.getHeight() &&
             nativeInputRect.w > 0 && nativeInputRect.h > 0,
         "begin editing frames the platform editor at the declared touch "
         "target");
  input.endEditing();
}

void testLanguageRefreshPreservesRawTextAndFocusedInput() {
  i18n::setLanguage(i18n::Language::English);
  View root;
  auto *button = new Button(0, 0, 200, 40);
  auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  label->setDeferredTextureMaterialization(true);
  label->setLocalizedText(i18n::message("settings.options.reset.label"));
  button->setContentView(label);
  root.addView(button);
  auto *raw = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  raw->setDeferredTextureMaterialization(true);
  raw->setText("Reset");
  root.addView(raw);
  auto *input = new TextInputBox("assets/fonts/notosanscjkjp.ttf", 20);
  input->setSize(200, 40);
  input->setEditingText("Reset");
  root.addView(input);
  input->beginEditing();
  int edits = 0;
  input->onTextChanged([&](const std::string &) { ++edits; });

  i18n::setLanguage(i18n::Language::Korean);
  root.propagateLanguageChange();
  expect(label->getText() == "초기화", "button content refreshes its bound message");
  expect(label->textureWidth() > 0 && label->textureHeight() > 0,
         "language refresh updates text geometry");
  expect(raw->getText() == "Reset", "raw metadata equal to English copy stays raw");
  expect(input->getText() == "Reset" && input->getSelected() && edits == 0,
         "language refresh preserves focused input without an edit event");

  label->setText("초기화");
  i18n::setLanguage(i18n::Language::Japanese);
  root.propagateLanguageChange();
  expect(label->getText() == "초기화",
         "even an equal raw replacement clears the old message binding");
  input->endEditing();
  i18n::setLanguage(i18n::Language::English);
}

void testLanguageRefreshKeepsOpenDropdownScrollAndSelection() {
  i18n::setLanguage(i18n::Language::English);
  DropdownView dropdown({});
  DropdownView::State state;
  state.selectedId = "chosen";
  state.open = true;
  state.maxVisibleItems = 2;
  state.options.push_back({"chosen", i18n::message("settings.options.reset.label")});
  for (int i = 0; i < 8; ++i) {
    state.options.push_back({std::to_string(i), "Raw chart name"});
  }
  dropdown.refresh(state);
  ScrollView *menu = nullptr;
  TextView *trigger = nullptr;
  for (auto *child : dropdown.getChildren()) {
    if (auto *scroll = dynamic_cast<ScrollView *>(child)) menu = scroll;
    if (auto *button = dynamic_cast<Button *>(child)) {
      for (auto *content : button->getContentView()->getChildren()) {
        if (auto *text = dynamic_cast<TextView *>(content); text && !trigger) {
          trigger = text;
        }
      }
    }
  }
  expect(menu != nullptr && trigger != nullptr, "dropdown exposes its open menu and trigger");
  menu->setScrollOffset(44.0F);
  expect(menu->getScrollOffset() > 0.0F, "open dropdown has scrollable overflow");
  const float scrollBefore = menu->getScrollOffset();
  i18n::setLanguage(i18n::Language::Korean);
  dropdown.propagateLanguageChange();
  expect(trigger->getText() == "초기화", "selected option refreshes in the trigger");
  expect(menu->getVisible() && menu->getScrollOffset() == scrollBefore,
         "language refresh retains the open dropdown and menu scroll");
  dropdown.refresh(state);
  expect(trigger->getText() == "초기화" && menu->getScrollOffset() == scrollBefore,
         "refreshing the same semantic state does not rebuild translated options");
  i18n::setLanguage(i18n::Language::English);
}

void testLanguageRefreshReachesPortalOverlay() {
  i18n::setLanguage(i18n::Language::English);
  OverlayPortal portal;
  ScrollView overlay(0, 0, 200, 100);
  auto *content = new View();
  content->setHeight(600);
  auto *label = new TextView("assets/fonts/notosanscjkjp.ttf", 20);
  label->setDeferredTextureMaterialization(true);
  label->setLocalizedText(i18n::message("menu.refresh_list.label"));
  content->addView(label);
  overlay.setContentView(content);
  overlay.setScrollOffset(64.0F);
  portal.present(&overlay);
  i18n::setLanguage(i18n::Language::Japanese);
  portal.propagateLanguageChange();
  expect(label->getText() == "一覧を更新" && portal.isPresented(&overlay),
         "portal scroll content refreshes without being dismissed");
  expect(overlay.getScrollOffset() == 64.0F,
         "portal scroll position survives language refresh");
  portal.dismiss(&overlay);
  i18n::setLanguage(i18n::Language::English);
}

class MultilineTextProbe final : public TextView {
public:
  using TextView::TextView;
  int lineHeight() const { return rasterTextLineHeight(); }
  int rasterWidth(const std::string &value) { return measureRasterTextWidth(value); }
  SDL_Surface *rasterizeRun(const std::string &value) {
    return renderFontSourceTextSurface(selectFont(static_cast<Uint32>(value.front())), value);
  }
  SDL_Surface *rasterizeLines(int wrapWidth = 0) {
    int width = 0;
    int height = 0;
    return renderFallbackTextSurface(wrapWidth, width, height);
  }
};

std::uint64_t alphaCoverage(SDL_Surface *surface) {
  expect(surface != nullptr, "descender text rasterizes");
  std::uint64_t coverage = 0;
  for (int y = 0; y < surface->h; ++y) {
    const auto *row = reinterpret_cast<const Uint32 *>(
        static_cast<const Uint8 *>(surface->pixels) + y * surface->pitch);
    for (int x = 0; x < surface->w; ++x) {
      Uint8 r, g, b, alpha;
      SDL_GetRGBA(row[x], surface->format, &r, &g, &b, &alpha);
      coverage += alpha;
    }
  }
  return coverage;
}

void testComposedTextPreservesDescenders() {
  for (const auto *fontPath : {"assets/fonts/notosanscjkjp.ttf", "assets/fonts/arial.ttf",
                               "assets/fonts/fa-solid-900.ttf"}) {
    for (const int size : {18, 22, 32}) {
      for (const auto *text : {"y", "gjpqy", "Ready to play", "Accessibility"}) {
        // Font Awesome's uppercase letters are icons, so use lowercase-only
        // samples when comparing its fallback against one reference font run.
        if (std::string(fontPath) == "assets/fonts/fa-solid-900.ttf" &&
            text[0] >= 'A' && text[0] <= 'Z') continue;
        MultilineTextProbe view(fontPath, size);
        view.setDeferredTextureMaterialization(true);
        view.setColor({255, 255, 255, 255});
        view.setAlign(TextView::CENTER);
        view.setText(text);
        SDL_Surface *run = view.rasterizeRun(text);
        SDL_Surface *composed = view.rasterizeLines();
        if (alphaCoverage(run) != alphaCoverage(composed)) {
          std::cerr << "Descender clipping: " << text << " font=" << fontPath << " size=" << size
                    << " run=" << run->w << 'x' << run->h
                    << " composed=" << composed->w << 'x' << composed->h << '\n';
        }
        expect(alphaCoverage(run) == alphaCoverage(composed),
               "line composition must preserve every descender pixel from the font renderer");
        SDL_FreeSurface(composed);
        view.setText(std::string(text) + "\n" + text);
        SDL_Surface *multiline = view.rasterizeLines();
        expect(alphaCoverage(multiline) == 2 * alphaCoverage(run),
               "every explicit line must retain its descenders without overlapping the next line");
        expect(view.textureHeight() == (multiline->h + 1) / 2,
               "deferred layout height must match the complete multiline raster");
        SDL_FreeSurface(multiline);

        const int logicalWidth = (view.rasterWidth(text) + 1) / 2;
        view.setWidth(logicalWidth);
        view.setWrap(true);
        view.setText(std::string(text) + " " + text);
        view.applyYogaLayout();
        SDL_Surface *wrapped = view.rasterizeLines(logicalWidth * 2);
        expect(alphaCoverage(wrapped) == 2 * alphaCoverage(run),
               "automatic wrapping must preserve all descender pixels");
        expect(view.textureHeight() == (wrapped->h + 1) / 2,
               "wrapped layout height must match the complete glyph raster");
        SDL_FreeSurface(wrapped);
        SDL_FreeSurface(run);
      }
    }
  }
}

int firstInkX(SDL_Surface *surface, int top, int bottom) {
  expect(surface != nullptr, "multiline text rasterizes");
  int first = surface->w;
  for (int y = top; y < std::min(bottom, surface->h); ++y) {
    const auto *row = reinterpret_cast<const Uint32 *>(
        static_cast<const Uint8 *>(surface->pixels) + y * surface->pitch);
    for (int x = 0; x < surface->w; ++x) {
      Uint8 r, g, b, alpha;
      SDL_GetRGBA(row[x], surface->format, &r, &g, &b, &alpha);
      if (alpha != 0) first = std::min(first, x);
    }
  }
  expect(first < surface->w, "each text line contains visible glyphs");
  return first;
}

void testMultilineAlignmentAcrossFonts() {
  for (const auto &glyph : {std::string("M"), std::string("あ"), std::string("가")}) {
    MultilineTextProbe view("assets/fonts/notosanscjkjp.ttf", 22);
    view.setDeferredTextureMaterialization(true);
    view.setColor({255, 255, 255, 255});
    const std::string longLine = glyph + glyph + glyph + glyph + glyph + glyph;
    view.setText(longLine + "\n" + glyph);
    SDL_Surface *left = view.rasterizeLines();
    const int leftX = firstInkX(left, view.lineHeight(), view.lineHeight() * 2);
    SDL_FreeSurface(left);
    const int spare = view.rasterWidth(longLine) - view.rasterWidth(glyph);
    view.setDeferredTextureMaterialization(false);
    expect(bgfx::isValid(view.textureHandle()), "left-aligned texture is materialized");
    view.setDeferredTextureMaterialization(true);
    for (const auto alignment : {TextView::CENTER, TextView::RIGHT}) {
      view.setAlign(alignment);
      expect(!bgfx::isValid(view.textureHandle()),
             "changing multiline alignment invalidates the previously rasterized texture");
      SDL_Surface *aligned = view.rasterizeLines();
      const int alignedX = firstInkX(aligned, view.lineHeight(), view.lineHeight() * 2);
      SDL_FreeSurface(aligned);
      expect(alignedX - leftX == (alignment == TextView::CENTER ? spare / 2 : spare),
             "alignment positions each primary or fallback text line independently");
    }
  }
}

void testReminderDescriptionPreservesLineBreaks() {
  for (const auto language : {i18n::Language::English, i18n::Language::Japanese,
                              i18n::Language::Korean}) {
    i18n::setLanguage(language);
    MultilineTextProbe view("assets/fonts/notosanscjkjp.ttf", 22);
    view.setDeferredTextureMaterialization(true);
    view.setSize(1800, 140);
    view.setAlign(TextView::CENTER);
    view.setLocalizedText(i18n::message("gameplay.ipad_gesture_reminder.help",
        {{"button", i18n::message("gameplay.ipad_gesture_reminder.button.unknown")}}));
    const int unwrappedHeight = view.textureHeight();
    expect(unwrappedHeight >= (view.lineHeight() * 2 + 1) / 2,
           "reminder description keeps its two setup paragraphs in every language");
    view.setWrap(true);
    view.applyYogaLayout();
    expect(view.textureHeight() == unwrappedHeight,
           "centered wrapping preserves the reminder's explicit paragraph breaks");
    expect(view.textureHeight() <= view.getContentHeight(),
           "the setup instructions fit without clipping");
    ReminderUIFixture fixture;
    fixture.root.setSize(rendering::window_width, rendering::window_height);
    fixture.showGuidedAccessReminder();
    fixture.root.applyYogaLayout();
    const auto *why = fixture.guidedAccessReminderWhy;
    const auto *help = fixture.guidedAccessReminderHelp;
    const auto *disableHelp = fixture.guidedAccessReminderDisableHelp;
    expect(why->pointSize() > help->pointSize(), "explanation is larger than setup instructions");
    expect(why->textureWidth() <= why->getContentWidth() &&
           why->textureHeight() <= why->getContentHeight() &&
           help->textureHeight() <= help->getContentHeight() &&
           disableHelp->textureHeight() <= disableHelp->getContentHeight(),
           "separate explanation and instructions fit in the production layout");
    expect(help->getY() - (why->getY() + why->getHeight()) >= 24,
           "explanation has a clear paragraph gap before the setup steps");
    expect(disableHelp->getY() - (help->getY() + help->getHeight()) >= 24,
           "disable reminder note is spaced separately from setup steps");
    expect(why->currentColor().r == ui_theme::cyan().r &&
           why->currentColor().g == ui_theme::cyan().g &&
           why->currentColor().b == ui_theme::cyan().b,
           "explanation uses the theme accent color");
    // Button identity remains known even when a partial window suppresses the edge cue.
    for (const auto &entry : {
             std::pair{"iPad7,5", "gameplay.ipad_gesture_reminder.button.home"},
             std::pair{"iPad16,3", "gameplay.ipad_gesture_reminder.button.top"},
             std::pair{"unknown", "gameplay.ipad_gesture_reminder.button.unknown"}}) {
      testButtonLocation = ipad_hardware::locateButton(
          ipad_hardware::modelForIdentifier(entry.first),
          ipad_hardware::Orientation::Unknown, false);
      fixture.showGuidedAccessReminder();
      fixture.root.applyYogaLayout();
      const std::string expectedHelp = i18n::message(
          "gameplay.ipad_gesture_reminder.help",
          {{"button", i18n::message(entry.second)}}).resolve();
      expect(help->getText() == expectedHelp &&
             help->getText().find("{button}") == std::string::npos,
             "instruction names the model's button or uses the unknown-model fallback");
      expect(help->textureHeight() <= help->getContentHeight(),
             "localized device-specific instructions fit the help area");
    }
    testButtonLocation = {};
  }
  i18n::setLanguage(i18n::Language::English);
}

void testHardwareButtonCueTextFits() {
  for (const auto language : {i18n::Language::English, i18n::Language::Japanese,
                              i18n::Language::Korean}) {
    i18n::setLanguage(language);
    for (const auto key : {"gameplay.ipad_gesture_reminder.home_button",
                           "gameplay.ipad_gesture_reminder.top_button",
                           "gameplay.ipad_gesture_reminder.enabled"}) {
      TextView view("assets/fonts/notosanscjkjp.ttf", 22);
      view.setDeferredTextureMaterialization(true);
      view.setSize(360, 112);
      view.setPadding(Edge::All, 8);
      if (std::string(key) == "gameplay.ipad_gesture_reminder.enabled") {
        view.setPadding(Edge::Left, 48); // Space for the separate Font Awesome check.
      }
      view.setWrap(true);
      view.setAlign(TextView::CENTER);
      view.setLocalizedText(i18n::message(key));
      view.applyYogaLayout();
      expect(view.textureWidth() <= view.getContentWidth() &&
             view.textureHeight() <= view.getContentHeight(),
             "localized hardware cue fits inside its padded label");
    }
  }
  i18n::setLanguage(i18n::Language::English);
}

void testHardwareCueUsesRealViewHierarchy() {
  using namespace ipad_hardware;
  testButtonLocation = locateButton(modelForIdentifier("iPad16,3"), Orientation::LandscapeRight, true);
  ReminderUIFixture fixture;
  fixture.root.setSize(rendering::window_width, rendering::window_height);
  fixture.showGuidedAccessReminder();
  fixture.root.applyYogaLayout();
  expect(fixture.guidedAccessButtonHint->getVisible(), "idle cue is visible");
  expect(!fixture.guidedAccessButtonCheck->getVisible(), "idle cue has no success check");
  fixture.guidedAccessReminder.update(true, true, 1000);
  fixture.showGuidedAccessReminder();
  fixture.root.applyYogaLayout();
  expect(fixture.guidedAccessButtonCheck->getVisible(), "confirmation displays its check");
  expect(!fixture.guidedAccessReminderWhy->getVisible() &&
         !fixture.guidedAccessReminderDisableHelp->getVisible(),
         "confirmation hides the setup explanation and disable note");
  expect(fixture.guidedAccessButtonCheck->primaryFontPath() == ui_icons::kFontAwesomeSolidPath,
         "confirmation uses the Font Awesome face");
  expect(fixture.guidedAccessButtonCheck->getText() == ui_icons::textForCodepoint(0xf00c),
         "confirmation uses Font Awesome's check glyph");
  for (const auto orientation : {Orientation::Portrait, Orientation::PortraitUpsideDown,
                                 Orientation::LandscapeLeft, Orientation::LandscapeRight}) {
    testButtonLocation = locateButton(modelForIdentifier("iPad16,3"), orientation, true);
    fixture.showGuidedAccessReminder();
    fixture.root.applyYogaLayout();
    const auto *hint = fixture.guidedAccessButtonHint;
    const auto *check = fixture.guidedAccessButtonCheck;
    const int textLeft = hint->getContentX() +
        (hint->getContentWidth() - hint->textureWidth()) / 2;
    expect(check->getX() >= hint->getX() &&
           check->getY() >= hint->getY() &&
           check->getY() + check->getHeight() <= hint->getY() + hint->getHeight() &&
           check->getX() + check->getWidth() <= textLeft - 4,
           "Font Awesome check stays inside the capsule and clear of its text after rotation");
  }
  for (const auto tick : {1600, 1795, 1800, 2000, 2999}) {
    fixture.guidedAccessReminder.update(true, true, tick);
    fixture.showGuidedAccessReminder();
    expect(fixture.guidedAccessButtonHint->getVisible() &&
           fixture.guidedAccessButtonMarker->getVisible() &&
           fixture.guidedAccessButtonCheck->getVisible(), "success cue remains visible until playback");
    expect(fixture.guidedAccessButtonCheck->currentColor().a == ui_theme::lime().a &&
           fixture.guidedAccessButtonHint->currentColor().a == ui_theme::textPrimary().a,
           "success text and Font Awesome check stay fully opaque");
    expect(!fixture.guidedAccessReminder.completed(), "cue does not shorten the startup delay");
  }
  fixture.guidedAccessReminder.reset();
  fixture.showGuidedAccessReminder();
  expect(fixture.guidedAccessButtonHint->getVisible() &&
         fixture.guidedAccessButtonHint->currentColor().a == ui_theme::textPrimary().a &&
         !fixture.guidedAccessButtonCheck->getVisible(), "interruption restores the idle cue");
  testButtonLocation = {};
  fixture.guidedAccessReminder.update(true, true, 2000);
  fixture.showGuidedAccessReminder();
  expect(!fixture.guidedAccessButtonHint->getVisible() &&
         !fixture.guidedAccessButtonCheck->getVisible(), "unknown location has no floating success icon");
}

void testDeferredTextKeepsRasterizedLineHeight() {
  constexpr int logicalSize = 20;
  constexpr int rasterScale = 2;
  constexpr const char *text = "Settings";
  TextView view("assets/fonts/notosanscjkjp.ttf", logicalSize);
  view.setDeferredTextureMaterialization(true);
  view.setText(text);

  TTF_Font *font = TTF_OpenFont("assets/fonts/notosanscjkjp.ttf",
                                logicalSize * rasterScale);
  expect(font != nullptr, "test font opens for text geometry comparison");
  int rasterWidth = 0;
  int rasterHeight = 0;
  expect(TTF_SizeUTF8(font, text, &rasterWidth, &rasterHeight) == 0,
         "SDL_ttf reports the unrendered text raster bounds");
  TTF_CloseFont(font);

  expect(view.textureHeight() ==
             (rasterHeight + rasterScale - 1) / rasterScale,
         "deferred text preserves the rasterized line height before rendering");
}

void testDeferredWrappedTextKeepsRasterizedLineHeight() {
  constexpr int logicalSize = 20;
  constexpr int rasterScale = 2;
  constexpr const char *text = "Settings";
  TextView view("assets/fonts/notosanscjkjp.ttf", logicalSize);
  view.setDeferredTextureMaterialization(true);
  view.setWrap(true);
  view.setText(text);
  view.setWidth(400.0F);
  view.applyYogaLayout();

  TTF_Font *font = TTF_OpenFont("assets/fonts/notosanscjkjp.ttf",
                                logicalSize * rasterScale);
  expect(font != nullptr,
         "test font opens for wrapped text geometry comparison");
  const int rasterHeight = TTF_FontLineSkip(font);
  TTF_CloseFont(font);

  expect(view.textureHeight() ==
             (rasterHeight + rasterScale - 1) / rasterScale,
         "deferred wrapped text preserves SDL_ttf line-skip height before "
         "rendering");
}

} // namespace

extern "C" void SDLCALL TextInputBoxTest_ClearComposition() {
  ++clearCompositionCalls;
}

extern "C" void SDLCALL TextInputBoxTest_SetTextInputRect(
    const SDL_Rect *rect) {
  nativeInputRect = rect != nullptr ? *rect : SDL_Rect{};
}

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  expect(bgfx::init(init), "headless bgfx initializes for text input tests");

  testMultilineAlignmentAcrossFonts();
  testComposedTextPreservesDescenders();
  testReminderDescriptionPreservesLineBreaks();
  testHardwareButtonCueTextFits();
  testHardwareCueUsesRealViewHierarchy();
  testLanguageRefreshPreservesRawTextAndFocusedInput();
  testLanguageRefreshReachesPortalOverlay();
  testLanguageRefreshKeepsOpenDropdownScrollAndSelection();
  testDefaultHorizontalPadding();
  testClearButtonVisibilityAndCallback();
  testEmptyInputHasNoClearHitTarget();
  testEmptyInputKeepsItsVisibleFrame();
  testFocusedInputConsumesItsInitiatingTouch();
  testBeginEditingUsesTheLatestDeclaredInputFrame();
  testDeferredTextKeepsRasterizedLineHeight();
  testDeferredWrappedTextKeepsRasterizedLineHeight();

  TextInputBox::releaseCachedCursors();
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
