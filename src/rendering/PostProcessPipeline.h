#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace rendering {
class PostProcessPass {
public:
  virtual ~PostProcessPass() = default;
  PostProcessPass() = default;
  PostProcessPass(const PostProcessPass &) = delete;
  PostProcessPass &operator=(const PostProcessPass &) = delete;
  PostProcessPass(PostProcessPass &&) = delete;
  PostProcessPass &operator=(PostProcessPass &&) = delete;
  virtual void init(uint16_t windowW, uint16_t windowH) = 0;
  virtual void resize(uint16_t windowW, uint16_t windowH) = 0;
  virtual void execute() = 0;
  virtual void shutdown() = 0;
  void setEnabled(bool enabled) { enabled_ = enabled; }
  bool isEnabled() const { return enabled_; }

private:
  bool enabled_ = true;
};

class BlurPass;

class PostProcessPipeline {
public:
  ~PostProcessPipeline();
  void init(uint16_t windowW, uint16_t windowH);
  void resize(uint16_t windowW, uint16_t windowH);
  void apply();
  void shutdown();

  BlurPass *addBlurPass(uint16_t downsampleFactor = 2, float tintAlpha = 0.6f);

private:
  uint16_t window_width_ = 1;
  uint16_t window_height_ = 1;
  bool initialized_ = false;
  std::vector<std::unique_ptr<PostProcessPass>> passes_;
};
} // namespace rendering
