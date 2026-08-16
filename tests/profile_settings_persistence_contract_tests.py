import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class ProfileSettingsPersistenceContractTests(unittest.TestCase):
    def test_ir_scene_uses_context_full_candidate_adapter(self):
        source = (ROOT / "src/scene/SettingsSceneIr.cpp").read_text(encoding="utf-8")
        self.assertIn("saveSettingsCandidate", source)
        self.assertNotIn("AppSettingsStore::Save", source)

    def test_lua_feature_off_core_has_no_lua_include_dependency(self):
        for relative in (
            "src/skin/SkinProfileSettings.h",
            "src/skin/SkinProfileSettings.cpp",
            "src/ProfileSettingsPersistenceCoordinator.h",
            "src/ProfileSettingsPersistenceCoordinator.cpp",
        ):
            source = (ROOT / relative).read_text(encoding="utf-8")
            self.assertNotIn("lua.hpp", source)
            self.assertNotIn("sol/sol.hpp", source)


if __name__ == "__main__":
    unittest.main()
