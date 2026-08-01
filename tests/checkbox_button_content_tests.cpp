#include "rendering/UniformCache.h"
#include "view/CheckboxButtonContent.h"
#include "view/IconText.h"
#include "view/TextView.h"

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

void expect(bool condition, const char *message) {
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
  expect(bgfx::init(init), "headless bgfx initializes for checkbox content");

  {
    CheckboxButtonContent content("Club Beat", 18, 17);
    content.setSize(220, 54);
    content.applyYogaLayout();

    expect(content.labelView()->getText() == "Club Beat",
           "checkbox keeps its label in a separate text view");
    expect(content.iconView()->primaryFontPath() ==
               ui_icons::kFontAwesomeSolidPath,
           "checkbox icon uses the FontAwesome solid font");
    expect(content.iconView()->getText() ==
               ui_icons::textForCodepoint(ui_icons::kSquare),
           "unchecked state uses FontAwesome square");
    expect(!content.checked(), "checkbox starts unchecked");

    content.setChecked(true);
    expect(content.checked(), "checkbox stores checked state");
    expect(content.iconView()->getText() ==
               ui_icons::textForCodepoint(ui_icons::kSquareCheck),
           "checked state uses FontAwesome square-check");
  }

  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
  return 0;
}
