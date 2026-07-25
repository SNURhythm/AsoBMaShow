from pathlib import Path
import sys

root = Path(sys.argv[1])
header = (root / "src/scene/MainMenuScene.h").read_text()
source = (root / "src/scene/MainMenuScene.cpp").read_text()
record_state_source = (root / "src/ir/IrReplayRecordState.cpp").read_text()
required = [
    "replayModalResultButton",
    "replayResultRecallInProgress",
    "startReplayResultRecall",
    'makeModalButton("View Result"',
    "loadChartResult",
    "loadCourseResult",
    "BuildChartResult",
    "BuildCourseResult",
    ".savedResultBrowsing = true",
    "onIrUploadRequested",
    "startReplayIrUpload",
    "finishReplayIrUpload",
    "refreshReplayIrMarker",
    "replayIrUploadInProgress",
    "executeIrSavedResultUpload",
    "observeReplayIrServiceRevisions",
    "replayIrObservedRevisions",
    "status.revision",
]
combined = header + source
missing = [token for token in required if token not in combined]
missing += ["record-state:" + token for token in [
    "void resolveReplayIrRecordState",
    "tachi::isReplayEligibleForBokutachi(",
    "summary.irRecordState = resolveIrRecordState({",
    ".eligible = summary.irSubmissionEligible",
    ".hasReceipt = summary.hasIrReceipt",
    ".outboxState = summary.requestedIrOutboxState",
    ".activity = activity",
] if token not in record_state_source]
missing += ["marker:" + token for token in [
    "ir::resolveReplayIrRecordState(*latest, activity);",
] if token not in source]
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
        "loadIrSubmissionSnapshot",
        "independently stored IR snapshot",
        "loadOutbox",
        "buildDraft",
        "enqueueManual",
        "retry",
    ] if token not in upload_source]
    forbidden_upload = [
        "LoadReplayResult",
        "loadChartReplayPlayback",
        "BuildChartResult",
        "historicalIr",
    ]
    missing += ["upload:forbidden:" + token for token in forbidden_upload
                if token in upload_source]
else:
    missing.append("upload:startReplayIrUpload definition")

cleanup_tokens = [
    "replayIrUploadInProgress = false",
    "replayIrObservedRevisions.clear()",
]
missing += ["cleanup:" + token for token in cleanup_tokens
            if token not in combined]
forbidden = [
    "replayModalPhotoButton",
    "startReplayImageExport",
    'makeModalButton("Export Photo"',
    "setIrUploadInProgress",
    "replayIrUploadReplayId",
]
present = [token for token in forbidden if token in combined]
if missing or present:
    raise SystemExit("records result recall contract failure; missing=" +
                     repr(missing) + " forbidden=" + repr(present))
