#include "ChartMusicCache.h"

#include "../BmsMetadataText.h"
#include "../PlayOptionUtils.h"
#include "../Utils.h"
#include "../path.h"
#include "../targets.h"
#include "SoundFileIO.h"
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

#include <SDL2/SDL.h>

#include <cstdint>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace chart_music_cache {
namespace {

std::uint64_t fnv1a64Append(std::uint64_t hash, std::string_view value) {
  constexpr std::uint64_t kPrime = 1099511628211ull;
  for (unsigned char c : value) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= kPrime;
  }
  hash ^= 0xffu;
  hash *= kPrime;
  return hash;
}

std::string hex64(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i) {
    text[static_cast<std::size_t>(i)] = kHex[value & 0xfu];
    value >>= 4u;
  }
  return text;
}

std::string lowerTrimmed(std::string value) {
  return asobmshow::bms_metadata::lowerCopy(
      asobmshow::bms_metadata::trimCopy(value));
}

std::string sanitizeFileNamePart(const std::string &value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char ch : value) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_') {
      result.push_back(static_cast<char>(ch));
    } else if (ch == ' ' || ch == '.' || ch == '[' || ch == ']') {
      result.push_back('_');
    }
  }
  while (!result.empty() && result.back() == '_') {
    result.pop_back();
  }
  if (result.empty()) {
    return "music";
  }
  return result.substr(0, 64);
}

std::string stableChartAudioKey(const bms_parser::ChartMeta &meta) {
  const std::string sha256 =
      asobmshow::bms_metadata::normalizedHash(meta.SHA256);
  const std::string md5 = asobmshow::bms_metadata::normalizedHash(meta.MD5);
  const std::string identity =
      !sha256.empty() ? "sha256:" + sha256
                      : (!md5.empty() ? "md5:" + md5
                                      : "path:" + fspath_to_utf8(meta.BmsPath));
  const std::string folder = fspath_to_utf8(meta.Folder.lexically_normal());
  std::uint64_t hash = 14695981039346656037ull;
  hash = fnv1a64Append(hash, folder);
  hash = fnv1a64Append(hash, identity);
  return hex64(hash);
}

void excludeCacheFromBackup(const std::filesystem::path &path) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  std::string errorMessage;
  if (!SetIOSFileExcludedFromBackup(path.string(), true, errorMessage) &&
      !errorMessage.empty()) {
    SDL_Log("Failed to exclude music cache path from backup: %s",
            errorMessage.c_str());
  }
#else
  (void)path;
#endif
}

bool ensureCacheDirectory(std::string &errorMessage) {
  const std::filesystem::path directory = CacheDirectory();
  std::error_code error;
  if (!Utils::EnsureDirectoryExists(directory, error)) {
    errorMessage = "Could not create music cache directory: " + error.message();
    return false;
  }
  excludeCacheFromBackup(directory);
  return true;
}

CacheResult resultForExistingFile(const std::filesystem::path &path) {
  return {.success = true,
          .rendered = false,
          .audioPath = path,
          .message = "Cached audio exists",
          .durationMicros = ReadAudioFileDurationMicros(path).value_or(0)};
}

std::string normalizedPathKey(const std::filesystem::path &path) {
  return path.lexically_normal().string();
}

} // namespace

std::filesystem::path CacheDirectory() {
  return Utils::GetDocumentsPath("music_cache");
}

std::filesystem::path CachedAudioPathForChart(
    const bms_parser::ChartMeta &meta, bool clubMode) {
  const std::string title =
      !meta.Title.empty() ? meta.Title : meta.BmsPath.stem().string();
  return CacheDirectory() /
         (sanitizeFileNamePart(title) + "_" + stableChartAudioKey(meta) +
          (clubMode ? "_club" : "") +
          ".wav");
}

bool CachedAudioExists(const bms_parser::ChartMeta &meta, bool clubMode) {
  std::error_code error;
  const std::filesystem::path path = CachedAudioPathForChart(meta, clubMode);
  const bool cached = std::filesystem::is_regular_file(path, error);
  if (error) {
    SDL_Log("Could not check cached music file %s: %s",
            fspath_to_utf8(path).c_str(), error.message().c_str());
    return false;
  }
  return cached;
}

std::optional<long long>
ReadAudioFileDurationMicros(const std::filesystem::path &path) {
  if (path.empty()) {
    return std::nullopt;
  }

  SF_INFO info{};
  auto file = asobmashow::audio::openSoundFileHandle(path, SFM_READ, info);
  if (file == nullptr) {
    return std::nullopt;
  }
  if (info.frames <= 0 || info.samplerate <= 0) {
    return std::nullopt;
  }
  return static_cast<long long>(
      static_cast<long double>(info.frames) * 1000000.0L /
      static_cast<long double>(info.samplerate));
}

void PruneCacheExcept(const std::vector<std::filesystem::path> &keepPaths) {
  std::error_code error;
  const std::filesystem::path directory = CacheDirectory();
  const bool cacheDirectoryExists =
      std::filesystem::is_directory(directory, error);
  if (error) {
    SDL_Log("Could not check music cache directory %s: %s",
            fspath_to_utf8(directory).c_str(), error.message().c_str());
    return;
  }
  if (!cacheDirectoryExists) {
    return;
  }

  std::unordered_set<std::string> keep;
  keep.reserve(keepPaths.size());
  for (const auto &path : keepPaths) {
    if (!path.empty()) {
      keep.insert(normalizedPathKey(path));
    }
  }

  for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
    if (error) {
      return;
    }
    if (!entry.is_regular_file(error) || error) {
      error.clear();
      continue;
    }
    const std::filesystem::path path = entry.path();
    const std::string extension = lowerTrimmed(path.extension().string());
    if (extension != ".wav" && extension != ".tmp") {
      continue;
    }
    if (keep.contains(normalizedPathKey(path))) {
      continue;
    }
    std::filesystem::remove(path, error);
    error.clear();
  }
}

CacheResult EnsureRenderedMusicFile(const bms_parser::ChartMeta &meta,
                                    std::atomic_bool &cancelled,
                                    chart_audio::LogCallback log) {
  return EnsureRenderedMusicFile(meta, cancelled, false, std::move(log));
}

CacheResult EnsureRenderedMusicFile(const bms_parser::ChartMeta &meta,
                                    std::atomic_bool &cancelled, bool clubMode,
                                    chart_audio::LogCallback log) {
  if (meta.BmsPath.empty()) {
    return {.success = false, .message = "Chart path is empty"};
  }

  const std::filesystem::path outputPath =
      CachedAudioPathForChart(meta, clubMode);
  if (CachedAudioExists(meta, clubMode)) {
    return resultForExistingFile(outputPath);
  }

  std::string errorMessage;
  if (!ensureCacheDirectory(errorMessage)) {
    return {.success = false, .audioPath = outputPath, .message = errorMessage};
  }

  auto chart = play_options::parseChart(meta.BmsPath, cancelled, "music cache");
  if (cancelled) {
    return {.success = false,
            .audioPath = outputPath,
            .message = "Music render cancelled"};
  }
  if (chart == nullptr) {
    return {.success = false,
            .audioPath = outputPath,
            .message = "Could not parse chart for music render"};
  }
  return EnsureRenderedMusicFile(*chart, cancelled, clubMode, std::move(log));
}

CacheResult EnsureRenderedMusicFile(bms_parser::Chart &chart,
                                    std::atomic_bool &cancelled,
                                    chart_audio::LogCallback log) {
  return EnsureRenderedMusicFile(chart, cancelled, false, std::move(log));
}

CacheResult EnsureRenderedMusicFile(bms_parser::Chart &chart,
                                    std::atomic_bool &cancelled, bool clubMode,
                                    chart_audio::LogCallback log) {
  const std::filesystem::path outputPath =
      CachedAudioPathForChart(chart.Meta, clubMode);
  if (CachedAudioExists(chart.Meta, clubMode)) {
    return resultForExistingFile(outputPath);
  }

  std::string errorMessage;
  if (!ensureCacheDirectory(errorMessage)) {
    return {.success = false, .audioPath = outputPath, .message = errorMessage};
  }

  std::filesystem::path tempPath = outputPath;
  tempPath += PATH(".tmp");
  std::error_code error;
  std::filesystem::remove(tempPath, error);
  if (error) {
    return {.success = false,
            .audioPath = outputPath,
            .message = "Could not remove stale music cache temp file: " +
                       error.message()};
  }

  const chart_audio::RenderOptions options{
      .keySoundMode = chart_audio::KeySoundMode::ChartTiming,
      .clubMode = clubMode,
      .isCancelled = &cancelled,
      .log = std::move(log),
  };
  const auto renderResult =
      chart_audio::RenderChartAudioToWav(chart, tempPath, options);
  if (!renderResult.success) {
    std::filesystem::remove(tempPath, error);
    return {.success = false,
            .audioPath = outputPath,
            .message = renderResult.message,
            .durationMicros = renderResult.durationMicros};
  }
  if (cancelled) {
    std::filesystem::remove(tempPath, error);
    return {.success = false,
            .audioPath = outputPath,
            .message = "Music render cancelled",
            .durationMicros = renderResult.durationMicros};
  }

  error.clear();
  std::filesystem::remove(outputPath, error);
  error.clear();
  std::filesystem::rename(tempPath, outputPath, error);
  if (error) {
    const std::string renameMessage = error.message();
    std::filesystem::remove(tempPath, error);
    return {.success = false,
            .audioPath = outputPath,
            .message = "Could not store rendered music file: " +
                       renameMessage,
            .durationMicros = renderResult.durationMicros};
  }

  excludeCacheFromBackup(outputPath);
  return {.success = true,
          .rendered = true,
          .audioPath = outputPath,
          .message = "Audio rendered",
          .durationMicros = renderResult.durationMicros};
}

} // namespace chart_music_cache
