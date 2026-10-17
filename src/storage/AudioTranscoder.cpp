#include "AudioTranscoder.h"

#include <sndfile.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <vector>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#else
#include <unistd.h>
#endif

namespace storage_management {
namespace fs = std::filesystem;
namespace {
using SoundFile = std::unique_ptr<SNDFILE, decltype(&sf_close)>;
SoundFile openRead(const fs::path &path, SF_INFO &info) {
#ifdef _WIN32
  return {sf_wchar_open(path.c_str(), SFM_READ, &info), sf_close};
#else
  return {sf_open(path.c_str(), SFM_READ, &info), sf_close};
#endif
}
std::uint32_t le32(const unsigned char *p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
         (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
bool descriptiveComment(std::string_view value) {
  const auto equal = value.find('=');
  if (equal == std::string_view::npos) return false;
  std::string key(value.substr(0, equal));
  for (char &c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return key == "TITLE" || key == "COPYRIGHT" || key == "SOFTWARE" || key == "ENCODER" ||
         key == "ARTIST" || key == "COMMENT" || key == "DATE" || key == "ALBUM" ||
         key == "LICENSE" || key == "TRACKNUMBER" || key == "GENRE";
}
// libsndfile tolerates truncated RIFF data and does not expose every custom loop
// tag. Inspect only media container headers, never chart declarations.
StorageDiagnostic inspectContainer(const fs::path &path, int format, Checkpoint check) {
  const auto bad = StorageDiagnostic{StorageReason::ValidationFailed, "Truncated or invalid audio container"};
  const auto metadata = StorageDiagnostic{StorageReason::UnsupportedFormat, "Playback metadata cannot be preserved"};
  std::ifstream input(path, std::ios::binary);
  std::error_code error;
  const auto bytes = fs::file_size(path, error);
  if (!input || error) return bad;
  std::array<unsigned char, 12> header{};
  if ((format & SF_FORMAT_TYPEMASK) == SF_FORMAT_FLAC) {
    if (!input.read(reinterpret_cast<char *>(header.data()), 4) || std::memcmp(header.data(), "fLaC", 4)) return bad;
    std::uint64_t offset = 4;
    bool last = false;
    while (!last) {
      if (check && !check()) return {StorageReason::Cancelled, "Cancelled"};
      if (!input.read(reinterpret_cast<char *>(header.data()), 4)) return bad;
      last = (header[0] & 128) != 0;
      const int type = header[0] & 127;
      const std::uint32_t length = (header[1] << 16) | (header[2] << 8) | header[3];
      offset += 4;
      if (offset > bytes || length > bytes - offset) return bad;
      if (type == 4) {
        // Bounded comment block parsing; huge or unknown tags are preserved by
        // retaining the original, rather than silently losing semantics.
        if (length > 65536) return metadata;
        std::vector<unsigned char> block(length);
        if (!input.read(reinterpret_cast<char *>(block.data()), length) || length < 8) return bad;
        std::size_t pos = 4 + std::uint64_t(le32(block.data()));
        if (pos > length - 4) return bad;
        const auto count = le32(block.data() + pos); pos += 4;
        for (std::uint32_t i = 0; i < count; ++i) {
          if (pos > length - 4) return bad;
          const auto size = le32(block.data() + pos); pos += 4;
          if (size > length - pos) return bad;
          if (!descriptiveComment({reinterpret_cast<const char *>(block.data() + pos), size})) return metadata;
          pos += size;
        }
      } else {
        if (type != 0 && type != 1 && type != 3) return metadata;
        input.seekg(length, std::ios::cur);
      }
      offset += length;
    }
    return offset < bytes ? StorageDiagnostic{} : bad;
  }
  if (!input.read(reinterpret_cast<char *>(header.data()), 12) ||
      std::memcmp(header.data(), "RIFF", 4) || std::memcmp(header.data() + 8, "WAVE", 4)) return bad;
  const std::uint64_t end = std::uint64_t(le32(header.data() + 4)) + 8;
  if (end != bytes) return bad;
  std::uint64_t offset = 12;
  bool data = false;
  while (offset < end) {
    if (check && !check()) return {StorageReason::Cancelled, "Cancelled"};
    if (end - offset < 8 || !input.read(reinterpret_cast<char *>(header.data()), 8)) return bad;
    const auto length = le32(header.data() + 4);
    offset += 8;
    const std::uint64_t padded = std::uint64_t(length) + (length & 1);
    if (padded > end - offset) return bad;
    const std::string_view type(reinterpret_cast<const char *>(header.data()), 4);
    if (type == "data") { if (data) return bad; data = true; }
    else if (type != "fmt " && type != "fact" && type != "JUNK" && type != "PAD ") return metadata;
    input.seekg(padded, std::ios::cur);
    offset += padded;
  }
  return data ? StorageDiagnostic{} : bad;
}
StorageCodecSupport support(bool available, const char *name) {
  return {available, available ? "" : std::string(name) + " is unavailable in this build"};
}
bool canFormat(int format) {
  SF_INFO info{}; info.format = format; info.channels = 2; info.samplerate = 44100;
  return sf_format_check(&info) != 0;
}
}

StorageCodecCapabilities ProbeStorageCodecs() {
  return {support(canFormat(SF_FORMAT_FLAC | SF_FORMAT_PCM_16), "FLAC"),
          support(canFormat(SF_FORMAT_OGG | SF_FORMAT_VORBIS), "Vorbis"),
          support(avcodec_find_encoder_by_name("libx264") != nullptr, "libx264"),
          support(avcodec_find_encoder(AV_CODEC_ID_AAC) != nullptr, "AAC"),
          support(av_guess_format("mp4", nullptr, nullptr) != nullptr, "MP4")};
}

AudioEncodingDecision ChooseAudioEncoding(int sourceFormat, int sampleRate, int channels,
                                          AudioAction action, const StorageCodecCapabilities &caps) {
  if (action == AudioAction::Keep) return {0, {StorageReason::PolicyKeep, "Audio policy is Keep"}};
  const int type = sourceFormat & SF_FORMAT_TYPEMASK;
  const int subtype = sourceFormat & SF_FORMAT_SUBMASK;
  const bool wav = type == SF_FORMAT_WAV || type == SF_FORMAT_WAVEX;
  if ((!wav && type != SF_FORMAT_FLAC) || (action == AudioAction::LosslessFlac && !wav))
    return {0, {StorageReason::UnsupportedSource, "Source is not eligible for this audio policy"}};
  // These layouts have unambiguous native ordering in both output containers.
  if (channels < 1 || channels > 2 || sampleRate <= 0)
    return {0, {StorageReason::UnsupportedFormat, "Unsupported native rate or channel layout"}};
  int format = 0;
  if (action == AudioAction::LosslessFlac) {
    if (subtype != SF_FORMAT_PCM_U8 && subtype != SF_FORMAT_PCM_S8 &&
        subtype != SF_FORMAT_PCM_16 && subtype != SF_FORMAT_PCM_24)
      return {0, {StorageReason::UnsupportedFormat, "Lossless FLAC requires 8, 16 or 24-bit integer PCM"}};
    if (!caps.flac.available) return {0, {StorageReason::CodecUnavailable, caps.flac.reason}};
    format = SF_FORMAT_FLAC | (subtype == SF_FORMAT_PCM_U8 ? SF_FORMAT_PCM_S8 : subtype);
  } else if (action == AudioAction::LossyVorbis) {
    if (!caps.vorbis.available) return {0, {StorageReason::CodecUnavailable, caps.vorbis.reason}};
    format = SF_FORMAT_OGG | SF_FORMAT_VORBIS;
  } else return {0, {StorageReason::UnsupportedFormat, "Invalid audio policy"}};
  SF_INFO output{}; output.format = format; output.channels = channels; output.samplerate = sampleRate;
  if (!sf_format_check(&output)) return {0, {StorageReason::UnsupportedFormat, "Encoder cannot preserve native audio format"}};
  return {format, {}};
}

EncodeResult EncodeAudio(const AssetCandidate &candidate, const StoragePolicy &policy,
                         const fs::path &temporaryOutput, Checkpoint check, ProgressCallback progress) {
  bool created = false;
  auto finish = [&](FileOutcome outcome, StorageReason reason, std::string detail) {
    if (created && outcome != FileOutcome::Converted) {
      std::error_code ignored; fs::remove(temporaryOutput, ignored);
    }
    return EncodeResult{outcome, outcome == FileOutcome::Converted ? temporaryOutput : fs::path{},
                        {reason, std::move(detail)}};
  };
  auto cancelled = [&] { return finish(FileOutcome::Cancelled, StorageReason::Cancelled, "Cancelled"); };
  if (check && !check()) return cancelled();
  std::string extension = candidate.source.extension().string();
  for (char &c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if ((extension != ".wav" && extension != ".flac") ||
      (policy.audio == AudioAction::LosslessFlac && extension != ".wav"))
    return finish(FileOutcome::Skipped, StorageReason::UnsupportedSource, "Source extension is not eligible");
  SF_INFO info{};
  auto input = openRead(candidate.source, info);
  if (!input) return finish(FileOutcome::Failed, StorageReason::ValidationFailed, "Cannot decode source audio");
  auto decision = ChooseAudioEncoding(info.format, info.samplerate, info.channels, policy.audio, ProbeStorageCodecs());
  if (!decision.format) return finish(FileOutcome::Skipped, decision.reason.code, decision.reason.detail);
  const auto container = inspectContainer(candidate.source, info.format, check);
  if (container.code == StorageReason::Cancelled) return cancelled();
  if (container.code != StorageReason::None)
    return finish(container.code == StorageReason::ValidationFailed ? FileOutcome::Failed : FileOutcome::Skipped,
                  container.code, container.detail);
  if (info.frames <= 0) return finish(FileOutcome::Failed, StorageReason::ValidationFailed, "Empty audio");
  SF_INFO outputInfo{};
  outputInfo.format = decision.format; outputInfo.channels = info.channels; outputInfo.samplerate = info.samplerate;
#ifdef _WIN32
  const int descriptor = _wopen(temporaryOutput.c_str(), _O_CREAT | _O_EXCL | _O_RDWR | _O_BINARY,
                                _S_IREAD | _S_IWRITE);
#else
  const int descriptor = open(temporaryOutput.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
#endif
  if (descriptor < 0) return finish(FileOutcome::Failed, StorageReason::IoError, "Cannot create exclusive temporary audio");
  created = true;
  SoundFile output(sf_open_fd(descriptor, SFM_WRITE, &outputInfo, SF_TRUE), sf_close);
  if (!output) {
#ifdef _WIN32
    _close(descriptor);
#else
    close(descriptor);
#endif
    return finish(FileOutcome::Failed, StorageReason::CodecUnavailable, "Cannot open requested audio encoder");
  }
  // Close before unlink on every failure, including on Windows.
  auto failOutput = [&](StorageReason reason, const char *message) {
    output.reset();
    return finish(reason == StorageReason::Cancelled ? FileOutcome::Cancelled : FileOutcome::Failed, reason, message);
  };
  if (policy.audio == AudioAction::LossyVorbis) {
    double quality = 0.5;
    if (!sf_command(output.get(), SFC_SET_VBR_ENCODING_QUALITY, &quality, sizeof(quality)))
      return failOutput(StorageReason::CodecUnavailable, "Vorbis quality control is unavailable");
  }
  for (int tag = SF_STR_FIRST; tag <= SF_STR_LAST; ++tag)
    if (const char *value = sf_get_string(input.get(), tag))
      if (sf_set_string(output.get(), tag, value) != 0)
        return failOutput(StorageReason::UnsupportedFormat, "Cannot preserve audio metadata");
  constexpr sf_count_t blockFrames = 4096;
  std::vector<int> integers(blockFrames * info.channels);
  std::vector<float> floats(blockFrames * info.channels);
  sf_count_t total = 0;
  const bool lossless = policy.audio == AudioAction::LosslessFlac;
  if (progress) progress({StoragePhase::Encode, candidate.source});
  for (;;) {
    if (check && !check()) return failOutput(StorageReason::Cancelled, "Cancelled");
    const auto read = lossless ? sf_readf_int(input.get(), integers.data(), blockFrames) :
                                sf_readf_float(input.get(), floats.data(), blockFrames);
    if (!read) break;
    if (!lossless && !std::all_of(floats.begin(), floats.begin() + read * info.channels,
                                 [](float value) { return std::isfinite(value) && std::abs(value) <= 1.f; }))
      return failOutput(StorageReason::UnsupportedFormat, "Audio exceeds the encoder's native range");
    const auto written = lossless ? sf_writef_int(output.get(), integers.data(), read) :
                                   sf_writef_float(output.get(), floats.data(), read);
    if (written != read) return failOutput(StorageReason::IoError, "Audio encoder write failed");
    total += read;
  }
  if (sf_error(input.get()) || total != info.frames)
    return failOutput(StorageReason::ValidationFailed, "Source audio did not decode completely");
  if (sf_close(output.release())) return finish(FileOutcome::Failed, StorageReason::IoError, "Audio encoder finalization failed");
  if (progress) progress({StoragePhase::Verify, candidate.source});
  SF_INFO verifyInfo{};
  auto verify = openRead(temporaryOutput, verifyInfo);
  if (!verify || (verifyInfo.format & (SF_FORMAT_TYPEMASK | SF_FORMAT_SUBMASK)) != decision.format ||
      verifyInfo.frames != info.frames || verifyInfo.channels != info.channels ||
      verifyInfo.samplerate != info.samplerate || sf_seek(input.get(), 0, SEEK_SET) != 0) {
    verify.reset(); return finish(FileOutcome::Failed, StorageReason::ValidationFailed, "Encoded audio format or timing changed");
  }
  std::vector<int> reference(integers.size());
  total = 0;
  bool valid = true;
  for (;;) {
    if (check && !check()) { verify.reset(); return cancelled(); }
    const auto read = sf_readf_int(verify.get(), integers.data(), blockFrames);
    if (!read) break;
    if (lossless && (sf_readf_int(input.get(), reference.data(), read) != read ||
        !std::equal(integers.begin(), integers.begin() + read * info.channels, reference.begin()))) { valid = false; break; }
    total += read;
  }
  valid = valid && total == info.frames && !sf_error(verify.get()) && !sf_error(input.get());
  verify.reset(); input.reset();
  if (!valid) return finish(FileOutcome::Failed, StorageReason::ValidationFailed, "Audio decode-back validation failed");
  std::error_code error;
  const auto outputBytes = fs::file_size(temporaryOutput, error);
  if (error) return finish(FileOutcome::Failed, StorageReason::IoError, "Cannot measure output audio");
  const auto sourceBytes = fs::file_size(candidate.source, error);
  if (error) return finish(FileOutcome::Failed, StorageReason::IoError, "Cannot measure source audio");
  if (outputBytes >= sourceBytes) return finish(FileOutcome::Skipped, StorageReason::NotSmaller, "Encoded audio is not smaller");
  return finish(FileOutcome::Converted, StorageReason::None, "");
}
} // namespace storage_management
