"""Exercise the production display/resize callbacks across Android startup rotation."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/main.cpp").read_text()
    fixture = (args.root / "tests/display_orientation_sync_fixture.cpp").read_text()
    # Extract the complete bodies; platform graphics calls are observed by the
    # fixture, while orientation policy uses the production state machine.
    transaction = extract(source, "std::string &syncError) {")
    fixture = fixture.replace("PRODUCTION_DISPLAY_SYNC", transaction[transaction.index("{"):])
    fixture = fixture.replace("PRODUCTION_RESIZE", extract(
        source, "auto applyWindowResize = [&](int logicalW, int logicalH)"))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(fixture)


if __name__ == "__main__":
    main()
