#include "rendering/UniformCache.h"
#include "rendering/ResponsiveUi.h"
#include "view/ChartDetailsView.h"
#include "view/ImageView.h"
#include "view/ScrollView.h"
#include "view/TextView.h"
#include "view/UiTheme.h"
#include "view/MainMenuTheme.h"
#include "view/ArtworkColor.h"
#include "view/PreviewGlow.h"

#include <bgfx/bgfx.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace rendering {
bgfx::VertexLayout PosTexCoord0Vertex::ms_decl;
bgfx::VertexLayout PosColorVertex::ms_decl;
bgfx::VertexLayout PosTexVertex::ms_decl;
int window_width = design_width;
int window_height = design_height;
int render_width = design_width;
int render_height = design_height;
float widthScale = 1.0F;
float heightScale = 1.0F;
float ui_scale_x = 1.0F;
float ui_scale_y = 1.0F;
int ui_offset_x = 0;
int ui_offset_y = 0;
int ui_view_width = design_width;
int ui_view_height = design_height;
} // namespace rendering

namespace {
void require(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

std::string text(View &view, const char *name) {
  auto *label = dynamic_cast<TextView *>(view.findViewByName(name));
  require(label != nullptr, "chart details expose the requested value");
  return label->getText();
}
class ArtworkLayoutDetails : public ChartDetailsView {
public:
  using ChartDetailsView::ChartDetailsView;
  using ChartDetailsView::renderImpl;
};

struct GlowCapture final : rendering::UiBatchBackend {
  std::optional<rendering::UiBatchState> glow;
  std::optional<rendering::UiBatchState> image;
  float top = 0;
  std::vector<rendering::PosColorVertex> glowVertices;
  std::vector<rendering::PosTexCoord0Vertex> imageVertices;
  bool submit(const rendering::UiBatchSubmission &submission) noexcept override {
    if (submission.colorVertices.size() > 32) {
      glow = submission.state;
      glowVertices.assign(submission.colorVertices.begin(), submission.colorVertices.end());
      top = std::min_element(glowVertices.begin(), glowVertices.end(),
                            [](const auto &a, const auto &b) { return a.y < b.y; })->y;
    } else if (!submission.texturedVertices.empty() && submission.state.uniformCount == 0) {
      image = submission.state;
      imageVertices.assign(submission.texturedVertices.begin(), submission.texturedVertices.end());
    }
    return true;
  }
};

GlowCapture captureGlow(View &frame, View &details) {
  GlowCapture capture;
  rendering::UiBatchRenderer batch(capture);
  RenderContext context(batch);
  context.scissor = {details.getX(), details.getY(), details.getWidth(), details.getHeight()};
  batch.begin();
  frame.render(context);
  batch.end();
  return capture;
}

void writePpm(const std::filesystem::path &path, int width, int height, bool split = false) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << "P6\n" << width << ' ' << height << "\n255\n";
  const char pixel[] = {char(0x33), char(0x66), char(0x99)};
  const char red[] = {char(0xdd), char(0x22), char(0x33)};
  for (int index = 0; index < width * height; ++index)
    output.write(split && index % width < width / 2 ? red : pixel, sizeof(pixel));
}
} // namespace

int main() {
  const audio::Spectrum fullSpectrum = [] {
    audio::Spectrum bands;
    bands.fill(1);
    return bands;
  }();
  {
    PreviewGlow glow;
    require(glow.update(0, fullSpectrum, 1.0f / 60) == 0, "silent preview has no halo");
    const float attack = glow.update(0.5f, fullSpectrum, 0.05f);
    require(attack > 0.25f && attack < 1, "audible preview rises smoothly");
    const float release = glow.update(0, fullSpectrum, 0.05f);
    require(release > 0 && release < attack, "silence gently releases the halo");
    for (int i = 0; i < 180; ++i) glow.update(0, fullSpectrum, 1.0f / 60);
    require(glow.update(0, fullSpectrum, 0) == 0, "stopped preview eventually clears the halo");
    glow.update(1, fullSpectrum, 0.1f);
    glow.reset();
    require(std::all_of(glow.spectrum().begin(), glow.spectrum().end(), [](float value) { return value == 0; }),
            "chart replacement clears spectrum smoothing");
    require(glow.update(0, fullSpectrum, 0) == 0, "chart replacement resets previous audio energy");
    audio::Spectrum bass{};
    bass[3] = 1;
    glow.update(0.5f, bass, 0.02f);
    require(glow.spectrum()[3] > 0.9f && glow.spectrum()[4] == 0,
            "spectrum peaks attack immediately at their own frequency position");
    for (int i = 0; i < 60; ++i) glow.update(0.5f, bass, 1.0f / 60);
    require(glow.spectrum()[3] > 0.99f && glow.spectrum()[4] == 0 && glow.spectrum()[2] == 0,
            "a sustained tone never cycles into neighboring positions");
    audio::Spectrum treble{};
    constexpr auto trebleBand = audio::kSpectrumBandCount - 4;
    treble[trebleBand] = 1;
    for (int i = 0; i < 10; ++i) glow.update(0.5f, treble, 1.0f / 60);
    require(glow.spectrum()[trebleBand] > 0.99f && glow.spectrum()[3] < 0.1f,
            "changing frequency moves the peak to the new band while the old peak releases quickly");
    ArtworkPalette solid;
    solid.fill(Color(255, 0, 0));
    ArtworkGlow shape{.intensity = 1, .spread = 44};
    shape.spectrum[0] = 1;
    const auto mesh = makeArtworkGlowMesh(100, 100, 400, 300, solid, shape);
    const auto outerRing = ArtworkGlowMesh::layers * ArtworkGlowMesh::segments;
    const float topExtent = 100 - mesh.vertices[outerRing].y;
    const float bottomExtent = mesh.vertices[outerRing + ArtworkGlowMesh::segments / 2].y - 400;
    require(std::abs(topExtent - 44) < 0.001f && bottomExtent < 1,
            "strong bands use the full spread while quiet bands collapse to the artwork");
    require(std::abs(mesh.vertices[0].y - 101) < 0.001f &&
                std::abs(mesh.vertices[ArtworkGlowMesh::segments / 2].y - 399) < 0.001f,
            "the aura follows the actual rectangular image edges");
    const auto alpha = [&](std::size_t ring, std::size_t segment = 0) {
      return mesh.vertices[ring * ArtworkGlowMesh::segments + segment].abgr >> 24;
    };
    require(alpha(0) > 220 && alpha(6) > 160 && alpha(10) < 15 && alpha(12) == 0,
            "artwork color fills the aura from the jacket through the middle and fades to transparent");
    for (std::size_t ring = 1; ring <= ArtworkGlowMesh::layers; ++ring)
      require(alpha(ring) <= alpha(ring - 1),
              "color fades continuously by distance without a bright bubble rim");
    require(alpha(6) > alpha(6, ArtworkGlowMesh::segments / 2) + 60,
            "active frequency bands spread their color farther than quiet bands");
    require(alpha(0, ArtworkGlowMesh::segments / 2) == 0,
            "quiet bands leave no opaque rectangular baseline around the artwork");
    shape.spectrum[0] = 0.4f;
    const auto weak = makeArtworkGlowMesh(100, 100, 400, 300, solid, shape);
    shape.spectrum[0] = 0.8f;
    const auto strong = makeArtworkGlowMesh(100, 100, 400, 300, solid, shape);
    require(100 - strong.vertices[outerRing].y > 6 * (100 - weak.vertices[outerRing].y) &&
                (strong.vertices[0].abgr >> 24) > 3 * (weak.vertices[0].abgr >> 24),
            "moderate audio changes create a wide range of both distance and opacity");
    const unsigned char pixels[] = {255, 0, 0, 0, 0, 80, 0, 255};
    const auto green = artworkAmbientPalette(pixels, 2, 1);
    require(green && (*green)[7].g >= 200 && (*green)[7].r == 0 && (*green)[7].b == 0,
            "transparent pixels do not tint the halo and dark colors stay visible");
    require(!artworkAmbientPalette(std::span(pixels).first(4), 2, 1),
            "incomplete image data has no palette");
    const unsigned char dark[] = {6, 6, 6, 255};
    const auto darkPalette = artworkAmbientPalette(dark, 1, 1);
    require(darkPalette && (*darkPalette)[0].r <= 12 && (*darkPalette)[0].g <= 12,
            "dark neutral borders do not turn into a bright gray aura");
    const unsigned char transparent[] = {255, 255, 255, 0};
    require(!artworkAmbientPalette(transparent, 1, 1), "transparent image has no halo");
  }

  bgfx::Init init;
  init.type = bgfx::RendererType::Noop;
  init.resolution.width = 64;
  init.resolution.height = 64;
  require(bgfx::init(init), "headless bgfx initializes for chart details");
  {
    const auto fixture = std::filesystem::temp_directory_path() /
        ("asobmashow-details-" + std::to_string(SDL_GetTicksNS()) + ".ppm");
    auto *image = new ImageView(0, 0, 0, 0);
    ArtworkLayoutDetails details(image);
    details.setWidth(472);
    details.setCompact(false);
    ChartMetaRecord record;
    record.meta.BmsPath = "/charts/artwork.bms";
    record.meta.StageFile = fixture.filename().string();
    details.setChart(&record, std::nullopt, kNoClearTypeRank, "");
    auto *frame = details.findViewByName("chartDetailsArtwork");
    auto *summary = details.findViewByName("chartDetailsSummary");
    details.applyYogaLayout();
    require(frame->getWidth() == 472 && frame->getHeight() == 472,
            "missing artwork uses a square placeholder");
    require(frame->getY() == details.getY(),
            "visualizer adds no top padding or height to the desktop details");
    const int placeholderSummaryY = summary->getY();
    writePpm(fixture, 32, 18, true);
    require(image->setImage(fspath_to_path_t(fixture)), "wide stagefile fixture decodes");
    const auto palette = image->ambientPalette();
    require(palette && (*palette)[7].b > (*palette)[7].r && (*palette)[0].r > (*palette)[0].b,
            "ambient palette preserves blue on the right and red on the left");
    RenderContext context;
    details.renderImpl(context);
    details.applyYogaLayout();
    require(frame->getWidth() == 472 && frame->getHeight() == 472 &&
                image->getWidth() == frame->getWidth() && image->getHeight() == frame->getHeight() &&
                summary->getY() == placeholderSummaryY,
            "decoded wide artwork retains the square frame and stable metadata position");
    details.updatePreviewGlow(0.5f, fullSpectrum, 0.1f);
    auto glow = captureGlow(*frame, details);
    require(!glow.glowVertices.empty(),
            "ambient color bleed renders a feathered contour instead of a rectangular shadow");
    const auto innerEnd = glow.glowVertices.begin() + ArtworkGlowMesh::segments;
    const auto rightPixel = std::max_element(glow.glowVertices.begin(), innerEnd,
        [](const auto &a, const auto &b) { return a.x < b.x; })->abgr;
    const auto leftPixel = std::min_element(glow.glowVertices.begin(), innerEnd,
        [](const auto &a, const auto &b) { return a.x < b.x; })->abgr;
    require((rightPixel >> 24) > 0 && ((rightPixel >> 16) & 255) > (rightPixel & 255) &&
                (leftPixel & 255) > ((leftPixel >> 16) & 255),
            "the rendered halo bleeds different artwork colors from each side");
    require(glow.glow && glow.glow->scissor && glow.top >= glow.glow->scissor->y,
            "the visualizer uses an expanded clip without changing the image layout");
    const auto rest = captureGlow(*image, details).imageVertices;
    require(!rest.empty() && rest[1].x - rest[0].x > frame->getWidth(),
            "the jacket gets a small centered scale pulse during audible preview");
    const auto outer = ArtworkGlowMesh::layers * ArtworkGlowMesh::segments;
    require((glow.glowVertices[outer].abgr >> 24) == 0,
            "the outer contour feathers all the way to transparency");
    for (int i = 0; i < 180; ++i) details.updatePreviewGlow(0, fullSpectrum, 1.0f / 60);
    const auto stopped = captureGlow(*frame, details);
    require(!stopped.glow, "stopped preview stops submitting a halo");
    require(stopped.imageVertices[1].x - stopped.imageVertices[0].x == frame->getWidth(),
            "silent preview restores the jacket's original size");
    details.updatePreviewGlow(0.5f, fullSpectrum, 0.1f);
    record.meta.BmsPath = "/charts/replacement.bms";
    details.setChart(&record, std::nullopt, kNoClearTypeRank, "");
    require(!captureGlow(*frame, details).glow, "chart change clears the previous halo");
    const int wideHeight = details.getHeight();
    details.renderImpl(context);
    require(details.getHeight() == wideHeight && !YGNodeIsDirty(details.getNode()),
            "unchanged decoded dimensions do not dirty layout on another frame");
    details.setCompact(true);
    details.applyYogaLayout();
    require(frame->getWidth() == 112 && frame->getHeight() == 112,
            "portrait contains wide artwork in a square frame");
    details.updatePreviewGlow(0.5f, fullSpectrum, 0.1f);
    glow = captureGlow(*frame, details);
    require(glow.glow && glow.glow->scissor && glow.top >= glow.glow->scissor->y,
            "compact artwork also leaves room above the halo");
    image->freeImage();
    details.updatePreviewGlow(0.5f, fullSpectrum, 0.1f);
    require(!captureGlow(*frame, details).glow, "unloaded artwork never glows");
    require(!image->ambientPalette(), "unloaded artwork clears its ambient color");
    ImageView::dropCache(fspath_to_path_t(fixture));
    writePpm(fixture, 18, 32);
    require(image->setImage(fspath_to_path_t(fixture)), "tall stagefile fixture decodes");
    details.renderImpl(context);
    details.applyYogaLayout();
    require(frame->getWidth() == 112 && frame->getHeight() == 112,
            "tall artwork retains the square compact frame");
    for (const bool compact : {false, true}) {
      details.setCompact(compact);
      details.applyYogaLayout();
      const int contentHeight = details.getHeight();
      const int imageY = frame->getY();
      for (int i = 0; i < 40; ++i) details.updatePreviewGlow(0.5f, fullSpectrum, 1.0f / 30);
      const auto maximum = captureGlow(*frame, details);
      require(maximum.glow && maximum.glow->scissor &&
                  maximum.top + 0.01f >= maximum.glow->scissor->y &&
                  maximum.top < details.getY() && maximum.glow->scissor->y < details.getY(),
              "full-height artwork visualizer bleeds above the layout through its own expanded clip");
      require(details.getHeight() == contentHeight && frame->getY() == imageY &&
                  !YGNodeIsDirty(details.getNode()),
              "the visualizer never increases content height or shifts the jacket");
      require(maximum.image && maximum.image->scissor && !maximum.imageVertices.empty() &&
                  std::all_of(maximum.imageVertices.begin(), maximum.imageVertices.end(),
                      [&](const auto &v) { return v.y >= maximum.image->scissor->y; }),
              "the bounced jacket remains visible above the unchanged layout boundary");
    }
    record.meta.StageFile.clear();
    details.setChart(&record, std::nullopt, kNoClearTypeRank, "");
    details.applyYogaLayout();
    require(frame->getWidth() == 112 && frame->getHeight() == 112,
            "missing artwork retains the square placeholder after a tall image");
    std::filesystem::remove(fixture);
  }
  {
    View scoreColumn;
    scoreColumn.setWidth(420);
    ChartDetailsView details(new ImageView(0, 0, 0, 0));
    details.setScoreContainer(&scoreColumn);
    details.setWidth(460);
    ChartMetaRecord record;
    record.meta.Title = "Chart in the left column";
    record.meta.BmsPath = "/charts/split.bms";
    ScoreBestSnapshot best;
    best.score = 1800;
    best.maxScore = 2000;
    details.setChart(&record, best, kClearTypeHardClearRank, "300");
    details.applyYogaLayout();
    scoreColumn.applyYogaLayout();
    require(details.findViewByName("chartDetailsPersonalBest") == nullptr &&
                scoreColumn.findViewByName("chartDetailsPersonalBest") != nullptr,
            "split details keep chart facts and personal best in separate columns");
    require(text(scoreColumn, "chartDetailsScore") == "1800 / 2000",
            "the external score card receives selection updates");
    const int naturalHeight = details.getHeight();
    details.setMinHeight(520);
    details.applyYogaLayout();
    const auto *facts = details.findViewByName("chartDetailsFacts");
    const auto *lastValue = details.findViewByName("chartDetailsTotal");
    require(details.getHeight() == 520 &&
                facts->getY() + facts->getHeight() == details.getY() + details.getHeight() &&
                std::abs(lastValue->getY() + lastValue->getHeight() -
                         (facts->getY() + facts->getHeight() - 14)) <= 1,
            "portrait chart facts distribute content through the aligned column height");
    for (const int height : {520, 720}) {
      details.setMinHeight(height);
      details.applyYogaLayout();
      auto *bpm = details.findViewByName("chartDetailsBpm");
      auto *length = details.findViewByName("chartDetailsLength");
      auto *bpmCell = static_cast<View *>(YGNodeGetContext(YGNodeGetParent(bpm->getNode())));
      auto *lengthCell = static_cast<View *>(YGNodeGetContext(YGNodeGetParent(length->getNode())));
      auto *bpmLabel = bpmCell->getChildren().front();
      auto *lengthLabel = lengthCell->getChildren().front();
      const int pairGap = bpm->getY() - bpmLabel->getY() - bpmLabel->getHeight();
      const int rowGap = lengthLabel->getY() - bpm->getY() - bpm->getHeight();
      require(pairGap == 0 && rowGap >= 8 && rowGap > pairGap,
              "stretched portrait facts keep label/value pairs close and distribute space between rows");
    }
    record.meta.Title = "A long portrait chart title that must scroll horizontally rather than wrap into clipped lines";
    details.setChart(&record, best, kClearTypeHardClearRank, "300");
    details.applyYogaLayout();
    auto *portraitTitle = dynamic_cast<TextView *>(details.findViewByName("chartDetailsTitle"));
    require(portraitTitle->textureWidth() > portraitTitle->getWidth() &&
                portraitTitle->textureHeight() <= portraitTitle->getHeight(),
            "portrait long titles retain a single overflowing line for marquee");
    details.setMinHeight(0);
    details.applyYogaLayout();
    require(details.getHeight() == naturalHeight,
            "removing the portrait fill constraint restores natural chart height");
    details.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    require(text(scoreColumn, "chartDetailsScore") == "—",
            "the external score card clears when the selection has no chart");
    details.setScoreContainer(nullptr);
    require(scoreColumn.getChildren().empty() &&
                text(details, "chartDetailsScore") == "—",
            "returning to landscape restores the original score card and ownership");
  }
  {
    ScrollView scroll(0, 0, 460, 800);
    auto *details = new ChartDetailsView(new ImageView(0, 0, 0, 0));
    scroll.setContentView(details);
    auto &view = *details;
    view.setWidth(460);
    ChartMetaRecord record;
    record.meta.Title = "Selected chart";
    record.meta.SubTitle = "[ANOTHER]";
    record.meta.BmsPath = "/charts/song.bms";
    record.meta.Bpm = 150;
    record.meta.MinBpm = 75.5;
    record.meta.MaxBpm = 180;
    record.meta.Rank = 2;
    record.meta.PlayLength = 125000000;
    record.meta.TotalNotes = 1350;
    record.meta.TotalLongNotes = 100;
    record.meta.TotalScratchNotes = 50;
    record.meta.HasTotal = true;
    ScoreBestSnapshot best;
    best.score = 2451;
    best.maxScore = 2700;
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    const int selectedHeight = view.getHeight();
    auto *artworkPlaceholder = view.findViewByName("chartDetailsArtworkPlaceholder");
    auto *reservedArtwork = view.findViewByName("chartDetailsArtwork");
    require(artworkPlaceholder && artworkPlaceholder->getVisible() &&
                reservedArtwork && reservedArtwork->getVisible() &&
                reservedArtwork->getWidth() > 0 && reservedArtwork->getHeight() > 0,
            "a chart without a stage image retains its artwork frame and music placeholder");
    require(text(view, "chartDetailsBpm") == "75.5–180",
            "variable BPM preserves both ends and fractional values");
    require(text(view, "chartDetailsJudge") == "NORMAL",
            "BMS judge rank uses a human-readable name");
    require(text(view, "chartDetailsLength") == "2:05",
            "play duration converts microseconds to minutes and seconds");
    require(text(view, "chartDetailsScore") == "2451 / 2700",
            "personal best uses the cached score and its own maximum");
    require(text(view, "chartDetailsRate") == "AAA · 90.78%",
            "personal best rate and rank use the stored maximum");
    require(text(view, "chartDetailsClear") == "HARD CLEAR",
            "clear lamp is independent of the high-score attempt");
    require(text(view, "chartDetailsNext") == "MAX −249",
            "AAA progress targets MAX rather than a duplicate displayed AAA");
    record.meta.KeyMode = 7;
    record.meta.PlayLevel = 11;
    for (const std::string units : {"★11", "sl3", "st2", "◆発狂", "sl3 / ★11"}) {
      record.difficultyTableLabels = units;
      view.setChart(&record, best, kClearTypeHardClearRank, "320");
      require(text(view, "chartDetailsDifficulty") == units,
              "table difficulty is the primary label and preserves every unit verbatim");
      require(text(view, "chartDetailsChartLevel") == "7K1S · Lv. 11",
              "the chart's own level remains available separately from table difficulty");
    }
    record.difficultyTableLabels.clear();
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    require(text(view, "chartDetailsDifficulty") == "Lv. 11" &&
                text(view, "chartDetailsChartLevel") == "7K1S",
            "charts outside a difficulty table fall back to their own level without duplicate labels");
    // At 16:9, the play dock is separate from the details and records column.
    const int shortViewportContentHeight = 1080 - 56 - 24 - 104 - 84 - 12;
    require(view.getHeight() + 32 <= shortViewportContentHeight,
            "chart facts and best score fit above pinned records at 16:9");

    // Landscape artwork uses a square frame; long details remain scrollable
    // without moving the primary controls into that scroll container.
    record.meta.StageFile = "stage.png";
    view.setCompact(false);
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(view.getHeight() > selectedHeight, "landscape artwork reserves its own space");
    auto *badge = dynamic_cast<TextView *>(view.findViewByName("chartDetailsDifficulty"));
    const int badgeLeft = badge->getX();
    const int shortBadgeWidth = badge->getWidth();
    record.difficultyTableLabels = "sl3 / ★11";
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(badge->getX() == badgeLeft && badge->getWidth() > shortBadgeWidth &&
                badge->getWidth() >= badge->measureTextWidth(badge->getText()) + 24,
            "detail level badge grows right to fit text with horizontal insets");
    record.difficultyTableLabels = "sl3 / ★11 / st2 / ◆15 / sl12 / ★25";
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    auto *chartLevelValue = view.findViewByName("chartDetailsChartLevel");
    require(badge->getX() == badgeLeft &&
                badge->getX() + badge->getWidth() <= chartLevelValue->getX() &&
                chartLevelValue->getWidth() >= 96 &&
                chartLevelValue->getX() + chartLevelValue->getWidth() <= view.findViewByName("chartDetailsGrade")->getX(),
            "long badges preserve space for key mode and the best-grade column");
    record.difficultyTableLabels.clear();
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(badge->getWidth() == shortBadgeWidth,
            "short levels restore their natural badge width");
    auto *artwork = view.findViewByName("chartDetailsArtwork");
    auto *summary = view.findViewByName("chartDetailsSummary");
    require(artwork && artwork->getWidth() == 460 && artwork->getHeight() == 460 &&
                summary && summary->getY() > artwork->getY() + artwork->getHeight(),
            "desktop artwork occupies the hero frame above the chart and best-score summary");
    require(view.findViewByName("chartDetailsTotal")->getX() < view.findViewByName("chartDetailsNotes")->getX() &&
                view.findViewByName("chartDetailsNotes")->getX() < view.findViewByName("chartDetailsNoteTypes")->getX() &&
                view.findViewByName("chartDetailsNoteTypes")->getX() < view.findViewByName("chartDetailsJudge")->getX(),
            "desktop facts contain total, notes, note types and judge without duplicated tempo");
    require(YGNodeStyleGetDisplay(YGNodeGetParent(view.findViewByName("chartDetailsBpm")->getNode())) == YGDisplayNone &&
                YGNodeStyleGetDisplay(YGNodeGetParent(view.findViewByName("chartDetailsLength")->getNode())) == YGDisplayNone,
            "BPM and length occur only in the desktop summary");
    require(view.findViewByName("chartDetailsGenre")->getY() >=
                view.findViewByName("chartDetailsArtist")->getY() + view.findViewByName("chartDetailsArtist")->getHeight() &&
                view.findViewByName("chartDetailsGenre")->getY() < summary->getY() &&
                view.findViewByName("chartDetailsMore") == nullptr,
            "genre follows the artist and the title has no overflow action");
    require(view.findViewByName("chartDetailsDifficulty")->getY() < view.findViewByName("chartDetailsTempo")->getY() &&
                view.findViewByName("chartDetailsGrade")->getX() > view.findViewByName("chartDetailsChartLevel")->getX(),
            "desktop summary places tempo below difficulty and the best grade on the right");
    require(text(view, "chartDetailsBestRate") == "90.78%" &&
                text(view, "chartDetailsTempo") == "BPM 75.5–180   |   2:05",
            "desktop summary displays percentage without repeating the grade and preserves tempo range");
    require(text(view, "chartDetailsJudge") == "NORMAL" && text(view, "chartDetailsScore") == "2451 / 2700",
            "all chart metadata and cached scores are visible without unfolding");
    auto *scoreCard = view.findViewByName("chartDetailsPersonalBest");
    auto *facts = view.findViewByName("chartDetailsFacts");
    require(scoreCard->getY() >= summary->getY() + summary->getHeight() &&
                scoreCard->getY() + scoreCard->getHeight() <= facts->getY() &&
                !view.findViewByName("chartDetailsRate")->getVisible() &&
                view.findViewByName("chartDetailsNext")->getVisible(),
            "desktop EX score and next-rank card sits below the summary without duplicate grade/rate");
    auto *next = view.findViewByName("chartDetailsNext");
    auto *clear = dynamic_cast<TextView *>(view.findViewByName("chartDetailsClear"));
    auto *scoreValue = view.findViewByName("chartDetailsScore");
    require(next->getX() + next->getWidth() == scoreCard->getX() + scoreCard->getWidth() &&
                clear->getX() + clear->getWidth() == scoreCard->getX() + scoreCard->getWidth() &&
                next->getY() < clear->getY() && clear->getY() == scoreValue->getY() &&
                clear->pointSize() == 32 && clear->fontWeight() == TextView::FontWeight::Bold,
            "next rank occupies the upper right above the larger clear rank at lower right");
    require(clear->getHeight() == scoreValue->getHeight() && clear->getWidth() > 100 &&
                clear->currentColor().r == 0 && clear->currentColor().g == 0 &&
                clear->currentColor().b == 0 && !view.findViewByName("chartDetailsClearLamp"),
            "clear status fills its score-row rectangle with contrasting text and no thin lamp");
    const int fullHeight = view.getHeight();
    record.meta.StageFile.clear();
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(artwork->getVisible() && artworkPlaceholder->getVisible() &&
                view.getHeight() == fullHeight,
            "missing landscape artwork preserves the hero frame and panel height");
    view.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    view.applyYogaLayout();
    require(artwork->getVisible() && artworkPlaceholder->getVisible() &&
                view.getHeight() == fullHeight,
            "an empty landscape selection retains the same artwork placeholder");
    record.meta.StageFile = "stage.png";
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();
    require(text(view, "chartDetailsGrade") == "AAA",
            "desktop summary gives the cached best grade its own display");

    scroll.setHeight(shortViewportContentHeight);
    scroll.refreshContentLayout();
    scroll.scrollToBottom();
    auto *score = view.findViewByName("chartDetailsScore");
    require(score->getY() >= scroll.getY() &&
                score->getY() + score->getHeight() <= scroll.getY() + scroll.getHeight(),
            "personal best remains reachable with landscape artwork");
    record.meta.StageFile.clear();
    view.setCompact(true);
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    view.applyYogaLayout();

    require(view.findViewByName("chartDetailsRate")->getVisible(),
            "portrait score card retains its sole rank/rate display after rotation");
    require(view.findViewByName("chartDetailsBpm")->getX() < view.findViewByName("chartDetailsJudge")->getX() &&
                view.findViewByName("chartDetailsLength")->getX() < view.findViewByName("chartDetailsNotes")->getX(),
            "portrait rotation restores the original metric order in two columns");
    require(view.findViewByName("chartDetailsBpm")->getY() < view.findViewByName("chartDetailsLength")->getY() &&
                view.findViewByName("chartDetailsLength")->getY() < view.findViewByName("chartDetailsTotal")->getY(),
            "portrait rotation restores the original vertical metric row order");

    // Selecting non-chart rows must not leave an existing scroll offset past
    // the end of the details now that placeholders preserve their height.
    scroll.setHeight(300);
    scroll.scrollToBottom();
    const float selectedOffset = scroll.getScrollOffset();
    require(selectedOffset > 0, "the details fixture starts scrolled down");
    const auto requireStableScroll = [&]() {
      view.applyYogaLayout();
      require(reservedArtwork->getVisible() && artworkPlaceholder->getVisible(),
              "non-chart selections retain the artwork placeholder");
      require(view.getHeight() == selectedHeight &&
                  scroll.getScrollOffset() == selectedOffset &&
                  view.getY() + view.getHeight() == scroll.getY() + scroll.getHeight(),
              "selection changes preserve the scrolled content bottom without another scroll event");
    };
    auto nonChart = record;
    nonChart.courseStart = true;
    view.setChart(&nonChart, std::nullopt, kNoClearTypeRank, "");
    requireStableScroll();
    nonChart.courseStart = false;
    nonChart.solidArchive = true;
    view.setChart(&nonChart, std::nullopt, kNoClearTypeRank, "");
    requireStableScroll();
    nonChart.solidArchive = false;
    nonChart.unavailable = true;
    view.setChart(&nonChart, std::nullopt, kNoClearTypeRank, "");
    requireStableScroll();
    view.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    requireStableScroll();
    scroll.setHeight(800);
    scroll.refreshContentLayout();

    record.meta.MinBpm = record.meta.MaxBpm = 0;
    record.meta.RankType = bms_parser::JudgeRankType::DefExRank;
    record.meta.Rank = 87;
    view.setChart(&record, std::nullopt, kNoClearTypeRank, "320");
    require(text(view, "chartDetailsBpm") == "150",
            "missing BPM range falls back to the chart's initial BPM");
    require(text(view, "chartDetailsJudge") == "87%",
            "extended judge rank is not mislabeled as an easy BMS rank");
    require(text(view, "chartDetailsScore").empty() &&
                text(view, "chartDetailsNext").empty(),
            "an unplayed selection clears previous score and progress");

    record.courseStart = true;
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    require(view.findViewByName("chartDetailsFacts")->getVisible() &&
                text(view, "chartDetailsScore") == "—",
            "course headers display placeholders instead of stale chart values");
    record.courseStart = false;
    record.solidArchive = true;
    view.setChart(&record, best, kClearTypeHardClearRank, "320");
    require(text(view, "chartDetailsBpm") == "—",
            "archive selections display placeholders instead of synthetic metadata");

    view.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    view.applyYogaLayout();
    require(text(view, "chartDetailsScore") == "—" &&
                text(view, "chartDetailsBpm") == "—",
            "clearing the selection replaces stale chart and score values with placeholders");
    require(view.getHeight() == selectedHeight,
            "empty selection reserves the same detail height as a played chart");
    record.solidArchive = false;
    view.setChart(&record, std::nullopt, kNoClearTypeRank, "320");
    view.applyYogaLayout();
    require(view.getHeight() == selectedHeight,
            "unplayed charts keep actions at the same position as played charts");
    view.setChart(nullptr, std::nullopt, kNoClearTypeRank, "");
    view.applyYogaLayout();
    require(view.getHeight() == selectedHeight,
            "clearing an unplayed chart also preserves the detail height");

    record.solidArchive = false;
    record.meta.PlayLength = 0;
    best.maxScore = 0;
    view.setChart(&record, best, kClearTypeEasyClearRank, "320");
    require(text(view, "chartDetailsLength") == "—" &&
                text(view, "chartDetailsScore") == "2451 / 2700",
            "unknown length stays unknown and legacy scores use chart max as fallback");
    for (const auto mode : {ui_theme::ThemeMode::Dark, ui_theme::ThemeMode::Light}) {
      ui_theme::setActiveMode(mode);
      scroll.propagateThemeChange();
      view.applyYogaLayout();
      auto *bpm = dynamic_cast<TextView *>(view.findViewByName("chartDetailsBpm"));
      const auto expectedColor = main_menu_theme::ink();
      const auto color = bpm->currentColor();
      require(color.r == expectedColor.r && color.g == expectedColor.g &&
                  color.b == expectedColor.b,
              "changing theme reaches details owned by the scrolling panel");
      for (const char *name : {"chartDetailsBpm", "chartDetailsJudge",
                              "chartDetailsScore", "chartDetailsClear"}) {
        auto *value = view.findViewByName(name);
        require(value->getWidth() > 0 && value->getX() >= view.getX() &&
                    value->getX() + value->getWidth() <= view.getX() + view.getWidth(),
                "chart values remain within the details panel");
      }
    }
  }
  {
    ChartDetailsView details(new ImageView(0, 0, 0, 0));
    for (const bool compactSpace : {true, false, true}) {
      rendering::compactMenuCanvasActive = compactSpace;
      details.setWidth(compactSpace ? 260 : 472);
      details.setCompact(false);
      details.applyYogaLayout();
      require(details.findViewByName("chartDetailsArtwork")->getVisible() == !compactSpace,
              "secondary artwork follows available space and returns in a wider window");
      require(details.findViewByName("chartDetailsSummary")->getVisible() == !compactSpace,
              "secondary summary follows available space without adding navigation");
      require(details.findViewByName("chartDetailsFacts")->getVisible(),
              "chart facts remain available in the compact details panel");
    }
    rendering::compactMenuCanvasActive = false;
  }
  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  bgfx::shutdown();
}
