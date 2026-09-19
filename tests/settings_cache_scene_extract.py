"""Compile unchanged Settings cache UI methods against a small view fixture."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/SettingsScene.cpp").read_text()
    signatures = [
        "std::string formatCacheBytes(",
        "std::string formatCacheCleanupResult(",
        "std::string\nformatCacheUsageResult(",
        "void SettingsScene::applyPendingArchiveCacheCleanupStatus()",
        "void SettingsScene::cleanupTemporaryArchiveCache()",
        "void SettingsScene::measureTemporaryArchiveCache()",
    ]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n\n".join(extract(source, item) for item in signatures) + "\n")


if __name__ == "__main__":
    main()
