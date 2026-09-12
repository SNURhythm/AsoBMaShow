import argparse
from pathlib import Path


def generate(root, output):
    source = (root / "src/scene/ResultScene.cpp").read_text()
    start = source.index("SkinGameplayGraphState\ncourseGraphPaddingForEntry(")
    end = source.index("long long totalPlayLengthForCourse(", start)
    padding_start = source.index("void appendMissingCourseGaugeHistory(")
    padding_end = source.index("RhythmState courseResultStateForSession(", padding_start)
    fixture = (root / "tests/graph_result_fixture.cpp").read_text()
    if fixture.count("ASOBMS_GRAPH_RESULT_METHODS") != 1:
        raise ValueError("Result graph fixture anchor changed")
    output.write_text(fixture.replace(
        "ASOBMS_GRAPH_RESULT_METHODS",
        source[start:end] + source[padding_start:padding_end],
    ))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    generate(args.root, args.output)
