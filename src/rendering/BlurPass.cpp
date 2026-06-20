#include "BlurPass.h"

#include "ShaderManager.h"
#include "UniformCache.h"
#include "common.h"

namespace rendering {
BlurPass::BlurPass(uint16_t downsampleFactor, float tintAlpha)
    : downsample_(downsampleFactor > 0 ? downsampleFactor : 1),
      tint_alpha_(tintAlpha), blur_view_h_(rendering::blur_view_h),
      blur_view_v_(rendering::blur_view_v), final_view_(rendering::final_view) {
}

void BlurPass::init(uint16_t windowW, uint16_t windowH) {
  window_width_ = windowW;
  window_height_ = windowH;
  prog_blur_h_ = ShaderManager::getInstance().getProgram("blur/vs_blur.bin",
                                                         "blur/fs_blurH.bin");
  prog_blur_v_ = ShaderManager::getInstance().getProgram("blur/vs_blur.bin",
                                                         "blur/fs_blurV.bin");
  prog_rect_ = ShaderManager::getInstance().getProgram("blur/vs_blur.bin",
                                                       "fs_rect_tint.bin");

  u_tex_color_ =
      rendering::UniformCache::getInstance().getSampler("s_texColor");
  if (!bgfx::isValid(u_texel_size_)) {
    u_texel_size_ = bgfx::createUniform("u_texelSize", bgfx::UniformType::Vec4);
  }
  if (!bgfx::isValid(u_tint_color_)) {
    u_tint_color_ = bgfx::createUniform("u_tintColor", bgfx::UniformType::Vec4);
  }
  if (!bgfx::isValid(u_blur_scale_)) {
    u_blur_scale_ = bgfx::createUniform("u_blurScale", bgfx::UniformType::Vec4);
  }

  resize(windowW, windowH);
  initialized_ = true;
}

void BlurPass::resize(uint16_t windowW, uint16_t windowH) {
  window_width_ = windowW;
  window_height_ = windowH;
  destroyFrameBuffers();

  scene_width_ = windowW / downsample_;
  scene_height_ = windowH / downsample_;
  if (scene_width_ < 1)
    scene_width_ = 1;
  if (scene_height_ < 1)
    scene_height_ = 1;

  createFrameBuffers();
}

void BlurPass::execute() {
  if (!initialized_)
    return;
  blurHorizontal();
  blurVertical();
  if (composite_enabled_) {
    drawFinal();
  }
}

void BlurPass::shutdown() {
  destroyFrameBuffers();

  if (bgfx::isValid(u_texel_size_))
    bgfx::destroy(u_texel_size_);
  if (bgfx::isValid(u_tint_color_))
    bgfx::destroy(u_tint_color_);
  if (bgfx::isValid(u_blur_scale_))
    bgfx::destroy(u_blur_scale_);

  u_tex_color_ = BGFX_INVALID_HANDLE;
  u_texel_size_ = BGFX_INVALID_HANDLE;
  u_tint_color_ = BGFX_INVALID_HANDLE;
  u_blur_scale_ = BGFX_INVALID_HANDLE;

  initialized_ = false;
}

void BlurPass::setInputViews(const std::vector<bgfx::ViewId> &views) {
  input_views_ = views;
  if (!bgfx::isValid(fb_scene_))
    return;
  for (const auto view : input_views_) {
    bgfx::setViewFrameBuffer(view, fb_scene_);
  }
}

void BlurPass::setViewIds(bgfx::ViewId blurH, bgfx::ViewId blurV,
                          bgfx::ViewId finalView) {
  blur_view_h_ = blurH;
  blur_view_v_ = blurV;
  final_view_ = finalView;
}

void BlurPass::setCompositeEnabled(bool enabled) {
  composite_enabled_ = enabled;
}

void BlurPass::setDownsample(uint16_t downsampleFactor) {
  downsample_ = downsampleFactor > 0 ? downsampleFactor : 1;
  resize(window_width_, window_height_);
}

void BlurPass::setBlurStrength(float strength) {
  blur_strength_ = strength;
  if (blur_strength_ < 0.0f) {
    blur_strength_ = 0.0f;
  }
}

void BlurPass::setTintAlpha(float alpha) { tint_alpha_ = alpha; }

void BlurPass::createFrameBuffers() {
  tex_scene_color_ =
      bgfx::createTexture2D(scene_width_, scene_height_, false, 1,
                            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  fb_scene_ = bgfx::createFrameBuffer(1, &tex_scene_color_, false);

  tex_blur_a_ =
      bgfx::createTexture2D(scene_width_, scene_height_, false, 1,
                            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  fb_blur_a_ = bgfx::createFrameBuffer(1, &tex_blur_a_, false);

  tex_blur_b_ =
      bgfx::createTexture2D(scene_width_, scene_height_, false, 1,
                            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  fb_blur_b_ = bgfx::createFrameBuffer(1, &tex_blur_b_, false);

  for (const auto view : input_views_) {
    bgfx::setViewFrameBuffer(view, fb_scene_);
  }

  bgfx::setViewRect(blur_view_h_, 0, 0, scene_width_, scene_height_);
  bgfx::setViewRect(blur_view_v_, 0, 0, scene_width_, scene_height_);
}

void BlurPass::destroyFrameBuffers() {
  if (bgfx::isValid(fb_scene_))
    bgfx::destroy(fb_scene_);
  if (bgfx::isValid(fb_blur_a_))
    bgfx::destroy(fb_blur_a_);
  if (bgfx::isValid(fb_blur_b_))
    bgfx::destroy(fb_blur_b_);

  if (bgfx::isValid(tex_scene_color_))
    bgfx::destroy(tex_scene_color_);
  if (bgfx::isValid(tex_blur_a_))
    bgfx::destroy(tex_blur_a_);
  if (bgfx::isValid(tex_blur_b_))
    bgfx::destroy(tex_blur_b_);

  fb_scene_ = BGFX_INVALID_HANDLE;
  fb_blur_a_ = BGFX_INVALID_HANDLE;
  fb_blur_b_ = BGFX_INVALID_HANDLE;
  tex_scene_color_ = BGFX_INVALID_HANDLE;
  tex_blur_a_ = BGFX_INVALID_HANDLE;
  tex_blur_b_ = BGFX_INVALID_HANDLE;
}

void BlurPass::blurHorizontal() {
  bgfx::setViewFrameBuffer(blur_view_h_, fb_blur_a_);
  bgfx::setViewRect(blur_view_h_, 0, 0, scene_width_, scene_height_);

  bgfx::setTexture(0, u_tex_color_, tex_scene_color_);
  rendering::screenSpaceQuad();

  float texelSize[4];
  texelSize[0] = 1.0f / float(scene_width_);
  texelSize[1] = 1.0f / float(scene_height_);
  texelSize[2] = 0.0f;
  texelSize[3] = 0.0f;
  bgfx::setUniform(u_texel_size_, texelSize);
  float blurScale[4] = {blur_strength_, 0.0f, 0.0f, 0.0f};
  bgfx::setUniform(u_blur_scale_, blurScale);

  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
  bgfx::submit(blur_view_h_, prog_blur_h_);
}

void BlurPass::blurVertical() {
  bgfx::setViewFrameBuffer(blur_view_v_, fb_blur_b_);
  bgfx::setViewRect(blur_view_v_, 0, 0, scene_width_, scene_height_);

  bgfx::setTexture(0, u_tex_color_, tex_blur_a_);
  rendering::screenSpaceQuad();

  float texelSize[4];
  texelSize[0] = 1.0f / float(scene_width_);
  texelSize[1] = 1.0f / float(scene_height_);
  texelSize[2] = 0.0f;
  texelSize[3] = 0.0f;
  bgfx::setUniform(u_texel_size_, texelSize);
  float blurScale[4] = {blur_strength_, 0.0f, 0.0f, 0.0f};
  bgfx::setUniform(u_blur_scale_, blurScale);

  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
  bgfx::submit(blur_view_v_, prog_blur_v_);
}

void BlurPass::drawFinal() {
  bgfx::setViewFrameBuffer(final_view_, BGFX_INVALID_HANDLE);
  bgfx::setViewRect(final_view_, rendering::ui_offset_x, rendering::ui_offset_y,
                    rendering::ui_view_width, rendering::ui_view_height);

  bgfx::setTexture(0, u_tex_color_, tex_blur_b_);
  float tintColor[4] = {1.0f, 1.0f, 1.0f, tint_alpha_};
  bgfx::setUniform(u_tint_color_, tintColor);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
  rendering::screenSpaceQuad();
  bgfx::submit(final_view_, prog_rect_);
}
} // namespace rendering
