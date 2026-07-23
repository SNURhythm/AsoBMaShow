#include "rendering/UniformCache.h"
#include "view/ImageView.h"

#include <bgfx/bgfx.h>

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
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for image fade state");

  {
    ImageView image(0, 0, 100, 50);
    require(!image.fade().has_value(), "image starts without a fade");

    image.setFade(ImageFadeDirection::RightToLeft, 2.0F);
    require(image.fade().has_value() &&
                image.fade()->direction == ImageFadeDirection::RightToLeft &&
                image.fade()->strength == 1.0F,
            "image stores direction and clamps high fade strength");

    image.setFade(ImageFadeDirection::TopToBottom, 0.25F);
    require(image.fade().has_value() &&
                image.fade()->direction == ImageFadeDirection::TopToBottom &&
                image.fade()->strength == 0.25F,
            "setting fade replaces direction and strength");

    image.clearFade();
    require(!image.fade().has_value(), "clear fade restores normal rendering");
  }

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
