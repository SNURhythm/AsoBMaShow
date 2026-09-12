import argparse
from pathlib import Path


def generate(root, output):
    source = (root / "src/scene/MusicSelectScene.cpp").read_text()
    start = source.index("void MusicSelectScene::cancelSelectedChartAnalysis()")
    end = source.index("TextInputBox *MusicSelectScene::skinTextInputForSize", start)
    methods = source[start:end]
    if methods.count("std::thread(") != 1:
        raise ValueError("Selector worker thread boundary changed")
    if methods.count("buildPlayfieldChartVisualModel(*chart, longNoteMode)") != 1:
        raise ValueError("Selector model boundary changed")
    methods = methods.replace("std::thread(", "GraphTestThread(")
    methods = methods.replace(
        "buildPlayfieldChartVisualModel(*chart, longNoteMode)",
        "buildSelectorFixtureModel(*chart, longNoteMode)",
    )
    fixture = (root / "tests/graph_selector_fixture.cpp").read_text()
    if fixture.count("ASOBMS_GRAPH_SELECTOR_METHODS") != 1:
        raise ValueError("Selector fixture anchor changed")
    output.write_text(fixture.replace("ASOBMS_GRAPH_SELECTOR_METHODS", methods))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.root, args.output)
