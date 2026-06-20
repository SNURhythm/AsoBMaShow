#pragma once
#include <sndfile.h> // Include the sndfile library for handling audio files
#include <vector>
#include <atomic>
#include "../path.h"
bool decodeAudioToPCM(const path_t &filePath, std::vector<short> &buffer,
                      SF_INFO &fileInfo, std::atomic<bool> &isCancelled);
bool decodeAudioBytesToPCM(const path_t &displayPath,
                           const std::vector<unsigned char> &bytes,
                           std::vector<short> &buffer, SF_INFO &fileInfo,
                           std::atomic<bool> &isCancelled);
