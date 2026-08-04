#include "skin/beatoraja/LuaSkinTableDecoder.h"
#include "skin/SkinStoragePaths.h"
#include "skin/beatoraja/LuaSkinFileSystem.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

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

class TempDirectory {
public:
  TempDirectory() {
    static std::atomic_uint64_t serial{0};
    do {
      root_ = fs::temp_directory_path() /
              ("asobmashow-lua-live-node-test-" + std::to_string(++serial));
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

void writeText(const fs::path &path, std::string_view value) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(value.data(), static_cast<std::streamsize>(value.size()));
}

BeatorajaSkinModelDecodeResult decodeInline(std::string_view sourceText) {
  TempDirectory temp;
  const SkinStorageRoots roots{
      .visiblePackages = temp.root() / "visible",
      .privateRevisions = temp.root() / "revisions",
      .privateCatalog = temp.root() / "catalog",
      .profileOverlays = temp.root() / "overlays",
  };
  const auto package = *normalizePackageId("LiveNodeContract").package;
  const fs::path source = temp.root() / "source";
  writeText(source / "skin/model.luaskin", sourceText);

  AcceptFiles aliases;
  SkinTreeSnapshotter snapshotter(roots, aliases);
  auto snapshot = snapshotter.snapshot(source, package, {}, {});
  expect(snapshot.prepared.has_value(), "live-node fixture snapshots");
  if (!snapshot.prepared) {
    return {};
  }

  const auto entry = *normalizeEntryPath(package, "skin/model.luaskin").entry;
  const auto makeFileSystem = [&]() {
    return LuaSkinFileSystem::create({.revision = snapshot.prepared->readView(),
                                      .entry = entry,
                                      .storageRoots = roots})
        .fileSystem;
  };
  auto runtimeFiles = makeFileSystem();
  auto reconciliationFiles = makeFileSystem();
  expect(runtimeFiles != nullptr && reconciliationFiles != nullptr,
         "live-node filesystems create");
  if (!runtimeFiles || !reconciliationFiles) {
    return {};
  }

  auto created = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::Validation,
       .fileSystem = std::move(runtimeFiles)});
  expect(created.runtime != nullptr, "live-node runtime creates");
  if (!created.runtime) {
    return {};
  }
  auto headerValue = created.runtime->loadHeader();
  expect(headerValue.value.has_value(), "live-node header executes");
  if (!headerValue.value) {
    return {};
  }
  LuaSkinTableDecoder decoder;
  const auto header = decoder.decodeHeader(*headerValue.value);
  expect(header.header.has_value(), "live-node header decodes");
  if (!header.header) {
    return {};
  }
  headerValue.value.reset();

  const auto reconciled = reconcileSkinConfiguration(
      *header.header, nullptr, *reconciliationFiles);
  expect(reconciled.configuration.has_value(),
         "live-node configuration reconciles");
  if (!reconciled.configuration) {
    return {};
  }
  auto configured =
      created.runtime->loadConfigured(*reconciled.configuration);
  expect(configured.value.has_value(), "live-node configured phase executes");
  if (!configured.value) {
    return {};
  }
  return decoder.decodeGameplay(*configured.value,
                                {.runtime = *created.runtime});
}

const SkinObjectDefinition *objectNamed(const BeatorajaSkinModel &model,
                                        std::string_view name) {
  const auto found = std::ranges::find_if(model.objects, [&](const auto &item) {
    return item.authoredName == name;
  });
  return found != model.objects.end() ? &*found : nullptr;
}

std::string gaugeFixture(std::size_t nodeCount, int animationType) {
  std::ostringstream lua;
  lua << "return {type=0,w=1280,h=720,"
         "source={{id='atlas',path='atlas.png'}},image={";
  for (std::size_t index = 0; index < nodeCount; ++index) {
    lua << "{id='node-" << index
        << "',src='atlas',x=" << index * 20
        << ",y=0,w=20,h=10,divx=2,timer=" << 100 + index
        << ",cycle=" << 200 + index << "},";
  }
  lua << "},gauge={id='gauge',nodes={";
  for (std::size_t index = 0; index < nodeCount; ++index) {
    lua << "'node-" << index << "',";
  }
  lua << "},parts=" << 40 + animationType << ",type=" << animationType
      << ",range=" << 10 + animationType
      << ",cycle=" << 20 + animationType
      << ",starttime=" << -10 - animationType
      << ",endtime=" << 700 + animationType
      << "},destination={{id='gauge',dst={{}}}}}";
  return lua.str();
}

void testLiveGaugeUsesEveryPinnedNodeLayout() {
  struct Case {
    std::size_t nodeCount;
    int animationType;
    std::array<int, 8> firstSources;
  };
  constexpr std::array cases{
      Case{4, 0, {0, 1, 2, 3, 0, 1, 0, 1}},
      Case{8, 1, {4, 5, 6, 7, 4, 5, 4, 5}},
      Case{12, 2, {4, 5, 6, 7, 10, 11, 4, 5}},
      Case{36, 3, {0, 1, 2, 3, 4, 5, 6, 7}},
  };
  for (const auto &entry : cases) {
    const auto decoded =
        decodeInline(gaugeFixture(entry.nodeCount, entry.animationType));
    expect(decoded.model.has_value(),
           "each pinned gauge cardinality decodes through the live path");
    if (!decoded.model) {
      continue;
    }
    const auto *definition = objectNamed(*decoded.model, "gauge");
    expect(definition != nullptr, "live Gauge destination materializes");
    if (!definition) {
      continue;
    }
    const auto *gauge = std::get_if<SkinGaugeObject>(&definition->payload);
    expect(gauge && gauge->orderedNodes.size() == 36,
           "live Gauge expands every supported cardinality to 36 roles");
    if (!gauge || gauge->orderedNodes.size() != 36) {
      continue;
    }
    for (std::size_t role = 0; role < entry.firstSources.size(); ++role) {
      expect(gauge->orderedNodes[role].frames.size() == 2 &&
                 gauge->orderedNodes[role].frames.front().x ==
                     entry.firstSources[role] * 20 &&
                 gauge->orderedNodes[role].cycleMillis == 0 &&
                 !gauge->orderedNodes[role].timer,
             "live Gauge uses the pinned role mapping and strips node timing");
    }
    expect(gauge->parts == 40 + entry.animationType &&
               static_cast<int>(gauge->animation) == entry.animationType &&
               gauge->animationRange == 10 + entry.animationType &&
               gauge->animationCycleMillis == 20 + entry.animationType &&
               gauge->resultStartMillis == -10 - entry.animationType &&
               gauge->resultEndMillis == 700 + entry.animationType,
           "live Gauge preserves parts, type, range, cycle, and result interval");
  }
}

std::string gaugeBudgetFixture(int fillerFrames) {
  std::ostringstream lua;
  lua << "return {type=0,w=1280,h=720,"
         "source={{id='atlas',path='atlas.png'}},"
         "image={{id='filler',src='atlas',w="
      << fillerFrames << ",h=1,divx=" << fillerFrames
      << ",divy=1},{id='node',src='atlas',w=5555,h=1,divx=5555,divy=1}},"
         "gauge={id='gauge',nodes={'node','node','node','node'}},"
         "destination={{id='filler',dst={{}}},{id='gauge',dst={{}}}}}";
  return lua.str();
}

void testGaugeExpansionSharesTheCumulativeFrameBudget() {
  const auto boundary = decodeInline(gaugeBudgetFixture(20));
  expect(boundary.model.has_value(),
         "ordinary and expanded Gauge frames may exactly consume 200k frames");

  const auto exceeded = decodeInline(gaugeBudgetFixture(21));
  expect(!exceeded.model && !exceeded.diagnostics.empty() &&
             exceeded.diagnostics.front().code ==
                 "skin_lua_model_limit_exceeded",
         "expanded Gauge frames participate in the cumulative 200k model budget");
}

} // namespace

int main() {
  testLiveGaugeUsesEveryPinnedNodeLayout();
  testGaugeExpansionSharesTheCumulativeFrameBudget();
  if (failures != 0) {
    std::cerr << failures << " assertion(s) failed\n";
    return 1;
  }
  std::cout << "lua skin gauge live integration tests passed\n";
  return 0;
}
