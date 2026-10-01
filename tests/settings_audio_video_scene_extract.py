"""Run production status methods and Test Sound callback with lightweight views."""
import argparse
import re
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/SettingsSceneAudioVideo.cpp").read_text()
    header = (args.root / "src/scene/SettingsScene.h").read_text()
    fields = [re.search(r"^  [^\n]+ " + name + r";", header, re.M).group(0)
              for name in ["audioStatusMessage", "displayStatusMessage"]]
    declarations = [re.search(r"  void " + name + r"\([^;]+;", header).group(0)
                    for name in ["setAudioStatus", "setDisplayStatus"]]
    helpers = []
    for name in ["audioApplyMessage", "displayApplyMessage"]:
        signature = re.search(r"^\S+ " + name + r"\(", source, re.M).group(0)
        helpers.append(extract(source, signature))
    methods = [extract(source, "void SettingsScene::" + name + "(")
               for name in ["setAudioStatus", "setDisplayStatus"]]
    callback = extract(source, "testSoundButton->setOnClickListener([this]()") + ");"
    fixture = (args.root / "tests/settings_audio_video_scene_fixture.cpp").read_text()
    fixture = fixture.replace("// STATUS_FIELDS", "\n".join(fields + declarations))
    fixture = fixture.replace("// STATUS_METHODS", "\n\n".join(helpers + methods))
    fixture = fixture.replace("// TEST_SOUND_CALLBACK", callback)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
