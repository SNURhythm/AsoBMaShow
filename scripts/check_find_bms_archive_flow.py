#!/usr/bin/env python3
from __future__ import annotations

import pathlib
import sys


root = (
    pathlib.Path(sys.argv[1])
    if len(sys.argv) > 1
    else pathlib.Path(__file__).parent.parent
).resolve()
settings_path = root / "src/scene/SettingsSceneLayout.cpp"
settings_tables_path = root / "src/scene/SettingsSceneTables.cpp"
main_menu_path = root / "src/scene/MainMenuScene.cpp"
download_support_path = root / "src/bms_search/DownloadSupport.cpp"
horie_path = root / "src/bms_search/HorieYuukaDriver.cpp"
package_sources_path = root / "src/bms_search/PackageSourceDrivers.cpp"
library_operations_path = root / "src/library/ChartLibraryOperations.cpp"
library_task_types_path = root / "src/library/ChartLibraryTaskTypes.h"

for path in (
    settings_path,
    settings_tables_path,
    main_menu_path,
    download_support_path,
    horie_path,
    package_sources_path,
    library_operations_path,
    library_task_types_path,
):
    if not path.is_file():
        print(f"FAIL: missing {path.relative_to(root)}", file=sys.stderr)
        raise SystemExit(1)

settings_source = settings_path.read_text(encoding="utf-8")
settings_tables_source = settings_tables_path.read_text(encoding="utf-8")
main_menu_source = main_menu_path.read_text(encoding="utf-8")
download_support_source = download_support_path.read_text(encoding="utf-8")
horie_source = horie_path.read_text(encoding="utf-8")
package_sources_source = package_sources_path.read_text(encoding="utf-8")
library_operations_source = library_operations_path.read_text(encoding="utf-8")
library_task_types_source = library_task_types_path.read_text(encoding="utf-8")
failures: list[str] = []


def require(condition: bool, message: str) -> None:
    if not condition:
        failures.append(message)


require(
    "BmsSearchService::kSkipUnarchivingSettingLabel" in settings_source,
    "Settings must use the canonical Find BMS option label",
)
require(
    settings_source.count("findBmsSkipUnarchivingForNonSolidArchives") >= 2,
    "Settings must toggle the persisted Find BMS option",
)
require(
    main_menu_source.count("skipUnarchivingForNonSolidArchives") >= 2,
    "automatic and candidate downloads must capture the option",
)
require(
    "startFindBmsPendingArtifactResolution(" in main_menu_source
    and "BmsSearchPendingArtifactDecision::Keep" in main_menu_source
    and "BmsSearchPendingArtifactDecision::Delete" in main_menu_source,
    "Find BMS mismatch UI must expose Keep and Delete",
)
require(
    'makeModalButton("Keep Files"' in main_menu_source
    and 'makeModalButton("Delete Files"' in main_menu_source,
    "Find BMS mismatch actions must use the approved visible labels",
)
require(
    "findBmsDialogPolicy(findBmsJobRunning.load(), findBmsResult)"
    in main_menu_source,
    "Find BMS dismissal must use the tested dialog policy",
)
close_handler_start = main_menu_source.find(
    "findBmsCloseButton->setOnClickListener([this]()"
)
close_handler_end = main_menu_source.find(
    "findBmsKeepFilesButton->setOnClickListener", close_handler_start
)
close_handler = main_menu_source[close_handler_start:close_handler_end]
close_apply_index = close_handler.find("applyFindBmsUpdates();")
close_policy_index = close_handler.find("findBmsDialogPolicy(")
require(
    close_apply_index >= 0
    and close_policy_index >= 0
    and close_apply_index < close_policy_index,
    "Find BMS close action must apply queued results before dismissal policy",
)
hide_handler_start = main_menu_source.find(
    "void MainMenuScene::hideFindBmsModal()"
)
hide_handler_end = main_menu_source.find(
    "void MainMenuScene::refreshFindBmsModal()", hide_handler_start
)
hide_handler = main_menu_source[hide_handler_start:hide_handler_end]
hide_apply_index = hide_handler.find("applyFindBmsUpdates();")
hide_policy_index = hide_handler.find("findBmsDialogPolicy(")
require(
    hide_apply_index >= 0
    and hide_policy_index >= 0
    and hide_apply_index < hide_policy_index,
    "Find BMS hide path must apply queued results before dismissal policy",
)
require(
    'makeText("Use for Downloads"' in settings_source
    and '"Download folder"' in settings_source,
    "BMS Library rows must expose download-folder selection",
)
require(
    '"Not writable by Find BMS"' in settings_source,
    "ineligible Android tree rows must explain why selection is disabled",
)
require(
    "SetPrimaryStorageEntry" in settings_tables_source,
    "Settings must persist the selected Find BMS download folder",
)
require(
    "SelectPrimaryStorageEntry()" in main_menu_source,
    "Find BMS must resolve the repository-selected download folder",
)
require(
    main_menu_source.count("preferredBmsDownloadRoot()") >= 3,
    "automatic and candidate downloads must share destination resolution",
)
require(
    "IndexDownloadedPath" in main_menu_source
    and "IndexDownloadedPath"
    in library_task_types_source,
    "Find BMS downloads must use a dedicated incremental library task",
)
require(
    "scanner.ScanAddedWithResult" in library_operations_source
    and "scanResult.completed" in library_operations_source
    and "scanResult.committed" in library_operations_source,
    "downloaded-path tasks must require a completed, committed additions-only scan",
)
require(
    "findBmsIndexTaskSucceeded(" in library_operations_source
    and "Downloaded BMS target was not parsed and indexed"
    in library_operations_source,
    "validated Find BMS tasks must fail without their committed target upsert",
)
require(
    "enqueueDownloadedPathIndexTask(" in main_menu_source
    and "findBmsResult.outputPath" in main_menu_source,
    "successful Find BMS downloads must index their exact committed output",
)
apply_find_bms_start = main_menu_source.find(
    "void MainMenuScene::applyFindBmsUpdates()"
)
apply_find_bms_end = main_menu_source.find(
    "void MainMenuScene::openFindBmsResultUrl(", apply_find_bms_start
)
apply_find_bms = main_menu_source[apply_find_bms_start:apply_find_bms_end]
require(
    "findBmsResult.removedPaths" in apply_find_bms,
    "Find BMS results must pass removed package variants to indexing",
)
download_task_start = library_operations_source.find(
    "TaskRunResult ChartLibraryOperations::runDownloadedIndex("
)
download_task_end = library_operations_source.find(
    "TaskRunResult ChartLibraryOperations::runAndroidImport(",
    download_task_start,
)
download_task = library_operations_source[download_task_start:download_task_end]
removed_paths_index = download_task.find("request.downloadedRemovedPaths")
targeted_delete_index = download_task.find("DeleteChartMetaInDirectory")
incremental_scan_index = download_task.find("ScanAddedWithResult(")
require(
    removed_paths_index >= 0
    and targeted_delete_index >= 0
    and incremental_scan_index >= 0
    and removed_paths_index <= targeted_delete_index < incremental_scan_index,
    "removed Find BMS variants must be reconciled before incremental indexing",
)
android_import_start = library_operations_source.find(
    "TaskRunResult ChartLibraryOperations::runAndroidImport("
)
android_import_end = library_operations_source.find(
    "const char *ChartLibraryOperations::progressStageText(",
    android_import_start,
)
android_import = library_operations_source[
    android_import_start:android_import_end
]
require(
    "scanner.ScanWithResult" in android_import
    and "scanResult.completed" in android_import,
    "Android imports must retain their source until a completed "
    "post-extraction scan",
)
require(
    main_menu_source.count(
        "findBmsSelectionGenerationAtDownloadStart = chartSelectionGeneration"
    )
    >= 2,
    "automatic and candidate downloads must capture chart selection generation",
)
require(
    "request.downloadedTargetIdentity" in library_operations_source
    and "request.downloadedSelectionGeneration" in library_operations_source
    and "SelectChartMetaByHash" in library_operations_source
    and "scanResult.upsertedChartPaths" in library_operations_source
    and "pendingFindBmsSelectionHandoff" in main_menu_source,
    "downloaded-path parsing must publish only a chart upserted by that scan",
)
require(
    "findBmsSelectionHandoffAllowed(" in main_menu_source
    and "AutoSelectionPreview::Load" in main_menu_source
    and "schedulePreviewLoad(meta)" in main_menu_source,
    "unchanged Find BMS selection must re-enter normal preview loading",
)
apply_updates_start = main_menu_source.find(
    "void MainMenuScene::applyPendingUiUpdates()"
)
apply_updates_end = main_menu_source.find(
    "void MainMenuScene::selectChartByPathAfterReload(", apply_updates_start
)
apply_updates = main_menu_source[apply_updates_start:apply_updates_end]
handoff_snapshot_index = apply_updates.find("findBmsSelectionBeforeReload")
reload_index = apply_updates.find("reloadChartList(true)")
handoff_validation_index = apply_updates.find(
    "*findBmsSelectionBeforeReload", reload_index
)
require(
    handoff_snapshot_index >= 0
    and reload_index >= 0
    and handoff_snapshot_index < reload_index
    and handoff_validation_index > reload_index,
    "Find BMS handoff must retain the unavailable selection before reload",
)
select_path_start = main_menu_source.find(
    "void MainMenuScene::selectChartByPathAfterReload("
)
select_path_end = main_menu_source.find(
    "void MainMenuScene::selectFolder(", select_path_start
)
select_path = main_menu_source[select_path_start:select_path_end]
load_fallback_index = select_path.find("AutoSelectionPreview::Load")
exact_lookup_index = select_path.find("SelectChartMetaByPaths")
handoff_record_index = select_path.find("findBmsUnfilteredHandoffRecord")
all_songs_fallback_index = select_path.find(
    "activeFolder.type != LibraryFolderItem::Type::AllSongs"
)
require(
    load_fallback_index >= 0
    and exact_lookup_index > load_fallback_index
    and handoff_record_index > exact_lookup_index
    and all_songs_fallback_index > handoff_record_index,
    "Find BMS preview handoff must exact-load charts hidden by active filters",
)
require(
    "searchText.clear()" not in select_path
    and "chartRecordFilters =" not in select_path,
    "Find BMS preview handoff must preserve active search and chart filters",
)
require(
    "AutoSelectionPreview::Suppress" in main_menu_source
    and "suppressPreviewForChartPath = record.meta.BmsPath" in main_menu_source,
    "unzip auto-selection must keep suppressing preview",
)
require(
    "startLibraryRefresh(findBmsDownloadRoot)" not in main_menu_source
    and "additionalFolderToScan" not in main_menu_source,
    "Find BMS indexing must not route through a full-library refresh",
)
require(
    "candidate.name, candidate.id" in horie_source,
    "Horie downloads must use the stable provider file ID for storage",
)
require(
    "const std::string &storageIdentity" in download_support_source
    and "!storageIdentity.empty()" in download_support_source
    and "!archiveKey.empty()" in download_support_source
    and "result.downloadUrl" in download_support_source,
    "Find BMS storage identity must fall back through chart hash and URL",
)
require(
    "lookup.candidate->archiveName" in package_sources_source
    and "archiveKey.empty() ? md5Hash : archiveKey" in package_sources_source,
    "package-source downloads must provide their chart hash as storage identity",
)

if failures:
    for failure in failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    raise SystemExit(1)

print("Find BMS archive-flow audit passed")
