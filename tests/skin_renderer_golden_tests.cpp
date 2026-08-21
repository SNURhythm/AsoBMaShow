#include "rendering/RenderPlan.h"
#include "rendering/ShaderManager.h"
#include "rendering/SkinQuadBatchRenderer.h"
#include "rendering/UniformCache.h"
#include "rendering/common.h"
#include "skin/beatoraja/PlaySkinViewport.h"
#include "skin/beatoraja/SkinDestinationEvaluator.h"
#include "skin/beatoraja/SkinGeneratedTextureRaster.h"
#include "view/View.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <lodepng.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace rendering {
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
float near_clip = -1.0F;
float far_clip = 1.0F;
} // namespace rendering

namespace {

int failures = 0;

void expect(bool value, std::string_view message) {
  if (!value) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<std::string> goldenCaseNames() {
  return {"fit_16x9", "stretch_16x9", "custom_16x9",
          "fit_4x3",  "stretch_4x3",  "custom_4x3"};
}

struct SyntheticTexture {
  unsigned width = 0;
  unsigned height = 0;
  std::vector<std::uint8_t> rgba;
};

struct FakeResources final : skin::SkinPreparedResourceView {
  const skin::PreparedSkinResource *
  find(skin::SkinResourceId id) const noexcept override {
    const auto found = images.find(id);
    return found == images.end() ? nullptr : &found->second;
  }

  const skin::SkinResolvedRegion *
  findResolvedRegion(skin::SkinResourceId,
                     const skin::SkinSourceRect &) const noexcept override {
    return nullptr;
  }

  const skin::PreparedSkinTextAtlas *
  findTextAtlas(skin::SkinTextAtlasId id) const noexcept override {
    const auto found = atlases.find(id);
    return found == atlases.end() ? nullptr : &found->second;
  }

  const skin::PreparedSkinTextAtlas *
  findTextAtlasForObject(skin::SkinObjectId) const noexcept override {
    return nullptr;
  }

  const skin::PreparedSkinGeneratedTexture *prepareGeneratedTexture(
      const skin::SkinGeneratedTextureKey &key,
      const skin::SkinGeneratedTextureData &data) const noexcept override {
    const auto existing = generated.find(key);
    if (existing != generated.end()) return &existing->second;
    if (data.width <= 0 || data.height <= 0 || data.rgba == nullptr ||
        data.width > std::numeric_limits<std::uint16_t>::max() ||
        data.height > std::numeric_limits<std::uint16_t>::max() ||
        data.rgba->size() !=
            static_cast<std::size_t>(data.width * data.height * 4))
      return nullptr;
    const auto *memory = bgfx::copy(
        data.rgba->data(), static_cast<std::uint32_t>(data.rgba->size()));
    if (memory == nullptr) return nullptr;
    const auto texture = bgfx::createTexture2D(
        static_cast<std::uint16_t>(data.width),
        static_cast<std::uint16_t>(data.height), false, 1,
        bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_NONE, memory);
    if (!bgfx::isValid(texture)) return nullptr;
    try {
      const auto [inserted, accepted] = generated.emplace(
          key, skin::PreparedSkinGeneratedTexture{
                   .key = key, .texture = texture,
                   .width = data.width, .height = data.height});
      if (!accepted) bgfx::destroy(texture);
      return accepted ? &inserted->second : nullptr;
    } catch (...) {
      bgfx::destroy(texture);
      return nullptr;
    }
  }

  void destroyGenerated() {
    for (const auto &[key, resource] : generated) {
      (void)key;
      if (bgfx::isValid(resource.texture)) bgfx::destroy(resource.texture);
    }
    generated.clear();
  }

  std::map<skin::SkinResourceId, skin::PreparedSkinResource> images;
  std::map<skin::SkinTextAtlasId, skin::PreparedSkinTextAtlas> atlases;
  mutable std::map<skin::SkinGeneratedTextureKey,
                   skin::PreparedSkinGeneratedTexture> generated;
};

std::uint32_t pack(const std::array<float, 4> &rgba) {
  const auto byte = [](float value) {
    return static_cast<std::uint32_t>(std::clamp(value, 0.0F, 1.0F) * 255.0F +
                                      0.5F);
  };
  return byte(rgba[0]) | (byte(rgba[1]) << 8U) | (byte(rgba[2]) << 16U) |
         (byte(rgba[3]) << 24U);
}

skin::SkinDrawCommand draw(std::uint32_t ordinal,
                           skin::SkinDrawPayload payload) {
  return {.authoredOrdinal = ordinal,
          .sourceObject = ordinal,
          .payload = std::move(payload)};
}

skin::SkinTexturedQuadCommand
projectedQuad(const skin::PlaySkinViewport &viewport,
              skin::SkinAuthoredRect rect, skin::SkinResourceId resource,
              skin::SkinSourceRect source, skin::SkinBlendMode blend,
              skin::SkinFilterMode filter, std::array<float, 4> rgba,
              double angle = 0.0,
              std::optional<skin::AuthoredRect> clip = std::nullopt) {
  skin::AuthoredDestinationGeometry geometry{.rect = rect,
                                             .clip = clip,
                                             .centerX = 0.5,
                                             .centerY = 0.5,
                                             .angleDegrees = angle,
                                             .rgba = rgba,
                                             .blend = blend,
                                             .filter = filter};
  const auto projected = skin::projectSkinDestinationToUi(
      geometry, {.textureWidth = 4, .textureHeight = 4, .region = source},
      viewport);
  skin::SkinTexturedQuadCommand result;
  result.resource = resource;
  result.state = {.blend = blend, .filter = filter, .scissor = projected.clip};
  const std::uint32_t color = pack(rgba);
  for (std::size_t index = 0; index < 4; ++index) {
    result.vertices[index] = {
        .x = static_cast<float>(projected.vertices[index][0]),
        .y = static_cast<float>(projected.vertices[index][1]),
        .u = static_cast<float>(projected.normalizedUvs[index][0]),
        .v = static_cast<float>(projected.normalizedUvs[index][1]),
        .rgba = color};
  }
  return result;
}

skin::SkinPrimitiveCommand
projectedPrimitive(const skin::PlaySkinViewport &viewport,
                   skin::SkinAuthoredRect rect, std::uint32_t color) {
  const auto projected = skin::projectSkinDestinationToUi(
      {.rect = rect},
      {.textureWidth = 1,
       .textureHeight = 1,
       .region = {.x = 0, .y = 0, .w = 1, .h = 1}},
      viewport);
  skin::SkinPrimitiveCommand result{.kind = skin::SkinPrimitiveKind::SolidQuad};
  for (const auto &vertex : projected.vertices) {
    result.vertices.push_back({.x = static_cast<float>(vertex[0]),
                               .y = static_cast<float>(vertex[1]),
                               .rgba = color});
  }
  return result;
}

struct GoldenCase {
  std::string name;
  unsigned width = 0;
  unsigned height = 0;
  skin::ViewportSettings viewport;
};

std::vector<GoldenCase> goldenCases() {
  skin::ViewportSettings custom;
  custom.mode = skin::ViewportMode::Custom;
  custom.customBase = skin::CustomViewportBase::Fit;
  custom.scaleX = 0.88F;
  custom.scaleY = 1.08F;
  custom.translateX = 3.0F;
  custom.translateY = -2.0F;
  return {
      {.name = "fit_16x9", .width = 160, .height = 90},
      {.name = "stretch_16x9",
       .width = 160,
       .height = 90,
       .viewport = {.mode = skin::ViewportMode::Stretch}},
      {.name = "custom_16x9", .width = 160, .height = 90, .viewport = custom},
      {.name = "fit_4x3", .width = 120, .height = 90},
      {.name = "stretch_4x3",
       .width = 120,
       .height = 90,
       .viewport = {.mode = skin::ViewportMode::Stretch}},
      {.name = "custom_4x3", .width = 120, .height = 90, .viewport = custom}};
}

SyntheticTexture imageTexture() {
  return {.width = 4,
          .height = 4,
          .rgba = {245, 70,  65,  255, 250, 170, 45,  255, 240, 225, 55,
                   255, 80,  210, 100, 255, 220, 50,  135, 255, 120, 80,
                   230, 255, 35,  180, 230, 255, 40,  225, 170, 255, 145,
                   55,  220, 255, 75,  105, 235, 255, 35,  190, 225, 255,
                   80,  225, 145, 255, 245, 85,  75,  255, 245, 155, 50,
                   255, 230, 220, 65,  255, 70,  205, 110, 255}};
}

SyntheticTexture atlasTexture() {
  return {.width = 4,
          .height = 4,
          .rgba = {255, 255, 255, 255, 30,  220, 255, 255, 255, 205, 45,
                   255, 255, 95,  180, 255, 255, 255, 255, 255, 30,  220,
                   255, 255, 255, 205, 45,  255, 255, 95,  180, 255, 255,
                   255, 255, 255, 30,  220, 255, 255, 255, 205, 45,  255,
                   255, 95,  180, 255, 255, 255, 255, 255, 30,  220, 255,
                   255, 255, 205, 45,  255, 255, 95,  180, 255}};
}

bgfx::TextureHandle createTexture(const SyntheticTexture &texture) {
  if (texture.width > std::numeric_limits<std::uint16_t>::max() ||
      texture.height > std::numeric_limits<std::uint16_t>::max()) {
    return BGFX_INVALID_HANDLE;
  }
  return bgfx::createTexture2D(
      static_cast<std::uint16_t>(texture.width),
      static_cast<std::uint16_t>(texture.height), false, 1,
      bgfx::TextureFormat::RGBA8, BGFX_SAMPLER_UVW_CLAMP,
      bgfx::copy(texture.rgba.data(),
                 static_cast<std::uint32_t>(texture.rgba.size())));
}

std::vector<skin::SkinDrawCommand>
buildScene(const skin::PlaySkinViewport &viewport) {
  std::vector<skin::SkinDrawCommand> commands;
  commands.push_back(
      draw(1, projectedPrimitive(viewport,
                                 {.x = 0, .y = 0, .width = 160, .height = 90},
                                 0xff241c14U)));
  commands.push_back(draw(
      2, projectedQuad(
             viewport, {.x = 12, .y = 12, .width = 82, .height = 52}, 1,
             {.x = 0, .y = 0, .w = 4, .h = 4}, skin::SkinBlendMode::Normal,
             skin::SkinFilterMode::Nearest, {1.0F, 1.0F, 1.0F, 1.0F}, 11.0,
             skin::AuthoredRect{.x = 5, .y = 5, .width = 145, .height = 75})));
  commands.push_back(draw(
      3, projectedQuad(
             viewport, {.x = 52, .y = 28, .width = 72, .height = 42}, 1,
             {.x = 0, .y = 0, .w = 4, .h = 4}, skin::SkinBlendMode::Additive,
             skin::SkinFilterMode::Linear, {0.65F, 0.75F, 1.0F, 0.58F}, -7.0)));

  skin::SkinGlyphRunCommand glyphRun;
  glyphRun.atlas = 2;
  glyphRun.state = {.blend = skin::SkinBlendMode::Normal,
                    .filter = skin::SkinFilterMode::Nearest};
  for (const auto [codepoint, x, sourceX] :
       std::array<std::tuple<char32_t, double, int>, 2>{
           std::tuple{U'A', 24.0, 0}, std::tuple{U'B', 43.0, 2}}) {
    const auto quad = projectedQuad(
        viewport, {.x = x, .y = 68, .width = 17, .height = 15}, 1,
        {.x = sourceX, .y = 0, .w = 2, .h = 4}, skin::SkinBlendMode::Normal,
        skin::SkinFilterMode::Nearest, {1.0F, 1.0F, 1.0F, 1.0F});
    glyphRun.glyphs.push_back(
        {.codepoint = codepoint, .vertices = quad.vertices});
  }
  commands.push_back(draw(4, std::move(glyphRun)));

  auto accent = projectedPrimitive(
      viewport, {.x = 105, .y = 9, .width = 38, .height = 21}, 0xffd060f0U);
  accent.state.blend = skin::SkinBlendMode::Multiply;
  commands.push_back(draw(5, std::move(accent)));

  skin::SkinPrimitiveCommand line{.kind = skin::SkinPrimitiveKind::LineStrip};
  const auto point = [&](double x, double y) {
    return skin::SkinVertex{
        .x = static_cast<float>(viewport.authoredToUi.m00 * x +
                                viewport.authoredToUi.tx),
        .y = static_cast<float>(viewport.authoredToUi.m11 * y +
                                viewport.authoredToUi.ty),
        .rgba = 0xfff7f0d8U};
  };
  line.vertices = {point(8, 82), point(80, 76), point(152, 84)};
  commands.push_back(draw(6, std::move(line)));

  const auto appendGenerated = [&](std::uint32_t ordinal,
                                   skin::AuthoredDestinationGeometry geometry,
                                   skin::SkinGeneratedTextureLayer layer,
                                   std::int64_t elapsedMillis,
                                   int revealMillis) {
    skin::SkinGeneratedTextureRaster raster(
        {.sourceObject = 100U + ordinal,
         .authoredOrdinal = ordinal,
         .layer = layer,
         .geometry = geometry,
         .viewport = viewport,
         .elapsedMillis = elapsedMillis,
         .revealMillis = revealMillis,
         .maximumCommands = 1,
         .maximumPrimitiveVertices = 0,
         .sourceWidth = 4,
         .sourceHeight = 4,
         .verticalFlip = false,
         .diagnosticObject = "generated golden"});
    if (auto *pixmap = raster.pixmap(); pixmap != nullptr) {
      pixmap->clear(0U);
      pixmap->fillRectangle(0, 0, 4, 4, 0xe8385080U);
      pixmap->fillRectangle(1, 1, 3, 2, 0x38a8f0a0U);
      pixmap->drawLine(0, 3, 3, 0, 0xffe060c0U);
    }
    auto lowered = raster.take();
    expect(!lowered.failure && lowered.commands.size() == 1,
           "generated golden Pixmap lowers to one texture command");
    commands.insert(commands.end(),
                    std::make_move_iterator(lowered.commands.begin()),
                    std::make_move_iterator(lowered.commands.end()));
  };

  appendGenerated(
      7,
      {.rect = {.x = 7, .y = 5, .width = 34, .height = 24},
       .rgba = {0.85F, 1.0F, 0.75F, 0.58F},
       .blend = skin::SkinBlendMode::Normal,
       .filter = skin::SkinFilterMode::Nearest,
       .stretch = skin::SkinStretchMode::Stretch},
      skin::SkinGeneratedTextureLayer::Primary, 1, 0);
  appendGenerated(
      8,
      {.rect = {.x = 52, .y = 8, .width = 46, .height = 27},
       .clip = skin::AuthoredRect{.x = 48, .y = 4, .width = 56, .height = 35},
       .centerX = 0.5,
       .centerY = 0.5,
       .angleDegrees = 17.0,
       .rgba = {0.7F, 0.8F, 1.0F, 0.62F},
       .blend = skin::SkinBlendMode::Additive,
       .filter = skin::SkinFilterMode::Linear,
       .stretch = skin::SkinStretchMode::KeepAspectRatioFitInner},
      skin::SkinGeneratedTextureLayer::Shape, 1, 0);
  appendGenerated(
      9,
      {.rect = {.x = 94, .y = 39, .width = 54, .height = 27},
       .clip = skin::AuthoredRect{.x = 100, .y = 40, .width = 38, .height = 22},
       .centerX = 0.25,
       .centerY = 0.75,
       .angleDegrees = -11.0,
       .rgba = {1.0F, 0.72F, 0.65F, 0.76F},
       .blend = skin::SkinBlendMode::Subtractive,
       .filter = skin::SkinFilterMode::Linear,
       .stretch = skin::SkinStretchMode::KeepAspectRatioFitOuterTrimmed},
      skin::SkinGeneratedTextureLayer::Shape, 50, 100);
  return commands;
}

void configureFixtureViews(unsigned width, unsigned height,
                           bgfx::FrameBufferHandle framebuffer) {
  rendering::window_width = static_cast<int>(width);
  rendering::window_height = static_cast<int>(height);
  rendering::render_width = static_cast<int>(width);
  rendering::render_height = static_cast<int>(height);
  rendering::ui_scale_x = 1.0F;
  rendering::ui_scale_y = 1.0F;
  rendering::ui_offset_x = 0;
  rendering::ui_offset_y = 0;
  rendering::ui_view_width = static_cast<int>(width);
  rendering::ui_view_height = static_cast<int>(height);

  float ortho[16];
  bx::mtxOrtho(ortho, 0.0F, static_cast<float>(width),
               static_cast<float>(height), 0.0F, 0.0F, 100.0F, 0.0F,
               bgfx::getCaps()->homogeneousDepth);
  bgfx::setViewFrameBuffer(rendering::ui_view, framebuffer);
  bgfx::setViewRect(rendering::ui_view, 0, 0, static_cast<std::uint16_t>(width),
                    static_cast<std::uint16_t>(height));
  bgfx::setViewTransform(rendering::ui_view, nullptr, ortho);
  bgfx::setViewMode(rendering::ui_view, bgfx::ViewMode::Sequential);
  bgfx::setViewClear(rendering::ui_view, BGFX_CLEAR_COLOR, 0x080a0effU);
  bgfx::setViewFrameBuffer(rendering::readback_view, BGFX_INVALID_HANDLE);
  bgfx::setViewRect(rendering::readback_view, 0, 0,
                    static_cast<std::uint16_t>(width),
                    static_cast<std::uint16_t>(height));
}

std::vector<std::uint8_t> renderGolden(const GoldenCase &fixture) {
  const auto viewport = skin::evaluatePlaySkinViewport(
      {.width = 160, .height = 90},
      {.x = 0,
       .y = 0,
       .width = static_cast<double>(fixture.width),
       .height = static_cast<double>(fixture.height)},
      fixture.viewport);
  expect(viewport.valid, "golden viewport is invertible");
  if (!viewport.valid) {
    return {};
  }

  const auto image = createTexture(imageTexture());
  const auto atlas = createTexture(atlasTexture());
  const auto output =
      bgfx::createTexture2D(static_cast<std::uint16_t>(fixture.width),
                            static_cast<std::uint16_t>(fixture.height), false,
                            1, bgfx::TextureFormat::BGRA8, BGFX_TEXTURE_RT);
  const auto readback =
      bgfx::createTexture2D(static_cast<std::uint16_t>(fixture.width),
                            static_cast<std::uint16_t>(fixture.height), false,
                            1, bgfx::TextureFormat::BGRA8,
                            BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
  bgfx::FrameBufferHandle framebuffer = BGFX_INVALID_HANDLE;
  if (bgfx::isValid(output)) {
    framebuffer = bgfx::createFrameBuffer(1, &output, false);
  }
  const bool valid = bgfx::isValid(image) && bgfx::isValid(atlas) &&
                     bgfx::isValid(output) && bgfx::isValid(readback) &&
                     bgfx::isValid(framebuffer);
  expect(valid, "real Metal fixture resources are created");
  if (!valid) {
    if (bgfx::isValid(framebuffer))
      bgfx::destroy(framebuffer);
    if (bgfx::isValid(readback))
      bgfx::destroy(readback);
    if (bgfx::isValid(output))
      bgfx::destroy(output);
    if (bgfx::isValid(atlas))
      bgfx::destroy(atlas);
    if (bgfx::isValid(image))
      bgfx::destroy(image);
    return {};
  }

  FakeResources resources;
  resources.images.emplace(
      1, skin::PreparedSkinResource{
             .id = 1, .texture = image, .width = 4, .height = 4});
  resources.atlases.emplace(
      2, skin::PreparedSkinTextAtlas{
             .id = 2,
             .key = {.font = 3,
                     .pointSize = 12,
                     .fallbackChainDigest = "3:0|synthetic:0"},
             .texture = atlas,
             .width = 4,
             .height = 4,
             .glyphs = {{U'A', {.region = {.x = 0, .y = 0, .w = 2, .h = 4}}},
                        {U'B', {.region = {.x = 2, .y = 0, .w = 2, .h = 4}}}},
             .ascent = 9,
             .capHeight = 8,
             .descent = -2,
             .lineHeight = 12});

  configureFixtureViews(fixture.width, fixture.height, framebuffer);
  bgfx::touch(rendering::ui_view);
  RenderContext context;
  rendering::SkinQuadBatchRenderer renderer;
  const auto commands = buildScene(viewport);
  renderer.begin(context, resources);
  expect(renderer.submit(commands),
         "golden scene passes whole-buffer preflight on real backend");
  renderer.flush();
  bgfx::blit(rendering::readback_view, readback, 0, 0, output);
  std::uint32_t currentFrame = bgfx::frame();

  std::vector<std::uint8_t> bgra(static_cast<std::size_t>(fixture.width) *
                                 fixture.height * 4U);
  const std::uint32_t expectedFrame = bgfx::readTexture(readback, bgra.data());
  expect(expectedFrame != std::numeric_limits<std::uint32_t>::max(),
         "real Metal readback is scheduled");
  for (unsigned guard = 0; currentFrame < expectedFrame && guard < 16;
       ++guard) {
    currentFrame = bgfx::frame();
  }
  expect(currentFrame >= expectedFrame, "real Metal readback completes");

  bgfx::setViewFrameBuffer(rendering::ui_view, BGFX_INVALID_HANDLE);
  resources.destroyGenerated();
  bgfx::destroy(framebuffer);
  bgfx::destroy(readback);
  bgfx::destroy(output);
  bgfx::destroy(atlas);
  bgfx::destroy(image);
  bgfx::frame();

  for (std::size_t offset = 0; offset < bgra.size(); offset += 4) {
    std::swap(bgra[offset], bgra[offset + 2]);
  }
  return bgra;
}

void verifyGolden(const GoldenCase &fixture) {
  const auto actual = renderGolden(fixture);
  expect(actual.size() ==
             static_cast<std::size_t>(fixture.width) * fixture.height * 4U,
         "real Metal readback has the exact expected byte count");
  if (actual.empty()) {
    return;
  }
  const std::filesystem::path path =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) /
      "tests/fixtures/beatoraja_skin/golden" / (fixture.name + ".png");
  if (std::getenv("ASOBMASHOW_UPDATE_GOLDENS") != nullptr) {
    std::filesystem::create_directories(path.parent_path());
    const unsigned error =
        lodepng::encode(path.string(), actual, fixture.width, fixture.height);
    expect(error == 0, "updated real Metal golden PNG encodes");
    return;
  }
  std::vector<std::uint8_t> expected;
  unsigned width = 0;
  unsigned height = 0;
  const unsigned error =
      lodepng::decode(expected, width, height, path.string());
  expect(error == 0, "committed real Metal golden PNG decodes");
  if (error != 0) {
    return;
  }
  expect(width == fixture.width && height == fixture.height,
         "golden dimensions match exactly");
  if (width != fixture.width || height != fixture.height ||
      expected.size() != actual.size()) {
    return;
  }
  std::size_t mismatches = 0;
  for (std::size_t index = 0; index < actual.size(); ++index) {
    if (std::abs(static_cast<int>(actual[index]) -
                 static_cast<int>(expected[index])) > 2) {
      ++mismatches;
    }
  }
  expect(mismatches == 0,
         "real Metal golden pixels stay within one-channel tolerance 2");
}

} // namespace

int main() {
  expect(goldenCaseNames() ==
             std::vector<std::string>{"fit_16x9", "stretch_16x9", "custom_16x9",
                                      "fit_4x3", "stretch_4x3", "custom_4x3"},
         "all six viewport goldens are covered");

  bgfx::Init init;
  init.type = bgfx::RendererType::Metal;
  init.fallback = false;
  init.resolution.width = 0;
  init.resolution.height = 0;
  if (!bgfx::init(init)) {
    std::cerr << "FAIL: headless Metal initialization failed\n";
    return 1;
  }
  const auto *caps = bgfx::getCaps();
  expect(bgfx::getRendererType() == bgfx::RendererType::Metal,
         "goldens execute on Metal without backend fallback");
  expect(caps != nullptr && (caps->supported & BGFX_CAPS_TEXTURE_BLIT) != 0 &&
             (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0,
         "Metal supports texture blit and readback");

  if (failures == 0) {
    for (const auto &fixture : goldenCases()) {
      verifyGolden(fixture);
    }
  }

  rendering::ShaderManager::getInstance().release();
  rendering::UniformCache::getInstance().destroyAll();
  // Retire deferred program/uniform destruction on the render thread before
  // shutting down the headless device.
  bgfx::frame();
  bgfx::frame();
  bgfx::shutdown();
  if (failures != 0) {
    return 1;
  }
  std::cout << "Skin renderer real-Metal golden tests passed\n";
  return 0;
}
