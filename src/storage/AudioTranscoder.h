#pragma once

#include "ChartResourceInventory.h"

namespace storage_management {
struct AudioEncodingDecision {
  int format = 0;
  StorageDiagnostic reason;
};
AudioEncodingDecision ChooseAudioEncoding(int sourceFormat, int sampleRate, int channels,
                                          AudioAction, const StorageCodecCapabilities &);
EncodeResult EncodeAudio(const AssetCandidate &, const StoragePolicy &,
                         const std::filesystem::path &temporaryOutput,
                         Checkpoint = {}, ProgressCallback = {});
} // namespace storage_management
