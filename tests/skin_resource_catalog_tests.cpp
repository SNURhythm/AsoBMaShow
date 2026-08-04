#include "skin/beatoraja/SkinResourceCatalog.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"
#include "view/ImageFileDecoder.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>

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
  int creates = 0;
  int destroys = 0;
  int live = 0;
  int failAt = 0;
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
  skin::BeatorajaSkinConfiguration configuration;
  skin::SkinResourcePreparationService service;
  const auto validated = service.validateResources({.revision=snapshot.prepared->readView(), .entry=entry, .fileSystem=*stagedFs.fileSystem, .model=model, .configuration=configuration});
  expect(validated.valid && !validated.cancelled,
         "synchronous validation consumes staged resource bytes without retaining the read view");
  stagedFs.fileSystem.reset();
  std::string publishError;
  auto lease = std::move(*snapshot.prepared).publish(publishError);
  expect(lease && publishError.empty(), "staged revision publishes after non-retaining validation");
  if (!lease) return;
  const auto weak = lease->weakPin();
  auto leasedFs = skin::LuaSkinFileSystem::create({.revision=lease->readView(), .entry=entry, .storageRoots=roots});
  expect(leasedFs.fileSystem != nullptr, "published resource filesystem is available");
  if (!leasedFs.fileSystem) return;
  auto planned = service.decodeAndPlan({.revision=std::move(*lease), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration});
  expect(planned.plan && planned.plan->images.size() == 2 &&
             planned.plan->images.front().aliases == std::vector<skin::SkinResourceId>{2},
         "duplicate resource locators decode once and retain an ID alias record");
  if (!planned.plan) return;
  skin::SkinResourceUploadPlan rollbackPlan{
      .revision = planned.plan->revision.clone(), .images = planned.plan->images};
  FakeTextureDevice failingDevice;
  failingDevice.failAt = 2;
  const auto failedUpload = skin::SkinResourceCatalog::upload(std::move(rollbackPlan), failingDevice);
  expect(!failedUpload.catalog && failingDevice.creates == 2 &&
             failingDevice.destroys == 1 && failingDevice.live == 0,
         "a partial texture upload rolls back every prior unique handle exactly once");
  std::stop_source cancelled;
  cancelled.request_stop();
  const auto cancelledPlan = service.decodeAndPlan({.revision=planned.plan->revision.clone(), .entry=entry, .fileSystem=*leasedFs.fileSystem, .model=model, .configuration=configuration, .stop=cancelled.get_token()});
  expect(cancelledPlan.cancelled && !cancelledPlan.plan && cancelledPlan.diagnostics.empty(),
         "a stop request returns cancellation without publishing a partial plan");
  FakeTextureDevice device;
  auto uploaded = skin::SkinResourceCatalog::upload(std::move(*planned.plan), device);
  expect(uploaded.catalog && device.creates == 2 && device.live == 2 &&
             uploaded.catalog->find(1) && uploaded.catalog->find(2) && uploaded.catalog->find(3),
         "upload owns unique physical textures while exposing immutable aliases");
  if (!uploaded.catalog) return;
  const int readsBefore = device.creates;
  uploaded.catalog->enterRenderPhase();
  for (int frame = 0; frame != 120; ++frame) {
    (void)uploaded.catalog->find(1);
    (void)uploaded.catalog->find(2);
    (void)uploaded.catalog->find(9999);
  }
  expect(device.creates == readsBefore,
         "render-phase ID lookups do not upload or access resource files");
  uploaded.catalog.reset();
  expect(device.destroys == 2 && device.live == 0 && !weak.hasLiveLease(),
         "catalog teardown destroys each unique texture once and releases the revision lease");
}
}

int main() {
  testBoundedPngAndJpegDecodeBeforeAllocation();
  testSpriteBoundsAndNormalizedGridCells();
  testSecurePreparationLeaseAliasAndCatalogLifetime();
  if (failures) return 1;
  std::cout << "Skin resource catalog tests passed\n";
  return 0;
}
