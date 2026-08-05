#include "GameplaySkinValidator.h"

#include "GameplaySkinBuiltinCatalog.h"
#include "../GameplaySkinTraits.h"
#include "LuaSkinFileSystem.h"
#include "LuaSkinRuntime.h"
#include "LuaSkinTableDecoder.h"

#include <algorithm>
#include <iterator>
#include <span>
#include <utility>

namespace skin {
namespace {

SkinDiagnostic validationDiagnostic(std::string code, std::string message,
                                    std::string virtualPath = {}) {
  return {.code = std::move(code),
          .message = std::move(message),
          .virtualPath = std::move(virtualPath),
          .severity = DiagnosticSeverity::Error};
}

void appendFailure(std::vector<SkinDiagnostic> &diagnostics,
                   std::optional<SkinDiagnostic> failure,
                   std::string_view fallbackCode,
                   std::string_view fallbackMessage) {
  if (failure) {
    diagnostics.push_back(std::move(*failure));
  } else {
    diagnostics.push_back(validationDiagnostic(std::string(fallbackCode),
                                               std::string(fallbackMessage)));
  }
}

SkinEntryMetadataSnapshot metadataFor(const BeatorajaSkinHeader &header) {
  SkinEntryMetadataSnapshot metadata;
  metadata.displayName = header.name;
  metadata.author = header.author;
  metadata.skinType = header.type;
  metadata.authoredWidth = header.width;
  metadata.authoredHeight = header.height;
  for (const auto &category : header.categories) {
    metadata.categories.push_back(
        {.name = category.name, .items = category.items});
  }
  for (const auto &option : header.options) {
    SkinCatalogOptionDeclaration catalogOption;
    catalogOption.category = option.category;
    catalogOption.name = option.name;
    catalogOption.defaultLabel = option.defaultLabel;
    for (const auto &choice : option.choices) {
      catalogOption.choices.push_back(
          {.label = choice.label, .value = choice.value});
    }
    metadata.options.push_back(std::move(catalogOption));
  }
  for (const auto &file : header.files) {
    metadata.files.push_back({.category = file.category,
                              .name = file.name,
                              .pattern = file.pattern,
                              .defaultValue = file.defaultValue});
  }
  for (const auto &offset : header.offsets) {
    metadata.offsets.push_back({.category = offset.category,
                                .name = offset.name,
                                .id = offset.id,
                                .permissions = offset.permissions});
  }
  return metadata;
}

bool cancelled(std::stop_token stop, SkinValidationResult &result) {
  if (!stop.stop_requested()) {
    return false;
  }
  result.cancelled = true;
  return true;
}

bool hasErrors(std::span<const SkinDiagnostic> diagnostics) {
  return std::ranges::any_of(diagnostics, [](const SkinDiagnostic &diagnostic) {
    return diagnostic.severity == DiagnosticSeverity::Error;
  });
}

} // namespace

GameplaySkinValidator::GameplaySkinValidator(
    SkinResourcePreparationService &resources) noexcept
    : resources_(&resources) {}

SkinValidationResult GameplaySkinValidator::validate(
    SkinRevisionReadView revision, const SkinEntryId &entry,
    const EntryProfileSettings *desiredSettings, std::stop_token stop) {
  SkinValidationResult result;
  if (cancelled(stop, result)) {
    return result;
  }
  try {
    auto runtimeFiles =
        LuaSkinFileSystem::create({.revision = revision, .entry = entry});
    if (!runtimeFiles.fileSystem) {
      const std::string message =
          runtimeFiles.failure ? runtimeFiles.failure->message
                               : "Lua skin filesystem could not be created";
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_filesystem_create_failed", message,
          runtimeFiles.failure ? runtimeFiles.failure->virtualPath : ""));
      return result;
    }
    if (cancelled(stop, result)) {
      return result;
    }

    auto runtime = LuaSkinRuntime::create(
        {.purpose = LuaRuntimePurpose::Validation,
         .fileSystem = std::move(runtimeFiles.fileSystem)});
    if (!runtime.runtime) {
      appendFailure(result.diagnostics, std::move(runtime.failure),
                    "skin_lua_runtime_create_failed",
                    "Lua validation runtime could not be created");
      return result;
    }

    LuaSkinTableDecoder decoder;
    auto headerValue = runtime.runtime->loadHeader();
    if (!headerValue.value) {
      appendFailure(result.diagnostics, std::move(headerValue.failure),
                    "skin_lua_header_load_failed",
                    "Lua skin header could not be loaded");
      return result;
    }
    auto decodedHeader = decoder.decodeHeader(*headerValue.value);
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(decodedHeader.diagnostics.begin()),
        std::make_move_iterator(decodedHeader.diagnostics.end()));
    if (!decodedHeader.header || hasErrors(result.diagnostics) ||
        cancelled(stop, result)) {
      return result;
    }

    // The configuration reconciliation uses a fresh, equivalent package
    // filesystem. The Lua runtime exclusively owns the first filesystem for
    // both loader phases, matching LuaSkinLoader.load's single-state flow.
    auto configurationFiles =
        LuaSkinFileSystem::create({.revision = revision, .entry = entry});
    if (!configurationFiles.fileSystem) {
      const std::string message =
          configurationFiles.failure
              ? configurationFiles.failure->message
              : "Lua skin configuration filesystem could not be created";
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_filesystem_create_failed", message,
          configurationFiles.failure ? configurationFiles.failure->virtualPath
                                     : ""));
      return result;
    }
    auto reconciliation = reconcileSkinConfiguration(
        *decodedHeader.header, desiredSettings, *configurationFiles.fileSystem);
    result.diagnostics.insert(
        result.diagnostics.end(),
        std::make_move_iterator(reconciliation.diagnostics.begin()),
        std::make_move_iterator(reconciliation.diagnostics.end()));
    if (!reconciliation.configuration || hasErrors(result.diagnostics) ||
        cancelled(stop, result)) {
      return result;
    }
    BeatorajaSkinConfiguration configuration =
        std::move(*reconciliation.configuration);
    const std::string reconciledConfigurationDigest =
        skinConfigurationDigest(reconciliation.reconciledSettings);
    if (configuration.lowercaseSha256.empty() ||
        configuration.lowercaseSha256 != reconciledConfigurationDigest) {
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_configuration_digest_invalid",
          "reconciled Lua skin configuration has an inconsistent digest"));
      return result;
    }
    if (cancelled(stop, result)) {
      return result;
    }

    result.metadata = metadataFor(*decodedHeader.header);
    if (!gameplaySkinTraitForSkinType(result.metadata->skinType)) {
      result.disposition = SkinValidationDisposition::UnavailableType;
      result.diagnostics.push_back(validationDiagnostic(
          "skin_lua_type_unavailable",
          "Lua skin declares a supported Beatoraja type that is not a "
          "gameplay keymode in this build"));
      return result;
    }

    result.disposition = SkinValidationDisposition::SelectableGameplay;
    result.reconciledSettings = std::move(reconciliation.reconciledSettings);
    result.configurationDigest = reconciledConfigurationDigest;
    return result;
  } catch (...) {
    result = {};
    result.diagnostics.push_back(
        validationDiagnostic("skin_lua_validation_failed",
                             "Lua gameplay skin validation failed closed"));
    return result;
  }
}

} // namespace skin
