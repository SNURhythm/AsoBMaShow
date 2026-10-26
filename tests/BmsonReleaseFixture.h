#pragma once

#include "bms_parser.hpp"
#include <atomic>
#include <memory>
#include <stdexcept>

inline std::unique_ptr<bms_parser::Chart> bmsonReleaseFixture(
    int lnType = 2, int x = 1, bool tailSound = true, int length = 480) {
  const std::string input =
      "{\"version\":\"1.0.0\",\"info\":{\"init_bpm\":120,\"ln_type\":" +
      std::to_string(lnType) + "},\"sound_channels\":[{\"name\":\"head.wav\",\"notes\":[{\"x\":" +
      std::to_string(x) + ",\"y\":480,\"l\":" + std::to_string(length) + "}]}" +
      (tailSound ? ",{\"name\":\"tail.wav\",\"notes\":[{\"x\":" +
          std::to_string(x) + ",\"y\":" + std::to_string(480 + length) + ",\"up\":true}]}" : "") + "]}";
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(std::vector<unsigned char>(input.begin(), input.end()), &raw,
               false, false, cancelled, "release.bmson");
  if (!raw) throw std::runtime_error("bmson release fixture failed");
  return std::unique_ptr<bms_parser::Chart>(raw);
}

inline std::unique_ptr<bms_parser::Chart> bmsonReleaseWithSentinelFixture(
    int lnType = 1, bool tailSound = true) {
  auto chart = bmsonReleaseFixture(lnType, 8, tailSound);
  // Keep gameplay active after the scratch tail so later empty presses are
  // tested independently of the chart-completion gate.
  auto *timeline = new bms_parser::TimeLine(8, false);
  timeline->Timing = 5'000'000;
  timeline->SetNote(0, new bms_parser::Note(bms_parser::Parser::NoWav));
  chart->Measures.back()->TimeLines.push_back(timeline);
  ++chart->Meta.TotalNotes;
  chart->Meta.PlayLength = chart->Meta.TotalLength = timeline->Timing;
  return chart;
}

inline std::unique_ptr<bms_parser::Chart> bmsonAdjacentHcnFixture(
    bool nextHcn, bool scratch = false) {
  const auto x = std::to_string(scratch ? 8 : 1);
  // Authored notes do not overlap: HCN 1--2s, next note at 2.1s. An early
  // judgement of the next note can still occur inside the first HCN's body.
  const std::string input =
      "{\"version\":\"1.0.0\",\"info\":{\"init_bpm\":120,\"ln_type\":3},"
      "\"sound_channels\":[{\"name\":\"head.wav\",\"notes\":[{\"x\":" + x +
      ",\"y\":480,\"l\":480}]},{\"name\":\"next.wav\",\"notes\":[{\"x\":" + x +
      ",\"y\":1008,\"l\":" + std::to_string(nextHcn ? 240 : 0) + "}]}" +
      (nextHcn ? ",{\"name\":\"tail.wav\",\"notes\":[{\"x\":" + x +
          ",\"y\":1248,\"up\":true}]}" : "") +
      ",{\"name\":\"sentinel.wav\",\"notes\":[{\"x\":4,\"y\":2400}]}]}";
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(std::vector<unsigned char>(input.begin(), input.end()), &raw,
               false, false, cancelled, "adjacent-hcn.bmson");
  if (!raw) throw std::runtime_error("adjacent HCN fixture failed");
  return std::unique_ptr<bms_parser::Chart>(raw);
}

inline std::unique_ptr<bms_parser::Chart> bmsonLateClassicFixture(bool scratch = false) {
  const auto x = std::to_string(scratch ? 8 : 1);
  const std::string input =
      "{\"version\":\"1.0.0\",\"info\":{\"init_bpm\":120,\"ln_type\":1},"
      "\"sound_channels\":[{\"name\":\"head.wav\",\"notes\":[{\"x\":" + x +
      ",\"y\":480,\"l\":24}]},{\"name\":\"next.wav\",\"notes\":[{\"x\":" + x +
      ",\"y\":720}]},{\"name\":\"sentinel.wav\",\"notes\":[{\"x\":4,\"y\":2400}]}]}";
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(std::vector<unsigned char>(input.begin(), input.end()), &raw,
               false, false, cancelled, "late-classic.bmson");
  if (!raw) throw std::runtime_error("late classic fixture failed");
  return std::unique_ptr<bms_parser::Chart>(raw);
}

inline std::unique_ptr<bms_parser::Chart> bmsonScratchHcnAuxiliaryFixture() {
  const std::string input = R"({"version":"1.0.0","info":{"init_bpm":120,"ln_type":3,"mode_hint":"beat-14k"},
    "sound_channels":[{"name":"head.wav","notes":[{"x":8,"y":480,"l":960},
    {"x":9,"y":480,"l":960}]},{"name":"sentinel.wav","notes":[{"x":4,"y":2400}]}]})";
  bms_parser::Parser parser;
  bms_parser::Chart *raw = nullptr;
  std::atomic_bool cancelled{false};
  parser.Parse(std::vector<unsigned char>(input.begin(), input.end()), &raw,
               false, false, cancelled, "scratch-hcn-auxiliary.bmson");
  if (!raw) throw std::runtime_error("scratch HCN auxiliary fixture failed");
  return std::unique_ptr<bms_parser::Chart>(raw);
}
