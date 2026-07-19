from pathlib import Path
import sys

root = Path(sys.argv[1])
header = (root / "src/scene/MainMenuScene.h").read_text()
source = (root / "src/scene/MainMenuScene.cpp").read_text()
required = [
    "replayModalResultButton",
    "replayResultRecallInProgress",
    "startReplayResultRecall",
    'makeModalButton("View Result"',
    "LoadReplayResult",
    "BuildChartResult",
    "BuildCourseResult",
    ".savedResultBrowsing = true",
]
combined = header + source
missing = [token for token in required if token not in combined]
forbidden = [
    "replayModalPhotoButton",
    "startReplayImageExport",
    'makeModalButton("Export Photo"',
]
present = [token for token in forbidden if token in combined]
if missing or present:
    raise SystemExit("records result recall contract failure; missing=" +
                     repr(missing) + " forbidden=" + repr(present))
