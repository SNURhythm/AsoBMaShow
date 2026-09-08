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
    result_util_source = (args.root / "src/ResultPresentationUtils.h").read_text()
    result_helpers = "namespace result_presentation {\n" + "\n\n".join(
        extract(result_util_source, signature) for signature in [
            "inline bms_parser::ChartMeta courseResultMeta(",
            "inline bool isFullComboCourseResult(",
        ]) + "\n}\n"
    result_helpers += "\n\n".join(extract(result_source, signature) for signature in [
        "std::int64_t nowUnixMillis()",
        "void applyModernCoursePersistencePresentation(",
        "int totalNotesForCourse(",
        "long long totalPlayLengthForCourse(",
        "bms_parser::ChartMeta\ncourseResultMetaForSession(",
        "int courseResultClearTypeForSession(",
    ])
    result_persist = extract(result_source, "bool ResultScene::persistModernCourseResult()")
    preparation_methods = "\n\n".join(extract(source, signature) for signature in [
        "Judge presentationJudgeForPolicy(",
        "std::optional<NoteTimeRange>\npracticeAllowedNoteRange(",
        "bool prepareRetryChart(",
        "GamePlayScene::GamePlayScene(ApplicationContext &context,\n                             bms_parser::Chart *chart",
        "GamePlayScene::GamePlayScene(ApplicationContext &context,\n                             std::unique_ptr<bms_parser::Chart> chart",
        "bool GamePlayScene::preparePracticeAttemptFromMenu(",
    ])
    fallback_source = extract(source, "bool GamePlayScene::enterPracticeMenu()")
    fallback_start = fallback_source.index('    if (!applyPracticePlayOptions(')
    fallback_end = fallback_source.index('\n  practiceMenuActive = true;')
    fallback_body = fallback_source[fallback_start:fallback_end].rsplit('\n  }', 1)[0]
    state_start = fallback_source.index('  ownedState = std::make_unique<RhythmState>')
    state_end = fallback_source.index('  capturePlayfieldVisualState(')
    preparation_methods += '\nbool PreparedGamePlayScene::prepareBuiltInFallback() {\n'
    preparation_methods += fallback_source[state_start:state_end] + fallback_body + '\n}\n'
    reset_source = extract(source, 'bool GamePlayScene::reset()')
    reset_start = reset_source.index('  for (const auto &measure : chart->Measures)')
    reset_end = reset_source.index('  context.jukebox.stop();', reset_start)
    preparation_methods += '\nvoid PreparedGamePlayScene::resetNotesForSameAttempt() {\n'
    preparation_methods += reset_source[reset_start:reset_end] + '\n}\n'
    preparation_methods = (preparation_methods
        .replace('GamePlayScene::GamePlayScene', 'PreparedGamePlayScene::PreparedGamePlayScene')
        .replace('GamePlayScene::preparePracticeAttemptFromMenu', 'PreparedGamePlayScene::preparePracticeAttemptFromMenu')
        .replace('ApplicationContext', 'PreparationContext')
        .replace(': Scene(context)', ': PreparationSceneBase(context)')
        .replace('resolvePlayStartInputDevices', 'resolvePreparationInputDevices')
        .replace('buildPlayfieldChartVisualModel', 'buildPreparationVisualModel')
        .replace('RhythmLaneInputController', 'PreparationLaneController'))
    viewer_source = (args.root / "src/scene/ChartViewerScene.cpp").read_text()
    preparation_methods += '\n' + '\n\n'.join(extract(viewer_source, signature) for signature in [
        'bool isLaneOrderSummaryOption(',
        'std::optional<std::string>\nformatLaneOrderSummary(',
        'bool ChartViewerScene::applyViewerPlayOptions(',
    ]).replace('ChartViewerScene::', 'PreparedViewerFixture::')
    viewer_start = viewer_source.index('        std::atomic_bool parseCancelled = false;',
                                        viewer_source.index('Preparing auto play...'))
    viewer_end = viewer_source.index('        context.jukebox.stop();', viewer_start)
    viewer_prefix = viewer_source[viewer_start:viewer_end].replace(
        '        std::unique_ptr<bms_parser::Chart> practiceChart;', '')
    preparation_methods += '''
std::unique_ptr<bms_parser::Chart> PreparedViewerFixture::freshLaunchChart(bool autoPlay) {
  const auto chartRandomSeed = record.meta.RandomSeed;
  const auto chartRandomPrng = record.meta.RandomPrng;
  const auto chartRandomValues = play_options::randomValuesOrNull(record.meta.RandomValues);
  std::unique_ptr<bms_parser::Chart> practiceChart;
  const bool failed = [&]() {
''' + viewer_prefix + '''
    return false;
  }();
  return failed ? nullptr : std::move(practiceChart);
}
'''
    export_source = (args.root / "src/ReplayVideoExporter.cpp").read_text()
    export_method = extract(export_source, "ReplayVideoExportResult\nReplayVideoExporter::Export(")
    export_prefix = export_method[:export_method.index("  reportReplayExportProgress")]
    export_prefix += '  ++context.resourceStarts;\n  return {.success = true};\n}'
    course_export = extract(export_source, "ReplayVideoExportResult exportCourseReplayImpl(")
    course_export_prefix = course_export[:course_export.index("  GameplaySkinSessionStopOwner")]
    course_export_prefix += '  ++context.resourceStarts;\n  return {.success = true};\n}'
    args.output.write_text(fixture.replace("SCENE_METHODS", methods)
                          .replace("RESULT_CONTINUE_PREFIX", result_prefix)
                          .replace("RESULT_PERSIST_HELPERS", result_helpers)
                          .replace("RESULT_PERSIST_METHOD", result_persist)
                          .replace("PREPARATION_IMPLEMENTATIONS", preparation_methods)
                          .replace("EXPORT_PREFIXES", export_prefix + "\n" + course_export_prefix))


if __name__ == "__main__":
    main()
