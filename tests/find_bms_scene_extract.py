"""Compile shared Find BMS modal methods and Main Menu callback delivery."""
import argparse
from pathlib import Path

from gameplay_terminal_scene_extract import extract


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/FindBmsModal.cpp").read_text()
    signatures = [
        "bool messageStartsWith(",
        "double progressRatio(",
        "std::string\nfindBmsProgressEventDisplayText(",
        "bool shouldReplaceFindBmsLogLine(",
        "double findBmsProgressFractionFor(",
        "bool FindBmsModal::inProgress() const",
        "bool FindBmsModal::updateQueuedDownload(",
        "void FindBmsModal::show(",
        "void FindBmsModal::startLookup()",
        "void FindBmsModal::cancelAndWait()",
        "void FindBmsModal::startPendingArtifactResolution(",
        "void FindBmsModal::hide()",
        "void FindBmsModal::update()",
        "void FindBmsModal::cancelOrClose()",
    ]
    methods = "\n\n".join(extract(source, item) for item in signatures)
    detail = source[source.index("  i18n::Text detail;", source.index("void FindBmsModal::refresh(")):]
    detail = detail[:detail.index("  const auto displayedOptions =")]
    methods += ("\ni18n::Text MainMenuScene::detailMessage() const {\n"
                "  const bool running = inProgress();\n"
                "  const auto retryRequest = findBmsTask.retryRequest();\n" +
                detail + "  return detail;\n}\n")
    # Adapt only names to the existing lightweight owner fixture; method bodies
    # and the scene's real callback wiring remain complete production code.
    names = {
        "FindBmsModal::": "MainMenuScene::",
        "show(": "showFindBmsModal(",
        "startPendingArtifactResolution(": "startFindBmsPendingArtifactResolution(",
        "hide()": "hideFindBmsModal()",
        "update()": "applyFindBmsUpdates()",
        "refresh()": "refreshFindBmsModal()",
        "cancelOrClose()": "cancelFindBms()",
    }
    for old, new in names.items():
        methods = methods.replace(old, new)
    owner = (args.root / "src/scene/MainMenuScene.cpp").read_text()
    callbacks = extract(owner, "findBmsModal_ = FindBmsModal::Create(")
    callbacks = callbacks[callbacks.index("{"):]
    methods += "\nvoid MainMenuScene::buildFindBmsCallbacks() { callbacks_ = " + callbacks + "; }\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(methods + "\n")


if __name__ == "__main__":
    main()
