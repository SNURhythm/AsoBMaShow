import argparse
from pathlib import Path


def extract(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise ValueError(f"Unterminated production method: {signature}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "src/scene/play/GamePlayScene.cpp").read_text()
    signatures = [
        "void GamePlayScene::update(float dt)",
        "void GamePlayScene::completePracticeSection(",
        "void GamePlayScene::finalizePracticeRangeMisses()",
        "void GamePlayScene::completePracticeAttempt()",
        "void GamePlayScene::finishReplayRecording()",
        "void GamePlayScene::abortPlayFromStartSelectControl()",
        "void GamePlayScene::consumeStartSelectInput(",
        "void GamePlayScene::drainRealtimeStartSelectInputs()",
        "GamePlayScene::CompletedModernReplayCapture\nGamePlayScene::completeModernReplayCapture()",
        "void GamePlayScene::processReplayEvents(",
        "void GamePlayScene::recordModernCourseStage(",
        "void GamePlayScene::finishPractice()",
        "void GamePlayScene::appendReplayEvent(",
        "bool GamePlayScene::finishIfGaugeFailed()",
        "practice::ResultCapturePolicy GamePlayScene::resultCapturePolicy() const",
        "void GamePlayScene::applyReplayEvent(",
        "void GamePlayScene::applyReplayGauge(",
        "void GamePlayScene::buildReplayNoteLookup()",
        "bms_parser::Note *\nGamePlayScene::findReplayNote(",
        "JudgeResult GamePlayScene::pressNote(",
        "JudgeResult GamePlayScene::releaseNote(",
        "void GamePlayScene::expireGimmickNote(",
    ]
    fixture = (args.root / "tests/gameplay_terminal_scene_fixture.cpp").read_text()
    helpers = [
        "replay::ReplayTouchAction modernTouchAction(",
        "bool longNoteTailJudgedBeforeTiming(",
        "void markReplayMissedNote(",
        "JudgeResult normalizeLongNoteReleaseJudge(",
        "JudgeResult judgeClassicLongNoteRelease(",
        "ReplayEventAction\nreplayActionFromRealtime(",
        "std::vector<bms_parser::Note *>\nbuildRealtimeGameplayNoteLookup(",
    ]
    methods = "\n\n".join(extract(source, signature) for signature in helpers) + "\n"
    methods += "\n\n".join(extract(source, signature) for signature in signatures)
    sync_method = extract(source, "void GamePlayScene::syncRealtimeGameplaySnapshot()")
    methods += "\n" + sync_method.replace("syncRealtimeGameplaySnapshot()",
                                           "syncRealtimeGameplaySnapshotFromWorker()", 1)
    stop_method = extract(source, "void GamePlayScene::stopRealtimeGameplayAuthority(")
    stop_tail = stop_method[stop_method.index("  session.worker->stop();"):]
    methods += "\nvoid GamePlayScene::stopRealtimeGameplayAuthorityFromWorker(bool transferReplay) {\n"
    methods += "  auto &session = *realtimeGameplaySession;\n" + stop_tail
    result_source = (args.root / "src/scene/ResultScene.cpp").read_text()
    result_method = extract(result_source, "void ResultScene::continueCourse()")
    result_prefix = result_method[:result_method.index("  std::atomic_bool parseCancelled")]
    result_prefix += '  require(false, "unfinished course stage advanced to parsing");\n}'
    export_source = (args.root / "src/ReplayVideoExporter.cpp").read_text()
    export_method = extract(export_source, "ReplayVideoExportResult\nReplayVideoExporter::Export(")
    export_prefix = export_method[:export_method.index("  reportReplayExportProgress")]
    export_prefix += '  ++context.resourceStarts;\n  return {.success = true};\n}'
    course_export = extract(export_source, "ReplayVideoExportResult exportCourseReplayImpl(")
    course_export_prefix = course_export[:course_export.index("  GameplaySkinSessionStopOwner")]
    course_export_prefix += '  ++context.resourceStarts;\n  return {.success = true};\n}'
    args.output.write_text(fixture.replace("SCENE_METHODS", methods)
                          .replace("RESULT_CONTINUE_PREFIX", result_prefix)
                          .replace("EXPORT_PREFIXES", export_prefix + "\n" + course_export_prefix))


if __name__ == "__main__":
    main()
