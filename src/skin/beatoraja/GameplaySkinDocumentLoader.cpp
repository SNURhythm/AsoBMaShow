#include "GameplaySkinDocumentLoader.h"

#include "GameplaySkinBuiltinCatalog.h"
#include "JsonGameplaySkinDecoder.h"
#include "Lr2GameplaySkinDecoder.h"
#include "Lr2SkinCsvParser.h"
#include "Lr2SkinHeaderDecoder.h"
#include "LuaSkinRuntime.h"
#include "LuaSkinTableDecoder.h"
#include "SkinModelValidator.h"
#include "../GameplaySkinTraits.h"
#include "../SkinTargetTraits.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace skin {
namespace {

struct DecodedGameplaySkinDocument {
  std::optional<BeatorajaSkinHeader> header;
  std::optional<BeatorajaSkinConfiguration> configuration;
  std::optional<EntryProfileSettings> reconciledSettings;
  std::optional<BeatorajaSkinModel> model;
  std::unique_ptr<LuaSkinRuntime> runtime;
  bool cancelled = false;
  bool fatal = false;
  std::vector<SkinDiagnostic> diagnostics;
};

SkinDiagnostic documentDiagnostic(std::string code, std::string message,
                                  std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

void appendMoved(std::vector<SkinDiagnostic> &destination,
                 std::vector<SkinDiagnostic> &source) {
  destination.insert(destination.end(),
                     std::make_move_iterator(source.begin()),
                     std::make_move_iterator(source.end()));
}

void appendFailure(std::vector<SkinDiagnostic> &diagnostics,
                   std::optional<SkinDiagnostic> failure,
                   std::string_view fallbackCode,
                   std::string_view fallbackMessage,
                   std::string_view virtualPath = {}) {
  if (failure) {
    diagnostics.push_back(std::move(*failure));
  } else {
    diagnostics.push_back(documentDiagnostic(
        std::string(fallbackCode), std::string(fallbackMessage),
        std::string(virtualPath)));
  }
}

void appendFileFailure(std::vector<SkinDiagnostic> &diagnostics,
                       const std::optional<SkinFileFailure> &failure,
                       std::string code, std::string message,
                       const SkinEntryId &entry) {
  diagnostics.push_back(documentDiagnostic(
      std::move(code), failure ? failure->message : std::move(message),
      failure ? failure->virtualPath : entry.packageRelativePath));
}

bool hasErrors(std::span<const SkinDiagnostic> diagnostics) {
  return std::ranges::any_of(diagnostics, [](const SkinDiagnostic &diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
  });
}

bool cancellationRequested(const GameplaySkinDocumentRequest &request,
                           DecodedGameplaySkinDocument &result) {
  if (!request.stop.stop_requested()) {
    return false;
  }
  result.cancelled = true;
  return true;
}

bool requestMatchesEntry(const GameplaySkinDocumentRequest &request,
                         DecodedGameplaySkinDocument &result) {
  const auto classified =
      gameplaySkinSourceFormatForPath(request.entry.packageRelativePath);
  if (!classified || *classified != request.sourceFormat ||
      request.documentFileSystem.entry() != request.entry) {
    result.diagnostics.push_back(documentDiagnostic(
        "skin.document.source_mismatch",
        "Gameplay skin source classification does not match its retained "
        "entry filesystem.",
        request.entry.packageRelativePath));
    return false;
  }
  return true;
}

std::uint64_t documentByteLimit(const GameplaySkinDocumentRequest &request,
                                std::size_t standard) {
  return request.safetyPolicy.documentByteLimit(
      static_cast<std::uint64_t>(standard));
}

DecodedGameplaySkinDocument decodeLua(GameplaySkinDocumentRequest &request,
                                      bool configured) {
  DecodedGameplaySkinDocument result;
  if (!request.luaFileSystem ||
      request.luaFileSystem->entry() != request.entry) {
    result.diagnostics.push_back(documentDiagnostic(
        "skin_lua_filesystem_create_failed",
        "Lua gameplay document requires its exact retained entry filesystem.",
        request.entry.packageRelativePath));
    return result;
  }
  auto runtime = LuaSkinRuntime::create(
      {.purpose = request.luaPurpose,
       .fileSystem = std::move(request.luaFileSystem),
       .safetyPolicy = request.safetyPolicy,
       .httpTransport = std::move(request.luaHttpTransport),
       .audioBackend = std::move(request.luaAudioBackend),
       .stop = request.stop});
  if (!runtime.runtime) {
    appendFailure(result.diagnostics, std::move(runtime.failure),
                  "skin_lua_runtime_create_failed",
                  "Lua gameplay document runtime could not be created",
                  request.entry.packageRelativePath);
    return result;
  }

  LuaSkinTableDecoder decoder(request.safetyPolicy);
  auto headerValue = runtime.runtime->loadHeader();
  if (!headerValue.value) {
    appendFailure(result.diagnostics, std::move(headerValue.failure),
                  "skin_lua_header_load_failed",
                  "Lua skin header could not be loaded",
                  request.entry.packageRelativePath);
    return result;
  }
  auto decodedHeader = decoder.decodeHeader(*headerValue.value);
  appendMoved(result.diagnostics, decodedHeader.diagnostics);
  headerValue.value.reset();
  if (!decodedHeader.header || hasErrors(result.diagnostics) ||
      cancellationRequested(request, result)) {
    return result;
  }
  result.header = std::move(decodedHeader.header);

  auto reconciliation = reconcileSkinConfiguration(
      *result.header, request.desiredSettings, request.documentFileSystem,
      request.pinnedRuntimeSelection);
  appendMoved(result.diagnostics, reconciliation.diagnostics);
  if (!reconciliation.configuration || hasErrors(result.diagnostics) ||
      cancellationRequested(request, result)) {
    return result;
  }
  result.configuration = std::move(reconciliation.configuration);
  result.reconciledSettings = std::move(reconciliation.reconciledSettings);
  const std::string reconciledDigest =
      skinConfigurationDigest(*result.reconciledSettings);
  if (result.configuration->lowercaseSha256.empty() ||
      result.configuration->lowercaseSha256 != reconciledDigest) {
    result.configuration.reset();
    result.reconciledSettings.reset();
    result.diagnostics.push_back(documentDiagnostic(
        "skin.document.configuration_digest_invalid",
        "Reconciled gameplay skin configuration has an inconsistent digest.",
        request.entry.packageRelativePath));
    return result;
  }
  if (!request.expectedConfigurationDigest.empty() &&
      result.configuration->lowercaseSha256 !=
          request.expectedConfigurationDigest) {
    result.diagnostics.push_back(documentDiagnostic(
        "skin.session.configuration_digest_mismatch",
        "Fresh gameplay skin configuration does not match the validated "
        "activation digest.",
        request.entry.packageRelativePath));
    return result;
  }
  if (!configured) {
    return result;
  }
  if (!request.loadConfiguredLua) {
    result.diagnostics.push_back(documentDiagnostic(
        "skin_lua_initial_state_missing",
        "Lua gameplay document requires an initialized frame authority.",
        request.entry.packageRelativePath));
    return result;
  }

  auto configuredValue = request.loadConfiguredLua(
      *runtime.runtime, *result.configuration, result.diagnostics);
  if (!configuredValue.value) {
    appendFailure(result.diagnostics, std::move(configuredValue.failure),
                  "skin_lua_configured_load_failed",
                  "Lua gameplay skin configured phase could not be loaded",
                  request.entry.packageRelativePath);
    return result;
  }
  if (hasErrors(result.diagnostics) ||
      cancellationRequested(request, result)) {
    return result;
  }
  auto decodedModel = decoder.decodeGameplay(
      *configuredValue.value,
      {.runtime = *runtime.runtime,
       .builtins = gameplaySkinBuiltinCatalog(),
       .safetyPolicy = request.safetyPolicy});
  appendMoved(result.diagnostics, decodedModel.diagnostics);
  configuredValue.value.reset();
  if (!decodedModel.model || hasErrors(result.diagnostics) ||
      cancellationRequested(request, result)) {
    return result;
  }
  result.model = std::move(decodedModel.model);
  result.runtime = std::move(runtime.runtime);
  return result;
}

DecodedGameplaySkinDocument decodeJson(GameplaySkinDocumentRequest &request) {
  DecodedGameplaySkinDocument result;
  const auto bytes = request.documentFileSystem.readEntry(documentByteLimit(
      request, JsonGameplaySkinDecoderPolicy::maxDocumentBytes));
  if (bytes.failure) {
    appendFileFailure(result.diagnostics, bytes.failure,
                      "skin_json_read_failed",
                      "JSON gameplay document could not be read",
                      request.entry);
    return result;
  }
  if (cancellationRequested(request, result)) {
    return result;
  }
  JsonGameplaySkinDecoder decoder;
  auto decoded = decoder.decode(bytes.bytes, request.entry,
                                request.desiredSettings,
                                gameplaySkinBuiltinCatalog(),
                                request.safetyPolicy, request.stop);
  result.header = std::move(decoded.header);
  result.configuration = std::move(decoded.configuration);
  result.reconciledSettings = std::move(decoded.reconciledSettings);
  result.model = std::move(decoded.model);
  result.cancelled = decoded.cancelled;
  result.diagnostics = std::move(decoded.diagnostics);
  (void)cancellationRequested(request, result);
  return result;
}

DecodedGameplaySkinDocument decodeLr2(GameplaySkinDocumentRequest &request) {
  DecodedGameplaySkinDocument result;
  const auto bytes = request.documentFileSystem.readEntry(documentByteLimit(
      request, Lr2SkinCsvParserLimits::maxDocumentBytes));
  if (bytes.failure) {
    appendFileFailure(result.diagnostics, bytes.failure,
                      "skin_lr2_read_failed",
                      "LR2 gameplay document could not be read", request.entry);
    return result;
  }
  const std::uint64_t requestedLimit = documentByteLimit(
      request, Lr2SkinCsvParserLimits::maxDocumentBytes);
  const std::size_t aggregateLimit = static_cast<std::size_t>(
      std::min<std::uint64_t>(requestedLimit,
                              std::numeric_limits<std::size_t>::max()));
  Lr2SkinCsvParser parser(
      {.maximumDocumentBytes = aggregateLimit,
       .maximumIncludeDepth = Lr2SkinCsvParserLimits::maxIncludeDepth});
  auto rootParsed = parser.parse(
      request.documentFileSystem, request.entry.packageRelativePath,
      bytes.bytes, request.stop,
      {.includeExpansion = Lr2IncludeExpansionMode::Preserve});
  appendMoved(result.diagnostics, rootParsed.diagnostics);
  if (rootParsed.cancelled || cancellationRequested(request, result)) {
    result.cancelled = true;
    return result;
  }
  if (rootParsed.fatal) {
    result.fatal = true;
    return result;
  }

  Lr2SkinHeaderDecoder headerDecoder;
  auto decodedHeader = headerDecoder.decode(rootParsed.commands);
  appendMoved(result.diagnostics, decodedHeader.diagnostics);
  result.header = std::move(decodedHeader.header);
  if (!result.header || !skinTargetTraitForType(result.header->type)) {
    return result;
  }

  Lr2GameplaySkinDecoder gameplayDecoder;
  auto reconciled = reconcileLr2GameplaySkinConfiguration(
      *result.header, request.desiredSettings);
  appendMoved(result.diagnostics, reconciled.diagnostics);
  if (reconciled.fatal || !reconciled.configuration) {
    result.fatal = true;
    return result;
  }
  auto parsed = parser.parse(
      request.documentFileSystem, request.entry.packageRelativePath,
      bytes.bytes, request.stop,
      {.includeExpansion = Lr2IncludeExpansionMode::ConditionAware,
       .optionStates = reconciled.configuration->optionStates});
  appendMoved(result.diagnostics, parsed.diagnostics);
  if (parsed.cancelled || cancellationRequested(request, result)) {
    result.cancelled = true;
    return result;
  }
  if (parsed.fatal) {
    result.fatal = true;
    return result;
  }
  auto decoded = gameplayDecoder.decode(
      *result.header, parsed.commands, request.desiredSettings,
      gameplaySkinBuiltinCatalog(), request.safetyPolicy, request.stop, {},
      &reconciled.configuration->optionStates);
  result.configuration = std::move(decoded.configuration);
  result.reconciledSettings = std::move(decoded.reconciledSettings);
  result.model = std::move(decoded.model);
  result.fatal = decoded.fatal;
  result.cancelled = decoded.cancelled;
  appendMoved(result.diagnostics, decoded.diagnostics);
  (void)cancellationRequested(request, result);
  return result;
}

DecodedGameplaySkinDocument decode(GameplaySkinDocumentRequest &request,
                                   bool configuredLua) {
  DecodedGameplaySkinDocument result;
  if (cancellationRequested(request, result) ||
      !requestMatchesEntry(request, result)) {
    return result;
  }
  switch (request.sourceFormat) {
  case GameplaySkinSourceFormat::Lua:
    return decodeLua(request, configuredLua);
  case GameplaySkinSourceFormat::Json:
    return decodeJson(request);
  case GameplaySkinSourceFormat::Lr2:
    return decodeLr2(request);
  }
  result.diagnostics.push_back(documentDiagnostic(
      "skin.document.source_unsupported",
      "Gameplay skin source format is unsupported.",
      request.entry.packageRelativePath));
  return result;
}

void verifyConfigurationDigest(DecodedGameplaySkinDocument &decoded,
                               const SkinEntryId &entry) {
  if (!decoded.configuration || !decoded.reconciledSettings) {
    return;
  }
  const std::string digest =
      skinConfigurationDigest(*decoded.reconciledSettings);
  if (decoded.configuration->lowercaseSha256 == digest && !digest.empty()) {
    return;
  }
  decoded.configuration.reset();
  decoded.reconciledSettings.reset();
  decoded.model.reset();
  decoded.runtime.reset();
  decoded.diagnostics.push_back(documentDiagnostic(
      "skin.document.configuration_digest_invalid",
      "Reconciled gameplay skin configuration has an inconsistent digest.",
      entry.packageRelativePath));
}

} // namespace

InspectedGameplaySkinDocument GameplaySkinDocumentLoader::inspect(
    GameplaySkinDocumentRequest request) const {
  InspectedGameplaySkinDocument result;
  try {
    DecodedGameplaySkinDocument decoded = decode(request, false);
    verifyConfigurationDigest(decoded, request.entry);
    if (decoded.fatal) {
      decoded.header.reset();
      decoded.configuration.reset();
      decoded.reconciledSettings.reset();
    }
    result.header = std::move(decoded.header);
    result.configuration = std::move(decoded.configuration);
    result.reconciledSettings = std::move(decoded.reconciledSettings);
    result.cancelled = decoded.cancelled;
    result.diagnostics = std::move(decoded.diagnostics);
  } catch (...) {
    result.diagnostics.push_back(documentDiagnostic(
        "skin.document.inspect_failed",
        "Gameplay skin document inspection failed closed.",
        request.entry.packageRelativePath));
  }
  return result;
}

GameplaySkinDocumentLoadResult GameplaySkinDocumentLoader::load(
    GameplaySkinDocumentRequest request) const {
  GameplaySkinDocumentLoadResult result;
  try {
    DecodedGameplaySkinDocument decoded = decode(request, true);
    verifyConfigurationDigest(decoded, request.entry);
    result.cancelled = decoded.cancelled;
    if (decoded.reconciledSettings) {
      result.reconciledSettings = *decoded.reconciledSettings;
    }
    if (decoded.configuration) {
      result.configurationDigest = decoded.configuration->lowercaseSha256;
    }
    if (decoded.cancelled) {
      result.diagnostics = std::move(decoded.diagnostics);
      return result;
    }
    if (request.sourceFormat != GameplaySkinSourceFormat::Lua &&
        !request.expectedConfigurationDigest.empty() &&
        result.configurationDigest != request.expectedConfigurationDigest) {
      decoded.diagnostics.push_back(documentDiagnostic(
          "skin.session.configuration_digest_mismatch",
          "Fresh gameplay skin configuration does not match the validated "
          "activation digest.",
          request.entry.packageRelativePath));
      result.diagnostics = std::move(decoded.diagnostics);
      return result;
    }
    const bool diagnosticsAreFatal =
        request.sourceFormat != GameplaySkinSourceFormat::Lr2 &&
        hasErrors(decoded.diagnostics);
    if (!decoded.header || !decoded.configuration ||
        !decoded.reconciledSettings || !decoded.model ||
        !skinTargetTraitForType(decoded.header->type) ||
        decoded.fatal || diagnosticsAreFatal) {
      result.diagnostics = std::move(decoded.diagnostics);
      if (result.diagnostics.empty()) {
        result.diagnostics.push_back(documentDiagnostic(
            "skin.document.incomplete",
            "Gameplay skin document did not produce a complete gameplay model.",
            request.entry.packageRelativePath));
      }
      return result;
    }

    SkinModelValidator validator;
    const std::optional<LuaCallbackLivenessView> callbacks =
        decoded.runtime
            ? std::optional<LuaCallbackLivenessView>{
                  decoded.runtime->callbackLiveness()}
            : std::nullopt;
    auto validated = validator.validate(
        std::move(*decoded.model),
        {.builtins = gameplaySkinBuiltinCatalog(), .callbacks = callbacks});
    const bool validationErrors = hasErrors(validated.diagnostics);
    appendMoved(decoded.diagnostics, validated.diagnostics);
    if (!validated.model || validationErrors) {
      result.diagnostics = std::move(decoded.diagnostics);
      return result;
    }

    result.document.emplace(LoadedGameplaySkinDocument{
        .header = std::move(*decoded.header),
        .configuration = std::move(*decoded.configuration),
        .reconciledSettings = std::move(*decoded.reconciledSettings),
        .model = std::move(*validated.model),
        .luaRuntime = std::move(decoded.runtime),
        .diagnostics = std::move(decoded.diagnostics),
        .sourceFormat = request.sourceFormat,
        .entry = std::move(request.entry)});
  } catch (...) {
    result.diagnostics.push_back(documentDiagnostic(
        "skin.document.load_failed",
        "Gameplay skin document loading failed closed.",
        request.entry.packageRelativePath));
  }
  return result;
}

} // namespace skin
