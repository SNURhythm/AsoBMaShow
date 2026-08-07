#!/usr/bin/env python3
"""Source contracts for the native Gameplay Skins settings tab."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class GameplaySkinSettingsUiContracts(unittest.TestCase):
    def read(self, relative: str) -> str:
        return (ROOT / relative).read_text()

    def test_gameplay_skins_tab_is_owned_by_the_enabled_gate(self) -> None:
        header = self.read("src/scene/SettingsScene.h")
        layout = self.read("src/scene/SettingsSceneLayout.cpp")
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")
        unavailable = self.read("src/scene/SettingsSceneSkinsUnavailable.cpp")

        self.assertIn("GameplaySkins", header)
        self.assertIn('"Gameplay Skins"', layout)
        self.assertIn("#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS", enabled)
        self.assertIn("#if !ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS", unavailable)
        self.assertIn("luaGameplaySkinsAvailable", unavailable)
        self.assertNotRegex(unavailable, r"GameplaySkinSettingsController|SkinPackage")

    def test_tab_transition_starts_rescan_and_update_polls_controller(self) -> None:
        layout = self.read("src/scene/SettingsSceneLayout.cpp")
        scene = self.read("src/scene/SettingsScene.cpp")
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        self.assertRegex(
            layout,
            r"activeTab = tab;[\s\S]{0,480}SettingsTab::GameplaySkins[\s\S]{0,240}requestRescan",
        )
        self.assertIn("updateGameplaySkinSettingsController();", scene)
        self.assertIn("gameplaySkinSettingsController->poll()", enabled)

    def test_enabled_tab_uses_app_owned_services_and_a_fresh_client(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        for required in (
            "context.skinPackageOperationService",
            "context.skinDiagnosticHistory",
            "context.skinCommitCoordinator",
            "context.gameplaySkinLifecycle",
            "createClient()",
            "profileChanged(",
            "beginArchiveImport",
            "beginFolderImport",
        ):
            self.assertIn(required, enabled)

    def test_unready_tab_exposes_the_sanitized_startup_diagnostic(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")
        context = self.read("src/context.h")
        self.assertIn("context.skinRecoveryResult", enabled)
        self.assertIn("skinRecoveryResult->diagnostics", enabled)
        self.assertIn("diagnosticPresentation", enabled)
        self.assertIn('"Retry Startup"', enabled)
        self.assertIn("context.retryGameplaySkinServices()", enabled)
        self.assertIn("bool retryGameplaySkinServices() noexcept", context)
        self.assertIn("unwindGameplaySkinServicesAfterStartupFailure();", context)

    def test_core_operator_labels_and_files_location_are_present(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")
        for label in (
            "Use Beatoraja Gameplay Skin",
            "Import Archive",
            "Add Folder",
            "Confirm Replace",
            "Rescan",
            "Revalidate",
            "Select",
            "Remove",
            "Reset Layout",
            "On My iPad/AsoBMaShow/Skins",
            "Fit",
            "Stretch",
            "Custom",
        ):
            self.assertIn(label, enabled)

    def test_enabled_controller_source_is_gated_into_main(self) -> None:
        cmake = self.read("src/scene/CMakeLists.txt")
        self.assertRegex(
            cmake,
            r"if\s*\(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS\)"
            r"[\s\S]*GameplaySkinSettingsController\.cpp[\s\S]*endif\s*\(\)",
        )

    def test_controller_results_are_consumed_and_rejections_are_visible(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")
        layout = self.read("src/scene/SettingsSceneLayout.cpp")

        self.assertIn("handleGameplaySkinActionResult", enabled)
        self.assertIn("!result.accepted", enabled)
        self.assertIn("gameplaySkinUiMessage = result.message", enabled)
        self.assertRegex(
            layout,
            r"handleGameplaySkinActionResult\([\s\S]{0,160}requestRescan\(\)",
        )
        action_methods = (
            "beginArchiveImport",
            "beginFolderImport",
            "setSuggestedPackageName",
            "confirmPreparedImport",
            "requestRescan",
            "requestRevalidation",
            "select",
            "setCompatibilityEnabled",
            "setOption",
            "setFileChoice",
            "setOffset",
            "setViewport",
            "requestRemoval",
            "resetLayout",
        )
        for method in action_methods:
            for match in re.finditer(rf"gameplaySkinSettingsController->{method}\(", enabled):
                prefix = enabled[max(0, match.start() - 120) : match.start()]
                self.assertIn("handleGameplaySkinActionResult(", prefix, method)

    def test_tab_uses_the_pure_presentation_key(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        self.assertIn('#include "GameplaySkinSettingsPresentation.h"', enabled)
        self.assertIn("gameplaySkinSettingsPresentationKey(snapshot)", enabled)

    def test_replacement_and_removal_are_two_step_confirmations(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")
        header = self.read("src/scene/SettingsScene.h")
        for state in (
            "gameplaySkinReplaceConfirmationArmed",
            "gameplaySkinRemovalConfirmationKey",
        ):
            self.assertIn(state, header)
            self.assertIn(state, enabled)
        for label in ("Replace Existing", "Confirm Replace", "Remove", "Confirm Remove"):
            self.assertIn(label, enabled)

    def test_metadata_configuration_and_numeric_custom_viewport_are_editable(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")
        for metadata in (
            "row.metadata.options",
            "row.metadata.files",
            "row.metadata.offsets",
        ):
            self.assertIn(metadata, enabled)
        for action in ("setOption", "setFileChoice", "setOffset"):
            self.assertIn(action, enabled)
        for label in ("Custom X", "Custom Y", "Custom Width", "Custom Height"):
            self.assertIn(label, enabled)
        self.assertIn("sanitizeViewportComponent", enabled)
        self.assertIn("SkinProfileSettingsPolicy::minCustomScale", enabled)
        self.assertIn("SkinProfileSettingsPolicy::maxCustomTranslation", enabled)

    def test_catalog_choices_and_offsets_use_semantic_controls(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        self.assertIn("option.choices.size() >= 3", enabled)
        self.assertIn("file.choices.size() >= 3", enabled)
        self.assertIn("std::vector<DropdownView::Option>", enabled)
        self.assertIn("sanitizeOffsetComponent", enabled)
        self.assertIn(
            "makeTextInput(metrics, metrics.compact ? 96 : 112)", enabled
        )
        self.assertNotIn("const auto next = option.choices.begin()", enabled)
        self.assertNotIn("const auto next = file.choices.begin()", enabled)

    def test_action_controls_use_the_pure_availability_projection(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        self.assertIn("gameplaySkinSettingsActionAvailability(snapshot)", enabled)
        self.assertIn("actionAvailability.ordinaryActions", enabled)
        self.assertIn("actionAvailability.canCancel", enabled)

    def test_catalog_action_controls_grow_to_their_labels(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        self.assertIn("labelView->textureWidth()", enabled)
        self.assertIn("contentWidth + horizontalContentPadding", enabled)
        self.assertIn("std::max(minimumWidth", enabled)

    def test_name_input_and_install_button_use_projected_availability(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        self.assertIn("snapshot.preparedName->validationError", enabled)
        self.assertIn("actionAvailability.canEditPreparedName", enabled)
        self.assertIn("actionAvailability.canInstallPrepared", enabled)

    def test_viewport_controls_use_copy_preserving_helpers(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        self.assertIn("Custom Base: Fit", enabled)
        self.assertIn("Custom Base: Stretch", enabled)
        self.assertIn("gameplaySkinViewportWithMode(", enabled)
        self.assertIn("gameplaySkinViewportWithCustomBase(", enabled)

    def test_metadata_diagnostics_and_history_are_visible_and_versioned(self) -> None:
        enabled = self.read("src/scene/SettingsSceneSkins.cpp")

        for visible_field in (
            "Selected",
            "Revision: ",
            "Configuration: ",
            "row.metadata.categories",
            "diagnostic.virtualPath",
            "diagnostic.source->line",
            "snapshot.history",
            "record.recordSerial",
        ):
            self.assertIn(visible_field, enabled)
        self.assertRegex(
            enabled,
            r"for \(const auto &record : snapshot\.history\)[\s\S]*"
            r"record\.recordSerial[\s\S]*record\.diagnostic",
        )


if __name__ == "__main__":
    unittest.main()
