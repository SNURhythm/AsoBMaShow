#include "decoder.h"
#include "../ArchiveFile.h"
#include "../RAII.h"
#include "SoundFileIO.h"
#include <SDL2/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
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
                     std::atomic<bool> &isCancelled) {
  UniqueResource<SNDFILE, sf_close> fileHandle(file);
  if (!file) {
    SDL_Log("Failed to open audio file %s, error: %s",
            path_t_to_utf8(displayPath).c_str(), sf_strerror(file));
    return false;
  }

  if (isCancelled) {
    return false;
  }
  // Prepare a buffer to hold the PCM data
  buffer.resize(fileInfo.frames * fileInfo.channels, 0);
  // Read through libsndfile's floating-point path, then convert explicitly.
  // Direct int16 reads can produce audible differences for some keysounds.
  std::vector<double> tempBuffer(fileInfo.frames * fileInfo.channels);
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
    SDL_Log("Failed to read all audio data from file %s, read %lld frames",
            path_t_to_utf8(displayPath).c_str(), numFrames);
    // Zero out the remaining buffer
    std::fill(buffer.begin() + numFrames * fileInfo.channels, buffer.end(), 0);
  }

  return true;
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
  return decodeAudioFile(file, displayPath, buffer, fileInfo, isCancelled);
}

// Function to decode audio file to PCM
bool decodeAudioToPCM(const path_t &filePath, std::vector<short> &buffer,
                      SF_INFO &fileInfo, std::atomic<bool> &isCancelled) {
  fileInfo = {};
  const std::filesystem::path fsPath(filePath);
  if (archive_file::isVirtualPath(fsPath)) {
    std::vector<unsigned char> bytes;
    std::string errorMessage;
    if (!archive_file::readFile(fsPath, bytes, &errorMessage)) {
      SDL_Log("Failed to read archived audio file %s: %s",
              path_t_to_utf8(filePath).c_str(), errorMessage.c_str());
      return false;
    }
    return decodeAudioBytesToPCM(filePath, bytes, buffer, fileInfo,
                                 isCancelled);
  }

  SNDFILE *file = asobmashow::audio::openSoundFile(fsPath, SFM_READ, fileInfo);
  return decodeAudioFile(file, filePath, buffer, fileInfo, isCancelled);
}
