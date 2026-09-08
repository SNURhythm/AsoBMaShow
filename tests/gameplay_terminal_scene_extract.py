import argparse
from pathlib import Path


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError(f"Unterminated production method: {signature}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
    signatures = [
        "void GamePlayScene::update(float dt)",
        "void GamePlayScene::completePracticeSection(",
        "void GamePlayScene::finalizePracticeRangeMisses()",
        "void GamePlayScene::completePracticeAttempt()",
        "void GamePlayScene::finishReplayRecording()",
    ]
    fixture = (args.root / "tests/gameplay_terminal_scene_fixture.cpp").read_text()
    methods = "\n\n".join(extract(source, signature) for signature in signatures)
    args.output.write_text(fixture.replace("SCENE_METHODS", methods))


if __name__ == "__main__":
    main()
