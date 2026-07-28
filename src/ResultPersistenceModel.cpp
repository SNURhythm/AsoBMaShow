#include "ResultPersistenceModel.h"

#include "BmsMetadataText.h"
#include "CanonicalDigest.h"
#include "FileChecksum.h"
#include "Utils.h"
#include "Uuid.h"
#include "path.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace result_persistence {
namespace {

using asobmshow::bms_metadata::normalizedHash;

class CanonicalEncoder {
public:
  void boolean(bool value) {
    bytes_.push_back(value ? std::byte{1} : std::byte{0});
  }

  template <typename Integer>
    requires(std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>)
  void integer(Integer value) {
    using Unsigned = std::make_unsigned_t<Integer>;
    const Unsigned bits = static_cast<Unsigned>(value);
    for (std::size_t offset = 0; offset < sizeof(Unsigned); ++offset) {
      const std::size_t shift = (sizeof(Unsigned) - offset - 1U) * 8U;
      bytes_.push_back(static_cast<std::byte>((bits >> shift) &
                                              static_cast<Unsigned>(0xffU)));
    }
  }

  template <typename Enum>
    requires std::is_enum_v<Enum>
  void enumeration(Enum value) {
    integer(static_cast<std::int32_t>(value));
  }

  void float32(float value) { integer(std::bit_cast<std::uint32_t>(value)); }
  void float64(double value) { integer(std::bit_cast<std::uint64_t>(value)); }

  void string(std::string_view value) {
    integer(static_cast<std::uint64_t>(value.size()));
    const auto raw = std::as_bytes(std::span(value.data(), value.size()));
    bytes_.insert(bytes_.end(), raw.begin(), raw.end());
  }

  void path(const std::filesystem::path &value) {
    string(fspath_to_utf8(value));
  }

  template <typename Value, typename Append>
  void optional(const std::optional<Value> &value, Append append) {
    boolean(value.has_value());
    if (value.has_value()) {
      append(*value);
    }
  }

  template <typename Value, typename Append>
  void vector(const std::vector<Value> &values, Append append) {
    integer(static_cast<std::uint64_t>(values.size()));
    for (const Value &value : values) {
      append(value);
    }
  }

  [[nodiscard]] std::string finish() const {
    file_checksum::Sha256 hash;
    hash.update(bytes_);
    return hash.finalHex();
  }

private:
  std::vector<std::byte> bytes_;
};

void appendChartMeta(CanonicalEncoder &encoder,
                     const bms_parser::ChartMeta &meta) {
  encoder.string(meta.SHA256);
  encoder.string(meta.MD5);
  encoder.path(meta.BmsPath);
  encoder.path(meta.Folder);
  encoder.string(meta.Artist);
  encoder.string(meta.SubArtist);
  encoder.float64(meta.Bpm);
  encoder.string(meta.Genre);
  encoder.string(meta.Title);
  encoder.string(meta.SubTitle);
  encoder.integer(static_cast<std::int32_t>(meta.Rank));
  encoder.float64(meta.Total);
  encoder.boolean(meta.HasTotal);
  encoder.integer(static_cast<std::int64_t>(meta.PlayLength));
  encoder.integer(static_cast<std::int64_t>(meta.TotalLength));
  encoder.path(meta.Banner);
  encoder.path(meta.StageFile);
  encoder.path(meta.BackBmp);
  encoder.path(meta.Preview);
  encoder.boolean(meta.BgaPoorDefault);
  encoder.integer(static_cast<std::int32_t>(meta.Difficulty));
  encoder.float64(meta.PlayLevel);
  encoder.float64(meta.MinBpm);
  encoder.float64(meta.MaxBpm);
  encoder.float64(meta.MostPrevalentBpm);
  encoder.float64(meta.GuessedBeatBpm);
  encoder.integer(static_cast<std::int32_t>(meta.GuessedBeatsPerMeasure));
  encoder.integer(static_cast<std::int32_t>(meta.Player));
  encoder.integer(static_cast<std::int32_t>(meta.KeyMode));
  encoder.boolean(meta.IsDP);
  encoder.integer(static_cast<std::int32_t>(meta.TotalNotes));
  encoder.integer(static_cast<std::int32_t>(meta.TotalLongNotes));
  encoder.integer(static_cast<std::int32_t>(meta.TotalScratchNotes));
  encoder.integer(static_cast<std::int32_t>(meta.TotalBackSpinNotes));
  encoder.integer(static_cast<std::int32_t>(meta.TotalLandmineNotes));
  encoder.integer(static_cast<std::int32_t>(meta.LnMode));
  encoder.optional(meta.RandomSeed, [&](unsigned int value) {
    encoder.integer(static_cast<std::uint32_t>(value));
  });
  encoder.optional(meta.RandomPrng,
                   [&](const std::string &value) { encoder.string(value); });
  encoder.vector(meta.RandomValues, [&](int value) {
    encoder.integer(static_cast<std::int32_t>(value));
  });
}

void appendProvenance(CanonicalEncoder &encoder,
                      const ScoreProvenance &provenance) {
  encoder.integer(static_cast<std::int32_t>(provenance.schemaVersion));
  encoder.string(provenance.ruleset.id);
  encoder.integer(static_cast<std::int32_t>(provenance.ruleset.version));
  encoder.string(provenance.ruleset.scoringModel);
  encoder.string(provenance.ruleset.judgementModel);
  encoder.string(provenance.ruleset.gaugeModel);
  encoder.vector(provenance.stages, [&](const ScoreStageProvenance &stage) {
    encoder.string(stage.chartMd5);
    encoder.string(stage.chartSha256);
    encoder.integer(static_cast<std::int32_t>(stage.longNoteMode));
    encoder.optional(stage.chartRandomSeed,
                     [&](std::uint64_t value) { encoder.integer(value); });
    encoder.optional(stage.chartRandomPrng,
                     [&](const std::string &value) { encoder.string(value); });
    encoder.vector(stage.chartRandomValues, [&](int value) {
      encoder.integer(static_cast<std::int32_t>(value));
    });
    encoder.enumeration(stage.judgeRankSource);
    encoder.optional(stage.sourceJudgeRank, [&](int value) {
      encoder.integer(static_cast<std::int32_t>(value));
    });
    encoder.integer(static_cast<std::int32_t>(stage.totalNotes));
    encoder.optional(stage.authoredGaugeTotal,
                     [&](double value) { encoder.float64(value); });
    encoder.float64(stage.effectiveGaugeTotal);
    encoder.enumeration(stage.candidateSelection);
    encoder.vector(
        stage.effectiveJudgeWindows, [&](const JudgeWindowProvenance &window) {
          encoder.enumeration(window.context);
          encoder.enumeration(window.judgement);
          encoder.integer(static_cast<std::int64_t>(window.earlyMicros));
          encoder.integer(static_cast<std::int64_t>(window.lateMicros));
        });
  });
  encoder.integer(
      static_cast<std::int32_t>(gaugeTypeIndex(provenance.gaugeType)));
  encoder.enumeration(provenance.gaugeProfile);
  encoder.integer(static_cast<std::int32_t>(
      gaugeAutoShiftModeValue(provenance.gaugeAutoShift)));
  encoder.integer(static_cast<std::int32_t>(
      gaugeTypeIndex(provenance.gaugeAutoShiftLowerBound)));
  const auto appendPlayer = [&](const PlayerOptionProvenance &player) {
    encoder.string(player.option);
    encoder.optional(player.seed,
                     [&](std::int64_t value) { encoder.integer(value); });
  };
  appendPlayer(provenance.player1);
  appendPlayer(provenance.player2);
  encoder.string(provenance.assistOption);
  encoder.vector(provenance.inputDevices, [&](InputDeviceCategory device) {
    encoder.enumeration(device);
  });
  encoder.boolean(provenance.autoPlay);
  encoder.boolean(provenance.practice);
  encoder.boolean(provenance.clubMode);
  encoder.integer(static_cast<std::int32_t>(provenance.playback.percent));
  encoder.enumeration(provenance.playback.mode);
  encoder.integer(
      static_cast<std::int32_t>(provenance.judgeWindowScalePercent));
  encoder.optional(provenance.startingGaugePercent, [&](int value) {
    encoder.integer(static_cast<std::int32_t>(value));
  });
  encoder.enumeration(provenance.eligibility);
}

void appendReplay(CanonicalEncoder &encoder, const ReplayData &replay) {
  encoder.boolean(replay.autoPlay);
  appendChartMeta(encoder, replay.chartMeta);
  encoder.optional(replay.randomSeed, [&](unsigned int value) {
    encoder.integer(static_cast<std::uint32_t>(value));
  });
  encoder.optional(replay.randomPrng,
                   [&](const std::string &value) { encoder.string(value); });
  encoder.vector(replay.randomValues, [&](int value) {
    encoder.integer(static_cast<std::int32_t>(value));
  });
  encoder.optional(replay.playOption,
                   [&](const std::string &value) { encoder.string(value); });
  encoder.optional(replay.playOptionSeed, [&](long long value) {
    encoder.integer(static_cast<std::int64_t>(value));
  });
  encoder.optional(replay.playOption2,
                   [&](const std::string &value) { encoder.string(value); });
  encoder.optional(replay.playOption2Seed, [&](long long value) {
    encoder.integer(static_cast<std::int64_t>(value));
  });
  encoder.string(replay.assistOption);
  encoder.integer(
      static_cast<std::int32_t>(gaugeTypeIndex(replay.initialGaugeType)));
  encoder.integer(static_cast<std::int32_t>(
      gaugeAutoShiftModeValue(replay.gaugeAutoShift)));
  encoder.integer(static_cast<std::int32_t>(
      gaugeTypeIndex(replay.gaugeAutoShiftLowerBound)));
  encoder.integer(static_cast<std::int32_t>(replay.finalScore));
  encoder.integer(static_cast<std::int32_t>(replay.maxCombo));
  encoder.float32(replay.finalGauge);
  encoder.integer(static_cast<std::int32_t>(replay.clearType));
  encoder.vector(replay.events, [&](const ReplayEvent &event) {
    encoder.enumeration(event.action);
    encoder.integer(static_cast<std::int32_t>(event.lane));
    encoder.integer(static_cast<std::int64_t>(event.noteTimeMicros));
    encoder.integer(static_cast<std::int64_t>(event.songTimeMicros));
    encoder.integer(static_cast<std::int64_t>(event.judgeTimeMicros));
    encoder.enumeration(event.judgement);
    encoder.integer(static_cast<std::int64_t>(event.diffMicros));
    encoder.float32(event.gauge);
    encoder.integer(static_cast<std::int32_t>(gaugeTypeIndex(event.gaugeType)));
    encoder.integer(static_cast<std::int32_t>(event.combo));
    encoder.integer(static_cast<std::int32_t>(event.score));
  });
  encoder.vector(replay.touchSamples, [&](const ReplayTouchSample &sample) {
    encoder.enumeration(sample.action);
    encoder.integer(static_cast<std::int64_t>(sample.fingerId));
    encoder.integer(static_cast<std::int64_t>(sample.songTimeMicros));
    encoder.float32(sample.x);
    encoder.float32(sample.y);
  });
  encoder.vector(
      replay.laneCoverEvents, [&](const ReplayLaneCoverEvent &event) {
        encoder.integer(static_cast<std::int64_t>(event.songTimeMicros));
        encoder.integer(
            static_cast<std::int32_t>(event.noteStartPositionPercent));
        encoder.boolean(event.resetVisibleTimeReference);
      });
  appendProvenance(encoder, replay.provenance);
}

void appendScore(CanonicalEncoder &encoder, const ChartScoreWrite &score) {
  encoder.string(score.chartPath);
  encoder.string(score.chartMd5);
  encoder.string(score.chartSha256);
  encoder.string(score.chartTitle);
  encoder.string(score.chartArtist);
  encoder.integer(static_cast<std::int32_t>(score.longNoteMode));
  encoder.integer(static_cast<std::int32_t>(score.score));
  encoder.integer(static_cast<std::int32_t>(score.maxScore));
  encoder.integer(static_cast<std::int32_t>(score.maxCombo));
  encoder.integer(static_cast<std::int32_t>(score.comboBreak));
  encoder.integer(static_cast<std::int32_t>(score.pGreat));
  encoder.integer(static_cast<std::int32_t>(score.great));
  encoder.integer(static_cast<std::int32_t>(score.good));
  encoder.integer(static_cast<std::int32_t>(score.bad));
  encoder.integer(static_cast<std::int32_t>(score.poor));
  encoder.integer(static_cast<std::int32_t>(score.kPoor));
  encoder.integer(static_cast<std::int32_t>(score.fast));
  encoder.integer(static_cast<std::int32_t>(score.slow));
  encoder.float32(score.finalGauge);
  encoder.integer(static_cast<std::int32_t>(score.clearType));
  appendProvenance(encoder, score.provenance);
}

struct NormalizedChartIdentity {
  std::string sha256;
  std::string md5;
};

std::optional<NormalizedChartIdentity>
chartIdentity(const bms_parser::ChartMeta &meta) {
  NormalizedChartIdentity identity{.sha256 = normalizedHash(meta.SHA256),
                                   .md5 = normalizedHash(meta.MD5)};
  if ((!meta.SHA256.empty() &&
       !canonical_digest::isCanonicalLowerHex(identity.sha256, 64)) ||
      (!meta.MD5.empty() &&
       !canonical_digest::isCanonicalLowerHex(identity.md5, 32)) ||
      (identity.sha256.empty() && identity.md5.empty())) {
    return std::nullopt;
  }
  return identity;
}

bool sameChartIdentity(const bms_parser::ChartMeta &left,
                       const bms_parser::ChartMeta &right) {
  const auto normalizedLeft = chartIdentity(left);
  const auto normalizedRight = chartIdentity(right);
  if (!normalizedLeft.has_value() || !normalizedRight.has_value()) {
    return false;
  }
  if (!normalizedLeft->sha256.empty() && !normalizedRight->sha256.empty()) {
    return normalizedLeft->sha256 == normalizedRight->sha256;
  }
  return !normalizedLeft->md5.empty() && !normalizedRight->md5.empty() &&
         normalizedLeft->md5 == normalizedRight->md5;
}

bool sameFloatBits(float left, float right) {
  return std::bit_cast<std::uint32_t>(left) ==
         std::bit_cast<std::uint32_t>(right);
}

std::optional<ChartResultAttempt> rejected(std::string_view invariant,
                                           std::string &diagnostic) {
  diagnostic.assign(invariant);
  return std::nullopt;
}

} // namespace

std::optional<ChartResultAttempt> makeChartResultAttempt(
    std::string attemptId, const bms_parser::ChartMeta &meta,
    const RhythmState &state, const ScoreProvenance &provenance,
    int storageLongNoteMode, ReplayData replay, std::string &diagnostic) {
  diagnostic.clear();
  if (!uuid::isCanonicalLowerV4(attemptId)) {
    return rejected("invalid attempt ID", diagnostic);
  }
  if (!sameChartIdentity(meta, replay.chartMeta)) {
    return rejected("chart identity mismatch", diagnostic);
  }
  if (replay.provenance != provenance) {
    return rejected("provenance mismatch", diagnostic);
  }

  ChartScoreWrite score =
      captureChartScoreWrite(meta, state, provenance, storageLongNoteMode);
  if (!hasProjectableChartIdentity(score)) {
    return rejected("chart identity is not projectable", diagnostic);
  }
  if (replay.finalScore != score.score) {
    return rejected("final score mismatch", diagnostic);
  }
  if (!sameFloatBits(replay.finalGauge, score.finalGauge)) {
    return rejected("final gauge mismatch", diagnostic);
  }
  if (replay.maxCombo != score.maxCombo) {
    return rejected("max combo mismatch", diagnostic);
  }

  const int totalNotes = std::max(0, meta.TotalNotes);
  const bool fullCombo =
      totalNotes > 0 && score.comboBreak == 0 && score.maxCombo >= totalNotes;
  const int expectedReplayClearType = clear_policy::fullComboRankForPlayback(
      score.clearType, fullCombo, provenance.playback);
  if (replay.clearType != expectedReplayClearType) {
    return rejected("clear type mismatch", diagnostic);
  }

  const std::string fingerprint = payloadFingerprint(replay, score);
  std::vector<float> adoptedGaugeHistory =
      state.gaugeHistoryFor(state.gaugeType);
  ChartJudgementTiming judgementTiming = captureChartJudgementTiming(state);
  return ChartResultAttempt{.attemptId = std::move(attemptId),
                            .replay = std::move(replay),
                            .score = std::move(score),
                            .keyMode = meta.KeyMode,
                            .adoptedGaugeType = state.gaugeType,
                            .adoptedGaugeHistory =
                                std::move(adoptedGaugeHistory),
                            .judgementTiming = std::move(judgementTiming),
                            .payloadFingerprint = fingerprint};
}

std::string payloadFingerprint(const ReplayData &replay,
                               const ChartScoreWrite &score) {
  CanonicalEncoder encoder;
  encoder.string("asobmashow-chart-result-v1");
  appendReplay(encoder, replay);
  appendScore(encoder, score);
  return encoder.finish();
}

} // namespace result_persistence
