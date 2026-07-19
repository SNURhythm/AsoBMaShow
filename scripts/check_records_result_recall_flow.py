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
    "onIrUploadRequested",
    "startReplayIrUpload",
    "finishReplayIrUpload",
    "refreshReplayIrMarker",
    "replayIrUploadInProgress",
    "executeIrSavedResultUpload",
    "setIrUploadInProgress",
]
combined = header + source
missing = [token for token in required if token not in combined]
recall_start = source.index("void MainMenuScene::startReplayResultRecall")
recall_end = source.index("void MainMenuScene::startCourseReplayResultRecall",
                          recall_start)
recall_source = source[recall_start:recall_end]
missing += ["recall:" + token for token in [
    "cancelActivePreviewLoading();",
    "loadThread.join()",
    "joinRetiredPreviewLoadThreads()",
] if token not in recall_source]
if "void MainMenuScene::startReplayIrUpload" in source:
    upload_start = source.index("void MainMenuScene::startReplayIrUpload")
    upload_end = source.index("void MainMenuScene::finishReplayIrUpload",
                              upload_start)
    upload_source = source[upload_start:upload_end]
    missing += ["upload:" + token for token in [
        "LoadReplayResult",
        "BuildChartResult",
        "historicalIr",
        "loadOutbox",
        "buildDraft",
        "enqueueManual",
        "retry",
    ] if token not in upload_source]
else:
    missing.append("upload:startReplayIrUpload definition")

cleanup_tokens = [
    "replayIrUploadInProgress = false",
    "replayIrUploadReplayId.reset()",
]
missing += ["cleanup:" + token for token in cleanup_tokens
            if token not in combined]
forbidden = [
    "replayModalPhotoButton",
    "startReplayImageExport",
    'makeModalButton("Export Photo"',
]
present = [token for token in forbidden if token in combined]
if missing or present:
    raise SystemExit("records result recall contract failure; missing=" +
                     repr(missing) + " forbidden=" + repr(present))
