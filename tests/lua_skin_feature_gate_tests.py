#!/usr/bin/env python3

import os
import pathlib
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
FEATURE_HEADER = ROOT / "src/skin/LuaGameplaySkinFeature.h"
ENABLED_SOURCE = ROOT / "src/skin/LuaGameplaySkinFeatureEnabled.cpp"
UNAVAILABLE_SOURCE = ROOT / "src/skin/LuaGameplaySkinFeatureUnavailable.cpp"
ROOT_CMAKE = ROOT / "CMakeLists.txt"
SKIN_CMAKE = ROOT / "src/skin/CMakeLists.txt"
RUNTIME_SOURCE = ROOT / "src/skin/beatoraja/LuaSkinRuntime.cpp"
HOST_SOURCE = ROOT / "src/skin/beatoraja/LuaSkinHostModules.cpp"
DECODER_SOURCE = ROOT / "src/skin/beatoraja/LuaSkinTableDecoder.cpp"
BINDING_DECODER_SOURCE = ROOT / "src/skin/beatoraja/LuaSkinBindingDecoder.cpp"
VALIDATOR_SOURCE = ROOT / "src/skin/beatoraja/SkinModelValidator.cpp"
NUMERIC_GLYPH_ATLAS_SOURCE = ROOT / "src/skin/beatoraja/NumericGlyphAtlas.cpp"
GAUGE_NODE_EXPANSION_SOURCE = (
    ROOT / "src/skin/beatoraja/SkinGaugeNodeExpansion.cpp"
)
JUDGE_NORMALIZATION_SOURCE = ROOT / "src/skin/beatoraja/SkinJudgeNormalization.cpp"
NOTE_NORMALIZATION_SOURCE = ROOT / "src/skin/beatoraja/SkinNoteNormalization.cpp"
NOTE_LANE_GEOMETRY_NORMALIZATION_SOURCE = (
    ROOT / "src/skin/beatoraja/SkinNoteLaneGeometryNormalization.cpp"
)
NOTE_LINE_NORMALIZATION_SOURCE = (
    ROOT / "src/skin/beatoraja/SkinNoteLineNormalization.cpp"
)
TEXT_GRAPH_NORMALIZATION_SOURCE = (
    ROOT / "src/skin/beatoraja/SkinTextGraphNormalization.cpp"
)
COVER_NORMALIZATION_SOURCE = ROOT / "src/skin/beatoraja/SkinCoverNormalization.cpp"
OBJECT_RESOLUTION_PRECEDENCE_SOURCE = (
    ROOT / "src/skin/beatoraja/SkinObjectResolutionPrecedence.cpp"
)
RENDERER_SOURCE = ROOT / "src/skin/beatoraja/Skin2DRenderer.cpp"
GAMEPLAY_SKIN_LIFECYCLE_SOURCE = ROOT / "src/skin/GameplaySkinLifecycle.cpp"
APPLICATION_CONTEXT_HEADER = ROOT / "src/context.h"
MAIN_SOURCE = ROOT / "src/main.cpp"
SCENE_MANAGER_SOURCE = ROOT / "src/scene/SceneManager.cpp"
GAMEPLAY_SCENE_SOURCE = ROOT / "src/scene/play/GamePlayScene.cpp"
PROFILE_SETTINGS_CONTROLLER_HEADER = ROOT / "src/scene/ProfileSettingsController.h"
PROFILE_SETTINGS_CONTROLLER_SOURCE = ROOT / "src/scene/ProfileSettingsController.cpp"
PROFILE_SETTINGS_CONTROLLER_CONTEXT_SOURCE = (
    ROOT / "src/scene/ProfileSettingsControllerContext.cpp"
)


def braced_body(source, marker):
    marker_offset = source.index(marker)
    opening = source.index("{", marker_offset + len(marker))
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : offset]
    raise AssertionError(f"unterminated body after {marker!r}")


def assert_feature_guarded(test_case, source, tokens):
    enabled_depth = 0
    for number, line in enumerate(source.splitlines(), start=1):
        directive = line.strip()
        if directive.startswith("#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS"):
            enabled_depth += 1
        elif directive.startswith("#endif") and enabled_depth:
            enabled_depth -= 1
        for token in tokens:
            if token in line:
                test_case.assertGreater(
                    enabled_depth,
                    0,
                    f"{token} escapes the disabled-build guard on line {number}",
                )


class LuaSkinFeatureGateTests(unittest.TestCase):
    def compile_and_run_query(self, definitions=()):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        self.assertIsNotNone(compiler, "a C++ compiler is required")

        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = pathlib.Path(temporary_directory)
            main = temporary / "main.cpp"
            executable = temporary / "feature-query"
            main.write_text(
                textwrap.dedent(
                    """\
                    #include "skin/LuaGameplaySkinFeature.h"
                    #include <iostream>

                    int main() {
                      std::cout << skin::luaGameplaySkinsAvailable();
                    }
                    """
                ),
                encoding="utf-8",
            )
            command = [
                compiler,
                "-std=c++23",
                *definitions,
                "-I",
                str(ROOT / "src"),
                str(main),
                str(ENABLED_SOURCE),
                str(UNAVAILABLE_SOURCE),
                "-o",
                str(executable),
            ]
            subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
            return subprocess.run(
                [str(executable)],
                cwd=ROOT,
                check=True,
                capture_output=True,
                text=True,
            ).stdout

    def test_desktop_and_ios_style_compiles_enable_the_feature_by_default(self):
        self.assertEqual("1", self.compile_and_run_query())

    def test_android_style_compile_disables_the_feature_by_default(self):
        self.assertEqual("0", self.compile_and_run_query(("-D__ANDROID__=1",)))

    def test_build_definition_overrides_the_platform_default(self):
        self.assertEqual(
            "0",
            self.compile_and_run_query(
                ("-DASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=0",)
            ),
        )
        self.assertEqual(
            "1",
            self.compile_and_run_query(
                (
                    "-D__ANDROID__=1",
                    "-DASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=1",
                )
            ),
        )

    def test_cmake_defaults_android_off_and_other_platforms_on(self):
        source = ROOT_CMAKE.read_text(encoding="utf-8")
        android_default = source.index(
            "set(ASOBMASHOW_LUA_GAMEPLAY_SKINS_DEFAULT OFF)"
        )
        other_default = source.index(
            "set(ASOBMASHOW_LUA_GAMEPLAY_SKINS_DEFAULT ON)"
        )
        option = source.index("option(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS")
        definition = source.index("ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=$<BOOL:")
        self.assertLess(android_default, option)
        self.assertLess(other_default, option)
        self.assertLess(option, definition)

    def test_cmake_selects_exactly_one_complementary_translation_unit(self):
        source = SKIN_CMAKE.read_text(encoding="utf-8")
        conditional_start = source.index(
            "if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)"
        )
        alternative = source.index("else()", conditional_start)
        conditional_end = source.index("endif()", alternative)
        enabled = source[conditional_start:alternative]
        unavailable = source[alternative:conditional_end]
        self.assertIn("LuaGameplaySkinFeatureEnabled.cpp", enabled)
        self.assertNotIn("LuaGameplaySkinFeatureUnavailable.cpp", enabled)
        self.assertIn("LuaGameplaySkinFeatureUnavailable.cpp", unavailable)
        self.assertNotIn("LuaGameplaySkinFeatureEnabled.cpp", unavailable)

    def test_enabled_runtime_sources_are_selected_only_behind_the_gate(self):
        source = SKIN_CMAKE.read_text(encoding="utf-8")
        conditional = source.index("if(ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS)")
        alternative = source.index("else()", conditional)
        conditional_end = source.index("endif()", alternative)
        enabled_block = source[conditional:alternative]
        unavailable_block = source[alternative:conditional_end]
        self.assertIn("beatoraja/LuaSkinRuntime.cpp", enabled_block)
        self.assertIn("beatoraja/LuaSkinHostModules.cpp", enabled_block)
        self.assertIn("beatoraja/LuaSkinTableDecoder.cpp", enabled_block)
        self.assertIn("beatoraja/LuaSkinBindingDecoder.cpp", enabled_block)
        self.assertIn("beatoraja/NumericGlyphAtlas.cpp", enabled_block)
        self.assertIn("beatoraja/SkinModelValidator.cpp", enabled_block)
        self.assertIn("beatoraja/SkinGaugeNodeExpansion.cpp", enabled_block)
        self.assertIn("beatoraja/SkinJudgeNormalization.cpp", enabled_block)
        self.assertIn("beatoraja/SkinNoteNormalization.cpp", enabled_block)
        self.assertIn(
            "beatoraja/SkinNoteLaneGeometryNormalization.cpp", enabled_block
        )
        self.assertIn("beatoraja/SkinNoteLineNormalization.cpp", enabled_block)
        self.assertIn("beatoraja/SkinTextGraphNormalization.cpp", enabled_block)
        self.assertIn("beatoraja/SkinCoverNormalization.cpp", enabled_block)
        self.assertIn("beatoraja/SkinObjectResolutionPrecedence.cpp", enabled_block)
        self.assertIn("beatoraja/Skin2DRenderer.cpp", enabled_block)
        self.assertIn("GameplaySkinLifecycle.cpp", enabled_block)
        self.assertNotIn("GameplaySkinLifecycle.cpp", unavailable_block)

    def test_xcode_discovered_runtime_implementations_have_source_guards(self):
        for path in (
             RUNTIME_SOURCE,
             HOST_SOURCE,
             DECODER_SOURCE,
             BINDING_DECODER_SOURCE,
             NUMERIC_GLYPH_ATLAS_SOURCE,
             VALIDATOR_SOURCE,
            GAUGE_NODE_EXPANSION_SOURCE,
            JUDGE_NORMALIZATION_SOURCE,
            NOTE_NORMALIZATION_SOURCE,
            NOTE_LANE_GEOMETRY_NORMALIZATION_SOURCE,
            NOTE_LINE_NORMALIZATION_SOURCE,
            TEXT_GRAPH_NORMALIZATION_SOURCE,
            COVER_NORMALIZATION_SOURCE,
            OBJECT_RESOLUTION_PRECEDENCE_SOURCE,
            RENDERER_SOURCE,
        ):
            source = path.read_text(encoding="utf-8")
            self.assertIn('#include "../LuaGameplaySkinFeature.h"', source)
            self.assertIn("#if ASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS", source)
            self.assertIn("#endif", source)

    def test_disabled_runtime_sources_compile_without_lua_headers_or_libraries(self):
        compiler = os.environ.get("CXX") or shutil.which("c++")
        self.assertIsNotNone(compiler, "a C++ compiler is required")
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = pathlib.Path(temporary_directory)
            for source in (
                 RUNTIME_SOURCE,
                 HOST_SOURCE,
                 DECODER_SOURCE,
                 BINDING_DECODER_SOURCE,
                 NUMERIC_GLYPH_ATLAS_SOURCE,
                 VALIDATOR_SOURCE,
                GAUGE_NODE_EXPANSION_SOURCE,
                JUDGE_NORMALIZATION_SOURCE,
                NOTE_NORMALIZATION_SOURCE,
                NOTE_LANE_GEOMETRY_NORMALIZATION_SOURCE,
                NOTE_LINE_NORMALIZATION_SOURCE,
                TEXT_GRAPH_NORMALIZATION_SOURCE,
                COVER_NORMALIZATION_SOURCE,
                OBJECT_RESOLUTION_PRECEDENCE_SOURCE,
                RENDERER_SOURCE,
            ):
                subprocess.run(
                    [
                        compiler,
                        "-std=c++23",
                        "-D__ANDROID__=1",
                        "-DASOBMASHOW_ENABLE_LUA_GAMEPLAY_SKINS=0",
                        "-I",
                        str(ROOT / "src"),
                        "-c",
                        str(source),
                        "-o",
                        str(temporary / (source.stem + ".o")),
                    ],
                    cwd=ROOT,
                    check=True,
                    capture_output=True,
                    text=True,
                )

    def test_application_loop_polls_skin_commits_first_and_exactly_once(self):
        source = MAIN_SOURCE.read_text(encoding="utf-8")
        loop = braced_body(source, "while (!context.quitFlag)")
        self.assertTrue(
            loop.lstrip().startswith("context.pollGameplaySkinCommits();"),
            "commit polling must precede every application-loop early continue",
        )
        self.assertEqual(1, loop.count("context.pollGameplaySkinCommits();"))
        context = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        self.assertIn("void pollGameplaySkinCommits() noexcept", context)

    def test_application_context_owns_enabled_skin_services_in_dependency_order(self):
        source = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        initialization = braced_body(source, "void initializeGameplaySkinServices()")
        compact = " ".join(initialization.split())
        ordered_initialization = (
            "skinStorageRoots = skin::defaultSkinStorageRoots();",
            "skinAliasDetector = skin::createPlatformSkinAliasDetector();",
            "skinPackageCatalog = std::make_unique<skin::SkinPackageCatalog>",
            "skinPackageStore = std::make_unique<skin::SkinPackageStore>",
            "skinRecoveryResult = skinPackageStore->recoverBeforeServiceStart();",
            "skinRecoveryResult->disposition == skin::SkinRecoveryDisposition::Recovered",
            "skinResourcePreparationService = std::make_unique<skin::SkinResourcePreparationService>();",
            "gameplaySkinValidator = std::make_unique<skin::GameplaySkinValidator>",
            "skinPackageOperationService = std::make_unique<skin::SkinPackageOperationService>",
            "skinDiagnosticHistory = std::make_unique<skin::SkinDiagnosticHistory>",
            "skinConfigurationWriteQueue = std::make_unique<skin::SkinConfigurationWriteQueue>();",
            "skinCommitCoordinator = std::make_unique<skin::SkinCommitCoordinator>",
        )
        positions = [compact.index(token) for token in ordered_initialization]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(
            1,
            initialization.count(
                "skinPackageStore->recoverBeforeServiceStart();"
            ),
        )
        self.assertNotIn(
            "SkinRecoveryDisposition::AlreadyRecovered",
            initialization,
            "a repeated recovery must never admit a second service graph",
        )

        constructor = braced_body(source, "ApplicationContext()")
        owner = constructor.index("profileSettingsPersistenceCoordinator =")
        services = constructor.index("initializeGameplaySkinServices();")
        self.assertLess(owner, services)

    def test_application_context_wires_one_lifecycle_after_recovery(self):
        source = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        initialization = braced_body(source, "void initializeGameplaySkinServices()")
        compact = " ".join(initialization.split())
        ordered = (
            "skinPackageStore->recoverBeforeServiceStart();",
            "skinPackageOperationService =",
            "skinCommitCoordinator =",
            "gameplaySkinLifecycle = std::make_unique<skin::GameplaySkinLifecycle>",
            "gameplaySkinLifecycle->startAfterProfileInitialization",
            "acquireGameplaySkinForNextChart = [this]",
        )
        positions = [compact.index(token) for token in ordered]
        self.assertEqual(positions, sorted(positions))
        self.assertEqual(
            1, initialization.count("skinCommitCoordinator->createClient()")
        )
        client = initialization.index("const auto lifecycleClientId =")
        created = initialization.index(
            "skinCommitCoordinator->createClient()", client
        )
        checked = initialization.index("if (lifecycleClientId == 0)", created)
        constructed = initialization.index(
            "gameplaySkinLifecycle = std::make_unique<skin::GameplaySkinLifecycle>",
            checked,
        )
        self.assertLess(client, created)
        self.assertLess(created, checked)
        self.assertLess(checked, constructed)

        polling = braced_body(source, "void pollGameplaySkinCommits() noexcept")
        self.assertLess(
            polling.index("skinCommitCoordinator->poll();"),
            polling.index("gameplaySkinLifecycle->poll();"),
        )

        constructor = braced_body(source, "ApplicationContext()")
        committed_tail = braced_body(constructor, ".activeProfileCommitted =")
        self.assertTrue(committed_tail.lstrip().startswith("try {"))
        caught = committed_tail.index("} catch (...) {")
        self.assertLess(
            committed_tail.index("bindCommittedActiveProfile"),
            committed_tail.index("gameplaySkinLifecycle->profileChanged"),
        )
        for token in (
            "makeSkinProfileId",
            "bindCommittedActiveProfile",
            "gameplaySkinLifecycle->profileChanged",
        ):
            self.assertLess(committed_tail.index(token), caught)

        teardown = braced_body(source, "~ApplicationContext()")
        self.assertLess(
            teardown.index("shutdownGameplaySkinLifecycle();"),
            teardown.index("shutdownGameplaySkinOperationService();"),
        )
        lifecycle_shutdown = braced_body(
            source, "void shutdownGameplaySkinLifecycle() noexcept"
        )
        self.assertLess(
            lifecycle_shutdown.index("acquireGameplaySkinForNextChart = {};"),
            lifecycle_shutdown.index("gameplaySkinLifecycle->shutdown();"),
        )

    def test_application_owned_live_resource_counters_reach_every_session(self):
        context = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        initialization = braced_body(context, "void initializeGameplaySkinServices()")
        self.assertIn(
            "skinLiveResourceCounters =\n            std::make_shared<skin::SkinLiveResourceCounters>();",
            initialization,
        )
        destruction = braced_body(context, "void destroyGameplaySkinServices() noexcept")
        self.assertIn("skinLiveResourceCounters.reset();", destruction)

        gameplay = GAMEPLAY_SCENE_SOURCE.read_text(encoding="utf-8")
        attempt = braced_body(
            gameplay, "void GamePlayScene::acquireGameplaySkinForAttempt()"
        )
        self.assertIn("!context.skinLiveResourceCounters", attempt)
        self.assertIn(
            ".liveResourceCounters = context.skinLiveResourceCounters", attempt
        )

    def test_skin_service_startup_failure_unwinds_to_sanitized_unavailable_state(self):
        source = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        initialization = braced_body(source, "void initializeGameplaySkinServices()")
        self.assertTrue(initialization.lstrip().startswith("try {"))
        failure = initialization.index("} catch (...) {")
        shutdown = initialization.index(
            "unwindGameplaySkinServicesAfterStartupFailure();", failure
        )
        result = initialization.index(
            "skinRecoveryResult = skin::SkinRecoveryResult{", shutdown
        )
        self.assertLess(failure, shutdown)
        self.assertLess(shutdown, result)
        self.assertIn(
            ".disposition = skin::SkinRecoveryDisposition::Failed",
            initialization[result:],
        )
        self.assertIn('.code = "skin.startup.unavailable"', initialization[result:])
        self.assertIn(
            '.message = "Gameplay skin services could not be initialized"',
            initialization[result:],
        )

    def test_disabled_context_names_no_enabled_skin_service_types(self):
        source = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        assert_feature_guarded(
            self,
            source,
            (
                "SkinStorageRoots",
                "SkinAliasDetector",
                "SkinPackageCatalog",
                "SkinPackageStore",
                "SkinRecoveryResult",
                "SkinResourcePreparationService",
                "GameplaySkinValidator",
                "SkinPackageOperationService",
                "SkinDiagnosticHistory",
                "SkinConfigurationWriteQueue",
                "SkinCommitCoordinator",
                "GameplaySkinLifecycle",
                "GameplaySkinActivationRequest",
                "AcquireGameplaySkinForNextChart",
            ),
        )

    def test_lifecycle_requires_reconciliation_and_scan_readiness_before_acquisition(
        self,
    ):
        source = GAMEPLAY_SKIN_LIFECYCLE_SOURCE.read_text(encoding="utf-8")
        inventory = braced_body(source, "void pollInventoryAndRescan()")
        self.assertIn("submitReconcileProfileActivations", inventory)
        self.assertNotIn("submitRescan", inventory)

        completion = braced_body(source, "consumePrepareCompletion(")
        reconcile = completion.index("PreparePurpose::Reconcile")
        rescan = completion.index("deps.submitRescan", reconcile)
        scan = completion.index("PreparePurpose::Rescan", rescan)
        ready = completion.index("acquisitionReady = true", scan)
        self.assertLess(reconcile, rescan)
        self.assertLess(rescan, scan)
        self.assertLess(scan, ready)

        acquisition = braced_body(source, "acquireForNextChart()")
        self.assertIn("!impl_->acquisitionReady", acquisition)

    def test_skin_service_teardown_preserves_dependency_lifetimes(self):
        source = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        destructor = braced_body(source, "~ApplicationContext()")
        ordered_destructor = (
            "shutdownGameplaySkinOperationService();",
            "temporaryPathCleanupService->shutdown();",
            "shutdownGameplaySkinCommitCoordinator();",
            "flushAndShutdownGameplaySkinBackingServices();",
            "profileSettingsPersistenceCoordinator->shutdown();",
            "profileSettingsPersistenceCoordinator.reset();",
            "destroyGameplaySkinServices();",
        )
        positions = [destructor.index(token) for token in ordered_destructor]
        self.assertEqual(positions, sorted(positions))

        operation = braced_body(
            source, "void shutdownGameplaySkinOperationService() noexcept"
        )
        self.assertLess(
            operation.index("skinPackageOperationService->shutdown();"),
            operation.index("skinPackageOperationService.reset();"),
        )

        commit = braced_body(
            source, "void shutdownGameplaySkinCommitCoordinator() noexcept"
        )
        self.assertLess(
            commit.index("skinCommitCoordinator->shutdown();"),
            commit.index("skinCommitCoordinator.reset();"),
        )

        backing = braced_body(
            source, "void flushAndShutdownGameplaySkinBackingServices() noexcept"
        )
        ordered_backing = (
            "skinDiagnosticHistory->flush();",
            "skinPackageCatalog->flush();",
            "skinPackageCatalog->shutdown();",
            "skinResourcePreparationService->shutdown();",
        )
        positions = [backing.index(token) for token in ordered_backing]
        self.assertEqual(positions, sorted(positions))

        destroy = braced_body(source, "void destroyGameplaySkinServices() noexcept")
        ordered_destroy = (
            "skinDiagnosticHistory.reset();",
            "gameplaySkinValidator.reset();",
            "skinPackageStore.reset();",
            "skinPackageCatalog.reset();",
            "skinResourcePreparationService.reset();",
            "skinAliasDetector.reset();",
        )
        positions = [destroy.index(token) for token in ordered_destroy]
        self.assertEqual(positions, sorted(positions))

        startup_unwind = braced_body(
            source,
            "void unwindGameplaySkinServicesAfterStartupFailure() noexcept",
        )
        ordered_startup_unwind = (
            "shutdownGameplaySkinOperationService();",
            "shutdownGameplaySkinCommitCoordinator();",
            "flushAndShutdownGameplaySkinBackingServices();",
            "destroyGameplaySkinServices();",
        )
        positions = [startup_unwind.index(token) for token in ordered_startup_unwind]
        self.assertEqual(positions, sorted(positions))
        self.assertNotIn("temporaryPathCleanupService", startup_unwind)
        self.assertNotIn("profileSettingsPersistenceCoordinator", startup_unwind)

    def test_profile_catalog_mutation_wiring_orders_both_enabled_barriers(self):
        context = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        begin = braced_body(
            context, "beginSkinProfileCatalogMutation("
        )
        self.assertLess(
            begin.index("skinCommitCoordinator->beginProfileMutation"),
            begin.index(
                "profileSettingsPersistenceCoordinator->beginInventoryMutation"
            ),
        )

        finish = braced_body(
            context, "finishSkinProfileCatalogMutation("
        )
        self.assertLess(
            finish.index("skinCommitCoordinator->finishProfileMutation"),
            finish.index(
                "profileSettingsPersistenceCoordinator->finishInventoryMutation"
            ),
        )

        wiring = PROFILE_SETTINGS_CONTROLLER_CONTEXT_SOURCE.read_text(
            encoding="utf-8"
        )
        dependencies = braced_body(wiring, "applicationDependencies(")
        self.assertIn(".beginSkinProfileCatalogMutation", dependencies)
        self.assertIn("context.beginSkinProfileCatalogMutation", dependencies)
        self.assertIn(".finishSkinProfileCatalogMutation", dependencies)
        self.assertIn("context.finishSkinProfileCatalogMutation", dependencies)

    def test_unguarded_profile_controller_sources_name_no_skin_barrier_types(self):
        forbidden = (
            "SkinCommitCoordinator",
            "SkinProfileMutationBarrier",
            "ProfileInventoryMutationBarrier",
            "ISkinProfileSnapshotProvider",
        )
        for path in (
            PROFILE_SETTINGS_CONTROLLER_HEADER,
            PROFILE_SETTINGS_CONTROLLER_SOURCE,
            PROFILE_SETTINGS_CONTROLLER_CONTEXT_SOURCE,
        ):
            source = path.read_text(encoding="utf-8")
            for token in forbidden:
                self.assertNotIn(token, source, f"{token} leaked into {path}")

    def test_scene_manager_resets_bga_composite_state_before_scene_lookup(self):
        context = APPLICATION_CONTEXT_HEADER.read_text(encoding="utf-8")
        self.assertIn(
            "GameplayBgaCompositeState gameplayBgaCompositeState;", context
        )
        source = SCENE_MANAGER_SOURCE.read_text(encoding="utf-8")
        render = braced_body(source, "void SceneManager::render()")
        self.assertTrue(
            render.lstrip().startswith("context.gameplayBgaCompositeState = {};"),
            "the per-frame state must reset even when there is no current scene",
        )
        self.assertLess(
            render.index("context.gameplayBgaCompositeState = {};"),
            render.index("if (currentScene)"),
        )

    def test_post_scene_bga_compositor_uses_the_same_frame_decision(self):
        source = MAIN_SOURCE.read_text(encoding="utf-8")
        render_branch = braced_body(
            source,
            "else if (bgfxLock.owns_lock() && !context.replayVideoExportActive.load",
        )
        scene = render_branch.index("sceneManager.render();")
        decision = render_branch.index(
            "const GameplayBgaCompositeState &bgaCompositeState ="
        )
        touches = render_branch.index("bgfx::touch(rendering::bga_view);")
        prepared = render_branch.index(
            "context.jukebox.submitFullscreen(*bgaCompositeState.prepared);"
        )
        legacy = render_branch.index("context.jukebox.render();")
        postprocess = render_branch.index("s_postProcess.apply();")
        prepared_decision = render_branch[decision : render_branch.index(
            "const bool submitLegacyFullscreen =", decision
        )]
        self.assertLess(scene, decision)
        self.assertLess(decision, touches)
        self.assertLess(touches, prepared)
        self.assertLess(prepared, legacy)
        self.assertLess(legacy, postprocess)
        self.assertEqual(1, render_branch.count("context.jukebox.submitFullscreen("))
        self.assertEqual(1, render_branch.count("context.jukebox.render();"))
        self.assertNotIn("hasActiveVisuals", prepared_decision)
        self.assertIn(
            "bgaCompositeState.mode ==\n                GameplayBgaCompositeMode::FullscreenBuiltIn",
            render_branch,
        )
        self.assertIn(
            "const bool compositeFullscreenBga =\n            submitPreparedFullscreen || submitLegacyFullscreen;",
            render_branch,
        )
        legacy_decision = render_branch[
            render_branch.index("const bool submitLegacyFullscreen =") :
            render_branch.index("const bool compositeFullscreenBga =")
        ]
        self.assertIn("bgaCompositeState.frameSerial == 0", legacy_decision)
        self.assertNotIn("if (hasActiveVisuals)", render_branch[scene:])


if __name__ == "__main__":
    unittest.main()
