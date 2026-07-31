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

for path in (
    settings_path,
    settings_tables_path,
    main_menu_path,
    download_support_path,
    horie_path,
    package_sources_path,
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
    in (root / "src/scene/MainMenuScene.h").read_text(encoding="utf-8"),
    "Find BMS downloads must use a dedicated incremental library task",
)
require(
    "scanner.ScanAdded" in main_menu_source,
    "downloaded-path tasks must use additions-only chart scanning",
)
require(
    "enqueueDownloadedPathIndexTask(" in main_menu_source
    and "findBmsResult.outputPath" in main_menu_source,
    "successful Find BMS downloads must index their exact committed output",
)
require(
    main_menu_source.count(
        "findBmsSelectionGenerationAtDownloadStart = chartSelectionGeneration"
    )
    >= 2,
    "automatic and candidate downloads must capture chart selection generation",
)
require(
    "downloadedTargetIdentity" in main_menu_source
    and "downloadedSelectionGeneration" in main_menu_source
    and "SelectChartMetaByHash" in main_menu_source
    and "pendingFindBmsSelectionHandoff" in main_menu_source,
    "downloaded-path parsing must resolve and publish the selected indexed chart",
)
require(
    "findBmsSelectionHandoffAllowed(" in main_menu_source
    and "AutoSelectionPreview::Load" in main_menu_source
    and "schedulePreviewLoad(meta)" in main_menu_source,
    "unchanged Find BMS selection must re-enter normal preview loading",
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
