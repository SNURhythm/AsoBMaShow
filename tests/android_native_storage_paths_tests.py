"""Run the native Android storage boundary through the skin store's no-follow check."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest

from gameplay_terminal_scene_extract import extract
from support.fixture_compiler import FixtureCompiler

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == "posix", "Android's no-follow storage boundary is POSIX")
class AndroidNativeStoragePathsTests(unittest.TestCase):
    def test_external_container_alias_preserves_document_symlink_rejection(self):
        native = (ROOT / "src/AndroidNatives.cpp").read_text()
        utils = (ROOT / "src/Utils.cpp").read_text()
        skin = (ROOT / "src/skin/SkinStoragePaths.cpp").read_text()
        store = (ROOT / "src/skin/package/SkinPackageStore.cpp").read_text()
        methods = extract(native, "std::string GetAndroidExternalFilesDir()")
        methods += "\n" + extract(utils, "std::filesystem::path\nUtils::GetDocumentsPath(")
        methods += "\n" + extract(skin, "SkinStorageRoots deriveSkinStorageRoots(")
        methods += "\n" + extract(skin, "SkinStorageRoots defaultSkinStorageRoots()")
        methods += "\n" + extract(store, "class UniqueDirectoryDescriptor") + ";"
        # The last definition is the POSIX branch used by Android (not Windows or iOS).
        no_follow = store[store.rindex("bool ensureDirectoryNoFollow("):]
        methods += "\n" + extract(no_follow, "bool ensureDirectoryNoFollow(")
        source = r'''
#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include "skin/package/SkinDirectoryTraversal.h"
#define TARGET_OS_ANDROID 1
#define TARGET_OS_IOS 0
#define TARGET_OS_SIMULATOR 0
namespace fs = std::filesystem;
using skin::skinAncestorDirectoryOpenFlag;
std::string externalPath, internalPath;
const char *SDL_GetAndroidExternalStoragePath() { return externalPath.c_str(); }
const char *SDL_GetAndroidInternalStoragePath() { return internalPath.c_str(); }
std::string GetAndroidInternalFilesDir() { return internalPath; }
struct Utils { static fs::path GetDocumentsPath(const fs::path &sub = {}); };
struct SkinStorageRoots {
  fs::path visiblePackages, privateRevisions, privateCatalog, profileOverlays;
  bool liveSources = false;
};
// PRODUCTION_METHODS
void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}
int main(int argc, char **argv) {
  const fs::path base = fs::canonical(argv[1]);
  fs::create_directories(base / "real/files");
  fs::create_directory_symlink(base / "real", base / "alias");
  externalPath = (base / "alias/files").string();
  internalPath = (base / "real/files").string(); // SDL canonicalizes its internal getter.
  const auto roots = defaultSkinStorageRoots();
  require(roots.visiblePackages == base / "real/files/Documents/Skins",
          "visible skin root retains the external container alias");
  require(ensureDirectoryNoFollow(roots.visiblePackages), "visible skin recovery rejected trusted alias");
  require(ensureDirectoryNoFollow(roots.privateCatalog), "private skin recovery failed");
  fs::create_directory(base / "outside");
  fs::remove_all(base / "real/files/Documents");
  fs::create_directory_symlink(base / "outside", base / "real/files/Documents");
  require(!ensureDirectoryNoFollow(defaultSkinStorageRoots().visiblePackages),
          "canonicalization must not follow user-controlled Documents symlinks");
  fs::remove(base / "real/files/Documents");
  fs::create_directory(base / "real/files/Documents");
  fs::create_directory_symlink(base / "outside", base / "real/files/Documents/Skins");
  require(!ensureDirectoryNoFollow(defaultSkinStorageRoots().visiblePackages),
          "canonicalization must not follow user-controlled Skins symlinks");
  require(fs::is_empty(base / "outside"), "rejected symlinks changed outside storage");
  externalPath.clear();
  require(GetAndroidExternalFilesDir() == internalPath, "internal fallback changed");
  internalPath.clear();
  require(GetAndroidExternalFilesDir() == ".", "unavailable-storage fallback changed");
  externalPath = (base / "missing/files").string();
  require(GetAndroidExternalFilesDir() == externalPath, "canonicalization failure changed path selection");
}
'''.replace("// PRODUCTION_METHODS", methods)
        with tempfile.TemporaryDirectory(prefix="android-native-storage-") as temporary:
            output = Path(temporary)
            fixture = output / "storage.cpp"
            fixture.write_text(source)
            compiler = FixtureCompiler.from_environment()
            binary = output / ("storage" + compiler.executable_suffix)
            compiler.build([fixture], binary, output, includes=[ROOT / "src"])
            subprocess.run([str(binary), str(output)], check=True)


if __name__ == "__main__":
    unittest.main()
