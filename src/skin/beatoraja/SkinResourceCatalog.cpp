#include "SkinResourceCatalog.h"
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
constexpr std::string_view kPracticeSystemFontVirtualPath =
    "@asobmashow/practice-system-font";
constexpr std::string_view kPracticeSystemFontPath =
    "assets/fonts/notosanscjkjp.ttf";

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
    const SkinSafetyPolicy &safetyPolicy) {
  const ConfiguredFile *match = nullptr;
  for (const auto &file : configuration.orderedFiles) {
    if (!authored.starts_with(file.pattern)) continue;
    if (match != nullptr) {
      return {.error="resource path matches multiple configured files"};
    }
    match = &file;
  }
  if (match == nullptr) {
    if (authored.find('*') != std::string_view::npos) {
      return {.error="resource wildcard has no configured file selection"};
    }
    return {.path=std::string(authored)};
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
      else if constexpr (std::is_same_v<T, SkinGraphObject>) addSprite(object.fill, critical);
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

bool isBitmapFontFace(std::string_view path);

std::vector<FontAtlasRequest> collectFontAtlasRequests(
    const ValidatedBeatorajaSkinModel &model, const CollectedResourceUses &uses,
    const LuaSkinFileSystem &files,
    const BeatorajaSkinConfiguration &configuration,
    std::span<const std::string> runtimeStrings,
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
          path, configuration, safetyPolicy);
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
      const auto candidate = files.resolveResourceCandidates(
          *configured.path, *configured.path);
      if (!candidate.normalizedVirtualPath) {
        if (primary) {
          resolved = false;
        } else {
          diagnostics.push_back(fontDiagnostic(
              found->second, false, "skin.resource.font_missing",
              "bitmap fallback path is unavailable"));
        }
        return;
      }
      digestWithinPolicy = appendStableFallbackChainEntry(
          digest, *candidate.normalizedVirtualPath, type, safetyPolicy);
      if (digestWithinPolicy) {
        resolvedFaces.push_back({.path = *candidate.normalizedVirtualPath,
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
    if (!resolvedFaces.empty() && isBitmapFontFace(resolvedFaces.front().path)) {
      // Bitmap pages and glyph metrics do not depend on a text object's size
      // or paint. SkinTextBitmap applies both at layout/draw time.
      key.pointSize = 1;
      key.outlineRgba = {255, 255, 255, 0};
      key.outlineWidth = 0.0;
      key.shadowRgba = {255, 255, 255, 0};
      key.shadowOffsetX = 0.0;
      key.shadowOffsetY = 0.0;
      key.shadowSmoothness = 0.0;
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
    if (text.receivesRuntimeStrings) for (const auto &runtime : runtimeStrings)
      if (!appendUtf8(runtime, request->second.codepoints,
                      request->second.pairs, safetyPolicy))
        diagnostics.push_back(fontDiagnostic(found->second, request->second.critical, "skin.resource.atlas_limit", "font glyph or kerning limit exceeds policy"));
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
    const std::string extension = practiceSystemFont
                                      ? ".ttf"
                                      : std::filesystem::path(face.path)
                                            .extension()
                                            .string();
    std::string lowerExtension;
    lowerExtension.reserve(extension.size());
    for (const char character : extension) lowerExtension.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    if (lowerExtension != ".ttf" && lowerExtension != ".otf") {
      diagnostics.push_back(fontDiagnostic(request.font, request.critical, "skin.resource.font_format_unsupported", "font extension must be TTF or OTF"));
      return std::nullopt;
    }
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

bool isBitmapFontFace(std::string_view path) {
  const std::string extension = lowercaseExtension(path);
  return extension == ".fnt" || extension == ".lr2font";
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
    BitmapFontAccountingIdentities &requestAccounting) {
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
    prepared.pages.reserve(prepared.font.pagePaths.size());
    bool usableFace = true;
    for (std::string pagePath : prepared.font.pagePaths) {
      if (pagePath.empty() && prepared.font.lr2Font) {
        prepared.pages.push_back({});
        continue;
      }
      std::ranges::replace(pagePath, '\\', '/');
      const auto combined =
          (std::filesystem::path(face.path).parent_path() / pagePath)
              .lexically_normal()
              .generic_string();
      const auto candidate =
          files.resolveResourceCandidates(combined, combined);
      if (!candidate.normalizedVirtualPath) {
        rejectPage("skin.resource.font_missing",
                   "bitmap-font page path is invalid");
        if (prepared.font.lr2Font) {
          prepared.pages.push_back({});
          continue;
        }
        usableFace = false;
        break;
      }
      auto decoded = cache.pages.find(*candidate.normalizedVirtualPath);
      if (decoded == cache.pages.end()) {
        const auto read = files.readResolvedResource(
            *candidate.normalizedVirtualPath,
            skinResourceLimit(safetyPolicy,
                              SkinResourcePolicy::maximumEncodedBytes));
        if (cancellationRequested()) return std::nullopt;
        if (read.failure) {
          rejectPage("skin.resource.font_missing",
                     "bitmap-font page image is unavailable");
          if (prepared.font.lr2Font) {
            prepared.pages.push_back(
                {.physicalKey = *candidate.normalizedVirtualPath});
            continue;
          }
          usableFace = false;
          break;
        }
        cache.pageEncodedBytes.insert_or_assign(
            *candidate.normalizedVirtualPath, read.bytes.size());
        const auto image = image_decode::decodeImageMemory(
            read.bytes,
            {.maximumDimension = skinResourceDimensionLimit(safetyPolicy),
             .maximumEncodedBytes = skinResourceLimit(
                 safetyPolicy, SkinResourcePolicy::maximumEncodedBytes),
             .maximumDecodedBytes = skinResourceLimit(
                 safetyPolicy, SkinResourcePolicy::maximumImageBytes),
             .stop = stop});
        if (cancellationRequested()) return std::nullopt;
        if (!image) {
          rejectPage("skin.resource.image_decode_failed",
                     "bitmap-font page image cannot be decoded within policy");
          if (prepared.font.lr2Font) {
            prepared.pages.push_back(
                {.physicalKey = *candidate.normalizedVirtualPath});
            continue;
          }
          usableFace = false;
          break;
        }
        decoded = cache.pages
                      .emplace(*candidate.normalizedVirtualPath, *image)
                      .first;
      }
      if (!cache.accountedPages.contains(*candidate.normalizedVirtualPath) &&
          !requestAccounting.pages.contains(
              *candidate.normalizedVirtualPath) &&
          !newlyAccountedPages.contains(*candidate.normalizedVirtualPath)) {
        const auto encodedBytes =
            cache.pageEncodedBytes.find(*candidate.normalizedVirtualPath);
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
        newlyAccountedPages.insert(*candidate.normalizedVirtualPath);
      }
      prepared.pages.push_back({.physicalKey = *candidate.normalizedVirtualPath,
                                .pixels = decoded->second});
    }
    if (!usableFace) {
      if (primary) return std::nullopt;
      continue;
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
    BitmapFontAccountingIdentities &requestAccounting) {
  if (request.resolvedFaces.empty()) return std::nullopt;
  if (isBitmapFontFace(request.resolvedFaces.front().path)) {
    const auto faces = readBitmapFontFaces(
        request, files, session, diagnostics, cancellationRequested,
        safetyPolicy, stop, cache, requestAccounting);
    if (!faces) return std::nullopt;
    return buildSkinBitmapTextAtlas(id, request.key, *faces,
                                    request.codepoints, request.pairs,
                                    safetyPolicy);
  }
  const auto faces = readFontFaces(request, files, session, diagnostics,
                                   cancellationRequested, safetyPolicy);
  if (!faces) return std::nullopt;
  return buildSkinTextAtlas(id, request.key, *faces, request.codepoints,
                            request.pairs, safetyPolicy);
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
    return AtlasAccountingDelta{.decodedBytes = atlas.pixels.byteSize(),
                                .physicalResources = 1};
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
    std::size_t kerningPairs, std::size_t physicalResources) noexcept {
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
                                   SkinResourcePolicy::maximumKerningPairs))) {
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
      if (!skinResourceDimensionsAllowed(atlas.pixels.width,
                                         atlas.pixels.height,
                                         atlas.pixels.byteSize(),
                                         safetyPolicy) ||
          atlas.pixels.width > deviceMaximumDimension ||
          atlas.pixels.height > deviceMaximumDimension ||
          atlas.pixels.byteSize() > skinResourceLimit(
                                        safetyPolicy,
                                        SkinResourcePolicy::maximumAtlasBytes) ||
          !atlas.pages.empty()) {
        result.diagnostics.push_back(diagnostic(
            "skin.resource.atlas_limit",
            "resource upload plan has invalid scalable atlas pixels"));
        return result;
      }
      atlasDecodedBytes = atlas.pixels.byteSize();
      atlasPhysicalResources = 1;
    } else {
      if (atlas.pixels.width != 0 || atlas.pixels.height != 0 ||
          atlas.pixels.rgba ||
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
      } else {
        validRegion = metric.page == 0 &&
                      canonicalRegion(metric.region, atlas.pixels.width,
                                      atlas.pixels.height);
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
                          atlas.kerning.size(), atlasPhysicalResources)) {
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
      const auto handle = catalog->device_->create(atlas.pixels, safetyPolicy);
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
    if (existing->second.pixels != nullptr &&
        *existing->second.pixels == *data.rgba) {
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
        if (stop.stop_requested() || !request.encoded || !decoder_) return std::nullopt;
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
        resource->virtualPath, input.configuration, input.safetyPolicy);
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
      input.requiredRuntimeStrings, result.diagnostics, input.safetyPolicy);
  SkinTextAtlasId atlasId = 1;
  BitmapFontPreparationCache bitmapFontCache;
  std::set<std::string, std::less<>> accountedBitmapPages;
  for (const auto &request : fontRequests) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    SkinResourceSessionAccounting fontSession = session;
    BitmapFontAccountingIdentities requestAccounting;
    const auto built = prepareFontAtlas(
        atlasId, request, input.fileSystem, fontSession, result.diagnostics,
        [this, &input] { return cancellationRequested(input.stop); },
        input.safetyPolicy, input.stop, bitmapFontCache, requestAccounting);
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!built) continue;
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!built->atlas) result.diagnostics.push_back(fontDiagnostic(request.font, request.critical, "skin.resource.glyph_missing", built->error));
    else if (const auto delta = atlasAccountingDelta(
                 *built->atlas, accountedBitmapPages);
             !delta || !fontSession.addAtlas(
                           delta->decodedBytes, built->atlas->glyphs.size(),
                           built->atlas->kerning.size(),
                           delta->physicalResources))
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
  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue;
    const auto use = uses.images.find(resource->id);
    if (use == uses.images.end()) continue;
    const auto configured = applyConfiguredFileSelection(
        resource->virtualPath, input.configuration, input.safetyPolicy);
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
    if (const auto found = unique.find(*candidate.normalizedVirtualPath); found != unique.end()) {
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
    file_checksum::Sha256 digest;
    digest.update(std::span<const std::byte>(read.bytes.data(),
                                             read.bytes.size()));
    const std::string key = plan.revision.revision().lowercaseSha256 + ":" +
                            *candidate.normalizedVirtualPath + ":" +
                            digest.finalHex();
    std::optional<image_decode::DecodedImageData> decoded;
    { std::lock_guard lock(serviceMutex_); decoded = cache_.get(key); }
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!decoded) {
      if (builtInDecoder_ && !input.safetyPolicy.enforces(
                                 SkinSafetyGuard::ResourceAllocationLimit)) {
        decoded = image_decode::decodeImageMemory(
            read.bytes,
            {.maximumDimension = skinResourceDimensionLimit(input.safetyPolicy),
             .maximumEncodedBytes = skinResourceLimit(
                 input.safetyPolicy,
                 SkinResourcePolicy::maximumEncodedBytes),
             .maximumDecodedBytes = skinResourceLimit(
                 input.safetyPolicy,
                 SkinResourcePolicy::maximumImageBytes),
             .stop = input.stop});
      } else {
        auto owned = std::make_shared<const std::vector<std::byte>>(
            std::move(read.bytes));
        const auto ticket = coordinator_.request(
            {.key = key, .path = {}, .encoded = std::move(owned)});
        const auto waited = coordinator_.waitTake(ticket, input.stop);
        if (waited.state == image_decode::ImageDecodeWaitState::Cancelled ||
            waited.state == image_decode::ImageDecodeWaitState::Stopped ||
            cancellationRequested(input.stop)) {
          result.cancelled = true;
          return result;
        }
        if (waited.state == image_decode::ImageDecodeWaitState::Ready &&
            waited.image) {
          decoded = waited.image;
        }
      }
      if (!decoded) {
        result.diagnostics.push_back(useDiagnostic(
            "skin.resource.image_decode_failed",
            "skin.resource.image_decode_failed",
            "image decode failed during planning", use->second.critical));
        continue;
      }
      {
        std::lock_guard lock(serviceMutex_);
        if (state_ != State::Running || stop_.stop_requested()) { result.cancelled = true; return result; }
        cache_.put(key, *decoded);
      }
    }
    if (!skinResourceDimensionsAllowed(decoded->width, decoded->height,
                                       decoded->byteSize(),
                                       input.safetyPolicy)) { result.diagnostics.push_back(useDiagnostic("skin.resource.session_limit", "skin.resource.session_limit", "decoded resource bytes exceed the session policy", use->second.critical)); continue; }
    std::vector<SkinSourceRect> regions;
    std::vector<SkinResolvedRegion> mappings;
    if (!resolveRegions(use->second, decoded->width, decoded->height,
                        regions, &mappings, result.diagnostics,
                        input.safetyPolicy)) continue;
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
    plan.images.push_back({.id=resource->id, .pixels=*decoded, .regions=std::move(regions), .regionMappings=std::move(mappings)});
  }
  const auto fontRequests = collectFontAtlasRequests(
      input.model, uses, input.fileSystem, input.configuration,
      input.requiredRuntimeStrings, result.diagnostics, input.safetyPolicy);
  SkinTextAtlasId atlasId = 1;
  BitmapFontPreparationCache bitmapFontCache;
  std::set<std::string, std::less<>> accountedBitmapPages;
  for (const auto &request : fontRequests) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    SkinResourceSessionAccounting fontSession = session;
    BitmapFontAccountingIdentities requestAccounting;
    const auto built = prepareFontAtlas(
        atlasId, request, input.fileSystem, fontSession, result.diagnostics,
        [this, &input] { return cancellationRequested(input.stop); },
        input.safetyPolicy, input.stop, bitmapFontCache, requestAccounting);
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
                                        delta->physicalResources)) {
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
} // namespace skin
#endif
