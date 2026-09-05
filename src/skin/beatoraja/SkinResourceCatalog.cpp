#include "SkinResourceCatalog.h"

#include "../../StartupTiming.h"
#include "SkinTextAtlas.h"
#include "../LuaGameplaySkinFeature.h"
#include "../package/SkinPackageTypes.h"
#include "../../FileChecksum.h"
#include "../../RAII.h"
#include "../../view/ImageFileDecoder.h"

#include <SDL2/SDL.h>

#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <exception>
#include <limits>
#include <functional>
#include <filesystem>
#include <cstdlib>
#include <set>
#include <tuple>
#include <type_traits>

namespace skin {

bool skinResourcePathIsMovie(std::string_view path) noexcept {
  constexpr std::array<std::string_view, 9> extensions{
      ".mp4", ".m4v", ".wmv", ".webm", ".mpg",
      ".mpeg", ".m1v", ".m2v", ".avi"};
  const auto extension = std::filesystem::path(path).extension().string();
  return std::ranges::any_of(extensions, [&](std::string_view candidate) {
    return extension.size() == candidate.size() &&
           std::ranges::equal(extension, candidate, [](char left, char right) {
             return static_cast<char>(std::tolower(
                        static_cast<unsigned char>(left))) == right;
           });
  });
}

namespace {
constexpr SkinResourceId kPracticeSystemFontResource =
    std::numeric_limits<SkinResourceId>::max();
constexpr SkinResourceId kBuiltinBlackResource =
    std::numeric_limits<SkinResourceId>::max() - 1U;
constexpr SkinResourceId kBuiltinWhiteResource =
    std::numeric_limits<SkinResourceId>::max() - 2U;
constexpr SkinResourceId kBuiltinStageResource =
    std::numeric_limits<SkinResourceId>::max() - 3U;
constexpr SkinResourceId kBuiltinBackResource =
    std::numeric_limits<SkinResourceId>::max() - 4U;
constexpr SkinResourceId kBuiltinBannerResource =
    std::numeric_limits<SkinResourceId>::max() - 5U;
constexpr std::string_view kPracticeSystemFontVirtualPath =
    "@asobmashow/practice-system-font";
constexpr std::string_view kPracticeSystemFontPath =
    "assets/fonts/notosanscjkjp.ttf";

SkinResourceId builtinImageResourceId(int reference) noexcept {
  switch (reference) {
  case 100:
    return kBuiltinStageResource;
  case 101:
    return kBuiltinBackResource;
  case 102:
    return kBuiltinBannerResource;
  case 110:
    return kBuiltinBlackResource;
  case 111:
    return kBuiltinWhiteResource;
  default:
    return 0;
  }
}

#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
std::atomic_size_t platformAssetReadsForTesting{0};
#endif

void recordPlatformAssetRead() noexcept {
#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
  platformAssetReadsForTesting.fetch_add(1, std::memory_order_relaxed);
#endif
}

std::optional<std::vector<std::byte>>
readPlatformAsset(std::string_view path, std::size_t maximumBytes) {
  recordPlatformAssetRead();
  const std::string ownedPath(path);
  UniqueResource<SDL_RWops, SDL_RWclose> input(
      SDL_RWFromFile(ownedPath.c_str(), "rb"));
  if (!input) {
    return std::nullopt;
  }
  const Sint64 reportedSize = SDL_RWsize(input.get());
  if (reportedSize >= 0) {
    const auto size = static_cast<std::uint64_t>(reportedSize);
    if (size > maximumBytes) {
      return std::nullopt;
    }
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    if (!result.empty() &&
        SDL_RWread(input.get(), result.data(), 1, result.size()) !=
            result.size()) {
      return std::nullopt;
    }
    return result;
  }

  std::vector<std::byte> result;
  std::array<std::byte, 64U * 1024U> buffer{};
  for (;;) {
    const std::size_t read =
        SDL_RWread(input.get(), buffer.data(), 1, buffer.size());
    if (read > maximumBytes - std::min(result.size(), maximumBytes)) {
      return std::nullopt;
    }
    result.insert(result.end(), buffer.begin(), buffer.begin() + read);
    if (read < buffer.size()) {
      return result;
    }
  }
}

SkinDiagnostic diagnostic(std::string code, std::string message) { return {.code=std::move(code), .message=std::move(message), .severity=DiagnosticSeverity::Error}; }
SkinDiagnostic warning(std::string code, std::string message) { return {.code=std::move(code), .message=std::move(message), .severity=DiagnosticSeverity::Warning}; }

struct ResourceUse {
  bool critical = false;
  std::vector<SkinSourceRect> regions;
};
struct CollectedResourceUses {
  std::map<SkinResourceId, ResourceUse> images;
  std::set<int> builtinImages;
  std::map<SkinResourceId, bool> fonts;
  struct TextUse {
    SkinObjectId object = 0;
    SkinTextAtlasKey key;
    std::string literal;
    bool receivesRuntimeStrings = false;
    bool critical = false;
  };
  std::vector<TextUse> texts;
};

struct ConfiguredResourcePath {
  std::optional<std::string> path;
  std::string error;
};

ConfiguredResourcePath applyConfiguredFileSelection(
    std::string_view authored,
    const BeatorajaSkinConfiguration &configuration,
    const LuaSkinFileSystem &files,
    const SkinSafetyPolicy &safetyPolicy) {
  const ConfiguredFile *match = nullptr;
  for (const auto &file : configuration.orderedFiles) {
    if (!authored.starts_with(file.pattern)) continue;
    match = &file;
    break;
  }
  if (match == nullptr) {
    const std::size_t wildcard = authored.rfind('*');
    if (wildcard == std::string_view::npos) {
      return {.path=std::string(authored)};
    }
    const std::size_t slash = authored.rfind('/', wildcard);
    if (slash == std::string_view::npos) {
      return {.path=std::string(authored)};
    }
    std::string suffix(authored.substr(wildcard + 1));
    if (const std::size_t pipe = authored.find('|'); pipe != std::string_view::npos) {
      suffix = std::string(authored.substr(wildcard + 1, pipe - wildcard - 1));
      if (pipe + 1 < authored.size()) {
        suffix.append(authored.substr(pipe + 1));
      }
    }
    const auto listed = files.listResourceDirectory(authored.substr(0, slash));
    if (listed.failure) {
      return {.path=std::string(authored)};
    }
    std::vector<std::string> candidates;
    for (const std::string &candidate : listed.entries) {
      std::string lowercase = candidate;
      std::ranges::transform(lowercase, lowercase.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (lowercase.ends_with(suffix)) {
        candidates.push_back(candidate);
      }
    }
    if (candidates.empty()) {
      return {.path=std::string(authored)};
    }
    return {.path=candidates[static_cast<std::size_t>(std::rand()) %
                             candidates.size()]};
  }
  const std::size_t wildcard = authored.rfind('*');
  if (wildcard == std::string::npos ||
      authored.size() < match->pattern.size()) {
    return {.error="configured resource pattern is invalid"};
  }
  const std::size_t suffixSize = authored.size() - match->pattern.size();
  const std::size_t maximumPathBytes = skinResourceLimit(
      safetyPolicy, SkinPackagePolicy::maxPathBytes);
  if (wildcard > maximumPathBytes ||
      match->selectedValue.size() >
          maximumPathBytes - wildcard ||
      suffixSize > maximumPathBytes - wildcard -
                       match->selectedValue.size()) {
    return {.error="configured resource path exceeds host limits"};
  }
  std::string selected;
  selected.reserve(wildcard + match->selectedValue.size() + suffixSize);
  selected.append(authored, 0, wildcard);
  selected.append(match->selectedValue);
  selected.append(authored, match->pattern.size(), suffixSize);
  return {.path=std::move(selected)};
}

CollectedResourceUses collectResourceUses(
    const ValidatedBeatorajaSkinModel &model, bool practiceMode) {
  CollectedResourceUses result;
  std::map<SkinObjectId, const SkinObjectDefinition *> objects;
  std::set<SkinObjectId> disabled(model.disabledOptionalObjects.begin(), model.disabledOptionalObjects.end());
  for (const auto &object : model.model.objects) objects.emplace(object.id, &object);
  const bool modelHasPracticeObject = std::ranges::any_of(
      model.model.objects, [](const SkinObjectDefinition &object) {
        return std::holds_alternative<SkinPracticeObject>(object.payload);
      });
  std::set<SkinObjectId> visited;
  const auto addSprite = [&](const SkinSpriteFrames &sprite, bool critical) {
    auto &use = result.images[sprite.resource];
    use.critical = use.critical || critical;
    use.regions.insert(use.regions.end(), sprite.frames.begin(), sprite.frames.end());
  };
  const auto addPracticeText = [&](SkinObjectId object) {
    result.fonts[kPracticeSystemFontResource] = false;
    result.texts.push_back(
        {.object = object,
         .key = {.font = kPracticeSystemFontResource, .pointSize = 18},
         .literal = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:#/=-,.+",
         .receivesRuntimeStrings = false,
         .critical = false});
  };
  std::function<void(SkinObjectId, bool)> visit = [&](SkinObjectId id, bool critical) {
    if (disabled.contains(id) || !visited.insert(id).second) return;
    const auto found = objects.find(id); if (found == objects.end()) return;
    const auto &payload = found->second->payload;
    std::visit([&](const auto &object) {
      using T = std::decay_t<decltype(object)>;
      if constexpr (std::is_same_v<T, SkinImageObject>) { for (const auto &s: object.orderedStates) addSprite(s, critical); }
      else if constexpr (std::is_same_v<T, SkinNumberObject> || std::is_same_v<T, SkinFloatObject>) { addSprite(object.digits.positive, critical); if (object.digits.negative) addSprite(*object.digits.negative, critical); }
      else if constexpr (std::is_same_v<T, SkinTextObject>) {
        result.fonts[object.font] = result.fonts[object.font] || critical;
        result.texts.push_back({.object=found->second->id,
                                .key={.font=object.font, .pointSize=object.pointSize,
                                     .outlineRgba=object.outlineRgba, .outlineWidth=object.outlineWidth,
                                     .shadowRgba=object.shadowRgba, .shadowOffsetX=object.shadowOffsetX,
                                     .shadowOffsetY=object.shadowOffsetY, .shadowSmoothness=object.shadowSmoothness},
                                .literal=object.literal, .receivesRuntimeStrings=object.value.has_value() || object.writer.has_value(), .critical=critical});
      }
      else if constexpr (std::is_same_v<T, SkinPracticeObject>) {
        if (object.visibleItems == 0) {
          addPracticeText(found->second->id);
        }
      }
      else if constexpr (std::is_same_v<T, SkinBgaObject>) {
        if (practiceMode && !modelHasPracticeObject) {
          addPracticeText(found->second->id);
        }
      }
      else if constexpr (std::is_same_v<T, SkinSliderObject>) addSprite(object.knob, critical);
      else if constexpr (std::is_same_v<T, SkinGraphObject>) {
        if (object.builtinImageReference) {
          result.builtinImages.insert(*object.builtinImageReference);
        } else {
          addSprite(object.fill, critical);
        }
      }
      else if constexpr (std::is_same_v<T, SkinBuiltinImageObject>) {
        result.builtinImages.insert(object.referenceId);
      }
      else if constexpr (std::is_same_v<T, SkinGaugeObject>) for (const auto &s: object.orderedNodes) addSprite(s, critical);
      else if constexpr (std::is_same_v<T, SkinNoteObject>) { for (const auto &lane: object.lanes) for (const auto &[kind, visual]: lane.visuals) { (void)kind; if (const auto *s=std::get_if<SkinSpriteFrames>(&visual)) addSprite(*s, critical); } for (const auto &line: object.lines) if (line.sprite) addSprite(*line.sprite, critical); }
      else if constexpr (std::is_same_v<T, SkinCoverObject>) addSprite(object.sprite, critical);
      else if constexpr (std::is_same_v<T, SkinJudgeObject>) for (const auto &grade: object.grades) { if (grade.image) visit(grade.image->object, critical); if (grade.detailNumber) visit(grade.detailNumber->object, critical); }
    }, payload);
  };
  // Critical roots run first so a nested optional object inherits criticality.
  for (const auto &object : model.model.objects) if (object.critical) visit(object.id, true);
  for (const auto &object : model.model.objects) if (!object.critical) visit(object.id, false);
  return result;
}

SkinDiagnostic useDiagnostic(std::string criticalCode, std::string optionalCode,
                            std::string message, bool critical) {
  return {.code = critical ? std::move(criticalCode) : std::move(optionalCode),
          .message = std::move(message),
          .severity = critical ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning};
}

bool sameRect(const SkinSourceRect &left, const SkinSourceRect &right) noexcept {
  return left.x == right.x && left.y == right.y && left.w == right.w && left.h == right.h &&
         left.gridColumn == right.gridColumn && left.gridRow == right.gridRow &&
         left.gridColumns == right.gridColumns && left.gridRows == right.gridRows;
}

void recordRegionIdentityCheck() noexcept;
void recordRegionLookupComparison() noexcept;

bool lessRect(const SkinSourceRect &left,
              const SkinSourceRect &right) noexcept {
  return std::tie(left.x, left.y, left.w, left.h, left.gridColumn,
                  left.gridRow, left.gridColumns, left.gridRows) <
         std::tie(right.x, right.y, right.w, right.h, right.gridColumn,
                  right.gridRow, right.gridColumns, right.gridRows);
}

struct SkinSourceRectLess {
  bool operator()(const SkinSourceRect &left,
                  const SkinSourceRect &right) const noexcept {
    recordRegionIdentityCheck();
    return lessRect(left, right);
  }
};

using RegionIdentityMap = std::map<SkinSourceRect, SkinSourceRect,
                                   SkinSourceRectLess>;

#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
std::atomic_size_t regionIdentityChecksForTesting{0};
std::atomic_size_t regionLookupComparisonsForTesting{0};
std::atomic_size_t fontAtlasRequestHighWaterForTesting{0};
std::atomic_size_t maximumSessionEncodedBytesForTesting{
    std::numeric_limits<std::size_t>::max()};
std::atomic_size_t maximumAtlasSessionBytesForTesting{
    std::numeric_limits<std::size_t>::max()};
std::atomic_size_t committedEncodedBytesForTesting{0};
std::atomic_size_t skinImageAppCacheHitCountForTesting{0};

void recordSkinImageAppCacheHit() noexcept {
  skinImageAppCacheHitCountForTesting.fetch_add(1, std::memory_order_relaxed);
}

void recordRegionIdentityCheck() noexcept {
  regionIdentityChecksForTesting.fetch_add(1, std::memory_order_relaxed);
}

void recordRegionLookupComparison() noexcept {
  regionLookupComparisonsForTesting.fetch_add(1, std::memory_order_relaxed);
}

void recordFontAtlasRequestCount(std::size_t count) noexcept {
  std::size_t observed =
      fontAtlasRequestHighWaterForTesting.load(std::memory_order_relaxed);
  while (observed < count &&
         !fontAtlasRequestHighWaterForTesting.compare_exchange_weak(
             observed, count, std::memory_order_relaxed)) {
  }
}

void recordCommittedEncodedBytes(std::size_t bytes) noexcept {
  committedEncodedBytesForTesting.store(bytes, std::memory_order_relaxed);
}
#else
void recordRegionIdentityCheck() noexcept {}
void recordRegionLookupComparison() noexcept {}
void recordFontAtlasRequestCount(std::size_t) noexcept {}
void recordCommittedEncodedBytes(std::size_t) noexcept {}
#endif

bool resolveRegions(const ResourceUse &use, int width, int height,
                    std::vector<SkinSourceRect> &output,
                    std::vector<SkinResolvedRegion> *mappings,
                    std::vector<SkinDiagnostic> &diagnostics,
                    const SkinSafetyPolicy &safetyPolicy) {
  const std::size_t maximumRegions = skinResourceLimit(
      safetyPolicy, SkinResourcePolicy::maximumRegions);
  if (use.regions.size() > maximumRegions ||
      output.size() > maximumRegions - use.regions.size()) {
    diagnostics.push_back(useDiagnostic("skin.resource.session_limit", "skin.resource.session_limit", "sprite region count exceeds the resource policy", use.critical));
    return false;
  }
  RegionIdentityMap identities;
  if (mappings != nullptr) {
    for (const SkinResolvedRegion &mapping : *mappings) {
      const auto [existing, inserted] = identities.emplace(
          mapping.authored, mapping.resolved);
      if (!inserted && !sameRect(existing->second, mapping.resolved)) {
        diagnostics.push_back(useDiagnostic("skin.resource.region_identity", "skin.resource.region_identity", "authored sprite rectangle resolves inconsistently", use.critical));
        return false;
      }
    }
  }
  for (const SkinSourceRect &authored : use.regions) {
    SkinSourceRect resolved;
    if (!skinResourceResolveRect(authored, width, height, resolved)) {
      diagnostics.push_back(useDiagnostic("skin.resource.sprite_bounds", "skin.resource.sprite_bounds", "sprite crop is outside its decoded image", use.critical));
      return false;
    }
    if (mappings) {
      const auto [existing, inserted] = identities.emplace(authored, resolved);
      if (!inserted && !sameRect(existing->second, resolved)) {
        diagnostics.push_back(useDiagnostic("skin.resource.region_identity", "skin.resource.region_identity", "authored sprite rectangle resolves inconsistently", use.critical));
        return false;
      }
    }
    output.push_back(resolved);
    if (mappings) mappings->push_back({.authored=authored, .resolved=resolved});
  }
  return true;
}

bool runtimeStringsWithinPolicy(std::span<const std::string> strings,
                                std::vector<SkinDiagnostic> &diagnostics,
                                const SkinSafetyPolicy &safetyPolicy) {
  if (strings.size() > skinResourceLimit(
                           safetyPolicy,
                           SkinResourcePolicy::maximumRuntimeStrings)) {
    diagnostics.push_back(diagnostic("skin.resource.session_limit",
                                     "runtime string count exceeds the resource policy"));
    return false;
  }
  std::size_t bytes = 0;
  for (const std::string &value : strings) {
    const std::size_t maximumRuntimeStringBytes = skinResourceLimit(
        safetyPolicy, SkinResourcePolicy::maximumRuntimeStringBytes);
    if (value.size() > maximumRuntimeStringBytes - bytes) {
      diagnostics.push_back(diagnostic("skin.resource.session_limit",
                                       "runtime string bytes exceed the resource policy"));
      return false;
    }
    bytes += value.size();
  }
  return true;
}

SkinDiagnostic fileDiagnostic(const SkinImageResource &resource,
                              const SkinFileFailure *failure, bool critical) {
  if (failure && failure->message == "resource path is ambiguous") {
    return useDiagnostic("skin.resource.path_ambiguous", "skin.resource.path_ambiguous",
                         "resource path resolves to multiple package files: " + resource.authoredName, critical);
  }
  const bool missing = failure && failure->code == SkinFileError::Missing;
  return useDiagnostic(missing ? "skin.resource.missing_critical" : "skin.resource.path_invalid",
                       missing ? "skin.resource.missing_optional" : "skin.resource.path_invalid",
                       "image resource is unavailable: " + resource.authoredName, critical);
}

bool checkInput(const SkinEntryId &entry, SkinRevisionReadView revision,
                const LuaSkinFileSystem &files,
                std::vector<SkinDiagnostic> &diagnostics) {
  const SkinRevision &expected = revision.revision();
  const SkinRevision &actual = files.revision();
  if (entry != files.entry() || expected.package != actual.package ||
      expected.lowercaseSha256 != actual.lowercaseSha256 ||
      expected.fileCount != actual.fileCount || expected.totalBytes != actual.totalBytes ||
      revision.root().lexically_normal() != files.revisionRoot().lexically_normal()) {
    diagnostics.push_back(diagnostic("skin.resource.path_invalid",
                                     "resource preparation entry does not match its filesystem"));
    return false;
  }
  return true;
}

struct FontResourceView {
  SkinResourceId id = 0;
  std::string_view name;
  std::string_view virtualPath;
  int type = 0;
  int originalSize = 0;
  bool bitmap = false;
  std::uint32_t authoredOrdinal = 0;
  std::span<const SkinFontFallbackResource> fallbacks;
};

SkinDiagnostic fontDiagnostic(const FontResourceView &resource, bool critical,
                             std::string code, std::string message) {
  const std::string_view label =
      resource.name.empty() ? resource.virtualPath : resource.name;
  return useDiagnostic(code, code,
                       std::move(message) + ": " + std::string(label),
                       critical);
}

bool appendUtf8(std::string_view value, std::set<char32_t> &codepoints,
                std::set<std::pair<char32_t, char32_t>> &pairs,
                const SkinSafetyPolicy &safetyPolicy) {
  std::optional<char32_t> previous;
  for (std::size_t index = 0; index < value.size();) {
    const unsigned char first = static_cast<unsigned char>(value[index++]);
    char32_t codepoint = 0xfffd;
    std::size_t remaining = 0;
    if (first < 0x80) codepoint = first;
    else if ((first & 0xe0) == 0xc0) { codepoint = first & 0x1f; remaining = 1; }
    else if ((first & 0xf0) == 0xe0) { codepoint = first & 0x0f; remaining = 2; }
    else if ((first & 0xf8) == 0xf0) { codepoint = first & 0x07; remaining = 3; }
    else { codepoints.insert(0xfffd); previous.reset(); continue; }
    if (index + remaining > value.size()) { codepoints.insert(0xfffd); break; }
    bool valid = true;
    for (std::size_t offset = 0; offset < remaining; ++offset) {
      const unsigned char next = static_cast<unsigned char>(value[index++]);
      if ((next & 0xc0) != 0x80) { valid = false; break; }
      codepoint = (codepoint << 6) | (next & 0x3f);
    }
    if (!valid || (remaining == 1 && codepoint < 0x80) ||
        (remaining == 2 && codepoint < 0x800) || (remaining == 3 && codepoint < 0x10000) ||
        codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) codepoint = 0xfffd;
    if (codepoint == U'\r' || codepoint == U'\n') { previous.reset(); continue; }
    codepoints.insert(codepoint);
    if (previous) pairs.emplace(*previous, codepoint);
    previous = codepoint;
  }
  return codepoints.size() <= skinResourceLimit(
                                  safetyPolicy,
                                  SkinResourcePolicy::maximumGlyphs) &&
         pairs.size() <= skinResourceLimit(
                             safetyPolicy,
                             SkinResourcePolicy::maximumKerningPairs);
}

struct FontAtlasRequest {
  SkinTextAtlasKey key;
  FontResourceView font;
  struct ResolvedFace {
    std::string path;
    int type = 0;
    int originalSize = 0;
  };
  std::vector<ResolvedFace> resolvedFaces;
  std::vector<SkinObjectId> objects;
  bool critical = false;
  std::set<char32_t> codepoints;
  std::set<std::pair<char32_t, char32_t>> pairs;
};

std::vector<FontAtlasRequest> collectFontAtlasRequests(
    const ValidatedBeatorajaSkinModel &model, const CollectedResourceUses &uses,
    const LuaSkinFileSystem &files,
    const BeatorajaSkinConfiguration &configuration,
    std::span<const std::string> runtimeStrings,
    const std::map<SkinObjectId, std::vector<std::string>>
        &runtimeStringsByObject,
    std::vector<SkinDiagnostic> &diagnostics,
    const SkinSafetyPolicy &safetyPolicy) {
  std::map<SkinResourceId, FontResourceView> resources;
  for (const auto &definition : model.model.resources) {
    if (const auto *font = std::get_if<SkinFontResource>(&definition)) {
      resources.emplace(
          font->id,
          FontResourceView{.id = font->id,
                           .name = font->authoredName,
                           .virtualPath = font->virtualPath,
                           .type = font->type,
                           .originalSize = font->bitmap
                                               ? font->bitmap->originalSize
                                               : 0,
                           .bitmap = font->bitmap.has_value(),
                           .authoredOrdinal = font->authoredOrdinal,
                           .fallbacks = font->fallbacks});
    }
  }
  if (uses.fonts.contains(kPracticeSystemFontResource)) {
    resources.emplace(
        kPracticeSystemFontResource,
        FontResourceView{.id = kPracticeSystemFontResource,
                         .name = "practice-system-font",
                         .virtualPath = kPracticeSystemFontVirtualPath,
                         .type = 0,
                         .bitmap = false,
                         .authoredOrdinal =
                             std::numeric_limits<std::uint32_t>::max()});
  }
  std::map<SkinTextAtlasKey, FontAtlasRequest> requests;
  for (const auto &text : uses.texts) {
    const auto found = resources.find(text.key.font);
    if (found == resources.end()) {
      diagnostics.push_back(useDiagnostic("skin.resource.font_missing", "skin.resource.font_optional", "text uses an undeclared font resource", text.critical));
      continue;
    }
    SkinTextAtlasKey key = text.key;
    std::string digest = std::to_string(found->second.id) + ':' +
                         std::to_string(found->second.type);
    bool resolved = true;
    bool digestWithinPolicy = true;
    std::vector<FontAtlasRequest::ResolvedFace> resolvedFaces;
    const auto addResolved = [&](std::string_view path, int type,
                                 int originalSize, bool primary) {
      if (!digestWithinPolicy) return;
      if (path == kPracticeSystemFontVirtualPath) {
        digestWithinPolicy = appendStableFallbackChainEntry(
            digest, kPracticeSystemFontVirtualPath, type, safetyPolicy);
        if (digestWithinPolicy) {
          resolvedFaces.push_back(
              {.path = std::string(kPracticeSystemFontVirtualPath),
               .type = type,
               .originalSize = originalSize});
        }
        return;
      }
      const auto configured = applyConfiguredFileSelection(
          path, configuration, files, safetyPolicy);
      if (!configured.path) {
        if (primary) {
          resolved = false;
        } else {
          diagnostics.push_back(fontDiagnostic(
              found->second, false, "skin.resource.font_path_ambiguous",
              "bitmap fallback path is invalid or ambiguous"));
        }
        return;
      }
      // JsonSkinObjectLoader resolves font declarations with Path.resolve,
      // unlike image Source.path declarations, which are handled by
      // resolveResourceCandidates below their entry parent.
      const std::filesystem::path authored(*configured.path);
      const std::filesystem::path candidate =
          (authored.is_absolute() ? authored : files.skinDirectory() / authored)
              .lexically_normal();
      digestWithinPolicy = appendStableFallbackChainEntry(
          digest, candidate.generic_string(), type, safetyPolicy);
      if (digestWithinPolicy) {
        resolvedFaces.push_back({.path = candidate.generic_string(),
                                 .type = type,
                                 .originalSize = originalSize});
      }
    };
    addResolved(found->second.virtualPath, found->second.type,
                found->second.originalSize, true);
    for (const auto &fallback : found->second.fallbacks) {
      if (!fallback.virtualPath.empty()) {
        addResolved(fallback.virtualPath, fallback.type, 0, false);
      }
    }
    if (!resolved) {
      diagnostics.push_back(fontDiagnostic(found->second, text.critical, "skin.resource.font_path_ambiguous", "font primary or fallback path is invalid or ambiguous"));
      continue;
    }
    if (!digestWithinPolicy) {
      diagnostics.push_back(fontDiagnostic(found->second, text.critical,
          "skin.resource.font_style_invalid",
          "font fallback-chain identity exceeds atlas key policy"));
      continue;
    }
    if (!resolvedFaces.empty() &&
        (found->second.bitmap ||
         !safetyPolicy.enforces(
             SkinSafetyGuard::ResourceAllocationLimit))) {
      // SkinTextBitmap applies its styling in the distance-field shader (or
      // ignores it for a standard bitmap font). SkinTextFont rasterizes plain
      // white glyphs and draws only a second, offset layout for its shadow.
      key.outlineRgba = {255, 255, 255, 0};
      key.outlineWidth = 0.0;
      key.shadowRgba = {255, 255, 255, 0};
      key.shadowOffsetX = 0.0;
      key.shadowOffsetY = 0.0;
      key.shadowSmoothness = 0.0;
    }
    if (!resolvedFaces.empty() && found->second.bitmap) {
      // Bitmap pages and glyph metrics do not depend on a text object's size.
      key.pointSize = 1;
    }
    key.fallbackChainDigest = std::move(digest);
    if (!canonicalizeSkinTextAtlasKey(key, safetyPolicy)) {
      diagnostics.push_back(fontDiagnostic(found->second, text.critical, "skin.resource.font_style_invalid", "font atlas style is invalid"));
      continue;
    }
    auto request = requests.find(key);
    if (request == requests.end()) {
      if (requests.size() >= skinResourceLimit(
                                 safetyPolicy,
                                 SkinResourcePolicy::maximumAtlases)) {
        diagnostics.push_back(fontDiagnostic(
            found->second, text.critical, "skin.resource.atlas_limit",
            "font atlas request count exceeds policy"));
        continue;
      }
      request = requests.emplace(
          key, FontAtlasRequest{.key=key, .font=found->second,
                                .resolvedFaces=std::move(resolvedFaces),
                                .critical=text.critical}).first;
      recordFontAtlasRequestCount(requests.size());
    }
    request->second.objects.push_back(text.object);
    request->second.critical = request->second.critical || text.critical;
    if (!appendUtf8(text.literal, request->second.codepoints,
                    request->second.pairs, safetyPolicy))
      diagnostics.push_back(fontDiagnostic(found->second, request->second.critical, "skin.resource.atlas_limit", "font glyph or kerning limit exceeds policy"));
    const auto objectStrings = runtimeStringsByObject.find(text.object);
    if (objectStrings != runtimeStringsByObject.end() ||
        text.receivesRuntimeStrings) {
      const auto appendRuntime = [&](std::string_view runtime) {
        if (!appendUtf8(runtime, request->second.codepoints,
                        request->second.pairs, safetyPolicy)) {
          diagnostics.push_back(fontDiagnostic(
              found->second, request->second.critical,
              "skin.resource.atlas_limit",
              "font glyph or kerning limit exceeds policy"));
        }
      };
      if (objectStrings != runtimeStringsByObject.end()) {
        for (const auto &runtime : objectStrings->second) {
          appendRuntime(runtime);
        }
      } else {
        for (const auto &runtime : runtimeStrings) {
          appendRuntime(runtime);
        }
      }
    }
  }
  std::vector<FontAtlasRequest> result;
  result.reserve(requests.size());
  SkinResourceSessionAccounting metadata(safetyPolicy);
  for (auto &[key, request] : requests) {
    (void)key;
    if (!metadata.addAtlas(/*decodedBytes=*/0, request.codepoints.size(),
                           request.pairs.size())) {
      diagnostics.push_back(fontDiagnostic(
          request.font, request.critical, "skin.resource.atlas_limit",
          "font atlas request aggregate exceeds policy"));
      continue;
    }
    result.push_back(std::move(request));
  }
  return result;
}

std::optional<std::vector<SkinTextAtlasFontBytes>> readFontFaces(
    const FontAtlasRequest &request, const LuaSkinFileSystem &files,
    SkinResourceSessionAccounting &session,
    std::vector<SkinDiagnostic> &diagnostics,
    const std::function<bool()> &cancellationRequested,
    const SkinSafetyPolicy &safetyPolicy) {
  std::vector<SkinTextAtlasFontBytes> faces;
  for (const auto &face : request.resolvedFaces) {
    if (cancellationRequested()) return std::nullopt;
    const bool practiceSystemFont =
        face.path == kPracticeSystemFontVirtualPath;
    if (cancellationRequested()) return std::nullopt;
    std::vector<std::byte> encoded;
    if (practiceSystemFont) {
      auto read = readPlatformAsset(
          kPracticeSystemFontPath,
          skinResourceLimit(safetyPolicy,
                            SkinResourcePolicy::maximumEncodedBytes));
      if (!read) {
        diagnostics.push_back(fontDiagnostic(
            request.font, request.critical, "skin.resource.font_missing",
            "practice system-font bytes are unavailable"));
        return std::nullopt;
      }
      encoded = std::move(*read);
    } else {
      const auto read = files.readResolvedResource(
          face.path, skinResourceLimit(safetyPolicy,
                                  SkinResourcePolicy::maximumEncodedBytes));
      if (read.failure) {
        diagnostics.push_back(fontDiagnostic(request.font, request.critical,
                                              "skin.resource.font_missing",
                                              "font bytes are unavailable"));
        return std::nullopt;
      }
      encoded = read.bytes;
    }
    if (cancellationRequested()) return std::nullopt;
    if (!session.addImage(/*physicalResources=*/0, /*logicalResources=*/0,
                          encoded.size(), /*decodedBytes=*/0,
                          /*regions=*/0)) {
      diagnostics.push_back(fontDiagnostic(request.font, request.critical, "skin.resource.encoded_limit", "font encoded bytes exceed session policy"));
      return std::nullopt;
    }
    faces.push_back({.encoded=std::move(encoded)});
  }
  return faces;
}

std::string lowercaseExtension(std::string_view path) {
  std::string extension = std::filesystem::path(path).extension().string();
  std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return extension;
}

struct BitmapFontPreparationCache {
  std::map<std::string, std::vector<std::byte>, std::less<>> descriptors;
  std::map<std::string, image_decode::DecodedImageData, std::less<>> pages;
  std::map<std::string, std::size_t, std::less<>> pageEncodedBytes;
  std::set<std::string, std::less<>> accountedDescriptors;
  std::set<std::string, std::less<>> accountedPages;
};

struct BitmapFontAccountingIdentities {
  // Payloads may remain cached after a rejected optional atlas. These
  // identities become globally accounted only with the outer session commit.
  std::set<std::string, std::less<>> descriptors;
  std::set<std::string, std::less<>> pages;
};

void commitBitmapFontAccounting(
    BitmapFontPreparationCache &cache,
    const BitmapFontAccountingIdentities &identities) {
  cache.accountedDescriptors.insert(identities.descriptors.begin(),
                                    identities.descriptors.end());
  cache.accountedPages.insert(identities.pages.begin(),
                              identities.pages.end());
}

std::optional<std::vector<SkinTextAtlasBitmapFace>> readBitmapFontFaces(
    const FontAtlasRequest &request, const LuaSkinFileSystem &files,
    SkinResourceSessionAccounting &session,
    std::vector<SkinDiagnostic> &diagnostics,
    const std::function<bool()> &cancellationRequested,
    const SkinSafetyPolicy &safetyPolicy, std::stop_token stop,
    BitmapFontPreparationCache &cache,
    BitmapFontAccountingIdentities &requestAccounting,
    image_decode::ImageDecodeCoordinator &coordinator,
    SkinDecodeCache *decodeCache) {
  std::vector<SkinTextAtlasBitmapFace> result;
  result.reserve(request.resolvedFaces.size());
  for (std::size_t faceIndex = 0; faceIndex < request.resolvedFaces.size();
       ++faceIndex) {
    const auto &face = request.resolvedFaces[faceIndex];
    const bool primary = faceIndex == 0;
    const auto rejectFace = [&](std::string code, std::string message) {
      diagnostics.push_back(fontDiagnostic(
          request.font, primary && request.critical, std::move(code),
          std::move(message)));
    };
    if (cancellationRequested()) return std::nullopt;
    const std::string extension = lowercaseExtension(face.path);
    if (extension != ".fnt" && extension != ".lr2font") {
      rejectFace("skin.resource.font_format_unsupported",
                 "bitmap fallback extension must be FNT or LR2FONT");
      if (primary) return std::nullopt;
      continue;
    }
    SkinResourceSessionAccounting faceSession = session;
    std::set<std::string, std::less<>> newlyAccountedDescriptors;
    std::set<std::string, std::less<>> newlyAccountedPages;
    auto encoded = cache.descriptors.find(face.path);
    if (encoded == cache.descriptors.end()) {
      const auto read = files.readResolvedResource(
          face.path, skinResourceLimit(safetyPolicy,
                                       SkinResourcePolicy::maximumEncodedBytes));
      if (cancellationRequested()) return std::nullopt;
      if (read.failure) {
        rejectFace("skin.resource.font_missing",
                   "bitmap-font descriptor bytes are unavailable");
        if (primary) return std::nullopt;
        continue;
      }
      encoded = cache.descriptors.emplace(face.path, read.bytes).first;
    }
    if (!cache.accountedDescriptors.contains(face.path) &&
        !requestAccounting.descriptors.contains(face.path)) {
      if (!faceSession.addImage(/*physicalResources=*/0,
                                /*logicalResources=*/0,
                                encoded->second.size(), /*decodedBytes=*/0,
                                /*regions=*/0)) {
        rejectFace("skin.resource.encoded_limit",
                   "bitmap-font descriptor bytes exceed session policy");
        if (primary) return std::nullopt;
        continue;
      }
      newlyAccountedDescriptors.insert(face.path);
    }
    auto parsed = parseSkinBitmapFont(
        SkinBitmapFontResource{.id = request.font.id,
                               .virtualPath = face.path,
                               .type = face.type,
                               .originalSize = face.originalSize,
                               .authoredOrdinal = request.font.authoredOrdinal},
        encoded->second,
        extension == ".fnt" ? SkinBitmapFontSourceFormat::BmFont
                            : SkinBitmapFontSourceFormat::Lr2Font,
        safetyPolicy);
    if (!parsed.font) {
      rejectFace("skin.resource.font_format_unsupported", parsed.error);
      if (primary) return std::nullopt;
      continue;
    }
    SkinTextAtlasBitmapFace prepared{.font = std::move(*parsed.font)};
    const auto rejectPage = [&](std::string code, std::string message) {
      diagnostics.push_back(fontDiagnostic(
          request.font,
          primary && request.critical && !prepared.font.lr2Font,
          std::move(code), std::move(message)));
    };
    prepared.pages.resize(prepared.font.pagePaths.size());
    // Only decode the pages that actually carry glyphs from the requested
    // corpus. LITONE12-style BMFonts declare hundreds of pages (a single
    // Select title font has 289) but a gameplay session only touches a small
    // glyph subset, so decoding every page up front is the dominant load
    // cost. Pages without a needed glyph stay absent (.available=false) and
    // are never read from disk.
    std::vector<unsigned char> neededPages(prepared.font.pagePaths.size(), 0);
    if (!prepared.font.lr2Font) {
      const auto markPage = [&](char32_t codepoint) {
        const auto found = prepared.font.glyphs.find(codepoint);
        if (found != prepared.font.glyphs.end() &&
            found->second.page >= 0 &&
            static_cast<std::size_t>(found->second.page) <
                neededPages.size()) {
          neededPages[static_cast<std::size_t>(found->second.page)] = 1;
        }
      };
      for (const char32_t codepoint : request.codepoints) {
        markPage(codepoint);
      }
      // Always keep the pages for the missing-glyph candidates so a fallback
      // substitution can still be rasterized from an actually-declared glyph.
      constexpr std::u32string_view missing =
          U"\u25a1\u25a2\u2610\u25a0?";
      for (const char32_t codepoint : missing) {
        markPage(codepoint);
      }
    }
    struct PendingPage {
      std::size_t page = 0;
      std::string path;
      image_decode::ImageDecodeCoordinator::Ticket ticket = 0;
    };
    std::vector<PendingPage> pendingPages;
    pendingPages.reserve(prepared.font.pagePaths.size());
    bool usableFace = true;
    for (std::size_t pageIndex = 0;
         pageIndex < prepared.font.pagePaths.size(); ++pageIndex) {
      std::string pagePath = prepared.font.pagePaths[pageIndex];
      if (pagePath.empty() && prepared.font.lr2Font) {
        continue;
      }
      auto &preparedPage = prepared.pages[pageIndex];
      preparedPage.physicalKey = "";
      if (!prepared.font.lr2Font &&
          neededPages[pageIndex] == 0) {
        // Page not needed by the requested corpus; leave it absent.
        continue;
      }
      std::ranges::replace(pagePath, '\\', '/');
      const auto combined =
          (std::filesystem::path(face.path).parent_path() / pagePath)
              .lexically_normal()
              .generic_string();
      preparedPage.physicalKey = combined;
      auto decoded = cache.pages.find(combined);
      if (decoded == cache.pages.end()) {
        if (decodeCache != nullptr) {
          if (auto cachedPage = decodeCache->findFontPage(
                  files.revision().lowercaseSha256, combined);
              cachedPage) {
            preparedPage.pixels = *cachedPage;
            // Reconstruct the encoded-bytes charge for this page so a warm
            // run accounts it identically to the cold run that first decoded
            // it. Without this the page would bypass the per-session
            // encoded-budget charge below and a face rejected cold could be
            // accepted warm (inconsistent per-load budgets).
            if (auto encodedBytes = decodeCache->findFontPageEncodedBytes(
                    files.revision().lowercaseSha256, combined);
                encodedBytes) {
              cache.pageEncodedBytes.insert_or_assign(combined,
                                                      *encodedBytes);
            }
            continue;
          }
        }
        const auto read = files.readResolvedResource(
            combined,
            skinResourceLimit(safetyPolicy,
                              SkinResourcePolicy::maximumEncodedBytes));
        if (cancellationRequested()) return std::nullopt;
        if (read.failure) {
          rejectPage("skin.resource.font_missing",
                     "bitmap-font page image is unavailable");
          if (prepared.font.lr2Font) continue;
          usableFace = false;
          break;
        }
        cache.pageEncodedBytes.insert_or_assign(
            combined, read.bytes.size());
        file_checksum::Sha256 digest;
        digest.update(std::span<const std::byte>(read.bytes.data(),
                                                 read.bytes.size()));
        const std::string decodeKey =
            "skin-font:" + files.revision().lowercaseSha256 + ":" +
            combined + ":" + digest.finalHex();
        auto owned = std::make_shared<const std::vector<std::byte>>(
            std::move(read.bytes));
        const auto ticket = coordinator.request(
            {.key = decodeKey,
             .path = {},
             .maximumDimension = skinResourceDimensionLimit(safetyPolicy),
             .maximumEncodedBytes = skinResourceLimit(
                 safetyPolicy, SkinResourcePolicy::maximumEncodedBytes),
             .maximumDecodedBytes = skinResourceLimit(
                 safetyPolicy, SkinResourcePolicy::maximumImageBytes),
             .encoded = std::move(owned)});
        pendingPages.push_back(
            {.page = pageIndex,
             .path = combined,
             .ticket = ticket});
      } else {
        preparedPage.pixels = decoded->second;
      }
    }
    if (!usableFace) {
      for (const auto &pending : pendingPages) coordinator.cancel(pending.ticket);
    }
    for (std::size_t pendingIndex = 0;
         usableFace && pendingIndex < pendingPages.size(); ++pendingIndex) {
      const auto &pending = pendingPages[pendingIndex];
      const auto waited = coordinator.waitTake(pending.ticket, stop);
      if (waited.state == image_decode::ImageDecodeWaitState::Cancelled ||
          waited.state == image_decode::ImageDecodeWaitState::Stopped ||
          cancellationRequested()) {
        for (std::size_t rest = pendingIndex + 1;
             rest < pendingPages.size(); ++rest) {
          coordinator.cancel(pendingPages[rest].ticket);
        }
        return std::nullopt;
      }
      if (waited.state != image_decode::ImageDecodeWaitState::Ready ||
          !waited.image) {
        rejectPage("skin.resource.image_decode_failed",
                   "bitmap-font page image cannot be decoded");
        if (!prepared.font.lr2Font) usableFace = false;
        continue;
      }
      const auto [decoded, _] =
          cache.pages.emplace(pending.path, *waited.image);
      prepared.pages[pending.page].pixels = decoded->second;
    }
    if (!usableFace) {
      for (const auto &pending : pendingPages) coordinator.cancel(pending.ticket);
    }
    for (const auto &page : prepared.pages) {
      if (!usableFace || !page.pixels ||
          cache.accountedPages.contains(page.physicalKey) ||
          requestAccounting.pages.contains(page.physicalKey) ||
          newlyAccountedPages.contains(page.physicalKey)) {
        continue;
      }
      const auto encodedBytes = cache.pageEncodedBytes.find(page.physicalKey);
      if (encodedBytes == cache.pageEncodedBytes.end() ||
          !faceSession.addImage(/*physicalResources=*/0,
                                /*logicalResources=*/0,
                                encodedBytes->second, /*decodedBytes=*/0,
                                /*regions=*/0)) {
        rejectFace("skin.resource.encoded_limit",
                   "bitmap-font page bytes exceed session policy");
        usableFace = false;
        break;
      }
      newlyAccountedPages.insert(page.physicalKey);
    }
    if (!usableFace) {
      if (primary) return std::nullopt;
      continue;
    }
    // Only persist pages into the app-level decode cache after the session
    // encoded-budget accounting above has charged them. Caching earlier (in
    // the waitTake loop) would let a face that is rejected cold on
    // encoded_limit be served warm with no charge, making the same skin+policy
    // pass warm but fail cold.
    if (decodeCache != nullptr) {
      for (const auto &page : prepared.pages) {
        if (!page.pixels || page.physicalKey.empty()) continue;
        std::optional<std::size_t> encodedBytes;
        if (const auto found = cache.pageEncodedBytes.find(page.physicalKey);
            found != cache.pageEncodedBytes.end()) {
          encodedBytes = found->second;
        }
        decodeCache->storeFontPage(files.revision().lowercaseSha256,
                                   page.physicalKey, *page.pixels,
                                   encodedBytes);
      }
    }
    if ((prepared.font.lr2Font ||
         !prepared.font.auxiliaryMetricsComplete) &&
        !prepared.pages.empty()) {
      const auto available = std::ranges::find_if(
          prepared.pages,
          [](const SkinTextAtlasBitmapPage &page) {
            return page.pixels.has_value();
          });
      if (available != prepared.pages.end()) {
        prepared.font.pageWidth = available->pixels->width;
        prepared.font.pageHeight = available->pixels->height;
      }
    }
    session = faceSession;
    requestAccounting.descriptors.insert(newlyAccountedDescriptors.begin(),
                                         newlyAccountedDescriptors.end());
    requestAccounting.pages.insert(newlyAccountedPages.begin(),
                                   newlyAccountedPages.end());
    result.push_back(std::move(prepared));
  }
  return result.empty() ? std::nullopt
                        : std::optional{std::move(result)};
}

std::optional<SkinTextAtlasBuildResult> prepareFontAtlas(
    SkinTextAtlasId id, const FontAtlasRequest &request,
    const LuaSkinFileSystem &files, SkinResourceSessionAccounting &session,
    std::vector<SkinDiagnostic> &diagnostics,
    const std::function<bool()> &cancellationRequested,
    const SkinSafetyPolicy &safetyPolicy, std::stop_token stop,
    BitmapFontPreparationCache &cache,
    BitmapFontAccountingIdentities &requestAccounting,
    std::size_t &remainingScalableFontPaintAttemptWork,
    image_decode::ImageDecodeCoordinator &coordinator,
    SkinDecodeCache *decodeCache,
    SkinResourcePreparationService *textAtlasCacheOwner) {
  if (request.resolvedFaces.empty()) return std::nullopt;
  if (request.font.bitmap) {
    const auto faces = readBitmapFontFaces(
        request, files, session, diagnostics, cancellationRequested,
        safetyPolicy, stop, cache, requestAccounting, coordinator,
        decodeCache);
    if (!faces) return std::nullopt;
    return buildSkinBitmapTextAtlas(id, request.key, *faces,
                                    request.codepoints, request.pairs,
                                    safetyPolicy);
  }
  const std::string contentKey = scalableTextAtlasContentKey(
      request.key, request.codepoints, request.pairs, safetyPolicy);
  if (textAtlasCacheOwner != nullptr) {
    if (auto cached = textAtlasCacheOwner->findCachedTextAtlas(
            files.revision().lowercaseSha256, contentKey);
        cached) {
      SkinTextAtlasBuildResult result;
      result.atlas = *cached;
      result.atlas->id = id;
      return result;
    }
  }
  const auto faces = readFontFaces(request, files, session, diagnostics,
                                   cancellationRequested, safetyPolicy);
  if (!faces) return std::nullopt;
  const auto reservePaintAttemptWork =
      [&remainingScalableFontPaintAttemptWork](std::size_t work) {
        if (work > remainingScalableFontPaintAttemptWork) return false;
        remainingScalableFontPaintAttemptWork -= work;
        return true;
      };
  ScalableGlyphCacheAccessor glyphCache;
  if (textAtlasCacheOwner != nullptr) {
    const std::string revisionKey = files.revision().lowercaseSha256;
    const SkinTextAtlasKey glyphKeyBase = request.key;
    const SkinSafetyPolicy glyphPolicy = safetyPolicy;
    glyphCache.find =
        [textAtlasCacheOwner, revisionKey, glyphKeyBase, glyphPolicy](
            char32_t codepoint) {
          return textAtlasCacheOwner->findCachedGlyph(
              revisionKey,
              scalableGlyphKey(glyphKeyBase, codepoint, glyphPolicy));
        };
    glyphCache.store =
        [textAtlasCacheOwner, revisionKey, glyphKeyBase, glyphPolicy](
            char32_t codepoint, SkinPreparedGlyphBitmap glyph) {
          textAtlasCacheOwner->storeCachedGlyph(
              revisionKey,
              scalableGlyphKey(glyphKeyBase, codepoint, glyphPolicy),
              std::move(glyph));
        };
  }
  auto built = buildSkinTextAtlas(id, request.key, *faces, request.codepoints,
                                  request.pairs, safetyPolicy,
                                  std::min(
                                      session.remainingScalableFontPaintBlendOperations(),
                                      remainingScalableFontPaintAttemptWork),
                                  cancellationRequested, reservePaintAttemptWork,
                                  textAtlasCacheOwner != nullptr ? &glyphCache
                                                                 : nullptr);
  if (built.atlas && textAtlasCacheOwner != nullptr) {
    textAtlasCacheOwner->storeCachedTextAtlas(
        files.revision().lowercaseSha256, contentKey, *built.atlas);
  }
  return built;
}


struct AtlasAccountingDelta {
  std::size_t decodedBytes = 0;
  std::size_t physicalResources = 0;
  std::vector<std::string> bitmapPageKeys;
};

std::optional<AtlasAccountingDelta> atlasAccountingDelta(
    const SkinPreparedGlyphAtlas &atlas,
    const std::set<std::string, std::less<>> &accountedBitmapPages) {
  if (!atlas.bitmapFont) {
    if (atlas.pages.empty()) {
      return AtlasAccountingDelta{.decodedBytes = atlas.pixels.byteSize(),
                                  .physicalResources = 1};
    }
    AtlasAccountingDelta result;
    for (const auto &page : atlas.pages) {
      if (!page.pixels ||
          page.pixels->byteSize() >
              std::numeric_limits<std::size_t>::max() - result.decodedBytes) {
        return std::nullopt;
      }
      result.decodedBytes += page.pixels->byteSize();
      ++result.physicalResources;
    }
    return result;
  }
  AtlasAccountingDelta result;
  std::set<std::string, std::less<>> local;
  for (const auto &page : atlas.pages) {
    if (!page.pixels || accountedBitmapPages.contains(page.physicalKey) ||
        !local.insert(page.physicalKey).second) {
      continue;
    }
    if (page.pixels->byteSize() >
        std::numeric_limits<std::size_t>::max() - result.decodedBytes) {
      return std::nullopt;
    }
    result.decodedBytes += page.pixels->byteSize();
    ++result.physicalResources;
    result.bitmapPageKeys.push_back(page.physicalKey);
  }
  return result;
}
}

void appendScalableBigEndian(std::string &serialized, std::uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    serialized.push_back(
        static_cast<char>((value >> static_cast<unsigned>(shift)) & 0xffU));
  }
}

void appendScalableAtlasKeyFields(std::string &serialized,
                                  const SkinTextAtlasKey &key,
                                  const SkinSafetyPolicy &safetyPolicy) {
  serialized.append("ASOBMASHOW-SCALABLE-ATLAS-V1");
  appendScalableBigEndian(serialized, key.font);
  appendScalableBigEndian(serialized, key.pointSize);
  appendScalableBigEndian(serialized, std::bit_cast<std::uint64_t>(key.outlineWidth));
  appendScalableBigEndian(serialized, std::bit_cast<std::uint64_t>(key.shadowOffsetX));
  appendScalableBigEndian(serialized, std::bit_cast<std::uint64_t>(key.shadowOffsetY));
  appendScalableBigEndian(serialized, std::bit_cast<std::uint64_t>(key.shadowSmoothness));
  serialized.push_back(static_cast<char>(key.outlineRgba[0]));
  serialized.push_back(static_cast<char>(key.outlineRgba[1]));
  serialized.push_back(static_cast<char>(key.outlineRgba[2]));
  serialized.push_back(static_cast<char>(key.outlineRgba[3]));
  serialized.push_back(static_cast<char>(key.shadowRgba[0]));
  serialized.push_back(static_cast<char>(key.shadowRgba[1]));
  serialized.push_back(static_cast<char>(key.shadowRgba[2]));
  serialized.push_back(static_cast<char>(key.shadowRgba[3]));
  appendScalableBigEndian(serialized, key.fallbackChainDigest.size());
  serialized.append(key.fallbackChainDigest);
  serialized.push_back(
      safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit) ? '1'
                                                                     : '0');
}

std::string scalableTextAtlasContentKey(
    const SkinTextAtlasKey &key, const std::set<char32_t> &codepoints,
    const std::set<std::pair<char32_t, char32_t>> &pairs,
    const SkinSafetyPolicy &safetyPolicy) {
  std::string serialized;
  appendScalableAtlasKeyFields(serialized, key, safetyPolicy);
  for (const char32_t codepoint : codepoints) {
    appendScalableBigEndian(serialized, codepoint);
  }
  for (const auto &[left, right] : pairs) {
    appendScalableBigEndian(serialized, left);
    appendScalableBigEndian(serialized, right);
  }
  return file_checksum::sha256(serialized);
}

std::string scalableGlyphKey(const SkinTextAtlasKey &key, char32_t codepoint,
                             const SkinSafetyPolicy &safetyPolicy) {
  std::string serialized;
  appendScalableAtlasKeyFields(serialized, key, safetyPolicy);
  appendScalableBigEndian(serialized, codepoint);
  return serialized;
}


#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
void resetSkinResourceRegionIdentityChecksForTesting() noexcept {
  regionIdentityChecksForTesting.store(0, std::memory_order_relaxed);
}

std::size_t skinResourceRegionIdentityChecksForTesting() noexcept {
  return regionIdentityChecksForTesting.load(std::memory_order_relaxed);
}

void resetSkinResourceRegionLookupComparisonsForTesting() noexcept {
  regionLookupComparisonsForTesting.store(0, std::memory_order_relaxed);
}

std::size_t skinResourceRegionLookupComparisonsForTesting() noexcept {
  return regionLookupComparisonsForTesting.load(std::memory_order_relaxed);
}

void resetSkinResourceFontAtlasRequestHighWaterForTesting() noexcept {
  fontAtlasRequestHighWaterForTesting.store(0, std::memory_order_relaxed);
}

std::size_t skinResourceFontAtlasRequestHighWaterForTesting() noexcept {
  return fontAtlasRequestHighWaterForTesting.load(std::memory_order_relaxed);
}

void resetSkinResourcePlatformAssetReadsForTesting() noexcept {
  platformAssetReadsForTesting.store(0, std::memory_order_relaxed);
}

std::size_t skinResourcePlatformAssetReadsForTesting() noexcept {
  return platformAssetReadsForTesting.load(std::memory_order_relaxed);
}

void setSkinResourceAccountingLimitsForTesting(
    std::size_t maximumSessionEncodedBytes,
    std::size_t maximumAtlasSessionBytes) noexcept {
  maximumSessionEncodedBytesForTesting.store(maximumSessionEncodedBytes,
                                              std::memory_order_relaxed);
  maximumAtlasSessionBytesForTesting.store(maximumAtlasSessionBytes,
                                            std::memory_order_relaxed);
  committedEncodedBytesForTesting.store(0, std::memory_order_relaxed);
}

void resetSkinResourceAccountingLimitsForTesting() noexcept {
  maximumSessionEncodedBytesForTesting.store(
      std::numeric_limits<std::size_t>::max(), std::memory_order_relaxed);
  maximumAtlasSessionBytesForTesting.store(
      std::numeric_limits<std::size_t>::max(), std::memory_order_relaxed);
  committedEncodedBytesForTesting.store(0, std::memory_order_relaxed);
}

std::size_t skinResourceCommittedEncodedBytesForTesting() noexcept {
  return committedEncodedBytesForTesting.load(std::memory_order_relaxed);
}

void resetSkinImageAppCacheHitsForTesting() noexcept {
  skinImageAppCacheHitCountForTesting.store(0, std::memory_order_relaxed);
}

std::size_t skinImageAppCacheHitsForTesting() noexcept {
  return skinImageAppCacheHitCountForTesting.load(std::memory_order_relaxed);
}
#endif

bool skinResourceDimensionsAllowed(int width, int height, std::size_t bytes,
                                   SkinSafetyPolicy safetyPolicy) noexcept {
  if (width <= 0 || height <= 0 ||
      width > skinResourceDimensionLimit(safetyPolicy) ||
      height > skinResourceDimensionLimit(safetyPolicy)) {
    return false;
  }
  const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  return pixels <= std::numeric_limits<std::size_t>::max()/4U &&
         bytes == static_cast<std::size_t>(pixels) * 4U &&
         bytes <= skinResourceLimit(safetyPolicy,
                                    SkinResourcePolicy::maximumImageBytes) &&
         bytes <= UINT32_MAX;
}

namespace {
bool addWithin(std::size_t &value, std::size_t increment,
               std::size_t maximum) noexcept {
  if (increment > maximum - value) return false;
  value += increment;
  return true;
}

std::size_t maximumSessionEncodedBytes(
    const SkinSafetyPolicy &safetyPolicy) noexcept {
  std::size_t maximum = skinResourceLimit(
      safetyPolicy, SkinResourcePolicy::maximumSessionEncodedBytes);
#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
  maximum = std::min(
      maximum,
      maximumSessionEncodedBytesForTesting.load(std::memory_order_relaxed));
#endif
  return maximum;
}

std::size_t maximumAtlasSessionBytes(
    const SkinSafetyPolicy &safetyPolicy) noexcept {
  std::size_t maximum = skinResourceLimit(
      safetyPolicy, SkinResourcePolicy::maximumAtlasSessionBytes);
#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
  maximum = std::min(
      maximum,
      maximumAtlasSessionBytesForTesting.load(std::memory_order_relaxed));
#endif
  return maximum;
}
}

bool SkinResourceSessionAccounting::addImage(
    std::size_t physicalResources, std::size_t logicalResources,
    std::size_t encodedBytes, std::size_t decodedBytes,
    std::size_t regions) noexcept {
  SkinResourceSessionAccounting next = *this;
  if (!addWithin(next.physicalResources_, physicalResources,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumResources)) ||
      !addWithin(next.logicalResources_, logicalResources,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumResources)) ||
      !addWithin(next.encodedBytes_, encodedBytes,
                 maximumSessionEncodedBytes(safetyPolicy_)) ||
      !addWithin(next.decodedBytes_, decodedBytes,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumSessionDecodedBytes)) ||
      !addWithin(next.regions_, regions,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumRegions))) {
    return false;
  }
  *this = next;
  return true;
}

bool SkinResourceSessionAccounting::addAtlas(
    std::size_t decodedBytes, std::size_t glyphs,
    std::size_t kerningPairs, std::size_t physicalResources,
    std::size_t scalableFontPaintBlendOperations) noexcept {
  SkinResourceSessionAccounting next = *this;
  if (!addWithin(next.physicalResources_, physicalResources,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumResources)) ||
      !addWithin(next.atlases_, 1,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumAtlases)) ||
      !addWithin(next.atlasBytes_, decodedBytes,
                 maximumAtlasSessionBytes(safetyPolicy_)) ||
      !addWithin(next.decodedBytes_, decodedBytes,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumSessionDecodedBytes)) ||
      !addWithin(next.glyphs_, glyphs,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumGlyphs)) ||
      !addWithin(next.kerningPairs_, kerningPairs,
                 skinResourceLimit(safetyPolicy_,
                                   SkinResourcePolicy::maximumKerningPairs)) ||
      !addWithin(next.scalableFontPaintBlendOperations_,
                 scalableFontPaintBlendOperations,
                 skinResourceLimit(
                     safetyPolicy_,
                     SkinResourcePolicy::
                         maximumScalableFontPaintBlendOperations))) {
    return false;
  }
  *this = next;
  return true;
}

bool skinResourceResolveRect(const SkinSourceRect &a, int iw, int ih, SkinSourceRect &r) noexcept {
  if (iw <= 0 || ih <= 0) return false;
  const int w = a.w == -1 ? iw : a.w, h = a.h == -1 ? ih : a.h;
  const int cols = a.gridColumns <= 0 ? 1 : a.gridColumns, rows = a.gridRows <= 0 ? 1 : a.gridRows;
  if (a.gridColumn < 0 || a.gridRow < 0 || a.gridColumn >= cols || a.gridRow >= rows) return false;
  const int cw = w / cols, ch = h / rows;
  const auto resolvedX = static_cast<std::int64_t>(a.x) +
                         static_cast<std::int64_t>(cw) * a.gridColumn;
  const auto resolvedY = static_cast<std::int64_t>(a.y) +
                         static_cast<std::int64_t>(ch) * a.gridRow;
  if (resolvedX < std::numeric_limits<int>::min() ||
      resolvedX > std::numeric_limits<int>::max() ||
      resolvedY < std::numeric_limits<int>::min() ||
      resolvedY > std::numeric_limits<int>::max()) {
    return false;
  }
  r = a; r.x = static_cast<int>(resolvedX); r.y = static_cast<int>(resolvedY); r.w = cw; r.h = ch; r.gridColumn=0; r.gridRow=0; r.gridColumns=1; r.gridRows=1; return true;
}
SkinResourceCatalog::SkinResourceCatalog(
    SkinRevisionLease &&revision, std::shared_ptr<SkinTextureDevice> device,
    std::shared_ptr<SkinLiveResourceCounters> liveCounters)
    : revision_(std::move(revision)), device_(std::move(device)),
      liveCounters_(std::move(liveCounters)),
      owner_(std::this_thread::get_id()) {}
SkinResourceCatalog::~SkinResourceCatalog() {
  if (!device_) return;
  if (!device_->ownsCurrentThread() || std::this_thread::get_id() != owner_) {
    std::terminate();
  }
  for (const auto &item : owned_) {
    if (!bgfx::isValid(item.handle)) continue;
    device_->destroy(item.handle);
    if (liveCounters_) liveCounters_->textureDestroyed();
  }
  if (liveResourceCounted_ && liveCounters_) {
    liveCounters_->resourceDestroyed();
  }
}
SkinResourceUploadResult SkinResourceCatalog::upload(
    SkinResourceUploadPlan &&plan, std::shared_ptr<SkinTextureDevice> device,
    std::shared_ptr<SkinLiveResourceCounters> liveCounters) {
  SkinResourceUploadResult result;
  const SkinSafetyPolicy safetyPolicy = plan.safetyPolicy;
  if (!device || !device->ownsCurrentThread()) { result.diagnostics.push_back(diagnostic("skin.resource.render_thread_violation", "resource upload requires the render owner thread")); return result; }
  const int reportedDeviceMaximumDimension =
      device->maximumTextureDimension(safetyPolicy);
  if (reportedDeviceMaximumDimension <= 0) {
    result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "texture device reports an invalid maximum dimension")); return result;
  }
  const int deviceMaximumDimension = reportedDeviceMaximumDimension;
  if (plan.decodedBytes > skinResourceLimit(
                              safetyPolicy,
                              SkinResourcePolicy::maximumSessionDecodedBytes)) {
    result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan exceeds fixed limits"));
    return result;
  }
  std::set<SkinResourceId> imageIds;
  std::set<SkinTextAtlasId> atlasIds;
  std::set<SkinTextAtlasKey> atlasKeys;
  SkinResourceSessionAccounting session(safetyPolicy);
  const auto canonicalRegion = [](const SkinSourceRect &region, int width, int height) {
    SkinSourceRect resolved;
    return skinResourceResolveRect(region, width, height, resolved) &&
           resolved.x == region.x && resolved.y == region.y && resolved.w == region.w && resolved.h == region.h &&
           resolved.gridColumn == region.gridColumn && resolved.gridRow == region.gridRow &&
           resolved.gridColumns == region.gridColumns && resolved.gridRows == region.gridRows;
  };
  for (const auto &image : plan.images) {
    if (image.id == 0 || !imageIds.insert(image.id).second ||
        imageIds.size() > skinResourceLimit(
                              safetyPolicy,
                              SkinResourcePolicy::maximumResources) ||
        image.aliases.size() >= skinResourceLimit(
                                    safetyPolicy,
                                    SkinResourcePolicy::maximumResources)) {
      result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has duplicate image IDs")); return result;
    }
    for (SkinResourceId alias : image.aliases) {
      if (alias == 0 || !imageIds.insert(alias).second ||
          imageIds.size() > skinResourceLimit(
                                safetyPolicy,
                                SkinResourcePolicy::maximumResources)) {
        result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has duplicate image aliases")); return result;
      }
    }
    if (!skinResourceDimensionsAllowed(image.pixels.width, image.pixels.height,
                                       image.pixels.byteSize(), safetyPolicy) ||
        image.pixels.width > deviceMaximumDimension || image.pixels.height > deviceMaximumDimension ||
        image.pixels.byteSize() > skinResourceLimit(
                                      safetyPolicy,
                                      SkinResourcePolicy::maximumSessionDecodedBytes)) {
      result.diagnostics.push_back(diagnostic("skin.resource.image_dimensions", "resource upload plan has invalid image dimensions or bytes")); return result;
    }
    if (image.regions.size() > skinResourceLimit(
                                   safetyPolicy,
                                   SkinResourcePolicy::maximumRegions)) { result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource region count exceeds fixed limit")); return result; }
    for (const auto &region : image.regions) if (!canonicalRegion(region, image.pixels.width, image.pixels.height)) { result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has invalid image region")); return result; }
    if (image.regionMappings.size() != image.regions.size()) { result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan is missing stable region identities")); return result; }
    RegionIdentityMap identities;
    for (std::size_t index = 0; index < image.regionMappings.size(); ++index) {
      SkinSourceRect resolvedAuthored;
      const SkinResolvedRegion &mapping = image.regionMappings[index];
      const auto [existing, inserted] = identities.emplace(mapping.authored,
                                                            mapping.resolved);
      if (!skinResourceResolveRect(image.regionMappings[index].authored,
                                   image.pixels.width, image.pixels.height,
                                   resolvedAuthored) ||
          !canonicalRegion(image.regionMappings[index].resolved, image.pixels.width, image.pixels.height) ||
          !sameRect(resolvedAuthored, image.regionMappings[index].resolved) ||
          !sameRect(image.regionMappings[index].resolved, image.regions[index]) ||
          (!inserted && !sameRect(existing->second, mapping.resolved))) {
        result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has conflicting region identities")); return result;
      }
    }
    std::size_t imageRegions = image.regions.size();
    for (SkinResourceId alias : image.aliases) {
      const auto aliasRegions = image.aliasRegions.find(alias);
      const std::size_t count = aliasRegions == image.aliasRegions.end()
          ? image.regions.size() : aliasRegions->second.size();
      const std::size_t maximumRegions = skinResourceLimit(
          safetyPolicy, SkinResourcePolicy::maximumRegions);
      if (count > maximumRegions || count > maximumRegions - imageRegions) {
        result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource region count exceeds fixed limit")); return result;
      }
      if (aliasRegions != image.aliasRegions.end()) for (const auto &region : aliasRegions->second)
        if (!canonicalRegion(region, image.pixels.width, image.pixels.height)) { result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has invalid alias region")); return result; }
      const auto aliasMappings = image.aliasRegionMappings.find(alias);
      if (aliasMappings == image.aliasRegionMappings.end() || aliasMappings->second.size() != count) { result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan is missing alias region identities")); return result; }
      RegionIdentityMap identities;
      for (std::size_t index = 0; index < aliasMappings->second.size(); ++index) {
        SkinSourceRect resolvedAuthored;
        const SkinResolvedRegion &mapping = aliasMappings->second[index];
        const auto [existing, inserted] = identities.emplace(mapping.authored,
                                                              mapping.resolved);
        if (!skinResourceResolveRect(aliasMappings->second[index].authored,
                                     image.pixels.width, image.pixels.height,
                                     resolvedAuthored) ||
            !canonicalRegion(aliasMappings->second[index].resolved, image.pixels.width, image.pixels.height) ||
            !sameRect(resolvedAuthored, aliasMappings->second[index].resolved) ||
            !sameRect(aliasMappings->second[index].resolved, aliasRegions == image.aliasRegions.end() ? image.regions[index] : aliasRegions->second[index]) ||
            (!inserted && !sameRect(existing->second, mapping.resolved))) {
          result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has conflicting alias region identities")); return result;
        }
      }
      imageRegions += count;
    }
    for (const auto &[alias, aliasRegions] : image.aliasRegions) {
      if (std::find(image.aliases.begin(), image.aliases.end(), alias) == image.aliases.end() ||
          aliasRegions.size() > skinResourceLimit(
                                    safetyPolicy,
                                    SkinResourcePolicy::maximumRegions)) {
        result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has invalid alias regions")); return result;
      }
    }
    for (const auto &[alias, aliasMappings] : image.aliasRegionMappings) {
      (void)aliasMappings;
      if (std::find(image.aliases.begin(), image.aliases.end(), alias) == image.aliases.end()) {
        result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has extra alias region identities")); return result;
      }
    }
    if (!session.addImage(/*physicalResources=*/1,
                          /*logicalResources=*/1 + image.aliases.size(),
                          /*encodedBytes=*/0, image.pixels.byteSize(),
                          imageRegions)) {
      result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan exceeds fixed aggregate limits")); return result;
    }
  }
  for (const auto &[reference, resource] : plan.builtinImageResources) {
    const bool recognized = reference == 100 || reference == 101 ||
                            reference == 102 || reference == 110 ||
                            reference == 111;
    if (!recognized || !imageIds.contains(resource)) {
      result.diagnostics.push_back(diagnostic(
          "skin.resource.texture_create_failed",
          "resource upload plan has an invalid built-in image mapping"));
      return result;
    }
  }
  std::map<std::string, const image_decode::DecodedImageData *, std::less<>>
      bitmapPhysicalPages;
  for (const auto &atlas : plan.atlases) {
    const auto finite = [safetyPolicy](double value) {
      return std::isfinite(value) &&
             (!safetyPolicy.enforces(SkinSafetyGuard::ResourceAllocationLimit) ||
              std::abs(value) <= 4096.0);
    };
    if (atlas.id == 0 || !finite(atlas.key.outlineWidth) || !finite(atlas.key.shadowOffsetX) ||
        !finite(atlas.key.shadowOffsetY) || !finite(atlas.key.shadowSmoothness) ||
        !atlasIds.insert(atlas.id).second || !atlasKeys.insert(atlas.key).second ||
        atlas.glyphs.size() > skinResourceLimit(
                                 safetyPolicy,
                                 SkinResourcePolicy::maximumGlyphs) ||
        atlas.kerning.size() > skinResourceLimit(
                                 safetyPolicy,
                                 SkinResourcePolicy::maximumKerningPairs)) {
      result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has invalid or duplicate atlas data")); return result;
    }
    std::size_t atlasDecodedBytes = 0;
    std::size_t atlasPhysicalResources = 0;
    if (!atlas.bitmapFont) {
      if (atlas.pages.empty()) {
        if (!skinResourceDimensionsAllowed(atlas.pixels.width,
                                           atlas.pixels.height,
                                           atlas.pixels.byteSize(),
                                           safetyPolicy) ||
            atlas.pixels.width > deviceMaximumDimension ||
            atlas.pixels.height > deviceMaximumDimension ||
            atlas.pixels.byteSize() > skinResourceLimit(
                                          safetyPolicy,
                                          SkinResourcePolicy::maximumAtlasBytes)) {
          result.diagnostics.push_back(diagnostic(
              "skin.resource.atlas_limit",
              "resource upload plan has invalid scalable atlas pixels"));
          return result;
        }
        atlasDecodedBytes = atlas.pixels.byteSize();
        atlasPhysicalResources = 1;
      } else {
        if (atlas.pixels.width != 0 || atlas.pixels.height != 0 ||
            atlas.pixels.rgba) {
          result.diagnostics.push_back(diagnostic(
              "skin.resource.atlas_limit",
              "paged scalable atlas also contains legacy pixels"));
          return result;
        }
        for (const auto &page : atlas.pages) {
          if (!page.pixels || page.physicalKey.empty() ||
              !skinResourceDimensionsAllowed(
                  page.pixels->width, page.pixels->height,
                  page.pixels->byteSize(), safetyPolicy) ||
              page.pixels->width > deviceMaximumDimension ||
              page.pixels->height > deviceMaximumDimension ||
              page.pixels->byteSize() >
                  std::numeric_limits<std::size_t>::max() -
                      atlasDecodedBytes) {
            result.diagnostics.push_back(diagnostic(
                "skin.resource.atlas_limit",
                "resource upload plan has an invalid scalable atlas page"));
            return result;
          }
          atlasDecodedBytes += page.pixels->byteSize();
          ++atlasPhysicalResources;
        }
      }
    } else {
      if (atlas.pixels.width != 0 || atlas.pixels.height != 0 ||
          atlas.pixels.rgba || atlas.paintBlendOperations != 0 ||
          atlas.pages.size() > skinResourceLimit(
                                   safetyPolicy,
                                   SkinResourcePolicy::maximumResources)) {
        result.diagnostics.push_back(diagnostic(
            "skin.resource.atlas_limit",
            "bitmap atlas must retain bounded separate pages"));
        return result;
      }
      for (const auto &page : atlas.pages) {
        if (page.bitmapFontType < 0 || page.bitmapFontType > 2) {
          result.diagnostics.push_back(diagnostic(
              "skin.resource.atlas_limit",
              "bitmap atlas page has an invalid face type"));
          return result;
        }
        if (!page.pixels) {
          continue;
        }
        if (page.physicalKey.empty() ||
            !skinResourceDimensionsAllowed(page.pixels->width,
                                           page.pixels->height,
                                           page.pixels->byteSize(),
                                           safetyPolicy) ||
            page.pixels->width > deviceMaximumDimension ||
            page.pixels->height > deviceMaximumDimension) {
          result.diagnostics.push_back(diagnostic(
              "skin.resource.atlas_limit",
              "bitmap atlas page exceeds texture policy"));
          return result;
        }
        const auto [existing, inserted] =
            bitmapPhysicalPages.emplace(page.physicalKey, &*page.pixels);
        if (!inserted) {
          const auto &prior = *existing->second;
          if (prior.width != page.pixels->width ||
              prior.height != page.pixels->height ||
              prior.rgba != page.pixels->rgba) {
            result.diagnostics.push_back(diagnostic(
                "skin.resource.atlas_limit",
                "aliased bitmap pages have conflicting pixels"));
            return result;
          }
          continue;
        }
        if (page.pixels->byteSize() >
            std::numeric_limits<std::size_t>::max() - atlasDecodedBytes) {
          result.diagnostics.push_back(diagnostic(
              "skin.resource.atlas_limit",
              "bitmap atlas page bytes overflow accounting"));
          return result;
        }
        atlasDecodedBytes += page.pixels->byteSize();
        ++atlasPhysicalResources;
      }
    }
    SkinTextAtlasKey canonicalKey = atlas.key;
    if (!canonicalizeSkinTextAtlasKey(canonicalKey, safetyPolicy) || canonicalKey != atlas.key ||
        atlas.lineHeight <= 0 || atlas.lineHeight > skinResourceDimensionLimit(safetyPolicy) ||
        std::llabs(static_cast<long long>(atlas.ascent)) > skinResourceDimensionLimit(safetyPolicy) ||
        atlas.capHeight <= 0 ||
        atlas.capHeight > skinResourceDimensionLimit(safetyPolicy) ||
        std::llabs(static_cast<long long>(atlas.descent)) > skinResourceDimensionLimit(safetyPolicy) ||
        (atlas.bitmapFont &&
         (atlas.bitmapFontType < 0 || atlas.bitmapFontType > 2 ||
          atlas.originalSize <= 0 || atlas.pageWidth <= 0 ||
          atlas.pageHeight <= 0 ||
          atlas.originalSize > skinResourceDimensionLimit(safetyPolicy) ||
          atlas.pageWidth > skinResourceDimensionLimit(safetyPolicy) ||
          atlas.pageHeight > skinResourceDimensionLimit(safetyPolicy) ||
          (atlas.layoutKind != SkinTextLayoutKind::Bitmap &&
           atlas.layoutKind != SkinTextLayoutKind::Lr2Image) ||
          std::llabs(static_cast<long long>(atlas.margin)) >
              skinResourceDimensionLimit(safetyPolicy))) ||
        (!atlas.bitmapFont &&
         (atlas.bitmapFontType != 0 || atlas.originalSize != 0 ||
          atlas.pageWidth != 0 || atlas.pageHeight != 0 ||
          atlas.layoutKind != SkinTextLayoutKind::Scalable ||
          atlas.margin != 0))) {
      result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has noncanonical atlas data")); return result;
    }
    for (const auto &[codepoint, metric] : atlas.glyphs) {
      (void)codepoint;
      bool validRegion = false;
      if (atlas.bitmapFont) {
        validRegion = metric.page < atlas.pages.size() &&
                      atlas.pages[metric.page].pixels &&
                      canonicalRegion(metric.region,
                                      atlas.pages[metric.page].pixels->width,
                                      atlas.pages[metric.page].pixels->height);
      } else if (atlas.pages.empty()) {
        validRegion = metric.page == 0 &&
                      canonicalRegion(metric.region, atlas.pixels.width,
                                      atlas.pixels.height);
      } else {
        validRegion = metric.page < atlas.pages.size() &&
                      atlas.pages[metric.page].pixels &&
                      canonicalRegion(metric.region,
                                      atlas.pages[metric.page].pixels->width,
                                      atlas.pages[metric.page].pixels->height);
      }
      if (!validRegion ||
          std::llabs(static_cast<long long>(metric.bearingX)) > skinResourceDimensionLimit(safetyPolicy) ||
          std::llabs(static_cast<long long>(metric.bearingY)) > skinResourceDimensionLimit(safetyPolicy) ||
          std::llabs(static_cast<long long>(metric.advance)) > skinResourceDimensionLimit(safetyPolicy) ||
          std::llabs(static_cast<long long>(metric.layoutOffsetY)) > skinResourceDimensionLimit(safetyPolicy) ||
          metric.bitmapFontType < -1 || metric.bitmapFontType > 2 ||
          (atlas.bitmapFont && metric.bitmapFontType < 0) ||
          (!atlas.bitmapFont && metric.bitmapFontType != -1)) { result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has invalid glyph region")); return result; }
    }
    for (const auto &[pair, amount] : atlas.kerning) {
      if (!atlas.glyphs.contains(pair.first) || !atlas.glyphs.contains(pair.second) ||
          std::llabs(static_cast<long long>(amount)) > skinResourceDimensionLimit(safetyPolicy)) { result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has invalid kerning pair")); return result; }
    }
    if (!session.addAtlas(atlasDecodedBytes, atlas.glyphs.size(),
                          atlas.kerning.size(), atlasPhysicalResources,
                          atlas.paintBlendOperations)) {
      result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan exceeds fixed aggregate limits")); return result;
    }
  }
  if (plan.textAtlasesByObject.size() > skinResourceLimit(
                                         safetyPolicy,
                                         SkinResourcePolicy::maximumTextAtlasUses)) {
    result.diagnostics.push_back(diagnostic(
        "skin.resource.atlas_limit",
        "resource upload plan has too many text-atlas uses"));
    return result;
  }
  for (const auto &[object, atlas] : plan.textAtlasesByObject) {
    if (object == 0 || atlas == 0 || !atlasIds.contains(atlas)) {
      result.diagnostics.push_back(diagnostic(
          "skin.resource.atlas_limit",
          "resource upload plan has an invalid text-atlas mapping"));
      return result;
    }
  }
  std::set<SkinObjectId> pomyuObjects;
  for (const auto &pomyu : plan.pomyuCharas) {
    if (pomyu.object == 0 || !pomyuObjects.insert(pomyu.object).second ||
        pomyu.animations.empty() ||
        (pomyu.relativePlacement &&
         (pomyu.coordinateWidth <= 0 || pomyu.coordinateHeight <= 0)) ||
        (!pomyu.relativePlacement &&
         (pomyu.coordinateWidth != 0 || pomyu.coordinateHeight != 0))) {
      result.diagnostics.push_back(diagnostic(
          "skin.resource.pomyu_invalid",
          "resource upload plan has invalid Pomyu character metadata"));
      return result;
    }
    for (const auto &animation : pomyu.animations) {
      if (animation.frames.empty() ||
          animation.loopStartFrame >= animation.frames.size() ||
          ((animation.frameMillis == 0 || animation.cycleMillis == 0) &&
           (animation.frameMillis != 0 || animation.cycleMillis != 0 ||
            animation.frames.size() != 1)) ||
          animation.frameMillis < 0 || animation.cycleMillis < 0 ||
          (animation.builtinTimerId &&
           (*animation.builtinTimerId < 900 ||
            *animation.builtinTimerId > 909))) {
        result.diagnostics.push_back(diagnostic(
            "skin.resource.pomyu_invalid",
            "resource upload plan has invalid Pomyu animation metadata"));
        return result;
      }
      for (const auto &frame : animation.frames) {
        if (frame.resource != 0 && !imageIds.contains(frame.resource)) {
          result.diagnostics.push_back(diagnostic(
              "skin.resource.pomyu_invalid",
              "Pomyu animation references an absent prepared image"));
          return result;
        }
      }
    }
  }
  if (std::ranges::any_of(plan.pomyuMotionCyclesMillis,
                          [](int cycle) { return cycle < 1; })) {
    result.diagnostics.push_back(diagnostic(
        "skin.resource.pomyu_invalid",
        "resource upload plan has invalid Pomyu motion cycles"));
    return result;
  }
  if (session.decodedBytes() != plan.decodedBytes) {
    result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan decoded byte total is inconsistent")); return result;
  }
  try {
  auto catalog=std::unique_ptr<SkinResourceCatalog>(new SkinResourceCatalog(std::move(plan.revision), std::move(device), std::move(liveCounters)));
  catalog->safetyPolicy_ = safetyPolicy;
  catalog->owned_.reserve(plan.images.size() + plan.atlases.size());
  auto rollback=[&]{ catalog.reset(); };
  struct PendingHandle { SkinTextureDevice &device; bgfx::TextureHandle handle = BGFX_INVALID_HANDLE; ~PendingHandle() { if (bgfx::isValid(handle)) device.destroy(handle); } void release() noexcept { handle = BGFX_INVALID_HANDLE; } };
  const auto prepareResource = [](SkinResourceId id,
                                  bgfx::TextureHandle texture, int width,
                                  int height,
                                  const std::vector<SkinSourceRect> &regions,
                                  const std::vector<SkinResolvedRegion> &mappings) {
    PreparedSkinResource prepared{.id = id,
                                  .texture = texture,
                                  .width = width,
                                  .height = height,
                                  .regions = regions,
                                  .regionMappings = mappings};
    prepared.regionLookupOrder.resize(mappings.size());
    for (std::size_t index = 0; index < mappings.size(); ++index) {
      prepared.regionLookupOrder[index] = static_cast<std::uint32_t>(index);
    }
    std::sort(prepared.regionLookupOrder.begin(),
              prepared.regionLookupOrder.end(),
              [&prepared](std::uint32_t left, std::uint32_t right) {
                return lessRect(prepared.regionMappings[left].authored,
                                prepared.regionMappings[right].authored);
              });
    return prepared;
  };
  for (const auto &image : plan.images) {
    const auto handle = catalog->device_->create(image.pixels, safetyPolicy);
    if (!bgfx::isValid(handle)) {
      result.diagnostics.push_back(diagnostic(
          "skin.resource.texture_create_failed", "texture creation failed"));
      rollback();
      return result;
    }
    PendingHandle pending{*catalog->device_, handle};
    catalog->resources_.emplace(
        image.id,
        prepareResource(image.id, handle, image.pixels.width,
                        image.pixels.height, image.regions,
                        image.regionMappings));
    for (SkinResourceId alias : image.aliases) {
      const auto found = image.aliasRegions.find(alias);
      const auto &regions =
          found == image.aliasRegions.end() ? image.regions : found->second;
      const auto mappings = image.aliasRegionMappings.find(alias);
      catalog->resources_.emplace(
          alias,
          prepareResource(alias, handle, image.pixels.width,
                          image.pixels.height, regions, mappings->second));
    }
    catalog->owned_.push_back({handle});
    if (catalog->liveCounters_) catalog->liveCounters_->textureCreated();
    pending.release();
  }
  struct UploadedBitmapPage {
    bgfx::TextureHandle texture = BGFX_INVALID_HANDLE;
    int width = 0;
    int height = 0;
  };
  std::map<std::string, UploadedBitmapPage, std::less<>> uploadedBitmapPages;
  for (const auto &atlas : plan.atlases) {
    PreparedSkinTextAtlas prepared{
        .id = atlas.id,
        .key = atlas.key,
        .glyphs = atlas.glyphs,
        .kerning = atlas.kerning,
        .ascent = atlas.ascent,
        .capHeight = atlas.capHeight,
        .descent = atlas.descent,
        .lineHeight = atlas.lineHeight,
        .bitmapFont = atlas.bitmapFont,
        .bitmapFontType = atlas.bitmapFontType,
        .originalSize = atlas.originalSize,
        .pageWidth = atlas.pageWidth,
        .pageHeight = atlas.pageHeight,
        .layoutKind = atlas.layoutKind,
        .margin = atlas.margin};
    if (!atlas.bitmapFont) {
      if (atlas.pages.empty()) {
        const auto handle =
            catalog->device_->create(atlas.pixels, safetyPolicy);
        if (!bgfx::isValid(handle)) {
          result.diagnostics.push_back(diagnostic(
              "skin.resource.texture_create_failed", "texture creation failed"));
          rollback();
          return result;
        }
        PendingHandle pending{*catalog->device_, handle};
        prepared.texture = handle;
        prepared.width = atlas.pixels.width;
        prepared.height = atlas.pixels.height;
        catalog->owned_.push_back({handle});
        if (catalog->liveCounters_) catalog->liveCounters_->textureCreated();
        pending.release();
      } else {
        prepared.pages.reserve(atlas.pages.size());
        for (const auto &page : atlas.pages) {
          const auto handle =
              catalog->device_->create(*page.pixels, safetyPolicy);
          if (!bgfx::isValid(handle)) {
            result.diagnostics.push_back(diagnostic(
                "skin.resource.texture_create_failed",
                "scalable-font page texture creation failed"));
            rollback();
            return result;
          }
          PendingHandle pending{*catalog->device_, handle};
          prepared.pages.push_back(
              {.texture = handle,
               .width = page.pixels->width,
               .height = page.pixels->height,
               .available = true});
          if (!bgfx::isValid(prepared.texture)) {
            prepared.texture = handle;
            prepared.width = page.pixels->width;
            prepared.height = page.pixels->height;
          }
          catalog->owned_.push_back({handle});
          if (catalog->liveCounters_) {
            catalog->liveCounters_->textureCreated();
          }
          pending.release();
        }
      }
    } else {
      prepared.pages.reserve(atlas.pages.size());
      for (const auto &page : atlas.pages) {
        PreparedSkinTextAtlas::Page preparedPage{
            .bitmapFontType = page.bitmapFontType};
        if (page.pixels) {
          auto uploaded = uploadedBitmapPages.find(page.physicalKey);
          if (uploaded == uploadedBitmapPages.end()) {
            const auto handle =
                catalog->device_->create(*page.pixels, safetyPolicy);
            if (!bgfx::isValid(handle)) {
              result.diagnostics.push_back(diagnostic(
                  "skin.resource.texture_create_failed",
                  "bitmap-font page texture creation failed"));
              rollback();
              return result;
            }
            PendingHandle pending{*catalog->device_, handle};
            uploaded = uploadedBitmapPages
                           .emplace(page.physicalKey,
                                    UploadedBitmapPage{
                                        .texture = handle,
                                        .width = page.pixels->width,
                                        .height = page.pixels->height})
                           .first;
            catalog->owned_.push_back({handle});
            if (catalog->liveCounters_) {
              catalog->liveCounters_->textureCreated();
            }
            pending.release();
          }
          preparedPage.texture = uploaded->second.texture;
          preparedPage.width = uploaded->second.width;
          preparedPage.height = uploaded->second.height;
          preparedPage.available = true;
          if (!bgfx::isValid(prepared.texture)) {
            prepared.texture = preparedPage.texture;
            prepared.width = preparedPage.width;
            prepared.height = preparedPage.height;
          }
        }
        prepared.pages.push_back(preparedPage);
      }
    }
    catalog->atlases_.emplace(atlas.id, std::move(prepared));
    catalog->atlasKeys_.emplace(atlas.key, atlas.id);
  }
  catalog->textAtlasesByObject_ = std::move(plan.textAtlasesByObject);
  catalog->builtinImageResources_ =
      std::move(plan.builtinImageResources);
  for (auto &pomyu : plan.pomyuCharas) {
    catalog->pomyuCharas_.emplace(pomyu.object, std::move(pomyu));
  }
  catalog->pomyuMotionCyclesMillis_ = plan.pomyuMotionCyclesMillis;
  if (catalog->liveCounters_) {
    catalog->liveCounters_->resourceCreated();
    catalog->liveResourceCounted_ = true;
  }
  result.catalog=std::move(catalog); return result;
  } catch (...) {
    result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload allocation failed"));
    return result;
  }
}
const PreparedSkinResource *SkinResourceCatalog::find(SkinResourceId id) const noexcept { const auto it=resources_.find(id); return it==resources_.end()?nullptr:&it->second; }
std::optional<SkinResourceId>
SkinResourceCatalog::builtinImageResource(int reference) const noexcept {
  const auto found = builtinImageResources_.find(reference);
  return found == builtinImageResources_.end()
             ? std::nullopt
             : std::optional<SkinResourceId>(found->second);
}
const SkinResolvedRegion *SkinResourceCatalog::findResolvedRegion(SkinResourceId id, const SkinSourceRect &authored) const noexcept {
  const auto *resource = find(id);
  if (!resource) return nullptr;
  const auto found = std::lower_bound(
      resource->regionLookupOrder.begin(), resource->regionLookupOrder.end(),
      authored, [resource](std::uint32_t index, const SkinSourceRect &value) {
        recordRegionLookupComparison();
        return lessRect(resource->regionMappings[index].authored, value);
      });
  if (found == resource->regionLookupOrder.end()) return nullptr;
  const SkinResolvedRegion &mapping = resource->regionMappings[*found];
  return sameRect(mapping.authored, authored) ? &mapping : nullptr;
}
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlas(SkinTextAtlasId id) const noexcept { const auto it=atlases_.find(id); return it==atlases_.end()?nullptr:&it->second; }
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
std::vector<SkinResourceId>
SkinResourceCatalog::preparedResourceIdsForTesting() const {
  std::vector<SkinResourceId> result;
  result.reserve(resources_.size());
  for (const auto &[id, resource] : resources_) {
    (void)resource;
    result.push_back(id);
  }
  return result;
}

std::vector<SkinObjectId>
SkinResourceCatalog::preparedTextObjectIdsForTesting() const {
  std::vector<SkinObjectId> result;
  result.reserve(textAtlasesByObject_.size());
  for (const auto &[id, atlas] : textAtlasesByObject_) {
    (void)atlas;
    result.push_back(id);
  }
  return result;
}
#endif
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlas(const SkinTextAtlasKey &key) const noexcept { const auto it=atlasKeys_.find(key); return it==atlasKeys_.end()?nullptr:findTextAtlas(it->second); }
const PreparedSkinTextAtlas *SkinResourceCatalog::findTextAtlasForObject(
    SkinObjectId object) const noexcept {
  const auto found = textAtlasesByObject_.find(object);
  return found == textAtlasesByObject_.end() ? nullptr
                                             : findTextAtlas(found->second);
}

const PreparedPomyuCharaResource *
SkinResourceCatalog::findPomyuChara(SkinObjectId object) const noexcept {
  const auto found = pomyuCharas_.find(object);
  return found == pomyuCharas_.end() ? nullptr : &found->second;
}

bool SkinResourceCatalog::replaceBuiltinImage(
    int reference,
    std::optional<image_decode::DecodedImageData> pixels) noexcept {
  const SkinResourceId resourceId = builtinImageResourceId(reference);
  if ((reference != 100 && reference != 101 && reference != 102) ||
      resourceId == 0 || !renderPhase_ || !device_ ||
      !device_->ownsCurrentThread() || std::this_thread::get_id() != owner_) {
    return false;
  }

  const auto current = builtinImageResources_.find(reference);
  const auto removeCurrent = [&] {
    if (current == builtinImageResources_.end()) {
      return;
    }
    const auto existing = resources_.find(current->second);
    if (existing != resources_.end()) {
      const bgfx::TextureHandle handle = existing->second.texture;
      resources_.erase(existing);
      for (OwnedTexture &owned : owned_) {
        if (owned.handle.idx != handle.idx) {
          continue;
        }
        device_->destroy(owned.handle);
        owned.handle = BGFX_INVALID_HANDLE;
        if (liveCounters_) {
          liveCounters_->textureDestroyed();
        }
        break;
      }
    }
    builtinImageResources_.erase(current);
  };
  if (!pixels) {
    removeCurrent();
    return true;
  }
  if (!pixels->rgba ||
      !skinResourceDimensionsAllowed(pixels->width, pixels->height,
                                     pixels->byteSize(), safetyPolicy_) ||
      pixels->width > device_->maximumTextureDimension(safetyPolicy_) ||
      pixels->height > device_->maximumTextureDimension(safetyPolicy_)) {
    return false;
  }

  const SkinSourceRect region{
      .x = 0, .y = 0, .w = pixels->width, .h = pixels->height};
  PreparedSkinResource replacement{
      .id = resourceId,
      .width = pixels->width,
      .height = pixels->height,
      .regions = {region},
      .regionMappings = {{.authored = region, .resolved = region}},
      .regionLookupOrder = {0}};

  const auto existing = resources_.find(resourceId);
  if (existing != resources_.end() &&
      existing->second.width == pixels->width &&
      existing->second.height == pixels->height) {
    try {
      return device_->update(existing->second.texture, *pixels);
    } catch (...) {
      return false;
    }
  }

  bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
  try {
    handle = device_->create(*pixels, safetyPolicy_);
  } catch (...) {
    return false;
  }
  if (!bgfx::isValid(handle)) {
    return false;
  }
  replacement.texture = handle;
  try {
    if (existing != resources_.end()) {
      const bgfx::TextureHandle oldHandle = existing->second.texture;
      existing->second = std::move(replacement);
      for (OwnedTexture &owned : owned_) {
        if (owned.handle.idx != oldHandle.idx) {
          continue;
        }
        device_->destroy(oldHandle);
        owned.handle = handle;
        if (liveCounters_) {
          liveCounters_->textureDestroyed();
          liveCounters_->textureCreated();
        }
        return true;
      }
      device_->destroy(handle);
      return false;
    }
    owned_.push_back({handle});
    try {
      resources_.emplace(resourceId, std::move(replacement));
      builtinImageResources_.insert_or_assign(reference, resourceId);
    } catch (...) {
      owned_.pop_back();
      throw;
    }
    if (liveCounters_) {
      liveCounters_->textureCreated();
    }
    return true;
  } catch (...) {
    device_->destroy(handle);
    return false;
  }
}

bool SkinResourceCatalog::replaceTextAtlas(
    SkinPreparedGlyphAtlas &&atlas,
    std::span<const SkinObjectId> objects) noexcept {
  if (!renderPhase_ || !device_ || !device_->ownsCurrentThread() ||
      std::this_thread::get_id() != owner_) {
    return false;
  }
  auto current = atlases_.end();
  for (const SkinObjectId object : objects) {
    const auto binding = textAtlasesByObject_.find(object);
    if (binding == textAtlasesByObject_.end()) {
      continue;
    }
    const auto candidate = atlases_.find(binding->second);
    if (candidate != atlases_.end()) {
      current = candidate;
      break;
    }
  }
  if (current == atlases_.end()) {
    const auto key = atlasKeys_.find(atlas.key);
    if (key != atlasKeys_.end()) {
      current = atlases_.find(key->second);
    }
  }
  if (current == atlases_.end()) {
    return false;
  }

  PreparedSkinTextAtlas replacement{
      .id = current->first,
      .key = std::move(atlas.key),
      .glyphs = std::move(atlas.glyphs),
      .kerning = std::move(atlas.kerning),
      .ascent = atlas.ascent,
      .capHeight = atlas.capHeight,
      .descent = atlas.descent,
      .lineHeight = atlas.lineHeight,
      .bitmapFont = atlas.bitmapFont,
      .bitmapFontType = atlas.bitmapFontType,
      .originalSize = atlas.originalSize,
      .pageWidth = atlas.pageWidth,
      .pageHeight = atlas.pageHeight,
      .layoutKind = atlas.layoutKind,
      .margin = atlas.margin};
  std::vector<bgfx::TextureHandle> created;
  const auto discardCreated = [&] {
    for (const bgfx::TextureHandle handle : created) {
      if (bgfx::isValid(handle)) {
        device_->destroy(handle);
      }
    }
  };
  const auto uploadPage = [&](const image_decode::DecodedImageData &pixels,
                              int bitmapFontType) -> bool {
    const bgfx::TextureHandle handle = device_->create(pixels, safetyPolicy_);
    if (!bgfx::isValid(handle)) {
      return false;
    }
    created.push_back(handle);
    replacement.pages.push_back(
        {.texture = handle,
         .width = pixels.width,
         .height = pixels.height,
         .bitmapFontType = bitmapFontType,
         .available = true});
    if (!bgfx::isValid(replacement.texture)) {
      replacement.texture = handle;
      replacement.width = pixels.width;
      replacement.height = pixels.height;
    }
    return true;
  };
  try {
    if (atlas.pages.empty()) {
      if (!uploadPage(atlas.pixels, atlas.bitmapFontType)) {
        discardCreated();
        return false;
      }
      replacement.pages.clear();
    } else {
      replacement.pages.reserve(atlas.pages.size());
      for (const auto &page : atlas.pages) {
        if (!page.pixels) {
          replacement.pages.push_back(
              {.bitmapFontType = page.bitmapFontType, .available = false});
          continue;
        }
        if (!uploadPage(*page.pixels, page.bitmapFontType)) {
          discardCreated();
          return false;
        }
      }
    }
    if (!bgfx::isValid(replacement.texture)) {
      discardCreated();
      return false;
    }
    owned_.reserve(owned_.size() + created.size());
  } catch (...) {
    discardCreated();
    return false;
  }

  std::vector<bgfx::TextureHandle> replaced;
  const auto collect = [&](bgfx::TextureHandle handle) {
    if (bgfx::isValid(handle) &&
        std::ranges::find_if(replaced, [handle](bgfx::TextureHandle value) {
          return value.idx == handle.idx;
        }) == replaced.end()) {
      replaced.push_back(handle);
    }
  };
  collect(current->second.texture);
  for (const auto &page : current->second.pages) {
    collect(page.texture);
  }
  current->second = std::move(replacement);
  for (const SkinObjectId object : objects) {
    if (textAtlasesByObject_.contains(object)) {
      textAtlasesByObject_.insert_or_assign(object, current->first);
    }
  }
  for (const bgfx::TextureHandle handle : created) {
    owned_.push_back({handle});
    if (liveCounters_) {
      liveCounters_->textureCreated();
    }
  }
  for (const bgfx::TextureHandle handle : replaced) {
    const bool stillReferenced = std::ranges::any_of(
        atlases_, [handle](const auto &entry) {
          const auto &prepared = entry.second;
          return prepared.texture.idx == handle.idx ||
                 std::ranges::any_of(
                     prepared.pages, [handle](const auto &page) {
                       return page.texture.idx == handle.idx;
                     });
        });
    if (stillReferenced) {
      continue;
    }
    const auto owned = std::ranges::find_if(
        owned_, [handle](const OwnedTexture &candidate) {
          return candidate.handle.idx == handle.idx;
        });
    if (owned == owned_.end()) {
      continue;
    }
    device_->destroy(owned->handle);
    owned->handle = BGFX_INVALID_HANDLE;
    if (liveCounters_) {
      liveCounters_->textureDestroyed();
    }
  }
  return true;
}

const PreparedSkinGeneratedTexture *
SkinResourceCatalog::prepareGeneratedTexture(
    const SkinGeneratedTextureKey &key,
    const SkinGeneratedTextureData &data) const noexcept {
  if (!renderPhase_ || !device_ || !device_->ownsCurrentThread() ||
      std::this_thread::get_id() != owner_ || key.sourceObject == 0 ||
      data.width <= 0 || data.height <= 0 || data.rgba == nullptr) {
    return nullptr;
  }
  const auto pixelCount = static_cast<std::uint64_t>(data.width) *
                          static_cast<std::uint64_t>(data.height);
  const auto byteCount64 = pixelCount * 4U;
  if (byteCount64 > std::numeric_limits<std::size_t>::max() ||
      byteCount64 > std::numeric_limits<std::uint32_t>::max()) {
    return nullptr;
  }
  const std::size_t byteCount = static_cast<std::size_t>(byteCount64);
  if (data.rgba->size() != byteCount ||
      !skinResourceDimensionsAllowed(data.width, data.height, byteCount,
                                     safetyPolicy_) ||
      data.width > device_->maximumTextureDimension(safetyPolicy_) ||
      data.height > device_->maximumTextureDimension(safetyPolicy_) ||
      byteCount > skinResourceLimit(
                      safetyPolicy_,
                      SkinResourcePolicy::maximumGeneratedSessionBytes)) {
    return nullptr;
  }

  auto existing = generatedTextures_.find(key);
  const std::size_t oldBytes =
      existing == generatedTextures_.end()
          ? 0U
          : static_cast<std::size_t>(existing->second.prepared.width) *
                static_cast<std::size_t>(existing->second.prepared.height) *
                4U;
  const std::size_t generatedLimit = skinResourceLimit(
      safetyPolicy_, SkinResourcePolicy::maximumGeneratedSessionBytes);
  if (generatedDecodedBytes_ < oldBytes ||
      byteCount > generatedLimit - (generatedDecodedBytes_ - oldBytes) ||
      (existing == generatedTextures_.end() &&
       generatedTextures_.size() >=
           skinResourceLimit(
               safetyPolicy_,
               SkinResourcePolicy::maximumGeneratedTextures))) {
    return nullptr;
  }

  image_decode::DecodedImageData image{
      .width = data.width,
      .height = data.height,
      .rgba = std::const_pointer_cast<std::vector<unsigned char>>(data.rgba)};
  if (existing != generatedTextures_.end() &&
      existing->second.prepared.width == data.width &&
      existing->second.prepared.height == data.height) {
    const bool stableRevision =
        data.contentRevision != 0 &&
        existing->second.contentRevision != 0 &&
        data.contentRevision == existing->second.contentRevision;
    const bool stableLegacyPixels =
        data.contentRevision == 0 &&
        existing->second.contentRevision == 0 &&
        existing->second.pixels != nullptr &&
        *existing->second.pixels == *data.rgba;
    if (stableRevision || stableLegacyPixels) {
      return &existing->second.prepared;
    }
    bool updated = false;
    try {
      updated = device_->update(existing->second.prepared.texture, image);
    } catch (...) {
      return nullptr;
    }
    if (!updated) {
      return nullptr;
    }
    existing->second.pixels = data.rgba;
    existing->second.contentRevision = data.contentRevision;
    return &existing->second.prepared;
  }

  bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
  try {
    handle = device_->create(image, safetyPolicy_);
  } catch (...) {
    return nullptr;
  }
  if (!bgfx::isValid(handle)) {
    return nullptr;
  }
  if (existing != generatedTextures_.end()) {
    const auto oldHandle = existing->second.prepared.texture;
    device_->destroy(oldHandle);
    if (liveCounters_) {
      liveCounters_->textureDestroyed();
    }
    owned_[existing->second.ownedIndex].handle = handle;
    existing->second.prepared = {.key = key,
                                 .texture = handle,
                                 .width = data.width,
                                 .height = data.height};
    existing->second.pixels = data.rgba;
    existing->second.contentRevision = data.contentRevision;
    generatedDecodedBytes_ = generatedDecodedBytes_ - oldBytes + byteCount;
    if (liveCounters_) {
      liveCounters_->textureCreated();
    }
    return &existing->second.prepared;
  }

  try {
    const std::size_t ownedIndex = owned_.size();
    owned_.push_back({handle});
    try {
      const auto [inserted, accepted] = generatedTextures_.emplace(
          key, GeneratedTextureEntry{
                   .prepared = {.key = key,
                                .texture = handle,
                                .width = data.width,
                                .height = data.height},
                   .pixels = data.rgba,
                   .contentRevision = data.contentRevision,
                   .ownedIndex = ownedIndex});
      if (!accepted) {
        owned_.pop_back();
        device_->destroy(handle);
        return nullptr;
      }
      generatedDecodedBytes_ += byteCount;
      if (liveCounters_) {
        liveCounters_->textureCreated();
      }
      return &inserted->second.prepared;
    } catch (...) {
      owned_.pop_back();
      throw;
    }
  } catch (...) {
    device_->destroy(handle);
    return nullptr;
  }
}

SkinResourcePreparationService::SkinResourcePreparationService()
    : SkinResourcePreparationService(
          [](std::span<const std::byte> encoded, std::stop_token stop)
              -> std::optional<image_decode::DecodedImageData> {
            if (stop.stop_requested()) return std::nullopt;
            return image_decode::decodeImageMemory(encoded,
                {.maximumDimension = SkinResourcePolicy::maximumDimension,
                 .maximumEncodedBytes = SkinResourcePolicy::maximumEncodedBytes,
                 .maximumDecodedBytes = SkinResourcePolicy::maximumImageBytes,
                 .stop = stop});
          },
          SkinResourcePolicy::workerCount) {
  builtInDecoder_ = true;
}

SkinResourcePreparationService::SkinResourcePreparationService(
    Decoder decoder, std::size_t workerCount)
    : cache_(SkinResourcePolicy::cacheByteBudget),
      decoder_(std::move(decoder)),
      coordinator_([this](const image_decode::ImageDecodeRequest &request,
                          std::stop_token stop)
                       -> std::optional<image_decode::DecodedImageData> {
        if (stop.stop_requested() || !request.encoded) return std::nullopt;
        if (builtInDecoder_) {
          return image_decode::decodeImageMemory(
              *request.encoded,
              {.maximumDimension = request.maximumDimension,
               .maximumEncodedBytes = request.maximumEncodedBytes,
               .maximumDecodedBytes = request.maximumDecodedBytes,
               .stop = stop});
        }
        if (!decoder_) return std::nullopt;
        return decoder_(std::span<const std::byte>(request.encoded->data(),
                                                   request.encoded->size()),
                        stop);
      }, workerCount) {}

SkinResourcePreparationService::~SkinResourcePreparationService() { shutdown(); }

bool SkinResourcePreparationService::beginCall() {
  std::lock_guard lock(serviceMutex_);
  if (state_ != State::Running) return false;
  ++activeCalls_;
  return true;
}
bool SkinResourcePreparationService::cancellationRequested(std::stop_token external) const noexcept {
  if (external.stop_requested()) return true;
  std::lock_guard lock(serviceMutex_);
  return state_ != State::Running || stop_.stop_requested();
}
void SkinResourcePreparationService::endCall() noexcept {
  std::lock_guard lock(serviceMutex_);
  if (activeCalls_ != 0) --activeCalls_;
  serviceCv_.notify_all();
}
void SkinResourcePreparationService::shutdown() noexcept {
  bool leader = false;
  {
    std::unique_lock lock(serviceMutex_);
    if (state_ == State::Running) {
      state_ = State::Stopping;
      stop_.request_stop();
      leader = true;
    } else {
      serviceCv_.wait(lock, [this] { return shutdownComplete_; });
      return;
    }
  }
  if (leader) coordinator_.shutdown();
  std::unique_lock lock(serviceMutex_);
  serviceCv_.wait(lock, [this] { return activeCalls_ == 0; });
  state_ = State::Stopped;
  shutdownComplete_ = true;
  lock.unlock();
  serviceCv_.notify_all();
}

namespace {
std::size_t scalableAtlasByteSize(const SkinPreparedGlyphAtlas &atlas) noexcept {
  std::size_t total = atlas.pixels.byteSize();
  for (const auto &page : atlas.pages) {
    if (page.pixels) total += page.pixels->byteSize();
  }
  return total;
}

// Eviction cap for the rasterized scalable-atlas cache. Each atlas page is up
// to 1024x1024 RGBA (4 MiB), so the cap is large enough for a whole skin's
// style corpus but bounded against unbounded per-chart growth. A routine skin
// change already drops every entry via dropTextAtlasCache.
constexpr std::size_t kMaximumTextAtlasCacheBytes = 128U * 1024U * 1024U;
// Eviction cap for the per-glyph rasterization cache. Raw glyph alphas are
// small (glyph-area bytes), so this is generous but bounded against unbounded
// per-chart codepoint accumulation.
constexpr std::size_t kMaximumGlyphCacheBytes = 64U * 1024U * 1024U;
}

std::shared_ptr<const SkinPreparedGlyphAtlas>
SkinResourcePreparationService::findCachedTextAtlas(
    std::string_view revisionKey, std::string_view contentKey) const {
  std::shared_lock lock(textAtlasCacheMutex_);
  const auto foundRevision = textAtlasCache_.find(revisionKey);
  if (foundRevision == textAtlasCache_.end()) {
    return nullptr;
  }
  const auto found = foundRevision->second.find(contentKey);
  if (found == foundRevision->second.end()) {
    return nullptr;
  }
  return found->second;
}

void SkinResourcePreparationService::storeCachedTextAtlas(
    std::string revisionKey, std::string contentKey,
    SkinPreparedGlyphAtlas atlas) {
  const std::size_t bytes = scalableAtlasByteSize(atlas);
  auto stored = std::make_shared<const SkinPreparedGlyphAtlas>(
      std::move(atlas));
  std::unique_lock lock(textAtlasCacheMutex_);
  auto &entries = textAtlasCache_[std::move(revisionKey)];
  const auto previous = entries.find(contentKey);
  if (previous != entries.end() && previous->second) {
    const std::size_t previousBytes =
        scalableAtlasByteSize(*previous->second);
    textAtlasCacheBytes_ =
        textAtlasCacheBytes_ > previousBytes ? textAtlasCacheBytes_ - previousBytes
                                             : 0;
  }
  entries.insert_or_assign(std::move(contentKey), std::move(stored));
  textAtlasCacheBytes_ += bytes;
  if (textAtlasCacheBytes_ > kMaximumTextAtlasCacheBytes) {
    textAtlasCache_.clear();
    textAtlasCacheBytes_ = 0;
  }
}

void SkinResourcePreparationService::dropTextAtlasCache() noexcept {
  std::unique_lock lock(textAtlasCacheMutex_);
  textAtlasCache_.clear();
  textAtlasCacheBytes_ = 0;
  glyphCache_.clear();
  glyphCacheBytes_ = 0;
}

std::optional<SkinPreparedGlyphBitmap>
SkinResourcePreparationService::findCachedGlyph(
    std::string_view revisionKey, std::string_view glyphKey) const {
  std::shared_lock lock(textAtlasCacheMutex_);
  const auto foundRevision = glyphCache_.find(revisionKey);
  if (foundRevision == glyphCache_.end()) {
    return std::nullopt;
  }
  const auto found = foundRevision->second.find(glyphKey);
  if (found == foundRevision->second.end()) {
    return std::nullopt;
  }
  return found->second;
}

void SkinResourcePreparationService::storeCachedGlyph(
    std::string revisionKey, std::string glyphKey,
    SkinPreparedGlyphBitmap glyph) {
  const std::size_t bytes = glyph.alpha.size();
  std::unique_lock lock(textAtlasCacheMutex_);
  auto &entries = glyphCache_[std::move(revisionKey)];
  const auto previous = entries.find(glyphKey);
  if (previous != entries.end()) {
    const std::size_t previousBytes = previous->second.alpha.size();
    glyphCacheBytes_ =
        glyphCacheBytes_ > previousBytes ? glyphCacheBytes_ - previousBytes : 0;
  }
  entries.insert_or_assign(std::move(glyphKey), std::move(glyph));
  glyphCacheBytes_ += bytes;
  if (glyphCacheBytes_ > kMaximumGlyphCacheBytes) {
    glyphCache_.clear();
    glyphCacheBytes_ = 0;
  }
}

SkinResourceValidationResult SkinResourcePreparationService::validateResources(
    SkinResourceValidationInputs input) {
  SkinResourceValidationResult result;
  if (!beginCall()) { result.diagnostics.push_back(diagnostic("skin.resource.service_stopped", "resource preparation service is stopped")); return result; }
  struct End { SkinResourcePreparationService *service; ~End(){ service->endCall(); } } end{this};
  if (!checkInput(input.entry, input.revision, input.fileSystem, result.diagnostics) ||
      !runtimeStringsWithinPolicy(input.requiredRuntimeStrings,
                                  result.diagnostics,
                                  input.safetyPolicy)) {
    return result;
  }
  const CollectedResourceUses uses =
      collectResourceUses(input.model, input.practiceMode);
  if (const auto unresolved = uses.images.find(0);
      unresolved != uses.images.end() && unresolved->second.critical) {
    result.diagnostics.push_back(diagnostic(
        "skin.resource.source_missing",
        "a required selector Value or Float source is not declared"));
    return result;
  }
  struct ValidatedImage { int width = 0; int height = 0; };
  std::map<std::string, ValidatedImage, std::less<>> decodedByPath;
  SkinResourceSessionAccounting session(input.safetyPolicy);
  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue; // Font rasterization is intentionally a later slice.
    const auto use = uses.images.find(resource->id);
    if (use == uses.images.end()) continue;
    const auto configured = applyConfiguredFileSelection(
        resource->virtualPath, input.configuration, input.fileSystem,
        input.safetyPolicy);
    if (!configured.path) {
      result.diagnostics.push_back(useDiagnostic(
          "skin.resource.configuration_ambiguous",
          "skin.resource.configuration_ambiguous", configured.error,
          use->second.critical));
      continue;
    }
    const auto candidate = input.fileSystem.resolveResourceCandidates(
        *configured.path, *configured.path);
    if (!candidate.normalizedVirtualPath) {
      result.diagnostics.push_back(fileDiagnostic(*resource, candidate.failure ? &*candidate.failure : nullptr, use->second.critical));
      continue;
    }
    if (skinResourcePathIsMovie(*candidate.normalizedVirtualPath)) {
      continue;
    }
    auto decoded = decodedByPath.find(*candidate.normalizedVirtualPath);
    if (decoded == decodedByPath.end()) {
      const auto read = input.fileSystem.readResolvedResource(
          *candidate.normalizedVirtualPath,
          skinResourceLimit(input.safetyPolicy,
                            SkinResourcePolicy::maximumEncodedBytes));
      if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
      if (read.failure) { result.diagnostics.push_back(fileDiagnostic(*resource, &*read.failure, use->second.critical)); continue; }
      SkinResourceSessionAccounting candidateSession = session;
      if (!candidateSession.addImage(/*physicalResources=*/1,
                                     /*logicalResources=*/1,
                                     read.bytes.size(), /*decodedBytes=*/0,
                                     /*regions=*/0)) {
        result.diagnostics.push_back(useDiagnostic(
            "skin.resource.session_limit", "skin.resource.session_limit",
            "resource session aggregate exceeds policy", use->second.critical));
        continue;
      }
      const auto image = image_decode::decodeImageMemory(
          read.bytes,
          {.maximumDimension = skinResourceDimensionLimit(input.safetyPolicy),
           .maximumEncodedBytes = skinResourceLimit(
               input.safetyPolicy, SkinResourcePolicy::maximumEncodedBytes),
           .maximumDecodedBytes = skinResourceLimit(
               input.safetyPolicy, SkinResourcePolicy::maximumImageBytes),
           .stop = input.stop});
      if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
      if (!image || !skinResourceDimensionsAllowed(
                        image->width, image->height, image->byteSize(),
                        input.safetyPolicy)) { result.diagnostics.push_back(useDiagnostic("skin.resource.image_decode_failed", "skin.resource.image_decode_failed", "image decode failed resource validation", use->second.critical)); continue; }
      std::vector<SkinSourceRect> ignored;
      if (!resolveRegions(use->second, image->width, image->height, ignored,
                          nullptr, result.diagnostics, input.safetyPolicy)) {
        continue;
      }
      if (!candidateSession.addImage(/*physicalResources=*/0,
                                     /*logicalResources=*/0,
                                     /*encodedBytes=*/0, image->byteSize(),
                                     ignored.size())) {
        result.diagnostics.push_back(useDiagnostic(
            "skin.resource.session_limit", "skin.resource.session_limit",
            "resource session aggregate exceeds policy", use->second.critical));
        continue;
      }
      session = candidateSession;
      decoded = decodedByPath.emplace(*candidate.normalizedVirtualPath, ValidatedImage{.width=image->width, .height=image->height}).first;
    } else {
      std::vector<SkinSourceRect> ignored;
      if (!resolveRegions(use->second, decoded->second.width,
                          decoded->second.height, ignored, nullptr,
                          result.diagnostics, input.safetyPolicy)) {
        continue;
      }
      if (!session.addImage(/*physicalResources=*/0, /*logicalResources=*/1,
                            /*encodedBytes=*/0, /*decodedBytes=*/0,
                            ignored.size())) {
        result.diagnostics.push_back(useDiagnostic(
            "skin.resource.session_limit", "skin.resource.session_limit",
            "resource session aggregate exceeds policy", use->second.critical));
      }
      continue;
    }
  }
  const auto fontRequests = collectFontAtlasRequests(
      input.model, uses, input.fileSystem, input.configuration,
      input.requiredRuntimeStrings, input.requiredRuntimeStringsByObject,
      result.diagnostics, input.safetyPolicy);
  SkinTextAtlasId atlasId = 1;
  BitmapFontPreparationCache bitmapFontCache;
  std::set<std::string, std::less<>> accountedBitmapPages;
  std::size_t remainingScalableFontPaintAttemptWork =
      skinResourceLimit(
          input.safetyPolicy,
          SkinResourcePolicy::maximumScalableFontPaintBlendOperations);
  for (const auto &request : fontRequests) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    SkinResourceSessionAccounting fontSession = session;
    BitmapFontAccountingIdentities requestAccounting;
    const auto built = prepareFontAtlas(
        atlasId, request, input.fileSystem, fontSession, result.diagnostics,
        [this, &input] { return cancellationRequested(input.stop); },
        input.safetyPolicy, input.stop, bitmapFontCache, requestAccounting,
        remainingScalableFontPaintAttemptWork, coordinator_, nullptr,
        nullptr);
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!built) continue;
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!built->atlas) result.diagnostics.push_back(fontDiagnostic(request.font, request.critical, "skin.resource.glyph_missing", built->error));
    else if (const auto delta = atlasAccountingDelta(
                 *built->atlas, accountedBitmapPages);
             !delta || !fontSession.addAtlas(
                           delta->decodedBytes, built->atlas->glyphs.size(),
                           built->atlas->kerning.size(),
                           delta->physicalResources,
                           built->atlas->paintBlendOperations))
      result.diagnostics.push_back(fontDiagnostic(request.font, request.critical, "skin.resource.atlas_limit", "font atlas session aggregate exceeds policy"));
    else {
      session = fontSession;
      commitBitmapFontAccounting(bitmapFontCache, requestAccounting);
      accountedBitmapPages.insert(delta->bitmapPageKeys.begin(),
                                    delta->bitmapPageKeys.end());
      ++atlasId;
    }
  }
  {
    std::lock_guard lock(serviceMutex_);
    if (state_ != State::Running || stop_.stop_requested() ||
        input.stop.stop_requested()) { result.cancelled = true; return result; }
    recordCommittedEncodedBytes(session.encodedBytes());
    result.valid = std::ranges::none_of(result.diagnostics, [](const SkinDiagnostic &d) { return d.severity == DiagnosticSeverity::Error; });
  }
  return result;
}

SkinResourcePlanResult SkinResourcePreparationService::decodeAndPlan(
    SkinResourcePreparationInputs input) {
  SkinResourcePlanResult result;
  if (!beginCall()) { result.diagnostics.push_back(diagnostic("skin.resource.service_stopped", "resource preparation service is stopped")); return result; }
  struct End { SkinResourcePreparationService *service; ~End(){ service->endCall(); } } end{this};
  if (!checkInput(input.entry, input.revision.readView(), input.fileSystem, result.diagnostics) ||
      !runtimeStringsWithinPolicy(input.requiredRuntimeStrings,
                                  result.diagnostics,
                                  input.safetyPolicy)) {
    return result;
  }
  const CollectedResourceUses uses =
      collectResourceUses(input.model, input.practiceMode);
  if (const auto unresolved = uses.images.find(0);
      unresolved != uses.images.end() && unresolved->second.critical) {
    result.diagnostics.push_back(diagnostic(
        "skin.resource.source_missing",
        "a required selector Value or Float source is not declared"));
    return result;
  }
  SkinResourceUploadPlan plan{.revision = std::move(input.revision),
                              .safetyPolicy = input.safetyPolicy};
  std::map<std::string, std::size_t, std::less<>> unique;
  SkinResourceSessionAccounting session(input.safetyPolicy);
  auto pomyu = preparePomyuCharaResources(
      input.fileSystem, input.model, input.configuration, input.safetyPolicy,
      input.stop);
  if (pomyu.cancelled || cancellationRequested(input.stop)) {
    result.cancelled = true;
    return result;
  }
  bool pomyuAccepted = !pomyu.budgetExceeded;
  SkinResourceSessionAccounting pomyuSession = session;
  for (std::size_t index = 0; index < pomyu.images.size() && pomyuAccepted;
       ++index) {
    const auto &image = pomyu.images[index];
    pomyuAccepted = pomyuSession.addImage(
        /*physicalResources=*/1, /*logicalResources=*/1,
        index == 0 ? pomyu.encodedBytes : 0, image.pixels.byteSize(),
        image.regions.size());
  }
  if (!pomyuAccepted) {
    result.diagnostics.push_back(warning(
        "skin.resource.pomyu_limit",
        "Pomyu character resources exceed the bounded session resource "
        "policy and were ignored"));
  } else {
    session = pomyuSession;
    plan.decodedBytes = session.decodedBytes();
    plan.pomyuCharas = std::move(pomyu.resources);
    plan.pomyuMotionCyclesMillis = pomyu.motionCyclesMillis;
    plan.images.reserve(plan.images.size() + pomyu.images.size());
    for (auto &image : pomyu.images) {
      SkinDecodedImage decoded{
          .id = image.id,
          .pixels = std::move(image.pixels),
          .regions = std::move(image.regions)};
      decoded.regionMappings.reserve(decoded.regions.size());
      for (const SkinSourceRect &region : decoded.regions) {
        decoded.regionMappings.push_back(
            {.authored = region, .resolved = region});
      }
      plan.images.push_back(std::move(decoded));
    }
  }

  const auto resourceForBuiltin = [](int reference) -> SkinResourceId {
    switch (reference) {
    case 100:
      return kBuiltinStageResource;
    case 101:
      return kBuiltinBackResource;
    case 102:
      return kBuiltinBannerResource;
    case 110:
      return kBuiltinBlackResource;
    case 111:
      return kBuiltinWhiteResource;
    default:
      return 0;
    }
  };
  std::vector<int> plainReferences;
  for (const int reference : {110, 111}) {
    if (uses.builtinImages.contains(reference)) {
      plainReferences.push_back(reference);
    }
  }
  if (!plainReferences.empty()) {
    SkinResourceSessionAccounting candidateSession = session;
    if (!candidateSession.addImage(
            /*physicalResources=*/1,
            /*logicalResources=*/plainReferences.size(),
            /*encodedBytes=*/0, /*decodedBytes=*/8,
            /*regions=*/plainReferences.size())) {
      result.diagnostics.push_back(diagnostic(
          "skin.resource.session_limit",
          "built-in black/white images exceed the session resource policy"));
      return result;
    }
    image_decode::DecodedImageData pixels{
        .width = 2,
        .height = 1,
        .rgba = std::make_shared<std::vector<unsigned char>>(
            std::initializer_list<unsigned char>{0, 0, 0, 255, 255, 255,
                                                 255, 255})};
    const auto regionForReference = [](int reference) {
      return SkinSourceRect{.x = reference == 111 ? 1 : 0,
                            .y = 0,
                            .w = 1,
                            .h = 1};
    };
    const int primaryReference = plainReferences.front();
    const SkinSourceRect primaryRegion =
        regionForReference(primaryReference);
    SkinDecodedImage image{
        .id = resourceForBuiltin(primaryReference),
        .pixels = std::move(pixels),
        .regions = {primaryRegion},
        .regionMappings = {{.authored = primaryRegion,
                            .resolved = primaryRegion}}};
    plan.builtinImageResources.emplace(primaryReference, image.id);
    for (std::size_t index = 1; index < plainReferences.size(); ++index) {
      const int reference = plainReferences[index];
      const SkinResourceId alias = resourceForBuiltin(reference);
      const SkinSourceRect region = regionForReference(reference);
      image.aliases.push_back(alias);
      image.aliasRegions.emplace(alias, std::vector<SkinSourceRect>{region});
      image.aliasRegionMappings.emplace(
          alias,
          std::vector<SkinResolvedRegion>{{.authored = region,
                                           .resolved = region}});
      plan.builtinImageResources.emplace(reference, alias);
    }
    session = candidateSession;
    plan.decodedBytes = session.decodedBytes();
    plan.images.push_back(std::move(image));
  }

const auto prepareChartBuiltinImages = [&]() -> bool {
    // Chart-owned stage/back/banner images are optional SkinSourceReference
    // inputs. Reads go through the injected reader; when a batch reader is
    // available, archive entries are read in one offset-based pass per
    // archive instead of a per-image stream. Decoded results are cached by
    // path+content so a later chart attempt skips both the read and decode.
    struct WantedBuiltin {
      int reference = 0;
      std::filesystem::path virtualPath;
    };
    std::vector<WantedBuiltin> wanted;
    for (const int reference : {100, 101, 102}) {
      if (!uses.builtinImages.contains(reference)) continue;
      const auto path = input.builtinImagePaths.find(reference);
      if (path == input.builtinImagePaths.end() || path->second.empty()) {
        continue;
      }
      if (!input.builtinImageReader) continue;
      wanted.push_back({reference, path->second});
    }
    if (wanted.empty()) return true;
    const std::size_t maximumEncodedBytes = skinResourceLimit(
        input.safetyPolicy, SkinResourcePolicy::maximumEncodedBytes);
    const auto readOne = [&](const std::filesystem::path &virtualPath,
                             std::vector<unsigned char> &encoded) {
      std::string readError;
      return input.builtinImageReader(virtualPath, encoded, maximumEncodedBytes,
                                      &readError, input.stop);
    };
    std::map<int, std::vector<unsigned char>> encodedByReference;
    std::map<int, std::filesystem::path> virtualPathByReference;
    for (const auto &item : wanted) {
      virtualPathByReference.emplace(item.reference, item.virtualPath);
    }
    if (input.builtinImageBatchReader) {
      std::vector<std::filesystem::path> paths;
      for (const auto &item : wanted) {
        paths.push_back(item.virtualPath);
      }
      std::vector<SkinBuiltinImageBatch> batch;
      if (input.builtinImageBatchReader(paths, batch, input.stop)) {
        for (const auto &item : batch) {
          encodedByReference.emplace(item.reference, std::move(item.bytes));
        }
      }
    }
    for (const auto &item : wanted) {
      if (encodedByReference.contains(item.reference)) continue;
      std::vector<unsigned char> encoded;
      if (readOne(item.virtualPath, encoded) &&
          encoded.size() <= maximumEncodedBytes) {
        encodedByReference[item.reference] = std::move(encoded);
      }
    }
    if (cancellationRequested(input.stop)) {
      result.cancelled = true;
      return false;
    }
    for (const auto &[reference, encoded] : encodedByReference) {
      std::ostringstream builtinNote;
      builtinNote << "builtin reference " << reference << " bytes "
                  << encoded.size();
      StartupTiming::instance().note(builtinNote.str());
      SkinResourceSessionAccounting candidateSession = session;
      if (!candidateSession.addImage(
              /*physicalResources=*/1, /*logicalResources=*/1,
              encoded.size(), /*decodedBytes=*/0, /*regions=*/0)) {
        result.diagnostics.push_back(warning(
            "skin.resource.builtin_image_unavailable",
            "chart built-in image exceeds the session resource policy"));
        continue;
      }
      file_checksum::Sha256 digest;
      digest.update(std::span<const std::byte>(
          reinterpret_cast<const std::byte *>(encoded.data()),
          encoded.size()));
      const std::string cacheKey =
          "builtin:" + virtualPathByReference.at(reference).generic_string() +
          ":" + digest.finalHex();
      std::optional<image_decode::DecodedImageData> decoded;
      {
        std::lock_guard lock(serviceMutex_);
        decoded = cache_.get(cacheKey);
      }
      if (cancellationRequested(input.stop)) {
        result.cancelled = true;
        return false;
      }
      if (!decoded) {
        decoded = image_decode::decodeImageMemory(
            std::as_bytes(std::span(encoded)),
            {.maximumDimension = skinResourceDimensionLimit(input.safetyPolicy),
             .maximumEncodedBytes = maximumEncodedBytes,
             .maximumDecodedBytes = skinResourceLimit(
                 input.safetyPolicy, SkinResourcePolicy::maximumImageBytes),
             .stop = input.stop});
        if (cancellationRequested(input.stop)) {
          result.cancelled = true;
          return false;
        }
        if (decoded) {
          std::lock_guard lock(serviceMutex_);
          cache_.put(cacheKey, *decoded);
        }
      }
      if (!decoded ||
          !skinResourceDimensionsAllowed(decoded->width, decoded->height,
                                         decoded->byteSize(),
                                         input.safetyPolicy)) {
        continue;
      }
      const SkinSourceRect region{
          .x = 0, .y = 0, .w = decoded->width, .h = decoded->height};
      if (!candidateSession.addImage(
              /*physicalResources=*/0, /*logicalResources=*/0,
              /*encodedBytes=*/0, decoded->byteSize(), /*regions=*/1)) {
        result.diagnostics.push_back(warning(
            "skin.resource.builtin_image_unavailable",
            "chart built-in image exceeds the session resource policy"));
        continue;
      }
      const SkinResourceId resource = resourceForBuiltin(reference);
      plan.images.push_back(
          {.id = resource,
           .pixels = std::move(*decoded),
           .regions = {region},
           .regionMappings = {{.authored = region, .resolved = region}}});
      plan.builtinImageResources.emplace(reference, resource);
      session = candidateSession;
      plan.decodedBytes = session.decodedBytes();
    }
    return true;
  };

  struct PendingImageDecode {
    image_decode::ImageDecodeCoordinator::Ticket ticket = 0;
    std::string key;
    std::string revisionKey;
    std::string imageKey;
    SkinResourceId resourceId = 0;
    ResourceUse use;
    std::string candidatePath;
    std::vector<SkinResourceId> aliasIds;
    std::vector<ResourceUse> aliasUses;
  };
  std::vector<PendingImageDecode> pendingImages;
  std::map<std::string, std::size_t, std::less<>> pendingImageIndexByPath;

  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue;
    const auto use = uses.images.find(resource->id);
    if (use == uses.images.end()) continue;
    const auto configured = applyConfiguredFileSelection(
        resource->virtualPath, input.configuration, input.fileSystem,
        input.safetyPolicy);
    if (!configured.path) {
      result.diagnostics.push_back(useDiagnostic(
          "skin.resource.configuration_ambiguous",
          "skin.resource.configuration_ambiguous", configured.error,
          use->second.critical));
      continue;
    }
    const auto candidate = input.fileSystem.resolveResourceCandidates(
        *configured.path, *configured.path);
    if (!candidate.normalizedVirtualPath) { result.diagnostics.push_back(fileDiagnostic(*resource, candidate.failure ? &*candidate.failure : nullptr, use->second.critical)); continue; }
    if (skinResourcePathIsMovie(*candidate.normalizedVirtualPath)) continue;
    const std::string candidatePath = *candidate.normalizedVirtualPath;
    if (const auto found = unique.find(candidatePath); found != unique.end()) {
      auto &image=plan.images[found->second];
      std::vector<SkinSourceRect> regions;
      std::vector<SkinResolvedRegion> mappings;
      if (resolveRegions(use->second,image.pixels.width,image.pixels.height,
                         regions,&mappings,result.diagnostics,
                         input.safetyPolicy)) {
        if (!session.addImage(/*physicalResources=*/0,
                              /*logicalResources=*/1,
                              /*encodedBytes=*/0, /*decodedBytes=*/0,
                              regions.size())) {
          result.diagnostics.push_back(useDiagnostic(
              "skin.resource.session_limit", "skin.resource.session_limit",
              "resource session aggregate exceeds policy", use->second.critical));
        } else {
          image.aliases.push_back(resource->id);
          image.aliasRegions.emplace(resource->id,std::move(regions));
          image.aliasRegionMappings.emplace(resource->id,std::move(mappings));
        }
      }
      continue;
    }
    if (const auto pendingFound = pendingImageIndexByPath.find(candidatePath);
        pendingFound != pendingImageIndexByPath.end()) {
      // Charge the logical resource now (like the unique-hit alias path) so a
      // distributed overage is still rejected while decode is queued.
      if (!session.addImage(/*physicalResources=*/0,
                            /*logicalResources=*/1,
                            /*encodedBytes=*/0, /*decodedBytes=*/0,
                            /*regions=*/0)) {
        result.diagnostics.push_back(useDiagnostic(
            "skin.resource.session_limit", "skin.resource.session_limit",
            "resource session aggregate exceeds policy", use->second.critical));
        continue;
      }
      auto &pending = pendingImages[pendingFound->second];
      pending.aliasIds.push_back(resource->id);
      pending.aliasUses.push_back(use->second);
      continue;
    }
    const auto read = input.fileSystem.readResolvedResource(
        *candidate.normalizedVirtualPath,
        skinResourceLimit(input.safetyPolicy,
                          SkinResourcePolicy::maximumEncodedBytes));
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (read.failure) { result.diagnostics.push_back(fileDiagnostic(*resource, &*read.failure, use->second.critical)); continue; }
    const std::size_t encodedBytes = read.bytes.size();
    SkinResourceSessionAccounting candidateSession = session;
    if (!candidateSession.addImage(/*physicalResources=*/1,
                                   /*logicalResources=*/1, encodedBytes,
                                   /*decodedBytes=*/0, /*regions=*/0)) {
      result.diagnostics.push_back(useDiagnostic(
          "skin.resource.session_limit", "skin.resource.session_limit",
          "resource session aggregate exceeds policy", use->second.critical));
      continue;
    }
    // Commit the encoded charge so a distributed/physical overage is rejected
    // before this resource's decode is queued, mirroring the serial path's
    // per-resource accumulation.
    session = candidateSession;
    plan.decodedBytes = session.decodedBytes();
    const std::string revisionKey =
        plan.revision.revision().lowercaseSha256;
    file_checksum::Sha256 digest;
    digest.update(std::span<const std::byte>(read.bytes.data(),
                                             read.bytes.size()));
    const std::string contentDigest = digest.finalHex();
    // Skin-owned images (unlike per-chart builtin images) are cached by
    // revision+path+content digest so a later chart attempt with an unchanged
    // revision and file reuses the decoded pixels. Including the content
    // digest means a live resource edit within the same revision still
    // re-decodes instead of serving stale pixels; only the expensive decode is
    // cached, while the cheap read and hash still run on every plan.
    const std::string imageKey = *candidate.normalizedVirtualPath + ":" +
                                 contentDigest;
    std::optional<image_decode::DecodedImageData> decoded;
    bool fromAppCache = false;
    if (auto cachedImage = decodeCache_.findSkinImage(revisionKey, imageKey);
        cachedImage) {
      decoded = *cachedImage;
      fromAppCache = true;
#if defined(ASOBMASHOW_SKIN_RESOURCE_TESTING)
      recordSkinImageAppCacheHit();
#endif
    }
    if (!decoded) {
      const std::string key = plan.revision.revision().lowercaseSha256 + ":" +
                              imageKey;
      { std::lock_guard lock(serviceMutex_); decoded = cache_.get(key); }
      if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
      if (!decoded) {
        auto owned = std::make_shared<const std::vector<std::byte>>(
            std::move(read.bytes));
        const auto ticket = coordinator_.request(
            {.key = key,
             .path = {},
             .maximumDimension =
                 skinResourceDimensionLimit(input.safetyPolicy),
             .maximumEncodedBytes = skinResourceLimit(
                 input.safetyPolicy, SkinResourcePolicy::maximumEncodedBytes),
             .maximumDecodedBytes = skinResourceLimit(
                 input.safetyPolicy, SkinResourcePolicy::maximumImageBytes),
             .encoded = std::move(owned)});
        // Queue the ticket without waiting so all image decodes run in
        // parallel on the coordinator's workers; finalize happens in the
        // wait pass after the loop.
        pendingImages.push_back({
            .ticket = ticket,
            .key = key,
            .revisionKey = revisionKey,
            .imageKey = imageKey,
            .resourceId = resource->id,
            .use = use->second,
            .candidatePath = candidatePath,
        });
        pendingImageIndexByPath.emplace(candidatePath,
                                        pendingImages.size() - 1);
        continue;
      }
    }
    // Cache hit (app or service cache): finalize inline with the same
    // accounting and plan writes as the deferred wait pass.
    if (!skinResourceDimensionsAllowed(decoded->width, decoded->height,
                                       decoded->byteSize(),
                                       input.safetyPolicy)) {
      result.diagnostics.push_back(useDiagnostic(
          "skin.resource.session_limit", "skin.resource.session_limit",
          "decoded resource bytes exceed the session policy",
          use->second.critical));
      continue;
    }
    std::vector<SkinSourceRect> regions;
    std::vector<SkinResolvedRegion> mappings;
    if (!resolveRegions(use->second, decoded->width, decoded->height, regions,
                        &mappings, result.diagnostics, input.safetyPolicy)) {
      continue;
    }
    if (!candidateSession.addImage(/*physicalResources=*/0,
                                   /*logicalResources=*/0,
                                   /*encodedBytes=*/0, decoded->byteSize(),
                                   regions.size())) {
      result.diagnostics.push_back(useDiagnostic(
          "skin.resource.session_limit", "skin.resource.session_limit",
          "resource session aggregate exceeds policy", use->second.critical));
      continue;
    }
    session = candidateSession;
    plan.decodedBytes = session.decodedBytes();
    unique.emplace(*candidate.normalizedVirtualPath, plan.images.size());
    plan.images.push_back({.id = resource->id,
                           .pixels = *decoded,
                           .regions = std::move(regions),
                           .regionMappings = std::move(mappings)});
    if (!fromAppCache) {
      decodeCache_.storeSkinImage(revisionKey, imageKey, *decoded);
    }
  }
  // Wait for every queued image decode and finalize it. The coordinator
  // decoded the images in parallel while this thread queued them, so each
  // wait resolves promptly.
  for (const auto &pending : pendingImages) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    const auto waited = coordinator_.waitTake(pending.ticket, input.stop);
    if (waited.state == image_decode::ImageDecodeWaitState::Cancelled ||
        waited.state == image_decode::ImageDecodeWaitState::Stopped ||
        cancellationRequested(input.stop)) {
      result.cancelled = true;
      return result;
    }
    if (waited.state != image_decode::ImageDecodeWaitState::Ready ||
        !waited.image) {
      result.diagnostics.push_back(useDiagnostic(
          "skin.resource.image_decode_failed",
          "skin.resource.image_decode_failed",
          "image decode failed during planning", pending.use.critical));
      continue;
    }
    auto decoded = *waited.image;
    {
      std::lock_guard lock(serviceMutex_);
      if (state_ != State::Running || stop_.stop_requested()) {
        result.cancelled = true;
        return result;
      }
      cache_.put(pending.key, decoded);
    }
    if (!skinResourceDimensionsAllowed(decoded.width, decoded.height,
                                       decoded.byteSize(),
                                       input.safetyPolicy)) {
      result.diagnostics.push_back(useDiagnostic(
          "skin.resource.session_limit", "skin.resource.session_limit",
          "decoded resource bytes exceed the session policy",
          pending.use.critical));
      continue;
    }
    SkinResourceSessionAccounting candidateSession = session;
    std::vector<SkinSourceRect> regions;
    std::vector<SkinResolvedRegion> mappings;
    if (!resolveRegions(pending.use, decoded.width, decoded.height, regions,
                        &mappings, result.diagnostics, input.safetyPolicy)) {
      continue;
    }
    if (!candidateSession.addImage(/*physicalResources=*/0,
                                   /*logicalResources=*/0,
                                   /*encodedBytes=*/0, decoded.byteSize(),
                                   regions.size())) {
      result.diagnostics.push_back(useDiagnostic(
          "skin.resource.session_limit", "skin.resource.session_limit",
          "resource session aggregate exceeds policy", pending.use.critical));
      continue;
    }
    session = candidateSession;
    plan.decodedBytes = session.decodedBytes();
    unique.emplace(pending.candidatePath, plan.images.size());
    plan.images.push_back({.id = pending.resourceId,
                           .pixels = decoded,
                           .regions = std::move(regions),
                           .regionMappings = std::move(mappings)});
    decodeCache_.storeSkinImage(pending.revisionKey, pending.imageKey,
                                plan.images.back().pixels);
    for (std::size_t aliasIndex = 0; aliasIndex < pending.aliasIds.size();
         ++aliasIndex) {
      auto &image = plan.images.back();
      std::vector<SkinSourceRect> aliasRegions;
      std::vector<SkinResolvedRegion> aliasMappings;
      if (resolveRegions(pending.aliasUses[aliasIndex], image.pixels.width,
                         image.pixels.height, aliasRegions, &aliasMappings,
                         result.diagnostics, input.safetyPolicy)) {
        if (session.addImage(/*physicalResources=*/0,
                             /*logicalResources=*/0,
                             /*encodedBytes=*/0, /*decodedBytes=*/0,
                             aliasRegions.size())) {
          image.aliases.push_back(pending.aliasIds[aliasIndex]);
          image.aliasRegions.emplace(pending.aliasIds[aliasIndex],
                                     std::move(aliasRegions));
          image.aliasRegionMappings.emplace(pending.aliasIds[aliasIndex],
                                            std::move(aliasMappings));
        }
      }
    }
  }
  StartupTiming::instance().mark("decodeAndPlan: image decodes finished");
  const auto fontRequests = collectFontAtlasRequests(
      input.model, uses, input.fileSystem, input.configuration,
      input.requiredRuntimeStrings, input.requiredRuntimeStringsByObject,
      result.diagnostics, input.safetyPolicy);
  SkinTextAtlasId atlasId = 1;
  BitmapFontPreparationCache bitmapFontCache;
  std::set<std::string, std::less<>> accountedBitmapPages;
  std::size_t remainingScalableFontPaintAttemptWork =
      skinResourceLimit(
          input.safetyPolicy,
          SkinResourcePolicy::maximumScalableFontPaintBlendOperations);
  for (const auto &request : fontRequests) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    SkinResourceSessionAccounting fontSession = session;
    BitmapFontAccountingIdentities requestAccounting;
    const auto built = prepareFontAtlas(
        atlasId, request, input.fileSystem, fontSession, result.diagnostics,
        [this, &input] { return cancellationRequested(input.stop); },
        input.safetyPolicy, input.stop, bitmapFontCache, requestAccounting,
        remainingScalableFontPaintAttemptWork, coordinator_, &decodeCache_,
        this);
    {
      std::ostringstream fontNote;
      fontNote << "font request object";
      for (const SkinObjectId object : request.objects) {
        fontNote << " " << object;
      }
      fontNote << " codepoints=" << request.codepoints.size()
               << " built=" << (built && built->atlas ? "yes" : "no");
      StartupTiming::instance().note(fontNote.str());
    }
    StartupTiming::instance().mark(
        ("decodeAndPlan: font atlas done (" +
         std::to_string(plan.atlases.size()) + "/" +
         std::to_string(fontRequests.size()) + ")")
            .c_str());
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!built) continue;
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!built->atlas) {
      result.diagnostics.push_back(fontDiagnostic(request.font, request.critical, "skin.resource.glyph_missing", built->error));
      continue;
    }
    const auto delta = atlasAccountingDelta(*built->atlas,
                                            accountedBitmapPages);
    if (!delta || !fontSession.addAtlas(delta->decodedBytes,
                                        built->atlas->glyphs.size(),
                                        built->atlas->kerning.size(),
                                        delta->physicalResources,
                                        built->atlas->paintBlendOperations)) {
      {
        std::ostringstream rejectNote;
        rejectNote << "font atlas REJECTED for object";
        for (const SkinObjectId object : request.objects) {
          rejectNote << " " << object;
        }
        rejectNote << " decoded=" << (delta ? delta->decodedBytes : 0)
                   << " glyphs=" << built->atlas->glyphs.size()
                   << " pairs=" << built->atlas->kerning.size()
                   << " phys=" << (delta ? delta->physicalResources : 0)
                   << " sessionDecoded=" << fontSession.decodedBytes();
        StartupTiming::instance().note(rejectNote.str());
      }
      result.diagnostics.push_back(fontDiagnostic(request.font, request.critical, "skin.resource.atlas_limit", "font atlas session aggregate exceeds policy"));
      continue;
    }
    session = fontSession;
    commitBitmapFontAccounting(bitmapFontCache, requestAccounting);
    accountedBitmapPages.insert(delta->bitmapPageKeys.begin(),
                                  delta->bitmapPageKeys.end());
    plan.decodedBytes = session.decodedBytes();
    plan.atlases.push_back(*built->atlas);
    for (const SkinObjectId object : request.objects) {
      plan.textAtlasesByObject.emplace(object, atlasId);
    }
    ++atlasId;
  }
  // Chart-owned images are optional SkinSourceReference inputs. Prepare them
  // only from the budget left after package-critical images/fonts so an
  // oversized stage file cannot starve the authored skin resources.
  StartupTiming::instance().mark("decodeAndPlan: font atlases done");
  if (!prepareChartBuiltinImages()) return result;
  StartupTiming::instance().mark("decodeAndPlan: chart builtins done");
  if (std::ranges::any_of(result.diagnostics, [](const SkinDiagnostic &d) { return d.severity == DiagnosticSeverity::Error; })) return result;
  {
    std::lock_guard lock(serviceMutex_);
    if (state_ != State::Running || stop_.stop_requested() ||
        input.stop.stop_requested()) { result.cancelled = true; return result; }
    recordCommittedEncodedBytes(session.encodedBytes());
    result.plan = std::move(plan);
  }
  return result;
}

SkinTextAtlasPreparationResult
SkinResourcePreparationService::prepareTextAtlasUpdates(
    SkinTextAtlasPreparationInputs input) {
  SkinTextAtlasPreparationResult result;
  if (!beginCall()) {
    result.diagnostics.push_back(
        diagnostic("skin.resource.service_stopped",
                   "resource preparation service is stopped"));
    return result;
  }
  struct End {
    SkinResourcePreparationService *service;
    ~End() { service->endCall(); }
  } end{this};
  if (!checkInput(input.entry, input.revision.readView(), input.fileSystem,
                  result.diagnostics)) {
    return result;
  }
  if (cancellationRequested(input.stop)) {
    result.cancelled = true;
    return result;
  }

  const CollectedResourceUses uses = collectResourceUses(input.model, false);
  const auto requests = collectFontAtlasRequests(
      input.model, uses, input.fileSystem, input.configuration, {},
      input.requiredRuntimeStringsByObject, result.diagnostics,
      input.safetyPolicy);
  SkinTextAtlasUpdatePlan plan;
  SkinResourceSessionAccounting session(input.safetyPolicy);
  BitmapFontPreparationCache bitmapFontCache;
  std::set<std::string, std::less<>> accountedBitmapPages;
  std::size_t remainingScalableFontPaintAttemptWork = skinResourceLimit(
      input.safetyPolicy,
      SkinResourcePolicy::maximumScalableFontPaintBlendOperations);
  SkinTextAtlasId atlasId = 1;
  for (const auto &request : requests) {
    if (cancellationRequested(input.stop)) {
      result.cancelled = true;
      return result;
    }
    if (std::ranges::none_of(request.objects, [&](SkinObjectId object) {
          return input.targetObjects.contains(object);
        })) {
      continue;
    }
    SkinResourceSessionAccounting fontSession = session;
    BitmapFontAccountingIdentities requestAccounting;
    const auto built = prepareFontAtlas(
        atlasId, request, input.fileSystem, fontSession, result.diagnostics,
        [this, &input] { return cancellationRequested(input.stop); },
        input.safetyPolicy, input.stop, bitmapFontCache, requestAccounting,
        remainingScalableFontPaintAttemptWork, coordinator_, &decodeCache_,
        this);
    if (cancellationRequested(input.stop)) {
      result.cancelled = true;
      return result;
    }
    if (!built || !built->atlas) {
      if (built && !built->error.empty()) {
        result.diagnostics.push_back(fontDiagnostic(
            request.font, request.critical, "skin.resource.glyph_missing",
            built->error));
      }
      continue;
    }
    const auto delta = atlasAccountingDelta(*built->atlas, accountedBitmapPages);
    if (!delta ||
        !fontSession.addAtlas(delta->decodedBytes, built->atlas->glyphs.size(),
                              built->atlas->kerning.size(),
                              delta->physicalResources,
                              built->atlas->paintBlendOperations)) {
      result.diagnostics.push_back(fontDiagnostic(
          request.font, request.critical, "skin.resource.atlas_limit",
          "font atlas session aggregate exceeds policy"));
      continue;
    }
    session = fontSession;
    commitBitmapFontAccounting(bitmapFontCache, requestAccounting);
    accountedBitmapPages.insert(delta->bitmapPageKeys.begin(),
                                delta->bitmapPageKeys.end());
    std::vector<SkinObjectId> objects;
    for (const SkinObjectId object : request.objects) {
      if (input.targetObjects.contains(object)) {
        objects.push_back(object);
      }
    }
    plan.atlases.push_back(
        {.atlas = *built->atlas, .objects = std::move(objects)});
    ++atlasId;
  }
  {
    std::lock_guard lock(serviceMutex_);
    if (state_ != State::Running || stop_.stop_requested() ||
        input.stop.stop_requested()) {
      result.cancelled = true;
      return result;
    }
  }
  result.plan = std::move(plan);
  return result;
}
} // namespace skin
#endif
