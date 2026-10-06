#include "rendering/UniformCache.h"
#include "view/SdlTtfRuntime.h"
#include "view/FontCacheSession.h"
#include "view/TextView.h"
#include "support/AllocationFailure.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sstream>
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
  TTF_Font *fontForGlyph(Uint32 codepoint) { return selectFont(codepoint).font; }
  TTF_Font *primaryFont() const {
    assert(!fontFaces.empty());
    return fontFaces.front().font;
  }
};

void testConstructorRollback(const std::string &path) {
  const auto initialReferences = text_runtime::activeReferencesForTesting();
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
  assert(text_runtime::activeReferencesForTesting() == initialReferences);
}

void testWarmFontCache(const std::string &path) {
  std::unique_ptr<FontProbeView> survivor;
  {
    text_runtime::FontCacheSession session;
    {
      FontProbeView first(path, 16);
      assert(TTF_GlyphIsProvided32(first.primaryFont(), 'A'));
    }
    const auto warm = text_runtime::fontCacheStatsForTesting();
    assert(warm.active == 0 && warm.idle == 1);
    {
      text_runtime::FontCacheSession nested;
      auto reused = std::make_unique<FontProbeView>(path, 16);
      assert(text_runtime::fontCacheStatsForTesting().opens == warm.opens);
      // Returning a font to the cache must not allocate, including on rollback.
      test_support::FailNextAllocation failure;
      reused.reset();
    }
    assert(text_runtime::fontCacheStatsForTesting().idle == 1);
    testConstructorRollback(path);
    {
      FontProbeView bold(path, 16, TextView::FontWeight::Bold);
    }
    const auto styled = text_runtime::fontCacheStatsForTesting().opens;
    {
      FontProbeView bold(path, 16, TextView::FontWeight::Bold);
      FontProbeView regular(path, 16);
      assert(bold.primaryFont() != regular.primaryFont());
      assert(text_runtime::fontCacheStatsForTesting().opens == styled);
    }
    survivor = std::make_unique<FontProbeView>(path, 16);
    for (int size = 20; size < 30; ++size) {
      FontProbeView other(path, size);
    }
    auto full = text_runtime::fontCacheStatsForTesting();
    assert(full.active == 1 && full.idle == 8);
    {
      FontProbeView recent(path, 22);
      FontProbeView pinned(path, 16);
      assert(pinned.primaryFont() == survivor->primaryFont());
      assert(text_runtime::fontCacheStatsForTesting().opens == full.opens);
    }
    {
      FontProbeView evicted(path, 20);
      assert(text_runtime::fontCacheStatsForTesting().opens == full.opens + 1);
    }
    {
      FontProbeView promoted(path, 22);
      assert(text_runtime::fontCacheStatsForTesting().opens == full.opens + 1);
    }
    {
      FontProbeView large(path, 100);
    }
    assert(text_runtime::fontCacheStatsForTesting().idle == 8);
  }
  // A view can outlive the session; only idle fonts are closed at teardown.
  assert(text_runtime::fontCacheStatsForTesting().idle == 0);
  assert(text_runtime::fontCacheStatsForTesting().active == 1);
  assert(TTF_FontHeight(survivor->primaryFont()) > 0);
  survivor.reset();
  assert(text_runtime::activeReferencesForTesting() == 0);
  assert(TTF_WasInit() == 0);
  {
    auto session = std::make_unique<text_runtime::FontCacheSession>();
    { FontProbeView cached(path, 16); }
    assert(text_runtime::fontCacheStatsForTesting().idle == 1);
    test_support::FailNextAllocation failure;
    session.reset();
  }
  assert(text_runtime::fontCacheStatsForTesting().idle == 0);
  assert(TTF_WasInit() == 0);
}

void testFullStaticFonts() {
  const std::string root = ASOBMASHOW_SOURCE_DIR "/assets/fonts/";
  FontProbeView regular(root + "notosanscjkjp.ttf", 22);
  FontProbeView bold(root + "notosanscjkjp.ttf", 22, TextView::FontWeight::Bold);
  FontProbeView sharedBold(root + "notosanscjkjp.ttf", 22, TextView::FontWeight::Bold);
  assert(regular.primaryFont() != bold.primaryFont());
  assert(bold.primaryFont() == sharedBold.primaryFont());
  assert(regular.primaryFontPath() == root + "notosanscjkjp.ttf");
  assert(bold.primaryFontPath() == root + "notosanscjkjp-bold.otf");
  assert(std::string(TTF_FontFaceStyleName(regular.primaryFont())) == "Regular");
  assert(std::string(TTF_FontFaceStyleName(bold.primaryFont())) == "Bold");

  // Preserve the complete legacy coverage, including dynamic chart text.
  std::ifstream coverage(ASOBMASHOW_SOURCE_DIR "/tests/fixtures/ui_font_legacy_coverage.txt");
  assert(coverage.is_open());
  std::string line;
  size_t legacyCharacters = 0;
  while (std::getline(coverage, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ranges(line);
    std::string range;
    while (ranges >> range) {
      const auto dash = range.find('-');
      const auto first = std::stoul(range.substr(0, dash), nullptr, 16);
      const auto last = std::stoul(range.substr(dash + 1), nullptr, 16);
      for (Uint32 codepoint = first; codepoint <= last; ++codepoint) {
        assert(TTF_GlyphIsProvided32(regular.primaryFont(), codepoint));
        assert(TTF_GlyphIsProvided32(bold.primaryFont(), codepoint));
        ++legacyCharacters;
      }
    }
  }
  assert(legacyCharacters == 28926);
  size_t mappedCharacters = 0;
  for (Uint32 codepoint = 32; codepoint <= 0x10ffff; ++codepoint) {
    const bool hasRegular = TTF_GlyphIsProvided32(regular.primaryFont(), codepoint) != 0;
    const bool hasBold = TTF_GlyphIsProvided32(bold.primaryFont(), codepoint) != 0;
    assert(hasRegular == hasBold);
    mappedCharacters += hasRegular;
  }
  assert(mappedCharacters == 44811);
  for (const auto &entry : {std::pair{&regular, "notosanscjkjp.ttf"},
                            std::pair{&bold, "notosanscjkjp-bold.otf"}}) {
    TTF_Font *reference = TTF_OpenFont((root + entry.second).c_str(), 44);
    assert(reference);
    for (const auto glyph : {U'A', U'힣', U'ア', U'龘', U'𠮷', U'≒'}) {
      assert(entry.first->fontForGlyph(glyph) == entry.first->primaryFont());
      assert(TTF_GlyphIsProvided32(entry.first->primaryFont(), glyph));
    }
    for (const char *text : {"Guided Access", "사용법 유도 힣", "アクセスガイド 龘𠮷≒"}) {
      SDL_Surface *actual = TTF_RenderUTF8_Blended(
          entry.first->primaryFont(), text, {255, 255, 255, 255});
      SDL_Surface *expected = TTF_RenderUTF8_Blended(reference, text, {255, 255, 255, 255});
      assert(actual && expected && actual->w == expected->w && actual->h == expected->h);
      for (int y = 0; y < actual->h; ++y) {
        assert(std::memcmp(static_cast<char *>(actual->pixels) + y * actual->pitch,
                           static_cast<char *>(expected->pixels) + y * expected->pitch,
                           actual->w * actual->format->BytesPerPixel) == 0);
      }
      SDL_FreeSurface(actual);
      SDL_FreeSurface(expected);
    }
    TTF_CloseFont(reference);
  }
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
  testFullStaticFonts();
  assert(text_runtime::activeReferencesForTesting() == 0);
  testConstructorRollback(path);
  testWarmFontCache(path);
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
}
