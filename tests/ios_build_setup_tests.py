#!/usr/bin/env python3
import os
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj"
WORKSPACE = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcworkspace/contents.xcworkspacedata"
SRC_GROUP_ID = "B76AAF3F2DA4A1C400E8327C"
EXCEPTION_SET_ID = "B76AAF692DA4A1C400E8327C"
TARGET_ID = "B70027002BF7A8D8000DB8EC"
DERIVED_DATA_HELPER = ROOT / "scripts/ios_derived_data_path.sh"
DEPLOY_SCRIPT = ROOT / "scripts/ios_firebase_deploy.sh"
FASTFILE = ROOT / "ios/Xcode/AsoBMaShow/fastlane/Fastfile"
PODS_CACHE_HELPER = ROOT / "scripts/ios_pods_cache.sh"
IOS_INIT = ROOT / "scripts/ios_init.sh"
AGENT_GUIDANCE = ROOT / "AGENTS.md"
SDL_HEADER_ALIAS = ROOT / "ios/Xcode/AsoBMaShow/include/SDL2"


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
        self.assertEqual(["AndroidNatives.cpp", *cmake_files], paths)
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

    def test_ios_uses_portable_stable_sdl_header_alias(self):
        self.assertTrue(SDL_HEADER_ALIAS.is_symlink())
        self.assertFalse(os.path.isabs(os.readlink(SDL_HEADER_ALIAS)))
        self.assertEqual((ROOT / "SDL/include").resolve(), SDL_HEADER_ALIAS.resolve())


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
        self.assertIn("clean: !distribute_to_firebase", fastfile)


class PodsCacheTests(unittest.TestCase):
    def bash(self, command: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["bash", "-c", f'source "{PODS_CACHE_HELPER}"; {command}'],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )

    @staticmethod
    def make_valid_pods(directory: Path, lock: Path, marker: str) -> None:
        (directory / "Pods.xcodeproj").mkdir(parents=True)
        (directory / "Pods.xcodeproj/project.pbxproj").write_text(
            "// generated pods project\n", encoding="utf-8"
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

    def test_ios_init_uses_copy_cache_instead_of_pods_symlink(self):
        script = IOS_INIT.read_text(encoding="utf-8")
        self.assertIn("ios_pods_cache_restore", script)
        self.assertIn("ios_pods_cache_store", script)
        self.assertNotIn('link_cache_dir "${IOS_DIR}/Pods"', script)


if __name__ == "__main__":
    unittest.main()
