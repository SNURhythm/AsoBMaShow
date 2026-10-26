#pragma once

#include "BmsonReleaseFixture.h"
#include "audio/AudioMix.h"
#include "audio/BmsonAudio.h"
#include "audio/SoundFileIO.h"
#include "audio/decoder.h"
#include "scene/play/ReplayKeysoundSchedule.h"
#include <cmath>
#include <map>

// Parsed, independently decoded voices for comparisons with the production
// mixer. The oracle renders continuously from chart zero, then crops output
// frames; it never reconstructs a seek cursor or an export source interval.
struct BmsonPhaseFixture {
  std::unique_ptr<bms_parser::Chart> chart;
  ReplayData replay{.provenance = {}};
  std::map<int, std::shared_ptr<SoundData>> sounds;
  std::vector<ScheduledAudioEvent> schedule;

  BmsonPhaseFixture(const std::filesystem::path &root, int sourceRate,
                    long long trigger, long long retrigger = 0,
                    bool controlled = false, bool layered = false, bool continued = false, int frameLimit = 0, bool bounded = false) {
    std::filesystem::create_directories(root);
    for (const auto name : {"head.wav", "layer.wav"}) {
      SF_INFO info{};
      info.channels = 1;
      info.samplerate = sourceRate;
      info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
      auto file = asobmashow::audio::openSoundFileHandle(root / name, SFM_WRITE, info);
      const bool layer = std::string_view(name) == "layer.wav";
      std::vector<short> pcm(sourceRate * ((layer ? 115 : 135) + (continued ? 100 : 0)) / 100);
      if (frameLimit > 0) pcm.resize(frameLimit);
      for (std::size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<short>((layer ? 3000 : 9000) * std::sin(i * (layer ? 0.47 : 0.7)));
      if (!file || sf_writef_short(file.get(), pcm.data(), pcm.size()) != static_cast<sf_count_t>(pcm.size()))
        throw std::runtime_error("phase WAV fixture failed");
    }
    const std::string prefix = continued ? R"({"x":3,"y":1},)" : "";
    const std::string boundary = bounded ? R"(,{"x":3,"y":959,"c":true})" : "";
    const std::string continuation = continued ? R"(,"c":true)" : "";
    const std::string json = std::string(R"({"version":"1.0.0","info":{"init_bpm":120,"ln_type":3},
      "sound_channels":[{"name":"head.wav","notes":[)") + prefix +
      R"({"x":1,"y":480,"l":)" + (controlled ? "960" : "0") + continuation +
      R"(},{"x":2,"y":480)" + continuation + "}" + boundary + "]}" +
      (layered ? std::string(R"(,{"name":"layer.wav","notes":[)") + prefix +
          R"({"x":1,"y":480,"l":960)" + continuation + "}" + boundary + "]}" : "") +
      R"(,{"name":"sentinel.wav","notes":[{"x":4,"y":2400}]}]})";
    bms_parser::Parser parser;
    bms_parser::Chart *raw = nullptr;
    std::atomic_bool cancelled{false};
    parser.Parse(std::vector<unsigned char>(json.begin(), json.end()), &raw, false, false,
                 cancelled, "phase.bmson");
    chart.reset(raw);
    if (!chart) throw std::runtime_error("phase bmson fixture failed");
    chart->Meta.Folder = root;
    replay.chartMeta = chart->Meta;
    replay.events = {{.action = ReplayEventAction::Press, .lane = 0,
        .noteTimeMicros = 1'000'000, .songTimeMicros = trigger, .judgement = PGreat}};
    if (controlled) {
      replay.events.push_back({.action = ReplayEventAction::Release, .lane = 0,
          .noteTimeMicros = 3'000'000, .songTimeMicros = 1'200'000, .judgement = Poor});
      replay.events.push_back({.action = ReplayEventAction::Press, .lane = 0,
          .songTimeMicros = 1'500'090});
    }
    if (retrigger) replay.events.push_back({.action = ReplayEventAction::Press, .lane = 1,
        .songTimeMicros = retrigger});
    if (controlled) {
      replay.events.push_back({.action = ReplayEventAction::Release, .lane = 0,
          .songTimeMicros = 1'700'110});
      replay.events.push_back({.action = ReplayEventAction::Press, .lane = 0,
          .songTimeMicros = 1'800'130});
    }
    const auto definition = gameplay::buildGameplayDefinition(*chart, 0);
    schedule = buildReplayKeysoundSchedule(definition, replay.events, 0, std::nullopt);
    std::map<int, std::shared_ptr<SoundData>> slices;
    for (const auto &[wav, recipe] : chart->WavSlices) {
      std::vector<std::shared_ptr<SoundData>> layers;
      for (const auto &slice : recipe) {
        auto &sound = slices[slice.SliceId];
        if (!sound) {
          const auto name = chart->WavTable.at(slice.SourceWav);
          if (name == "sentinel.wav") continue;
          SF_INFO info{};
          std::vector<short> decoded;
          if (!decodeAudioToPCM(fspath_to_path_t(root / name), decoded, info, cancelled))
            throw std::runtime_error("phase source decode failed");
          const auto range = audio::bmson::sourceRange(slice.StartMicros, slice.DurationMicros,
                                                       info.samplerate, info.frames);
          sound = std::make_shared<SoundData>();
          sound->channels = 1;
          sound->monophonic = true;
          sound->outputData = audio::ResamplePcm(std::span(decoded).subspan(range.start, range.count),
                                                1, info.samplerate, 44100);
          sound->outputFrameCount = sound->outputData.size();
        }
        layers.push_back(sound);
      }
      if (layers.empty()) continue;
      if (layers.size() == 1) sounds[wav] = layers.front();
      else {
        auto group = std::make_shared<SoundData>();
        group->layers = std::move(layers);
        sounds[wav] = std::move(group);
      }
    }
  }

  static long long frameAt(long long time, int speed) {
    return static_cast<long long>(std::floor(static_cast<long double>(time) * 44100 * 100 /
                                             (1'000'000 * speed) + 0.5L));
  }

  std::vector<float> continuous(int speed, std::size_t frames, unsigned chunk = 257) {
    AudioCallbackState state;
    std::uint64_t sequence = 0;
    for (const auto &event : schedule) {
      if (!audio::playback::InsertScheduledSound(state, {.soundData = sounds.at(event.wav).get(),
          .bus = event.bus, .startMicros = event.timeMicros, .sequence = sequence++,
          .gain = event.gain, .voiceId = event.voiceId, .setVoiceGain = event.setVoiceGain}))
        throw std::runtime_error("phase oracle scheduling failed");
    }
    std::vector<float> result(frames);
    for (std::size_t offset = 0; offset < frames; offset += chunk) {
      const auto count = static_cast<unsigned>(std::min<std::size_t>(chunk, frames - offset));
      audio::playback::ActivateScheduledSounds(state, 0, 44100, count, speed, offset);
      audio::playback::MixActiveSounds(state, std::span(result).subspan(offset, count),
                                       count, 1, 1, 1, speed);
    }
    return result;
  }
};
