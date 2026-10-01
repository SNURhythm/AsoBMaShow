#include "scene/SettingsSceneShared.h"
#include "rendering/UniformCache.h"

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
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  assert(bgfx::init(init));
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
