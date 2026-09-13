#include "rendering/UniformCache.h"
#include "view/SdlTtfRuntime.h"
#include "view/TextView.h"
#include "support/AllocationFailure.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <string>

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

class FontProbeView : public TextView {
public:
  using TextView::TextView;
  TTF_Font *primaryFont() const {
    assert(!fontFaces.empty());
    return fontFaces.front().font;
  }
};

void testConstructorRollback(const std::string &path) {
  for (const bool shareExisting : {false, true}) {
    std::unique_ptr<FontProbeView> survivor;
    if (shareExisting) survivor = std::make_unique<FontProbeView>(path, 16);
    const auto baseline = text_runtime::activeReferencesForTesting();
    const auto construct = [&] {
      auto view = std::make_unique<FontProbeView>(path, 16);
      assert(view->primaryFont());
      if (survivor) assert(view->primaryFont() == survivor->primaryFont());
    };
    construct();
    std::size_t failures = 0;
    for (; failures < 256; ++failures) {
      bool threw = false;
      {
        test_support::FailAllocationAfter failure(failures);
        try { construct(); }
        catch (const std::bad_alloc &) { threw = true; }
      }
      const auto references = text_runtime::activeReferencesForTesting();
      if (references != baseline) {
        std::cerr << "Constructor allocation " << failures << " retained "
                  << references - baseline << " runtime references\n";
        std::abort();
      }
      if (survivor) {
        text_runtime::OperationGuard operation;
        assert(TTF_FontHeight(survivor->primaryFont()) > 0);
      }
      construct();
      assert(text_runtime::activeReferencesForTesting() == baseline);
      if (!threw) break;
    }
    assert(failures > 0 && failures < 256);
    std::cout << (shareExisting ? "Shared" : "Fresh") << " font construction: "
              << failures << " allocation failures passed\n";
  }
  assert(text_runtime::activeReferencesForTesting() == 0);
}

int main() {
  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  assert(bgfx::init(init));

  const std::string path = ASOBMASHOW_SOURCE_DIR
      "/tests/fixtures/beatoraja_skin/resources/fixture.ttf";
  auto first = std::make_unique<FontProbeView>(path, 16);
  auto shared = std::make_unique<FontProbeView>(path, 16);
  auto larger = std::make_unique<FontProbeView>(path, 20);
  auto bold = std::make_unique<FontProbeView>(path, 16, TextView::FontWeight::Bold);
  assert(first->primaryFont() && first->primaryFont() == shared->primaryFont());
  assert(first->primaryFont() != larger->primaryFont());
  assert(first->primaryFont() != bold->primaryFont());
  assert(text_runtime::activeReferencesForTesting() == 4);

  {
    test_support::FailNextAllocation failure;
    first.reset();
  }
  assert(text_runtime::activeReferencesForTesting() == 3);
  {
    text_runtime::OperationGuard operation;
    assert(TTF_FontHeight(shared->primaryFont()) > 0);
  }
  for (auto *view : {&shared, &larger, &bold}) {
    test_support::FailNextAllocation failure;
    view->reset();
  }
  assert(text_runtime::activeReferencesForTesting() == 0);
  assert(TTF_WasInit() == 0);
  testConstructorRollback(path);
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
}
