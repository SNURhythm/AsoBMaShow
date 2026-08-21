#include "PomyuCharaResource.h"

#include "PomyuCharaCycles.h"
#include "SkinResourceCatalog.h"
#include "../package/SkinPackageTypes.h"
#include "../../Utils.h"
#include "../../view/ImageFileDecoder.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace skin {
namespace {

constexpr int kUnspecified = std::numeric_limits<int>::min();
constexpr std::size_t kCoordinateCount = 36U * 36U;
constexpr std::size_t kImageCount = 8;
constexpr std::size_t kMaximumRelevantFieldBytes = 64U * 1024U;

#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
std::atomic_size_t pomyuFileReadsForTestingValue{0};
std::atomic_size_t pomyuRequirementParsesForTestingValue{0};
std::atomic_size_t pomyuCycleParsesForTestingValue{0};
#endif

enum class DirectiveKind : std::uint8_t { Pattern, Texture, Layer };

struct ChpDirective {
  DirectiveKind kind = DirectiveKind::Pattern;
  int motion = kUnspecified;
  std::array<std::string, 4> destination;
};

struct ChpModel {
  std::array<std::optional<std::string>, kImageCount> imagePaths;
  std::array<SkinSourceRect, kCoordinateCount> coordinates{};
  SkinSourceRect faceUpper{.x = 0, .y = 0, .w = 256, .h = 256};
  SkinSourceRect faceAll{.x = 320, .y = 0, .w = 320, .h = 480};
  std::array<int, 2> size{};
  std::array<int, 20> frames{};
  std::array<int, 20> loops{};
  int anime = 100;
  bool hasTextureDefinitions = false;
  std::vector<ChpDirective> directives;
};

struct FileCache {
  std::map<std::string, std::optional<std::string>> chpByConfiguredPath;
  std::map<std::string, std::optional<std::vector<std::byte>>> bytesByPath;
  std::map<std::string, SkinFileError> failuresByPath;
  std::map<std::string, std::optional<ChpModel>> modelsByPath;
  std::map<std::pair<std::string, bool>, std::optional<SkinResourceId>>
      imagesByPath;
  std::set<std::tuple<std::string, int, int>> parsedCycleKeys;
  std::size_t encodedBytes = 0;
  std::size_t decodedBytes = 0;
  bool budgetExceeded = false;
  bool cancelled = false;
  SkinResourceId nextImageId = 1;
  std::vector<PomyuCharaDecodedImage> images;
};

bool hasChpExtension(const std::filesystem::path &path) {
  std::string extension = path.extension().string();
  std::ranges::transform(extension, extension.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return extension == ".chp";
}

std::optional<std::string> configuredPath(
    std::string_view authored, const BeatorajaSkinConfiguration &configuration,
    SkinSafetyPolicy safetyPolicy) {
  const ConfiguredFile *match = nullptr;
  for (const auto &file : configuration.orderedFiles) {
    if (!authored.starts_with(file.pattern)) {
      continue;
    }
    if (match != nullptr) {
      return std::nullopt;
    }
    match = &file;
  }
  if (match == nullptr) {
    return authored.find('*') == std::string_view::npos
               ? std::optional<std::string>(authored)
               : std::nullopt;
  }
  const std::size_t wildcard = authored.rfind('*');
  if (wildcard == std::string_view::npos ||
      authored.size() < match->pattern.size()) {
    return std::nullopt;
  }
  const std::size_t suffixSize = authored.size() - match->pattern.size();
  const std::size_t maximumPathBytes =
      skinResourceLimit(safetyPolicy, SkinPackagePolicy::maxPathBytes);
  if (wildcard > maximumPathBytes ||
      match->selectedValue.size() > maximumPathBytes - wildcard ||
      suffixSize > maximumPathBytes - wildcard - match->selectedValue.size()) {
    return std::nullopt;
  }
  std::string selected;
  selected.reserve(wildcard + match->selectedValue.size() + suffixSize);
  selected.append(authored, 0, wildcard);
  selected.append(match->selectedValue);
  selected.append(authored, match->pattern.size(), suffixSize);
  return selected;
}

const std::vector<std::byte> *readFile(const LuaSkinFileSystem &files,
                                       std::string_view path,
                                       SkinSafetyPolicy safetyPolicy,
                                       std::stop_token stop,
                                       FileCache &cache) {
  const std::string key(path);
  if (const auto found = cache.bytesByPath.find(key);
      found != cache.bytesByPath.end()) {
    return found->second ? &*found->second : nullptr;
  }
  if (stop.stop_requested()) {
    cache.cancelled = true;
    return nullptr;
  }
  const std::size_t maximumFileBytes = skinResourceLimit(
      safetyPolicy, SkinResourcePolicy::maximumEncodedBytes);
  const std::size_t maximumSessionBytes = skinResourceLimit(
      safetyPolicy, SkinResourcePolicy::maximumSessionEncodedBytes);
  auto read = files.readResolvedResource(key, maximumFileBytes);
  if (stop.stop_requested()) {
    cache.cancelled = true;
    return nullptr;
  }
  if (read.failure) {
    cache.failuresByPath.emplace(key, read.failure->code);
    cache.bytesByPath.emplace(key, std::nullopt);
    return nullptr;
  }
  if (read.bytes.size() >
      maximumSessionBytes - std::min(cache.encodedBytes, maximumSessionBytes)) {
    cache.budgetExceeded = true;
    return nullptr;
  }
  cache.encodedBytes += read.bytes.size();
  const auto [inserted, ignored] = cache.bytesByPath.emplace(
      key, std::optional<std::vector<std::byte>>(std::move(read.bytes)));
  (void)ignored;
  return &*inserted->second;
}

const std::vector<std::byte> *readChp(
    const LuaSkinFileSystem &files, std::string_view configured,
    SkinSafetyPolicy safetyPolicy, std::stop_token stop, FileCache &cache,
    std::string &resolvedPath) {
  if (const auto found = cache.chpByConfiguredPath.find(std::string(configured));
      found != cache.chpByConfiguredPath.end()) {
    if (!found->second) {
      return nullptr;
    }
    resolvedPath = *found->second;
    return readFile(files, resolvedPath, safetyPolicy, stop, cache);
  }
  const std::filesystem::path path(configured);
  if (hasChpExtension(path)) {
    const auto candidate =
        files.resolveResourceCandidates(configured, configured);
    if (!candidate.normalizedVirtualPath) {
      cache.chpByConfiguredPath.emplace(std::string(configured), std::nullopt);
      return nullptr;
    }
    resolvedPath = *candidate.normalizedVirtualPath;
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
    pomyuFileReadsForTestingValue.fetch_add(1, std::memory_order_relaxed);
#endif
    if (const auto *bytes =
            readFile(files, resolvedPath, safetyPolicy, stop, cache)) {
      cache.chpByConfiguredPath.emplace(std::string(configured), resolvedPath);
      return bytes;
    }
    if (cache.cancelled || cache.budgetExceeded) {
      return nullptr;
    }
    const auto failure = cache.failuresByPath.find(resolvedPath);
    if (failure == cache.failuresByPath.end() ||
        failure->second != SkinFileError::Missing) {
      cache.chpByConfiguredPath.emplace(std::string(configured), std::nullopt);
      return nullptr;
    }
  }
  const std::string directory = hasChpExtension(path)
                                    ? path.parent_path().generic_string()
                                    : std::string(configured);
  const auto listed = files.listResourceDirectory(directory);
  if (stop.stop_requested()) {
    cache.cancelled = true;
    return nullptr;
  }
  if (listed.failure) {
    cache.chpByConfiguredPath.emplace(std::string(configured), std::nullopt);
    return nullptr;
  }
  for (const std::string &entry : listed.entries) {
    if (!hasChpExtension(std::filesystem::path(entry))) {
      continue;
    }
    resolvedPath = entry;
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
    pomyuFileReadsForTestingValue.fetch_add(1, std::memory_order_relaxed);
#endif
    const auto *bytes = readFile(files, entry, safetyPolicy, stop, cache);
    cache.chpByConfiguredPath.emplace(
        std::string(configured),
        bytes != nullptr ? std::optional<std::string>(entry) : std::nullopt);
    return bytes;
  }
  cache.chpByConfiguredPath.emplace(std::string(configured), std::nullopt);
  return nullptr;
}

std::optional<std::string> normalizedResourcePath(
    std::string_view raw, SkinSafetyPolicy safetyPolicy) {
  const std::size_t maximumPathBytes =
      skinResourceLimit(safetyPolicy, SkinPackagePolicy::maxPathBytes);
  if (raw.empty() || raw.size() > maximumPathBytes ||
      raw.find('\0') != std::string_view::npos) {
    return std::nullopt;
  }
  auto utf8 = cp932_to_utf8(raw);
  if (!utf8 || utf8->empty() || utf8->size() > maximumPathBytes ||
      utf8->find('\0') != std::string::npos) {
    return std::nullopt;
  }
  std::ranges::replace(*utf8, '\\', '/');
  return utf8;
}

std::optional<int> parseBase36(std::string_view value) noexcept {
  if (value.size() != 2) {
    return std::nullopt;
  }
  const auto digit = [](char character) -> std::optional<int> {
    if (character >= '0' && character <= '9') {
      return character - '0';
    }
    if (character >= 'a' && character <= 'z') {
      return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'Z') {
      return character - 'A' + 10;
    }
    return std::nullopt;
  };
  const auto high = digit(value[0]);
  const auto low = digit(value[1]);
  return high && low ? std::optional<int>(*high * 36 + *low) : std::nullopt;
}

std::optional<int> parseHex(std::string_view value) noexcept {
  if (value.size() != 2) {
    return std::nullopt;
  }
  int output = 0;
  for (const char character : value) {
    output *= 16;
    if (character >= '0' && character <= '9') {
      output += character - '0';
    } else if (character >= 'a' && character <= 'f') {
      output += character - 'a' + 10;
    } else if (character >= 'A' && character <= 'F') {
      output += character - 'A' + 10;
    } else {
      return std::nullopt;
    }
  }
  return output;
}

bool forEachLine(std::string_view contents, std::stop_token stop,
                 const auto &visit) {
  std::size_t begin = 0;
  while (begin <= contents.size()) {
    if (stop.stop_requested()) {
      return false;
    }
    const std::size_t end = contents.find('\n', begin);
    std::string_view line = contents.substr(
        begin, end == std::string_view::npos ? contents.size() - begin
                                             : end - begin);
    if (line.ends_with('\r')) {
      line.remove_suffix(1);
    }
    if (!visit(line)) {
      return false;
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return true;
}

std::optional<ChpModel> parseChp(std::string_view contents,
                                 SkinSafetyPolicy safetyPolicy,
                                 std::stop_token stop) {
  ChpModel result;
  result.frames.fill(kUnspecified);
  result.loops.fill(-1);
  const bool parsed = forEachLine(contents, stop, [&](std::string_view line) {
    if (!line.starts_with('#')) {
      return true;
    }
    const std::size_t tab = line.find('\t');
    if (tab == std::string_view::npos) {
      return true;
    }
    const std::string_view command = line.substr(0, tab);
    const auto fields = pomyu_chara_cycles_detail::parseFields<6>(line);
    for (std::size_t index = 1; index < fields.size; ++index) {
      if (fields[index].size() > kMaximumRelevantFieldBytes) {
        return false;
      }
    }
    const auto setPath = [&](std::size_t index) {
      if (fields.size > 1) {
        result.imagePaths[index] =
            normalizedResourcePath(fields[1], safetyPolicy);
      }
    };
    if (pomyu_chara_cycles_detail::equalsIgnoreCase(command, "#CharBMP")) {
      setPath(0);
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#CharBMP2P")) {
      setPath(1);
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#CharTex")) {
      setPath(2);
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#CharTex2P")) {
      setPath(3);
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#CharFace")) {
      setPath(4);
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#CharFace2P")) {
      setPath(5);
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#SelectCG")) {
      setPath(6);
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#SelectCG2P")) {
      setPath(7);
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Pattern") ||
               pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Patern") ||
               pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Texture") ||
               pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Layer")) {
      if (fields.size > 1) {
        const auto motion =
            pomyu_chara_cycles_detail::parseDecimal(fields[1]);
        if (!motion || *motion < 0 || *motion >= 20) {
          return true;
        }
        ChpDirective directive;
        directive.kind = pomyu_chara_cycles_detail::equalsIgnoreCase(
                             command, "#Texture")
                             ? DirectiveKind::Texture
                             : pomyu_chara_cycles_detail::equalsIgnoreCase(
                                   command, "#Layer")
                                   ? DirectiveKind::Layer
                                   : DirectiveKind::Pattern;
        directive.motion = *motion;
        for (std::size_t index = 0; index < directive.destination.size();
             ++index) {
          if (fields.size > index + 2) {
            directive.destination[index] =
                pomyu_chara_cycles_detail::compactDestination(
                    fields[index + 2]);
          }
        }
        result.hasTextureDefinitions =
            result.hasTextureDefinitions ||
            directive.kind == DirectiveKind::Texture;
        result.directives.push_back(std::move(directive));
      }
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Frame") ||
               pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Flame")) {
      if (fields.size > 2) {
        const auto index =
            pomyu_chara_cycles_detail::parseDecimal(fields[1]);
        const auto value =
            pomyu_chara_cycles_detail::parseDecimal(fields[2]);
        if (index && value && *index >= 0 && *index < 20) {
          result.frames[static_cast<std::size_t>(*index)] = *value;
        }
      }
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Anime")) {
      if (fields.size > 1) {
        if (const auto value =
                pomyu_chara_cycles_detail::parseDecimal(fields[1])) {
          result.anime = *value;
        }
      }
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Loop")) {
      if (fields.size > 2) {
        const auto index =
            pomyu_chara_cycles_detail::parseDecimal(fields[1]);
        const auto value =
            pomyu_chara_cycles_detail::parseDecimal(fields[2]);
        if (index && value && *index >= 0 && *index < 20) {
          result.loops[static_cast<std::size_t>(*index)] = *value;
        }
      }
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(command,
                                                            "#Size")) {
      if (fields.size > 2) {
        const auto width =
            pomyu_chara_cycles_detail::parseDecimal(fields[1]);
        const auto height =
            pomyu_chara_cycles_detail::parseDecimal(fields[2]);
        if (width && height) {
          result.size = {*width, *height};
        }
      }
    } else if (pomyu_chara_cycles_detail::equalsIgnoreCase(
                   command, "#CharFaceUpperSize") ||
               pomyu_chara_cycles_detail::equalsIgnoreCase(
                   command, "#CharFaceAllSize")) {
      if (fields.size > 4) {
        std::array<int, 4> values{};
        bool valid = true;
        for (std::size_t index = 0; index < values.size(); ++index) {
          const auto value = pomyu_chara_cycles_detail::parseDecimal(
              fields[index + 1]);
          valid = valid && value.has_value();
          values[index] = value.value_or(0);
        }
        if (valid) {
          SkinSourceRect &destination =
              pomyu_chara_cycles_detail::equalsIgnoreCase(
                  command, "#CharFaceUpperSize")
                  ? result.faceUpper
                  : result.faceAll;
          destination = {.x = values[0],
                         .y = values[1],
                         .w = values[2],
                         .h = values[3]};
        }
      }
    } else if (command.size() == 3) {
      const auto index = parseBase36(command.substr(1));
      if (index && fields.size > 4) {
        std::array<int, 4> values{};
        bool valid = true;
        for (std::size_t field = 0; field < values.size(); ++field) {
          const auto value = pomyu_chara_cycles_detail::parseDecimal(
              fields[field + 1]);
          valid = valid && value.has_value();
          values[field] = value.value_or(0);
        }
        if (valid) {
          result.coordinates[static_cast<std::size_t>(*index)] = {
              .x = values[0], .y = values[1], .w = values[2], .h = values[3]};
        }
      }
    }
    return true;
  });
  if (!parsed || stop.stop_requested()) {
    return std::nullopt;
  }
  for (int &frame : result.frames) {
    if (frame == kUnspecified) {
      frame = result.anime;
    }
    if (frame < 1) {
      frame = 100;
    }
  }
  std::stable_sort(result.directives.begin(), result.directives.end(),
                   [](const ChpDirective &left, const ChpDirective &right) {
                     return left.kind < right.kind;
                   });
  return result;
}

std::string joinedResourcePath(std::string_view chpPath,
                               std::string_view declaredPath) {
  std::string joined =
      std::filesystem::path(chpPath).parent_path().generic_string();
  if (!joined.empty() && !joined.ends_with('/')) {
    joined.push_back('/');
  }
  joined.append(declaredPath);
  return std::filesystem::path(joined).lexically_normal().generic_string();
}

void applyTransparentColor(image_decode::DecodedImageData &image) {
  if (!image.valid() || image.width <= 0 || image.height <= 0) {
    return;
  }
  auto pixels = std::make_shared<std::vector<unsigned char>>(*image.rgba);
  const std::size_t keyOffset =
      (static_cast<std::size_t>(image.width) *
           static_cast<std::size_t>(image.height) -
       1U) *
      4U;
  const std::array<unsigned char, 4> key = {
      (*pixels)[keyOffset], (*pixels)[keyOffset + 1],
      (*pixels)[keyOffset + 2], (*pixels)[keyOffset + 3]};
  for (std::size_t offset = 0; offset < pixels->size(); offset += 4U) {
    if ((*pixels)[offset] == key[0] && (*pixels)[offset + 1] == key[1] &&
        (*pixels)[offset + 2] == key[2] &&
        (*pixels)[offset + 3] == key[3]) {
      (*pixels)[offset] = 0;
      (*pixels)[offset + 1] = 0;
      (*pixels)[offset + 2] = 0;
      (*pixels)[offset + 3] = 0;
    }
  }
  image.rgba = std::move(pixels);
}

std::optional<SkinResourceId> decodeImage(
    const LuaSkinFileSystem &files, std::string_view chpPath,
    const std::optional<std::string> &declared, bool transparent,
    SkinSafetyPolicy safetyPolicy, std::stop_token stop, FileCache &cache) {
  if (!declared || declared->empty()) {
    return std::nullopt;
  }
  const std::string path = joinedResourcePath(chpPath, *declared);
  const auto key = std::pair(path, transparent);
  if (const auto found = cache.imagesByPath.find(key);
      found != cache.imagesByPath.end()) {
    return found->second;
  }
  const auto *bytes = readFile(files, path, safetyPolicy, stop, cache);
  if (bytes == nullptr) {
    cache.imagesByPath.emplace(key, std::nullopt);
    return std::nullopt;
  }
  auto decoded = image_decode::decodeImageMemory(
      std::span<const std::byte>(bytes->data(), bytes->size()),
      {.maximumDimension = skinResourceDimensionLimit(safetyPolicy),
       .maximumEncodedBytes = skinResourceLimit(
           safetyPolicy, SkinResourcePolicy::maximumEncodedBytes),
       .maximumDecodedBytes = skinResourceLimit(
           safetyPolicy, SkinResourcePolicy::maximumImageBytes),
       .stop = stop});
  if (stop.stop_requested()) {
    cache.cancelled = true;
    return std::nullopt;
  }
  const std::size_t maximumDecodedBytes = skinResourceLimit(
      safetyPolicy, SkinResourcePolicy::maximumSessionDecodedBytes);
  if (!decoded || !decoded->valid() ||
      decoded->byteSize() >
          maximumDecodedBytes - std::min(cache.decodedBytes,
                                          maximumDecodedBytes)) {
    cache.budgetExceeded = decoded && decoded->valid();
    cache.imagesByPath.emplace(key, std::nullopt);
    return std::nullopt;
  }
  if (transparent) {
    applyTransparentColor(*decoded);
  }
  if (cache.nextImageId == 0) {
    cache.budgetExceeded = true;
    return std::nullopt;
  }
  const SkinResourceId id = cache.nextImageId++;
  cache.decodedBytes += decoded->byteSize();
  cache.images.push_back(
      {.id = id, .pixels = std::move(*decoded), .regions = {}});
  cache.imagesByPath.emplace(key, id);
  return id;
}

PomyuCharaDecodedImage *findImage(FileCache &cache, SkinResourceId id) {
  const auto found = std::ranges::find(cache.images, id,
                                       &PomyuCharaDecodedImage::id);
  return found == cache.images.end() ? nullptr : &*found;
}

bool addRegion(FileCache &cache, SkinResourceId resource,
               const SkinSourceRect &region) {
  if (resource == 0 || region.w <= 0 || region.h <= 0) {
    return false;
  }
  auto *image = findImage(cache, resource);
  SkinSourceRect resolved;
  if (image == nullptr ||
      !skinResourceResolveRect(region, image->pixels.width,
                               image->pixels.height, resolved) ||
      resolved.x != region.x || resolved.y != region.y ||
      resolved.w != region.w || resolved.h != region.h) {
    return false;
  }
  if (std::ranges::any_of(image->regions,
                          [&](const SkinSourceRect &existing) {
        return existing.x == region.x && existing.y == region.y &&
               existing.w == region.w && existing.h == region.h;
      })) {
    return true;
  }
  image->regions.push_back(region);
  return true;
}

int motionForType(int type) noexcept {
  switch (type) {
  case 6: return 1;
  case 7: return 6;
  case 8: return 7;
  case 9: return 8;
  case 10: return 10;
  case 11: return 17;
  case 12: return 15;
  case 13: return 16;
  case 14: return 3;
  case 15: return 14;
  default: return kUnspecified;
  }
}

struct MotionBinding {
  std::optional<int> timer;
  std::array<int, 3> options{};
};

MotionBinding bindingForMotion(int motion, int side, int type) {
  if (type != 0) {
    return {};
  }
  MotionBinding result;
  if (side != 2) {
    switch (motion) {
    case 1: result.timer = 900; break;
    case 6: result.timer = 901; break;
    case 7: result.timer = 902; break;
    case 8: result.timer = 903; break;
    case 10: result.timer = 904; break;
    case 15:
      result.timer = 908;
      result.options = {1240, -240, 0};
      break;
    case 16:
      result.timer = 908;
      result.options = {-1240, 0, 0};
      break;
    case 17:
      result.timer = 908;
      result.options = {240, 0, 0};
      break;
    default: break;
    }
  } else {
    switch (motion) {
    case 1: result.timer = 905; break;
    case 7: result.timer = 906; break;
    case 10: result.timer = 907; break;
    case 15:
      result.timer = 908;
      result.options = {-1240, 0, 0};
      break;
    case 16:
      result.timer = 908;
      result.options = {1240, 0, 0};
      break;
    default: break;
    }
  }
  return result;
}

bool validDestination(const ChpDirective &directive) {
  const auto &destination = directive.destination;
  return !destination[0].empty() && destination[0].size() % 2 == 0 &&
         (destination[1].empty() ||
          destination[1].size() == destination[0].size()) &&
         (destination[2].empty() ||
          destination[2].size() == destination[0].size()) &&
         (destination[3].empty() ||
          destination[3].size() == destination[0].size());
}

int interpolationIncreaseRate(const ChpDirective &directive,
                              int frameMillis) {
  const bool interpolates =
      std::ranges::any_of(directive.destination | std::views::drop(1),
                          [](const std::string &value) {
                            return value.find('-') != std::string::npos;
                          });
  if (!interpolates || frameMillis < 17) {
    return 1;
  }
  for (int rate = 1; rate <= frameMillis; ++rate) {
    if (frameMillis / rate < 17 && frameMillis % rate == 0) {
      return rate;
    }
  }
  return 1;
}

std::vector<std::string> expandedDestinations(const ChpDirective &directive,
                                               int increaseRate) {
  std::vector<std::string> result(directive.destination.begin(),
                                  directive.destination.end());
  for (std::size_t index = 1; index < result.size(); ++index) {
    if (increaseRate == 1 || result[index].empty()) {
      continue;
    }
    std::string expanded;
    expanded.reserve(result[index].size() *
                     static_cast<std::size_t>(increaseRate));
    for (std::size_t offset = 0; offset < result[index].size(); offset += 2) {
      for (int repeat = 0; repeat < increaseRate; ++repeat) {
        expanded.append(result[index], offset, 2);
      }
    }
    result[index] = std::move(expanded);
  }
  return result;
}

bool decodeGeometry(const ChpModel &model, const std::string &encoded,
                    std::vector<SkinSourceRect> &output) {
  output.assign(encoded.empty() ? 0 : encoded.size() / 2,
                {.x = 0,
                 .y = 0,
                 .w = model.size[0],
                 .h = model.size[1]});
  SkinSourceRect start{.x = 0,
                       .y = 0,
                       .w = model.size[0],
                       .h = model.size[1]};
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
    const std::string_view token(encoded.data() + offset, 2);
    if (token == "--") {
      std::size_t count = 0;
      while (offset + count * 2 + 2 <= encoded.size() &&
             std::string_view(encoded.data() + offset + count * 2, 2) ==
                 "--") {
        ++count;
      }
      if (offset + count * 2 + 2 > encoded.size()) {
        return false;
      }
      const auto endIndex = parseBase36(
          std::string_view(encoded.data() + offset + count * 2, 2));
      if (!endIndex) {
        return false;
      }
      const SkinSourceRect end =
          model.coordinates[static_cast<std::size_t>(*endIndex)];
      for (std::size_t step = 0; step < count; ++step) {
        SkinSourceRect value;
        const auto interpolate = [&](int left, int right) {
          return left + (right - left) * static_cast<int>(step + 1) /
                            static_cast<int>(count + 1);
        };
        value.x = interpolate(start.x, end.x);
        value.y = interpolate(start.y, end.y);
        value.w = interpolate(start.w, end.w);
        value.h = interpolate(start.h, end.h);
        output[offset / 2 + step] = value;
      }
      offset += (count - 1) * 2;
      continue;
    }
    const auto index = parseBase36(token);
    if (index) {
      start = model.coordinates[static_cast<std::size_t>(*index)];
      output[offset / 2] = start;
    }
  }
  return true;
}

bool decodeValues(const std::string &encoded, int defaultValue, bool angle,
                  std::vector<int> &output) {
  if (encoded.empty()) {
    std::ranges::fill(output, defaultValue);
    return true;
  }
  int start = 0;
  for (std::size_t offset = 0; offset < encoded.size(); offset += 2) {
    const std::string_view token(encoded.data() + offset, 2);
    if (token == "--") {
      std::size_t count = 0;
      while (offset + count * 2 + 2 <= encoded.size() &&
             std::string_view(encoded.data() + offset + count * 2, 2) ==
                 "--") {
        ++count;
      }
      if (offset + count * 2 + 2 > encoded.size()) {
        return false;
      }
      const auto parsed = parseHex(
          std::string_view(encoded.data() + offset + count * 2, 2));
      if (!parsed) {
        return false;
      }
      const int end = angle ? static_cast<int>(
                                  std::lround(*parsed * 360.0 / 256.0))
                            : *parsed;
      for (std::size_t step = 0; step < count; ++step) {
        output[offset / 2 + step] =
            start + (end - start) * static_cast<int>(step + 1) /
                        static_cast<int>(count + 1);
      }
      offset += (count - 1) * 2;
      continue;
    }
    const auto parsed = parseHex(token);
    if (parsed) {
      start = angle
                  ? static_cast<int>(std::lround(*parsed * 360.0 / 256.0))
                  : *parsed;
      output[offset / 2] = start;
    }
  }
  return true;
}

std::optional<PreparedPomyuCharaAnimation> prepareAnimation(
    const ChpModel &model, const ChpDirective &directive,
    SkinResourceId source, int type, int side, FileCache &cache) {
  if (source == 0 || !validDestination(directive) || model.size[0] == 0 ||
      model.size[1] == 0) {
    return std::nullopt;
  }
  const int expectedMotion = motionForType(type);
  if (type != 0 && directive.motion != expectedMotion) {
    return std::nullopt;
  }
  const MotionBinding binding = bindingForMotion(directive.motion, side, type);
  if (type == 0 && !binding.timer) {
    return std::nullopt;
  }
  const std::size_t sourceFrameCount = directive.destination[0].size() / 2;
  int loop = model.loops[static_cast<std::size_t>(directive.motion)];
  if (loop >= static_cast<int>(sourceFrameCount) - 1) {
    loop = static_cast<int>(sourceFrameCount) - 2;
  } else if (loop < -1) {
    loop = -1;
  }
  const int sourceFrameMillis =
      model.frames[static_cast<std::size_t>(directive.motion)];
  const int increaseRate = interpolationIncreaseRate(directive,
                                                      sourceFrameMillis);
  const int frameMillis = sourceFrameMillis / increaseRate;
  const int cycleMillis = pomyu_chara_cycles_detail::multiplyAsJavaInt(
      sourceFrameMillis, sourceFrameCount);
  if (frameMillis < 1 || cycleMillis < 1) {
    return std::nullopt;
  }
  const auto destination = expandedDestinations(directive, increaseRate);
  const std::size_t expandedFrameCount = sourceFrameCount *
                                         static_cast<std::size_t>(increaseRate);
  std::vector<SkinSourceRect> geometry;
  if (destination[1].empty()) {
    geometry.assign(expandedFrameCount,
                    {.x = 0,
                     .y = 0,
                     .w = model.size[0],
                     .h = model.size[1]});
  } else if (!decodeGeometry(model, destination[1], geometry)) {
    return std::nullopt;
  }
  std::vector<int> alpha(expandedFrameCount, 255);
  std::vector<int> angle(expandedFrameCount, 0);
  if (!decodeValues(destination[2], 255, false, alpha) ||
      !decodeValues(destination[3], 0, true, angle)) {
    return std::nullopt;
  }
  PreparedPomyuCharaAnimation result{
      .motion = directive.motion,
      .builtinTimerId = binding.timer,
      .options = binding.options,
      .frameMillis = frameMillis,
      .cycleMillis = cycleMillis,
      .loopStartFrame = static_cast<std::size_t>(loop + 1) *
                        static_cast<std::size_t>(increaseRate)};
  result.frames.reserve(expandedFrameCount);
  for (std::size_t frame = 0; frame < expandedFrameCount; ++frame) {
    const std::size_t sourceOffset =
        (frame / static_cast<std::size_t>(increaseRate)) * 2;
    const auto coordinate = parseBase36(std::string_view(
        directive.destination[0].data() + sourceOffset, 2));
    SkinSourceRect region;
    SkinResourceId frameResource = 0;
    if (coordinate) {
      region = model.coordinates[static_cast<std::size_t>(*coordinate)];
      if (region.w > 0 && region.h > 0 && addRegion(cache, source, region)) {
        frameResource = source;
      }
    }
    const auto &placement = geometry[frame];
    result.frames.push_back(
        {.resource = frameResource,
         .region = region,
         .x = placement.x,
         .y = placement.y,
         .width = placement.w,
         .height = placement.h,
         .alpha = static_cast<std::uint8_t>(std::clamp(alpha[frame], 0, 255)),
         .angleDegrees = angle[frame]});
  }
  return result;
}

void addStaticFrame(PreparedPomyuCharaResource &output, FileCache &cache,
                    SkinResourceId resource, SkinSourceRect region) {
  if (resource == 0 || region.w <= 0 || region.h <= 0) {
    return;
  }
  if (!addRegion(cache, resource, region)) {
    return;
  }
  output.animations.push_back(
      {.motion = kUnspecified,
       .frameMillis = 0,
       .cycleMillis = 0,
       .frames = {{.resource = resource, .region = region}}});
}

} // namespace

PomyuCharaPreparationResult preparePomyuCharaResources(
    const LuaSkinFileSystem &files, const ValidatedBeatorajaSkinModel &model,
    const BeatorajaSkinConfiguration &configuration,
    SkinSafetyPolicy safetyPolicy, std::stop_token stop) {
  PomyuCharaPreparationResult result;
  FileCache cache;
  for (const auto &definition : model.model.resources) {
    std::visit(
        [&](const auto &resource) {
          if (resource.id >= cache.nextImageId) {
            cache.nextImageId = resource.id ==
                                        std::numeric_limits<SkinResourceId>::max()
                                    ? 0
                                    : resource.id + 1;
          }
        },
        definition);
  }
  for (const auto &definition : model.model.objects) {
    if (stop.stop_requested()) {
      cache.cancelled = true;
      break;
    }
    const auto *object = std::get_if<SkinPmCharaObject>(&definition.payload);
    if (object == nullptr || object->sourcePath.empty()) {
      continue;
    }
    const auto selected =
        configuredPath(object->sourcePath, configuration, safetyPolicy);
    if (!selected) {
      continue;
    }
    std::string chpPath;
    const auto *bytes = readChp(files, *selected, safetyPolicy, stop, cache,
                                chpPath);
    if (bytes == nullptr) {
      if (cache.cancelled || cache.budgetExceeded) {
        break;
      }
      continue;
    }
    auto parsed = cache.modelsByPath.find(chpPath);
    if (parsed == cache.modelsByPath.end()) {
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
      pomyuRequirementParsesForTestingValue.fetch_add(
          1, std::memory_order_relaxed);
#endif
      const std::string_view contents(
          reinterpret_cast<const char *>(bytes->data()), bytes->size());
      parsed = cache.modelsByPath
                   .emplace(chpPath,
                            parseChp(contents, safetyPolicy, stop))
                   .first;
    }
    if (!parsed->second) {
      if (stop.stop_requested()) {
        cache.cancelled = true;
        break;
      }
      continue;
    }
    const ChpModel &chp = *parsed->second;
    std::array<std::optional<SkinResourceId>, kImageCount> images;
    for (std::size_t index = 0; index < images.size(); ++index) {
      images[index] = decodeImage(files, chpPath, chp.imagePaths[index],
                                  index < 6, safetyPolicy, stop, cache);
      if (cache.cancelled || cache.budgetExceeded) {
        break;
      }
    }
    if (cache.cancelled || cache.budgetExceeded) {
      break;
    }
    if (!images[0]) {
      continue;
    }
    const bool useSecondColor =
        object->color == 2 && images[1] &&
        (!chp.hasTextureDefinitions || images[3].has_value());
    const std::size_t colorOffset = useSecondColor ? 1U : 0U;
    if (!useSecondColor && chp.hasTextureDefinitions && !images[2]) {
      continue;
    }
    PreparedPomyuCharaResource prepared{
        .object = definition.id,
        .relativePlacement = object->type == 0 || object->type >= 6,
        .coordinateWidth = object->type == 0 || object->type >= 6
                               ? chp.size[0]
                               : 0,
        .coordinateHeight = object->type == 0 || object->type >= 6
                                ? chp.size[1]
                                : 0};
    if (object->type == 1) {
      addStaticFrame(prepared, cache, *images[colorOffset], chp.coordinates[1]);
    } else if (object->type == 2) {
      addStaticFrame(prepared, cache, *images[colorOffset], chp.coordinates[0]);
    } else if (object->type == 3 || object->type == 4) {
      const auto face = useSecondColor && images[5] ? images[5] : images[4];
      if (face) {
        addStaticFrame(prepared, cache, *face,
                       object->type == 3 ? chp.faceUpper : chp.faceAll);
      }
    } else if (object->type == 5) {
      const auto select = useSecondColor && images[7] ? images[7] : images[6];
      if (select) {
        const auto *decoded = findImage(cache, *select);
        if (decoded != nullptr) {
          addStaticFrame(prepared, cache, *select,
                         {.x = 0,
                          .y = 0,
                          .w = decoded->pixels.width,
                          .h = decoded->pixels.height});
        }
      }
    } else {
      const bool firstCycleParse = cache.parsedCycleKeys
                                       .emplace(chpPath, object->type,
                                                object->side)
                                       .second;
#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
      if (object->type == 0 && firstCycleParse) {
        pomyuCycleParsesForTestingValue.fetch_add(1,
                                                   std::memory_order_relaxed);
      }
#else
      (void)firstCycleParse;
#endif
      for (const ChpDirective &directive : chp.directives) {
        const std::size_t sourceIndex =
            directive.kind == DirectiveKind::Texture ? 2U + colorOffset
                                                     : colorOffset;
        if (!images[sourceIndex]) {
          continue;
        }
        auto animation = prepareAnimation(chp, directive, *images[sourceIndex],
                                          object->type, object->side, cache);
        if (!animation) {
          continue;
        }
        if (object->type == 0 && animation->builtinTimerId &&
            *animation->builtinTimerId >= 900 &&
            *animation->builtinTimerId <= 907) {
          const std::size_t timer = static_cast<std::size_t>(
              *animation->builtinTimerId - 900);
          result.motionCyclesMillis[timer] = animation->cycleMillis;
        }
        prepared.animations.push_back(std::move(*animation));
      }
    }
    if (!prepared.animations.empty()) {
      result.resources.push_back(std::move(prepared));
    }
  }
  result.images = std::move(cache.images);
  result.encodedBytes = cache.encodedBytes;
  result.decodedBytes = cache.decodedBytes;
  result.budgetExceeded = cache.budgetExceeded;
  result.cancelled = cache.cancelled || stop.stop_requested();
  return result;
}

#if defined(ASOBMASHOW_PLAY_SKIN_SESSION_TESTING)
void resetPomyuCyclePreparationCountersForTesting() noexcept {
  pomyuFileReadsForTestingValue.store(0, std::memory_order_relaxed);
  pomyuRequirementParsesForTestingValue.store(0, std::memory_order_relaxed);
  pomyuCycleParsesForTestingValue.store(0, std::memory_order_relaxed);
}

std::size_t pomyuCycleFileReadsForTesting() noexcept {
  return pomyuFileReadsForTestingValue.load(std::memory_order_relaxed);
}

std::size_t pomyuRequirementParsesForTesting() noexcept {
  return pomyuRequirementParsesForTestingValue.load(std::memory_order_relaxed);
}

std::size_t pomyuCycleParsesForTesting() noexcept {
  return pomyuCycleParsesForTestingValue.load(std::memory_order_relaxed);
}
#endif

} // namespace skin
