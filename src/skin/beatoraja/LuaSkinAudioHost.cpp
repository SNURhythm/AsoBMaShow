#include "LuaSkinAudioHost.h"
#include "../package/SkinPathPolicy.h"

#include <utility>

namespace skin {
namespace {

SkinFileFailure audioFailure(std::string_view path,
                             std::string_view message) {
  return {.code = SkinFileError::IoError,
          .virtualPath = std::string(path),
          .message = std::string(message)};
}

} // namespace

LuaSkinAudioHost::LuaSkinAudioHost(
    LuaSkinFileSystem &fileSystem,
    std::shared_ptr<LuaSkinAudioBackend> backend, std::stop_token stop,
    LuaSkinAudioPolicy policy) noexcept
    : fileSystem_(&fileSystem), backend_(std::move(backend)), stop_(stop),
      policy_(policy) {}

LuaSkinAudioHost::~LuaSkinAudioHost() {
  if (!backend_) {
    return;
  }
  for (const auto &[path, identity] : loaded_) {
    (void)path;
    if (identity) {
      backend_->dispose(*identity);
    }
  }
}

LuaSkinAudioOperationResult
LuaSkinAudioHost::resolve(std::string_view authored,
                          std::filesystem::path &resolved) noexcept {
  try {
    // SkinLuaPathResolver resolves even a missing file (and the empty path)
    // before the audio driver decides whether loading succeeds.
    const auto result = fileSystem_->normalizeVirtualPath(authored, true);
    if (!result.normalizedVirtualPath) {
      return {.failure = result.failure};
    }
    resolved = pathFromUtf8(*result.normalizedVirtualPath);
    return {};
  } catch (...) {
    return {.failure = audioFailure(authored,
                                    "skin audio path resolution failed")};
  }
}

LuaSkinAudioHost::LoadedIdentity *
LuaSkinAudioHost::load(std::string_view authored,
                       LuaSkinAudioOperationResult &result) noexcept {
  std::filesystem::path resolved;
  result = resolve(authored, resolved);
  if (!result.ok()) {
    return nullptr;
  }
  try {
    if (auto found = loaded_.find(resolved); found != loaded_.end()) {
      return &found->second;
    }
    if (loaded_.size() >= policy_.maximumIdentities) {
      result.failure = audioFailure(authored,
                                    "skin audio identity quota exceeded");
      return nullptr;
    }
    auto [inserted, unused] =
        loaded_.emplace(std::move(resolved), LoadedIdentity{});
    (void)unused;
    // Render callbacks may address only identities declared or preloaded while
    // the configured document was prepared. Unknown render-time paths remain
    // cached misses so a callback never synchronously decodes audio.
    if (backend_ && !renderPhase_ && !stop_.stop_requested()) {
      inserted->second = backend_->load(inserted->first, stop_);
      if (inserted->second && !*inserted->second) {
        inserted->second.reset();
      }
    }
    return &inserted->second;
  } catch (...) {
    result.failure = audioFailure(authored, "skin audio identity allocation failed");
    return nullptr;
  }
}

LuaSkinAudioOperationResult
LuaSkinAudioHost::play(std::string_view path, float volume,
                       bool loop) noexcept {
  LuaSkinAudioOperationResult result;
  LoadedIdentity *identity = load(path, result);
  if (identity != nullptr && *identity && backend_) {
    backend_->play(**identity, backend_->systemVolume() * volume, loop);
  }
  return result;
}

LuaSkinAudioOperationResult
LuaSkinAudioHost::preload(std::string_view path) noexcept {
  LuaSkinAudioOperationResult result;
  LoadedIdentity *identity = load(path, result);
  if (identity != nullptr && *identity && backend_) {
    backend_->play(**identity, 0.0F, false);
  }
  return result;
}

LuaSkinAudioOperationResult
LuaSkinAudioHost::stop(std::string_view authored) noexcept {
  std::filesystem::path resolved;
  auto result = resolve(authored, resolved);
  if (!result.ok()) {
    return result;
  }
  const auto found = loaded_.find(resolved);
  if (found != loaded_.end() && found->second && backend_) {
    backend_->stop(*found->second);
  }
  return result;
}

LuaSkinAudioOperationResult
LuaSkinAudioHost::dispose(std::string_view authored) noexcept {
  std::filesystem::path resolved;
  auto result = resolve(authored, resolved);
  if (!result.ok()) {
    return result;
  }
  const auto found = loaded_.find(resolved);
  if (found == loaded_.end() || !found->second) {
    return result;
  }
  const LuaSkinAudioIdentity identity = *found->second;
  if (backend_) {
    backend_->dispose(identity);
  }
  loaded_.erase(found);
  return result;
}

} // namespace skin
