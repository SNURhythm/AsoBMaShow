#include "skin/beatoraja/SkinResourceCatalog.h"
#include "skin/beatoraja/SkinTextAtlas.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"
#include "view/ImageFileDecoder.h"
#include "view/SdlTtfRuntime.h"

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>

namespace {
int failures = 0;
void expect(bool value, std::string_view message) { if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }

void testBoundedPngAndJpegDecodeBeforeAllocation() {
  const std::filesystem::path resources =
      std::filesystem::path(ASOBMASHOW_SOURCE_DIR) / "tests/fixtures/beatoraja_skin/resources";
  const auto png = image_decode::decodeImageFile(resources / "fixture.png", 40, 3200);
  const auto jpg = image_decode::decodeImageFile(resources / "fixture.jpg", 40, 3200);
  expect(png && png->width == 40 && png->height == 20 && png->byteSize() == 3200,
         "purpose-built PNG decodes to bounded RGBA");
  expect(jpg && jpg->width == 40 && jpg->height == 20 && jpg->byteSize() == 3200,
         "purpose-built JPEG decodes to bounded RGBA");
  expect(!image_decode::decodeImageFile(resources / "fixture.png", 39, 3200),
         "header dimensions exceeding policy are rejected before full decode");
  const auto oversized = std::filesystem::temp_directory_path() / "asobmashow-task13-encoded-limit.bin";
  { std::ofstream stream(oversized, std::ios::binary); stream.seekp(4095); stream.put('\0'); }
  expect(!image_decode::decodeImageFile(oversized, 40, 3200, 16),
         "file decoder rejects encoded bytes before allocating a read buffer");
  std::error_code removeError;
  std::filesystem::remove(oversized, removeError);
}

void testSharedSdlTtfRuntimeFinalRelease() {
  expect(text_runtime::acquire() && text_runtime::acquire() &&
             text_runtime::activeReferencesForTesting() == 2,
         "overlapping SDL_ttf owners retain one process runtime");
  text_runtime::release();
  expect(text_runtime::activeReferencesForTesting() == 1,
         "releasing one SDL_ttf owner cannot quit the shared runtime");
  text_runtime::release();
  expect(text_runtime::activeReferencesForTesting() == 0,
         "the final SDL_ttf owner releases only after prior operations close");
}

void testSpriteBoundsAndNormalizedGridCells() {
  skin::SkinSourceRect authored{.x=0,.y=0,.w=40,.h=20,.gridColumn=2,.gridRow=0,.gridColumns=4,.gridRows=2};
  skin::SkinSourceRect resolved;
  expect(skin::skinResourceResolveRect(authored, 40, 20, resolved) &&
             resolved.x == 20 && resolved.y == 0 && resolved.w == 10 && resolved.h == 10,
         "row-major sprite preparation resolves the third grid cell for UV 0.5 to 0.75");
  authored.x = 31;
  expect(!skin::skinResourceResolveRect(authored, 40, 20, resolved),
         "out-of-bounds source crops are rejected before publication");
  authored = {.x=0,.y=0,.w=-1,.h=-1,.gridColumn=1,.gridRow=1,.gridColumns=2,.gridRows=2};
  expect(skin::skinResourceResolveRect(authored, 40, 20, resolved) &&
             resolved.x == 20 && resolved.y == 10 && resolved.w == 20 && resolved.h == 10,
         "negative-one source dimensions resolve against decoded image bounds before UV preparation");
}

void testTextAtlasKeyRejectsNegativePaintExtents() {
  skin::SkinTextAtlasKey key{.font=1, .pointSize=16};
  key.outlineWidth = -0.25;
  expect(!skin::canonicalizeSkinTextAtlasKey(key),
         "negative outline extent cannot become a canonical atlas key");
  key.outlineWidth = 0.0;
  key.shadowSmoothness = -0.25;
  expect(!skin::canonicalizeSkinTextAtlasKey(key),
         "negative shadow smoothing cannot become a canonical atlas key");
}

struct TemporaryDirectory {
  TemporaryDirectory() : root(std::filesystem::temp_directory_path() /
                              ("asobmashow-task13-" + std::to_string(++serial))) {
    std::filesystem::create_directories(root);
  }
  ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(root, error); }
  std::filesystem::path root;
  static inline std::atomic_uint64_t serial = 0;
};

struct FakeTextureDevice final : skin::SkinTextureDevice {
  bgfx::TextureHandle create(const image_decode::DecodedImageData &) override {
    ++creates;
    if (failAt != 0 && creates == failAt) return BGFX_INVALID_HANDLE;
    ++live;
    return {.idx = static_cast<std::uint16_t>(creates)};
  }
  void destroy(bgfx::TextureHandle handle) noexcept override {
    if (bgfx::isValid(handle)) { ++destroys; --live; }
  }
  bool ownsCurrentThread() const noexcept override { return true; }
  int maximumTextureDimension() const noexcept override {
    return maximumDimension;
  }
  int creates = 0;
  int destroys = 0;
  int live = 0;
  int failAt = 0;
  int maximumDimension = skin::SkinResourcePolicy::maximumDimension;
};

void testSecurePreparationLeaseAliasAndCatalogLifetime() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path source = temporary.root / "source";
  fs::create_directories(source / "entry/resources");
  std::ofstream(source / "entry/play.luaskin") << "return {}\n";
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.png",
                source / "entry/resources/fixture.png");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.jpg",
                source / "entry/resources/fixture.jpg");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.ttf",
                source / "entry/resources/fixture.ttf");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                    "tests/fixtures/beatoraja_skin/resources/fixture.ttf",
                source / "entry/resources/unsupported.fnt");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) / "assets/fonts/fa-solid-900.ttf",
                source / "entry/resources/icons.ttf");
  fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) / "bgfx/bgfx/examples/runtime/font/signika-regular.ttf",
                source / "entry/resources/signika.ttf");
  const auto package = *skin::normalizePackageId("Task13Fixture").package;
  const auto entry = *skin::normalizeEntryPath(package, "entry/play.luaskin").entry;
  skin::SkinStorageRoots roots{.visiblePackages=temporary.root/"visible",
                               .privateRevisions=temporary.root/"revisions",
                               .privateCatalog=temporary.root/"catalog",
                               .profileOverlays=temporary.root/"overlays"};
  auto aliases = skin::createPlatformSkinAliasDetector();
  skin::SkinTreeSnapshotter snapshotter(roots, *aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(), "resource fixture creates a staged revision");
  if (!snapshot.prepared) return;
  auto stagedFs = skin::LuaSkinFileSystem::create({.revision=snapshot.prepared->readView(), .entry=entry, .storageRoots=roots});
  expect(stagedFs.fileSystem != nullptr, "staged resource filesystem is entry-aware");
  if (!stagedFs.fileSystem) return;
  skin::ValidatedBeatorajaSkinModel model;
  model.model.resources.emplace_back(skin::SkinImageResource{.id=1, .authoredName="one", .virtualPath="resources/fixture.png"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=2, .authoredName="two", .virtualPath="resources/fixture.png"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=3, .authoredName="three", .virtualPath="resources/fixture.jpg"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=4, .authoredName="unused", .virtualPath="resources/does-not-exist.png"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=5, .authoredName="optional", .virtualPath="resources/does-not-exist.png"});
  model.model.resources.emplace_back(skin::SkinImageResource{.id=6, .authoredName="disabled", .virtualPath="resources/does-not-exist.png"});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=7, .authoredName="fixture-font", .virtualPath="resources/fixture.ttf", .type=0});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=8, .authoredName="fallback-font", .virtualPath="resources/icons.ttf", .type=0, .fallbacks={{.virtualPath="resources/fixture.ttf", .type=0}}});
  model.model.resources.emplace_back(skin::SkinFontResource{.id=9, .authoredName="kerning-font", .virtualPath="resources/signika.ttf", .type=0});
  model.model.objects.push_back({.id=1, .authoredName="primary", .payload=skin::SkinImageObject{.orderedStates={{.resource=1, .frames={{.x=0,.y=0,.w=40,.h=20,.gridColumns=4,.gridRows=2}}}}}, .critical=true});
  model.model.objects.push_back({.id=2, .authoredName="alias", .payload=skin::SkinImageObject{.orderedStates={{.resource=2, .frames={{.x=2,.y=3,.w=20,.h=10,.gridColumns=2,.gridRows=1}}}}}, .critical=true});
  model.model.objects.push_back({.id=3, .authoredName="jpeg", .payload=skin::SkinImageObject{.orderedStates={{.resource=3, .frames={{.x=0,.y=0,.w=40,.h=20}}}}}, .critical=true});
  model.model.objects.push_back({.id=4, .authoredName="optional", .payload=skin::SkinImageObject{.orderedStates={{.resource=5, .frames={{.x=0,.y=0,.w=40,.h=20}}}}}, .critical=false});
  model.model.objects.push_back({.id=5, .authoredName="disabled", .payload=skin::SkinImageObject{.orderedStates={{.resource=6, .frames={{.x=0,.y=0,.w=40,.h=20}}}}}, .critical=false});
  model.model.objects.push_back({.id=6, .authoredName="caption", .payload=skin::SkinTextObject{.font=7, .value=skin::SkinStringPropertyId{.value=1}, .literal="AV 123 日本", .pointSize=16}, .critical=true});
  model.model.objects.push_back({.id=7, .authoredName="styled-caption", .payload=skin::SkinTextObject{.font=7, .literal="AV", .pointSize=24, .outlineRgba={255,0,0,255}, .outlineWidth=1.0, .shadowRgba={0,0,255,128}, .shadowOffsetX=1.0, .shadowOffsetY=2.0, .shadowSmoothness=0.5}, .critical=true});
  model.model.objects.push_back({.id=8, .authoredName="fallback-caption", .payload=skin::SkinTextObject{.font=8, .literal="日本", .pointSize=16}, .critical=true});
  model.model.objects.push_back({.id=9, .authoredName="kerning-caption", .payload=skin::SkinTextObject{.font=9, .literal="AV", .pointSize=16}, .critical=true});
  model.disabledOptionalObjects.push_back(5);
  skin::BeatorajaSkinConfiguration configuration;
  const std::array<std::string, 1> runtimeStrings{"Artist 日本 42"};
  skin::SkinResourcePreparationService service;
  std::ofstream(source / "entry/play.luaskin", std::ios::app) << "-- different revision\n";
  auto mismatchSnapshot = snapshotter.snapshot(source, package, {}, {});
  expect(mismatchSnapshot.prepared.has_value(),
         "second staged revision is available for provenance testing");
  if (mismatchSnapshot.prepared) {
    auto mismatchFs = skin::LuaSkinFileSystem::create({.revision=mismatchSnapshot.prepared->readView(), .entry=entry, .storageRoots=roots});
    expect(mismatchFs.fileSystem != nullptr, "second staged revision creates an independent filesystem");
    if (!mismatchFs.fileSystem) return;
    const auto mismatch = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*mismatchFs.fileSystem, .model=model, .configuration=configuration});
    expect(!mismatch.valid && !mismatch.diagnostics.empty() &&
               mismatch.diagnostics.front().code == "skin.resource.path_invalid",
           "preparation rejects a filesystem from another immutable revision before reading bytes");
  }
  const auto hasDiagnostic = [](const std::vector<skin::SkinDiagnostic> &diagnostics,
                                std::string_view code) {
    for (const auto &item : diagnostics) if (item.code == code) return true;
    return false;
  };
  auto criticalMissingModel = model;
  criticalMissingModel.model.objects[3].critical = true;
  const auto criticalMissing = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=criticalMissingModel, .configuration=configuration});
  expect(!criticalMissing.valid && hasDiagnostic(criticalMissing.diagnostics, "skin.resource.missing_critical"),
         "a live critical resource missing from the immutable revision is a blocking error");
  auto invalidCropModel = model;
  auto &badFrames = std::get<skin::SkinImageObject>(invalidCropModel.model.objects[0].payload).orderedStates.front().frames;
  badFrames.front().x = 39;
  badFrames.front().w = 2;
  const auto invalidCrop = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=invalidCropModel, .configuration=configuration});
  expect(!invalidCrop.valid && hasDiagnostic(invalidCrop.diagnostics, "skin.resource.sprite_bounds"),
         "a critical post-decode source crop outside its image is a blocking error");
  auto unsupportedFontModel = model;
  std::get<skin::SkinFontResource>(unsupportedFontModel.model.resources[6]).virtualPath = "resources/unsupported.fnt";
  const auto unsupportedFont = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=unsupportedFontModel, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(!unsupportedFont.valid && hasDiagnostic(unsupportedFont.diagnostics, "skin.resource.font_format_unsupported"),
         "a critical bitmap font declaration is explicitly unsupported");
  unsupportedFontModel.model.objects[5].critical = false;
  unsupportedFontModel.model.objects[6].critical = false;
  const auto optionalUnsupportedFont = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=unsupportedFontModel, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(optionalUnsupportedFont.valid && hasDiagnostic(optionalUnsupportedFont.diagnostics, "skin.resource.font_format_unsupported"),
         "an optional bitmap font declaration reports a non-blocking diagnostic");
  auto unknownGlyphModel = model;
  std::get<skin::SkinTextObject>(unknownGlyphModel.model.objects[5].payload).literal = "\xF4\x8F\xBF\xBF";
  const auto unknownGlyph = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=unknownGlyphModel, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(!unknownGlyph.valid && hasDiagnostic(unknownGlyph.diagnostics, "skin.resource.glyph_missing"),
         "unknown live text glyphs fail synchronously before resource publication");
  const auto validated = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=model, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(validated.valid && !validated.cancelled && validated.diagnostics.size() == 1 &&
             validated.diagnostics.front().code == "skin.resource.missing_optional",
         "optional missing resources warn while unreferenced declarations do not become false critical failures");
  stagedFs.fileSystem.reset();
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease && publishError.empty(), "staged revision publishes after non-retaining validation");
  if (!lease) return;
  const auto weak = lease->weakPin();
  auto leasedFs = skin::LuaSkinFileSystem::create({.revision=lease->readView(), .entry=entry, .storageRoots=roots});
  expect(leasedFs.fileSystem != nullptr, "published resource filesystem is available");
  if (!leasedFs.fileSystem) return;
  auto planned = service.decodeAndPlan({.revision=std::move(*lease), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(planned.plan && planned.plan->images.size() == 2 && planned.plan->atlases.size() == 4 &&
             planned.plan->images.front().aliases == std::vector<skin::SkinResourceId>{2},
         "duplicate image locators reuse one texture while live text styles produce distinct atlases");
  std::optional<skin::SkinTextAtlasKey> firstAtlasKey;
  if (planned.plan && planned.plan->atlases.size() == 4) {
    const auto &firstAtlas = planned.plan->atlases[0];
    const auto &secondAtlas = planned.plan->atlases[1];
    firstAtlasKey = firstAtlas.key;
    bool styledColor = false;
    for (const unsigned char component : *secondAtlas.pixels.rgba)
      if (component != 0 && component != 255) { styledColor = true; break; }
    const auto signikaAtlas = std::find_if(planned.plan->atlases.begin(), planned.plan->atlases.end(),
        [](const skin::SkinPreparedGlyphAtlas &atlas) { return atlas.key.font == 9; });
    expect(firstAtlas.id == 1 && secondAtlas.id == 2 && firstAtlas.key.pointSize == 16 &&
               secondAtlas.key.pointSize == 24 && firstAtlas.glyphs.contains(U'日') &&
               firstAtlas.glyphs.contains(U'4') && firstAtlas.glyphs.at(U'A').region.x > 0 &&
               static_cast<double>(firstAtlas.glyphs.at(U'A').region.x) / firstAtlas.pixels.width > 0.0 &&
               static_cast<double>(firstAtlas.glyphs.at(U'A').region.x) / firstAtlas.pixels.width < 1.0 && styledColor &&
               signikaAtlas != planned.plan->atlases.end() && signikaAtlas->kerning.contains({U'A', U'V'}) &&
               signikaAtlas->kerning.at({U'A', U'V'}) != 0,
           "prepared font atlases have stable keys, normalized UV regions, styled pixels, Japanese/runtime glyphs, fallback selection, and real AV kerning");
  }
  if (!planned.plan) return;
  const auto copyUploadPlan = [&] {
    return skin::SkinResourceUploadPlan{
        .revision = planned.plan->revision.clone(),
        .images = planned.plan->images,
        .atlases = planned.plan->atlases,
        .decodedBytes = planned.plan->decodedBytes};
  };
  const auto rejectBeforeUpload = [&](std::string_view message,
                                      const auto &mutate) {
    auto rejectedPlan = copyUploadPlan();
    auto preflightDevice = std::make_shared<FakeTextureDevice>();
    mutate(rejectedPlan, *preflightDevice);
    const auto rejected = skin::SkinResourceCatalog::upload(
        std::move(rejectedPlan), preflightDevice);
    expect(!rejected.catalog && preflightDevice->creates == 0 &&
               preflightDevice->live == 0,
           message);
  };
  const auto sharedSixteenMiBPixels = [] {
    auto rgba = std::make_shared<std::vector<unsigned char>>(16U * 1024U *
                                                              1024U);
    return image_decode::DecodedImageData{.width = 2048,
                                          .height = 2048,
                                          .rgba = std::move(rgba)};
  };
  rejectBeforeUpload("zero device texture limit fails before upload or lease move",
                     [](auto &, auto &device) { device.maximumDimension = 0; });
  rejectBeforeUpload("effective device texture limit bounds every decoded image before upload",
                     [](auto &, auto &device) { device.maximumDimension = 1; });
  rejectBeforeUpload("resource-count cap rejects shared-pixel synthetic plans before upload",
                     [](auto &plan, auto &) {
                       const auto image = plan.images.front();
                       plan.images.clear();
                       for (skin::SkinResourceId id = 1;
                            id <= skin::SkinResourcePolicy::maximumResources;
                            ++id) {
                         auto copy = image;
                         copy.id = id;
                         copy.aliases.clear();
                         copy.aliasRegions.clear();
                         copy.aliasRegionMappings.clear();
                         plan.images.push_back(std::move(copy));
                       }
                       auto overflow = image;
                       overflow.id = skin::SkinResourcePolicy::maximumResources + 1;
                       overflow.aliases.clear();
                       overflow.aliasRegions.clear();
                       overflow.aliasRegionMappings.clear();
                       plan.images.push_back(std::move(overflow));
                     });
  rejectBeforeUpload("alias-count cap rejects before upload",
                     [](auto &plan, auto &) {
                       auto &image = plan.images.front();
                       image.aliases.clear();
                       image.aliasRegions.clear();
                       image.aliasRegionMappings.clear();
                       for (skin::SkinResourceId id = 4;
                            image.aliases.size() < skin::SkinResourcePolicy::maximumResources;
                            ++id) {
                         image.aliases.push_back(id);
                       }
                     });
  rejectBeforeUpload("duplicate image aliases fail complete preflight before upload",
                     [](auto &plan, auto &) {
                       plan.images.front().aliases.push_back(plan.images.back().id);
                     });
  rejectBeforeUpload("decoded image dimensions and shared RGBA byte count must agree",
                     [](auto &plan, auto &) { ++plan.images.front().pixels.width; });
  rejectBeforeUpload("canonical primary regions are verified before upload",
                     [](auto &plan, auto &) { plan.images.front().regions.front().x = -1; });
  rejectBeforeUpload("primary authored-to-resolved mappings cannot be substituted",
                     [](auto &plan, auto &) { ++plan.images.front().regionMappings.front().resolved.x; });
  rejectBeforeUpload("canonical alias regions are verified before upload",
                     [](auto &plan, auto &) { plan.images.front().aliasRegions.begin()->second.front().x = -1; });
  rejectBeforeUpload("alias authored-to-resolved mappings cannot be substituted",
                     [](auto &plan, auto &) { ++plan.images.front().aliasRegionMappings.begin()->second.front().resolved.x; });
  rejectBeforeUpload("aggregate image decoded-byte accounting rejects shared pixels before upload",
                     [&](auto &plan, auto &) {
                       const auto templateImage = plan.images.front();
                       const auto pixels = sharedSixteenMiBPixels();
                       plan.images.clear();
                       plan.atlases.clear();
                       for (skin::SkinResourceId id = 1; id <= 17; ++id) {
                         auto image = templateImage;
                         image.id = id;
                         image.aliases.clear();
                         image.aliasRegions.clear();
                         image.aliasRegionMappings.clear();
                         image.pixels = pixels;
                         plan.images.push_back(std::move(image));
                       }
                     });
  rejectBeforeUpload("upload plan decoded-byte totals must exactly match shared pixels",
                     [](auto &plan, auto &) { ++plan.decodedBytes; });
  rejectBeforeUpload("atlas-count cap rejects shared-pixel synthetic plans before upload",
                     [](auto &plan, auto &) {
                       const auto atlas = plan.atlases.front();
                       plan.atlases.clear();
                       for (skin::SkinTextAtlasId id = 1;
                            id <= skin::SkinResourcePolicy::maximumAtlases;
                            ++id) {
                         auto copy = atlas;
                         copy.id = id;
                         copy.key.pointSize += static_cast<int>(id);
                         plan.atlases.push_back(std::move(copy));
                       }
                       auto overflow = atlas;
                       overflow.id = skin::SkinResourcePolicy::maximumAtlases + 1;
                       overflow.key.pointSize +=
                           static_cast<int>(skin::SkinResourcePolicy::maximumAtlases + 1);
                       plan.atlases.push_back(std::move(overflow));
                     });
  rejectBeforeUpload("duplicate atlas IDs fail before upload",
                     [](auto &plan, auto &) { plan.atlases.back().id = plan.atlases.front().id; });
  rejectBeforeUpload("atlas styles must retain canonical finite keys",
                     [](auto &plan, auto &) { plan.atlases.front().key.outlineWidth = -1.0; });
  rejectBeforeUpload("atlas dimensions and shared RGBA byte counts must agree",
                     [](auto &plan, auto &) { ++plan.atlases.front().pixels.width; });
  rejectBeforeUpload("aggregate atlas decoded-byte accounting rejects shared pixels before upload",
                     [&](auto &plan, auto &) {
                       const auto templateAtlas = plan.atlases.front();
                       const auto pixels = sharedSixteenMiBPixels();
                       plan.atlases.clear();
                       for (skin::SkinTextAtlasId id = 1; id <= 9; ++id) {
                         auto atlas = templateAtlas;
                         atlas.id = id;
                         atlas.key.pointSize += static_cast<int>(id);
                         atlas.pixels = pixels;
                         plan.atlases.push_back(std::move(atlas));
                       }
                     });
  rejectBeforeUpload("atlas glyph regions must be canonical",
                     [](auto &plan, auto &) { plan.atlases.front().glyphs.begin()->second.region.x = -1; });
  rejectBeforeUpload("atlas kerning pairs must reference prepared glyphs",
                     [](auto &plan, auto &) { plan.atlases.front().kerning[{U'\U0010ffff', U'\U0010ffff'}] = 1; });
  rejectBeforeUpload("atlas metrics stay within fixed policy bounds",
                     [](auto &plan, auto &) { plan.atlases.front().lineHeight = skin::SkinResourcePolicy::maximumDimension + 1; });
  rejectBeforeUpload("atlas glyph-count cap rejects before upload",
                     [](auto &plan, auto &) {
                       auto &glyphs = plan.atlases.front().glyphs;
                       const auto metric = glyphs.begin()->second;
                       for (char32_t codepoint = U'\U0010fffe';
                            glyphs.size() <= skin::SkinResourcePolicy::maximumGlyphs;
                            --codepoint) {
                         glyphs.emplace(codepoint, metric);
                       }
                     });
  rejectBeforeUpload("atlas kerning-count cap rejects before upload",
                     [](auto &plan, auto &) {
                       auto &atlas = plan.atlases.front();
                       const auto metric = atlas.glyphs.begin()->second;
                       std::vector<char32_t> codepoints;
                       for (char32_t codepoint = U'\u1000'; codepoint < U'\u1082'; ++codepoint) {
                         atlas.glyphs.emplace(codepoint, metric);
                         codepoints.push_back(codepoint);
                       }
                       for (const char32_t left : codepoints) {
                         for (const char32_t right : codepoints) {
                           atlas.kerning.emplace(std::pair{left, right}, 0);
                           if (atlas.kerning.size() > skin::SkinResourcePolicy::maximumKerningPairs) return;
                         }
                       }
                     });
  rejectBeforeUpload("aggregate glyph-count cap rejects otherwise valid atlases before upload",
                     [](auto &plan, auto &) {
                       auto first = plan.atlases.front();
                       auto second = first;
                       second.id = first.id + 1;
                       ++second.key.pointSize;
                       for (auto *atlas : {&first, &second}) {
                         const auto metric = atlas->glyphs.begin()->second;
                         for (char32_t codepoint = U'\u3000';
                              atlas->glyphs.size() < 5000;
                              ++codepoint) {
                           atlas->glyphs.emplace(codepoint, metric);
                         }
                       }
                       plan.atlases = {std::move(first), std::move(second)};
                     });
  rejectBeforeUpload("aggregate kerning-count cap rejects otherwise valid atlases before upload",
                     [](auto &plan, auto &) {
                       auto first = plan.atlases.front();
                       auto second = first;
                       second.id = first.id + 1;
                       ++second.key.pointSize;
                       for (auto *atlas : {&first, &second}) {
                         const auto metric = atlas->glyphs.begin()->second;
                         std::vector<char32_t> codepoints;
                         for (char32_t codepoint = U'\u4000'; codepoint < U'\u4064'; ++codepoint) {
                           atlas->glyphs.emplace(codepoint, metric);
                           codepoints.push_back(codepoint);
                         }
                         for (const char32_t left : codepoints) {
                           for (const char32_t right : codepoints) {
                             atlas->kerning.emplace(std::pair{left, right}, 0);
                           }
                         }
                       }
                       plan.atlases = {std::move(first), std::move(second)};
                     });
  {
    skin::SkinResourceUploadPlan duplicatePlan{
        .revision = planned.plan->revision.clone(), .images = planned.plan->images,
        .atlases = planned.plan->atlases,
        .decodedBytes = planned.plan->decodedBytes};
    duplicatePlan.images.front().aliases.push_back(duplicatePlan.images.front().id);
    auto preflightDevice = std::make_shared<FakeTextureDevice>();
    const auto duplicateUpload = skin::SkinResourceCatalog::upload(std::move(duplicatePlan), preflightDevice);
    expect(!duplicateUpload.catalog && preflightDevice->creates == 0,
           "duplicate upload IDs fail complete preflight before any texture creation");
  }
  {
    skin::SkinResourceUploadPlan malformedMappingPlan{
        .revision = planned.plan->revision.clone(), .images = planned.plan->images,
        .atlases = planned.plan->atlases,
        .decodedBytes = planned.plan->decodedBytes};
    malformedMappingPlan.images.front().regionMappings.front().authored.x = -1;
    auto preflightDevice = std::make_shared<FakeTextureDevice>();
    const auto malformedMappingUpload =
        skin::SkinResourceCatalog::upload(std::move(malformedMappingPlan),
                                          preflightDevice);
    expect(!malformedMappingUpload.catalog && preflightDevice->creates == 0,
           "noncanonical authored mapping fails before upload or lease move");
  }
  std::mutex decoderMutex;
  std::condition_variable decoderCv;
  bool decoderStarted = false;
  skin::SkinResourcePreparationService gatedService(
      [&](std::span<const std::byte>, std::stop_token stop)
          -> std::optional<image_decode::DecodedImageData> {
        {
          std::lock_guard lock(decoderMutex);
          decoderStarted = true;
        }
        decoderCv.notify_all();
        while (!stop.stop_requested()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return std::nullopt;
      }, 1);
  std::optional<skin::SkinResourcePlanResult> stoppedPlan;
  std::jthread planningThread([&] {
    stoppedPlan = gatedService.decodeAndPlan({.revision=planned.plan->revision.clone(), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration});
  });
  {
    std::unique_lock lock(decoderMutex);
    expect(decoderCv.wait_for(lock, std::chrono::seconds(2), [&] { return decoderStarted; }),
           "injected resource decoder begins an in-flight planning operation");
  }
  std::jthread firstShutdown([&] { gatedService.shutdown(); });
  std::jthread secondShutdown([&] { gatedService.shutdown(); });
  firstShutdown.join();
  secondShutdown.join();
  planningThread.join();
  expect(stoppedPlan && stoppedPlan->cancelled && !stoppedPlan->plan,
         "concurrent shutdown stops the injectable decoder and prevents plan publication");
  const auto afterShutdown = gatedService.validateResources({.revision=planned.plan->revision.readView(), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration, .requiredRuntimeStrings=runtimeStrings});
  expect(!afterShutdown.valid && !afterShutdown.diagnostics.empty() &&
             afterShutdown.diagnostics.front().code == "skin.resource.service_stopped",
         "the service-owned stop state rejects synchronous validation after shutdown begins");
  skin::SkinResourceUploadPlan rollbackPlan{
      .revision = planned.plan->revision.clone(), .images = planned.plan->images, .atlases = planned.plan->atlases,
      .decodedBytes = planned.plan->decodedBytes};
  auto failingDevice = std::make_shared<FakeTextureDevice>();
  failingDevice->failAt = 2;
  const auto failedUpload = skin::SkinResourceCatalog::upload(std::move(rollbackPlan), failingDevice);
  expect(!failedUpload.catalog && failingDevice->creates == 2 &&
             failingDevice->destroys == 1 && failingDevice->live == 0,
         "a partial texture upload rolls back every prior unique handle exactly once");
  std::stop_source cancelled;
  cancelled.request_stop();
  const auto cancelledPlan = service.decodeAndPlan({.revision=planned.plan->revision.clone(), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration, .stop=cancelled.get_token()});
  expect(cancelledPlan.cancelled && !cancelledPlan.plan && cancelledPlan.diagnostics.empty(),
         "a stop request returns cancellation without publishing a partial plan");
  leasedFs.fileSystem.reset();
  auto device = std::make_shared<FakeTextureDevice>();
  auto uploaded = skin::SkinResourceCatalog::upload(std::move(*planned.plan), device);
  planned.plan.reset();
  expect(uploaded.catalog && device->creates == 6 && device->live == 6 &&
             uploaded.catalog->find(1) && uploaded.catalog->find(2) && uploaded.catalog->find(3) &&
             uploaded.catalog->findTextAtlas(1) && uploaded.catalog->findTextAtlas(2) && uploaded.catalog->findTextAtlas(3) && uploaded.catalog->findTextAtlas(4) &&
             firstAtlasKey && uploaded.catalog->findTextAtlas(*firstAtlasKey) &&
             uploaded.catalog->find(1)->regions.front().x == 0 &&
             uploaded.catalog->find(2)->regions.front().x == 2,
         "upload owns unique physical textures while preserving per-alias sprite regions");
  if (!uploaded.catalog) return;
  const skin::SkinSourceRect firstAuthored{.x=0,.y=0,.w=40,.h=20,.gridColumns=4,.gridRows=2};
  const skin::SkinSourceRect aliasAuthored{.x=2,.y=3,.w=20,.h=10,.gridColumns=2,.gridRows=1};
  const auto *firstMapped = uploaded.catalog->findResolvedRegion(1, firstAuthored);
  const auto *aliasMapped = uploaded.catalog->findResolvedRegion(2, aliasAuthored);
  expect(firstMapped && aliasMapped && firstMapped->resolved.x == 0 && firstMapped->resolved.w == 10 &&
             aliasMapped->resolved.x == 2 && aliasMapped->resolved.w == 10,
         "immutable resource lookup preserves each authored frame identity through resolution and alias reuse");
  const int readsBefore = device->creates;
  uploaded.catalog->enterRenderPhase();
  for (int frame = 0; frame != 120; ++frame) {
    (void)uploaded.catalog->find(1);
    (void)uploaded.catalog->find(2);
    (void)uploaded.catalog->find(9999);
    (void)uploaded.catalog->findTextAtlas(1);
  }
  expect(device->creates == readsBefore,
         "render-phase ID lookups do not upload or access resource files");
  std::weak_ptr<skin::SkinTextureDevice> backendLifetime = device;
  device.reset();
  expect(!backendLifetime.expired(),
         "catalog structurally retains the texture backend through its owner-thread teardown");
  uploaded.catalog.reset();
  expect(backendLifetime.expired(),
         "catalog teardown releases the backend after once-only destruction");
  expect(!weak.hasLiveLease(),
         "catalog teardown releases the revision lease");
}
}

int main() {
  testBoundedPngAndJpegDecodeBeforeAllocation();
  testSharedSdlTtfRuntimeFinalRelease();
  testSpriteBoundsAndNormalizedGridCells();
  testTextAtlasKeyRejectsNegativePaintExtents();
  testSecurePreparationLeaseAliasAndCatalogLifetime();
  if (failures) return 1;
  std::cout << "Skin resource catalog tests passed\n";
  return 0;
}
