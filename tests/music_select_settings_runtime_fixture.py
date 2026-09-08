import argparse
from pathlib import Path

from tests.music_select_error_flow_contract_tests import ROOT, function_body


def generate():
    source = (ROOT / "src/scene/MusicSelectScene.cpp").read_text()
    header = (ROOT / "src/scene/MusicSelectScene.h").read_text()
    methods = []
    for signature in (
        "bool MusicSelectScene::activateSkin(",
        "bool MusicSelectScene::reactivateSkinAfterSettings()",
        "void MusicSelectScene::cancelSkinPreparation()",
        "void MusicSelectScene::openSettings()",
        "void MusicSelectScene::onResume()",
    ):
        start = source.index(signature)
        methods.append(source[start:source.index("{", start)] + function_body(source, signature))
    identity = ""
    if "struct SkinActivationIdentity" in header:
        start = header.index("struct SkinActivationIdentity")
        end = header.index("activeSkinIdentity_;", start) + len("activeSkinIdentity_;")
        identity = header[start:end]
    fixture = (ROOT / "tests/music_select_settings_runtime_fixture.cpp").read_text()
    init = function_body(source, "void MusicSelectScene::init()")
    activation = next(line for line in init.splitlines() if "activateSkin(" in line)
    return (fixture.replace("SCENE_METHODS", "\n".join(methods))
            .replace("ACTIVATION_IDENTITY", identity)
            .replace("INITIAL_ACTIVATION", activation))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    arguments.output.write_text(generate())
