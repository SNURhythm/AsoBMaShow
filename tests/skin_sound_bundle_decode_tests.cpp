#include "audio/decoder.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class SoundSandbox {
public:
  SoundSandbox() {
    root_ = std::filesystem::temp_directory_path() /
            ("skin_sound_bundle_decode_tests_" +
             std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::create_directories(root_);
  }
  ~SoundSandbox() { std::filesystem::remove_all(root_); }
  SoundSandbox(const SoundSandbox &) = delete;
  SoundSandbox &operator=(const SoundSandbox &) = delete;

  // Copies a repo asset ("assets/select.wav") into the sandbox so decoding
  // exercises a real bundled WAV rather than a hand-written fixture.
  std::filesystem::path copyBundledWav() {
    const std::filesystem::path source =
        std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "assets" / "select.wav";
    const auto destination = root_ / "select.wav";
    std::ifstream input(source, std::ios::binary);
    std::ofstream output(destination, std::ios::binary);
    output << input.rdbuf();
    if (!output) {
      expect(false, "sandbox copies assets/select.wav into the temp dir");
    }
    return destination;
  }

  [[nodiscard]] const std::filesystem::path &root() const { return root_; }

private:
  std::filesystem::path root_;
};

void testBundleAwareDecodeProducesPcm() {
  SoundSandbox sandbox;
  const auto wavPath = sandbox.copyBundledWav();
  std::vector<short> pcm;
  SF_INFO info;
  std::atomic<bool> cancelled{false};
  const bool decoded = decodeSkinSoundBundleAware(
      fspath_to_path_t(wavPath), pcm, info, cancelled, {});
  expect(decoded,
         "the bundle-aware skin-sound decode reads a real bundled WAV");
  expect(!pcm.empty(),
         "the bundle-aware decode produces non-empty PCM");
  expect(info.channels > 0,
         "the bundle-aware decode reports a positive channel count");
  expect(info.samplerate > 0,
         "the bundle-aware decode reports a positive sample rate");
  expect(!cancelled.load(), "an uncancelled decode stays uncancelled");
}

void testBundleAwareDecodeHonorsPcmBudget() {
  SoundSandbox sandbox;
  const auto wavPath = sandbox.copyBundledWav();
  std::vector<short> pcm;
  SF_INFO info;
  std::atomic<bool> cancelled{false};
  const bool decoded = decodeSkinSoundBundleAware(
      fspath_to_path_t(wavPath), pcm, info, cancelled,
      {.maximumPcmSamples = 1});
  expect(!decoded,
         "a decode past the PCM sample budget is rejected before allocation");
  expect(pcm.empty(),
         "the rejected decode leaves no partial PCM behind");
}

void testBundleAwareDecodeRejectsOversizedEncodedFile() {
  SoundSandbox sandbox;
  const auto wavPath = sandbox.copyBundledWav();
  std::vector<short> pcm;
  SF_INFO info;
  std::atomic<bool> cancelled{false};
  const bool decoded = decodeSkinSoundBundleAware(
      fspath_to_path_t(wavPath), pcm, info, cancelled,
      {.maximumEncodedBytes = 1});
  expect(!decoded,
         "an encoded file past the byte cap is rejected without allocation");
}

void testBundleAwareDecodeMissingFileFallsBackToRecordedFailure() {
  std::vector<short> pcm;
  SF_INFO info;
  std::atomic<bool> cancelled{false};
  const bool decoded = decodeSkinSoundBundleAware(
      PATH("/does/not/exist/missing.wav"), pcm, info, cancelled, {});
  expect(!decoded,
         "a missing absolute asset returns false (bundle read and sf_open "
         "both miss) without crashing");
}

void testBundleAwareDecodeBoundedEncodedFallback() {
  // An absolute user file (a real WAV in the temp dir) that the bundle read
  // also resolves must decode through the same bounded budget path.
  SoundSandbox sandbox;
  const auto wavPath = sandbox.copyBundledWav();
  std::vector<short> pcm;
  SF_INFO info;
  std::atomic<bool> cancelled{false};
  const bool decoded = decodeSkinSoundBundleAware(
      fspath_to_path_t(wavPath), pcm, info, cancelled,
      {.maximumEncodedBytes = 4U * 1024U * 1024U,
       .maximumPcmSamples = 4U * 1024U * 1024U});
  expect(decoded && info.channels > 0 && info.samplerate > 0,
         "a bounded budget that fits the file still decodes through the "
         "bundle-aware path");
}

void testBundleAwareDecodeHonorsStopToken() {
  SoundSandbox sandbox;
  const auto wavPath = sandbox.copyBundledWav();
  std::vector<short> pcm;
  SF_INFO info;
  std::atomic<bool> cancelled{false};
  std::stop_source source;
  source.request_stop();
  const bool decoded = decodeSkinSoundBundleAware(
      fspath_to_path_t(wavPath), pcm, info, cancelled, {},
      source.get_token());
  expect(!decoded,
         "a stop-requested bundle-aware decode returns no audio");
  expect(pcm.empty(),
         "the stopped bundle-aware decode leaves no PCM behind");
}

} // namespace

int main() {
  testBundleAwareDecodeProducesPcm();
  testBundleAwareDecodeHonorsPcmBudget();
  testBundleAwareDecodeRejectsOversizedEncodedFile();
  testBundleAwareDecodeMissingFileFallsBackToRecordedFailure();
  testBundleAwareDecodeBoundedEncodedFallback();
  testBundleAwareDecodeHonorsStopToken();
  if (failures != 0) {
    std::cerr << "skin_sound_bundle_decode_tests: " << failures
              << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "skin_sound_bundle_decode_tests passed\n";
  return 0;
}