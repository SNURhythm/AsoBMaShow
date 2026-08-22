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
