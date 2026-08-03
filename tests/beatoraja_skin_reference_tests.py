#!/usr/bin/env python3
import base64
import copy
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
PINNED_SELECTED_LUA_CLOSURE_SHA256 = "717b46b6641c84e431490fff24f45a0ee23a1208017cc4dae4ea2cad438f5bb0"
MANIFEST_PATH = ROOT / "tests/fixtures/beatoraja_skin/reference_manifest.json"
CHECKER_PATH = ROOT / "scripts/check_beatoraja_reference.py"
AUDIT_PATH = ROOT / "scripts/audit_beatoraja_skin.py"
TRACE_CAPTURE_PATH = ROOT / "scripts/capture_beatoraja_skin_traces.py"
TRACE_ROOT = ROOT / "tests/fixtures/beatoraja_skin/traces"
SANDBOX_POLICY_PATH = (
    ROOT / "tests/fixtures/beatoraja_skin/policies/lua_sandbox_v1.json"
)
TWO_PHASE_ENTRY_PATH = (
    ROOT / "tests/fixtures/beatoraja_skin/lua/two_phase/entry.luaskin"
)
TWO_PHASE_SHARED_PATH = (
    ROOT / "tests/fixtures/beatoraja_skin/lua/two_phase/shared.lua"
)
SANDBOX_PROBE_PATH = (
    ROOT / "tests/fixtures/beatoraja_skin/lua/sandbox_probe.luaskin"
)
TREE_DIGEST_PARITY_ROOT = ROOT / "tests/fixtures/skin_tree_digest_v1"
GAMEPLAY_CONTRACT_PATH = ROOT / "docs/skin-compat/beatoraja-lua-gameplay-contract.md"
ACCEPTANCE_PATH = ROOT / "docs/skin-compat/modernchic-scuro-4.02-acceptance.md"
LOWER_SHA256 = __import__("re").compile(r"^[0-9a-f]{64}$")

TRACE_FILES = {
    "lua-language": "lua_language_v1.json",
    "destination": "destination_v1.json",
    "properties": "properties_v1.json",
    "timers-events": "timers_events_v1.json",
    "legacy-lua-upstream": "legacy_lua_upstream_v1.json",
}


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


def canonical_json_sha256(value: object) -> str:
    encoded = json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def opaque_guard_vector_sha256(configuration_sha256: str, guard_vector: list[dict]) -> str:
    digest = hashlib.sha256()
    digest.update(b"ASOBMSKIN-OPAQUE-GUARD-VECTOR-V2\0")
    digest.update(bytes.fromhex(configuration_sha256))
    digest.update(struct.pack(">I", len(guard_vector)))
    for guard in sorted(guard_vector, key=lambda item: item["guardId"].encode("utf-8")):
        for field in ("guardId", "optionId", "choiceId", "value"):
            encoded = guard[field].encode("utf-8")
            digest.update(struct.pack(">I", len(encoded)))
            digest.update(encoded)
    return digest.hexdigest()


def effective_configuration_sha256(
    selected_revision_sha256: str,
    entry_sha256: str,
    option_selections: list[dict],
) -> str:
    digest = hashlib.sha256()
    digest.update(b"ASOBMSKIN-AUDITED-EFFECTIVE-CONFIG-V2\0")
    digest.update(bytes.fromhex(selected_revision_sha256))
    digest.update(bytes.fromhex(entry_sha256))
    ordered = sorted(option_selections, key=lambda item: item["optionId"].encode("utf-8"))
    digest.update(struct.pack(">I", len(ordered)))
    for selection in ordered:
        for field in ("optionId", "choiceId"):
            encoded = selection[field].encode("utf-8")
            digest.update(struct.pack(">I", len(encoded)))
            digest.update(encoded)
    return digest.hexdigest()


def selected_lua_closure_sha256(source_bytes: dict[str, bytes]) -> str:
    digest = hashlib.sha256()
    digest.update(b"ASOBMSKIN-SELECTED-LUA-CLOSURE-V1\0")
    digest.update(struct.pack(">I", len(source_bytes)))
    for path in sorted(source_bytes, key=lambda value: value.encode("utf-8")):
        encoded_path = path.encode("utf-8")
        encoded_source = source_bytes[path]
        digest.update(struct.pack(">I", len(encoded_path)))
        digest.update(encoded_path)
        digest.update(struct.pack(">Q", len(encoded_source)))
        digest.update(encoded_source)
    return digest.hexdigest()


def load_audit_module():
    module_name = "asobmashow_beatoraja_skin_audit_test"
    spec = importlib.util.spec_from_file_location(module_name, AUDIT_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def load_capture_module():
    module_name = "asobmashow_beatoraja_skin_trace_capture_test"
    spec = importlib.util.spec_from_file_location(module_name, TRACE_CAPTURE_PATH)
    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


def mutate_first_scalar(value: object) -> object:
    if isinstance(value, bool):
        return not value
    if isinstance(value, int):
        return value + 1
    if isinstance(value, float):
        return value + 0.125
    if isinstance(value, str):
        return value + "-mutation"
    if value is None:
        return "mutation"
    if isinstance(value, list):
        if not value:
            return ["mutation"]
        result = copy.deepcopy(value)
        result[0] = mutate_first_scalar(result[0])
        return result
    if isinstance(value, dict):
        if not value:
            return {"mutation": True}
        result = copy.deepcopy(value)
        first_key = next(iter(result))
        result[first_key] = mutate_first_scalar(result[first_key])
        return result
    raise AssertionError(f"unsupported mutation value: {value!r}")


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
            "scripts/capture_beatoraja_skin_traces.py",
            "tests/fixtures/beatoraja_skin/lua/two_phase/entry.luaskin",
            "tests/fixtures/beatoraja_skin/lua/two_phase/shared.lua",
            "tests/fixtures/beatoraja_skin/lua/sandbox_probe.luaskin",
            "tests/fixtures/beatoraja_skin/policies/lua_sandbox_v1.json",
            *(f"tests/fixtures/beatoraja_skin/traces/{name}" for name in TRACE_FILES.values()),
        )
        for relative_path in required:
            with self.subTest(path=relative_path):
                self.assertTrue((ROOT / relative_path).is_file(), relative_path)

    def test_skin_tree_digest_v1_shared_fixture_matches_task1_audit(self):
        audit = load_audit_module()
        expected = (TREE_DIGEST_PARITY_ROOT / "expected.sha256").read_text(
            encoding="ascii"
        ).strip()
        actual, regular = audit.inspect_disk_tree(TREE_DIGEST_PARITY_ROOT / "tree")
        self.assertEqual(expected, actual)
        self.assertEqual(["a.txt", "nested/b.txt"], [item.path for item in regular])

    def load_trace(self, kind: str) -> dict:
        path = TRACE_ROOT / TRACE_FILES[kind]
        self.assertTrue(path.is_file(), f"missing committed trace: {path.relative_to(ROOT)}")
        return json.loads(path.read_text(encoding="utf-8"))

    def trace_case(self, kind: str, name: str) -> dict:
        trace = self.load_trace(kind)
        matches = [case for case in trace["cases"] if case["name"] == name]
        self.assertEqual(len(matches), 1, f"expected one {kind} case named {name}")
        return matches[0]

    def test_upstream_trace_envelopes_are_pinned_clone_independent_contracts(self):
        for expected_kind, filename in TRACE_FILES.items():
            with self.subTest(trace=filename):
                trace = self.load_trace(expected_kind)
                self.assertEqual(trace["schemaVersion"], 1)
                self.assertEqual(trace["kind"], expected_kind)
                self.assertEqual(trace["referenceCommit"], PINNED_COMMIT)
                self.assertTrue(trace["provenance"])
                self.assertTrue(trace["cases"])
                for provenance in trace["provenance"]:
                    self.assertEqual(provenance["commit"], PINNED_COMMIT)
                    self.assertFalse(Path(provenance["path"]).is_absolute())
                    self.assertTrue(provenance["path"])
                    self.assertTrue(provenance["symbol"])
                    self.assertTrue(provenance["behavior"])
                names = [case["name"] for case in trace["cases"]]
                self.assertEqual(names, list(dict.fromkeys(names)))
                for case in trace["cases"]:
                    self.assertTrue(case["name"])
                    self.assertIsInstance(case["input"], dict)
                    self.assertIsInstance(case["expected"], dict)
                    self.assertIsInstance(case["precision"], (int, float))
                    self.assertGreaterEqual(case["precision"], 0)
                    if "callOrder" in case:
                        self.assertIsInstance(case["callOrder"], list)
                        self.assertTrue(all(isinstance(value, str) for value in case["callOrder"]))
                    if "callCount" in case:
                        self.assertIsInstance(case["callCount"], dict)
                        self.assertTrue(all(
                            isinstance(value, int) and value >= 0
                            for value in case["callCount"].values()
                        ))

    def test_two_phase_trace_is_stable(self):
        trace = self.load_trace("lua-language")
        case = trace["cases"][0]
        self.assertEqual(trace["referenceCommit"], PINNED_COMMIT)
        self.assertEqual(case["name"], "two-phase-require-before-branch")
        self.assertEqual(case["expected"]["packageLoaded"], True)
        self.assertEqual(case["input"]["entry"], "lua/two_phase/entry.luaskin")
        self.assertEqual(case["input"]["requiredModule"], "shared")
        self.assertTrue(case["input"]["requirePrecedesSkinConfigBranch"])
        self.assertEqual(case["expected"]["header"]["moduleLoadCount"], 1)
        self.assertEqual(case["expected"]["configuredSameState"]["moduleLoadCount"], 1)
        self.assertTrue(case["expected"]["configuredSameState"]["sameModuleTable"])
        self.assertEqual(
            case["expected"]["configuredSameState"]["observedHeaderMutation"],
            "header-mutated",
        )
        self.assertFalse(case["expected"]["freshCatalogBeforeExecution"]["packageLoaded"])
        self.assertEqual(case["expected"]["freshCatalogBeforeExecution"]["moduleLoadCount"], 0)
        self.assertIsNone(case["expected"]["freshCatalogBeforeExecution"]["globalMutation"])
        self.assertEqual(case["callOrder"], ["header", "configured-same-state"])
        self.assertEqual(case["callCount"], {"entryExecutions": 2, "sharedModuleLoads": 1})

    def test_lua_language_trace_freezes_conversions_and_bit32(self):
        conversions = self.trace_case("lua-language", "lua-value-conversions")
        self.assertEqual(
            conversions["expected"],
            {
                "boolean": True,
                "integer": 17,
                "float": 12.5,
                "stringFromNumber": "42",
                "nonTableArrayLength": 0,
                "orderedTableValues": ["first", "named", "second"],
            },
        )
        bit32 = self.trace_case("lua-language", "bit32-operations")
        self.assertEqual(
            bit32["expected"],
            {
                "band": 240,
                "bor": 4095,
                "bxor": 255,
                "bnot": 4294967295,
                "lshift": 2147483648,
                "rshift": 2147483647,
                "arshift": 4294967295,
                "extract": 15,
                "replace": 42405,
            },
        )

    def test_destination_trace_freezes_timer_loop_easing_and_geometry(self):
        timer_off = self.trace_case("destination", "timer-off-suppresses-draw")
        self.assertEqual(timer_off["input"]["timerStartMicros"], -9223372036854775808)
        self.assertEqual(timer_off["expected"], {"draw": False})
        loop = self.trace_case("destination", "loop-and-easing-vectors")
        self.assertFalse(loop["expected"]["stopDraw"])
        self.assertEqual(loop["expected"]["wrappedTimeMillis"], 650)
        self.assertEqual(
            loop["expected"]["easingRates"],
            {"linear": 0.25, "easeIn": 0.0625, "easeOut": 0.4375, "step": 0.0},
        )
        geometry = self.trace_case("destination", "color-angle-clip-offset-interpolation")
        self.assertEqual(
            geometry["expected"],
            {
                "rect": {"x": 24.0, "y": 42.0, "w": 140.0, "h": 70.0},
                "rgbaNormalized": [0.5, 0.2509804, 0.1254902, 0.7509804],
                "angle": 50,
                "clip": {"x": 9.0, "y": 12.0, "w": 100.0, "h": 50.0},
            },
        )

    def test_property_trace_freezes_dispatch_and_missing_semantics(self):
        dispatch = self.trace_case("properties", "model-property-dispatch")
        self.assertEqual(
            dispatch["expected"]["forms"],
            {
                "function": "runtime-callback",
                "numeric": "built-in-id",
                "recognizedString": "built-in-name",
                "unrecognizedString": "runtime-script",
                "other": "unsupported-null",
            },
        )
        self.assertEqual(dispatch["expected"]["timerNegativeId"], "unsupported-null")
        direct = self.trace_case("properties", "direct-main-state-dispatch")
        self.assertEqual(
            direct["expected"]["factories"],
            {
                "option": "BooleanPropertyFactory.getBooleanProperty",
                "number": "IntegerPropertyFactory.getIntegerProperty",
                "float_number": "FloatPropertyFactory.getRateProperty",
                "text": "StringPropertyFactory.getStringProperty",
                "event_index": "IntegerPropertyFactory.getImageIndexProperty",
                "timer": "MainState.timer.getMicroTimer",
                "event_exec": "MainState.executeEvent",
            },
        )
        self.assertEqual(direct["expected"]["unknownDirectLookup"], "NullPointerException")

    def test_timer_event_trace_freezes_off_sentinel_calls_and_order(self):
        sentinel = self.trace_case("timers-events", "timer-off-sentinel")
        self.assertEqual(sentinel["expected"]["offValue"], -9223372036854775808)
        self.assertEqual(sentinel["expected"]["eventAtZeroIsOn"], True)
        caching = self.trace_case("timers-events", "custom-timer-once-per-frame")
        self.assertEqual(caching["expected"]["readsWithinFrame"], [123456, 123456])
        self.assertEqual(caching["expected"]["nextFrameRead"], 234567)
        self.assertEqual(caching["callCount"], {"frame1TimerFunction": 1, "frame2TimerFunction": 1})
        ordered = self.trace_case("timers-events", "timer-phase-before-event-phase")
        self.assertEqual(
            ordered["callOrder"],
            ["timer:3", "timer:17", "event:4", "event:9"],
        )
        self.assertEqual(ordered["expected"]["eventObservedTimerValue"], 7000)
        events = self.trace_case("timers-events", "zero-one-two-argument-events")
        self.assertEqual(events["expected"]["observedArguments"], [[], [11], [11, 0]])
        self.assertEqual(events["callCount"], {"zeroArg": 1, "oneArg": 1, "twoArg": 1})

    def test_intmap_trace_records_complete_trace_specific_rng_setup(self):
        case = self.trace_case("timers-events", "libgdx-intmap-backing-order")
        setup = case["input"]["traceSetup"]
        self.assertEqual(setup["requestedCapacity"], 4)
        self.assertEqual(setup["initialBackingCapacity"], 8)
        self.assertEqual(setup["loadFactor"], 0.8)
        self.assertEqual(setup["randomImplementation"], "java.util.Random-compatible-lcg48")
        self.assertIsInstance(setup["randomSeed"], int)
        self.assertRegex(setup["randomStateBeforeSha256"], LOWER_SHA256)
        self.assertRegex(setup["randomStateAfterSha256"], LOWER_SHA256)
        self.assertNotEqual(
            setup["randomStateBeforeSha256"],
            setup["randomStateAfterSha256"],
        )
        self.assertEqual(
            setup["operations"],
            [
                {"operation": "put", "key": 1, "value": "one"},
                {"operation": "put", "key": 217, "value": "two-one-seven"},
                {"operation": "put", "key": 545, "value": "five-four-five"},
                {"operation": "put", "key": 761, "value": "seven-six-one"},
                {"operation": "put", "key": 217, "value": "TWO-ONE-SEVEN"},
                {"operation": "put", "key": 977, "value": "nine-seven-seven"},
                {"operation": "put", "key": 1305, "value": "thirteen-oh-five"},
                {"operation": "put", "key": 1521, "value": "fifteen-twenty-one"},
                {"operation": "put", "key": 1737, "value": "seventeen-thirty-seven"},
                {"operation": "put", "key": 1953, "value": "nineteen-fifty-three"},
                {"operation": "put", "key": 2281, "value": "twenty-two-eighty-one"},
                {"operation": "put", "key": 2497, "value": "twenty-four-ninety-seven"},
                {"operation": "put", "key": 2713, "value": "twenty-seven-thirteen"},
            ],
        )
        self.assertTrue(setup["collisionAndResizeExercised"])
        self.assertEqual(
            setup["capacityAfterEachOperation"],
            [8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 16, 16, 16],
        )
        self.assertEqual(len(setup["randomDraws"]), 88)
        self.assertEqual(
            canonical_json_sha256(setup["randomDraws"]),
            "de2e43a8477d22cf4a30fb4dd5ed8698d582a915770d914411967cd8edddc3bf",
        )
        self.assertEqual(case["expected"]["replacementValue"], "TWO-ONE-SEVEN")
        self.assertEqual(case["expected"]["size"], 12)
        self.assertEqual(
            case["callOrder"],
            [
                "key:217",
                "key:1521",
                "key:1305",
                "key:2713",
                "key:977",
                "key:761",
                "key:545",
                "key:1",
                "key:1953",
                "key:1737",
                "key:2497",
                "key:2281",
            ],
        )
        self.assertEqual(case["expected"]["iterationKeys"], case["callOrder"])
        self.assertEqual(
            case["callCount"],
            {"uniqueInsertions": 12, "replacements": 1, "rngDraws": 88},
        )
        manifest = self.require_manifest()
        self.assertEqual(
            manifest["selectedCustomObjectMaps"],
            manifest["selectedFileIoSurface"]["configuredModelEvidence"]["customObjectMaps"],
        )
        self.assertEqual(
            case["expected"]["selectedConfiguredMaps"],
            manifest["selectedCustomObjectMaps"],
        )
        self.assertEqual(case["expected"]["scope"], "trace-specific-not-universal-order")

    def test_legacy_trace_contains_only_normalized_upstream_facts(self):
        file_shapes = self.trace_case("legacy-lua-upstream", "standard-file-call-shapes")
        self.assertEqual(file_shapes["expected"]["dofileReturn"], ["alpha", 7])
        self.assertEqual(file_shapes["expected"]["ioOpenModes"], ["default", "r", "w", "a"])
        self.assertEqual(file_shapes["expected"]["handleMethods"], ["lines", "write", "close"])
        self.assertEqual(file_shapes["expected"]["explicitReadFirstLine"], "first-line")
        self.assertTrue(file_shapes["expected"]["appendReturnsSameHandle"])
        self.assertEqual(file_shapes["expected"]["outputContent"], "alpha:7:tail")
        self.assertEqual(
            file_shapes["callCount"],
            {"dofile": 1, "ioOpen": 4, "lines": 2, "write": 3, "close": 4},
        )
        legacy = self.trace_case("legacy-lua-upstream", "legacy-luajava-selected-facts")
        self.assertTrue(legacy["expected"]["repeatedRequireSameTable"])
        self.assertTrue(legacy["expected"]["requireUsesInstalledTable"])
        self.assertEqual(legacy["expected"]["bindClasses"], ["java.io.File", "com.badlogic.gdx.Gdx"])
        self.assertEqual(legacy["expected"]["fileConstructorArguments"], ["File-token", "relative-path"])
        self.assertEqual(legacy["expected"]["listShape"], "normalized-relative-path-array-or-nil")
        self.assertFalse(legacy["expected"]["gdxAppPresent"])
        self.assertNotIn("hostPath", json.dumps(legacy, sort_keys=True))

        upstream_serialized = json.dumps(
            [self.load_trace(kind) for kind in TRACE_FILES],
            ensure_ascii=False,
            sort_keys=True,
        )
        for policy_only_claim in (
            "virtual-path-enforcement",
            "overlay-only-writes",
            "automatic-overlay-parents",
            "class-member-denial",
            "handle-invalidation",
            "render-phase-denial",
        ):
            self.assertNotIn(policy_only_claim, upstream_serialized)

    def test_sandbox_policy_is_separate_aso_authority_bound_to_task_1a(self):
        self.assertTrue(SANDBOX_POLICY_PATH.is_file(), "sandbox policy must be committed separately")
        policy = json.loads(SANDBOX_POLICY_PATH.read_text(encoding="utf-8"))
        manifest = self.require_manifest()
        self.assertEqual(
            set(policy),
            {
                "schemaVersion",
                "authority",
                "selectedSurfaceDigest",
                "policyBodyDigest",
                "allowed",
                "denied",
                "phaseRules",
                "handlePolicy",
                "serialization",
                "closedLegacyFacade",
            },
        )
        self.assertEqual(
            policy["selectedSurfaceDigest"],
            canonical_json_sha256(manifest["selectedFileIoSurface"]),
        )
        self.assertRegex(policy["selectedSurfaceDigest"], LOWER_SHA256)
        body = {key: value for key, value in policy.items() if key != "policyBodyDigest"}
        self.assertRegex(policy["policyBodyDigest"], LOWER_SHA256)
        self.assertEqual(policy["policyBodyDigest"], canonical_json_sha256(body))
        self.assertNotEqual(policy["policyBodyDigest"], policy["selectedSurfaceDigest"])
        self.assertEqual(
            body,
            {
                "schemaVersion": 1,
                "authority": "AsoBMaShow",
                "selectedSurfaceDigest": canonical_json_sha256(
                    manifest["selectedFileIoSurface"]
                ),
                "allowed": [
                    {
                        "capability": "package-text-dofile",
                        "constraints": [
                            "activated-revision-text-only",
                            "load-phase-only",
                            "virtual-path-enforcement",
                        ],
                    },
                    {
                        "capability": "restricted-io-open",
                        "constraints": [
                            "modes-default-r-w-a",
                            "handle-members-lines-write-close",
                            "overlay-first-read",
                            "overlay-only-writes",
                            "automatic-overlay-parents",
                        ],
                    },
                    {
                        "capability": "legacy-file-list",
                        "constraints": [
                            "load-phase-only",
                            "package-local-bounded-scan",
                            "normalized-virtual-results",
                        ],
                    },
                    {
                        "capability": "legacy-overlay-mkdir",
                        "constraints": [
                            "load-phase-only",
                            "overlay-only-writes",
                            "automatic-overlay-parents",
                        ],
                    },
                ],
                "denied": [
                    {"capability": "network", "effect": "deny-before-effect"},
                    {
                        "capability": "arbitrary-class-member",
                        "effect": "class-member-denial",
                    },
                    {"capability": "reflection", "effect": "deny-before-effect"},
                    {"capability": "native-access", "effect": "deny-before-effect"},
                    {
                        "capability": "process-execution",
                        "effect": "deny-before-effect",
                    },
                    {
                        "capability": "unaudited-legacy-surface",
                        "effect": "compatibility-error",
                    },
                ],
                "phaseRules": [
                    {
                        "phase": "load",
                        "allowedOperations": [
                            "filesystem-read",
                            "filesystem-write",
                            "directory-scan",
                            "resource-upload",
                        ],
                        "effect": "audited-bounded-execution",
                    },
                    {
                        "phase": "render",
                        "deniedOperations": [
                            "filesystem-read",
                            "filesystem-write",
                            "directory-scan",
                            "resource-upload",
                        ],
                        "effect": "deny-before-effect",
                    },
                ],
                "handlePolicy": {
                    "configuredToRenderTransition": "handle-invalidation",
                    "dirtyHandleTransition": "validation-failure",
                    "readBuffers": "release",
                    "unclosedWriteBuffers": "discard",
                },
                "serialization": {
                    "externalIdentity": "opaque-ids-and-digests-only",
                    "order": "deterministic",
                },
                "closedLegacyFacade": {
                    "tableKind": "ordinary-lua-closed-host-table",
                    "module": "luajava",
                    "exports": ["bindClass", "new"],
                    "classes": [
                        {
                            "class": "java.io.File",
                            "token": "unforgeable-lua-class-token",
                            "constructorArguments": ["virtual-path"],
                            "members": ["listFiles", "mkdir"],
                        },
                        {
                            "class": "com.badlogic.gdx.Gdx",
                            "members": [],
                            "absentMembers": ["app"],
                        },
                    ],
                    "rejected": [
                        "all-other-classes",
                        "all-other-constructors",
                        "all-other-members",
                        "all-other-argument-shapes",
                        "newInstance",
                        "url-http-reader",
                        "controller-input",
                        "reflection",
                        "java-objects",
                        "native-handles",
                        "debug",
                        "class-loaders",
                    ],
                },
            },
        )
        for trace_path in TRACE_ROOT.glob("*.json"):
            self.assertNotEqual(trace_path.resolve(), SANDBOX_POLICY_PATH.resolve())

    def test_capture_tool_requires_explicit_reference_root(self):
        result = run_python(TRACE_CAPTURE_PATH, "--output-dir", TRACE_ROOT)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--beatoraja-root", result.stdout)

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
        closure = manifest.get("selectedLuaClosureContract")
        self.assertIsNotNone(closure, "the selected Lua closure must be version-locked")
        self.assertEqual(
            closure,
            {
                "schemaVersion": 1,
                "algorithm": "SelectedLuaClosureContractV1",
                "sha256": PINNED_SELECTED_LUA_CLOSURE_SHA256,
                "changePolicy": "explicit-source-constant-manifest-and-acceptance-review",
            },
            "the closure contract must serialize metadata and one opaque digest only",
        )
        self.assertEqual(
            manifest["acceptanceContract"]["externalDigests"][
                "selectedLuaClosureSha256"
            ],
            PINNED_SELECTED_LUA_CLOSURE_SHA256,
        )

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

        self.assertIn("runtimeConfigurationBinding", selected)
        runtime_binding = selected["runtimeConfigurationBinding"]
        self.assertEqual(runtime_binding["schemaVersion"], 1)
        self.assertEqual(
            runtime_binding["algorithm"],
            "opaque-header-option-choice-v1",
        )
        self.assertEqual(
            runtime_binding["selectedRevisionSha256"],
            manifest["archivePayloadTreeSha256"],
        )
        self.assertEqual(runtime_binding["entrySha256"], manifest["entries"][0]["sha256"])
        self.assertEqual(len(runtime_binding["guardBindings"]), 2)
        for binding in runtime_binding["guardBindings"]:
            self.assertRegex(binding["guardId"], r"^guard-[0-9a-f]{24}$")
            self.assertRegex(binding["optionId"], r"^option-[0-9a-f]{24}$")
            self.assertRegex(binding["reachableChoiceId"], r"^choice-[0-9a-f]{24}$")
            self.assertRegex(binding["defaultChoiceId"], r"^choice-[0-9a-f]{24}$")
            self.assertTrue(binding["nonReachableChoiceIds"])
            self.assertTrue(binding["orderedOperationKinds"])
            self.assertIn(
                binding["orderedOperationKinds"][0],
                {"filesystemRead", "filesystemWrite", "filesystemDirectoryScan"},
            )

        self.assertIn("configuredModelEvidence", selected)
        configured_model = selected["configuredModelEvidence"]
        self.assertEqual(configured_model["evaluator"], "bounded-return-model-v1")
        self.assertEqual(configured_model["conversion"], "LuaSkinLoader.fromLuaValue")
        self.assertEqual(configured_model["customObjectMaps"], manifest["selectedCustomObjectMaps"])

        configurations = selected["auditedGuardConfigurations"]
        self.assertEqual([item["role"] for item in configurations], ["passing", "negative"])
        for configuration in selected["auditedGuardConfigurations"]:
            self.assertEqual(
                configuration["auditedGuardConfigurationSha256"],
                effective_configuration_sha256(
                    runtime_binding["selectedRevisionSha256"],
                    runtime_binding["entrySha256"],
                    configuration["optionSelections"],
                ),
            )
            self.assertEqual(
                configuration["guardVectorSha256"],
                opaque_guard_vector_sha256(
                    configuration["auditedGuardConfigurationSha256"],
                    configuration["guardVector"],
                ),
            )
            selected_choices = {
                item["optionId"]: item["choiceId"]
                for item in configuration["optionSelections"]
            }
            for guard in configuration["guardVector"]:
                self.assertEqual(guard["choiceId"], selected_choices[guard["optionId"]])
                binding = next(
                    item for item in runtime_binding["guardBindings"]
                    if item["guardId"] == guard["guardId"]
                )
                self.assertEqual(
                    guard["value"],
                    "reachable"
                    if guard["choiceId"] == binding["reachableChoiceId"]
                    else "not-reachable",
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
        manifest = self.require_manifest()
        contract = manifest["acceptanceContract"]
        scenarios = contract["negativeScenarios"]
        self.assertEqual(len(scenarios), 1)
        scenario = scenarios[0]
        negative = next(
            item for item in manifest["selectedFileIoSurface"]["auditedGuardConfigurations"]
            if item["role"] == "negative"
        )
        self.assertEqual(scenario["guardConfigurationId"], negative["id"])
        self.assertEqual(
            scenario["auditedGuardConfigurationSha256"],
            negative["auditedGuardConfigurationSha256"],
        )
        self.assertEqual(scenario["expectedGuardVectorSha256"], negative["guardVectorSha256"])
        reachable = [item for item in negative["guardVector"] if item["value"] == "reachable"]
        self.assertEqual(len(reachable), 1)
        self.assertTrue(all(
            item["value"] == "not-reachable"
            for item in negative["guardVector"]
            if item["guardId"] != reachable[0]["guardId"]
        ))
        self.assertIn(
            "runtimeConfigurationBinding",
            manifest["selectedFileIoSurface"],
        )
        binding = next(
            item for item in manifest["selectedFileIoSurface"]["runtimeConfigurationBinding"]["guardBindings"]
            if item["guardId"] == reachable[0]["guardId"]
        )
        self.assertEqual(scenario["expectedDeniedOperation"], binding["orderedOperationKinds"][0])
        self.assertEqual(scenario["expectedDiagnostic"], "skin_file_render_phase_denied")
        self.assertEqual(
            scenario["expectedAction"],
            "discard_frame_disable_session_same_frame_builtin",
        )
        self.assertEqual(scenario["criticality"], "session-critical-sandbox-integrity")
        self.assertEqual(
            scenario["performedCountersExpected"],
            {
                "filesystemReads": 0,
                "filesystemWrites": 0,
                "filesystemDirectoryScans": 0,
                "resourceUploads": 0,
            },
        )
        denied = scenario["deniedCountersExpected"]
        self.assertEqual(sum(value == "positive" for value in denied.values()), 1)
        self.assertEqual(scenario["overlayDigestBeforeCapture"], "complete-before-chart-session-bind")
        self.assertEqual(scenario["overlayDigestAfterCapture"], "asynchronous-after-session-teardown")
        self.assertEqual(scenario["overlayDigestComparison"], "equal")
        self.assertEqual(scenario["overlayDigestPolling"], "memory-only-precomputed-status")
        self.assertEqual(scenario["overlayDigestBefore"], "pending")
        self.assertEqual(scenario["overlayDigestAfter"], "pending")
        passing = [
            item["guardVectorSha256"]
            for item in manifest["selectedFileIoSurface"]["auditedGuardConfigurations"]
            if item["role"] == "passing"
        ]
        self.assertEqual(contract["passingGuardVectorSha256"], passing)
        self.assertEqual(
            contract["externalDigests"]["configurationSha256"],
            {"status": "pending", "value": None},
            "static guard evidence must not populate physical SkinConfigurationDigestV1",
        )
        for document in (GAMEPLAY_CONTRACT_PATH, ACCEPTANCE_PATH):
            text = document.read_text(encoding="utf-8")
            with self.subTest(document=document.name):
                self.assertIn("before chart/session binding", text)
                self.assertIn("only after session teardown", text)
                self.assertIn("must be equal", text)
                self.assertIn("memory-only", text)

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


class BeatorajaTraceHarnessBehaviorTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.capture = load_capture_module()
        cls.manifest = json.loads(MANIFEST_PATH.read_text(encoding="utf-8"))
        cls.committed = {
            kind: json.loads((TRACE_ROOT / filename).read_text(encoding="utf-8"))
            for kind, filename in TRACE_FILES.items()
        }

    def committed_case_payloads(self) -> dict[str, dict]:
        return {
            f"{kind}/{case['name']}": copy.deepcopy(case)
            for kind, trace in self.committed.items()
            for case in trace["cases"]
        }

    @staticmethod
    def assembled_case_payloads(traces: dict[str, dict]) -> dict[str, dict]:
        return {
            f"{kind}/{case['name']}": case
            for kind, trace in traces.items()
            for case in trace["cases"]
        }

    def test_every_case_field_is_driver_owned_and_mutation_coupled(self):
        payloads = self.committed_case_payloads()
        baseline = self.capture.assemble_traces(payloads, self.manifest)
        self.assertEqual(self.assembled_case_payloads(baseline), payloads)

        for case_key, payload in payloads.items():
            for field in ("input", "expected", "callOrder", "callCount", "precision"):
                if field not in payload:
                    continue
                with self.subTest(case=case_key, field=field):
                    mutated_payloads = copy.deepcopy(payloads)
                    mutated_payloads[case_key][field] = mutate_first_scalar(payload[field])
                    mutated = self.capture.assemble_traces(
                        mutated_payloads,
                        self.manifest,
                    )
                    mutated_cases = self.assembled_case_payloads(mutated)
                    changed = [
                        key
                        for key, baseline_case in payloads.items()
                        if mutated_cases[key] != baseline_case
                    ]
                    self.assertEqual(changed, [case_key])
                    self.assertEqual(
                        mutated_cases[case_key][field],
                        mutated_payloads[case_key][field],
                    )

    def test_manifest_custom_map_evidence_is_relational_and_fail_closed(self):
        payloads = self.committed_case_payloads()
        selected = self.manifest["selectedCustomObjectMaps"]
        surface = self.manifest["selectedFileIoSurface"]["configuredModelEvidence"][
            "customObjectMaps"
        ]
        self.assertEqual(selected, surface)
        assembled = self.capture.assemble_traces(payloads, self.manifest)
        timer_case = self.assembled_case_payloads(assembled)[
            "timers-events/libgdx-intmap-backing-order"
        ]
        self.assertEqual(timer_case["expected"]["selectedConfiguredMaps"], selected)

        mismatched = copy.deepcopy(self.manifest)
        first_key = next(iter(mismatched["selectedCustomObjectMaps"]))
        mismatched["selectedCustomObjectMaps"][first_key] += 1
        with self.assertRaisesRegex(ValueError, "selected custom object maps"):
            self.capture.assemble_traces(payloads, mismatched)

    def test_strict_driver_fact_parser_rejects_duplicates_noise_and_missing_cases(self):
        encoded = base64.b64encode(b'{"input":{},"expected":{},"precision":0}').decode(
            "ascii"
        )
        one_line = f"FACT\tlua-language/example\t{encoded}\n"
        self.assertEqual(
            self.capture.parse_driver_output(
                one_line.encode("ascii"),
                {"lua-language/example"},
            ),
            {
                "lua-language/example": {
                    "input": {},
                    "expected": {},
                    "precision": 0,
                }
            },
        )
        with self.assertRaisesRegex(ValueError, "duplicate driver fact"):
            self.capture.parse_driver_output(
                (one_line + one_line).encode("ascii"),
                {"lua-language/example"},
            )
        duplicate_field = base64.b64encode(
            b'{"input":{},"input":{},"expected":{},"precision":0}'
        ).decode("ascii")
        with self.assertRaisesRegex(ValueError, "duplicate JSON field"):
            self.capture.parse_driver_output(
                f"FACT\tlua-language/example\t{duplicate_field}\n".encode("ascii"),
                {"lua-language/example"},
            )
        with self.assertRaisesRegex(ValueError, "unexpected driver output"):
            self.capture.parse_driver_output(
                ("noise\n" + one_line).encode("ascii"),
                {"lua-language/example"},
            )
        with self.assertRaisesRegex(ValueError, "missing driver facts"):
            self.capture.parse_driver_output(b"", {"lua-language/example"})

    def test_bounded_subprocess_enforces_timeout_and_output_limit(self):
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            self.capture.run_bounded(
                sys.executable,
                "-c",
                "import time; time.sleep(2)",
                timeout_seconds=0.05,
                max_output_bytes=1024,
            )
        with self.assertRaisesRegex(RuntimeError, "output limit"):
            self.capture.run_bounded(
                sys.executable,
                "-c",
                "import sys; sys.stdout.write('x' * 4096)",
                timeout_seconds=2,
                max_output_bytes=64,
            )

    def test_reference_manifest_loader_rejects_wrong_commit_and_map_relation(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "manifest.json"
            wrong_commit = copy.deepcopy(self.manifest)
            wrong_commit["beatorajaCommit"] = "0" * 40
            path.write_text(json.dumps(wrong_commit), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "manifest Beatoraja commit"):
                self.capture.load_reference_manifest(path)

            wrong_map = copy.deepcopy(self.manifest)
            first_key = next(iter(wrong_map["selectedCustomObjectMaps"]))
            wrong_map["selectedFileIoSurface"]["configuredModelEvidence"][
                "customObjectMaps"
            ][first_key] += 1
            path.write_text(json.dumps(wrong_map), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "selected custom object maps"):
                self.capture.load_reference_manifest(path)

    @unittest.skipUnless(
        os.environ.get("ASOBMASHOW_BEATORAJA_ROOT"),
        "live pinned Beatoraja coupling probe requires an explicit reference root",
    )
    def test_live_pinned_methods_are_case_coupled_under_input_mutation(self):
        reference_root = Path(os.environ["ASOBMASHOW_BEATORAJA_ROOT"])
        changed_by_probe = self.capture.verify_live_case_coupling(
            reference_root,
            self.manifest,
        )
        expected_keys = set(self.committed_case_payloads())
        self.assertEqual(set(changed_by_probe), expected_keys)
        for case_key in sorted(expected_keys):
            with self.subTest(case=case_key):
                self.assertEqual(changed_by_probe[case_key], [case_key])


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

    def synthetic_archive_data(self, audit):
        archive, _ = self.make_skin_archive()
        archive_data = audit.inspect_archive(archive, "Bundle")
        self.assertEqual(
            set(archive_data["luaSourceBytes"]),
            set(archive_data["luaText"]),
        )
        return archive, archive_data

    def selected_closure_sources(self, audit, archive_data):
        entry = audit.choose_entry(archive_data["luaText"])
        loaded, _, _ = audit.loaded_lua_closure(entry, archive_data["luaText"])
        return {
            path: archive_data["luaSourceBytes"][path]
            for path in loaded
        }

    def mutate_archive_lua(self, archive_data, label):
        mutated = {
            **archive_data,
            "luaText": dict(archive_data["luaText"]),
            "luaSourceBytes": dict(archive_data["luaSourceBytes"]),
        }

        def replace(path, old, new):
            text = mutated["luaText"][path].replace(old, new)
            self.assertNotEqual(text, mutated["luaText"][path], label)
            mutated["luaText"][path] = text
            mutated["luaSourceBytes"][path] = text.encode("utf-8")

        helper = "fixture/helper.lua"
        if label == "computed-custom-key":
            replace(
                helper,
                " return { image =",
                ' local computed = "custom" .. "Timers"\n return { [computed] = {}, image =',
            )
        elif label == "tuple-reassignment":
            replace(
                helper,
                "return m\n",
                "local tuple_a, tuple_b = {}, {}\ntuple_a, tuple_b = tuple_b, tuple_a\nreturn m\n",
            )
        elif label == "dynamic-global-alias":
            replace(helper, "return m\n", '_G["dynamic_alias"] = m\nreturn m\n')
        elif label == "source-object-reassignment":
            replace(helper, "return m\n", "m = setmetatable({}, {})\nreturn m\n")
        elif label == "one-byte":
            mutated["luaText"][helper] += " "
            mutated["luaSourceBytes"][helper] += b" "
        elif label == "path-identity":
            old_path = "fixture/options.lua"
            new_path = "fixture/options-renamed.lua"
            mutated["luaText"][new_path] = mutated["luaText"].pop(old_path)
            mutated["luaSourceBytes"][new_path] = mutated["luaSourceBytes"].pop(old_path)
            replace(
                "fixture/entry.lua",
                'require("fixture.options")',
                'require("fixture.options-renamed")',
            )
        elif label == "add-loaded-source":
            mutated["luaText"]["fixture/added.lua"] = "return {}\n"
            mutated["luaSourceBytes"]["fixture/added.lua"] = b"return {}\n"
            replace(
                "fixture/entry.lua",
                'local helper = require("fixture.helper")\n',
                'local helper = require("fixture.helper")\nlocal added = require("fixture.added")\n',
            )
        elif label == "remove-loaded-source":
            mutated["luaText"].pop("fixture/options.lua")
            mutated["luaSourceBytes"].pop("fixture/options.lua")
            replace(
                "fixture/entry.lua",
                'require("fixture.options")',
                "{ SYNTHETIC_ENABLED = 777 }",
            )
        else:
            raise AssertionError(label)
        return mutated

    def make_render_policy_closure(
        self,
        operations=("read",),
        *,
        option_keys=None,
        choice_labels=None,
        extra_choice_for_first=False,
        direct_callbacks=False,
        computed_custom_key=None,
        indirect_computed_custom_key=None,
        custom_map_mutation=None,
        model_escape=None,
        explicit_custom_counts=None,
        duplicate_first_callback=False,
    ):
        option_keys = option_keys or [f"Synthetic option {index}" for index in range(len(operations))]
        choice_labels = choice_labels or [("disabled", "enabled") for _ in operations]
        property_lines = [
            "local module = {}",
            "local customoptionNumber = 899",
            "local customoption = {}",
            "customoption.parent = function(name) return {name = name} end",
            "customoption.chiled = function(cName, pName)",
            "  customoptionNumber = customoptionNumber + 1",
            "  local num = customoptionNumber",
            "  return {name = cName, num = num}, function() return skin_config.option[pName] == num end",
            "end",
            "local function load()",
        ]
        property_records = []
        for index, option_key in enumerate(option_keys):
            disabled, enabled = choice_labels[index]
            parent = f"option{index}"
            property_lines.append(f"  local {parent} = customoption.parent({json.dumps(option_key)})")
            property_lines.append(
                f"  {parent}.off, module.guard{index}Off = "
                f"customoption.chiled({json.dumps(disabled)}, {parent}.name)"
            )
            if index == 0 and extra_choice_for_first:
                property_lines.append(
                    f"  {parent}.middle, module.guard{index}Middle = "
                    f"customoption.chiled(\"middle\", {parent}.name)"
                )
            property_lines.append(
                f"  {parent}.on, module.guard{index} = "
                f"customoption.chiled({json.dumps(enabled)}, {parent}.name)"
            )
            items = [
                f"{{name = {parent}.off.name, op = {parent}.off.num}}",
                *(
                    [f"{{name = {parent}.middle.name, op = {parent}.middle.num}}"]
                    if index == 0 and extra_choice_for_first else []
                ),
                f"{{name = {parent}.on.name, op = {parent}.on.num}}",
            ]
            property_records.append(
                "{name = " + parent + ".name, def = " + parent + ".off.name, item = {"
                + ",".join(items) + "}}"
            )
        property_lines.extend(
            [
                "  module.property = {" + ",".join(property_records) + "}",
                "  return module",
                "end",
                "return {load = load}",
            ]
        )

        helper_lines = ["local module = {}"]
        model_prelude = []
        additional_files = {}
        callback_lines = []
        for index, operation in enumerate(operations):
            if operation == "read":
                direct_body = 'local f = io.open("state.txt", "r"); f:close()'
            elif operation == "write":
                direct_body = 'local f = io.open("state.txt", "w"); f:write("x"); f:close()'
            elif operation == "read-write":
                direct_body = (
                    'local f = io.open("state.txt", "r"); f:close(); '
                    'local w = io.open("state.txt", "a"); w:write("x"); w:close()'
                )
            elif operation == "dofile":
                direct_body = 'dofile("state.lua")'
            elif operation == "unresolved-call":
                direct_body = "UNKNOWN.perform()"
            elif operation == "unresolved-method":
                direct_body = "UNKNOWN:perform()"
            elif operation == "mkdir":
                direct_body = (
                    'local File = luajava.bindClass("java.io.File"); '
                    'local dir = luajava.new(File, "state"); dir:mkdir()'
                )
            elif operation == "list-files":
                direct_body = (
                    'local File = luajava.bindClass("java.io.File"); '
                    'local dir = luajava.new(File, "state"); dir:listFiles()'
                )
            elif operation == "chained-mkdir":
                direct_body = (
                    'local File = luajava.bindClass("java.io.File"); '
                    'luajava.new(File, "state"):mkdir()'
                )
            elif operation == "legacy-alias-mkdir":
                direct_body = (
                    'local File = luajava.bindClass("java.io.File"); '
                    'local dir = luajava.new(File, "state"); '
                    'local dir_alias = dir; dir_alias:mkdir()'
                )
            elif operation in {"captured-lines", "captured-close-read"}:
                model_prelude.append(
                    f'local captured{index} = io.open("captured{index}.txt", "r")'
                )
                direct_body = (
                    f'for line in captured{index}:lines() do break end'
                    if operation == "captured-lines"
                    else f'captured{index}:close()'
                )
            elif operation in {"captured-write", "captured-close-write"}:
                model_prelude.append(
                    f'local captured{index} = io.open("captured{index}.txt", "w")'
                )
                direct_body = (
                    f'captured{index}:write("x")'
                    if operation == "captured-write"
                    else f'captured{index}:close()'
                )
            elif operation == "captured-close-ambiguous":
                model_prelude.extend(
                    [
                        f"local captured{index}",
                        f"if skin_config.option.synthetic then captured{index} = "
                        f'io.open("captured{index}.txt", "r") else captured{index} = '
                        f'io.open("captured{index}.txt", "w") end',
                    ]
                )
                direct_body = f'captured{index}:close()'
            elif operation == "captured-wrapped-overwrite":
                model_prelude.extend(
                    [
                        f'local captured{index} = io.open("captured{index}.txt", "r")',
                        "local function wrapped_open(path)",
                        '  return io.open(path, "w")',
                        "end",
                    ]
                )
                direct_body = (
                    f'captured{index} = wrapped_open("wrapped{index}.txt"); '
                    f'captured{index}:close()'
                )
            elif operation == "captured-handle-alias":
                model_prelude.append(
                    f'local captured{index} = io.open("captured{index}.txt", "r")'
                )
                direct_body = (
                    f'local captured_alias = captured{index}; captured_alias:close()'
                )
            elif operation == "source-backed-local-methods":
                model_prelude.append(
                    'local local_state = require("fixture.local_state").load()'
                )
                additional_files["fixture/local_state.lua"] = (
                    "local module = {value = 0}\n"
                    "module.increment = function(self) self.value = self.value + 1 end\n"
                    "module.values = {{get = function() return 1 end}}\n"
                    "return {load = function() return module end}\n"
                )
                direct_body = (
                    "local_state:increment(); local_state.values[1].get(); "
                    'local f = io.open("state.txt", "r"); f:close()'
                )
            elif operation == "source-backed-unknown-call":
                model_prelude.append(
                    'local unsafe_state = require("fixture.unsafe_state").load()'
                )
                additional_files["fixture/unsafe_state.lua"] = (
                    "local module = {}\n"
                    "module.perform = function() UNKNOWN.perform() end\n"
                    "return {load = function() return module end}\n"
                )
                direct_body = (
                    "unsafe_state:perform(); "
                    'local f = io.open("state.txt", "r"); f:close()'
                )
            else:
                raise AssertionError(operation)
            helper_lines.extend(
                [
                    f"local function operation{index}Leaf() {direct_body} end",
                    f"module.operation{index} = function() operation{index}Leaf() end",
                ]
            )
            callback_body = direct_body if direct_callbacks else f"HELPER.operation{index}()"
            callback_lines.extend(
                [
                    f"  if PROPERTY.guard{index}() then",
                    "    table.insert(skin.destination, {timer = function()",
                    f"      {callback_body}",
                    "    end})",
                    "  end",
                ]
            )
            if index == 0 and duplicate_first_callback:
                callback_lines.extend(
                    [
                        f"  if PROPERTY.guard{index}() then",
                        "    table.insert(skin.destination, {timer = function()",
                        '      local f = io.open("second.txt", "w"); f:close()',
                        "    end})",
                        "  end",
                    ]
                )
        helper_lines.append("return module")

        model_lines = [
            'local PROPERTY = require("fixture.property").load()',
            'local HELPER = require("fixture.helper")',
            *model_prelude,
            "local function main()",
            "  local skin = {}",
            "  skin.destination = {}",
            *callback_lines,
        ]
        if indirect_computed_custom_key is not None:
            suffix = "Timers" if indirect_computed_custom_key == "customTimers" else "Events"
            model_lines.extend(
                [
                    "  local function mutate_model(model)",
                    f'    local computed = "custom" .. "{suffix}"',
                    "    model[computed] = {{id = 1}}",
                    "  end",
                    "  mutate_model(skin)",
                ]
            )
        if computed_custom_key is not None:
            suffix = "Timers" if computed_custom_key == "customTimers" else "Events"
            model_lines.extend(
                [
                    f'  local computed = "custom" .. "{suffix}"',
                    "  skin[computed] = {{id = 1}}",
                ]
            )
        if explicit_custom_counts is not None:
            timer_count, event_count = explicit_custom_counts
            model_lines.append(
                "  skin.customTimers = {"
                + ",".join(f"{{id = {index}}}" for index in range(timer_count)) + "}"
            )
            model_lines.append(
                "  skin.customEvents = {"
                + ",".join(f"{{id = {index}}}" for index in range(event_count)) + "}"
            )
        if custom_map_mutation is not None:
            field, mutation = custom_map_mutation
            model_lines.append(f"  skin.{field} = {{}}")
            if mutation == "direct":
                model_lines.append(f"  table.insert(skin.{field}, {{id = 1}})")
            elif mutation == "alias":
                model_lines.extend(
                    [
                        f"  local custom_map_alias = skin.{field}",
                        "  table.insert(custom_map_alias, {id = 1})",
                    ]
                )
            elif mutation == "rawset":
                model_lines.append(f"  rawset(skin.{field}, 1, {{id = 1}})")
            elif mutation == "helper":
                model_lines.extend(
                    [
                        "  local function append_custom_map(custom_map)",
                        "    table.insert(custom_map, {id = 1})",
                        "  end",
                        f"  append_custom_map(skin.{field})",
                    ]
                )
            else:
                raise AssertionError(mutation)
        if model_escape is not None:
            field, escape = model_escape
            if escape == "alias":
                model_lines.extend(
                    [
                        "  local model_alias = skin",
                        f"  model_alias.{field} = {{{{id = 1}}}}",
                    ]
                )
            elif escape == "captured-helper":
                model_lines.extend(
                    [
                        "  local function mutate_captured_model()",
                        f"    skin.{field} = {{{{id = 1}}}}",
                        "  end",
                        "  mutate_captured_model()",
                    ]
                )
            elif escape == "later-argument":
                model_lines.extend(
                    [
                        "  local function mutate_later_argument(ignored, model)",
                        f"    model.{field} = {{{{id = 1}}}}",
                        "  end",
                        "  mutate_later_argument(nil, skin)",
                    ]
                )
            else:
                raise AssertionError(escape)
        model_lines.extend(["  return skin", "end", "return {main = main}"])
        return {
            "play7_fixture.luaskin": 'local t = require("fixture.model"); return t.main()\n',
            "fixture/model.lua": "\n".join(model_lines) + "\n",
            "fixture/property.lua": "\n".join(property_lines) + "\n",
            "fixture/helper.lua": "\n".join(helper_lines) + "\n",
            **additional_files,
        }

    def analyze_render_policy(self, closure):
        audit = load_audit_module()
        try:
            return audit._analyze_selected_file_io_surface_unpinned(
                closure,
                "22" * 32,
                "11" * 32,
            )
        except TypeError as error:
            self.fail(f"selected revision must be an explicit audit input: {error}")

    def test_runtime_guard_bindings_change_with_actual_option_key_choice_and_value(self):
        baseline, _ = self.analyze_render_policy(self.make_render_policy_closure())
        key_drift, _ = self.analyze_render_policy(
            self.make_render_policy_closure(option_keys=["Renamed synthetic option"])
        )
        choice_drift, _ = self.analyze_render_policy(
            self.make_render_policy_closure(choice_labels=[("disabled", "renamed enabled")])
        )
        value_drift, _ = self.analyze_render_policy(
            self.make_render_policy_closure(extra_choice_for_first=True)
        )

        def configuration_digests(selected):
            return [
                item["auditedGuardConfigurationSha256"]
                for item in selected["auditedGuardConfigurations"]
            ]

        self.assertNotEqual(configuration_digests(baseline), configuration_digests(key_drift))
        self.assertNotEqual(configuration_digests(baseline), configuration_digests(choice_drift))
        self.assertNotEqual(configuration_digests(baseline), configuration_digests(value_drift))
        binding = baseline["runtimeConfigurationBinding"]
        self.assertEqual(binding["selectedRevisionSha256"], "11" * 32)
        self.assertEqual(binding["entrySha256"], "22" * 32)

    def test_render_operation_graph_covers_direct_and_transitive_callbacks(self):
        direct, _ = self.analyze_render_policy(
            self.make_render_policy_closure(direct_callbacks=True)
        )
        transitive, _ = self.analyze_render_policy(
            self.make_render_policy_closure(operations=("read-write",))
        )
        self.assertEqual(
            direct["runtimeConfigurationBinding"]["guardBindings"][0]["orderedOperationKinds"],
            ["filesystemRead"],
        )
        self.assertEqual(
            transitive["runtimeConfigurationBinding"]["guardBindings"][0]["orderedOperationKinds"],
            ["filesystemRead", "filesystemWrite"],
        )

    def test_render_operation_graph_classifies_direct_and_transitive_dofile(self):
        for direct_callbacks in (True, False):
            with self.subTest(direct_callbacks=direct_callbacks):
                selected, _ = self.analyze_render_policy(
                    self.make_render_policy_closure(
                        operations=("dofile",),
                        direct_callbacks=direct_callbacks,
                    )
                )
                operations = [
                    binding["orderedOperationKinds"]
                    for binding in selected["runtimeConfigurationBinding"]["guardBindings"]
                ]
                self.assertEqual(operations, [["filesystemRead"]])

    def test_render_operation_graph_classifies_closed_legacy_file_methods(self):
        cases = {
            "mkdir": "filesystemWrite",
            "list-files": "filesystemDirectoryScan",
        }
        for operation, expected in cases.items():
            with self.subTest(operation=operation):
                selected, _ = self.analyze_render_policy(
                    self.make_render_policy_closure(
                        operations=(operation,),
                        direct_callbacks=True,
                    )
                )
                operations = [
                    binding["orderedOperationKinds"]
                    for binding in selected["runtimeConfigurationBinding"]["guardBindings"]
                ]
                self.assertEqual(operations, [[expected]])

    def test_render_operation_graph_classifies_captured_handle_methods_by_origin(self):
        cases = {
            "captured-lines": "filesystemRead",
            "captured-write": "filesystemWrite",
            "captured-close-read": "filesystemRead",
            "captured-close-write": "filesystemWrite",
        }
        for operation, expected in cases.items():
            with self.subTest(operation=operation):
                selected, _ = self.analyze_render_policy(
                    self.make_render_policy_closure(
                        operations=(operation,),
                        direct_callbacks=True,
                    )
                )
                operations = [
                    binding["orderedOperationKinds"]
                    for binding in selected["runtimeConfigurationBinding"]["guardBindings"]
                ]
                self.assertEqual(operations, [[expected]])

    def test_render_operation_graph_rejects_ambiguous_captured_handle_origin(self):
        audit = load_audit_module()
        with self.assertRaisesRegex(audit.AuditError, "ambiguous render-I/O operation graph"):
            audit._analyze_selected_file_io_surface_unpinned(
                self.make_render_policy_closure(
                    operations=("captured-close-ambiguous",),
                    direct_callbacks=True,
                ),
                "22" * 32,
                "11" * 32,
            )

    def test_render_operation_graph_rejects_unresolved_retained_calls(self):
        audit = load_audit_module()
        with self.assertRaisesRegex(audit.AuditError, "unresolved render-I/O operation graph"):
            audit._analyze_selected_file_io_surface_unpinned(
                self.make_render_policy_closure(
                    operations=("unresolved-call",),
                    direct_callbacks=True,
                ),
                "22" * 32,
                "11" * 32,
            )

    def test_render_operation_graph_rejects_unclassified_retained_methods(self):
        audit = load_audit_module()
        with self.assertRaisesRegex(audit.AuditError, "unresolved render-I/O operation graph"):
            audit._analyze_selected_file_io_surface_unpinned(
                self.make_render_policy_closure(
                    operations=("unresolved-method",),
                    direct_callbacks=True,
                ),
                "22" * 32,
                "11" * 32,
            )

    def test_render_operation_graph_rejects_unproven_legacy_expression_origins(self):
        audit = load_audit_module()
        for operation in ("chained-mkdir", "legacy-alias-mkdir"):
            with self.subTest(operation=operation):
                with self.assertRaisesRegex(
                    audit.AuditError, "ambiguous render-I/O operation graph"
                ):
                    audit._analyze_selected_file_io_surface_unpinned(
                        self.make_render_policy_closure(
                            operations=(operation,),
                            direct_callbacks=True,
                        ),
                        "22" * 32,
                        "11" * 32,
                    )

    def test_render_operation_graph_invalidates_unproven_handle_assignments_and_aliases(self):
        audit = load_audit_module()
        for operation in ("captured-wrapped-overwrite", "captured-handle-alias"):
            with self.subTest(operation=operation):
                with self.assertRaisesRegex(
                    audit.AuditError, "ambiguous render-I/O operation graph captured handle"
                ):
                    audit._analyze_selected_file_io_surface_unpinned(
                        self.make_render_policy_closure(
                            operations=(operation,),
                            direct_callbacks=True,
                        ),
                        "22" * 32,
                        "11" * 32,
                    )

    def test_render_operation_graph_accepts_only_closed_source_backed_local_methods(self):
        audit = load_audit_module()
        try:
            selected, _ = audit._analyze_selected_file_io_surface_unpinned(
                self.make_render_policy_closure(
                    operations=("source-backed-local-methods",),
                    direct_callbacks=True,
                ),
                "22" * 32,
                "11" * 32,
            )
        except audit.AuditError as error:
            self.fail(f"closed source-backed local method was not classified: {error}")
        self.assertEqual(
            selected["runtimeConfigurationBinding"]["guardBindings"][0][
                "orderedOperationKinds"
            ],
            ["filesystemRead"],
        )

    def test_render_operation_graph_rejects_source_backed_methods_with_unknown_calls(self):
        audit = load_audit_module()
        with self.assertRaisesRegex(audit.AuditError, "unresolved render-I/O operation graph"):
            audit._analyze_selected_file_io_surface_unpinned(
                self.make_render_policy_closure(
                    operations=("source-backed-unknown-call",),
                    direct_callbacks=True,
                ),
                "22" * 32,
                "11" * 32,
            )

    def test_negative_kind_comes_from_selected_first_operation_and_is_order_independent(self):
        read_then_write = self.make_render_policy_closure(operations=("read", "write"))
        selected, _ = self.analyze_render_policy(read_then_write)
        reordered, _ = self.analyze_render_policy(dict(reversed(list(read_then_write.items()))))
        self.assertEqual(selected, reordered)

        negative = next(
            item for item in selected["auditedGuardConfigurations"]
            if item["role"] == "negative"
        )
        reachable = [item for item in negative["guardVector"] if item["value"] == "reachable"]
        self.assertEqual(len(reachable), 1)
        self.assertTrue(all(
            item["value"] == "not-reachable"
            for item in negative["guardVector"]
            if item["guardId"] != reachable[0]["guardId"]
        ))
        binding = next(
            item for item in selected["runtimeConfigurationBinding"]["guardBindings"]
            if item["guardId"] == reachable[0]["guardId"]
        )
        self.assertEqual(selected["negativeExpectedDeniedOperation"], binding["orderedOperationKinds"][0])

        swapped, _ = self.analyze_render_policy(
            self.make_render_policy_closure(operations=("write", "read"))
        )
        swapped_negative = next(
            item for item in swapped["auditedGuardConfigurations"]
            if item["role"] == "negative"
        )
        swapped_reachable = next(
            item for item in swapped_negative["guardVector"] if item["value"] == "reachable"
        )
        self.assertEqual(reachable[0]["guardId"], swapped_reachable["guardId"])
        self.assertNotEqual(
            selected["negativeExpectedDeniedOperation"],
            swapped["negativeExpectedDeniedOperation"],
        )

    def test_configured_model_counts_literal_custom_maps_and_rejects_computed_keys(self):
        _, counts = self.analyze_render_policy(
            self.make_render_policy_closure(explicit_custom_counts=(2, 1))
        )
        self.assertEqual(counts, {"customTimers": 2, "customEvents": 1})

        audit = load_audit_module()
        for field in ("customTimers", "customEvents"):
            with self.subTest(field=field):
                try:
                    with self.assertRaisesRegex(audit.AuditError, "configured return model"):
                        audit._analyze_selected_file_io_surface_unpinned(
                            self.make_render_policy_closure(computed_custom_key=field),
                            "22" * 32,
                            "11" * 32,
                        )
                except TypeError as error:
                    self.fail(f"selected revision must be an explicit audit input: {error}")
                with self.assertRaisesRegex(audit.AuditError, "configured return model"):
                    audit._analyze_selected_file_io_surface_unpinned(
                        self.make_render_policy_closure(indirect_computed_custom_key=field),
                        "22" * 32,
                        "11" * 32,
                    )

    def test_configured_model_rejects_unbounded_custom_map_mutations(self):
        audit = load_audit_module()
        for field in ("customTimers", "customEvents"):
            for mutation in ("direct", "alias", "rawset", "helper"):
                with self.subTest(field=field, mutation=mutation):
                    with self.assertRaisesRegex(audit.AuditError, "configured return model"):
                        audit._analyze_selected_file_io_surface_unpinned(
                            self.make_render_policy_closure(
                                custom_map_mutation=(field, mutation)
                            ),
                            "22" * 32,
                            "11" * 32,
                        )

    def test_configured_model_rejects_alias_capture_and_argument_escapes(self):
        audit = load_audit_module()
        for field in ("customTimers", "customEvents"):
            for escape in ("alias", "captured-helper", "later-argument"):
                with self.subTest(field=field, escape=escape):
                    with self.assertRaisesRegex(audit.AuditError, "configured return model"):
                        audit._analyze_selected_file_io_surface_unpinned(
                            self.make_render_policy_closure(
                                model_escape=(field, escape)
                            ),
                            "22" * 32,
                            "11" * 32,
                        )

    def test_ambiguous_multiple_retained_io_callbacks_fail_closed(self):
        audit = load_audit_module()
        try:
            with self.assertRaisesRegex(audit.AuditError, "ambiguous render-I/O operation graph"):
                audit._analyze_selected_file_io_surface_unpinned(
                    self.make_render_policy_closure(duplicate_first_callback=True),
                    "22" * 32,
                    "11" * 32,
                )
        except TypeError as error:
            self.fail(f"selected revision must be an explicit audit input: {error}")

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

    def test_build_manifest_rejects_every_selected_closure_byte_and_identity_mutation(self):
        audit = load_audit_module()
        archive, archive_data = self.synthetic_archive_data(audit)
        reviewed_digest = selected_lua_closure_sha256(
            self.selected_closure_sources(audit, archive_data)
        )
        arguments = type("Arguments", (), {"archive_path": archive})()
        with (
            mock.patch.object(
                audit,
                "PINNED_SELECTED_LUA_CLOSURE_SHA256",
                reviewed_digest,
                create=True,
            ),
            mock.patch.object(
                audit,
                "PINNED_ARCHIVE_SHA256",
                archive_data["archiveSha256"],
            ),
        ):
            baseline = audit.build_manifest(arguments, archive_data, "11" * 32, [])
            self.assertIn("selectedLuaClosureContract", baseline)
            self.assertEqual(
                baseline["selectedLuaClosureContract"]["sha256"], reviewed_digest
            )
            for label in (
                "computed-custom-key",
                "tuple-reassignment",
                "dynamic-global-alias",
                "source-object-reassignment",
                "one-byte",
                "path-identity",
                "add-loaded-source",
                "remove-loaded-source",
            ):
                with self.subTest(mutation=label):
                    with self.assertRaisesRegex(
                        audit.AuditError, "selected Lua closure contract"
                    ):
                        audit.build_manifest(
                            arguments,
                            self.mutate_archive_lua(archive_data, label),
                            "11" * 32,
                            [],
                        )

    def test_audit_entrypoint_rejects_an_unpinned_selected_closure_before_evidence(self):
        reference_root = self.make_reference_root()
        archive, skin_root = self.make_skin_archive()
        manifest_path = self.temp_path / "unreviewed.json"
        result = run_python(
            AUDIT_PATH,
            *self.audit_arguments(reference_root, archive, skin_root, "--output", manifest_path),
            env=self.tool_env,
        )
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("selected Lua closure contract", result.stdout)
        self.assertFalse(manifest_path.exists())

    def test_audit_hashes_archive_and_extracted_tree_and_verifies_deterministically(self):
        self.assertTrue(AUDIT_PATH.is_file(), str(AUDIT_PATH))
        archive, skin_root = self.make_skin_archive()
        audit = load_audit_module()
        archive_data = audit.inspect_archive(archive, "Bundle")
        disk_tree_sha, _ = audit.inspect_disk_tree(skin_root)
        reviewed_digest = selected_lua_closure_sha256(
            self.selected_closure_sources(audit, archive_data)
        )
        arguments = type("Arguments", (), {"archive_path": archive})()
        with (
            mock.patch.object(
                audit,
                "PINNED_SELECTED_LUA_CLOSURE_SHA256",
                reviewed_digest,
            ),
            mock.patch.object(
                audit,
                "PINNED_ARCHIVE_SHA256",
                archive_data["archiveSha256"],
            ),
        ):
            manifest = audit.build_manifest(arguments, archive_data, disk_tree_sha, [])
            regenerated = audit.build_manifest(arguments, archive_data, disk_tree_sha, [])
        self.assertEqual(manifest, regenerated)
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
