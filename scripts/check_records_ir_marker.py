from pathlib import Path
import sys

root = Path(sys.argv[1])
repository = (root / "src/repositories/ReplayRepositoryRecords.cpp").read_text()
menu = (root / "src/scene/MainMenuScene.cpp").read_text()
view = (root / "src/view/ReplaySummaryListView.h").read_text()
required = {
    "repository": ["requestedIrOutboxState", "irProviderId", "ir_outbox"],
    "menu": ["shouldShowReplayUploadMarker", "irUploadPending"],
    "view": ["IR ↑", "irUploadPending", "ui_theme::amber"],
}
texts = {"repository": repository, "menu": menu, "view": view}
missing = []
for group, tokens in required.items():
    missing.extend(group + ":" + token
                   for token in tokens if token not in texts[group])
if missing:
    raise SystemExit("missing Records IR marker contracts: " +
                     ", ".join(missing))
