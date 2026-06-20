#include "UniformCache.h"

namespace rendering {
namespace {
struct UniformHandleGuard {
  explicit UniformHandleGuard(bgfx::UniformHandle handle) : handle(handle) {}
  UniformHandleGuard(const UniformHandleGuard &) = delete;
  UniformHandleGuard &operator=(const UniformHandleGuard &) = delete;
  ~UniformHandleGuard() {
    if (bgfx::isValid(handle)) {
      bgfx::destroy(handle);
    }
  }

  bgfx::UniformHandle get() const { return handle; }
  void release() { handle = BGFX_INVALID_HANDLE; }

private:
  bgfx::UniformHandle handle = BGFX_INVALID_HANDLE;
};
} // namespace

UniformCache &UniformCache::getInstance() {
  static UniformCache instance;
  return instance;
}

bgfx::UniformHandle UniformCache::getSampler(const std::string &name) {
  auto it = samplers_.find(name);
  if (it != samplers_.end()) {
    return it->second;
  }
  UniformHandleGuard handle(
      bgfx::createUniform(name.c_str(), bgfx::UniformType::Sampler));
  samplers_.emplace(name, handle.get());
  const bgfx::UniformHandle result = handle.get();
  handle.release();
  return result;
}

bgfx::UniformHandle UniformCache::getVec4(const std::string &name) {
  auto it = vec4s_.find(name);
  if (it != vec4s_.end()) {
    return it->second;
  }
  UniformHandleGuard handle(
      bgfx::createUniform(name.c_str(), bgfx::UniformType::Vec4));
  vec4s_.emplace(name, handle.get());
  const bgfx::UniformHandle result = handle.get();
  handle.release();
  return result;
}

void UniformCache::destroyAll() {
  for (auto &pair : samplers_) {
    if (bgfx::isValid(pair.second)) {
      bgfx::destroy(pair.second);
    }
  }
  samplers_.clear();
  for (auto &pair : vec4s_) {
    if (bgfx::isValid(pair.second)) {
      bgfx::destroy(pair.second);
    }
  }
  vec4s_.clear();
}
} // namespace rendering
