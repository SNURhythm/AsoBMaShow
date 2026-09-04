from pathlib import Path
import sys

root = (Path(sys.argv[1]) if len(sys.argv) > 1
        else Path(__file__).resolve().parents[1])
repository = (root / "src/repositories/ReplayRepositoryModernResults.cpp").read_text()
header = (root / "src/scene/MainMenuScene.h").read_text()
menu = (root / "src/scene/MainMenuScene.cpp").read_text()
modal = (root / "src/scene/ReplayRecordsModal.cpp").read_text()
result_view = (root / "src/view/ResultRecordListView.h").read_text()

observer_start = menu.index("void MainMenuScene::observeReplayIrServiceRevisions()")
observer_end = menu.index("\nvoid MainMenuScene::startModernReplayIrUpload", observer_start)
observer = menu[observer_start:observer_end]
loader_start = menu.index("MainMenuScene::loadRecordsForModal(")
loader_end = menu.index("\nvoid MainMenuScene::shareReplayFile(", loader_start)
loader = menu[loader_start:loader_end]
filter_start = modal.index("void ReplayRecordsModal::applyFilters(")
filter_end = modal.index("\nvoid ReplayRecordsModal::refreshActions", filter_start)
filter_restore = modal[filter_start:filter_end]
reload_start = modal.index("void ReplayRecordsModal::reloadRecords(")
reload_end = modal.index("\nvoid ReplayRecordsModal::setExportInProgress", reload_start)
modal_reload = modal[reload_start:reload_end]

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
        "std::unique_ptr<ReplayRecordsModal> recordsModal_",
    ],
    "menu": [
        "recordActivityFor(ir::IrActiveRequestKind activeRequest)",
        "observeReplayIrServiceRevisions()",
        "activeReplayIrServerOrigin()",
    ],
    "observer": [
        "recordsModal_->isVisible()",
        "context.irSubmissionService->status(",
        "replayIrObservedRevisions",
        "status.revision",
        "recordsModal_->reloadRecords(true)",
    ],
    "loader": [
        "ListIrUploadRecordsForChart(",
        "irRecordsByAttempt",
        "resolvedState(",
        "recordActivityFor(serviceStatus.activeRequest)",
    ],
    "modal_reload": [
        "previousScrollOffset",
        "preferredStableKey",
        "applyFilters(std::move(preferredStableKey))",
    ],
    "filter_restore": [
        "preferredStableKey",
        "visibleRecords_",
        "list_->restoreSelection",
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
    "loader": loader,
    "modal_reload": modal_reload,
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
