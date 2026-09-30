import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
CMAKE = ROOT / "CMakeLists.txt"


class WindowsLuaSkinFileSystemContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = CMAKE.read_text(encoding="utf-8")

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
