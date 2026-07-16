#!/usr/bin/env python3
import subprocess
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcodeproj/project.pbxproj"
WORKSPACE = ROOT / "ios/Xcode/AsoBMaShow/AsoBMaShow.xcworkspace/contents.xcworkspacedata"
SRC_GROUP_ID = "B76AAF3F2DA4A1C400E8327C"
EXCEPTION_SET_ID = "B76AAF692DA4A1C400E8327C"
TARGET_ID = "B70027002BF7A8D8000DB8EC"


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

    def test_android_native_is_only_source_membership_exception(self):
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
        self.assertEqual(["AndroidNatives.cpp"], paths)
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


if __name__ == "__main__":
    unittest.main()
