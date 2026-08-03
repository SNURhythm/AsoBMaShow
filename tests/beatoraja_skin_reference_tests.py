#!/usr/bin/env python3
import hashlib
import importlib.util
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
from unittest import mock


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


def opaque_guard_vector_sha256(configuration_sha256: str, guard_vector: list[dict]) -> str:
    digest = hashlib.sha256()
    digest.update(b"ASOBMSKIN-OPAQUE-GUARD-VECTOR-V1\0")
    digest.update(bytes.fromhex(configuration_sha256))
    digest.update(struct.pack(">I", len(guard_vector)))
    for guard in sorted(guard_vector, key=lambda item: item["guardId"].encode("utf-8")):
        for field in ("guardId", "value"):
            encoded = guard[field].encode("utf-8")
            digest.update(struct.pack(">I", len(encoded)))
            digest.update(encoded)
    return digest.hexdigest()


def load_audit_module():
    module_name = "asobmashow_beatoraja_skin_audit_test"
    spec = importlib.util.spec_from_file_location(module_name, AUDIT_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


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
        legacy = manifest.get("legacyLuaApiSurface")
        self.assertIsNotNone(legacy, "the exact selected-closure legacy API surface is required")
        self.assertEqual(legacy["module"], "luajava")
        self.assertEqual(legacy["helperCount"], 2)
        self.assertEqual(
            legacy["imports"],
            {
                "siteCount": 2,
                "criticality": "critical",
                "reachability": "unguarded-top-level",
            },
        )
        self.assertEqual(
            legacy["bindClass"],
            [
                {
                    "className": "com.badlogic.gdx.Gdx",
                    "siteCount": 1,
                    "criticality": "critical",
                    "reachability": "unguarded-top-level",
                },
                {
                    "className": "java.io.File",
                    "siteCount": 1,
                    "criticality": "critical",
                    "reachability": "unguarded-top-level",
                },
            ],
        )
        self.assertEqual(
            legacy["fileFacade"],
            {
                "constructorSiteCount": 2,
                "reachableConstructorSiteCount": 1,
                "mkdirSiteCount": 1,
                "mkdirReachableFromSelectedEntry": False,
                "listFilesSiteCount": 1,
                "listFilesReachableFromSelectedEntry": True,
                "criticality": "critical",
                "reachability": "configured-load-listFiles-deferred-mkdir",
            },
        )
        self.assertEqual(
            legacy["audioFacade"],
            {
                "initializationSiteCount": 1,
                "playSiteCount": 1,
                "disposeSiteCount": 1,
                "criticality": "optional",
                "reachability": "pcall-guarded",
            },
        )
        self.assertEqual(
            manifest.get("timerEventOrdering"),
            {
                "phaseOrder": ["customTimers", "customEvents"],
                "withinPhase": "IntMap-backing-hash-iteration",
                "sortedById": False,
                "selectedIdTraceRequired": True,
            },
        )

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
        self.assertEqual(contract["limits"]["activeRenderFilesystemWrites"], 0)
        self.assertEqual(contract["limits"]["activeRenderFilesystemDirectoryScans"], 0)
        self.assertEqual(contract["limits"]["activeRenderResourceUploads"], 0)
        self.assertNotIn("activeRenderUploads", contract["limits"])
        self.assertEqual(contract["limits"]["liveResourceGrowthAfterTenExits"], 0)
        external_digests = contract["externalDigests"]
        self.assertIn("activatedRevisionSha256", external_digests)
        self.assertEqual(
            external_digests["activatedRevisionSha256"],
            {"status": "pending", "value": None},
        )
        self.assertEqual(
            external_digests["configurationSha256"],
            {"status": "pending", "value": None},
        )
        for screenshot in contract["screenshotTimestamps"]:
            self.assertEqual(screenshot["status"], "pending")
            self.assertEqual(screenshot["timestampsMicros"], [])
            self.assertIsNone(screenshot["evidenceReference"])
        self.assertIn("timerEventTrace", contract)
        self.assertEqual(
            contract["timerEventTrace"],
            {
                "status": "pending",
                "selectedIds": [],
                "observedOrder": [],
                "evidenceReference": None,
            },
        )
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

    def test_selected_file_io_surface_is_complete_opaque_and_deterministic(self):
        manifest = self.require_manifest()
        selected = manifest["selectedFileIoSurface"]
        self.assertEqual(selected["schemaVersion"], 1)
        self.assertEqual(selected["authority"], "audited-upstream-selected-closure")
        self.assertEqual(
            manifest["selectedCustomObjectMaps"],
            {"customTimers": 0, "customEvents": 0},
        )

        upstream = selected["upstreamFacts"]
        self.assertEqual(
            upstream["dofile"],
            {
                "siteCount": 20,
                "argumentShape": "one-virtual-path",
                "returnShape": "callee-return-values",
                "phases": ["configured-load"],
            },
        )
        self.assertEqual(upstream["ioOpen"]["siteCount"], 17)
        self.assertEqual(
            upstream["ioOpen"]["modes"],
            [
                {"mode": "default", "siteCount": 4},
                {"mode": "r", "siteCount": 3},
                {"mode": "w", "siteCount": 7},
                {"mode": "a", "siteCount": 3},
            ],
        )
        self.assertEqual(
            upstream["ioOpen"]["phases"],
            ["configured-load", "render-callback"],
        )
        self.assertEqual(
            upstream["handleMethods"],
            [
                {
                    "method": "lines",
                    "siteCount": 6,
                    "argumentShape": "zero",
                    "returnShape": "iterator",
                },
                {
                    "method": "write",
                    "siteCount": 16,
                    "argumentSiteCounts": {"zero": 1, "one": 15, "multiple": 0},
                    "acceptedArgumentShape": "zero-or-more",
                    "returnShape": "same-handle",
                    "chainable": True,
                },
                {
                    "method": "close",
                    "siteCount": 14,
                    "argumentShape": "zero",
                    "returnShape": "true-on-success",
                },
            ],
        )
        self.assertEqual(
            upstream["legacyDirectoryScan"],
            {
                "siteCount": 1,
                "method": "listFiles",
                "phases": ["configured-load"],
                "returnShape": "virtual-path-array-or-nil",
            },
        )
        self.assertEqual(
            upstream["phaseEvidence"],
            [
                {
                    "phase": "configured-load",
                    "operationKinds": [
                        "filesystemRead",
                        "filesystemWrite",
                        "filesystemDirectoryScan",
                    ],
                },
                {
                    "phase": "render-callback",
                    "operationKinds": ["filesystemRead", "filesystemWrite"],
                    "guardCount": 2,
                },
            ],
        )

        expected_configurations = [
            {
                "id": "configuration-aa5abad1b4239c645a388601",
                "role": "passing",
                "auditedGuardConfigurationSha256": "95515ea717f93471dcbf5255a89d27d455865a7cf2121e21fa9bf042371f4f4e",
                "guardVectorSha256": "7d9bd2e9ea2925f354135113fc7d7f5efe9688837f16c334f8cde88b381edd33",
                "guardVector": [
                    {"guardId": "guard-2f076b2588a5528dc738eaa6", "value": "not-reachable"},
                    {"guardId": "guard-81067ade3e053e6cea868e0b", "value": "not-reachable"},
                ],
            },
            {
                "id": "configuration-51f549ae554ff70239c324eb",
                "role": "negative",
                "auditedGuardConfigurationSha256": "031dbe40093326911ce9e724bfa7c5613fef90a8380668ea2c68c79e6eb10761",
                "guardVectorSha256": "c0ce75286dbc3b0b76559e5f03b9eff0a30a154f3ffa95d6266b6aa326b776bb",
                "guardVector": [
                    {"guardId": "guard-2f076b2588a5528dc738eaa6", "value": "reachable"},
                    {"guardId": "guard-81067ade3e053e6cea868e0b", "value": "not-reachable"},
                ],
            },
        ]
        self.assertEqual(selected["auditedGuardConfigurations"], expected_configurations)
        for configuration in selected["auditedGuardConfigurations"]:
            self.assertEqual(
                configuration["guardVectorSha256"],
                opaque_guard_vector_sha256(
                    configuration["auditedGuardConfigurationSha256"],
                    configuration["guardVector"],
                ),
            )

        policy = selected["asoBMaShowPolicy"]
        self.assertEqual(policy["authority"], "AsoBMaShow")
        self.assertEqual(
            policy["nestedWriteParentCreation"],
            "safe-automatic-overlay-parents",
        )
        self.assertEqual(
            policy["renderTransitionHandles"],
            "invalidate-release-read-buffers-discard-unclosed-write-buffers",
        )
        self.assertEqual(policy["dirtyHandleTransition"], "validation-failure")
        self.assertEqual(policy["postTransitionOperationCriticality"], "session-critical")
        self.assertTrue(policy["denyBeforeEffect"])
        self.assertTrue(policy["performedAndDeniedCountersSeparate"])
        self.assertEqual(policy["externalIdentitySerialization"], "opaque-ids-and-digests-only")
        self.assertEqual(policy["serializationOrder"], "deterministic")

        serialized = json.dumps(selected, ensure_ascii=False, sort_keys=True)
        self.assertNotIn(manifest["entries"][0]["path"], serialized)
        self.assertNotRegex(serialized, r"(?:^|[\"/])(?:Play|Root)/")
        self.assertNotRegex(serialized, r"\.lua(?:skin)?")

    def test_render_io_negative_policy_is_frozen(self):
        contract = self.require_manifest()["acceptanceContract"]
        scenarios = contract["negativeScenarios"]
        self.assertEqual(len(scenarios), 1)
        self.assertEqual(
            scenarios[0],
            {
                "id": "scenario-f7395bddf2b0f715a900b5cd",
                "guardConfigurationId": "configuration-51f549ae554ff70239c324eb",
                "auditedGuardConfigurationSha256": "031dbe40093326911ce9e724bfa7c5613fef90a8380668ea2c68c79e6eb10761",
                "expectedGuardVectorSha256": "c0ce75286dbc3b0b76559e5f03b9eff0a30a154f3ffa95d6266b6aa326b776bb",
                "expectedDeniedOperation": "filesystemRead",
                "expectedDiagnostic": "skin_file_render_phase_denied",
                "expectedAction": "discard_frame_disable_session_same_frame_builtin",
                "criticality": "session-critical-sandbox-integrity",
                "performedCountersExpected": {
                    "filesystemReads": 0,
                    "filesystemWrites": 0,
                    "filesystemDirectoryScans": 0,
                    "resourceUploads": 0,
                },
                "deniedCountersExpected": {
                    "filesystemReads": "positive",
                    "filesystemWrites": 0,
                    "filesystemDirectoryScans": 0,
                    "resourceUploads": 0,
                },
                "overlayDigestCapture": "asynchronous-after-session-teardown",
                "overlayDigestBefore": "pending",
                "overlayDigestAfter": "pending",
            },
        )
        self.assertEqual(
            contract["passingGuardVectorSha256"],
            ["7d9bd2e9ea2925f354135113fc7d7f5efe9688837f16c334f8cde88b381edd33"],
        )
        self.assertEqual(
            contract["externalDigests"]["configurationSha256"],
            {"status": "pending", "value": None},
            "static guard evidence must not populate physical SkinConfigurationDigestV1",
        )

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
                "class SkinLuaAccessor { execFile(){} setDirectory(){} exportMainStateAccessor(){} exportSkinProperty(){} class RestrictedIoLib { openFile(){} } }",
            "src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java":
                "class LegacySkinLuaApi { install(){} }",
            "src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java":
                "class MainStatePropertyLuaApiExporter { export(){} "
                "class OptionFunction {} class NumberFunction {} "
                "class FloatNumberFunction {} class TextFunction {} "
                "class TimerFunction {} class EventExecFunction {} "
                "class EventIndexFunction {} }",
            "src/bms/player/beatoraja/skin/json/JSONSkinLoader.java":
                "class JSONSkinLoader { loadJsonSkinHeader(){} loadJsonSkin(){} setDestination(){} }",
            "src/bms/player/beatoraja/skin/json/JsonSkin.java":
                "class JsonSkin { class Skin { "
                "public CustomEvent[] customEvents = new CustomEvent[0]; "
                "public CustomTimer[] customTimers = new CustomTimer[0]; } "
                "class Destination {} class NoteSet {} }",
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
            "play7_hw.luaskin": b'local t = require("fixture.entry")\nreturn t\n',
            "fixture/entry.lua": (
                b'local state = require("main_state")\n'
                b'local helper = require("fixture.helper")\n'
                b'MAIN = { OP = require("fixture.options") }\n'
                b'return { header = { type = 0 }, main = helper.main }\n'
            ),
            "fixture/helper.lua": (
                b'local luajava = require("luajava")\n'
                b'local File = luajava.bindClass("java.io.File")\n'
                b'local m = {}\n'
                b'm.main = function()\n'
                b' local f = io.open("state.txt", "r")\n'
                b' local event_index = main_state.event_index(8123)\n'
                b' local timer = main_state.timer(8124)\n'
                b' main_state.event_exec(8125)\n'
                b' return { image = {{id="i", src="image.png"}}, '
                b'note = {id="notes"}, bga={id="bga"}, '
                b'destination={{id="i", timer=3, draw=function() '
                b'return main_state.option(MAIN.OP.SYNTHETIC_ENABLED) end, '
                b'dst={{x=0,y=0,w=1,h=1}}}} }\n'
                b'end\nreturn m\n'
            ),
            "fixture/options.lua": b'return { SYNTHETIC_ENABLED = 777 }\n',
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
        self.assertNotIn("fixture/helper.lua", serialized)
        self.assertNotIn("image.png", serialized)
        surface_by_key = {
            (item["kind"], item["id"]): item
            for item in manifest["surface"]
        }
        self.assertIn(("property", "boolean:777"), surface_by_key)
        self.assertEqual(
            surface_by_key[("object", "note")]["provenance"][0]["symbol"],
            "SkinNote.prepare",
        )
        self.assertEqual(
            surface_by_key[("module", "host-main-state")]["provenance"][0]["symbol"],
            "SkinLuaAccessor.exportMainStateAccessor",
        )
        self.assertEqual(
            surface_by_key[("file-api", "main_state.option")]["provenance"][0]["symbol"],
            "MainStatePropertyLuaApiExporter.export",
        )
        direct_host_provenance = {
            ("property", "integer:8123"): [
                "MainStatePropertyLuaApiExporter.EventIndexFunction",
                "IntegerPropertyFactory.getImageIndexProperty",
            ],
            ("timer", "8124"): [
                "MainStatePropertyLuaApiExporter.TimerFunction",
            ],
            ("event", "8125"): [
                "MainStatePropertyLuaApiExporter.EventExecFunction",
            ],
        }
        for key, expected_symbols in direct_host_provenance.items():
            with self.subTest(direct_host_surface=key):
                self.assertEqual(
                    [
                        provenance["symbol"]
                        for provenance in surface_by_key[key]["provenance"]
                    ],
                    expected_symbols,
                )

        verified = run_python(
            AUDIT_PATH,
            *self.audit_arguments(reference_root, archive, skin_root, "--verify", manifest_path),
            env=self.tool_env,
        )
        self.assertEqual(verified.returncode, 0, verified.stdout)

        audit = load_audit_module()
        optional_closure = {
            "play7_hw.luaskin": (
                "-- require('comment_only_host')\n"
                "--[[ io.File require('long_comment_host') ]]\n"
                "local text = \"io.File require('string_only_host')\"\n"
                "local critical = require('critical_host')\n"
                "local path = skin_config.get_path('fixture/optional.lua')\n"
                "pcall(function() return dofile(path) end)\n"
                "if enabled then require('conditional_host') end\n"
                "local ok = pcall(require, 'protected_host')\n"
                "return {}\n"
            ),
            "fixture/optional.lua": "return require('fixture.shared')\n",
            "fixture/shared.lua": (
                "local file = io.open('synthetic-state.txt', 'r')\n"
                "return require('optional_descendant_host')\n"
            ),
        }
        loaded, criticality, host_modules = audit.loaded_lua_closure(
            "play7_hw.luaskin",
            optional_closure,
        )
        self.assertEqual(criticality["fixture/optional.lua"], "optional")
        self.assertEqual(criticality["fixture/shared.lua"], "optional")
        self.assertEqual(host_modules["critical_host"], "critical")
        self.assertEqual(host_modules["conditional_host"], "optional")
        self.assertEqual(host_modules["protected_host"], "optional")
        self.assertEqual(host_modules["optional_descendant_host"], "optional")
        self.assertNotIn("comment_only_host", host_modules)
        self.assertNotIn("long_comment_host", host_modules)
        self.assertNotIn("string_only_host", host_modules)
        surface = audit.build_surface(
            "play7_hw.luaskin",
            loaded,
            criticality,
            host_modules,
        )
        self.assertNotIn(
            "io.File",
            {item["id"] for item in surface if item["kind"] == "file-api"},
        )
        optional_io = next(
            item for item in surface
            if item["kind"] == "file-api" and item["id"] == "io.open"
        )
        self.assertEqual(optional_io["criticality"], "optional")

        promoted_closure = dict(optional_closure)
        promoted_closure["play7_hw.luaskin"] = (
            optional_closure["play7_hw.luaskin"]
            + "local bridge = require('fixture.bridge_one')\n"
        )
        promoted_closure["fixture/bridge_one.lua"] = (
            "return require('fixture.bridge_two')\n"
        )
        promoted_closure["fixture/bridge_two.lua"] = (
            "return require('fixture.shared')\n"
        )
        _, promoted_criticality, promoted_hosts = audit.loaded_lua_closure(
            "play7_hw.luaskin",
            promoted_closure,
        )
        self.assertEqual(promoted_criticality["fixture/shared.lua"], "critical")
        self.assertEqual(promoted_hosts["optional_descendant_host"], "critical")
        promoted_surface = audit.build_surface(
            "play7_hw.luaskin",
            {path: promoted_closure[path] for path in promoted_criticality},
            promoted_criticality,
            promoted_hosts,
        )
        promoted_io = next(
            item for item in promoted_surface
            if item["kind"] == "file-api" and item["id"] == "io.open"
        )
        self.assertEqual(promoted_io["criticality"], "critical")

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

        implicit_case_collision = self.temp_path / "implicit-case-collision.zip"
        with zipfile.ZipFile(implicit_case_collision, "w") as output:
            output.writestr("Bundle/Foo/a.lua", b"return {}")
            output.writestr("Bundle/foo/b.lua", b"return {}")
        cases["implicit directory case collision"] = (
            implicit_case_collision,
            "structural path spelling conflict",
        )

        explicit_implicit_collision = self.temp_path / "explicit-implicit-collision.zip"
        with zipfile.ZipFile(explicit_implicit_collision, "w") as output:
            output.writestr("Bundle/Foo/", b"")
            output.writestr("Bundle/foo/a.lua", b"return {}")
        cases["explicit/implicit directory collision"] = (
            explicit_implicit_collision,
            "structural path spelling conflict",
        )

        implicit_nfc_collision = self.temp_path / "implicit-nfc-collision.zip"
        with zipfile.ZipFile(implicit_nfc_collision, "w") as output:
            output.writestr("Bundle/Cafe\u0301/a.lua", b"return {}")
            output.writestr("Bundle/Caf\u00e9/b.lua", b"return {}")
        cases["implicit directory NFC collision"] = (
            implicit_nfc_collision,
            "structural path spelling conflict",
        )

        explicit_nfc_collision = self.temp_path / "explicit-nfc-collision.zip"
        with zipfile.ZipFile(explicit_nfc_collision, "w") as output:
            output.writestr("Bundle/Cafe\u0301/", b"")
            output.writestr("Bundle/Caf\u00e9/a.lua", b"return {}")
        cases["explicit/implicit directory NFC collision"] = (
            explicit_nfc_collision,
            "structural path spelling conflict",
        )

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

        audit = load_audit_module()
        self.assertTrue(
            hasattr(audit, "validate_structural_nodes"),
            "disk and ZIP validation need one shared structural-node table",
        )
        with self.assertRaisesRegex(audit.AuditError, "structural path spelling conflict"):
            audit.validate_structural_nodes(
                ["Foo/a.lua", "foo/b.lua"],
                [],
                label="extracted tree",
            )

        oversized_archive = self.temp_path / "oversized-lua.zip"
        scanner_limit = 16 * 1024 * 1024
        with zipfile.ZipFile(oversized_archive, "w", zipfile.ZIP_STORED) as output:
            output.writestr("Bundle/oversized.lua", b"x" * (scanner_limit + 2))

        original_open = audit.zipfile.ZipFile.open
        source_open_count = 0
        source_read_requests = []
        source_bytes_consumed = 0

        class ObservedLuaStream:
            def __init__(self, stream):
                self.stream = stream

            def __enter__(self):
                self.stream.__enter__()
                return self

            def __exit__(self, *arguments):
                return self.stream.__exit__(*arguments)

            def read(self, size=-1):
                nonlocal source_bytes_consumed
                source_read_requests.append(size)
                chunk = self.stream.read(size)
                source_bytes_consumed += len(chunk)
                return chunk

        def observed_open(zip_file, name, *arguments, **keywords):
            nonlocal source_open_count
            stream = original_open(zip_file, name, *arguments, **keywords)
            filename = name.filename if isinstance(name, zipfile.ZipInfo) else name
            if filename == "Bundle/oversized.lua":
                source_open_count += 1
                if source_open_count == 2:
                    return ObservedLuaStream(stream)
            return stream

        scanner_error = None
        with mock.patch.object(audit.zipfile.ZipFile, "open", observed_open):
            try:
                audit.inspect_archive(oversized_archive, "Bundle")
            except audit.AuditError as error:
                scanner_error = str(error)

        self.assertLessEqual(source_bytes_consumed, scanner_limit + 1)
        self.assertTrue(source_read_requests)
        self.assertLessEqual(max(source_read_requests), scanner_limit + 1)
        self.assertIsNotNone(scanner_error)
        self.assertIn("Lua source exceeds the 16 MiB scanner limit", scanner_error)


if __name__ == "__main__":
    unittest.main()
