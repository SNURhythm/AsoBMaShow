#include "scene/SettingsSceneShared.h"
#include "rendering/UniformCache.h"

#include <cassert>
#include <memory>

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

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  assert(bgfx::init(init));
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
  bgfx::shutdown();
}
