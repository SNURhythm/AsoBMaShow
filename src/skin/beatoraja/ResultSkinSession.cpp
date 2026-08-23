#include "ResultSkinSession.h"

#include "BgfxSkinTextureDevice.h"
#include "GameplaySkinSourceFormat.h"
#include "LuaSkinFileSystem.h"
#include "PlaySkinViewport.h"
#include "../SkinTargetTraits.h"
#include "../../rendering/SkinQuadBatchRenderer.h"
#include "../../rendering/common.h"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace skin {
namespace {

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
                            &data.skinAuthor}) {
    appendRuntimeString(strings, *value);
  }
  for (const auto &title : data.courseTitles) appendRuntimeString(strings, title);
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
  LuaFrameStateBinding(LuaSkinRuntime *runtime, ISkinFrameState *state)
      : runtime_(runtime) {
    if (runtime_ != nullptr) runtime_->setFrameState(state);
  }
  ~LuaFrameStateBinding() {
    if (runtime_ != nullptr) runtime_->setFrameState(nullptr);
  }

private:
  LuaSkinRuntime *runtime_ = nullptr;
};

} // namespace

ResultSkinSession::ResultSkinSession(
    SkinRevisionLease revision, SkinEntryId entry,
    ValidatedBeatorajaSkinModel model, BeatorajaSkinConfiguration configuration,
    std::unique_ptr<LuaSkinRuntime> runtime,
    std::unique_ptr<SkinResourceCatalog> resources, SkinSafetyPolicy safetyPolicy,
    ViewportSettings viewportSettings)
    : revision_(std::move(revision)), entry_(std::move(entry)),
      model_(std::move(model)), configuration_(std::move(configuration)),
      runtime_(std::move(runtime)), resources_(std::move(resources)),
      safetyPolicy_(safetyPolicy), viewportSettings_(viewportSettings),
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
    const auto runtimeStrings = resultRuntimeStrings(resourceData);
    auto planned = context.resourcePreparation.decodeAndPlan(
        {.revision = activation.revision.clone(), .entry = activation.entry,
         .fileSystem = *resourceFiles.fileSystem, .model = document.model,
         .configuration = document.configuration,
         .requiredRuntimeStrings = runtimeStrings,
         .builtinImagePaths = resultBuiltinImagePaths(context.initialData),
         .builtinImageReader = std::move(context.builtinImageReader),
         .safetyPolicy = context.safetyPolicy,
         .stop = context.stop});
    result.diagnostics.insert(result.diagnostics.end(),
                              std::make_move_iterator(planned.diagnostics.begin()),
                              std::make_move_iterator(planned.diagnostics.end()));
    if (planned.cancelled || !planned.plan || hasErrors(result.diagnostics)) {
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
        context.safetyPolicy, activation.reconciledSettings.viewport));
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
  if (!resources_ || frameSerial == 0) return false;
  ResultSkinData skinData = data;
  skinData.skinName = model_.model.header.name;
  skinData.skinAuthor = model_.model.header.author;
  ResultSkinStateBridge bridge(std::move(skinData), frameSerial, elapsedMillis,
                               &configuration_);
  LuaFrameStateBinding frameState(runtime_.get(), &bridge);
  const auto &header = model_.model.header;
  const auto viewport = evaluatePlaySkinViewport(
      {.width = static_cast<double>(header.width),
       .height = static_cast<double>(header.height)},
      {.x = 0.0, .y = 0.0, .width = static_cast<double>(rendering::window_width),
       .height = static_cast<double>(rendering::window_height)}, viewportSettings_);
  if (!viewport.valid) return false;
  auto evaluated = renderer_.evaluateFrame(
      {.frameSerial = frameSerial, .sessionSerial = 1,
       .visualTimeMicros = elapsedMillis * 1000, .model = model_,
       .configuration = configuration_, .resources = *resources_, .viewport = viewport,
       .runtime = runtime_.get(), .state = bridge, .safetyPolicy = safetyPolicy_});
  return evaluated.submitReady && renderer_.submit(*evaluated.submitReady,
                                                    *resources_, renderContext,
                                                    *quadRenderer_);
}

const SkinEntryId &ResultSkinSession::entry() const noexcept { return entry_; }

} // namespace skin
