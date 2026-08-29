#include "skin/beatoraja/PlaySkinSession.h"
#include "skin/beatoraja/ResultSkinSession.h"

#include "ArchiveFile.h"

#include "rendering/SkinQuadBatchRenderer.h"
#include "scene/play/PlayfieldPresentation.h"
#include "skin/SkinStoragePaths.h"
#include "skin/ResultSkinConfiguration.h"
#include "skin/SkinConfigurationWriteQueue.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#include "skin/beatoraja/GameplaySkinBuiltinCatalog.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinAudioHost.h"
#include "skin/beatoraja/PlaySkinViewport.h"
#include "skin/beatoraja/SkinModelValidator.h"
#include "skin/beatoraja/SyntheticReplayGhostOverlay.h"
#include "skin/beatoraja/SkinResourceCatalog.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinArchiveImporter.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"
#include "view/View.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace session_test_allocation_fault {
thread_local bool failNext = false;
}

void *operator new(std::size_t size) {
  if (session_test_allocation_fault::failNext) {
    session_test_allocation_fault::failNext = false;
    throw std::bad_alloc();
  }
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t size) { return ::operator new(size); }

void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

namespace rendering {
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void writeStoredZip(const fs::path &path,
                    const std::vector<std::pair<std::string,
                                                std::vector<unsigned char>>>
                        &members) {
  archive *writer = archive_write_new();
  expect(writer != nullptr && archive_write_set_format_zip(writer) == ARCHIVE_OK &&
             archive_write_set_options(writer, "zip:compression=store") ==
                 ARCHIVE_OK &&
             archive_write_open_filename(writer, path.string().c_str()) ==
                 ARCHIVE_OK,
         "chart resource ZIP opens");
  if (writer == nullptr) return;
  for (const auto &[name, bytes] : members) {
    archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, name.c_str());
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, 0644);
    archive_entry_set_size(entry, static_cast<la_int64_t>(bytes.size()));
    expect(archive_write_header(writer, entry) == ARCHIVE_OK &&
               archive_write_data(writer, bytes.data(), bytes.size()) ==
                   static_cast<la_ssize_t>(bytes.size()) &&
               archive_write_finish_entry(writer) == ARCHIVE_OK,
           "chart resource ZIP member writes");
    archive_entry_free(entry);
  }
  expect(archive_write_close(writer) == ARCHIVE_OK,
         "chart resource ZIP closes");
  archive_write_free(writer);
}

class TempDirectory final {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-play-skin-session-" + std::to_string(++serial));
    } while (!fs::create_directory(root_));
  }

  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }

  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class AcceptFiles final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class SessionResources final : public SkinPreparedResourceView {
public:
  void addImage(SkinResourceId id) {
    const SkinSourceRect region{.x = 0, .y = 0, .w = 10, .h = 10};
    PreparedSkinResource resource;
    resource.id = id;
    resource.texture = bgfx::TextureHandle{
        static_cast<std::uint16_t>(id == 0 ? 1 : id)};
    resource.width = 10;
    resource.height = 10;
    resource.regions = {region};
    resource.regionMappings = {{.authored = region, .resolved = region}};
    resources_.emplace(id, std::move(resource));
  }

  void addTextAtlas(SkinObjectId object, SkinTextAtlasId id) {
    PreparedSkinTextAtlas atlas;
    atlas.id = id;
    atlas.texture = bgfx::TextureHandle{static_cast<std::uint16_t>(id)};
    atlas.width = 32;
    atlas.height = 16;
    atlas.key.pointSize = 10;
    atlas.ascent = 8;
    atlas.capHeight = 8;
    atlas.descent = -2;
    atlas.lineHeight = 10;
    for (const auto [codepoint, x] :
         std::array<std::pair<char32_t, int>, 2>{{{U'A', 0}, {U'B', 8}}}) {
      atlas.glyphs.emplace(
          codepoint,
          SkinPreparedGlyphMetrics{.region = {.x = x,
                                               .y = 0,
                                               .w = 8,
                                               .h = 10},
                                   .bearingX = 0,
                                   .bearingY = 8,
                                   .advance = 8,
                                   .layoutOffsetY = -10});
    }
    textAtlasesByObject_.emplace(object, id);
    atlases_.emplace(id, std::move(atlas));
  }

  const PreparedSkinResource *find(SkinResourceId id) const noexcept override {
    const auto found = resources_.find(id);
    return found == resources_.end() ? nullptr : &found->second;
  }
  const SkinResolvedRegion *
  findResolvedRegion(SkinResourceId id,
                     const SkinSourceRect &region) const noexcept override {
    const auto *resource = find(id);
    if (resource == nullptr || resource->regionMappings.empty()) {
      return nullptr;
    }
    const auto &resolved = resource->regionMappings.front();
    return resolved.authored.x == region.x && resolved.authored.y == region.y &&
                   resolved.authored.w == region.w &&
                   resolved.authored.h == region.h
               ? &resolved
               : nullptr;
  }
  const PreparedSkinTextAtlas *
  findTextAtlas(SkinTextAtlasId id) const noexcept override {
    const auto found = atlases_.find(id);
    return found == atlases_.end() ? nullptr : &found->second;
  }
  const PreparedSkinTextAtlas *
  findTextAtlasForObject(SkinObjectId object) const noexcept override {
    const auto found = textAtlasesByObject_.find(object);
    return found == textAtlasesByObject_.end() ? nullptr
                                               : findTextAtlas(found->second);
  }

private:
  std::map<SkinResourceId, PreparedSkinResource> resources_;
  std::map<SkinTextAtlasId, PreparedSkinTextAtlas> atlases_;
  std::map<SkinObjectId, SkinTextAtlasId> textAtlasesByObject_;
};

class SessionQuadBackend final : public rendering::SkinQuadBatchBackend {
public:
  bool preflightVertexLayouts(
      std::span<const bgfx::VertexLayout *const> layouts) override {
    ++layoutPreflightCalls;
    layoutCount = layouts.size();
    return preflightReady;
  }

  bool preflightSamplers(std::span<const SkinFilterMode> samplers) override {
    ++samplerPreflightCalls;
    samplerCount = samplers.size();
    return preflightReady;
  }

  bool reserve(std::size_t vertexCount, std::size_t indexCount,
               std::size_t, const GameplayBgaTransientRequirements &) override {
    ++reserveCalls;
    reservedVertices = vertexCount;
    reservedIndices = indexCount;
    return preflightReady;
  }

  void submit(const rendering::SkinQuadBackendBatch &) override {
    ++submitCalls;
    if (failNextAllocationAfterSubmit) {
      session_test_allocation_fault::failNext = true;
    }
  }

  bool preflightReady = true;
  bool failNextAllocationAfterSubmit = false;
  std::size_t layoutPreflightCalls = 0;
  std::size_t samplerPreflightCalls = 0;
  std::size_t reserveCalls = 0;
  std::size_t submitCalls = 0;
  std::size_t layoutCount = 0;
  std::size_t samplerCount = 0;
  std::size_t reservedVertices = 0;
  std::size_t reservedIndices = 0;
};

bool sameBgaFrame(const PreparedGameplayBgaFrame &left,
                  const PreparedGameplayBgaFrame &right) {
  const auto sameSurface = [](const auto &first, const auto &second) {
    if (first.has_value() != second.has_value()) {
      return false;
    }
    return !first ||
           (first->role == second->role &&
            first->mediaKind == second->mediaKind &&
            first->surfaceToken == second->surfaceToken &&
            first->sourceWidth == second->sourceWidth &&
            first->sourceHeight == second->sourceHeight);
  };
  return left.sequence == right.sequence &&
         left.composition == right.composition &&
         sameSurface(left.base, right.base) &&
         sameSurface(left.layer, right.layer) &&
         sameSurface(left.miss, right.miss);
}

class SessionBgaSubmitter final : public IGameplayBgaSubmitter {
public:
  PreparedGameplayBgaFrame prepareVisualFrameAt(
      std::uint64_t, std::int64_t,
      const GameplayBgaMissState &) override {
    return {};
  }

  BgaPreflightResult
  preflight(const PreparedGameplayBgaFrame &frame,
            std::span<const BgaDrawTarget> targets) override {
    ++preflightCalls;
    preflightFrame = frame;
    targetCount = targets.size();
    return {.ready = preflightReady,
            .failure = preflightReady
                           ? std::nullopt
                           : std::optional<SkinDiagnostic>{SkinDiagnostic{
                                 .code = "session.bga.preflight",
                                 .message = "forced preflight failure"}},
            .requirements = preflightReady
                                ? GameplayBgaTransientRequirements{
                                      .vertexBytes = 64,
                                      .vertexAlignmentPadding = 8,
                                      .indexCount = 6}
                                : GameplayBgaTransientRequirements{}};
  }

  void commitPrepared(const PreparedGameplayBgaFrame &frame) noexcept override {
    ++commitCalls;
    committedFrame = frame;
  }

  void submitPrepared(const PreparedGameplayBgaFrame &frame,
                      const BgaDrawTarget &) noexcept override {
    ++submitCalls;
    submittedFrame = frame;
  }

  void finalizePrepared(
      const PreparedGameplayBgaFrame &frame) noexcept override {
    ++finalizeCalls;
    finalizedFrame = frame;
  }

  void submitFullscreen(
      const PreparedGameplayBgaFrame &) noexcept override {
    ++fullscreenCalls;
  }

  bool preflightReady = true;
  std::size_t preflightCalls = 0;
  std::size_t commitCalls = 0;
  std::size_t submitCalls = 0;
  std::size_t finalizeCalls = 0;
  std::size_t fullscreenCalls = 0;
  std::size_t targetCount = 0;
  PreparedGameplayBgaFrame preflightFrame;
  PreparedGameplayBgaFrame committedFrame;
  PreparedGameplayBgaFrame submittedFrame;
  PreparedGameplayBgaFrame finalizedFrame;
};

class SerialOnlyState final : public ISkinFrameState {
public:
  explicit SerialOnlyState(std::uint64_t serial) : serial_(serial) {}

  std::uint64_t frameSerial() const noexcept override { return serial_; }
  SkinPropertyLookup<bool>
  booleanProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<std::int64_t>
  integerProperty(const SkinBuiltinPropertySelector &,
                  SkinIntegerPropertyDomain) override {
    return {};
  }
  SkinPropertyLookup<double>
  floatProperty(const SkinBuiltinPropertySelector &,
                SkinFloatPropertyDomain) override {
    return {};
  }
  SkinPropertyLookup<std::string_view>
  stringProperty(const SkinBuiltinPropertySelector &) override {
    return {};
  }
  SkinPropertyLookup<SkinRuntimeOffset> offsetProperty(int) override {
    return {};
  }
  std::int64_t timerProperty(const SkinBuiltinPropertySelector &) override {
    return INT64_MIN;
  }
  std::span<const SkinProjectedNoteView>
  projectedNotes() const noexcept override {
    return {};
  }
  std::span<const SkinProjectedLongNoteView>
  projectedLongNotes() const noexcept override {
    return {};
  }
  std::span<const SkinProjectedLineView>
  projectedLines() const noexcept override {
    return {};
  }
  SkinGaugeStateView gaugeState() const noexcept override { return {}; }
  SkinJudgeStateView judgeState(int) const noexcept override { return {}; }
  SkinNoteExpansionStateView noteExpansionState() const noexcept override {
    return {};
  }

private:
  std::uint64_t serial_ = 0;
};

PlayfieldVisualState stateAt(std::uint64_t serial) {
  PlayfieldVisualState state;
  state.clock.serial = serial;
  state.clock.visualTimeMicros = static_cast<long long>(serial) * 10'000;
  state.clock.gameplayTimeMicros = state.clock.visualTimeMicros;
  return state;
}

PlayfieldProjectionResult projectionAt(std::uint64_t serial) {
  PlayfieldProjectionResult projection;
  projection.frameSerial = serial;
  return projection;
}

bool hasDiagnostic(std::span<const SkinDiagnostic> diagnostics,
                   std::string_view code) {
  return std::ranges::any_of(
      diagnostics,
      [code](const SkinDiagnostic &diagnostic) { return diagnostic.code == code; });
}

bool hasDiagnostic(const PlaySkinFrameTransactionResult &result,
                   std::string_view code) {
  return hasDiagnostic(result.diagnostics, code) ||
         hasDiagnostic(result.evaluation.diagnostics, code);
}

void testCallbackBindingWithoutRuntimeFailsValidation() {
  BeatorajaSkinModel model;
  model.header.type = 0;
  model.booleanProperties.push_back(
      {.id = SkinBooleanPropertyId{1},
       .source = LuaCallbackId{.slot = 1, .generation = 1},
       .authoredOrdinal = 1});
  SkinModelValidator validator;
  const auto result = validator.validate(
      std::move(model),
      {.builtins = gameplaySkinBuiltinCatalog(), .callbacks = std::nullopt});
  expect(!result.model && result.criticalFailure &&
             hasDiagnostic(result.diagnostics,
                           "skin.model.callback_runtime_missing"),
         "callback binding without a live Lua runtime fails closed");
}

PreparedGameplayBgaFrame bgaFrame(std::uint64_t sequence) {
  return {.sequence = sequence,
          .composition = GameplayBgaComposition::BaseThenLayer,
          .base = PreparedGameplayBgaSurface{
              .role = GameplayBgaRole::Base,
              .mediaKind = GameplayBgaMediaKind::Image,
              .surfaceToken = 1000 + sequence,
              .sourceWidth = 640,
              .sourceHeight = 360}};
}

SkinFrameMutation persisted(PersistedSkinConfigurationWrite write) {
  return SkinFrameMutation{std::move(write)};
}

class SessionTextureDevice final : public SkinTextureDevice {
public:
  SessionTextureDevice() : owner_(std::this_thread::get_id()) {}

  bgfx::TextureHandle
  create(const image_decode::DecodedImageData &image) override {
    ++createCalls;
    if (failNextCreate_) {
      failNextCreate_ = false;
      return BGFX_INVALID_HANDLE;
    }
    CreatedImage created{.width = image.width, .height = image.height};
    if (image.rgba) {
      const std::size_t retained = std::min<std::size_t>(8, image.rgba->size());
      created.firstPixels.assign(image.rgba->begin(),
                                 image.rgba->begin() + retained);
    }
    createdImages.push_back(std::move(created));
    if (!ownsCurrentThread()) {
      ++wrongThreadOperations;
    }
    if (stopAfterCreateCalls && createCalls == *stopAfterCreateCalls) {
      stopSource->request_stop();
    }
    return bgfx::TextureHandle{nextHandle_++};
  }

  void destroy(bgfx::TextureHandle) noexcept override {
    ++destroyCalls;
    if (!ownsCurrentThread()) {
      ++wrongThreadOperations;
    }
    if (observedRevision && !observedRevision->hasLiveLease()) {
      revisionLiveDuringDestroy = false;
    }
  }

  bool ownsCurrentThread() const noexcept override {
    return std::this_thread::get_id() == owner_;
  }

  void observeRevision(SkinRevisionWeakPin revision) {
    observedRevision = std::move(revision);
  }

  void requestStopAfter(std::size_t createCall, std::stop_source &source) {
    stopAfterCreateCalls = createCall;
    stopSource = &source;
  }

  void failNextCreate() noexcept { failNextCreate_ = true; }

  std::size_t createCalls = 0;
  std::size_t destroyCalls = 0;
  std::size_t wrongThreadOperations = 0;
  bool revisionLiveDuringDestroy = true;
  struct CreatedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> firstPixels;
  };
  std::vector<CreatedImage> createdImages;

private:
  std::thread::id owner_;
  std::uint16_t nextHandle_ = 1;
  std::optional<SkinRevisionWeakPin> observedRevision;
  std::optional<std::size_t> stopAfterCreateCalls;
  std::stop_source *stopSource = nullptr;
  bool failNextCreate_ = false;
};

class SessionMovieDevice final : public SkinMovieDevice {
public:
  std::optional<SkinMovieLoadResult>
  load(const fs::path &path, const SkinMovieLoadLimits &limits,
       std::stop_token) override {
    ++loadCalls;
    lastLimits = limits;
    loadedPaths.push_back(path);
    pathExistedDuringLoad = pathExistedDuringLoad && fs::is_regular_file(path);
    const auto layout = skinMovieDecodedLayout(80, 40, limits);
    if (!layout) {
      return std::nullopt;
    }
    const auto handle = SkinMoviePlayerHandle{++nextHandle_};
    live.push_back(handle);
    if (stopAfterLoad_ != nullptr) {
      stopAfterLoad_->request_stop();
    }
    return SkinMovieLoadResult{
        .handle = handle,
        .width = 80,
        .height = 40,
        .durationMillis = 1'000,
        .decodedBytes = layout->residentBytes};
  }

  void destroy(SkinMoviePlayerHandle handle) noexcept override {
    ++destroyCalls;
    const auto found = std::ranges::find(live, handle);
    if (found != live.end()) {
      live.erase(found);
    }
  }

  bool ownsCurrentThread() const noexcept override { return true; }
  void beginFrame() noexcept override { ++beginFrameCalls; }
  SkinMovieFramePreparationResult
  prepareFrame(SkinMoviePlayerHandle, const SkinMovieCommand &command,
               const PlaySkinViewport &) override {
    preparedTimes.push_back(command.sourceTimeMillis);
    return {.ready = true, .drawable = true};
  }
  void discardFrame() noexcept override { ++discardFrameCalls; }
  void commitFrame() noexcept override { ++commitFrameCalls; }
  void submitPrepared(std::size_t index) noexcept override {
    submittedIndices.push_back(index);
  }

  void stopAfterLoad(std::stop_source &source) noexcept {
    stopAfterLoad_ = &source;
  }

  std::size_t loadCalls = 0;
  std::size_t destroyCalls = 0;
  std::size_t beginFrameCalls = 0;
  std::size_t discardFrameCalls = 0;
  std::size_t commitFrameCalls = 0;
  SkinMovieLoadLimits lastLimits;
  bool pathExistedDuringLoad = true;
  std::vector<fs::path> loadedPaths;
  std::vector<SkinMoviePlayerHandle> live;
  std::vector<std::int64_t> preparedTimes;
  std::vector<std::size_t> submittedIndices;

private:
  std::uint64_t nextHandle_ = 0;
  std::stop_source *stopAfterLoad_ = nullptr;
};

struct SessionAudioState {
  std::vector<fs::path> loads;
  std::vector<LuaSkinAudioIdentity> plays;
  std::vector<LuaSkinAudioIdentity> stops;
  std::vector<LuaSkinAudioIdentity> disposals;
  bool backendDestroyed = false;
};

class SessionAudioBackend final : public LuaSkinAudioBackend {
public:
  SessionAudioBackend(std::shared_ptr<SessionAudioState> state,
                      std::shared_ptr<SkinLiveResourceCounters> counters)
      : state_(std::move(state)), counters_(std::move(counters)) {}
  ~SessionAudioBackend() override { state_->backendDestroyed = true; }

  float systemVolume() const noexcept override { return 0.4F; }
  std::optional<LuaSkinAudioIdentity>
  load(const fs::path &path, std::stop_token) noexcept override {
    state_->loads.push_back(path);
    const LuaSkinAudioIdentity identity{.value = ++nextIdentity_};
    live_.insert(identity);
    counters_->audioCreated(0);
    return identity;
  }
  void play(LuaSkinAudioIdentity identity, float, bool) noexcept override {
    state_->plays.push_back(identity);
  }
  void stop(LuaSkinAudioIdentity identity) noexcept override {
    state_->stops.push_back(identity);
  }
  void dispose(LuaSkinAudioIdentity identity) noexcept override {
    state_->disposals.push_back(identity);
    if (live_.erase(identity) != 0) counters_->audioDestroyed(0);
  }
  LuaSkinAudioActivityCounters activityCounters() const noexcept override {
    return {.loadAttempts = state_->loads.size(),
            .loadsSucceeded = state_->loads.size(),
            .liveIdentities = state_->loads.size() - state_->disposals.size()};
  }

private:
  std::shared_ptr<SessionAudioState> state_;
  std::shared_ptr<SkinLiveResourceCounters> counters_;
  std::set<LuaSkinAudioIdentity> live_;
  std::uint64_t nextIdentity_ = 0;
};

enum class MalformedPomyuNumeric {
  None,
  Anime,
  Frame,
  Size,
  Coordinate,
  Motion,
  Loop,
  FaceRectangle,
};

struct ActivationFixtureOptions {
  int skinType = 0;
  int configuredSkinType = -1;
  bool resourceBearing = false;
  bool movieBearing = false;
  bool audioBearing = false;
  bool requireConfiguredState = false;
  bool requireResultConfiguredState = false;
  bool resultEventExec = false;
  bool resultNestedEventExec = false;
  bool resultIntervalEventExec = false;
  bool resultRecursiveEventExec = false;
  bool resultDuplicateEventExec = false;
  bool resultDuplicateTimerExec = false;
  bool staticResultCustomEvent = false;
  bool legacyInputBearing = false;
  bool repeatedPomyu = false;
  bool oversizedPomyuWithSibling = false;
  bool pomyuMissingCharBmp = false;
  bool pomyuTextureMissingCharTex = false;
  bool pomyuCp932BackslashPath = false;
  bool pomyuRootedResourcePath = false;
  bool pomyuLeadingBackslashPath = false;
  bool pomyuSecondPlayerTextures = false;
  bool pomyuSecondPlayerTextureFallback = false;
  MalformedPomyuNumeric malformedPomyuNumeric = MalformedPomyuNumeric::None;
};

class ActivationFixture final {
public:
  explicit ActivationFixture(ActivationFixtureOptions options = {})
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("ActivationContract").package),
        entry_(*normalizeEntryPath(
            package_, options.staticResultCustomEvent ? "skin/main.json"
                                                     : "skin/main.luaskin")
                    .entry),
        profile_(*makeSkinProfileId(
            "66666666-6666-4666-8666-666666666666")),
        device_(std::make_shared<SessionTextureDevice>()),
        movieDevice_(std::make_shared<SessionMovieDevice>()),
        audioState_(std::make_shared<SessionAudioState>()) {
    audioBackend_ = std::make_shared<SessionAudioBackend>(
        audioState_, liveResourceCounters_);
    chart_.text.title = "Artist 日本 42";
    chart_.text.subtitle = "Session subtitle";
    chart_.text.artist = "Session artist";

    const fs::path source = temp_.root() / "source";
    if (options.resourceBearing) {
      fs::create_directories(source / "skin/resources");
      fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/resources/fixture.png",
                    source / "skin/resources/fixture.png");
      fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/resources/fixture.ttf",
                    source / "skin/resources/fixture.ttf");
    }
    if (options.movieBearing) {
      fs::create_directories(source / "skin/resources");
      fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                        "tests/fixtures/beatoraja_skin/resources/fixture.png",
                    source / "skin/resources/source.MP4");
    }
    const bool hasPomyu = options.repeatedPomyu ||
                          options.oversizedPomyuWithSibling ||
                          options.pomyuMissingCharBmp ||
                          options.pomyuTextureMissingCharTex ||
                          options.pomyuCp932BackslashPath ||
                          options.pomyuRootedResourcePath ||
                          options.pomyuLeadingBackslashPath ||
                          options.pomyuSecondPlayerTextures ||
                          options.pomyuSecondPlayerTextureFallback ||
                          options.malformedPomyuNumeric !=
                              MalformedPomyuNumeric::None;
    if (hasPomyu) {
      fs::create_directories(source / "skin/characters");
      if (options.pomyuSecondPlayerTextures ||
          options.pomyuSecondPlayerTextureFallback) {
        for (const std::string_view name : {
                 "primary.png", "second.png", "texture.png", "face.png",
                 "select.png"}) {
          fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                            "tests/fixtures/beatoraja_skin/resources/fixture.png",
                        source / "skin/characters" / name);
        }
        std::string chp =
            "#CharBMP\tprimary.png\n#CharBMP2P\tsecond.png\n"
            "#CharTex\ttexture.png\n";
        if (options.pomyuSecondPlayerTextures) {
          fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                            "tests/fixtures/beatoraja_skin/resources/fixture.png",
                        source / "skin/characters/texture2p.png");
          chp += "#CharTex2P\ttexture2p.png\n";
        }
        chp +=
            "#CharFace\tface.png\n#SelectCG\tselect.png\n"
            "#CharFaceUpperSize\t0\t0\t10\t10\n"
            "#Size\t40\t20\n#00\t0\t0\t10\t10\n"
            "#01\t10\t0\t10\t10\n#Frame\t1\t40\n"
            "#Pattern\t1\t0001\n#Texture\t1\t0001\n";
        writeText(source / "skin/characters/alpha.chp", chp);
      } else if (options.malformedPomyuNumeric !=
                 MalformedPomyuNumeric::None) {
        fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                          "tests/fixtures/beatoraja_skin/resources/fixture.png",
                      source / "skin/characters/fixture.png");
        std::string chp =
            "#CharBMP\tfixture.png\n#Size\t40\t20\n"
            "#00\t0\t0\t10\t10\n#Anime\t100\n"
            "#Frame\t1\t40\n#Loop\t1\t-1\n#Pattern\t1\t00\n"
            "#Comment\t\x83\x65\x83\x58\x83\x67\n";
        switch (options.malformedPomyuNumeric) {
        case MalformedPomyuNumeric::Anime:
          chp += "#Anime\toops\n";
          break;
        case MalformedPomyuNumeric::Frame:
          chp += "#Frame\t1\toops\n";
          break;
        case MalformedPomyuNumeric::Size:
          chp += "#Size\toops\t20\n";
          break;
        case MalformedPomyuNumeric::Coordinate:
          chp += "#00\toops\t0\t10\t10\n";
          break;
        case MalformedPomyuNumeric::Motion:
          chp += "#Pattern\toops\t00\n";
          break;
        case MalformedPomyuNumeric::Loop:
          chp += "#Loop\t1\toops\n";
          break;
        case MalformedPomyuNumeric::FaceRectangle:
          chp += "#CharFaceUpperSize\toops\t0\t10\t10\n";
          break;
        case MalformedPomyuNumeric::None:
          break;
        }
        writeText(source / "skin/characters/alpha.chp", chp);
      } else if (options.oversizedPomyuWithSibling) {
        writeText(source / "skin/characters/alpha.chp",
                  std::string(SkinResourcePolicy::maximumEncodedBytes + 1U,
                              'x'));
        writeText(source / "skin/characters/beta.chp",
                  "#Anime\t100\n#Frame\t1\t40\n#Pattern\t1\t000102\n");
      } else if (options.pomyuMissingCharBmp) {
        writeText(source / "skin/characters/alpha.chp",
                  "#Anime\t100\n#Frame\t1\t40\n#Pattern\t1\t000102\n");
      } else if (options.pomyuTextureMissingCharTex) {
        fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                          "tests/fixtures/beatoraja_skin/resources/fixture.png",
                      source / "skin/characters/fixture.png");
        writeText(source / "skin/characters/alpha.chp",
                  "#CharBMP\tfixture.png\n#Anime\t100\n#Frame\t1\t40\n"
                  "#Texture\t1\t000102\n");
      } else if (options.pomyuCp932BackslashPath) {
        const fs::path japaneseDirectory =
            source / "skin/characters" /
            fs::path("\xe2\x85\xb0\xe8\xa1\xa8~");
        fs::create_directories(japaneseDirectory);
        fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                          "tests/fixtures/beatoraja_skin/resources/fixture.png",
                      japaneseDirectory / "fixture.png");
        writeText(source / "skin/characters/alpha.chp",
                  "#CharBMP\t\xfa\x40\x95\x5c~\\fixture.png\n"
                  "#Anime\t100\n#Frame\t1\t40\n#Pattern\t1\t000102\n");
      } else if (options.pomyuRootedResourcePath) {
        fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                          "tests/fixtures/beatoraja_skin/resources/fixture.png",
                      source / "skin/characters/rooted.png");
        writeText(source / "skin/characters/alpha.chp",
                  "#CharBMP\t/rooted.png\n"
                  "#Anime\t100\n#Frame\t1\t40\n#Pattern\t1\t000102\n");
      } else if (options.pomyuLeadingBackslashPath) {
        fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                          "tests/fixtures/beatoraja_skin/resources/fixture.png",
                      source / "skin/characters/rooted.png");
        writeText(source / "skin/characters/alpha.chp",
                  "#CharBMP\t\\rooted.png\n"
                  "#Anime\t100\n#Frame\t1\t40\n#Pattern\t1\t000102\n");
      } else {
        fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                          "tests/fixtures/beatoraja_skin/resources/fixture.png",
                      source / "skin/characters/fixture.png");
        writeText(source / "skin/characters/alpha.chp",
                  "#CharBMP\tfixture.png\n#Size\t40\t20\n"
                  "#00\t0\t0\t10\t10\n#Anime\t100\n"
                  "#Pattern\t1\t00\n");
      }
    }
    std::string script = R"lua(
local phase_count = (rawget(_G, "session_activation_phase_count") or 0) + 1
_G.session_activation_phase_count = phase_count
if skin_config then
  if phase_count ~= 2 then
    error("configured phase did not reuse exactly one fresh header state")
  end
 )lua";
    if (options.requireConfiguredState) {
      script += R"lua(
  if main_state.option(81) ~= true then
    error("configured state did not expose the initialized loaded option")
  end
)lua";
    }
    if (options.requireResultConfiguredState) {
      script += R"lua(
  if main_state.option(90) ~= false then
    error("configured result state did not expose the result clear property")
  end
)lua";
    }
    if (options.legacyInputBearing) {
      script += R"lua(
  local Gdx = luajava.bindClass("com.badlogic.gdx.Gdx")
  if Gdx.graphics:getWidth() ~= 640 or Gdx.graphics:getHeight() ~= 360 then
    error("configured phase did not receive the initial legacy-input snapshot")
  end
)lua";
    }
    script += R"lua(
  local marker = io.open("configured-phase-marker.txt", "w")
  if marker then
    marker:write("configured")
    marker:close()
  end
)lua";
    if (options.audioBearing) {
      script += R"lua(
  assert(main_state.audio_preload("session-audio.ogg") == true)
  assert(main_state.audio_play("session-audio.ogg") == true)
  assert(main_state.audio_loop("session-audio.ogg", 0.5) == true)
)lua";
    }
    if (options.movieBearing && options.resourceBearing) {
      script += R"lua(
  return {
    type = 0, w = 1280, h = 720,
    source = {
      {id = "fixture-image", path = "resources/fixture.png"},
      {id = "movie-one", path = "resources/source.MP4"}
    },
    font = {{id = "fixture-font", path = "resources/fixture.ttf", type = 0}},
    image = {
      {id = "fixture-object", src = "fixture-image", x = 0, y = 0, w = 40, h = 20},
      {id = "movie-object", src = "movie-one", x = 0, y = 0, w = 80, h = 40}
    },
    text = {{id = "runtime-title", font = "fixture-font", size = 16, ref = 10}},
    destination = {
      {id = "fixture-object", dst = {{x = 0, y = 0, w = 40, h = 20}}},
      {id = "movie-object", dst = {{x = 40, y = 0, w = 80, h = 40}}},
      {id = "runtime-title", dst = {{x = 50, y = 50, w = 500, h = 30}}}
    }
  }
)lua";
    } else if (options.movieBearing) {
      script += R"lua(
  return {
    type = 0, w = 1280, h = 720,
    source = {
      {id = "movie-one", path = "resources/source.MP4"},
      {id = "movie-two", path = "resources/source.MP4"}
    },
    image = {
      {id = "movie-object-one", src = "movie-one", x = 0, y = 0, w = 80, h = 40},
      {id = "movie-object-two", src = "movie-two", x = 0, y = 0, w = 80, h = 40}
    },
    destination = {
      {id = "movie-object-one", dst = {{x = 0, y = 0, w = 80, h = 40}}},
      {id = "movie-object-two", dst = {{x = 80, y = 0, w = 80, h = 40}}}
    }
  }
)lua";
    } else if (options.resourceBearing) {
      script += "\n  return {\n    type = " +
                std::to_string(options.skinType) + R"lua(, w = 1280, h = 720,
    source = {{id = "fixture-image", path = "resources/fixture.png"}},
    font = {{id = "fixture-font", path = "resources/fixture.ttf", type = 0}},
    image = {{id = "fixture-object", src = "fixture-image", x = 0, y = 0, w = 40, h = 20}},
    text = {{id = "runtime-title", font = "fixture-font", size = 16, ref = 10}},
    destination = {
      {id = "fixture-object", dst = {{x = 0, y = 0, w = 40, h = 20}}},
      {id = "runtime-title", dst = {{x = 50, y = 50, w = 500, h = 30}}}
    }
  }
)lua";
    } else if (options.pomyuSecondPlayerTextures ||
               options.pomyuSecondPlayerTextureFallback) {
      script += R"lua(
  return {
    type = 0, w = 1280, h = 720,
    source = {{id = "shared-chara", path = "characters/alpha.chp"}},
    pmchara = {
      {id = "pomyu-play", src = "shared-chara", color = 2, type = 0},
      {id = "pomyu-face", src = "shared-chara", color = 2, type = 3},
      {id = "pomyu-select", src = "shared-chara", color = 2, type = 5},
      {id = "pomyu-background", src = "shared-chara", color = 2, type = 1},
      {id = "pomyu-default-type", src = "shared-chara"}
    },
    destination = {
      {id = "pomyu-play", loop = 0, dst = {{x = 0, y = 0, w = 80, h = 40}}},
      {id = "pomyu-face", loop = 0, dst = {{x = 100, y = 0, w = 80, h = 40}}},
      {id = "pomyu-select", loop = 0, dst = {{x = 200, y = 0, w = 80, h = 40}}},
      {id = "pomyu-background", loop = 0, dst = {{x = 300, y = 0, w = 80, h = 40}}},
      {id = "pomyu-default-type", loop = 0, dst = {{x = 400, y = 0, w = 80, h = 40}}}
    }
  }
)lua";
    } else if (hasPomyu) {
      script += R"lua(
  return {
    type = 0, w = 1280, h = 720,
    source = {{id = "shared-chara", path = "characters/alpha.chp"}},
    pmchara = {
      {id = "pomyu-one", src = "shared-chara", type = 0, side = 1},
      {id = "pomyu-two", src = "shared-chara", type = 0, side = 1}
    },
    destination = {
      {id = "pomyu-one", dst = {{x = 0, y = 0, w = 64, h = 64}}},
      {id = "pomyu-two", dst = {{x = 64, y = 0, w = 64, h = 64}}}
    }
  }
)lua";
    } else if (options.resultNestedEventExec) {
      script += "\n  return { type = " +
                std::to_string(options.skinType) + R"lua(, w = 1280, h = 720,
    customEvents = {
      {id = 1000, action = function()
        assert(main_state.event_exec(1001))
      end},
      {id = 1001, action = function()
        assert(main_state.event_exec(1002))
      end},
      {id = 1002, action = 210}
    },
    customTimers = {{id = 10000, timer = function()
      assert(main_state.event_exec(1000))
      return 0
    end}}
  }
)lua";
    } else if (options.resultIntervalEventExec) {
      script += "\n  return { type = " +
                std::to_string(options.skinType) + R"lua(, w = 1280, h = 720,
    customEvents = {{id = 1000, action = 210, condition = function()
      return true
    end, minInterval = 1000}},
    customTimers = {{id = 10000, timer = function()
      assert(main_state.event_exec(1000))
      return 0
    end}}
  }
)lua";
    } else if (options.resultRecursiveEventExec) {
      script += "\n  return { type = " +
                std::to_string(options.skinType) + R"lua(, w = 1280, h = 720,
    customEvents = {{id = 1000, action = 1000}},
    customTimers = {{id = 10000, timer = function()
      assert(main_state.event_exec(1000))
      return 0
    end}}
  }
)lua";
    } else if (options.resultDuplicateEventExec) {
      script += "\n  return { type = " +
                std::to_string(options.skinType) + R"lua(, w = 1280, h = 720,
    customEvents = {
      {id = 1000, action = 210, condition = function() return true end},
      {id = 1000, action = 999, condition = function() return true end}
    },
    customTimers = {{id = 10000, timer = function()
      assert(main_state.event_exec(1000))
      return 0
    end}}
  }
)lua";
    } else if (options.resultDuplicateTimerExec) {
      script += "\n  return { type = " +
                std::to_string(options.skinType) + R"lua(, w = 1280, h = 720,
    customTimers = {
      {id = 10000, timer = function()
        assert(main_state.event_exec(210))
        return 0
      end},
      {id = 10000, timer = function() return 0 end}
    }
  }
)lua";
    } else if (options.resultEventExec) {
      script += "\n  return { type = " +
                std::to_string(options.skinType) + R"lua(, w = 1280, h = 720,
    customEvents = {{id = 1000, action = 210}},
    customTimers = {{id = 10000, timer = function()
      assert(main_state.event_exec(1000, 17, 23))
      return 0
    end}}
  }
)lua";
    } else {
      const int configuredSkinType = options.configuredSkinType >= 0
                                         ? options.configuredSkinType
                                         : options.skinType;
      script += "\n  return { type = " + std::to_string(configuredSkinType) +
                ", w = 1280, h = 720 }\n";
    }
    script += "\nend\nif phase_count ~= 1 then\n"
              "  error(\"header phase did not begin in a fresh state\")\n"
              "end\nreturn { type = " + std::to_string(options.skinType) +
              ", name = \"activation shell\", w = 1280, h = 720 }\n";
    writeText(source / "skin/main.luaskin", script);
    if (options.staticResultCustomEvent) {
      writeText(source / "skin/main.json", R"json({
  "type": 7,
  "w": 1280,
  "h": 720,
  "customEvents": [{"id": 1000, "action": 210, "condition": 50}]
})json");
    }

    // Runtime execution follows the installed, Files-visible package exactly
    // as Beatoraja follows its selected skin directory.  Keep the immutable
    // revision for activation identity, but make this fixture exercise the
    // writable visible copy used by a real installation.
    fs::create_directories(roots_.visiblePackages);
    fs::copy(source, roots_.visiblePackages / package_.directoryName,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);

    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(),
           "activation fixture snapshots an immutable revision");
    if (!snapshot.prepared) {
      return;
    }
    std::string publishError;
    lease_ = std::move(*snapshot.prepared).publish(publishError);
    expect(lease_.has_value() && publishError.empty(),
           "activation fixture publishes the immutable revision");
    if (!lease_) {
      return;
    }

    GameplaySkinValidator validator(resources_);
    validation_ = validator.validate(lease_->readView(), entry_, &desired_, {});
    expect(validation_.disposition ==
           SkinValidationDisposition::SelectableGameplay &&
               validation_.reconciledSettings.has_value() &&
               !validation_.configurationDigest.empty(),
           "activation fixture validates a selectable skin");
  }

  bool ready() const noexcept {
    return lease_.has_value() && validation_.reconciledSettings.has_value() &&
           !validation_.configurationDigest.empty();
  }

  ValidatedSkinActivation takeActivation() {
    return {.revision = std::move(*lease_),
            .entry = entry_,
            .reconciledSettings = *validation_.reconciledSettings,
            .configurationDigest = validation_.configurationDigest};
  }

  PlaySkinSessionContext context(
      ViewportSettings viewport = {}, std::stop_token stop = {}) {
    initialState_ = stateAt(1);
    initialState_.authority.loadingState = PlayfieldLoadingState::Loaded;
    initialProjection_ = projectionAt(initialState_.clock.serial);
    return {.sessionSerial = 73,
            .profileId = profile_,
            .chartModel = chart_,
            .initialState = &initialState_,
            .initialProjection = &initialProjection_,
            .viewport = viewport,
            .safeUiBounds = {.x = 0.0,
                             .y = 0.0,
                             .width = 1280.0,
                             .height = 720.0},
            .storageRoots = roots_,
            .resourcePreparation = resources_,
            .textureDevice = device_,
            .movieDevice = movieDevice_,
            .audioBackend = audioBackend_,
            .liveResourceCounters = liveResourceCounters_,
            .configurationWrites = configurationWrites_,
            .stop = stop};
  }

  ResultSkinSessionContext resultContext(ResultSkinData initialData = {}) {
    return {.profileId = profile_,
            .storageRoots = roots_,
            .resourcePreparation = resources_,
            .initialData = std::move(initialData),
            .textureDevice = device_,
            .audioBackend = audioBackend_,
            .liveResourceCounters = liveResourceCounters_};
  }

  const SkinEntryId &entry() const noexcept { return entry_; }
  const SkinProfileId &profile() const noexcept { return profile_; }
  const std::string &configurationDigest() const noexcept {
    return validation_.configurationDigest;
  }
  fs::path configuredMarkerPath() const {
    const auto overlay =
        deriveSkinPrivateOverlayRoot(roots_, profile_, entry_);
    return *overlay.root / "skin/configured-phase-marker.txt";
  }
  const std::shared_ptr<SessionTextureDevice> &device() const noexcept {
    return device_;
  }
  const std::shared_ptr<SessionMovieDevice> &movieDevice() const noexcept {
    return movieDevice_;
  }
  const std::shared_ptr<SessionAudioState> &audioState() const noexcept {
    return audioState_;
  }
  void releaseAudioBackend() noexcept { audioBackend_.reset(); }
  const std::shared_ptr<SkinLiveResourceCounters> &liveCounters() const
      noexcept {
    return liveResourceCounters_;
  }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  PlayfieldChartVisualModel chart_;
  PlayfieldVisualState initialState_;
  PlayfieldProjectionResult initialProjection_;
  EntryProfileSettings desired_;
  SkinResourcePreparationService resources_;
  std::shared_ptr<SessionTextureDevice> device_;
  std::shared_ptr<SessionMovieDevice> movieDevice_;
  std::shared_ptr<SessionAudioState> audioState_;
  std::shared_ptr<SessionAudioBackend> audioBackend_;
  std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters_ =
      std::make_shared<SkinLiveResourceCounters>();
  SkinConfigurationWriteQueue configurationWrites_;
  std::optional<SkinRevisionLease> lease_;
  SkinValidationResult validation_;
};

class ExternalResultSkinFixture final {
public:
  ExternalResultSkinFixture(const fs::path &source, std::string_view entryPath)
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("ExternalResultSkin").package),
        profile_(*makeSkinProfileId(
            "77777777-7777-4777-8777-777777777777")),
        state_(nullptr, false) {
    // Real result state has at least one resolved judgement.  Several shipped
    // Beatoraja skins derive graph dimensions from this distribution, where an
    // all-zero synthetic state would instead manufacture NaN Lua numbers.
    state_.judgeCount[PGreat] = meta_.TotalNotes;
    state_.judgementFastSlowCount[PGreat].fast = 1;
    const auto normalizedEntry = normalizeEntryPath(package_, entryPath);
    expect(normalizedEntry.entry.has_value(),
           "external result skin entry path is valid");
    if (!normalizedEntry.entry) {
      return;
    }
    entry_ = *normalizedEntry.entry;
    const fs::path stagedSource = temp_.root() / "source";
    if (!copyLuaSources(source, stagedSource)) {
      return;
    }
    std::error_code error;
    fs::create_directories(roots_.visiblePackages, error);
    if (error || !copyLuaSources(stagedSource,
                                 roots_.visiblePackages / package_.directoryName)) {
      return;
    }
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(stagedSource, package_, {}, {});
    expect(snapshot.prepared.has_value(),
           "external result skin immutable revision snapshots");
    if (!snapshot.prepared) {
      return;
    }
    std::string publishError;
    lease_ = std::move(*snapshot.prepared).publish(publishError);
    expect(lease_.has_value() && publishError.empty(),
           "external result skin revision publishes");
    if (!lease_) {
      return;
    }
    GameplaySkinValidator validator(resources_);
    validation_ = validator.validate(lease_->readView(), entry_, nullptr, {});
    if (validation_.disposition != SkinValidationDisposition::SelectableGameplay) {
      for (const auto &diagnostic : validation_.diagnostics) {
        std::cerr << "external result validation diagnostic: "
                  << diagnostic.code << ": " << diagnostic.message << '\n';
      }
    }
    expect(validation_.disposition == SkinValidationDisposition::SelectableGameplay &&
               validation_.reconciledSettings.has_value() &&
               !validation_.configurationDigest.empty(),
           "external result skin validates as selectable");
  }

  GameplaySkinDocumentLoadResult configure() {
    if (!lease_ || !validation_.reconciledSettings ||
        validation_.configurationDigest.empty()) {
      return {};
    }
    const auto format = gameplaySkinSourceFormatForPath(entry_.packageRelativePath);
    if (!format) {
      return {};
    }
    const auto revision = lease_->readView();
    auto document = LuaSkinFileSystem::create(
        {.revision = revision, .entry = entry_, .storageRoots = roots_,
         .safetyPolicy = SkinSafetyPolicy{}});
    auto lua = LuaSkinFileSystem::create(
        {.revision = revision, .entry = entry_, .storageRoots = roots_,
         .profileId = profile_, .allowDataWrites = true,
         .safetyPolicy = SkinSafetyPolicy{}});
    if (!document.fileSystem || !lua.fileSystem) {
      return {};
    }
    ResultSkinData data{.state = &state_, .meta = &meta_, .context = nullptr};
    GameplaySkinDocumentLoader loader;
    return loader.load(
        {.sourceFormat = *format,
         .entry = entry_,
         .documentFileSystem = *document.fileSystem,
         .luaFileSystem = std::move(lua.fileSystem),
         .desiredSettings = &*validation_.reconciledSettings,
         .expectedConfigurationDigest = validation_.configurationDigest,
         .luaPurpose = LuaRuntimePurpose::Gameplay,
         .loadConfiguredLua = [&data](
                                  LuaSkinRuntime &runtime,
                                  const BeatorajaSkinConfiguration &configuration,
                                  std::vector<SkinDiagnostic> &) {
           ResultSkinStateBridge bridge(data, 1, 0);
           runtime.setFrameState(&bridge);
           auto loaded = runtime.loadConfigured(configuration);
           runtime.setFrameState(nullptr);
           return loaded;
         }});
  }

private:
  static bool copyLuaSources(const fs::path &source, const fs::path &target) {
    std::error_code error;
    for (fs::recursive_directory_iterator iterator(source, error), end;
         !error && iterator != end; iterator.increment(error)) {
      if (!iterator->is_regular_file(error)) {
        continue;
      }
      const auto extension = iterator->path().extension();
      if (extension != ".lua" && extension != ".luaskin") {
        continue;
      }
      const fs::path destination =
          target / iterator->path().lexically_relative(source);
      fs::create_directories(destination.parent_path(), error);
      if (error) {
        break;
      }
      fs::copy_file(iterator->path(), destination,
                    fs::copy_options::overwrite_existing, error);
    }
    expect(!error, "external result skin Lua sources copy into the fixture");
    return !error;
  }

  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  RhythmState state_;
  bms_parser::ChartMeta meta_{.TotalNotes = 100, .Bpm = 120.0};
  SkinResourcePreparationService resources_;
  std::optional<SkinRevisionLease> lease_;
  SkinValidationResult validation_;
};

struct ParityQuadOutput {
  SkinObjectId object = 0;
  SkinResourceId resource = 0;
  std::array<std::array<float, 4>, 4> vertices{};
  std::array<std::uint32_t, 4> colors{};
  bool operator==(const ParityQuadOutput &) const = default;
};

struct ParityTextOutput {
  SkinObjectId object = 0;
  std::vector<char32_t> glyphs;
  std::vector<char32_t> fallbackGlyphs;
  bool operator==(const ParityTextOutput &) const = default;
};

struct ParityCommandOutput {
  std::vector<ParityQuadOutput> quads;
  std::vector<ParityTextOutput> texts;
  std::size_t otherCommands = 0;
  bool operator==(const ParityCommandOutput &) const = default;
};

ParityCommandOutput parityCommandOutput(const SkinCommandBuffer &commands) {
  ParityCommandOutput output;
  for (const auto &command : commands.commands) {
    if (const auto *quad =
            std::get_if<SkinTexturedQuadCommand>(&command.payload);
        quad != nullptr &&
        (command.sourceObject == 1 || command.sourceObject == 2)) {
      ParityQuadOutput normalized{.object = command.sourceObject,
                                  .resource = quad->resource};
      for (std::size_t index = 0; index < quad->vertices.size(); ++index) {
        const auto &vertex = quad->vertices[index];
        normalized.vertices[index] = {vertex.x, vertex.y, vertex.u, vertex.v};
        normalized.colors[index] = vertex.rgba;
      }
      output.quads.push_back(std::move(normalized));
      continue;
    }
    if (const auto *text = std::get_if<SkinGlyphRunCommand>(&command.payload);
        text != nullptr && command.sourceObject == 3) {
      ParityTextOutput normalized{.object = command.sourceObject};
      for (const auto &glyph : text->glyphs) {
        normalized.glyphs.push_back(glyph.codepoint);
      }
      for (const auto &glyph : text->fallbackColorOverlays) {
        normalized.fallbackGlyphs.push_back(glyph.codepoint);
      }
      output.texts.push_back(std::move(normalized));
      continue;
    }
    ++output.otherCommands;
  }
  return output;
}

class FormatParityFixture final {
public:
  FormatParityFixture()
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("GameplayFormats").package),
        profile_(*makeSkinProfileId(
            "88888888-8888-4888-8888-888888888888")) {
    chart_.keyCount = 7;
    chart_.text.title = "AV";
    chart_.text.artist = "format fixture";
    chart_.text.fullArtist = "format fixture";

    const fs::path source = temp_.root() / "source";
    const fs::path chartResources = temp_.root() / "chart-resources.zip";
    fs::create_directories(source / "skin/resources");
    fs::create_directories(source / "skin/fonts");
    fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                      "tests/fixtures/beatoraja_skin/resources/fixture.png",
                  source / "skin/resources/fixture.png");
    std::ifstream chartImage(
        fs::path(ASOBMASHOW_SOURCE_DIR) /
            "tests/fixtures/beatoraja_skin/resources/fixture.png",
        std::ios::binary);
    const std::vector<unsigned char> chartImageBytes{
        std::istreambuf_iterator<char>(chartImage),
        std::istreambuf_iterator<char>()};
    writeStoredZip(chartResources,
                   {{"song/stage.png", chartImageBytes},
                    {"song/back.png", chartImageBytes},
                    {"song/banner.png", chartImageBytes}});
    chart_.staticMetadata.stageFileResourcePath =
        chartResources / "song/stage.png";
    chart_.staticMetadata.backBmpResourcePath =
        chartResources / "song/back.png";
    chart_.staticMetadata.bannerResourcePath =
        chartResources / "song/banner.png";
    unavailableChart_ = chart_;
    unavailableChart_.staticMetadata.stageFileResourcePath.clear();
    unavailableChart_.staticMetadata.backBmpResourcePath.clear();
    unavailableChart_.staticMetadata.bannerResourcePath.clear();
    fs::copy_file(
        fs::path(ASOBMASHOW_SOURCE_DIR) /
            "tests/fixtures/beatoraja_skin/resources/bitmap-font/fixture.fnt",
        source / "skin/fonts/fixture.fnt");
    fs::copy_file(
        fs::path(ASOBMASHOW_SOURCE_DIR) /
            "tests/fixtures/beatoraja_skin/resources/bitmap-font/page.png",
        source / "skin/fonts/page.png");
    writeText(source / "skin/fonts/fixture.lr2font",
              "#S,10\n#M,0\n#T,0,page.png\n"
              "#R,65,0,9,0,6,8\n#R,86,0,15,0,7,8\n");

    writeText(source / "skin/parity.luaskin", R"lua(
local skin = {
  type = 0, name = "Lua parity", author = "fixture", w = 640, h = 480
}
if skin_config then
  skin.source = {{id = "atlas", path = "resources/fixture.png"}}
  skin.font = {{id = "font", path = "fonts/fixture.fnt", type = 0}}
  skin.image = {{id = "image", src = "atlas", x = 0, y = 0,
                 w = 40, h = 20, divx = 1, divy = 1}}
  skin.value = {{id = "number", src = "atlas", x = 0, y = 0,
                 w = 40, h = 20, divx = 10, divy = 1,
                 ref = 10, align = 0, digit = 3, padding = 0,
                 zeropadding = 0, space = 0}}
  skin.text = {{id = "text", font = "font", size = 10,
                align = 0, ref = 10}}
  skin.destination = {
    {id = "image", dst = {{time = 0, x = 10, y = 100, w = 40, h = 20}}},
    {id = "number", dst = {{time = 0, x = 60, y = 100, w = 4, h = 20}}},
    {id = "text", dst = {{time = 0, x = 100, y = 100, w = 200, h = 40}}}
  }
end
return skin
)lua");

    writeText(source / "skin/parity.json", R"json({
  "type": 0, "name": "JSON parity", "author": "fixture", "w": 640, "h": 480,
  "source": [{"id":"atlas","path":"resources/fixture.png"}],
  "font": [{"id":"font","path":"fonts/fixture.fnt","type":0}],
  "image": [{"id":"image","src":"atlas","x":0,"y":0,"w":40,"h":20,"divx":1,"divy":1}],
  "value": [{"id":"number","src":"atlas","x":0,"y":0,"w":40,"h":20,"divx":10,"divy":1,"ref":10,"align":0,"digit":3,"padding":0,"zeropadding":0,"space":0}],
  "text": [{"id":"text","font":"font","size":10,"align":0,"ref":10}],
  "destination": [
    {"id":"image","dst":[{"time":0,"x":10,"y":100,"w":40,"h":20}]},
    {"id":"number","dst":[{"time":0,"x":60,"y":100,"w":4,"h":20}]},
    {"id":"text","dst":[{"time":0,"x":100,"y":100,"w":200,"h":40}]}
  ]
})json");
    writeText(source / "skin/commented.json", R"json(/* production comment */
{
  "type": 0,
  // resource declaration
  "source": [{"id":"atlas","path":"resources/fixture.png"}],
  "image": [
    /* object comment */
    {"id":"image","src":"atlas","x":0,"y":0,"w":40,"h":20}
  ],
  "destination": [
    // presentation comment
    {"id":"image","dst":[{"x":10,"y":100,"w":40,"h":20}]}
  ]
}
// accepted trailing comment
)json");

    writeText(source / "skin/parity.lr2skin", R"lr2(#INFORMATION,0,LR2 parity,fixture
#RESOLUTION,0
#IMAGE,resources/fixture.png
#LR2FONT,fonts/fixture.lr2font
#SRC_IMAGE,0,0,0,0,40,20,1,1,0,0
#DST_IMAGE,0,0,10,360,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#SRC_NUMBER,0,0,0,0,40,20,10,1,0,0,10,0,3,0,0
#DST_NUMBER,0,0,60,360,4,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#SRC_TEXT,0,0,10,0,0,0
#DST_TEXT,0,0,100,340,200,40,0,255,255,255,255,0,0,0,0,0,0,0,0,0
)lr2");

    writeText(source / "skin/builtin-graphs.lr2skin", R"lr2(#INFORMATION,0,Builtin graphs,fixture
#RESOLUTION,0
#SRC_BARGRAPH,0,110,0,0,1,1,1,1,0,0,2,0
#DST_BARGRAPH,0,0,10,400,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#SRC_BARGRAPH,0,111,0,0,1,1,1,1,0,0,2,0
#DST_BARGRAPH,0,0,60,400,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#SRC_BARGRAPH,0,100,0,0,1,1,1,1,0,0,2,0
#DST_BARGRAPH,0,0,110,400,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#SRC_BARGRAPH,0,101,0,0,1,1,1,1,0,0,2,0
#DST_BARGRAPH,0,0,160,400,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#SRC_BARGRAPH,0,102,0,0,1,1,1,1,0,0,2,0
#DST_BARGRAPH,0,0,210,400,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
)lr2");
    writeText(source / "skin/unavailable-builtin-graphs.lr2skin", R"lr2(#INFORMATION,0,Unavailable builtin graphs,fixture
#RESOLUTION,0
#SRC_BARGRAPH,0,100,0,0,1,1,1,1,0,0,2,0
#DST_BARGRAPH,0,0,10,400,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#SRC_BARGRAPH,0,101,0,0,1,1,1,1,0,0,2,0
#DST_BARGRAPH,0,0,60,400,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#SRC_BARGRAPH,0,102,0,0,1,1,1,1,0,0,2,0
#DST_BARGRAPH,0,0,110,400,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
)lr2");
    writeText(source / "skin/option-state.lr2skin", R"lr2(#INFORMATION,0,Option state,fixture
#RESOLUTION,0
#CUSTOMOPTION,Mode,900,Selected,Unselected
#IF,!901
#INCLUDE,active-option.inc
#ENDIF
#IF,!900
#INCLUDE,missing-selected-sibling.inc
#ENDIF
#IF,!9999
#INCLUDE,missing-runtime-only.inc
#ENDIF
)lr2");
    writeText(source / "skin/active-option.inc", R"lr2(#IMAGE,resources/fixture.png
#SRC_IMAGE,0,0,0,0,40,20,1,1,0,0
#DST_IMAGE,0,0,10,360,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
)lr2");
    const fs::path lr2Fixtures = fs::path(ASOBMASHOW_SOURCE_DIR) /
                                 "tests/fixtures/beatoraja_skin/lr2/header";
    for (const std::string_view name : {"malformed-setoption.lr2skin",
                                        "malformed-setoption.inc",
                                        "plus-option.inc"}) {
      fs::copy_file(lr2Fixtures / name, source / "skin" / name);
    }

    writeText(source / "skin/recoverable.lr2skin", R"lr2(#INFORMATION,0,Recoverable LR2,fixture
#RESOLUTION,0
#IMAGE,resources/fixture.png
#SRC_IMAGE,0,0,0,0,40,20,1,1,0,0
#DST_IMAGE,0,0,10,360,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
#INCLUDE,missing-optional.inc
#RESOLUTION,bad
#IMAGE
#SRC_IMAGE,0,0,0,0,40,20,1,1,0,0
#DST_IMAGE,0,0,60,360,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
)lr2");
    writeText(source / "skin/skipped.lr2skin", R"lr2(#INFORMATION,0,Skipped LR2,fixture
#RESOLUTION,0
#IF,9999
#INCLUDE,missing-skipped.inc
#INCLUDE,cycle-a.inc
#ENDIF
#IMAGE,resources/fixture.png
#SRC_IMAGE,0,0,0,0,40,20,1,1,0,0
#DST_IMAGE,0,0,10,360,40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0
)lr2");
    writeText(source / "skin/cycle-a.inc", "#INCLUDE,cycle-b.inc\n");
    writeText(source / "skin/cycle-b.inc", "#INCLUDE,cycle-a.inc\n");
    writeText(source / "skin/unsafe.lr2skin", R"lr2(#INFORMATION,0,Unsafe LR2,fixture
#RESOLUTION,0
#INCLUDE,../../outside.inc
#IMAGE,resources/fixture.png
)lr2");
    writeText(source / "skin/invalid-encoding.lr2skin",
              std::string(1, static_cast<char>(0x81)));

    writeText(source / "config/settings.json",
              R"json({"application":"configuration"})json");
    writeText(source / "select/select.lr2skin",
              "#INFORMATION,5,Music select,fixture\n#RESOLUTION,0\n");
    writeText(source / "notes/readme.txt", "not a gameplay document\n");

    SkinArchiveImporter importer(roots_, aliases_);
    auto imported = importer.prepareFolder(source, package_, {}, {});
    expect(imported.prepared.has_value(),
           "mixed-format gameplay package imports as one candidate revision");
    if (!imported.prepared) {
      return;
    }
    std::vector<std::string> importedPaths;
    std::size_t selectable = 0;
    std::size_t unavailable = 0;
    GameplaySkinValidator validator(resources_);
    for (const auto &entry : imported.prepared->entries()) {
      importedPaths.push_back(entry.packageRelativePath);
      const auto validation = validator.validate(
          imported.prepared->readView(), entry, nullptr, {});
      selectable += validation.disposition ==
                    SkinValidationDisposition::SelectableGameplay;
      unavailable += validation.disposition ==
                     SkinValidationDisposition::UnavailableType;
    }
    std::ranges::sort(importedPaths);
    expect(importedPaths ==
               std::vector<std::string>{"config/settings.json",
                                        "select/select.lr2skin",
                                        "skin/builtin-graphs.lr2skin",
                                        "skin/commented.json",
                                        "skin/invalid-encoding.lr2skin",
                                        "skin/malformed-setoption.lr2skin",
                                        "skin/option-state.lr2skin",
                                        "skin/parity.json",
                                        "skin/parity.lr2skin",
                                        "skin/parity.luaskin",
                                        "skin/recoverable.lr2skin",
                                        "skin/skipped.lr2skin",
                                        "skin/unavailable-builtin-graphs.lr2skin",
                                        "skin/unsafe.lr2skin"} &&
               selectable == 10 && unavailable == 2,
           "header admission keeps gameplay and recoverable LR2 entries "
           "selectable while fatal documents remain invalid");

    fs::create_directories(roots_.visiblePackages);
    fs::copy(source, roots_.visiblePackages / package_.directoryName,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing);
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(),
           "multi-format parity package snapshots");
    if (!snapshot.prepared) {
      return;
    }
    std::string error;
    lease_ = std::move(*snapshot.prepared).publish(error);
    expect(lease_.has_value() && error.empty(),
           "multi-format parity revision publishes");
  }

  PlaySkinSessionCreateResult create(std::string_view path,
                                     std::uint64_t sessionSerial,
                                     bool chartImagesAvailable = true,
                                     std::shared_ptr<SessionTextureDevice>
                                         textureDevice = {}) {
    PlaySkinSessionCreateResult failed;
    if (!lease_) {
      return failed;
    }
    const auto entry = normalizeEntryPath(package_, path).entry;
    expect(entry.has_value(), "multi-format parity entry ID is valid");
    if (!entry) {
      return failed;
    }
    GameplaySkinValidator validator(resources_);
    auto validation =
        validator.validate(lease_->readView(), *entry, nullptr, {});
    if (validation.disposition !=
            SkinValidationDisposition::SelectableGameplay ||
        !validation.reconciledSettings ||
        validation.configurationDigest.empty()) {
      failed.diagnostics = std::move(validation.diagnostics);
      return failed;
    }

    PlayfieldVisualState initialState = stateAt(1);
    initialState.authority.loadingState = PlayfieldLoadingState::Loaded;
    const PlayfieldProjectionResult initialProjection = projectionAt(1);
    const PlayfieldChartVisualModel &chartModel =
        chartImagesAvailable ? chart_ : unavailableChart_;
    if (!textureDevice) {
      textureDevice = std::make_shared<SessionTextureDevice>();
    }
    return PlaySkinSession::create(
        {.revision = lease_->clone(),
         .entry = *entry,
         .reconciledSettings = *validation.reconciledSettings,
         .configurationDigest = validation.configurationDigest},
        {.sessionSerial = sessionSerial,
         .profileId = profile_,
         .chartModel = chartModel,
         .initialState = &initialState,
         .initialProjection = &initialProjection,
         .safeUiBounds = {.x = 0.0, .y = 0.0, .width = 640.0, .height = 480.0},
         .storageRoots = roots_,
         .resourcePreparation = resources_,
         .builtinImageReader = archive_file::readFileBounded,
         .textureDevice = std::move(textureDevice),
         .liveResourceCounters = std::make_shared<SkinLiveResourceCounters>(),
         .configurationWrites = configurationWrites_});
  }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  PlayfieldChartVisualModel chart_;
  PlayfieldChartVisualModel unavailableChart_;
  SkinResourcePreparationService resources_;
  SkinConfigurationWriteQueue configurationWrites_;
  std::optional<SkinRevisionLease> lease_;
};

void testLuaJsonAndLr2SessionsEmitEquivalentSharedObjects() {
  FormatParityFixture fixture;
  auto lua = fixture.create("skin/parity.luaskin", 101);
  auto json = fixture.create("skin/parity.json", 102);
  auto lr2 = fixture.create("skin/parity.lr2skin", 103);
  auto genericJson = fixture.create("config/settings.json", 104);
  auto nonGameplayLr2 = fixture.create("select/select.lr2skin", 105);
  expect(lua.session && json.session && lr2.session,
         "Lua, JSON, and LR2 gameplay documents all create owning sessions");
  expect(!genericJson.session && !nonGameplayLr2.session,
         "generic JSON and non-gameplay LR2 entries are not session-capable");
  if (!lua.session || !json.session || !lr2.session) {
    return;
  }
  expect(lua.session->hasLuaRuntimeForTesting() &&
             !json.session->hasLuaRuntimeForTesting() &&
             !lr2.session->hasLuaRuntimeForTesting(),
         "only the Lua gameplay session owns a Lua VM");

  const auto luaFrame =
      lua.session->prepareFrame(stateAt(2), projectionAt(2), {});
  const auto jsonFrame =
      json.session->prepareFrame(stateAt(2), projectionAt(2), {});
  const auto lr2Frame =
      lr2.session->prepareFrame(stateAt(2), projectionAt(2), {});
  expect(luaFrame.ready() && jsonFrame.ready() && lr2Frame.ready() &&
             luaFrame.evaluation.submitReady &&
             jsonFrame.evaluation.submitReady &&
             lr2Frame.evaluation.submitReady,
         "all three gameplay formats prepare a renderable frame");
  if (!luaFrame.evaluation.submitReady ||
      !jsonFrame.evaluation.submitReady || !lr2Frame.evaluation.submitReady) {
    return;
  }

  const auto luaOutput = parityCommandOutput(*luaFrame.evaluation.submitReady);
  const auto jsonOutput = parityCommandOutput(*jsonFrame.evaluation.submitReady);
  const auto lr2Output = parityCommandOutput(*lr2Frame.evaluation.submitReady);
  const auto hasSharedObjects = [](const ParityCommandOutput &output) {
    return output.otherCommands == 0 && output.quads.size() >= 2 &&
           std::ranges::any_of(output.quads, [](const auto &quad) {
             return quad.object == 1;
           }) &&
           std::ranges::any_of(output.quads, [](const auto &quad) {
             return quad.object == 2;
           }) &&
           output.texts.size() == 1 && output.texts.front().object == 3 &&
           output.texts.front().glyphs == std::vector<char32_t>{U'A', U'V'};
  };
  expect(hasSharedObjects(luaOutput) && luaOutput == jsonOutput &&
             luaOutput == lr2Output,
         "Lua, JSON, and LR2 emit equivalent image, number, and text output");
}

void testLr2ProductionRecoveryAndFatalBoundaries() {
  FormatParityFixture fixture;
  auto recoverable = fixture.create("skin/recoverable.lr2skin", 106);
  auto skipped = fixture.create("skin/skipped.lr2skin", 107);
  auto unsafe = fixture.create("skin/unsafe.lr2skin", 108);
  auto invalidEncoding =
      fixture.create("skin/invalid-encoding.lr2skin", 109);
  const auto hasCode = [](const auto &result, std::string_view code) {
    return std::ranges::any_of(result.diagnostics, [&](const auto &diagnostic) {
      return diagnostic.code == code;
    });
  };
  expect(recoverable.session &&
             hasCode(recoverable, "skin_lr2_include_read") &&
             hasCode(recoverable, "skin_lr2_header_command_invalid") &&
             hasCode(recoverable, "skin_lr2_gameplay_command_invalid"),
         "missing optional include and malformed header/gameplay lines retain "
         "a usable production session with diagnostics");
  if (recoverable.session) {
    const auto frame = recoverable.session->prepareFrame(
        stateAt(2), projectionAt(2), {});
    expect(frame.ready() && frame.evaluation.submitReady &&
               frame.evaluation.submitReady->commands.size() == 2,
           "valid LR2 commands before and after recoverable failures render");
  }
  expect(skipped.session &&
             !hasCode(skipped, "skin_lr2_include_read") &&
             !hasCode(skipped, "skin_lr2_include_cycle"),
         "false IF skips missing and cyclic includes without production "
         "diagnostics");
  expect(!unsafe.session && hasCode(unsafe, "skin_lr2_include_read"),
         "an unsafe active include remains fatal at production load");
  expect(!invalidEncoding.session &&
             hasCode(invalidEncoding, "skin_lr2_encoding_invalid"),
         "invalid root encoding remains fatal at production load");
}

void testLr2ProductionBuiltInGraphsOwnChartAndPlainImages() {
  FormatParityFixture fixture;
  auto device = std::make_shared<SessionTextureDevice>();
  auto available =
      fixture.create("skin/builtin-graphs.lr2skin", 110, true, device);
  expect(available.session != nullptr,
         "LR2 built-in graphs create a production owning session");
  if (!available.session) {
    return;
  }
  auto graphState = stateAt(2);
  graphState.authority.loadingState = PlayfieldLoadingState::Loaded;
  const auto frame = available.session->prepareFrame(
      graphState, projectionAt(2), {});
  expect(frame.ready() && frame.evaluation.submitReady &&
             frame.evaluation.submitReady->commands.size() == 5,
         "production resolves black, white, stage, back, and banner graphs");
  if (frame.evaluation.submitReady &&
      frame.evaluation.submitReady->commands.size() == 5) {
    const auto &black = std::get<SkinTexturedQuadCommand>(
        frame.evaluation.submitReady->commands[0].payload);
    const auto &white = std::get<SkinTexturedQuadCommand>(
        frame.evaluation.submitReady->commands[1].payload);
    expect(black.resource != white.resource && black.vertices[0].u == 0.0F &&
               black.vertices[1].u == 0.5F && white.vertices[0].u == 0.5F &&
               white.vertices[1].u == 1.0F,
           "production keeps exact black/white region identities on one "
           "owned 2x1 texture");
  }
  const auto plain = std::ranges::find_if(
      device->createdImages, [](const auto &created) {
        return created.width == 2 && created.height == 1;
      });
  expect(plain != device->createdImages.end() &&
             plain->firstPixels ==
                 std::vector<std::uint8_t>{0, 0, 0, 255, 255, 255, 255, 255},
         "production uploads the pinned opaque black and white pixels once");
  const std::size_t created = device->createCalls;
  available.session.reset();
  expect(device->destroyCalls == created,
         "session teardown destroys every built-in and chart texture exactly "
         "once");

  auto unavailable = fixture.create("skin/unavailable-builtin-graphs.lr2skin",
                                    111, false);
  expect(unavailable.session != nullptr,
         "unavailable chart references do not reject the production session");
  if (unavailable.session) {
    const auto missingFrame = unavailable.session->prepareFrame(
        graphState, projectionAt(2), {});
    expect(missingFrame.ready() && missingFrame.evaluation.submitReady &&
               missingFrame.evaluation.submitReady->commands.empty() &&
               missingFrame.evaluation.diagnostics.empty(),
           "unavailable stage, back, and banner references suppress before "
           "their rate property");
  }
}

void testLr2DeclaredFalseOptionActivatesNegatedInclude() {
  FormatParityFixture fixture;
  auto created = fixture.create("skin/option-state.lr2skin", 112);
  const auto hasCode = [&](std::string_view code) {
    return std::ranges::any_of(created.diagnostics, [&](const auto &diagnostic) {
      return diagnostic.code == code;
    });
  };
  expect(created.session && !hasCode("skin_lr2_include_read"),
         "declared option conditions retain a usable production session and "
         "skip selected/unknown negated include paths");
  if (created.session) {
    const auto frame = created.session->prepareFrame(
        stateAt(2), projectionAt(2), {});
    expect(frame.ready() && frame.evaluation.submitReady &&
               frame.evaluation.submitReady->commands.size() == 1,
           "a negated unselected declared choice executes its included image");
  }
}

void testMalformedLr2SetOptionDoesNotDivergeFromIncludeFold() {
  FormatParityFixture fixture;
  auto created = fixture.create("skin/malformed-setoption.lr2skin", 114);
  const auto countCode = [&](std::string_view code) {
    return std::ranges::count_if(
        created.diagnostics, [&](const SkinDiagnostic &diagnostic) {
          return diagnostic.code == code;
        });
  };
  expect(created.session && countCode("skin_lr2_include_read") == 0 &&
             countCode("skin_lr2_gameplay_command_invalid") == 2,
         "malformed root and included SETOPTION commands stay recoverable "
         "without activating missing or unsafe includes");
  if (!created.session) return;
  const auto frame = created.session->prepareFrame(
      stateAt(2), projectionAt(2), {});
  expect(frame.ready() && frame.evaluation.submitReady &&
             frame.evaluation.submitReady->commands.size() == 3,
         "following root/included commands and a Java-valid plus-signed "
         "SETOPTION branch all render");
}

void testCommentedJsonCreatesProductionSession() {
  FormatParityFixture fixture;
  auto created = fixture.create("skin/commented.json", 113);
  expect(created.session &&
             std::ranges::none_of(created.diagnostics,
                                  [](const auto &diagnostic) {
                                    return diagnostic.code ==
                                           "skin_json_source_index_failed";
                                  }),
         "commented JSON remains selectable and session-capable in production");
  if (created.session) {
    const auto frame = created.session->prepareFrame(
        stateAt(2), projectionAt(2), {});
    expect(frame.ready() && frame.evaluation.submitReady &&
               frame.evaluation.submitReady->commands.size() == 1,
           "commented JSON retains its decoded image through production "
           "rendering");
  }
}

void testSessionOwnsDeduplicatedMoviesAndRollsBackBeforePublication() {
  {
    ActivationFixture fixture(
        {.resourceBearing = true, .movieBearing = true});
    if (!fixture.ready()) {
      return;
    }
    auto created =
        PlaySkinSession::create(fixture.takeActivation(), fixture.context());
    expect(created.session && fixture.movieDevice()->loadCalls == 1 &&
               fixture.movieDevice()->lastLimits.maximumDecodedBytes <
                   SkinResourcePolicy::maximumSessionDecodedBytes,
           "session resource planning debits ordinary images and text atlases "
           "before the skin movie adapter receives its allocation budget");
  }

  {
    ActivationFixture fixture({.movieBearing = true});
    if (!fixture.ready()) {
      return;
    }
    auto created =
        PlaySkinSession::create(fixture.takeActivation(), fixture.context());
    expect(created.session && fixture.movieDevice()->loadCalls == 1 &&
               fixture.movieDevice()->destroyCalls == 0 &&
               fixture.movieDevice()->live.size() == 1 &&
               fixture.movieDevice()->pathExistedDuringLoad,
           "session creation materializes and owns one player for duplicate movie source paths");
    if (!created.session) {
      return;
    }
    const fs::path materialized = fixture.movieDevice()->loadedPaths.front();
    expect(fs::is_regular_file(materialized),
           "the session retains its stable materialized movie while active");
    created.session.reset();
    expect(fixture.movieDevice()->destroyCalls == 1 &&
               fixture.movieDevice()->live.empty() &&
               !fs::exists(materialized),
           "session destruction releases its movie player and materialized source exactly once");
  }

  {
    ActivationFixture fixture({.movieBearing = true});
    if (!fixture.ready()) {
      return;
    }
    std::stop_source stop;
    fixture.movieDevice()->stopAfterLoad(stop);
    auto created = PlaySkinSession::create(fixture.takeActivation(),
                                           fixture.context({}, stop.get_token()));
    expect(!created.session && created.cancelled &&
               fixture.movieDevice()->loadCalls == 1 &&
               fixture.movieDevice()->destroyCalls == 1 &&
               fixture.movieDevice()->live.empty() &&
               !fs::exists(fixture.movieDevice()->loadedPaths.front()),
           "cancellation after movie load prevents session publication and rolls ownership back exactly once");
  }

  {
    ActivationFixture fixture({.movieBearing = true});
    if (!fixture.ready()) {
      return;
    }
    auto context = fixture.context();
    context.safeUiBounds.width = 0.0;
    auto created = PlaySkinSession::create(fixture.takeActivation(),
                                           std::move(context));
    expect(!created.session && fixture.movieDevice()->loadCalls == 1 &&
               fixture.movieDevice()->destroyCalls == 1 &&
               fixture.movieDevice()->live.empty() &&
               !fs::exists(fixture.movieDevice()->loadedPaths.front()),
           "a post-load session creation failure tears down the movie graph exactly once");
  }
}

void testSessionOwnsLuaAudioAndRollsBackBeforePublication() {
  {
    ActivationFixture fixture({.audioBearing = true});
    if (!fixture.ready()) {
      return;
    }
    auto created =
        PlaySkinSession::create(fixture.takeActivation(), fixture.context());
    const auto state = fixture.audioState();
    expect(created.session && state->loads.size() == 1 &&
               state->loads.front().generic_string().ends_with(
                   "/skin/session-audio.ogg") &&
               state->plays == std::vector<LuaSkinAudioIdentity>(
                                   3, LuaSkinAudioIdentity{.value = 1}) &&
               state->disposals.empty(),
           "published session retains one resolved audio identity across "
           "preload, play, and loop calls");
    created.session.reset();
    expect(state->disposals ==
               std::vector<LuaSkinAudioIdentity>{{.value = 1}},
           "session destruction disposes its Lua audio identity exactly once");
    const auto callsAfterDestruction = state->disposals.size();
    fixture.releaseAudioBackend();
    expect(state->backendDestroyed &&
               state->disposals.size() == callsAfterDestruction,
           "backend destruction cannot call back into the destroyed session");
  }

  {
    ActivationFixture fixture({.audioBearing = true});
    if (!fixture.ready()) {
      return;
    }
    auto context = fixture.context();
    context.safeUiBounds.width = 0.0;
    auto created = PlaySkinSession::create(fixture.takeActivation(),
                                           std::move(context));
    expect(!created.session && fixture.audioState()->loads.size() == 1 &&
               fixture.audioState()->disposals ==
                   std::vector<LuaSkinAudioIdentity>{{.value = 1}},
           "post-configured session failure rolls back loaded Lua audio once");
  }
}

void testActivationCreatesAnOwningFreshStateSession() {
  ActivationFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  const auto weakRevision = activation.revision.weakPin();
  const std::string revisionDigest =
      activation.revision.revision().lowercaseSha256;
  const ViewportSettings customViewport{
      .mode = ViewportMode::Custom,
      .customBase = CustomViewportBase::Stretch,
      .scaleX = 0.5F,
      .scaleY = 0.5F,
      .translateX = 10.0F,
      .translateY = -20.0F,
  };

  auto created = PlaySkinSession::create(
      std::move(activation), fixture.context(customViewport));
  expect(created.session != nullptr && !created.cancelled &&
             created.configurationDigest == fixture.configurationDigest() &&
             created.reconciledSettings.viewport == customViewport,
         "activation creates one fresh owning session and context viewport wins");
  if (!created.session) {
    return;
  }

  const auto &identity = created.session->identity();
  expect(identity.sessionSerial == 73 &&
             identity.profileId == fixture.profile() &&
             identity.entry == fixture.entry() &&
             identity.revisionDigest == revisionDigest &&
             identity.configurationDigest == fixture.configurationDigest(),
         "session identity retains the exact immutable activation identity");

  const auto frame = created.session->prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(frame.ready() && frame.evaluation.interactionLayout.has_value(),
         "fresh activation session enters render phase and prepares a frame");
  if (frame.evaluation.interactionLayout) {
    const auto &transform = frame.evaluation.interactionLayout->uiToAuthored;
    expect(transform.m00 == 2.0 && transform.m11 == -2.0 &&
               transform.tx == -660.0 && transform.ty == 1040.0,
           "context custom viewport is the frame's exact authored transform");
  }
  expect(weakRevision.hasLiveLease(),
         "session and uploaded catalog retain immutable revision pins");
  expect(fixture.liveCounters()->snapshot() ==
             SkinLiveResourceSnapshot{.liveTextures = 0,
                                      .liveResources = 1},
         "a successful resource-free session owns one published catalog graph");
  created.session.reset();
  expect(!weakRevision.hasLiveLease() &&
             fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{},
         "final session teardown releases every revision pin and counter");
}

void testConfiguredLoadUsesTheInitializedAuthoritativeState() {
  ActivationFixture fixture({.requireConfiguredState = true});
  if (!fixture.ready()) {
    return;
  }
  const auto created =
      PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(created.session != nullptr && created.diagnostics.empty(),
         "configured Lua load receives the initialized authoritative state");
}

void testLuaSessionCapturesLegacyInputAtEachAuthoritativeBoundary() {
  ActivationFixture fixture({.legacyInputBearing = true});
  if (!fixture.ready()) {
    return;
  }
  int captures = 0;
  auto context = fixture.context();
  context.captureLegacyInputGeneration = [&captures] {
    ++captures;
    LuaSkinLegacyInputGeneration generation;
    generation.drawableWidth = captures == 1 ? 640 : 800;
    generation.drawableHeight = captures == 1 ? 360 : 450;
    generation.pressedGdxKeys.set(29);
    return generation;
  };
  auto created =
      PlaySkinSession::create(fixture.takeActivation(), std::move(context));
  expect(created.session != nullptr && created.diagnostics.empty() &&
             captures == 1,
         "configured Lua load captures legacy input with its initial main_state boundary");
  if (!created.session) {
    return;
  }
  const auto frame = created.session->prepareFrame(
      stateAt(2), projectionAt(2), {});
  expect(frame.ready() && captures == 2,
         "each later Lua frame replaces the legacy-input snapshot at the same boundary");
}

void testRepeatedPomyuObjectsShareCyclePreparation() {
  ActivationFixture fixture({.repeatedPomyu = true});
  if (!fixture.ready()) {
    return;
  }
  resetPomyuCyclePreparationCountersForTesting();
  const auto created =
      PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(created.session != nullptr &&
             pomyuCycleFileReadsForTesting() == 1 &&
             pomyuRequirementParsesForTesting() == 1 &&
             pomyuCycleParsesForTesting() == 1,
         "repeated Pomyu objects share one bounded CHP read, prerequisite "
         "scan, and cycle parse");
}

void testMalformedPomyuNumericDirectivesAbortTheCp932Character() {
  for (const auto malformed : {
           MalformedPomyuNumeric::Anime,
           MalformedPomyuNumeric::Frame,
           MalformedPomyuNumeric::Size,
           MalformedPomyuNumeric::Coordinate,
           MalformedPomyuNumeric::Motion,
           MalformedPomyuNumeric::Loop,
           MalformedPomyuNumeric::FaceRectangle,
       }) {
    ActivationFixture fixture({.malformedPomyuNumeric = malformed});
    if (!fixture.ready()) {
      continue;
    }
    resetPomyuCyclePreparationCountersForTesting();
    auto created =
        PlaySkinSession::create(fixture.takeActivation(), fixture.context());
    if (!created.session) {
      expect(false, "malformed optional Pomyu metadata preserves the session");
      continue;
    }
    const auto frame = created.session->prepareFrame(
        stateAt(1), projectionAt(1), {});
    const bool drewPomyu =
        frame.evaluation.submitReady &&
        std::ranges::any_of(frame.evaluation.submitReady->commands,
                            [](const SkinDrawCommand &command) {
                              return std::holds_alternative<
                                  SkinTexturedQuadCommand>(command.payload);
                            });
    expect(frame.ready() && !drewPomyu &&
               pomyuCycleParsesForTesting() == 0,
           "a malformed numeric Anime/Frame/Size/coordinate/motion/loop/face "
           "field aborts the CP932 CHP before it prepares or draws");
  }
}

void testExplicitOversizedPomyuDoesNotFallBackToSibling() {
  ActivationFixture fixture({.oversizedPomyuWithSibling = true});
  if (!fixture.ready()) {
    return;
  }
  resetPomyuCyclePreparationCountersForTesting();
  (void)PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(pomyuCycleFileReadsForTesting() == 1 &&
             pomyuCycleParsesForTesting() == 0,
         "an oversized explicit CHP does not silently borrow a sibling's "
         "Pomyu timing metadata");
}

void testIncompletePomyuResourcesKeepDefaultCycles() {
  for (const ActivationFixtureOptions options : {
           ActivationFixtureOptions{.pomyuMissingCharBmp = true},
           ActivationFixtureOptions{.pomyuTextureMissingCharTex = true}}) {
    ActivationFixture fixture(options);
    if (!fixture.ready()) {
      continue;
    }
    resetPomyuCyclePreparationCountersForTesting();
    (void)PlaySkinSession::create(fixture.takeActivation(), fixture.context());
    expect(pomyuCycleFileReadsForTesting() == 1 &&
               pomyuCycleParsesForTesting() == 0,
           "Pomyu cycle metadata is ignored when its upstream character "
           "image prerequisites cannot load");
  }
}

void testPomyuResourcesUseMs932AndWindowsSeparators() {
  ActivationFixture fixture({.pomyuCp932BackslashPath = true});
  if (!fixture.ready()) {
    return;
  }
  resetPomyuCyclePreparationCountersForTesting();
  const auto created =
      PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(created.session != nullptr &&
             pomyuRequirementParsesForTesting() == 1 &&
             pomyuCycleParsesForTesting() == 1,
         "Pomyu character prerequisites resolve MS932 names and Windows "
         "path separators without corrupting multibyte trail bytes");
}

void testPomyuRootedResourcePathIsRejected() {
  ActivationFixture fixture({.pomyuRootedResourcePath = true});
  if (!fixture.ready()) {
    return;
  }
  resetPomyuCyclePreparationCountersForTesting();
  const auto created =
      PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(created.session != nullptr && pomyuCycleParsesForTesting() == 0,
         "a rooted CHP image value cannot escape the Pomyu character "
         "directory or apply unrelated timing metadata");
}

void testPomyuLeadingBackslashPathRemainsCharacterRelative() {
  ActivationFixture fixture({.pomyuLeadingBackslashPath = true});
  if (!fixture.ready()) {
    return;
  }
  resetPomyuCyclePreparationCountersForTesting();
  const auto created =
      PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(created.session != nullptr && pomyuCycleParsesForTesting() == 1,
         "a leading CHP backslash stays relative to the Pomyu character "
         "directory like the pinned loader");
}

void testPomyuPreparationSelectsSecondPlayerTexturesAndStaticFallbacks() {
  for (const bool hasSecondPlayerTexture : {true, false}) {
    ActivationFixture fixture(
        hasSecondPlayerTexture
            ? ActivationFixtureOptions{.pomyuSecondPlayerTextures = true}
            : ActivationFixtureOptions{
                  .pomyuSecondPlayerTextureFallback = true});
    if (!fixture.ready()) {
      continue;
    }
    const auto created =
        PlaySkinSession::create(fixture.takeActivation(), fixture.context());
    expect(created.session != nullptr,
           "Pomyu primary/2P/Texture fixture creates a session");
    if (!created.session) {
      continue;
    }
    auto state = stateAt(1);
    state.sceneStartMicros = 0;
    state.clock.playTimer = {.active = true,
                             .startMicros = 0,
                             .elapsedMillisExact = true};
    const auto frame =
        created.session->prepareFrame(state, projectionAt(1), {});
    expect(frame.ready(),
           "prepared Pomyu resources render without frame file access");
    if (!frame.evaluation.submitReady) {
      continue;
    }
    std::vector<SkinResourceId> resources;
    for (const auto &command : frame.evaluation.submitReady->commands) {
      if (const auto *quad =
              std::get_if<SkinTexturedQuadCommand>(&command.payload)) {
        resources.push_back(quad->resource);
        expect(quad->vertices[0].x != quad->vertices[1].x &&
                   quad->vertices[0].y != quad->vertices[3].y,
               "animated and static Pomyu frames retain non-empty "
               "destination geometry");
      }
    }
    const std::vector<SkinResourceId> expected =
        hasSecondPlayerTexture
            ? std::vector<SkinResourceId>{3, 5, 6, 7, 3}
            : std::vector<SkinResourceId>{2, 4, 5, 6, 2};
    expect(resources == expected,
           "Pomyu uses 2P BMP+Texture only as a complete pair and falls "
           "missing 2P face/select images back to primary resources");
  }
}

void testRequestedExternalGameplaySkinCreatesARealSession() {
  const char *configuredRoot =
      std::getenv("ASOBMASHOW_EXTERNAL_GAMEPLAY_SKIN_ROOT");
  if (configuredRoot == nullptr || *configuredRoot == '\0') {
    return;
  }
  const fs::path source(configuredRoot);
  expect(fs::is_directory(source),
         "requested external gameplay skin root is a readable directory");
  if (!fs::is_directory(source)) {
    return;
  }
  const char *configuredEntry =
      std::getenv("ASOBMASHOW_EXTERNAL_GAMEPLAY_SKIN_ENTRY");
  const std::string entryPath =
      configuredEntry != nullptr && *configuredEntry != '\0'
          ? configuredEntry
          : "play7.luaskin";

  TempDirectory temp;
  const SkinStorageRoots roots{
      .visiblePackages = temp.root() / "visible",
      .privateRevisions = temp.root() / "revisions",
      .privateCatalog = temp.root() / "catalog",
      .profileOverlays = temp.root() / "overlays",
  };
  const auto package = normalizePackageId("ExternalGameplaySkin").package;
  const auto entry = package ? normalizeEntryPath(*package, entryPath).entry
                             : std::nullopt;
  const auto profile =
      makeSkinProfileId("77777777-7777-4777-8777-777777777777");
  expect(package && entry && profile,
         "requested external gameplay source has portable activation IDs");
  if (!package || !entry || !profile) {
    return;
  }

  AcceptFiles aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(source, *package, {}, {});
  expect(snapshot.prepared.has_value(),
         "requested external gameplay package snapshots for a real session");
  if (!snapshot.prepared) {
    return;
  }
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease.has_value() && publishError.empty(),
         "requested external gameplay revision publishes for a real session");
  if (!lease) {
    return;
  }

  SkinResourcePreparationService resources;
  GameplaySkinValidator validator(resources);
  const auto validation = validator.validate(lease->readView(), *entry, nullptr, {});
  expect(validation.disposition == SkinValidationDisposition::Selectable7Key &&
             validation.reconciledSettings.has_value() &&
             !validation.configurationDigest.empty(),
         "requested external gameplay skin is selectable before session creation");
  if (validation.disposition != SkinValidationDisposition::Selectable7Key ||
      !validation.reconciledSettings || validation.configurationDigest.empty()) {
    return;
  }

  PlayfieldChartVisualModel chart;
  chart.keyCount = 7;
  chart.text = {.title = "external title",
                .artist = "external artist",
                .fullArtist = "external artist"};
  PlayfieldVisualState initialState = stateAt(1);
  initialState.authority.loadingState = PlayfieldLoadingState::Loaded;
  const PlayfieldProjectionResult initialProjection = projectionAt(1);
  SkinConfigurationWriteQueue configurationWrites;
  auto device = std::make_shared<SessionTextureDevice>();
  auto counters = std::make_shared<SkinLiveResourceCounters>();
  auto created = PlaySkinSession::create(
      {.revision = std::move(*lease),
       .entry = *entry,
       .reconciledSettings = *validation.reconciledSettings,
       .configurationDigest = validation.configurationDigest},
      {.sessionSerial = 92,
       .profileId = *profile,
       .chartModel = chart,
       .initialState = &initialState,
       .initialProjection = &initialProjection,
       .safeUiBounds = {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
       .storageRoots = roots,
       .resourcePreparation = resources,
       .textureDevice = std::move(device),
       .liveResourceCounters = std::move(counters),
       .configurationWrites = configurationWrites});
  if (!created.session) {
    for (const auto &diagnostic : created.diagnostics) {
      std::cerr << "external gameplay session diagnostic: " << diagnostic.code
                << ": " << diagnostic.message << " • "
                << diagnostic.virtualPath << '\n';
    }
  }
  const bool hasError = std::ranges::any_of(
      created.diagnostics, [](const SkinDiagnostic &diagnostic) {
        return diagnostic.severity == DiagnosticSeverity::Error;
      });
  expect(created.session != nullptr && !created.cancelled && !hasError,
         "requested external gameplay skin creates a full configured session; "
         "unsupported optional visuals may remain visible as warnings");
}

void testActivationRejectsAReconciledDigestMismatch() {
  ActivationFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  activation.configurationDigest.assign(64, '0');
  const auto expectedDigest = fixture.configurationDigest();

  auto created =
      PlaySkinSession::create(std::move(activation), fixture.context());
  expect(!created.session && !created.cancelled &&
             created.configurationDigest == expectedDigest &&
             hasDiagnostic(created.diagnostics,
                           "skin.session.configuration_digest_mismatch") &&
             !fs::exists(fixture.configuredMarkerPath()),
         "digest mismatch rejects before configured-phase sandbox writes");
}

void testResourceSessionOwnsUploadsAndExactRuntimeStringAtlas() {
  ActivationFixture fixture({.resourceBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  const auto weakRevision = activation.revision.weakPin();
  fixture.device()->observeRevision(weakRevision);

  auto created =
      PlaySkinSession::create(std::move(activation), fixture.context());
  expect(created.session && fixture.device()->createCalls == 2 &&
             fixture.device()->destroyCalls == 0 &&
             fixture.device()->wrongThreadOperations == 0 &&
             weakRevision.hasLiveLease() &&
             isCompleteSkinLoadingTelemetry(created.loadingTelemetry) &&
             created.loadingTelemetry.resources.imageDecodes == 1 &&
             created.loadingTelemetry.resources.fontDecodes == 1 &&
             created.loadingTelemetry.resources.movieDecodes == 0 &&
             created.loadingTelemetry.resources.audioDecodes == 0 &&
             created.loadingTelemetry.resources.textureUploads == 2,
         "session owns one image and one runtime-string glyph atlas upload");
  expect(fixture.liveCounters()->snapshot() ==
             SkinLiveResourceSnapshot{.liveTextures = 2,
                                      .liveResources = 1},
         "session ownership exposes only its two unique physical textures");
  if (!created.session) {
    return;
  }
  const auto frame = created.session->prepareFrame(
      stateAt(1), projectionAt(1), {});
  const auto textCommand =
      frame.evaluation.submitReady
          ? std::ranges::find_if(
                frame.evaluation.submitReady->commands,
                [](const SkinDrawCommand &command) {
                  return std::holds_alternative<SkinGlyphRunCommand>(
                      command.payload);
                })
          : std::vector<SkinDrawCommand>::const_iterator{};
  const bool emittedTitle =
      frame.evaluation.submitReady &&
      textCommand != frame.evaluation.submitReady->commands.end() &&
      std::get<SkinGlyphRunCommand>(textCommand->payload).glyphs.size() == 12;
  expect(frame.ready() && emittedTitle,
         "title selector emits one runtime text command with every glyph");

  created.session.reset();
  expect(fixture.device()->destroyCalls == 2 &&
             fixture.device()->wrongThreadOperations == 0 &&
             fixture.device()->revisionLiveDuringDestroy &&
             !weakRevision.hasLiveLease() &&
             fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{},
         "catalog textures tear down on owner thread before the final revision "
         "pin releases");
}

void testPostUploadCancellationRollsBackResourcesOnOwnerThread() {
  ActivationFixture fixture({.resourceBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  const auto weakRevision = activation.revision.weakPin();
  fixture.device()->observeRevision(weakRevision);
  std::stop_source stop;
  fixture.device()->requestStopAfter(2, stop);

  auto created = PlaySkinSession::create(
      std::move(activation), fixture.context({}, stop.get_token()));
  expect(created.cancelled && !created.session && fixture.device()->createCalls == 2 &&
             fixture.device()->destroyCalls == 2 &&
             fixture.device()->wrongThreadOperations == 0 &&
             fixture.device()->revisionLiveDuringDestroy &&
             !weakRevision.hasLiveLease() &&
             fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{} &&
             created.loadingTelemetry.cancelled &&
             !created.loadingTelemetry.sessionPublished &&
             !isCompleteSkinLoadingTelemetry(created.loadingTelemetry),
         "post-upload cancellation destroys both owner-thread textures and "
         "releases the final revision pin");
}

void testPreparedSessionRunsFiveHundredFramesWithoutLoadingAgain() {
  ActivationFixture fixture(
      {.resourceBearing = true, .movieBearing = true, .audioBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto created =
      PlaySkinSession::create(fixture.takeActivation(), fixture.context());
  expect(created.session &&
             created.loadingTelemetry.resources.imageDecodes == 1 &&
             created.loadingTelemetry.resources.fontDecodes == 1 &&
             created.loadingTelemetry.resources.movieDecodes == 1 &&
             created.loadingTelemetry.resources.audioDecodes == 1 &&
             created.loadingTelemetry.resources.textureUploads == 2,
         "combined fixture records each prepared resource kind exactly once");
  if (!created.session) {
    return;
  }
  expect(created.session->preparedTextureCountForTesting() ==
                 created.loadingTelemetry.resources.textureUploads &&
             !created.session->modelForTesting().model.objects.empty(),
         "an owning session exposes only test-scoped model and materialized "
         "texture acceptance facts");
  const auto resourceEvidence =
      created.session->resourcePreparationEvidenceForTesting();
  const auto exhaustiveReferences = skinModelReferencedResourceIdsForTesting(
      created.session->modelForTesting(), false);
  expect(resourceEvidence.referencedImageResourceIds ==
                 exhaustiveReferences.images &&
             resourceEvidence.referencedTextObjectIds ==
                 exhaustiveReferences.textObjects &&
             skinResourcePreparationEvidenceCompleteForTesting(
                 resourceEvidence) &&
             resourceEvidence.referencedImageResourceIds ==
                 resourceEvidence.preparedImageResourceIds &&
             resourceEvidence.referencedTextObjectIds ==
                 resourceEvidence.preparedTextObjectIds &&
             !resourceEvidence.referencedImageResourceIds.empty() &&
             !resourceEvidence.referencedTextObjectIds.empty(),
         "resource completeness compares independent model references with "
         "prepared image/movie and text-atlas identities");
  auto omittedPlannerResource = resourceEvidence;
  omittedPlannerResource.preparedImageResourceIds.pop_back();
  expect(!skinResourcePreparationEvidenceCompleteForTesting(
             omittedPlannerResource),
         "independent model traversal detects one planner resource omission");
  const auto textureCreates = fixture.device()->createCalls;
  const auto movieLoads = fixture.movieDevice()->loadCalls;
  const auto audioLoads = fixture.audioState()->loads.size();
  const auto fileActivity =
      created.session->fileActivityCountersForTesting();
  bool framesReady = true;
  for (std::uint64_t serial = 2; serial <= 501; ++serial) {
    const auto frame = created.session->prepareFrame(
        stateAt(serial), projectionAt(serial), {});
    framesReady = framesReady && frame.ready();
  }
  expect(framesReady && fixture.device()->createCalls == textureCreates &&
             fixture.movieDevice()->loadCalls == movieLoads &&
             fixture.audioState()->loads.size() == audioLoads &&
             fileActivity.readsPerformed > 0 &&
             created.session->fileActivityCountersForTesting().readsPerformed ==
                 fileActivity.readsPerformed &&
             created.session->fileActivityCountersForTesting()
                     .renderReadsPerformed == 0 &&
             created.session->fileActivityCountersForTesting()
                     .renderDirectoryScansPerformed == 0,
         "five hundred evaluated frames perform no image/font/movie/audio "
         "decode or upload after preparation");
  expect(created.session->callbackWallMicrosForTesting() <=
             static_cast<std::uint64_t>(
                 LuaRuntimePolicy::gameplayFrame.maxWallTime.count()) *
                 1'000U,
         "the test-scoped callback measurement stays within the production "
         "gameplay-frame budget");
  const auto active = fixture.liveCounters()->snapshot();
  expect(active.liveTextures == 2 && active.liveResources == 1 &&
             active.liveCpuPixmaps == 0 && active.liveMovies == 1 &&
             active.liveMovieBytes > 0 && active.liveAudioIdentities == 1,
         "published session exposes CPU/GPU/movie/audio ownership by kind");
  created.session.reset();
  expect(fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{},
         "combined session teardown returns every resource kind to baseline");
}

void testInvalidViewportRollsBackUploadedResourcesOnOwnerThread() {
  ActivationFixture fixture({.resourceBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto activation = fixture.takeActivation();
  const auto weakRevision = activation.revision.weakPin();
  fixture.device()->observeRevision(weakRevision);
  auto context = fixture.context();
  context.safeUiBounds.width = 0.0;

  auto created =
      PlaySkinSession::create(std::move(activation), std::move(context));
  expect(!created.session &&
             hasDiagnostic(created.diagnostics,
                           "skin.session.viewport_invalid") &&
             fixture.device()->createCalls == 2 &&
             fixture.device()->destroyCalls == 2 &&
             fixture.device()->wrongThreadOperations == 0 &&
             fixture.device()->revisionLiveDuringDestroy &&
             !weakRevision.hasLiveLease() &&
             fixture.liveCounters()->snapshot() == SkinLiveResourceSnapshot{},
         "post-upload viewport failure rolls back resources and releases all "
         "revision pins on the owner thread");
}

void testActivationCancellationAndZeroSerialDoNotPublishSessions() {
  {
    ActivationFixture fixture;
    if (fixture.ready()) {
      std::stop_source stop;
      stop.request_stop();
      auto created = PlaySkinSession::create(
          fixture.takeActivation(), fixture.context({}, stop.get_token()));
      expect(!created.session && created.cancelled,
             "pre-cancelled activation publishes no session");
    }
  }
  {
    ActivationFixture fixture;
    if (fixture.ready()) {
      auto context = fixture.context();
      context.sessionSerial = 0;
      auto created =
          PlaySkinSession::create(fixture.takeActivation(), std::move(context));
      expect(!created.session && !created.cancelled &&
                 hasDiagnostic(created.diagnostics,
                               "skin.session.serial_invalid"),
             "zero session serial is rejected before runtime publication");
    }
  }
  {
    ActivationFixture fixture({.resourceBearing = true});
    if (fixture.ready()) {
      auto context = fixture.context();
      context.liveResourceCounters.reset();
      auto created = PlaySkinSession::create(fixture.takeActivation(),
                                              std::move(context));
      expect(!created.session && !created.cancelled &&
                 fixture.device()->createCalls == 0 &&
                 hasDiagnostic(created.diagnostics,
                               "skin.session.live_resource_counters_missing"),
             "a missing app-owned live-resource counter fails closed before "
             "uploads");
    }
  }
}

const SessionPresentationWrite *presentationMutation(
    const SkinFrameMutation &mutation) {
  return std::get_if<SessionPresentationWrite>(&mutation);
}

class SessionFixture final {
public:
  explicit SessionFixture(
      std::uint64_t sessionSerial = 37,
      UiLogicalRect safeUiBounds =
          {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0})
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("SessionContract").package),
        entry_(*normalizeEntryPath(package_, "skin/main.luaskin").entry),
        profile_(*makeSkinProfileId(
            "55555555-5555-4555-8555-555555555555")),
        viewport_(evaluatePlaySkinViewport(
            {.width = 1280.0, .height = 720.0},
            safeUiBounds, {})),
        quadRenderer_(quadBackend_) {
    chart_.keyCount = 7;
    chart_.laneOrder = {7, 0};
    const fs::path source = temp_.root() / "source";
    writeText(source / "skin/main.luaskin", R"lua(
local captured_main_state = require("main_state")
return {
  type = 0,
  writer_a = function(value)
    captured_main_state.event_exec(900, math.floor(value * 100 + 0.5))
  end,
  writer_drag = function(value)
    _G.session_writer_drag_values =
      (rawget(_G, "session_writer_drag_values") or "") ..
      math.floor(value * 100 + 0.5) .. ","
    captured_main_state.event_exec(900, math.floor(value * 100 + 0.5))
  end,
  writer_drag_value = function()
    return 0.5
  end,
  writer_drag_values = function()
    return rawget(_G, "session_writer_drag_values") or ""
  end,
  writer_b = function(value)
    captured_main_state.event_exec(901, math.floor(value * 100 + 0.5))
  end,
  writer_fail = function()
    captured_main_state.event_exec(900, 99)
    error("forced writer failure")
  end,
  writer_once = function(value)
    _G.session_writer_once_count =
      (rawget(_G, "session_writer_once_count") or 0) + 1
    if _G.session_writer_once_count ~= 1 then
      error("writer queued more than once for one Down")
    end
    captured_main_state.event_exec(900, math.floor(value * 100 + 0.5))
  end,
  writer_once_verify = function()
    _G.session_writer_verify_count =
      (rawget(_G, "session_writer_verify_count") or 0) + 1
    if _G.session_writer_verify_count >= 2 and
       (rawget(_G, "session_writer_once_count") or 0) ~= 1 then
      error("one Down did not queue exactly one writer")
    end
    return 0.5
  end,
  text_writer_utf8 = function(value)
    _G.session_text_utf8_count =
      (rawget(_G, "session_text_utf8_count") or 0) + 1
    if _G.session_text_utf8_count ~= 1 or value ~= "A한" then
      error("UTF-8 text writer received the wrong exact-once value")
    end
  end,
  text_writer_first = function(value)
    if (rawget(_G, "session_text_transfer_order") or 0) ~= 0 or
       value ~= "A1" then
      error("first transferred text writer was not ordered")
    end
    _G.session_text_transfer_order = 1
  end,
  text_writer_second = function(value)
    if (rawget(_G, "session_text_transfer_order") or 0) ~= 1 or
       value ~= "B2" then
      error("second transferred text writer was not ordered")
    end
    _G.session_text_transfer_order = 2
  end,
  text_writer_cancel = function()
    _G.session_text_cancel_count =
      (rawget(_G, "session_text_cancel_count") or 0) + 1
    error("cancelled text writer was invoked")
  end,
  text_cancel_verify = function()
    if (rawget(_G, "session_text_cancel_count") or 0) ~= 0 then
      error("cancelled text edit escaped teardown")
    end
    return true
  end,
  text_utf8_count = function()
    return rawget(_G, "session_text_utf8_count") or 0
  end,
  interaction_string = function(value)
    _G.session_interaction_order =
      (rawget(_G, "session_interaction_order") or "") .. "string,"
    captured_main_state.event_exec(900, 11)
  end,
  interaction_event = function()
    _G.session_interaction_order =
      (rawget(_G, "session_interaction_order") or "") .. "event,"
    captured_main_state.event_exec(900, 22)
  end,
  interaction_float = function()
    _G.session_interaction_order =
      (rawget(_G, "session_interaction_order") or "") .. "float,"
    captured_main_state.event_exec(900, 33)
  end,
  interaction_order = function()
    return rawget(_G, "session_interaction_order") or ""
  end,
}
)lua");
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    expect(snapshot.prepared.has_value(), "session runtime fixture snapshots");
    if (!snapshot.prepared) {
      return;
    }
    prepared_.emplace(std::move(*snapshot.prepared));
    auto fileSystem = LuaSkinFileSystem::create(
        {.revision = prepared_->readView(),
         .entry = entry_,
         .storageRoots = roots_,
         .profileId = profile_});
    expect(fileSystem.fileSystem != nullptr,
           "session runtime filesystem creates");
    if (!fileSystem.fileSystem) {
      return;
    }
    auto created = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Gameplay,
         .fileSystem = std::move(fileSystem.fileSystem)});
    runtime_ = std::move(created.runtime);
    expect(runtime_ != nullptr, "session gameplay runtime creates");
    if (!runtime_) {
      return;
    }
    auto header = runtime_->loadHeader();
    expect(header.value.has_value(), "session runtime header executes");
    if (!header.value) {
      return;
    }
    writerA_ = header.value->callbackNamed("writer_a");
    writerDrag_ = header.value->callbackNamed("writer_drag");
    writerDragValue_ = header.value->callbackNamed("writer_drag_value");
    writerDragValues_ = header.value->callbackNamed("writer_drag_values");
    writerB_ = header.value->callbackNamed("writer_b");
    writerFail_ = header.value->callbackNamed("writer_fail");
    writerOnce_ = header.value->callbackNamed("writer_once");
    writerOnceVerify_ = header.value->callbackNamed("writer_once_verify");
    textWriterUtf8_ = header.value->callbackNamed("text_writer_utf8");
    textWriterFirst_ = header.value->callbackNamed("text_writer_first");
    textWriterSecond_ = header.value->callbackNamed("text_writer_second");
    textWriterCancel_ = header.value->callbackNamed("text_writer_cancel");
    textCancelVerify_ = header.value->callbackNamed("text_cancel_verify");
    textUtf8Count_ = header.value->callbackNamed("text_utf8_count");
    interactionString_ = header.value->callbackNamed("interaction_string");
    interactionEvent_ = header.value->callbackNamed("interaction_event");
    interactionFloat_ = header.value->callbackNamed("interaction_float");
    interactionOrder_ = header.value->callbackNamed("interaction_order");
    expect(writerA_ && writerDrag_ && writerDragValue_ && writerDragValues_ &&
               writerB_ &&
               writerFail_ && writerOnce_ &&
               writerOnceVerify_ && textWriterUtf8_ && textWriterFirst_ &&
               textWriterSecond_ && textWriterCancel_ && textCancelVerify_ &&
               textUtf8Count_ && interactionString_ && interactionEvent_ &&
               interactionFloat_ && interactionOrder_,
           "session writer callbacks are retained");
    header.value.reset();
    expect(runtime_->loadConfigured({}).value.has_value(),
           "session runtime configured phase executes");
    expect(runtime_->enterRenderPhase().ok,
           "session runtime enters render phase");

    model_.model.floatWriters = {
        {.id = SkinFloatWriterId{1}, .source = *writerA_},
        {.id = SkinFloatWriterId{2}, .source = *writerB_},
        {.id = SkinFloatWriterId{3}, .source = *writerFail_},
        {.id = SkinFloatWriterId{4}, .source = *writerOnce_},
        {.id = SkinFloatWriterId{5}, .source = *interactionFloat_},
        {.id = SkinFloatWriterId{6}, .source = *writerDrag_},
    };
    model_.model.stringWriters = {
        {.id = SkinStringWriterId{1}, .source = *textWriterUtf8_},
        {.id = SkinStringWriterId{2}, .source = *textWriterFirst_},
        {.id = SkinStringWriterId{3}, .source = *textWriterSecond_},
        {.id = SkinStringWriterId{4}, .source = *textWriterCancel_},
        {.id = SkinStringWriterId{5}, .source = *interactionString_},
    };
    model_.model.header.width = 1280;
    model_.model.header.height = 720;
    auto pinned = makePinnedSkinEventMutationTableV1();
    std::vector<SkinEventMutationRule> rules(pinned.rules().begin(),
                                              pinned.rules().end());
    rules.push_back({.builtInEventId = 900,
                     .kind = SkinEventMutationKind::SessionPresentation,
                     .maximumArguments = 2});
    rules.push_back({.builtInEventId = 901,
                     .kind = SkinEventMutationKind::SessionPresentation,
                     .maximumArguments = 2});
    mutations_ = SkinEventMutationTable(std::move(rules));
    bridge_ = std::make_unique<PlaySkinStateBridge>(PlaySkinStateBridgeContext{
        .chartModel = chart_,
        .model = &model_,
        .configuration = configuration_,
        .runtime = runtime_.get(),
        .mutationTable = mutations_});
    session_ = std::make_unique<PlaySkinSession>(PlaySkinSessionFrameContext{
        .sessionSerial = sessionSerial,
        .identity = {.sessionSerial = sessionSerial,
                     .profileId = profile_,
                     .entry = entry_,
                     .revisionDigest = "session-revision",
                     .configurationDigest = "session-configuration"},
        .chartModel = chart_,
        .model = model_,
        .configuration = configuration_,
        .resources = resources_,
        .viewportSettings = {},
        .viewport = viewport_,
        .runtime = runtime_.get(),
        .bridge = *bridge_,
        .renderer = renderer_,
        .quadRenderer = quadRenderer_,
        .configurationWrites = configurationWrites_,
        .applyAudioVolume = [this](SkinAudioVolumeWriterTarget target,
                                   float value) {
          audioVolumeWrites_.emplace_back(target, value);
        },
        .applyPracticeItemScroll = [this](float position) {
          practiceItemScrollWrites_.push_back(position);
        },
        .applyPracticeMenuItem = [this](std::size_t index, bool increment) {
          practiceMenuItemWrites_.emplace_back(index, increment);
        },
        .applyPracticeVisibleItems = [this](int count) {
          practiceVisibleItemWrites_.push_back(count);
        }});
  }

  bool ready() const noexcept { return session_ != nullptr; }
  PlaySkinSession &session() { return *session_; }
  PlaySkinStateBridge &bridge() { return *bridge_; }
  LuaSkinRuntime &runtime() { return *runtime_; }
  Skin2DRenderer &renderer() { return renderer_; }
  ValidatedBeatorajaSkinModel &model() { return model_; }
  const BeatorajaSkinConfiguration &configuration() const {
    return configuration_;
  }
  const SessionResources &resources() const { return resources_; }
  const PlaySkinViewport &viewport() const { return viewport_; }
  SessionQuadBackend &quadBackend() { return quadBackend_; }
  SkinConfigurationWriteQueue &configurationWrites() {
    return configurationWrites_;
  }
  const std::vector<std::pair<SkinAudioVolumeWriterTarget, float>> &
  audioVolumeWrites() const {
    return audioVolumeWrites_;
  }
  const std::vector<float> &practiceItemScrollWrites() const {
    return practiceItemScrollWrites_;
  }
  const std::vector<std::pair<std::size_t, bool>> &practiceMenuItemWrites()
      const {
    return practiceMenuItemWrites_;
  }
  const std::vector<int> &practiceVisibleItemWrites() const {
    return practiceVisibleItemWrites_;
  }

  void addBgaMarker(std::uint32_t ordinal = 90) {
    model_.model.objects.push_back(
        {.id = 90,
         .authoredName = "session-bga",
         .payload = SkinBgaObject{},
         .authoredOrdinal = ordinal,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 90,
         .presentation =
             {.loop = 0,
              .frames = {{.timeMillis = 0,
                          .x = 100.0,
                          .y = 100.0,
                          .width = 640.0,
                          .height = 360.0}},
              .authoredOrdinal = ordinal}});
  }

  void addTouchGeometry(
      SkinFloatWriterId writer = SkinFloatWriterId{1},
      std::optional<double> firstLaneSecondaryDestinationY = std::nullopt,
      double destinationX = 100.0) {
    resources_.addImage(80);
    const bool dynamicValue = writer == SkinFloatWriterId{4} ||
                              writer == SkinFloatWriterId{6};
    const SkinFloatPropertyId valueProperty{dynamicValue ? 2U : 1U};
    const std::variant<SkinBuiltinPropertySelector, LuaCallbackId> valueSource =
        writer == SkinFloatWriterId{4}
            ? std::variant<SkinBuiltinPropertySelector, LuaCallbackId>{
                  *writerOnceVerify_}
            : (writer == SkinFloatWriterId{6}
                   ? std::variant<SkinBuiltinPropertySelector, LuaCallbackId>{
                         *writerDragValue_}
                   : std::variant<SkinBuiltinPropertySelector, LuaCallbackId>{
                         SkinBuiltinPropertySelector{.value = 4}});
    model_.model.floatProperties.push_back(
        {.id = valueProperty,
         .domain = SkinFloatPropertyDomain::Rate,
         .source = valueSource,
         .authoredOrdinal = 1});
    SkinSliderObject slider;
    slider.knob = {.resource = 80,
                   .frames = {{.x = 0, .y = 0, .w = 10, .h = 10}}};
    slider.value = valueProperty;
    slider.writer = writer;
    slider.direction = 1;
    slider.range = 100.0;
    slider.changeable = true;
    SkinNoteObject notes;
    notes.lanes = {
        {.authoredLane = 7,
         .laneDestination = {.x = 100.0,
                             .y = 20.0,
                             .width = 80.0,
                             .height = 500.0},
         .secondaryDestinationY = firstLaneSecondaryDestinationY},
        {.authoredLane = 0,
         .laneDestination = {.x = 200.0,
                             .y = 20.0,
                             .width = 100.0,
                             .height = 500.0}},
    };
    model_.model.objects.push_back(
        {.id = 80,
         .authoredName = "session-slider",
         .payload = std::move(slider),
         .authoredOrdinal = 80,
         .critical = true});
    model_.model.objects.push_back(
        {.id = 81,
         .authoredName = "session-notes",
         .payload = std::move(notes),
         .authoredOrdinal = 81,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 80,
         .presentation =
             {.loop = 0,
              .frames = {{.timeMillis = 0,
                          .x = destinationX,
                          .y = 100.0,
                          .width = 20.0,
                          .height = 20.0}},
              .authoredOrdinal = 800}});
  }

  void addReplayGhostNoteGeometry() {
    const auto addVisuals = [](SkinLaneNotePresentation &lane) {
      constexpr std::array kinds{
          SkinNoteVisualKind::Normal,
          SkinNoteVisualKind::LnEnd,
          SkinNoteVisualKind::LnStart,
          SkinNoteVisualKind::LnBodyActive,
          SkinNoteVisualKind::LnBodyInactive,
          SkinNoteVisualKind::HcnEnd,
          SkinNoteVisualKind::HcnStart,
          SkinNoteVisualKind::HcnBodyActive,
          SkinNoteVisualKind::HcnBodyInactive,
          SkinNoteVisualKind::HcnDamage,
          SkinNoteVisualKind::HcnReactive,
          SkinNoteVisualKind::Mine,
          SkinNoteVisualKind::Hidden,
          SkinNoteVisualKind::Processed,
      };
      for (const auto kind : kinds) {
        lane.visuals.emplace(kind, SkinSynthesizedNoteVisual{.kind = kind});
      }
    };
    SkinNoteObject notes;
    notes.lanes = {
        {.authoredLane = 7,
         .laneDestination = {.x = 10.0,
                             .y = 40.0,
                             .width = 30.0,
                             .height = 200.0},
         .authoredNoteHeight = 8.0},
        {.authoredLane = 0,
         .laneDestination = {.x = 80.0,
                             .y = 160.0,
                             .width = 54.0,
                             .height = 400.0},
         .authoredNoteHeight = 17.0},
    };
    addVisuals(notes.lanes[0]);
    addVisuals(notes.lanes[1]);
    model_.model.objects.push_back(
        {.id = 83,
         .authoredName = "session-replay-ghost-notes",
         .payload = std::move(notes),
         .authoredOrdinal = 83,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 83, .presentation = {.authoredOrdinal = 830}});
  }

  void addClickableImage() {
    resources_.addImage(82);
    model_.model.events.push_back(
        {.id = SkinEventBindingId{1},
         .source = SkinBuiltinPropertySelector{.value = 900},
         .authoredOrdinal = 1});
    SkinImageObject image;
    image.orderedStates = {{.resource = 82,
                            .frames = {{.x = 0, .y = 0, .w = 10, .h = 10}}}};
    image.clickEvent = SkinEventBindingId{1};
    image.clickMode = 2;
    model_.model.objects.push_back(
        {.id = 82,
         .authoredName = "session-click-image",
         .payload = std::move(image),
         .authoredOrdinal = 82,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 82,
         .presentation = {.loop = 0,
                          .frames = {{.timeMillis = 0,
                                      .x = 100.0,
                                      .y = 100.0,
                                      .width = 40.0,
                                      .height = 20.0}},
                          .authoredOrdinal = 820}});
  }

  void addOrderedClickableImage(double destinationX) {
    resources_.addImage(86);
    model_.model.events.push_back(
        {.id = SkinEventBindingId{2},
         .source = *interactionEvent_,
         .authoredOrdinal = 2});
    SkinImageObject image;
    image.orderedStates = {{.resource = 86,
                            .frames = {{.x = 0, .y = 0, .w = 10, .h = 10}}}};
    image.clickEvent = SkinEventBindingId{2};
    image.clickMode = 2;
    model_.model.objects.push_back(
        {.id = 86,
         .authoredName = "session-ordered-click-image",
         .payload = std::move(image),
         .authoredOrdinal = 86,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = 86,
         .presentation = {.loop = 0,
                          .frames = {{.timeMillis = 0,
                                      .x = destinationX,
                                      .y = 100.0,
                                      .width = 40.0,
                                      .height = 20.0}},
                          .authoredOrdinal = 860}});
  }

  void addEditableText(SkinObjectId id, std::string value,
                       SkinStringWriterId writer, double x,
                       bool editable = true) {
    resources_.addTextAtlas(id, id);
    SkinTextObject text;
    text.literal = std::move(value);
    text.writer = writer;
    text.pointSize = 10;
    text.editable = editable;
    model_.model.objects.push_back(
        {.id = id,
         .authoredName = "session-text-" + std::to_string(id),
         .payload = std::move(text),
         .authoredOrdinal = id,
         .critical = true});
    model_.model.destinations.push_back(
        {.object = id,
         .presentation = {.loop = 0,
                          .frames = {{.timeMillis = 0,
                                      .x = x,
                                      .y = 100.0,
                                      .width = 80.0,
                                      .height = 20.0}},
                          .authoredOrdinal = id}});
  }

  bool verifyTextCancellation(std::uint64_t frameSerial) {
    const auto begun = runtime_->beginFrame(frameSerial);
    if (!begun.ok) {
      return false;
    }
    return !runtime_->invoke(*textCancelVerify_, {}).failure;
  }

  std::optional<std::int64_t> textUtf8Count(std::uint64_t frameSerial) {
    const auto begun = runtime_->beginFrame(frameSerial);
    if (!begun.ok) {
      return std::nullopt;
    }
    const auto result = runtime_->invoke(*textUtf8Count_, {});
    if (result.failure || !result.value) {
      return std::nullopt;
    }
    if (const auto *value = std::get_if<std::int64_t>(&*result.value)) {
      return *value;
    }
    return std::nullopt;
  }

  std::optional<std::string> writerDragValues(std::uint64_t frameSerial) {
    const auto begun = runtime_->beginFrame(frameSerial);
    if (!begun.ok) {
      return std::nullopt;
    }
    const auto result = runtime_->invoke(*writerDragValues_, {});
    if (result.failure || !result.value) {
      return std::nullopt;
    }
    if (const auto *value = std::get_if<std::string>(&*result.value)) {
      return *value;
    }
    return std::nullopt;
  }

  std::optional<std::string> interactionOrder(std::uint64_t frameSerial) {
    const auto begun = runtime_->beginFrame(frameSerial);
    if (!begun.ok) {
      return std::nullopt;
    }
    const auto result = runtime_->invoke(*interactionOrder_, {});
    if (result.failure || !result.value) {
      return std::nullopt;
    }
    if (const auto *value = std::get_if<std::string>(&*result.value)) {
      return *value;
    }
    return std::nullopt;
  }

  void destroySession() { session_.reset(); }

private:
  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinEntryId entry_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  std::optional<PreparedSkinRevision> prepared_;
  std::unique_ptr<LuaSkinRuntime> runtime_;
  std::optional<LuaCallbackId> writerA_;
  std::optional<LuaCallbackId> writerDrag_;
  std::optional<LuaCallbackId> writerDragValue_;
  std::optional<LuaCallbackId> writerDragValues_;
  std::optional<LuaCallbackId> writerB_;
  std::optional<LuaCallbackId> writerFail_;
  std::optional<LuaCallbackId> writerOnce_;
  std::optional<LuaCallbackId> writerOnceVerify_;
  std::optional<LuaCallbackId> textWriterUtf8_;
  std::optional<LuaCallbackId> textWriterFirst_;
  std::optional<LuaCallbackId> textWriterSecond_;
  std::optional<LuaCallbackId> textWriterCancel_;
  std::optional<LuaCallbackId> textCancelVerify_;
  std::optional<LuaCallbackId> textUtf8Count_;
  std::optional<LuaCallbackId> interactionString_;
  std::optional<LuaCallbackId> interactionEvent_;
  std::optional<LuaCallbackId> interactionFloat_;
  std::optional<LuaCallbackId> interactionOrder_;
  PlayfieldChartVisualModel chart_;
  ValidatedBeatorajaSkinModel model_;
  BeatorajaSkinConfiguration configuration_;
  SkinEventMutationTable mutations_;
  SessionResources resources_;
  PlaySkinViewport viewport_;
  Skin2DRenderer renderer_;
  SessionQuadBackend quadBackend_;
  rendering::SkinQuadBatchRenderer quadRenderer_;
  SkinConfigurationWriteQueue configurationWrites_;
  std::vector<std::pair<SkinAudioVolumeWriterTarget, float>>
      audioVolumeWrites_;
  std::vector<float> practiceItemScrollWrites_;
  std::vector<std::pair<std::size_t, bool>> practiceMenuItemWrites_;
  std::vector<int> practiceVisibleItemWrites_;
  std::unique_ptr<PlaySkinStateBridge> bridge_;
  std::unique_ptr<PlaySkinSession> session_;
};

void testSuccessfulFrameCommitsWriterMutationsInInputOrder() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  const std::array writers{
      SkinWriterInvocation{.writer = SkinFloatWriterId{1},
                           .normalizedValue = 0.2,
                           .eventMicros = 10},
      SkinWriterInvocation{.writer = SkinFloatWriterId{2},
                           .normalizedValue = 0.7,
                           .eventMicros = 20},
      SkinWriterInvocation{.writer = SkinFloatWriterId{1},
                           .normalizedValue = 4.0,
                           .eventMicros = 30},
  };
  const auto result = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), writers);
  expect(result.ready() && result.frameSerial == 1 &&
             result.evaluation.submitReady &&
             result.committed.frameSerial == 1,
         "matched empty-custom frame evaluates and commits exactly once");
  expect(result.committed.orderedMutations.size() == 3,
         "every successful queued writer contributes one ordered mutation");
  const std::array expectedEvents{900, 901, 900};
  const std::array expectedArguments{20, 70, 100};
  for (std::size_t index = 0;
       index < result.committed.orderedMutations.size() && index < 3; ++index) {
    const auto *mutation =
        presentationMutation(result.committed.orderedMutations[index]);
    expect(mutation && mutation->eventId == expectedEvents[index] &&
               mutation->argumentCount == 1 &&
               mutation->arguments[0] == expectedArguments[index],
           "queued writer mutation order and clamped values are preserved");
  }
  expect(fixture.bridge().frameSerial() == 0,
         "successful publication closes the bridge transaction");

  const auto duplicate = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(!duplicate.ready() && !duplicate.evaluation.submitReady &&
             duplicate.committed.frameSerial == 0 &&
             hasDiagnostic(duplicate,
                           "skin.play_state.frame_serial_not_increasing"),
         "one visual serial cannot begin or commit a second frame");
}

void testWriterFailureDiscardsEarlierAndFailedCallbackMutations() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  const std::array writers{
      SkinWriterInvocation{.writer = SkinFloatWriterId{1},
                           .normalizedValue = 0.25},
      SkinWriterInvocation{.writer = SkinFloatWriterId{3},
                           .normalizedValue = 0.5},
      SkinWriterInvocation{.writer = SkinFloatWriterId{2},
                           .normalizedValue = 0.75},
  };
  const auto failed = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), writers);
  expect(!failed.ready() && !failed.evaluation.submitReady &&
             failed.committed.orderedMutations.empty() &&
             hasDiagnostic(failed, "skin_lua_execution_failed") &&
             fixture.bridge().frameSerial() == 0,
         "writer failure discards the whole frame transaction");

  const std::array nextWriters{
      SkinWriterInvocation{.writer = SkinFloatWriterId{2},
                           .normalizedValue = 0.4}};
  const auto next = fixture.session().prepareFrame(
      stateAt(2), projectionAt(2), nextWriters);
  expect(next.ready() && next.committed.orderedMutations.size() == 1,
         "writer failure does not poison the next visual frame budget");
  if (!next.committed.orderedMutations.empty()) {
    const auto *mutation =
        presentationMutation(next.committed.orderedMutations.front());
    expect(mutation && mutation->eventId == 901 &&
               mutation->arguments[0] == 40,
           "only the next frame mutation is published after rollback");
  }
}

void testEvaluatorFailureDiscardsWriterTransaction() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.model().model.destinations.push_back(
      {.object = 999, .presentation = {.authoredOrdinal = 1}});
  const std::array writers{
      SkinWriterInvocation{.writer = SkinFloatWriterId{1},
                           .normalizedValue = 0.3}};
  const auto result = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), writers);
  expect(!result.ready() && !result.evaluation.submitReady &&
             result.committed.orderedMutations.empty() &&
             hasDiagnostic(result, "skin.renderer.model.destination_object") &&
             fixture.bridge().frameSerial() == 0,
         "whole-buffer evaluation failure rolls back staged writers");
}

void testSerialMismatchDoesNotConsumeRuntimeFrame() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  const auto mismatch = fixture.session().prepareFrame(
      stateAt(10), projectionAt(11), {});
  expect(!mismatch.ready() &&
             hasDiagnostic(mismatch, "skin.play_state.frame_serial_invalid"),
         "mismatched state and projection fail before runtime frame begin");
  const auto corrected = fixture.session().prepareFrame(
      stateAt(10), projectionAt(10), {});
  expect(corrected.ready() && corrected.committed.frameSerial == 10,
         "corrected matched serial can still begin exactly once");
}

void testSyntheticReplayGhostUsesMatchingLaneGeometry() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 9,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {
          {.lane = 0,
           .normalNote = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
           .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0}},
          {.lane = 1,
           .normalNote = {.x = 90.0, .y = 250.0, .width = 52.0, .height = 16.0},
           .clip = {.x = 90.0, .y = 250.0, .width = 52.0, .height = 500.0}},
      }};
  const std::array events{ReplayGhostEvent{.lane = 1,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 2.2,
                                            .judgement = Great}};

  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 9,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 2.0,
                 .hispeed = 1.0,
                 .enabled = true,
                 .events = events});
  expect(overlay.frameSerial == 9 && overlay.commands.size() == 4,
         "enabled synthetic replay ghost emits one outline");
  if (overlay.commands.size() != 4) {
    return;
  }
  const auto *top =
      std::get_if<SkinPrimitiveCommand>(&overlay.commands.front().payload);
  expect(top != nullptr && top->vertices.size() == 4 &&
             std::abs(top->vertices[0].x - 90.0F) < 0.0001F &&
             std::abs(top->vertices[0].y - 290.0F) < 0.0001F &&
             std::abs(top->vertices[1].x - 142.0F) < 0.0001F &&
             std::abs(top->vertices[2].y - 291.92F) < 0.0001F,
         "synthetic replay ghost uses lane one width, height, and scroll");
}

void testSelectedSkinHudUsesThePublishedSkinNoteLaneSpan() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 17,
      .viewport = {.authoredToUi = {.m00 = 0.5, .tx = 7.0,
                                    .m11 = 0.25, .ty = 9.0},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {
          {.lane = 0,
           .normalNote = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
           .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0}},
          {.lane = 1,
           .normalNote = {.x = 90.0, .y = 100.0, .width = 52.0, .height = 16.0},
           .clip = {.x = 90.0, .y = 100.0, .width = 52.0, .height = 500.0}},
      }};

  const auto hud = selectedSkinHudGeometry(geometry);
  expect(hud && hud->frameSerial == 17 && hud->laneCount == 2 &&
             std::abs(hud->playArea.x - 12.0) < 0.0001 &&
             std::abs(hud->playArea.y - 34.0) < 0.0001 &&
             std::abs(hud->playArea.width - 66.0) < 0.0001 &&
             std::abs(hud->playArea.height - 125.0) < 0.0001 &&
             std::abs(hud->judgementLineY - 34.0) < 0.0001,
         "selected-skin HUD derives and projects its full lane span and "
         "judgement line from published SkinNote geometry");
}

void testPmsPoorDestinationUsesFirstSelectedSkinLane() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addTouchGeometry(SkinFloatWriterId{1}, -42.0);
  const auto geometry = fixture.session().pmsPoorDestinationGeometry();
  expect(geometry.has_value() &&
             std::abs(geometry->laneOriginY - 20.0) < 0.0001 &&
             std::abs(geometry->laneHeight - 500.0) < 0.0001 &&
             std::abs(geometry->secondaryDestinationY + 42.0) < 0.0001,
         "PMS dst2 forwards the selected SkinNote's source lane-zero geometry");
}

void testSyntheticStartLaneIndicatorsUseSelectedSkinLaneGeometry() {
  const PlaySkinViewport viewport{
      .authoredToUi = {},
      .uiToAuthored = {},
      .drawableAuthoredBounds =
          {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
      .safeUiBounds =
          {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
      .valid = true};
  const std::array geometry{
      SyntheticStartLaneIndicatorLaneGeometry{
          .lane = 0,
          .laneRegion = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0},
          .rgba = {1.0F, 1.0F, 1.0F, 1.0F}},
      SyntheticStartLaneIndicatorLaneGeometry{
          .lane = 1,
          .laneRegion = {.x = 90.0, .y = 250.0, .width = 52.0, .height = 500.0},
          .rgba = {1.0F, 0.0F, 0.0F, 1.0F}},
  };
  const std::array requestedLanes{1, 99};
  const auto overlay = buildSyntheticStartLaneIndicatorOverlay(
      viewport, geometry,
      {.frameSerial = 12,
       .lanes = requestedLanes,
       .visibleLaneHeightRatio = 0.5});
  expect(overlay.frameSerial == 12 && overlay.commands.size() == 1,
         "start-lane overlay emits only selected skin lanes that exist");
  if (overlay.commands.size() != 1) {
    return;
  }
  const auto *triangle =
      std::get_if<SkinPrimitiveCommand>(&overlay.commands.front().payload);
  expect(triangle != nullptr &&
             triangle->kind == SkinPrimitiveKind::TriangleStrip &&
             triangle->vertices.size() == 3 &&
             std::abs(triangle->vertices[0].x - 104.04F) < 0.001F &&
             std::abs(triangle->vertices[0].y - 499.92F) < 0.001F &&
             std::abs(triangle->vertices[1].x - 116.0F) < 0.001F &&
             std::abs(triangle->vertices[1].y - 479.12F) < 0.001F &&
             triangle->vertices[0].rgba == 0xff0000ffU &&
             triangle->state.scissor.has_value(),
         "start-lane triangle uses the selected skin lane width, origin, "
         "color, and live lane-cover edge");
}

void testSyntheticReplayGhostRespectsDisabledOption() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 1,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {{.lane = 0,
                 .normalNote =
                     {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
                 .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0}}}};
  const std::array events{ReplayGhostEvent{.lane = 0,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 2.2,
                                            .judgement = Great}};
  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 1,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 2.0,
                 .hispeed = 1.0,
                 .enabled = false,
                 .events = events});
  expect(overlay.commands.empty(),
         "disabled replay ghost option suppresses synthetic skin ghosts");
}

void testSyntheticReplayGhostSkipsEventsOutsideLaneClip() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 1,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {{.lane = 0,
                 .normalNote =
                     {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
                 .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 500.0}}}};
  const std::array events{ReplayGhostEvent{.lane = 0,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 10.0,
                                            .judgement = Great}};
  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 1,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 2.0,
                 .hispeed = 1.0,
                 .enabled = true,
                 .events = events});
  expect(overlay.commands.empty(),
         "synthetic replay ghost skips events outside the active lane clip");
}

void testSyntheticReplayGhostUsesSharedPlayAreaClip() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 1,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneHeight = 200.0,
      .lanes = {
          // Beatoraja's LaneRenderer uses lane zero for the common vertical
          // play-area bounds, even when the next lane has a taller region.
          {.lane = 0,
           .normalNote = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
           .clip = {.x = 10.0, .y = 100.0, .width = 30.0, .height = 100.0}},
          {.lane = 1,
           .normalNote = {.x = 80.0, .y = 100.0, .width = 52.0, .height = 16.0},
           .clip = {.x = 80.0, .y = 100.0, .width = 52.0, .height = 500.0}},
      }};
  const std::array events{ReplayGhostEvent{.lane = 1,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 1.0,
                                            .judgement = Great}};
  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 1,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 0.0,
                 .hispeed = 1.0,
                 .enabled = true,
                 .events = events});
  expect(overlay.commands.empty(),
         "synthetic replay ghosts obey the shared skin play-area clip");
}

void testSyntheticReplayGhostRespectsLaneCoverVisibleHeight() {
  SyntheticReplayGhostGeometry geometry{
      .frameSerial = 1,
      .viewport = {.authoredToUi = {},
                   .uiToAuthored = {},
                   .drawableAuthoredBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .safeUiBounds =
                       {.x = 0.0, .y = 0.0, .width = 1280.0, .height = 720.0},
                   .valid = true},
      .sharedLaneOriginY = 100.0,
      .sharedLaneHeight = 200.0,
      .lanes = {{.lane = 0,
                 .normalNote =
                     {.x = 10.0, .y = 100.0, .width = 30.0, .height = 8.0},
                 .clip =
                     {.x = 10.0, .y = 100.0, .width = 30.0, .height = 200.0}}}};
  const std::array events{ReplayGhostEvent{.lane = 0,
                                            .noteTimeMicros = 1'000,
                                            .judgeTimeMicros = 1'100,
                                            .judgeScrollPosition = 0.75,
                                            .judgement = Great}};

  const auto overlay = buildSyntheticReplayGhostOverlay(
      geometry, {.frameSerial = 1,
                 .visualTimeMicros = 1'000,
                 .currentScrollPosition = 0.0,
                 .hispeed = 1.0,
                 .visibleLaneHeightRatio = 0.5,
                 .enabled = true,
                 .events = events});
  expect(overlay.commands.empty(),
         "synthetic replay ghosts remain below the active lane-cover cutoff");
}

void testEvaluatedSkinPublishesPerLaneReplayGhostGeometry() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addReplayGhostNoteGeometry();
  const auto frame = fixture.session().prepareFrame(stateAt(1), projectionAt(1), {});
  const auto &geometry = frame.evaluation.syntheticReplayGhostGeometry;
  expect(frame.ready() && geometry && geometry->frameSerial == 1 &&
             geometry->sharedLaneOriginY == 40.0 &&
             geometry->sharedLaneHeight == 200.0 &&
             geometry->lanes.size() == 2 && geometry->lanes[0].lane == 7 &&
             geometry->lanes[0].normalNote.width == 30.0 &&
             geometry->lanes[1].lane == 0 &&
             geometry->lanes[1].normalNote.x == 80.0 &&
             geometry->lanes[1].normalNote.height == 17.0,
         "evaluated skin publishes the active note source's per-lane ghost geometry");
}

void testSubmittedSkinRendersOptionGatedSyntheticReplayGhosts() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addReplayGhostNoteGeometry();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(301), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "skin frame is submitted before its optional replay overlay");
  const std::array events{ReplayGhostEvent{.lane = 0,
                                            .noteTimeMicros = 100,
                                            .judgeTimeMicros = 200,
                                            .judgeScrollPosition = 1.2,
                                            .judgement = Great}};
  const auto submitsBeforeGhost = fixture.quadBackend().submitCalls;
  fixture.session().submitSyntheticReplayGhosts(
      context, {.frameSerial = 1,
                .visualTimeMicros = 100,
                .currentScrollPosition = 1.0,
                .hispeed = 1.0,
                .enabled = true,
                .events = events});
  expect(fixture.quadBackend().submitCalls > submitsBeforeGhost,
         "submitted selected skin draws the enabled synthetic replay ghost");
  const auto submitsAfterEnabled = fixture.quadBackend().submitCalls;
  fixture.session().submitSyntheticReplayGhosts(
      context, {.frameSerial = 1,
                .visualTimeMicros = 100,
                .currentScrollPosition = 1.0,
                .hispeed = 1.0,
                .enabled = false,
                .events = events});
  expect(fixture.quadBackend().submitCalls == submitsAfterEnabled,
         "submitted selected skin suppresses synthetic ghosts when disabled");
}

void testSubmittedSkinRendersPreparationIndicatorsFromItsLaneLayout() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(302), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "skin frame publishes its static SkinNote lane layout before the cue");
  const std::array requestedLanes{0, 7};
  const auto submitsBeforeCue = fixture.quadBackend().submitCalls;
  fixture.session().submitSyntheticStartLaneIndicators(
      context, {.frameSerial = 1, .lanes = requestedLanes});
  expect(fixture.quadBackend().submitCalls > submitsBeforeCue,
         "selected skin submits preparation indicators through its own renderer");
  const auto submitsAfterCue = fixture.quadBackend().submitCalls;
  fixture.session().submitSyntheticStartLaneIndicators(
      context, {.frameSerial = 2, .lanes = requestedLanes});
  expect(fixture.quadBackend().submitCalls == submitsAfterCue,
         "a preparation cue cannot use a stale selected-skin frame geometry");
}

void testInvalidSessionSerialDoesNotConsumeFrameOwners() {
  SessionFixture fixture(0);
  if (!fixture.ready()) {
    return;
  }
  const auto rejected = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(!rejected.ready() &&
             hasDiagnostic(rejected, "skin.session.serial_invalid") &&
             fixture.bridge().frameSerial() == 0,
         "zero session serial fails before opening the bridge frame");

  SerialOnlyState state(1);
  const auto evaluation = fixture.renderer().evaluateFrame({
      .frameSerial = 1,
      .sessionSerial = 37,
      .visualTimeMicros = 10'000,
      .model = fixture.model(),
      .configuration = fixture.configuration(),
      .resources = fixture.resources(),
      .viewport = fixture.viewport(),
      .runtime = &fixture.runtime(),
      .state = state,
  });
  expect(evaluation.submitReady.has_value(),
         "zero session serial does not consume the Lua frame serial");
}

void testPassiveCustomTimerUsesTheSharedSessionFrame() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.model().model.customTimers.push_back(
      {.id = 10'001, .timer = std::nullopt});
  const auto result = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(result.ready() && result.committed.frameSerial == 1 &&
             result.committed.orderedMutations.empty(),
         "nonempty passive custom timers execute within the shared session frame");
}

void testProductionPrepareIsExternallySideEffectFreeAndRejectsDoublePrepare() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;

  const auto prepared =
      fixture.session().prepareFrame(stateAt(1), projectionAt(1));
  const auto doublePrepare =
      fixture.session().prepareFrame(stateAt(2), projectionAt(2));
  expect(prepared == PresentationFrameOutcome::Ready &&
             doublePrepare == PresentationFrameOutcome::CriticalFailure &&
             bga.preflightCalls == 0 && bga.submitCalls == 0 &&
             fixture.quadBackend().submitCalls == 0 &&
             fixture.session().touchLayout().laneRegions.empty() &&
             fixture.session().touchHitRegions().empty() &&
             fixture.configurationWrites().drain().empty(),
         "prepare retains one value-owned transaction without submission, "
         "layout publication, persistence, or replacement by a second frame");

  RenderContext context;
  const auto exactBga = bgaFrame(1);
  const auto rendered = fixture.session().render(context, exactBga, bga);
  expect(rendered.frameSerial == 1 &&
             rendered.outcome == PresentationFrameOutcome::Ready &&
             rendered.submittedMode == PresentationMode::Skin &&
             rendered.bgaCompositeMode ==
                 GameplayBgaCompositeMode::EmbeddedSkin &&
             rendered.preparedBga &&
             sameBgaFrame(*rendered.preparedBga, exactBga),
         "the first pending frame remains renderable after double-prepare "
         "rejection");
}

void testSuccessfulRenderConsumesOnceSubmitsExactBgaAndPublishesLayout() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;
  const auto exactBga = bgaFrame(44);

  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "successful frame prepares");
  RenderContext context;
  const auto result = fixture.session().render(context, exactBga, bga);
  const auto layout = fixture.session().touchLayout();
  expect(result.frameSerial == 1 &&
             result.outcome == PresentationFrameOutcome::Ready &&
             result.submittedMode == PresentationMode::Skin &&
             result.bgaCompositeMode ==
                 GameplayBgaCompositeMode::EmbeddedSkin &&
             result.preparedBga &&
             sameBgaFrame(*result.preparedBga, exactBga) &&
             bga.preflightCalls == 1 && bga.commitCalls == 1 &&
             bga.submitCalls == 1 && bga.finalizeCalls == 1 &&
             sameBgaFrame(bga.preflightFrame, exactBga) &&
             sameBgaFrame(bga.committedFrame, exactBga) &&
             sameBgaFrame(bga.submittedFrame, exactBga) &&
             sameBgaFrame(bga.finalizedFrame, exactBga) &&
             fixture.quadBackend().submitCalls == 1,
         "successful render emits one complete skin frame with the exact "
         "prepared BGA value");
  expect(layout.revision == fixture.session().touchLayoutRevision() &&
             layout.keyMode == 7 && layout.laneCount == 2 &&
             layout.laneRegions.size() == 2 &&
             layout.laneRegions[0].lane == 7 &&
             layout.laneRegions[0].scratch &&
             layout.laneRegions[1].lane == 0 &&
             !layout.laneRegions[1].scratch &&
             std::abs(layout.laneRegions[0].bottomLeft.x - 0.0520833F) <
                 0.0001F &&
             std::abs(layout.laneRegions[0].bottomLeft.y -
                      0.6481481F) < 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.x - 0.15625F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.y - 0.1851852F) <
                 0.0001F &&
             fixture.session().touchHitRegions().size() == 1,
         "successful submission publishes normalized authored lane and "
         "control geometry");

  const auto repeated = fixture.session().render(context, exactBga, bga);
  expect(repeated.outcome == PresentationFrameOutcome::CriticalFailure &&
             bga.preflightCalls == 1 && bga.commitCalls == 1 &&
             bga.submitCalls == 1 && bga.finalizeCalls == 1 &&
             fixture.quadBackend().submitCalls == 1 &&
             fixture.configurationWrites().drain().empty(),
         "repeat render cannot resubmit the consumed frame or enqueue writes");
}

void testSkinLaneTouchLayoutUsesDrawableScreenCoordinates() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();

  const auto savedWindowWidth = rendering::window_width;
  const auto savedWindowHeight = rendering::window_height;
  const auto savedRenderWidth = rendering::render_width;
  const auto savedRenderHeight = rendering::render_height;
  const auto savedScaleX = rendering::ui_scale_x;
  const auto savedScaleY = rendering::ui_scale_y;
  const auto savedOffsetX = rendering::ui_offset_x;
  const auto savedOffsetY = rendering::ui_offset_y;
  rendering::window_width = 1920;
  rendering::window_height = 1080;
  rendering::render_width = 2400;
  rendering::render_height = 1400;
  rendering::ui_scale_x = 1.1F;
  rendering::ui_scale_y = 1.2F;
  rendering::ui_offset_x = 240;
  rendering::ui_offset_y = 70;

  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(71), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "drawable-coordinate fixture publishes skin lane geometry");
  const auto layout = fixture.session().touchLayout();
  expect(layout.laneRegions.size() == 2 &&
             std::abs(layout.laneRegions[0].bottomLeft.x -
                      (350.0F / 2400.0F)) < 0.0001F &&
             std::abs(layout.laneRegions[0].bottomLeft.y -
                      (910.0F / 1400.0F)) < 0.0001F,
         "skin lane routing uses the same drawable scale and offset as raw "
         "iOS touch input");

  rendering::window_width = savedWindowWidth;
  rendering::window_height = savedWindowHeight;
  rendering::render_width = savedRenderWidth;
  rendering::render_height = savedRenderHeight;
  rendering::ui_scale_x = savedScaleX;
  rendering::ui_scale_y = savedScaleY;
  rendering::ui_offset_x = savedOffsetX;
  rendering::ui_offset_y = savedOffsetY;
}

void testCriticalEvaluationAndPreflightFailuresPublishNoFrameState() {
  {
    SessionFixture fixture;
    if (fixture.ready()) {
      fixture.addBgaMarker();
      fixture.addTouchGeometry();
      SessionBgaSubmitter bga;
      RenderContext context;
      expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
                 fixture.session().render(context, bgaFrame(70), bga).outcome ==
                     PresentationFrameOutcome::Ready,
             "evaluation-failure fixture first publishes valid geometry");
      const UiLogicalPoint oldPoint{.x = 150.0F, .y = 610.0F};
      const auto oldHit = fixture.session().hitTestUiControl(oldPoint);
      expect(fixture.session().beginPresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = oldPoint,
                  .eventMicros = 1,
                  .hit = oldHit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true},
             "evaluation-failure fixture captures the published control");
      fixture.model().model.destinations.push_back(
          {.object = 999, .presentation = {.authoredOrdinal = 1}});
      const auto outcome =
          fixture.session().prepareFrame(stateAt(2), projectionAt(2));
      const auto exactBga = bgaFrame(71);
      const auto result = fixture.session().render(context, exactBga, bga);
      expect(outcome == PresentationFrameOutcome::CriticalFailure &&
                 result.outcome == PresentationFrameOutcome::CriticalFailure &&
                 result.frameSerial == 2 && result.failure &&
                 result.preparedBga &&
                 sameBgaFrame(*result.preparedBga, exactBga) &&
                 result.failure->frameSerial == 2 &&
                 result.failure->entry == fixture.session().identity().entry &&
                 result.failure->revisionDigest ==
                     fixture.session().identity().revisionDigest &&
                 result.failure->configurationDigest ==
                     fixture.session().identity().configurationDigest &&
                 result.submittedMode == PresentationMode::BuiltIn &&
                 result.bgaCompositeMode ==
                     GameplayBgaCompositeMode::FullscreenBuiltIn &&
                 bga.preflightCalls == 1 && bga.commitCalls == 1 &&
                 bga.submitCalls == 1 && bga.finalizeCalls == 1 &&
                 bga.fullscreenCalls == 0 &&
                 fixture.session().touchLayout().laneRegions.empty() &&
                 fixture.session().touchHitRegions().empty() &&
                 fixture.session().updatePresentationTouch(
                     {.pointerId = 1,
                      .uiPoint = oldPoint,
                      .eventMicros = 2,
                      .hit = oldHit}) == PresentationTouchResult{} &&
                 fixture.configurationWrites().drain().empty(),
             "critical evaluation failure carries exact identity and frame, "
             "performs no additional BGA work, and clears prior geometry, "
             "capture, and writes");
    }
  }
  {
    SessionFixture fixture;
    if (fixture.ready()) {
      fixture.addBgaMarker();
      fixture.addTouchGeometry();
      SessionBgaSubmitter bga;
      expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready,
             "preflight fixture first evaluates");
      RenderContext context;
      expect(fixture.session().render(context, bgaFrame(70), bga).outcome ==
                 PresentationFrameOutcome::Ready,
             "preflight fixture first publishes valid geometry");
      const UiLogicalPoint oldPoint{.x = 150.0F, .y = 610.0F};
      const auto oldHit = fixture.session().hitTestUiControl(oldPoint);
      expect(fixture.session().beginPresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = oldPoint,
                  .eventMicros = 1,
                  .hit = oldHit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true},
             "preflight fixture captures the published control");
      bga.preflightReady = false;
      expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
                 PresentationFrameOutcome::Ready,
             "preflight failure frame evaluates before submission");
      const auto exactBga = bgaFrame(72);
      const auto result = fixture.session().render(context, exactBga, bga);
      expect(result.outcome == PresentationFrameOutcome::Ready &&
                 result.frameSerial == 2 && !result.failure &&
                 result.preparedBga &&
                 sameBgaFrame(*result.preparedBga, exactBga) &&
                 result.submittedMode == PresentationMode::Skin &&
                 result.bgaCompositeMode ==
                     GameplayBgaCompositeMode::EmbeddedSkin &&
                 bga.preflightCalls == 2 && bga.commitCalls == 1 &&
                 bga.submitCalls == 1 && bga.finalizeCalls == 2 &&
                 bga.fullscreenCalls == 0 &&
                 fixture.quadBackend().submitCalls == 2 &&
                 !fixture.session().touchLayout().laneRegions.empty() &&
                 fixture.configurationWrites().drain().empty(),
             "BGA preflight failure blanks only BGA while the authored skin "
             "continues to draw and releases its prepared frame");
    }
  }
}

void testForwardCompatiblePersistedMutationsEnqueueOneExactOrderedBatch() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  // Pinned v1 exposes no persisted mutation selector. These value-owned
  // variants exercise only the forward-compatible post-submit session
  // plumbing, through the same pending evaluation and renderer path.
  std::vector<SkinFrameMutation> extraMutations{
      SessionPresentationWrite{.eventId = 900,
                               .arguments = {1, 0},
                               .argumentCount = 1},
      persisted(SetSkinOption{.key = "gauge", .value = 3}),
      SessionPresentationWrite{.eventId = 901,
                               .arguments = {2, 0},
                               .argumentCount = 1},
      persisted(SetSkinFilePath{.key = "background",
                                .declaredValue = "blue.png"}),
      persisted(SetSkinOffset{.key = "lane",
                              .value = {.x = 1,
                                        .y = 2,
                                        .w = 3,
                                        .h = 4,
                                        .r = 5,
                                        .a = 6}}),
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, extraMutations) ==
             PresentationFrameOutcome::Ready,
         "test-only forward-compatible mutations enter the regular pending "
         "frame");
  std::get<SetSkinOption>(
      std::get<PersistedSkinConfigurationWrite>(extraMutations[1])).key =
      "mutated-gauge";
  std::get<SetSkinFilePath>(
      std::get<PersistedSkinConfigurationWrite>(extraMutations[3]))
      .declaredValue = "mutated.png";
  extraMutations.clear();
  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(81), bga);
  const auto requests = fixture.configurationWrites().drain();
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             requests.size() == 1 && requests[0].sessionSerial == 37 &&
             requests[0].profileId == fixture.session().identity().profileId &&
             requests[0].entry == fixture.session().identity().entry &&
             requests[0].expectedRevisionDigest == "session-revision" &&
             requests[0].expectedConfigurationDigest ==
                 "session-configuration" &&
             requests[0].frameSerial == 1 &&
             requests[0].orderedWrites.size() == 3 &&
             std::get<SetSkinOption>(requests[0].orderedWrites[0]).key ==
                 "gauge" &&
             std::get<SetSkinOption>(requests[0].orderedWrites[0]).value == 3 &&
             std::get<SetSkinFilePath>(requests[0].orderedWrites[1])
                     .declaredValue == "blue.png" &&
             std::get<SetSkinOffset>(requests[0].orderedWrites[2]).value.r == 5,
         "successful skin draw consumes session-local mutations and enqueues "
         "one exact deep-owned persisted batch in authored order");
}

void testAudioVolumeMutationAppliesOnlyAfterSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  const std::vector<SkinFrameMutation> writes{
      SetSkinAudioVolume{.target = SkinAudioVolumeWriterTarget::Keysound,
                         .value = 0.35F},
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready &&
             fixture.audioVolumeWrites().empty(),
         "audio volume mutation remains staged until the skin frame submits");

  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(82), bga);
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             fixture.audioVolumeWrites().size() == 1 &&
             fixture.audioVolumeWrites().front().first ==
                 SkinAudioVolumeWriterTarget::Keysound &&
             fixture.audioVolumeWrites().front().second == 0.35F,
         "submitted skin frame applies the staged native audio writer exactly "
         "once");
}

void testPracticeScrollMutationAppliesOnlyAfterSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  const std::vector<SkinFrameMutation> writes{
      SetPracticeItemScroll{.position = 0.5F},
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready &&
             fixture.practiceItemScrollWrites().empty(),
         "practice scroll remains staged until the skin frame submits");

  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(83), bga);
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             fixture.practiceItemScrollWrites().size() == 1 &&
             fixture.practiceItemScrollWrites().front() == 0.5F,
         "submitted skin frame applies the staged practice viewport writer "
         "exactly once");
}

void testPracticeMenuItemMutationAppliesOnlyAfterSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  const std::vector<SkinFrameMutation> writes{
      SetPracticeMenuItem{.visibleIndex = 9, .increment = false},
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready &&
             fixture.practiceMenuItemWrites().empty(),
         "practice item remains staged until the skin frame submits");

  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(84), bga);
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             fixture.practiceMenuItemWrites().size() == 1 &&
             fixture.practiceMenuItemWrites().front() ==
                 std::pair<std::size_t, bool>{9, false},
         "submitted skin frame applies the staged source practice row exactly "
         "once");
}

void testPracticeVisibleItemsMutationAppliesOnlyAfterSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  const std::vector<SkinFrameMutation> writes{
      SetPracticeVisibleItems{.count = 7},
  };
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready &&
             fixture.practiceVisibleItemWrites().empty(),
         "practice visible-row count remains staged until the skin frame "
         "submits");

  SessionBgaSubmitter bga;
  RenderContext context;
  const auto result = fixture.session().render(context, bgaFrame(85), bga);
  expect(result.outcome == PresentationFrameOutcome::Ready &&
             fixture.practiceVisibleItemWrites() == std::vector<int>{7},
         "submitted skin frame applies the SkinPractice visible-row count "
         "exactly once");
}

void testPersistenceRequestIsFullyAllocatedBeforeSkinSubmission() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  const std::vector<SkinFrameMutation> writes{
      persisted(SetSkinFilePath{.key = "background",
                                .declaredValue = "preallocated.png"})};
  expect(fixture.session().prepareFrameForTesting(
             stateAt(1), projectionAt(1), {}, writes) ==
             PresentationFrameOutcome::Ready,
         "allocation-boundary fixture prepares a persisted write");
  fixture.quadBackend().failNextAllocationAfterSubmit = true;
  SessionBgaSubmitter bga;
  RenderContext context;
  const auto exactBga = bgaFrame(85);
  PresentationFrameResult result;
  bool exceptionEscaped = false;
  try {
    result = fixture.session().render(context, exactBga, bga);
  } catch (...) {
    exceptionEscaped = true;
  }
  // A correct implementation performs no allocation after backend submit, so
  // the one-shot fault can remain armed until the observation phase.
  session_test_allocation_fault::failNext = false;
  const auto requests = fixture.configurationWrites().drain();
  expect(!exceptionEscaped && result.outcome == PresentationFrameOutcome::Ready &&
             result.submittedMode == PresentationMode::Skin &&
             result.bgaCompositeMode ==
                 GameplayBgaCompositeMode::EmbeddedSkin &&
             result.preparedBga &&
             sameBgaFrame(*result.preparedBga, exactBga) &&
             bga.commitCalls == 1 && bga.submitCalls == 1 &&
             bga.finalizeCalls == 1 && bga.fullscreenCalls == 0 &&
             fixture.quadBackend().submitCalls == 1 &&
             requests.size() == 1 && requests[0].frameSerial == 1 &&
             requests[0].orderedWrites.size() == 1 &&
             std::get<SetSkinFilePath>(requests[0].orderedWrites.front())
                     .declaredValue == "preallocated.png",
         "persistence ownership and diagnostics allocate before drawing, with "
         "no post-submit exception or hybrid fallback boundary");
}

void testQueueFullAndClosedAreRecoverableOnlyAfterSuccessfulSkinDraw() {
  const auto exercise = [](bool closeQueue) {
    SessionFixture fixture;
    if (!fixture.ready()) {
      return;
    }
    fixture.addBgaMarker();
    if (closeQueue) {
      fixture.configurationWrites().close();
    } else {
      for (std::size_t index = 0;
           index < SkinConfigurationWriteQueue::maxPending; ++index) {
        SkinConfigurationWriteRequest request;
        request.frameSerial = index + 1;
        expect(fixture.configurationWrites().enqueue(std::move(request)) ==
                   SkinConfigurationEnqueueResult::Enqueued,
               "queue-full fixture fills every slot");
      }
    }
    const std::vector<SkinFrameMutation> writes{
        persisted(SetSkinOption{.key = "lane-cover", .value = 1})};
    expect(fixture.session().prepareFrameForTesting(
               stateAt(1), projectionAt(1), {}, writes) ==
               PresentationFrameOutcome::Ready,
           "recoverable queue fixture prepares normally");
    SessionBgaSubmitter bga;
    RenderContext context;
    const auto exactBga = bgaFrame(closeQueue ? 83 : 82);
    const auto result = fixture.session().render(context, exactBga, bga);
    expect(result.outcome == PresentationFrameOutcome::RecoverableFailure &&
               result.frameSerial == 1 && result.failure &&
               result.failure->frameSerial == 1 &&
               result.failure->diagnostic.code ==
                   (closeQueue
                        ? "skin.session.configuration_write_queue_closed"
                        : "skin.session.configuration_write_queue_full") &&
               result.submittedMode == PresentationMode::Skin &&
               result.bgaCompositeMode ==
                   GameplayBgaCompositeMode::EmbeddedSkin &&
               result.preparedBga &&
               sameBgaFrame(*result.preparedBga, exactBga) &&
               bga.preflightCalls == 1 && bga.commitCalls == 1 &&
               bga.submitCalls == 1 && bga.finalizeCalls == 1 &&
               bga.fullscreenCalls == 0,
           closeQueue
               ? "closed persistence queue reports recoverable failure after "
                 "the complete skin draw"
               : "full persistence queue reports recoverable failure after "
                 "the complete skin draw");
  };
  exercise(false);
  exercise(true);
}

void testEditableTextUsesEndCursorUtf8BackspaceAndReturnCommit() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addEditableText(84, "A", SkinStringWriterId{1}, 100.0);
  SessionBgaSubmitter bga;
  RenderContext context;
  const UiLogicalPoint inside{.x = 110.0F, .y = 610.0F};

  expect(!fixture.session().focusTextInput(inside, 1'000),
         "text cannot focus before its matching skin frame submits");
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "editable text frame prepares");
  const auto first = fixture.session().render(context, bgaFrame(1), bga);
  expect(first.outcome == PresentationFrameOutcome::Ready &&
             fixture.session().focusTextInput(inside, 2'000) &&
             fixture.session().hasFocusedTextInput(),
         "submitted editable text takes the one session focus");
  expect(fixture.session().appendTextInput("é") &&
             fixture.session().backspaceTextInput() &&
             fixture.session().appendTextInput("한") &&
             fixture.session().commitTextInput(3'000) &&
             !fixture.session().hasFocusedTextInput() &&
             !fixture.session().commitTextInput(3'001),
         "text editing starts at the end, removes one UTF-8 codepoint, and "
         "Return queues one commit");

  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready,
         "the queued UTF-8 value reaches its typed string writer");
  const auto second = fixture.session().render(context, bgaFrame(2), bga);
  expect(second.outcome == PresentationFrameOutcome::Ready &&
             fixture.session().prepareFrame(stateAt(3), projectionAt(3)) ==
                 PresentationFrameOutcome::Ready,
         "a submitted commit is exact once and is not queued again");
  const auto third = fixture.session().render(context, bgaFrame(3), bga);
  expect(third.outcome == PresentationFrameOutcome::Ready,
         "the post-commit frame remains renderable");
}

void testEditableTextOutsideClickCommitsAndFocusTransferIsOrdered() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addEditableText(84, "A", SkinStringWriterId{2}, 100.0);
  fixture.addEditableText(85, "B", SkinStringWriterId{3}, 220.0);
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "focus-transfer text frame prepares");
  const auto first = fixture.session().render(context, bgaFrame(1), bga);
  expect(first.outcome == PresentationFrameOutcome::Ready,
         "focus-transfer text frame submits");

  expect(fixture.session().focusTextInput({.x = 110.0F, .y = 610.0F},
                                          2'000) &&
             fixture.session().appendTextInput("1") &&
             fixture.session().focusTextInput({.x = 230.0F, .y = 610.0F},
                                              3'000) &&
             fixture.session().appendTextInput("2") &&
             !fixture.session().focusTextInput({.x = 400.0F, .y = 300.0F},
                                               4'000) &&
             !fixture.session().hasFocusedTextInput(),
         "focus transfer commits the first editor and an outside click "
         "commits the second");
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready,
         "focus-transfer commits invoke typed writers in pointer order");
  const auto second = fixture.session().render(context, bgaFrame(2), bga);
  expect(second.outcome == PresentationFrameOutcome::Ready,
         "ordered focus-transfer commits submit with the matching frame");
}

void testEditableTextOutsideClickPreservesGlobalInteractionOrder() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addEditableText(84, "A", SkinStringWriterId{5}, 100.0);
  fixture.addOrderedClickableImage(300.0);
  fixture.addTouchGeometry(SkinFloatWriterId{5}, std::nullopt, 500.0);
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(1), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "heterogeneous interaction fixture publishes all three controls");

  expect(fixture.session().focusTextInput({.x = 110.0F, .y = 610.0F},
                                          1'000) &&
             fixture.session().appendTextInput("1") &&
             !fixture.session().focusTextInput({.x = 320.0F, .y = 610.0F},
                                               2'000),
         "outside click queues the focused text commit before hit dispatch");
  const UiLogicalPoint imagePoint{.x = 320.0F, .y = 610.0F};
  const auto imageHit = fixture.session().hitTestUiControl(imagePoint);
  const UiLogicalPoint sliderPoint{.x = 550.0F, .y = 610.0F};
  const auto sliderHit = fixture.session().hitTestUiControl(sliderPoint);
  expect(imageHit.kind == PresentationUiControlKind::Image &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = imagePoint,
                  .eventMicros = 2'001,
                  .hit = imageHit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true} &&
             (sliderHit.kind == PresentationUiControlKind::Slider ||
              sliderHit.kind == PresentationUiControlKind::LaneCover) &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 2,
                  .uiPoint = sliderPoint,
                  .eventMicros = 2'002,
                  .hit = sliderHit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true},
         "the following image event and float writer join the same queue");
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(2), bga).outcome ==
                 PresentationFrameOutcome::Ready &&
             fixture.interactionOrder(3) ==
                 std::optional<std::string>{"string,event,float,"},
         "string, image-event, and float callbacks retain pointer order "
         "across successful submission");
}

void testEditableStringWriterDoesNotRunWhenSubmissionPreflightFails() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addEditableText(84, "A", SkinStringWriterId{1}, 100.0);
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(1), bga).outcome ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().focusTextInput(
                 {.x = 110.0F, .y = 610.0F}, 1'000) &&
             fixture.session().appendTextInput("한") &&
             fixture.session().commitTextInput(2'000),
         "failed-submit fixture queues one exact UTF-8 string commit");
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready,
         "string callback remains pending while the visual frame prepares");
  fixture.quadBackend().preflightReady = false;
  const auto failed = fixture.session().render(context, bgaFrame(2), bga);
  expect(failed.outcome == PresentationFrameOutcome::CriticalFailure &&
             fixture.textUtf8Count(3) == std::optional<std::int64_t>{0},
         "renderer preflight failure cancels the queued string without any "
         "global Lua side effect");
}

void testEditableTextBoundsFocusedAndQueuedUtf8() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addEditableText(84, "A", SkinStringWriterId{5}, 100.0);
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(1), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "bounded editable fixture publishes its field");
  const UiLogicalPoint point{.x = 110.0F, .y = 610.0F};

  expect(fixture.session().focusTextInput(point, 500) &&
             fixture.session().appendTextInput(std::string(32'767, 'x')) &&
             !fixture.session().appendTextInput("x") &&
             fixture.session().hasFocusedTextInput(),
         "focused text accepts its exact codepoint bound and rejects the "
         "next complete codepoint");
  fixture.session().cancelTextInput();

  std::string exactBytes(3, 'x');
  exactBytes.reserve(65'535);
  for (std::size_t index = 0; index < 16'383; ++index) {
    exactBytes.append("\xF4\x8F\xBF\xBF");
  }
  expect(fixture.session().focusTextInput(point, 750) &&
             fixture.session().appendTextInput(exactBytes) &&
             !fixture.session().appendTextInput("x") &&
             fixture.session().hasFocusedTextInput(),
         "focused text accepts its exact byte bound and rejects the next "
         "complete codepoint");
  fixture.session().cancelTextInput();

  expect(fixture.session().focusTextInput(point, 1'000) &&
             !fixture.session().appendTextInput(std::string(32'768, 'x')) &&
             fixture.session().hasFocusedTextInput(),
         "focused text rejects a codepoint count beyond its fixed bound");
  fixture.session().cancelTextInput();

  std::string fourByteText;
  fourByteText.reserve(65'540);
  for (std::size_t index = 0; index < 16'385; ++index) {
    fourByteText.append("\xF4\x8F\xBF\xBF");
  }
  expect(fixture.session().focusTextInput(point, 2'000) &&
             !fixture.session().appendTextInput(fourByteText) &&
             fixture.session().hasFocusedTextInput(),
         "focused text rejects valid UTF-8 bytes beyond its fixed byte bound");
  fixture.session().cancelTextInput();

  std::string queuedValue;
  queuedValue.reserve(40'000);
  for (std::size_t index = 0; index < 10'000; ++index) {
    queuedValue.append("\xF4\x8F\xBF\xBF");
  }
  expect(fixture.session().focusTextInput(point, 3'000) &&
             fixture.session().appendTextInput(queuedValue) &&
             fixture.session().commitTextInput(3'001) &&
             fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().focusTextInput(point, 4'000) &&
             fixture.session().appendTextInput(queuedValue) &&
             !fixture.session().commitTextInput(4'001) &&
             fixture.session().hasFocusedTextInput(),
         "aggregate queued string bytes include a prepared but unsubmitted "
         "frame and reject another commit without losing the focused edit");
  fixture.session().cancelTextInput();
  expect(fixture.session().render(context, bgaFrame(2), bga).outcome ==
             PresentationFrameOutcome::Ready,
         "the bounded pending string still completes after successful submit");
}

void testEditableTextCancellationTeardownAndNoneditableRejection() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addEditableText(84, "A", SkinStringWriterId{4}, 100.0);
  fixture.addEditableText(85, "B", SkinStringWriterId{4}, 220.0, false);
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "cancellable text frame prepares");
  const auto first = fixture.session().render(context, bgaFrame(1), bga);
  const UiLogicalPoint editablePoint{.x = 110.0F, .y = 610.0F};
  const auto textHit = fixture.session().hitTestUiControl(editablePoint);
  expect(first.outcome == PresentationFrameOutcome::Ready &&
             !fixture.session().focusTextInput({.x = 230.0F, .y = 610.0F},
                                              2'000) &&
             textHit.kind == PresentationUiControlKind::Text &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 77,
                  .uiPoint = editablePoint,
                  .eventMicros = 3'000,
                  .hit = textHit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true} &&
             fixture.session().appendTextInput("x"),
         "noneditable text rejects focus while a semantic pointer captures "
         "editable text without exposing SDL ownership");
  (void)fixture.session().endPresentationTouch(
      {.pointerId = 77,
       .uiPoint = editablePoint,
       .eventMicros = 3'001,
       .hit = textHit},
      false);
  fixture.session().cancelTextInput();
  expect(!fixture.session().hasFocusedTextInput() &&
             fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
                 PresentationFrameOutcome::Ready,
         "explicit cancellation discards the uncommitted writer");
  const auto second = fixture.session().render(context, bgaFrame(2), bga);
  expect(second.outcome == PresentationFrameOutcome::Ready &&
             fixture.session().focusTextInput({.x = 110.0F, .y = 610.0F},
                                              4'000) &&
             fixture.session().appendTextInput("y"),
         "a later editor can focus before session teardown");
  fixture.destroySession();
  expect(fixture.verifyTextCancellation(3),
         "session teardown cancels focused text without invoking its writer");
}

void testTouchCaptureLifecycleKeepsWritingCapturedSlidersDuringDrag() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry(SkinFloatWriterId{6});
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "touch fixture prepares");
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().render(context, bgaFrame(91), bga).outcome ==
             PresentationFrameOutcome::Ready,
         "touch fixture publishes geometry after draw");
  const UiLogicalPoint point{.x = 150.0F, .y = 610.0F};
  const auto hit = fixture.session().hitTestUiControl(point);
  const PresentationTouchEvent down{
      .pointerId = 1, .uiPoint = point, .eventMicros = 1'000, .hit = hit};
  expect(hit.kind == PresentationUiControlKind::Slider && hit.writer,
         "touch fixture exposes an authored slider hit");
  expect(fixture.session().beginPresentationTouch(down) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true} &&
             fixture.session().beginPresentationTouch(down) ==
                 PresentationTouchResult{},
         "Down captures a slider exactly once");
  expect(fixture.session().updatePresentationTouch(
             {.pointerId = 1,
              .uiPoint = {.x = 180.0F, .y = 610.0F},
              .eventMicros = 1'001,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true},
         "a captured slider Move stays consumed");
  expect(fixture.session().endPresentationTouch(
             {.pointerId = 1,
              .uiPoint = point,
              .eventMicros = 1'002,
              .hit = hit},
             false) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true} &&
             fixture.session().updatePresentationTouch(down) ==
                 PresentationTouchResult{},
         "Up clears the slider capture");

  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(92), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "a slider drag queues its Down and in-track Move values for the next "
         "frame");
  auto stale = hit;
  ++stale.layoutRevision;
  auto forged = hit;
  ++forged.sourceObject;
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 2,
              .uiPoint = point,
              .eventMicros = 2'000,
              .hit = stale}) == PresentationTouchResult{} &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 3,
                  .uiPoint = point,
                  .eventMicros = 2'001,
                  .hit = forged}) == PresentationTouchResult{} &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 4,
                  .uiPoint = {.x = std::numeric_limits<float>::quiet_NaN(),
                              .y = point.y},
                  .eventMicros = 2'002,
                  .hit = hit}) == PresentationTouchResult{},
         "stale, forged, and nonfinite Down events fail closed");

  for (long long pointer = 10; pointer < 42; ++pointer) {
    expect(fixture.session().beginPresentationTouch(
               {.pointerId = pointer,
                .uiPoint = point,
                .eventMicros = pointer,
                .hit = hit}) ==
               PresentationTouchResult{.consumed = true,
                                       .excludeFromGameplay = true},
           "each of the first 32 distinct pointers captures");
  }
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 42,
              .uiPoint = point,
              .eventMicros = 42,
              .hit = hit}) == PresentationTouchResult{},
         "the 33rd simultaneous pointer fails closed");
  fixture.session().cancelPresentationTouches(3'000);
  expect(fixture.session().updatePresentationTouch(
             {.pointerId = 10,
              .uiPoint = point,
              .eventMicros = 3'001,
              .hit = hit}) == PresentationTouchResult{},
         "cancel-all clears every capture without a writer invocation");

  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 50,
              .uiPoint = point,
              .eventMicros = 50,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true} &&
             fixture.session().endPresentationTouch(
                 {.pointerId = 50,
                  .uiPoint = point,
                  .eventMicros = 51,
                  .hit = forged},
                 false) == PresentationTouchResult{} &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 50,
                  .uiPoint = point,
                  .eventMicros = 52,
                  .hit = hit}) == PresentationTouchResult{},
         "a mismatched End fails closed but still releases its capture");
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 51,
              .uiPoint = point,
              .eventMicros = 53,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true} &&
             fixture.session().endPresentationTouch(
                 {.pointerId = 51,
                  .uiPoint = point,
                  .eventMicros = 54,
                  .hit = hit},
                 true) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true} &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 51,
                  .uiPoint = point,
                  .eventMicros = 55,
                  .hit = hit}) == PresentationTouchResult{},
         "a cancelled matching End consumes and releases without a writer");
  expect(fixture.writerDragValues(3) ==
             std::optional<std::string>{"50,80,"},
         "dragging a captured gameplay slider invokes its writer at each "
         "in-track pointer position");
}

void testImageActTouchQueuesPinnedEventOnDown() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addClickableImage();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(120), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "Image act fixture publishes its first rendered hit region");
  const UiLogicalPoint point{.x = 130.0F, .y = 610.0F};
  const auto hit = fixture.session().hitTestUiControl(point);
  expect(hit.kind == PresentationUiControlKind::Image &&
             hit.eventBinding == 1U &&
             fixture.session().beginPresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = point,
                  .eventMicros = 2'000,
                  .hit = hit}) ==
                 PresentationTouchResult{.consumed = true,
                                         .excludeFromGameplay = true},
         "Image act consumes the primary pointer-down and queues its event");
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(121), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "the queued Image act event reaches the next frame transaction");
}

void testViewportChangeCancelsCapturesAndInvalidatesPublishedGeometry() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry(SkinFloatWriterId{3});
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready,
         "viewport fixture prepares");
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().render(context, bgaFrame(101), bga).outcome ==
             PresentationFrameOutcome::Ready,
         "viewport fixture publishes geometry");
  const UiLogicalPoint point{.x = 150.0F, .y = 610.0F};
  const auto hit = fixture.session().hitTestUiControl(point);
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 1,
              .uiPoint = point,
              .eventMicros = 1,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true},
         "viewport fixture captures the failing writer before reset");
  const auto oldLayoutRevision = fixture.session().touchLayoutRevision();
  const auto oldHitRevision = fixture.session().touchHitRegionsRevision();
  const auto originalIdentity = fixture.session().identity();
  const ViewportSettings customViewport{
      .mode = ViewportMode::Custom,
      .customBase = CustomViewportBase::Fit,
      .scaleX = 0.75F,
      .scaleY = 0.75F,
      .translateX = 15.0F,
      .translateY = -10.0F};
  fixture.session().setViewport(customViewport);
  expect(fixture.session().touchLayoutRevision() != oldLayoutRevision &&
             fixture.session().touchHitRegionsRevision() != oldHitRevision &&
             fixture.session().touchLayout().laneRegions.empty() &&
             fixture.session().touchHitRegions().empty() &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 1,
                  .uiPoint = point,
                  .eventMicros = 2,
                  .hit = hit}) == PresentationTouchResult{} &&
             fixture.configurationWrites().drain().empty(),
         "setViewport cancels captures, clears geometry, advances revisions, "
         "and never persists directly");
  // The captured writer throws if invoked. Ready proves the viewport change
  // discarded its queued old-layout Down before the next transaction.
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready,
         "viewport reset discards the captured old-layout writer");

  const auto submitsBeforePendingReset = bga.submitCalls;
  fixture.session().setViewport(customViewport);
  const auto discarded =
      fixture.session().render(context, bgaFrame(102), bga);
  expect(discarded.outcome == PresentationFrameOutcome::CriticalFailure &&
             !discarded.preparedBga &&
             bga.preflightCalls == submitsBeforePendingReset &&
             bga.commitCalls == submitsBeforePendingReset &&
             bga.submitCalls == submitsBeforePendingReset &&
             bga.finalizeCalls == submitsBeforePendingReset &&
             bga.fullscreenCalls == 0 &&
             fixture.configurationWrites().drain().empty(),
         "setViewport discards an already prepared old-viewport frame before "
         "any BGA, skin, fullscreen, or persistence submission");

  expect(fixture.session().prepareFrame(stateAt(3), projectionAt(3)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(103), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "the next frame republishes with the custom viewport");
  const auto customLayout = fixture.session().touchLayout();
  const auto customRegions = fixture.session().touchHitRegions();
  UiLogicalPoint customControlPoint;
  if (!customRegions.empty()) {
    customControlPoint = {
        .x = (customRegions.front().boundary[0].x +
              customRegions.front().boundary[2].x) /
             2.0F,
        .y = (customRegions.front().boundary[0].y +
              customRegions.front().boundary[2].y) /
             2.0F};
  }
  const auto customHit =
      fixture.session().hitTestUiControl(customControlPoint);
  const auto &identityAfter = fixture.session().identity();
  expect(customLayout.laneRegions.size() == 2 &&
             std::abs(customLayout.laneRegions[0].bottomLeft.x -
                      0.1302083F) < 0.0001F &&
             std::abs(customLayout.laneRegions[0].bottomLeft.y -
                      0.5601852F) < 0.0001F &&
             std::abs(customLayout.laneRegions[1].topRight.x -
                      0.2083333F) < 0.0001F &&
             std::abs(customLayout.laneRegions[1].topRight.y -
                      0.2129630F) < 0.0001F,
         "custom viewport republishes window-normalized touch coordinates");
  expect(customRegions.size() == 1 &&
             customHit.kind == PresentationUiControlKind::LaneCover &&
             customHit.writer == SkinFloatWriterId{3},
         "custom viewport republishes the transformed lane-cover hit region");
  expect(identityAfter.sessionSerial == originalIdentity.sessionSerial &&
             identityAfter.profileId == originalIdentity.profileId &&
             identityAfter.entry == originalIdentity.entry &&
             identityAfter.revisionDigest == originalIdentity.revisionDigest &&
             identityAfter.configurationDigest ==
                 originalIdentity.configurationDigest,
         "viewport changes do not alter the five-field session identity");
}

void testViewportGeometryChangeCancelsOldInputAndPreservesSessionIdentity() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry(SkinFloatWriterId{3});
  SessionBgaSubmitter bga;
  RenderContext context;
  const ViewportSettings customViewport{
      .mode = ViewportMode::Custom,
      .customBase = CustomViewportBase::Fit,
      .scaleX = 0.75F,
      .scaleY = 0.75F,
      .translateX = 15.0F,
      .translateY = -10.0F};
  fixture.session().setViewport(customViewport);
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(201), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "resize fixture publishes its initial touch geometry");

  const auto initialRegions = fixture.session().touchHitRegions();
  UiLogicalPoint oldPoint;
  if (!initialRegions.empty()) {
    oldPoint = {
        .x = (initialRegions.front().boundary[0].x +
              initialRegions.front().boundary[2].x) /
             2.0F,
        .y = (initialRegions.front().boundary[0].y +
              initialRegions.front().boundary[2].y) /
             2.0F};
  }
  const auto oldHit = fixture.session().hitTestUiControl(oldPoint);
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 91,
              .uiPoint = oldPoint,
              .eventMicros = 1,
              .hit = oldHit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true},
         "resize fixture owns one authored capture and queued Down writer");

  const auto identityBefore = fixture.session().identity();
  const auto layoutRevisionBefore = fixture.session().touchLayoutRevision();
  const auto hitRevisionBefore =
      fixture.session().touchHitRegionsRevision();
  fixture.session().updateViewportGeometry(
      {.x = 100.0, .y = 50.0, .width = 1280.0, .height = 720.0});

  expect(fixture.session().touchLayoutRevision() != layoutRevisionBefore &&
             fixture.session().touchHitRegionsRevision() !=
                 hitRevisionBefore &&
             fixture.session().touchLayout().laneRegions.empty() &&
             fixture.session().touchHitRegions().empty() &&
             fixture.session().updatePresentationTouch(
                 {.pointerId = 91,
                  .uiPoint = oldPoint,
                  .eventMicros = 2,
                  .hit = oldHit}) == PresentationTouchResult{} &&
             fixture.configurationWrites().drain().empty(),
         "safe-area replacement cancels old captures, queued writers, and "
         "published geometry");

  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(202), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "the next frame republishes against the replacement safe area");
  const auto layout = fixture.session().touchLayout();
  const auto identityAfter = fixture.session().identity();
  expect(layout.laneRegions.size() == 2 &&
             std::abs(layout.laneRegions[0].bottomLeft.x - 0.1822917F) <
                 0.0001F &&
             std::abs(layout.laneRegions[0].bottomLeft.y - 0.6064815F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.x - 0.2604167F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.y - 0.2592593F) <
                 0.0001F,
         "rotation republishes lane routing with the unchanged Custom "
         "viewport settings in the new UI-logical geometry");
  expect(identityAfter.sessionSerial == identityBefore.sessionSerial &&
             identityAfter.profileId == identityBefore.profileId &&
             identityAfter.entry == identityBefore.entry &&
             identityAfter.revisionDigest == identityBefore.revisionDigest &&
             identityAfter.configurationDigest ==
                 identityBefore.configurationDigest,
         "geometry-only replacement preserves immutable activation identity");
}

void testTouchLayoutNormalizesAgainstTheWholeWindowWithSafeOrigin() {
  SessionFixture fixture(
      37, {.x = 100.0, .y = 50.0, .width = 1280.0, .height = 720.0});
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(104), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "safe-origin fixture publishes geometry");
  const auto layout = fixture.session().touchLayout();
  expect(layout.laneRegions.size() == 2 &&
             std::abs(layout.laneRegions[0].bottomLeft.x - 0.1041667F) <
                 0.0001F &&
             std::abs(layout.laneRegions[0].bottomLeft.y - 0.6944444F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.x - 0.2083333F) <
                 0.0001F &&
             std::abs(layout.laneRegions[1].topRight.y - 0.2314815F) <
                 0.0001F,
         "nonzero safe origin is retained in UI coordinates before whole-"
         "window normalization");
}

void testSuccessfulGeometryChangesOnlyHitRevisionAndTeardownDiscardsState() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  fixture.addBgaMarker();
  fixture.addTouchGeometry();
  SessionBgaSubmitter bga;
  RenderContext context;
  expect(fixture.session().prepareFrame(stateAt(1), projectionAt(1)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(111), bga).outcome ==
                 PresentationFrameOutcome::Ready,
         "first geometry frame succeeds");
  const auto layoutRevision = fixture.session().touchLayoutRevision();
  const auto hitRevision = fixture.session().touchHitRegionsRevision();
  fixture.model().model.destinations.back().presentation.frames.front().x =
      300.0;
  expect(fixture.session().prepareFrame(stateAt(2), projectionAt(2)) ==
             PresentationFrameOutcome::Ready &&
             fixture.session().render(context, bgaFrame(112), bga).outcome ==
                 PresentationFrameOutcome::Ready &&
             fixture.session().touchLayoutRevision() == layoutRevision &&
             fixture.session().touchHitRegionsRevision() != hitRevision,
         "successful animated control geometry advances hit publication while "
         "lane topology remains stable");
  const UiLogicalPoint point{.x = 350.0F, .y = 610.0F};
  const auto hit = fixture.session().hitTestUiControl(point);
  expect(fixture.session().beginPresentationTouch(
             {.pointerId = 9,
              .uiPoint = point,
              .eventMicros = 9,
              .hit = hit}) ==
             PresentationTouchResult{.consumed = true,
                                     .excludeFromGameplay = true} &&
             fixture.session().prepareFrame(stateAt(3), projectionAt(3)) ==
                 PresentationFrameOutcome::Ready,
         "teardown fixture owns both a capture and pending frame");
  const auto submitsBeforeTeardown = bga.submitCalls;
  fixture.destroySession();
  expect(bga.submitCalls == submitsBeforeTeardown &&
             fixture.configurationWrites().drain().empty(),
         "session destruction discards pending frame, captures, and writes "
         "without submission or enqueue");
}

void testLegacyRendererAdapterBeginsInternallyAndRejectsDoubleBegin() {
  SessionFixture fixture;
  if (!fixture.ready()) {
    return;
  }
  const auto sessionFrame = fixture.session().prepareFrame(
      stateAt(1), projectionAt(1), {});
  expect(sessionFrame.ready(),
         "session-owned renderer evaluation consumes external ownership");

  SerialOnlyState state(2);
  const auto inputs = SkinFrameInputs{
      .frameSerial = 2,
      .sessionSerial = 37,
      .visualTimeMicros = 20'000,
      .model = fixture.model(),
      .configuration = fixture.configuration(),
      .resources = fixture.resources(),
      .viewport = fixture.viewport(),
      .runtime = &fixture.runtime(),
      .state = state};
  auto unrestrictedInputs = inputs;
  unrestrictedInputs.safetyPolicy = SkinSafetyPolicy(
      SkinSafetyLevel::Unrestricted);
  expect(skinFrameMaximumCommands(inputs) ==
             SkinCommandPolicy::maximumCommands &&
             skinFrameMaximumGlyphInstances(inputs) ==
                 SkinCommandPolicy::maximumGlyphInstances &&
             skinFrameMaximumCommands(unrestrictedInputs) ==
                 std::numeric_limits<std::size_t>::max() &&
             skinFrameMaximumGlyphInstances(unrestrictedInputs) ==
                 std::numeric_limits<std::size_t>::max(),
         "Unrestricted frame inputs lift the renderer command and glyph limits");
  const auto first = fixture.renderer().evaluateFrame(inputs);
  const auto second = fixture.renderer().evaluateFrame(inputs);
  expect(first.submitReady.has_value(),
         "legacy renderer adapter still owns an internal runtime begin");
  expect(!second.submitReady &&
             hasDiagnostic(second.diagnostics, "skin_lua_frame_invalid"),
         "legacy double begin is rejected deterministically");
}

void testResultLuaSessionBindsMainStateDuringConfiguredLoad() {
  ActivationFixture fixture({.skinType = 7,
                             .audioBearing = true,
                             .requireResultConfiguredState = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  expect(created.session != nullptr && created.diagnostics.empty() &&
             fixture.audioState()->loads.size() == 1 &&
             fixture.audioState()->plays.size() == 3,
         "result Lua session configures against the result main_state and "
         "application audio backend");
}

void testResultLuaSessionRoutesOpenIrEvent() {
  ActivationFixture fixture({.skinType = 7, .resultEventExec = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  RenderContext context;
  expect(created.session != nullptr &&
             created.session->render(context, {}, 1, 0) &&
             created.session->takeQueuedBuiltinEventIds() ==
                 std::vector<int>{210},
         "result Lua main_state.event_exec resolves a custom event before "
         "routing its open_ir action through ResultScene");
}

void testResultLuaSessionDefersNestedCustomEventsToTheNextFrame() {
  ActivationFixture fixture({.skinType = 7, .resultNestedEventExec = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  RenderContext context;
  expect(created.session != nullptr &&
             created.session->render(context, {}, 1, 0) &&
             created.session->takeQueuedBuiltinEventIds().empty() &&
             created.session->render(context, {}, 2, 1) &&
             created.session->takeQueuedBuiltinEventIds() ==
                 std::vector<int>{210},
         "nested result Lua custom events remain queued for the following frame");
}

void testResultLuaSessionManualCustomEventSuppressesAutomaticRepeat() {
  ActivationFixture fixture({.skinType = 7, .resultIntervalEventExec = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  RenderContext context;
  expect(created.session != nullptr &&
             created.session->render(context, {}, 1, 0) &&
             created.session->takeQueuedBuiltinEventIds() ==
                 std::vector<int>{210},
         "manual result custom events advance the shared automatic interval clock");
}

void testResultLuaSessionRejectsRecursiveCustomEvents() {
  ActivationFixture fixture({.skinType = 7, .resultRecursiveEventExec = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  RenderContext context;
  expect(created.session != nullptr &&
             !created.session->render(context, {}, 1, 0) &&
             hasDiagnostic(created.session->takeLastDiagnostics(),
                           "skin.result_session.custom_event_cycle"),
         "recursive result custom events fail with a skin diagnostic instead of "
         "recursing through the event dispatcher");
}

void testResultLuaSessionUsesTheLastDuplicateCustomEventDefinition() {
  ActivationFixture fixture({.skinType = 7, .resultDuplicateEventExec = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  RenderContext context;
  expect(created.session != nullptr &&
             created.session->render(context, {}, 1, 0) &&
             created.session->takeQueuedBuiltinEventIds().empty(),
         "duplicate result custom events replace both automatic and manual "
         "actions with the final Beatoraja definition");
}

void testResultLuaSessionUsesTheLastDuplicateCustomTimerDefinition() {
  ActivationFixture fixture({.skinType = 7, .resultDuplicateTimerExec = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  RenderContext context;
  expect(created.session != nullptr &&
             created.session->render(context, {}, 1, 0) &&
             created.session->takeQueuedBuiltinEventIds().empty(),
         "duplicate result custom timers replace the obsolete timer callback");
}

void testStaticResultSessionRunsCustomBuiltinEvent() {
  ActivationFixture fixture({.skinType = 7, .staticResultCustomEvent = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  RenderContext context;
  expect(created.session != nullptr &&
             created.session->render(context, {}, 1, 0) &&
             created.session->takeQueuedBuiltinEventIds() ==
                 std::vector<int>{210},
         "static result custom events evaluate built-in conditions and actions");
}

void testResultSkinInputAvailabilityMatchesResultTimer() {
  expect(!resultSkinInputAvailable(1'000, 999'999) &&
             resultSkinInputAvailable(1'000, 1'000'000) &&
             resultSkinInputAvailable(-1, 0),
         "result pointer input uses the same threshold as timer 1");
}

void testResultSessionRefreshesForAsynchronousRankingNames() {
  ActivationFixture fixture(
      {.skinType = 7, .resourceBearing = true, .audioBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto context = fixture.resultContext({.courseTitle = "Initial result"});
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           std::move(context));
  const ResultSkinData rankedData{
      .courseTitle = "Initial result",
      .irRankingEntries = {{.rank = 1,
                            .playerName = "\xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88"}}};
  expect(created.session != nullptr,
         "result session accepts a result-typed resource skin");
  expect(fixture.device()->createCalls == 2,
         "result session uploads its image and runtime text atlas");
  expect(fixture.liveCounters()->snapshot() ==
             SkinLiveResourceSnapshot{.liveTextures = 2,
                                      .liveResources = 1,
                                      .liveAudioIdentities = 1},
         "result session accounts for its result-typed resources");
  if (!created.session) {
    return;
  }
  const auto activeResources = fixture.liveCounters()->snapshot();
  const auto initialTextureCreates = fixture.device()->createCalls;
  const auto initialTextureDestroys = fixture.device()->destroyCalls;
  fixture.device()->failNextCreate();
  const bool refreshFailed =
      !created.session->refreshRuntimeStrings(rankedData);
  expect(refreshFailed &&
             created.session->requiresRuntimeStringRefresh(rankedData) &&
             fixture.device()->destroyCalls == initialTextureDestroys &&
             fixture.liveCounters()->snapshot() == activeResources,
         "a failed result text refresh retains the published resource catalog");
  expect(created.session->refreshRuntimeStrings(rankedData) &&
             !created.session->requiresRuntimeStringRefresh(rankedData) &&
             fixture.device()->createCalls == initialTextureCreates + 3 &&
             fixture.device()->destroyCalls == initialTextureDestroys + 2 &&
             fixture.liveCounters()->snapshot() == activeResources &&
             fixture.audioState()->loads.size() == 1,
         "result sessions replace runtime glyph atlases without recreating "
         "their configured Lua runtime");
}

void testResultSessionRefreshesForAllStringSelectors() {
  ActivationFixture fixture({.skinType = 7, .resourceBearing = true});
  if (!fixture.ready()) {
    return;
  }
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           fixture.resultContext());
  const ResultSkinData stringSelectorData{
      .pacemaker = ResultPacemakerData{.label = "MAX -"},
      .chartMd5 = "0123456789abcdef0123456789abcdef",
      .chartSha256 =
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
  };
  expect(created.session != nullptr &&
             created.session->requiresRuntimeStringRefresh(stringSelectorData),
         "result font atlases refresh for pacemaker and chart-hash strings");
  if (created.session == nullptr) {
    return;
  }
  expect(created.session->refreshRuntimeStrings(stringSelectorData) &&
             !created.session->requiresRuntimeStringRefresh(stringSelectorData),
         "result font atlases retain every dynamically projected string");
}

void testResultSessionRejectsConfiguredModelForAnotherResultTarget() {
  ActivationFixture fixture({.skinType = 7, .configuredSkinType = 15});
  if (!fixture.ready()) {
    return;
  }
  auto context = fixture.resultContext();
  context.expectedSkinType = 7;
  auto created = ResultSkinSession::create(fixture.takeActivation(),
                                           std::move(context));
  expect(!created.session && hasDiagnostic(created.diagnostics,
                                            "skin.result_session.type_mismatch"),
         "result session rejects a configured model for another result target");
}

void testResultBridgeSupportsBeatorajaIrAvailabilityProperties() {
  ResultSkinStateBridge bridge({}, 1, 0);
  const auto offline = bridge.booleanProperty({50});
  const auto online = bridge.booleanProperty({51});
  const auto namedOffline =
      bridge.booleanProperty({.value = std::string{"ir_offline"}});
  const auto namedOnline =
      bridge.booleanProperty({.value = std::string{"ir_online"}});
  const auto inverseOffline =
      bridge.booleanProperty({.value = std::string{"!ir_offline"}});
  const auto inactiveNamedState =
      bridge.booleanProperty({.value = std::string{"course_random"}});
  expect(offline.supported && offline.value && online.supported && !online.value &&
             namedOffline.supported && namedOffline.value &&
             namedOnline.supported && !namedOnline.value &&
             inverseOffline.supported && !inverseOffline.value &&
             inactiveNamedState.supported && !inactiveNamedState.value,
         "result bridge mirrors Beatoraja's offline IR properties when no IR "
         "provider is active and resolves named BooleanPropertyFactory values");
}

void testResultBridgeUsesSourceDefaultScoreReferencesAndRateFallbacks() {
  RhythmState state(nullptr, false);
  state.judgeCount[PGreat] = 5;
  bms_parser::ChartMeta meta{.TotalNotes = 10};
  ResultSkinStateBridge bridge({.state = &state, .meta = &meta}, 1, 0);
  const auto bestNow = bridge.floatProperty({112}, SkinFloatPropertyDomain::Rate);
  const auto best = bridge.floatProperty({113}, SkinFloatPropertyDomain::Rate);
  const auto targetNow = bridge.floatProperty({114}, SkinFloatPropertyDomain::Rate);
  const auto target = bridge.floatProperty({115}, SkinFloatPropertyDomain::Rate);
  const auto floatBest =
      bridge.floatProperty({183}, SkinFloatPropertyDomain::FloatValue);
  const auto floatTarget =
      bridge.floatProperty({122}, SkinFloatPropertyDomain::FloatValue);
  const auto rankingPosition =
      bridge.floatProperty({8}, SkinFloatPropertyDomain::Rate);
  const auto loadProgress =
      bridge.floatProperty({102}, SkinFloatPropertyDomain::Rate);
  const auto level = bridge.floatProperty({103}, SkinFloatPropertyDomain::Rate);
  expect(bestNow.supported && bestNow.value == 0.0 && best.supported &&
             best.value == 0.0 && targetNow.supported &&
             targetNow.value == 0.0 && target.supported &&
             target.value == 0.0 && floatBest.supported &&
             floatBest.value == 0.0 && floatTarget.supported &&
             floatTarget.value == 0.0 && rankingPosition.supported &&
             rankingPosition.value == 0.0 && loadProgress.supported &&
             loadProgress.value == 1.0 && level.supported && level.value == 0.0,
         "result float properties preserve AbstractResult's default old and "
         "target ScoreData plus its non-selection RateType fallbacks");
}

void testResultBridgeRetainsIrRatePropertyFallbacks() {
  ResultSkinStateBridge bridge({}, 1, 0);
  const auto named = bridge.floatProperty(
      {.value = std::string{"ir_player_failed_rate"}},
      SkinFloatPropertyDomain::FloatValue);
  const auto numeric = bridge.floatProperty({.value = 211},
                                            SkinFloatPropertyDomain::FloatValue);
  const auto total = bridge.floatProperty({.value = 227},
                                          SkinFloatPropertyDomain::FloatValue);
  const double minimum = static_cast<double>(std::numeric_limits<float>::min());
  expect(named.supported && named.value == minimum && numeric.supported &&
             numeric.value == minimum && total.supported && total.value == minimum,
         "result IR-rate properties retain Float.MIN_VALUE when the ranking "
         "snapshot has no source clear-count histogram");
}

void testResultBridgeRecognizesBeatorajaNamedIrIntegerProperties() {
  ResultSkinStateBridge bridge({}, 1, 0);
  const auto clearCount = bridge.integerProperty(
      {.value = std::string{"ir_player_failed"}},
      SkinIntegerPropertyDomain::IntegerValue);
  const auto rate = bridge.integerProperty(
      {.value = std::string{"ir_player_failed_rate"}},
      SkinIntegerPropertyDomain::IntegerValue);
  const auto fraction = bridge.integerProperty(
      {.value = std::string{"ir_player_failed_rate_afterdot"}},
      SkinIntegerPropertyDomain::IntegerValue);
  const auto totalClear = bridge.integerProperty(
      {.value = 226}, SkinIntegerPropertyDomain::IntegerValue);
  const auto totalRate = bridge.integerProperty(
      {.value = 227}, SkinIntegerPropertyDomain::IntegerValue);
  const auto totalFraction = bridge.integerProperty(
      {.value = 241}, SkinIntegerPropertyDomain::IntegerValue);
  constexpr auto minimum =
      static_cast<std::int64_t>(std::numeric_limits<int>::min());
  expect(clearCount.supported && clearCount.value == minimum &&
             rate.supported && rate.value == minimum &&
             fraction.supported && fraction.value == minimum &&
             totalClear.supported && totalClear.value == minimum &&
             totalRate.supported && totalRate.value == minimum &&
             totalFraction.supported && totalFraction.value == minimum,
         "result bridge exposes every Beatoraja IR aggregate integer property "
         "with its unavailable-ranking fallback");
}

void testResultBridgeKeepsMainStateGaugeHelpersGameplayOnly() {
  RhythmState state(nullptr, false);
  state.gaugeValues[gaugeTypeIndex(GaugeType::Normal)] = 82.0F;
  ResultSkinStateBridge bridge({.state = &state}, 1, 2'500);
  const auto gauge = bridge.floatProperty(
      {.value = std::string{"lua_gauge"}}, SkinFloatPropertyDomain::Rate);
  const auto gaugeType = bridge.integerProperty(
      {.value = std::string{"lua_gauge_type"}},
      SkinIntegerPropertyDomain::IntegerValue);
  const auto time = bridge.integerProperty({.value = std::string{"time"}},
                                           SkinIntegerPropertyDomain::IntegerValue);
  expect(gauge.supported && gauge.value == 0.0 && gaugeType.supported &&
             gaugeType.value == 0 && time.supported && time.value == 2'500'000,
         "main_state gauge helpers retain AbstractResult's zero fallback while "
         "main_state.time uses the result timer");
}

void testResultBridgeUsesPreparedArtworkAvailability() {
  bms_parser::ChartMeta declared{
      .StageFile = "missing-stage.png",
      .Banner = "missing-banner.png",
      .BackBmp = "missing-backbmp.png"};
  ResultSkinStateBridge unavailable({.meta = &declared}, 1, 0);
  ResultSkinStateBridge prepared({.meta = &declared,
                                  .stageFileAvailable = true,
                                  .bannerAvailable = true,
                                  .backBmpAvailable = true},
                                 1, 0);
  const auto stageFallback = unavailable.booleanProperty({190});
  const auto stageArtwork = unavailable.booleanProperty({191});
  const auto bannerFallback = unavailable.booleanProperty({192});
  const auto bannerArtwork = unavailable.booleanProperty({193});
  const auto backBmpFallback = unavailable.booleanProperty({194});
  const auto backBmpArtwork = unavailable.booleanProperty({195});
  expect(stageFallback.supported && stageFallback.value &&
             stageArtwork.supported && !stageArtwork.value &&
             bannerFallback.supported && bannerFallback.value &&
             bannerArtwork.supported && !bannerArtwork.value &&
             backBmpFallback.supported && backBmpFallback.value &&
             backBmpArtwork.supported && !backBmpArtwork.value &&
             prepared.booleanProperty({190}).supported &&
             !prepared.booleanProperty({190}).value &&
             prepared.booleanProperty({191}).supported &&
             prepared.booleanProperty({191}).value &&
             prepared.booleanProperty({192}).supported &&
             !prepared.booleanProperty({192}).value &&
             prepared.booleanProperty({193}).supported &&
             prepared.booleanProperty({193}).value &&
             prepared.booleanProperty({194}).supported &&
             !prepared.booleanProperty({194}).value &&
             prepared.booleanProperty({195}).supported &&
             prepared.booleanProperty({195}).value,
         "result artwork selectors follow successfully prepared resources, not "
         "unreadable chart declarations");
}

void testResultBridgeKeepsAutoplayOptionsOffOnResultScreens() {
  ResultSkinStateBridge bridge({.autoPlayResult = true}, 1, 0);
  const auto autoplayOff = bridge.booleanProperty({32});
  const auto autoplayOn = bridge.booleanProperty({33});
  const auto loaded = bridge.booleanProperty({81});
  expect(autoplayOff.supported && !autoplayOff.value && autoplayOn.supported &&
             !autoplayOn.value && loaded.supported && !loaded.value,
         "Beatoraja exposes neither autoplay option from an AbstractResult");
}

void testResultBridgeRecognizesScratchLongNotes() {
  bms_parser::ChartMeta meta{.TotalLongNotes = 0, .TotalBackSpinNotes = 1};
  ResultSkinStateBridge bridge({.meta = &meta}, 1, 0);
  const auto noLongNote = bridge.booleanProperty({172});
  const auto longNote = bridge.booleanProperty({173});
  expect(noLongNote.supported && !noLongNote.value && longNote.supported &&
             longNote.value,
         "result LN options count scratch long notes as long notes");
}

void testResultBridgeMatchesBeatorajaResultScoreFamilies() {
  RhythmState state(nullptr, false);
  state.judgeCount[PGreat] = 10;
  state.judgeCount[Bad] = 2;
  state.judgeCount[Poor] = 3;
  state.judgeCount[Kpoor] = 4;
  state.judgementFastSlowCount[Great].fast = 5;
  state.judgementFastSlowCount[Poor].slow = 6;
  bms_parser::ChartMeta meta{.KeyMode = 0, .TotalNotes = 10};
  state.maxCombo = 7;
  ResultSkinStateBridge bridge({
      .state = &state,
      .meta = &meta,
      .previousBest = ResultPreviousBestData{.score = 12,
                                              .maxCombo = 6,
                                              .badPoints = 9},
      .pacemaker = ResultPacemakerData{.targetScore = 18},
      .songReviewFavorite = 10,
  }, 1, 0);

  const auto rank = bridge.booleanProperty({220});
  const auto lowerRank = bridge.booleanProperty({221});
  const auto negatedRank = bridge.booleanProperty({-220});
  const auto poor = bridge.integerProperty({114}, {});
  const auto miss = bridge.integerProperty({420}, {});
  const auto poorPlusMiss = bridge.integerProperty({426}, {});
  const auto badPoorMiss = bridge.integerProperty({427}, {});
  const auto scoreRate = bridge.integerProperty({102}, {});
  const auto scoreRateAfterDot = bridge.integerProperty({103}, {});
  const auto point = bridge.integerProperty({100}, {});
  const auto bestRate = bridge.floatProperty({112}, {});
  const auto targetRate = bridge.floatProperty({114}, {});
  const auto pGreatRate = bridge.floatProperty({"rate_pgreat"}, {});
  const auto comboRate = bridge.floatProperty({145}, {});
  const auto exscoreRate = bridge.floatProperty({147}, {});
  const auto timingAverageFloat = bridge.floatProperty({374}, {});
  const auto imageFavorite = bridge.integerProperty(
      {90}, SkinIntegerPropertyDomain::ImageIndex);
  const auto nowRank = bridge.booleanProperty({200});
  const auto notNowRank = bridge.booleanProperty({207});
  const auto resultRank = bridge.booleanProperty({300});
  const auto updatedScore = bridge.booleanProperty({330});
  const auto updatedCombo = bridge.booleanProperty({331});
  const auto drawnMissCount = bridge.booleanProperty({1332});
  const auto targetWin = bridge.booleanProperty({352});
  const auto goodExists = bridge.booleanProperty({2243});
  const auto missExists = bridge.booleanProperty({2246});
  const auto targetMissCount = bridge.integerProperty({176}, {});
  const auto differenceMissCount = bridge.integerProperty({178}, {});
  expect(rank.supported && rank.value && lowerRank.supported && lowerRank.value &&
             negatedRank.supported &&
             !negatedRank.value && poor.supported && poor.value == 3 &&
             miss.supported && miss.value == 4 && poorPlusMiss.supported &&
             poorPlusMiss.value == 7 && badPoorMiss.supported &&
             badPoorMiss.value == 9 && scoreRate.supported &&
             scoreRate.value == 100 && scoreRateAfterDot.supported &&
             scoreRateAfterDot.value == 0 && bestRate.supported &&
             std::abs(bestRate.value - 0.6) < 0.000001 &&
             targetRate.supported && std::abs(targetRate.value - 0.9) < 0.000001 &&
             pGreatRate.supported && std::abs(pGreatRate.value - 1.0) < 0.000001 &&
             comboRate.supported && std::abs(comboRate.value - 0.7) < 0.000001 &&
             exscoreRate.supported && exscoreRate.value == 0.0 &&
             timingAverageFloat.supported && timingAverageFloat.value == 0.15 &&
             imageFavorite.supported && imageFavorite.value == 2,
         "result bridge follows Beatoraja's negated options, score rate, and "
         "Poor-versus-Miss result families");
  expect(point.supported && point.value == 1'000'000 && nowRank.supported &&
             nowRank.value && notNowRank.supported && !notNowRank.value &&
             resultRank.supported && resultRank.value && updatedScore.supported &&
             updatedScore.value && updatedCombo.supported && updatedCombo.value &&
             drawnMissCount.supported && drawnMissCount.value && targetWin.supported &&
             targetWin.value && goodExists.supported && !goodExists.value &&
             missExists.supported && missExists.value &&
             targetMissCount.supported && targetMissCount.value == 9 &&
             differenceMissCount.supported && differenceMissCount.value == 0,
         "result bridge matches Beatoraja result rank, update, and point properties");
}

void testResultBridgeUsesProjectedKeyModeForScorePoint() {
  RhythmState state(nullptr, false);
  state.judgeCount[PGreat] = 1;
  state.judgeCount[Great] = 1;
  state.combo = 0;
  state.maxCombo = 1;
  bms_parser::ChartMeta courseMeta{.KeyMode = 0, .TotalNotes = 2};
  ResultSkinStateBridge bridge({.state = &state,
                                 .meta = &courseMeta,
                                 .keyModeOverride = 7},
                                1, 0);
  const auto point = bridge.integerProperty({100}, {});
  expect(point.supported && point.value == 150'000,
         "result score point uses the result's projected key mode and maximum combo");
}

void testResultBridgeMatchesResultAliasesAndTimerUnits() {
  bms_parser::ChartMeta meta{.Title = "Title", .SubTitle = "Subtitle",
                             .PlayLevel = 12.0F,
                             .PlayLength = 125'000'000};
  ResultSkinConfigurationData configuration{
      .modeFilterName = "ALL",
      .sortId = "TITLE",
  };
  ResultSkinStateBridge bridge({.meta = &meta,
                                 .configuration = configuration,
                                 .courseTitle = "Course title",
                                 .courseTitles = {"First stage", "Second stage"},
                                 .skinName = "Result skin",
                                 .skinAuthor = "Skin author"},
                                1, 125);
  const auto level = bridge.integerProperty({45}, {});
  const auto fullTitleProperty = bridge.stringProperty({12});
  const auto fullTitle = std::string(fullTitleProperty.value);
  const auto mode = std::string(bridge.stringProperty({60}).value);
  const auto order = std::string(bridge.stringProperty({61}).value);
  const auto namedTitle = std::string(bridge.stringProperty({std::string("title")}).value);
  const auto firstCourseTitle = std::string(bridge.stringProperty({150}).value);
  const auto namedCourseTitle =
      std::string(bridge.stringProperty({std::string("coursetitle1")}).value);
  const auto chartMinutes = bridge.integerProperty({1163}, {});
  const auto chartSeconds = bridge.integerProperty({1164}, {});
  const auto skinName = std::string(bridge.stringProperty({50}).value);
  const auto skinAuthor = std::string(bridge.stringProperty({51}).value);
  expect(level.supported && level.value == 12 && fullTitleProperty.supported &&
             fullTitle == "Course title" && mode == "ALL" && order == "TITLE" &&
             namedTitle == "Course title" && firstCourseTitle == "First stage" &&
             namedCourseTitle == "First stage" &&
             chartMinutes.supported && chartMinutes.value == 2 &&
             chartSeconds.supported && chartSeconds.value == 5 &&
             skinName == "Result skin" && skinAuthor == "Skin author" &&
             bridge.timerProperty({1}) == 0 &&
             bridge.timerProperty({100}) == INT64_MIN,
         "result aliases and timers match Beatoraja result properties");
}

void testResultBridgeUsesCapturedReplayImageIndexes() {
  RhythmState state(nullptr, false);
  state.gaugeType = GaugeType::Hard;
  ResultSkinStateBridge bridge({.state = &state,
                                 .replayRandomOption1P = 2,
                                 .replayRandomOption2P = 9,
                                 .replayDoublePlayOption = 1},
                                1, 0);
  const auto gauge = bridge.integerProperty(
      {40}, SkinIntegerPropertyDomain::ImageIndex);
  const auto random1P = bridge.integerProperty(
      {42}, SkinIntegerPropertyDomain::ImageIndex);
  const auto random2P = bridge.integerProperty(
      {43}, SkinIntegerPropertyDomain::ImageIndex);
  const auto doublePlay = bridge.integerProperty(
      {54}, SkinIntegerPropertyDomain::ImageIndex);
  expect(gauge.supported && gauge.value == gaugeTypeIndex(GaugeType::Hard) &&
             random1P.supported && random1P.value == 2 &&
             random2P.supported && random2P.value == 9 &&
             doublePlay.supported && doublePlay.value == 1,
         "result image indexes use the immutable played gauge and replay options");
}

void testResultBridgeUsesSourceImageIndexFactoryFallbacks() {
  ResultSkinStateBridge bridge({}, 1, 0);
  const auto difficulty = bridge.integerProperty(
      {10}, SkinIntegerPropertyDomain::ImageIndex);
  const auto targetOption = bridge.integerProperty(
      {61}, SkinIntegerPropertyDomain::ImageIndex);
  const auto resultConstant = bridge.integerProperty(
      {.value = std::string{"constant"}},
      SkinIntegerPropertyDomain::ImageIndex);
  const auto unmapped = bridge.integerProperty(
      {13}, SkinIntegerPropertyDomain::ImageIndex);
  const auto outOfRange = bridge.integerProperty(
      {65'536}, SkinIntegerPropertyDomain::ImageIndex);
  const auto skinSelect = bridge.integerProperty(
      {177}, SkinIntegerPropertyDomain::ImageIndex);
  const auto shadowedSkinSelect = bridge.integerProperty(
      {386}, SkinIntegerPropertyDomain::ImageIndex);
  const auto judge = bridge.integerProperty(
      {500}, SkinIntegerPropertyDomain::ImageIndex);
  const auto extendedJudge = bridge.integerProperty(
      {1699}, SkinIntegerPropertyDomain::ImageIndex);
  constexpr auto minimum =
      static_cast<std::int64_t>(std::numeric_limits<int>::min());
  expect(difficulty.supported && difficulty.value == minimum &&
             targetOption.supported && targetOption.value == minimum &&
             resultConstant.supported && resultConstant.value == -1 &&
             skinSelect.supported && skinSelect.value == minimum &&
             shadowedSkinSelect.supported && shadowedSkinSelect.value == minimum &&
             judge.supported && judge.value == 0 &&
             extendedJudge.supported && extendedJudge.value == 0 &&
             unmapped.supported && unmapped.value == 0 && !outOfRange.supported,
         "result image-index properties preserve SkinImage's frame-zero "
         "fallback when the numeric factory leaves its ref unset");
}

void testResultBridgeResolvesImageNamesBeforeValueAliases() {
  ResultSkinStateBridge bridge({}, 1, 0);
  const auto mode = bridge.integerProperty(
      {.value = std::string{"mode"}}, SkinIntegerPropertyDomain::ImageIndex);
  const auto minimum = static_cast<std::int64_t>(std::numeric_limits<int>::min());
  expect(mode.supported && mode.value == minimum,
         "result image property names use IndexType before the conflicting "
         "IntegerProperty ValueType aliases");
}

void testResultBridgeRetainsResultConfigurationProperties() {
  ResultSkinConfigurationData configuration{
      .gameplayHispeed = 2.75F,
      .notesDisplayTimingMilliseconds = -18,
      .visibleTimeDurationMilliseconds = 720,
      .bgaEnabled = false,
      .customJudge = true,
      .showJudgeArea = true,
      .markProcessedNotes = true,
      .notesDisplayTimingAutoAdjust = true,
      .autoSaveReplay = {4, 3, 2, 1},
      .guideSoundEffects = true,
      .extraNoteDepth = 6,
      .mineMode = 2,
      .scrollMode = 1,
      .longNoteModifierMode = 3,
      .sevenToNinePattern = 5,
      .sevenToNineType = 4,
      .laneCoverEnabled = false,
      .liftEnabled = true,
      .hiddenEnabled = true,
      .hispeedAutoAdjust = true,
      .judgeAlgorithmImageIndex = 1,
      .gaugeAutoShiftImageIndex = 3,
      .bottomShiftableGaugeImageIndex = 2,
  };

  ResultSkinStateBridge bridge({.configuration = configuration},
                                1, 0);
  const auto value = [&](int id) { return bridge.integerProperty({id}, {}); };
  const auto image = [&](int id) {
    return bridge.integerProperty({id}, SkinIntegerPropertyDomain::ImageIndex);
  };
  const auto floating = [&](int id) { return bridge.floatProperty({id}, {}); };
  expect(value(10).supported && value(10).value == 275 &&
             value(12).supported && value(12).value == -18 &&
             value(165).supported && value(165).value == 100 &&
             value(310).supported && value(310).value == 2 &&
             value(311).supported && value(311).value == 75 &&
             value(312).supported && value(312).value == 720 &&
             value(313).supported && value(313).value == 432 &&
             floating(310).supported &&
             std::abs(floating(310).value - 2.75) < 0.000001 &&
             image(72).supported && image(72).value == 2 &&
             image(75).supported && image(75).value == 1 &&
             image(78).supported && image(78).value == 3 &&
             image(301).supported && image(301).value == 1 &&
             image(302).supported && image(302).value == 1 &&
             image(303).supported && image(303).value == 1 &&
             image(304).supported && image(304).value == 0 &&
             image(305).supported && image(305).value == 1 &&
             image(307).supported && image(307).value == 0 &&
             image(321).supported && image(321).value == 4 &&
             image(324).supported && image(324).value == 1 &&
             image(330).supported && image(330).value == 0 &&
             image(331).supported && image(331).value == 1 &&
             image(332).supported && image(332).value == 1 &&
             image(340).supported && image(340).value == 1 &&
             image(341).supported && image(341).value == 2 &&
             image(342).supported && image(342).value == 1 &&
             image(343).supported && image(343).value == 1 &&
             image(350).supported && image(350).value == 6 &&
             image(353).supported && image(353).value == 3 &&
             image(360).supported && image(360).value == 5 &&
             image(361).supported && image(361).value == 4,
         "result properties retain Beatoraja's live player configuration in "
         "their separate value and image-index namespaces");
}

void testResultBridgeRetainsResultIntegerFactoryFallbacks() {
  bms_parser::ChartMeta meta{.TotalNotes = 10};
  RhythmState state(nullptr, false);
  state.judgeCount[PGreat] = 5;
  state.judgeCount[Great] = 3;
  state.judgeCount[Good] = 2;
  ResultSkinStateBridge bridge({.state = &state, .meta = &meta}, 1, 0);
  const auto value = [&](int id) { return bridge.integerProperty({id}, {}); };
  const auto named = bridge.integerProperty(
      {.value = std::string{"lanecover1"}}, {});
  const auto minimum = std::numeric_limits<int>::min();
  expect(value(80).supported && value(80).value == 5 &&
             value(85).supported && value(85).value == 50 &&
             value(14).supported && value(14).value == minimum &&
             named.supported && named.value == minimum &&
             value(161).supported && value(161).value == 0 &&
             value(162).supported && value(162).value == 0 &&
             value(163).supported && value(163).value == minimum &&
             value(243).supported && value(243).value == minimum &&
             value(280).supported && value(280).value == minimum &&
             value(320).supported && value(320).value == minimum &&
             value(1312).supported && value(1312).value == 0,
         "result IntegerPropertyFactory values retain score-data ranges and "
         "their AbstractResult fallback sentinels");
}

void testResultBridgeRetainsAuthenticatedScoreDate() {
  ResultSkinStateBridge bridge({.currentScoreDateUnixSeconds = 1'700'000'001},
                               1, 0);
  const auto timestamp = bridge.integerProperty({243}, {});
  expect(timestamp.supported && timestamp.value == 1'700'000'001,
         "result lastplay timestamp uses the authenticated ScoreData date");
}

void testResultSkinConfigurationCarriesPlayerConfigAcrossResultSurfaces() {
  AppSettings settings;
  settings.gameplayHispeed = 2.75F;
  settings.notesDisplayTimingMilliseconds = -18;
  settings.visibleTimeDurationMilliseconds = 720;
  settings.hispeedFixMode = AppSettings::HiSpeedFixMode::Max;
  settings.bgaEnabled = false;
  settings.customJudge = true;
  settings.showJudgeArea = true;
  settings.markProcessedNotes = true;
  settings.notesDisplayTimingAutoAdjust = true;
  settings.autoSaveReplay = {4, 3, 2, 1};
  settings.guideSoundEffects = true;
  settings.extraNoteDepth = 6;
  settings.mineMode = 2;
  settings.scrollMode = 1;
  settings.longNoteModifierMode = 3;
  settings.sevenToNinePattern = 5;
  settings.sevenToNineType = 4;
  settings.laneCoverEnabled = false;
  settings.liftEnabled = true;
  settings.hiddenEnabled = true;
  settings.hispeedAutoAdjust = true;
  settings.notePriorityMode = AppSettings::NotePriorityMode::Duration;
  settings.selectedAssistOption = assist_options::kBpmGuide;
  settings.selectedGaugeAutoShiftMode = "best_clear";
  settings.selectedGaugeAutoShiftLowerBound = "normal";
  settings.skinModeFilterName = "7 KEYS";
  settings.skinSortId = "LAMPSCORE";
  settings.skinDifficultyFilterName = "ANOTHER";
  settings.skinChartReplicationMode = "NONE";
  settings.skinTargetId = "RATE_A";
  settings.skinTargetList = {"MAX", "RATE_A"};
  settings.irProviders.clear();
  settings.irProviders.emplace("IR Provider", ir::IrProviderSettings{});

  const ResultSkinConfigurationData configuration =
      makeResultSkinConfiguration(settings);
  expect(configuration.gameplayHispeed == 2.75F &&
             configuration.notesDisplayTimingMilliseconds == -18 &&
             configuration.visibleTimeDurationMilliseconds == 720 &&
             configuration.hispeedFixMode == 2 && !configuration.bgaEnabled &&
             configuration.bpmGuideEnabled && configuration.customJudge &&
             configuration.showJudgeArea && configuration.markProcessedNotes &&
             configuration.notesDisplayTimingAutoAdjust &&
             configuration.autoSaveReplay == std::array<int, 4>{4, 3, 2, 1} &&
             configuration.guideSoundEffects &&
             configuration.extraNoteDepth == 6 && configuration.mineMode == 2 &&
             configuration.scrollMode == 1 &&
             configuration.longNoteModifierMode == 3 &&
             configuration.sevenToNinePattern == 5 &&
             configuration.sevenToNineType == 4 &&
             !configuration.laneCoverEnabled && configuration.liftEnabled &&
             configuration.hiddenEnabled && configuration.hispeedAutoAdjust &&
             configuration.judgeAlgorithmImageIndex == 1 &&
             configuration.gaugeAutoShiftImageIndex == 3 &&
             configuration.bottomShiftableGaugeImageIndex == 2 &&
             configuration.modeFilterName == "7 KEYS" &&
             configuration.sortId == "LAMPSCORE" &&
             configuration.difficultyFilterName == "ANOTHER" &&
             configuration.chartReplicationMode == "NONE" &&
             configuration.irName == "IR Provider" &&
             configuration.skinTargetId == "RATE_A" &&
             configuration.skinTargetList == std::vector<std::string>{"MAX", "RATE_A"},
         "every result rendering entry point can project PlayerConfig through "
         "one shared source-compatible configuration snapshot");
}

void testResultSkinConfigurationClampsExtendedGaugeLowerBounds() {
  const auto configuredLowerBound = [](const char *value) {
    AppSettings settings;
    settings.selectedGaugeAutoShiftLowerBound = value;
    return makeResultSkinConfiguration(settings).bottomShiftableGaugeImageIndex;
  };

  expect(configuredLowerBound("hard") == 2 &&
             configuredLowerBound("exhard") == 2 &&
             configuredLowerBound("hazard") == 2,
         "result bottom-shiftable gauge follows PlayerConfig's Normal upper "
         "bound when Aso's extended lower-bound options are selected");
}

void testResultBridgeSharesSourceStringPropertyResolution() {
  ResultSkinConfigurationData configuration{
      .modeFilterName = "7 KEYS",
      .sortId = "LAMPSCORE",
      .difficultyFilterName = "ANOTHER",
      .chartReplicationMode = "NONE",
      .irName = "TACHI",
      .irAccountName = "Skin Player",
      .skinTargetId = "RATE_A",
      .skinTargetList = {"MAX", "RATE_A", "RANK_NEXT"},
  };
  ResultSkinStateBridge bridge({
      .configuration = configuration,
      .irRankingEntries = {{.rank = 1, .playerName = "Top player"}},
  }, 1, 0);
  const auto read = [&](std::string name) {
    const auto property = bridge.stringProperty({.value = std::move(name)});
    return std::pair{property.supported, std::string(property.value)};
  };
  const auto [modeSupported, modeValue] = read("mode");
  const auto [sortSupported, sortValue] = read("sort");
  const auto [difficultySupported, difficultyValue] = read("difficulty");
  const auto [replicationSupported, replicationValue] =
      read("chartreplication");
  const auto [irNameSupported, irNameValue] = read("irname");
  const auto [irAccountSupported, irAccountValue] = read("irUserName");
  const auto [searchSupported, searchValue] = read("searchword");
  const auto [keyedSupported, keyedValue] = read("key+01");
  const auto [rankingSupported, rankingValue] = read("rankingname+01");
  const auto [previousTargetSupported, previousTargetValue] =
      read("targetnamep1");
  const auto [nextTargetSupported, nextTargetValue] = read("targetnamen1");
  const auto catalog = gameplaySkinBuiltinCatalog();
  const SkinBindingType string{.kind = SkinBindingKind::StringProperty};
  expect(modeSupported && modeValue == "7 KEYS" && sortSupported &&
             sortValue == "LAMPSCORE" && difficultySupported &&
             difficultyValue == "ANOTHER" && replicationSupported &&
             replicationValue == "NONE" && irNameSupported &&
             irNameValue == "TACHI" && irAccountSupported &&
             irAccountValue == "Skin Player" && searchSupported &&
             searchValue.empty() && keyedSupported && keyedValue.empty() &&
             rankingSupported && rankingValue == "Top player" &&
             previousTargetSupported && previousTargetValue == "MAX" &&
             nextTargetSupported && nextTargetValue == "NEXT RANK" &&
             catalog.contains(string,
                              SkinBuiltinPropertySelector{"rankingname+01"}),
         "gameplay and result bridges share every StringPropertyFactory name "
         "and Java-numbered pattern without repurposing its namespace");
}

void testResultBridgeProjectsLongNoteModeImageIndex() {
  for (const auto [mode, expected] :
       std::array<std::pair<int, std::int64_t>, 3>{{{1, 0}, {2, 1}, {3, 2}}}) {
    bms_parser::ChartMeta meta{.LnMode = mode};
    ResultSkinStateBridge bridge({.meta = &meta}, 1, 0);
    const auto value = bridge.integerProperty(
        {308}, SkinIntegerPropertyDomain::ImageIndex);
    expect(value.supported && value.value == expected,
           "result LN mode image index compacts LN/CN/HCN like Beatoraja");
  }
}

void testResultBridgeMatchesBeatorajaTableFullString() {
  ResultSkinStateBridge bridge({.tableName = "Insane Table",
                                 .tableLevel = "★12"},
                                1, 0);
  const auto tableFull = bridge.stringProperty({1003});
  expect(tableFull.supported && tableFull.value == "★12Insane Table",
         "result tablefull preserves Beatoraja's level-first concatenation");
}

void testResultBridgeDoesNotInventRemoteGaugeImageIndex() {
  ResultPresentationModel remote{.score = 100, .maxScore = 200};
  ResultSkinStateBridge unknown({.presentation = &remote}, 1, 0);
  const auto unavailable = unknown.integerProperty(
      {40}, SkinIntegerPropertyDomain::ImageIndex);
  ResultSkinStateBridge known({.presentation = &remote,
                               .gaugeTypeOverride = GaugeType::Hard},
                              1, 0);
  const auto explicitGauge = known.integerProperty(
      {40}, SkinIntegerPropertyDomain::ImageIndex);
  expect(!unavailable.supported && explicitGauge.supported &&
             explicitGauge.value == gaugeTypeIndex(GaugeType::Hard),
         "remote results do not substitute NORMAL when their gauge is absent");
}

void testResultBridgeKeepsResultPropertyContractsForAbsentAndStaticData() {
  bms_parser::ChartMeta meta{.Rank = 2,
                             .PlayLevel = 12.5,
                             .PlayLevelText = "12.5",
                             .TotalNotes = 100};
  ResultSkinStateBridge firstPlay({.meta = &meta}, 1, 0);
  const auto previousRank = firstPlay.booleanProperty({320});
  const auto noPreviousRank = firstPlay.booleanProperty({-320});
  const auto veryHard = firstPlay.booleanProperty({180});
  const auto hardRank = firstPlay.booleanProperty({181});
  const auto normalRank = firstPlay.booleanProperty({182});
  const auto authoredLevel = firstPlay.integerProperty({96}, {});

  RhythmState normalState(nullptr, false);
  normalState.gaugeType = GaugeType::Normal;
  normalState.judgeCount[PGreat] = 50;
  normalState.maxCombo = 50;
  ResultSkinStateBridge normal({.state = &normalState, .meta = &meta}, 1, 0);
  RhythmState hardState(nullptr, false);
  hardState.gaugeType = GaugeType::Hard;
  ResultSkinStateBridge hardResult({.state = &hardState, .meta = &meta}, 1, 0);
  const auto normalGauge = normal.booleanProperty({42});
  const auto normalHardGauge = normal.booleanProperty({43});
  const auto firstPlayHighScore = normal.integerProperty({150}, {});
  const auto firstPlayTargetScore = normal.integerProperty({121}, {});
  const auto firstPlayBestRate = normal.integerProperty({183}, {});
  const auto firstPlayBestRank = normal.booleanProperty({327});
  const auto firstPlayUpdatedScore = normal.booleanProperty({330});
  const auto firstPlayUpdatedCombo = normal.booleanProperty({331});
  const auto firstPlayUpdatedMiss = normal.booleanProperty({332});
  const auto firstPlayUpdatedRank = normal.booleanProperty({335});
  const auto firstPlayUpdatedTarget = normal.booleanProperty({336});
  const auto firstPlayTargetWin = normal.booleanProperty({352});
  const auto hardGauge = hardResult.booleanProperty({42});
  const auto hardHardGauge = hardResult.booleanProperty({43});

  ResultPresentationModel remote{.finalGauge = 72.0F,
                                 .gaugeSeries = {{.points = {30.0F, 72.0F},
                                                  .maximum = 100.0F}}};
  ResultSkinStateBridge unknownRemoteGauge({.presentation = &remote}, 1, 0);
  const auto remoteGauge = unknownRemoteGauge.gaugeState();
  const auto remoteGraph = unknownRemoteGauge.gameplayGraphState();

  expect(previousRank.supported && !previousRank.value &&
             noPreviousRank.supported && noPreviousRank.value &&
             veryHard.supported && !veryHard.value && hardRank.supported &&
             !hardRank.value && normalRank.supported && normalRank.value &&
             authoredLevel.supported && authoredLevel.value == 0 &&
             normalGauge.supported && normalGauge.value &&
             normalHardGauge.supported && !normalHardGauge.value &&
             firstPlayHighScore.supported && firstPlayHighScore.value == 0 &&
             firstPlayTargetScore.supported && firstPlayTargetScore.value == 0 &&
             firstPlayBestRate.supported && firstPlayBestRate.value == 0 &&
             firstPlayBestRank.supported && firstPlayBestRank.value &&
             firstPlayUpdatedScore.supported && firstPlayUpdatedScore.value &&
             firstPlayUpdatedCombo.supported && firstPlayUpdatedCombo.value &&
             firstPlayUpdatedMiss.supported && firstPlayUpdatedMiss.value &&
             firstPlayUpdatedRank.supported && firstPlayUpdatedRank.value &&
             firstPlayUpdatedTarget.supported && firstPlayUpdatedTarget.value &&
             firstPlayTargetWin.supported && firstPlayTargetWin.value &&
             hardGauge.supported && !hardGauge.value &&
             hardHardGauge.supported && hardHardGauge.value &&
             !remoteGauge.supported && !remoteGraph.gaugeSupported,
         "result bridge preserves Beatoraja result conditions and does not "
         "invent metadata for unknown remote gauges");
}

void testResultBridgeConvertsClearRanksToBeatorajaImageIndexes() {
  ResultSkinStateBridge bridge({
      .currentClearRankOverride = kClearTypeFullComboRank,
      .previousLampBest = ResultPreviousBestData{
          .clearType = kClearTypeEasyClearRank},
  }, 1, 0);
  const auto current = bridge.integerProperty(
      {370}, SkinIntegerPropertyDomain::ImageIndex);
  const auto previous = bridge.integerProperty(
      {371}, SkinIntegerPropertyDomain::ImageIndex);
  const auto currentNumber = bridge.integerProperty({370}, {});
  const auto previousNumber = bridge.integerProperty({371}, {});
  ResultSkinStateBridge failed({
      .currentClearRankOverride = kClearTypeFailedRank,
  }, 1, 0);
  const auto failedCurrent = failed.integerProperty(
      {370}, SkinIntegerPropertyDomain::ImageIndex);
  const auto failedCurrentNumber = failed.integerProperty({370}, {});
  ResultSkinStateBridge noPreviousScore({}, 1, 0);
  const auto noPrevious = noPreviousScore.integerProperty(
      {371}, SkinIntegerPropertyDomain::ImageIndex);
  const auto noPreviousNumber = noPreviousScore.integerProperty({371}, {});
  expect(current.supported && current.value == 8 &&
             previous.supported && previous.value == 4 &&
             currentNumber.supported && currentNumber.value == 8 &&
             previousNumber.supported && previousNumber.value == 4 &&
             failedCurrent.supported && failedCurrent.value == 1 &&
             failedCurrentNumber.supported && failedCurrentNumber.value == 1 &&
             noPrevious.supported && noPrevious.value == 0 &&
             noPreviousNumber.supported && noPreviousNumber.value == 0,
         "result clear properties use Beatoraja ClearType IDs in every domain");
}

void testResultBridgeRetainsPreparedChartResultProperties() {
  bms_parser::ChartMeta meta{.Rank = 2,
                             .Bpm = 128.0,
                             .MinBpm = 96.0,
                             .MaxBpm = 196.0,
                             .Total = 210.5,
                             .HasTotal = true};
  auto graph = std::make_shared<SkinGameplayChartGraphState>();
  graph->mainBpm = 172.0;
  graph->normalKeyNotes = 12;
  graph->longKeyNotes = 13;
  graph->normalScratchNotes = 14;
  graph->longScratchNotes = 15;
  graph->peakDensity = 23.45;
  graph->endDensity = 16.78;
  graph->averageDensity = 8.91;
  graph->hasBpmStop = true;
  graph->hasBga = true;
  graph->hasRandomSequence = true;
  graph->hasAnyLongNote = true;
  graph->hasUndefinedLongNote = false;
  graph->hasLongNote = false;
  graph->hasChargeNote = true;
  graph->hasHellChargeNote = false;
  ResultSkinStateBridge bridge({.meta = &meta,
                                 .chartHasDocument = true,
                                 .gameplayGraph = {.chart = graph}},
                                1, 0);
  const auto mainBpm = bridge.integerProperty({92}, {});
  const auto totalGauge = bridge.integerProperty({368}, {});
  const auto totalGaugeFloat = bridge.floatProperty({368}, {});
  const auto judgeRank = bridge.integerProperty({400}, {});
  const auto judgeDuration = bridge.integerProperty({525}, {});
  const auto normalKeys = bridge.integerProperty({350}, {});
  const auto longKeys = bridge.integerProperty({351}, {});
  const auto normalScratch = bridge.integerProperty({352}, {});
  const auto longScratch = bridge.integerProperty({353}, {});
  const auto peakDensity = bridge.integerProperty({360}, {});
  const auto peakDensityFraction = bridge.integerProperty({361}, {});
  const auto endDensity = bridge.integerProperty({362}, {});
  const auto endDensityFraction = bridge.integerProperty({363}, {});
  const auto averageDensity = bridge.integerProperty({364}, {});
  const auto averageDensityFraction = bridge.integerProperty({365}, {});
  const auto peakDensityFloat = bridge.floatProperty({360}, {});
  const auto endDensityFloat = bridge.floatProperty({362}, {});
  const auto averageDensityFloat = bridge.floatProperty({367}, {});
  const auto hasBpmStop = bridge.booleanProperty({1177});
  const auto noBga = bridge.booleanProperty({170});
  const auto hasBga = bridge.booleanProperty({171});
  const auto noRandomSequence = bridge.booleanProperty({178});
  const auto hasRandomSequence = bridge.booleanProperty({179});
  const auto noLongNote = bridge.booleanProperty({172});
  const auto hasLongNote = bridge.booleanProperty({173});
  const auto longNoteMode = bridge.integerProperty(
      {308}, SkinIntegerPropertyDomain::ImageIndex);
  const auto noDocument = bridge.booleanProperty({174});
  const auto hasDocument = bridge.booleanProperty({175});

  expect(mainBpm.supported && mainBpm.value == 172 &&
             totalGauge.supported && totalGauge.value == 210 &&
             totalGaugeFloat.supported && totalGaugeFloat.value == 210.5 &&
             judgeRank.supported && judgeRank.value == 2 &&
             judgeDuration.supported && judgeDuration.value == 0 &&
             normalKeys.supported && normalKeys.value == 12 &&
             longKeys.supported && longKeys.value == 13 &&
             normalScratch.supported && normalScratch.value == 14 &&
             longScratch.supported && longScratch.value == 15 &&
             peakDensity.supported && peakDensity.value == 23 &&
             peakDensityFraction.supported && peakDensityFraction.value == 45 &&
             endDensity.supported && endDensity.value == 16 &&
             endDensityFraction.supported && endDensityFraction.value == 78 &&
             averageDensity.supported && averageDensity.value == 8 &&
             averageDensityFraction.supported && averageDensityFraction.value == 91 &&
             peakDensityFloat.supported && peakDensityFloat.value == 23.45 &&
             endDensityFloat.supported && endDensityFloat.value == 16.78 &&
             averageDensityFloat.supported && averageDensityFloat.value == 8.91 &&
             hasBpmStop.supported && hasBpmStop.value &&
             noBga.supported && !noBga.value && hasBga.supported && hasBga.value &&
             noRandomSequence.supported && !noRandomSequence.value &&
             hasRandomSequence.supported && hasRandomSequence.value &&
             noLongNote.supported && !noLongNote.value && hasLongNote.supported &&
             hasLongNote.value && longNoteMode.supported && longNoteMode.value == 1 &&
             noDocument.supported && !noDocument.value && hasDocument.supported &&
             hasDocument.value,
         "result bridge retains Beatoraja's prepared chart properties");
}

void testResultBridgeProjectsIrRankingRows() {
  ResultSkinStateBridge bridge({
      .irRankingEntries = {{.rank = 1,
                            .playerName = "Top player",
                            .score = 1998,
                            .clearType = kClearTypeFullComboRank},
                           {.rank = 2,
                            .playerName = "Current player",
                            .score = 1888,
                            .clearType = kClearTypeHardClearRank,
                            .currentUser = true}},
  }, 1, 0);
  const auto currentRank = bridge.integerProperty({179}, {});
  const auto firstScore = bridge.integerProperty({380}, {});
  const auto secondRank = bridge.integerProperty({391}, {});
  const auto secondLamp = bridge.integerProperty(
      {391}, SkinIntegerPropertyDomain::ImageIndex);
  const auto secondName = bridge.stringProperty({121});
  expect(currentRank.supported && currentRank.value == 2 &&
             firstScore.supported && firstScore.value == 1998 &&
             secondRank.supported && secondRank.value == 2 &&
             secondLamp.supported && secondLamp.value == 6 &&
             secondName.supported && secondName.value == "Current player",
         "result IR ranking properties use the active ranking snapshot");
}

void testResultBridgeMapsNamedResultAndRankingProperties() {
  ResultPresentationModel remote{.lampRank = kClearTypeHardClearRank};
  ResultSkinStateBridge bridge({
      .presentation = &remote,
      .irRankingEntries = {{.rank = 1,
                            .playerName = "Top player",
                            .score = 1998,
                            .clearType = kClearTypeFullComboRank},
                           {.rank = 2,
                            .playerName = "Current player",
                            .score = 1888,
                            .clearType = kClearTypeHardClearRank,
                            .currentUser = true}},
  }, 1, 0);
  const auto clear = bridge.integerProperty(
      {.value = std::string{"cleartype"}},
      SkinIntegerPropertyDomain::IntegerValue);
  const auto firstScore = bridge.integerProperty(
      {.value = std::string{"ranking_exscore+01"}},
      SkinIntegerPropertyDomain::IntegerValue);
  const auto secondRank = bridge.integerProperty(
      {.value = std::string{"ranking_index+02"}},
      SkinIntegerPropertyDomain::IntegerValue);
  const auto secondPlayer = bridge.integerProperty(
      {.value = std::string{"playertype_ranking+02"}},
      SkinIntegerPropertyDomain::ImageIndex);
  const auto secondLamp = bridge.integerProperty(
      {.value = std::string{"cleartype_ranking+02"}},
      SkinIntegerPropertyDomain::ImageIndex);
  const auto catalog = gameplaySkinBuiltinCatalog();
  const SkinBindingType value{
      .kind = SkinBindingKind::IntegerProperty,
      .integerDomain = SkinIntegerPropertyDomain::IntegerValue};
  const SkinBindingType image{
      .kind = SkinBindingKind::IntegerProperty,
      .integerDomain = SkinIntegerPropertyDomain::ImageIndex};
  expect(clear.supported && clear.value == 6 && firstScore.supported &&
             firstScore.value == 1998 && secondRank.supported &&
             secondRank.value == 2 && secondPlayer.supported &&
             secondPlayer.value == 1 && secondLamp.supported &&
             secondLamp.value == 6 &&
             catalog.contains(value,
                              SkinBuiltinPropertySelector{"ranking_exscore+01"}) &&
             catalog.contains(image,
                              SkinBuiltinPropertySelector{"cleartype_ranking+02"}) &&
             catalog.contains(value,
                              SkinBuiltinPropertySelector{"ir_player_failed"}) &&
             catalog.contains(
                 value, SkinBuiltinPropertySelector{"ir_player_failed_rate"}) &&
             catalog.contains(value, SkinBuiltinPropertySelector{
                                         "ir_player_failed_rate_afterdot"}),
         "result bridge retains source names and domains for result and "
         "ranking properties");
}

void testResultBridgeMapsNamedIntegerScoreProperties() {
  ResultPresentationModel result{
      .score = 150,
      .maxScore = 200,
      .finalGauge = 82.5F,
      .maxCombo = 70,
      .comboBreak = 3,
      .badPoints = 4};
  ResultSkinStateBridge bridge({
      .presentation = &result,
      .previousBest = ResultPreviousBestData{.score = 125,
                                              .maxCombo = 60,
                                              .badPoints = 5},
      .pacemaker = ResultPacemakerData{.targetScore = 160},
  }, 1, 0);
  const auto score = bridge.integerProperty({.value = std::string{"score"}}, {});
  const auto maximum = bridge.integerProperty(
      {.value = std::string{"maxscore"}}, {});
  const auto rate = bridge.integerProperty(
      {.value = std::string{"score_rate"}}, {});
  const auto rateFraction = bridge.integerProperty(
      {.value = std::string{"score_rate_afterdot"}}, {});
  const auto gauge = bridge.integerProperty(
      {.value = std::string{"groovegauge"}}, {});
  const auto gaugeFraction = bridge.integerProperty(
      {.value = std::string{"groovegauge_afterdot"}}, {});
  const auto totalRate = bridge.integerProperty(
      {.value = std::string{"total_rate"}}, {});
  const auto target = bridge.integerProperty(
      {.value = std::string{"target_score"}}, {});
  const auto previous = bridge.integerProperty(
      {.value = std::string{"highscore"}}, {});
  const auto unsupportedCombo = bridge.integerProperty(
      {.value = std::string{"combo"}}, {});
  const auto catalog = gameplaySkinBuiltinCatalog();
  const SkinBindingType integerValue{
      .kind = SkinBindingKind::IntegerProperty,
      .integerDomain = SkinIntegerPropertyDomain::IntegerValue};
  const SkinBindingType imageIndex{
      .kind = SkinBindingKind::IntegerProperty,
      .integerDomain = SkinIntegerPropertyDomain::ImageIndex};
  expect(score.supported && score.value == 150 && maximum.supported &&
             maximum.value == 200 && rate.supported && rate.value == 75 &&
             rateFraction.supported && rateFraction.value == 0 &&
             gauge.supported && gauge.value == 82 && gaugeFraction.supported &&
             gaugeFraction.value == 5 && totalRate.supported &&
             totalRate.value == 75 && target.supported && target.value == 160 &&
             previous.supported && previous.value == 125,
         "result bridge resolves IntegerPropertyFactory score names in their "
         "integer namespace");
  expect(catalog.contains(integerValue,
                          SkinBuiltinPropertySelector{std::string{"score"}}) &&
             catalog.contains(
                 integerValue,
                 SkinBuiltinPropertySelector{std::string{"score_rate"}}) &&
             catalog.contains(
                 integerValue,
                 SkinBuiltinPropertySelector{std::string{"groovegauge"}}),
         "result skins admit IntegerPropertyFactory score names as built-ins");
  expect(catalog.contains(imageIndex,
                          SkinBuiltinPropertySelector{std::string{"cleartype"}}),
         "result skins admit IndexType clear-lamp names in the image namespace");
  expect(!unsupportedCombo.supported &&
             !catalog.contains(integerValue,
                               SkinBuiltinPropertySelector{std::string{"combo"}}),
         "result skins do not invent a named IntegerProperty absent from "
         "Beatoraja's factory");
}

void testResultBridgeProjectsReplayLaneAssignments() {
  ResultSkinStateBridge bridge({
      .replayRandomOption1P = 8,
      .replayRandomOption2P = 2,
      .replayKeyMode = 14,
      .replayLaneShufflePattern1P = std::vector<int>{3, 0, 1, 2, 4, 5, 6, 7},
      .replayLaneShufflePattern2P = std::vector<int>{9, 8, 10, 11, 12, 13, 14, 15},
  }, 1, 0);
  const auto firstKey = bridge.integerProperty(
      {450}, SkinIntegerPropertyDomain::ImageIndex);
  const auto firstScratch = bridge.integerProperty(
      {459}, SkinIntegerPropertyDomain::ImageIndex);
  const auto secondKey = bridge.integerProperty(
      {460}, SkinIntegerPropertyDomain::ImageIndex);
  expect(firstKey.supported && firstKey.value == 4 &&
             firstScratch.supported && firstScratch.value == 8 &&
             secondKey.supported && secondKey.value == 3,
         "result lane-assignment indexes retain Beatoraja replay patterns");
}

void testResultBridgeExposesPlayerHistoryProperties() {
  ResultSkinStateBridge bridge({
      .playerHistory = ResultPlayerHistoryData{
          .playCount = 130,
          .clearCount = 91,
          .judgementCounts = {1000, 900, 80, 20, 10},
          .playDurationSeconds = 3'661}},
      1, 0);
  const auto playCount = bridge.integerProperty({30}, {});
  const auto failureCount = bridge.integerProperty({32}, {});
  const auto great = bridge.integerProperty({34}, {});
  const auto notes = bridge.integerProperty({333}, {});
  const auto hours = bridge.integerProperty({17}, {});
  const auto minutes = bridge.integerProperty({18}, {});
  const auto seconds = bridge.integerProperty({19}, {});
  expect(playCount.supported && playCount.value == 130 &&
             failureCount.supported && failureCount.value == 39 &&
             great.supported && great.value == 900 && notes.supported &&
             notes.value == 2'000 && hours.supported && hours.value == 1 &&
             minutes.supported && minutes.value == 1 && seconds.supported &&
             seconds.value == 1,
         "result bridge exposes Beatoraja PlayerData history properties");
}

void testResultBridgeExposesResultTimingDistributionStatistics() {
  ResultSkinStateBridge bridge({.timingAverageMillis = -12.34,
                                 .timingStandardDeviationMillis = 8.76,
                                 .averageJudgeMicros = 1'234'560,
                                 .timingDistribution = {0, 3, 7},
                                 .timingDistributionCenter = 1},
                                1, 0);
  const auto duration = bridge.integerProperty({372}, {});
  const auto durationFraction = bridge.integerProperty({373}, {});
  const auto average = bridge.integerProperty({374}, {});
  const auto averageFraction = bridge.integerProperty({375}, {});
  const auto deviation = bridge.integerProperty({376}, {});
  const auto deviationFraction = bridge.integerProperty({377}, {});
  const auto floatDeviation = bridge.floatProperty({376}, {});
  const auto graph = bridge.gameplayGraphState();
  expect(duration.supported && duration.value == 1234 &&
             durationFraction.supported && durationFraction.value == 56 &&
             average.supported && average.value == -12 &&
             averageFraction.supported && averageFraction.value == -34 &&
             deviation.supported && deviation.value == 8 &&
             deviationFraction.supported && deviationFraction.value == 76 &&
             floatDeviation.supported &&
             std::abs(floatDeviation.value - 8.76) < 0.000001 &&
             graph.timingDistribution.size() == 3 &&
             graph.timingDistributionCenter == 1 &&
             graph.timingDistributionAverageMillis &&
             std::abs(*graph.timingDistributionAverageMillis + 12.34) < 0.000001,
         "result timing statistic properties use the completed timing distribution");
}

void testResultBridgeKeepsEmptyMusicResultTimingDefaults() {
  RhythmState state(nullptr, false);
  ResultSkinStateBridge bridge({.state = &state}, 1, 0);
  const auto value = [&](int id) { return bridge.integerProperty({id}, {}); };
  const auto standardDeviation = bridge.floatProperty({376}, {});
  expect(value(372).supported && value(372).value == 0 &&
             value(373).supported && value(373).value == 0 &&
             value(374).supported &&
             value(374).value == std::numeric_limits<int>::max() &&
             value(375).supported && value(375).value == 47 &&
             value(376).supported && value(376).value == -1 &&
             value(377).supported && value(377).value == 0 &&
             standardDeviation.supported && standardDeviation.value == -1.0,
         "empty MusicResult timing data keeps Beatoraja's initialized "
         "TimingDistribution defaults");
}

void testCourseResultBridgeDoesNotInventMusicResultTimingStatistics() {
  ResultSkinStateBridge bridge({.courseResult = true,
                                 .timingAverageMillis = -12.34,
                                 .timingStandardDeviationMillis = 8.76,
                                 .averageJudgeMicros = 1'234'560,
                                 .timingDistribution = {0, 3, 7},
                                 .timingDistributionCenter = 1},
                                1, 0);
  const auto duration = bridge.integerProperty({372}, {});
  const auto average = bridge.integerProperty({374}, {});
  const auto deviation = bridge.integerProperty({376}, {});
  const auto floatDuration = bridge.floatProperty({372}, {});
  const auto floatDeviation = bridge.floatProperty({376}, {});
  const auto graph = bridge.gameplayGraphState();
  expect(duration.supported && duration.value == 0 && average.supported &&
             average.value == 0 && deviation.supported && deviation.value == 0 &&
             floatDuration.supported && floatDuration.value == 0.0 &&
             floatDeviation.supported && floatDeviation.value == 0.0 &&
             graph.timingDistribution.empty(),
         "course results retain AbstractResult's empty timing state instead "
         "of projecting MusicResult replay statistics");
}

void testResultBridgeUsesRemotePresentationValues() {
  ResultPresentationModel remote{
      .score = 1400,
      .maxScore = 2000,
      .lampRank = 3,
      .finalGauge = 79.96F,
      .maxCombo = 720,
      .judgements = {{.label = "P-GREAT", .total = 800},
                     {.label = "GREAT", .total = 100, .early = 0, .late = 0},
                     {.label = "GOOD", .total = 50},
                     {.label = "BAD", .total = 20},
                     {.label = "POOR", .total = 10}},
      .fast = 12,
      .slow = 8,
      .gaugeSeries = {{.points = {20.0F, 50.0F, 79.96F},
                       .maximum = 100.0F}},
  };
  ResultSkinStateBridge bridge({.presentation = &remote,
                                 .playLevelOverride = 12.7F,
                                 .configuration = ResultSkinConfigurationData{
                                     .difficultyFilterName = "ALL"},
                                 .chartMd5 = "remote-md5",
                                 .chartSha256 = "remote-sha256",
                                 .keyModeOverride = 7,
                                 .gaugeTypeOverride = GaugeType::Normal},
                                1, 0);
  const auto poor = bridge.integerProperty({114}, {});
  const auto finalGauge = bridge.integerProperty({107}, {});
  const auto gaugeDecimal = bridge.integerProperty({407}, {});
  const auto fastGreat = bridge.integerProperty({412}, {});
  const auto totalFast = bridge.integerProperty({423}, {});
  const auto graph = bridge.gameplayGraphState();
  const auto gauge = bridge.gaugeState();
  const auto level = bridge.integerProperty({45}, {});
  const auto difficultyProperty = bridge.stringProperty({62});
  const std::string difficulty(difficultyProperty.value);
  const auto chartMd5Property = bridge.stringProperty({1030});
  const std::string chartMd5(chartMd5Property.value);
  expect(level.supported && level.value == 13,
         "remote result level uses remote level number");
  expect(difficultyProperty.supported && difficulty == "ALL",
         "result StringProperty difficulty keeps PlayerConfig's filter value");
  expect(chartMd5Property.supported && chartMd5 == "remote-md5",
         "remote result chart hash uses remote metadata");
  expect(bridge.booleanProperty({160}).supported &&
             bridge.booleanProperty({160}).value,
         "remote result key-mode options use the remote game type");
  expect(bridge.booleanProperty({90}).supported &&
             bridge.booleanProperty({90}).value && poor.supported &&
             poor.value == 10 && finalGauge.supported &&
             finalGauge.value == 79 && gaugeDecimal.supported &&
             gaugeDecimal.value == 9 && fastGreat.supported &&
             fastGreat.value == 0 && totalFast.supported &&
             totalFast.value == 12 && graph.gaugeSupported &&
             graph.gaugeHistory.size() == 3 && graph.gaugeRevision != 0 &&
             gauge.supported && std::abs(gauge.value - 79.96) < 0.0001 &&
             gauge.minimum == 2.0 && gauge.maximum == 100.0 &&
             gauge.border == 80.0 &&
             std::abs(bridge.floatProperty({1107}, {}).value - 79.96) < 0.0001 &&
             bridge.judgeState(0).supported && bridge.judgeState(0).combo == 720 &&
             level.supported && level.value == 13 && difficultyProperty.supported &&
             difficulty == "ALL" && chartMd5Property.supported &&
             chartMd5 == "remote-md5",
         "remote result properties project presentation scores, timing, and gauges");
}

void testResultBridgePreservesCompletedGameplayGraph() {
  auto chart = std::make_shared<SkinGameplayChartGraphState>();
  chart->normalDistribution = {{{1, 2, 3, 4, 5, 6, 7}}};
  chart->bpmSeries = {{.chartTimeMicros = 0, .bpm = 150.0,
                       .bpmTimesScroll = 150.0, .graphSpeed = 150.0}};
  chart->mainBpm = 150.0;
  chart->minimumBpm = 120.0;
  chart->maximumBpm = 180.0;
  auto dynamic = std::make_shared<SkinGameplayDynamicGraphState>();
  dynamic->judgementDistribution = {{{1, 2, 3, 4, 5, 6}}};
  dynamic->earlyLateDistribution = {{{6, 5, 4, 3, 2, 1, 0, 0, 0, 0}}};
  dynamic->recentJudgeTimingsMillis[0] = 12;
  dynamic->recentJudgeTimingIndex = 1;
  dynamic->judgeWindows[0] = {.judgement = PGreat,
                              .minimumTimingMillis = -20,
                              .maximumTimingMillis = 20};
  dynamic->gaugeHistories[gaugeTypeIndex(GaugeType::Normal)] = {20.0F, 80.0F};
  dynamic->gaugeType = GaugeType::Normal;
  dynamic->gaugeMinimum = 2.0F;
  dynamic->gaugeMaximum = 100.0F;
  dynamic->gaugeBorder = 80.0F;
  dynamic->gaugeSupported = true;
  dynamic->judgementRevision = 11;
  dynamic->gaugeRevision = 12;

  ResultSkinStateBridge bridge({.gameplayGraph = {.chart = std::move(chart),
                                                   .dynamic = std::move(dynamic)}},
                                1, 0);
  const auto graph = bridge.gameplayGraphState();
  expect(graph.normalDistribution.size() == 1 &&
             graph.judgementDistribution.size() == 1 &&
             graph.earlyLateDistribution.size() == 1 &&
             graph.bpmSeries.size() == 1 && graph.judgeWindows.size() == 5 &&
             graph.recentJudgeTimingsMillis.size() == kSkinRecentJudgeTimingCapacity &&
             graph.recentJudgeTimingIndex == 1 && graph.gaugeHistory.size() == 2 &&
             graph.judgementRevision == 11 && graph.gaugeRevision == 12,
         "result bridge preserves the completed gameplay graph snapshot");
}

void testRequestedExternalResultSkinCreatesSession() {
  const char *configuredRoot =
      std::getenv("ASOBMASHOW_EXTERNAL_RESULT_SKIN_ROOT");
  if (configuredRoot == nullptr || *configuredRoot == '\0') {
    return;
  }
  const fs::path source(configuredRoot);
  expect(fs::is_directory(source),
         "requested external result skin root is a readable directory");
  if (!fs::is_directory(source)) {
    return;
  }
  const char *configuredEntry =
      std::getenv("ASOBMASHOW_EXTERNAL_RESULT_SKIN_ENTRY");
  const std::string entryPath =
      configuredEntry != nullptr && *configuredEntry != '\0'
          ? configuredEntry
          : "result.luaskin";
  ExternalResultSkinFixture fixture(source, entryPath);
  auto configured = fixture.configure();
  if (!configured.document) {
    for (const auto &diagnostic : configured.diagnostics) {
      std::cerr << "external result session diagnostic: " << diagnostic.code
                << ": " << diagnostic.message << '\n';
    }
  }
  expect(configured.document.has_value(),
         "requested external result skin configures its result document");
}

} // namespace

int main() {
  testLuaJsonAndLr2SessionsEmitEquivalentSharedObjects();
  testLr2ProductionRecoveryAndFatalBoundaries();
  testLr2ProductionBuiltInGraphsOwnChartAndPlainImages();
  testLr2DeclaredFalseOptionActivatesNegatedInclude();
  testMalformedLr2SetOptionDoesNotDivergeFromIncludeFold();
  testCommentedJsonCreatesProductionSession();
  testSessionOwnsDeduplicatedMoviesAndRollsBackBeforePublication();
  testSessionOwnsLuaAudioAndRollsBackBeforePublication();
  testCallbackBindingWithoutRuntimeFailsValidation();
  testActivationCreatesAnOwningFreshStateSession();
  testConfiguredLoadUsesTheInitializedAuthoritativeState();
  testLuaSessionCapturesLegacyInputAtEachAuthoritativeBoundary();
  testRepeatedPomyuObjectsShareCyclePreparation();
  testMalformedPomyuNumericDirectivesAbortTheCp932Character();
  testExplicitOversizedPomyuDoesNotFallBackToSibling();
  testIncompletePomyuResourcesKeepDefaultCycles();
  testPomyuResourcesUseMs932AndWindowsSeparators();
  testPomyuRootedResourcePathIsRejected();
  testPomyuLeadingBackslashPathRemainsCharacterRelative();
  testPomyuPreparationSelectsSecondPlayerTexturesAndStaticFallbacks();
  testRequestedExternalGameplaySkinCreatesARealSession();
  testActivationRejectsAReconciledDigestMismatch();
  testResourceSessionOwnsUploadsAndExactRuntimeStringAtlas();
  testPostUploadCancellationRollsBackResourcesOnOwnerThread();
  testPreparedSessionRunsFiveHundredFramesWithoutLoadingAgain();
  testInvalidViewportRollsBackUploadedResourcesOnOwnerThread();
  testActivationCancellationAndZeroSerialDoNotPublishSessions();
  testSuccessfulFrameCommitsWriterMutationsInInputOrder();
  testWriterFailureDiscardsEarlierAndFailedCallbackMutations();
  testEvaluatorFailureDiscardsWriterTransaction();
  testSerialMismatchDoesNotConsumeRuntimeFrame();
  testSyntheticReplayGhostUsesMatchingLaneGeometry();
  testSelectedSkinHudUsesThePublishedSkinNoteLaneSpan();
  testPmsPoorDestinationUsesFirstSelectedSkinLane();
  testSyntheticStartLaneIndicatorsUseSelectedSkinLaneGeometry();
  testSyntheticReplayGhostRespectsDisabledOption();
  testSyntheticReplayGhostSkipsEventsOutsideLaneClip();
  testSyntheticReplayGhostUsesSharedPlayAreaClip();
  testSyntheticReplayGhostRespectsLaneCoverVisibleHeight();
  testEvaluatedSkinPublishesPerLaneReplayGhostGeometry();
  testSubmittedSkinRendersOptionGatedSyntheticReplayGhosts();
  testSubmittedSkinRendersPreparationIndicatorsFromItsLaneLayout();
  testInvalidSessionSerialDoesNotConsumeFrameOwners();
  testPassiveCustomTimerUsesTheSharedSessionFrame();
  testProductionPrepareIsExternallySideEffectFreeAndRejectsDoublePrepare();
  testSuccessfulRenderConsumesOnceSubmitsExactBgaAndPublishesLayout();
  testSkinLaneTouchLayoutUsesDrawableScreenCoordinates();
  testCriticalEvaluationAndPreflightFailuresPublishNoFrameState();
  testForwardCompatiblePersistedMutationsEnqueueOneExactOrderedBatch();
  testAudioVolumeMutationAppliesOnlyAfterSkinSubmission();
  testPracticeScrollMutationAppliesOnlyAfterSkinSubmission();
  testPracticeMenuItemMutationAppliesOnlyAfterSkinSubmission();
  testPracticeVisibleItemsMutationAppliesOnlyAfterSkinSubmission();
  testPersistenceRequestIsFullyAllocatedBeforeSkinSubmission();
  testQueueFullAndClosedAreRecoverableOnlyAfterSuccessfulSkinDraw();
  testEditableTextUsesEndCursorUtf8BackspaceAndReturnCommit();
  testEditableTextOutsideClickCommitsAndFocusTransferIsOrdered();
  testEditableTextOutsideClickPreservesGlobalInteractionOrder();
  testEditableStringWriterDoesNotRunWhenSubmissionPreflightFails();
  testEditableTextBoundsFocusedAndQueuedUtf8();
  testEditableTextCancellationTeardownAndNoneditableRejection();
  testTouchCaptureLifecycleKeepsWritingCapturedSlidersDuringDrag();
  testImageActTouchQueuesPinnedEventOnDown();
  testViewportChangeCancelsCapturesAndInvalidatesPublishedGeometry();
  testViewportGeometryChangeCancelsOldInputAndPreservesSessionIdentity();
  testTouchLayoutNormalizesAgainstTheWholeWindowWithSafeOrigin();
  testSuccessfulGeometryChangesOnlyHitRevisionAndTeardownDiscardsState();
  testLegacyRendererAdapterBeginsInternallyAndRejectsDoubleBegin();
  testResultLuaSessionBindsMainStateDuringConfiguredLoad();
  testResultLuaSessionRoutesOpenIrEvent();
  testResultLuaSessionDefersNestedCustomEventsToTheNextFrame();
  testResultLuaSessionManualCustomEventSuppressesAutomaticRepeat();
  testResultLuaSessionRejectsRecursiveCustomEvents();
  testResultLuaSessionUsesTheLastDuplicateCustomEventDefinition();
  testResultLuaSessionUsesTheLastDuplicateCustomTimerDefinition();
  testStaticResultSessionRunsCustomBuiltinEvent();
  testResultSkinInputAvailabilityMatchesResultTimer();
  testResultSessionRefreshesForAsynchronousRankingNames();
  testResultSessionRefreshesForAllStringSelectors();
  testResultSessionRejectsConfiguredModelForAnotherResultTarget();
  testResultBridgeSupportsBeatorajaIrAvailabilityProperties();
  testResultBridgeUsesSourceDefaultScoreReferencesAndRateFallbacks();
  testResultBridgeRetainsIrRatePropertyFallbacks();
  testResultBridgeRecognizesBeatorajaNamedIrIntegerProperties();
  testResultBridgeKeepsMainStateGaugeHelpersGameplayOnly();
  testResultBridgeUsesPreparedArtworkAvailability();
  testResultBridgeKeepsAutoplayOptionsOffOnResultScreens();
  testResultBridgeRecognizesScratchLongNotes();
  testResultBridgeMatchesBeatorajaResultScoreFamilies();
  testResultBridgeUsesProjectedKeyModeForScorePoint();
  testResultBridgeMatchesResultAliasesAndTimerUnits();
  testResultBridgeUsesCapturedReplayImageIndexes();
  testResultBridgeUsesSourceImageIndexFactoryFallbacks();
  testResultBridgeResolvesImageNamesBeforeValueAliases();
  testResultBridgeRetainsResultConfigurationProperties();
  testResultBridgeRetainsResultIntegerFactoryFallbacks();
  testResultBridgeRetainsAuthenticatedScoreDate();
  testResultSkinConfigurationCarriesPlayerConfigAcrossResultSurfaces();
  testResultSkinConfigurationClampsExtendedGaugeLowerBounds();
  testResultBridgeSharesSourceStringPropertyResolution();
  testResultBridgeProjectsLongNoteModeImageIndex();
  testResultBridgeMatchesBeatorajaTableFullString();
  testResultBridgeDoesNotInventRemoteGaugeImageIndex();
  testResultBridgeKeepsResultPropertyContractsForAbsentAndStaticData();
  testResultBridgeConvertsClearRanksToBeatorajaImageIndexes();
  testResultBridgeRetainsPreparedChartResultProperties();
  testResultBridgeProjectsIrRankingRows();
  testResultBridgeMapsNamedResultAndRankingProperties();
  testResultBridgeMapsNamedIntegerScoreProperties();
  testResultBridgeProjectsReplayLaneAssignments();
  testResultBridgeExposesPlayerHistoryProperties();
  testResultBridgeExposesResultTimingDistributionStatistics();
  testResultBridgeKeepsEmptyMusicResultTimingDefaults();
  testCourseResultBridgeDoesNotInventMusicResultTimingStatistics();
  testResultBridgeUsesRemotePresentationValues();
  testResultBridgePreservesCompletedGameplayGraph();
  testRequestedExternalResultSkinCreatesSession();
  if (failures != 0) {
    std::cerr << failures << " play skin session test(s) failed\n";
    return 1;
  }
  std::cout << "play skin session tests passed\n";
  return 0;
}
