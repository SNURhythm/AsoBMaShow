#include "scene/play/PlayfieldChartVisualModel.h"
#include "rendering/common.h"
#include "scene/play/PlayfieldProjection.h"
#include "scene/play/PlayfieldVisualState.h"
#include "skin/SkinConfigurationWriteQueue.h"
#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/GameplaySkinValidator.h"
#if __has_include("skin/beatoraja/GameplaySkinSourceFormat.h")
#include "skin/beatoraja/GameplaySkinSourceFormat.h"
#define ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS 1
#else
#define ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS 0
#endif
#include "skin/beatoraja/PlaySkinSession.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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
} // namespace rendering

namespace {

namespace fs = std::filesystem;
using namespace skin;

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

class TempDirectory final {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-skin-loading-benchmark-" +
               std::to_string(++serial));
    } while (!fs::create_directory(root_));
  }
  ~TempDirectory() {
    std::error_code ignored;
    fs::remove_all(root_, ignored);
  }
  const fs::path &root() const noexcept { return root_; }

private:
  fs::path root_;
};

class AcceptFiles final : public SkinAliasDetector {
public:
  SkinRejectedLinkKind inspectNoFollow(const fs::path &) const override {
    return SkinRejectedLinkKind::None;
  }
};

class BenchmarkTextureDevice final : public SkinTextureDevice {
public:
  bgfx::TextureHandle
  create(const image_decode::DecodedImageData &) override {
    ++created;
    return bgfx::TextureHandle{next_++};
  }
  void destroy(bgfx::TextureHandle) noexcept override { ++destroyed; }
  bool ownsCurrentThread() const noexcept override { return true; }

  std::size_t created = 0;
  std::size_t destroyed = 0;

private:
  std::uint16_t next_ = 1;
};

struct BenchmarkOptions {
  bool benchmark = false;
  bool warm = false;
  std::size_t samples = 1;
  std::optional<fs::path> skin;
  bool luaOnly = false;
};

std::optional<BenchmarkOptions> parseOptions(int argc, char **argv) {
  BenchmarkOptions result;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--benchmark") {
      result.benchmark = true;
    } else if (argument == "--mode" && index + 1 < argc) {
      const std::string_view mode(argv[++index]);
      if (mode != "cold" && mode != "warm") return std::nullopt;
      result.warm = mode == "warm";
    } else if (argument == "--samples" && index + 1 < argc) {
      try {
        result.samples = std::stoull(argv[++index]);
      } catch (...) {
        return std::nullopt;
      }
      if (result.samples == 0 || result.samples > 100) return std::nullopt;
    } else if (argument == "--skin" && index + 1 < argc) {
      result.skin = fs::path(argv[++index]);
    } else if (argument == "--format" && index + 1 < argc) {
      if (std::string_view(argv[++index]) != "lua") return std::nullopt;
      result.luaOnly = true;
    } else {
      return std::nullopt;
    }
  }
  return result;
}

class BenchmarkFixture final {
public:
  BenchmarkFixture(const std::optional<fs::path> &localSkin, bool luaOnly)
      : roots_{.visiblePackages = temp_.root() / "visible",
               .privateRevisions = temp_.root() / "revisions",
               .privateCatalog = temp_.root() / "catalog",
               .profileOverlays = temp_.root() / "overlays"},
        package_(*normalizePackageId("LoadingBenchmark").package),
        profile_(*makeSkinProfileId(
            "99999999-9999-4999-8999-999999999999")) {
    chart_.keyCount = 7;
    chart_.text.title = "loading benchmark";
    chart_.text.artist = "AsoBMaShow tests";
    chart_.text.fullArtist = chart_.text.artist;
    initialState_.clock.serial = 1;
    initialState_.clock.visualTimeMicros = 0;
    initialState_.clock.gameplayTimeMicros = 0;
    initialState_.authority.loadingState = PlayfieldLoadingState::Loaded;
    initialProjection_.frameSerial = 1;

    fs::path source = temp_.root() / "source";
    if (localSkin) {
      if (!prepareLocalSkin(*localSkin, source)) return;
    } else {
      prepareSyntheticFormats(source, luaOnly);
    }
    fs::create_directories(roots_.visiblePackages);
    fs::copy(source, roots_.visiblePackages / package_.directoryName,
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing);
    SkinTreeSnapshotter snapshotter(roots_, aliases_);
    auto snapshot = snapshotter.snapshot(source, package_, {}, {});
    if (!snapshot.prepared) return;
    std::string error;
    lease_ = std::move(*snapshot.prepared).publish(error);
    if (!lease_ || !error.empty()) {
      lease_.reset();
      return;
    }
    for (const auto &relative : entryPaths_) {
      const auto normalized = normalizeEntryPath(package_, relative).entry;
      if (!normalized) continue;
      GameplaySkinValidator validator(validationResources_);
      auto validated = validator.validate(lease_->readView(), *normalized,
                                          nullptr, {});
      if (validated.disposition !=
              SkinValidationDisposition::SelectableGameplay ||
          !validated.reconciledSettings ||
          validated.configurationDigest.empty()) {
        continue;
      }
      entries_.push_back({.entry = *normalized,
                          .settings = *validated.reconciledSettings,
                          .digest = std::move(validated.configurationDigest)});
    }
  }

  bool ready() const noexcept {
    return lease_.has_value() && !entries_.empty();
  }

  std::optional<std::uint64_t>
  runSample(SkinResourcePreparationService &preparation,
            std::uint64_t &sessionSerial) {
    const auto started = std::chrono::steady_clock::now();
    std::uint64_t total = 0;
    for (const auto &entry : entries_) {
      auto device = std::make_shared<BenchmarkTextureDevice>();
      auto counters = std::make_shared<SkinLiveResourceCounters>();
      auto created = PlaySkinSession::create(
          {.revision = lease_->clone(),
           .entry = entry.entry,
           .reconciledSettings = entry.settings,
           .configurationDigest = entry.digest},
          {.sessionSerial = ++sessionSerial,
           .profileId = profile_,
           .chartModel = chart_,
           .initialState = &initialState_,
           .initialProjection = &initialProjection_,
           .safeUiBounds = {.x = 0.0,
                            .y = 0.0,
                            .width = 1280.0,
                            .height = 720.0},
           .storageRoots = roots_,
           .resourcePreparation = preparation,
           .textureDevice = device,
           .liveResourceCounters = counters,
           .configurationWrites = configurationWrites_});
      if (!created.session) {
        return std::nullopt;
      }
#if defined(ASOBMASHOW_SKIN_LOADING_TELEMETRY)
      if (!isCompleteSkinLoadingTelemetry(created.loadingTelemetry)) {
        return std::nullopt;
      }
#endif
      created.session.reset();
      if (device->created != device->destroyed ||
          counters->snapshot() != SkinLiveResourceSnapshot{}) {
        return std::nullopt;
      }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    total = elapsed.count() <= 0 ? 0U
                                 : static_cast<std::uint64_t>(elapsed.count());
    return total;
  }

  std::size_t formatCount() const noexcept { return entries_.size(); }

private:
  struct Entry {
    SkinEntryId entry;
    EntryProfileSettings settings;
    std::string digest;
  };

  void prepareSyntheticFormats(const fs::path &source, bool luaOnly) {
    constexpr int objectCount = 48;
    fs::create_directories(source / "skin/resources");
    fs::copy_file(fs::path(ASOBMASHOW_SOURCE_DIR) /
                      "tests/fixtures/beatoraja_skin/resources/fixture.png",
                  source / "skin/resources/fixture.png");
    writeText(source / "skin/parity.luaskin", R"lua(
local skin = {type=0, name="Lua loading", author="fixture", w=1280, h=720}
if skin_config then
  skin.source = {{id="atlas", path="resources/fixture.png"}}
  skin.image = {}
  skin.destination = {}
  for index = 1, 48 do
    local id = "image" .. index
    skin.image[index] = {id=id, src="atlas", x=0, y=0, w=40, h=20}
    skin.destination[index] = {
      id=id, dst={{x=(index - 1) * 3, y=(index - 1) % 8 * 22,
                  w=40, h=20}}
    }
  end
end
return skin
)lua");
    entryPaths_ = {"skin/parity.luaskin"};
#if ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS
    if (luaOnly) return;
    std::string json =
        R"json({"type":0,"name":"JSON loading","author":"fixture","w":1280,"h":720,"source":[{"id":"atlas","path":"resources/fixture.png"}],"image":[)json";
    for (int index = 1; index <= objectCount; ++index) {
      if (index != 1) json += ',';
      json += "{\"id\":\"image" + std::to_string(index) +
              "\",\"src\":\"atlas\",\"x\":0,\"y\":0,\"w\":40,\"h\":20}";
    }
    json += R"json(],"destination":[)json";
    for (int index = 1; index <= objectCount; ++index) {
      if (index != 1) json += ',';
      json += "{\"id\":\"image" + std::to_string(index) +
              "\",\"dst\":[{\"x\":" + std::to_string((index - 1) * 3) +
              ",\"y\":" + std::to_string((index - 1) % 8 * 22) +
              ",\"w\":40,\"h\":20}]}";
    }
    json += "]}";
    writeText(source / "skin/parity.json", json);

    std::string lr2 = "#INFORMATION,0,LR2 loading,fixture\n#RESOLUTION,1\n"
                      "#IMAGE,resources/fixture.png\n";
    for (int index = 0; index < objectCount; ++index) {
      lr2 += "#SRC_IMAGE," + std::to_string(index) +
             ",0,0,0,40,20,1,1,0,0\n";
      lr2 += "#DST_IMAGE," + std::to_string(index) + ",0," +
             std::to_string(index * 3) + ',' +
             std::to_string(index % 8 * 22) +
             ",40,20,0,255,255,255,255,0,0,0,0,0,0,0,0,0\n";
    }
    writeText(source / "skin/parity.lr2skin", lr2);
    entryPaths_ = {"skin/parity.luaskin", "skin/parity.json",
                   "skin/parity.lr2skin"};
#else
    (void)luaOnly;
#endif
  }

  bool prepareLocalSkin(const fs::path &input, const fs::path &destination) {
    std::error_code error;
    if (fs::is_regular_file(input, error)) {
      fs::copy(input.parent_path(), destination,
               fs::copy_options::recursive |
                   fs::copy_options::overwrite_existing, error);
      if (error) return false;
      entryPaths_ = {input.filename().generic_string()};
      return true;
    }
    if (!fs::is_directory(input, error)) return false;
    fs::copy(input, destination,
             fs::copy_options::recursive |
                 fs::copy_options::overwrite_existing, error);
    if (error) return false;
    for (const auto &item : fs::recursive_directory_iterator(input, error)) {
      if (error) return false;
      if (!item.is_regular_file()) continue;
      const auto relative = item.path().lexically_relative(input);
#if ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS
      if (gameplaySkinSourceFormatForPath(relative.generic_string())) {
        entryPaths_.push_back(relative.generic_string());
      }
#else
      if (relative.extension() == ".luaskin") {
        entryPaths_.push_back(relative.generic_string());
      }
#endif
    }
    std::ranges::sort(entryPaths_);
    return !entryPaths_.empty();
  }

  TempDirectory temp_;
  SkinStorageRoots roots_;
  SkinPackageId package_;
  SkinProfileId profile_;
  AcceptFiles aliases_;
  PlayfieldChartVisualModel chart_;
  PlayfieldVisualState initialState_;
  PlayfieldProjectionResult initialProjection_;
  SkinResourcePreparationService validationResources_;
  SkinConfigurationWriteQueue configurationWrites_;
  std::vector<std::string> entryPaths_;
  std::vector<Entry> entries_;
  std::optional<SkinRevisionLease> lease_;
};

std::optional<std::vector<std::uint64_t>>
runSamples(BenchmarkFixture &fixture, bool warm, std::size_t samples) {
  std::vector<std::uint64_t> values;
  values.reserve(samples);
  std::uint64_t serial = 100;
  SkinResourcePreparationService warmPreparation;
  for (std::size_t index = 0; index < samples; ++index) {
    if (warm) {
      const auto sample = fixture.runSample(warmPreparation, serial);
      if (!sample) return std::nullopt;
      values.push_back(*sample);
    } else {
      SkinResourcePreparationService coldPreparation;
      const auto sample = fixture.runSample(coldPreparation, serial);
      if (!sample) return std::nullopt;
      values.push_back(*sample);
    }
  }
  return values;
}

std::uint64_t median(std::vector<std::uint64_t> values) {
  std::ranges::sort(values);
  return values[values.size() / 2U];
}

} // namespace

int main(int argc, char **argv) {
  const auto options = parseOptions(argc, argv);
  if (!options) {
    std::cerr << "usage: gameplay_skin_loading_benchmark_tests "
                 "[--benchmark --mode cold|warm --samples N] [--skin PATH]\n";
    return 2;
  }
  BenchmarkFixture fixture(options->skin, options->luaOnly);
  if (!fixture.ready()) {
    std::cerr << "gameplay skin loading benchmark fixture is unavailable\n";
    return 1;
  }
  if (!options->benchmark) {
    const auto cold = runSamples(fixture, false, 1);
    const auto warm = runSamples(fixture, true, 2);
    expect(cold && warm && cold->size() == 1 && warm->size() == 2 &&
               fixture.formatCount() ==
                   (ASOBMASHOW_BENCHMARK_HAS_STATIC_FORMATS ? 3U : 1U),
           "cold and warm production session creation covers Lua/JSON/LR2");
    if (failures == 0) {
      std::cout << "Gameplay skin loading benchmark tests passed\n";
    }
    return failures == 0 ? 0 : 1;
  }
  const auto samples = runSamples(fixture, options->warm, options->samples);
  if (!samples) return 1;
  std::cout << "{\"mode\":\"" << (options->warm ? "warm" : "cold")
            << "\",\"formats\":" << fixture.formatCount()
            << ",\"sampleCount\":" << samples->size()
            << ",\"medianMicros\":" << median(*samples)
            << ",\"samplesMicros\":[";
  for (std::size_t index = 0; index < samples->size(); ++index) {
    if (index != 0) std::cout << ',';
    std::cout << (*samples)[index];
  }
  std::cout << "]}\n";
  return 0;
}
