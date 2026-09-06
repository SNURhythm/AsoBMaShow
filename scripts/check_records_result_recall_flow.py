from pathlib import Path
import sys

root = Path(sys.argv[1])
header = (root / "src/scene/MainMenuScene.h").read_text()
source = (root / "src/scene/MainMenuScene.cpp").read_text()
modal_source = (root / "src/scene/ReplayRecordsModal.cpp").read_text()
modal_header = (root / "src/scene/ReplayRecordsModal.h").read_text()
combined = header + source + modal_header + modal_source

required = [
    "Button *resultButton_ = nullptr;",
    "modal->resultButton_ = resultButton;",
    "replayResultRecallInProgress",
    'makeModalButton("View Result"',
    "resultRecordActionTarget(",
    "startModernReplayResultRecall",
    "startModernCourseReplayResultRecall",
    "startRemoteResultRecall",
    "LoadModernChartResultByAttempt",
    "LoadModernCourseResultByAttempt",
    "result_recall::BuildChartResult",
    "result_recall::BuildCourseResult",
    ".savedResultBrowsing = true",
    "onIrUploadRequested",
    "startModernReplayIrUpload",
    "finishReplayIrUpload",
    "replayIrUploadInProgress",
    "LoadModernIrSubmissionSnapshot",
    "executeIrSavedResultUpload",
    "observeReplayIrServiceRevisions",
    "replayIrObservedRevisions",
    "status.revision",
]
missing = [token for token in required if token not in combined]

chart_recall_start = source.index(
    "void MainMenuScene::startModernReplayResultRecall")
chart_recall_end = source.index(
    "\nvoid MainMenuScene::startModernCourseReplayResultRecall",
    chart_recall_start)
chart_recall = source[chart_recall_start:chart_recall_end]
missing += ["chart-recall:" + token for token in [
    "pendingStopAndClearSelectedChartAfterPreview = true;",
    "startReplayLoadWorker(",
    "queueReplayLoadCompletion(",
    "previewWorker_->stop();",
    "LoadModernChartResultByAttempt(",
    "result_recall::BuildChartResult(",
] if token not in chart_recall]

course_recall_start = chart_recall_end + 1
course_recall_end = source.index(
    "\nvoid MainMenuScene::startRemoteResultRecall", course_recall_start)
course_recall = source[course_recall_start:course_recall_end]
missing += ["course-recall:" + token for token in [
    "previewWorker_->cancel();",
    "pendingStopAndClearSelectedChartAfterPreview = true;",
    "startReplayLoadWorker(",
    "queueReplayLoadCompletion(",
    "previewWorker_->stop();",
    "LoadModernCourseResultByAttempt(",
    "result_recall::BuildCourseResult(",
    ".savedResultBrowsing = true",
] if token not in course_recall]

upload_start = source.index("void MainMenuScene::startModernReplayIrUpload")
upload_end = source.index("\nvoid MainMenuScene::finishReplayIrUpload",
                          upload_start)
upload_source = source[upload_start:upload_end]
missing += ["upload:" + token for token in [
    "LoadModernIrSubmissionSnapshot",
    "snapshot.snapshot->submission",
    "loadOutbox",
    "buildDraft",
    "enqueueManual",
    "retry",
] if token not in upload_source]

cleanup_tokens = [
    "replayIrUploadInProgress = false",
    "replayIrObservedRevisions.clear()",
]
missing += ["cleanup:" + token for token in cleanup_tokens
            if token not in combined]

if missing:
    raise SystemExit("records result recall contract failure; missing=" +
                     repr(missing))
