//
// Created by XF on 8/28/2024.
//

#include "ImageView.h"
#include "../rendering/common.h"
#include "../rendering/ShaderManager.h"
#include "../rendering/UniformCache.h"
#include <bgfx/bgfx.h>
#ifdef _WIN32
#define STBI_WINDOWS_UTF8
#endif
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <vector>
std::map<std::string, ImageView::ImageCache> ImageView::imageCache = {};
ImageView::ImageView(int x, int y, int width, int height, const path_t &path)
    : View(x, y, width, height) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
  loadTexture(path);
}
ImageView::~ImageView() { freeTexture(); }
bool ImageView::loadTexture(const path_t &path) {

  freeTexture();
  std::string utf8Path = path_t_to_utf8(path);
  unsigned char *data;
  int width, height, channels;
  if (imageCache.find(path_t_to_utf8(path)) != imageCache.end()) {
    auto &cache = imageCache[path_t_to_utf8(path)];
    data = cache.data;
    width = cache.width;
    height = cache.height;
  } else {
    data = stbi_load(utf8Path.c_str(), &width, &height, &channels, 4);
    if (!data) {
      SDL_Log("Failed to load image: %s", utf8Path.c_str());
      return false;
    }
    SDL_Log("Loaded image: %s; width: %d; height: %d", utf8Path.c_str(), width,
            height);
  }
  texture =
      bgfx::createTexture2D(width, height, false, 1, bgfx::TextureFormat::RGBA8,
                            0, bgfx::copy(data, width * height * 4));
  imageCache[utf8Path] = {width, height, data};

  return true;
}

void ImageView::freeTexture() {
  if (bgfx::isValid(texture)) {
    bgfx::destroy(texture);
  }
  texture = BGFX_INVALID_HANDLE;
}

bool ImageView::setImage(const path_t &path) { return loadTexture(path); }
void ImageView::freeImage() { freeTexture(); }
void ImageView::renderImpl(RenderContext &context) {
  if (!bgfx::isValid(texture)) {
    return;
  }
  // Submit a quad with the image texture
  bgfx::TransientVertexBuffer tvb{};
  bgfx::TransientIndexBuffer tib{};

  //  SDL_Log("Rendering video texture frame %d; time: %f", currentFrame,
  //  currentFrame / 30.0f);

  bgfx::allocTransientVertexBuffer(&tvb, 4,
                                   rendering::PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&tib, 6);
  auto *vertex = (rendering::PosTexCoord0Vertex *)tvb.data;

  // Define quad vertices
  vertex[0].x = getX();
  vertex[0].y = getY() + getHeight();
  vertex[0].z = 0.0f;
  vertex[0].u = 0.0f;
  vertex[0].v = 1.0f;
  vertex[1].x = getX() + getWidth();
  vertex[1].y = getY() + getHeight();
  vertex[1].z = 0.0f;
  vertex[1].u = 1.0f;
  vertex[1].v = 1.0f;
  vertex[2].x = getX();
  vertex[2].y = getY();
  vertex[2].z = 0.0f;
  vertex[2].u = 0.0f;
  vertex[2].v = 0.0f;
  vertex[3].x = getX() + getWidth();
  vertex[3].y = getY();
  vertex[3].z = 0.0f;
  vertex[3].u = 1.0f;
  vertex[3].v = 0.0f;

  // Define quad indices
  auto *indices = (uint16_t *)tib.data;
  indices[0] = 0;
  indices[1] = 1;
  indices[2] = 2;
  indices[3] = 1;
  indices[4] = 3;
  indices[5] = 2;
  bgfx::setVertexBuffer(0, &tvb);
  bgfx::setIndexBuffer(&tib);

  bgfx::setTexture(0, s_texColor, texture);
  rendering::setScissorUI(context.scissor.x, context.scissor.y,
                          context.scissor.width, context.scissor.height);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A |
                 BGFX_STATE_BLEND_ALPHA);
  static const bgfx::ProgramHandle kProgram =
      rendering::ShaderManager::getInstance().getProgram(SHADER_TEXT);
  bgfx::submit(rendering::ui_view, kProgram);
}
ImageView::ImageView(int x, int y, int width, int height)
    : View(x, y, width, height) {
  s_texColor = rendering::UniformCache::getInstance().getSampler("s_texColor");
}
void ImageView::dropCache(const path_t &path) {
  std::string utf8Path = path_t_to_utf8(path);
  if (imageCache.find(utf8Path) != imageCache.end()) {
    auto data = imageCache[utf8Path].data;
    imageCache.erase(utf8Path);
    stbi_image_free(data);
  }
}
void ImageView::dropAllCache() {
  std::vector<unsigned char *> toFree;
  for (auto &pair : imageCache) {
    toFree.push_back(pair.second.data);
  }
  // clear first to prevent race condition
  imageCache.clear();
  for (auto data : toFree) {
    stbi_image_free(data);
  }
}
