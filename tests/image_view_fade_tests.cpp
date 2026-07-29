#include "ArchiveFile.h"
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

bool sameColor(const Color &actual, const Color &expected) {
  return actual.r == expected.r && actual.g == expected.g &&
         actual.b == expected.b && actual.a == expected.a;
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
    require(!image.scrimColor().has_value(),
            "image starts without a readability scrim");

    image.setScrimColor(Color(3, 4, 5, 96));
    require(image.scrimColor().has_value() &&
                sameColor(*image.scrimColor(), Color(3, 4, 5, 96)),
            "image stores a fixed scrim independently of fade state");

    bool useLightScrim = false;
    image.setThemedScrimColor([&useLightScrim] {
      return useLightScrim ? Color(255, 255, 255, 168)
                           : Color(5, 10, 18, 144);
    });
    require(image.scrimColor().has_value() &&
                sameColor(*image.scrimColor(), Color(5, 10, 18, 144)),
            "themed scrim evaluates immediately");
    useLightScrim = true;
    image.propagateThemeChange();
    require(image.scrimColor().has_value() &&
                sameColor(*image.scrimColor(),
                          Color(255, 255, 255, 168)),
            "themed scrim reevaluates during theme propagation");

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

    image.clearScrimColor();
    require(!image.scrimColor().has_value(),
            "clearing scrim restores untreated image color");
  }

  {
    int cachePathNormalizations = 0;
    archive_file::setCachePathNormalizer(
        [&cachePathNormalizations](std::filesystem::path &) {
          ++cachePathNormalizations;
        });
    ImageView::dropAllCache();

    ImageView image(0, 0, 100, 50);
    image.setImageAsync(
        path_t("/definitely-missing/asobmashow-jacket-performance.png"),
        true);
    const int normalizationsAfterBinding = cachePathNormalizations;
    RenderContext renderContext;
    image.render(renderContext);
    image.render(renderContext);
    image.render(renderContext);

    archive_file::setCachePathNormalizer({});
    require(normalizationsAfterBinding == 1,
            "one async jacket binding derives one filesystem cache identity");
    require(cachePathNormalizations == normalizationsAfterBinding,
            "pending jacket polling reuses its cache identity without repeated "
            "filesystem metadata work");
  }

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
