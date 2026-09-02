#include "MusicSelectSkinSession.h"

#include "BgfxSkinTextureDevice.h"
#include "GameplaySkinSourceFormat.h"
#include "LuaJValueCoercion.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinHostModules.h"
#include "MusicSelectBarRenderer.h"
#include "PlaySkinViewport.h"
#include "../SkinTargetTraits.h"
#include "../../rendering/SkinQuadBatchRenderer.h"
#include "../../rendering/common.h"
#include "../../view/ImageFileDecoder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <limits>
#include <ranges>
#include <set>
#include <utility>
#include <utf8proc.h>

namespace skin {
namespace {

SkinDiagnostic failure(std::string code, std::string message,
                       std::string path = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(path),
          .severity = DiagnosticSeverity::Error};
}

bool hasErrors(const std::vector<SkinDiagnostic> &diagnostics) {
  return std::ranges::any_of(diagnostics, [](const auto &diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
  });
}

using RuntimeStringsByObject =
    std::map<SkinObjectId, std::vector<std::string>>;

void appendRuntimeString(RuntimeStringsByObject &strings, SkinObjectId object,
                         std::string_view value) {
  if (value.empty()) return;
  auto &values = strings[object];
  if (std::ranges::find(values, value) == values.end()) {
    values.emplace_back(value);
  }
}

RuntimeStringsByObject musicSelectRuntimeAtlasStrings(
    const ValidatedBeatorajaSkinModel &model,
    const MusicSelectSkinFrame &frame,
    const RuntimeStringsByObject &observedText = {}) {
  RuntimeStringsByObject result;
  MusicSelectSkinStateBridge state(frame);
  for (const auto &definition : model.model.objects) {
    const auto *text = std::get_if<SkinTextObject>(&definition.payload);
    if (text == nullptr || !text->value) continue;
    const auto binding = std::ranges::find_if(
        model.model.stringProperties,
        [&](const SkinStringPropertyBinding &candidate) {
          return candidate.id == *text->value;
        });
    if (binding == model.model.stringProperties.end()) continue;
    if (const auto *builtin =
            std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
      const auto value = state.stringProperty(*builtin);
      if (value.supported) {
        appendRuntimeString(result, definition.id, value.value);
      }
    }
  }
  for (const auto &definition : model.model.objects) {
    const auto *songList =
        std::get_if<SkinSongListObject>(&definition.payload);
    if (songList == nullptr) continue;
    // BarRenderer prepares every title character in the current directory
    // when its text changes, not merely the sixty rows visible this frame.
    // Keep the same per-text-object corpus so list scrolling cannot request a
    // whole resource catalog rebuild for a title which was already known to
    // the selector's current directory.
    for (const auto &presentation : songList->text) {
      if (presentation.object == 0) {
        continue;
      }
      for (const MusicSelectBarFrame &bar : frame.songList.bars) {
        appendRuntimeString(result, presentation.object, bar.title);
      }
    }
  }
  for (const auto &[object, values] : observedText) {
    for (const auto &value : values) {
      appendRuntimeString(result, object, value);
    }
  }
  return result;
}

bool atlasContainsText(const PreparedSkinTextAtlas &atlas,
                       std::string_view value) {
  for (std::size_t offset = 0; offset < value.size();) {
    utf8proc_int32_t codepoint = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t *>(value.data() + offset),
        static_cast<utf8proc_ssize_t>(value.size() - offset), &codepoint);
    if (consumed <= 0) {
      codepoint = 0xfffd;
      ++offset;
    } else {
      offset += static_cast<std::size_t>(consumed);
    }
    if (codepoint != '\r' && codepoint != '\n' &&
        !atlas.glyphs.contains(static_cast<char32_t>(codepoint))) {
      return false;
    }
  }
  return true;
}

std::map<int, std::filesystem::path>
musicSelectBuiltinImagePaths(const MusicSelectSkinFrame &frame) {
  std::map<int, std::filesystem::path> paths;
  if (!frame.stageFile.empty()) paths.emplace(100, frame.stageFile);
  if (!frame.banner.empty()) paths.emplace(102, frame.banner);
  return paths;
}

MusicSelectBuiltinImagePatch prepareBuiltinImagePatch(
    std::map<int, std::filesystem::path> paths,
    SkinBuiltinImageReader reader, SkinSafetyPolicy safetyPolicy,
    std::stop_token stop) {
  MusicSelectBuiltinImagePatch result{.paths = std::move(paths)};
  for (const int reference : {100, 102}) {
    const auto path = result.paths.find(reference);
    if (stop.stop_requested() || path == result.paths.end() ||
        path->second.empty() || !reader) {
      result.images.emplace(reference, std::nullopt);
      continue;
    }
    std::vector<unsigned char> encoded;
    std::string readError;
    if (!reader(path->second, encoded,
                skinResourceLimit(safetyPolicy,
                                  SkinResourcePolicy::maximumEncodedBytes),
                &readError, stop) ||
        stop.stop_requested()) {
      result.images.emplace(reference, std::nullopt);
      continue;
    }
    auto decoded = image_decode::decodeImageMemory(
        std::as_bytes(std::span(encoded)),
        {.maximumDimension = skinResourceDimensionLimit(safetyPolicy),
         .maximumEncodedBytes = skinResourceLimit(
             safetyPolicy, SkinResourcePolicy::maximumEncodedBytes),
         .maximumDecodedBytes = skinResourceLimit(
             safetyPolicy, SkinResourcePolicy::maximumImageBytes),
         .stop = stop});
    result.images.emplace(reference, std::move(decoded));
  }
  return result;
}

MusicSelectTextAtlasPatch prepareTextAtlasPatch(
    SkinRevisionLease revision, SkinEntryId entry,
    ValidatedBeatorajaSkinModel model,
    BeatorajaSkinConfiguration configuration, SkinStorageRoots storageRoots,
    SkinProfileId profileId, SkinResourcePreparationService &preparation,
    SkinSafetyPolicy safetyPolicy, RuntimeStringsByObject runtimeStrings,
    std::set<SkinObjectId> targetObjects, std::stop_token stop) {
  MusicSelectTextAtlasPatch result{
      .runtimeStrings = std::move(runtimeStrings)};
  if (stop.stop_requested()) {
    result.cancelled = true;
    return result;
  }
  auto files = LuaSkinFileSystem::create(
      {.revision = revision.readView(),
       .entry = entry,
       .storageRoots = std::move(storageRoots),
       .profileId = std::move(profileId),
       .safetyPolicy = safetyPolicy});
  if (!files.fileSystem) {
    result.diagnostics.push_back(failure(
        "skin.music_select_session.atlas_filesystem_create_failed",
        files.failure ? files.failure->message
                      : "Music-select font refresh filesystem could not be created."));
    return result;
  }
  auto planned = preparation.prepareTextAtlasUpdates(
      {.revision = std::move(revision),
       .entry = std::move(entry),
       .fileSystem = *files.fileSystem,
       .model = model,
       .configuration = configuration,
       .requiredRuntimeStringsByObject = result.runtimeStrings,
       .targetObjects = std::move(targetObjects),
       .safetyPolicy = safetyPolicy,
       .stop = stop});
  result.cancelled = planned.cancelled;
  result.diagnostics = std::move(planned.diagnostics);
  if (planned.plan) {
    result.atlases = std::move(planned.plan->atlases);
  }
  return result;
}

class LuaFrameStateBinding final {
public:
  LuaFrameStateBinding(LuaSkinRuntime *runtime, ISkinFrameState *state,
                       LuaSkinEventExecutor executor = {})
      : runtime_(runtime) {
    if (runtime_ != nullptr) {
      runtime_->setFrameState(state);
      runtime_->setEventExecutor(executor);
    }
  }

  ~LuaFrameStateBinding() {
    if (runtime_ != nullptr) {
      runtime_->setEventExecutor({});
      runtime_->setFrameState(nullptr);
    }
  }

private:
  LuaSkinRuntime *runtime_ = nullptr;
};

} // namespace

AuthoredSize
musicSelectSkinSourceResolution(const BeatorajaSkinHeader &header) noexcept {
  constexpr std::array<AuthoredSize, 15> resolutions{{
      {640.0, 480.0},   {800.0, 600.0},    {1024.0, 768.0},
      {1280.0, 720.0},  {1280.0, 960.0},   {1366.0, 768.0},
      {1400.0, 1050.0}, {1600.0, 900.0},   {1600.0, 1200.0},
      {1680.0, 1050.0}, {1920.0, 1080.0},  {1920.0, 1200.0},
      {2048.0, 1536.0}, {2560.0, 1440.0},  {3840.0, 2160.0},
  }};
  const auto matched = std::ranges::find_if(resolutions, [&](const auto &size) {
    return header.width == static_cast<int>(size.width) &&
           header.height == static_cast<int>(size.height);
  });
  return matched != resolutions.end() ? *matched : AuthoredSize{1280.0, 720.0};
}

MusicSelectSkinSession::MusicSelectSkinSession(
    std::uint64_t sessionSerial, SkinProfileId profileId,
    SkinRevisionLease revision, SkinEntryId entry,
    ValidatedBeatorajaSkinModel model,
    BeatorajaSkinConfiguration configuration,
    std::unique_ptr<LuaSkinRuntime> runtime,
    std::unique_ptr<SkinResourceCatalog> resources,
    std::unique_ptr<SkinMovieCatalog> movies, SkinStorageRoots storageRoots,
    SkinResourcePreparationService &resourcePreparation,
    std::shared_ptr<SkinTextureDevice> textureDevice,
    SkinBuiltinImageReader builtinImageReader,
    std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters,
    rendering::SkinQuadBatchBackend *quadBackend,
    SkinSafetyPolicy safetyPolicy, ViewportSettings viewportSettings,
    std::stop_token stop,
    RuntimeStringsByObject preparedRuntimeStringsByObject,
    std::map<int, std::filesystem::path> preparedBuiltinImagePaths)
    : sessionSerial_(sessionSerial), profileId_(std::move(profileId)),
      revision_(std::move(revision)), entry_(std::move(entry)),
      model_(std::move(model)), configuration_(std::move(configuration)),
      runtime_(std::move(runtime)), resources_(std::move(resources)),
      movies_(std::move(movies)), storageRoots_(std::move(storageRoots)),
      resourcePreparation_(&resourcePreparation),
      textureDevice_(std::move(textureDevice)),
      builtinImageReader_(std::move(builtinImageReader)),
      liveResourceCounters_(std::move(liveResourceCounters)),
      safetyPolicy_(safetyPolicy), viewportSettings_(viewportSettings),
      stop_(stop),
      quadRenderer_(quadBackend
                        ? std::make_unique<rendering::SkinQuadBatchRenderer>(
                              *quadBackend)
                        : std::make_unique<rendering::SkinQuadBatchRenderer>()),
      preparedRuntimeStringsByObject_(
          std::move(preparedRuntimeStringsByObject)),
      preparedBuiltinImagePaths_(std::move(preparedBuiltinImagePaths)) {
  for (std::size_t index = 0; index < model_.model.customEvents.size();
       ++index) {
    customEventLastDefinitionIndexes_.insert_or_assign(
        model_.model.customEvents[index].id, index);
  }
  for (std::size_t index = 0; index < model_.model.customTimers.size();
       ++index) {
    customTimerLastDefinitionIndexes_.insert_or_assign(
        model_.model.customTimers[index].id, index);
    if (model_.model.customTimers[index].timer) {
      activeCustomTimerIds_.insert(model_.model.customTimers[index].id);
    }
  }
}

MusicSelectSkinSession::~MusicSelectSkinSession() {
  builtinImagePatchStop_.request_stop();
  textAtlasPatchStop_.request_stop();
  if (pendingBuiltinImagePatch_.valid()) {
    try {
      (void)pendingBuiltinImagePatch_.get();
    } catch (...) {
      // A selected-song image is optional, and the session is already ending.
    }
  }
  if (pendingTextAtlasPatch_.valid()) {
    try {
      (void)pendingTextAtlasPatch_.get();
    } catch (...) {
      // The next selector frame will never consume a cancelled atlas patch.
    }
  }
}

int MusicSelectSkinSession::inputDelayMillis() const noexcept {
  return model_.model.timing.inputMillis;
}

void MusicSelectSkinSession::suspendAudio() noexcept {
  if (runtime_) runtime_->suspendAudio();
}

void MusicSelectSkinSession::resumeAudio() noexcept {
  if (runtime_) runtime_->resumeAudio();
}

MusicSelectSkinSessionPreparationResult MusicSelectSkinSession::prepare(
    GameplaySkinActivationRequest request,
    MusicSelectSkinSessionPreparationContext context) {
  MusicSelectSkinSessionPreparationResult result;
  const auto safetyPolicy = musicSelectSkinCompatibilityPolicy();
  const auto &activation = request.activation;
  if (activation.entry.package != activation.revision.revision().package) {
    result.diagnostics.push_back(failure(
        "skin.music_select_session.context_invalid",
        "Music-select skin activation identity is invalid."));
    return result;
  }
  const auto format = gameplaySkinSourceFormatForPath(
      activation.entry.packageRelativePath);
  if (!format || *format != GameplaySkinSourceFormat::Lua) {
    result.diagnostics.push_back(failure(
        "skin.music_select_session.source_unsupported",
        "Music-select skin entry is not a Lua skin.",
        activation.entry.packageRelativePath));
    return result;
  }

  try {
    const auto revision = activation.revision.readView();
    auto resourceFiles = LuaSkinFileSystem::create(
        {.revision = revision,
         .entry = activation.entry,
         .storageRoots = context.storageRoots,
         .profileId = request.profileId,
         .safetyPolicy = safetyPolicy});
    if (!resourceFiles.fileSystem) {
      result.diagnostics.push_back(failure(
          "skin.music_select_session.filesystem_create_failed",
          resourceFiles.failure
              ? resourceFiles.failure->message
              : "Music-select skin filesystem could not be created."));
      return result;
    }
    auto luaFiles = LuaSkinFileSystem::create(
        {.revision = revision,
         .entry = activation.entry,
         .storageRoots = context.storageRoots,
         .profileId = request.profileId,
         .allowDataWrites = true,
         .safetyPolicy = safetyPolicy});
    if (!luaFiles.fileSystem) {
      result.diagnostics.push_back(failure(
          "skin_lua_filesystem_create_failed",
          luaFiles.failure
              ? luaFiles.failure->message
              : "Music-select Lua filesystem could not be created."));
      return result;
    }

    std::vector<MusicSelectSkinAction> initialActions;
    GameplaySkinDocumentLoader loader;
    auto loaded = loader.load(
        {.sourceFormat = GameplaySkinSourceFormat::Lua,
         .entry = activation.entry,
         .documentFileSystem = *resourceFiles.fileSystem,
         .luaFileSystem = std::move(luaFiles.fileSystem),
         .luaAudioBackend = std::move(context.audioBackend),
         .desiredSettings = &activation.reconciledSettings,
         .expectedConfigurationDigest = activation.configurationDigest,
         .luaPurpose = LuaRuntimePurpose::MusicSelect,
         .loadConfiguredLua = [&context, &initialActions](
                                  LuaSkinRuntime &runtime,
                                  const BeatorajaSkinConfiguration &configuration,
                                  std::vector<SkinDiagnostic> &) {
           if (context.initialLegacyInputGeneration) {
             runtime.setLegacyInputGeneration(
                 *context.initialLegacyInputGeneration);
           }
           MusicSelectSkinStateBridge bridge(
               context.initialFrame,
               {.floatWriter = [&initialActions](int id, double value) {
                  initialActions.push_back(
                      {.kind = MusicSelectSkinActionKind::FloatWriter,
                       .selector = SkinBuiltinPropertySelector{.value = id},
                       .floatValue = value});
                }});
           LuaFrameStateBinding frameState(&runtime, &bridge);
           return runtime.loadConfigured(configuration);
         },
         .safetyPolicy = safetyPolicy,
         .stop = context.stop});
    result.diagnostics = std::move(loaded.diagnostics);
    if (loaded.cancelled || context.stop.stop_requested()) {
      result.cancelled = true;
      return result;
    }
    if (!loaded.document || hasErrors(result.diagnostics)) {
      return result;
    }
    auto document = std::move(*loaded.document);
    const auto target = skinTargetTraitForType(document.model.model.header.type);
    if (!target || target->kind != SkinTargetKind::MusicSelect ||
        document.model.model.header.type != 5) {
      result.diagnostics.push_back(failure(
          "skin.music_select_session.type_mismatch",
          "Configured document is not a Beatoraja type-5 music-select skin.",
          activation.entry.packageRelativePath));
      return result;
    }

    auto runtimeAtlasStrings = musicSelectRuntimeAtlasStrings(
        document.model, context.initialFrame);
    // Beatoraja's selector receives its selected chart artwork from the
    // loader after the skin becomes active. Do not hold first paint for it.
    std::map<int, std::filesystem::path> builtinImagePaths;
    auto planned = context.resourcePreparation.decodeAndPlan(
        {.revision = activation.revision.clone(),
         .entry = activation.entry,
         .fileSystem = *resourceFiles.fileSystem,
         .model = document.model,
         .configuration = document.configuration,
         .requiredRuntimeStringsByObject = runtimeAtlasStrings,
         .builtinImagePaths = builtinImagePaths,
         .builtinImageReader = context.builtinImageReader,
         .safetyPolicy = safetyPolicy,
         .stop = context.stop});
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(planned.diagnostics.begin()),
        std::make_move_iterator(planned.diagnostics.end()));
    if (planned.cancelled || context.stop.stop_requested()) {
      result.cancelled = true;
      return result;
    }
    if (!planned.plan || hasErrors(result.diagnostics)) {
      return result;
    }

    // Movies and GPU resources remain pending until render-owner finalization.

    result.prepared.emplace(MusicSelectSkinSessionPrepared{
        .request = std::move(request),
        .storageRoots = std::move(context.storageRoots),
        .document = std::move(document),
        .resourcePlan = std::move(*planned.plan),
        .runtimeAtlasStrings = std::move(runtimeAtlasStrings),
        .builtinImagePaths = std::move(builtinImagePaths),
        .initialActions = std::move(initialActions),
        .builtinImageReader = std::move(context.builtinImageReader),
        .safetyPolicy = safetyPolicy,
        .stop = context.stop,
    });
  } catch (...) {
    result.diagnostics.push_back(failure(
        "skin.music_select_session.prepare_failed",
        "Music-select skin preparation failed."));
  }
  return result;
}

MusicSelectSkinSessionCreateResult MusicSelectSkinSession::finalize(
    MusicSelectSkinSessionPrepared prepared,
    MusicSelectSkinSessionFinalizationContext context) {
  MusicSelectSkinSessionCreateResult result;
  auto &request = prepared.request;
  auto &activation = request.activation;
  if (!context.textureDevice || !context.liveResourceCounters) {
    result.diagnostics.push_back(failure(
        "skin.music_select_session.context_invalid",
        "Music-select skin render services are unavailable."));
    return result;
  }
  if (prepared.stop.stop_requested()) return result;

  try {
    const auto revision = activation.revision.readView();
    auto resourceFiles = LuaSkinFileSystem::create(
        {.revision = revision,
         .entry = activation.entry,
         .storageRoots = prepared.storageRoots,
         .profileId = request.profileId,
         .safetyPolicy = prepared.safetyPolicy});
    if (!resourceFiles.fileSystem) {
      result.diagnostics.push_back(failure(
          "skin.music_select_session.filesystem_create_failed",
          resourceFiles.failure
              ? resourceFiles.failure->message
              : "Music-select skin filesystem could not be created."));
      return result;
    }

    auto movieDevice = std::move(context.movieDevice);
#if ASOBMASHOW_ENABLE_SKIN_MOVIE_DEVICE
    if (!movieDevice) movieDevice = createSkinMovieDevice();
#endif
    auto preparedMovies = SkinMovieCatalog::prepare(
        {.fileSystem = *resourceFiles.fileSystem,
         .model = prepared.document.model,
         .configuration = prepared.document.configuration,
         .device = std::move(movieDevice),
         .safetyPolicy = prepared.safetyPolicy,
         .liveResourceCounters = context.liveResourceCounters,
         .stop = prepared.stop,
         .sessionDecodedBytes = prepared.resourcePlan.decodedBytes});
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(preparedMovies.diagnostics.begin()),
        std::make_move_iterator(preparedMovies.diagnostics.end()));
    if (preparedMovies.cancelled || prepared.stop.stop_requested() ||
        !preparedMovies.catalog || hasErrors(result.diagnostics)) {
      return result;
    }

    auto uploaded = SkinResourceCatalog::upload(
        std::move(prepared.resourcePlan), context.textureDevice,
        context.liveResourceCounters);
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(uploaded.diagnostics.begin()),
        std::make_move_iterator(uploaded.diagnostics.end()));
    if (!uploaded.catalog || hasErrors(result.diagnostics)) return result;

    if (!prepared.document.luaRuntime) {
      result.diagnostics.push_back(failure(
          "skin_lua_runtime_missing",
          "Music-select Lua skin has no configured runtime."));
      return result;
    }
    const auto entered = prepared.document.luaRuntime->enterRenderPhase();
    if (!entered.ok) {
      result.diagnostics.push_back(entered.failure.value_or(failure(
          "skin_lua_render_phase_failed",
          "Music-select Lua skin could not enter its render phase.")));
      return result;
    }
    uploaded.catalog->enterRenderPhase();
    result.session = std::unique_ptr<MusicSelectSkinSession>(
        new MusicSelectSkinSession(
            request.sessionSerial, std::move(request.profileId),
            std::move(activation.revision), std::move(activation.entry),
            std::move(prepared.document.model),
            std::move(prepared.document.configuration),
            std::move(prepared.document.luaRuntime),
            std::move(uploaded.catalog), std::move(preparedMovies.catalog),
            std::move(prepared.storageRoots), context.resourcePreparation,
            std::move(context.textureDevice),
            std::move(prepared.builtinImageReader),
            std::move(context.liveResourceCounters), context.quadBackend,
            prepared.safetyPolicy, request.viewport, prepared.stop,
            std::move(prepared.runtimeAtlasStrings),
            std::move(prepared.builtinImagePaths)));
    result.session->queuedActions_ = std::move(prepared.initialActions);
    result.session->captureLegacyInputGeneration_ =
        std::move(context.captureLegacyInputGeneration);
  } catch (...) {
    result.session.reset();
    result.diagnostics.push_back(failure(
        "skin.music_select_session.finalize_failed",
        "Music-select skin finalization failed."));
  }
  return result;
}

MusicSelectSkinSessionCreateResult MusicSelectSkinSession::create(
    GameplaySkinActivationRequest request,
    MusicSelectSkinSessionContext context) {
  std::optional<LuaSkinLegacyInputGeneration> initialGeneration;
  if (context.captureLegacyInputGeneration) {
    initialGeneration = context.captureLegacyInputGeneration();
  }
  auto preparation = prepare(
      std::move(request),
      {.storageRoots = std::move(context.storageRoots),
       .resourcePreparation = context.resourcePreparation,
       .initialFrame = std::move(context.initialFrame),
       .builtinImageReader = std::move(context.builtinImageReader),
       .audioBackend = std::move(context.audioBackend),
       .initialLegacyInputGeneration = std::move(initialGeneration),
       .stop = context.stop});
  MusicSelectSkinSessionCreateResult result;
  result.diagnostics = std::move(preparation.diagnostics);
  if (!preparation.prepared) return result;
  auto finalized = finalize(
      std::move(*preparation.prepared),
      {.resourcePreparation = context.resourcePreparation,
       .textureDevice = std::move(context.textureDevice),
       .movieDevice = std::move(context.movieDevice),
       .liveResourceCounters = std::move(context.liveResourceCounters),
       .quadBackend = context.quadBackend,
       .captureLegacyInputGeneration =
           std::move(context.captureLegacyInputGeneration)});
  result.diagnostics.insert(
      result.diagnostics.end(),
      std::make_move_iterator(finalized.diagnostics.begin()),
      std::make_move_iterator(finalized.diagnostics.end()));
  result.session = std::move(finalized.session);
  return result;
}

bool MusicSelectSkinSession::render(RenderContext &renderContext,
                                    const MusicSelectSkinFrame &frame) {
  if (!resources_ || !movies_ || frame.serial == 0) {
    diagnostics_.push_back(failure(
        "skin.music_select_session.frame_invalid",
        "Music-select skin frame has no prepared resources or valid serial."));
    return false;
  }
  updateBuiltinImages(frame);
  // A dynamic text atlas may still be preparing. Skin2DRenderer suppresses
  // only the text run whose glyphs are not resident, allowing the rest of
  // the selector to follow BarRenderer's normal draw path.
  (void)updateRuntimeTextAtlases(frame);

  currentEventMicros_ = frame.elapsedMillis * 1'000;
  frameActions_ = std::exchange(queuedActions_, {});
  executingFrame_ = true;
  const auto finishFrame = [this](bool success) {
    executingFrame_ = false;
    if (success) {
      publishedActions_ = std::exchange(frameActions_, {});
    } else {
      frameActions_.clear();
    }
    return success;
  };

  if (captureLegacyInputGeneration_) {
    runtime_->setLegacyInputGeneration(captureLegacyInputGeneration_());
  }
  MusicSelectSkinStateBridge bridge(
      frame, customTimerValues_, activeCustomTimerIds_,
      {.floatWriter = [this](int id, double value) {
         frameActions_.push_back(
             {.kind = MusicSelectSkinActionKind::FloatWriter,
              .selector = SkinBuiltinPropertySelector{.value = id},
              .floatValue = value});
       }});
  bridge.setPublishedSongResources(
      {.stageFile = resources_->builtinImageResource(100).has_value(),
       .banner = resources_->builtinImageResource(102).has_value(),
       .backBmp = false});
  LuaFrameStateBinding frameState(
      runtime_.get(), &bridge,
      {.context = this,
       .execute = &MusicSelectSkinSession::executeHostEvent});
  const auto viewport = evaluatePlaySkinViewport(
      musicSelectSkinSourceResolution(model_.model.header),
      {.x = 0.0,
       .y = 0.0,
       .width = static_cast<double>(rendering::window_width),
       .height = static_cast<double>(rendering::window_height)},
      viewportSettings_);
  if (!viewport.valid) {
    diagnostics_.push_back(failure(
        "skin.music_select_session.viewport_invalid",
        "Music-select skin viewport could not be evaluated."));
    return finishFrame(false);
  }
  const auto begun = runtime_->beginFrame(frame.serial);
  if (!begun.ok) {
    diagnostics_.push_back(begun.failure.value_or(failure(
        "skin.music_select_session.frame_begin_failed",
        "Music-select Lua runtime rejected the render frame.")));
    return finishFrame(false);
  }
  if (!executeQueuedCallbacks(bridge)) return finishFrame(false);

  for (const auto &[id, index] : customTimerLastDefinitionIndexes_) {
    const auto &timer = model_.model.customTimers[index];
    std::int64_t value = bridge.timerProperty({.value = id});
    if (timer.timer) {
      const auto binding = std::ranges::find_if(
          model_.model.timerProperties,
          [&](const SkinTimerPropertyBinding &candidate) {
            return candidate.id == *timer.timer;
          });
      if (binding == model_.model.timerProperties.end()) {
        diagnostics_.push_back(failure(
            "skin.music_select_session.custom_timer_missing",
            "Music-select custom timer binding is absent."));
        return finishFrame(false);
      }
      if (const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(
              &binding->source)) {
        value = bridge.timerProperty(*builtin);
      } else {
        const auto callback = runtime_->invoke(
            std::get<LuaCallbackId>(binding->source), {});
        if (callback.failure || !callback.value ||
            (safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit) &&
             !std::holds_alternative<std::int64_t>(*callback.value))) {
          if (!safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit)) {
            value = std::numeric_limits<std::int64_t>::min();
            bridge.setCustomTimer(id, value);
            continue;
          }
          diagnostics_.push_back(callback.failure.value_or(failure(
              "skin.music_select_session.custom_timer_type",
              "Music-select custom timer did not return an integer timestamp.")));
          return finishFrame(false);
        }
        value = !safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit)
                    ? luaJToLong(*callback.value)
                    : std::get<std::int64_t>(*callback.value);
      }
    }
    bridge.setCustomTimer(id, value);
  }

  for (const auto &[id, index] : customEventLastDefinitionIndexes_) {
    const auto &event = model_.model.customEvents[index];
    if (!event.condition) continue;
    const auto condition = std::ranges::find_if(
        model_.model.booleanProperties,
        [&](const SkinBooleanPropertyBinding &candidate) {
          return candidate.id == *event.condition;
        });
    if (condition == model_.model.booleanProperties.end()) {
      diagnostics_.push_back(failure(
          "skin.music_select_session.custom_event_condition_missing",
          "Music-select custom event condition is absent."));
      return finishFrame(false);
    }
    bool active = false;
    if (const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(
            &condition->source)) {
      active = bridge.booleanProperty(*builtin).value;
    } else {
      const auto callback = runtime_->invoke(
          std::get<LuaCallbackId>(condition->source), {});
      if (callback.failure || !callback.value ||
          (safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit) &&
           !std::holds_alternative<bool>(*callback.value))) {
        if (!safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit)) {
          continue;
        }
        diagnostics_.push_back(callback.failure.value_or(failure(
            "skin.music_select_session.custom_event_condition_type",
            "Music-select custom event condition did not return a boolean.")));
        return finishFrame(false);
      }
      active = !safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit)
                   ? luaJToBoolean(*callback.value)
                   : std::get<bool>(*callback.value);
    }
    const auto previous = customEventLastExecutionMicros_.find(id);
    if (!active ||
        (previous != customEventLastExecutionMicros_.end() &&
         (currentEventMicros_ - previous->second) / 1'000 <
             event.minimumIntervalMillis)) {
      continue;
    }
    if (!queueEventBinding(event.action, {}) ||
        !executeQueuedCallbacks(bridge)) {
      return finishFrame(false);
    }
    customEventLastExecutionMicros_.insert_or_assign(id,
                                                     currentEventMicros_);
  }

  if (!executeQueuedCallbacks(bridge)) return finishFrame(false);

  RuntimeStringsByObject observedText;
  SkinExternalFrameOwnership ownership(frame.serial, sessionSerial_);
  auto evaluated = renderer_.evaluateFrame(
      {.frameSerial = frame.serial,
       .sessionSerial = sessionSerial_,
       .visualTimeMicros = currentEventMicros_,
       .model = model_,
       .configuration = configuration_,
       .resources = *resources_,
       .movies = movies_.get(),
       .viewport = viewport,
       .runtime = runtime_.get(),
       .state = bridge,
       .safetyPolicy = safetyPolicy_,
       .musicSelectSongList = &frame.songList,
       .observedTextValue = [&observedText](SkinObjectId object,
                                             std::string_view value) {
         appendRuntimeString(observedText, object, value);
       }},
      std::move(ownership));
  observedRuntimeStringsByObject_ = std::move(observedText);
  if (!evaluated.submitReady) {
    diagnostics_.insert(
        diagnostics_.end(),
        std::make_move_iterator(evaluated.diagnostics.begin()),
        std::make_move_iterator(evaluated.diagnostics.end()));
    if (!hasErrors(diagnostics_)) {
      diagnostics_.push_back(failure(
          "skin.music_select_session.frame_evaluation_failed",
          "Music-select skin frame did not produce a drawable command buffer."));
    }
    return finishFrame(false);
  }
  if (!renderer_.submit(*evaluated.submitReady, *resources_, renderContext,
                        *quadRenderer_, movies_.get(), viewport)) {
    diagnostics_.insert(
        diagnostics_.end(),
        std::make_move_iterator(evaluated.diagnostics.begin()),
        std::make_move_iterator(evaluated.diagnostics.end()));
    diagnostics_.push_back(failure(
        "skin.music_select_session.frame_submission_failed",
        "Music-select skin frame submission failed."));
    return finishFrame(false);
  }
  diagnostics_.insert(
      diagnostics_.end(),
      std::make_move_iterator(evaluated.diagnostics.begin()),
      std::make_move_iterator(evaluated.diagnostics.end()));
  publishedInteractionLayout_ = std::move(evaluated.interactionLayout);
  return finishFrame(true);
}

void MusicSelectSkinSession::updateBuiltinImages(
    const MusicSelectSkinFrame &frame) {
  const auto paths = musicSelectBuiltinImagePaths(frame);
  if (pendingBuiltinImagePatch_.valid()) {
    if (pendingBuiltinImagePatch_.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready) {
      if (paths != pendingBuiltinImagePaths_) {
        builtinImagePatchStop_.request_stop();
      }
      return;
    }
    MusicSelectBuiltinImagePatch patch = pendingBuiltinImagePatch_.get();
    pendingBuiltinImagePaths_.clear();
    builtinImagePatchStop_ = std::stop_source{};
    if (patch.paths == paths && resources_) {
      for (auto &[reference, pixels] : patch.images) {
        (void)resources_->replaceBuiltinImage(reference, std::move(pixels));
      }
      preparedBuiltinImagePaths_ = std::move(patch.paths);
    }
  }
  if (paths == preparedBuiltinImagePaths_) {
    return;
  }
  if (resources_) {
    for (const int reference : {100, 102}) {
      const auto requested = paths.find(reference);
      const auto published = preparedBuiltinImagePaths_.find(reference);
      if ((requested == paths.end()) !=
              (published == preparedBuiltinImagePaths_.end()) ||
          (requested != paths.end() &&
           published != preparedBuiltinImagePaths_.end() &&
           requested->second != published->second)) {
        // MusicSelector replaces the BMSResource entry with this SongBar's
        // pixmap. Do not show the former chart while this replacement is
        // pending or after the new image fails to decode.
        (void)resources_->replaceBuiltinImage(reference, std::nullopt);
      }
    }
  }
  if (pendingBuiltinImagePatch_.valid()) {
    return;
  }
  pendingBuiltinImagePaths_ = paths;
  pendingBuiltinImagePatch_ = std::async(
      std::launch::async,
      [paths, reader = builtinImageReader_, safetyPolicy = safetyPolicy_,
       stop = builtinImagePatchStop_.get_token()] mutable {
        return prepareBuiltinImagePatch(std::move(paths), std::move(reader),
                                        safetyPolicy, stop);
      });
}

bool MusicSelectSkinSession::requiresResourceRefresh(
    const MusicSelectSkinFrame &) const {
  // Dynamic selector strings now refresh only their matching font atlas.
  // This compatibility seam remains for callers which previously asked
  // whether the whole package resource catalog had to be rebuilt.
  return false;
}

bool MusicSelectSkinSession::updateRuntimeTextAtlases(
    const MusicSelectSkinFrame &frame) {
  const auto strings = musicSelectRuntimeAtlasStrings(
      model_, frame, observedRuntimeStringsByObject_);
  const auto missingObjects = [&] {
    std::set<SkinObjectId> result;
    for (const auto &[object, values] : strings) {
      if (unavailableTextAtlasObjects_.contains(object)) {
        continue;
      }
      const auto *atlas = resources_ ? resources_->findTextAtlasForObject(object)
                                     : nullptr;
      if (atlas == nullptr ||
          std::ranges::any_of(values, [&](const std::string &value) {
            return !atlasContainsText(*atlas, value);
          })) {
        result.insert(object);
      }
    }
    return result;
  };

  if (pendingTextAtlasPatch_.valid()) {
    if (pendingTextAtlasPatch_.wait_for(std::chrono::milliseconds(0)) !=
        std::future_status::ready) {
      if (strings != pendingRuntimeStringsByObject_) {
        textAtlasPatchStop_.request_stop();
      }
      return missingObjects().empty();
    }
    MusicSelectTextAtlasPatch patch = pendingTextAtlasPatch_.get();
    pendingRuntimeStringsByObject_.clear();
    textAtlasPatchStop_ = std::stop_source{};
    const auto targetObjects = std::exchange(pendingTextAtlasObjects_, {});
    if (!patch.cancelled && patch.runtimeStrings == strings && resources_) {
      diagnostics_.insert(
          diagnostics_.end(),
          std::make_move_iterator(patch.diagnostics.begin()),
          std::make_move_iterator(patch.diagnostics.end()));
      bool applied = !patch.atlases.empty();
      std::set<SkinObjectId> updatedObjects;
      for (auto &update : patch.atlases) {
        const bool replaced = resources_->replaceTextAtlas(
            std::move(update.atlas), update.objects);
        if (replaced) {
          updatedObjects.insert(update.objects.begin(), update.objects.end());
        }
        applied = replaced && applied;
      }
      if (applied) {
        preparedRuntimeStringsByObject_ = std::move(patch.runtimeStrings);
      }
      for (const SkinObjectId object : targetObjects) {
        if (!updatedObjects.contains(object)) {
          unavailableTextAtlasObjects_.insert(object);
        }
      }
    }
  }

  const auto missing = missingObjects();
  if (missing.empty()) {
    return true;
  }
  if (pendingTextAtlasPatch_.valid()) {
    return false;
  }
  pendingRuntimeStringsByObject_ = strings;
  pendingTextAtlasObjects_ = missing;
  pendingTextAtlasPatch_ = std::async(
      std::launch::async,
      [revision = revision_.clone(), entry = entry_, model = model_,
       configuration = configuration_, storageRoots = storageRoots_,
       profileId = profileId_, preparation = resourcePreparation_,
       safetyPolicy = safetyPolicy_, runtimeStrings = std::move(strings),
       targetObjects = missing, stop = textAtlasPatchStop_.get_token()] mutable {
        return prepareTextAtlasPatch(
            std::move(revision), std::move(entry), std::move(model),
            std::move(configuration), std::move(storageRoots),
            std::move(profileId), *preparation, safetyPolicy,
            std::move(runtimeStrings), std::move(targetObjects), stop);
      });
  return false;
}

bool MusicSelectSkinSession::refreshResources(
    const MusicSelectSkinFrame &frame) {
  if (!requiresResourceRefresh(frame)) return true;
  try {
    auto resourceFiles = LuaSkinFileSystem::create(
        {.revision = revision_.readView(),
         .entry = entry_,
         .storageRoots = storageRoots_,
         .profileId = profileId_,
         .safetyPolicy = safetyPolicy_});
    if (!resourceFiles.fileSystem) {
      diagnostics_.push_back(failure(
          "skin.music_select_session.refresh_filesystem_create_failed",
          resourceFiles.failure
              ? resourceFiles.failure->message
              : "Music-select refresh filesystem could not be created."));
      return false;
    }
    auto runtimeAtlasStrings = musicSelectRuntimeAtlasStrings(
        model_, frame, observedRuntimeStringsByObject_);
    auto builtinImagePaths = musicSelectBuiltinImagePaths(frame);
    auto planned = resourcePreparation_->decodeAndPlan(
        {.revision = revision_.clone(),
         .entry = entry_,
         .fileSystem = *resourceFiles.fileSystem,
         .model = model_,
         .configuration = configuration_,
         .requiredRuntimeStringsByObject = runtimeAtlasStrings,
         .builtinImagePaths = builtinImagePaths,
         .builtinImageReader = builtinImageReader_,
         .safetyPolicy = safetyPolicy_,
         .stop = stop_});
    diagnostics_.insert(
        diagnostics_.end(),
        std::make_move_iterator(planned.diagnostics.begin()),
        std::make_move_iterator(planned.diagnostics.end()));
    if (planned.cancelled || !planned.plan || hasErrors(diagnostics_)) {
      return false;
    }
    auto uploaded = SkinResourceCatalog::upload(
        std::move(*planned.plan), textureDevice_, liveResourceCounters_);
    diagnostics_.insert(
        diagnostics_.end(),
        std::make_move_iterator(uploaded.diagnostics.begin()),
        std::make_move_iterator(uploaded.diagnostics.end()));
    if (!uploaded.catalog || hasErrors(diagnostics_)) return false;
    uploaded.catalog->enterRenderPhase();
    resources_ = std::move(uploaded.catalog);
    preparedRuntimeStringsByObject_ = std::move(runtimeAtlasStrings);
    preparedBuiltinImagePaths_ = std::move(builtinImagePaths);
    return true;
  } catch (...) {
    diagnostics_.push_back(failure(
        "skin.music_select_session.refresh_failed",
        "Music-select skin resources could not be refreshed."));
    return false;
  }
}

MusicSelectSkinPointerTarget
MusicSelectSkinSession::pointerTargetAt(UiLogicalPoint point) const noexcept {
  if (!publishedInteractionLayout_) return {};
  const auto bar = publishedInteractionLayout_->musicSelectBarAt(point);
  const auto control = publishedInteractionLayout_->hitTestUiControl(point);
  if (bar &&
      (control.kind == PresentationUiControlKind::None ||
       bar->authoredOrdinal >= control.authoredOrdinal)) {
    return {.kind = MusicSelectSkinPointerTargetKind::Bar,
            .selectIndex = bar->barIndex};
  }
  switch (control.kind) {
  case PresentationUiControlKind::Slider:
  case PresentationUiControlKind::LaneCover:
    return {.kind = MusicSelectSkinPointerTargetKind::Slider};
  case PresentationUiControlKind::Image:
    return {.kind = MusicSelectSkinPointerTargetKind::Image};
  case PresentationUiControlKind::Text:
    return {.kind = MusicSelectSkinPointerTargetKind::Text};
  case PresentationUiControlKind::None:
  case PresentationUiControlKind::NativeOverlay:
  case PresentationUiControlKind::VirtualController:
    return {};
  }
  return {};
}

MusicSelectSkinPointerResult MusicSelectSkinSession::queuePointerDown(
    UiLogicalPoint point, int button, long long eventMicros) {
  MusicSelectSkinPointerResult result;
  const auto target = pointerTargetAt(point);
  if (target.kind == MusicSelectSkinPointerTargetKind::Bar) {
    result.consumed = true;
    result.selectIndex = button == 0 ? target.selectIndex : std::nullopt;
    result.closeDirectory = button != 0;
    return result;
  }
  if (target.kind == MusicSelectSkinPointerTargetKind::None ||
      !publishedInteractionLayout_) {
    return result;
  }
  const auto control = publishedInteractionLayout_->hitTestUiControl(point);
  result.consumed = true;
  if (control.kind == PresentationUiControlKind::Image) {
    const auto invocation = publishedInteractionLayout_->eventInvocationFor(
        control, point, eventMicros);
    if (!invocation) return {};
    const auto binding = std::ranges::find_if(
        model_.model.events, [&](const SkinEventBinding &candidate) {
          return candidate.id.value == invocation->eventBinding;
        });
    if (binding == model_.model.events.end() ||
        !queueEventBinding(binding->id, {&invocation->argument, 1})) {
      return {};
    }
  } else if (control.kind == PresentationUiControlKind::Text) {
    const auto *text = publishedInteractionLayout_->editableTextAtUi(point);
    if (text == nullptr) return {};
    const auto regions = publishedInteractionLayout_->uiHitRegions();
    const auto region = std::ranges::find_if(
        regions, [&](const PresentationUiHitRegion &candidate) {
          return candidate.hit.kind == PresentationUiControlKind::Text &&
                 candidate.hit.sourceObject == text->sourceObject &&
                 candidate.hit.authoredOrdinal == text->authoredOrdinal;
        });
    if (region == regions.end()) return {};
    const auto [minimumX, maximumX] = std::ranges::minmax(
        region->boundary, {}, &UiLogicalPoint::x);
    const auto [minimumY, maximumY] = std::ranges::minmax(
        region->boundary, {}, &UiLogicalPoint::y);
    result.focusedStringWriter = MusicSelectSkinPointerResult::StringFocus{
        .writer = text->writer,
        .currentValue = text->currentValue,
        .bounds = {.x = minimumX.x,
                   .y = minimumY.y,
                   .width = maximumX.x - minimumX.x,
                   .height = maximumY.y - minimumY.y},
        .rgba = text->rgba};
  } else {
    const auto invocation = publishedInteractionLayout_->writerInvocationFor(
        control, point, eventMicros);
    if (!invocation || !queueFloatWriter(*invocation)) return {};
  }
  return result;
}

bool MusicSelectSkinSession::queuePointerDrag(UiLogicalPoint point,
                                              long long eventMicros) {
  if (!publishedInteractionLayout_) return false;
  const auto invocation = publishedInteractionLayout_->sliderWriterInvocationAt(
      point, eventMicros);
  return invocation && queueFloatWriter(*invocation);
}

bool MusicSelectSkinSession::queueStringWrite(SkinStringWriterId writer,
                                              std::string value) {
  const auto binding = std::ranges::find_if(
      model_.model.stringWriters,
      [&](const SkinStringWriterBinding &candidate) {
        return candidate.id == writer;
      });
  if (binding == model_.model.stringWriters.end()) return false;
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    queuedActions_.push_back({.kind = MusicSelectSkinActionKind::StringWriter,
                              .selector = *builtin,
                              .stringValue = std::move(value)});
  } else {
    queuedStringWriters_.push_back(
        {.writer = writer, .value = std::move(value)});
  }
  return true;
}

std::vector<MusicSelectSkinAction>
MusicSelectSkinSession::takePublishedActions() {
  return std::exchange(publishedActions_, {});
}

std::vector<SkinDiagnostic> MusicSelectSkinSession::takeLastDiagnostics() {
  return std::exchange(diagnostics_, {});
}

const SkinEntryId &MusicSelectSkinSession::entry() const noexcept {
  return entry_;
}

bool MusicSelectSkinSession::queueEvent(
    int eventId, std::span<const int> arguments,
    std::span<const int> resolutionPath) {
  if (eventId >= 1'000 && eventId <= 1'999) {
    const auto custom = customEventLastDefinitionIndexes_.find(eventId);
    if (custom == customEventLastDefinitionIndexes_.end()) return true;
    const auto &event = model_.model.customEvents[custom->second];
    if (std::ranges::find(resolutionPath, eventId) != resolutionPath.end()) {
      diagnostics_.push_back(failure(
          "skin.music_select_session.custom_event_cycle",
          "Music-select skin custom event action references itself "
          "recursively."));
      return false;
    }
    std::vector<int> nestedResolutionPath(resolutionPath.begin(),
                                          resolutionPath.end());
    nestedResolutionPath.push_back(eventId);
    if (!queueEventBinding(event.action, arguments, nestedResolutionPath)) {
      return false;
    }
    customEventLastExecutionMicros_.insert_or_assign(eventId,
                                                     currentEventMicros_);
    return true;
  }
  return queueBuiltinEvent({.value = eventId}, arguments);
}

bool MusicSelectSkinSession::queueBuiltinEvent(
    SkinBuiltinPropertySelector selector, std::span<const int> arguments) {
  MusicSelectSkinAction action{.kind = MusicSelectSkinActionKind::Event,
                               .selector = std::move(selector)};
  action.arguments.assign(arguments.begin(), arguments.end());
  (executingFrame_ ? frameActions_ : queuedActions_)
      .push_back(std::move(action));
  return true;
}

bool MusicSelectSkinSession::queueEventBinding(
    SkinEventBindingId id, std::span<const int> arguments,
    std::span<const int> resolutionPath) {
  const auto binding = std::ranges::find_if(
      model_.model.events,
      [id](const SkinEventBinding &candidate) { return candidate.id == id; });
  if (binding == model_.model.events.end()) {
    diagnostics_.push_back(failure(
        "skin.music_select_session.event_binding_missing",
        "Music-select event binding is absent."));
    return false;
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    if (const auto *numeric = std::get_if<int>(&builtin->value)) {
      return queueEvent(*numeric, arguments, resolutionPath);
    }
    return queueBuiltinEvent(*builtin, arguments);
  }
  QueuedEventBinding queued{.binding = binding->id,
                            .argumentCount = arguments.size()};
  std::ranges::copy(arguments, queued.arguments.begin());
  queuedEvents_.push_back(std::move(queued));
  return true;
}

bool MusicSelectSkinSession::queueFloatWriter(
    const SkinWriterInvocation &invocation) {
  const auto binding = std::ranges::find_if(
      model_.model.floatWriters,
      [&](const SkinFloatWriterBinding &candidate) {
        return candidate.id == invocation.writer;
      });
  if (binding == model_.model.floatWriters.end()) {
    diagnostics_.push_back(failure(
        "skin.music_select_session.float_writer_missing",
        "Music-select float writer binding is absent."));
    return false;
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    queuedActions_.push_back({.kind = MusicSelectSkinActionKind::FloatWriter,
                              .selector = *builtin,
                              .floatValue = invocation.normalizedValue});
  } else {
    queuedFloatWriters_.push_back(
        {.writer = invocation.writer,
         .value = invocation.normalizedValue});
  }
  return true;
}

bool MusicSelectSkinSession::executeQueuedCallbacks(
    MusicSelectSkinStateBridge &) {
  while (!queuedFloatWriters_.empty() || !queuedStringWriters_.empty() ||
         !queuedEvents_.empty()) {
    auto floats = std::exchange(queuedFloatWriters_, {});
    for (const auto &queued : floats) {
      const auto binding = std::ranges::find_if(
          model_.model.floatWriters,
          [&](const SkinFloatWriterBinding &candidate) {
            return candidate.id == queued.writer;
          });
      if (binding == model_.model.floatWriters.end() ||
          !std::holds_alternative<LuaCallbackId>(binding->source)) {
        diagnostics_.push_back(failure(
            "skin.music_select_session.float_writer_callback_missing",
            "Music-select float writer callback is absent."));
        return false;
      }
      const std::array<LuaScalar, 1> arguments{LuaScalar{queued.value}};
      const auto callback = runtime_->invoke(
          std::get<LuaCallbackId>(binding->source), arguments);
      if (callback.failure) {
        if (!safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit)) {
          continue;
        }
        diagnostics_.push_back(std::move(*callback.failure));
        return false;
      }
    }

    auto strings = std::exchange(queuedStringWriters_, {});
    for (const auto &queued : strings) {
      const auto binding = std::ranges::find_if(
          model_.model.stringWriters,
          [&](const SkinStringWriterBinding &candidate) {
            return candidate.id == queued.writer;
          });
      if (binding == model_.model.stringWriters.end() ||
          !std::holds_alternative<LuaCallbackId>(binding->source)) {
        diagnostics_.push_back(failure(
            "skin.music_select_session.string_writer_callback_missing",
            "Music-select string writer callback is absent."));
        return false;
      }
      const std::array<LuaScalar, 1> arguments{LuaScalar{queued.value}};
      const auto callback = runtime_->invoke(
          std::get<LuaCallbackId>(binding->source), arguments);
      if (callback.failure) {
        if (!safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit)) {
          continue;
        }
        diagnostics_.push_back(std::move(*callback.failure));
        return false;
      }
    }

    auto events = std::exchange(queuedEvents_, {});
    for (const auto &queued : events) {
      const auto binding = std::ranges::find_if(
          model_.model.events, [&](const SkinEventBinding &candidate) {
            return candidate.id == queued.binding;
          });
      if (binding == model_.model.events.end() ||
          !std::holds_alternative<LuaCallbackId>(binding->source)) {
        diagnostics_.push_back(failure(
            "skin.music_select_session.event_callback_missing",
            "Music-select event callback is absent."));
        return false;
      }
      const auto eventCallback = std::get<LuaCallbackId>(binding->source);
      const auto parameterCount = runtime_->callbackParameterCount(eventCallback);
      const std::size_t suppliedArgumentCount =
          parameterCount && *parameterCount >= 0
              ? static_cast<std::size_t>(std::min(*parameterCount, 2))
              : queued.argumentCount;
      std::array<LuaScalar, 2> arguments{LuaScalar{std::int64_t{0}},
                                          LuaScalar{std::int64_t{0}}};
      for (std::size_t index = 0;
           index < std::min(queued.argumentCount, suppliedArgumentCount);
           ++index) {
        arguments[index] =
            static_cast<std::int64_t>(queued.arguments[index]);
      }
      const auto callback = runtime_->invoke(
          eventCallback, std::span<const LuaScalar>{arguments.data(),
                                                    suppliedArgumentCount});
      if (callback.failure) {
        if (!safetyPolicy_.enforces(SkinSafetyGuard::LuaDecoderLimit)) {
          continue;
        }
        diagnostics_.push_back(std::move(*callback.failure));
        return false;
      }
    }
  }
  return true;
}

LuaSkinEventExecutionResult MusicSelectSkinSession::executeHostEvent(
    void *opaque, int eventId, std::span<const int> arguments) noexcept {
  if (opaque == nullptr) {
    return {.failure = SkinDiagnostic{
                .code = "skin_lua_event_executor_unavailable",
                .message =
                    "Skin event executor has no active music-select session."}};
  }
  try {
    auto &session = *static_cast<MusicSelectSkinSession *>(opaque);
    if (session.queueEvent(eventId, arguments)) return {};
  } catch (...) {
  }
  return {.failure = SkinDiagnostic{
              .code = "skin_lua_event_execution_failed",
              .message = "Music-select skin event could not be executed."}};
}

} // namespace skin
