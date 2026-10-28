#pragma once

// Private TextView implementation. SDL_ttf owns shaping; this engine only
// consumes its public draw operations and retains white coverage textures.
#include <SDL3_ttf/SDL_textengine.h>
#include <cstring>
#include "SdlTtfRuntime.h"
#include "View.h"

class TextGlyphAtlas {
  using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
  static constexpr int kPageSize = 512;
  static constexpr int kMaximumPages = 4;
  static constexpr int kMaximumGlyphs = 128;
  struct Glyph {
    bgfx::TextureHandle texture;
    SDL_Rect bounds;
  };
  struct Page {
    bgfx::TextureHandle texture;
    int x = 0, y = 0, rowHeight = 0;
  };
  struct FontAtlas {
    TTF_Font *font;
    Uint32 generation;
    std::vector<Page> pages;
    std::unordered_map<Uint32, Glyph> glyphs;

    explicit FontAtlas(TTF_Font *source)
        : font(source), generation(TTF_GetFontGeneration(source)) {
      // Allocate the page list before creating GPU resources, including on
      // allocation failure; later pushes cannot strand a texture handle.
      pages.reserve(kMaximumPages);
    }
    ~FontAtlas() {
      for (const auto &page : pages) bgfx::destroy(page.texture);
      if (!pages.empty()) rendering::UiBatchRenderer::notifyTextureChanged();
    }

    const Glyph *get(Uint32 index) {
      if (const auto it = glyphs.find(index); it != glyphs.end())
        return &it->second;
      if (glyphs.size() >= kMaximumGlyphs) return nullptr;
      TTF_ImageType type;
      Surface surface(TTF_GetGlyphImageForIndex(font, index, &type), SDL_DestroySurface);
      if (!surface || type != TTF_IMAGE_ALPHA) return nullptr;
      // Two transparent texels separate ink in neighboring slots. One is also
      // drawn around the glyph so bilinear filtering matches a string surface.
      const int width = surface->w + 2, height = surface->h + 2;
      if (width > kPageSize || height > kPageSize) return nullptr;
      Page *page = pages.empty() ? nullptr : &pages.back();
      if (page && page->x + width > kPageSize) {
        page->x = 0;
        page->y += page->rowHeight;
        page->rowHeight = 0;
      }
      if (!page || page->y + height > kPageSize) {
        if (pages.size() >= kMaximumPages) return nullptr;
        const auto texture = bgfx::createTexture2D(
            kPageSize, kPageSize, false, 1, bgfx::TextureFormat::BGRA8);
        if (!bgfx::isValid(texture)) return nullptr;
        const auto *memory = bgfx::alloc(kPageSize * kPageSize * 4);
        std::memset(memory->data, 0, memory->size);
        bgfx::updateTexture2D(texture, 0, 0, 0, 0, kPageSize, kPageSize, memory);
        pages.push_back({texture});
        page = &pages.back();
      }
      const SDL_Rect bounds{page->x + 1, page->y + 1, surface->w, surface->h};
      // Normalize channel byte order for bgfx, independently of host endianness.
      Surface converted(SDL_ConvertSurface(surface.get(), SDL_PIXELFORMAT_BGRA32),
                         SDL_DestroySurface);
      if (!converted) return nullptr;
      const auto *pixels = bgfx::alloc(bounds.w * bounds.h * 4);
      for (int row = 0; row < bounds.h; ++row)
        std::memcpy(pixels->data + row * bounds.w * 4,
                    static_cast<const Uint8 *>(converted->pixels) + row * converted->pitch,
                    bounds.w * 4);
      bgfx::updateTexture2D(page->texture, 0, 0, bounds.x, bounds.y,
                            bounds.w, bounds.h, pixels);
      page->x += width;
      page->rowHeight = std::max(page->rowHeight, height);
      rendering::UiBatchRenderer::notifyTextureChanged();
      return &glyphs.emplace(index, Glyph{page->texture, bounds}).first->second;
    }
  };

  struct Quad {
    Glyph glyph;
    SDL_Rect source, destination;
  };
  std::shared_ptr<FontAtlas> atlas_;
  TTF_Text *layout_ = nullptr;
  std::string text_;
  std::vector<Quad> quads_;
  int width_ = 0, height_ = 0;
  bool ready_ = false;

  static TTF_TextEngine *engine() {
    static TTF_TextEngine instance = [] {
      TTF_TextEngine result{};
      SDL_INIT_INTERFACE(&result);
      result.CreateText = [](void *, TTF_Text *text) -> bool {
        text->internal->engine_text = text;
        return true;
      };
      result.DestroyText = [](void *, TTF_Text *text) {
        text->internal->engine_text = nullptr;
      };
      return result;
    }();
    return &instance;
  }

  static std::shared_ptr<FontAtlas> acquire(TTF_Font *font) {
    // Weak entries never extend font/SDL_ttf/GPU lifetime. Fonts themselves
    // belong to this thread's TextView font cache.
    static thread_local std::unordered_map<TTF_Font *, std::weak_ptr<FontAtlas>> cache;
    std::erase_if(cache, [](const auto &entry) { return entry.second.expired(); });
    auto &entry = cache[font];
    auto atlas = entry.lock();
    if (!atlas || atlas->generation != TTF_GetFontGeneration(font)) {
      atlas = std::make_shared<FontAtlas>(font);
      entry = atlas;
    }
    return atlas;
  }

public:
  ~TextGlyphAtlas() {
    text_runtime::OperationGuard operation;
    if (layout_) TTF_DestroyText(layout_);
  }

  bool prepare(TTF_Font *font, const std::string &text) {
    text_runtime::OperationGuard operation;
    if (!font) return false;
    if (!atlas_ || atlas_->font != font ||
        atlas_->generation != TTF_GetFontGeneration(font)) {
      if (layout_) TTF_DestroyText(layout_);
      layout_ = nullptr;
      atlas_ = acquire(font);
      text_.clear();
    }
    if (text_ == text) return ready_;
    text_ = text;
    ready_ = false;
    quads_.clear();
    if (text.empty() || text.size() > 128 ||
        !std::all_of(text.begin(), text.end(), [font](unsigned char c) {
          return c >= 32 && c <= 126 && TTF_FontHasGlyph(font, c);
        }) || TTF_GetFontHinting(font) == TTF_HINTING_LIGHT_SUBPIXEL)
      return false;
    if (!layout_) layout_ = TTF_CreateText(engine(), font, text.c_str(), text.size());
    else if (!TTF_SetTextString(layout_, text.c_str(), text.size())) return false;
    if (!layout_ || !TTF_UpdateText(layout_)) return false;
    width_ = layout_->internal->w;
    height_ = layout_->internal->h;
    if (width_ <= 0 || height_ <= 0) return false;
    for (int i = 0; i < layout_->internal->num_ops; ++i) {
      const auto &op = layout_->internal->ops[i];
      if (op.cmd == TTF_DRAW_COMMAND_NOOP) continue;
      if (op.cmd != TTF_DRAW_COMMAND_COPY || op.copy.glyph_font != font)
        return false;
      const auto &copy = op.copy;
      // Sampling at a whole-string edge clamps to its outer texel. Atlas
      // gutters are transparent instead; keep that rare edge-touching case
      // on the surface path (for example a tightly cropped capital T).
      if (copy.dst.x == 0 || copy.dst.y == 0 ||
          copy.dst.x + copy.dst.w == width_ ||
          copy.dst.y + copy.dst.h == height_) return false;
      // The surface renderer combines overlapping coverage before blending.
      // Separate quads cannot reproduce that, so preserve its path here.
      for (const auto &quad : quads_) {
        const auto &other = quad.destination;
        if (copy.dst.x < other.x + other.w + 1 &&
            copy.dst.x + copy.dst.w + 1 > other.x &&
            copy.dst.y < other.y + other.h + 1 &&
            copy.dst.y + copy.dst.h + 1 > other.y) return false;
      }
      const Glyph *glyph = atlas_->get(copy.glyph_index);
      if (!glyph) return false;
      quads_.push_back({*glyph, copy.src, copy.dst});
    }
    ready_ = true;
    return true;
  }

  void render(RenderContext &context, const SDL_Rect &rect, SDL_Color color,
              bgfx::UniformHandle sampler) const {
    auto state = context.makeUiBatchState(
        rendering::ShaderManager::getInstance().getProgram("vs_text.bin", "fs_image_fade.bin"),
        BGFX_STATE_WRITE_RGB | BGFX_STATE_BLEND_ALPHA);
    state.sampler = sampler;
    state.samplerFlags = BGFX_SAMPLER_U_CLAMP | BGFX_SAMPLER_V_CLAMP;
    // SDL_ttf treats alpha zero as opaque for blended text surfaces.
    const float alpha = color.a == 0 ? 1.0f : color.a / 255.0f;
    auto &uniforms = rendering::UniformCache::getInstance();
    state.uniforms[0] = {uniforms.getVec4("u_imageFadeParams"), {0, 0, 0, 1 - alpha}};
    state.uniforms[1] = {uniforms.getVec4("u_imageScrimColor"),
                         {color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, 1}};
    state.uniformCount = 2;
    const float scaleX = static_cast<float>(rect.w) / width_;
    const float scaleY = static_cast<float>(rect.h) / height_;
    for (const auto &quad : quads_) {
      const auto &dst = quad.destination;
      const int x0 = std::max(0, dst.x - 1), y0 = std::max(0, dst.y - 1);
      const int x1 = std::min(width_, dst.x + dst.w + 1);
      const int y1 = std::min(height_, dst.y + dst.h + 1);
      const float u0 = float(quad.glyph.bounds.x + quad.source.x + x0 - dst.x) / kPageSize;
      const float v0 = float(quad.glyph.bounds.y + quad.source.y + y0 - dst.y) / kPageSize;
      const float u1 = float(quad.glyph.bounds.x + quad.source.x + x1 - dst.x) / kPageSize;
      const float v1 = float(quad.glyph.bounds.y + quad.source.y + y1 - dst.y) / kPageSize;
      const float left = rect.x + x0 * scaleX, top = rect.y + y0 * scaleY;
      const float right = rect.x + x1 * scaleX, bottom = rect.y + y1 * scaleY;
      const std::array vertices = {
          rendering::PosTexCoord0Vertex{left, top, 0, u0, v0},
          rendering::PosTexCoord0Vertex{right, top, 0, u1, v0},
          rendering::PosTexCoord0Vertex{right, bottom, 0, u1, v1},
          rendering::PosTexCoord0Vertex{left, bottom, 0, u0, v1}};
      constexpr std::array<uint16_t, 6> indices{0, 1, 2, 0, 2, 3};
      state.texture = quad.glyph.texture;
      context.appendUiTextured(vertices, indices, state);
    }
  }
};
