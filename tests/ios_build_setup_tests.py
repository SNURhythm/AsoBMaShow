#!/usr/bin/env python3
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj"
PODFILE = ROOT / "ios/Xcode/AsoBMaShow/Podfile"
INFO_PLIST = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow/Info.plist"
WORKSPACE = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcworkspace/contents.xcworkspacedata"
SRC_GROUP_ID = "B76AAF3F2DA4A1C400E8327C"
EXCEPTION_SET_ID = "B76AAF692DA4A1C400E8327C"
TARGET_ID = "B70027002BF7A8D8000DB8EC"
DERIVED_DATA_HELPER = ROOT / "scripts/ios_derived_data_path.sh"
DEPLOY_SCRIPT = ROOT / "scripts/ios_firebase_deploy.sh"
FASTFILE = ROOT / "ios/Xcode/AsoBMaShow/fastlane/Fastfile"
PODS_CACHE_HELPER = ROOT / "scripts/ios_pods_cache.sh"
IOS_INIT = ROOT / "scripts/ios_init.sh"
IOS_RELEASE_VERIFY = ROOT / "scripts/ios_release_verify.sh"
AGENT_GUIDANCE = ROOT / "AGENTS.md"
SDL_HEADER_ALIAS = ROOT / "ios/Xcode/AsoBMaShow/include/SDL2"
MAIN_SOURCE = ROOT / "src/main.cpp"
IOS_NATIVES_SOURCE = ROOT / "src/iOSNatives.mm"
IOS_NATIVES_HEADER = ROOT / "src/iOSNatives.hpp"
SKIN_STORAGE_PATHS_SOURCE = ROOT / "src/skin/SkinStoragePaths.cpp"
SKIN_PACKAGE_STORE_SOURCE = ROOT / "src/skin/package/SkinPackageStore.cpp"
SKIN_PACKAGE_CATALOG_SOURCE = ROOT / "src/skin/package/SkinPackageCatalog.cpp"
SKIN_ALIAS_DETECTOR_APPLE = ROOT / "src/skin/package/SkinAliasDetectorApple.mm"
VCPKG_MANIFEST = ROOT / "vcpkg.json"
IOS_LIB_SCRIPT = ROOT / "scripts/get_ios_libs.py"
IOS_UTF8PROC_PREPARE = ROOT / "scripts/ios_utf8proc.py"
BUILD_IDENTITY_HEADER = ROOT / "src/BuildIdentity.h"
BUILD_IDENTITY_SOURCE = ROOT / "src/BuildIdentity.cpp"
SKIN_ACCEPTANCE_INSTALL = ROOT / "scripts/ios_build_install_for_skin_acceptance.sh"
UTF8PROC_LICENSE = ROOT / "assets/legal/utf8proc.txt"
UTF8PROC_LICENSE_SHA256 = (
    "3b510150d34f248a221bb88e1d811238d6c6c18b51231822c42974c39bb07256"
)


def object_block(project: str, object_id: str, next_section: str) -> str:
    start = project.index(f"\t\t{object_id}")
    end = project.index(next_section, start)
    return project[start:end]


class IOSBuildSetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.project = PROJECT.read_text(encoding="utf-8")

    def test_app_target_owns_synchronized_src_group(self):
        target = object_block(
            self.project, TARGET_ID, "/* End PBXNativeTarget section */"
        )
        self.assertIn("fileSystemSynchronizedGroups = (", target)
        self.assertIn(SRC_GROUP_ID, target)

    def test_only_platform_and_build_metadata_are_membership_exceptions(self):
        exceptions = object_block(
            self.project,
            EXCEPTION_SET_ID,
            "/* End PBXFileSystemSynchronizedBuildFileExceptionSet section */",
        )
        start = exceptions.index("membershipExceptions = (")
        end = exceptions.index("\n\t\t\t);", start)
        paths = [
            line.strip().removesuffix(",")
            for line in exceptions[start:end].splitlines()[1:]
            if line.strip()
        ]
        cmake_files = sorted(
            str(path.relative_to(ROOT / "src"))
            for path in (ROOT / "src").rglob("CMakeLists.txt")
        )
        self.assertEqual(
            sorted(
                ["AndroidNatives.cpp", "ChartScanWorkScheduler.md", *cmake_files]
            ),
            paths,
        )
        self.assertIn(f"target = {TARGET_ID}", exceptions)

    def test_audio_wrapper_keeps_objective_cpp_override(self):
        group = object_block(
            self.project,
            SRC_GROUP_ID,
            "/* End PBXFileSystemSynchronizedRootGroup section */",
        )
        self.assertIn(
            "audio/AudioWrapper.cpp = sourcecode.cpp.objcpp;", group
        )

    def test_ios_release_contract_remains_version_0_0_1_on_ios_14(self):
        target_configurations = [
            object_block(
                self.project,
                configuration_id,
                "\n\t\t};",
            )
            for configuration_id in (
                "B700271D2BF7A8DA000DB8EC",
                "B700271E2BF7A8DA000DB8EC",
            )
        ]
        for configuration in target_configurations:
            self.assertIn("IPHONEOS_DEPLOYMENT_TARGET = 14.0;", configuration)
            self.assertIn("MARKETING_VERSION = 0.0.1;", configuration)

    def test_skin_acceptance_build_identity_is_compiled_and_mirrored_to_plist(self):
        with INFO_PLIST.open("rb") as handle:
            info = plistlib.load(handle)
        self.assertEqual("$(ASOBMASHOW_BUILD_COMMIT)", info["AsoBMaShowBuildCommit"])
        self.assertEqual("$(CONFIGURATION)", info["AsoBMaShowBuildConfiguration"])
        self.assertEqual("$(ASOBMASHOW_SOURCE_CLEAN)", info["AsoBMaShowSourceClean"])

        target_configurations = [
            object_block(self.project, configuration_id, "\n\t\t};")
            for configuration_id in (
                "B700271D2BF7A8DA000DB8EC",
                "B700271E2BF7A8DA000DB8EC",
            )
        ]
        for configuration in target_configurations:
            self.assertIn("ASOBMASHOW_BUILD_COMMIT =", configuration)
            self.assertIn("ASOBMASHOW_SOURCE_CLEAN = 0;", configuration)
            self.assertIn(
                r'ASOBMASHOW_BUILD_COMMIT=\\\"$(ASOBMASHOW_BUILD_COMMIT)\\\"',
                configuration,
            )
            self.assertIn(
                r'ASOBMASHOW_BUILD_CONFIGURATION=\\\"$(CONFIGURATION)\\\"',
                configuration,
            )
            self.assertIn(
                "ASOBMASHOW_SOURCE_CLEAN=$(ASOBMASHOW_SOURCE_CLEAN)",
                configuration,
            )

    def compile_build_identity(self, root: Path, commit: str, configuration: str, clean: int):
        compiler = shutil.which("clang++") or shutil.which("c++")
        if compiler is None:
            self.skipTest("C++ compiler is required for build-identity tests")
        driver = root / "build_identity_driver.cpp"
        executable = root / "build_identity_driver"
        driver.write_text(
            '#include "BuildIdentity.h"\n'
            "#include <iostream>\n"
            "int main() {\n"
            "  const auto identity = skin::compiledSkinBuildIdentity();\n"
            "  std::cout << identity.commit << '\\n'\n"
            "            << identity.configuration << '\\n'\n"
            "            << identity.cleanSource << '\\n'\n"
            "            << identity.validForAcceptance() << '\\n';\n"
            "}\n",
            encoding="utf-8",
        )
        result = subprocess.run(
            [
                compiler,
                "-std=c++20",
                "-I",
                str(ROOT / "src"),
                f'-DASOBMASHOW_BUILD_COMMIT="{commit}"',
                f'-DASOBMASHOW_BUILD_CONFIGURATION="{configuration}"',
                f"-DASOBMASHOW_SOURCE_CLEAN={clean}",
                str(driver),
                str(BUILD_IDENTITY_SOURCE),
                "-o",
                str(executable),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        return subprocess.run(
            [str(executable)], text=True, capture_output=True, check=True
        ).stdout.splitlines()

    def test_compiled_skin_build_identity_rejects_placeholder_dirty_and_nonclean_values(self):
        valid_commit = "1234567890abcdef1234567890abcdef12345678"
        cases = (
            (valid_commit, "Release", 1, [valid_commit, "Release", "1", "1"]),
            ("0" * 40, "Release", 1, ["0" * 40, "Release", "1", "0"]),
            (valid_commit, "dirty", 1, [valid_commit, "dirty", "1", "0"]),
            (valid_commit, "Release", 0, [valid_commit, "Release", "0", "0"]),
        )
        for commit, configuration, clean, expected in cases:
            with self.subTest(commit=commit, configuration=configuration, clean=clean):
                with tempfile.TemporaryDirectory() as temporary:
                    self.assertEqual(
                        expected,
                        self.compile_build_identity(
                            Path(temporary), commit, configuration, clean
                        ),
                    )

    def test_compiled_skin_build_identity_marker_survives_ios_dead_stripping(self):
        xcrun = shutil.which("xcrun")
        if xcrun is None:
            self.skipTest("Xcode command-line tools are required")
        commit = "1234567890abcdef1234567890abcdef12345678"
        marker = f"AsoBMaShowBuildIdentityV1|{commit}|Release|1"
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            driver = root / "unreferencing_main.cpp"
            executable = root / "dead_stripped_identity"
            driver.write_text("int main() { return 0; }\n", encoding="utf-8")
            result = subprocess.run(
                [
                    xcrun,
                    "--sdk",
                    "iphoneos",
                    "clang++",
                    "-std=c++20",
                    "-arch",
                    "arm64",
                    "-miphoneos-version-min=14.0",
                    "-I",
                    str(ROOT / "src"),
                    f'-DASOBMASHOW_BUILD_COMMIT="{commit}"',
                    '-DASOBMASHOW_BUILD_CONFIGURATION="Release"',
                    "-DASOBMASHOW_SOURCE_CLEAN=1",
                    str(BUILD_IDENTITY_SOURCE),
                    str(driver),
                    "-Wl,-dead_strip",
                    "-o",
                    str(executable),
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            strings = subprocess.run(
                ["/usr/bin/strings", "-a", str(executable)],
                text=True,
                capture_output=True,
                check=True,
            )
            self.assertIn(marker, strings.stdout.splitlines())

    def test_skin_acceptance_direct_install_script_is_private_and_distribution_free(self):
        script = SKIN_ACCEPTANCE_INSTALL.read_text(encoding="utf-8")
        for argument in (
            "--commit",
            "--configuration",
            "--device-id",
            "--development-team",
        ):
            self.assertIn(argument, script)
        self.assertIn('scripts/ios_init.sh', script)
        self.assertIn('xcodebuild', script)
        self.assertIn('-configuration "${CONFIGURATION}"', script)
        self.assertIn('[ "${CONFIGURATION}" = "Release" ]', script)
        self.assertIn('-derivedDataPath "${DERIVED_DATA_PATH}"', script)
        self.assertIn('ASOBMASHOW_BUILD_COMMIT="${COMMIT}"', script)
        self.assertIn('ASOBMASHOW_SOURCE_CLEAN=1', script)
        self.assertIn('scripts/ios_artifact_audit.sh', script)
        self.assertIn('xcrun devicectl device install app', script)
        self.assertNotIn('set -x', script)
        self.assertNotRegex(
            script.lower(), r"firebase|testflight|fastlane|upload|distribution"
        )

    def test_skin_acceptance_direct_install_rejects_wrong_or_dirty_checkout_before_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary)
            scripts = fixture / "scripts"
            scripts.mkdir()
            shutil.copy2(SKIN_ACCEPTANCE_INSTALL, scripts / SKIN_ACCEPTANCE_INSTALL.name)
            subprocess.run(["git", "init", "-q"], cwd=fixture, check=True)
            subprocess.run(
                ["git", "config", "user.email", "fixture@example.invalid"],
                cwd=fixture,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "Build Identity Fixture"],
                cwd=fixture,
                check=True,
            )
            (fixture / "tracked.txt").write_text("clean\n", encoding="utf-8")
            subprocess.run(["git", "add", "."], cwd=fixture, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "fixture"], cwd=fixture, check=True
            )
            head = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=fixture,
                text=True,
                capture_output=True,
                check=True,
            ).stdout.strip()
            command = [
                str(scripts / SKIN_ACCEPTANCE_INSTALL.name),
                "--configuration", "Release",
                "--device-id", "private-device",
                "--development-team", "TEAM123",
            ]
            wrong = subprocess.run(
                [*command, "--commit", "f" * 40],
                cwd=fixture,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(0, wrong.returncode)
            self.assertIn("HEAD", wrong.stderr)
            self.assertNotIn("private-device", wrong.stdout + wrong.stderr)

            (fixture / "tracked.txt").write_text("dirty\n", encoding="utf-8")
            dirty = subprocess.run(
                [*command, "--commit", head],
                cwd=fixture,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(0, dirty.returncode)
            self.assertIn("clean", dirty.stderr)
            self.assertNotIn("private-device", dirty.stdout + dirty.stderr)

    def test_skin_acceptance_direct_install_rejects_nested_parent_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            parent = Path(temporary)
            subprocess.run(["git", "init", "-q"], cwd=parent, check=True)
            subprocess.run(
                ["git", "config", "user.email", "fixture@example.invalid"],
                cwd=parent,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "Parent Repository Fixture"],
                cwd=parent,
                check=True,
            )
            (parent / ".gitignore").write_text("ignored/\n", encoding="utf-8")
            subprocess.run(["git", "add", ".gitignore"], cwd=parent, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "parent"], cwd=parent, check=True
            )
            head = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=parent,
                text=True,
                capture_output=True,
                check=True,
            ).stdout.strip()
            nested_scripts = parent / "ignored/source/scripts"
            nested_scripts.mkdir(parents=True)
            script = nested_scripts / SKIN_ACCEPTANCE_INSTALL.name
            shutil.copy2(SKIN_ACCEPTANCE_INSTALL, script)
            result = subprocess.run(
                [
                    str(script),
                    "--commit", head,
                    "--configuration", "Release",
                    "--device-id", "private-device",
                    "--development-team", "TEAM123",
                ],
                cwd=parent / "ignored/source",
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("repository root", result.stderr)
            self.assertNotIn("private-device", result.stdout + result.stderr)

    def test_skin_acceptance_direct_install_rechecks_checkout_after_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            fixture = Path(temporary)
            scripts = fixture / "scripts"
            fake_bin = fixture / "fake-bin"
            scripts.mkdir()
            fake_bin.mkdir()
            shutil.copy2(SKIN_ACCEPTANCE_INSTALL, scripts / SKIN_ACCEPTANCE_INSTALL.name)
            (scripts / "ios_init.sh").write_text(
                "#!/usr/bin/env bash\nset -euo pipefail\n",
                encoding="utf-8",
            )
            (scripts / "ios_artifact_audit.sh").write_text(
                "#!/usr/bin/env bash\nset -euo pipefail\n"
                'touch "${FAKE_REPO_ROOT}/audit-called"\n',
                encoding="utf-8",
            )
            (fake_bin / "xcodebuild").write_text(
                "#!/usr/bin/env bash\nset -euo pipefail\n"
                'printf "changed during build\\n" > "${FAKE_REPO_ROOT}/tracked.txt"\n'
                'derived=""\n'
                'while [ "$#" -gt 0 ]; do\n'
                '  if [ "$1" = "-derivedDataPath" ]; then derived="$2"; shift 2; else shift; fi\n'
                'done\n'
                'mkdir -p "${derived}/Build/Products/Release-iphoneos/AsoBMaShow.app"\n',
                encoding="utf-8",
            )
            (fake_bin / "xcrun").write_text(
                "#!/usr/bin/env bash\nset -euo pipefail\n"
                'touch "${FAKE_REPO_ROOT}/install-called"\n',
                encoding="utf-8",
            )
            for executable in (
                scripts / SKIN_ACCEPTANCE_INSTALL.name,
                scripts / "ios_init.sh",
                scripts / "ios_artifact_audit.sh",
                fake_bin / "xcodebuild",
                fake_bin / "xcrun",
            ):
                executable.chmod(0o755)
            subprocess.run(["git", "init", "-q"], cwd=fixture, check=True)
            subprocess.run(
                ["git", "config", "user.email", "fixture@example.invalid"],
                cwd=fixture,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "Post-build Fixture"],
                cwd=fixture,
                check=True,
            )
            (fixture / "tracked.txt").write_text("clean\n", encoding="utf-8")
            subprocess.run(["git", "add", "."], cwd=fixture, check=True)
            subprocess.run(
                ["git", "commit", "-qm", "fixture"], cwd=fixture, check=True
            )
            head = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=fixture,
                text=True,
                capture_output=True,
                check=True,
            ).stdout.strip()
            environment = {
                **os.environ,
                "PATH": f"{fake_bin}{os.pathsep}{os.environ['PATH']}",
                "FAKE_REPO_ROOT": str(fixture),
            }
            result = subprocess.run(
                [
                    str(scripts / SKIN_ACCEPTANCE_INSTALL.name),
                    "--commit", head,
                    "--configuration", "Release",
                    "--device-id", "private-device",
                    "--development-team", "TEAM123",
                ],
                cwd=fixture,
                text=True,
                capture_output=True,
                env=environment,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("changed during iOS build", result.stderr)
            self.assertFalse((fixture / "audit-called").exists())
            self.assertFalse((fixture / "install-called").exists())
            self.assertNotIn("private-device", result.stdout + result.stderr)

    def test_pods_and_generated_bgfx_align_to_ios_14(self):
        podfile = PODFILE.read_text(encoding="utf-8")
        self.assertIn("platform :ios, '14.0'", podfile)
        self.assertIn("IPHONEOS_DEPLOYMENT_TARGET", podfile)
        self.assertIn("'14.0'", podfile)
        init_script = IOS_INIT.read_text(encoding="utf-8")
        self.assertIn("-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0", init_script)

    def test_all_ios_build_entrypoints_override_dependencies_to_ios_14(self):
        self.assertIn(
            "IPHONEOS_DEPLOYMENT_TARGET=14.0", DEPLOY_SCRIPT.read_text()
        )
        self.assertIn("IPHONEOS_DEPLOYMENT_TARGET=14.0", FASTFILE.read_text())

    def test_ats_exception_is_retained_without_privacy_manifest(self):
        plist = subprocess.run(
            [
                "plutil",
                "-extract",
                "NSAppTransportSecurity.NSAllowsArbitraryLoads",
                "raw",
                str(INFO_PLIST),
            ],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        )
        self.assertEqual("true", plist.stdout.strip())
        self.assertFalse(any(ROOT.rglob("PrivacyInfo.xcprivacy")))
        verify_script = IOS_RELEASE_VERIFY.read_text(encoding="utf-8")
        self.assertIn("ios_artifact_audit.sh", verify_script)
        self.assertNotIn("PrivacyInfo.xcprivacy", verify_script)

    def test_ios_forces_bgfx_metal_work_onto_the_main_thread(self):
        source = MAIN_SOURCE.read_text(encoding="utf-8")
        limits = source.index("rendering::applyBgfxTransientBufferLimits")
        ios_guard = source.index("#if TARGET_OS_IPHONE", limits)
        force_single_threaded = source.index("bgfx::renderFrame();", ios_guard)
        non_ios_branch = source.index("#else", ios_guard)
        initialize_bgfx = source.index("int appExitCode = runApplication(bgfx_init);")
        self.assertLess(ios_guard, force_single_threaded)
        self.assertLess(force_single_threaded, non_ios_branch)
        self.assertLess(force_single_threaded, initialize_bgfx)
        self.assertIn("Using bgfx single-threaded mode on iOS", source)

    def test_ios_active_window_lookup_uses_a_validated_weak_cache(self):
        source = IOS_NATIVES_SOURCE.read_text(encoding="utf-8")
        lookup_start = source.index("UIWindow *FindActiveWindow()")
        lookup_end = source.index(
            "void RestoreIOSViewportAfterKeyboardFocusOnce()", lookup_start
        )
        lookup = source[lookup_start:lookup_end]

        cache_declaration = lookup.index(
            "static __weak UIWindow *cachedActiveWindow"
        )
        cache_validation = lookup.index(
            "cachedWindow.windowScene.activationState =="
        )
        key_window_validation = lookup.index("cachedWindow.isKeyWindow")
        visible_window_validation = lookup.index("!cachedWindow.hidden")
        scene_enumeration = lookup.index(
            "UIApplication.sharedApplication.connectedScenes"
        )
        self.assertLess(cache_declaration, cache_validation)
        self.assertLess(cache_validation, key_window_validation)
        self.assertLess(key_window_validation, visible_window_validation)
        self.assertLess(visible_window_validation, scene_enumeration)
        self.assertIn("cachedActiveWindow = window;", lookup)

    def test_ios_private_skin_storage_uses_application_support_and_excludes_backup(self):
        header = IOS_NATIVES_HEADER.read_text(encoding="utf-8")
        source = IOS_NATIVES_SOURCE.read_text(encoding="utf-8")
        self.assertIn("std::string GetIOSApplicationSupportPath();", header)
        implementation_start = source.index("GetIOSApplicationSupportPath()")
        implementation_end = source.index("\n}\n\n// get nwh", implementation_start)
        implementation = source[implementation_start:implementation_end]
        self.assertIn("NSApplicationSupportDirectory", implementation)
        self.assertIn("createDirectoryAtURL", implementation)
        self.assertIn("NSURLIsExcludedFromBackupKey", implementation)
        self.assertIn("setResourceValue", implementation)
        self.assertIn("URLByResolvingSymlinksInPath", implementation)
        self.assertLess(
            implementation.index("URLByResolvingSymlinksInPath"),
            implementation.index('URLByAppendingPathComponent:@"AsoBMaShow"'),
        )
        self.assertIn(
            "return std::string(directory.fileSystemRepresentation);",
            implementation,
        )
        self.assertNotIn(
            "if (![directory setResourceValue:@YES\n"
            "                              forKey:NSURLIsExcludedFromBackupKey\n"
            "                               error:&error]) {\n"
            "      return {};\n"
            "    }",
            implementation,
        )
        self.assertIn(
            "Could not exclude private skin storage from backup",
            implementation,
        )
        self.assertNotIn(
            "directory.URLByResolvingSymlinksInPath", implementation
        )

    def test_ios_documents_path_resolves_the_trusted_sandbox_alias(self):
        source = IOS_NATIVES_SOURCE.read_text(encoding="utf-8")
        implementation_start = source.index("std::string GetIOSDocumentsPath()")
        implementation_end = source.index(
            "std::string GetIOSApplicationSupportPath()", implementation_start
        )
        implementation = source[implementation_start:implementation_end]
        self.assertIn("NSDocumentDirectory", implementation)
        self.assertIn("NSSearchPathForDirectoriesInDomains", implementation)
        self.assertIn("if (paths.count == 0)", implementation)
        self.assertIn("createDirectoryAtURL", implementation)
        self.assertIn("URLByResolvingSymlinksInPath", implementation)
        self.assertIn("resolvedDirectory.fileSystemRepresentation", implementation)

    def test_ios_skin_storage_keeps_all_state_in_files_visible_documents(self):
        source = SKIN_STORAGE_PATHS_SOURCE.read_text(encoding="utf-8")
        start = source.index("SkinStorageRoots defaultSkinStorageRoots()")
        end = source.index("#endif", start)
        implementation = source[start:end]
        self.assertIn('Utils::GetDocumentsPath("Skins")', implementation)
        self.assertIn('Utils::GetDocumentsPath("_runtime")', implementation)
        self.assertIn("return deriveSkinStorageRoots(visible, workspace);", implementation)
        self.assertNotIn("GetIOSApplicationSupportPath", implementation)

    def test_ios_skin_storage_bootstrap_uses_normal_directory_creation(self):
        source = SKIN_PACKAGE_STORE_SOURCE.read_text(encoding="utf-8")
        self.assertIn("#if TARGET_OS_IOS || TARGET_OS_SIMULATOR", source)
        ios_start = source.index("#if TARGET_OS_IOS || TARGET_OS_SIMULATOR")
        ios_end = source.index("#else", ios_start)
        implementation = source[ios_start:ios_end]
        self.assertIn("bool ensureDirectoryNoFollow", implementation)
        self.assertIn("fs::create_directories(directory, error)", implementation)
        self.assertIn("fs::is_directory(directory, error)", implementation)
        self.assertNotIn("O_NOFOLLOW", implementation)

    def test_ios_catalog_bootstrap_uses_normal_directory_creation(self):
        source = SKIN_PACKAGE_CATALOG_SOURCE.read_text(encoding="utf-8")
        self.assertIn("bool ensureDirectoryNoFollow", source)
        self.assertIn("#if TARGET_OS_IOS || TARGET_OS_SIMULATOR", source)
        ios_start = source.index("#if TARGET_OS_IOS || TARGET_OS_SIMULATOR")
        ios_end = source.index("#else", ios_start)
        implementation = source[ios_start:ios_end]
        self.assertIn("fs::create_directories(directory, error)", implementation)
        self.assertIn("fs::is_directory(directory, error)", implementation)
        self.assertNotIn("O_NOFOLLOW", implementation)

    def test_ios_folder_handoff_and_files_document_access_are_declared(self):
        with INFO_PLIST.open("rb") as handle:
            info = plistlib.load(handle)
        for key in (
            "UIFileSharingEnabled",
            "LSSupportsOpeningDocumentsInPlace",
            "UISupportsDocumentBrowser",
        ):
            self.assertIs(
                True,
                info.get(key),
                f"{key} must be enabled in source Info.plist",
            )

        header = IOS_NATIVES_HEADER.read_text(encoding="utf-8")
        source = IOS_NATIVES_SOURCE.read_text(encoding="utf-8")
        self.assertIn("ImportIOSDirectory", header)
        self.assertIn("ValidateIOSTemporaryDirectory", header)
        self.assertIn("CleanupIOSTemporaryDirectory", header)
        self.assertIn("UTTypeFolder", source)
        self.assertIn("ImportIOSDirectory", source)
        self.assertIn("CopyIOSDirectoryURLBounded", source)
        self.assertIn("NSURLIsSymbolicLinkKey", source)
        self.assertIn("NSURLIsAliasFileKey", source)

    def test_apple_skin_alias_detection_is_no_follow(self):
        source = SKIN_ALIAS_DETECTOR_APPLE.read_text(encoding="utf-8")
        self.assertIn("lstat", source)
        self.assertIn("NSURLIsAliasFileKey", source)
        self.assertIn("getResourceValue", source)
        self.assertNotIn("URLByResolvingAliasFile", source)

    def test_workspace_has_one_relative_pods_project(self):
        tree = ET.parse(WORKSPACE)
        locations = [node.attrib["location"] for node in tree.findall("FileRef")]
        pods_locations = [value for value in locations if "Pods.xcodeproj" in value]
        self.assertEqual(["group:Pods/Pods.xcodeproj"], pods_locations)

    def test_pods_path_is_not_tracked(self):
        result = subprocess.run(
            ["git", "ls-files", "--", "ios/Xcode/AsoBMaShow/Pods"],
            cwd=ROOT,
            check=True,
            text=True,
            capture_output=True,
        )
        self.assertEqual("", result.stdout.strip())

    def test_agent_guidance_describes_automatic_ios_sources(self):
        guidance = AGENT_GUIDANCE.read_text(encoding="utf-8")
        self.assertNotIn("add its path to `membershipExceptions`", guidance)
        self.assertIn("automatically discovers supported files under `src`", guidance)
        self.assertIn("checkout-specific DerivedData", guidance)
        self.assertIn("stable Firebase archive object root", guidance)

    def test_ios_uses_portable_stable_sdl_header_alias(self):
        self.assertTrue(SDL_HEADER_ALIAS.is_symlink())
        self.assertFalse(os.path.isabs(os.readlink(SDL_HEADER_ALIAS)))
        self.assertEqual((ROOT / "SDL/include").resolve(), SDL_HEADER_ALIAS.resolve())

    def test_ios_links_7zip_archive_registration_for_device_and_simulator(self):
        xcodebuild = shutil.which("xcodebuild")
        if xcodebuild is None:
            self.skipTest("xcodebuild is only available with Xcode")
        for configuration in ("Debug", "Release"):
            for sdk in ("iphoneos", "iphonesimulator"):
                with self.subTest(configuration=configuration, sdk=sdk):
                    result = subprocess.run(
                        [
                            xcodebuild,
                            "-project",
                            str(PROJECT.parent),
                            "-target",
                            "AsoBMaShow",
                            "-configuration",
                            configuration,
                            "-sdk",
                            sdk,
                            "-showBuildSettings",
                        ],
                        cwd=ROOT,
                        check=True,
                        text=True,
                        capture_output=True,
                    )
                    linker_flags = next(
                        line.partition("=")[2].strip()
                        for line in result.stdout.splitlines()
                        if line.strip().startswith("OTHER_LDFLAGS =")
                    )
                    self.assertIn("7zRegister.cpp.o", linker_flags)
                    self.assertIn("LzmaRegister.cpp.o", linker_flags)
                    self.assertIn("Lzma2Register.cpp.o", linker_flags)
                    self.assertIn("DeltaFilter.cpp.o", linker_flags)

    def test_utf8proc_is_packaged_for_device_and_simulator(self):
        manifest = VCPKG_MANIFEST.read_text(encoding="utf-8")
        script = IOS_LIB_SCRIPT.read_text(encoding="utf-8")
        self.assertIn('"utf8proc"', manifest)
        self.assertIn("ios_utf8proc.py", script)
        self.assertIn('"ensure"', script)
        self.assertTrue(UTF8PROC_LICENSE.is_file())
        self.assertIn("libutf8proc.xcframework", self.project)
        self.assertIn("libutf8proc.xcframework in Frameworks", self.project)


class IOSUtf8procSetupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        required = ("xcodebuild", "xcrun", "ar")
        missing = [tool for tool in required if shutil.which(tool) is None]
        if missing:
            raise unittest.SkipTest(
                "utf8proc artifact tests require " + ", ".join(missing)
            )

    @staticmethod
    def write_repository_manifest(repository: Path, baseline: str) -> None:
        (repository / "vcpkg.json").write_text(
            json.dumps(
                {
                    "name": "asobmashow-ios-utf8proc-test",
                    "version-string": "1",
                    "builtin-baseline": baseline,
                    "dependencies": ["utf8proc"],
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )

    @staticmethod
    def commit_registry_version(
        vcpkg: Path, version: str, port_tree: str
    ) -> str:
        baseline_path = vcpkg / "versions/baseline.json"
        version_path = vcpkg / "versions/u-/utf8proc.json"
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        version_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(
            json.dumps(
                {
                    "default": {
                        "utf8proc": {"baseline": version, "port-version": 0}
                    }
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        version_path.write_text(
            json.dumps(
                {
                    "versions": [
                        {
                            "git-tree": port_tree,
                            "version": version,
                            "port-version": 0,
                        }
                    ]
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        if not (vcpkg / ".git").exists():
            subprocess.run(["git", "init", "-q"], cwd=vcpkg, check=True)
            subprocess.run(
                ["git", "config", "user.email", "tests@example.invalid"],
                cwd=vcpkg,
                check=True,
            )
            subprocess.run(
                ["git", "config", "user.name", "AsoBMaShow Tests"],
                cwd=vcpkg,
                check=True,
            )
        subprocess.run(["git", "add", "versions"], cwd=vcpkg, check=True)
        subprocess.run(
            ["git", "commit", "-q", "-m", f"registry {version}"],
            cwd=vcpkg,
            check=True,
        )
        return subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=vcpkg,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()

    def make_vcpkg_fixture(self, root: Path) -> tuple[Path, Path, Path]:
        vcpkg = root / "vcpkg"
        repository = root / "repository"
        for triplet in ("arm64-ios", "arm64-ios-simulator"):
            triplet_source = ROOT / "vcpkg-triplets" / f"{triplet}.cmake"
            triplet_target = repository / "vcpkg-triplets" / triplet_source.name
            triplet_target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(triplet_source, triplet_target)
        fixture_license = repository / "assets/legal/utf8proc.txt"
        fixture_license.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(UTF8PROC_LICENSE, fixture_license)
        installed = vcpkg / "fixture-installed"
        source = root / "utf8proc.c"
        source.write_text(
            'const char *utf8proc_version(void) { return "fixture"; }\n',
            encoding="utf-8",
        )
        header = installed / "arm64-ios/include/utf8proc.h"
        header.parent.mkdir(parents=True)
        header.write_text(
            "#ifndef UTF8PROC_H\n"
            "#define UTF8PROC_H\n"
            "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
            "const char *utf8proc_version(void);\n"
            "#ifdef __cplusplus\n}\n#endif\n"
            "#endif\n",
            encoding="utf-8",
        )
        license_path = installed / "arm64-ios/share/utf8proc/copyright"
        license_path.parent.mkdir(parents=True)
        shutil.copy2(UTF8PROC_LICENSE, license_path)

        for triplet, sdk, target in (
            ("arm64-ios", "iphoneos", "arm64-apple-ios14.0"),
            (
                "arm64-ios-simulator",
                "iphonesimulator",
                "arm64-apple-ios14.0-simulator",
            ),
        ):
            library_dir = installed / triplet / "lib"
            library_dir.mkdir(parents=True)
            sdk_path = subprocess.run(
                ["xcrun", "--sdk", sdk, "--show-sdk-path"],
                check=True,
                text=True,
                capture_output=True,
            ).stdout.strip()
            object_path = root / f"utf8proc-{triplet}.o"
            subprocess.run(
                [
                    "xcrun",
                    "--sdk",
                    sdk,
                    "clang",
                    "-target",
                    target,
                    "-isysroot",
                    sdk_path,
                    "-c",
                    str(source),
                    "-o",
                    str(object_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run(
                [
                    "ar",
                    "rcs",
                    str(library_dir / "libutf8proc.a"),
                    str(object_path),
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        count_file = root / "vcpkg-invocations.txt"
        executable = vcpkg / "vcpkg"
        executable.write_text(
            "#!/bin/sh\n"
            "set -eu\n"
            ': "${FAKE_VCPKG_COUNT_FILE:?}"\n'
            'case " $* " in *" --classic "*) echo "classic mode forbidden" >&2; exit 84 ;; esac\n'
            '[ -f "$PWD/vcpkg.json" ] || { echo "missing private manifest" >&2; exit 83; }\n'
            'grep -q \"builtin-baseline\" "$PWD/vcpkg.json" || exit 85\n'
            'grep -q \"utf8proc\" "$PWD/vcpkg.json" || exit 86\n'
            'install_root=""\n'
            'triplet=""\n'
            'previous=""\n'
            'for argument in "$@"; do\n'
            '  if [ "$previous" = "--triplet" ]; then triplet="$argument"; fi\n'
            '  case "$argument" in\n'
            '    --x-install-root=*) install_root="${argument#*=}" ;;\n'
            '  esac\n'
            '  previous="$argument"\n'
            'done\n'
            '[ -n "$install_root" ] || { echo "missing private install root" >&2; exit 82; }\n'
            '[ -n "$triplet" ] || { echo "missing manifest triplet" >&2; exit 81; }\n'
            'mkdir -p "$install_root"\n'
            'cp -R "$(dirname "$0")/fixture-installed/$triplet" "$install_root/"\n'
            'printf "called:%s\\n" "$triplet" >> "${FAKE_VCPKG_COUNT_FILE}"\n',
            encoding="utf-8",
        )
        executable.chmod(0o755)
        baseline = self.commit_registry_version(vcpkg, "2.11.3", "1" * 40)
        self.write_repository_manifest(repository, baseline)
        return repository, vcpkg, count_file

    def run_prepare(
        self,
        command: str,
        *arguments: str | Path,
        environment: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        self.assertTrue(
            IOS_UTF8PROC_PREPARE.is_file(),
            "scripts/ios_utf8proc.py must prepare clean iOS checkouts",
        )
        env = os.environ.copy()
        if environment:
            env.update(environment)
        return subprocess.run(
            [
                "python3",
                str(IOS_UTF8PROC_PREPARE),
                command,
                *map(str, arguments),
            ],
            cwd=ROOT,
            env=env,
            text=True,
            capture_output=True,
        )

    def ensure_fixture(
        self, root: Path
    ) -> tuple[Path, Path, Path, Path, Path, dict[str, str]]:
        repository, vcpkg, count_file = self.make_vcpkg_fixture(root)
        output = root / "output"
        cache = root / "cache"
        environment = {"FAKE_VCPKG_COUNT_FILE": str(count_file)}
        result = self.run_prepare(
            "ensure",
            "--repository-root",
            repository,
            "--vcpkg-root",
            vcpkg,
            "--cache-root",
            cache,
            "--output-root",
            output,
            environment=environment,
        )
        self.assertEqual(0, result.returncode, result.stderr)
        return repository, vcpkg, count_file, output, cache, environment

    @staticmethod
    def archive_build_version(archive: Path) -> tuple[str, str]:
        with tempfile.TemporaryDirectory() as temp:
            extracted = Path(temp)
            subprocess.run(
                ["ar", "-x", str(archive)],
                cwd=extracted,
                check=True,
                capture_output=True,
                text=True,
            )
            objects = sorted(
                path
                for path in extracted.iterdir()
                if not path.name.startswith("__.SYMDEF")
            )
            if not objects:
                raise AssertionError(f"empty static archive: {archive}")
            details = subprocess.run(
                ["xcrun", "vtool", "-show-build", str(objects[0])],
                check=True,
                text=True,
                capture_output=True,
            ).stdout
        platform = re.search(r"^\s*platform (\S+)$", details, re.MULTILINE)
        minimum = re.search(r"^\s*minos (\S+)$", details, re.MULTILINE)
        if platform is None or minimum is None:
            raise AssertionError(f"missing build version in {archive}: {details}")
        return platform.group(1), minimum.group(1)

    def compile_header(self, header: Path, sdk: str, target: str) -> None:
        sdk_path = subprocess.run(
            ["xcrun", "--sdk", sdk, "--show-sdk-path"],
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()
        subprocess.run(
            [
                "xcrun",
                "--sdk",
                sdk,
                "clang++",
                "-target",
                target,
                "-isysroot",
                sdk_path,
                "-std=c++20",
                "-fsyntax-only",
                "-x",
                "c++",
                "-I",
                str(header.parent),
                "-include",
                header.name,
                "/dev/null",
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    def test_clean_checkout_generates_two_ios14_slices_and_compilable_header(self):
        with tempfile.TemporaryDirectory() as temp:
            _, _, count_file, output, _, _ = self.ensure_fixture(Path(temp))
            framework = output / "lib/libutf8proc.xcframework"
            header = output / "include/utf8proc.h"
            self.assertTrue(header.is_file())
            with (framework / "Info.plist").open("rb") as stream:
                info = plistlib.load(stream)
            libraries = {
                item["LibraryIdentifier"]: item
                for item in info["AvailableLibraries"]
            }
            self.assertEqual(
                {"ios-arm64", "ios-arm64-simulator"}, set(libraries)
            )
            self.assertNotIn("SupportedPlatformVariant", libraries["ios-arm64"])
            self.assertEqual(
                "simulator",
                libraries["ios-arm64-simulator"]["SupportedPlatformVariant"],
            )
            self.assertEqual(
                ("IOS", "14.0"),
                self.archive_build_version(
                    framework / "ios-arm64/libutf8proc.a"
                ),
            )
            self.assertEqual(
                ("IOSSIMULATOR", "14.0"),
                self.archive_build_version(
                    framework / "ios-arm64-simulator/libutf8proc.a"
                ),
            )
            self.compile_header(
                header, "iphoneos", "arm64-apple-ios14.0"
            )
            self.compile_header(
                header,
                "iphonesimulator",
                "arm64-apple-ios14.0-simulator",
            )
            self.assertEqual(
                ["called:arm64-ios", "called:arm64-ios-simulator"],
                count_file.read_text().splitlines(),
            )

    def test_cache_restores_missing_outputs_without_rebuilding_and_prunes_stale_key(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repository, vcpkg, count_file, output, cache, environment = (
                self.ensure_fixture(root)
            )
            stale = cache / "utf8proc/stale-key"
            stale.mkdir(parents=True)
            abandoned_staging = cache / "utf8proc/.staging-abandoned"
            abandoned_staging.mkdir()
            shutil.rmtree(output / "lib/libutf8proc.xcframework")
            (output / "include/utf8proc.h").unlink()

            result = self.run_prepare(
                "ensure",
                "--repository-root",
                repository,
                "--vcpkg-root",
                vcpkg,
                "--cache-root",
                cache,
                "--output-root",
                output,
                environment=environment,
            )

            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(
                ["called:arm64-ios", "called:arm64-ios-simulator"],
                count_file.read_text().splitlines(),
            )
            self.assertFalse(stale.exists())
            self.assertFalse(abandoned_staging.exists())
            self.assertTrue((output / "include/utf8proc.h").is_file())
            self.assertTrue(
                (output / "lib/libutf8proc.xcframework/Info.plist").is_file()
            )

    def test_verifier_rejects_one_slice_missing_header_and_tampered_archive(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            _, _, _, output, cache, _ = self.ensure_fixture(root)
            framework = output / "lib/libutf8proc.xcframework"
            header = output / "include/utf8proc.h"
            manifest = next((cache / "utf8proc").glob("*/manifest.json"))

            one_slice = root / "one-slice.xcframework"
            shutil.copytree(framework, one_slice)
            shutil.rmtree(one_slice / "ios-arm64-simulator")
            result = self.run_prepare(
                "verify",
                "--framework",
                one_slice,
                "--header",
                header,
                "--license",
                UTF8PROC_LICENSE,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("ios-arm64-simulator", result.stderr)

            result = self.run_prepare(
                "verify",
                "--framework",
                framework,
                "--header",
                root / "missing-utf8proc.h",
                "--license",
                UTF8PROC_LICENSE,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("header", result.stderr.lower())

            tampered = root / "tampered.xcframework"
            shutil.copytree(framework, tampered)
            with (tampered / "ios-arm64/libutf8proc.a").open("ab") as stream:
                stream.write(b"tampered")
            result = self.run_prepare(
                "verify",
                "--framework",
                tampered,
                "--header",
                header,
                "--license",
                UTF8PROC_LICENSE,
                "--manifest",
                manifest,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("artifact error", result.stderr.lower())

    def test_registry_change_rekeys_rebuilds_and_prunes_without_tool_change(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repository, vcpkg, count_file, output, cache, environment = (
                self.ensure_fixture(root)
            )
            executable_digest = hashlib.sha256(
                (vcpkg / "vcpkg").read_bytes()
            ).hexdigest()
            first_entry = next(
                path
                for path in (cache / "utf8proc").iterdir()
                if path.is_dir()
            )
            first_manifest = json.loads(
                (first_entry / "manifest.json").read_text(encoding="utf-8")
            )

            baseline = self.commit_registry_version(
                vcpkg, "2.11.4", "2" * 40
            )
            self.write_repository_manifest(repository, baseline)
            result = self.run_prepare(
                "ensure",
                "--repository-root",
                repository,
                "--vcpkg-root",
                vcpkg,
                "--cache-root",
                cache,
                "--output-root",
                output,
                environment=environment,
            )

            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(
                executable_digest,
                hashlib.sha256((vcpkg / "vcpkg").read_bytes()).hexdigest(),
            )
            self.assertEqual(4, len(count_file.read_text().splitlines()))
            entries = [
                path
                for path in (cache / "utf8proc").iterdir()
                if path.is_dir()
            ]
            self.assertEqual(1, len(entries))
            second_manifest = json.loads(
                (entries[0] / "manifest.json").read_text(encoding="utf-8")
            )
            self.assertNotEqual(
                first_manifest["cacheKey"], second_manifest["cacheKey"]
            )
            self.assertEqual(
                {
                    "builtinBaseline": baseline,
                    "gitTree": "2" * 40,
                    "name": "utf8proc",
                    "portVersion": 0,
                    "version": "2.11.4",
                },
                second_manifest["cacheIdentity"]["dependency"],
            )

    def test_verifier_and_reuse_reject_tampered_manifest_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repository, vcpkg, count_file, _, cache, environment = (
                self.ensure_fixture(root)
            )
            entry = next(
                path
                for path in (cache / "utf8proc").iterdir()
                if path.is_dir()
            )
            manifest_path = entry / "manifest.json"
            original = json.loads(manifest_path.read_text(encoding="utf-8"))
            framework = entry / "lib/libutf8proc.xcframework"
            header = entry / "include/utf8proc.h"
            license_path = repository / "assets/legal/utf8proc.txt"

            tampered_key = json.loads(json.dumps(original))
            tampered_key["cacheKey"] = "0" * 24
            manifest_path.write_text(
                json.dumps(tampered_key, indent=2) + "\n", encoding="utf-8"
            )
            result = self.run_prepare(
                "verify",
                "--framework",
                framework,
                "--header",
                header,
                "--license",
                license_path,
                "--manifest",
                manifest_path,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("cache key", result.stderr.lower())

            tampered_version = json.loads(json.dumps(original))
            tampered_version["cacheIdentity"]["dependency"]["version"] = "9.9.9"
            manifest_path.write_text(
                json.dumps(tampered_version, indent=2) + "\n", encoding="utf-8"
            )
            result = self.run_prepare(
                "verify",
                "--framework",
                framework,
                "--header",
                header,
                "--license",
                license_path,
                "--manifest",
                manifest_path,
            )
            self.assertNotEqual(0, result.returncode)
            self.assertIn("cache key", result.stderr.lower())

            result = self.run_prepare(
                "ensure",
                "--repository-root",
                repository,
                "--vcpkg-root",
                vcpkg,
                "--cache-root",
                cache,
                "--output-root",
                root / "restored-output",
                environment=environment,
            )
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual(4, len(count_file.read_text().splitlines()))
            restored = json.loads(manifest_path.read_text(encoding="utf-8"))
            self.assertEqual("2.11.3", restored["cacheIdentity"]["dependency"]["version"])

    def test_committed_utf8proc_license_is_complete_and_tamper_checked(self):
        license_bytes = UTF8PROC_LICENSE.read_bytes()
        text = license_bytes.decode("utf-8")
        self.assertEqual(
            UTF8PROC_LICENSE_SHA256, hashlib.sha256(license_bytes).hexdigest()
        )
        self.assertIn("Original utf8proc license", text)
        self.assertIn("Unicode data license", text)
        self.assertGreaterEqual(text.count("Permission is hereby granted"), 3)
        self.assertIn("THE DATA FILES AND SOFTWARE ARE PROVIDED", text)

    def test_ios_init_can_prepare_native_dependency_from_a_clean_checkout(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            repository, vcpkg, count_file = self.make_vcpkg_fixture(root)
            output = root / "output"
            cache = root / "cache"
            fake_bin = root / "bin"
            fake_bin.mkdir()
            fake_cmake = fake_bin / "cmake"
            fake_cmake.write_text("#!/bin/sh\nexit 43\n", encoding="utf-8")
            fake_cmake.chmod(0o755)
            env = os.environ.copy()
            env.update(
                {
                    "FAKE_VCPKG_COUNT_FILE": str(count_file),
                    "VCPKG_ROOT": str(vcpkg),
                    "IOS_DEPLOY_CACHE_ROOT": str(cache),
                    "IOS_UTF8PROC_OUTPUT_ROOT": str(output),
                    "IOS_UTF8PROC_REPOSITORY_ROOT": str(repository),
                    "PATH": f"{fake_bin}:{env['PATH']}",
                }
            )

            result = subprocess.run(
                [str(IOS_INIT), "--native-deps-only"],
                cwd=ROOT,
                env=env,
                text=True,
                capture_output=True,
            )

            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue((output / "include/utf8proc.h").is_file())
            self.assertTrue(
                (output / "lib/libutf8proc.xcframework/Info.plist").is_file()
            )
            self.assertEqual(
                ["called:arm64-ios", "called:arm64-ios-simulator"],
                count_file.read_text().splitlines(),
            )


class DerivedDataPathTests(unittest.TestCase):
    def resolve(self, root: Path, **environment: str) -> str:
        env = os.environ.copy()
        env.update(environment)
        return subprocess.run(
            [str(DERIVED_DATA_HELPER), "--root", str(root)],
            cwd=ROOT,
            env=env,
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()

    def test_explicit_override_is_returned_exactly(self):
        self.assertEqual(
            "/tmp/custom-ios-derived-data",
            self.resolve(ROOT, IOS_DERIVED_DATA_PATH="/tmp/custom-ios-derived-data"),
        )

    def test_checkout_path_is_stable_and_isolated(self):
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            first = parent / "checkout-a"
            second = parent / "checkout-b"
            first.mkdir()
            second.mkdir()
            home = parent / "home"
            home.mkdir()
            first_path = self.resolve(first, HOME=str(home), IOS_DERIVED_DATA_PATH="")
            self.assertEqual(
                first_path,
                self.resolve(first, HOME=str(home), IOS_DERIVED_DATA_PATH=""),
            )
            self.assertNotEqual(
                first_path,
                self.resolve(second, HOME=str(home), IOS_DERIVED_DATA_PATH=""),
            )
            self.assertTrue(
                first_path.startswith(
                    str(home / "Library/Developer/Xcode/DerivedData/AsoBMaShow-FirebaseCI-")
                )
            )

    def test_fastlane_and_build_only_share_resolver(self):
        self.assertIn("ios_derived_data_path.sh", DEPLOY_SCRIPT.read_text())
        fastfile = FASTFILE.read_text()
        self.assertIn("ios_derived_data_path.sh", fastfile)
        self.assertIn("clean: false", fastfile)

    def test_firebase_archive_uses_stable_object_root(self):
        fastfile = FASTFILE.read_text()
        self.assertIn("def firebase_archive_objroot(derived_data_path)", fastfile)
        self.assertIn(
            'File.join(derived_data_path, "Build", '
            '"FirebaseArchiveIntermediates.noindex")',
            fastfile,
        )
        self.assertIn(
            'xcargs: "#{xcargs} '
            'OBJROOT=#{Shellwords.escape(firebase_archive_objroot(derived_data_path))}"',
            fastfile,
        )
        self.assertNotIn("SYMROOT=", fastfile)


class PodsCacheTests(unittest.TestCase):
    def bash(self, command: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", "-c", f'source "{PODS_CACHE_HELPER}"; {command}'],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )

    def cache_key(self, podfile: Path, pod_lock: Path, gem_lock: Path) -> str:
        result = self.bash(
            f'ios_pods_cache_key "{podfile}" "{pod_lock}" "{gem_lock}"'
        )
        self.assertEqual(0, result.returncode, result.stderr)
        return result.stdout.strip()

    @staticmethod
    def make_valid_pods(directory: Path, lock: Path, marker: str) -> None:
        (directory / "Pods.xcodeproj").mkdir(parents=True)
        (directory / "Pods.xcodeproj/project.pbxproj").write_text(
            "// generated pods project\n", encoding="utf-8"
        )
        support = directory / "Target Support Files/Pods-AsoBMaShow"
        support.mkdir(parents=True)
        for configuration in ("debug", "release"):
            (support / f"Pods-AsoBMaShow.{configuration}.xcconfig").write_text(
                "PODS_ROOT = ${SRCROOT}/Pods\n", encoding="utf-8"
            )
        (directory / "Manifest.lock").write_bytes(lock.read_bytes())
        (directory / "marker.txt").write_text(marker, encoding="utf-8")

    def test_restore_creates_real_local_directory_and_preserves_timestamp(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lock = root / "Podfile.lock"
            lock.write_text("PODS:\n", encoding="utf-8")
            cache = root / "cache"
            local = root / "Pods"
            self.make_valid_pods(cache, lock, "cached")
            marker_time = 1_700_000_000
            os.utime(cache / "marker.txt", (marker_time, marker_time))

            result = self.bash(
                f'ios_pods_cache_restore "{cache}" "{local}" "{lock}"'
            )
            self.assertEqual(0, result.returncode, result.stderr)
            self.assertTrue(local.is_dir())
            self.assertFalse(local.is_symlink())
            self.assertEqual("cached", (local / "marker.txt").read_text())
            self.assertEqual(marker_time, int((local / "marker.txt").stat().st_mtime))

    def test_cache_key_changes_when_only_podfile_changes(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            podfile = root / "Podfile"
            pod_lock = root / "Podfile.lock"
            gem_lock = root / "Gemfile.lock"
            podfile.write_text("platform :ios, '13.4'\n", encoding="utf-8")
            pod_lock.write_text("PODS:\n", encoding="utf-8")
            gem_lock.write_text("GEM\n", encoding="utf-8")
            original = self.cache_key(podfile, pod_lock, gem_lock)

            podfile.write_text(
                "platform :ios, '13.4'\npost_install { |installer| installer }\n",
                encoding="utf-8",
            )

            self.assertNotEqual(original, self.cache_key(podfile, pod_lock, gem_lock))

    def test_ios_init_includes_podfile_in_cache_key(self):
        script = IOS_INIT.read_text(encoding="utf-8")
        self.assertIn('ios_pods_cache_key "${IOS_DIR}/Podfile"', script)

    def test_invalid_source_does_not_replace_existing_cache(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lock = root / "Podfile.lock"
            lock.write_text("PODS:\n", encoding="utf-8")
            source = root / "Pods"
            source.mkdir()
            cache = root / "cache"
            self.make_valid_pods(cache, lock, "preserved")

            result = self.bash(
                f'ios_pods_cache_store "{source}" "{cache}" "{lock}"'
            )
            self.assertNotEqual(0, result.returncode)
            self.assertEqual("preserved", (cache / "marker.txt").read_text())

    def test_restore_rejects_cache_generated_for_external_pods_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            lock = root / "Podfile.lock"
            lock.write_text("PODS:\n", encoding="utf-8")
            cache = root / "cache"
            local = root / "Pods"
            self.make_valid_pods(cache, lock, "stale")
            xcconfig = (
                cache
                / "Target Support Files/Pods-AsoBMaShow/Pods-AsoBMaShow.release.xcconfig"
            )
            xcconfig.write_text(
                "PODS_ROOT = ${SRCROOT}/../../Library/Caches/Pods\n",
                encoding="utf-8",
            )

            result = self.bash(
                f'ios_pods_cache_restore "{cache}" "{local}" "{lock}"'
            )
            self.assertNotEqual(0, result.returncode)
            self.assertFalse(local.exists())

    def test_ios_init_uses_copy_cache_instead_of_pods_symlink(self):
        script = IOS_INIT.read_text(encoding="utf-8")
        self.assertIn("ios_pods_cache_restore", script)
        self.assertIn("ios_pods_cache_store", script)
        self.assertNotIn('link_cache_dir "${IOS_DIR}/Pods"', script)


if __name__ == "__main__":
    unittest.main()
