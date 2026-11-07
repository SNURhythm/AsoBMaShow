#pragma once

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <string>
#include <sys/types.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <filesystem>
#include <io.h>
#else
#include <unistd.h>
#endif

namespace video {

// Media supplied by charts and skins is one selected file. Demuxers must not
// interpret its contents as permission to open other files, URLs, or devices.
inline int restrictMediaInput(AVFormatContext *format) {
  format->io_open = +[](AVFormatContext *, AVIOContext **, const char *, int,
                       AVDictionary **) { return AVERROR(EACCES); };
  // Some demuxers create child contexts without inheriting io_open, but copy
  // this whitelist. An empty list rejects every protocol in those children.
  int result = av_opt_set(format, "protocol_whitelist", "", 0);
  if (result < 0) return result;
  try {
    static const std::string formats = [] {
      std::string names;
      void *cursor = nullptr;
      while (const auto *input = av_demuxer_iterate(&cursor)) {
        // AVFMT_NOFILE includes devices and image sequences that may perform
        // their own file discovery. Playlists are not single-file assets either.
        if ((input->flags & AVFMT_NOFILE) ||
            std::strcmp(input->name, "concat") == 0 ||
            std::strcmp(input->name, "hls") == 0 ||
            std::strcmp(input->name, "dash") == 0) continue;
        if (!names.empty()) names += ',';
        names += input->name;
      }
      return names;
    }();
    return av_opt_set(format, "format_whitelist", formats.c_str(), 0);
  } catch (const std::bad_alloc &) {
    return AVERROR(ENOMEM);
  }
}

inline int openRegularMediaIo(AVIOContext **io, const char *path,
                              const AVIOInterruptCB *interrupt) {
  int descriptor = -1;
#ifdef _WIN32
  try {
    descriptor = _wopen(std::filesystem::u8path(path).c_str(),
                        _O_RDONLY | _O_BINARY | _O_NOINHERIT);
  } catch (const std::bad_alloc &) {
    return AVERROR(ENOMEM);
  } catch (const std::filesystem::filesystem_error &) {
    return AVERROR(EINVAL);
  }
#else
  // Opening a FIFO must not wait for a writer before we can reject its type.
  descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
#endif
  if (descriptor < 0) return AVERROR(errno);
#ifdef _WIN32
  struct _stat64 status{};
  const int inspected = _fstat64(descriptor, &status);
  const bool regular = (status.st_mode & _S_IFMT) == _S_IFREG;
#else
  struct stat status{};
  const int inspected = fstat(descriptor, &status);
  const bool regular = S_ISREG(status.st_mode);
#endif
  int result = inspected < 0 ? AVERROR(errno) : regular ? 0 : AVERROR(EACCES);
  if (result >= 0) {
    AVDictionary *options = nullptr;
    result = av_dict_set_int(&options, "fd", descriptor, 0);
    if (result >= 0) {
      // FFmpeg duplicates this validated descriptor. Never reopen the pathname
      // after fstat: it could have changed, including /proc/self/fd references.
      result = avio_open2(io, "fd:", AVIO_FLAG_READ, interrupt, &options);
    }
    av_dict_free(&options);
  }
#ifdef _WIN32
  _close(descriptor);
#else
  close(descriptor);
#endif
  return result;
}

// Matches avformat_open_input ownership: on failure *format is null; on
// success avformat_close_input owns both the context and its selected file.
inline int openLocalMediaInput(AVFormatContext **format, const char *path,
                              const AVInputFormat *input = nullptr) {
  if (!*format) *format = avformat_alloc_context();
  if (!*format) return AVERROR(ENOMEM);
  AVIOContext *io = nullptr;
  int result = restrictMediaInput(*format);
  if (result >= 0) {
    result = openRegularMediaIo(&io, path, &(*format)->interrupt_callback);
  }
  if (result < 0) {
    avio_closep(&io);
    avformat_free_context(*format);
    *format = nullptr;
    return result;
  }
  (*format)->pb = io;
  result = avformat_open_input(format, path, input, nullptr);
  if (result < 0) {
    // Supplying pb makes it custom I/O during open, including error cleanup.
    avio_closep(&io);
  } else {
    (*format)->flags &= ~AVFMT_FLAG_CUSTOM_IO;
  }
  return result;
}

} // namespace video
