from pathlib import Path
import sys

root = Path(sys.argv[1])
header = (root / "src/scene/ResultScene.h").read_text()
source = (root / "src/scene/ResultScene.cpp").read_text()
required_header = ["bool savedResultBrowsing = false;"]
required_source = [
    "courseOptions.savedResultBrowsing",
    "showSavedCourseStage",
    "session->completedResults[session->currentIndex]",
    ".savedResultBrowsing = true",
]
missing = [token for token in required_header if token not in header]
missing += [token for token in required_source if token not in source]
if missing:
    raise SystemExit("missing saved course result contracts: " +
                     ", ".join(missing))
