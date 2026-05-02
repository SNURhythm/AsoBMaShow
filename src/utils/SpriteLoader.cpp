//
// Created by XF on 12/15/2024.
//

#include "SpriteLoader.h"
#include <stb_image.h>
#include <SDL2/SDL_error.h>
#include <SDL2/SDL_log.h>
#include <SDL2/SDL_stdinc.h>

SpriteLoader::SpriteLoader(const path_t& path) {
  if (path.empty()) {
    return;
  }
  this->path = path;
}

SpriteLoader::~SpriteLoader() {
  if (data) {
    free(data);
  }
}

bool SpriteLoader::load() {
  if (data) {
    SDL_Log("Image already loaded");
    return true;
  }
  const std::string utf8Path = path_t_to_utf8(path);
  constexpr int kRequestedChannels = 4;
  data = stbi_load(utf8Path.c_str(), &width, &height, &channels,
                   kRequestedChannels);
  if (!data) {
    SDL_Log("Failed to load image: %s", SDL_GetError());
    return false;
  }
  channels = kRequestedChannels;
  switch (channels) {
  case 1:
    SDL_Log("Image has 1 channel");
    break;
  case 2:
    SDL_Log("Image has 2 channels");
    break;
  case 3:
    SDL_Log("Image has 3 channels");
    break;
  case 4:
    SDL_Log("Image has 4 channels");
    break;
  default:
    SDL_Log("Image has %d channels", channels);
    break;
  }
  SDL_Log("Image dimensions: %d x %d", width, height);
  return true;
}

void SpriteLoader::unload() {
  if (data) {
    free(data);
    data = nullptr;
  }
}
bool SpriteLoader::isLoaded() const { return data != nullptr; }
int SpriteLoader::getWidth() const { return width; }
int SpriteLoader::getHeight() const { return height; }
int SpriteLoader::getChannels() const { return channels; }
unsigned char *SpriteLoader::getData() const { return data; }
unsigned char *SpriteLoader::crop(const int x, const int y, const int w,
                                  const int h) const {
  if (x < 0 || y < 0 || w < 0 || h < 0) {
    return nullptr;
  }
  if (x + w > width || y + h > height) {
    return nullptr;
  }
  auto *newData = static_cast<unsigned char *>(SDL_malloc(w * h * channels));
  if (!newData) {
    return nullptr;
  }
  for (int row = 0; row < h; ++row) {
    const unsigned char* src_ptr = data + ((y + row) * width + x) * channels;
    unsigned char* dst_ptr = newData + row * w * channels;
    SDL_memcpy(dst_ptr, src_ptr, w * channels);
  }
  return newData;
}
