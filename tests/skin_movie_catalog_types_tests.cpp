#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/SkinMovieCatalog.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"
#include "skin/SkinStoragePaths.h"

#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {
int failures = 0;
void expect(bool value, std::string_view message) { if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; } }

struct TemporaryDirectory {
  TemporaryDirectory() : root(std::filesystem::temp_directory_path() /
                              ("asobmashow-movie-types-" + std::to_string(++serial))) {
    std::filesystem::create_directories(root);
  }
  ~TemporaryDirectory() { std::error_code error; std::filesystem::remove_all(root, error); }
  std::filesystem::path root;
  static inline std::atomic_uint64_t serial = 0;
};

struct FakeMovieDevice final : skin::SkinMovieDevice {
  std::optional<skin::SkinMovieLoadResult>
  load(const std::filesystem::path &path,
       const skin::SkinMovieLoadLimits &limits, std::stop_token) override {
    ++loads;
    lastLimits = limits;
    loadedPaths.push_back(path);
    pathExistedDuringLoad = pathExistedDuringLoad &&
                            std::filesystem::is_regular_file(path);
    if (failAt != 0 && loads == failAt) {
      return std::nullopt;
    }
    const auto layout =
        skin::skinMovieDecodedLayout(resultWidth, resultHeight, limits);
    if (!layout) {
      return std::nullopt;
    }
    ++expensiveAllocations;
    const auto handle = skin::SkinMoviePlayerHandle{
        static_cast<std::uint64_t>(loads)};
    live.push_back(handle);
    return skin::SkinMovieLoadResult{
        .handle = handle,
        .width = resultWidth,
        .height = resultHeight,
        .durationMillis = 1'000,
        .decodedBytes = layout->residentBytes};
  }

  void destroy(skin::SkinMoviePlayerHandle handle) noexcept override {
    ++destroys;
    const auto found = std::ranges::find(live, handle);
    if (found != live.end()) {
      live.erase(found);
    }
  }

  bool ownsCurrentThread() const noexcept override { return true; }
  void beginFrame() noexcept override { ++begins; }
  skin::SkinMovieFramePreparationResult prepareFrame(
      skin::SkinMoviePlayerHandle, const skin::SkinMovieCommand &command,
      const skin::PlaySkinViewport &) override {
    ++prepares;
    preparedTimes.push_back(command.sourceTimeMillis);
    return {.ready = true, .drawable = true};
  }
  void discardFrame() noexcept override { ++discards; }
  void commitFrame() noexcept override { ++commits; }
  void submitPrepared(std::size_t index) noexcept override {
    submitted.push_back(index);
  }

  int loads = 0;
  int destroys = 0;
  int begins = 0;
  int prepares = 0;
  int discards = 0;
  int commits = 0;
  int failAt = 0;
  int expensiveAllocations = 0;
  int resultWidth = 80;
  int resultHeight = 40;
  skin::SkinMovieLoadLimits lastLimits;
  bool pathExistedDuringLoad = true;
  std::vector<std::filesystem::path> loadedPaths;
  std::vector<skin::SkinMoviePlayerHandle> live;
  std::vector<std::int64_t> preparedTimes;
  std::vector<std::size_t> submitted;
};

struct LeasedMoviePackage {
  skin::SkinPackageId package;
  skin::SkinStorageRoots roots;
  skin::SkinEntryId entry;
  std::shared_ptr<skin::SkinAliasDetector> aliases;
  std::optional<skin::SkinRevisionLease> lease;
  std::unique_ptr<skin::LuaSkinFileSystem> fileSystem;

  static std::optional<LeasedMoviePackage>
  create(const std::string &packageId,
         const std::filesystem::path &sourceRoot) {
    LeasedMoviePackage result;
    result.package =
        *skin::normalizePackageId(packageId).package;
    result.entry = *skin::normalizeEntryPath(
        result.package, "entry/play.luaskin").entry;
    result.roots = skin::SkinStorageRoots{
        .visiblePackages = sourceRoot / "visible",
        .privateRevisions = sourceRoot / "revisions",
        .privateCatalog = sourceRoot / "catalog",
        .profileOverlays = sourceRoot / "overlays",
        .liveSources = true};
    result.aliases = skin::createPlatformSkinAliasDetector();
    skin::SkinTreeSnapshotter snapshotter(result.roots, *result.aliases);
    auto snapshot = snapshotter.snapshot(
        result.roots.visiblePackages / packageId, result.package, {}, {});
    if (!snapshot.prepared) {
      return std::nullopt;
    }
    std::string publishError;
    result.lease = std::move(*snapshot.prepared).publish(publishError);
    if (!result.lease) {
      return std::nullopt;
    }
    result.fileSystem = skin::LuaSkinFileSystem::create(
        {.revision = result.lease->readView(),
         .entry = result.entry,
         .storageRoots = result.roots}).fileSystem;
    if (!result.fileSystem) {
      return std::nullopt;
    }
    return result;
  }
};

// One SkinImageResource plus a used SkinImageObject that references it, which
// is the shape a JSON/Lua select skin produces for an image binary.
skin::ValidatedBeatorajaSkinModel singleImageModel(std::string virtualPath) {
  skin::ValidatedBeatorajaSkinModel model;
  const std::string authoredName =
      virtualPath.ends_with(".png") ? "still" : "movie";
  model.model.resources.emplace_back(skin::SkinImageResource{
      .id = 1, .authoredName = authoredName, .virtualPath = std::move(virtualPath)});
  model.model.objects.push_back(
      {.id = 1,
       .authoredName = "image-object",
       .payload = skin::SkinImageObject{.orderedStates = {{
           .resource = 1, .frames = {{.x = 0, .y = 0, .w = 40, .h = 20}}}}},
       .critical = true});
  return model;
}

void testMovieExtensionImageResourcePromotesToPreparedMovie() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path sourceRoot = temporary.root;
  const fs::path skinRoot = sourceRoot / "visible" / "PromotionFixture";
  fs::create_directories(skinRoot / "entry/resources");
  std::ofstream(skinRoot / "entry/play.luaskin") << "return {}\n";
  std::ofstream(skinRoot / "entry/resources/movie.mp4", std::ios::binary)
      << "fake movie bytes\n";
  auto leased = LeasedMoviePackage::create("PromotionFixture", sourceRoot);
  expect(leased.has_value() && leased->fileSystem != nullptr,
         "movie promotion fixture publishes and leases its resource filesystem");
  if (!leased || !leased->fileSystem) {
    return;
  }

  const auto model = singleImageModel("resources/movie.mp4");
  skin::BeatorajaSkinConfiguration configuration;
  auto movieDevice = std::make_shared<FakeMovieDevice>();
  auto movies = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leased->fileSystem,
       .model = model,
       .configuration = configuration,
       .device = movieDevice});
  expect(movies.catalog && !movies.cancelled && movies.diagnostics.empty() &&
             movieDevice->loads == 1 && movieDevice->live.size() == 1 &&
             movieDevice->pathExistedDuringLoad &&
             movies.catalog->decodedBytes() > 0,
         "a movie-extension image source promotes to a prepared movie and "
         "loads exactly once with a bounded decoded working set");
  if (movies.catalog) {
    const auto *prepared = movies.catalog->findMovie(1);
    expect(prepared != nullptr && prepared->resource.id == 1 &&
               prepared->resource.virtualPath == "resources/movie.mp4",
           "findMovie returns the promoted resource under the image id");
    if (prepared) {
      skin::SkinMovieCommand movieCommand{.resource = 1, .sourceTimeMillis = 375};
      const std::array<const skin::SkinMovieCommand *, 1> movieCommands{
          &movieCommand};
      const skin::PlaySkinViewport movieViewport{.valid = true};
      const auto preparedMovieFrame =
          movies.catalog->prepareFrame(movieCommands, movieViewport);
      movies.catalog->commitFrame();
      movies.catalog->submitPrepared(0);
      movies.catalog->discardFrame();
      expect(preparedMovieFrame.ready && movieDevice->begins == 1 &&
                 movieDevice->prepares == 1 &&
                 movieDevice->preparedTimes ==
                     std::vector<std::int64_t>{375} &&
                 movieDevice->commits == 1 &&
                 movieDevice->submitted == std::vector<std::size_t>{0},
             "a promoted movie participates in prepare, commit, and submit");
    }
  }
  movies.catalog.reset();
  expect(movieDevice->destroys == 1 && movieDevice->live.empty(),
         "promoted movie catalog teardown destroys its player exactly once");
}

void testSharedMovieExtensionPathMaterializesOnce() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path sourceRoot = temporary.root;
  const fs::path skinRoot = sourceRoot / "visible" / "DedupFixture";
  fs::create_directories(skinRoot / "entry/resources");
  std::ofstream(skinRoot / "entry/play.luaskin") << "return {}\n";
  std::ofstream(skinRoot / "entry/resources/movie.mp4", std::ios::binary)
      << "fake movie bytes\n";
  auto leased = LeasedMoviePackage::create("DedupFixture", sourceRoot);
  expect(leased.has_value() && leased->fileSystem != nullptr,
         "movie dedup fixture publishes and leases its resource filesystem");
  if (!leased || !leased->fileSystem) {
    return;
  }

  skin::ValidatedBeatorajaSkinModel model;
  model.model.resources.emplace_back(skin::SkinImageResource{
      .id = 1, .authoredName = "movie-one", .virtualPath = "resources/movie.mp4"});
  model.model.resources.emplace_back(skin::SkinImageResource{
      .id = 2, .authoredName = "movie-two", .virtualPath = "resources/movie.mp4"});
  model.model.objects.push_back(
      {.id = 1,
       .authoredName = "movie-one-object",
       .payload = skin::SkinImageObject{.orderedStates = {{.resource = 1}}},
       .critical = true});
  model.model.objects.push_back(
      {.id = 2,
       .authoredName = "movie-two-object",
       .payload = skin::SkinImageObject{.orderedStates = {{.resource = 2}}},
       .critical = true});
  skin::BeatorajaSkinConfiguration configuration;
  auto movieDevice = std::make_shared<FakeMovieDevice>();
  auto movies = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leased->fileSystem,
       .model = model,
       .configuration = configuration,
       .device = movieDevice});
  expect(movies.catalog && !movies.cancelled && movies.diagnostics.empty() &&
             movieDevice->loads == 1 && movieDevice->live.size() == 1 &&
             movies.catalog->findMovie(1) && movies.catalog->findMovie(2) &&
             movies.catalog->findMovie(1)->handle ==
                 movies.catalog->findMovie(2)->handle &&
             movies.catalog->findMovie(1)->resource.id == 1 &&
             movies.catalog->findMovie(2)->resource.id == 2,
         "two image resources sharing one movie path materialize together and "
         "both ids resolve to the same prepared handle");
}

void testStillExtensionImageResourceStaysImage() {
  namespace fs = std::filesystem;
  TemporaryDirectory temporary;
  const fs::path sourceRoot = temporary.root;
  const fs::path skinRoot = sourceRoot / "visible" / "StillFixture";
  fs::create_directories(skinRoot / "entry/resources");
  std::ofstream(skinRoot / "entry/play.luaskin") << "return {}\n";
  std::ofstream(skinRoot / "entry/resources/movie.png", std::ios::binary)
      << "fake still bytes\n";
  auto leased = LeasedMoviePackage::create("StillFixture", sourceRoot);
  expect(leased.has_value() && leased->fileSystem != nullptr,
         "still-image fixture publishes and leases its resource filesystem");
  if (!leased || !leased->fileSystem) {
    return;
  }

  const auto model = singleImageModel("resources/movie.png");
  skin::BeatorajaSkinConfiguration configuration;
  auto movieDevice = std::make_shared<FakeMovieDevice>();
  auto movies = skin::SkinMovieCatalog::prepare(
      {.fileSystem = *leased->fileSystem,
       .model = model,
       .configuration = configuration,
       .device = movieDevice});
  expect(movies.catalog && !movies.cancelled && movies.diagnostics.empty() &&
             movies.catalog->movieCount() == 0 &&
             movies.catalog->findMovie(1) == nullptr &&
             movieDevice->loads == 0 && movieDevice->live.empty(),
         "a still-extension image source is not promoted and stays an image");
}

} // namespace

int main() {
  testMovieExtensionImageResourcePromotesToPreparedMovie();
  testSharedMovieExtensionPathMaterializesOnce();
  testStillExtensionImageResourceStaysImage();
  if (failures) return 1;
  std::cout << "Skin movie catalog type-promotion tests passed\n";
  return 0;
}