#pragma once

#include "../RAII.h"

#include <filesystem>
#include <sndfile.h>

namespace asobmashow::audio {

using SoundFileHandle = UniqueResource<SNDFILE, sf_close>;

inline SNDFILE *openSoundFile(const std::filesystem::path &path, int mode,
                              SF_INFO &info) {
#ifdef _WIN32
  return sf_wchar_open(path.wstring().c_str(), mode, &info);
#else
  return sf_open(path.string().c_str(), mode, &info);
#endif
}

inline SoundFileHandle openSoundFileHandle(const std::filesystem::path &path,
                                           int mode, SF_INFO &info) {
  return SoundFileHandle(openSoundFile(path, mode, info));
}

} // namespace asobmashow::audio
