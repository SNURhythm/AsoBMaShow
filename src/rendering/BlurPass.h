#pragma once

#include <bgfx/bgfx.h>
#include <cstdint>
#include <vector>

#include "PostProcessPipeline.h"

namespace rendering {
class BlurPass final : public PostProcessPass {
public:
  BlurPass(uint16_t downsampleFactor, float tintAlpha);
  ~BlurPass() override;
  void init(uint16_t windowW, uint16_t windowH) override;
  void resize(uint16_t windowW, uint16_t windowH) override;
  void execute() override;
  void shutdown() override;

  void setInputViews(const std::vector<bgfx::ViewId> &views);
  void setViewIds(bgfx::ViewId blurH, bgfx::ViewId blurV,
                  bgfx::ViewId finalView);
  void setCompositeEnabled(bool enabled);
  void setDownsample(uint16_t downsampleFactor);
  void setBlurStrength(float strength);
  void setTintAlpha(float alpha);

  uint16_t sceneWidth() const { return scene_width_; }
  uint16_t sceneHeight() const { return scene_height_; }
  bgfx::ViewId blurViewH() const { return blur_view_h_; }
  bgfx::ViewId blurViewV() const { return blur_view_v_; }
  bgfx::ViewId finalView() const { return final_view_; }
  bgfx::TextureHandle sceneTexture() const { return tex_scene_color_; }
  bgfx::TextureHandle outputTexture() const { return tex_blur_b_; }

private:
  void createFrameBuffers();
  void destroyFrameBuffers();
  void blurHorizontal();
  void blurVertical();
  void drawFinal();

  uint16_t downsample_ = 2;
  float tint_alpha_ = 0.6f;
  float blur_strength_ = 1.0f;
  uint16_t window_width_ = 1;
  uint16_t window_height_ = 1;
  uint16_t scene_width_ = 1;
  uint16_t scene_height_ = 1;
  bool initialized_ = false;
  std::vector<bgfx::ViewId> input_views_{};
  bgfx::ViewId blur_view_h_ = 0;
  bgfx::ViewId blur_view_v_ = 0;
  bgfx::ViewId final_view_ = 0;
  bool composite_enabled_ = true;

  bgfx::ProgramHandle prog_blur_h_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle prog_blur_v_ = BGFX_INVALID_HANDLE;
  bgfx::ProgramHandle prog_rect_ = BGFX_INVALID_HANDLE;

  bgfx::UniformHandle u_tex_color_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_texel_size_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_tint_color_ = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_blur_scale_ = BGFX_INVALID_HANDLE;

  bgfx::TextureHandle tex_scene_color_ = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle fb_scene_ = BGFX_INVALID_HANDLE;

  bgfx::TextureHandle tex_blur_a_ = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle fb_blur_a_ = BGFX_INVALID_HANDLE;

  bgfx::TextureHandle tex_blur_b_ = BGFX_INVALID_HANDLE;
  bgfx::FrameBufferHandle fb_blur_b_ = BGFX_INVALID_HANDLE;
};
} // namespace rendering
