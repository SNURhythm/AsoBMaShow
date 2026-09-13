from pathlib import Path
import sys

root = Path(sys.argv[1])
header = (root / "src/scene/MainMenuScene.h").read_text()
source = (root / "src/scene/MainMenuScene.cpp").read_text()
modal_source = (root / "src/scene/ReplayRecordsModal.cpp").read_text()
modal_header = (root / "src/scene/ReplayRecordsModal.h").read_text()
chart_actions = (root / "src/scene/ChartRecordActions.cpp").read_text()
course_actions = (root / "src/scene/CourseRecordActions.cpp").read_text()
ir_actions = (root / "src/scene/RecordsIrActions.cpp").read_text()
selector = (root / "src/scene/MusicSelectSceneRecords.cpp").read_text()
combined = header + source + modal_header + modal_source + chart_actions + course_actions + ir_actions

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
    "startReplayLoadWorker(",
    "queueReplayLoadCompletion(",
    "previewWorker_->stop();",
    "chart_records::prepareChartResult(",
] if token not in chart_recall]

course_recall_start = chart_recall_end + 1
course_recall_end = source.index(
    "\nvoid MainMenuScene::startRemoteResultRecall", course_recall_start)
course_recall = source[course_recall_start:course_recall_end]
missing += ["course-recall:" + token for token in [
    "startReplayLoadWorker(",
    "queueReplayLoadCompletion(",
    "previewWorker_->stop();",
    "course_records::prepareCourseResult(",
    ".savedResultBrowsing = true",
] if token not in course_recall]

upload_start = source.index("void MainMenuScene::startModernReplayIrUpload")
upload_end = source.index("\nvoid MainMenuScene::finishReplayIrUpload",
                          upload_start)
upload_source = source[upload_start:upload_end]
missing += ["upload:" + token for token in [
    "replay_records::irUploadUnavailable(context)",
    "replay_records::uploadSavedResult(context, modern.result.attemptId)",
] if token not in upload_source]

for name, text, tokens in [
    ("chart-service", chart_actions, ["LoadModernChartResultByAttempt(", "result_recall::BuildChartResult("]),
    ("course-service", course_actions, ["LoadModernCourseResultByAttempt(", "result_recall::BuildCourseResult("]),
    ("ir-service", ir_actions, ["LoadModernIrSubmissionSnapshot", "snapshot.snapshot->submission",
                               "loadOutbox", "buildDraft", "enqueueManual", "retry"]),
    ("selector", selector, ["chart_records::prepareChartResult(", "course_records::prepareCourseResult(",
                             "replay_records::uploadSavedResult(context, attemptId)",
                             "executeRemoteResultRecall(request, callbacks)"]),
]:
    missing += [name + ":" + token for token in tokens if token not in text]

cleanup_tokens = [
    "replayIrUploadInProgress = false",
    "replayIrObservedRevisions.clear()",
]
missing += ["cleanup:" + token for token in cleanup_tokens
            if token not in combined]

if missing:
    raise SystemExit("records result recall contract failure; missing=" +
                     repr(missing))
