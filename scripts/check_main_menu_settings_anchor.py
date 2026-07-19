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
source_path = root / "src/scene/MainMenuScene.cpp"
settings_source_path = root / "src/scene/SettingsSceneIr.cpp"

if not source_path.is_file():
    print("FAIL: missing src/scene/MainMenuScene.cpp", file=sys.stderr)
    raise SystemExit(1)
if not settings_source_path.is_file():
    print("FAIL: missing src/scene/SettingsSceneIr.cpp", file=sys.stderr)
    raise SystemExit(1)

source = source_path.read_text(encoding="utf-8")
settings_source = settings_source_path.read_text(encoding="utf-8")
block_start = source.find("auto right = new View();")
block_end = source.find("rootLayout->addView(right);", block_start)
if block_start < 0 or block_end < 0:
    print("FAIL: unable to locate the main-menu right panel", file=sys.stderr)
    raise SystemExit(1)

block = source[block_start : block_end + len("rootLayout->addView(right);")]
failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


def require_in_order(*needles: str) -> None:
    position = -1
    for needle in needles:
        next_position = block.find(needle, position + 1)
        if next_position < 0:
            failures.append(f"missing or out-of-order right-panel step: {needle}")
            return
        position = next_position


def require_settings_in_order(*needles: str) -> None:
    position = -1
    for needle in needles:
        next_position = settings_source.find(needle, position + 1)
        if next_position < 0:
            failures.append(
                f"missing or out-of-order credential invalidation step: {needle}"
            )
            return
        position = next_position


require(
    "rightContent->addView(settingsButton);" not in block,
    "Settings must not scroll with chart actions",
)
require(
    "settingsSpacer" not in block,
    "Settings footer must not rely on an intrinsic-height spacer",
)
require(
    "right->setGap(12);" in block and "right->setPadding(Edge::Bottom, 16);" in block,
    "right panel must separate and pad its fixed Settings footer",
)
require_in_order(
    "rightScroll->setContentView(rightContent);",
    "right->addView(rightScroll);",
    "right->addView(settingsButton);",
    "rootLayout->addView(right);",
)
require_settings_in_order(
    ".quiesceRemoteWork =",
    "context.pauseIrProfileServices(diagnostic)",
    ".invalidateProviderIdentity =",
    "context.scoreRepository.ClearImportedIrScores(providerId)",
    "context.replayRepository.ClearIrProviderAccountEvidence(",
    "context.bokutachiCacheStore->clearUserIds(diagnostic)",
    "context.irRankingService->invalidate(",
    "context.irAccountEvidenceRevision.fetch_add(",
    ".replaceCredential =",
    ".removeCredential =",
    ".credentialCommitted =",
    ".reactivateRemoteWork =",
    "context.activateIrProfileServices(",
)
require(
    "context.irAccountEvidenceRevision.load(" in source
    and "observedIrAccountEvidenceRevision" in source
    and "refreshIrRecordListIfNeeded()" in source
    and "reloadReplayRecordModels(true)" in source,
    "the retained Main Menu must observe provider account-evidence revisions "
    "and rebuild the Records view",
)
require(
    settings_source.count("clearUserIds(diagnostic)") == 1,
    "cached Bokutachi identity clearing must live only in remote invalidation",
)
require(
    settings_source.count("ClearImportedIrScores(providerId)") == 1
    and settings_source.count("ClearIrProviderAccountEvidence(") == 1
    and "ClearIrAccountEvidence(" not in settings_source
    and "ClearIrSubmissionReceipts(" not in settings_source
    and "ClearIrRemoteScores(" not in settings_source,
    "credential changes must clear persisted imported scores before the "
    "provider-wide account evidence",
)
require_settings_in_order(
    "if (presentation.showQueueActions)",
    "if (presentation.showRecordSync)",
    "requestUserScoreReconciliation(",
)
record_sync_requests = re.findall(
    r"requestUserScoreReconciliation\s*\(\s*kProviderId\s*\)",
    settings_source,
)
require(
    len(record_sync_requests) == 1
    and "fetchUserScoreSnapshot" not in settings_source
    and "irHttpClient" not in settings_source,
    "the Settings sync action must only request serialized service work",
)
require_settings_in_order(
    "context.irSubmissionService->reconciliationStatus(kProviderId)",
    "irSettingsModel->observeReconciliationRevision(status.revision)",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("main-menu Settings footer audit passed")
