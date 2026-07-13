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
coordinator_header = read("src/ResultPersistenceCoordinator.h")
skin_interface = read("src/skin/ISkin.h")
default_skin = read("src/skin/DefaultSkin.cpp")
capture_source = read("src/practice/PracticeResultFlow.cpp")
cmake = read("CMakeLists.txt")
main_cmake = read("src/CMakeLists.txt")
main_source = read("src/main.cpp")
profile_header = read("src/ProfileSessionCoordinator.h")
profile_source = read("src/ProfileSessionCoordinator.cpp")
application_recovery_header = read("src/ApplicationResultRecovery.h")
application_recovery_source = read("src/ApplicationResultRecovery.cpp")
ios_project = read("ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj")

require(
    context.count("result_persistence::Coordinator resultPersistence") == 1,
    "ApplicationContext must own exactly one result persistence coordinator",
)
require(
    len(
        re.findall(
            r"resultPersistence\(\s*ScoreDBHelper::GetInstance\(\),\s*"
            r"ReplayDBHelper::GetInstance\(\)\s*\)",
            context,
        )
    )
    == 1,
    "ApplicationContext coordinator must bind the active singleton helpers",
)
require(
    "struct ResultPersistenceOptions" in result_header
    and "std::shared_ptr<const result_persistence::ChartResultAttempt> attempt"
    in result_header
    and "result_persistence::SaveOutcome outcome" in result_header,
    "ResultPersistenceOptions must retain one shared immutable attempt and outcome",
)
require(
    "ResultPersistenceOptions resultPersistenceOptions" in game_header,
    "GamePlayScene must retain ResultPersistenceOptions",
)

policy_body = function_body(game_source, "GamePlayScene", "resultCapturePolicy")
for exclusion in (
    "options.autoPlay",
    "options.practiceMode",
    "options.practiceSession",
    "isReplayPlayback()",
    "isCoursePlayback()",
):
    require(
        exclusion in policy_body,
        f"central result capture policy is missing {exclusion}",
    )
require(
    game_source.count("practice::resultCapturePolicy(") == 1,
    "GamePlayScene must use one centralized resultCapturePolicy mapping",
)
capture_policy_body = unqualified_function_body(capture_source, "resultCapturePolicy")
require(
    "!context.autoPlay" in capture_policy_body
    and "!context.replayPlayback" in capture_policy_body
    and "!context.practice" in capture_policy_body
    and "!context.coursePlayback" in capture_policy_body,
    "Auto, replay playback, practice, and course branches must all be non-persistent",
)

begin_body = function_body(game_source, "GamePlayScene", "beginReplayRecording")
require(
    "resultPersistenceOptions = {}" in begin_body
    and "resultPersistenceAttemptId.clear()" in begin_body
    and "resultPersistenceAttemptCreationTried = false" in begin_body,
    "beginReplayRecording must reset all retained attempt state",
)

schedule_body = function_body(
    game_source, "GamePlayScene", "scheduleResultTransition"
)
require(
    "capturePolicy.persistScore && capturePolicy.persistReplay" in schedule_body,
    "eligible staging must require both centralized persistence flags",
)
require(
    ordered(
        schedule_body,
        "finishReplayRecording();",
        "resultPersistenceAttemptCreationTried = true",
        "makeChartResultAttempt(",
        "context.resultPersistence.persist(",
        "delayMillis = 0",
        "defer(",
    ),
    "finish, attempt creation, persistence, and non-saved zero-delay must occur before defer",
)
require(
    schedule_body.count("makeChartResultAttempt(") == 1
    and schedule_body.count("context.resultPersistence.persist(") == 1,
    "scheduleResultTransition must contain one attempt factory and one persistence call",
)
require(
    "if (resultPersistenceAttemptId.empty())" in schedule_body
    and "uuid::generateV4()" in schedule_body,
    "attempt identity must be generated once and retained",
)
require(
    "SaveState::InvalidAttempt" in schedule_body
    and "resultPersistenceOptions.attempt.reset()" in schedule_body
    and "saveStateUserMessage(" in schedule_body,
    "deterministic attempt construction failures must use centralized non-retryable copy",
)
require(
    "InvalidAttempt" in coordinator_header and "retryable()" in coordinator_header,
    "invalid attempts need a truthful typed non-retryable contract",
)
state_name_body = unqualified_function_body(game_source, "resultPersistenceStateName")
require(
    "SaveState::InvalidAttempt" in state_name_body
    and 'return "InvalidAttempt"' in state_name_body,
    "typed persistence logging must name invalid attempt failures explicitly",
)
require(
    "if (!resultPersistenceOptions.outcome.saved())" in schedule_body,
    "every non-saved persistence outcome must force the transition delay to zero",
)
for sensitive in ("recordedReplay", "BmsPath", "chart->Meta.Path", "attempt->replay"):
    for log_call in re.findall(r"SDL_Log[^;]*;", schedule_body, re.DOTALL):
        require(
            sensitive not in log_call,
            f"persistence logging must not expose {sensitive}",
        )
require(
    "presentationReplay, resultPersistenceOptions, retrySource" in schedule_body,
    "GamePlayScene must hand the retained persistence options to ResultScene",
)

scene_sources = "\n".join(
    path.read_text(encoding="utf-8")
    for path in (root / "src/scene").rglob("*.cpp")
)
require(
    re.search(r"\bStageChartResult\s*\(", scene_sources) is None,
    "scenes must never call StageChartResult directly",
)
for obsolete in ("scoreSaved", "replaySaved", "shouldSaveScore", "replayToSave"):
    require(
        re.search(rf"\b{obsolete}\b", result_header + result_source) is None,
        f"ResultScene still owns obsolete chart persistence state: {obsolete}",
    )
require(
    re.search(r"\.SaveScore\s*\(", result_source) is None
    and re.search(r"\.SaveReplay\s*\(", result_source) is None,
    "ResultScene must not directly save chart scores or chart replays",
)

course_score_span = function_span(result_source, "ResultScene", "saveCourseScore")
course_replay_span = function_span(result_source, "ResultScene", "saveCourseReplay")
masked = list(result_source)
for span in (course_score_span, course_replay_span):
    if span is not None:
        masked[span[0] : span[1]] = " " * (span[1] - span[0])
masked_source = "".join(masked)
require(
    course_score_span is not None
    and "SaveCourseScore("
    in result_source[course_score_span[0] : course_score_span[1]],
    "course score persistence must remain in its course-only method",
)
require(
    course_replay_span is not None
    and "SaveCourseReplay("
    in result_source[course_replay_span[0] : course_replay_span[1]],
    "course replay persistence must remain in its course-only method",
)
require(
    "SaveCourseScore(" not in masked_source
    and "SaveCourseReplay(" not in masked_source,
    "SaveCourse* calls are allowed only inside course-only persistence methods",
)

require(result_source.count('"Retry Save"') == 1, "missing exact Retry Save action")
require(
    result_source.count('"Continue Without Saving"') == 1,
    "missing exact Continue Without Saving action",
)
status_body = function_body(
    result_source, "ResultScene", "addResultPersistenceStatus"
)
require(
    "persistenceOptions.outcome.userMessage" in status_body,
    "status panel must render the coordinator userMessage verbatim",
)
require(
    "retryResultPersistence" in status_body
    and "continueWithoutSaving" in status_body,
    "status panel must bind both blocking decisions",
)

retry_body = function_body(
    result_source, "ResultScene", "retryResultPersistence"
)
require(
    "context.resultPersistence.persist(*persistenceOptions.attempt)" in retry_body,
    "Retry Save must reuse the exact immutable attempt",
)
require(
    "applyResultPersistenceReceipt();" in retry_body,
    "Retry Save must propagate a returned receipt",
)
require(
    ordered(
        retry_body,
        "applyResultPersistenceReceipt();",
        "loadPreviousBest();",
        "defer(",
        "refreshResultSummary();",
    ),
    "Retry Save must defer rebuilding visible comparison and pacemaker summary after reloading best",
)
require(
    re.search(
        r"defer\(\s*\[this\]\(\)\s*\{\s*"
        r"refreshResultSummary\(\);\s*"
        r"updateResultPersistencePresentation\(\);\s*"
        r"return true;",
        retry_body,
        re.DOTALL,
    )
    is not None,
    "normal result actions must remain blocked until the deferred summary refresh completes",
)
for sensitive in ("presentationReplay", "retryData", "BmsPath", "attempt->replay"):
    for log_call in re.findall(r"SDL_Log[^;]*;", retry_body, re.DOTALL):
        require(
            sensitive not in log_call,
            f"retry logging must not expose {sensitive}",
        )
require(
    result_source.count("applyResultPersistenceReceipt();") >= 2,
    "receipt identity/timestamp must be applied initially and after Retry Save",
)
receipt_body = function_body(
    result_source, "ResultScene", "applyResultPersistenceReceipt"
)
require(
    "validatedReceiptFor(" in receipt_body
    and "receipt->replayId" in receipt_body
    and "receipt->createdAt" in receipt_body
    and "presentationReplay" in receipt_body
    and "retryData" in receipt_body,
    "receipt replay ID and createdAt must reach presentation and retry replay copies",
)

previous_body = function_body(result_source, "ResultScene", "loadPreviousBest")
require(
    "excludeAttemptId" in previous_body
    and "validatedReceiptFor(" in previous_body
    and "persistenceOptions.attempt->attemptId" in previous_body,
    "previous best must exclude only a receipt-proven staged live attempt",
)

summary_body = function_body(result_source, "ResultScene", "refreshResultSummary")
require(
    'findViewByName("resultSummary")' in summary_body
    and 'rebuildLayoutSection("ResultSummary"' in summary_body
    and "makeResultSkinData()" in summary_body,
    "visible result summary refresh must target the named skin section with fresh data",
)
require(
    "rebuildLayoutSection" in skin_interface
    and 'sectionName != "ResultSummary"' in default_skin
    and "buildResultSummary(" in default_skin
    and "View::LayoutBatchScope" in default_skin
    and "clearChildren();" in default_skin,
    "the skin must rebuild only the result summary section in place",
)
require(
    ordered(
        previous_body,
        "beforeCreatedAt",
        "excludeAttemptId",
        "LoadBestScore(",
    ),
    "previous-best query must pass legacy timestamp and staged-attempt filters",
)
require(
    "replayResult" in previous_body and "retryData->createdAt" in previous_body,
    "legacy replay results must preserve the beforeCreatedAt boundary",
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
    "normal actions must stay hidden behind the blocking persistence status",
)
decision_body = function_body(
    result_source, "ResultScene", "persistenceDecisionRequired"
)
require(
    "outcome.requiresUserDecision(" in decision_body
    and "persistenceContinueChosen" in decision_body,
    "the scene exit guard must delegate to tested decision semantics",
)

init_body = function_body(result_source, "ResultScene", "init")
require(
    ordered(init_body, "addResultPersistenceStatus();", "addRetryButtons();"),
    "the persistence status must be installed before normal result actions",
)
require(
    "result_persistence_flow_audit" in cmake
    and "find_package(Python3 REQUIRED COMPONENTS Interpreter)" in cmake
    and "${Python3_EXECUTABLE}" in cmake
    and "scripts/check_result_persistence_flow.py" in cmake
    and "scripts/check_result_persistence_flow.sh" not in cmake,
    "CTest must invoke the Python audit through CMake's cross-platform interpreter",
)

require(
    coordinator_header.count("recoveryUserMessage()") == 1
    and coordinator_header.count("recoveryFailureSummary(") == 1,
    "the coordinator must expose one aggregate warning source and sanitized failure factory",
)
require(
    "namespace application_result_recovery" in application_recovery_header
    and "std::function<result_persistence::RecoverySummary()> recover" in application_recovery_header
    and "reportWarning" in application_recovery_header
    and "runReadyRuntime" in application_recovery_header
    and "void execute(const Dependencies &dependencies)" in application_recovery_header,
    "startup recovery must expose the pure three-callback orchestration contract",
)
application_recovery_body = unqualified_function_body(
    application_recovery_source, "execute"
)
require(
    ordered(
        application_recovery_body,
        "dependencies.recover()",
        "!summary.userMessage.empty()",
        "dependencies.reportWarning(summary)",
        "dependencies.runReadyRuntime()",
    ),
    "startup recovery must recover once, optionally warn, then run runtime",
)
require(
    application_recovery_body.count("dependencies.recover()") == 1
    and application_recovery_body.count("dependencies.reportWarning(summary)") == 1
    and application_recovery_body.count("dependencies.runReadyRuntime()") == 1,
    "startup recovery callbacks must each have one call site",
)

require(
    profile_header.count("recoverPendingResults") == 1
    and "std::function<result_persistence::RecoverySummary()>" in profile_header,
    "profile sessions must inject exactly one typed pending-result recovery callback",
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
    "profile recovery must follow both target binds and precede input, caches, and commit",
)
require(
    "recoveryUserMessage()" in forward_switch
    and "catch (const std::exception &" in forward_switch
    and "catch (...)" in forward_switch,
    "profile recovery callback exceptions must become the centralized sanitized warning",
)

require(
    context.count("recoverPendingResults() noexcept") == 1
    and context.count(".recoverPendingResults = [this]") == 1
    and context.count("resultPersistence.recoverAll()") == 1
    and context.count("recoveryFailureSummary(") == 2,
    "ApplicationContext must own one nonthrowing recovery adapter reused by profile activation",
)
require(
    main_source.count('#include "ApplicationResultRecovery.h"') == 1
    and main_source.count("application_result_recovery::execute(") == 1,
    "main must delegate post-database recovery to the pure orchestrator exactly once",
)
require(
    main_source.count("runReadyApplicationAfterResultRecovery") == 2
    and main_source.count("SceneManager sceneManager(context)") == 1,
    "scene registration must live only in the post-recovery runtime body",
)
main_recovery_body = unqualified_function_body(main_source, "runReadyApplication")
require(
    ordered(
        main_recovery_body,
        "application_result_recovery::execute(",
        ".recover = [&context]",
        "context.recoverPendingResults()",
        ".reportWarning",
        ".runReadyRuntime",
        "runReadyApplicationAfterResultRecovery(context)",
    ),
    "startup ready callback must bind recovery, warning, and post-recovery runtime",
)
warning_body = unqualified_function_body(main_source, "reportResultRecoveryWarning")
require(
    '"AsoBMaShow Result Recovery"' in warning_body
    and "recovery.userMessage.c_str()" in warning_body
    and "s_window" in warning_body
    and "recovery.diagnostic" not in warning_body
    and "attemptId" not in warning_body,
    "native recovery warning must show only sanitized copy on the current window",
)
require(
    cmake.count("tests/application_result_recovery_tests.cpp") == 1
    and cmake.count("src/ApplicationResultRecovery.cpp") == 1
    and cmake.count("application_result_recovery_tests") >= 3,
    "CMake must build and register the pure startup recovery test once",
)
require(
    main_cmake.count("ApplicationResultRecovery.cpp") == 1,
    "the main target must compile ApplicationResultRecovery.cpp exactly once",
)
require(
    len(re.findall(r"\n\s*ApplicationResultRecovery\.cpp,", ios_project)) == 1,
    "iOS source membership must contain ApplicationResultRecovery.cpp exactly once",
)

if failures:
    for failure in failures:
        print(f"result persistence flow audit failed: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("result persistence flow audit passed")
