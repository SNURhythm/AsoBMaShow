#include "scene/SettingsSceneShared.h"
#include "rendering/UniformCache.h"
#include "view/ActionButtonContent.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <stdexcept>
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
struct TextGeometryBackend final : rendering::UiBatchBackend {
  std::vector<rendering::PosTexCoord0Vertex> vertices;

  bool submit(const rendering::UiBatchSubmission &submission) noexcept override {
    vertices.insert(vertices.end(), submission.texturedVertices.begin(),
                    submission.texturedVertices.end());
    return true;
  }
};

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

SDL_FRect renderedTextBounds(View &view) {
  TextGeometryBackend backend;
  rendering::UiBatchRenderer renderer(backend);
  RenderContext context(renderer);
  renderer.begin();
  view.render(context);
  renderer.end();
  require(backend.vertices.size() == 4, "button submits its complete text quad");
  return {backend.vertices[0].x, backend.vertices[0].y,
          backend.vertices[1].x - backend.vertices[0].x,
          backend.vertices[2].y - backend.vertices[0].y};
}

void requireFits(const SDL_FRect &text, int x, int y, int width, int height) {
  require(text.x >= x && text.y >= y && text.x + text.w <= x + width &&
              text.y + text.h <= y + height && text.w > 0 && text.h > 0,
          "Japanese button text fits its allocated content without clipping");
}

void testFlatSettingsSectionUsesContentHeight() {
  settings_scene::LayoutMetrics metrics;
  metrics.flatSections = true;
  metrics.cardPadding = 14;
  metrics.cardGap = 12;
  for (const auto language : {i18n::Language::English, i18n::Language::Japanese,
                              i18n::Language::Korean}) {
    i18n::setLanguage(language);
    View root(0, 0, 600, 900);
    root.setFlexDirection(FlexDirection::Column);
    auto *body = new View();
    body->setHeight(48);
    auto *section = settings_scene::makeCard(
        metrics, i18n::message("settings.navigation.audio.label"), "", body, 500, 600);
    root.addView(section);
    root.applyYogaLayout();
    require(section->getHeight() < 200,
            "flat settings sections discard legacy oversized card minimums");
    require(body->getHeight() == 48 && body->getY() >= section->getY() + 14,
            "section density preserves controls and localized heading space");
    require(section->getChildren().front()->getHeight() == 1,
            "flat settings groups use a single hairline divider");
  }
  i18n::setLanguage(i18n::Language::English);
}

void testCompactActionCaptionsFitLocalizedControls() {
  for (const auto language : {i18n::Language::English, i18n::Language::Japanese,
                              i18n::Language::Korean}) {
    i18n::setLanguage(language);
    for (const int width : {128, 144, 160}) {
      for (const auto key : {"settings.profiles.activate.label",
                            "settings.profiles.rename.label", "settings.profiles.copy.label",
                            "settings.profiles.delete.label", "settings.profiles.export.label",
                            "settings.profiles.import_over.label", "records.export_video.label"}) {
        View root(0, 0, width, 72);
        auto *button = new Button(0, 0, width, 72);
        auto *caption = settings_scene::makeText(i18n::message(key), 18,
                                                app_theme::onAction());
        button->setContentView(action_button::makeContent(caption, ui_icons::kDownload));
        action_button::setIconColor(button, app_theme::onAction);
        root.addView(button);
        root.applyYogaLayout();
        auto *icon = button->getContentView()->findViewByName("actionIcon");
        require(icon && icon->getY() + icon->getHeight() <= caption->getY(),
                "compact action glyph and caption never overlap");
        requireFits(renderedTextBounds(*caption), button->getX() + 8,
                    button->getY(), width - 16, 72);
        requireFits(renderedTextBounds(*icon), button->getX(), button->getY(), width, 72);
        require(caption->getText() == i18n::tr(key),
                "compact actions retain their localized caption");
      }
    }
  }
  i18n::setLanguage(i18n::Language::English);
}

void testInlineIconsPreserveCompactLocalizedLabels() {
  require(TTF_Init(), "icon font verification initializes SDL_ttf");
  auto *font = TTF_OpenFont(ui_icons::kFontAwesomeSolidPath, 18);
  require(font != nullptr, "bundled icon font opens");
  for (const auto glyph : {ui_icons::kUser, ui_icons::kKeyboard, ui_icons::kClock,
                          ui_icons::kRoad, ui_icons::kDisplay, ui_icons::kImage,
                          ui_icons::kVolume, ui_icons::kGlobe, ui_icons::kRuleset,
                          ui_icons::kHeart, ui_icons::kRotate, ui_icons::kArrowDown,
                          ui_icons::kShuffle, ui_icons::kGripLines, ui_icons::kLifeRing,
                          ui_icons::kBars, ui_icons::kGauge, ui_icons::kPlay,
                          ui_icons::kFlagCheckered}) {
    require(TTF_FontHasGlyph(font, glyph), "readability icons exist in the shipped font");
  }
  TTF_CloseFont(font);
  TTF_Quit();
  for (const auto language : {i18n::Language::English, i18n::Language::Japanese,
                              i18n::Language::Korean}) {
    i18n::setLanguage(language);
    for (const int width : {150, 280}) {
      Button button(0, 0, width, 58);
      auto *caption = settings_scene::makeText(
          i18n::message("settings.navigation.difficulty_tables.label"), 20,
          app_theme::textPrimary());
      auto *content = action_button::makeInlineContent(caption, ui_icons::kChartLine);
      content->setPadding(Edge::Left, 14)->setPadding(Edge::Right, 14);
      button.setContentView(content);
      button.applyYogaLayout();
      auto *icon = button.getContentView()->findViewByName("actionIcon");
      require(icon && icon->getX() == 14 &&
                  icon->getX() + icon->getWidth() + 10 <= caption->getX(),
              "inline icons reserve a stable slot before the localized label");
      requireFits(renderedTextBounds(*caption), caption->getX(), button.getY(),
                  width - 14 - caption->getX(), 58);
      requireFits(renderedTextBounds(*icon), button.getX(), button.getY(), width, 58);
      require(button.getHeight() == 58, "icons do not increase compact navigation height");
    }
  }
  i18n::setLanguage(i18n::Language::English);
}

void testApplicationThemePreservesGameplayPalette() {
  const auto originalMode = ui_theme::activeMode();
  const auto same = [](Color a, Color b) { return a.toABGR() == b.toABGR(); };
  for (const auto mode : {ui_theme::ThemeMode::Dark, ui_theme::ThemeMode::Light}) {
    ui_theme::setActiveMode(mode);
    const auto gameplayAccent = ui_theme::cyan();
    const auto gameplayRadius = ui_theme::controlRadius();
    require(same(app_theme::primaryAction(), main_menu_theme::action()),
            "primary actions retain the selector accent");
    const auto luminance = [](Color color) {
      const auto linear = [](double channel) {
        channel /= 255.0;
        return channel <= 0.04045 ? channel / 12.92
                                 : std::pow((channel + 0.055) / 1.055, 2.4);
      };
      return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) +
             0.0722 * linear(color.b);
    };
    using Category = app_theme::ActionCategory;
    const auto categories = {Category::Primary, Category::Confirmation,
                             Category::Information, Category::DataTransfer,
                             Category::Caution, Category::Destructive};
    for (auto first = categories.begin(); first != categories.end(); ++first) {
      const auto palette = app_theme::actionPalette(*first);
      for (auto second = first + 1; second != categories.end(); ++second) {
        require(!same(palette.normal, app_theme::actionPalette(*second).normal),
                "grouped semantic actions remain visually distinct");
      }
      for (const auto fill : {palette.normal, palette.hover, palette.pressed}) {
        require(fill.a == 255 && palette.label.a == 255,
                "semantic action buttons and labels remain opaque");
        require((luminance(palette.label) + 0.05) / (luminance(fill) + 0.05) >= 4.5,
                "semantic action labels meet normal text contrast in every state");
        require(same(app_theme::textOn(fill), palette.label),
                "legacy action labels follow the category palette");
      }
    }
    require(same(app_theme::textOn(app_theme::primaryAction()), main_menu_theme::onAction()),
            "blue application actions retain white labels in both themes");
    require(app_theme::panelRadius() == 0 && app_theme::controlRadius() == 0 &&
                app_theme::shadow().a == 0 && app_theme::cardShadow().a == 0,
            "application chrome uses square surfaces without decorative shadows");
    for (const auto surface : {app_theme::backdrop(), app_theme::panel(),
                               app_theme::panelStrong(), app_theme::panelSubtle(),
                               app_theme::resultBackdrop(), app_theme::resultPanelStrong(),
                               app_theme::mainMenuBackdrop()}) {
      require(surface.a == 255, "scene and modal backgrounds block content behind them");
    }
    require(main_menu_theme::backdrop().a < 255 && main_menu_theme::surface().a < 255 &&
                main_menu_theme::control().a < 255 && app_theme::hudPanel().a < 255 &&
                app_theme::hudPanelStrong().a < 255 && app_theme::hudPanelSubtle().a < 255 &&
                app_theme::textPrimary().a == 255,
            "only BGA-facing selector and HUD surfaces retain translucent backgrounds");
    require(same(ui_theme::cyan(), gameplayAccent) && gameplayRadius == 8 &&
                ui_theme::controlRadius() == gameplayRadius &&
                !same(app_theme::primaryAction(), ui_theme::primaryAction()),
            "application styling does not mutate the gameplay palette or geometry");
  }
  ui_theme::setActiveMode(originalMode);
}

void testFixedJapaneseButtonsShrinkOnlyTheirRenderedText() {
  i18n::setLanguage(i18n::Language::Japanese);
  for (const auto &[key, width] :
       {std::pair{"menu.viewer.label", 90},
        std::pair{"menu.reveal.label", 120},
        std::pair{"settings.difficulty_tables.add_table.label", 140}}) {
    auto *label = settings_scene::makeText(i18n::message(key), 28,
                                           ui_theme::textPrimary());
    std::unique_ptr<Button> button(settings_scene::makeControlButton(width, 60, label));
    const int naturalWidth = label->textureWidth();
    const int naturalHeight = label->textureHeight();
    require(naturalWidth > width, "fixture has a Japanese label wider than its button");
    const auto fitted = renderedTextBounds(*button);
    requireFits(fitted, button->getX(), button->getY(), width, 60);
    require(fitted.w < naturalWidth && fitted.h < naturalHeight,
            "button shrinks text uniformly rather than clipping its edges");
    require(std::abs(fitted.w * naturalHeight - fitted.h * naturalWidth) <=
                naturalWidth + naturalHeight,
            "button text keeps its aspect ratio within pixel rounding");
    require(label->textureWidth() == naturalWidth && label->pointSize() == 28,
            "fitting preserves natural measurement and the requested font size");

    button->setSize(naturalWidth + 40, naturalHeight + 20);
    const auto restored = renderedTextBounds(*button);
    require(restored.w == naturalWidth && restored.h == naturalHeight,
            "a wider button restores the nominal text size without upscaling");
    button->setSize(width, 60);
    label->setText("Go");
    const auto shortText = renderedTextBounds(*button);
    require(shortText.w == label->textureWidth() && shortText.h == label->textureHeight(),
            "shorter button text restores its nominal size");
  }
  i18n::setLanguage(i18n::Language::English);
}

void testNestedButtonLabelsFitButOrdinaryTextKeepsItsOverflow() {
  Button button(0, 0, 110, 50);
  auto *content = new View();
  content->setWidthPercent(100);
  content->setHeight(50);
  content->setPadding(Edge::All, 10);
  auto *label = settings_scene::makeText("保存場所を開く", 28,
                                         ui_theme::textPrimary(),
                                         TextView::CENTER, TextView::MIDDLE);
  content->addView(label);
  button.addView(content);
  const auto fitted = renderedTextBounds(button);
  requireFits(fitted, 10, 10, 90, 30);

  TextView ordinary("assets/fonts/notosanscjkjp.ttf", 28);
  ordinary.setText("保存場所を開く");
  ordinary.setSize(90, 30);
  const auto unchanged = renderedTextBounds(ordinary);
  require(unchanged.w == ordinary.textureWidth() && unchanged.w > 90,
          "ordinary text outside buttons retains its overflow behavior");
}

void testButtonFittingRespondsToLanguageAndAvailableHeight() {
  i18n::setLanguage(i18n::Language::English);
  auto *label = settings_scene::makeText(
      i18n::message("settings.difficulty_tables.add_table.label"), 28,
      ui_theme::textPrimary());
  std::unique_ptr<Button> button(settings_scene::makeControlButton(140, 60, label));
  const auto english = renderedTextBounds(*button);
  require(english.w == label->textureWidth(), "short English label uses nominal size");
  i18n::setLanguage(i18n::Language::Japanese);
  button->propagateLanguageChange();
  label->setOverflow(TextView::TextOverflow::Marquee);
  requireFits(renderedTextBounds(*button), 0, 0, 140, 60);
  button->setSize(140, 18);
  const auto shortButton = renderedTextBounds(*button);
  requireFits(shortButton, 0, 0, 140, 18);
  require(shortButton.h < label->textureHeight(), "button height also limits text size");
  require(std::abs(shortButton.x * 2 + shortButton.w - 140) <= 1,
          "fitted marquee labels keep centered alignment instead of scrolling");
  i18n::setLanguage(i18n::Language::English);
  button->propagateLanguageChange();
  button->setSize(140, 60);
  const auto restored = renderedTextBounds(*button);
  require(restored.w == english.w && restored.h == english.h,
          "switching back restores the label's nominal rendering size");
}
} // namespace

int main() {
  testApplicationThemePreservesGameplayPalette();
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  assert(bgfx::init(init));
  testCompactActionCaptionsFitLocalizedControls();
  testInlineIconsPreserveCompactLocalizedLabels();
  testFlatSettingsSectionUsesContentHeight();
  testFixedJapaneseButtonsShrinkOnlyTheirRenderedText();
  testNestedButtonLabelsFitButOrdinaryTextKeepsItsOverflow();
  testButtonFittingRespondsToLanguageAndAvailableHeight();
  {
    i18n::setLanguage(i18n::Language::English);
    View row;
    row.setSize(600, 200);
    row.setFlexDirection(FlexDirection::Row);
    auto *label = settings_scene::makeText(
        i18n::message("settings.options.reset.label"), 22,
        ui_theme::textPrimary());
    auto *button = settings_scene::makeControlButton(
        settings_scene::kFitContentWidth, 60, label);
    row.addView(button);
    auto *input = settings_scene::makeTextInput(
        settings_scene::resolveLayoutMetrics(), 240);
    input->setEditingText("Reset");
    row.addView(input);
    const std::string englishLabel = label->getText();

    i18n::setLanguage(i18n::Language::Korean);
    row.propagateLanguageChange();
    assert(label->getText() == i18n::tr("settings.options.reset.label"));
    assert(label->getText() != englishLabel);
    assert(input->getText() == "Reset");
    assert(button->getWidth() >= label->measureTextWidth(label->getText()) + 32);

    i18n::setLanguage(i18n::Language::Japanese);
    row.propagateLanguageChange();
    assert(label->getText() == i18n::tr("settings.options.reset.label"));
    assert(input->getText() == "Reset");
    i18n::setLanguage(i18n::Language::English);
  }
  {
    View row;
    row.setSize(400, 200);
    row.setFlexDirection(FlexDirection::Row);
    row.setFlexWrap(YGWrapWrap);
    row.setAlignItems(YGAlignFlexStart);
    row.setAlignContent(YGAlignFlexStart);
    row.setGap(10);
    auto *label = settings_scene::makeText("라이브러리 다시 구축", 22,
                                          ui_theme::textPrimary());
    auto *button = settings_scene::makeAccentButton(
        settings_scene::kFitContentWidth, 60, label, ui_theme::lime());
    row.addView(button);
    const auto fits = [&] {
      const int textWidth = label->measureTextWidth(label->getText());
      assert(button->getWidth() >= textWidth + 32);
      assert(button->getWidth() <= textWidth + 34);
      assert(button->getHeight() == 60);
      assert(label->getX() >= button->getX() + 16);
      assert(label->getX() + label->getWidth() <=
             button->getX() + button->getWidth() - 16);
      const auto text = renderedTextBounds(*button);
      require(text.w == label->textureWidth() && text.h == label->textureHeight(),
              "growable buttons expand to natural text instead of shrinking it");
    };
    fits();
    const int koreanWidth = button->getWidth();
    label->setText("開始メトロノーム: オフ");
    fits();
    label->setText("시작 메트로놈 꺼짐");
    fits();
    label->setText("Go");
    fits();
    assert(button->getWidth() < koreanWidth);
    label->setText("라이브러리 다시 구축");
    fits();
    auto *second = settings_scene::makeAccentButton(
        settings_scene::kFitContentWidth, 60,
        settings_scene::makeText("라이브러리 다시 구축", 22, ui_theme::textPrimary()),
        ui_theme::lime());
    row.addView(second);
    assert(second->getY() >= button->getY() + button->getHeight() + 10);
    fits();
  }
  rendering::UniformCache::getInstance().destroyAll();
  rendering::ShaderManager::getInstance().release();
  bgfx::shutdown();
}
