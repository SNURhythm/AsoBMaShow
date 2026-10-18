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
StorageDiagnostic validateFlacFrames(const fs::path &path, const SF_INFO &info, const Checkpoint &check) {
  const auto invalid = StorageDiagnostic{StorageReason::ValidationFailed, "FLAC did not decode completely"};
  const auto cancelled = StorageDiagnostic{StorageReason::Cancelled, "Cancelled"};
  struct Decoder {
    AVFormatContext *input = nullptr;
    AVCodecContext *codec = nullptr;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    ~Decoder() {
      av_frame_free(&frame); av_packet_free(&packet);
      avcodec_free_context(&codec); avformat_close_input(&input);
    }
  } decoder;
  const auto utf8 = path.u8string();
  const std::string name(utf8.begin(), utf8.end());
  if (!decoder.packet || !decoder.frame ||
      avformat_open_input(&decoder.input, name.c_str(), nullptr, nullptr) < 0 ||
      avformat_find_stream_info(decoder.input, nullptr) < 0) return invalid;
  int audio = -1;
  for (unsigned i = 0; i < decoder.input->nb_streams; ++i) {
    const auto stream = decoder.input->streams[i];
    if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && audio < 0 &&
        stream->codecpar->codec_id == AV_CODEC_ID_FLAC) audio = i;
    else if (stream->codecpar->codec_type != AVMEDIA_TYPE_VIDEO ||
             !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) return invalid;
  }
  if (audio < 0) return invalid;
  const auto codec = avcodec_find_decoder(AV_CODEC_ID_FLAC);
  if (!codec || !(decoder.codec = avcodec_alloc_context3(codec)) ||
      avcodec_parameters_to_context(decoder.codec, decoder.input->streams[audio]->codecpar) < 0) return invalid;
  decoder.codec->thread_count = 1;
  decoder.codec->err_recognition = AV_EF_CRCCHECK | AV_EF_BITSTREAM | AV_EF_BUFFER | AV_EF_EXPLODE;
  if (avcodec_open2(decoder.codec, codec, nullptr) < 0) return invalid;
  sf_count_t frames = 0;
  const auto drain = [&](bool flushing) -> StorageDiagnostic {
    for (;;) {
      if (check && !check()) return cancelled;
      const auto status = avcodec_receive_frame(decoder.codec, decoder.frame);
      if (status == AVERROR_EOF || (!flushing && status == AVERROR(EAGAIN))) return {};
      if (status < 0 || decoder.frame->decode_error_flags || (decoder.frame->flags & AV_FRAME_FLAG_CORRUPT) ||
          decoder.frame->sample_rate != info.samplerate || decoder.frame->ch_layout.nb_channels != info.channels ||
          decoder.frame->nb_samples <= 0 || decoder.frame->nb_samples > info.frames - frames) return invalid;
      frames += decoder.frame->nb_samples;
      av_frame_unref(decoder.frame);
    }
  };
  // libsndfile stops at STREAMINFO's count, hiding extra or damaged frames when
  // that count is understated. Read packets to physical EOF and flush instead.
  int status = 0;
  while ((status = av_read_frame(decoder.input, decoder.packet)) >= 0) {
    if (check && !check()) return cancelled;
    if (decoder.packet->stream_index == audio) {
      if ((decoder.packet->flags & AV_PKT_FLAG_CORRUPT) ||
          avcodec_send_packet(decoder.codec, decoder.packet) < 0) return invalid;
      const auto result = drain(false);
      if (result.code != StorageReason::None) return result;
    }
    av_packet_unref(decoder.packet);
  }
  if (status != AVERROR_EOF || (decoder.input->pb && decoder.input->pb->error < 0 &&
      decoder.input->pb->error != AVERROR_EOF) || avcodec_send_packet(decoder.codec, nullptr) < 0) return invalid;
  const auto result = drain(true);
  if (result.code != StorageReason::None) return result;
  return frames == info.frames ? StorageDiagnostic{} : invalid;
}
std::uint32_t le32(const unsigned char *p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
         (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
int descriptiveCommentTag(std::string_view value) {
  const auto equal = value.find('=');
  if (equal == std::string_view::npos) return 0;
  std::string key(value.substr(0, equal));
  for (char &c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (key == "TITLE") return SF_STR_TITLE;
  if (key == "COPYRIGHT") return SF_STR_COPYRIGHT;
  if (key == "SOFTWARE" || key == "ENCODER") return SF_STR_SOFTWARE;
  if (key == "ARTIST") return SF_STR_ARTIST;
  if (key == "COMMENT") return SF_STR_COMMENT;
  if (key == "DATE") return SF_STR_DATE;
  if (key == "ALBUM") return SF_STR_ALBUM;
  if (key == "LICENSE") return SF_STR_LICENSE;
  if (key == "TRACKNUMBER") return SF_STR_TRACKNUMBER;
  if (key == "GENRE") return SF_STR_GENRE;
  return 0;
}
struct WaveMetadata {
  bool preserve = false;
  bool last = false;
  bool unpaddedData = false;
  std::uint64_t sourceEnd = 0;
  std::uint64_t bytes = 20; // APPLICATION header, riff ID and RIFF/WAVE header.
};
// libsndfile tolerates truncated RIFF data and does not expose every custom loop
// tag. Inspect only media container headers, never chart declarations.
StorageDiagnostic inspectContainer(const fs::path &path, const SF_INFO &info, SNDFILE *audio, Checkpoint check,
                                   bool lossless, WaveMetadata &foreign) {
  const auto bad = StorageDiagnostic{StorageReason::ValidationFailed, "Truncated or invalid audio container"};
  const auto metadata = StorageDiagnostic{StorageReason::UnsupportedFormat, "Playback metadata cannot be preserved"};
  std::ifstream input(path, std::ios::binary);
  std::error_code error;
  const auto bytes = fs::file_size(path, error);
  if (!input || error) return bad;
  std::array<unsigned char, 12> header{};
  if ((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_FLAC) {
    if (!input.read(reinterpret_cast<char *>(header.data()), 4) || std::memcmp(header.data(), "fLaC", 4)) return bad;
    std::uint64_t offset = 4;
    bool last = false;
    std::array<bool, SF_STR_LAST + 1> seenTags{};
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
          const int tag = descriptiveCommentTag({reinterpret_cast<const char *>(block.data() + pos), size});
          // libsndfile exposes one value per field, including SOFTWARE/ENCODER
          // aliases. Retain the source whenever conversion would lose a value.
          if (!tag || seenTags[tag]) return metadata;
          seenTags[tag] = true;
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
  const std::uint64_t declaredEnd = std::uint64_t(le32(header.data() + 4)) + 8;
  const auto sizeMismatch = StorageDiagnostic{StorageReason::ValidationFailed,
      "WAV RIFF declares " + std::to_string(declaredEnd) + " bytes; file contains " + std::to_string(bytes)};
  // A writer may count an omitted terminal data pad in the outer RIFF size.
  // Defer this one-byte discrepancy until the complete PCM payload is checked.
  if (declaredEnd != bytes && declaredEnd != bytes + 1) return sizeMismatch;
  const std::uint64_t end = bytes;
  foreign.sourceEnd = end;
  int sampleBytes = 0;
  switch (info.format & SF_FORMAT_SUBMASK) {
  case SF_FORMAT_PCM_U8: case SF_FORMAT_PCM_S8: sampleBytes = 1; break;
  case SF_FORMAT_PCM_16: sampleBytes = 2; break;
  case SF_FORMAT_PCM_24: sampleBytes = 3; break;
  case SF_FORMAT_PCM_32: case SF_FORMAT_FLOAT: sampleBytes = 4; break;
  case SF_FORMAT_DOUBLE: sampleBytes = 8; break;
  }
  const int frameBytes = sampleBytes * info.channels;
  std::uint64_t offset = 12;
  bool data = false;
  bool oversized = false;
  while (offset < end) {
    if (check && !check()) return {StorageReason::Cancelled, "Cancelled"};
    if (end - offset < 8 || !input.read(reinterpret_cast<char *>(header.data()), 8))
      return {StorageReason::ValidationFailed, "Incomplete WAV chunk header at byte " + std::to_string(offset)};
    const auto length = le32(header.data() + 4);
    offset += 8;
    const std::string_view type(reinterpret_cast<const char *>(header.data()), 4);
    std::uint64_t padded = std::uint64_t(length) + (length & 1);
    // libsndfile rounds partial PCM frames down without reporting an error.
    // Check the declared payload even when RIFF bounds and padding are valid.
    if (type == "data" && frameBytes > 0 &&
        (length % frameBytes || length / frameBytes != info.frames))
      return {StorageReason::ValidationFailed, "WAV data does not contain complete declared native audio frames"};
    if (type == "data" && (length & 1) && length == end - offset) {
      // Some WAV writers omit the final alignment byte. Accept only a complete
      // terminal PCM payload, never missing samples or an unpadded middle chunk.
      if (frameBytes <= 0)
        return {StorageReason::ValidationFailed, "Unpadded WAV data does not contain complete declared PCM frames"};
      foreign.unpaddedData = true;
      padded = length;
    }
    if (padded > end - offset)
      return {StorageReason::ValidationFailed, "WAV chunk '" + std::string(type) + "' at byte " +
          std::to_string(offset - 8) + " requires " + std::to_string(padded) +
          " payload/padding bytes; only " + std::to_string(end - offset) + " remain"};
    const auto stored = type == "data" ? 8 : 8 + padded;
    foreign.bytes += 8 + stored;
    oversized = oversized || stored > 0xffffff - 4;
    if (type == "data") {
      if (data) return {StorageReason::ValidationFailed, "WAV contains multiple data chunks"};
      data = true;
    }
    // FLAC's RIFF APPLICATION blocks retain ancillary chunks verbatim, including
    // unknown vendor metadata. Only lossy conversion needs a semantic whitelist.
    else if (lossless && type != "fmt " && type != "fact" && type != "JUNK" && type != "PAD ") foreign.preserve = true;
    else if (type == "LIST") {
      // Accept only descriptive INFO fields actually exposed by the decoder.
      // Unknown fields, conflicting duplicates and playback lists stay intact.
      if (length > 65536) return metadata;
      std::string block(length, '\0');
      if (length < 4 || !input.read(block.data(), length)) return bad;
      if (block.compare(0, 4, "INFO") != 0) return metadata;
      for (std::size_t pos = 4; pos < block.size();) {
        if (block.size() - pos < 8) return bad;
        const auto name = std::string_view(block).substr(pos, 4);
        const auto size = le32(reinterpret_cast<const unsigned char *>(block.data() + pos + 4));
        pos += 8;
        const auto paddedSize = std::uint64_t(size) + (size & 1);
        if (paddedSize > block.size() - pos) return bad;
        int tag = 0;
        if (name == "INAM") tag = SF_STR_TITLE;
        else if (name == "ICOP") tag = SF_STR_COPYRIGHT;
        else if (name == "ISFT") tag = SF_STR_SOFTWARE;
        else if (name == "IART") tag = SF_STR_ARTIST;
        else if (name == "ICMT") tag = SF_STR_COMMENT;
        else if (name == "ICRD") tag = SF_STR_DATE;
        else if (name == "IPRD") tag = SF_STR_ALBUM;
        else if (name == "ITRK") tag = SF_STR_TRACKNUMBER;
        else if (name == "IGNR") tag = SF_STR_GENRE;
        else return {StorageReason::UnsupportedFormat, "Cannot preserve WAV INFO field: " + std::string(name)};
        auto value = std::string_view(block).substr(pos, size);
        while (!value.empty() && value.back() == '\0') value.remove_suffix(1);
        const char *decoded = sf_get_string(audio, tag);
        if (value.find('\0') != std::string_view::npos || !decoded || value != decoded) return metadata;
        pos += paddedSize;
      }
      input.seekg(length & 1, std::ios::cur);
      offset += padded;
      continue;
    } else if (type != "fmt " && type != "fact" && type != "JUNK" && type != "PAD ")
      return {StorageReason::UnsupportedFormat, "Cannot preserve WAV metadata chunk: " + std::string(type)};
    input.seekg(padded, std::ios::cur);
    offset += padded;
  }
  if (declaredEnd != end && !foreign.unpaddedData) return sizeMismatch;
  if (foreign.preserve && oversized)
    return {StorageReason::UnsupportedFormat, "WAV metadata chunk exceeds the FLAC metadata block limit"};
  return data ? StorageDiagnostic{} : StorageDiagnostic{StorageReason::ValidationFailed, "WAV has no data chunk"};
}
// Match flac --keep-foreign-metadata: one riff APPLICATION block per RIFF
// chunk, with only the RIFF/WAVE and data headers for the audio containers.
// Copy opaque ancillary bytes rather than interpreting or rebuilding their fields.
StorageDiagnostic preserveWaveMetadata(const fs::path &source, const fs::path &destination,
                                       WaveMetadata &foreign, Checkpoint check, bool verify) {
  const auto bad = StorageDiagnostic{verify ? StorageReason::ValidationFailed : StorageReason::IoError,
                                     verify ? "WAV metadata verification failed" : "Cannot preserve WAV metadata"};
  std::ifstream input(source, std::ios::binary);
  std::fstream output(destination, std::ios::binary | std::ios::in | (verify ? std::ios::openmode{} : std::ios::out));
  std::array<char, 65536> buffer{}, comparison{};
  std::array<unsigned char, 12> header{};
  if (!input || !output || !output.read(reinterpret_cast<char *>(header.data()), 8) ||
      std::memcmp(header.data(), "fLaC", 4) || (header[4] & 127) != 0 ||
      header[5] != 0 || header[6] != 0 || header[7] != 34) return bad;
  constexpr std::uint64_t insertion = 42; // Magic plus STREAMINFO header/body.
  if (!verify) {
    foreign.last = (header[4] & 128) != 0;
    std::error_code error;
    const auto size = fs::file_size(destination, error);
    if (error || size < insertion) return bad;
    // Move only this temporary encoded file backwards, with bounded scratch.
    for (auto end = size; end > insertion;) {
      if (check && !check()) return {StorageReason::Cancelled, "Cancelled"};
      const auto count = std::min<std::uint64_t>(buffer.size(), end - insertion);
      end -= count;
      output.seekg(end);
      if (!output.read(buffer.data(), count)) return bad;
      output.seekp(end + foreign.bytes);
      if (!output.write(buffer.data(), count)) return bad;
    }
    output.seekp(4); output.put(0); // Inserted blocks now follow STREAMINFO.
    output.seekp(insertion);
  } else output.seekg(insertion);
  if (!input.read(reinterpret_cast<char *>(header.data()), 12) ||
      std::memcmp(header.data(), "RIFF", 4) || std::memcmp(header.data() + 8, "WAVE", 4)) return bad;
  const std::uint64_t sourceEnd = foreign.sourceEnd;
  std::uint64_t offset = 0, written = 0;
  while (offset < sourceEnd) {
    if (check && !check()) return {StorageReason::Cancelled, "Cancelled"};
    std::uint64_t stored = 12, next = 12;
    if (offset) {
      input.seekg(offset);
      if (sourceEnd - offset < 8 || !input.read(reinterpret_cast<char *>(header.data()), 8)) return bad;
      const auto size = le32(header.data() + 4);
      std::uint64_t padded = std::uint64_t(size) + (size & 1);
      if (foreign.unpaddedData && !std::memcmp(header.data(), "data", 4) && size == sourceEnd - offset - 8)
        padded = size;
      if (padded > sourceEnd - offset - 8) return bad;
      next = offset + 8 + padded;
      stored = std::memcmp(header.data(), "data", 4) ? 8 + padded : 8;
    }
    if (stored > 0xffffff - 4 || written + 8 + stored > foreign.bytes) return bad;
    const auto length = static_cast<std::uint32_t>(stored + 4);
    const std::array<unsigned char, 8> block{
        static_cast<unsigned char>(2 | (foreign.last && next == sourceEnd ? 128 : 0)),
        static_cast<unsigned char>(length >> 16), static_cast<unsigned char>(length >> 8),
        static_cast<unsigned char>(length), 'r', 'i', 'f', 'f'};
    if (verify) {
      if (!output.read(comparison.data(), block.size()) ||
          std::memcmp(comparison.data(), block.data(), block.size())) return bad;
    } else if (!output.write(reinterpret_cast<const char *>(block.data()), block.size())) return bad;
    input.seekg(offset);
    for (std::uint64_t remaining = stored; remaining;) {
      if (check && !check()) return {StorageReason::Cancelled, "Cancelled"};
      const auto count = std::min<std::uint64_t>(buffer.size(), remaining);
      if (!input.read(buffer.data(), count)) return bad;
      if (verify) {
        if (!output.read(comparison.data(), count) || std::memcmp(buffer.data(), comparison.data(), count)) return bad;
      } else if (!output.write(buffer.data(), count)) return bad;
      remaining -= count;
    }
    written += 8 + stored;
    offset = next;
  }
  if (written != foreign.bytes) return bad;
  if (!verify) { output.flush(); if (!output) return bad; }
  return {};
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

StorageDiagnostic ValidateExistingAudio(const fs::path &path, AudioAction action, Checkpoint check) {
  const auto invalid = StorageDiagnostic{StorageReason::ValidationFailed, "Existing audio is incomplete or invalid"};
  const auto cancelled = StorageDiagnostic{StorageReason::Cancelled, "Cancelled"};
  if (check && !check()) return cancelled;
  SF_INFO info{};
  auto audio = openRead(path, info);
  const bool flac = action == AudioAction::LosslessFlac;
  if (!audio || info.frames <= 0 || info.channels <= 0 || info.channels > 32 || info.samplerate <= 0) return invalid;
  if ((!flac && action != AudioAction::LossyVorbis) ||
      (info.format & SF_FORMAT_TYPEMASK) != (flac ? SF_FORMAT_FLAC : SF_FORMAT_OGG) ||
      (!flac && (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_VORBIS))
    return {StorageReason::ValidationFailed, "Existing destination does not contain the requested audio codec"};
  std::ifstream input(path, std::ios::binary);
  std::error_code error;
  const auto bytes = fs::file_size(path, error);
  if (!input || error) return invalid;
  if (flac) {
    std::array<unsigned char, 34> header{};
    if (!input.read(reinterpret_cast<char *>(header.data()), 4) || std::memcmp(header.data(), "fLaC", 4)) return invalid;
    std::uint64_t offset = 4;
    bool first = true, last = false;
    while (!last) {
      if (check && !check()) return cancelled;
      if (!input.read(reinterpret_cast<char *>(header.data()), 4)) return invalid;
      last = (header[0] & 128) != 0;
      const auto type = header[0] & 127;
      const std::uint32_t length = (header[1] << 16) | (header[2] << 8) | header[3];
      offset += 4;
      if (offset > bytes || length > bytes - offset || type == 127) return invalid;
      if (first) {
        if (type != 0 || length != 34 || !input.read(reinterpret_cast<char *>(header.data()), 34)) return invalid;
        const std::uint64_t frames = (std::uint64_t(header[13] & 15) << 32) |
            (std::uint64_t(header[14]) << 24) | (std::uint64_t(header[15]) << 16) |
            (std::uint64_t(header[16]) << 8) | header[17];
        if (!frames || frames != static_cast<std::uint64_t>(info.frames)) return invalid;
      } else {
        if (type == 0) return invalid;
        // The target is retained verbatim, so no metadata whitelist is needed.
        input.seekg(length, std::ios::cur);
      }
      first = false; offset += length;
    }
    if (offset >= bytes) return invalid;
    audio.reset();
    return validateFlacFrames(path, info, check);
  } else {
    // Verify every Ogg page, including EOS: tolerant decoders may otherwise
    // accept a stream cut cleanly at a page boundary. Scratch stays under 64 KiB.
    static constexpr auto crcTable = [] {
      std::array<std::uint32_t, 256> table{};
      for (std::uint32_t i = 0; i < table.size(); ++i) {
        auto value = i << 24;
        for (int bit = 0; bit < 8; ++bit) value = (value << 1) ^ ((value & 0x80000000u) ? 0x04c11db7u : 0);
        table[i] = value;
      }
      return table;
    }();
    std::uint64_t offset = 0;
    std::uint32_t serial = 0, sequence = 0;
    bool ended = false;
    while (offset < bytes) {
      if (check && !check()) return cancelled;
      std::vector<unsigned char> page(27);
      if (ended || !input.read(reinterpret_cast<char *>(page.data()), 27) ||
          std::memcmp(page.data(), "OggS", 4) || page[4] != 0 || (page[5] & ~7)) return invalid;
      const auto flags = page[5];
      if (!offset) serial = le32(page.data() + 14);
      if (le32(page.data() + 14) != serial || le32(page.data() + 18) != sequence++ ||
          bool(flags & 2) != (offset == 0)) return invalid;
      const auto segments = page[26];
      page.resize(27 + segments);
      if (!input.read(reinterpret_cast<char *>(page.data() + 27), segments)) return invalid;
      std::size_t payload = 0;
      for (std::size_t i = 27; i < page.size(); ++i) payload += page[i];
      const auto headerBytes = page.size();
      page.resize(headerBytes + payload);
      if (!input.read(reinterpret_cast<char *>(page.data() + headerBytes), payload)) return invalid;
      const auto expected = le32(page.data() + 22);
      std::fill(page.begin() + 22, page.begin() + 26, 0);
      std::uint32_t crc = 0;
      for (const auto byte : page) crc = (crc << 8) ^ crcTable[(crc >> 24) ^ byte];
      if (crc != expected) return invalid;
      offset += page.size(); ended = (flags & 4) != 0;
      if (ended) {
        const std::uint64_t frames = le32(page.data() + 6) | (std::uint64_t(le32(page.data() + 10)) << 32);
        if (frames != static_cast<std::uint64_t>(info.frames)) return invalid;
      }
    }
    if (!ended || offset != bytes) return invalid;
  }
  std::vector<float> samples(4096 * info.channels);
  sf_count_t frames = 0;
  for (;;) {
    if (check && !check()) return cancelled;
    const auto count = sf_readf_float(audio.get(), samples.data(), 4096);
    if (!count) break;
    if (count < 0 || count > info.frames - frames ||
        !std::all_of(samples.begin(), samples.begin() + count * info.channels, [](float sample) { return std::isfinite(sample); }))
      return invalid;
    frames += count;
  }
  return sf_error(audio.get()) || frames != info.frames ? invalid : StorageDiagnostic{};
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
  int channelMap[2]{};
  if (sf_command(input.get(), SFC_GET_CHANNEL_MAP_INFO, channelMap, info.channels * sizeof(int))) {
    const bool mono = info.channels == 1 && (channelMap[0] == SF_CHANNEL_MAP_MONO ||
        channelMap[0] == SF_CHANNEL_MAP_CENTER || channelMap[0] == SF_CHANNEL_MAP_FRONT_CENTER);
    const bool stereo = info.channels == 2 &&
        (channelMap[0] == SF_CHANNEL_MAP_LEFT || channelMap[0] == SF_CHANNEL_MAP_FRONT_LEFT) &&
        (channelMap[1] == SF_CHANNEL_MAP_RIGHT || channelMap[1] == SF_CHANNEL_MAP_FRONT_RIGHT);
    if (!mono && !stereo)
      return finish(FileOutcome::Skipped, StorageReason::UnsupportedFormat, "Native channel assignments cannot be preserved");
  }
  const bool lossless = policy.audio == AudioAction::LosslessFlac;
  WaveMetadata foreign;
  const auto container = inspectContainer(candidate.source, info, input.get(), check, lossless, foreign);
  if (container.code == StorageReason::Cancelled) return cancelled();
  if (container.code != StorageReason::None)
    return finish(container.code == StorageReason::ValidationFailed ? FileOutcome::Failed : FileOutcome::Skipped,
                  container.code, container.detail);
  if (info.frames <= 0) return finish(FileOutcome::Failed, StorageReason::ValidationFailed, "Empty audio");
  if ((info.format & SF_FORMAT_TYPEMASK) == SF_FORMAT_FLAC) {
    if (progress) progress({StoragePhase::Verify, candidate.source});
    const auto validation = validateFlacFrames(candidate.source, info, check);
    if (validation.code == StorageReason::Cancelled) return cancelled();
    if (validation.code != StorageReason::None)
      return finish(FileOutcome::Failed, validation.code, validation.detail);
  }
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
  if (foreign.preserve) {
    const auto metadata = preserveWaveMetadata(candidate.source, temporaryOutput, foreign, check, false);
    if (metadata.code == StorageReason::Cancelled) return cancelled();
    if (metadata.code != StorageReason::None) return finish(FileOutcome::Failed, metadata.code, metadata.detail);
  }
  if (progress) progress({StoragePhase::Verify, candidate.source});
  if (foreign.preserve) {
    const auto metadata = preserveWaveMetadata(candidate.source, temporaryOutput, foreign, check, true);
    if (metadata.code == StorageReason::Cancelled) return cancelled();
    if (metadata.code != StorageReason::None) return finish(FileOutcome::Failed, metadata.code, metadata.detail);
  }
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
  for (int tag = SF_STR_FIRST; tag <= SF_STR_LAST; ++tag) {
    if (const char *value = sf_get_string(input.get(), tag)) {
      const char *encoded = sf_get_string(verify.get(), tag);
      // libsndfile appends its version to an editor attribution on write.
      // Retain the original attribution while permitting that exact suffix.
      const bool softwareAttribution = tag == SF_STR_SOFTWARE && encoded &&
          std::string(encoded) == (*value ? std::string(value) + " (" + sf_version_string() + ")" : sf_version_string());
      if (!encoded || (std::strcmp(value, encoded) && !softwareAttribution)) valid = false;
    }
  }
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
  if (!lossless && outputBytes >= sourceBytes) return finish(FileOutcome::Skipped, StorageReason::NotSmaller, "Encoded audio is not smaller");
  return finish(FileOutcome::Converted, StorageReason::None, "");
}
} // namespace storage_management
