#include "../src/storage/AudioTranscoder.h"
// Include the implementation to exercise malformed timeline boundaries directly.
#include "../src/storage/BgaTranscoder.cpp"
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
  const auto tagged = temp.root / "tagged.wav";
  const auto taggedOutput = temp.root / "tagged.flac";
  const auto taggedPcm = samples(8192, 1, 16);
  writeAudio(tagged, SF_FORMAT_WAV | SF_FORMAT_PCM_16, 1, taggedPcm, false, false, true);
  require(EncodeAudio(candidate(tagged), policy, taggedOutput, {}, {}).outcome == FileOutcome::Converted,
          "descriptive RIFF INFO metadata permits lossless compression");
  require(readAudio(taggedOutput, 1, 8192) == taggedPcm, "tagged WAV preserves exact samples");
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
    require(EncodeAudio(candidate(unknown), policy, temp.root / "unknown-info.flac", {}, {}).reason.code ==
        StorageReason::UnsupportedFormat, "unknown INFO fields keep the original audio");
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
  observeVideoColor(output, params, *decoded);
  observed.video = {}; observed.video.timeBase = output.video.timeBase = {1, 25};
  observed.video.add(0, 1); output.video.add(0, 1);
  output.description.h264 = true; output.description.width = output.description.height = 16;
  VideoCompressionPlan plan; plan.width = plan.height = 16; plan.frameRate = {25, 1};
  verify(observed, output, plan, {1});
  for (int field = 0; field < 4; ++field) {
    if (field == 0) output.colorRange = AVCOL_RANGE_UNSPECIFIED;
    if (field == 1) output.colorSpace = AVCOL_SPC_UNSPECIFIED;
    if (field == 2) output.colorPrimaries = AVCOL_PRI_UNSPECIFIED;
    if (field == 3) output.colorTransfer = AVCOL_TRC_UNSPECIFIED;
    bool rejected = false;
    try { verify(observed, output, plan, {1}); }
    catch (const Failure &failure) { rejected = failure.reason == StorageReason::ValidationFailed; }
    require(rejected, "decode-back rejects lost color tags even when geometry and timing match");
    output.colorRange = observed.colorRange; output.colorSpace = observed.colorSpace;
    output.colorPrimaries = observed.colorPrimaries; output.colorTransfer = observed.colorTransfer;
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
  int failures = 0;
  for (const auto test : {decodedColorTests, decodedAudioFormatTests, timelineTests, declaredVideoTimelineTests,
                          crossStreamTimelineTests, changingAspectRatioTests, audioTests, videoTests}) {
    try { test(); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; ++failures; }
  }
  return failures ? 1 : 0;
}
