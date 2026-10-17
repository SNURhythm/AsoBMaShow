#include "../src/storage/AudioTranscoder.h"
#include "StorageTestSupport.h"

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
                const std::vector<int> &pcm, bool loop = false) {
  SF_INFO info{};
  info.samplerate = 22050;
  info.channels = channels;
  info.format = format;
  auto file = sf_open(path.string().c_str(), SFM_WRITE, &info);
  require(file, "open generated audio");
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
}
int main() {
  try { audioTests(); }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
  return 0;
}
