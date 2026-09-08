#include "skin/beatoraja/GameplaySkinDocumentLoader.h"
#include "skin/beatoraja/LuaSkinRuntime.h"
#include "skin/package/SkinAliasDetector.h"
#include "skin/package/SkinPathPolicy.h"
#include "skin/package/SkinTreeSnapshotter.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <variant>

namespace {

namespace fs = std::filesystem;
using namespace skin;

bool expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

void printDiagnostics(const std::vector<SkinDiagnostic> &diagnostics) {
  for (const auto &diagnostic : diagnostics) {
    std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
  }
}

bool budgetFailure(const std::optional<SkinDiagnostic> &failure) {
  return failure &&
         (failure->code == "skin_lua_instruction_limit_exceeded" ||
          failure->code == "skin_lua_wall_time_limit_exceeded");
}

int run(const fs::path &source, std::string_view phase, bool budget) {
  const fs::path root = source.parent_path().parent_path();
  const SkinStorageRoots roots{.visiblePackages = root / "visible",
                               .privateRevisions = root / "revisions",
                               .privateCatalog = root / "catalog",
                               .profileOverlays = root / "overlays"};
  const auto package = normalizePackageId("CoroutineContract").package;
  const auto entry = normalizeEntryPath(*package, "skin/probe.luaskin").entry;
  const fs::path packageSource = root / "package";
  fs::create_directories(packageSource / "skin");
  fs::copy_file(source, packageSource / entry->packageRelativePath);
  fs::create_directories(roots.visiblePackages);
  fs::copy(packageSource, roots.visiblePackages / package->directoryName,
           fs::copy_options::recursive);
  auto aliases = createPlatformSkinAliasDetector();
  SkinTreeSnapshotter snapshotter(roots, *aliases);
  auto snapshot = snapshotter.snapshot(packageSource, *package, {}, {});
  if (!expect(snapshot.prepared.has_value(), "generated package snapshots")) {
    return 1;
  }
  const auto makeFiles = [&] {
    return LuaSkinFileSystem::create({.revision = snapshot.prepared->readView(),
                                     .entry = *entry,
                                     .storageRoots = roots});
  };
  auto documentFiles = makeFiles();
  auto luaFiles = makeFiles();
  if (!expect(documentFiles.fileSystem && luaFiles.fileSystem,
              "generated document filesystems create")) {
    return 1;
  }
  std::cout << "ENTER: GameplaySkinDocumentLoader::inspect " << phase << std::endl;
  const auto inspected = GameplaySkinDocumentLoader{}.inspect(
      {.entry = *entry,
       .documentFileSystem = *documentFiles.fileSystem,
       .luaFileSystem = std::move(luaFiles.fileSystem),
       .luaPurpose = LuaRuntimePurpose::Catalog});
  printDiagnostics(inspected.diagnostics);
  if (budget && phase == "header") {
    for (const auto &diagnostic : inspected.diagnostics) {
      if (budgetFailure(diagnostic) && !inspected.header) {
        std::cout << "PASS: RENDER-01 header budget rejection\n";
        return 0;
      }
    }
    return expect(false, "header coroutine must consume the VM budget") ? 0 : 1;
  }
  if (!expect(inspected.header && inspected.header->name == "RENDER-01 passed" &&
                  inspected.configuration && !inspected.cancelled,
              "real loader returns the asserted header and configuration")) {
    return 1;
  }
  if (phase == "header") {
    std::cout << "PASS: RENDER-01 asserted header values\n";
    return 0;
  }
  auto runtimeFiles = makeFiles();
  auto created = LuaSkinRuntime::create(
      {.purpose = LuaRuntimePurpose::Gameplay,
       .fileSystem = std::move(runtimeFiles.fileSystem)});
  if (!expect(created.runtime != nullptr, "gameplay runtime creates")) return 1;
  auto header = created.runtime->loadHeader();
  if (!expect(header.value.has_value(), "live fixture header loads")) return 1;
  auto configured = created.runtime->loadConfigured(*inspected.configuration);
  if (!expect(configured.value.has_value(), "live fixture configures")) return 1;
  const auto callback = configured.value->callbackNamed("run_callback");
  if (!expect(callback && created.runtime->enterRenderPhase().ok &&
                  created.runtime->beginFrame(1).ok,
              "configured callback is registered and enters a live frame")) {
    return 1;
  }
  std::cout << "ENTER: LuaSkinRuntime::invoke" << std::endl;
  const auto invoked = created.runtime->invoke(*callback, {});
  if (invoked.failure) {
    std::cerr << invoked.failure->code << ": " << invoked.failure->message << '\n';
  }
  const auto *value = invoked.value ? std::get_if<std::string>(&*invoked.value)
                                    : nullptr;
  const bool passed = budget ? (!invoked.value && budgetFailure(invoked.failure))
                             : (!invoked.failure && value &&
                                *value == "RENDER-01 passed");
  if (!expect(passed, "live callback returns asserted values or typed budget rejection")) {
    return 1;
  }
  std::cout << "PASS: RENDER-01 live callback " << (budget ? "budget" : "values") << '\n';
  return 0;
}

}

int main(int argc, char **argv) {
  if (argc != 4 || (std::string_view(argv[2]) != "header" &&
                    std::string_view(argv[2]) != "callback") ||
      (std::string_view(argv[3]) != "success" &&
       std::string_view(argv[3]) != "budget")) {
    std::cerr << "usage: lua_skin_coroutine_tests GENERATED_ENTRY header|callback success|budget\n";
    return 2;
  }
  return run(argv[1], argv[2], std::string_view(argv[3]) == "budget");
}
