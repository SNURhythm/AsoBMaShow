#include "replay/BeatorajaReplayCodec.h"
#include "replay/BeatorajaReplayPath.h"
#include "replay/ReplayFileStore.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

class TempDirectory {
public:
  TempDirectory() {
    path = std::filesystem::temp_directory_path() /
           ("asobmashow-codec-store-contract-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(path);
  }
  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  std::filesystem::path path;
};

replay::ReplayChartDocument document() {
  replay::ReplayChartDocument value;
  value.timeBounds = {.completionSongTimeMicros = 5'000'000};
  value.playback.setup.chart = {
      .md5 = std::string(32, 'b'),
      .sha256 = std::string(64, 'a'),
      .keyMode = 7,
  };
  value.playback.setup.longNoteMode = 1;
  value.playback.input = {
      {.songTimeMicros = -1'000'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = true},
      {.songTimeMicros = 1'000,
       .control = {.kind = replay::LogicalControlKind::Lane,
                   .player = 1,
                   .lane = 0},
       .pressed = false},
  };
  return value;
}

void testProducerCodecStoreConsumerClosure() {
  const auto source = document();
  expect(replay::validateReplayPlayback(source.playback,
                                        replay::ReplaySetupSource::LocalCapture,
                                        source.timeBounds)
             .valid(),
         "local producer data satisfies canonical contract");

  replay::BeatorajaReplayCodec codec;
  std::string diagnostic;
  const auto encoded = codec.encodeChart(source, 1, diagnostic);
  expect(encoded.has_value(), "canonical producer data encodes");
  if (!encoded) {
    return;
  }
  const auto stem = replay::chartStem(
      source.playback.setup.chart.sha256, source.playback.setup.longNoteMode,
      source.playback.setup.hasUndefinedLongNotes, diagnostic);
  const auto path =
      stem ? replay::pathForStem(*stem, 0, diagnostic) : std::nullopt;
  expect(path.has_value(), "canonical setup produces stock path identity");
  if (!path) {
    return;
  }

  TempDirectory profile;
  replay::ReplayFileStore store(profile.path);
  const auto reservation = store.reserve(*path, *encoded, "attempt-closure");
  const auto installed = reservation.reservation
                             ? store.install(*reservation.reservation, *encoded)
                             : replay::ReplayInstallOutcome{};
  expect(installed.file &&
             installed.state == replay::ReplayInstallState::InstalledVerified,
         "encoded bytes install under contained Beatoraja path");
  if (!installed.file) {
    return;
  }

  const auto read = store.readVerified(installed.file->metadata);
  expect(read.state == replay::ReplayFileState::Available &&
             read.bytes == encoded,
         "consumer receives only metadata-verified bytes");
  if (!read.bytes) {
    return;
  }
  const replay::ReplayDecodeContext context{
      .stageKeyModes = {source.playback.setup.chart.keyMode},
      .stageTimeBounds = {source.timeBounds},
  };
  const auto decoded = codec.decode(*read.bytes, context);
  expect(decoded.chart == std::optional(source),
         "stored producer bytes close through the same consumer contract");

  expect(store.removeIfMatches(installed.file->metadata, diagnostic) &&
             store.inspect(installed.file->metadata).state ==
                 replay::ReplayFileState::Missing,
         "proven unassociated artifact can be removed without another owner");
}

} // namespace

int main() {
  testProducerCodecStoreConsumerClosure();
  if (failures != 0) {
    std::cerr << failures << " codec/store closure test(s) failed\n";
    return 1;
  }
  std::cout << "codec/store closure tests passed\n";
  return 0;
}
