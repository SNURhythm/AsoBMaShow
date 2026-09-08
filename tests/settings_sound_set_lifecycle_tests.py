import re
import subprocess
import unittest

from tests import music_select_error_flow_contract_tests as fixture_tools


ROOT = fixture_tools.ROOT


class SettingsSoundSetLifecycleTests(unittest.TestCase):
    def test_late_picker_publication_after_real_layout_reset(self):
        layout = (ROOT / "src/scene/SettingsSceneLayout.cpp").read_text()
        skins = (ROOT / "src/scene/SettingsSceneSkins.cpp").read_text()
        reset = fixture_tools.function_body(layout, "void SettingsScene::resetViewState()")
        pointers = sorted(set(re.findall(r"^  (\w+) = nullptr;", reset, re.MULTILINE)) -
                          {"skinSelectSoundSetInput"})
        methods = "\n".join(signature + fixture_tools.function_body(source, signature)
                            for source, signature in (
                                (layout, "void SettingsScene::resetViewState()"),
                                (layout, "void SettingsScene::ensureLayoutUpToDate()"),
                                (skins, "void SettingsScene::applyPendingSoundSetFolderPick()")))
        fixture = (ROOT / "tests/settings_sound_set_lifecycle_fixture.cpp").read_text()
        fixture = (fixture.replace("VIEW_FIELDS", "\n".join(
            f"View *{name} = nullptr;" for name in pointers))
                   .replace("SCENE_METHODS", methods))
        try:
            fixture_tools.MusicSelectSceneBehaviorTests().compile_and_run(fixture)
        except subprocess.CalledProcessError as error:
            self.fail(error.stderr)


if __name__ == "__main__":
    unittest.main()
