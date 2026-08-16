#include "SkinResourceCatalog.h"
#include "SkinTextAtlas.h"
#include "../LuaGameplaySkinFeature.h"
#include "../package/SkinPackageTypes.h"
#include "../../FileChecksum.h"
#include "../../view/ImageFileDecoder.h"

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
namespace {
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
    const BeatorajaSkinConfiguration &configuration) {
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
  if (wildcard > SkinPackagePolicy::maxPathBytes ||
      match->selectedValue.size() >
          SkinPackagePolicy::maxPathBytes - wildcard ||
      suffixSize > SkinPackagePolicy::maxPathBytes - wildcard -
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

CollectedResourceUses collectResourceUses(const ValidatedBeatorajaSkinModel &model) {
  CollectedResourceUses result;
  std::map<SkinObjectId, const SkinObjectDefinition *> objects;
  std::set<SkinObjectId> disabled(model.disabledOptionalObjects.begin(), model.disabledOptionalObjects.end());
  for (const auto &object : model.model.objects) objects.emplace(object.id, &object);
  std::set<SkinObjectId> visited;
  const auto addSprite = [&](const SkinSpriteFrames &sprite, bool critical) {
    auto &use = result.images[sprite.resource];
    use.critical = use.critical || critical;
    use.regions.insert(use.regions.end(), sprite.frames.begin(), sprite.frames.end());
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
#else
void recordRegionIdentityCheck() noexcept {}
void recordRegionLookupComparison() noexcept {}
void recordFontAtlasRequestCount(std::size_t) noexcept {}
#endif

bool resolveRegions(const ResourceUse &use, int width, int height,
                    std::vector<SkinSourceRect> &output,
                    std::vector<SkinResolvedRegion> *mappings,
                    std::vector<SkinDiagnostic> &diagnostics) {
  if (use.regions.size() > SkinResourcePolicy::maximumRegions ||
      output.size() > SkinResourcePolicy::maximumRegions - use.regions.size()) {
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
                                std::vector<SkinDiagnostic> &diagnostics) {
  if (strings.size() > SkinResourcePolicy::maximumRuntimeStrings) {
    diagnostics.push_back(diagnostic("skin.resource.session_limit",
                                     "runtime string count exceeds the resource policy"));
    return false;
  }
  std::size_t bytes = 0;
  for (const std::string &value : strings) {
    if (value.size() > SkinResourcePolicy::maximumRuntimeStringBytes - bytes) {
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

SkinDiagnostic fontDiagnostic(const SkinFontResource &resource, bool critical,
                             std::string code, std::string message) {
  return useDiagnostic(code, code, std::move(message) + ": " + resource.authoredName,
                       critical);
}

bool appendUtf8(std::string_view value, std::set<char32_t> &codepoints,
                std::set<std::pair<char32_t, char32_t>> &pairs) {
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
  return codepoints.size() <= SkinResourcePolicy::maximumGlyphs &&
         pairs.size() <= SkinResourcePolicy::maximumKerningPairs;
}

struct FontAtlasRequest {
  SkinTextAtlasKey key;
  const SkinFontResource *font = nullptr;
  std::vector<std::string> resolvedFacePaths;
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
    std::vector<SkinDiagnostic> &diagnostics) {
  std::map<SkinResourceId, const SkinFontResource *> resources;
  for (const auto &definition : model.model.resources)
    if (const auto *font = std::get_if<SkinFontResource>(&definition)) resources.emplace(font->id, font);
  std::map<SkinTextAtlasKey, FontAtlasRequest> requests;
  for (const auto &text : uses.texts) {
    const auto found = resources.find(text.key.font);
    if (found == resources.end()) {
      diagnostics.push_back(useDiagnostic("skin.resource.font_missing", "skin.resource.font_optional", "text uses an undeclared font resource", text.critical));
      continue;
    }
    SkinTextAtlasKey key = text.key;
    std::string digest = std::to_string(found->second->id) + ':' +
                         std::to_string(found->second->type);
    bool resolved = true;
    bool digestWithinPolicy = true;
    std::vector<std::string> resolvedFacePaths;
    const auto addResolved = [&](std::string_view path, int type) {
      if (!digestWithinPolicy) return;
      const auto configured = applyConfiguredFileSelection(path, configuration);
      if (!configured.path) { resolved = false; return; }
      const auto candidate = files.resolveResourceCandidates(
          *configured.path, *configured.path);
      if (!candidate.normalizedVirtualPath) { resolved = false; return; }
      digestWithinPolicy = appendStableFallbackChainEntry(
          digest, *candidate.normalizedVirtualPath, type);
      if (digestWithinPolicy) {
        resolvedFacePaths.push_back(*candidate.normalizedVirtualPath);
      }
    };
    addResolved(found->second->virtualPath, found->second->type);
    for (const auto &fallback : found->second->fallbacks) {
      if (!fallback.virtualPath.empty()) {
        addResolved(fallback.virtualPath, fallback.type);
      }
    }
    if (!resolved) {
      diagnostics.push_back(fontDiagnostic(*found->second, text.critical, "skin.resource.font_path_ambiguous", "font primary or fallback path is invalid or ambiguous"));
      continue;
    }
    if (!digestWithinPolicy) {
      diagnostics.push_back(fontDiagnostic(*found->second, text.critical,
          "skin.resource.font_style_invalid",
          "font fallback-chain identity exceeds atlas key policy"));
      continue;
    }
    key.fallbackChainDigest = std::move(digest);
    if (!canonicalizeSkinTextAtlasKey(key)) {
      diagnostics.push_back(fontDiagnostic(*found->second, text.critical, "skin.resource.font_style_invalid", "font atlas style is invalid"));
      continue;
    }
    auto request = requests.find(key);
    if (request == requests.end()) {
      if (requests.size() >= SkinResourcePolicy::maximumAtlases) {
        diagnostics.push_back(fontDiagnostic(
            *found->second, text.critical, "skin.resource.atlas_limit",
            "font atlas request count exceeds policy"));
        continue;
      }
      request = requests.emplace(
          key, FontAtlasRequest{.key=key, .font=found->second,
                                .resolvedFacePaths=std::move(resolvedFacePaths),
                                .critical=text.critical}).first;
      recordFontAtlasRequestCount(requests.size());
    }
    request->second.objects.push_back(text.object);
    request->second.critical = request->second.critical || text.critical;
    if (!appendUtf8(text.literal, request->second.codepoints, request->second.pairs))
      diagnostics.push_back(fontDiagnostic(*found->second, request->second.critical, "skin.resource.atlas_limit", "font glyph or kerning limit exceeds policy"));
    if (text.receivesRuntimeStrings) for (const auto &runtime : runtimeStrings)
      if (!appendUtf8(runtime, request->second.codepoints, request->second.pairs))
        diagnostics.push_back(fontDiagnostic(*found->second, request->second.critical, "skin.resource.atlas_limit", "font glyph or kerning limit exceeds policy"));
  }
  std::vector<FontAtlasRequest> result;
  result.reserve(requests.size());
  SkinResourceSessionAccounting metadata;
  for (auto &[key, request] : requests) {
    (void)key;
    if (!metadata.addAtlas(/*decodedBytes=*/0, request.codepoints.size(),
                           request.pairs.size())) {
      diagnostics.push_back(fontDiagnostic(
          *request.font, request.critical, "skin.resource.atlas_limit",
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
    const std::function<bool()> &cancellationRequested) {
  std::vector<SkinTextAtlasFontBytes> faces;
  for (const std::string &path : request.resolvedFacePaths) {
    if (cancellationRequested()) return std::nullopt;
    const std::string extension = std::filesystem::path(path).extension().string();
    std::string lowerExtension;
    lowerExtension.reserve(extension.size());
    for (const char character : extension) lowerExtension.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    if (lowerExtension == ".fnt") {
      diagnostics.push_back(fontDiagnostic(*request.font, request.critical, "skin.resource.font_format_unsupported", "bitmap .fnt fonts are unsupported"));
      return std::nullopt;
    }
    if (lowerExtension != ".ttf" && lowerExtension != ".otf") {
      diagnostics.push_back(fontDiagnostic(*request.font, request.critical, "skin.resource.font_format_unsupported", "font extension must be TTF or OTF"));
      return std::nullopt;
    }
    if (cancellationRequested()) return std::nullopt;
    const auto read = files.readResolvedResource(
        path, SkinResourcePolicy::maximumEncodedBytes);
    if (cancellationRequested()) return std::nullopt;
    if (read.failure) {
      diagnostics.push_back(fontDiagnostic(*request.font, request.critical, "skin.resource.font_missing", "font bytes are unavailable"));
      return std::nullopt;
    }
    if (!session.addImage(/*physicalResources=*/0, /*logicalResources=*/0,
                          read.bytes.size(), /*decodedBytes=*/0,
                          /*regions=*/0)) {
      diagnostics.push_back(fontDiagnostic(*request.font, request.critical, "skin.resource.encoded_limit", "font encoded bytes exceed session policy"));
      return std::nullopt;
    }
    faces.push_back({.encoded=read.bytes});
  }
  return faces;
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
#endif

bool skinResourceDimensionsAllowed(int width, int height, std::size_t bytes) noexcept {
  if (width <= 0 || height <= 0 || width > SkinResourcePolicy::maximumDimension || height > SkinResourcePolicy::maximumDimension) return false;
  const auto pixels = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
  return pixels <= std::numeric_limits<std::size_t>::max()/4U && bytes == static_cast<std::size_t>(pixels)*4U && bytes <= SkinResourcePolicy::maximumImageBytes && bytes <= UINT32_MAX;
}

namespace {
bool addWithin(std::size_t &value, std::size_t increment,
               std::size_t maximum) noexcept {
  if (increment > maximum - value) return false;
  value += increment;
  return true;
}
}

bool SkinResourceSessionAccounting::addImage(
    std::size_t physicalResources, std::size_t logicalResources,
    std::size_t encodedBytes, std::size_t decodedBytes,
    std::size_t regions) noexcept {
  SkinResourceSessionAccounting next = *this;
  if (!addWithin(next.physicalResources_, physicalResources,
                 SkinResourcePolicy::maximumResources) ||
      !addWithin(next.logicalResources_, logicalResources,
                 SkinResourcePolicy::maximumResources) ||
      !addWithin(next.encodedBytes_, encodedBytes,
                 SkinResourcePolicy::maximumSessionEncodedBytes) ||
      !addWithin(next.decodedBytes_, decodedBytes,
                 SkinResourcePolicy::maximumSessionDecodedBytes) ||
      !addWithin(next.regions_, regions, SkinResourcePolicy::maximumRegions)) {
    return false;
  }
  *this = next;
  return true;
}

bool SkinResourceSessionAccounting::addAtlas(
    std::size_t decodedBytes, std::size_t glyphs,
    std::size_t kerningPairs) noexcept {
  SkinResourceSessionAccounting next = *this;
  if (!addWithin(next.atlases_, 1, SkinResourcePolicy::maximumAtlases) ||
      !addWithin(next.atlasBytes_, decodedBytes,
                 SkinResourcePolicy::maximumAtlasSessionBytes) ||
      !addWithin(next.decodedBytes_, decodedBytes,
                 SkinResourcePolicy::maximumSessionDecodedBytes) ||
      !addWithin(next.glyphs_, glyphs, SkinResourcePolicy::maximumGlyphs) ||
      !addWithin(next.kerningPairs_, kerningPairs,
                 SkinResourcePolicy::maximumKerningPairs)) {
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
  if (!device || !device->ownsCurrentThread()) { result.diagnostics.push_back(diagnostic("skin.resource.render_thread_violation", "resource upload requires the render owner thread")); return result; }
  const int reportedDeviceMaximumDimension = device->maximumTextureDimension();
  if (reportedDeviceMaximumDimension <= 0) {
    result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "texture device reports an invalid maximum dimension")); return result;
  }
  const int deviceMaximumDimension = std::min(reportedDeviceMaximumDimension, SkinResourcePolicy::maximumDimension);
  if (plan.decodedBytes > SkinResourcePolicy::maximumSessionDecodedBytes) {
    result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan exceeds fixed limits"));
    return result;
  }
  std::set<SkinResourceId> imageIds;
  std::set<SkinTextAtlasId> atlasIds;
  std::set<SkinTextAtlasKey> atlasKeys;
  SkinResourceSessionAccounting session;
  const auto canonicalRegion = [](const SkinSourceRect &region, int width, int height) {
    SkinSourceRect resolved;
    return skinResourceResolveRect(region, width, height, resolved) &&
           resolved.x == region.x && resolved.y == region.y && resolved.w == region.w && resolved.h == region.h &&
           resolved.gridColumn == region.gridColumn && resolved.gridRow == region.gridRow &&
           resolved.gridColumns == region.gridColumns && resolved.gridRows == region.gridRows;
  };
  for (const auto &image : plan.images) {
    if (image.id == 0 || !imageIds.insert(image.id).second ||
        imageIds.size() > SkinResourcePolicy::maximumResources ||
        image.aliases.size() >= SkinResourcePolicy::maximumResources) {
      result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has duplicate image IDs")); return result;
    }
    for (SkinResourceId alias : image.aliases) {
      if (alias == 0 || !imageIds.insert(alias).second ||
          imageIds.size() > SkinResourcePolicy::maximumResources) {
        result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed", "resource upload plan has duplicate image aliases")); return result;
      }
    }
    if (!skinResourceDimensionsAllowed(image.pixels.width, image.pixels.height, image.pixels.byteSize()) ||
        image.pixels.width > deviceMaximumDimension || image.pixels.height > deviceMaximumDimension ||
        image.pixels.byteSize() > SkinResourcePolicy::maximumSessionDecodedBytes) {
      result.diagnostics.push_back(diagnostic("skin.resource.image_dimensions", "resource upload plan has invalid image dimensions or bytes")); return result;
    }
    if (image.regions.size() > SkinResourcePolicy::maximumRegions) { result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource region count exceeds fixed limit")); return result; }
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
      if (count > SkinResourcePolicy::maximumRegions ||
          count > SkinResourcePolicy::maximumRegions - imageRegions) {
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
          aliasRegions.size() > SkinResourcePolicy::maximumRegions) {
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
  for (const auto &atlas : plan.atlases) {
    const auto finite = [](double value) { return std::isfinite(value) && std::abs(value) <= 4096.0; };
    if (atlas.id == 0 || !finite(atlas.key.outlineWidth) || !finite(atlas.key.shadowOffsetX) ||
        !finite(atlas.key.shadowOffsetY) || !finite(atlas.key.shadowSmoothness) ||
        !atlasIds.insert(atlas.id).second || !atlasKeys.insert(atlas.key).second ||
        !skinResourceDimensionsAllowed(atlas.pixels.width, atlas.pixels.height, atlas.pixels.byteSize()) ||
        atlas.pixels.width > deviceMaximumDimension || atlas.pixels.height > deviceMaximumDimension ||
        atlas.pixels.byteSize() > SkinResourcePolicy::maximumAtlasBytes ||
        atlas.glyphs.size() > SkinResourcePolicy::maximumGlyphs || atlas.kerning.size() > SkinResourcePolicy::maximumKerningPairs) {
      result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has invalid or duplicate atlas data")); return result;
    }
    SkinTextAtlasKey canonicalKey = atlas.key;
    if (!canonicalizeSkinTextAtlasKey(canonicalKey) || canonicalKey != atlas.key ||
        atlas.lineHeight <= 0 || atlas.lineHeight > SkinResourcePolicy::maximumDimension ||
        std::llabs(static_cast<long long>(atlas.ascent)) > SkinResourcePolicy::maximumDimension ||
        atlas.capHeight <= 0 ||
        atlas.capHeight > SkinResourcePolicy::maximumDimension ||
        std::llabs(static_cast<long long>(atlas.descent)) > SkinResourcePolicy::maximumDimension) {
      result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has noncanonical atlas data")); return result;
    }
    for (const auto &[codepoint, metric] : atlas.glyphs) {
      (void)codepoint;
      if (!canonicalRegion(metric.region, atlas.pixels.width, atlas.pixels.height) ||
          std::llabs(static_cast<long long>(metric.bearingX)) > SkinResourcePolicy::maximumDimension ||
          std::llabs(static_cast<long long>(metric.bearingY)) > SkinResourcePolicy::maximumDimension ||
          std::llabs(static_cast<long long>(metric.advance)) > SkinResourcePolicy::maximumDimension ||
          std::llabs(static_cast<long long>(metric.layoutOffsetY)) > SkinResourcePolicy::maximumDimension) { result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has invalid glyph region")); return result; }
    }
    for (const auto &[pair, amount] : atlas.kerning) {
      if (!atlas.glyphs.contains(pair.first) || !atlas.glyphs.contains(pair.second) ||
          std::llabs(static_cast<long long>(amount)) > SkinResourcePolicy::maximumDimension) { result.diagnostics.push_back(diagnostic("skin.resource.atlas_limit", "resource upload plan has invalid kerning pair")); return result; }
    }
    if (!session.addAtlas(atlas.pixels.byteSize(), atlas.glyphs.size(),
                          atlas.kerning.size())) {
      result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan exceeds fixed aggregate limits")); return result;
    }
  }
  if (plan.textAtlasesByObject.size() >
      SkinResourcePolicy::maximumTextAtlasUses) {
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
  if (session.decodedBytes() != plan.decodedBytes) {
    result.diagnostics.push_back(diagnostic("skin.resource.session_limit", "resource upload plan decoded byte total is inconsistent")); return result;
  }
  try {
  auto catalog=std::unique_ptr<SkinResourceCatalog>(new SkinResourceCatalog(std::move(plan.revision), std::move(device), std::move(liveCounters)));
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
    const auto handle = catalog->device_->create(image.pixels);
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
  for (const auto &atlas : plan.atlases) { const auto handle=catalog->device_->create(atlas.pixels); if(!bgfx::isValid(handle)){result.diagnostics.push_back(diagnostic("skin.resource.texture_create_failed","texture creation failed")); rollback(); return result;} PendingHandle pending{*catalog->device_, handle}; catalog->atlases_.emplace(atlas.id,PreparedSkinTextAtlas{.id=atlas.id,.key=atlas.key,.texture=handle,.width=atlas.pixels.width,.height=atlas.pixels.height,.glyphs=atlas.glyphs,.kerning=atlas.kerning,.ascent=atlas.ascent,.capHeight=atlas.capHeight,.descent=atlas.descent,.lineHeight=atlas.lineHeight}); catalog->atlasKeys_.emplace(atlas.key,atlas.id); catalog->owned_.push_back({handle}); if(catalog->liveCounters_) catalog->liveCounters_->textureCreated(); pending.release(); }
  catalog->textAtlasesByObject_ = std::move(plan.textAtlasesByObject);
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
          SkinResourcePolicy::workerCount) {}

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
      !runtimeStringsWithinPolicy(input.requiredRuntimeStrings, result.diagnostics)) return result;
  const CollectedResourceUses uses = collectResourceUses(input.model);
  struct ValidatedImage { int width = 0; int height = 0; };
  std::map<std::string, ValidatedImage, std::less<>> decodedByPath;
  SkinResourceSessionAccounting session;
  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue; // Font rasterization is intentionally a later slice.
    const auto use = uses.images.find(resource->id);
    if (use == uses.images.end()) continue;
    const auto configured = applyConfiguredFileSelection(
        resource->virtualPath, input.configuration);
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
    auto decoded = decodedByPath.find(*candidate.normalizedVirtualPath);
    if (decoded == decodedByPath.end()) {
      const auto read = input.fileSystem.readResolvedResource(*candidate.normalizedVirtualPath, SkinResourcePolicy::maximumEncodedBytes);
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
          {.maximumDimension = SkinResourcePolicy::maximumDimension,
           .maximumEncodedBytes = SkinResourcePolicy::maximumEncodedBytes,
           .maximumDecodedBytes = SkinResourcePolicy::maximumImageBytes,
           .stop = input.stop});
      if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
      if (!image || !skinResourceDimensionsAllowed(image->width, image->height, image->byteSize())) { result.diagnostics.push_back(useDiagnostic("skin.resource.image_decode_failed", "skin.resource.image_decode_failed", "image decode failed resource validation", use->second.critical)); continue; }
      std::vector<SkinSourceRect> ignored;
      if (!resolveRegions(use->second, image->width, image->height, ignored,
                          nullptr, result.diagnostics)) {
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
                          result.diagnostics)) {
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
      input.requiredRuntimeStrings, result.diagnostics);
  SkinTextAtlasId atlasId = 1;
  for (const auto &request : fontRequests) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    SkinResourceSessionAccounting fontSession = session;
    const auto faces = readFontFaces(
        request, input.fileSystem, fontSession, result.diagnostics,
        [this, &input] { return cancellationRequested(input.stop); });
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!faces) continue;
    const auto built = buildSkinTextAtlas(atlasId, request.key, *faces, request.codepoints, request.pairs);
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!built.atlas) result.diagnostics.push_back(fontDiagnostic(*request.font, request.critical, "skin.resource.glyph_missing", built.error));
    else if (!fontSession.addAtlas(built.atlas->pixels.byteSize(),
                                   built.atlas->glyphs.size(),
                                   built.atlas->kerning.size()))
      result.diagnostics.push_back(fontDiagnostic(*request.font, request.critical, "skin.resource.atlas_limit", "font atlas session aggregate exceeds policy"));
    else {
      session = fontSession;
      ++atlasId;
    }
  }
  {
    std::lock_guard lock(serviceMutex_);
    if (state_ != State::Running || stop_.stop_requested() ||
        input.stop.stop_requested()) { result.cancelled = true; return result; }
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
      !runtimeStringsWithinPolicy(input.requiredRuntimeStrings, result.diagnostics)) return result;
  const CollectedResourceUses uses = collectResourceUses(input.model);
  SkinResourceUploadPlan plan{.revision = std::move(input.revision)};
  std::map<std::string, std::size_t, std::less<>> unique;
  SkinResourceSessionAccounting session;
  for (const SkinResourceDefinition &definition : input.model.model.resources) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    const auto *resource = std::get_if<SkinImageResource>(&definition);
    if (!resource) continue;
    const auto use = uses.images.find(resource->id);
    if (use == uses.images.end()) continue;
    const auto configured = applyConfiguredFileSelection(
        resource->virtualPath, input.configuration);
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
    if (const auto found = unique.find(*candidate.normalizedVirtualPath); found != unique.end()) {
      auto &image=plan.images[found->second];
      std::vector<SkinSourceRect> regions;
      std::vector<SkinResolvedRegion> mappings;
      if (resolveRegions(use->second,image.pixels.width,image.pixels.height,
                         regions,&mappings,result.diagnostics)) {
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
    const auto read = input.fileSystem.readResolvedResource(*candidate.normalizedVirtualPath, SkinResourcePolicy::maximumEncodedBytes);
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
      auto owned = std::make_shared<const std::vector<std::byte>>(std::move(read.bytes));
      const auto ticket = coordinator_.request({.key=key, .path={}, .encoded=std::move(owned)});
      const auto waited = coordinator_.waitTake(ticket, input.stop);
      if (waited.state == image_decode::ImageDecodeWaitState::Cancelled || waited.state == image_decode::ImageDecodeWaitState::Stopped || cancellationRequested(input.stop)) { result.cancelled = true; return result; }
      if (waited.state != image_decode::ImageDecodeWaitState::Ready || !waited.image) { result.diagnostics.push_back(useDiagnostic("skin.resource.image_decode_failed", "skin.resource.image_decode_failed", "image decode failed during planning", use->second.critical)); continue; }
      decoded = waited.image;
      {
        std::lock_guard lock(serviceMutex_);
        if (state_ != State::Running || stop_.stop_requested()) { result.cancelled = true; return result; }
        cache_.put(key, *decoded);
      }
    }
    if (!skinResourceDimensionsAllowed(decoded->width, decoded->height, decoded->byteSize())) { result.diagnostics.push_back(useDiagnostic("skin.resource.session_limit", "skin.resource.session_limit", "decoded resource bytes exceed the session policy", use->second.critical)); continue; }
    std::vector<SkinSourceRect> regions;
    std::vector<SkinResolvedRegion> mappings;
    if (!resolveRegions(use->second, decoded->width, decoded->height, regions, &mappings, result.diagnostics)) continue;
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
      input.requiredRuntimeStrings, result.diagnostics);
  SkinTextAtlasId atlasId = 1;
  for (const auto &request : fontRequests) {
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    SkinResourceSessionAccounting fontSession = session;
    const auto faces = readFontFaces(
        request, input.fileSystem, fontSession, result.diagnostics,
        [this, &input] { return cancellationRequested(input.stop); });
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!faces) continue;
    const auto built = buildSkinTextAtlas(atlasId, request.key, *faces, request.codepoints, request.pairs);
    if (cancellationRequested(input.stop)) { result.cancelled = true; return result; }
    if (!built.atlas) {
      result.diagnostics.push_back(fontDiagnostic(*request.font, request.critical, "skin.resource.glyph_missing", built.error));
      continue;
    }
    if (!fontSession.addAtlas(built.atlas->pixels.byteSize(),
                              built.atlas->glyphs.size(),
                              built.atlas->kerning.size())) {
      result.diagnostics.push_back(fontDiagnostic(*request.font, request.critical, "skin.resource.atlas_limit", "font atlas session aggregate exceeds policy"));
      continue;
    }
    session = fontSession;
    plan.decodedBytes = session.decodedBytes();
    plan.atlases.push_back(*built.atlas);
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
    result.plan = std::move(plan);
  }
  return result;
}
} // namespace skin
#endif
