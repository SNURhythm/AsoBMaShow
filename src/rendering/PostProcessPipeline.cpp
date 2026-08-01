#include "PostProcessPipeline.h"

#include "BlurPass.h"

namespace rendering {
PostProcessPipeline::~PostProcessPipeline() { shutdown(); }

void PostProcessPipeline::init(uint16_t windowW, uint16_t windowH) {
  window_width_ = windowW;
  window_height_ = windowH;
  initialized_ = true;
}

void PostProcessPipeline::resize(uint16_t windowW, uint16_t windowH) {
  window_width_ = windowW;
  window_height_ = windowH;
  for (auto &pass : passes_) {
    pass->resize(windowW, windowH);
  }
}

void PostProcessPipeline::apply() {
  for (auto &pass : passes_) {
    if (pass->isEnabled()) {
      pass->execute();
    }
  }
}

void PostProcessPipeline::shutdown() {
  for (auto &pass : passes_) {
    pass->shutdown();
  }
  passes_.clear();
  initialized_ = false;
}

BlurPass *PostProcessPipeline::addBlurPass(uint16_t downsampleFactor,
                                           float tintAlpha) {
  auto pass = std::make_unique<BlurPass>(downsampleFactor, tintAlpha);
  BlurPass *passPtr = pass.get();
  if (initialized_) {
    pass->init(window_width_, window_height_);
  }
  passes_.push_back(std::move(pass));
  return passPtr;
}
} // namespace rendering
