#pragma once

#include "bms_parser.hpp"
#include "audio/SoundFileIO.h"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

inline std::unique_ptr<bms_parser::Chart>
bmsonAudioFixture(const std::filesystem::path &root) {
  std::filesystem::create_directories(root);
  for (const bool layer : {false, true}) {
    SF_INFO info{};
    info.channels = 1;
    info.samplerate = 44100;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    auto file = asobmashow::audio::openSoundFileHandle(root / (layer ? "layer.wav" : "stem.wav"), SFM_WRITE, info);
    std::vector<short> pcm(layer ? 5512 : 44100, layer ? 500 : 1000);
    if (!layer) std::fill(pcm.begin() + 22050, pcm.end(), -2000);
    if (!file || sf_writef_short(file.get(), pcm.data(), pcm.size()) != static_cast<sf_count_t>(pcm.size()))
      throw std::runtime_error("bmson WAV fixture failed");
  }
  const auto path = root / "slices.BMSON";
  std::ofstream(path) << R"({"version":"1.0.0","info":{"title":"Sliced audio","init_bpm":120},"sound_channels":[{"name":"stem.wav","notes":[{"x":1,"y":0},{"x":2,"y":0},{"x":0,"y":0},{"x":2,"y":120,"c":true},{"x":3,"y":240,"c":true},{"x":4,"y":360}]},{"name":"layer.wav","notes":[{"x":1,"y":0}]}]})";
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(path, &raw, false, false, cancelled);
  if (!raw) throw std::runtime_error("bmson chart fixture failed");
  return std::unique_ptr<bms_parser::Chart>(raw);
}
