#pragma once

#include "View.h"
#include "PreviewGlow.h"
#include <array>
#include "../repositories/ChartRepository.h"
#include "../repositories/ScoreRepositoryModels.h"

class ImageView;
class TextView;

// Presentation only: binding uses the selector's existing metadata and score
// caches and never reads a database or parses a chart.
class ChartDetailsView : public View {
public:
  explicit ChartDetailsView(ImageView *artwork);
  static constexpr float minimumChartHeight() {
    return kIdentityHeight + kDifficultyHeight + 28 + 3 * kSectionGap +
           3 * kFactRowHeight + 2 * kFactGap + 2 * kFactPadding;
  }
  void setScoreContainer(View *container);
  void setCompact(bool compact);
  void updatePreviewGlow(float level, const audio::Spectrum &spectrum, float dt);
  template <typename ReadSpectrum>
  void updatePreviewGlow(float level, float dt, ReadSpectrum readSpectrum) {
    if (!canShowPreviewGlow()) {
      sampledSpectrum_.fill(0);
      spectrumCooldown_ = 0;
      updatePreviewGlow(0, sampledSpectrum_, dt);
      return;
    }
    // FFT work runs at most 30 Hz. Envelope interpolation still runs every UI
    // frame, and hidden/missing artwork or silent output never requests an FFT.
    if (std::isfinite(dt)) spectrumCooldown_ -= std::max(0.0f, dt);
    if (!std::isfinite(level) || level <= 0) {
      updatePreviewGlow(0, sampledSpectrum_, dt);
      return;
    }
    if (spectrumCooldown_ <= 0.000001f) {
      sampledSpectrum_ = readSpectrum();
      spectrumCooldown_ = 1.0f / 30;
    }
    updatePreviewGlow(level, sampledSpectrum_, dt);
  }
  void setChart(const ChartMetaRecord *record,
                const std::optional<ScoreBestSnapshot> &best,
                int clearRank, const std::string &total);

protected:
  void onLayout() override;
  void renderImpl(RenderContext &context) override;

private:
  void refreshArtworkLayout();
  bool canShowPreviewGlow() const;
  static constexpr float kIdentityHeight = 132;
  static constexpr float kDifficultyHeight = 34;
  static constexpr float kSectionGap = 10;
  static constexpr float kFactRowHeight = 60;
  static constexpr float kFactGap = 8;
  static constexpr float kFactPadding = 14;
  View *artworkFrame_ = nullptr;
  TextView *artworkPlaceholder_ = nullptr;
  View *identity_ = nullptr;
  View *titles_ = nullptr;
  View *titleRow_ = nullptr;
  std::array<View *, 3> factRows_{};
  std::array<View *, 6> factCells_{};
  ImageView *artwork_ = nullptr;
  bool artworkHasImage_ = false;
  PreviewGlow previewGlow_;
  audio::Spectrum sampledSpectrum_{};
  float spectrumCooldown_ = 0;
  std::string glowChartPath_;
  bool compact_ = true;
  int artworkLayoutWidth_ = -1;
  int artworkLayoutHeight_ = -1;
  bool artworkLayoutCompact_ = true;
  View *summary_ = nullptr;
  View *scoreSlot_ = nullptr;
  View *levelGroup_ = nullptr;
  View *levelLine_ = nullptr;
  TextView *tempo_ = nullptr;
  TextView *desktopRate_ = nullptr;
  TextView *grade_ = nullptr;
  View *facts_ = nullptr;
  View *personalBest_ = nullptr;
  View *scoreContainer_ = nullptr;
  TextView *title_ = nullptr;
  TextView *artist_ = nullptr;
  TextView *genre_ = nullptr;
  TextView *difficulty_ = nullptr;
  TextView *chartLevel_ = nullptr;
  TextView *bpm_ = nullptr;
  TextView *judge_ = nullptr;
  TextView *length_ = nullptr;
  TextView *notes_ = nullptr;
  TextView *total_ = nullptr;
  TextView *totalLabel_ = nullptr;
  TextView *noteTypes_ = nullptr;
  TextView *bestLabel_ = nullptr;
  TextView *clear_ = nullptr;
  TextView *score_ = nullptr;
  TextView *rate_ = nullptr;
  TextView *next_ = nullptr;
  TextView *emptyScore_ = nullptr;
};
