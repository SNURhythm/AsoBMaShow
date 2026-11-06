#pragma once

#include "PlayfieldChartVisualModel.h"

// SongData prepares SongInformation before BMSPlayer applies practice's
// modifiers. Keep its cached properties apart from the playable section model.
struct GameplaySkinPracticeSongInformation {
  std::vector<SkinNormalDistribution> normalDistribution;
  bool distributionOmitted = false;
  double mainBpm = 0.0;
  std::array<int, 4> noteCounts{};
  std::optional<PlayfieldSongInformation> information;

  explicit GameplaySkinPracticeSongInformation(
      const PlayfieldChartVisualModel &model)
      : normalDistribution(model.skinGameplayGraph.normalDistribution),
        distributionOmitted(model.skinGameplayGraph.distributionOmitted),
        mainBpm(model.staticMetadata.mainBpm),
        noteCounts{model.staticMetadata.normalKeyNotes,
                   model.staticMetadata.longKeyNotes,
                   model.staticMetadata.normalScratchNotes,
                   model.staticMetadata.longScratchNotes},
        information(model.staticMetadata.songInformation) {}

  void restore(PlayfieldChartVisualModel &model) const {
    auto &metadata = model.staticMetadata;
    metadata.mainBpm = mainBpm;
    metadata.normalKeyNotes = noteCounts[0];
    metadata.longKeyNotes = noteCounts[1];
    metadata.normalScratchNotes = noteCounts[2];
    metadata.longScratchNotes = noteCounts[3];
    metadata.songInformation = information;

    auto &graph = model.skinGameplayGraph;
    graph.normalDistribution = normalDistribution;
    graph.distributionOmitted = distributionOmitted;
    graph.normalKeyNotes = noteCounts[0];
    graph.longKeyNotes = noteCounts[1];
    graph.normalScratchNotes = noteCounts[2];
    graph.longScratchNotes = noteCounts[3];
    graph.averageDensity = information ? std::optional{information->density}
                                       : std::nullopt;
    graph.peakDensity = information ? std::optional{information->peakDensity}
                                    : std::nullopt;
    graph.endDensity = information ? std::optional{information->endDensity}
                                   : std::nullopt;
    graph.totalGauge = information ? std::optional{information->total}
                                   : std::nullopt;
  }
};
