#pragma once

#include "LuaSkinFileSystem.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace skin {

struct LuaSkinAudioIdentity {
  std::uint64_t value = 0;
  explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const LuaSkinAudioIdentity &) const = default;
};

struct LuaSkinAudioActivityCounters {
  std::uint64_t loadAttempts = 0;
  std::uint64_t loadsSucceeded = 0;
  std::uint64_t liveIdentities = 0;
};

// Finite application seam for the Beatoraja skin-audio surface. The Lua
// runtime never receives the application mixer or callback state.
class LuaSkinAudioBackend {
public:
  virtual ~LuaSkinAudioBackend() = default;
  [[nodiscard]] virtual float systemVolume() const noexcept = 0;
  [[nodiscard]] virtual std::optional<LuaSkinAudioIdentity>
  load(const std::filesystem::path &, std::stop_token) noexcept = 0;
  virtual void play(LuaSkinAudioIdentity, float volume, bool loop) noexcept = 0;
  virtual void stop(LuaSkinAudioIdentity) noexcept = 0;
  virtual void dispose(LuaSkinAudioIdentity) noexcept = 0;
  [[nodiscard]] virtual LuaSkinAudioActivityCounters
  activityCounters() const noexcept {
    return {};
  }
};

struct LuaSkinAudioOperationResult {
  std::optional<SkinFileFailure> failure;
  [[nodiscard]] bool ok() const noexcept { return !failure.has_value(); }
};

struct LuaSkinAudioPolicy {
  std::size_t maximumIdentities = 256;
  bool allowRenderPhaseLoads = false;
};

class LuaSkinAudioHost final {
public:
  LuaSkinAudioHost(LuaSkinFileSystem &,
                   std::shared_ptr<LuaSkinAudioBackend>,
                   std::stop_token = {}, LuaSkinAudioPolicy = {}) noexcept;
  ~LuaSkinAudioHost();

  LuaSkinAudioHost(const LuaSkinAudioHost &) = delete;
  LuaSkinAudioHost &operator=(const LuaSkinAudioHost &) = delete;

  [[nodiscard]] LuaSkinAudioOperationResult
  play(std::string_view path, float volume, bool loop) noexcept;
  [[nodiscard]] LuaSkinAudioOperationResult
  preload(std::string_view path) noexcept;
  [[nodiscard]] LuaSkinAudioOperationResult
  stop(std::string_view path) noexcept;
  [[nodiscard]] LuaSkinAudioOperationResult
  dispose(std::string_view path) noexcept;
  void suspend() noexcept;
  void resume() noexcept;
  void enterRenderPhase() noexcept { renderPhase_ = true; }

private:
  using LoadedIdentity = std::optional<LuaSkinAudioIdentity>;
  struct ActivePlayback {
    float volume = 0.0F;
    bool loop = false;
  };

  [[nodiscard]] LuaSkinAudioOperationResult
  resolve(std::string_view, std::filesystem::path &) noexcept;
  [[nodiscard]] LoadedIdentity *
  load(std::string_view, LuaSkinAudioOperationResult &) noexcept;

  LuaSkinFileSystem *fileSystem_ = nullptr;
  std::shared_ptr<LuaSkinAudioBackend> backend_;
  std::stop_token stop_;
  LuaSkinAudioPolicy policy_;
  bool renderPhase_ = false;
  bool suspended_ = false;
  std::map<std::filesystem::path, LoadedIdentity> loaded_;
  std::map<std::filesystem::path, ActivePlayback> active_;
};

} // namespace skin
