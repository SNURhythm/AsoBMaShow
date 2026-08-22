#include "ResultSkinSession.h"

#include "BgfxSkinTextureDevice.h"
#include "GameplaySkinSourceFormat.h"
#include "LuaSkinFileSystem.h"
#include "PlaySkinViewport.h"
#include "../SkinTargetTraits.h"
#include "../../rendering/SkinQuadBatchRenderer.h"
#include "../../rendering/common.h"

#include <algorithm>

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

} // namespace

ResultSkinSession::ResultSkinSession(
    SkinRevisionLease revision, SkinEntryId entry,
    ValidatedBeatorajaSkinModel model, BeatorajaSkinConfiguration configuration,
    std::unique_ptr<LuaSkinRuntime> runtime,
    std::unique_ptr<SkinResourceCatalog> resources, SkinSafetyPolicy safetyPolicy)
    : revision_(std::move(revision)), entry_(std::move(entry)),
      model_(std::move(model)), configuration_(std::move(configuration)),
      runtime_(std::move(runtime)), resources_(std::move(resources)),
      safetyPolicy_(safetyPolicy),
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
         .desiredSettings = &activation.reconciledSettings,
         .expectedConfigurationDigest = activation.configurationDigest,
         .luaPurpose = LuaRuntimePurpose::Gameplay,
         .loadConfiguredLua = [](LuaSkinRuntime &runtime,
                                  const BeatorajaSkinConfiguration &configuration,
                                  std::vector<SkinDiagnostic> &) {
           return runtime.loadConfigured(configuration);
         },
         .safetyPolicy = context.safetyPolicy, .stop = context.stop});
    result.diagnostics = std::move(loaded.diagnostics);
    if (loaded.cancelled || !loaded.document || hasErrors(result.diagnostics)) {
      return result;
    }
    auto document = std::move(*loaded.document);
    const auto type = skinTargetTraitForType(document.header.type);
    if (!type || (type->kind != SkinTargetKind::Result &&
                  type->kind != SkinTargetKind::CourseResult)) {
      result.diagnostics.push_back(failure("skin.result_session.type_mismatch",
          "Loaded document is not a Beatoraja result skin.",
          activation.entry.packageRelativePath));
      return result;
    }
    auto planned = context.resourcePreparation.decodeAndPlan(
        {.revision = activation.revision.clone(), .entry = activation.entry,
         .fileSystem = *resourceFiles.fileSystem, .model = document.model,
         .configuration = document.configuration, .requiredRuntimeStrings = {},
         .builtinImagePaths = {}, .safetyPolicy = context.safetyPolicy,
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
        context.safetyPolicy));
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
  ResultSkinStateBridge bridge(data, frameSerial, elapsedMillis);
  const auto &header = model_.model.header;
  const auto viewport = evaluatePlaySkinViewport(
      {.width = static_cast<double>(header.width),
       .height = static_cast<double>(header.height)},
      {.x = 0.0, .y = 0.0, .width = static_cast<double>(rendering::window_width),
       .height = static_cast<double>(rendering::window_height)}, {});
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
