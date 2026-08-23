#include "ResultSkinSession.h"

#include "BgfxSkinTextureDevice.h"
#include "GameplaySkinSourceFormat.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinHostModules.h"
#include "PlaySkinViewport.h"
#include "../SkinTargetTraits.h"
#include "../../rendering/SkinQuadBatchRenderer.h"
#include "../../rendering/common.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace skin {
namespace {

class DeterministicResultGaugeRandomSource final
    : public ISkinGaugeRandomSource {
public:
  explicit DeterministicResultGaugeRandomSource(std::uint64_t seed) noexcept
      : seed_(seed) {}

  std::optional<std::uint32_t>
  next(SkinObjectId object, std::uint64_t animationEpoch,
       std::uint32_t exclusiveUpperBound) override {
    if (exclusiveUpperBound == 0) return std::nullopt;
    std::uint64_t value = seed_ ^ (static_cast<std::uint64_t>(object) << 32U) ^
                          animationEpoch;
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return static_cast<std::uint32_t>(value % exclusiveUpperBound);
  }

private:
  std::uint64_t seed_ = 0;
};

std::uint64_t resultGaugeRandomSeed(const SkinEntryId &entry) noexcept {
  std::uint64_t seed = 1469598103934665603ULL;
  const auto append = [&seed](std::string_view text) {
    for (const unsigned char byte : text) {
      seed ^= byte;
      seed *= 1099511628211ULL;
    }
  };
  append(entry.package.directoryName);
  append(entry.package.collisionKey);
  append(entry.packageRelativePath);
  return seed;
}

SkinDiagnostic failure(std::string code, std::string message,
                       std::string path = {}) {
  return {.code = std::move(code), .message = std::move(message),
          .virtualPath = std::move(path), .severity = DiagnosticSeverity::Error};
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

std::vector<std::string> resultRuntimeStrings(const ResultSkinData &data) {
  std::vector<std::string> strings;
  if (data.meta != nullptr) {
    appendRuntimeString(strings, data.meta->Title);
    appendRuntimeString(strings, data.meta->SubTitle);
    appendRuntimeString(strings, data.meta->Artist);
    appendRuntimeString(strings, data.meta->SubArtist);
    appendRuntimeString(strings, data.meta->Genre);
    appendRuntimeString(strings, data.meta->MD5);
    appendRuntimeString(strings, data.meta->SHA256);
    if (!data.courseTitle.empty()) {
      appendRuntimeString(strings, data.courseTitle);
    } else if (!data.meta->SubTitle.empty()) {
      appendRuntimeString(strings, data.meta->Title + " " +
                                       data.meta->SubTitle);
    }
    if (!data.meta->SubArtist.empty()) {
      appendRuntimeString(strings, data.meta->Artist + " " +
                                       data.meta->SubArtist);
    }
  }
  if (data.presentation != nullptr) {
    appendRuntimeString(strings, data.presentation->title);
    if (data.presentation->artist) {
      appendRuntimeString(strings, *data.presentation->artist);
    }
    if (data.presentation->difficulty) {
      appendRuntimeString(strings, *data.presentation->difficulty);
    }
    if (data.presentation->playtype) {
      appendRuntimeString(strings, *data.presentation->playtype);
    }
  }
  for (const auto *value : {&data.playerName, &data.tableName, &data.tableLevel,
                            &data.playModeLabel, &data.laneOrderLabel,
                            &data.difficultyLabel, &data.skinName,
                            &data.skinAuthor, &data.courseTitle}) {
    appendRuntimeString(strings, *value);
  }
  appendRuntimeString(strings,
                      data.pacemaker ? data.pacemaker->label : "");
  appendRuntimeString(strings, data.chartMd5);
  appendRuntimeString(strings, data.chartSha256);
  appendRuntimeString(strings, ASOBMASHOW_APPLICATION_VERSION);
  if (!data.tableName.empty() && !data.tableLevel.empty()) {
    appendRuntimeString(strings, data.tableName + " " + data.tableLevel);
  }
  for (const auto &title : data.courseTitles) appendRuntimeString(strings, title);
  for (const auto &entry : data.irRankingEntries) {
    appendRuntimeString(strings, entry.playerName);
  }
  return strings;
}

std::map<int, std::filesystem::path>
resultBuiltinImagePaths(const ResultSkinData &data) {
  std::map<int, std::filesystem::path> paths;
  if (data.meta == nullptr) return paths;
  const std::filesystem::path directory =
      !data.meta->BmsPath.empty() ? data.meta->BmsPath.parent_path()
                                  : data.meta->Folder;
  const auto add = [&paths, &directory](int reference,
                                        const std::filesystem::path &declared) {
    if (!declared.empty()) paths.emplace(reference, (directory / declared).lexically_normal());
  };
  add(100, data.meta->StageFile);
  add(101, data.meta->BackBmp);
  add(102, data.meta->Banner);
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

ResultSkinSession::ResultSkinSession(
    SkinRevisionLease revision, SkinEntryId entry,
    ValidatedBeatorajaSkinModel model, BeatorajaSkinConfiguration configuration,
    std::unique_ptr<LuaSkinRuntime> runtime,
    std::unique_ptr<SkinResourceCatalog> resources,
    std::unique_ptr<SkinMovieCatalog> movies, SkinProfileId profileId,
    SkinStorageRoots storageRoots,
    SkinResourcePreparationService &resourcePreparation,
    std::shared_ptr<SkinTextureDevice> textureDevice,
    SkinBuiltinImageReader builtinImageReader,
    std::shared_ptr<SkinLiveResourceCounters> liveResourceCounters,
    SkinSafetyPolicy safetyPolicy,
    ViewportSettings viewportSettings,
    std::vector<std::string> preparedRuntimeStrings)
    : revision_(std::move(revision)), entry_(std::move(entry)),
      model_(std::move(model)), configuration_(std::move(configuration)),
      runtime_(std::move(runtime)), resources_(std::move(resources)),
      movies_(std::move(movies)), profileId_(std::move(profileId)),
      storageRoots_(std::move(storageRoots)),
      resourcePreparation_(&resourcePreparation),
      textureDevice_(std::move(textureDevice)),
      builtinImageReader_(std::move(builtinImageReader)),
      liveResourceCounters_(std::move(liveResourceCounters)),
      gaugeRandom_(std::make_unique<DeterministicResultGaugeRandomSource>(
          resultGaugeRandomSeed(entry_))),
      safetyPolicy_(safetyPolicy), viewportSettings_(viewportSettings),
      preparedRuntimeStrings_(std::move(preparedRuntimeStrings)),
      quadRenderer_(std::make_unique<rendering::SkinQuadBatchRenderer>()) {}

ResultSkinSession::~ResultSkinSession() = default;

ResultSkinSessionCreateResult ResultSkinSession::create(
    ValidatedSkinActivation activation, ResultSkinSessionContext context) {
  ResultSkinSessionCreateResult result;
  if (!context.liveResourceCounters || !context.textureDevice ||
      activation.entry.package != activation.revision.revision().package) {
    result.diagnostics.push_back(failure(
        "skin.result_session.context_invalid",
        "Result skin session services or activation identity are invalid."));
    return result;
  }
  const auto format = gameplaySkinSourceFormatForPath(
      activation.entry.packageRelativePath);
  if (!format) {
    result.diagnostics.push_back(failure("skin.document.source_unsupported",
        "Result skin entry has an unsupported source extension.",
        activation.entry.packageRelativePath));
    return result;
  }
  try {
    const auto revision = activation.revision.readView();
    auto resourceFiles = LuaSkinFileSystem::create(
        {.revision = revision, .entry = activation.entry,
         .storageRoots = context.storageRoots, .profileId = context.profileId,
         .safetyPolicy = context.safetyPolicy});
    if (!resourceFiles.fileSystem) {
      result.diagnostics.push_back(failure("skin.document.filesystem_create_failed",
          resourceFiles.failure ? resourceFiles.failure->message
                                : "Result skin filesystem could not be created."));
      return result;
    }
    std::unique_ptr<LuaSkinFileSystem> luaFiles;
    if (*format == GameplaySkinSourceFormat::Lua) {
      auto created = LuaSkinFileSystem::create(
          {.revision = revision, .entry = activation.entry,
           .storageRoots = context.storageRoots, .profileId = context.profileId,
           .allowDataWrites = true, .safetyPolicy = context.safetyPolicy});
      if (!created.fileSystem) {
        result.diagnostics.push_back(failure("skin_lua_filesystem_create_failed",
            created.failure ? created.failure->message
                            : "Result Lua filesystem could not be created."));
        return result;
      }
      luaFiles = std::move(created.fileSystem);
    }
    GameplaySkinDocumentLoader loader;
    auto loaded = loader.load(
        {.sourceFormat = *format, .entry = activation.entry,
         .documentFileSystem = *resourceFiles.fileSystem,
         .luaFileSystem = std::move(luaFiles),
         .luaAudioBackend = std::move(context.audioBackend),
         .desiredSettings = &activation.reconciledSettings,
         .expectedConfigurationDigest = activation.configurationDigest,
         .luaPurpose = LuaRuntimePurpose::Gameplay,
         .loadConfiguredLua = [&context](
                                  LuaSkinRuntime &runtime,
                                  const BeatorajaSkinConfiguration &configuration,
                                  std::vector<SkinDiagnostic> &) {
           ResultSkinStateBridge bridge(context.initialData, 1, 0,
                                        &configuration);
           LuaFrameStateBinding frameState(&runtime, &bridge);
           return runtime.loadConfigured(configuration);
         },
         .safetyPolicy = context.safetyPolicy, .stop = context.stop});
    result.diagnostics = std::move(loaded.diagnostics);
    if (loaded.cancelled || !loaded.document || hasErrors(result.diagnostics)) {
      return result;
    }
    auto document = std::move(*loaded.document);
    const int configuredType = document.model.model.header.type;
    const auto type = skinTargetTraitForType(configuredType);
    if (!type || (type->kind != SkinTargetKind::Result &&
                  type->kind != SkinTargetKind::CourseResult) ||
        (context.expectedSkinType != 0 &&
         configuredType != context.expectedSkinType)) {
      result.diagnostics.push_back(failure("skin.result_session.type_mismatch",
          "Configured document does not match the selected Beatoraja result target.",
          activation.entry.packageRelativePath));
      return result;
    }
    ResultSkinData resourceData = context.initialData;
    resourceData.skinName = document.model.model.header.name;
    resourceData.skinAuthor = document.model.model.header.author;
    auto runtimeStrings = resultRuntimeStrings(resourceData);
    auto planned = context.resourcePreparation.decodeAndPlan(
        {.revision = activation.revision.clone(), .entry = activation.entry,
         .fileSystem = *resourceFiles.fileSystem, .model = document.model,
         .configuration = document.configuration,
         .requiredRuntimeStrings = runtimeStrings,
         .builtinImagePaths = resultBuiltinImagePaths(context.initialData),
         .builtinImageReader = context.builtinImageReader,
         .safetyPolicy = context.safetyPolicy,
         .stop = context.stop});
    result.diagnostics.insert(result.diagnostics.end(),
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
    auto uploaded = SkinResourceCatalog::upload(std::move(*planned.plan),
                                                context.textureDevice,
                                                context.liveResourceCounters);
    result.diagnostics.insert(result.diagnostics.end(),
                              std::make_move_iterator(uploaded.diagnostics.begin()),
                              std::make_move_iterator(uploaded.diagnostics.end()));
    if (!uploaded.catalog || hasErrors(result.diagnostics)) return result;
    if (document.luaRuntime && !document.luaRuntime->enterRenderPhase().ok) {
      result.diagnostics.push_back(failure("skin_lua_render_phase_failed",
          "Result Lua skin could not enter its render phase."));
      return result;
    }
    uploaded.catalog->enterRenderPhase();
    result.session = std::unique_ptr<ResultSkinSession>(new ResultSkinSession(
        std::move(activation.revision), std::move(activation.entry),
        std::move(document.model), std::move(document.configuration),
        std::move(document.luaRuntime), std::move(uploaded.catalog),
        std::move(preparedMovies.catalog), std::move(context.profileId),
        std::move(context.storageRoots), context.resourcePreparation,
        std::move(context.textureDevice), std::move(context.builtinImageReader),
        std::move(context.liveResourceCounters),
        context.safetyPolicy, activation.reconciledSettings.viewport,
        std::move(runtimeStrings)));
  } catch (...) {
    result.session.reset();
    result.diagnostics.push_back(failure("skin.result_session.create_failed",
        "Result skin session creation failed."));
  }
  return result;
}

bool ResultSkinSession::render(RenderContext &renderContext,
                               const ResultSkinData &data,
                               std::uint64_t frameSerial,
                               std::int64_t elapsedMillis) {
  lastDiagnostics_.insert(lastDiagnostics_.end(),
                          std::make_move_iterator(pendingDiagnostics_.begin()),
                          std::make_move_iterator(pendingDiagnostics_.end()));
  pendingDiagnostics_.clear();
  currentEventMicros_ = std::max<std::int64_t>(0, elapsedMillis) * 1000;
  if (!resources_ || !movies_ || frameSerial == 0) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.frame_invalid",
        "Result skin frame has no prepared resources or valid frame serial."));
    return false;
  }
  ResultSkinData skinData = data;
  skinData.skinName = model_.model.header.name;
  skinData.skinAuthor = model_.model.header.author;
  ResultSkinStateBridge bridge(std::move(skinData), frameSerial, elapsedMillis,
                               &configuration_, &model_.model);
  LuaFrameStateBinding frameState(
      runtime_.get(), &bridge,
      {.context = this, .execute = &ResultSkinSession::executeHostEvent});
  const auto &header = model_.model.header;
  const auto viewport = evaluatePlaySkinViewport(
      {.width = static_cast<double>(header.width),
       .height = static_cast<double>(header.height)},
      {.x = 0.0, .y = 0.0, .width = static_cast<double>(rendering::window_width),
       .height = static_cast<double>(rendering::window_height)}, viewportSettings_);
  if (!viewport.valid) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.viewport_invalid",
        "Result skin viewport could not be evaluated from its selected settings."));
    return false;
  }
  if (runtime_ != nullptr) {
    const auto begun = runtime_->beginFrame(frameSerial);
    if (!begun.ok) {
      lastDiagnostics_.push_back(begun.failure.value_or(failure(
          "skin.result_session.frame_begin_failed",
          "Result Lua runtime rejected the render frame.")));
      return false;
    }
  }
  for (const auto &timer : model_.model.customTimers) {
      std::int64_t value = std::numeric_limits<std::int64_t>::min();
      if (timer.timer) {
        const auto binding = std::ranges::find_if(
            model_.model.timerProperties, [&](const SkinTimerPropertyBinding &candidate) {
              return candidate.id == *timer.timer;
            });
        if (binding == model_.model.timerProperties.end()) {
          lastDiagnostics_.push_back(failure("skin.result_session.custom_timer_missing",
              "Result custom timer binding is absent."));
          return false;
        }
        if (const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(
                &binding->source)) {
          value = bridge.timerProperty(*builtin);
        } else if (runtime_ != nullptr) {
          const auto callback = runtime_->invoke(
              std::get<LuaCallbackId>(binding->source), {});
          if (callback.failure || !callback.value ||
              !std::holds_alternative<std::int64_t>(*callback.value)) {
            lastDiagnostics_.push_back(callback.failure.value_or(failure(
                "skin.result_session.custom_timer_type",
                "Result custom timer did not return an integer timestamp.")));
            return false;
          }
          value = std::get<std::int64_t>(*callback.value);
        } else {
          lastDiagnostics_.push_back(failure(
              "skin.result_session.custom_timer_runtime_missing",
              "Result custom timer callback requires a Lua runtime."));
          return false;
        }
      }
      bridge.setCustomTimer(timer.id, value);
  }
  for (const auto &event : model_.model.customEvents) {
    if (!event.condition) continue;
    bool active = false;
    {
      const auto condition = std::ranges::find_if(
          model_.model.booleanProperties,
          [&](const SkinBooleanPropertyBinding &candidate) {
            return candidate.id == *event.condition;
          });
      if (condition == model_.model.booleanProperties.end()) {
        lastDiagnostics_.push_back(failure(
            "skin.result_session.custom_event_condition_missing",
            "Result custom event condition is absent."));
        return false;
      }
      if (const auto *builtin = std::get_if<SkinBuiltinPropertySelector>(
              &condition->source)) {
        active = bridge.booleanProperty(*builtin).value;
      } else {
        if (runtime_ == nullptr) {
          lastDiagnostics_.push_back(failure(
              "skin.result_session.custom_event_runtime_missing",
              "Result custom event callback requires a Lua runtime."));
          return false;
        }
        const auto callback = runtime_->invoke(
            std::get<LuaCallbackId>(condition->source), {});
        active = callback.value && std::holds_alternative<bool>(*callback.value) &&
                 std::get<bool>(*callback.value);
        if (callback.failure || !callback.value ||
            !std::holds_alternative<bool>(*callback.value)) {
          lastDiagnostics_.push_back(callback.failure.value_or(failure(
              "skin.result_session.custom_event_condition_type",
              "Result custom event condition did not return a boolean.")));
          return false;
        }
      }
    }
    if (!active) continue;
    const std::int64_t now = elapsedMillis * 1000;
    const auto previous = customEventLastExecutionMicros_.find(event.id);
    if (previous != customEventLastExecutionMicros_.end() &&
        now - previous->second <
            static_cast<std::int64_t>(event.minimumIntervalMillis) * 1000) {
      continue;
    }
    if (!queueEventBinding(event.action, {}, now)) {
      return false;
    }
    customEventLastExecutionMicros_.insert_or_assign(event.id, now);
  }
  auto queuedInvocations = std::exchange(queuedEventInvocations_, {});
  for (const auto &invocation : queuedInvocations) {
    if (runtime_ == nullptr) {
      lastDiagnostics_.push_back(failure(
          "skin.result_session.custom_event_runtime_missing",
          "Result custom event callback requires a Lua runtime."));
      queuedEventInvocations_.clear();
      return false;
    }
    const auto binding = std::ranges::find_if(
        model_.model.events, [&](const SkinEventBinding &candidate) {
          return candidate.id == invocation.eventBinding;
        });
    if (binding == model_.model.events.end() ||
        !std::holds_alternative<LuaCallbackId>(binding->source)) {
      lastDiagnostics_.push_back(failure(
          "skin.result_session.custom_event_binding_missing",
          "Result custom event action binding is absent."));
      queuedEventInvocations_.clear();
      return false;
    }
    std::array<LuaScalar, 2> arguments{};
    for (std::size_t index = 0; index < invocation.argumentCount; ++index) {
      arguments[index] = static_cast<std::int64_t>(invocation.arguments[index]);
    }
    const auto callback = runtime_->invoke(
        std::get<LuaCallbackId>(binding->source),
        std::span<const LuaScalar>{arguments.data(), invocation.argumentCount});
    if (callback.failure) {
      lastDiagnostics_.push_back(std::move(*callback.failure));
      queuedEventInvocations_.clear();
      return false;
    }
  }
  SkinExternalFrameOwnership ownership(frameSerial, 1);
  auto evaluated = renderer_.evaluateFrame(
      {.frameSerial = frameSerial, .sessionSerial = 1,
       .visualTimeMicros = elapsedMillis * 1000, .model = model_,
       .configuration = configuration_, .resources = *resources_, .movies = movies_.get(),
       .viewport = viewport,
       .runtime = runtime_.get(), .state = bridge, .safetyPolicy = safetyPolicy_,
       .gaugeRandomSource = gaugeRandom_.get()},
      std::move(ownership));
  if (!evaluated.submitReady) {
    lastDiagnostics_.insert(lastDiagnostics_.end(),
                            std::make_move_iterator(evaluated.diagnostics.begin()),
                            std::make_move_iterator(evaluated.diagnostics.end()));
    if (!hasErrors(lastDiagnostics_)) {
      lastDiagnostics_.push_back(failure(
          "skin.result_session.frame_evaluation_failed",
          "Result skin frame evaluation did not produce a drawable command buffer."));
    }
    return false;
  }
  if (!renderer_.submit(*evaluated.submitReady, *resources_, renderContext,
                        *quadRenderer_, movies_.get(), viewport)) {
    lastDiagnostics_.insert(lastDiagnostics_.end(),
                            std::make_move_iterator(evaluated.diagnostics.begin()),
                            std::make_move_iterator(evaluated.diagnostics.end()));
    lastDiagnostics_.push_back(failure(
        "skin.result_session.frame_submission_failed",
        "Result skin frame submission could not be prepared."));
    return false;
  }
  lastDiagnostics_.insert(lastDiagnostics_.end(),
                          std::make_move_iterator(evaluated.diagnostics.begin()),
                          std::make_move_iterator(evaluated.diagnostics.end()));
  publishedInteractionLayout_ = std::move(evaluated.interactionLayout);
  return true;
}

std::vector<SkinDiagnostic> ResultSkinSession::takeLastDiagnostics() {
  return std::exchange(lastDiagnostics_, {});
}

std::vector<int> ResultSkinSession::takeQueuedBuiltinEventIds() {
  return std::exchange(queuedBuiltinEventIds_, {});
}

bool ResultSkinSession::queueEvent(int eventId, std::span<const int> arguments,
                                   long long eventMicros) {
  if (arguments.size() > 2) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.event_argument_count",
        "Result skin events accept no more than two arguments."));
    return false;
  }
  if (const auto custom = std::ranges::find_if(
          model_.model.customEvents, [eventId](const SkinCustomEvent &event) {
            return event.id == eventId;
          }); custom != model_.model.customEvents.end()) {
    if (!queueEventBinding(custom->action, arguments, eventMicros)) {
      return false;
    }
    customEventLastExecutionMicros_.insert_or_assign(custom->id, eventMicros);
    return true;
  }

  // open_ir is the only EventFactory action with an AsoBMaShow result
  // transition. Replay-save and selector-only events have no equivalent here;
  // preserve Beatoraja's successful no-op behavior and emit one contextual
  // warning instead of rejecting the skin.
  if (eventId != 210) {
    reportUnsupportedEvent(eventId);
    return true;
  }
  if (queuedEventInvocations_.size() + queuedBuiltinEventIds_.size() >= 64) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.event_queue_full",
        "Result skin event queue reached its frame limit."));
    return false;
  }
  queuedBuiltinEventIds_.push_back(eventId);
  return true;
}

bool ResultSkinSession::queueEventBinding(SkinEventBindingId id,
                                          std::span<const int> arguments,
                                          long long eventMicros) {
  const auto binding = std::ranges::find_if(
      model_.model.events, [id](const SkinEventBinding &candidate) {
        return candidate.id == id;
      });
  if (binding == model_.model.events.end()) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.custom_event_binding_missing",
        "Result custom event action binding is absent."));
    return false;
  }
  if (const auto *builtin =
          std::get_if<SkinBuiltinPropertySelector>(&binding->source)) {
    const auto *eventId = std::get_if<int>(&builtin->value);
    if (eventId == nullptr) {
      lastDiagnostics_.push_back(failure(
          "skin.result_session.custom_event_binding_invalid",
          "Result custom event action is not a numeric event ID."));
      return false;
    }
    return queueEvent(*eventId, arguments, eventMicros);
  }
  if (runtime_ == nullptr) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.custom_event_runtime_missing",
        "Result custom event callback requires a Lua runtime."));
    return false;
  }
  if (queuedEventInvocations_.size() + queuedBuiltinEventIds_.size() >= 64) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.event_queue_full",
        "Result skin event queue reached its frame limit."));
    return false;
  }
  QueuedEventInvocation invocation{.eventBinding = binding->id,
                                    .argumentCount = arguments.size(),
                                    .eventMicros = eventMicros};
  std::ranges::copy(arguments, invocation.arguments.begin());
  queuedEventInvocations_.push_back(std::move(invocation));
  return true;
}

void ResultSkinSession::reportUnsupportedEvent(int eventId) {
  if (!reportedUnsupportedEventIds_.insert(eventId).second) return;
  pendingDiagnostics_.push_back(
      {.code = "skin.result_session.event_unavailable",
       .message = "Result skin event " + std::to_string(eventId) +
                  " has no equivalent in this result context.",
       .severity = DiagnosticSeverity::Warning});
}

LuaSkinEventExecutionResult ResultSkinSession::executeHostEvent(
    void *opaque, int eventId, std::span<const int> arguments) noexcept {
  if (opaque == nullptr) {
    return {.failure = SkinDiagnostic{
                .code = "skin_lua_event_executor_unavailable",
                .message = "Skin event executor has no active result session."}};
  }
  auto &session = *static_cast<ResultSkinSession *>(opaque);
  try {
    if (session.queueEvent(eventId, arguments, session.currentEventMicros_)) {
      return {};
    }
    return {.failure = SkinDiagnostic{
                .code = "skin_lua_event_execution_failed",
                .message = "Result skin event could not be queued."}};
  } catch (...) {
    return {.failure = SkinDiagnostic{
                .code = "skin_lua_event_execution_failed",
                .message = "Result skin event could not be queued."}};
  }
}

bool ResultSkinSession::requiresRuntimeStringRefresh(
    const ResultSkinData &data) const {
  const auto runtimeStrings = resultRuntimeStrings(data);
  return std::ranges::any_of(runtimeStrings, [this](const std::string &value) {
    return std::ranges::find(preparedRuntimeStrings_, value) ==
           preparedRuntimeStrings_.end();
  });
}

bool ResultSkinSession::refreshRuntimeStrings(const ResultSkinData &data) {
  lastDiagnostics_.clear();
  if (!requiresRuntimeStringRefresh(data)) return true;
  if (resourcePreparation_ == nullptr || !textureDevice_ ||
      !liveResourceCounters_) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.refresh_context_invalid",
        "Result skin resources cannot refresh without their session services."));
    return false;
  }
  try {
    auto resourceFiles = LuaSkinFileSystem::create(
        {.revision = revision_.readView(), .entry = entry_,
         .storageRoots = storageRoots_, .profileId = profileId_,
         .safetyPolicy = safetyPolicy_});
    if (!resourceFiles.fileSystem) {
      lastDiagnostics_.push_back(failure(
          "skin.result_session.refresh_filesystem_create_failed",
          resourceFiles.failure ? resourceFiles.failure->message
                                : "Result skin refresh filesystem could not be created."));
      return false;
    }
    ResultSkinData resourceData = data;
    resourceData.skinName = model_.model.header.name;
    resourceData.skinAuthor = model_.model.header.author;
    auto runtimeStrings = resultRuntimeStrings(resourceData);
    auto planned = resourcePreparation_->decodeAndPlan(
        {.revision = revision_.clone(), .entry = entry_,
         .fileSystem = *resourceFiles.fileSystem, .model = model_,
         .configuration = configuration_, .requiredRuntimeStrings = runtimeStrings,
         .builtinImagePaths = resultBuiltinImagePaths(data),
         .builtinImageReader = builtinImageReader_, .safetyPolicy = safetyPolicy_});
    lastDiagnostics_.insert(lastDiagnostics_.end(),
                            std::make_move_iterator(planned.diagnostics.begin()),
                            std::make_move_iterator(planned.diagnostics.end()));
    if (planned.cancelled || !planned.plan || hasErrors(lastDiagnostics_)) {
      return false;
    }
    auto uploaded = SkinResourceCatalog::upload(
        std::move(*planned.plan), textureDevice_, liveResourceCounters_);
    lastDiagnostics_.insert(lastDiagnostics_.end(),
                            std::make_move_iterator(uploaded.diagnostics.begin()),
                            std::make_move_iterator(uploaded.diagnostics.end()));
    if (!uploaded.catalog || hasErrors(lastDiagnostics_)) return false;
    uploaded.catalog->enterRenderPhase();
    resources_ = std::move(uploaded.catalog);
    preparedRuntimeStrings_ = std::move(runtimeStrings);
    return true;
  } catch (...) {
    lastDiagnostics_.push_back(failure(
        "skin.result_session.refresh_failed",
        "Result skin text resources could not be refreshed."));
    return false;
  }
}

bool ResultSkinSession::queuePointerDown(UiLogicalPoint point,
                                         long long eventMicros) {
  if (!publishedInteractionLayout_ ||
      queuedEventInvocations_.size() + queuedBuiltinEventIds_.size() >= 64) {
    return false;
  }
  const PresentationUiHit hit = publishedInteractionLayout_->hitTestUiControl(point);
  if (hit.kind != PresentationUiControlKind::Image) return false;
  const auto invocation = publishedInteractionLayout_->eventInvocationFor(
      hit, point, eventMicros);
  if (!invocation) return false;
  const auto binding = std::ranges::find_if(
      model_.model.events, [&](const SkinEventBinding &candidate) {
        return candidate.id.value == invocation->eventBinding;
      });
  if (binding == model_.model.events.end()) {
    return false;
  }
  return queueEventBinding(binding->id, {&invocation->argument, 1},
                           invocation->eventMicros);
}

const SkinEntryId &ResultSkinSession::entry() const noexcept { return entry_; }

} // namespace skin
