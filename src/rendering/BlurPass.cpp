#include "BlurPass.h"

#include "ShaderManager.h"
#include "UniformCache.h"
#include "common.h"
#include <algorithm>
#include <cmath>

namespace rendering {
BlurPass::BlurPass(uint16_t downsampleFactor, float tintAlpha)
    : downsample_(downsampleFactor > 0 ? downsampleFactor : 1),
      tint_alpha_(tintAlpha), blur_view_h_(rendering::blur_view_h),
      blur_view_v_(rendering::blur_view_v), final_view_(rendering::final_view) {
}

BlurPass::~BlurPass() { shutdown(); }

void BlurPass::init(uint16_t windowW, uint16_t windowH) {
  window_width_ = windowW;
  window_height_ = windowH;
  prog_blur_h_ = ShaderManager::getInstance().getProgram("blur/vs_blur.bin",
                                                         "blur/fs_blurH.bin");
  prog_blur_v_ = ShaderManager::getInstance().getProgram("blur/vs_blur.bin",
                                                         "blur/fs_blurV.bin");
  prog_rect_ = ShaderManager::getInstance().getProgram("blur/vs_blur.bin",
                                                       "fs_rect_tint.bin");
  prog_output_ = ShaderManager::getInstance().getProgram("vs_text.bin",
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
  (void)executeComplete();
}

bool BlurPass::prepareOutput() {
  if (!initialized_ || !std::isfinite(blur_strength_)) return false;
  if (!bgfx::isValid(fb_scene_) || !bgfx::isValid(tex_scene_color_)) {
    createFrameBuffers();
  }
  if (!bgfx::isValid(fb_scene_) || !bgfx::isValid(tex_scene_color_) ||
      !bgfx::isValid(u_tex_color_)) return false;
  if (blur_strength_ != 0.0f) {
    if (!bgfx::isValid(fb_blur_a_) || !bgfx::isValid(fb_blur_b_)) {
      createBlurFrameBuffers();
    }
    if (!bgfx::isValid(fb_blur_a_) || !bgfx::isValid(fb_blur_b_)) {
      return false;
    }
    if (!bgfx::isValid(prog_blur_h_) || !bgfx::isValid(prog_blur_v_) ||
        !bgfx::isValid(u_texel_size_) || !bgfx::isValid(u_blur_scale_)) return false;
  }
  return true;
}

bool BlurPass::executeComplete() {
  if (!prepareOutput()) return false;
  const std::uint32_t quads = (blur_strength_ != 0.0F ? 2U : 0U) +
      (composite_enabled_ ? 1U : 0U);
  if ((composite_enabled_ && (!bgfx::isValid(prog_rect_) || !bgfx::isValid(u_tint_color_))) ||
      (quads != 0 &&
       (bgfx::getAvailTransientVertexBuffer(quads * 4, PosTexCoord0Vertex::ms_decl) < quads * 4 ||
        bgfx::getAvailTransientIndexBuffer(quads * 6) < quads * 6))) return false;
  if (blur_strength_ != 0.0f) {
    blurHorizontal();
    blurVertical();
  }
  if (composite_enabled_) {
    drawFinal();
  }
  return true;
}

bool BlurPass::drawOutput(float brightness) {
  if (!initialized_ || !std::isfinite(brightness) ||
      rendering::window_width <= 0 || rendering::window_height <= 0 ||
      !bgfx::isValid(outputTexture()) || !bgfx::isValid(prog_output_) ||
      !bgfx::isValid(u_tex_color_) || !bgfx::isValid(u_tint_color_) ||
      bgfx::getAvailTransientVertexBuffer(4, PosTexCoord0Vertex::ms_decl) < 4 ||
      bgfx::getAvailTransientIndexBuffer(6) < 6) return false;
  bgfx::TransientVertexBuffer vertices{};
  bgfx::TransientIndexBuffer indices{};
  bgfx::allocTransientVertexBuffer(&vertices, 4, PosTexCoord0Vertex::ms_decl);
  bgfx::allocTransientIndexBuffer(&indices, 6);
  auto *quad = reinterpret_cast<PosTexCoord0Vertex *>(vertices.data);
  const float width = float(rendering::window_width), height = float(rendering::window_height);
  quad[0] = {0, height, 0, 0, 1};
  quad[1] = {width, height, 0, 1, 1};
  quad[2] = {0, 0, 0, 0, 0};
  quad[3] = {width, 0, 0, 1, 0};
  const std::uint16_t order[] = {0, 1, 2, 1, 3, 2};
  std::copy_n(order, 6, reinterpret_cast<std::uint16_t *>(indices.data));
  bgfx::setVertexBuffer(0, &vertices);
  bgfx::setIndexBuffer(&indices);
  bgfx::setTexture(0, u_tex_color_, outputTexture());
  const float value = std::clamp(brightness, 0.0F, 1.0F);
  const float tint[4] = {value, value, value, 1.0F};
  bgfx::setUniform(u_tint_color_, tint);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A | BGFX_STATE_BLEND_ALPHA);
  bgfx::submit(final_view_, prog_output_);
  return true;
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
  // Also called after reset/export to restore bindings at the same size.
  target_generation_ = nextContentIdentity();
  input_views_ = views;
  if (!bgfx::isValid(fb_scene_))
    return;
  for (const auto view : input_views_) {
    bgfx::setViewFrameBuffer(view, fb_scene_);
  }
}

void BlurPass::setViewIds(bgfx::ViewId blurH, bgfx::ViewId blurV,
                          bgfx::ViewId finalView) {
  target_generation_ = nextContentIdentity();
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
  strength = std::isfinite(strength) ? std::max(0.0F, strength) : 0.0F;
  if (strength == blur_strength_) return;
  target_generation_ = nextContentIdentity();
  blur_strength_ = strength;
  if (blur_strength_ == 0.0f) {
    destroyBlurFrameBuffers();
  }
}

void BlurPass::setTintAlpha(float alpha) { tint_alpha_ = alpha; }

void BlurPass::createFrameBuffers() {
  bgfx::TextureHandle sceneTexture =
      bgfx::createTexture2D(scene_width_, scene_height_, false, 1,
                            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  if (!bgfx::isValid(sceneTexture)) {
    return;
  }
  bgfx::FrameBufferHandle sceneFrameBuffer =
      bgfx::createFrameBuffer(1, &sceneTexture, false);
  if (!bgfx::isValid(sceneFrameBuffer)) {
    bgfx::destroy(sceneTexture);
    return;
  }
  tex_scene_color_ = sceneTexture;
  fb_scene_ = sceneFrameBuffer;
  target_generation_ = nextContentIdentity();
  for (const auto view : input_views_) {
    bgfx::setViewFrameBuffer(view, fb_scene_);
  }
}

void BlurPass::createBlurFrameBuffers() {
  auto destroyTarget = [](bgfx::TextureHandle &texture,
                          bgfx::FrameBufferHandle &frameBuffer) {
    if (bgfx::isValid(frameBuffer)) {
      bgfx::destroy(frameBuffer);
      frameBuffer = BGFX_INVALID_HANDLE;
    }
    if (bgfx::isValid(texture)) {
      bgfx::destroy(texture);
      texture = BGFX_INVALID_HANDLE;
    }
  };

  bgfx::TextureHandle blurATexture =
      bgfx::createTexture2D(scene_width_, scene_height_, false, 1,
                            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  if (!bgfx::isValid(blurATexture)) {
    return;
  }
  bgfx::FrameBufferHandle blurAFrameBuffer =
      bgfx::createFrameBuffer(1, &blurATexture, false);
  if (!bgfx::isValid(blurAFrameBuffer)) {
    destroyTarget(blurATexture, blurAFrameBuffer);
    return;
  }

  bgfx::TextureHandle blurBTexture =
      bgfx::createTexture2D(scene_width_, scene_height_, false, 1,
                            bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
  if (!bgfx::isValid(blurBTexture)) {
    destroyTarget(blurATexture, blurAFrameBuffer);
    return;
  }
  bgfx::FrameBufferHandle blurBFrameBuffer =
      bgfx::createFrameBuffer(1, &blurBTexture, false);
  if (!bgfx::isValid(blurBFrameBuffer)) {
    destroyTarget(blurATexture, blurAFrameBuffer);
    destroyTarget(blurBTexture, blurBFrameBuffer);
    return;
  }

  tex_blur_a_ = blurATexture;
  fb_blur_a_ = blurAFrameBuffer;
  tex_blur_b_ = blurBTexture;
  fb_blur_b_ = blurBFrameBuffer;
  target_generation_ = nextContentIdentity();

  bgfx::setViewRect(blur_view_h_, 0, 0, scene_width_, scene_height_);
  bgfx::setViewRect(blur_view_v_, 0, 0, scene_width_, scene_height_);
}

void BlurPass::destroyFrameBuffers() {
  target_generation_ = nextContentIdentity();
  if (bgfx::isValid(fb_scene_))
    bgfx::destroy(fb_scene_);
  if (bgfx::isValid(tex_scene_color_))
    bgfx::destroy(tex_scene_color_);
  fb_scene_ = BGFX_INVALID_HANDLE;
  tex_scene_color_ = BGFX_INVALID_HANDLE;
  destroyBlurFrameBuffers();
}

void BlurPass::destroyBlurFrameBuffers() {
  if (bgfx::isValid(fb_blur_a_))
    bgfx::destroy(fb_blur_a_);
  if (bgfx::isValid(fb_blur_b_))
    bgfx::destroy(fb_blur_b_);

  if (bgfx::isValid(tex_blur_a_))
    bgfx::destroy(tex_blur_a_);
  if (bgfx::isValid(tex_blur_b_))
    bgfx::destroy(tex_blur_b_);

  fb_blur_a_ = BGFX_INVALID_HANDLE;
  fb_blur_b_ = BGFX_INVALID_HANDLE;
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

  bgfx::setTexture(0, u_tex_color_, outputTexture());
  float tintColor[4] = {1.0f, 1.0f, 1.0f, tint_alpha_};
  bgfx::setUniform(u_tint_color_, tintColor);
  bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
  rendering::screenSpaceQuad();
  bgfx::submit(final_view_, prog_rect_);
}
} // namespace rendering
