#pragma once

#include "audio/AudioMix.h"
#include "audio/BmsonAudio.h"
#include "audio/SoundFileIO.h"
#include "audio/decoder.h"
#include "scene/play/ReplayKeysoundSchedule.h"
#include <cmath>
#include <map>

// The oracle retains each native channel through slicing and device-rate
// preparation, then uses the live mixer to fold channels and apply controls.
struct BmsonMultichannelFixture {
  std::unique_ptr<bms_parser::Chart> chart;
  ReplayData replay{.provenance = {}};
  std::map<int, std::shared_ptr<SoundData>> sounds;

  BmsonMultichannelFixture(const std::filesystem::path &root, int channels,
                           int sourceRate, int isolatedChannel, bool layered) {
    std::filesystem::create_directories(root);
    for (const bool layer : {false, true}) {
      SF_INFO info{};
      info.channels = channels;
      info.samplerate = sourceRate;
      info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
      auto file = asobmashow::audio::openSoundFileHandle(
          root / (layer ? "layer.wav" : "stem.wav"), SFM_WRITE, info);
      std::vector<short> pcm(sourceRate * channels);
      for (int frame = 0; frame < sourceRate; ++frame)
        for (int channel = 0; channel < channels; ++channel)
          if (isolatedChannel < 0 || isolatedChannel == channel)
            pcm[frame * channels + channel] = static_cast<short>(
                (layer ? 3000 : 24000) * std::sin(frame * (layer ? 0.29 : 0.37) + channel * 0.13));
      if (!file || sf_writef_short(file.get(), pcm.data(), sourceRate) != sourceRate)
        throw std::runtime_error("multichannel WAV fixture failed");
    }
    const std::string notes = R"({"x":3,"y":0},{"x":1,"y":48,"l":144,"c":true})";
    const std::string json = std::string(R"({"version":"1.0.0","info":{"init_bpm":120,"ln_type":3},
      "sound_channels":[{"name":"stem.wav","notes":[)") + notes +
      R"(,{"x":2,"y":48,"c":true},{"x":4,"y":264,"c":true}]})" +
      (layered ? std::string(R"(,{"name":"layer.wav","notes":[)") + notes +
          R"(,{"x":4,"y":264,"c":true}]})" : "") + "]}";
    bms_parser::Parser parser;
    bms_parser::Chart *raw = nullptr;
    std::atomic_bool cancelled{false};
    parser.Parse(std::vector<unsigned char>(json.begin(), json.end()), &raw, false, false,
                 cancelled, "multichannel.bmson");
    chart.reset(raw);
    if (!chart) throw std::runtime_error("multichannel bmson fixture failed");
    chart->Meta.Folder = root;
    replay.chartMeta = chart->Meta;
    replay.events = {
        {.action = ReplayEventAction::Press, .lane = 0, .noteTimeMicros = 100'000,
         .songTimeMicros = 100'020, .judgement = PGreat},
        {.action = ReplayEventAction::Release, .lane = 0, .noteTimeMicros = 400'000,
         .songTimeMicros = 160'000, .judgement = Poor},
        {.action = ReplayEventAction::Press, .lane = 0, .songTimeMicros = 230'090},
        {.action = ReplayEventAction::Press, .lane = 1, .songTimeMicros = 280'043},
        {.action = ReplayEventAction::Release, .lane = 0, .songTimeMicros = 300'110},
        {.action = ReplayEventAction::Press, .lane = 0, .songTimeMicros = 320'130}};
    std::map<int, std::shared_ptr<SoundData>> slices;
    for (const auto &[wav, recipe] : chart->WavSlices) {
      auto group = std::make_shared<SoundData>();
      for (const auto &slice : recipe) {
        auto &sound = slices[slice.SliceId];
        if (!sound) {
          SF_INFO info{};
          std::vector<short> decoded;
          if (!decodeAudioToPCM(fspath_to_path_t(root / chart->WavTable.at(slice.SourceWav)),
                                 decoded, info, cancelled))
            throw std::runtime_error("multichannel source decode failed");
          const auto range = audio::bmson::sourceRange(slice.StartMicros, slice.DurationMicros,
                                                       info.samplerate, info.frames);
          sound = std::make_shared<SoundData>();
          sound->channels = channels;
          sound->monophonic = true;
          sound->outputData = audio::ResamplePcm(
              std::span(decoded).subspan(range.start * channels, range.count * channels),
              channels, sourceRate, 44100);
          sound->outputFrameCount = sound->outputData.size() / channels;
        }
        group->layers.push_back(sound);
      }
      sounds[wav] = group;
    }
  }

  std::vector<ScheduledAudioEvent> replaySchedule() const {
    return buildReplayKeysoundSchedule(gameplay::buildGameplayDefinition(*chart, 0),
                                       replay.events, 0, std::nullopt);
  }

  std::vector<ScheduledAudioEvent> chartSchedule() const {
    std::vector<ScheduledAudioEvent> result;
    for (const auto *measure : chart->Measures)
      for (const auto *timeline : measure->TimeLines) {
        for (const auto *note : timeline->Notes)
          if (note && note->Wav != bms_parser::Parser::NoWav)
            result.push_back({.timeMicros = timeline->Timing, .wav = note->Wav,
                              .bus = audio::Bus::Keysound});
        for (const auto *note : timeline->BackgroundNotes)
          if (note && note->Wav != bms_parser::Parser::NoWav)
            result.push_back({.timeMicros = timeline->Timing, .wav = note->Wav,
                              .bus = audio::Bus::Bgm});
      }
    return result;
  }

  std::vector<float> continuous(std::span<const ScheduledAudioEvent> schedule,
                                 int speed, std::size_t frames) const {
    AudioCallbackState state;
    std::uint64_t sequence = 0;
    for (const auto &event : schedule)
      if (!audio::playback::InsertScheduledSound(state, {.soundData = sounds.at(event.wav).get(),
          .bus = event.bus, .startMicros = event.timeMicros, .sequence = sequence++,
          .gain = event.gain, .voiceId = event.voiceId, .setVoiceGain = event.setVoiceGain}))
        throw std::runtime_error("multichannel mixer schedule failed");
    std::vector<float> pcm(frames * 2);
    for (std::size_t offset = 0; offset < frames; offset += 257) {
      const auto count = static_cast<unsigned>(std::min<std::size_t>(257, frames - offset));
      audio::playback::ActivateScheduledSounds(state, 0, 44100, count, speed, offset);
      audio::playback::MixActiveSounds(state, std::span(pcm).subspan(offset * 2, count * 2),
                                       count, 2, 1, 1, speed);
    }
    return pcm;
  }

  static short pcm16(float liveSample) {
    return static_cast<short>(std::lrint(std::clamp(liveSample / 0.9F, -1.0F, 1.0F) * 32767));
  }
};
