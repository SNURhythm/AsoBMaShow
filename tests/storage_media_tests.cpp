#include "../src/storage/AudioTranscoder.h"
// Include the implementation to exercise malformed timeline boundaries directly.
#include "../src/storage/BgaTranscoder.cpp"
#include "StorageTestSupport.h"
#include "StorageVideoTestSupport.h"

#include <sndfile.h>
extern "C" {
#include <libavutil/downmix_info.h>
}
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
                const std::vector<int> &pcm, bool loop = false, bool rearChannels = false, bool tags = false) {
  SF_INFO info{};
  info.samplerate = 22050;
  info.channels = channels;
  info.format = format;
  auto file = sf_open(path.string().c_str(), SFM_WRITE, &info);
  require(file, "open generated audio");
  if (tags) {
    require(sf_set_string(file, SF_STR_TITLE, "Chart keysound") == 0 &&
        sf_set_string(file, SF_STR_SOFTWARE, "Wave editor") == 0, "write descriptive WAV tags");
  }
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
void verifyWaveMetadata(const std::string &source, const std::filesystem::path &output) {
  std::ifstream encoded(output, std::ios::binary);
  char magic[4]{}; encoded.read(magic, 4);
  require(std::string(magic, 4) == "fLaC", "metadata output is native FLAC");
  std::vector<std::string> foreign;
  bool last = false;
  while (!last) {
    unsigned char header[4]{};
    require(bool(encoded.read(reinterpret_cast<char *>(header), 4)), "read FLAC metadata header");
    last = (header[0] & 128) != 0;
    const auto size = (header[1] << 16) | (header[2] << 8) | header[3];
    std::string block(size, '\0');
    require(bool(encoded.read(block.data(), size)), "read FLAC metadata block");
    if ((header[0] & 127) == 2 && block.compare(0, 4, "riff") == 0) foreign.push_back(block.substr(4));
  }
  std::vector<std::string> expected{source.substr(0, 12)};
  for (std::size_t offset = 12; offset < source.size();) {
    std::uint32_t size = 0;
    for (int i = 0; i < 4; ++i) size |= std::uint32_t(static_cast<unsigned char>(source[offset + 4 + i])) << (8 * i);
    expected.push_back(source.substr(offset, source.compare(offset, 4, "data") == 0 ? 8 : 8 + size + (size & 1)));
    offset += 8 + size + (size & 1);
  }
  require(foreign == expected, "standard riff APPLICATION blocks retain every chunk, order and padding exactly");
}
std::string withFlacComments(std::string bytes, const std::vector<std::string> &comments,
                             std::string_view trailing = {}) {
  std::string block;
  const auto appendSize = [&](std::uint32_t value) {
    for (int i = 0; i < 4; ++i) block.push_back(static_cast<char>(value >> (8 * i)));
  };
  appendSize(7); block += "fixture";
  appendSize(static_cast<std::uint32_t>(comments.size()));
  for (const auto &comment : comments) {
    appendSize(static_cast<std::uint32_t>(comment.size())); block += comment;
  }
  block += trailing;
  require(bytes.compare(0, 4, "fLaC") == 0, "comment fixture is native FLAC");
  for (std::size_t offset = 4; offset + 4 <= bytes.size();) {
    const auto type = static_cast<unsigned char>(bytes[offset]);
    const auto size = (static_cast<unsigned char>(bytes[offset + 1]) << 16) |
        (static_cast<unsigned char>(bytes[offset + 2]) << 8) | static_cast<unsigned char>(bytes[offset + 3]);
    require(size <= bytes.size() - offset - 4, "comment fixture has complete metadata blocks");
    if ((type & 127) == 4) {
      bytes.replace(offset + 4, size, block);
      for (int i = 0; i < 3; ++i) bytes[offset + 1 + i] = static_cast<char>(block.size() >> (16 - 8 * i));
      return bytes;
    }
    if (type & 128) break;
    offset += 4 + size;
  }
  throw std::runtime_error("FLAC fixture has no comment block");
}
void timelineTests() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  for (const auto start : std::array<std::int64_t, 2>{minimum + 1, -10}) {
    Timeline timeline;
    timeline.add(start, 1);
    bool rejected = false;
    try { timeline.add(maximum - 2, 1); }
    catch (const Failure &failure) { rejected = failure.reason == StorageReason::UnsupportedFormat; }
    require(rejected, "reject overflowing presentation span before subtracting timestamps");
  }
  Timeline negative;
  negative.add(minimum + 1, 1); negative.add(minimum + 2, 1);
  require(!negative.discontinuous && negative.minimumInterval == 1,
          "adjacent negative timestamps remain valid near the lower bound");
  Timeline continuous;
  continuous.add(-10, 10); continuous.add(0, 10);
  require(!continuous.discontinuous && continuous.minimumInterval == 10,
          "valid timeline may cross zero");
  Timeline overlapping;
  overlapping.add(-5, 10); overlapping.add(0, 1, 5);
  require(!overlapping.discontinuous, "overlap at tolerance is accepted");
  overlapping.add(1, 1);
  require(!overlapping.discontinuous, "adjacent intervals remain continuous");
}
void declaredVideoTimelineTests() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto accepts = [](std::int64_t start, std::int64_t duration,
                          std::int64_t first, std::int64_t frameDuration) {
    AVStream stream{};
    stream.start_time = start; stream.duration = duration;
    Timeline decoded;
    decoded.add(first, frameDuration);
    try { validateDeclaredVideoTimeline(stream, decoded); }
    catch (const Failure &failure) {
      require(failure.reason == StorageReason::ValidationFailed, "malformed container timing fails validation");
      return false;
    }
    return true;
  };
  require(!accepts(maximum - 1, 2, maximum - 1, 1),
          "declared end overflow cannot wrap into an accepted duration tolerance");
  require(!accepts(maximum, 0, -1, 1),
          "opening timestamp difference cannot overflow abs at INT64_MIN");
  require(!accepts(minimum + 1, 0, maximum - 1, 1),
          "opening timestamps at opposite extremes are rejected");
  require(accepts(maximum - 1, 1, maximum - 1, 1) &&
              accepts(minimum + 1, 1, minimum + 1, 1),
          "representable declared timing remains valid at both limits");
  require(accepts(0, 2, 0, 1) && !accepts(0, 3, 0, 1),
          "declared end keeps the one-frame tolerance boundary");
  require(accepts(AV_NOPTS_VALUE, maximum, 0, 1),
          "unknown container start does not invent a declared end");
}
void crossStreamTimelineTests() {
  const auto maximum = std::numeric_limits<std::int64_t>::max();
  const auto minimum = std::numeric_limits<std::int64_t>::min();
  const auto rejectsRange = [](auto operation) {
    try { operation(); }
    catch (const Failure &failure) { return failure.reason == StorageReason::UnsupportedFormat; }
    return false;
  };
  require(rejectsRange([&] { normalizedTimestamp(maximum - 2, AV_TIME_BASE_Q, -10, AV_TIME_BASE_Q); }) &&
              rejectsRange([&] { normalizedTimestamp(minimum + 1, AV_TIME_BASE_Q, 10, AV_TIME_BASE_Q); }),
          "origin subtraction rejects overflow at either signed boundary");
  require(rejectsRange([&] { normalizedTimestamp(maximum / 2, {1, 1}, 0, AV_TIME_BASE_Q); }) &&
              rejectsRange([&] { normalizedTimestamp(0, AV_TIME_BASE_Q, maximum / 2, {1, 1000000000}); }),
          "timestamp and origin rescaling reject FFmpeg's overflow sentinel");
  require(rejectsRange([&] { timestampAdd(maximum, 1); }) &&
              rejectsRange([&] { timestampAdd(minimum + 1, -2); }) &&
              rejectsRange([&] { timestampSubtract(minimum + 1, 1); }),
          "frame offsets and durations cannot overflow or become unknown timestamps");
  require(normalizedTimestamp(maximum - 1, AV_TIME_BASE_Q, maximum - 2, AV_TIME_BASE_Q) == 1 &&
              normalizedTimestamp(minimum + 2, AV_TIME_BASE_Q, minimum + 1, AV_TIME_BASE_Q) == 1 &&
              normalizedTimestamp(-48000, {1, 48000}, -2000000, {1, 48000}) == 48000,
          "near-limit and negative origins preserve representable relative timing");
  AVPacket remux{};
  remux.pts = maximum / 2; remux.dts = remux.pts; remux.duration = 1;
  require(rejectsRange([&] { rescalePacketTimestamps(remux, {1, 1}, AV_TIME_BASE_Q); }),
          "remux rescaling rejects timestamps that cannot fit the muxer clock");
  Scan source, output;
  output.videoDisposition = AV_DISPOSITION_DEFAULT;
  source.video.timeBase = output.video.timeBase = AV_TIME_BASE_Q;
  source.audio.timeBase = AV_TIME_BASE_Q;
  source.video.add(maximum - 2, 1);
  source.audio.add(-10, 1);
  source.originMicros = -10;
  output.video.add(0, 1);
  output.description.h264 = true;
  output.description.width = output.description.height = 16;
  output.description.sampleAspectRatio = {1, 1};
  VideoCompressionPlan plan;
  plan.width = plan.height = 16; plan.frameRate = {25, 1}; plan.sampleAspectRatio = {1, 1};
  bool rejected = false;
  try { verify(source, output, plan, {1}); }
  catch (const Failure &failure) { rejected = failure.reason == StorageReason::UnsupportedFormat; }
  require(rejected, "cross-stream origin rejects an unrepresentable normalized video timestamp");
  plan.timeBase = AV_TIME_BASE_Q;
  bool created = false;
  require(rejectsRange([&] { encode({}, {}, source, plan, {}, created); }) && !created,
          "encoder rejects cross-stream overflow before opening an output");
  source.video = {}; source.video.timeBase = AV_TIME_BASE_Q; source.video.add(0, 1);
  source.audio = {}; source.audio.timeBase = AV_TIME_BASE_Q; source.audio.add(maximum - 2, 1);
  source.description.hasAudio = true;
  source.description.audioSampleRate = 48000;
  plan.audio = VideoAudioTreatment::Remux;
  require(rejectsRange([&] { encode({}, {}, source, plan, {}, created); }) && !created,
          "encoder also validates a distant remuxed audio endpoint");
  plan.audio = VideoAudioTreatment::Aac;
  source.originMicros = minimum + 1;
  require(rejectsRange([&] { encode({}, {}, source, plan, {}, created); }) && !created,
          "encoder rejects a shared origin outside the destination timestamp range");
}
void existingAudioTests() {
  storage_test::TempDirectory temp;
  for (const auto action : {AudioAction::LosslessFlac, AudioAction::LossyVorbis}) {
    const bool flac = action == AudioAction::LosslessFlac;
    const auto path = temp.root / (flac ? "existing.flac" : "existing.ogg");
    writeAudio(path, flac ? SF_FORMAT_FLAC | SF_FORMAT_PCM_16 : SF_FORMAT_OGG | SF_FORMAT_VORBIS,
               2, samples(8192, 2, 16), false, false, true);
    require(ValidateExistingAudio(path, action).code == StorageReason::None,
            "complete existing FLAC/Vorbis can be retained without re-encoding");
    require(ValidateExistingAudio(path, flac ? AudioAction::LossyVorbis : AudioAction::LosslessFlac).code != StorageReason::None,
            "existing destination must contain the requested audio codec");
    require(ValidateExistingAudio(path, action, [] { return false; }).code == StorageReason::Cancelled,
            "existing audio validation observes cancellation");
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    temp.write(path.filename(), bytes.substr(0, bytes.size() - 32));
    require(ValidateExistingAudio(path, action).code == StorageReason::ValidationFailed,
            "truncated existing audio cannot authorize source removal");
    if (flac) {
      auto understated = bytes;
      require(understated.size() > 26, "FLAC fixture has a complete STREAMINFO block");
      // Keep every encoded frame, but advertise only 1000 of the 8192 samples.
      understated[21] &= 0xf0;
      understated[22] = understated[23] = 0;
      understated[24] = 3; understated[25] = static_cast<char>(0xe8);
      temp.write(path.filename(), understated);
      require(ValidateExistingAudio(path, action).code == StorageReason::ValidationFailed,
              "FLAC validation reaches physical EOF rather than stopping at an understated sample count");
      StoragePolicy policy;
      policy.audio = AudioAction::LossyVorbis;
      const auto output = temp.root / "understated.ogg";
      const auto result = EncodeAudio(candidate(path), policy, output, {}, {});
      require(result.reason.code == StorageReason::ValidationFailed && !std::filesystem::exists(output),
              "FLAC conversion cannot retire trailing samples hidden by an understated sample count");
    } else {
      const auto endPage = bytes.rfind("OggS");
      require(endPage != std::string::npos && endPage > 0, "Vorbis fixture has a terminal page");
      temp.write(path.filename(), bytes.substr(0, endPage));
      require(ValidateExistingAudio(path, action).code == StorageReason::ValidationFailed,
              "clean page-boundary truncation still requires an Ogg end-of-stream marker");
      auto corrupt = bytes; corrupt.back() ^= 0x40;
      temp.write(path.filename(), corrupt);
      require(ValidateExistingAudio(path, action).code == StorageReason::ValidationFailed,
              "Ogg page checksum detects corruption that a tolerant decoder could conceal");
    }
  }
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
  const auto rejectPartialFrame = [&](const std::filesystem::path &source, int missingBytes,
                                       const StoragePolicy &selectedPolicy) {
    std::ifstream input(source, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    const auto data = bytes.find("data");
    require(data != std::string::npos && data + 8 < bytes.size(), "fixture has PCM data");
    const auto length = bytes.size() - data - 8 - missingBytes;
    bytes.resize(data + 8 + length);
    if (length & 1) bytes += '\0';
    for (int i = 0; i < 4; ++i) {
      bytes[data + 4 + i] = static_cast<char>(length >> (8 * i));
      bytes[4 + i] = static_cast<char>((bytes.size() - 8) >> (8 * i));
    }
    const auto malformed = temp.root / "incomplete-frame.wav";
    const auto output = temp.root / "incomplete-frame-output";
    temp.write(malformed, bytes);
    require(EncodeAudio(candidate(malformed), selectedPolicy, output, {}, {}).reason.code ==
                StorageReason::ValidationFailed && !std::filesystem::exists(output),
            "complete RIFF bounds and padding cannot hide an incomplete native audio frame");
    std::ifstream retained(malformed, std::ios::binary);
    require(std::string((std::istreambuf_iterator<char>(retained)), {}) == bytes,
            "incomplete PCM frames retain every original byte");
  };
  rejectPartialFrame(temp.root / "16-2.wav", 2, policy); // Even payload: final left channel only.
  rejectPartialFrame(temp.root / "16-1.wav", 1, policy); // Odd payload with a complete RIFF pad.
  for (int bits : {8, 24}) {
    // Older WAV writers omit the terminal pad byte after an odd PCM payload.
    // Start with known PCM and shorten the declared payload by one full sample.
    std::ifstream input(temp.root / (std::to_string(bits) + "-1.wav"), std::ios::binary);
    const int sampleBytes = bits / 8;
    std::string unpadded((std::istreambuf_iterator<char>(input)), {});
    const auto data = unpadded.find("data");
    require(data != std::string::npos && data + 8 + 8192 * sampleBytes == unpadded.size(),
            "fixture has one terminal PCM data chunk");
    auto put32 = [](std::string &bytes, std::size_t offset, std::uint32_t value) {
      for (int i = 0; i < 4; ++i) bytes[offset + i] = static_cast<char>(value >> (8 * i));
    };
    unpadded.resize(unpadded.size() - sampleBytes);
    put32(unpadded, data + 4, 8191 * sampleBytes);
    put32(unpadded, 4, unpadded.size() - 8);
    auto pcm = samples(8192, 1, bits); pcm.pop_back();
    for (bool metadata : {false, true}) for (bool declaredPad : {false, true}) {
      auto bytes = unpadded;
      if (metadata) {
        bytes.insert(12, std::string("Xfoo\x02\0\0\0ok", 10));
        put32(bytes, 4, bytes.size() - 8);
      }
      // Some writers count the omitted alignment byte in the outer RIFF size.
      // The declared data payload still ends exactly at the physical EOF.
      if (declaredPad) put32(bytes, 4, bytes.size() - 8 + 1);
      const auto name = std::to_string(bits) + (metadata ? "-unpadded-metadata" : "-unpadded") +
          (declaredPad ? "-declared-pad" : "");
      const auto source = temp.root / (std::string(name) + ".wav");
      const auto output = temp.root / (std::string(name) + ".flac");
      temp.write(source.filename(), bytes);
      require(EncodeAudio(candidate(source), policy, output, {}, {}).outcome == FileOutcome::Converted,
              "complete terminal odd PCM data does not require a non-audio pad byte");
      require(readAudio(output, 1, 8191) == pcm, "unpadded WAV preserves every sample including the last");
      if (metadata) verifyWaveMetadata(bytes, output);
    }
    // Missing a declared audio byte is truncation with either outer RIFF size.
    auto truncated = unpadded; truncated.pop_back(); put32(truncated, 4, truncated.size() - 8);
    for (bool declaredPad : {false, true}) {
      put32(truncated, 4, truncated.size() - 8 + (declaredPad ? 1 : 0));
      temp.write("unpadded-truncated.wav", truncated);
      require(EncodeAudio(candidate(temp.root / "unpadded-truncated.wav"), policy,
                        temp.root / "unpadded-truncated.flac", {}, {}).reason.code == StorageReason::ValidationFailed,
            "terminal padding tolerance never accepts missing declared PCM bytes");
    }
    if (bits == 24) {
      auto partialFrame = unpadded; partialFrame.resize(partialFrame.size() - 2);
      put32(partialFrame, data + 4, 8191 * sampleBytes - 2);
      put32(partialFrame, 4, partialFrame.size() - 8);
      temp.write("partial-frame.wav", partialFrame);
      require(EncodeAudio(candidate(temp.root / "partial-frame.wav"), policy,
                          temp.root / "partial-frame.flac", {}, {}).reason.code == StorageReason::ValidationFailed,
              "an odd payload ending inside a PCM frame is not mistaken for missing padding");
    }
    // The exception cannot cover ancillary payloads or padding before another chunk.
    for (bool ancillary : {false, true}) {
      auto bytes = unpadded;
      if (ancillary) bytes += std::string("\0Xfoo\x01\0\0\0x", 10);
      else bytes += std::string("JUNK\0\0\0\0", 8);
      put32(bytes, 4, bytes.size() - 8);
      temp.write("unpadded-invalid.wav", bytes);
      require(EncodeAudio(candidate(temp.root / "unpadded-invalid.wav"), policy,
                          temp.root / "unpadded-invalid.flac", {}, {}).reason.code == StorageReason::ValidationFailed,
              "padding remains mandatory between chunks and for ancillary metadata");
    }
  }
  for (int subtype : {SF_FORMAT_FLOAT, SF_FORMAT_PCM_32}) {
    auto source = temp.root / "unsupported.wav";
    writeAudio(source, SF_FORMAT_WAV | subtype, 1, samples(8192, 1, 24));
    require(EncodeAudio(candidate(source), policy, temp.root / "unsupported.flac", {}, {}).reason.code ==
                StorageReason::UnsupportedFormat, "lossless mode never quantizes float or 32-bit PCM");
    if (subtype == SF_FORMAT_PCM_32) {
      StoragePolicy lossy; lossy.audio = AudioAction::LossyVorbis;
      rejectPartialFrame(source, 2, lossy);
    }
  }
  const auto tagged = temp.root / "tagged.wav";
  const auto taggedOutput = temp.root / "tagged.flac";
  const auto taggedPcm = samples(8192, 1, 16);
  writeAudio(tagged, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, taggedPcm, false, false, true);
  require(EncodeAudio(candidate(tagged), policy, taggedOutput, {}, {}).outcome == FileOutcome::Converted,
          "descriptive RIFF INFO metadata permits lossless compression");
  require(readAudio(taggedOutput, 1, 8192) == taggedPcm, "tagged WAV preserves exact samples");
  {
    std::ifstream input(tagged, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(input)), {});
    // Invalid first comment used an uninitialized pointer; invalid subsequent
    // comments reused the previous comment's pointer and double-freed on close.
    for (const auto &field : {std::string("Chart keysound"), std::string("Wave editor")})
        for (const auto &invalid : {std::string("\x82"), std::string("\xef\xbf\xbe"), std::string("\xef\xbf\xbf")}) {
      auto bytes = original;
      const auto offset = bytes.find(field);
      require(offset != std::string::npos, "legacy tag fixture contains descriptive text");
      // Legacy code-page bytes and the two noncharacters libFLAC rejects.
      bytes.replace(offset, invalid.size(), invalid);
      const auto source = temp.root / "legacy-tags.wav", output = temp.root / "legacy-tags.flac";
      temp.write(source.filename(), bytes);
      require(EncodeAudio(candidate(source), policy, output, {}, {}).outcome == FileOutcome::Converted,
              "invalid descriptive text does not crash FLAC finalization or reject playable audio");
      require(readAudio(output, 1, 8192) == taggedPcm, "legacy descriptive tags retain exact PCM");
      verifyWaveMetadata(bytes, output);
      std::filesystem::remove(output);
    }
    auto bytes = original;
    const std::string unicodeTitle = "키音🎵....";
    bytes.replace(bytes.find("Chart keysound"), unicodeTitle.size(), unicodeTitle);
    temp.write("unicode-tags.wav", bytes);
    const auto output = temp.root / "unicode-tags.flac";
    require(EncodeAudio(candidate(temp.root / "unicode-tags.wav"), policy, output, {}, {}).outcome ==
                FileOutcome::Converted, "valid multibyte descriptive tags remain supported");
    SF_INFO info{};
    auto file = sf_open(output.string().c_str(), SFM_READ, &info);
    require(file && sf_get_string(file, SF_STR_TITLE) && sf_get_string(file, SF_STR_TITLE) == unicodeTitle,
            "valid UTF-8 is copied into native FLAC comments");
    sf_close(file);
  }
  {
    std::ifstream input(tagged, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(input)), {});
    for (const int adjustment : {-100, 100}) {
      auto bytes = original;
      const auto declaredSize = static_cast<std::uint32_t>(bytes.size() - 8 + adjustment);
      for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<char>(declaredSize >> (8 * i));
      const auto source = temp.root / ("stale-riff-" + std::to_string(adjustment) + ".wav");
      const auto output = source.parent_path() / (source.stem().string() + ".flac");
      temp.write(source.filename(), bytes);
      require(EncodeAudio(candidate(source), policy, output, {}, {}).outcome == FileOutcome::Converted,
              "stale outer RIFF sizes permit complete physically bounded WAV payloads");
      require(readAudio(output, 1, 8192) == taggedPcm, "stale RIFF length never trims decoded samples");
      verifyWaveMetadata(bytes, output);
    }
  }
  {
    std::ifstream input(temp.root / "16-1.wav", std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(input)), {});
    StoragePolicy lossy; lossy.audio = AudioAction::LossyVorbis;
    for (const auto &second : {std::string("First title"), std::string("Second title"),
                               std::string("Title\0hidden", 12)}) {
      auto bytes = original;
      const auto appendSize = [](std::string &target, std::uint32_t size) {
        for (int i = 0; i < 4; ++i) target.push_back(static_cast<char>(size >> (8 * i)));
      };
      std::string info = "INFO";
      for (const auto &title : {std::string("First title"), second}) {
        info += "INAM"; appendSize(info, title.size() + 1);
        info += title; info += '\0';
        if ((title.size() + 1) & 1) info += '\0';
      }
      bytes += "LIST"; appendSize(bytes, info.size()); bytes += info;
      for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<char>((bytes.size() - 8) >> (8 * i));
      const auto source = temp.root / "duplicate-info.wav", output = temp.root / "duplicate-info.ogg";
      temp.write(source.filename(), bytes);
      require(EncodeAudio(candidate(source), lossy, output, {}, {}).outcome == FileOutcome::Converted,
              "duplicate or imperfect descriptive INFO text may collapse during Vorbis conversion");
      readAudio(output, 1, 8192);
      std::filesystem::remove(output);
    }
  }
  SF_INFO taggedInfo{};
  auto taggedSource = sf_open(tagged.string().c_str(), SFM_READ, &taggedInfo);
  auto taggedFile = sf_open(taggedOutput.string().c_str(), SFM_READ, &taggedInfo);
  require(taggedSource && taggedFile && sf_get_string(taggedFile, SF_STR_TITLE) &&
      std::string(sf_get_string(taggedFile, SF_STR_TITLE)) == "Chart keysound" &&
      sf_get_string(taggedFile, SF_STR_SOFTWARE) &&
      std::string(sf_get_string(taggedFile, SF_STR_SOFTWARE)) == sf_get_string(taggedSource, SF_STR_SOFTWARE),
      "descriptive tags survive encoding");
  sf_close(taggedSource);
  sf_close(taggedFile);
  {
    std::ifstream input(tagged, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    const auto field = bytes.find("ISFT");
    require(field != std::string::npos, "tagged fixture contains an INFO software field");
    bytes.replace(field, 4, "ZZZZ");
    temp.write("unknown-info.wav", bytes);
    const auto unknown = temp.root / "unknown-info.wav";
    const auto output = temp.root / "unknown-info.flac";
    require(EncodeAudio(candidate(unknown), policy, output, {}, {}).outcome == FileOutcome::Converted,
            "unknown INFO fields permit lossless conversion without interpretation");
    verifyWaveMetadata(bytes, output);
    require(readAudio(output, 1, 8192) == taggedPcm, "unknown INFO fields retain exact PCM");
    StoragePolicy lossy; lossy.audio = AudioAction::LossyVorbis;
    require(EncodeAudio(candidate(unknown), lossy, temp.root / "unknown-info.ogg", {}, {}).reason.code ==
        StorageReason::UnsupportedFormat, "unknown INFO fields remain protected in lossy conversion");
  }
  // Binary broadcast extensions include fields unavailable through libsndfile,
  // unknown versions and a nonzero odd-length RIFF pad byte.
  std::ifstream broadcastInput(tagged, std::ios::binary);
  std::string broadcastBytes((std::istreambuf_iterator<char>(broadcastInput)), {});
  std::string broadcastChunk("bext", 4);
  const std::uint32_t broadcastLength = 65537;
  for (int i = 0; i < 4; ++i) broadcastChunk.push_back(static_cast<char>(broadcastLength >> (8 * i)));
  for (std::uint32_t i = 0; i < broadcastLength; ++i)
    broadcastChunk.push_back(static_cast<char>((i * 37) & 255));
  broadcastChunk[8 + 346] = '\x7f'; broadcastChunk[8 + 347] = '\x01';
  broadcastChunk.push_back('P');
  broadcastBytes.insert(12, broadcastChunk);
  for (int i = 0; i < 4; ++i) broadcastBytes[4 + i] = static_cast<char>((broadcastBytes.size() - 8) >> (8 * i));
  temp.write("broadcast.wav", broadcastBytes);
  const auto broadcast = temp.root / "broadcast.wav";
  const auto broadcastOutput = temp.root / "broadcast.flac";
  require(EncodeAudio(candidate(broadcast), policy, broadcastOutput, {}, {}).outcome == FileOutcome::Converted,
          "broadcast WAV converts without discarding bext");
  require(readAudio(broadcastOutput, 1, 8192) == taggedPcm, "broadcast WAV retains exact PCM");
  verifyWaveMetadata(broadcastBytes, broadcastOutput);
  const auto cancelledMetadata = temp.root / "cancelled-broadcast.flac";
  require(EncodeAudio(candidate(broadcast), policy, cancelledMetadata, [&] {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(cancelledMetadata, error);
    return error || bytes < broadcastChunk.size();
  }, {}).outcome == FileOutcome::Cancelled && !std::filesystem::exists(cancelledMetadata),
          "cancel during foreign metadata insertion removes the temporary output");
  const auto damagedMetadata = temp.root / "damaged-broadcast.flac";
  require(EncodeAudio(candidate(broadcast), policy, damagedMetadata, {}, [&](const StorageProgress &value) {
    if (value.phase != StoragePhase::Verify) return;
    std::ifstream input(damagedMetadata, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    const auto offset = bytes.find("riffbext");
    require(offset != std::string::npos, "encoded broadcast metadata can be located");
    std::fstream output(damagedMetadata, std::ios::in | std::ios::out | std::ios::binary);
    output.seekp(offset + 12); output.put('!');
  }).reason.code == StorageReason::ValidationFailed && !std::filesystem::exists(damagedMetadata),
          "metadata decode-back detects corruption independently of unchanged PCM");
  auto loop = temp.root / "loop.wav";
  writeAudio(loop, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, samples(8192, 1, 16), true);
  const auto loopOutput = temp.root / "loop.flac";
  require(EncodeAudio(candidate(loop), policy, loopOutput, {}, {}).outcome == FileOutcome::Converted,
          "WAV sampler metadata permits lossless conversion");
  require(readAudio(loopOutput, 1, 8192) == taggedPcm, "sampler WAV retains exact PCM");
  std::ifstream loopInput(loop, std::ios::binary);
  const std::string loopBytes((std::istreambuf_iterator<char>(loopInput)), {});
  verifyWaveMetadata(loopBytes, loopOutput);
  StoragePolicy vorbis; vorbis.audio = AudioAction::LossyVorbis;
  require(EncodeAudio(candidate(loop), vorbis, temp.root / "loop.ogg", {}, {}).reason.code ==
              StorageReason::UnsupportedFormat, "Vorbis keeps sampler metadata in the original WAV");
  // ID3v2.4 with a title frame and an odd chunk length. Keep the original ID,
  // binary contents and nonzero pad byte, including metadata after PCM data.
  const std::string id3Payload("ID3\x04\0\0\0\0\0\x0fTIT2\0\0\0\x05\0\0\x03test", 25);
  for (bool upper : {false, true}) {
    const std::string id = upper ? "ID3 " : "id3 ";
    std::string chunk = id;
    chunk.append("\x19\0\0\0", 4); chunk += id3Payload; chunk += 'P';
    // Exercise the ID3 trigger by itself, then both triggers in one source.
    auto bytes = upper ? loopBytes : std::string{};
    if (!upper) {
      std::ifstream input(tagged, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(input), {});
    }
    if (upper) bytes.insert(12, chunk); else bytes += chunk;
    for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<char>((bytes.size() - 8) >> (8 * i));
    const auto name = upper ? "sampler-ID3" : "id3";
    const auto source = temp.root / (std::string(name) + ".wav");
    const auto output = temp.root / (std::string(name) + ".flac");
    temp.write(source.filename().string(), bytes);
    require(EncodeAudio(candidate(source), policy, output, {}, {}).outcome == FileOutcome::Converted,
            "WAV ID3 chunks permit lossless conversion");
    verifyWaveMetadata(bytes, output);
    require(readAudio(output, 1, 8192) == taggedPcm, "ID3 WAV retains exact PCM");
    const auto lossy = temp.root / (std::string(name) + ".ogg");
    require(EncodeAudio(candidate(source), vorbis, lossy, {}, {}).reason.code == StorageReason::UnsupportedFormat &&
                !std::filesystem::exists(lossy), "Vorbis does not discard WAV ID3 metadata");
  }
  {
    std::ifstream input(temp.root / "16-1.wav", std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    // Common ancillary chunks plus duplicate and unknown vendor chunks. Their
    // payloads are opaque to the transcoder; no whitelist should be necessary.
    for (const auto &[id, payload] : std::vector<std::pair<std::string, std::string>>{
        {"cue ", std::string(4, '\0')}, {"inst", std::string(7, '\0')},
        {"LIST", std::string("adtllabl\x06\0\0\0\x01\0\0\0" "A\0", 18)},
        {"iXML", "<BWFXML><PROJECT>Session</PROJECT></BWFXML>"},
        {"axml", "<metadata>Copyright</metadata>"},
        {"cart", "0101" + std::string(2044, '\0')},
        {"DISP", std::string("\x01\0\0\0" "Sample\0", 11)},
        {"PEAK", std::string("\x01\0\0\0", 4) + std::string(12, '\0')},
        {"acid", std::string(24, '\0')}, {"iXML", "<extra/>"},
        {"Xfoo", std::string("\0\xff!", 3)}, {"Xnil", ""}}) {
      bytes += id;
      for (int i = 0; i < 4; ++i) bytes += static_cast<char>(payload.size() >> (8 * i));
      bytes += payload;
      if (payload.size() & 1) bytes += 'P';
    }
    for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<char>((bytes.size() - 8) >> (8 * i));
    temp.write("ancillary.wav", bytes);
    const auto source = temp.root / "ancillary.wav", output = temp.root / "ancillary.flac";
    require(EncodeAudio(candidate(source), policy, output, {}, {}).outcome == FileOutcome::Converted,
            "common and unknown RIFF chunks permit lossless conversion");
    verifyWaveMetadata(bytes, output);
    require(readAudio(output, 1, 8192) == taggedPcm, "ancillary chunks preserve exact PCM");
    // An out-of-bounds vendor chunk must still fail even though its ID is allowed.
    bytes += "Xbad"; bytes.append("\xff\xff\xff\x7f", 4);
    for (int i = 0; i < 4; ++i) bytes[4 + i] = static_cast<char>((bytes.size() - 8) >> (8 * i));
    temp.write("truncated-chunk.wav", bytes);
    const auto invalidOutput = temp.root / "truncated-chunk.flac";
    require(EncodeAudio(candidate(temp.root / "truncated-chunk.wav"), policy, invalidOutput, {}, {}).reason.code ==
        StorageReason::ValidationFailed && !std::filesystem::exists(invalidOutput),
        "opaque metadata still requires complete RIFF chunk bounds");
  }
  auto rear = temp.root / "rear.wav";
  writeAudio(rear, SF_FORMAT_WAVEX | SF_FORMAT_PCM_16, 2, samples(8192, 2, 16), false, true);
  require(EncodeAudio(candidate(rear), policy, temp.root / "rear.flac", {}, {}).reason.code ==
              StorageReason::UnsupportedFormat, "two rear channels are not silently relabeled as front stereo");
  auto tiny = temp.root / "tiny.wav";
  writeAudio(tiny, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, {1 << 28});
  require(EncodeAudio(candidate(tiny), policy, temp.root / "tiny.flac", {}, {}).outcome ==
              FileOutcome::Converted, "lossless policy keeps larger FLAC output");
  require(std::filesystem::file_size(temp.root / "tiny.flac") > std::filesystem::file_size(tiny) &&
      readAudio(temp.root / "tiny.flac", 1, 1) == std::vector<int>{1 << 28},
      "larger FLAC remains sample-exact");
  // Set the source's harmless RIFF padding to the encoder's exact container
  // size. Lossless format preference applies to equality as well.
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
  require(EncodeAudio(candidate(tiny), policy, temp.root / "equal.flac", {}, {}).outcome ==
              FileOutcome::Converted && std::filesystem::file_size(temp.root / "equal.flac") == equalSize,
          "lossless policy keeps equal-size FLAC output");

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
  require(EncodeAudio(candidate(broadcast), policy, temp.root / "broadcast.ogg", {}, {}).reason.code ==
      StorageReason::UnsupportedFormat, "Vorbis does not silently discard broadcast metadata");
  require(EncodeAudio(candidate(temp.root / "tiny.flac"), policy, temp.root / "tiny.ogg", {}, {}).reason.code ==
      StorageReason::NotSmaller && !std::filesystem::exists(temp.root / "tiny.ogg"),
      "larger lossy output is still discarded");
  auto truncatedFlac = temp.root / "truncated.flac";
  std::filesystem::copy_file(temp.root / "24-1.flac", truncatedFlac);
  std::filesystem::resize_file(truncatedFlac, std::filesystem::file_size(truncatedFlac) - 30);
  require(EncodeAudio(candidate(truncatedFlac), policy, temp.root / "truncated.ogg", {}, {}).reason.code ==
              StorageReason::ValidationFailed, "truncated FLAC must not be silently shortened");
  {
    std::ifstream input(temp.root / "24-1.flac", std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(input)), {});
    const auto taggedFlac = temp.root / "comments.flac", output = temp.root / "comments.ogg";
    temp.write(taggedFlac, withFlacComments(original, {"ARTIST=First artist", "ALBUM=Chart album"}));
    require(EncodeAudio(candidate(taggedFlac), policy, output, {}, {}).outcome == FileOutcome::Converted,
            "distinct descriptive FLAC comments still permit Vorbis conversion");
    std::filesystem::remove(output);
    for (const auto &comments : std::vector<std::vector<std::string>>{
             {"ARTIST=First artist", "ARTIST=Second artist"},
             {"ARTIST=First artist", "aRtIsT=Second artist"},
             {"SOFTWARE=First encoder", "eNcOdEr=Second encoder"}}) {
      const auto bytes = withFlacComments(original, comments);
      temp.write(taggedFlac, bytes);
      const auto result = EncodeAudio(candidate(taggedFlac), policy, output, {}, {});
      require(result.outcome == FileOutcome::Converted,
              "duplicate descriptive FLAC fields and encoder aliases may collapse during conversion");
      readAudio(output, 1, 8192);
      std::filesystem::remove(output);
    }
    // Even a length-prefixed string outside the declared count is not a comment.
    const std::string uncountedLoop = std::string("\x0c\0\0\0", 4) + "LOOPSTART=10";
    for (const auto &trailing : {std::string(8, '\0'), uncountedLoop}) {
      const auto bytes = withFlacComments(original, {"ARTIST=First artist", "ALBUM=Chart album"}, trailing);
      temp.write(taggedFlac, bytes);
      const auto result = EncodeAudio(candidate(taggedFlac), policy, output, {}, {});
      require(result.outcome == FileOutcome::Failed && result.reason.code == StorageReason::ValidationFailed &&
                  !std::filesystem::exists(output),
              "FLAC decoder rejects unused comment bytes before conversion can replace the source");
      std::ifstream retained(taggedFlac, std::ios::binary);
      require(std::string((std::istreambuf_iterator<char>(retained)), {}) == bytes,
              "decoder-rejected comment blocks retain all original metadata and audio bytes");
    }
    temp.write(taggedFlac, withFlacComments(original, {"TITLE=Keysound", "LOOPSTART=10", "LOOPEND=100"}));
    require(EncodeAudio(candidate(taggedFlac), policy, output, {}, {}).reason.code == StorageReason::UnsupportedFormat &&
                !std::filesystem::exists(output), "descriptive tolerance never discards FLAC playback comments");
  }
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

void decodedDisplayTransformTests() {
  AVCodecParameters params{};
  auto decoded = frame();
  validateVideoFramePresentation(params, *decoded);
  auto *side = av_frame_new_side_data(decoded.get(), AV_FRAME_DATA_DISPLAYMATRIX, 9 * sizeof(std::int32_t));
  require(side != nullptr, "allocate decoded display transform");
  auto *matrix = reinterpret_cast<std::int32_t *>(side->data);
  const auto rejected = [&] {
    bool rejected = false;
    try { validateVideoFramePresentation(params, *decoded); }
    catch (const Failure &failure) {
      rejected = failure.reason == StorageReason::UnsupportedFormat && failure.outcome == FileOutcome::Skipped;
    }
    return rejected;
  };
  av_display_rotation_set(matrix, 90);
  require(rejected(), "frame-only display transforms preserve the original video");
  std::array<std::int32_t, 9> streamMatrix{};
  av_display_rotation_set(streamMatrix.data(), 90);
  AVPacketSideData streamSide{};
  streamSide.type = AV_PKT_DATA_DISPLAYMATRIX;
  streamSide.data = reinterpret_cast<std::uint8_t *>(streamMatrix.data());
  streamSide.size = sizeof(streamMatrix);
  params.coded_side_data = &streamSide; params.nb_coded_side_data = 1;
  validateVideoFramePresentation(params, *decoded);
  av_display_rotation_set(matrix, 0);
  require(rejected(), "identity frame transform cannot reset the copied stream orientation");
  av_display_rotation_set(matrix, 90);
  av_display_matrix_flip(matrix, 1, 0);
  require(rejected(), "frame reflection cannot silently disagree with the stream transform");
  av_display_rotation_set(matrix, 90);
  --side->size;
  require(rejected(), "truncated frame display matrices preserve the original video");
  for (const auto type : {AV_FRAME_DATA_A53_CC, AV_FRAME_DATA_AFD, AV_FRAME_DATA_PANSCAN,
                          AV_FRAME_DATA_SPHERICAL, AV_FRAME_DATA_ICC_PROFILE, AV_FRAME_DATA_DYNAMIC_HDR_PLUS,
                          AV_FRAME_DATA_FILM_GRAIN_PARAMS, AV_FRAME_DATA_DOVI_METADATA}) {
    AVCodecParameters plain{};
    auto presentation = frame();
    require(av_frame_new_side_data(presentation.get(), type, 32), "allocate playback frame metadata");
    bool skipped = false;
    try { validateVideoFramePresentation(plain, *presentation); }
    catch (const Failure &failure) { skipped = failure.outcome == FileOutcome::Skipped; }
    require(skipped, "unsupported decoded playback metadata keeps the original video");
  }
  AVCodecParameters plain{};
  auto descriptive = frame();
  require(av_frame_new_side_data(descriptive.get(), AV_FRAME_DATA_SEI_UNREGISTERED, 32),
          "allocate harmless encoder metadata");
  validateVideoFramePresentation(plain, *descriptive);
  auto uncropped = frame(); uncropped->width = 160; uncropped->height = 120;
  auto panData = av_frame_new_side_data(uncropped.get(), AV_FRAME_DATA_PANSCAN, sizeof(AVPanScan));
  require(panData, "allocate decoded default pan-scan");
  std::memset(panData->data, 0, panData->size);
  validateVideoFramePresentation(plain, *uncropped);
  auto &pan = *reinterpret_cast<AVPanScan *>(panData->data);
  pan.width = 160 * 16; pan.height = 120 * 16;
  validateVideoFramePresentation(plain, *uncropped);
  for (bool offset : {false, true}) {
    pan.width = (offset ? 160 : 120) * 16; pan.position[0][0] = offset ? 16 : 0;
    bool skipped = false;
    try { validateVideoFramePresentation(plain, *uncropped); }
    catch (const Failure &failure) { skipped = failure.outcome == FileOutcome::Skipped; }
    require(skipped, "actual pan-scan crops and offsets preserve the original video");
  }
}

void decodedColorTests() {
  AVCodecParameters params{};
  params.color_range = AVCOL_RANGE_MPEG; params.color_space = AVCOL_SPC_BT470BG;
  params.color_primaries = AVCOL_PRI_BT470BG; params.color_trc = AVCOL_TRC_GAMMA28;
  auto decoded = frame();
  decoded->color_range = AVCOL_RANGE_JPEG; decoded->colorspace = AVCOL_SPC_BT709;
  decoded->color_primaries = AVCOL_PRI_BT709; decoded->color_trc = AVCOL_TRC_BT709;
  Scan observed;
  observeVideoColor(observed, params, *decoded);
  require(observed.colorRange == AVCOL_RANGE_JPEG && observed.colorSpace == AVCOL_SPC_BT709 &&
              observed.colorPrimaries == AVCOL_PRI_BT709 && observed.colorTransfer == AVCOL_TRC_BT709,
          "constant decoded color metadata overrides stream defaults");
  observed.video.frames = 1;
  observeVideoColor(observed, params, *decoded);
  for (int field = 0; field < 4; ++field) {
    auto changed = frame();
    changed->color_range = decoded->color_range; changed->colorspace = decoded->colorspace;
    changed->color_primaries = decoded->color_primaries; changed->color_trc = decoded->color_trc;
    if (field == 0) changed->color_range = AVCOL_RANGE_MPEG;
    if (field == 1) changed->colorspace = AVCOL_SPC_BT470BG;
    if (field == 2) changed->color_primaries = AVCOL_PRI_BT470BG;
    if (field == 3) changed->color_trc = AVCOL_TRC_GAMMA28;
    bool rejected = false;
    try { observeVideoColor(observed, params, *changed); }
    catch (const Failure &failure) { rejected = failure.reason == StorageReason::UnsupportedFormat; }
    require(rejected, "each changing decoded color property preserves the source");
  }
  Scan fallback;
  auto unspecified = frame();
  observeVideoColor(fallback, params, *unspecified);
  require(fallback.colorRange == AVCOL_RANGE_MPEG && fallback.colorSpace == AVCOL_SPC_BT470BG &&
              fallback.colorPrimaries == AVCOL_PRI_BT470BG && fallback.colorTransfer == AVCOL_TRC_GAMMA28,
          "unspecified decoded colors use stream metadata");
  Scan output;
  output.videoDisposition = AV_DISPOSITION_DEFAULT;
  observeVideoColor(output, params, *decoded);
  observed.video = {}; observed.video.timeBase = output.video.timeBase = {1, 25};
  observed.video.add(0, 1); output.video.add(0, 1);
  output.description.h264 = true; output.description.width = output.description.height = 16;
  VideoCompressionPlan plan; plan.width = plan.height = 16; plan.frameRate = {25, 1};
  verify(observed, output, plan, {1});
  for (int field = 0; field < 5; ++field) {
    if (field == 0) output.colorRange = AVCOL_RANGE_UNSPECIFIED;
    if (field == 1) output.colorSpace = AVCOL_SPC_UNSPECIFIED;
    if (field == 2) output.colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    if (field == 3) output.colorTransfer = AVCOL_TRC_UNSPECIFIED;
    if (field == 4) output.chromaLocation = AVCHROMA_LOC_LEFT;
    bool rejected = false;
    try { verify(observed, output, plan, {1}); }
    catch (const Failure &failure) { rejected = failure.reason == StorageReason::ValidationFailed; }
    require(rejected, "decode-back rejects lost color tags even when geometry and timing match");
    output.colorRange = observed.colorRange; output.colorSpace = observed.colorSpace;
    output.colorPrimaries = observed.colorPrimaries; output.colorTransfer = observed.colorTransfer;
    output.chromaLocation = observed.chromaLocation;
  }
  for (const auto matrix : {AVCOL_SPC_BT2020_CL, AVCOL_SPC_YCGCO, AVCOL_SPC_RGB}) {
    Scan unsupportedColor;
    decoded->colorspace = matrix;
    bool rejected = false;
    try { observeVideoColor(unsupportedColor, params, *decoded); }
    catch (const Failure &failure) { rejected = failure.reason == StorageReason::UnsupportedFormat; }
    require(rejected, "unsupported matrices cannot silently use swscale default coefficients");
  }
}
void chromaLocationTests() {
  storage_test::TempDirectory temp;
  const auto checkLocation = [](const std::filesystem::path &path, AVChromaLocation expected) {
    Input input(path);
    bool found = false;
    decode(input, {}, [&](int index, const AVFrame &decoded) {
      if (index != input.video) return;
      found = true;
      require(decoded.chroma_location == expected, "encoded video retains the chroma sample location");
    }, [](int, const AVPacket &) {});
    require(found, "chroma fixture contains decoded video");
  };
  for (bool scaled : {false, true}) {
    storage_test::VideoFixture fixture;
    fixture.lossless = fixture.millisecondContainer = scaled;
    if (scaled) {
      fixture.width = 192; fixture.height = 128;
      fixture.sampleAspectRatio = {4, 1};
      fixture.chromaLocation = AVCHROMA_LOC_TOPLEFT;
    }
    const auto source = temp.root / (scaled ? "top-left.mp4" : "center.mpg");
    const auto output = temp.root / (scaled ? "scaled.mp4" : "converted.mp4");
    storage_test::makeVideo(source, fixture);
    const auto expected = scaled ? AVCHROMA_LOC_TOPLEFT : AVCHROMA_LOC_CENTER;
    checkLocation(source, expected);
    StoragePolicy policy; policy.bga = BgaAction::CompressH264;
    if (scaled) policy.resolution = ResolutionCap::Low;
    const auto result = EncodeBga(candidate(source), policy, output, {}, {});
    require(result.outcome == FileOutcome::Converted, "ordinary center-sited and scaled top-left video can be compressed");
    checkLocation(output, expected);
    if (scaled) require(scan(output, {}).description.width < fixture.width, "chroma regression exercises spatial scaling");
  }
  AVCodecParameters params{};
  params.color_space = AVCOL_SPC_UNSPECIFIED;
  params.color_primaries = AVCOL_PRI_UNSPECIFIED; params.color_trc = AVCOL_TRC_UNSPECIFIED;
  auto decoded = frame(); decoded->format = AV_PIX_FMT_YUV420P;
  decoded->chroma_location = AVCHROMA_LOC_CENTER;
  Scan observed; observeVideoColor(observed, params, *decoded); observed.video.frames = 1;
  decoded->chroma_location = AVCHROMA_LOC_LEFT;
  bool rejected = false;
  try { observeVideoColor(observed, params, *decoded); }
  catch (const Failure &failure) { rejected = failure.reason == StorageReason::UnsupportedFormat; }
  require(rejected, "changing decoded chroma sample locations retain the original video");
  decoded->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
  params.chroma_location = AVCHROMA_LOC_TOP;
  Scan fallback; observeVideoColor(fallback, params, *decoded);
  require(fallback.chromaLocation == AVCHROMA_LOC_TOP, "unspecified decoded chroma uses the stream location");
  params.chroma_location = AVCHROMA_LOC_UNSPECIFIED;
  observeVideoColor(fallback, params, *decoded);
  require(fallback.chromaLocation == AVCHROMA_LOC_CENTER, "absent chroma tags retain swscale's center assumption");
  decoded->format = AV_PIX_FMT_YUV444P;
  decoded->chroma_location = AVCHROMA_LOC_TOPLEFT;
  Scan fullChroma; observeVideoColor(fullChroma, params, *decoded); fullChroma.video.frames = 1;
  decoded->chroma_location = AVCHROMA_LOC_BOTTOM;
  observeVideoColor(fullChroma, params, *decoded);
  require(fullChroma.chromaLocation == AVCHROMA_LOC_CENTER, "full-resolution chroma is independent of advisory siting tags");
  decoded->format = AV_PIX_FMT_YUV422P;
  decoded->chroma_location = AVCHROMA_LOC_LEFT;
  Scan horizontalChroma; observeVideoColor(horizontalChroma, params, *decoded); horizontalChroma.video.frames = 1;
  for (const auto location : {AVCHROMA_LOC_TOPLEFT, AVCHROMA_LOC_BOTTOMLEFT}) {
    decoded->chroma_location = location;
    observeVideoColor(horizontalChroma, params, *decoded);
    require(horizontalChroma.chromaLocation == AVCHROMA_LOC_LEFT, "4:2:2 ignores changes on its full-rate vertical axis");
  }
  decoded->chroma_location = AVCHROMA_LOC_CENTER;
  rejected = false;
  try { observeVideoColor(horizontalChroma, params, *decoded); }
  catch (const Failure &failure) { rejected = failure.reason == StorageReason::UnsupportedFormat; }
  require(rejected, "4:2:2 still retains the source when its horizontal chroma position changes");

  // Check the actual scaler grid, not only the output tag: matching tags can
  // still hide a half-pixel shift if swscale used its default input positions.
  ScaleBuffer scaler;
  AVCodecContext destination{};
  destination.width = destination.height = 32; destination.pix_fmt = AV_PIX_FMT_YUV420P;
  decoded->width = decoded->height = 64;
  struct PositionCase { AVPixelFormat format; AVChromaLocation location; int x, y, outX, outY; };
  for (const auto &test : {PositionCase{AV_PIX_FMT_YUV420P, AVCHROMA_LOC_CENTER, 128, 128, 128, 128},
                          PositionCase{AV_PIX_FMT_YUV420P, AVCHROMA_LOC_LEFT, 0, 128, 0, 128},
                          PositionCase{AV_PIX_FMT_YUV420P, AVCHROMA_LOC_TOPLEFT, 0, 0, 0, 0},
                          PositionCase{AV_PIX_FMT_YUV422P, AVCHROMA_LOC_CENTER, 128, 0, 128, 128},
                          PositionCase{AV_PIX_FMT_YUV444P, AVCHROMA_LOC_BOTTOMLEFT, 0, 0, 0, 256},
                          PositionCase{AV_PIX_FMT_YUV411P, AVCHROMA_LOC_CENTER, 384, 0, 128, 128}}) {
    decoded->format = test.format; destination.chroma_sample_location = test.location;
    scaler.configure(*decoded, destination, test.location);
    const std::pair<const char *, int> positions[] = {
        {"src_h_chr_pos", test.x}, {"src_v_chr_pos", test.y},
        {"dst_h_chr_pos", test.outX}, {"dst_v_chr_pos", test.outY}};
    for (const auto &[name, expected] : positions) {
      std::int64_t position = -1;
      require(av_opt_get_int(scaler.value, name, 0, &position) >= 0 && position == expected,
              "spatial scaling uses native source and destination chroma sample grids");
    }
  }
}
void decodedAudioFormatTests() {
  auto audio = frame(); audio->sample_rate = 48000; audio->format = AV_SAMPLE_FMT_S16;
  av_channel_layout_default(&audio->ch_layout, 2);
  validateAudioFrame(*audio, *audio);
  for (int field = 0; field < 4; ++field) {
    auto changed = frame(); changed->sample_rate = 48000; changed->format = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&changed->ch_layout, 2);
    if (field == 0) changed->sample_rate = 44100;
    if (field == 1) changed->format = AV_SAMPLE_FMT_S16P;
    if (field == 2) changed->ch_layout.u.mask = AV_CH_LAYOUT_STEREO_DOWNMIX;
    if (field == 3) { changed->ch_layout.order = AV_CHANNEL_ORDER_UNSPEC; changed->ch_layout.u.mask = 0; }
    bool rejected = false;
    try { validateAudioFrame(*audio, *changed); }
    catch (const Failure &failure) { rejected = failure.reason == StorageReason::UnsupportedFormat; }
    require(rejected, "decoded audio changes in rate, packing, channel identity or layout order preserve source");
  }
}
void decodedAudioPresentationTests() {
  auto audio = frame(); audio->sample_rate = 48000; audio->format = AV_SAMPLE_FMT_S16;
  av_channel_layout_default(&audio->ch_layout, 2);
  const auto rejects = [&] {
    try { validateAudioFrame(*audio, *audio); }
    catch (const Failure &failure) {
      return failure.reason == StorageReason::UnsupportedFormat && failure.outcome == FileOutcome::Skipped;
    }
    return false;
  };
  auto matrix = av_frame_new_side_data(audio.get(), AV_FRAME_DATA_MATRIXENCODING, sizeof(AVMatrixEncoding));
  require(matrix, "allocate decoded audio matrix metadata");
  auto &encoding = *reinterpret_cast<AVMatrixEncoding *>(matrix->data);
  encoding = AV_MATRIX_ENCODING_NONE;
  validateAudioFrame(*audio, *audio);
  encoding = AV_MATRIX_ENCODING_DOLBY;
  require(rejects(), "AAC conversion must retain the source when decoded audio needs Dolby matrix playback");
  av_frame_remove_side_data(audio.get(), AV_FRAME_DATA_MATRIXENCODING);

  auto service = av_frame_new_side_data(audio.get(), AV_FRAME_DATA_AUDIO_SERVICE_TYPE, sizeof(AVAudioServiceType));
  require(service, "allocate decoded audio service metadata");
  auto &serviceType = *reinterpret_cast<AVAudioServiceType *>(service->data);
  serviceType = AV_AUDIO_SERVICE_TYPE_MAIN;
  validateAudioFrame(*audio, *audio);
  serviceType = AV_AUDIO_SERVICE_TYPE_VISUALLY_IMPAIRED;
  require(rejects(), "AAC conversion must retain decoded accessibility audio service semantics");
  validateAudioFrame(*audio, *audio, true, AV_AUDIO_SERVICE_TYPE_VISUALLY_IMPAIRED);
  bool mismatch = false;
  try { validateAudioFrame(*audio, *audio, true, AV_AUDIO_SERVICE_TYPE_MAIN); }
  catch (const Failure &failure) { mismatch = failure.outcome == FileOutcome::Skipped; }
  require(mismatch, "remux cannot replace an external service override with a different codec-native service");
  av_frame_remove_side_data(audio.get(), AV_FRAME_DATA_AUDIO_SERVICE_TYPE);

  auto downmix = av_downmix_info_update_side_data(audio.get());
  require(downmix, "allocate decoded audio downmix metadata");
  *downmix = {};
  validateAudioFrame(*audio, *audio);
  downmix->preferred_downmix_type = AV_DOWNMIX_TYPE_DPLII;
  require(rejects(), "AAC conversion must retain decoded matrix downmix instructions");
  av_frame_remove_side_data(audio.get(), AV_FRAME_DATA_DOWNMIX_INFO);

  require(av_dict_set(&audio->metadata, "chart_editor_note", "Descriptive metadata", 0) >= 0,
          "allocate harmless decoded audio description");
  validateAudioFrame(*audio, *audio);
  require(av_dict_set(&audio->metadata, "replaygain_track_gain", "-12 dB", 0) >= 0,
          "allocate decoded audio playback gain");
  require(rejects(), "AAC conversion must retain decoded playback gain dictionary entries");
}
void encodedAudioPresentationTests() {
  storage_test::TempDirectory temp;
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  for (int kind = 0; kind < 3; ++kind) {
    storage_test::VideoFixture fixture;
    fixture.audio = true; fixture.audioMatrix = kind != 0;
    fixture.audioCodec = kind == 2 ? AV_CODEC_ID_AC3 : AV_CODEC_ID_DTS;
    for (int i = 0; i < 32; ++i) fixture.timestamps.push_back(i);
    const auto source = temp.root / ("matrix-" + std::to_string(kind) + ".mpg");
    const auto output = temp.root / ("matrix-" + std::to_string(kind) + ".mp4");
    storage_test::makeVideo(source, fixture);
    std::ifstream originalFile(source, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(originalFile)), {});
    int matrixFrames = 0;
    const auto inspectMatrix = [&](const std::filesystem::path &path) {
      Input input(path);
      decode(input, {}, [&](int index, const AVFrame &decoded) {
        if (index != input.audio) return;
        const auto matrix = av_frame_get_side_data(&decoded, AV_FRAME_DATA_MATRIXENCODING);
        require(matrix && matrix->size == sizeof(AVMatrixEncoding), "fixture decoder emits audio matrix metadata");
        AVMatrixEncoding encoding; std::memcpy(&encoding, matrix->data, sizeof(encoding));
        require(encoding == (fixture.audioMatrix ? AV_MATRIX_ENCODING_DOLBY : AV_MATRIX_ENCODING_NONE),
                "real decoder exposes the intended default or Dolby matrix playback");
        ++matrixFrames;
      }, [](int, const AVPacket &) {});
    };
    inspectMatrix(source);
    require(matrixFrames > 0, "fixture includes decoded audio matrix frames");
    const auto result = EncodeBga(candidate(source), policy, output, {}, {});
    if (kind == 1) {
      require(result.outcome == FileOutcome::Skipped && result.reason.code == StorageReason::UnsupportedFormat &&
                  !std::filesystem::exists(output), "DTS Dolby matrix audio retains its source instead of losing playback metadata in AAC");
      std::ifstream retained(source, std::ios::binary);
      require(std::string((std::istreambuf_iterator<char>(retained)), {}) == original, "skipped DTS retains original bytes");
    } else {
      require(result.outcome == FileOutcome::Converted,
              ("ordinary DTS converts to AAC and codec-native AC3 matrix metadata permits remuxing: case " +
                  std::to_string(kind) + ", " + result.reason.detail).c_str());
      if (kind == 2) {
        require(storage_test::audioPackets(source) == storage_test::audioPackets(output),
                "AC3 matrix playback retains the exact encoded audio packets");
        inspectMatrix(output);
        const auto before = scan(source, {}), after = scan(output, {});
        const auto expectedStart = av_rescale_q(before.video.first, before.video.timeBase, AV_TIME_BASE_Q) - before.originMicros;
        const auto actualStart = av_rescale_q(after.video.first, after.video.timeBase, AV_TIME_BASE_Q);
        require(expectedStart == 5333 && std::abs(actualStart - expectedStart) <= 2,
                "MP4 edit lists preserve the fractional-millisecond video offset from AC3 encoder delay");
      }
    }
  }
}
void embeddedAudioSideDataTests() {
  const auto rejects = [](const AVPacketSideData &side, bool remux) {
    try { validateAudioSideData(&side, 1, remux); }
    catch (const Failure &failure) { return failure.outcome == FileOutcome::Skipped; }
    return false;
  };
  AVPacketSideData side{};
  AVAudioServiceType service = AV_AUDIO_SERVICE_TYPE_MAIN;
  side.type = AV_PKT_DATA_AUDIO_SERVICE_TYPE;
  side.data = reinterpret_cast<std::uint8_t *>(&service); side.size = sizeof(service);
  for (bool remux : {false, true}) {
    service = AV_AUDIO_SERVICE_TYPE_MAIN;
    validateAudioSideData(&side, 1, remux);
    service = AV_AUDIO_SERVICE_TYPE_COMMENTARY;
    require(rejects(side, remux), "container-only audio service cannot disappear during AAC conversion or remux");
  }
  std::uint8_t channel = 2;
  side.type = AV_PKT_DATA_JP_DUALMONO; side.data = &channel; side.size = 1;
  validateAudioSideData(&side, 1, true);
  for (const auto selection : {0, 1}) {
    channel = selection;
    validateAudioSideData(&side, 1, false);
    require(rejects(side, true), "remux cannot discard packet-only dual-mono channel selection");
  }
  for (const auto type : {AV_PKT_DATA_IAMF_MIX_GAIN_PARAM, AV_PKT_DATA_IAMF_DEMIXING_INFO_PARAM,
                          AV_PKT_DATA_IAMF_RECON_GAIN_INFO_PARAM}) {
    side.type = type;
    require(rejects(side, false) && rejects(side, true), "audio samples or copied packets cannot replace IAMF presentation parameters");
  }
  for (const auto type : {AV_PKT_DATA_STRINGS_METADATA, AV_PKT_DATA_METADATA_UPDATE}) {
    const std::string description("chart_editor_note\0Description\0", 30);
    const std::string gain("rEpLaYgAiN_tRaCk_GaIn\0-12 dB\0", 29);
    side.type = type;
    side.data = reinterpret_cast<std::uint8_t *>(const_cast<char *>(description.data())); side.size = description.size();
    validateAudioSideData(&side, 1, false); validateAudioSideData(&side, 1, true);
    side.data = reinterpret_cast<std::uint8_t *>(const_cast<char *>(gain.data())); side.size = gain.size();
    require(rejects(side, false) && rejects(side, true), "packet metadata updates cannot silently lose playback gain");
  }
}
void audioRemuxTrimTests() {
  storage_test::TempDirectory temp;
  storage_test::VideoFixture fixture;
  fixture.audio = true; fixture.audioCodec = AV_CODEC_ID_AC3; fixture.millisecondContainer = true;
  fixture.audioTrimPacket = 10;
  for (int i = 0; i < 32; ++i) fixture.timestamps.push_back(i);
  const auto source = temp.root / "trimmed-audio.avi", output = temp.root / "trimmed-audio.mp4";
  for (const auto trim : {600, 2000}) {
    fixture.audioTrimSamples = trim;
    storage_test::makeVideo(source, fixture);
    std::ifstream input(source, std::ios::binary);
    const std::string original((std::istreambuf_iterator<char>(input)), {});
    Input decoded(source);
    std::int64_t sampleCount = 0;
    decode(decoded, {}, [&](int index, const AVFrame &audio) {
      if (index == decoded.audio) sampleCount += audio.nb_samples;
    }, [](int, const AVPacket &) {});
    require(sampleCount == (trim == 600 ? 60840 : 61440),
            ("fixture applies valid discard padding and ignores padding larger than an AC3 frame: trim=" +
                std::to_string(trim) + ", samples=" + std::to_string(sampleCount)).c_str());
    StoragePolicy policy; policy.bga = BgaAction::CompressH264;
    const auto result = EncodeBga(candidate(source), policy, output, {}, {});
    if (trim == 600)
      require(result.outcome != FileOutcome::Converted && !std::filesystem::exists(output),
              "audio remux cannot restore the 600 interior samples removed by Matroska discard padding");
    else
      require(result.outcome == FileOutcome::Converted,
              "decoder-ignored oversized discard padding does not prevent unchanged audio remuxing");
    std::ifstream retained(source, std::ios::binary);
    require(std::string((std::istreambuf_iterator<char>(retained)), {}) == original,
            "discarded remux retains the original trimmed audio bytes");
  }
}
void audioRemuxContentTests() {
  storage_test::TempDirectory temp;
  storage_test::VideoFixture fixture; fixture.audio = true; fixture.audioCodec = AV_CODEC_ID_AC3;
  for (int i = 0; i < 32; ++i) fixture.timestamps.push_back(i);
  const auto source = temp.root / "audio-packets.mpg", output = temp.root / "audio-packets.mp4";
  storage_test::makeVideo(source, fixture);
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  const auto result = EncodeBga(candidate(source), policy, output, {}, [&](const StorageProgress &progress) {
    if (progress.phase != StoragePhase::Verify) return;
    std::array<std::int64_t, 2> positions{};
    std::array<std::string, 2> payloads;
    {
      Input input(output); auto compressed = packet(); int audioPacket = 0;
      while (av_read_frame(input.format, compressed.get()) >= 0) {
        if (compressed->stream_index == input.audio) {
          if (audioPacket == 10 || audioPacket == 11) {
            const auto index = audioPacket - 10;
            positions[index] = compressed->pos;
            payloads[index].assign(reinterpret_cast<const char *>(compressed->data), compressed->size);
          }
          ++audioPacket;
        }
        av_packet_unref(compressed.get());
      }
    }
    require(positions[0] > 0 && positions[1] > 0 && !payloads[0].empty() &&
                payloads[0].size() == payloads[1].size() && payloads[0] != payloads[1],
            "fixture exposes distinct complete AC3 packets with equal decoded sample counts");
    std::fstream bytes(output, std::ios::binary | std::ios::in | std::ios::out);
    for (int index = 0; index < 2; ++index) {
      bytes.seekp(positions[index]); bytes.write(payloads[1 - index].data(), payloads[1 - index].size());
    }
    require(bytes.good(), "swap valid AC3 payloads without changing MP4 timestamps or sample count");
  });
  require(result.outcome == FileOutcome::Failed && result.reason.code == StorageReason::ValidationFailed &&
              !std::filesystem::exists(output), "remux verification rejects changed audio even when sample count and endpoints match");
}
void changingAspectRatioTests() {
  storage_test::TempDirectory temp;
  storage_test::VideoFixture fixture;
  fixture.laterSampleAspectRatio = {4, 3};
  const auto input = temp.root / "changing-sar.mpg";
  const auto output = temp.root / "changing-sar.mp4";
  storage_test::makeVideo(input, fixture);
  const auto original = storage_test::inspectVideo(input);
  require(original.frameSampleAspectRatios.size() == 30 &&
              av_cmp_q(original.frameSampleAspectRatios.front(), original.frameSampleAspectRatios.back()) != 0,
          "fixture decodes constant pixel dimensions with changing frame aspect ratio");
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  const auto result = EncodeBga(candidate(input), policy, output, {}, {});
  require(result.outcome == FileOutcome::Skipped && result.reason.code == StorageReason::UnsupportedFormat &&
              !std::filesystem::exists(output) && std::filesystem::exists(input),
          "changing decoded aspect ratio keeps original without creating an output");
}
void existingBgaTests() {
  storage_test::TempDirectory temp;
  storage_test::VideoFixture fixture;
  fixture.h264 = true; fixture.rotation = 90; fixture.colorSpace = AVCOL_SPC_BT2020_CL;
  fixture.width = fixture.height = 64;
  const auto source = temp.root / "existing.mp4";
  storage_test::makeVideo(source, fixture);
  require(ValidateExistingBga(source).code == StorageReason::None,
          "existing MP4 H264 is reusable without applying color-conversion or display-transform policy");
  require(ValidateExistingBga(source, [] { return false; }).code == StorageReason::Cancelled,
          "existing video validation cooperates with cancellation");
  const auto truncated = temp.root / "truncated.mp4";
  std::filesystem::copy_file(source, truncated);
  std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) / 2);
  require(ValidateExistingBga(truncated).code == StorageReason::ValidationFailed,
          "truncated existing MP4 cannot authorize retiring the original video");
  {
    Input original(source);
    auto encoded = packet();
    std::int64_t lastVideo = -1;
    while (av_read_frame(original.format, encoded.get()) >= 0) {
      if (encoded->stream_index == original.video && encoded->size > 4) lastVideo = encoded->pos;
      av_packet_unref(encoded.get());
    }
    std::ifstream input(source, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    require(lastVideo >= 0 && static_cast<std::uint64_t>(lastVideo) + 4 <= bytes.size(),
            "MP4 fixture exposes the final encoded video packet");
    std::fill_n(bytes.begin() + lastVideo, 4, static_cast<char>(0xff));
    const auto corrupt = temp.root / "late-corruption.mp4";
    temp.write(corrupt.filename(), bytes);
    Input header(corrupt);
    require(header.format->streams[header.video]->codecpar->codec_id == AV_CODEC_ID_H264,
            "late packet corruption retains readable MP4 headers and H264 identification");
    require(ValidateExistingBga(corrupt).code == StorageReason::ValidationFailed,
            "full decode rejects a corrupt late video packet even when header probing succeeds");
    StoragePolicy policy; policy.bga = BgaAction::CompressH264;
    const auto unused = temp.root / "preserved.mp4";
    require(EncodeBga(candidate(corrupt), policy, unused, {}, {}).reason.code == StorageReason::AlreadyOptimized &&
                !std::filesystem::exists(unused),
            "compression preserves efficient source bytes without validating their late payload");
  }
  fixture.rotation = 0; fixture.colorSpace = AVCOL_SPC_UNSPECIFIED;
  const auto disguised = temp.root / "disguised.mp4";
  storage_test::makeVideo(disguised, fixture);
  require(ValidateExistingBga(disguised).code == StorageReason::UnsupportedFormat,
          "a filename extension cannot make H264 in another container a reusable MP4");
}
void quantizedAudioTimelineTests() {
  std::int64_t next = AV_NOPTS_VALUE;
  for (const auto pts : {0, 1008, 2064, 3072}) advanceAudioInput(pts, 1024, {1, 1000}, {1, 48000}, next);
  require(next == 4096, "millisecond PCM timestamps keep the exact accumulated sample count");
  bool gap = false;
  try { advanceAudioInput(4240, 1024, {1, 1000}, {1, 48000}, next); }
  catch (const Failure &failure) { gap = failure.reason == StorageReason::UnsupportedFormat; }
  require(gap, "audio gaps beyond source-clock rounding remain unsupported");
  next = AV_NOPTS_VALUE;
  bool drift = false;
  try {
    for (int i = 0; i < 8; ++i) advanceAudioInput(i * 1008, 1024, {1, 1000}, {1, 48000}, next);
  } catch (const Failure &failure) { drift = failure.reason == StorageReason::UnsupportedFormat; }
  require(drift, "small per-packet timestamp errors cannot accumulate into accepted audio drift");
}
void asfTimelineTests() {
  storage_test::TempDirectory temp;
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  for (bool audio : {false, true}) {
    storage_test::VideoFixture fixture;
    fixture.asf = true; fixture.audio = audio; fixture.width = fixture.height = 64;
    fixture.audioTailSamples = audio ? 4800 : 0;
    for (int i = 2; i < 32; ++i) fixture.timestamps.push_back(i);
    const auto source = temp.root / (audio ? "audio.wmv" : "video.wmv");
    const auto output = temp.root / (audio ? "audio.mp4" : "video.mp4");
    storage_test::makeVideo(source, fixture);
    const auto original = storage_test::inspectVideo(source);
    require(original.timestamps.size() == 30 && std::abs(original.timestamps.front() - .08) < .001 &&
                std::abs(original.timestamps.back() - 1.24) < .001,
            "ASF fixture has a complete nonzero-start WMV presentation timeline");
    const auto result = EncodeBga(candidate(source), policy, output, {}, {});
    if (result.outcome != FileOutcome::Converted) std::cerr << "ASF conversion: " << result.reason.detail << '\n';
    require(result.outcome == FileOutcome::Converted,
            "valid ASF global duration is not added to the individual video start again");
    const auto converted = storage_test::inspectVideo(output);
    require(converted.timestamps.size() == 30 && std::abs(converted.videoEnd - (audio ? 1.28 : 1.2)) < .002 &&
                std::abs(converted.timestamps.front() - (audio ? .08 : 0)) < .002,
            "WMV conversion preserves every frame and its offset relative to optional audio");
    if (audio) require(std::abs(converted.audioEnd - 1.38) < .025,
        "ASF global endpoint may belong to audio extending beyond the last video frame");
  }
}
void truncatedAsfTests() {
  storage_test::TempDirectory temp;
  storage_test::VideoFixture fixture; fixture.asf = true; fixture.width = fixture.height = 64;
  const auto source = temp.root / "complete.wmv", truncated = temp.root / "truncated.wmv";
  storage_test::makeVideo(source, fixture);
  std::ifstream input(source, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  const auto fileHeader = bytes.find(std::string("\xa1\xdc\xab\x8c\x47\xa9\xcf\x11\x8e\xe4\x00\xc0\x0c\x20\x53\x65", 16));
  const auto data = bytes.find(std::string("\x36\x26\xb2\x75\x8e\x66\xcf\x11\xa6\xd9\x00\xaa\x00\x62\xce\x6c", 16));
  require(fileHeader != std::string::npos && data != std::string::npos, "fixture has ASF file and data objects");
  const auto little = [&](std::size_t offset, int count) {
    require(offset + count <= bytes.size(), "complete ASF fixture field");
    std::uint64_t value = 0;
    for (int i = 0; i < count; ++i) value |= std::uint64_t(static_cast<unsigned char>(bytes[offset + i])) << (i * 8);
    return value;
  };
  const auto packets = little(fileHeader + 56, 8), packetBytes = little(fileHeader + 96, 4);
  require(packets > 4 && packetBytes > 0, "fixture contains multiple fixed-size ASF packets");
  bytes.resize(data + 50 + (packets / 2) * packetBytes);
  temp.write(truncated, bytes);
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  const auto output = temp.root / "truncated.mp4";
  const auto result = EncodeBga(candidate(truncated), policy, output, {}, {});
  require(result.outcome == FileOutcome::Converted,
          "clean packet-aligned ASF EOF intentionally tolerates stale size and packet declarations");
  const auto original = storage_test::inspectVideo(truncated), converted = storage_test::inspectVideo(output);
  // The source decoder omits frame duration at clean EOF. This fixture has a
  // known CFR cadence, so its last decoded frame still occupies one full tick.
  require(!original.timestamps.empty() && original.timestamps.size() < 30 &&
              original.timestamps.size() == converted.timestamps.size() &&
              std::abs(original.timestamps.back() + av_q2d(av_inv_q(fixture.rate)) -
                       original.timestamps.front() - converted.videoEnd) < .002,
          "ASF conversion preserves every frame actually decoded from the source");
  for (std::size_t i = 0; i < original.timestamps.size(); ++i)
    require(std::abs(converted.timestamps[i] - (original.timestamps[i] - original.timestamps.front())) < .002,
            "tolerated ASF size mismatch retains each decoded frame timestamp");
}
void remuxVideoDisposition(const std::filesystem::path &source, const std::filesystem::path &destination,
                           const char *container, int disposition, int audioDisposition = AV_DISPOSITION_DEFAULT,
                           int clusterMilliseconds = 0) {
  struct Resources {
    AVFormatContext *input = nullptr, *output = nullptr;
    AVPacket *packet = av_packet_alloc();
    ~Resources() {
      av_packet_free(&packet); avformat_close_input(&input);
      if (output) { if (output->pb) avio_closep(&output->pb); avformat_free_context(output); }
    }
  } resources;
  require(avformat_open_input(&resources.input, source.string().c_str(), nullptr, nullptr) >= 0 &&
              avformat_find_stream_info(resources.input, nullptr) >= 0 && resources.input->nb_streams <= 2,
          "open disposition fixture");
  require(avformat_alloc_output_context2(&resources.output, nullptr, container, destination.string().c_str()) >= 0,
          "create disposition fixture container");
  if (clusterMilliseconds)
    require(av_opt_set_int(resources.output->priv_data, "cluster_time_limit", clusterMilliseconds, 0) >= 0,
            "bound fixture cluster duration");
  for (unsigned i = 0; i < resources.input->nb_streams; ++i) {
    const auto input = resources.input->streams[i];
    const auto output = avformat_new_stream(resources.output, nullptr);
    require(output && avcodec_parameters_copy(output->codecpar, input->codecpar) >= 0,
            "copy disposition fixture codec parameters");
    output->codecpar->codec_tag = 0; output->time_base = input->time_base;
    output->disposition = input->codecpar->codec_type == AVMEDIA_TYPE_VIDEO ? disposition : audioDisposition;
  }
  require(avio_open(&resources.output->pb, destination.string().c_str(), AVIO_FLAG_WRITE) >= 0 &&
              avformat_write_header(resources.output, nullptr) >= 0, "write disposition fixture header");
  int status;
  while ((status = av_read_frame(resources.input, resources.packet)) >= 0) {
    const auto input = resources.input->streams[resources.packet->stream_index];
    const auto output = resources.output->streams[resources.packet->stream_index];
    av_packet_rescale_ts(resources.packet, input->time_base, output->time_base);
    require(av_interleaved_write_frame(resources.output, resources.packet) >= 0, "remux disposition fixture packet");
    av_packet_unref(resources.packet);
  }
  require(status == AVERROR_EOF && av_write_trailer(resources.output) >= 0, "complete disposition fixture");
}
void matroskaTimelineTests() {
  storage_test::TempDirectory temp;
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  for (bool audio : {false, true}) {
    storage_test::VideoFixture fixture;
    fixture.millisecondContainer = fixture.lossless = true;
    fixture.audio = audio; fixture.width = fixture.height = 64;
    fixture.audioTailSamples = audio ? 4800 : 0;
    for (int i = 2; i < 32; ++i) fixture.timestamps.push_back(i);
    const auto plain = temp.root / "plain.webm";
    const auto source = temp.root / (audio ? "audio.webm" : "video.webm");
    const auto output = temp.root / (audio ? "audio.mp4" : "video.mp4");
    storage_test::makeVideo(plain, fixture);
    remuxVideoDisposition(plain, source, "matroska", AV_DISPOSITION_DEFAULT, AV_DISPOSITION_DEFAULT, 200);
    {
      Input input(source);
      require(input.format->streams[input.video]->duration == AV_NOPTS_VALUE &&
                  std::abs(input.format->duration / double(AV_TIME_BASE) - (audio ? 1.38 : 1.28)) < .002,
              "Matroska declares the final presentation endpoint at container level only");
    }
    const auto result = EncodeBga(candidate(source), policy, output, {}, {});
    if (result.outcome != FileOutcome::Converted) std::cerr << "Matroska conversion: " << result.reason.detail << '\n';
    require(result.outcome == FileOutcome::Converted,
            "complete Matroska retains nonzero video starts and audio extending beyond video");
    const auto converted = storage_test::inspectVideo(output);
    require(converted.timestamps.size() == 30 && std::abs(converted.videoEnd - (audio ? 1.28 : 1.2)) < .002,
            "Matroska conversion preserves the complete video presentation");
    if (audio) require(std::abs(converted.audioEnd - 1.38) < .025,
        "Matroska container endpoint can belong to a longer audio stream");
    std::ifstream input(source, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    const std::string cluster("\x1f\x43\xb6\x75", 4);
    auto cut = bytes.find(cluster);
    for (int i = 0; i < 2 && cut != std::string::npos; ++i) cut = bytes.find(cluster, cut + cluster.size());
    require(cut != std::string::npos, "small Matroska fixture has at least three complete clusters");
    bytes.resize(cut);
    const auto truncated = temp.root / "truncated.webm", partialOutput = temp.root / "truncated.mp4";
    temp.write(truncated, bytes);
    const auto partial = storage_test::inspectVideo(truncated);
    require(!partial.timestamps.empty() && partial.timestamps.size() < 30 && partial.videoEnd < .8,
            "cluster-boundary truncation leaves a decodable prefix but removes the declared endpoint");
    const auto accepted = EncodeBga(candidate(truncated), policy, partialOutput, {}, {});
    require(accepted.outcome == FileOutcome::Converted,
            "clean Matroska EOF intentionally tolerates a stale declared endpoint");
    const auto decodedPartial = storage_test::inspectVideo(partialOutput);
    const auto origin = audio ? 0.0 : partial.timestamps.front();
    const auto partialEnd = partial.timestamps.back() + av_q2d(av_inv_q(fixture.rate));
    require(decodedPartial.timestamps.size() == partial.timestamps.size() &&
                std::abs(decodedPartial.videoEnd - (partialEnd - origin)) < .002,
            "Matroska conversion preserves the actual decoded source presentation");
    for (std::size_t i = 0; i < partial.timestamps.size(); ++i)
      require(std::abs(decodedPartial.timestamps[i] - (partial.timestamps[i] - origin)) < .002,
              "tolerated Matroska metadata mismatch retains each decoded frame timestamp");
    if (audio) require(decodedPartial.audioChannels == partial.audioChannels &&
                           decodedPartial.audioRate == partial.audioRate &&
                           std::abs(decodedPartial.audioEnd - partial.audioEnd) < .025,
                       "tolerated Matroska endpoint mismatch retains decoded audio");
    std::filesystem::remove(partialOutput);
  }
}
void videoDispositionTests() {
  storage_test::TempDirectory temp;
  storage_test::VideoFixture fixture; fixture.lossless = fixture.millisecondContainer = fixture.audio = true;
  fixture.width = fixture.height = 64;
  const auto plain = temp.root / "plain.webm", source = temp.root / "flags.webm", output = temp.root / "flags.mp4";
  storage_test::makeVideo(plain, fixture);
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  const int flags = AV_DISPOSITION_DEFAULT | AV_DISPOSITION_FORCED | AV_DISPOSITION_COMMENT | AV_DISPOSITION_DUB;
  const int audioFlags = AV_DISPOSITION_DEFAULT | AV_DISPOSITION_COMMENT | AV_DISPOSITION_DUB;
  remuxVideoDisposition(plain, source, "matroska", flags, audioFlags);
  {
    Input input(source);
    require(input.format->streams[input.video]->disposition == flags, "source fixture carries requested video dispositions");
    require(input.audio >= 0 && input.format->streams[input.audio]->disposition == audioFlags,
            "source fixture carries requested audio dispositions");
  }
  const auto converted = EncodeBga(candidate(source), policy, output, {}, {});
  require(converted.outcome == FileOutcome::Converted, "representable video dispositions permit compression");
  {
    Input input(output);
    require(input.format->streams[input.video]->disposition == flags, "MP4 preserves forced, default, commentary and dub video flags");
    require(input.audio >= 0 && input.format->streams[input.audio]->disposition == audioFlags,
            "MP4 preserves embedded audio dispositions when converting PCM to AAC");
  }
  policy.maxFps = 15;
  const auto remuxed = temp.root / "remuxed.mp4";
  fixture.audioCodec = AV_CODEC_ID_AAC;
  storage_test::makeVideo(plain, fixture);
  remuxVideoDisposition(plain, source, "matroska", flags, audioFlags);
  require(EncodeBga(candidate(source), policy, remuxed, {}, {}).outcome == FileOutcome::Converted &&
              storage_test::audioPackets(source) == storage_test::audioPackets(remuxed),
          "compatible flagged audio is remuxed without changing its packets");
  {
    Input input(remuxed);
    require(input.format->streams[input.video]->disposition == flags &&
                input.audio >= 0 && input.format->streams[input.audio]->disposition == audioFlags,
            "video and audio dispositions survive compatible-audio remux");
  }
  policy.maxFps = 0;
  std::filesystem::remove(output);
  for (bool video : {true, false}) {
    const auto corrupted = EncodeBga(candidate(source), policy, output, {}, [&](const StorageProgress &progress) {
      if (progress.phase != StoragePhase::Verify) return;
      const auto replacement = temp.root / "no-flags.mp4";
      remuxVideoDisposition(output, replacement, "mp4", video ? AV_DISPOSITION_DEFAULT : flags,
                            video ? audioFlags : AV_DISPOSITION_DEFAULT);
      std::filesystem::remove(output); std::filesystem::rename(replacement, output);
    });
    require(corrupted.reason.code == StorageReason::ValidationFailed && !std::filesystem::exists(output),
            "decode-back rejects dropped video or audio dispositions despite unchanged media packets");
  }
  for (bool video : {true, false}) {
    remuxVideoDisposition(plain, source, "matroska", AV_DISPOSITION_DEFAULT | (video ? AV_DISPOSITION_ORIGINAL : 0),
                          AV_DISPOSITION_DEFAULT | (video ? 0 : AV_DISPOSITION_ORIGINAL));
    const auto unsupported = EncodeBga(candidate(source), policy, output, {}, {});
    require(unsupported.reason.code == StorageReason::UnsupportedFormat && !std::filesystem::exists(output),
            "video or audio dispositions that MP4 cannot represent preserve the original file");
  }
}
void efficientCodecHeaderTests() {
  std::unique_ptr<AVFormatContext, decltype(&avformat_free_context)> format(avformat_alloc_context(), avformat_free_context);
  require(bool(format), "allocate codec header fixture");
  const auto stream = avformat_new_stream(format.get(), nullptr);
  require(stream, "allocate video header fixture");
  stream->codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
  const std::pair<AVCodecID, bool> cases[] = {
      {AV_CODEC_ID_H264, true}, {AV_CODEC_ID_HEVC, true}, {AV_CODEC_ID_VP9, true},
      {AV_CODEC_ID_AV1, true}, {AV_CODEC_ID_VVC, true}, {AV_CODEC_ID_MPEG1VIDEO, false},
      {AV_CODEC_ID_MPEG2VIDEO, false}, {AV_CODEC_ID_MPEG4, false}, {AV_CODEC_ID_VP8, false}, {AV_CODEC_ID_FFV1, false}};
  for (const auto [codec, efficient] : cases) {
    stream->codecpar->codec_id = codec;
    bool preserved = false;
    try { preserveEfficientVideo(*format); }
    catch (const Failure &failure) {
      preserved = failure.reason == StorageReason::AlreadyOptimized && failure.outcome == FileOutcome::Skipped;
    }
    require(preserved == efficient, "detected efficient codecs are retained while legacy codecs remain eligible");
  }
  stream->codecpar->codec_id = AV_CODEC_ID_H264;
  stream->disposition = AV_DISPOSITION_ATTACHED_PIC;
  preserveEfficientVideo(*format);
}
void efficientVideoPreservationTests() {
  storage_test::TempDirectory temp;
  storage_test::VideoFixture fixture; fixture.h264 = true;
  fixture.width = 192; fixture.height = 128; fixture.sampleAspectRatio = {4, 1};
  const auto source = temp.root / "efficient.avi", output = temp.root / "unused.mp4";
  storage_test::makeVideo(source, fixture);
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  policy.resolution = ResolutionCap::Low; policy.maxFps = 15;
  int checkpoints = 0; bool encoded = false;
  const auto result = EncodeBga(candidate(source), policy, output, [&] { return ++checkpoints <= 3; },
      [&](const StorageProgress &progress) { encoded |= progress.phase == StoragePhase::Encode; });
  require(result.outcome == FileOutcome::Skipped && result.reason.code == StorageReason::AlreadyOptimized &&
              !encoded && !std::filesystem::exists(output),
          "H264 is preserved before full decoding even when resolution and FPS caps are exceeded");
}
void videoTests() {
  storage_test::TempDirectory temp;
  StoragePolicy policy; policy.bga = BgaAction::CompressH264;
  {
    storage_test::VideoFixture captioned; captioned.h264 = captioned.captions = true;
    const auto input = temp.root / "captions.mp4", output = temp.root / "captions-capped.mp4";
    storage_test::makeVideo(input, captioned);
    Input media(input);
    std::size_t captionFrames = 0;
    decode(media, {}, [&](int, const AVFrame &decoded) {
      if (av_frame_get_side_data(&decoded, AV_FRAME_DATA_A53_CC)) ++captionFrames;
    }, [](int, const AVPacket &) {});
    require(captionFrames > 0, "H264 fixture decodes real A53 closed captions");
    policy.maxFps = 15;
    const auto result = EncodeBga(candidate(input), policy, output, {}, {});
    require(result.outcome == FileOutcome::Skipped && result.reason.code == StorageReason::AlreadyOptimized &&
                !std::filesystem::exists(output), "efficient source is retained with its original captions");
    require(ValidateExistingBga(input).code == StorageReason::None,
            "unchanged existing H264 remains reusable with its original captions");
    policy.maxFps = 0;
  }
  {
    storage_test::VideoFixture tagged;
    tagged.lossless = tagged.millisecondContainer = tagged.audio = tagged.descriptiveTag = true;
    const auto input = temp.root / "tagged.webm", output = temp.root / "tagged.mp4";
    storage_test::makeVideo(input, tagged);
    Input source(input);
    require(av_dict_get(source.format->metadata, "chart_editor_note", nullptr, 0),
            "supported source contains an arbitrary descriptive container tag");
    const auto original = storage_test::inspectVideo(input);
    require(EncodeBga(candidate(input), policy, output, {}, {}).outcome == FileOutcome::Converted,
            "unsupported descriptive container tags intentionally do not block conversion");
    Input destination(output);
    require(!av_dict_get(destination.format->metadata, "chart_editor_note", nullptr, 0),
            "fixture demonstrates permitted harmless descriptive metadata loss in MP4");
    const auto converted = storage_test::inspectVideo(output);
    require(converted.timestamps.size() == original.timestamps.size() && converted.width == original.width &&
                converted.height == original.height && std::abs(converted.videoEnd - original.videoEnd) < .002 &&
                converted.audioChannels == original.audioChannels && converted.audioRate == original.audioRate &&
                std::abs(converted.audioEnd - original.audioEnd) < .025,
            "descriptive tag loss leaves decoded video and audio playback intact");
    for (std::size_t i = 0; i < original.timestamps.size(); ++i)
      require(std::abs(converted.timestamps[i] - original.timestamps[i]) < .00001,
              "descriptive tag loss preserves every decoded presentation timestamp");
  }
  for (bool audioTag : {false, true}) {
    storage_test::VideoFixture tagged;
    tagged.lossless = tagged.millisecondContainer = tagged.audio = true;
    tagged.replayGain = !audioTag; tagged.audioReplayGain = audioTag;
    const auto input = temp.root / (audioTag ? "audio-gain.webm" : "container-gain.webm");
    const auto output = temp.root / (audioTag ? "audio-gain.mp4" : "container-gain.mp4");
    storage_test::makeVideo(input, tagged);
    Input source(input);
    const auto metadata = audioTag ? source.format->streams[source.audio]->metadata : source.format->metadata;
    const auto gain = av_dict_get(metadata, "REPLAYGAIN_TRACK_GAIN", nullptr, 0);
    require(gain && std::string_view(gain->value) == "-12.0 dB",
            "Matroska fixture retains actual container or audio playback gain");
    const auto result = EncodeBga(candidate(input), policy, output, {}, {});
    require(result.outcome == FileOutcome::Skipped && result.reason.code == StorageReason::UnsupportedFormat &&
                !std::filesystem::exists(output), "video conversion preserves playback gain instead of dropping it as descriptive text");
  }
  for (const auto key : {"replaygain_track_gain", "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_GAIN",
                         "REPLAYGAIN_ALBUM_PEAK", "R128_TRACK_GAIN", "r128_album_gain"}) {
    AVDictionary *metadata = nullptr;
    require(av_dict_set(&metadata, key, "1", 0) >= 0, "allocate playback gain dictionary");
    bool skipped = false;
    try { validatePlaybackGainMetadata(metadata); }
    catch (const Failure &failure) { skipped = failure.outcome == FileOutcome::Skipped; }
    av_dict_free(&metadata);
    require(skipped, "known ReplayGain and R128 volume tags cannot be discarded as descriptions");
  }
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
    fixture.colorRange = AVCOL_RANGE_JPEG; fixture.colorSpace = AVCOL_SPC_BT709;
    fixture.colorPrimaries = AVCOL_PRI_BT709; fixture.colorTransfer = AVCOL_TRC_BT709;
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
    const auto outputColor = scan(output, {});
    require(outputColor.colorRange == AVCOL_RANGE_JPEG && outputColor.colorSpace == AVCOL_SPC_BT709 &&
                outputColor.colorPrimaries == AVCOL_PRI_BT709 && outputColor.colorTransfer == AVCOL_TRC_BT709,
            "encoded video preserves effective color tags through complete decode-back");
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
  storage_test::VideoFixture large; large.width = 800; large.height = 600;
  for (int i = 0; i < 12; ++i) large.timestamps.push_back(i);
  storage_test::makeVideo(temp.root / "large.avi", large); policy.resolution = ResolutionCap::Low;
  result = EncodeBga(candidate(temp.root / "large.avi"), policy, temp.root / "large.mp4", {}, {});
  require(result.outcome == FileOutcome::Converted, "legacy MPEG exceeding a cap is converted");
  const auto scaled = storage_test::inspectVideo(temp.root / "large.mp4");
  require(scaled.width == 480 && scaled.height == 360, "legacy MPEG obeys orientation-independent cap");
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
  audioFixture.audioCodec = AV_CODEC_ID_AAC;
  storage_test::makeVideo(temp.root / "legacy-aac.avi", audioFixture);
  result = EncodeBga(candidate(temp.root / "legacy-aac.avi"), policy, temp.root / "remux.mp4", {}, {});
  require(result.outcome == FileOutcome::Converted &&
              storage_test::audioPackets(temp.root / "legacy-aac.avi") == storage_test::audioPackets(temp.root / "remux.mp4"),
          "compatible embedded audio is remuxed without another lossy encode");
  policy.maxFps = 0;
  for (int kind = 0; kind < 3; ++kind) {
    storage_test::VideoFixture unsupported; unsupported.timestamps = {0, 1};
    unsupported.alpha = kind == 0; unsupported.hdr = kind == 1; unsupported.interlaced = kind == 2;
    unsupported.lossless = unsupported.millisecondContainer = unsupported.hdr || unsupported.interlaced;
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
  int failures = 0;
  for (const auto test : {efficientCodecHeaderTests, efficientVideoPreservationTests, decodedDisplayTransformTests, decodedColorTests, chromaLocationTests, decodedAudioFormatTests, decodedAudioPresentationTests,
                          encodedAudioPresentationTests, embeddedAudioSideDataTests, audioRemuxTrimTests, audioRemuxContentTests, timelineTests,
                          declaredVideoTimelineTests, crossStreamTimelineTests, changingAspectRatioTests, quantizedAudioTimelineTests, asfTimelineTests,
                          truncatedAsfTests, matroskaTimelineTests, existingBgaTests, existingAudioTests, audioTests, videoDispositionTests, videoTests}) {
    try { test(); }
    catch (const Failure &failure) { std::cerr << failure.detail << '\n'; ++failures; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; ++failures; }
  }
  return failures ? 1 : 0;
}
