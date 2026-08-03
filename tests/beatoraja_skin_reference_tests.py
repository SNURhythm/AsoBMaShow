#!/usr/bin/env python3
import hashlib
import json
import os
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
MANIFEST_PATH = ROOT / "tests/fixtures/beatoraja_skin/reference_manifest.json"
CHECKER_PATH = ROOT / "scripts/check_beatoraja_reference.py"
AUDIT_PATH = ROOT / "scripts/audit_beatoraja_skin.py"
LOWER_SHA256 = __import__("re").compile(r"^[0-9a-f]{64}$")


def run_python(script: Path, *arguments: object, env=None) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(script), *(str(argument) for argument in arguments)],
        cwd=ROOT,
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class BeatorajaSkinCommittedContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = None
        if MANIFEST_PATH.is_file():
            cls.manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))

    def require_manifest(self) -> dict:
        self.assertIsNotNone(self.manifest, "reference manifest must be committed")
        return self.manifest

    def test_required_contract_artifacts_are_committed(self):
        required = (
            "docs/skin-compat/beatoraja-lua-gameplay-contract.md",
            "docs/skin-compat/modernchic-scuro-4.02-acceptance.md",
            "tests/fixtures/beatoraja_skin/reference_manifest.json",
            "tests/fixtures/beatoraja_skin/README.md",
            "scripts/check_beatoraja_reference.py",
            "scripts/audit_beatoraja_skin.py",
        )
        for relative_path in required:
            with self.subTest(path=relative_path):
                self.assertTrue((ROOT / relative_path).is_file(), relative_path)

    def test_committed_contract_is_clone_independent(self):
        manifest = self.require_manifest()
        self.assertEqual(manifest["schemaVersion"], 1)
        self.assertEqual(manifest["beatorajaCommit"], PINNED_COMMIT)
        self.assertEqual(manifest["acceptanceContract"]["schemaVersion"], 1)
        self.assertEqual(manifest["targetVersion"], "4.02")

    def test_archive_and_source_tree_are_cryptographically_bound(self):
        manifest = self.require_manifest()
        for field in (
            "archiveSha256",
            "archivePayloadTreeSha256",
            "auditedSourceTreeSha256",
        ):
            self.assertRegex(manifest[field], LOWER_SHA256, field)
        self.assertEqual(
            manifest["archivePayloadTreeSha256"],
            manifest["auditedSourceTreeSha256"],
        )
        self.assertNotEqual(manifest["archivePackagePrefix"], "")

    def test_selected_entry_and_surface_have_complete_dispositions(self):
        manifest = self.require_manifest()
        entries = manifest["entries"]
        self.assertTrue(
            any(entry["format"] == "lua" and entry["keys"] == 7 for entry in entries)
        )
        surface = manifest["surface"]
        required_kinds = {"object", "property", "timer", "event", "module", "file-api"}
        self.assertTrue(required_kinds.issubset({item["kind"] for item in surface}))
        for item in surface:
            with self.subTest(kind=item.get("kind"), item_id=item.get("id")):
                self.assertIn(item["criticality"], {"critical", "optional"})
                self.assertTrue(item["provenance"])
                for provenance in item["provenance"]:
                    self.assertEqual(provenance["commit"], PINNED_COMMIT)
                    self.assertTrue(provenance["path"])
                    self.assertTrue(provenance["symbol"])
                    self.assertTrue(provenance["behavior"])

    def test_acceptance_schema_freezes_device_protocol_and_completion_evidence(self):
        contract = self.require_manifest()["acceptanceContract"]
        self.assertEqual(contract["schemaVersion"], 1)
        self.assertEqual(contract["protocol"]["warmupSeconds"], 30)
        self.assertEqual(contract["protocol"]["measurementSeconds"], 180)
        self.assertEqual(contract["protocol"]["repetitions"], 3)
        self.assertEqual(
            {(case["aspect"], case["mode"]) for case in contract["layouts"]},
            {
                ("16:9", "fit"),
                ("16:9", "stretch"),
                ("16:9", "custom"),
                ("4:3", "fit"),
                ("4:3", "stretch"),
                ("4:3", "custom"),
            },
        )
        self.assertEqual(contract["limits"]["p99SkinCpuFrameFraction"], 0.9)
        self.assertEqual(contract["limits"]["missedPresentationPercent"], 0.5)
        self.assertEqual(contract["limits"]["residentMemoryDriftMiB"], 32)
        self.assertEqual(contract["limits"]["activeRenderFilesystemReads"], 0)
        self.assertEqual(contract["limits"]["activeRenderUploads"], 0)
        self.assertEqual(contract["limits"]["liveResourceGrowthAfterTenExits"], 0)
        for key in (
            "hardwareModel",
            "iPadOS",
            "drawableSize",
            "safeInsets",
            "configuredHz",
            "measurementBuild",
            "externalDigests",
            "syntheticChartHashes",
            "autoplayScripts",
            "screenshotTimestamps",
        ):
            self.assertIn(key, contract)
        self.assertTrue(contract["completionCriteria"])
        for criterion in contract["completionCriteria"]:
            self.assertIn(criterion["status"], {"pending", "pass", "fail"})
            self.assertIn("evidenceReference", criterion)

    def test_external_payload_digest_set_covers_every_sensitive_file_kind(self):
        payloads = self.require_manifest()["externalPayloadDigests"]
        self.assertTrue(
            {"lua", "font", "audio", "video", "archive", "image"}.issubset(
                {payload["kind"] for payload in payloads}
            )
        )
        for payload in payloads:
            with self.subTest(payload=payload.get("id")):
                self.assertRegex(payload["id"], r"^payload-[0-9a-f]{24}$")
                self.assertIsInstance(payload["byteCount"], int)
                self.assertGreaterEqual(payload["byteCount"], 0)
                self.assertRegex(payload["sha256"], LOWER_SHA256)
                self.assertNotIn("path", payload)

    def test_no_external_payload_is_tracked_anywhere_in_git(self):
        manifest = self.require_manifest()
        external_hashes = {
            payload["sha256"] for payload in manifest["externalPayloadDigests"]
        }
        tracked = subprocess.run(
            ["git", "ls-files", "-z"],
            cwd=ROOT,
            check=True,
            stdout=subprocess.PIPE,
        ).stdout.split(b"\0")
        collisions = []
        for raw_path in tracked:
            if not raw_path:
                continue
            path = ROOT / os.fsdecode(raw_path)
            if path.is_file() and sha256(path) in external_hashes:
                collisions.append(os.fsdecode(raw_path))
        self.assertEqual(collisions, [])


class BeatorajaReferenceToolBehaviorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="beatoraja-reference-test-")
        self.temp_path = Path(self.temp.name)
        self.fake_bin = self.temp_path / "bin"
        self.fake_bin.mkdir()
        fake_git = self.fake_bin / "git"
        fake_git.write_text(
            "#!/bin/sh\n"
            "root=''\n"
            "if test \"$1\" = '-C'; then root=$2; shift 2; fi\n"
            "if test \"$1 $2\" = 'rev-parse HEAD'; then cat \"$root/.head\"; exit 0; fi\n"
            "if test \"$1 $2\" = 'status --porcelain'; then "
            "test -f \"$root/.dirty\" && echo ' M source'; exit 0; fi\n"
            "exit 2\n",
            encoding="utf-8",
        )
        fake_git.chmod(fake_git.stat().st_mode | stat.S_IXUSR)
        self.tool_env = os.environ.copy()
        self.tool_env["PATH"] = str(self.fake_bin) + os.pathsep + self.tool_env["PATH"]

    def tearDown(self):
        self.temp.cleanup()

    def make_reference_root(self) -> Path:
        root = self.temp_path / "beatoraja"
        root.mkdir()
        (root / ".head").write_text(PINNED_COMMIT + "\n", encoding="ascii")
        sources = {
            "src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java":
                "class LuaSkinLoader { loadHeader(){} load(){} fromLuaValue(){} serializeLuaScript(){} }",
            "src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java":
                "class SkinLuaAccessor { execFile(){} setDirectory(){} exportSkinProperty(){} }",
            "src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java":
                "class LegacySkinLuaApi { install(){} }",
            "src/bms/player/beatoraja/skin/json/JSONSkinLoader.java":
                "class JSONSkinLoader { loadJsonSkinHeader(){} loadJsonSkin(){} setDestination(){} }",
            "src/bms/player/beatoraja/skin/json/JsonSkin.java":
                "class JsonSkin { class Skin {} class Destination {} class NoteSet {} }",
            "src/bms/player/beatoraja/skin/SkinLoader.java":
                "class SkinLoader { load(){} }",
            "src/bms/player/beatoraja/skin/SkinHeader.java":
                "class SkinHeader { setSkinConfigProperty(){} }",
            "src/bms/player/beatoraja/skin/Skin.java":
                "class Skin { prepare(){} drawAllObjects(){} updateCustomObjects(){} }",
            "src/bms/player/beatoraja/skin/SkinObject.java":
                "class SkinObject { prepareRegion(){} getRate(){} prepare(){} }",
            "src/bms/player/beatoraja/play/PlaySkin.java":
                "class PlaySkin {}",
            "src/bms/player/beatoraja/play/SkinNote.java":
                "class SkinNote { prepare(){} draw(){} }",
            "src/bms/player/beatoraja/play/LaneRenderer.java":
                "class LaneRenderer { drawLongNote(){} }",
            "src/bms/player/beatoraja/play/SkinBGA.java":
                "class SkinBGA { prepare(){} draw(){} }",
            "src/bms/player/beatoraja/play/bga/BGAProcessor.java":
                "class BGAProcessor { prepareBGA(){} drawBGA(){} }",
            "src/bms/player/beatoraja/skin/property/BooleanPropertyFactory.java":
                "class BooleanPropertyFactory { getBooleanProperty(){} }",
            "src/bms/player/beatoraja/skin/property/IntegerPropertyFactory.java":
                "class IntegerPropertyFactory { getIntegerProperty(){} getImageIndexProperty(){} }",
            "src/bms/player/beatoraja/skin/property/FloatPropertyFactory.java":
                "class FloatPropertyFactory { getRateProperty(){} getRateWriter(){} }",
            "src/bms/player/beatoraja/skin/property/StringPropertyFactory.java":
                "class StringPropertyFactory { getStringProperty(){} getStringWriter(){} }",
            "src/bms/player/beatoraja/skin/property/TimerPropertyFactory.java":
                "class TimerPropertyFactory { getTimerProperty(){} }",
            "src/bms/player/beatoraja/skin/property/EventFactory.java":
                "class EventFactory { getEvent(){} }",
        }
        for relative_path, source in sources.items():
            path = root / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source, encoding="utf-8")
        return root

    def make_skin_archive(self):
        skin_root = self.temp_path / "skin"
        files = {
            "play7_hw.luaskin": b'local t = require("play7_hw")\nreturn t\n',
            "play7_hw.lua": (
                b'local state = require("main_state")\n'
                b'local module = require("module")\n'
                b'return { header = { type = 0 }, main = module.main }\n'
            ),
            "module.lua": (
                b'local luajava = require("luajava")\n'
                b'local File = luajava.bindClass("java.io.File")\n'
                b'local m = {}\n'
                b'm.main = function()\n'
                b' local f = io.open("state.txt", "r")\n'
                b' return { image = {{id="i", src="image.png"}}, '
                b'note = {id="notes"}, bga={id="bga"}, '
                b'destination={{id="i", timer=3, draw=function() '
                b'return main_state.option(42) end, dst={{x=0,y=0,w=1,h=1}}}} }\n'
                b'end\nreturn m\n'
            ),
            "image.png": b"synthetic-image",
            "font.ttf": b"synthetic-font",
            "sound.ogg": b"synthetic-audio",
            "movie.mp4": b"synthetic-video",
            "nested.zip": b"synthetic-archive",
        }
        for relative_path, content in files.items():
            path = skin_root / relative_path
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        archive = self.temp_path / "Synthetic402.zip"
        with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as output:
            output.writestr("Bundle/", b"")
            output.writestr("Bundle/assets/", b"")
            for relative_path, content in files.items():
                output.writestr(f"Bundle/{relative_path}", content)
        return archive, skin_root

    def audit_arguments(self, reference_root, archive, skin_root, mode, manifest_path):
        return (
            "--beatoraja-root", reference_root,
            "--archive-path", archive,
            "--archive-package-prefix", "Bundle",
            "--skin-root", skin_root,
            "--expected-archive-sha256", sha256(archive),
            mode, manifest_path,
        )

    def test_checker_accepts_only_the_exact_commit_and_optional_clean_tree(self):
        self.assertTrue(CHECKER_PATH.is_file(), str(CHECKER_PATH))
        reference_root = self.make_reference_root()
        result = run_python(CHECKER_PATH, "--root", reference_root, "--require-clean", env=self.tool_env)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn(PINNED_COMMIT, result.stdout)

        (reference_root / ".dirty").touch()
        dirty = run_python(CHECKER_PATH, "--root", reference_root, "--require-clean", env=self.tool_env)
        self.assertNotEqual(dirty.returncode, 0, dirty.stdout)
        (reference_root / ".dirty").unlink()
        (reference_root / ".head").write_text("0" * 40 + "\n", encoding="ascii")
        wrong = run_python(CHECKER_PATH, "--root", reference_root, env=self.tool_env)
        self.assertNotEqual(wrong.returncode, 0, wrong.stdout)

    def test_audit_hashes_archive_and_extracted_tree_and_verifies_deterministically(self):
        self.assertTrue(AUDIT_PATH.is_file(), str(AUDIT_PATH))
        reference_root = self.make_reference_root()
        archive, skin_root = self.make_skin_archive()
        manifest_path = self.temp_path / "manifest.json"
        result = run_python(
            AUDIT_PATH,
            *self.audit_arguments(reference_root, archive, skin_root, "--output", manifest_path),
            env=self.tool_env,
        )
        self.assertEqual(result.returncode, 0, result.stdout)
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        self.assertEqual(manifest["archiveSha256"], sha256(archive))
        self.assertEqual(
            manifest["archivePayloadTreeSha256"], manifest["auditedSourceTreeSha256"]
        )
        self.assertEqual(manifest["archivePackagePrefix"], "Bundle")
        self.assertTrue(any(item["kind"] == "file-api" for item in manifest["surface"]))
        luajava_apis = [
            item
            for item in manifest["surface"]
            if item["kind"] == "file-api" and item["id"].startswith("luajava.")
        ]
        self.assertTrue(luajava_apis)
        for item in luajava_apis:
            self.assertEqual(
                item["provenance"][0]["symbol"],
                "LegacySkinLuaApi.install",
            )
        self.assertTrue(
            any(
                item["kind"] == "module"
                and item["provenance"][0]["symbol"] == "LegacySkinLuaApi.install"
                for item in manifest["surface"]
            )
        )
        serialized = json.dumps(manifest, ensure_ascii=False)
        self.assertNotIn(str(self.temp_path), serialized)
        self.assertNotIn("module.lua", serialized)
        self.assertNotIn("image.png", serialized)

        verified = run_python(
            AUDIT_PATH,
            *self.audit_arguments(reference_root, archive, skin_root, "--verify", manifest_path),
            env=self.tool_env,
        )
        self.assertEqual(verified.returncode, 0, verified.stdout)

    def test_audit_rejects_prefix_tree_digest_and_archive_digest_mismatches(self):
        self.assertTrue(AUDIT_PATH.is_file(), str(AUDIT_PATH))
        reference_root = self.make_reference_root()
        archive, skin_root = self.make_skin_archive()
        manifest_path = self.temp_path / "manifest.json"

        common = [
            "--beatoraja-root", reference_root,
            "--archive-path", archive,
            "--skin-root", skin_root,
            "--output", manifest_path,
        ]
        wrong_prefix = run_python(
            AUDIT_PATH,
            *common,
            "--archive-package-prefix", "Wrong",
            env=self.tool_env,
        )
        self.assertNotEqual(wrong_prefix.returncode, 0, wrong_prefix.stdout)
        wrong_digest = run_python(
            AUDIT_PATH,
            *common,
            "--archive-package-prefix", "Bundle",
            "--expected-archive-sha256", "0" * 64,
            env=self.tool_env,
        )
        self.assertNotEqual(wrong_digest.returncode, 0, wrong_digest.stdout)
        (skin_root / "image.png").write_bytes(b"changed")
        wrong_tree = run_python(
            AUDIT_PATH,
            *common,
            "--archive-package-prefix", "Bundle",
            env=self.tool_env,
        )
        self.assertNotEqual(wrong_tree.returncode, 0, wrong_tree.stdout)

    def test_audit_rejects_unsafe_zip_entries_and_accepts_structural_directories(self):
        self.assertTrue(AUDIT_PATH.is_file(), str(AUDIT_PATH))
        reference_root = self.make_reference_root()
        _, skin_root = self.make_skin_archive()

        cases = {}
        traversal = self.temp_path / "traversal.zip"
        with zipfile.ZipFile(traversal, "w") as output:
            output.writestr("Bundle/../escape.lua", b"return {}")
        cases["traversal"] = (traversal, "unsafe archive path component")

        duplicate = self.temp_path / "duplicate.zip"
        with zipfile.ZipFile(duplicate, "w") as output:
            output.writestr("Bundle/a.lua", b"one")
            output.writestr("Bundle/./a.lua", b"two")
        cases["normalized duplicate"] = (duplicate, "unsafe archive path component")

        case_collision = self.temp_path / "case-collision.zip"
        with zipfile.ZipFile(case_collision, "w") as output:
            output.writestr("Bundle/A.lua", b"one")
            output.writestr("Bundle/a.lua", b"two")
        cases["case collision"] = (case_collision, "duplicate or colliding ZIP path")

        symlink = self.temp_path / "symlink.zip"
        link = zipfile.ZipInfo("Bundle/link")
        link.create_system = 3
        link.external_attr = (stat.S_IFLNK | 0o777) << 16
        with zipfile.ZipFile(symlink, "w") as output:
            output.writestr(link, b"target")
        cases["link"] = (symlink, "link or special ZIP entry")

        collision = self.temp_path / "file-directory-collision.zip"
        with zipfile.ZipFile(collision, "w") as output:
            output.writestr("Bundle/a", b"file")
            output.writestr("Bundle/a/b.lua", b"child")
        cases["file/directory collision"] = (collision, "file/directory collision")

        for label, (archive, expected_error) in cases.items():
            with self.subTest(label=label):
                result = run_python(
                    AUDIT_PATH,
                    "--beatoraja-root", reference_root,
                    "--archive-path", archive,
                    "--archive-package-prefix", "Bundle",
                    "--skin-root", skin_root,
                    "--output", self.temp_path / f"{label}.json",
                    env=self.tool_env,
                )
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(expected_error, result.stdout)


if __name__ == "__main__":
    unittest.main()
