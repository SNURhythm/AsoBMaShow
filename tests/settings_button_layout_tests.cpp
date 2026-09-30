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
