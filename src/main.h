#pragma once
#include "context.h"
#include <bgfx/bgfx.h>
#include <cstdint>
int main(int argv, char **args);
int run();
void resetViewTransform(uint16_t bgaWidth, uint16_t bgaHeight,
                        bgfx::ViewId blurViewH, bgfx::ViewId blurViewV,
                        bgfx::ViewId finalView, const AppSettings &settings);
