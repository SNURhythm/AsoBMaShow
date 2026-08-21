#include "GameplaySkinValidator.h"

#include "GameplaySkinDocumentLoader.h"
#include "GameplaySkinSourceFormat.h"
#include "../GameplaySkinTraits.h"
#include "LuaSkinFileSystem.h"

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

SkinEntryMetadataSnapshot
metadataFor(const BeatorajaSkinHeader &header,
            const BeatorajaSkinConfiguration &configuration) {
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
    // SkinConfigurationView always appends this synthetic entry after the
    // authored CustomOption values.  It is a real selectable configuration,
    // not a presentation convention inferred from an option's label.
    catalogOption.choices.push_back({.label = "Random", .value = -1});
    metadata.options.push_back(std::move(catalogOption));
  }
  for (std::size_t index = 0; index < header.files.size(); ++index) {
    const auto &file = header.files[index];
    std::vector<std::string> choices;
    if (index < configuration.orderedFiles.size()) {
      choices = configuration.orderedFiles[index].choices;
    }
    metadata.files.push_back({.category = file.category,
                              .name = file.name,
                              .pattern = file.pattern,
                              .defaultValue = file.defaultValue,
                              .choices = std::move(choices)});
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

std::pair<std::string, std::string>
unavailableTypeDiagnostic(GameplaySkinSourceFormat format) {
  switch (format) {
  case GameplaySkinSourceFormat::Lua:
    return {"skin_lua_type_unavailable",
            "Lua skin declares a supported Beatoraja type that is not a "
            "gameplay keymode in this build"};
  case GameplaySkinSourceFormat::Json:
    return {"skin_json_type_unavailable",
            "JSON document does not declare a gameplay skin type available "
            "in this build"};
  case GameplaySkinSourceFormat::Lr2:
    return {"skin_lr2_type_unavailable",
            "LR2 skin declares a Beatoraja type that is not a gameplay "
            "keymode in this build"};
  }
  return {"skin.document.type_unavailable",
          "Gameplay skin document type is unavailable"};
}

} // namespace

GameplaySkinValidator::GameplaySkinValidator(
    SkinResourcePreparationService &resources) noexcept
    : resources_(&resources) {}

SkinValidationResult GameplaySkinValidator::validate(
    SkinRevisionReadView revision, const SkinEntryId &entry,
    const EntryProfileSettings *desiredSettings, std::stop_token stop) {
  return validate(std::move(revision), entry, desiredSettings, stop,
                  SkinSafetyPolicy{});
}

SkinValidationResult GameplaySkinValidator::validate(
    SkinRevisionReadView revision, const SkinEntryId &entry,
    const EntryProfileSettings *desiredSettings, std::stop_token stop,
    const SkinSafetyPolicy &safetyPolicy) {
  SkinValidationResult result;
  if (cancelled(stop, result)) {
    return result;
  }
  try {
    const auto sourceFormat =
        gameplaySkinSourceFormatForPath(entry.packageRelativePath);
    if (!sourceFormat) {
      result.diagnostics.push_back(validationDiagnostic(
          "skin.document.source_unsupported",
          "Gameplay skin entry has an unsupported source extension.",
          entry.packageRelativePath));
      return result;
    }

    auto documentFiles = LuaSkinFileSystem::create(
        {.revision = revision, .entry = entry, .safetyPolicy = safetyPolicy});
    if (!documentFiles.fileSystem) {
      const std::string message =
          documentFiles.failure
              ? documentFiles.failure->message
              : "Gameplay skin document filesystem could not be created";
      result.diagnostics.push_back(validationDiagnostic(
          "skin.document.filesystem_create_failed", message,
          documentFiles.failure ? documentFiles.failure->virtualPath : ""));
      return result;
    }
    if (cancelled(stop, result)) {
      return result;
    }

    std::unique_ptr<LuaSkinFileSystem> luaFiles;
    if (*sourceFormat == GameplaySkinSourceFormat::Lua) {
      auto created = LuaSkinFileSystem::create(
          {.revision = revision, .entry = entry, .safetyPolicy = safetyPolicy});
      if (!created.fileSystem) {
        result.diagnostics.push_back(validationDiagnostic(
            "skin_lua_filesystem_create_failed",
            created.failure
                ? created.failure->message
                : "Lua validation filesystem could not be created",
            created.failure ? created.failure->virtualPath : ""));
        return result;
      }
      luaFiles = std::move(created.fileSystem);
    }

    GameplaySkinDocumentLoader loader;
    auto inspected = loader.inspect(
        {.sourceFormat = *sourceFormat,
         .entry = entry,
         .documentFileSystem = *documentFiles.fileSystem,
         .luaFileSystem = std::move(luaFiles),
         .desiredSettings = desiredSettings,
         .luaPurpose = LuaRuntimePurpose::Validation,
         .safetyPolicy = safetyPolicy,
         .stop = stop});
    result.diagnostics = std::move(inspected.diagnostics);
    if (inspected.cancelled || cancelled(stop, result)) {
      result.cancelled = true;
      return result;
    }
    if (!inspected.header) {
      return result;
    }

    result.metadata = metadataFor(
        *inspected.header,
        inspected.configuration.value_or(BeatorajaSkinConfiguration{}));
    if (!gameplaySkinTraitForSkinType(result.metadata->skinType)) {
      result.disposition = SkinValidationDisposition::UnavailableType;
      const auto [code, message] = unavailableTypeDiagnostic(*sourceFormat);
      result.diagnostics.push_back(validationDiagnostic(
          code, message, entry.packageRelativePath));
      return result;
    }
    if (!inspected.configuration || !inspected.reconciledSettings) {
      return result;
    }

    result.disposition = SkinValidationDisposition::SelectableGameplay;
    result.reconciledSettings = std::move(inspected.reconciledSettings);
    result.configurationDigest = inspected.configuration->lowercaseSha256;
    return result;
  } catch (...) {
    result = {};
    result.diagnostics.push_back(
        validationDiagnostic("skin.document.validation_failed",
                             "Gameplay skin validation failed closed"));
    return result;
  }
}

} // namespace skin
