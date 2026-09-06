#include "decoder.h"
#include "../ArchiveFile.h"
#include "../RAII.h"
#include "SelectAudioDiagnostics.h"
#include "SoundFileIO.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
namespace {
struct MemoryAudioFile {
  const unsigned char *data = nullptr;
  sf_count_t size = 0;
  sf_count_t offset = 0;
};

sf_count_t memoryFileLength(void *userData) {
  auto *file = static_cast<MemoryAudioFile *>(userData);
  return file == nullptr ? 0 : file->size;
}

sf_count_t memoryFileSeek(sf_count_t offset, int whence, void *userData) {
  auto *file = static_cast<MemoryAudioFile *>(userData);
  if (file == nullptr) {
    return -1;
  }

  sf_count_t target = 0;
  switch (whence) {
  case SEEK_SET:
    target = offset;
    break;
  case SEEK_CUR:
    target = file->offset + offset;
    break;
  case SEEK_END:
    target = file->size + offset;
    break;
  default:
    return -1;
  }
  if (target < 0 || target > file->size) {
    return -1;
  }
  file->offset = target;
  return file->offset;
}

sf_count_t memoryFileRead(void *ptr, sf_count_t count, void *userData) {
  auto *file = static_cast<MemoryAudioFile *>(userData);
  if (file == nullptr || ptr == nullptr || count <= 0) {
    return 0;
  }
  const sf_count_t remaining = file->size - file->offset;
  const sf_count_t toRead = std::min(count, remaining);
  if (toRead <= 0) {
    return 0;
  }
  std::memcpy(ptr, file->data + file->offset, static_cast<size_t>(toRead));
  file->offset += toRead;
  return toRead;
}

sf_count_t memoryFileWrite(const void *, sf_count_t, void *) { return 0; }

sf_count_t memoryFileTell(void *userData) {
  auto *file = static_cast<MemoryAudioFile *>(userData);
  return file == nullptr ? 0 : file->offset;
}

bool decodeAudioFile(SNDFILE *file, const path_t &displayPath,
                     std::vector<short> &buffer, SF_INFO &fileInfo,
                     std::atomic<bool> &isCancelled,
                     std::size_t maximumPcmSamples) {
  UniqueResource<SNDFILE, sf_close> fileHandle(file);
  if (!file) {
    SDL_Log("Failed to open audio file %s, error: %s",
            path_t_to_utf8(displayPath).c_str(), sf_strerror(file));
    audio::diag::SelectAudioLog(std::string("[dec] sf_open FAILED: ") +
                                path_t_to_utf8(displayPath) + ": " +
                                sf_strerror(file));
    return false;
  }

  if (isCancelled) {
    return false;
  }
  if (fileInfo.frames < 0 || fileInfo.channels <= 0 ||
      static_cast<std::uintmax_t>(fileInfo.frames) >
          static_cast<std::uintmax_t>(maximumPcmSamples) /
              static_cast<std::uintmax_t>(fileInfo.channels)) {
    SDL_Log("Decoded audio exceeds the PCM sample limit for %s",
            path_t_to_utf8(displayPath).c_str());
    audio::diag::SelectAudioLog(
        std::string("[dec] PCM sample limit exceeded: ") +
        path_t_to_utf8(displayPath));
    return false;
  }
  const std::size_t sampleCount =
      static_cast<std::size_t>(fileInfo.frames) *
      static_cast<std::size_t>(fileInfo.channels);
  // Prepare a buffer to hold the PCM data
  buffer.resize(sampleCount, 0);
  // Read through libsndfile's floating-point path, then convert explicitly.
  // Direct int16 reads can produce audible differences for some keysounds.
  std::vector<double> tempBuffer(sampleCount);
  if (isCancelled) {
    return false;
  }
  sf_count_t numFrames =
      sf_readf_double(fileHandle.get(), tempBuffer.data(), fileInfo.frames);
  if (isCancelled) {
    return false;
  }
  if (numFrames < 0) {
    SDL_Log("Failed to read audio data from file %s, error: %s",
            path_t_to_utf8(displayPath).c_str(),
            sf_strerror(fileHandle.get()));
    audio::diag::SelectAudioLog(std::string("[dec] sf_read FAILED: ") +
                                path_t_to_utf8(displayPath) + ": " +
                                sf_strerror(fileHandle.get()));
    return false;
  }
  // Convert the double buffer to short
  std::transform(
      tempBuffer.begin(), tempBuffer.end(), buffer.begin(), [](double val) {
        return static_cast<short>(std::clamp(val, -1.0, 1.0) * 32767);
      });
  if (isCancelled) {
    return false;
  }

  if (numFrames < fileInfo.frames) {
    SDL_Log("Failed to read all audio data from file %s, read %" PRId64
            " frames",
            path_t_to_utf8(displayPath).c_str(),
            static_cast<std::int64_t>(numFrames));
    // Zero out the remaining buffer
    std::fill(buffer.begin() + numFrames * fileInfo.channels, buffer.end(), 0);
  }

  return true;
}

// Reads an audio asset through SDL_RWFromFile, which resolves relative asset
// paths against the app bundle on iOS/macOS (the same bundle-aware lookup the
// skin image/font reads use). Returns std::nullopt when the lookup misses or
// the encoded file exceeds maximumEncodedBytes.
std::optional<std::vector<unsigned char>>
readBundleAwareAudioBytes(const path_t &path, std::size_t maximumEncodedBytes) {
  const std::string utf8Path = path_t_to_utf8(path);
  SDL_RWops *input = SDL_RWFromFile(utf8Path.c_str(), "rb");
  if (input == nullptr) {
    return std::nullopt;
  }
  struct RwCloser {
    void operator()(SDL_RWops *ops) const { SDL_RWclose(ops); }
  };
  std::unique_ptr<SDL_RWops, RwCloser> owned(input);
  const Sint64 reportedSize = SDL_RWsize(owned.get());
  if (reportedSize >= 0) {
    const auto size = static_cast<std::uint64_t>(reportedSize);
    if (size > maximumEncodedBytes) {
      return std::nullopt;
    }
    std::vector<unsigned char> result(static_cast<std::size_t>(size));
    if (!result.empty() &&
        SDL_RWread(owned.get(), result.data(), 1, result.size()) !=
            result.size()) {
      return std::nullopt;
    }
    return result;
  }

  std::vector<unsigned char> result;
  std::array<unsigned char, 64U * 1024U> buffer{};
  for (;;) {
    const std::size_t read =
        SDL_RWread(owned.get(), buffer.data(), 1, buffer.size());
    if (read >
        maximumEncodedBytes - std::min(result.size(), maximumEncodedBytes)) {
      return std::nullopt;
    }
    result.insert(result.end(), buffer.begin(), buffer.begin() + read);
    if (read < buffer.size()) {
      return result;
    }
  }
}
} // namespace

bool decodeAudioBytesToPCM(const path_t &displayPath,
                           const std::vector<unsigned char> &bytes,
                           std::vector<short> &buffer, SF_INFO &fileInfo,
                           std::atomic<bool> &isCancelled) {
  fileInfo = {};
  MemoryAudioFile memoryFile{
      .data = bytes.data(),
      .size = static_cast<sf_count_t>(bytes.size()),
      .offset = 0,
  };
  SF_VIRTUAL_IO io{
      .get_filelen = memoryFileLength,
      .seek = memoryFileSeek,
      .read = memoryFileRead,
      .write = memoryFileWrite,
      .tell = memoryFileTell,
  };
  SNDFILE *file = sf_open_virtual(&io, SFM_READ, &fileInfo, &memoryFile);
  return decodeAudioFile(file, displayPath, buffer, fileInfo, isCancelled,
                         std::numeric_limits<std::size_t>::max());
}

bool decodeAudioBytesToPCMBounded(const path_t &displayPath,
                                  const std::vector<unsigned char> &bytes,
                                  std::vector<short> &buffer, SF_INFO &fileInfo,
                                  std::atomic<bool> &isCancelled,
                                  std::size_t maximumPcmSamples) {
  fileInfo = {};
  MemoryAudioFile memoryFile{
      .data = bytes.data(),
      .size = static_cast<sf_count_t>(bytes.size()),
      .offset = 0,
  };
  SF_VIRTUAL_IO io{
      .get_filelen = memoryFileLength,
      .seek = memoryFileSeek,
      .read = memoryFileRead,
      .write = memoryFileWrite,
      .tell = memoryFileTell,
  };
  SNDFILE *file = sf_open_virtual(&io, SFM_READ, &fileInfo, &memoryFile);
  return decodeAudioFile(file, displayPath, buffer, fileInfo, isCancelled,
                         maximumPcmSamples);
}

bool decodeSkinSoundBundleAware(const path_t &displayPath,
                                std::vector<short> &buffer, SF_INFO &fileInfo,
                                std::atomic<bool> &isCancelled,
                                AudioDecodeLimits limits, std::stop_token stop) {
  if (stop.stop_requested()) {
    return false;
  }
  const std::filesystem::path fsPath(displayPath);
  // Bundle-aware read first: SDL_RWFromFile resolves relative asset paths
  // against the app bundle on iOS/macOS, and reads Files-app storage files that
  // plain fopen cannot (the same SDL read images use). Archive (virtual) paths
  // are excluded because their synthetic form must stay with the archive reader.
  if (!archive_file::isVirtualPath(fsPath)) {
    audio::diag::SelectAudioLog(
        std::string("[dec] plain-file read stage for ") +
        fsPath.generic_string());
    if (auto bytes =
            readBundleAwareAudioBytes(displayPath, limits.maximumEncodedBytes)) {
      if (decodeAudioBytesToPCMBounded(displayPath, *bytes, buffer, fileInfo,
                                       isCancelled,
                                       limits.maximumPcmSamples) &&
          !stop.stop_requested() && !isCancelled) {
        return true;
      }
      buffer.clear();
      fileInfo = {};
      audio::diag::SelectAudioLog(
          "[dec] SDL byte read ok but in-memory decode FAILED");
    } else {
      audio::diag::SelectAudioLog(
          "[dec] SDL_RWFromFile could not open the audio file");
    }
    // The SDL read missed (e.g. a path the bundle/filesystem layer cannot open
    // with SDL_RWFromFile). Fall back to readFileBounded, which on iOS reads
    // through the same SDL-backed path and on other platforms through ifstream;
    // decode the bytes from memory so we never rely on sf_open's plain fopen
    // (which cannot open iOS Files-app storage).
    std::string readError;
    std::vector<unsigned char> bytes;
    if (archive_file::readFileBounded(fsPath, bytes,
                                      limits.maximumEncodedBytes,
                                      &readError, stop) &&
        !bytes.empty() &&
        decodeAudioBytesToPCMBounded(displayPath, bytes, buffer, fileInfo,
                                     isCancelled,
                                     limits.maximumPcmSamples) &&
        !stop.stop_requested() && !isCancelled) {
      return true;
    }
    buffer.clear();
    fileInfo = {};
    audio::diag::SelectAudioLog(
        std::string("[dec] readFileBounded fallback FAILED") +
        (readError.empty() ? "" : (": " + readError)));
  }
  // Fallback: archives, user files with absolute paths, and relative paths the
  // bundle lookup genuinely cannot see keep using the existing bounded decode.
  return decodeAudioToPCMBounded(displayPath, buffer, fileInfo, isCancelled,
                                 limits, std::move(stop));
}

// Function to decode audio file to PCM
bool decodeAudioToPCM(const path_t &filePath, std::vector<short> &buffer,
                      SF_INFO &fileInfo, std::atomic<bool> &isCancelled) {
  return decodeAudioToPCMBounded(filePath, buffer, fileInfo, isCancelled, {});
}

bool decodeAudioToPCMBounded(const path_t &filePath,
                             std::vector<short> &buffer, SF_INFO &fileInfo,
                             std::atomic<bool> &isCancelled,
                             AudioDecodeLimits limits, std::stop_token stop) {
  fileInfo = {};
  const std::filesystem::path fsPath(filePath);
  if (archive_file::isVirtualPath(fsPath)) {
    std::vector<unsigned char> bytes;
    std::string errorMessage;
    if (!archive_file::readFileBounded(fsPath, bytes,
                                       limits.maximumEncodedBytes,
                                       &errorMessage, stop)) {
      SDL_Log("Failed to read archived audio file %s: %s",
              path_t_to_utf8(filePath).c_str(), errorMessage.c_str());
      return false;
    }
    MemoryAudioFile memoryFile{
        .data = bytes.data(),
        .size = static_cast<sf_count_t>(bytes.size()),
        .offset = 0,
    };
    SF_VIRTUAL_IO io{
        .get_filelen = memoryFileLength,
        .seek = memoryFileSeek,
        .read = memoryFileRead,
        .write = memoryFileWrite,
        .tell = memoryFileTell,
    };
    SNDFILE *file = sf_open_virtual(&io, SFM_READ, &fileInfo, &memoryFile);
    return decodeAudioFile(file, filePath, buffer, fileInfo, isCancelled,
                           limits.maximumPcmSamples);
  }

  std::error_code sizeError;
  const std::uintmax_t encodedBytes = std::filesystem::file_size(fsPath,
                                                                 sizeError);
  if (!sizeError && encodedBytes > limits.maximumEncodedBytes) {
    SDL_Log("Encoded audio exceeds the byte limit for %s",
            path_t_to_utf8(filePath).c_str());
    return false;
  }

  SNDFILE *file = asobmashow::audio::openSoundFile(fsPath, SFM_READ, fileInfo);
  return decodeAudioFile(file, filePath, buffer, fileInfo, isCancelled,
                         limits.maximumPcmSamples);
}
