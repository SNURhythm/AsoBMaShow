#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import re
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
failures: list[str] = []


def read(relative: str) -> str:
    path = root / relative
    if not path.is_file():
        failures.append(f"missing required file: {relative}")
        return ""
    return path.read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def matching_brace(text: str, opening: int) -> int:
    depth = 0
    state = "code"
    index = opening
    while index < len(text):
        char = text[index]
        following = text[index + 1] if index + 1 < len(text) else ""
        if state == "code":
            if char == "/" and following == "/":
                state = "line_comment"
                index += 2
                continue
            if char == "/" and following == "*":
                state = "block_comment"
                index += 2
                continue
            if char == '"':
                state = "string"
            elif char == "'":
                state = "character"
            elif char == "{":
                depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return index
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and following == "/":
                state = "code"
                index += 2
                continue
        elif state in {"string", "character"}:
            if char == "\\":
                index += 2
                continue
            if (state == "string" and char == '"') or (
                state == "character" and char == "'"
            ):
                state = "code"
        index += 1
    raise ValueError("unbalanced function body")


def function_span(text: str, owner: str, name: str) -> tuple[int, int] | None:
    signature = re.compile(
        rf"\b{re.escape(owner)}::{re.escape(name)}\s*\(", re.MULTILINE
    )
    match = signature.search(text)
    if match is None:
        failures.append(f"missing function body: {owner}::{name}")
        return None
    opening = text.find("{", match.end())
    if opening < 0:
        failures.append(f"missing opening brace: {owner}::{name}")
        return None
    try:
        closing = matching_brace(text, opening)
    except ValueError:
        failures.append(f"unbalanced function body: {owner}::{name}")
        return None
    return opening + 1, closing


def function_body(text: str, owner: str, name: str) -> str:
    span = function_span(text, owner, name)
    return "" if span is None else text[span[0] : span[1]]


def unqualified_function_body(text: str, name: str) -> str:
    match = re.search(rf"\b{re.escape(name)}\s*\([^)]*\)[^{{;]*\{{", text)
    if match is None:
        failures.append(f"missing function body: {name}")
        return ""
    opening = text.find("{", match.start())
    try:
        closing = matching_brace(text, opening)
    except ValueError:
        failures.append(f"unbalanced function body: {name}")
        return ""
    return text[opening + 1 : closing]


def ordered(body: str, *needles: str) -> bool:
    cursor = -1
    for needle in needles:
        cursor = body.find(needle, cursor + 1)
        if cursor < 0:
            return False
    return True


context = read("src/context.h")
game_header = read("src/scene/play/GamePlayScene.h")
game_source = read("src/scene/play/GamePlayScene.cpp")
result_header = read("src/scene/ResultScene.h")
result_source = read("src/scene/ResultScene.cpp")
capture_policy_source = read("src/practice/PracticeResultFlow.cpp")
chart_capture_source = read("src/replay/ChartReplayCapture.cpp")
course_capture_source = read("src/replay/CourseReplayCapture.cpp")
chart_persistence_header = read("src/replay/ChartReplayPersistence.h")
chart_persistence_source = read("src/replay/ChartReplayPersistence.cpp")
course_persistence_source = read("src/replay/CourseReplayPersistence.cpp")
course_result_source = read("src/replay/CourseResultPersistence.cpp")
skin_interface = read("src/skin/ISkin.h")
default_skin = read("src/skin/DefaultSkin.cpp")
cmake = read("CMakeLists.txt")
main_cmake = read("src/CMakeLists.txt")
main_source = read("src/main.cpp")
profile_header = read("src/ProfileSessionCoordinator.h")
profile_source = read("src/ProfileSessionCoordinator.cpp")
application_recovery_header = read("src/ApplicationResultRecovery.h")
application_recovery_source = read("src/ApplicationResultRecovery.cpp")
ios_project = read("ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj")

# ApplicationContext is only an adapter. File ownership, summary staging, and
# score projection belong to the modern persistence coordinators.
require(
    "result_persistence::Coordinator resultPersistence" not in context,
    "ApplicationContext must not retain the retired SQLite replay coordinator",
)
require(
    context.count("persistModernChart(") == 1
    and context.count("persistModernCourse(") == 1
    and context.count("recoverPendingResults() noexcept") == 1,
    "ApplicationContext must expose one chart, course, and recovery adapter",
)
chart_adapter = context
course_adapter = context
context_recovery_body = context
require(
    ordered(
        chart_adapter,
        "replay::ChartReplayPersistence persistence(scoreRepository,",
        "replayRepository)",
        "persistence.persist(attempt, drafts)",
    ),
    "chart adapter must bind both profile repositories to ChartReplayPersistence",
)
require(
    ordered(
        course_adapter,
        "replay::CourseResultPersistence persistence(scoreRepository,",
        "replayRepository)",
        "persistence.persist(attempt)",
    ),
    "course adapter must bind both profile repositories to CourseResultPersistence",
)
require(
    ordered(
        context_recovery_body,
        "replay::ChartReplayPersistence chartPersistence(scoreRepository,",
        "replayRepository)",
        "chartPersistence.recoverAll()",
        "replay::CourseResultPersistence coursePersistence(scoreRepository,",
        "coursePersistence.recoverAll()",
    ),
    "recovery must use the modern chart and course persistence authorities",
)

require(
    "struct ResultPersistenceOptions" in result_header
    and "std::shared_ptr<const replay::ChartReplayPersistenceAttempt> chartAttempt"
    in result_header
    and "std::optional<replay::ChartReplayPersistenceOutcome> chartOutcome"
    in result_header,
    "ResultPersistenceOptions must retain one immutable modern chart attempt",
)
require(
    "ResultPersistenceOptions resultPersistenceOptions" in game_header,
    "GamePlayScene must retain the modern persistence presentation",
)

policy_body = function_body(game_source, "GamePlayScene", "resultCapturePolicy")
for exclusion in (
    "options.autoPlay",
    "options.practiceMode",
    "options.practiceSession",
    "isReplayPlayback()",
    "isCoursePlayback()",
):
    require(exclusion in policy_body, f"central capture policy is missing {exclusion}")
require(
    game_source.count("practice::resultCapturePolicy(") == 1,
    "GamePlayScene must map persistence eligibility through one policy",
)
capture_policy_body = unqualified_function_body(
    capture_policy_source, "resultCapturePolicy"
)
require(
    "!context.autoPlay" in capture_policy_body
    and "!context.replayPlayback" in capture_policy_body
    and "!context.practice" in capture_policy_body
    and "!context.coursePlayback" in capture_policy_body,
    "auto, replay, practice, and course playback must not create chart results",
)

begin_body = function_body(game_source, "GamePlayScene", "beginReplayRecording")
require(
    ordered(
        begin_body,
        "resultPersistenceOptions = {}",
        "resultPersistenceAttemptId.clear()",
        "resultPersistenceAttemptCreationTried = false",
        "modernReplayInputRecorder.reset()",
    ),
    "new gameplay must reset every retained modern capture identity",
)

schedule_body = function_body(game_source, "GamePlayScene", "scheduleResultTransition")
require(
    ordered(
        schedule_body,
        "finishReplayRecording();",
        "completedAttemptPersistenceRoute(",
        "completeModernReplayCapture()",
        "uuid::generateV4()",
        "captureModernChartResult(",
        "captureChartReplayPersistenceAttempt(",
        "context.persistModernChart(",
        "delayMillis = 0",
        "defer(",
    ),
    "chart capture, persistence, and non-saved presentation must have one ordered route",
)
require(
    schedule_body.count("captureModernChartResult(") == 1
    and schedule_body.count("captureChartReplayPersistenceAttempt(") == 1
    and schedule_body.count("context.persistModernChart(") == 1,
    "scheduleResultTransition must have one modern chart capture/persist authority",
)
require(
    "capturePolicy.persistScore && capturePolicy.persistReplay" in schedule_body
    and "if (resultPersistenceAttemptId.empty())" in schedule_body,
    "modern staging must require the shared policy and retain one attempt ID",
)
require(
    "presentationReplay, resultPersistenceOptions, retrySource" in schedule_body,
    "GamePlayScene must hand the exact retained attempt to ResultScene",
)

chart_capture_body = unqualified_function_body(
    chart_capture_source, "captureChartReplayPersistenceAttempt"
)
require(
    ordered(
        chart_capture_body,
        "validateModernChartResult(",
        "captureIrSubmissionSnapshot(",
        "captureLocalReplaySetup(",
        "validateReplayPlayback(",
        "compareChartReplayToResult(",
        "attempt.replay = std::move(document)",
    ),
    "chart capture must derive snapshot and replay from one validated result",
)
require(
    "return attempt;" in chart_capture_body,
    "missing raw replay capture must still preserve the modern result and IR snapshot",
)
course_capture_body = unqualified_function_body(
    course_capture_source, "captureCourseReplayAttempt"
)
require(
    ordered(
        course_capture_body,
        "validateModernCourseResult(",
        "pathInput.stageSha256.push_back",
        "validateReplayPlayback(",
        "compareCourseReplayToResult(",
        "compareCourseReplayPathToResult(",
        "attempt.replay = std::move(document)",
    ),
    "course capture must share result, setup, limits, and path agreement",
)

chart_persist_body = function_body(
    chart_persistence_source, "ChartReplayPersistence", "persist"
)
require(
    ordered(
        chart_persist_body,
        "validateModernChartResult(",
        "captureIrSubmissionSnapshot(",
        "dependencies_.loadResult(",
        "compareChartReplayToResult(",
        "fileCoordinator.associate(",
        "dependencies_.stage(",
        "dependencies_.loadPending(",
        "completePendingChartScore(",
    ),
    "chart persistence must validate once, associate BRD, stage summary, then project score",
)
require(
    "repository.StageModernChartResult(result, snapshot," in chart_persistence_source
    and "repository.GetResolvedProfileRoot()" in chart_persistence_source
    and "codec->encodeChart(" in chart_persistence_source,
    "chart persistence must own the Beatoraja file and modern repository boundary",
)
require(
    "SavedWithReplay" in chart_persistence_header
    and "SavedWithoutReplay" in chart_persistence_header
    and "PendingScore" in chart_persistence_header
    and "IntegrityConflict" in chart_persistence_header,
    "chart persistence must distinguish durable summary, file, and projection states",
)

course_persist_body = function_body(
    course_persistence_source, "CourseReplayPersistence", "persist"
)
require(
    ordered(
        course_persist_body,
        "validateModernCourseResult(",
        "dependencies_.loadResult(",
        "compareCourseReplayPathToResult(",
        "compareCourseReplayToResult(",
        "fileCoordinator.associate(",
        "dependencies_.stage(",
    ),
    "course persistence must share result, path, replay, and file association authorities",
)
require(
    "repository.StageModernCourseResult(result, file, path)" in course_persistence_source
    and "repository.GetResolvedProfileRoot()" in course_persistence_source
    and "codec->encodeCourse(" in course_persistence_source,
    "course persistence must own the Beatoraja file and modern repository boundary",
)
course_result_body = function_body(
    course_result_source, "CourseResultPersistence", "persist"
)
require(
    ordered(
        course_result_body,
        "dependencies_.persistResult(attempt)",
        "resultOutcome.receipt->attemptId != attempt.result.attemptId",
        "makePendingScoreWrite(",
        "dependencies_.projectScore(*pending)",
    ),
    "course score projection must follow a receipt-proven durable modern result",
)
course_recovery_body = function_body(
    course_result_source, "CourseResultPersistence", "recoverAll"
)
require(
    ordered(
        course_recovery_body,
        "dependencies_.listScoreSources(",
        "makePendingScoreWrite(",
        "dependencies_.projectScore(*pending)",
    ),
    "course recovery must reuse stored-result identity and score projection authorities",
)

scene_sources = "\n".join(
    path.read_text(encoding="utf-8")
    for path in (root / "src/scene").rglob("*.cpp")
)
for obsolete_call in (
    "StageChartResult",
    "StageModernChartResult",
    "StageModernCourseResult",
    "SaveReplay",
    "SaveCourseReplay",
    "SaveCourseScore",
    "SaveReplayEvent",
    "SaveReplayTouch",
    "SaveLaneCover",
):
    require(
        re.search(rf"\b{obsolete_call}\s*\(", scene_sources) is None,
        f"scenes must not own repository persistence: {obsolete_call}",
    )

require(result_source.count('"Retry Save"') == 1, "missing exact Retry Save action")
require(
    result_source.count('"Continue Without Saving"') == 1,
    "missing exact Continue Without Saving action",
)
status_body = function_body(result_source, "ResultScene", "addResultPersistenceStatus")
require(
    "persistenceOptions.outcome.userMessage" in status_body
    and "retryResultPersistence" in status_body
    and "continueWithoutSaving" in status_body,
    "persistence status must render and bind both blocking decisions",
)
retry_body = function_body(result_source, "ResultScene", "retryResultPersistence")
require(
    re.search(
        r"context\.persistModernChart\(\s*"
        r"\*persistenceOptions\.chartAttempt\s*,\s*automaticDrafts\s*\)",
        retry_body,
        re.DOTALL,
    )
    is not None,
    "Retry Save must reuse the exact immutable chart attempt",
)
require(
    ordered(
        retry_body,
        "context.persistModernChart(",
        "chartResultPersistencePresentation(",
        "previousBestLoaded = false",
        "loadPreviousBest();",
        "defer(",
        "refreshResultSummary();",
    ),
    "chart retry must update durable state before rebuilding visible comparison",
)
require(
    ordered(
        retry_body,
        "persistModernCourseResult();",
        "loadPreviousBest();",
        "refreshResultSummary();",
    ),
    "course retry must use the same modern course persistence path",
)

previous_body = function_body(result_source, "ResultScene", "loadPreviousBest")
require(
    ordered(previous_body, "excludeAttemptId", "chartOutcome->durable()", "LoadBestScore("),
    "previous best must exclude the receipt-proven modern attempt ID",
)
require(
    "chartAttempt->result.attemptId" in previous_body
    and "retryData->createdAt" in previous_body,
    "modern attempt exclusion and legacy browse timestamp must remain distinct",
)
summary_body = function_body(result_source, "ResultScene", "refreshResultSummary")
require(
    'findViewByName("resultSummary")' in summary_body
    and 'rebuildLayoutSection("ResultSummary"' in summary_body
    and "makeResultSkinData()" in summary_body,
    "visible result summary refresh must rebuild the named section from stored facts",
)
require(
    "rebuildLayoutSection" in skin_interface
    and 'sectionName != "ResultSummary"' in default_skin
    and "buildResultSummary(" in default_skin
    and "clearChildren();" in default_skin,
    "the skin must rebuild only the result summary section in place",
)

exit_body = function_body(result_source, "ResultScene", "exitResult")
exit_without_space = re.sub(r"\s+", " ", exit_body).strip()
require(
    exit_without_space.startswith("if (persistenceDecisionRequired()) { return; }"),
    "exitResult must guard unresolved persistence before every side effect",
)
presentation_body = function_body(
    result_source, "ResultScene", "updateResultPersistencePresentation"
)
require(
    "normalResultActions->setVisible(!decisionRequired)" in presentation_body
    and "resultPersistenceStatus->setVisible(decisionRequired)" in presentation_body,
    "normal actions must remain hidden behind the persistence decision",
)
decision_body = function_body(result_source, "ResultScene", "persistenceDecisionRequired")
require(
    "outcome.requiresUserDecision(" in decision_body
    and "persistenceContinueChosen" in decision_body,
    "the scene exit guard must delegate to typed persistence semantics",
)
init_body = function_body(result_source, "ResultScene", "init")
require(
    ordered(init_body, "persistModernCourseResult();", "addResultPersistenceStatus();"),
    "course final results must persist before the blocking status is rendered",
)

require(
    "namespace application_result_recovery" in application_recovery_header
    and "std::function<replay::ChartReplayRecoverySummary()> recover" in application_recovery_header
    and "reportWarning" in application_recovery_header
    and "startProfileServices" in application_recovery_header
    and "runReadyRuntime" in application_recovery_header,
    "startup recovery must expose typed recovery, warning, services, and runtime callbacks",
)
application_recovery_body = unqualified_function_body(application_recovery_source, "execute")
require(
    ordered(
        application_recovery_body,
        "dependencies.recover()",
        "summary.pending != 0 || summary.conflicts != 0",
        "dependencies.reportWarning(summary)",
        "dependencies.startProfileServices()",
        "dependencies.runReadyRuntime()",
    ),
    "startup must recover and warn before optional services and ready runtime",
)
require(
    profile_header.count("recoverPendingResults") == 1
    and "std::function<replay::ChartReplayRecoverySummary()>" in profile_header,
    "profile switching must inject one typed modern recovery callback",
)
switch_body = function_body(profile_source, "ProfileSessionCoordinator", "switchTo")
score_bind = switch_body.find("dependencies_.bindScore(")
replay_bind = switch_body.find("dependencies_.bindReplay(", score_bind + 1)
forward_switch = switch_body[replay_bind:] if replay_bind >= 0 else ""
require(
    score_bind >= 0
    and replay_bind > score_bind
    and ordered(
        forward_switch,
        "dependencies_.bindReplay(",
        "dependencies_.recoverPendingResults()",
        "applyInput_(",
        "refreshCaches_()",
        "manager_.commitActiveProfile(",
    ),
    "profile recovery must follow both target binds and precede input and commit",
)
require(
    "chartReplayRecoveryUserMessage()" in forward_switch
    and "catch (const std::exception &)" in forward_switch
    and "catch (...)" in forward_switch,
    "profile recovery failures must become one sanitized warning",
)

require(
    main_source.count('#include "ApplicationResultRecovery.h"') == 1
    and main_source.count("application_result_recovery::execute(") == 1,
    "main must delegate post-database recovery exactly once",
)
main_recovery_body = unqualified_function_body(main_source, "runReadyApplication")
require(
    ordered(
        main_recovery_body,
        "application_result_recovery::execute(",
        "context.recoverPendingResults()",
        ".reportWarning",
        ".startProfileServices",
        "context.startIrServices()",
        ".runReadyRuntime",
        "runReadyApplicationAfterResultRecovery(context)",
    ),
    "main must bind recovery, optional IR services, and post-recovery runtime",
)
warning_body = unqualified_function_body(main_source, "reportResultRecoveryWarning")
require(
    '"AsoBMaShow Result Recovery"' in warning_body
    and "chartReplayRecoveryUserMessage().data()" in warning_body
    and "s_window" in warning_body
    and "diagnostic" not in warning_body
    and "attemptId" not in warning_body,
    "native recovery warning must show only centralized sanitized copy",
)

require(
    "result_persistence_flow_audit" in cmake
    and "find_package(Python3 REQUIRED COMPONENTS Interpreter)" in cmake
    and "${Python3_EXECUTABLE}" in cmake
    and "scripts/check_result_persistence_flow.py" in cmake
    and cmake.count("tests/application_result_recovery_tests.cpp") == 1
    and cmake.count("src/ApplicationResultRecovery.cpp") == 1,
    "CTest must register the flow audit and startup recovery test",
)
for source in (
    "replay/ChartReplayCapture.cpp",
    "replay/CourseReplayCapture.cpp",
    "replay/ChartReplayPersistence.cpp",
    "replay/CourseReplayPersistence.cpp",
    "replay/CourseResultPersistence.cpp",
    "ApplicationResultRecovery.cpp",
):
    require(source in main_cmake, f"main target is missing {source}")
require(
    ios_project.count("fileSystemSynchronizedGroups = (") == 1
    and "B76AAF3F2DA4A1C400E8327C /* ../../../../src */" in ios_project,
    "iOS target must compile the synchronized src folder",
)

if failures:
    for failure in failures:
        print(f"result persistence flow audit failed: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("result persistence flow audit passed")
