#pragma once

#include "LuaSkinFileSystem.h"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string_view>

namespace skin {

struct LuaSkinAudioIdentity {
  std::uint64_t value = 0;
  explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const LuaSkinAudioIdentity &) const = default;
};

// Finite application seam for the Beatoraja skin-audio surface. The Lua
// runtime never receives the application mixer or callback state.
class LuaSkinAudioBackend {
public:
  virtual ~LuaSkinAudioBackend() = default;
  [[nodiscard]] virtual float systemVolume() const noexcept = 0;
  [[nodiscard]] virtual std::optional<LuaSkinAudioIdentity>
  load(const std::filesystem::path &) noexcept = 0;
  virtual void play(LuaSkinAudioIdentity, float volume, bool loop) noexcept = 0;
  virtual void stop(LuaSkinAudioIdentity) noexcept = 0;
  virtual void dispose(LuaSkinAudioIdentity) noexcept = 0;
};

struct LuaSkinAudioOperationResult {
  std::optional<SkinFileFailure> failure;
  [[nodiscard]] bool ok() const noexcept { return !failure.has_value(); }
};

class LuaSkinAudioHost final {
public:
  LuaSkinAudioHost(LuaSkinFileSystem &,
                   std::shared_ptr<LuaSkinAudioBackend>) noexcept;
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

private:
  using LoadedIdentity = std::optional<LuaSkinAudioIdentity>;

  [[nodiscard]] LuaSkinAudioOperationResult
  resolve(std::string_view, std::filesystem::path &) noexcept;
  [[nodiscard]] LoadedIdentity *
  load(std::string_view, LuaSkinAudioOperationResult &) noexcept;

  LuaSkinFileSystem *fileSystem_ = nullptr;
  std::shared_ptr<LuaSkinAudioBackend> backend_;
  std::map<std::filesystem::path, LoadedIdentity> loaded_;
};

} // namespace skin
