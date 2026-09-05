#pragma once
#include <sndfile.h> // Include the sndfile library for handling audio files
#include <vector>
#include <atomic>
#include <cstddef>
#include <limits>
#include <stop_token>
#include "../path.h"

struct AudioDecodeLimits {
  std::size_t maximumEncodedBytes = std::numeric_limits<std::size_t>::max();
  std::size_t maximumPcmSamples = std::numeric_limits<std::size_t>::max();
};

bool decodeAudioToPCM(const path_t &filePath, std::vector<short> &buffer,
                      SF_INFO &fileInfo, std::atomic<bool> &isCancelled);
bool decodeAudioToPCMBounded(const path_t &filePath,
                             std::vector<short> &buffer, SF_INFO &fileInfo,
                             std::atomic<bool> &isCancelled,
                             AudioDecodeLimits limits,
                             std::stop_token stop = {});
bool decodeAudioBytesToPCM(const path_t &displayPath,
                           const std::vector<unsigned char> &bytes,
                           std::vector<short> &buffer, SF_INFO &fileInfo,
                           std::atomic<bool> &isCancelled);
// Like decodeAudioBytesToPCM but pre-checks the header's PCM frame count
// against maximumPcmSamples before allocating the output buffer (the same
// guard decodeAudioToPCMBounded applies to files and archives).
bool decodeAudioBytesToPCMBounded(const path_t &displayPath,
                                  const std::vector<unsigned char> &bytes,
                                  std::vector<short> &buffer,
                                  SF_INFO &fileInfo,
                                  std::atomic<bool> &isCancelled,
                                  std::size_t maximumPcmSamples);
// Bundle-aware skin-sound decode for the music-select sound set. Reads the
// encoded bytes through SDL_RWFromFile (which resolves relative asset paths
// against the app bundle on iOS/macOS, so the bundled `assets/*.wav` default
// sounds load there), decodes from memory with limits.maximumPcmSamples, and
// falls back to decodeAudioToPCMBounded for archive (virtual) paths and user
// files with absolute paths that the bundle lookup cannot see. Returns false
// when neither path yields decodable PCM within the limits.
bool decodeSkinSoundBundleAware(const path_t &displayPath,
                                std::vector<short> &buffer, SF_INFO &fileInfo,
                                std::atomic<bool> &isCancelled,
                                AudioDecodeLimits limits,
                                std::stop_token stop = {});
