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

inline std::unique_ptr<bms_parser::Chart>
bmsonSharedBusFixture(const std::filesystem::path &root, bool layered,
                      int chordKeys = 1) {
  std::filesystem::create_directories(root);
  const char *names[] = {"hold.wav", "shared.wav", "layer.wav"};
  const short amplitudes[] = {0, 10000, 1000};
  for (int index = 0; index < 3; ++index) {
    SF_INFO info{};
    info.channels = 1;
    info.samplerate = 44100;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    auto file = asobmashow::audio::openSoundFileHandle(root / names[index], SFM_WRITE, info);
    const std::vector<short> pcm(44100, amplitudes[index]);
    if (!file || sf_writef_short(file.get(), pcm.data(), pcm.size()) != static_cast<sf_count_t>(pcm.size()))
      throw std::runtime_error("bmson shared-bus WAV fixture failed");
  }
  const auto path = root / "shared-bus.bmson";
  {
    std::ofstream out(path);
    // The note inside the hold becomes BGM. Its slice also belongs to the
    // following lanes, either directly or as one layer of a composite WAV.
    // Sixteen playable keys plus BGM cross libstdc++'s small-sort boundary.
    out << R"({"version":"1.0.0","info":{"init_bpm":120)";
    if (chordKeys > 1) out << R"(,"mode_hint":"keyboard-24k")";
    out << R"(},"sound_channels":[{"name":"hold.wav","notes":[{"x":1,"y":0,"l":480}]},{"name":"shared.wav","notes":[{"x":1,"y":240})";
    for (int x = 2; x <= chordKeys + 1; ++x)
      out << ",{\"x\":" << x << ",\"y\":240}";
    out << "]}";
    if (layered) out << R"(,{"name":"layer.wav","notes":[{"x":2,"y":240}]})";
    out << "]}";
  }
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(path, &raw, false, false, cancelled);
  if (!raw) throw std::runtime_error("bmson shared-bus chart fixture failed");
  return std::unique_ptr<bms_parser::Chart>(raw);
}
