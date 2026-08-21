#include "SkinMovieCatalog.h"

#include "SkinDestinationEvaluator.h"
#include "../../rendering/RenderPlan.h"
#include "../../rendering/SkinQuadBatchRenderer.h"
#include "../../rendering/common.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#if ASOBMASHOW_ENABLE_SKIN_MOVIE_DEVICE
#include "../../video/VideoFrameLayout.h"
#include "../../video/VideoPlayer.h"
#include "../../view/View.h"
#endif

namespace skin {
namespace {

std::atomic_uint64_t nextMaterializedMovieRoot{
    static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count())};

SkinDiagnostic movieDiagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

std::optional<std::string> configuredMoviePath(
    std::string_view authored,
    const BeatorajaSkinConfiguration &configuration,
    const SkinSafetyPolicy &safetyPolicy) {
  const ConfiguredFile *match = nullptr;
  for (const auto &file : configuration.orderedFiles) {
    if (!authored.starts_with(file.pattern)) {
      continue;
    }
    if (match != nullptr) {
      return std::nullopt;
    }
    match = &file;
  }
  if (match == nullptr) {
    return authored.find('*') == std::string_view::npos
               ? std::optional<std::string>(authored)
               : std::nullopt;
  }
  const std::size_t wildcard = authored.rfind('*');
  if (wildcard == std::string_view::npos ||
      authored.size() < match->pattern.size()) {
    return std::nullopt;
  }
  const std::size_t suffixSize = authored.size() - match->pattern.size();
  const std::size_t maximumPathBytes = skinResourceLimit(
      safetyPolicy, SkinPackagePolicy::maxPathBytes);
  if (wildcard > maximumPathBytes ||
      match->selectedValue.size() > maximumPathBytes - wildcard ||
      suffixSize >
          maximumPathBytes - wildcard - match->selectedValue.size()) {
    return std::nullopt;
  }
  std::string selected;
  selected.reserve(wildcard + match->selectedValue.size() + suffixSize);
  selected.append(authored, 0, wildcard);
  selected.append(match->selectedValue);
  selected.append(authored, match->pattern.size(), suffixSize);
  return selected;
}

struct ResolvedMovieDefinition {
  SkinMovieResource resource;
  std::string path;
};

std::vector<ResolvedMovieDefinition> resolveMovies(
    const SkinMoviePreparationInputs &input,
    std::vector<SkinDiagnostic> &diagnostics) {
  std::vector<ResolvedMovieDefinition> result;
  std::set<SkinResourceId> usedMovies;
  const std::set<SkinObjectId> disabled(
      input.model.disabledOptionalObjects.begin(),
      input.model.disabledOptionalObjects.end());
  for (const auto &object : input.model.model.objects) {
    if (disabled.contains(object.id)) {
      continue;
    }
    if (const auto *image = std::get_if<SkinImageObject>(&object.payload)) {
      for (const auto &state : image->orderedStates) {
        usedMovies.insert(state.resource);
      }
    }
  }
  for (const auto &definition : input.model.model.resources) {
    SkinMovieResource resource;
    if (const auto *movie = std::get_if<SkinMovieResource>(&definition)) {
      resource = *movie;
    } else if (const auto *image = std::get_if<SkinImageResource>(&definition)) {
      resource = {.id = image->id,
                  .virtualPath = image->virtualPath,
                  .authoredOrdinal = image->authoredOrdinal};
    } else {
      continue;
    }
    if (!usedMovies.contains(resource.id)) {
      continue;
    }
    const auto configured = configuredMoviePath(
        resource.virtualPath, input.configuration, input.safetyPolicy);
    if (!configured) {
      if (std::holds_alternative<SkinMovieResource>(definition)) {
        diagnostics.push_back(movieDiagnostic(
            "skin.movie.configuration_ambiguous",
            "movie resource has an invalid configured path"));
      }
      continue;
    }
    const auto candidate = input.fileSystem.resolveResourceCandidates(
        *configured, *configured);
    if (!candidate.normalizedVirtualPath) {
      if (std::holds_alternative<SkinMovieResource>(definition) ||
          skinResourcePathIsMovie(*configured)) {
        diagnostics.push_back(movieDiagnostic(
            "skin.movie.path_invalid", "movie resource is unavailable"));
      }
      continue;
    }
    if (!std::holds_alternative<SkinMovieResource>(definition) &&
        !skinResourcePathIsMovie(*candidate.normalizedVirtualPath)) {
      continue;
    }
    result.push_back(
        {.resource = std::move(resource),
         .path = std::move(*candidate.normalizedVirtualPath)});
  }
  return result;
}

} // namespace

SkinMovieCatalog::SkinMovieCatalog(std::shared_ptr<SkinMovieDevice> device)
    : device_(std::move(device)), owner_(std::this_thread::get_id()) {}

SkinMovieCatalog::~SkinMovieCatalog() {
  if (device_ &&
      (!device_->ownsCurrentThread() || owner_ != std::this_thread::get_id())) {
    std::terminate();
  }
  if (device_) {
    device_->discardFrame();
    for (const auto handle : ownedPlayers_) {
      device_->destroy(handle);
    }
  }
  if (!materializedRoot_.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(materializedRoot_, ignored);
  }
}

SkinMovieCatalogPreparationResult
SkinMovieCatalog::prepare(SkinMoviePreparationInputs input) {
  SkinMovieCatalogPreparationResult result;
  if (input.stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  auto definitions = resolveMovies(input, result.diagnostics);
  if (!result.diagnostics.empty()) {
    return result;
  }
  auto catalog = std::unique_ptr<SkinMovieCatalog>(
      new SkinMovieCatalog(std::move(input.device)));
  if (definitions.empty()) {
    result.catalog = std::move(catalog);
    return result;
  }
  if (definitions.size() > skinResourceLimit(
                               input.safetyPolicy,
                               SkinResourcePolicy::maximumResources)) {
    result.diagnostics.push_back(movieDiagnostic(
        "skin.movie.session_limit",
        "movie resource count exceeds the session resource budget"));
    return result;
  }
  if (!catalog->device_ || !catalog->device_->ownsCurrentThread()) {
    result.diagnostics.push_back(movieDiagnostic(
        "skin.movie.device_unavailable",
        "movie preparation requires an owner-thread movie device"));
    return result;
  }
  catalog->materializedRoot_ =
      std::filesystem::temp_directory_path() /
      ("asobmashow-skin-movies-" +
       std::to_string(nextMaterializedMovieRoot.fetch_add(
                          1, std::memory_order_relaxed) +
                      1));
  std::error_code directoryError;
  if (!std::filesystem::create_directory(catalog->materializedRoot_,
                                         directoryError) || directoryError) {
    result.diagnostics.push_back(movieDiagnostic(
        "skin.movie.materialize_failed",
        "movie materialization directory could not be created"));
    return result;
  }

  std::map<std::string, PreparedSkinMovie, std::less<>> unique;
  std::set<SkinResourceId> ids;
  std::size_t encodedBytes = 0;
  for (const auto &definition : definitions) {
    if (input.stop.stop_requested()) {
      result.cancelled = true;
      return result;
    }
    if (definition.resource.id == 0 ||
        !ids.insert(definition.resource.id).second) {
      result.diagnostics.push_back(movieDiagnostic(
          "skin.movie.identity_invalid",
          "movie resource IDs must be nonzero and unique"));
      return result;
    }
    if (const auto found = unique.find(definition.path);
        found != unique.end()) {
      PreparedSkinMovie alias = found->second;
      alias.resource = definition.resource;
      catalog->movies_.emplace(alias.resource.id, std::move(alias));
      continue;
    }
    const auto read = input.fileSystem.readResolvedResource(
        definition.path,
        skinResourceLimit(input.safetyPolicy,
                          SkinResourcePolicy::maximumEncodedBytes));
    if (input.stop.stop_requested()) {
      result.cancelled = true;
      return result;
    }
    if (read.failure) {
      result.diagnostics.push_back(movieDiagnostic(
          "skin.movie.read_failed", "movie source bytes could not be read"));
      return result;
    }
    const auto maximumSessionBytes = skinResourceLimit(
        input.safetyPolicy, SkinResourcePolicy::maximumSessionEncodedBytes);
    if (read.bytes.size() >
        maximumSessionBytes - std::min(encodedBytes, maximumSessionBytes)) {
      result.diagnostics.push_back(movieDiagnostic(
          "skin.movie.session_limit",
          "movie source bytes exceed the session resource budget"));
      return result;
    }
    encodedBytes += read.bytes.size();
    const auto extension =
        std::filesystem::path(definition.path).extension().string();
    const auto materialized =
        catalog->materializedRoot_ /
        (std::to_string(unique.size() + 1U) + extension);
    {
      std::ofstream output(materialized,
                           std::ios::binary | std::ios::trunc);
      if (!output ||
          (!read.bytes.empty() &&
           !output.write(reinterpret_cast<const char *>(read.bytes.data()),
                         static_cast<std::streamsize>(read.bytes.size())))) {
        result.diagnostics.push_back(movieDiagnostic(
            "skin.movie.materialize_failed",
            "movie source bytes could not be materialized"));
        return result;
      }
    }
    auto loaded = catalog->device_->load(materialized, input.stop);
    if (!loaded || !loaded->handle || loaded->width <= 0 ||
        loaded->height <= 0 || loaded->durationMillis < 0) {
      result.diagnostics.push_back(movieDiagnostic(
          "skin.movie.load_failed", "movie source could not be opened"));
      return result;
    }
    catalog->ownedPlayers_.push_back(loaded->handle);
    if (input.stop.stop_requested()) {
      result.cancelled = true;
      result.diagnostics.clear();
      return result;
    }
    PreparedSkinMovie prepared{.resource = definition.resource,
                               .handle = loaded->handle,
                               .width = loaded->width,
                               .height = loaded->height,
                               .durationMillis = loaded->durationMillis};
    unique.emplace(definition.path, prepared);
    catalog->movies_.emplace(prepared.resource.id, std::move(prepared));
  }
  result.catalog = std::move(catalog);
  return result;
}

#if ASOBMASHOW_ENABLE_SKIN_MOVIE_DEVICE
namespace {

class VideoPlayerSkinMovieDevice final : public SkinMovieDevice {
public:
  VideoPlayerSkinMovieDevice() : owner_(std::this_thread::get_id()) {}

  std::optional<SkinMovieLoadResult>
  load(const std::filesystem::path &path, std::stop_token stop) override {
    if (!ownsCurrentThread() || stop.stop_requested()) {
      return std::nullopt;
    }
    auto entry = std::make_unique<Entry>();
    entry->clock.start();
    entry->player = std::make_unique<VideoPlayer>(&entry->clock);
    std::atomic_bool cancelled{stop.stop_requested()};
    std::stop_callback cancellation(stop, [&] {
      cancelled.store(true, std::memory_order_relaxed);
    });
    if (!entry->player->loadVideo(path.string(), cancelled) || cancelled) {
      return std::nullopt;
    }
    entry->width = entry->player->getFrameWidth();
    entry->height = entry->player->getFrameHeight();
    entry->durationMillis =
        std::max<std::int64_t>(0, entry->player->getDurationMicros() / 1000);
    if (entry->width <= 0 || entry->height <= 0) {
      return std::nullopt;
    }
    const SkinMoviePlayerHandle handle{++nextHandle_};
    const auto result = SkinMovieLoadResult{.handle = handle,
                                            .width = entry->width,
                                            .height = entry->height,
                                            .durationMillis =
                                                entry->durationMillis};
    players_.emplace(handle, std::move(entry));
    return result;
  }

  void destroy(SkinMoviePlayerHandle handle) noexcept override {
    if (ownsCurrentThread()) {
      players_.erase(handle);
    }
  }

  bool ownsCurrentThread() const noexcept override {
    return owner_ == std::this_thread::get_id();
  }

  void beginFrame() noexcept override {
    plans_.clear();
    layouts_.reset();
    layoutRegistered_ = false;
  }

  SkinMovieFramePreparationResult
  prepareFrame(SkinMoviePlayerHandle handle, const SkinMovieCommand &command,
               const PlaySkinViewport &viewport) override {
    const auto found = players_.find(handle);
    if (found == players_.end()) {
      return {};
    }
    Entry &entry = *found->second;
    std::int64_t sourceMillis = std::max<std::int64_t>(0,
                                                       command.sourceTimeMillis);
    if (entry.durationMillis > 0) {
      sourceMillis %= entry.durationMillis;
    }
    if (!entry.started || sourceMillis < entry.lastSourceMillis) {
      entry.player->playFrom(sourceMillis * 1000);
      entry.started = true;
    } else {
      entry.clock.seek(sourceMillis * 1000);
    }
    entry.lastSourceMillis = sourceMillis;
    entry.player->update();

    const auto projected = projectSkinDestinationToUi(
        command.geometry,
        {.textureWidth = entry.width,
         .textureHeight = entry.height,
         .region = {.x = 0, .y = 0, .w = entry.width, .h = entry.height}},
        viewport);
    std::array<video::VideoQuadPoint, 4> destination{};
    std::array<video::VideoQuadPoint, 4> uvs{};
    for (std::size_t index = 0; index < destination.size(); ++index) {
      destination[index] = {
          .x = static_cast<float>(projected.vertices[index][0]),
          .y = static_cast<float>(projected.vertices[index][1])};
      uvs[index] = {.x = static_cast<float>(projected.normalizedUvs[index][0]),
                    .y = static_cast<float>(projected.normalizedUvs[index][1])};
    }
    const auto quad = video::makeEmbeddedYuvQuadLayout(
        destination, uvs,
        {.r = command.geometry.rgba[0],
         .g = command.geometry.rgba[1],
         .b = command.geometry.rgba[2],
         .a = command.geometry.rgba[3]});
    if (!quad) {
      return {};
    }
    std::optional<rendering::DrawableScissor> scissor;
    if (projected.clip) {
      scissor = rendering::toDrawableScissor(
          projected.clip->x, projected.clip->y, projected.clip->width,
          projected.clip->height);
    }
    auto submission = entry.player->prepareEmbeddedSubmission(
        *quad, rendering::skinBgfxState(command.state.blend), scissor);
    plans_.push_back({.entry = &entry, .submission = std::move(submission)});
    if (!plans_.back().submission) {
      return {.ready = true, .drawable = false};
    }
    const auto &layout = VideoPlayer::embeddedVertexLayout();
    if (!layoutRegistered_ && !layouts_.registerLayout(layout)) {
      return {};
    }
    layoutRegistered_ = true;
    const auto stride = static_cast<std::uint64_t>(layout.getStride());
    return {.ready = true,
            .drawable = true,
            .requirements = {.vertexBytes = stride * 4U,
                             .vertexAlignmentPadding = stride - 1U,
                             .indexCount = 6U}};
  }

  void discardFrame() noexcept override {
    plans_.clear();
    layouts_.reset();
  }

  void commitFrame() noexcept override {
    for (auto &plan : plans_) {
      if (plan.submission) {
        plan.entry->player->commitPreparedEmbedded(*plan.submission);
      }
    }
  }

  void submitPrepared(std::size_t index) noexcept override {
    if (index < plans_.size() && plans_[index].submission) {
      plans_[index].entry->player->submitPreparedEmbedded(
          rendering::ui_view, *plans_[index].submission);
    }
  }

private:
  struct Entry {
    Stopwatch clock;
    std::unique_ptr<VideoPlayer> player;
    int width = 0;
    int height = 0;
    std::int64_t durationMillis = 0;
    std::int64_t lastSourceMillis = 0;
    bool started = false;
  };
  struct Plan {
    Entry *entry = nullptr;
    std::optional<VideoPlayer::PreparedEmbeddedSubmission> submission;
  };

  std::thread::id owner_;
  std::uint64_t nextHandle_ = 0;
  std::map<SkinMoviePlayerHandle, std::unique_ptr<Entry>> players_;
  std::vector<Plan> plans_;
  rendering::BgfxVertexLayoutRegistration layouts_;
  bool layoutRegistered_ = false;
};

} // namespace

std::shared_ptr<SkinMovieDevice> createSkinMovieDevice() {
  return std::make_shared<VideoPlayerSkinMovieDevice>();
}
#endif

} // namespace skin
