#include "MusicSelectSkinSession.h"

#include "BgfxSkinTextureDevice.h"
#include "GameplaySkinSourceFormat.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinHostModules.h"
#include "MusicSelectBarRenderer.h"
#include "PlaySkinViewport.h"
#include "../SkinTargetTraits.h"
#include "../../rendering/SkinQuadBatchRenderer.h"
#include "../../rendering/common.h"

#include <algorithm>
#include <array>
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

void appendRuntimeString(std::vector<std::string> &strings,
                         std::string_view value) {
  if (!value.empty() && std::ranges::find(strings, value) == strings.end()) {
    strings.emplace_back(value);
  }
}

std::vector<std::string>
musicSelectRuntimeStrings(const MusicSelectSkinFrame &frame) {
  std::vector<std::string> strings;
  for (const auto &[_, value] : frame.properties.strings) {
    appendRuntimeString(strings, value);
  }
  for (const auto &[_, value] : frame.properties.namedStrings) {
    appendRuntimeString(strings, value);
  }
  for (const auto &bar : frame.songList.bars) {
    appendRuntimeString(strings, bar.title);
  }
  return strings;
}

std::vector<std::string>
musicSelectRuntimeAtlasStrings(const MusicSelectSkinFrame &frame) {
  constexpr std::size_t maximumBytes =
      SkinResourcePolicy::maximumRuntimeStringBytes;
  std::string corpus;
  std::set<utf8proc_int32_t> codepoints;
  bool exactCorpusFits = true;
  const auto append = [&](std::string_view value) {
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
      if (codepoint != '\r' && codepoint != '\n') {
        codepoints.insert(codepoint);
      }
    }
    if (!exactCorpusFits || value.size() >= maximumBytes - corpus.size()) {
      exactCorpusFits = false;
      return;
    }
    corpus.append(value);
    corpus.push_back('\n');
  };
  for (const auto &[_, value] : frame.properties.strings) {
    append(value);
  }
  for (const auto &[_, value] : frame.properties.namedStrings) {
    append(value);
  }
  for (const auto &bar : frame.songList.bars) {
    append(bar.title);
  }
  if (exactCorpusFits) return corpus.empty() ? std::vector<std::string>{}
                                             : std::vector{std::move(corpus)};

  std::string compact;
  compact.reserve(codepoints.size() * 2U);
  for (const auto codepoint : codepoints) {
    std::array<utf8proc_uint8_t, 4> encoded{};
    const auto size = utf8proc_encode_char(codepoint, encoded.data());
    if (size > 0) {
      compact.append(reinterpret_cast<const char *>(encoded.data()),
                     static_cast<std::size_t>(size));
    }
  }
  return compact.empty() ? std::vector<std::string>{}
                         : std::vector{std::move(compact)};
}

std::map<int, std::filesystem::path>
musicSelectBuiltinImagePaths(const MusicSelectSkinFrame &frame) {
  std::map<int, std::filesystem::path> paths;
  if (!frame.stageFile.empty()) paths.emplace(100, frame.stageFile);
  if (!frame.backBmp.empty()) paths.emplace(101, frame.backBmp);
  if (!frame.banner.empty()) paths.emplace(102, frame.banner);
  return paths;
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
    SkinSafetyPolicy safetyPolicy, ViewportSettings viewportSettings,
    std::stop_token stop, std::vector<std::string> preparedRuntimeStrings,
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
      stop_(stop), quadRenderer_(
                       std::make_unique<rendering::SkinQuadBatchRenderer>()),
      preparedRuntimeStrings_(std::move(preparedRuntimeStrings)),
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

MusicSelectSkinSession::~MusicSelectSkinSession() = default;

int MusicSelectSkinSession::inputDelayMillis() const noexcept {
  return model_.model.timing.inputMillis;
}

void MusicSelectSkinSession::suspendAudio() noexcept {
  if (runtime_) runtime_->suspendAudio();
}

void MusicSelectSkinSession::resumeAudio() noexcept {
  if (runtime_) runtime_->resumeAudio();
}

MusicSelectSkinSessionCreateResult MusicSelectSkinSession::create(
    GameplaySkinActivationRequest request,
    MusicSelectSkinSessionContext context) {
  MusicSelectSkinSessionCreateResult result;
  auto activation = std::move(request.activation);
  if (!context.liveResourceCounters || !context.textureDevice ||
      activation.entry.package != activation.revision.revision().package) {
    result.diagnostics.push_back(failure(
        "skin.music_select_session.context_invalid",
        "Music-select skin session services or activation identity are invalid."));
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
         .safetyPolicy = context.safetyPolicy});
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
         .safetyPolicy = context.safetyPolicy});
    if (!luaFiles.fileSystem) {
      result.diagnostics.push_back(failure(
          "skin_lua_filesystem_create_failed",
          luaFiles.failure
              ? luaFiles.failure->message
              : "Music-select Lua filesystem could not be created."));
      return result;
    }

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
         .loadConfiguredLua = [&context](
                                  LuaSkinRuntime &runtime,
                                  const BeatorajaSkinConfiguration &configuration,
                                  std::vector<SkinDiagnostic> &) {
           MusicSelectSkinStateBridge bridge(context.initialFrame);
           LuaFrameStateBinding frameState(&runtime, &bridge);
           return runtime.loadConfigured(configuration);
         },
         .safetyPolicy = context.safetyPolicy,
         .stop = context.stop});
    result.diagnostics = std::move(loaded.diagnostics);
    if (loaded.cancelled || !loaded.document || hasErrors(result.diagnostics)) {
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

    auto runtimeStrings = musicSelectRuntimeStrings(context.initialFrame);
    auto runtimeAtlasStrings =
        musicSelectRuntimeAtlasStrings(context.initialFrame);
    auto builtinImagePaths =
        musicSelectBuiltinImagePaths(context.initialFrame);
    auto planned = context.resourcePreparation.decodeAndPlan(
        {.revision = activation.revision.clone(),
         .entry = activation.entry,
         .fileSystem = *resourceFiles.fileSystem,
         .model = document.model,
         .configuration = document.configuration,
         .requiredRuntimeStrings = runtimeAtlasStrings,
         .builtinImagePaths = builtinImagePaths,
         .builtinImageReader = context.builtinImageReader,
         .safetyPolicy = context.safetyPolicy,
         .stop = context.stop});
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(planned.diagnostics.begin()),
        std::make_move_iterator(planned.diagnostics.end()));
    if (planned.cancelled || !planned.plan || hasErrors(result.diagnostics)) {
      return result;
    }

    auto movieDevice = std::move(context.movieDevice);
#if ASOBMASHOW_ENABLE_SKIN_MOVIE_DEVICE
    if (!movieDevice) movieDevice = createSkinMovieDevice();
#endif
    auto preparedMovies = SkinMovieCatalog::prepare(
        {.fileSystem = *resourceFiles.fileSystem,
         .model = document.model,
         .configuration = document.configuration,
         .device = std::move(movieDevice),
         .safetyPolicy = context.safetyPolicy,
         .liveResourceCounters = context.liveResourceCounters,
         .stop = context.stop,
         .sessionDecodedBytes = planned.plan->decodedBytes});
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(preparedMovies.diagnostics.begin()),
        std::make_move_iterator(preparedMovies.diagnostics.end()));
    if (preparedMovies.cancelled || !preparedMovies.catalog ||
        hasErrors(result.diagnostics)) {
      return result;
    }

    auto uploaded = SkinResourceCatalog::upload(
        std::move(*planned.plan), context.textureDevice,
        context.liveResourceCounters);
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(uploaded.diagnostics.begin()),
        std::make_move_iterator(uploaded.diagnostics.end()));
    if (!uploaded.catalog || hasErrors(result.diagnostics)) return result;

    if (!document.luaRuntime) {
      result.diagnostics.push_back(failure(
          "skin_lua_runtime_missing",
          "Music-select Lua skin has no configured runtime."));
      return result;
    }
    const auto entered = document.luaRuntime->enterRenderPhase();
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
            std::move(document.model), std::move(document.configuration),
            std::move(document.luaRuntime), std::move(uploaded.catalog),
            std::move(preparedMovies.catalog), std::move(context.storageRoots),
            context.resourcePreparation, std::move(context.textureDevice),
            std::move(context.builtinImageReader),
            std::move(context.liveResourceCounters), context.safetyPolicy,
            request.viewport, context.stop, std::move(runtimeStrings),
            std::move(builtinImagePaths)));
  } catch (...) {
    result.session.reset();
    result.diagnostics.push_back(failure(
        "skin.music_select_session.create_failed",
        "Music-select skin session creation failed."));
  }
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
  if (requiresResourceRefresh(frame) && !refreshResources(frame)) {
    return false;
  }

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

  MusicSelectSkinStateBridge bridge(frame, customTimerValues_,
                                    activeCustomTimerIds_);
  LuaFrameStateBinding frameState(
      runtime_.get(), &bridge,
      {.context = this,
       .execute = &MusicSelectSkinSession::executeHostEvent});
  const auto &header = model_.model.header;
  const auto viewport = evaluatePlaySkinViewport(
      {.width = static_cast<double>(header.width),
       .height = static_cast<double>(header.height)},
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
            !std::holds_alternative<std::int64_t>(*callback.value)) {
          diagnostics_.push_back(callback.failure.value_or(failure(
              "skin.music_select_session.custom_timer_type",
              "Music-select custom timer did not return an integer timestamp.")));
          return finishFrame(false);
        }
        value = std::get<std::int64_t>(*callback.value);
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
          !std::holds_alternative<bool>(*callback.value)) {
        diagnostics_.push_back(callback.failure.value_or(failure(
            "skin.music_select_session.custom_event_condition_type",
            "Music-select custom event condition did not return a boolean.")));
        return finishFrame(false);
      }
      active = std::get<bool>(*callback.value);
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
       .musicSelectSongList = &frame.songList},
      std::move(ownership));
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

bool MusicSelectSkinSession::requiresResourceRefresh(
    const MusicSelectSkinFrame &frame) const {
  const auto strings = musicSelectRuntimeStrings(frame);
  return musicSelectBuiltinImagePaths(frame) != preparedBuiltinImagePaths_ ||
         std::ranges::any_of(strings, [this](const std::string &value) {
           return std::ranges::find(preparedRuntimeStrings_, value) ==
                  preparedRuntimeStrings_.end();
         });
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
    auto runtimeStrings = musicSelectRuntimeStrings(frame);
    auto runtimeAtlasStrings = musicSelectRuntimeAtlasStrings(frame);
    auto builtinImagePaths = musicSelectBuiltinImagePaths(frame);
    auto planned = resourcePreparation_->decodeAndPlan(
        {.revision = revision_.clone(),
         .entry = entry_,
         .fileSystem = *resourceFiles.fileSystem,
         .model = model_,
         .configuration = configuration_,
         .requiredRuntimeStrings = runtimeAtlasStrings,
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
    preparedRuntimeStrings_ = std::move(runtimeStrings);
    preparedBuiltinImagePaths_ = std::move(builtinImagePaths);
    return true;
  } catch (...) {
    diagnostics_.push_back(failure(
        "skin.music_select_session.refresh_failed",
        "Music-select skin resources could not be refreshed."));
    return false;
  }
}

MusicSelectSkinPointerResult MusicSelectSkinSession::queuePointerDown(
    UiLogicalPoint point, int button, long long eventMicros) {
  MusicSelectSkinPointerResult result;
  if (!publishedInteractionLayout_) return result;
  const auto bar = publishedInteractionLayout_->musicSelectBarAt(point);
  const auto control = publishedInteractionLayout_->hitTestUiControl(point);
  if (bar &&
      (control.kind == PresentationUiControlKind::None ||
       bar->authoredOrdinal >= control.authoredOrdinal)) {
    result.consumed = true;
    result.selectIndex = button == 0
                             ? std::optional<std::size_t>(bar->barIndex)
                             : std::nullopt;
    result.closeDirectory = button != 0;
    return result;
  }
  if (control.kind == PresentationUiControlKind::None) return result;
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
    result.focusedStringWriter = text->writer;
  } else {
    const auto invocation = publishedInteractionLayout_->writerInvocationFor(
        control, point, eventMicros);
    if (!invocation || !queueFloatWriter(*invocation)) return {};
    result.capturedControl = control;
  }
  return result;
}

bool MusicSelectSkinSession::queuePointerMove(
    const PresentationUiHit &hit, UiLogicalPoint point,
    long long eventMicros) {
  if (!publishedInteractionLayout_) return false;
  const auto invocation = publishedInteractionLayout_->writerInvocationFor(
      hit, point, eventMicros);
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
      std::array<LuaScalar, 2> arguments{};
      for (std::size_t index = 0; index < queued.argumentCount; ++index) {
        arguments[index] =
            static_cast<std::int64_t>(queued.arguments[index]);
      }
      const auto callback = runtime_->invoke(
          std::get<LuaCallbackId>(binding->source),
          std::span<const LuaScalar>{arguments.data(), queued.argumentCount});
      if (callback.failure) {
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
