import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FILESYSTEM = ROOT / "src/skin/beatoraja/LuaSkinFileSystem.cpp"
CMAKE = ROOT / "CMakeLists.txt"


class WindowsLuaSkinFileSystemContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = FILESYSTEM.read_text(encoding="utf-8")
        cls.cmake = CMAKE.read_text(encoding="utf-8")

    def test_runtime_keeps_beatoraja_selected_directory_boundary(self):
        for token in (
            "normalizeAtSkinDirectory",
            "isWithinDirectory(resolved, root)",
            "beatorajaSkinRoot",
            'constexpr std::string_view beatorajaRootPrefix = "skin/"',
        ):
            self.assertIn(token, self.source)

    def test_runtime_uses_ordinary_live_file_io(self):
        for token in (
            "std::ifstream input(path, std::ios::binary)",
            "openDirectRegularFile",
            "openVerifiedRegularFile",
            "fs::directory_iterator",
            "fs::create_directories(target.parent_path(), error)",
        ):
            self.assertIn(token, self.source)
        self.assertNotIn("validatePrivateOverlayRoot", self.source)
        self.assertNotIn("secureReplaceOverlayFile", self.source)

    def test_render_transition_does_not_invent_a_file_io_freeze(self):
        transition = self.source[
            self.source.index("LuaSkinFileSystem::enterRenderPhase") :
            self.source.index("SkinFileActivityCounters", self.source.index("LuaSkinFileSystem::enterRenderPhase"))
        ]
        self.assertIn("Beatoraja does not freeze Lua file access", transition)
        self.assertIn("return {.ok = true};", transition)

    def test_windows_test_target_links_the_security_api(self):
        target = self.cmake[
            self.cmake.index("add_executable(lua_skin_file_system_tests") :
            self.cmake.index(
                "function(asobmashow_register_test",
                self.cmake.index("add_executable(lua_skin_file_system_tests"),
            )
        ]
        self.assertIn("advapi32", target)


if __name__ == "__main__":
    unittest.main()
