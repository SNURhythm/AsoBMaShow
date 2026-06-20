//
// Created by XF on 12/15/2024.
//

#pragma once


#include "../path.h"
#include <memory>

class SpriteLoader {
  public:
    explicit SpriteLoader(const path_t& path);
    SpriteLoader() = delete;
    ~SpriteLoader();
    bool load();
    void unload();
    [[nodiscard]] bool isLoaded() const;
    [[nodiscard]] int getWidth() const;
    [[nodiscard]] int getHeight() const;
    [[nodiscard]] int getChannels() const;
    [[nodiscard]] unsigned char *getData() const;
    [[nodiscard]] unsigned char *crop(int x, int y, int w, int h) const;

private:
  struct ImageDataDeleter {
    void operator()(unsigned char *ptr) const;
  };
  using ImageDataPtr = std::unique_ptr<unsigned char, ImageDataDeleter>;

  int channels = 0;
  int width = 0;
  int height = 0;
  ImageDataPtr data;
  path_t path;

};
