from pathlib import Path
import sys

root = (Path(sys.argv[1]) if len(sys.argv) > 1
        else Path(__file__).resolve().parents[1])
repository = (root / "src/repositories/ReplayRepositoryModernResults.cpp").read_text()
header = (root / "src/scene/MainMenuScene.h").read_text()
menu = (root / "src/scene/MainMenuScene.cpp").read_text()
result_view = (root / "src/view/ResultRecordListView.h").read_text()

observer_start = menu.index("void MainMenuScene::observeReplayIrServiceRevisions()")
observer_end = menu.index("\nvoid MainMenuScene::startModernReplayIrUpload", observer_start)
observer = menu[observer_start:observer_end]
reload_start = menu.index("void MainMenuScene::reloadReplayRecordModels(")
reload_end = menu.index("\nvoid MainMenuScene::showReplayListModal", reload_start)
reload = menu[reload_start:reload_end]
filter_start = menu.index("void MainMenuScene::applyReplayRecordFilters(")
filter_end = menu.index("\nvoid MainMenuScene::setReplayClearFilter", filter_start)
filter_restore = menu[filter_start:filter_end]

required = {
    "repository": [
        "ReplayRepository::ListIrUploadRecordsForChart(",
        "loadIrSourcesOnConnection",
        "projectIrUploadRecords",
        "BEGIN TRANSACTION",
    ],
    "header": [
        "std::unordered_map<std::string, std::uint64_t> "
        "replayIrObservedRevisions",
        "ResultRecordListView *replayListView",
    ],
    "menu": [
        "recordActivityFor(ir::IrActiveRequestKind activeRequest)",
        "observeReplayIrServiceRevisions()",
        "activeReplayIrServerOrigin()",
    ],
    "observer": [
        "replayModalRoot->getVisible()",
        "replayListContent->getVisible()",
        "context.irSubmissionService->status(",
        "replayIrObservedRevisions",
        "status.revision",
        "reloadReplayRecordModels(true)",
    ],
    "reload": [
        "ListIrUploadRecordsForChart(",
        "irRecordsByAttempt",
        "resolvedState(",
        "recordActivityFor(serviceStatus.activeRequest)",
        "previousScrollOffset",
        "preferredStableKey",
        "applyReplayRecordFilters(std::move(preferredStableKey))",
    ],
    "filter_restore": [
        "preferredStableKey",
        "visibleResultRecordSummaries",
        "replayListView->restoreSelection",
    ],
    "result_view": [
        "summary.isRemote() ? ir::IrRecordState::Uploaded",
        "summary.isLocal() && summary.capabilities.irUpload",
        "boundStableKey_ = summary.stableKey()",
        "irBadgeCallbackStableKey_.reset()",
        "irBadge->setOnClickListener({})",
        "ui_icons::textForCodepoint",
    ],
}
texts = {
    "repository": repository,
    "header": header,
    "menu": menu,
    "observer": observer,
    "reload": reload,
    "filter_restore": filter_restore,
    "result_view": result_view,
}
missing = []
for group, tokens in required.items():
    missing.extend(group + ":" + token
                   for token in tokens if token not in texts[group])
if missing:
    raise SystemExit("missing Records IR marker contracts: " +
                     ", ".join(missing))

forbidden_reload_tokens = [
    "ListReplays(",
    "LoadReplayResult(",
    "resolveReplayIrRecordState(",
    "ListIrUploadRecords(",
    "nextBeforeModernChartResultId",
]
legacy_reads = [token for token in forbidden_reload_tokens if token in reload]
if legacy_reads:
    raise SystemExit("Records IR markers must not reconstruct legacy replay state: " +
                     ", ".join(legacy_reads))

forbidden_menu_tokens = [
    "IrCredentialStore",
    "credentials.apiKeys",
    "irCredentialsPath",
]
credential_lookups = [token for token in forbidden_menu_tokens if token in menu]
if credential_lookups:
    raise SystemExit("MainMenuScene must not look up IR API keys: " +
                     ", ".join(credential_lookups))
