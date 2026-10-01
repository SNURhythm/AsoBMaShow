"""Exercise retained IR statuses using production callbacks and result publisher."""
import argparse
import re
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/SettingsSceneIr.cpp").read_text()
    header = (args.root / "src/scene/SettingsScene.h").read_text()
    fixture = (args.root / "tests/settings_ir_scene_fixture.cpp").read_text()
    field = re.search(r"^  [^\n]+ irStatusMessage;", header, re.M).group(0)
    fixture = fixture.replace("// STATUS_FIELD", field)
    fixture = fixture.replace("// PUBLISH_RESULT", extract(source, "auto publishResult =") + ";")
    for name in ["cancelKey", "replaceKey", "discard"]:
        callback = extract(source, name + "->setOnClickListener(") + ");"
        fixture = fixture.replace("// CALLBACK_" + name, callback)
    # Evaluate the actual success argument at each production publish call.
    # The action result is supplied independently to cover success/failure paths.
    arguments = re.findall(r'publishResult\([\s\S]*?,\s*(i18n::(?:tr|message)\("([^"]+)"\))\);', source)
    fixture = fixture.replace("// SUCCESS_CASES", "\n".join(
        f'    publishResult(result, {expression});\n    verifyRetained("{key}");'
        for expression, key in arguments))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
