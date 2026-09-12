#include "audio/decoder.h"
#include "ArchiveFile.h"
#include "ArchiveRAII.h"
#include "RAII.h"
#include "Utils.h"

#include <archive_entry.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#ifdef __APPLE__
#include <sys/wait.h>
#include <unistd.h>
#endif

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

void testPreviewExtensionFallback(bool archived, bool bundleAware) {
  SoundSandbox sandbox;
  const auto storedPath = sandbox.root() / "preview.ogg";
  SF_INFO writtenInfo{};
  writtenInfo.samplerate = 44100;
  writtenInfo.channels = 1;
  writtenInfo.format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
  auto *sound = sf_open(storedPath.string().c_str(), SFM_WRITE, &writtenInfo);
  expect(sound != nullptr, "create a real Ogg preview fixture");
  if (!sound) return;
  std::array<short, 1024> samples;
  samples.fill(4000);
  expect(sf_write_short(sound, samples.data(), samples.size()) == samples.size(),
         "write the complete Ogg preview fixture");
  expect(sf_close(sound) == 0, "close the Ogg preview fixture");
  auto requestedPath = sandbox.root() / "preview.wav";
  if (archived) {
    const auto archivePath = sandbox.root() / "chart.zip";
    std::ifstream input(storedPath, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    auto writer = makeArchiveWriteHandle();
    expect(archive_write_set_format_zip(writer.get()) == ARCHIVE_OK,
           "choose ZIP format for preview fixture");
    expect(archive_write_open_filename(writer.get(), archivePath.string().c_str()) == ARCHIVE_OK,
           "open preview ZIP fixture");
    auto entry = std::unique_ptr<archive_entry, decltype(&archive_entry_free)>(
        archive_entry_new(), archive_entry_free);
    archive_entry_set_pathname(entry.get(), "preview.ogg");
    archive_entry_set_size(entry.get(), bytes.size());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    expect(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK,
           "write preview ZIP entry header");
    expect(archive_write_data(writer.get(), bytes.data(), bytes.size()) == bytes.size(),
           "write preview ZIP entry data");
    expect(archive_write_close(writer.get()) == ARCHIVE_OK, "close preview ZIP fixture");
    requestedPath = archive_file::makeVirtualPath(archivePath, "preview.wav");
  }
  std::vector<short> pcm;
  SF_INFO info{};
  std::atomic<bool> cancelled{false};
  const auto decode = bundleAware ? decodeSkinSoundBundleAware : decodeAudioToPCMBounded;
  const bool decoded = decode(fspath_to_path_t(requestedPath), pcm, info,
                              cancelled, {}, {});
  expect(decoded, archived ? "archived preview.wav resolves to preview.ogg"
                           : "loose preview.wav resolves to preview.ogg");
  expect(pcm.size() == samples.size() && info.channels == 1 && info.samplerate == 44100,
         "preview extension fallback decodes the complete requested audio");
  for (const auto limits : {AudioDecodeLimits{.maximumEncodedBytes = 1},
                             AudioDecodeLimits{.maximumPcmSamples = 1}}) {
    pcm.clear();
    expect(!decode(fspath_to_path_t(requestedPath), pcm, info, cancelled, limits, {}),
           "preview extension fallback preserves encoded and decoded size limits");
    expect(pcm.empty(), "oversized fallback audio does not publish partial PCM");
  }
  std::stop_source stopped;
  stopped.request_stop();
  expect(!decode(fspath_to_path_t(requestedPath), pcm, info, cancelled, {}, stopped.get_token()),
         "a stopped preview does not decode an alternate extension");
  cancelled = true;
  expect(!decode(fspath_to_path_t(requestedPath), pcm, info, cancelled, {}, {}),
         "a cancelled preview does not decode an alternate extension");
  expect(pcm.empty(), "cancelled fallback audio does not publish PCM");
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

#ifdef __APPLE__
void testBundleOnlyPreviewExtensionFallback(const char *executable) {
  SoundSandbox sandbox;
  const auto bundle = sandbox.root() / "Preview.app" / "Contents";
  const auto binary = bundle / "MacOS" / "preview-test";
  const auto resources = bundle / "Resources" / "bundle-preview";
  std::filesystem::create_directories(binary.parent_path());
  std::filesystem::create_directories(resources);
  std::filesystem::copy_file(std::filesystem::absolute(executable), binary);
  std::filesystem::copy_file(sandbox.copyBundledWav(), resources / "select.wav");
  std::ofstream(bundle / "Info.plist") <<
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<plist version=\"1.0\"><dict>"
      "<key>CFBundleExecutable</key><string>preview-test</string>"
      "<key>CFBundleIdentifier</key><string>org.asobmashow.preview-test</string>"
      "<key>CFBundlePackageType</key><string>APPL</string>"
      "</dict></plist>";
  const auto child = fork();
  expect(child >= 0, "launch a decoder inside a real application bundle");
  if (child == 0) {
    execl(binary.c_str(), binary.c_str(), "--bundle-preview", nullptr);
    _exit(127);
  }
  if (child < 0) return;
  int status = 0;
  expect(waitpid(child, &status, 0) == child, "wait for bundled decoder regression");
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0,
         "bundle-only previews resolve alternate extensions outside the working directory");
}

int runBundleOnlyPreviewDecode() {
  const std::filesystem::path requested = "bundle-preview/select.ogg";
  expect(!std::filesystem::exists("bundle-preview/select.wav"),
         "the preview fixture is not visible through ordinary filesystem lookup");
  std::vector<short> pcm;
  SF_INFO info{};
  std::atomic<bool> cancelled{false};
  expect(decodeSkinSoundBundleAware(PATH("bundle-preview/select.wav"), pcm, info, cancelled, {}),
         "the real SDL bundle reader can reach the exact fixture");
  pcm.clear();
  expect(decodeSkinSoundBundleAware(fspath_to_path_t(requested), pcm, info, cancelled, {}),
         "the real SDL bundle reader resolves select.ogg to bundled select.wav");
  expect(!pcm.empty(), "bundle-only extension fallback publishes decoded PCM");
  return failures == 0 ? 0 : 1;
}
#endif

} // namespace

int main(int argc, char **argv) {
#ifdef __APPLE__
  if (argc == 2 && std::string_view(argv[1]) == "--bundle-preview") {
    return runBundleOnlyPreviewDecode();
  }
#endif
#ifndef _WIN32
  SoundSandbox documentsSandbox;
  const char *home = std::getenv("HOME");
  const std::optional<std::string> previousHome = home ? std::make_optional(home) : std::nullopt;
  if (setenv("HOME", documentsSandbox.root().c_str(), 1) != 0) return 2;
  ScopeExit restoreHome([&] {
    if (previousHome) setenv("HOME", previousHome->c_str(), 1);
    else unsetenv("HOME");
  });
  const auto documents = Utils::GetDocumentsPath();
  std::filesystem::create_directories(documents);
#endif
#ifdef __APPLE__
  testBundleOnlyPreviewExtensionFallback(argv[0]);
#endif
  testBundleAwareDecodeProducesPcm();
  for (bool archived : {false, true}) {
    for (bool bundleAware : {false, true}) {
      testPreviewExtensionFallback(archived, bundleAware);
    }
  }
  testBundleAwareDecodeHonorsPcmBudget();
  testBundleAwareDecodeRejectsOversizedEncodedFile();
  testBundleAwareDecodeMissingFileFallsBackToRecordedFailure();
  testBundleAwareDecodeBoundedEncodedFallback();
  testBundleAwareDecodeHonorsStopToken();
#ifndef _WIN32
  expect(std::filesystem::is_empty(documents),
         "skin sound decoding does not create debug files in documents");
#endif
  if (failures != 0) {
    std::cerr << "skin_sound_bundle_decode_tests: " << failures
              << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "skin_sound_bundle_decode_tests passed\n";
  return 0;
}
