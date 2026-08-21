#pragma once

#include "BeatorajaSkinConfiguration.h"
#include "BeatorajaSkinModel.h"
#include "LuaSkinFileSystem.h"
#include "PlaySkinViewport.h"
#include "SkinDrawCommand.h"
#include "../../audio/GameplayBgaFrame.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <thread>
#include <vector>

namespace skin {

struct SkinMoviePlayerHandle {
  std::uint64_t value = 0;
  explicit operator bool() const noexcept { return value != 0; }
  auto operator<=>(const SkinMoviePlayerHandle &) const = default;
};

struct SkinMovieLoadResult {
  SkinMoviePlayerHandle handle;
  int width = 0;
  int height = 0;
  std::int64_t durationMillis = 0;
};

struct SkinMovieFramePreparationResult {
  bool ready = false;
  bool drawable = false;
  GameplayBgaTransientRequirements requirements;
};

class SkinMovieDevice {
public:
  virtual ~SkinMovieDevice() = default;
  virtual std::optional<SkinMovieLoadResult>
  load(const std::filesystem::path &, std::stop_token) = 0;
  virtual void destroy(SkinMoviePlayerHandle) noexcept = 0;
  virtual bool ownsCurrentThread() const noexcept = 0;
  virtual void beginFrame() noexcept = 0;
  virtual SkinMovieFramePreparationResult
  prepareFrame(SkinMoviePlayerHandle, const SkinMovieCommand &,
               const PlaySkinViewport &) = 0;
  virtual void discardFrame() noexcept = 0;
  virtual void commitFrame() noexcept = 0;
  virtual void submitPrepared(std::size_t) noexcept = 0;
};

struct PreparedSkinMovie {
  SkinMovieResource resource;
  SkinMoviePlayerHandle handle;
  int width = 0;
  int height = 0;
  std::int64_t durationMillis = 0;
};

class SkinPreparedMovieView {
public:
  virtual ~SkinPreparedMovieView() = default;
  virtual const PreparedSkinMovie *
  findMovie(SkinResourceId) const noexcept = 0;
};

struct SkinMoviePreparationInputs {
  const LuaSkinFileSystem &fileSystem;
  const ValidatedBeatorajaSkinModel &model;
  const BeatorajaSkinConfiguration &configuration;
  std::shared_ptr<SkinMovieDevice> device;
  SkinSafetyPolicy safetyPolicy{};
  std::stop_token stop;
};

class SkinMovieCatalog;

struct SkinMovieCatalogPreparationResult {
  std::unique_ptr<SkinMovieCatalog> catalog;
  bool cancelled = false;
  std::vector<SkinDiagnostic> diagnostics;
};

struct SkinMovieCatalogFrameResult {
  bool ready = false;
  GameplayBgaTransientRequirements requirements;
};

class SkinMovieCatalog final : public SkinPreparedMovieView {
public:
  static SkinMovieCatalogPreparationResult prepare(SkinMoviePreparationInputs);
  ~SkinMovieCatalog();

  SkinMovieCatalog(const SkinMovieCatalog &) = delete;
  SkinMovieCatalog &operator=(const SkinMovieCatalog &) = delete;

  const PreparedSkinMovie *
  findMovie(SkinResourceId) const noexcept override;
  SkinMovieCatalogFrameResult prepareFrame(
      std::span<const SkinMovieCommand *const>, const PlaySkinViewport &);
  void discardFrame() noexcept;
  void commitFrame() noexcept;
  void submitPrepared(std::size_t) noexcept;

private:
  explicit SkinMovieCatalog(std::shared_ptr<SkinMovieDevice>);

  std::shared_ptr<SkinMovieDevice> device_;
  std::thread::id owner_;
  std::filesystem::path materializedRoot_;
  std::map<SkinResourceId, PreparedSkinMovie> movies_;
  std::vector<SkinMoviePlayerHandle> ownedPlayers_;
  std::size_t preparedCount_ = 0;
};

[[nodiscard]] std::shared_ptr<SkinMovieDevice> createSkinMovieDevice();

} // namespace skin
