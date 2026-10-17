#include "../src/storage/AudioTranscoder.h"
#include "../src/storage/BgaTranscoder.h"
#include "StorageTestSupport.h"
#include "StorageVideoTestSupport.h"

#include <sndfile.h>
#include <cmath>
#include <iostream>
#include <vector>

using namespace storage_management;
using storage_test::require;
namespace {
std::vector<int> samples(int frames, int channels, int bits) {
  std::vector<int> result(frames * channels);
  const int step = 1 << (32 - bits);
  for (int i = 0; i < frames; ++i)
    for (int channel = 0; channel < channels; ++channel)
      result[i * channels + channel] = static_cast<int>(
          std::sin(i * .07 + channel) * (1 << (bits - 2))) * step;
  result[0] = 1 << 29;
  result.back() = 1 << 28;
  return result;
}
void writeAudio(const std::filesystem::path &path, int format, int channels,
                const std::vector<int> &pcm, bool loop = false, bool rearChannels = false) {
  SF_INFO info{};
  info.samplerate = 22050;
  info.channels = channels;
  info.format = format;
  auto file = sf_open(path.string().c_str(), SFM_WRITE, &info);
  require(file, "open generated audio");
  if (rearChannels) {
    int map[2] = {SF_CHANNEL_MAP_REAR_LEFT, SF_CHANNEL_MAP_REAR_RIGHT};
    require(sf_command(file, SFC_SET_CHANNEL_MAP_INFO, map, sizeof(map)), "write nonstandard channel layout");
  }
  if (loop) {
    SF_INSTRUMENT instrument{};
    instrument.loop_count = 1;
    instrument.loops[0].mode = SF_LOOP_FORWARD;
    instrument.loops[0].start = 10;
    instrument.loops[0].end = 100;
    require(sf_command(file, SFC_SET_INSTRUMENT, &instrument, sizeof(instrument)),
            "write playback loop metadata");
  }
  require(sf_write_int(file, pcm.data(), pcm.size()) == pcm.size(), "write PCM");
  require(sf_close(file) == 0, "close generated audio");
}
std::vector<int> readAudio(const std::filesystem::path &path, int channels, int frames) {
  SF_INFO info{};
  auto file = sf_open(path.string().c_str(), SFM_READ, &info);
  require(file && info.channels == channels && info.samplerate == 22050 &&
              info.frames == frames, "native rate, channels and exact length preserved");
  std::vector<int> pcm(frames * channels);
  require(sf_read_int(file, pcm.data(), pcm.size()) == pcm.size(), "complete decode");
  int extra[2]{};
  require(sf_read_int(file, extra, channels) == 0 && sf_error(file) == 0,
          "no trailing decoded samples or error");
  sf_close(file);
  return pcm;
}
AssetCandidate candidate(const std::filesystem::path &path) {
  AssetCandidate result;
  result.source = path;
  result.identity.bytes = std::filesystem::file_size(path);
  return result;
}
void audioTests() {
  storage_test::TempDirectory temp;
  StoragePolicy policy;
  policy.audio = AudioAction::LosslessFlac;
  const auto caps = ProbeStorageCodecs();
  require(caps.flac.available && caps.vorbis.available, "linked FLAC and Vorbis encoders available");
  auto unavailable = caps;
  unavailable.flac.available = false;
  require(ChooseAudioEncoding(SF_FORMAT_WAV | SF_FORMAT_PCM_16, 22050, 1,
                             policy.audio, unavailable).reason.code == StorageReason::CodecUnavailable,
          "unavailable encoder never changes requested codec");
  require(ChooseAudioEncoding(SF_FORMAT_WAV | SF_FORMAT_PCM_16, 22050, 9,
                             policy.audio, caps).reason.code == StorageReason::UnsupportedFormat,
          "unsupported channel layouts are not remixed");
  for (int bits : {8, 16, 24}) {
    for (int channels : {1, 2}) {
      const auto source = temp.root / (std::to_string(bits) + "-" + std::to_string(channels) + ".wav");
      const auto output = temp.root / (source.stem().string() + ".flac");
      const int subtype = bits == 8 ? SF_FORMAT_PCM_U8 : bits == 16 ? SF_FORMAT_PCM_16 : SF_FORMAT_PCM_24;
      const auto pcm = samples(8192, channels, bits);
      writeAudio(source, SF_FORMAT_WAV | subtype, channels, pcm);
      auto result = EncodeAudio(candidate(source), policy, output, {}, {});
      require(result.outcome == FileOutcome::Converted, "compress supported integer WAV to FLAC");
      require(readAudio(output, channels, 8192) == pcm, "FLAC preserves every native integer sample");
      require(std::filesystem::exists(source), "encoding never retires source");
    }
  }
  for (int subtype : {SF_FORMAT_FLOAT, SF_FORMAT_PCM_32}) {
    auto source = temp.root / "unsupported.wav";
    writeAudio(source, SF_FORMAT_WAV | subtype, 1, samples(8192, 1, 24));
    require(EncodeAudio(candidate(source), policy, temp.root / "unsupported.flac", {}, {}).reason.code ==
                StorageReason::UnsupportedFormat, "lossless mode never quantizes float or 32-bit PCM");
  }
  auto loop = temp.root / "loop.wav";
  writeAudio(loop, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, samples(8192, 1, 16), true);
  require(EncodeAudio(candidate(loop), policy, temp.root / "loop.flac", {}, {}).reason.code ==
              StorageReason::UnsupportedFormat, "unrepresentable loop metadata keeps original");
  auto rear = temp.root / "rear.wav";
  writeAudio(rear, SF_FORMAT_WAVEX | SF_FORMAT_PCM_16, 2, samples(8192, 2, 16), false, true);
  require(EncodeAudio(candidate(rear), policy, temp.root / "rear.flac", {}, {}).reason.code ==
              StorageReason::UnsupportedFormat, "two rear channels are not silently relabeled as front stereo");
  auto tiny = temp.root / "tiny.wav";
  writeAudio(tiny, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, {1 << 28});
  require(EncodeAudio(candidate(tiny), policy, temp.root / "tiny.flac", {}, {}).reason.code ==
              StorageReason::NotSmaller, "larger output is discarded");
  require(!std::filesystem::exists(temp.root / "tiny.flac"), "discarded output leaves no temporary file");
  // Set the source's harmless RIFF padding to the encoder's exact container
  // size. Equality must still discard the result, not report a conversion.
  writeAudio(temp.root / "calibration.flac", SF_FORMAT_FLAC | SF_FORMAT_PCM_16, 1, {1 << 28});
  const auto equalSize = std::filesystem::file_size(temp.root / "calibration.flac");
  const auto oldSize = std::filesystem::file_size(tiny);
  require(equalSize >= oldSize + 8 && (equalSize - oldSize) % 2 == 0, "equal-size fixture has RIFF padding space");
  {
    std::fstream wave(tiny, std::ios::in | std::ios::out | std::ios::binary);
    auto put32 = [&](std::uint32_t value) {
      for (int i = 0; i < 4; ++i) wave.put(static_cast<char>((value >> (8 * i)) & 255));
    };
    wave.seekp(4); put32(equalSize - 8);
    wave.seekp(0, std::ios::end); wave.write("JUNK", 4); put32(equalSize - oldSize - 8);
    wave << std::string(equalSize - oldSize - 8, '\0');
  }
  require(EncodeAudio(candidate(tiny), policy, temp.root / "equal.flac", {}, {}).reason.code ==
              StorageReason::NotSmaller, "equal-size output is discarded too");

  auto source = temp.root / "16-2.wav";
  const auto wrongContainer = temp.root / "wrong-container.flac";
  require(EncodeAudio(candidate(source), policy, wrongContainer, {}, [&](const StorageProgress &value) {
    if (value.phase == StoragePhase::Verify)
      std::filesystem::copy_file(source, wrongContainer, std::filesystem::copy_options::overwrite_existing);
  }).reason.code == StorageReason::ValidationFailed, "decode-back verifies the requested codec, not only PCM shape");
  require(EncodeAudio(candidate(source), policy, temp.root / "cancel.flac", [] { return false; }, {}).outcome ==
              FileOutcome::Cancelled, "cancel before encode");
  int checkpoints = 0;
  bool encoding = false;
  require(EncodeAudio(candidate(source), policy, temp.root / "cancel.flac",
                      [&] { return !encoding || ++checkpoints < 2; },
                      [&](const StorageProgress &value) { encoding = value.phase == StoragePhase::Encode; }).outcome == FileOutcome::Cancelled &&
              !std::filesystem::exists(temp.root / "cancel.flac"), "cancel during bounded encoding removes partial output");
  temp.write("foreign.flac", "foreign bytes");
  require(EncodeAudio(candidate(source), policy, temp.root / "foreign.flac", {}, {}).outcome != FileOutcome::Converted &&
              std::filesystem::file_size(temp.root / "foreign.flac") == 13, "temporary output never clobbers existing data");
  auto truncated = temp.root / "truncated.wav";
  std::filesystem::copy_file(source, truncated);
  std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) - 20);
  require(EncodeAudio(candidate(truncated), policy, temp.root / "truncated.flac", {}, {}).reason.code ==
              StorageReason::ValidationFailed, "truncated WAV must not be silently shortened");
  temp.write("corrupt.wav", "not audio");
  require(EncodeAudio(candidate(temp.root / "corrupt.wav"), policy, temp.root / "corrupt.flac", {}, {}).outcome !=
              FileOutcome::Converted, "corrupt source retained");

  policy.audio = AudioAction::LossyVorbis;
  auto truncatedFlac = temp.root / "truncated.flac";
  std::filesystem::copy_file(temp.root / "24-1.flac", truncatedFlac);
  std::filesystem::resize_file(truncatedFlac, std::filesystem::file_size(truncatedFlac) - 30);
  require(EncodeAudio(candidate(truncatedFlac), policy, temp.root / "truncated.ogg", {}, {}).reason.code ==
              StorageReason::ValidationFailed, "truncated FLAC must not be silently shortened");
  for (const auto &name : {"16-2.wav", "24-1.flac"}) {
    source = temp.root / name;
    const int channels = name == std::string("16-2.wav") ? 2 : 1;
    auto output = temp.root / (std::string(name) + ".ogg");
    auto result = EncodeAudio(candidate(source), policy, output, {}, {});
    require(result.outcome == FileOutcome::Converted, "WAV and FLAC convert to Vorbis");
    const auto decoded = readAudio(output, channels, 8192);
    require(std::abs(static_cast<double>(decoded.front())) > 1000000 &&
                std::abs(static_cast<double>(decoded.back())) > 1000000,
            "Vorbis retains leading impulse and nonzero tail");
    require(EncodeAudio(candidate(output), policy, temp.root / "reencoded.ogg", {}, {}).reason.code ==
                StorageReason::UnsupportedSource, "existing Ogg is never recompressed");
  }
  require(ChooseAudioEncoding(SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III, 22050, 2,
                             policy.audio, caps).reason.code == StorageReason::UnsupportedSource,
          "existing MP3 is never recompressed");
}

void videoTests() {
  storage_test::TempDirectory temp;
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  const auto source = temp.root / "old.mpg";
  storage_test::makeVideo(source, {});
  auto result = EncodeBga(candidate(source), policy, temp.root / "old.mp4", {}, {});
  if (result.outcome != FileOutcome::Converted) std::cerr << "MPEG conversion: " << result.reason.detail << '\n';
  require(result.outcome == FileOutcome::Converted, "inefficient MPEG1 becomes smaller H264");
  const auto ordinary = storage_test::inspectVideo(temp.root / "old.mp4");
  require(ordinary.h264 && ordinary.width == 160 && ordinary.height == 120 && ordinary.timestamps.size() == 30 &&
              std::abs(ordinary.videoEnd - 1.2) < .001, "original geometry, frame count and full presentation timeline preserved");
  require(EncodeBga(candidate(temp.root / "old.mp4"), policy, temp.root / "twice.mp4", {}, {}).reason.code ==
              StorageReason::AlreadyOptimized, "existing H264 under caps is skipped");
  std::filesystem::copy_file(source, temp.root / "preserved-image.png");
  require(EncodeBga(candidate(temp.root / "preserved-image.png"), policy, temp.root / "image.mp4", {}, {}).reason.code ==
              StorageReason::UnsupportedSource, "video encoder cannot consume a preserved image path");
  for (bool highRate : {false, true}) {
    storage_test::VideoFixture fixture; fixture.rate = {highRate ? 60000 : 30000, 1001};
    for (int i = 0; i < (highRate ? 60 : 30); ++i) fixture.timestamps.push_back(i);
    const auto input = temp.root / (highRate ? "high.mpg" : "fractional.mpg");
    const auto output = temp.root / (highRate ? "high.mp4" : "fractional.mp4");
    storage_test::makeVideo(input, fixture); policy.maxFps = 30;
    result = EncodeBga(candidate(input), policy, output, {}, {});
    if (result.outcome != FileOutcome::Converted) std::cerr << "FPS conversion: " << result.reason.detail << '\n';
    require(result.outcome == FileOutcome::Converted, "fractional-rate MPEG conversion succeeds");
    const auto decoded = storage_test::inspectVideo(output);
    require(decoded.timestamps.size() == 30 && std::abs(decoded.videoEnd - 1.001) < .002,
            "rational FPS cap retains duration without duplicate frames");
    for (std::size_t i = 1; i < decoded.timestamps.size(); ++i)
      require(std::abs(decoded.timestamps[i] - i * (highRate ? 1.0 / 30 : 1001.0 / 30000)) < .00001,
              "retained frames follow the intended rational timeline");
  }
  // Matroska's millisecond clock rounds 29.97/30 FPS to 33/34 ms intervals.
  // A cap must preserve an already compliant cadence, including every frame.
  for (const auto rate : {AVRational{30000, 1001}, AVRational{30, 1}}) {
    storage_test::VideoFixture fixture; fixture.rate = rate; fixture.millisecondContainer = fixture.lossless = true;
    for (int i = 0; i < 60; ++i) fixture.timestamps.push_back(i);
    const auto input = temp.root / (rate.den == 1 ? "rounded-30.webm" : "rounded-2997.webm");
    const auto output = temp.root / (rate.den == 1 ? "rounded-30.mp4" : "rounded-2997.mp4");
    storage_test::makeVideo(input, fixture);
    const auto original = storage_test::inspectVideo(input);
    require(original.timestamps.size() == 60 && std::abs(original.timestamps[1] - .033) < .00001 &&
                std::abs(original.timestamps[2] - .067) < .00001,
            "fixture exercises millisecond-quantized frame timing");
    result = EncodeBga(candidate(input), policy, output, {}, {});
    if (result.outcome != FileOutcome::Converted) std::cerr << "Quantized CFR: " << result.reason.detail << '\n';
    require(result.outcome == FileOutcome::Converted, "quantized CFR conversion succeeds below FPS cap");
    const auto decoded = storage_test::inspectVideo(output);
    require(decoded.timestamps.size() == 60, "under-cap quantized CFR never drops frames");
    for (std::size_t i = 0; i < decoded.timestamps.size(); ++i)
      require(std::abs(decoded.timestamps[i] - original.timestamps[i]) < .00001,
              "under-cap quantized CFR preserves each presentation timestamp");
    require(std::abs(decoded.videoEnd - original.videoEnd) < .001,
            "under-cap quantized CFR retains final duration");
  }
  storage_test::VideoFixture roundedH264; roundedH264.rate = {30000, 1001};
  roundedH264.h264 = roundedH264.millisecondContainer = true;
  storage_test::makeVideo(temp.root / "rounded-h264.webm", roundedH264);
  require(EncodeBga(candidate(temp.root / "rounded-h264.webm"), policy,
                    temp.root / "rounded-h264.mp4", {}, {}).reason.code == StorageReason::AlreadyOptimized,
          "quantized H264 below the cap is not repeatedly reencoded");
  storage_test::VideoFixture gapFixture; gapFixture.timestamps = {0, 1, 2, 8, 9, 10, 20, 21, 22, 23, 24};
  storage_test::makeVideo(temp.root / "gaps.mpg", gapFixture);
  result = EncodeBga(candidate(temp.root / "gaps.mpg"), policy, temp.root / "gaps.mp4", {}, {});
  require(result.outcome == FileOutcome::Converted, "VFR source converts");
  const auto gaps = storage_test::inspectVideo(temp.root / "gaps.mp4");
  require(gaps.timestamps.size() == 11 && gaps.timestamps[3] >= .3 && gaps.timestamps[6] >= .79 &&
              std::abs(gaps.videoEnd - 1.0) < .002, "VFR gaps and final duration survive without fill frames");
  gapFixture.rate = {60, 1}; gapFixture.timestamps = {0, 1, 2, 12, 13, 14, 24, 25, 26, 28, 29};
  storage_test::makeVideo(temp.root / "fast-gaps.mpg", gapFixture);
  result = EncodeBga(candidate(temp.root / "fast-gaps.mpg"), policy, temp.root / "fast-gaps.mp4", {}, {});
  require(result.outcome == FileOutcome::Converted, "VFR bursts convert with an FPS cap");
  const auto fastGaps = storage_test::inspectVideo(temp.root / "fast-gaps.mp4");
  require(fastGaps.timestamps.size() == 7 && std::abs(fastGaps.timestamps[2] - .2) < .00001 &&
              std::abs(fastGaps.timestamps[4] - .4) < .00001 && std::abs(fastGaps.videoEnd - .5) < .001,
          "capped VFR drops burst frames while retaining gaps and end time");
  storage_test::VideoFixture large; large.h264 = true; large.width = 800; large.height = 600;
  for (int i = 0; i < 12; ++i) large.timestamps.push_back(i);
  storage_test::makeVideo(temp.root / "large.avi", large); policy.resolution = ResolutionCap::Low;
  result = EncodeBga(candidate(temp.root / "large.avi"), policy, temp.root / "large.mp4", {}, {});
  require(result.outcome == FileOutcome::Converted, "H264 exceeding a cap is converted");
  const auto scaled = storage_test::inspectVideo(temp.root / "large.mp4");
  require(scaled.width == 480 && scaled.height == 360, "real H264 obeys orientation-independent cap");
  auto oriented = large; oriented.rotation = 90; oriented.sampleAspectRatio = {16, 15};
  storage_test::makeVideo(temp.root / "oriented.mp4", oriented);
  result = EncodeBga(candidate(temp.root / "oriented.mp4"), policy, temp.root / "oriented-small.mp4", {}, {});
  require(result.outcome == FileOutcome::Converted, "rotated anamorphic source converts");
  const auto orientation = storage_test::inspectVideo(temp.root / "oriented-small.mp4");
  require(std::abs(orientation.rotation) == 90 && av_cmp_q(orientation.sampleAspectRatio, {16, 15}) == 0,
          "real sample aspect ratio and display matrix survive MP4 conversion");
  oriented.rotation = 0; oriented.scaledDisplayMatrix = true;
  storage_test::makeVideo(temp.root / "matrix-scale.mp4", oriented);
  require(EncodeBga(candidate(temp.root / "matrix-scale.mp4"), policy, temp.root / "matrix-output.mp4", {}, {}).reason.code ==
              StorageReason::UnsupportedFormat, "unhandled display-matrix scale must not bypass displayed resolution caps");
  policy.resolution = ResolutionCap::Original; policy.maxFps = 0;
  storage_test::VideoFixture audioFixture; audioFixture.audio = true;
  storage_test::makeVideo(temp.root / "audio.avi", audioFixture);
  result = EncodeBga(candidate(temp.root / "audio.avi"), policy, temp.root / "audio.mp4", {}, {});
  if (result.outcome != FileOutcome::Converted) std::cerr << "Audio conversion: " << result.reason.detail << '\n';
  require(result.outcome == FileOutcome::Converted, "embedded PCM is converted to AAC without dropping audio");
  const auto audio = storage_test::inspectVideo(temp.root / "audio.mp4");
  require(audio.audioChannels == 2 && audio.audioRate == 48000 && std::abs(audio.audioEnd - 1.2) < .022,
          "embedded audio keeps native layout, rate and timeline within one packet");
  policy.maxFps = 15;
  result = EncodeBga(candidate(temp.root / "audio.mp4"), policy, temp.root / "remux.mp4", {}, {});
  require(result.outcome == FileOutcome::Converted &&
              storage_test::audioPackets(temp.root / "audio.mp4") == storage_test::audioPackets(temp.root / "remux.mp4"),
          "compatible embedded audio is remuxed without another lossy encode");
  policy.maxFps = 0;
  for (int kind = 0; kind < 3; ++kind) {
    storage_test::VideoFixture unsupported; unsupported.timestamps = {0, 1};
    unsupported.alpha = kind == 0; unsupported.hdr = kind == 1; unsupported.interlaced = kind == 2;
    unsupported.h264 = !unsupported.alpha;
    const auto path = temp.root / ("unsupported-" + std::to_string(kind) + ".avi");
    storage_test::makeVideo(path, unsupported);
    require(EncodeBga(candidate(path), policy, temp.root / "unsupported.mp4", {}, {}).reason.code ==
                StorageReason::UnsupportedFormat, "real alpha HDR and interlaced inputs are preserved");
  }
  require(EncodeBga(candidate(source), policy, temp.root / "cancel.mp4", [] { return false; }, {}).outcome ==
              FileOutcome::Cancelled, "BGA cancellation preserves source");
  bool encoding = false; int checks = 0;
  require(EncodeBga(candidate(source), policy, temp.root / "cancel.mp4",
                    [&] { return !encoding || ++checks < 8; },
                    [&](const StorageProgress &value) { encoding = value.phase == StoragePhase::Encode; }).outcome ==
              FileOutcome::Cancelled && !std::filesystem::exists(temp.root / "cancel.mp4"),
          "cancelled video conversion discards temporary output");
  require(EncodeBga(candidate(source), policy, temp.root / "broken.mp4", {},
                    [&](const StorageProgress &value) {
                      if (value.phase == StoragePhase::Verify) std::filesystem::resize_file(temp.root / "broken.mp4", 40);
                    }).reason.code == StorageReason::ValidationFailed,
          "truncated output is rejected by complete decode-back");
  storage_test::VideoFixture tiny; tiny.width = 16; tiny.height = 16; tiny.timestamps = {0};
  storage_test::makeVideo(temp.root / "tiny.mpg", tiny);
  require(EncodeBga(candidate(temp.root / "tiny.mpg"), policy, temp.root / "tiny.mp4", {}, {}).reason.code ==
              StorageReason::NotSmaller, "larger MP4 overhead keeps original video");
  temp.write("corrupt.mpg", "bad packet stream");
  require(EncodeBga(candidate(temp.root / "corrupt.mpg"), policy, temp.root / "corrupt.mp4", {}, {}).reason.code ==
              StorageReason::ValidationFailed, "corrupt video is retained");
  std::ifstream original(source, std::ios::binary);
  std::string corruptBytes((std::istreambuf_iterator<char>(original)), {});
  const auto picture = corruptBytes.find(std::string("\0\0\1\0", 4));
  require(picture != std::string::npos && picture + 80 < corruptBytes.size(), "fixture contains a real MPEG picture packet");
  corruptBytes.replace(picture + 4, 64, 64, '\xff');
  temp.write("corrupt-packet.mpg", corruptBytes);
  require(EncodeBga(candidate(temp.root / "corrupt-packet.mpg"), policy, temp.root / "corrupt-packet.mp4", {}, {}).outcome !=
              FileOutcome::Converted, "corrupt MPEG packet must never install a concealed decode");
}
}
int main() {
  av_log_set_level(AV_LOG_ERROR);
  try { audioTests(); videoTests(); }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
  return 0;
}
