#include "bms_parser.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
struct Digest {
  std::uint64_t value = 14695981039346656037ULL;
  template <typename T> void add(const T &item) {
    static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>);
    const auto *bytes = reinterpret_cast<const unsigned char *>(&item);
    for (std::size_t i = 0; i < sizeof(T); ++i) {
      value ^= bytes[i]; value *= 1099511628211ULL;
    }
  }
  void add(const std::string &item) {
    add(item.size());
    for (const auto byte : item) add(static_cast<unsigned char>(byte));
  }
  void add(const std::filesystem::path &item) { add(item.generic_string()); }
  template <typename T> void optional(const std::optional<T> &item) {
    add(item.has_value());
    if (item) add(*item);
  }
};

void digestNote(Digest &digest, bms_parser::Note *note) {
  digest.add(note != nullptr);
  if (!note) return;
  digest.add(note->Lane); digest.add(note->Wav);
  digest.add(note->IsPlayed); digest.add(note->IsDead); digest.add(note->PlayedTime);
  digest.add(note->IsLongNote()); digest.add(note->IsLandmineNote());
  if (note->IsLongNote()) {
    auto *ln = static_cast<bms_parser::LongNote *>(note);
    digest.add(ln->IsTail()); digest.add(ln->GetType());
    const auto *partner = ln->IsTail() ? ln->Head : ln->Tail;
    digest.add(partner != nullptr);
    if (partner) {
      digest.add(partner->Lane); digest.add(partner->Wav);
      digest.add(partner->Timeline->Timing);
    }
  }
  if (note->IsLandmineNote())
    digest.add(static_cast<bms_parser::LandmineNote *>(note)->Damage);
}

std::uint64_t chartDigest(const bms_parser::Chart &chart) {
  Digest digest;
  const auto &m = chart.Meta;
#define META(field) digest.add(m.field)
  META(SHA256); META(MD5); META(BmsPath); META(Folder);
  META(Artist); META(SubArtist); META(Bpm); META(Genre); META(Title); META(SubTitle);
  META(Rank); META(RankType); META(Total); META(HasTotal);
  META(PlayLength); META(TotalLength); META(Banner); META(StageFile);
  META(BackBmp); META(Preview); META(BgaPoorDefault); META(Difficulty);
  META(PlayLevel); META(PlayLevelText); META(MinBpm); META(MaxBpm);
  META(MostPrevalentBpm); META(GuessedBeatBpm); META(GuessedBeatsPerMeasure);
  META(Player); META(KeyMode); META(IsDP); META(TotalNotes); META(TotalLongNotes);
  META(TotalScratchNotes); META(TotalBackSpinNotes); META(TotalLandmineNotes); META(LnMode);
#undef META
  digest.optional(m.RandomSeed); digest.optional(m.RandomPrng);
  digest.add(m.RandomValues.size());
  for (const auto value : m.RandomValues) digest.add(value);
  for (const auto *table : {&chart.WavTable, &chart.ReferencedWavTable,
                            &chart.BmpTable, &chart.ReferencedBmpTable}) {
    const std::map<int, std::string> ordered(table->begin(), table->end());
    digest.add(ordered.size());
    for (const auto &[id, path] : ordered) { digest.add(id); digest.add(path); }
  }
  digest.add(chart.Measures.size());
  for (const auto *measure : chart.Measures) {
    digest.add(measure->Scale); digest.add(measure->Timing); digest.add(measure->Pos);
    digest.add(measure->TimeLines.size());
    for (const auto *t : measure->TimeLines) {
#define TIMELINE(field) digest.add(t->field)
      TIMELINE(Bpm); TIMELINE(BpmChange); TIMELINE(BpmChangeApplied);
      TIMELINE(ScrollChange); TIMELINE(HasSpeedObject); TIMELINE(BgaBase);
      TIMELINE(BgaLayer); TIMELINE(StopLength); TIMELINE(Scroll); TIMELINE(Speed);
      TIMELINE(Timing); TIMELINE(BeatPosition); TIMELINE(IsFirstInMeasure);
#undef TIMELINE
      digest.add(t->BgaPoor.has_value());
      if (t->BgaPoor) {
        digest.add(t->BgaPoor->Frames.size());
        for (const auto frame : t->BgaPoor->Frames) digest.add(frame);
      }
      for (const auto *notes : {&t->Notes, &t->InvisibleNotes, &t->BackgroundNotes}) {
        digest.add(notes->size());
        for (auto *note : *notes) digestNote(digest, note);
      }
      digest.add(t->LandmineNotes.size());
      for (auto *note : t->LandmineNotes) digestNote(digest, note);
    }
  }
  return digest.value;
}

std::unique_ptr<bms_parser::Chart> parse(const std::filesystem::path &path,
    const std::vector<unsigned char> &bytes, const std::string &mode) {
  bms_parser::Parser parser;
  parser.SetRandomSeed(20261001);
  std::atomic_bool cancelled{false};
  bms_parser::Chart *chart = nullptr;
  if (mode == "file") parser.Parse(path, &chart, false, false, cancelled);
  else parser.Parse(bytes, &chart, mode == "ready", mode == "metadata", cancelled);
  if (!chart) throw std::runtime_error("parser returned no chart");
  return std::unique_ptr<bms_parser::Chart>(chart);
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc != 5) throw std::runtime_error("usage: parser_benchmark FILE MODE ITERATIONS verify|time");
    const std::filesystem::path path = argv[1];
    const std::string mode = argv[2];
    if (mode != "full" && mode != "metadata" && mode != "file" && mode != "ready")
      throw std::runtime_error("unknown parse mode");
    const int iterations = std::stoi(argv[3]);
    if (iterations < 1) throw std::runtime_error("iterations must be positive");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open corpus file");
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
    if (std::string(argv[4]) == "verify") {
      const auto chart = parse(path, bytes, mode);
      std::cout << "digest=" << chartDigest(*chart) << " notes=" << chart->Meta.TotalNotes
                << " measures=" << chart->Measures.size() << '\n';
      return 0;
    }
    for (int warm = 0; warm < 3; ++warm) parse(path, bytes, mode);
    std::uint64_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
      std::atomic_signal_fence(std::memory_order_seq_cst);
      const auto chart = parse(path, bytes, mode);
      checksum += chart->Meta.TotalNotes + chart->Measures.size() + chart->Meta.TotalLength;
    }
    const double micros = std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - start).count() / iterations;
    std::cout << std::setprecision(10) << "us=" << micros << " checksum=" << checksum << '\n';
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
