#include "StorageTestSupport.h"
#include "StorageVideoTestSupport.h"
#include "../src/storage/StorageOptimizer.h"
#include "../src/audio/decoder.h"

#include <cmath>
#include <map>

using namespace storage_management;
using storage_test::require;
namespace fs = std::filesystem;

void storageLibraryIntegration() {
  storage_test::TempDirectory temp;
  const auto root = temp.root / "charts";
  temp.write("charts/normal.bms", "#TITLE Fixture\n#WAV01 lead.wav\n#WAV02 shared.wav\n#BMP01 movie.mpg\n#STAGEFILE stage.png\n#BMP02 stage.png\n");
  temp.write("charts/hard.bme", "#TITLE Fixture hard\n#WAV01 nested/sabun.wav\n#WAV02 shared.wav\n#BMP01 nested/sabun.mpg\n");
  temp.write("charts/stage.png", "preserved artwork also used as BGA");
  temp.write("archive.zip", "archive group remains unavailable");
  fs::create_directories(root / "nested");
  for (const auto name : {"lead.wav", "shared.wav", "nested/sabun.wav", "nested/unused.wav"}) {
    SF_INFO info{}; info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    info.samplerate = 22050; info.channels = 1;
    auto file = sf_open((root / name).string().c_str(), SFM_WRITE, &info);
    require(file, "create integration keysound");
    std::vector<short> pcm(22050);
    for (std::size_t i = 0; i < pcm.size(); ++i) pcm[i] = static_cast<short>(16000 * std::sin(i * .17));
    require(sf_write_short(file, pcm.data(), pcm.size()) == pcm.size() && sf_close(file) == 0,
            "write integration keysound");
  }
  for (const auto name : {"movie.mpg", "nested/sabun.mpg", "nested/unused.mpg"})
    storage_test::makeVideo(root / name, {});
  std::string error;
  std::map<fs::path, std::string> identities;
  for (const auto name : {"normal.bms", "hard.bme", "stage.png"})
    identities[root / name] = *HashStorageFile(root / name, {}, error);
  // These persisted references use chart paths and hashes, never media names.
  temp.write("profile/score", identities[root / "normal.bms"]);
  temp.write("profile/playlist", (root / "hard.bme").string());
  for (const auto name : {"score", "playlist"})
    identities[temp.root / "profile" / name] = *HashStorageFile(temp.root / "profile" / name, {}, error);
  const auto original = BuildInventory({.groups = {{"folder", root}}}).totalBytes;
  auto run = [&](StoragePolicy policy) {
    OptimizationRequest request;
    request.policy = request.inventory.policy = policy;
    request.inventory.groups = {{"folder", root}, {"overlapping", root / "nested"},
                                {"archive", temp.root / "archive.zip", false}};
    auto report = StorageOptimizer(DefaultOptimizerDependencies(
        MakeStoragePlatformPaths(temp.root / "private", {root}))).Run(request);
    require(!report.failed && !report.cancelled && !report.recoveryRequired && !report.cleanupPending,
            "real codecs and transactions finish without failed or unsettled files");
    for (const auto &[path, hash] : identities)
      require(HashStorageFile(path, {}, error) == hash, "chart, artwork, score and playlist identities survive");
    return report;
  };
  auto decodeSounds = [&] {
    for (const auto name : {"lead.wav", "shared.wav", "nested/sabun.wav", "nested/unused.wav"}) {
      std::vector<short> pcm; SF_INFO info{}; std::atomic_bool cancelled = false;
      require(decodeAudioToPCM(fspath_to_path_t(root / name), pcm, info, cancelled) &&
                  info.samplerate == 22050 && info.channels == 1 && pcm.size() == 22050,
              "gameplay/preview decoder resolves authored WAV names after conversion");
    }
  };
  auto report = run({.audio = AudioAction::LosslessFlac, .bga = BgaAction::CompressH264});
  require(report.converted == 7 && report.reclaimedBytes > 0,
          "whole-folder real conversion includes sabun and unreferenced media exactly once");
  decodeSounds();
  auto lookup = [](const fs::path &path) -> std::optional<fs::path> {
    return fs::is_regular_file(path) ? std::optional(path) : std::nullopt;
  };
  for (const auto name : {"movie.mpg", "nested/sabun.mpg", "nested/unused.mpg"}) {
    auto resolved = asobmshow::chart_assets::ResolveVisualAsset(root / name, lookup);
    require(resolved && resolved->video && storage_test::inspectVideo(resolved->path).h264,
            "shared gameplay/music visual resolver loads converted BGA under authored names");
  }
  report = run({.audio = AudioAction::LossyVorbis});
  require(report.converted == 4, "all four lossless files become smaller Vorbis files");
  decodeSounds();
  storage_test::VideoFixture large; large.h264 = true; large.width = 800; large.height = 600;
  storage_test::makeVideo(root / "large.avi", large);
  report = run({.bga = BgaAction::CompressH264, .resolution = ResolutionCap::Low, .maxFps = 15});
  require(report.converted >= 1, "cap request installs smaller outputs when available");
  const auto capped = storage_test::inspectVideo(root / "large.mp4");
  require(capped.width == 480 && capped.height == 360, "installed output obeys resolution cap");
  for (std::size_t i = 1; i < capped.timestamps.size(); ++i)
    require(capped.timestamps[i] - capped.timestamps[i - 1] >= 1.0 / 15 - .00001,
            "installed output obeys FPS cap");
  report = run({.bga = BgaAction::Delete});
  require(report.deleted == 4 && !fs::exists(root / "movie.mp4") && fs::exists(root / "stage.png"),
          "deletion removes all supported video while preserving BGA/stage image");
  decodeSounds();
  const auto final = BuildInventory({.groups = {{"folder", root}}}).totalBytes;
  require(original && final && *final < *original, "fixture has fewer logical bytes after committed operations");
}
