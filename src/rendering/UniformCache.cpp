#include "UniformCache.h"
#include "BgfxRAII.h"

namespace rendering {

UniformCache &UniformCache::getInstance() {
  static UniformCache instance;
  return instance;
}

bgfx::UniformHandle UniformCache::getSampler(std::string_view name) {
  auto it = samplers_.find(name);
  if (it != samplers_.end()) {
    return it->second;
  }
  std::string ownedName(name);
  BgfxHandleGuard<bgfx::UniformHandle> handle(
      bgfx::createUniform(ownedName.c_str(), bgfx::UniformType::Sampler));
  samplers_.emplace(std::move(ownedName), handle.get());
  const bgfx::UniformHandle result = handle.get();
  handle.release();
  return result;
}

bgfx::UniformHandle UniformCache::getVec4(std::string_view name) {
  auto it = vec4s_.find(name);
  if (it != vec4s_.end()) {
    return it->second;
  }
  std::string ownedName(name);
  BgfxHandleGuard<bgfx::UniformHandle> handle(
      bgfx::createUniform(ownedName.c_str(), bgfx::UniformType::Vec4));
  vec4s_.emplace(std::move(ownedName), handle.get());
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
