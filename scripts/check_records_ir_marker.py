from pathlib import Path
import sys

root = (Path(sys.argv[1]) if len(sys.argv) > 1
        else Path(__file__).resolve().parents[1])
repository = (root / "src/repositories/ReplayRepositoryRecords.cpp").read_text()
header = (root / "src/scene/MainMenuScene.h").read_text()
menu = (root / "src/scene/MainMenuScene.cpp").read_text()
view = (root / "src/view/ReplaySummaryListView.h").read_text()
observer_start = menu.index("void MainMenuScene::observeReplayIrServiceRevisions()")
observer_end = menu.index("\nvoid MainMenuScene::startReplayIrUpload", observer_start)
observer = menu[observer_start:observer_end]
refresh_start = menu.index("void MainMenuScene::refreshReplayIrMarker(")
refresh_end = menu.index("\nvoid MainMenuScene::startReplayResultRecall", refresh_start)
refresh = menu[refresh_start:refresh_end]
filter_start = menu.index("void MainMenuScene::applyReplayRecordFilters(")
filter_end = menu.index("\nvoid MainMenuScene::setReplayClearFilter", filter_start)
filter_restore = menu[filter_start:filter_end]
required = {
    "repository": [
        "requestedIrOutboxState",
        "irProviderId",
        "irServerOrigin",
        "hasIrReceipt",
        "receiptRemoteScoreId",
    ],
    "header": [
        "std::unordered_map<std::string, std::uint64_t> "
        "replayIrObservedRevisions",
    ],
    "menu": [
        "observeReplayIrServiceRevisions()",
        "ir::normalizeServerOrigin",
    ],
    "observer": [
        "replayModalRoot->getVisible()",
        "replayListContent->getVisible()",
        "context.irSubmissionService->status(",
        "replayIrObservedRevisions",
        "status.revision",
        "recordActivityFor(status.activeRequest)",
        "refreshReplayIrMarker(summary.id",
    ],
    "refresh": [
        "activeReplayIrServerOrigin()",
        "ListReplays(",
        "previousScrollOffset",
        "preferredReplayId",
        "applyReplayRecordFilters(preferredReplayId)",
    ],
    "filter_restore": [
        "preferredReplayId",
        "replayListView->restoreSelection",
    ],
    "view": [
        "bindingForIrRecordState",
        "ui_icons::kFontAwesomeSolidPath",
        "ui_icons::textForCodepoint",
        "irBadgeActionable",
    ],
}
texts = {
    "repository": repository,
    "header": header,
    "menu": menu,
    "observer": observer,
    "refresh": refresh,
    "filter_restore": filter_restore,
    "view": view,
}
missing = []
for group, tokens in required.items():
    missing.extend(group + ":" + token
                   for token in tokens if token not in texts[group])
if missing:
    raise SystemExit("missing Records IR marker contracts: " +
                     ", ".join(missing))

forbidden_menu_tokens = [
    "IrCredentialStore",
    "credentials.apiKeys",
    "irCredentialsPath",
]
credential_lookups = [token for token in forbidden_menu_tokens if token in menu]
if credential_lookups:
    raise SystemExit("MainMenuScene must not look up IR API keys: " +
                     ", ".join(credential_lookups))
