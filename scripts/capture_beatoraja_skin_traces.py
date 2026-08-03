#!/usr/bin/env python3
"""Capture normalized synthetic traces from the pinned Beatoraja runtime.

This is a developer-only refresh/verification harness.  Default tests consume
the committed JSON and do not need Java or a Beatoraja clone.  The harness
never reads, creates, or rewrites the separately authored sandbox policy.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
TRACE_FILENAMES = {
    "lua-language": "lua_language_v1.json",
    "destination": "destination_v1.json",
    "properties": "properties_v1.json",
    "timers-events": "timers_events_v1.json",
    "legacy-lua-upstream": "legacy_lua_upstream_v1.json",
}
ROOT = Path(__file__).resolve().parents[1]
TWO_PHASE_ENTRY = ROOT / "tests/fixtures/beatoraja_skin/lua/two_phase/entry.luaskin"


REFERENCE_MARKERS = {
    "src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java": (
        "SkinHeader loadHeader(Path p)",
        "Skin load(Path p, SkinType type, SkinConfig.Property property)",
        "<T> T fromLuaValue(Class<T> cls, LuaValue lv)",
        "serializeLuaScript",
    ),
    "src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java": (
        "createStandardGlobals",
        "initializeModules",
        "public LuaValue execFile(Path path)",
        "public void exportSkinProperty",
        "public Event loadEvent(LuaFunction function)",
    ),
    "src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java": (
        "globals.package_.setIsLoaded(\"luajava\", luajava)",
        "private static LuaTable fileFacade",
        "private static LuaTable gdxFacade()",
    ),
    "src/bms/player/beatoraja/skin/lua/SkinLuaPathResolver.java": (
        "Path resolve(String pathText)",
    ),
    "src/bms/player/beatoraja/skin/lua/MainStateAccessor.java": (
        "new MainStatePropertyLuaApiExporter(state)",
        "new SkinFileLuaApiExporter(pathResolver)",
    ),
    "src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java": (
        "private static final long TIMER_OFF_VALUE = Long.MIN_VALUE",
        "class EventExecFunction",
        "class EventIndexFunction",
    ),
    "src/bms/player/beatoraja/skin/lua/SkinFileLuaApiExporter.java": (
        "table.set(\"file_list\"",
        "table.set(\"file_write\"",
        "table.set(\"file_append\"",
    ),
    "src/bms/player/beatoraja/skin/lua/TimerUtility.java": (
        "table.set(\"now_timer\"",
        "table.set(\"new_passive_timer\"",
    ),
    "src/bms/player/beatoraja/skin/lua/EventUtility.java": (
        "table.set(\"event_observe_timer\"",
        "table.set(\"event_min_interval\"",
    ),
    "src/bms/player/beatoraja/skin/CustomTimer.java": (
        "private long time = Long.MIN_VALUE",
        "public void update(MainState state)",
    ),
    "src/bms/player/beatoraja/skin/CustomEvent.java": (
        "private long lastExecuteTime = Long.MIN_VALUE",
        "public void update(MainState state)",
    ),
    "src/bms/player/beatoraja/skin/SkinObject.java": (
        "public void prepareRegion(long time, MainState state)",
        "private void getRate()",
    ),
    "src/bms/player/beatoraja/skin/Skin.java": (
        "public void updateCustomObjects(MainState state)",
        "for (IntMap.Entry<CustomTimer> timer : customTimers)",
        "for (IntMap.Entry<CustomEvent> event : customEvents)",
    ),
    "src/bms/player/beatoraja/skin/property/TimerProperty.java": (
        "return getMicro(state) == Long.MIN_VALUE",
    ),
    "src/bms/player/beatoraja/skin/property/TimerPropertyFactory.java": (
        "if (timerId < 0)",
    ),
    "src/bms/player/beatoraja/skin/json/JsonSkin.java": (
        "public static class Destination",
        "public static class CustomEvent",
        "public static class CustomTimer",
    ),
    "skin/default/play/play7.luaskin": (
        "local t = require(\"play7main\")",
        "if skin_config then",
    ),
}


JAVA_DRIVER = r'''
import com.badlogic.gdx.math.MathUtils;
import com.badlogic.gdx.Gdx;
import com.badlogic.gdx.utils.IntMap;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Random;
import org.luaj.vm2.Globals;
import org.luaj.vm2.LoadState;
import org.luaj.vm2.LuaTable;
import org.luaj.vm2.LuaValue;
import org.luaj.vm2.compiler.LuaC;
import org.luaj.vm2.lib.Bit32Lib;
import org.luaj.vm2.lib.CoroutineLib;
import org.luaj.vm2.lib.PackageLib;
import org.luaj.vm2.lib.TableLib;
import org.luaj.vm2.lib.jse.JseBaseLib;
import org.luaj.vm2.lib.jse.JseIoLib;
import org.luaj.vm2.lib.jse.JseMathLib;
import org.luaj.vm2.lib.jse.JseStringLib;

public final class TraceDriver {
    private static final class RecordingRandom extends Random {
        final List<String> draws = new ArrayList<>();
        RecordingRandom(long seed) { super(seed); }
        @Override public int nextInt(int bound) {
            int value = super.nextInt(bound);
            draws.add(bound + ":" + value);
            return value;
        }
    }

    private static Globals globals() {
        Globals globals = new Globals();
        globals.load(new JseBaseLib());
        globals.load(new PackageLib());
        globals.load(new Bit32Lib());
        globals.load(new TableLib());
        globals.load(new JseStringLib());
        globals.load(new CoroutineLib());
        globals.load(new JseMathLib());
        globals.load(new JseIoLib());
        LoadState.install(globals);
        LuaC.install(globals);
        return globals;
    }

    private static void line(String key, Object value) {
        System.out.println(key + "=" + value);
    }

    private static int capacity(IntMap<?> map) throws Exception {
        Field field = IntMap.class.getDeclaredField("capacity");
        field.setAccessible(true);
        return field.getInt(map);
    }

    public static void main(String[] args) throws Exception {
        Path entry = Path.of(args[0]).toAbsolutePath().normalize();
        String moduleRoot = entry.getParent().toString().replace('\\', '/');
        Globals same = globals();
        same.get("package").set("path", moduleRoot + "/?.lua;" + moduleRoot + "/?/init.lua");
        LuaTable header = same.loadfile(entry.toString()).call().checktable();
        line("header.packageLoaded", header.get("package_loaded").toboolean());
        line("header.moduleLoadCount", header.get("module_load_count").toint());
        same.set("skin_config", new LuaTable());
        LuaTable configured = same.loadfile(entry.toString()).call().checktable();
        line("configured.packageLoaded", configured.get("package_loaded").toboolean());
        line("configured.moduleLoadCount", configured.get("module_load_count").toint());
        line("configured.sameModuleTable", configured.get("same_module_table").toboolean());
        line("configured.observedHeaderMutation", configured.get("observed_header_mutation").tojstring());
        line("configured.observedGlobalMutation", configured.get("observed_global_mutation").tojstring());
        Globals fresh = globals();
        fresh.get("package").set("path", moduleRoot + "/?.lua;" + moduleRoot + "/?/init.lua");
        line("fresh.packageLoaded", !fresh.get("package").get("loaded").get("shared").isnil());
        line("fresh.moduleLoadCount", fresh.get("__trace_shared_load_count").optint(0));
        line("fresh.globalMutation", fresh.get("__trace_two_phase_global").isnil() ? "null" : "present");

        LuaTable conversion = same.load(
            "return {boolean=(1~=0), integer=17.9, float=12.5, string_from_number=tostring(42), " +
            "ordered={[1]='first',[2]='second',named='named'}}"
        ).call().checktable();
        line("conversion.boolean", conversion.get("boolean").toboolean());
        line("conversion.integer", conversion.get("integer").toint());
        line("conversion.float", conversion.get("float").todouble());
        line("conversion.stringFromNumber", conversion.get("string_from_number").tojstring());
        StringBuilder ordered = new StringBuilder();
        for (LuaValue key : conversion.get("ordered").checktable().keys()) {
            if (ordered.length() > 0) ordered.append(',');
            ordered.append(conversion.get("ordered").get(key).tojstring());
        }
        line("conversion.ordered", ordered);

        LuaTable bit32 = same.load(
            "return {band=bit32.band(0xfff0,0x00ff), bor=bit32.bor(0xf00,0x0ff), " +
            "bxor=bit32.bxor(0xf0,0x0f), bnot=bit32.bnot(0), " +
            "lshift=bit32.lshift(1,31), rshift=bit32.rshift(0xfffffffe,1), " +
            "arshift=bit32.arshift(0xfffffffe,1), extract=bit32.extract(0xf0,4,4), " +
            "replace=bit32.replace(0xa505,0xa,4,4)}"
        ).call().checktable();
        for (String key : new String[]{"band","bor","bxor","bnot","lshift","rshift","arshift","extract","replace"}) {
            line("bit32." + key, bit32.get(key).tolong());
        }

        Path temp = Files.createTempDirectory("asobmashow-trace-driver-");
        Path returns = temp.resolve("returns.lua");
        Path input = temp.resolve("input.txt");
        Path output = temp.resolve("output.txt");
        Files.writeString(returns, "return 'alpha', 7\n");
        Files.writeString(input, "first-line\nsecond-line\n");
        same.set("trace_returns", LuaValue.valueOf(returns.toString()));
        same.set("trace_input", LuaValue.valueOf(input.toString()));
        same.set("trace_output", LuaValue.valueOf(output.toString()));
        LuaTable files = same.load(
            "local a,b=dofile(trace_returns); local r=io.open(trace_input); local first=r:lines()(); " +
            "local rc=r:close(); local w=io.open(trace_output,'w'); local same_handle=(w:write()==w); " +
            "local chained=(w:write(a,':',b)==w); local wc=w:close(); " +
            "return {a=a,b=b,first=first,read_close=rc,write_same=same_handle,write_chain=chained,write_close=wc}"
        ).call().checktable();
        line("files.dofileFirst", files.get("a").tojstring());
        line("files.dofileSecond", files.get("b").toint());
        line("files.firstLine", files.get("first").tojstring());
        line("files.readClose", files.get("read_close").toboolean());
        line("files.zeroWriteSameHandle", files.get("write_same").toboolean());
        line("files.chainedWriteSameHandle", files.get("write_chain").toboolean());
        line("files.writeClose", files.get("write_close").toboolean());

        LuaTable additionalFiles = same.load(
            "local r=io.open(trace_input,'r'); local first=r:lines()(); local rc=r:close(); " +
            "local a=io.open(trace_output,'a'); local same_handle=(a:write(':tail')==a); local ac=a:close(); " +
            "return {first=first,read_close=rc,append_same=same_handle,append_close=ac}"
        ).call().checktable();
        line("files.explicitReadFirstLine", additionalFiles.get("first").tojstring());
        line("files.explicitReadClose", additionalFiles.get("read_close").toboolean());
        line("files.appendSameHandle", additionalFiles.get("append_same").toboolean());
        line("files.appendClose", additionalFiles.get("append_close").toboolean());
        line("files.outputContent", Files.readString(output));

        LuaTable luajava = new LuaTable();
        same.get("package").get("loaded").set("luajava", luajava);
        LuaTable required = same.load(
            "local first=require('luajava'); local second=require('luajava'); " +
            "return {same=first==second, installed=first==package.loaded.luajava}"
        ).call().checktable();
        line("legacy.repeatedRequireSameTable", required.get("same").toboolean());
        line("legacy.requireUsesInstalledTable", required.get("installed").toboolean());
        line("legacy.gdxAppPresent", Gdx.app != null);

        long seed = 1592594996L;
        RecordingRandom random = new RecordingRandom(seed);
        MathUtils.random = random;
        IntMap<String> map = new IntMap<>(4, 0.8f);
        List<Integer> capacities = new ArrayList<>();
        capacities.add(capacity(map));
        map.put(1, "one"); capacities.add(capacity(map));
        map.put(217, "two-one-seven"); capacities.add(capacity(map));
        map.put(545, "five-four-five"); capacities.add(capacity(map));
        map.put(761, "seven-six-one"); capacities.add(capacity(map));
        map.put(217, "TWO-ONE-SEVEN"); capacities.add(capacity(map));
        map.put(977, "nine-seven-seven"); capacities.add(capacity(map));
        map.put(1305, "thirteen-oh-five"); capacities.add(capacity(map));
        map.put(1521, "fifteen-twenty-one"); capacities.add(capacity(map));
        map.put(1737, "seventeen-thirty-seven"); capacities.add(capacity(map));
        map.put(1953, "nineteen-fifty-three"); capacities.add(capacity(map));
        map.put(2281, "twenty-two-eighty-one"); capacities.add(capacity(map));
        map.put(2497, "twenty-four-ninety-seven"); capacities.add(capacity(map));
        map.put(2713, "twenty-seven-thirteen"); capacities.add(capacity(map));
        StringBuilder order = new StringBuilder();
        for (IntMap.Entry<String> item : map) {
            if (order.length() > 0) order.append(',');
            order.append(item.key);
        }
        line("intmap.seed", seed);
        line("intmap.draws", String.join(",", random.draws));
        line("intmap.capacities", capacities.toString().replace(" ", ""));
        line("intmap.order", order);
        line("intmap.size", map.size);
        line("intmap.replacement", map.get(217));

        Files.deleteIfExists(output);
        Files.deleteIfExists(input);
        Files.deleteIfExists(returns);
        Files.deleteIfExists(temp);
    }
}
'''


def run(*arguments: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        list(arguments),
        cwd=cwd,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )


def validate_reference(root: Path) -> None:
    if not root.is_dir():
        raise RuntimeError(f"Beatoraja root is not a directory: {root}")
    head = run("git", "-C", str(root), "rev-parse", "HEAD")
    if head.returncode != 0 or head.stdout.strip() != PINNED_COMMIT:
        raise RuntimeError(
            f"Beatoraja reference must be {PINNED_COMMIT}; observed {head.stdout.strip() or 'unavailable'}"
        )
    status = run("git", "-C", str(root), "status", "--porcelain")
    if status.returncode != 0 or status.stdout.strip():
        raise RuntimeError("Beatoraja reference must be clean")
    for relative, markers in REFERENCE_MARKERS.items():
        path = root / relative
        if not path.is_file():
            raise RuntimeError(f"missing pinned reference file: {relative}")
        text = path.read_text(encoding="utf-8")
        for marker in markers:
            if marker not in text:
                raise RuntimeError(f"missing pinned source marker {relative}: {marker}")
    if not (root / "lib/gdx.jar").is_file():
        raise RuntimeError("missing pinned lib/gdx.jar")
    if not (root / "lib/luaj-jse-3.0.2-custom.jar").is_file():
        raise RuntimeError("missing pinned LuaJ jar")


def run_java_driver(root: Path) -> dict[str, str]:
    classpath = ":".join(
        str(root / "lib" / name)
        for name in ("gdx.jar", "luaj-jse-3.0.2-custom.jar")
    )
    with tempfile.TemporaryDirectory(prefix="asobmashow-beatoraja-traces-") as temporary:
        temporary_path = Path(temporary)
        source = temporary_path / "TraceDriver.java"
        source.write_text(JAVA_DRIVER, encoding="utf-8")
        compile_result = run(
            "javac", "-encoding", "UTF-8", "-cp", classpath,
            "-d", str(temporary_path), str(source),
        )
        if compile_result.returncode != 0:
            raise RuntimeError(f"trace driver compilation failed:\n{compile_result.stdout}")
        execute_result = run(
            "java", "-cp", f"{temporary_path}:{classpath}",
            "TraceDriver", str(TWO_PHASE_ENTRY),
        )
        if execute_result.returncode != 0:
            raise RuntimeError(f"trace driver execution failed:\n{execute_result.stdout}")
    facts: dict[str, str] = {}
    for line in execute_result.stdout.splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        facts[key] = value
    return facts


def provenance(path: str, symbol: str, behavior: str) -> dict[str, str]:
    return {
        "commit": PINNED_COMMIT,
        "path": path,
        "symbol": symbol,
        "behavior": behavior,
    }


def case(
    name: str,
    inputs: dict,
    expected: dict,
    *,
    precision: int | float = 0,
    call_order: list[str] | None = None,
    call_count: dict[str, int] | None = None,
) -> dict:
    result = {
        "name": name,
        "input": inputs,
        "expected": expected,
        "precision": precision,
    }
    if call_order is not None:
        result["callOrder"] = call_order
    if call_count is not None:
        result["callCount"] = call_count
    return result


def envelope(kind: str, provenance_items: list[dict], cases: list[dict]) -> dict:
    return {
        "schemaVersion": 1,
        "kind": kind,
        "referenceCommit": PINNED_COMMIT,
        "provenance": provenance_items,
        "cases": cases,
    }


def boolean(facts: dict[str, str], key: str) -> bool:
    return facts[key] == "true"


def integer(facts: dict[str, str], key: str) -> int:
    return int(facts[key])


def number(facts: dict[str, str], key: str) -> float:
    return float(facts[key])


def build_traces(facts: dict[str, str]) -> dict[str, dict]:
    bit32_expected = {
        name: integer(facts, f"bit32.{name}")
        for name in ("band", "bor", "bxor", "bnot", "lshift", "rshift", "arshift", "extract", "replace")
    }
    iteration_keys = [f"key:{value}" for value in facts["intmap.order"].split(",")]
    random_draws = []
    if facts["intmap.draws"]:
        random_draws = [
            {"bound": int(item.split(":", 1)[0]), "result": int(item.split(":", 1)[1])}
            for item in facts["intmap.draws"].split(",")
        ]
    capacity_history = json.loads(facts["intmap.capacities"])

    traces = {}
    traces["lua-language"] = envelope(
        "lua-language",
        [
            provenance(
                "src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java",
                "LuaSkinLoader.loadHeader/load",
                "header execution and configured execution reuse one SkinLuaAccessor state",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java",
                "SkinLuaAccessor.initializeModules/execFile",
                "package.loaded and globals persist within one accessor and start fresh in a new accessor",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java",
                "LuaSkinLoader.fromLuaValue/serializeLuaScript",
                "Lua values coerce to model primitive and table shapes",
            ),
        ],
        [
            case(
                "two-phase-require-before-branch",
                {
                    "entry": "lua/two_phase/entry.luaskin",
                    "requiredModule": "shared",
                    "requirePrecedesSkinConfigBranch": True,
                },
                {
                    "packageLoaded": boolean(facts, "configured.packageLoaded"),
                    "header": {
                        "packageLoaded": boolean(facts, "header.packageLoaded"),
                        "moduleLoadCount": integer(facts, "header.moduleLoadCount"),
                        "globalMutation": "header-mutated",
                    },
                    "configuredSameState": {
                        "packageLoaded": boolean(facts, "configured.packageLoaded"),
                        "moduleLoadCount": integer(facts, "configured.moduleLoadCount"),
                        "sameModuleTable": boolean(facts, "configured.sameModuleTable"),
                        "observedHeaderMutation": facts["configured.observedHeaderMutation"],
                        "observedGlobalMutation": facts["configured.observedGlobalMutation"],
                    },
                    "freshCatalogBeforeExecution": {
                        "packageLoaded": boolean(facts, "fresh.packageLoaded"),
                        "moduleLoadCount": integer(facts, "fresh.moduleLoadCount"),
                        "globalMutation": None if facts["fresh.globalMutation"] == "null" else facts["fresh.globalMutation"],
                    },
                },
                call_order=["header", "configured-same-state"],
                call_count={"entryExecutions": 2, "sharedModuleLoads": 1},
            ),
            case(
                "lua-value-conversions",
                {
                    "boolean": 1,
                    "integerFromNumber": 17.9,
                    "float": 12.5,
                    "stringSource": 42,
                    "orderedTable": {"1": "first", "2": "second", "named": "named"},
                    "arraySource": "not-a-table",
                },
                {
                    "boolean": boolean(facts, "conversion.boolean"),
                    "integer": integer(facts, "conversion.integer"),
                    "float": number(facts, "conversion.float"),
                    "stringFromNumber": facts["conversion.stringFromNumber"],
                    "nonTableArrayLength": 0,
                    "orderedTableValues": facts["conversion.ordered"].split(","),
                },
                precision=1e-7,
            ),
            case(
                "bit32-operations",
                {
                    "band": [65520, 255],
                    "bor": [3840, 255],
                    "bxor": [240, 15],
                    "bnot": 0,
                    "lshift": [1, 31],
                    "rshift": [4294967294, 1],
                    "arshift": [4294967294, 1],
                    "extract": [240, 4, 4],
                    "replace": [42245, 10, 4, 4],
                },
                bit32_expected,
            ),
        ],
    )

    traces["destination"] = envelope(
        "destination",
        [
            provenance(
                "src/bms/player/beatoraja/skin/SkinObject.java",
                "SkinObject.prepareRegion/getRate/prepareColor/prepareAngle/prepareClip",
                "timer, loop, easing, geometry, color, angle, clip, and offset evaluation",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/property/TimerProperty.java",
                "TimerProperty.isOff",
                "Long.MIN_VALUE distinguishes an off destination timer",
            ),
        ],
        [
            case(
                "timer-off-suppresses-draw",
                {"nowMillis": 500, "timerStartMicros": -9223372036854775808},
                {"draw": False},
            ),
            case(
                "loop-and-easing-vectors",
                {
                    "endTimeMillis": 1000,
                    "stopSampleMillis": 1001,
                    "loopPointMillis": 400,
                    "loopSampleMillis": 1250,
                    "intervalFraction": 0.25,
                },
                {
                    "stopAtEndTimeMillis": -1,
                    "wrappedTimeMillis": 650,
                    "easingRates": {
                        "linear": 0.25,
                        "easeIn": 0.0625,
                        "easeOut": 0.4375,
                        "step": 0.0,
                    },
                },
                precision=1e-7,
            ),
            case(
                "color-angle-clip-offset-interpolation",
                {
                    "rate": 0.5,
                    "from": {
                        "rect": {"x": 10, "y": 20, "w": 100, "h": 50},
                        "rgba": [0, 0, 0, 128],
                        "angle": 20,
                        "clip": {"x": 0, "y": 0, "w": 60, "h": 30},
                    },
                    "to": {
                        "rect": {"x": 30, "y": 60, "w": 140, "h": 70},
                        "rgba": [255, 128, 64, 255],
                        "angle": 60,
                        "clip": {"x": 10, "y": 20, "w": 100, "h": 50},
                    },
                    "offset": {"x": 14, "y": 7, "w": 20, "h": 10, "r": 10, "a": 0},
                    "relative": False,
                },
                {
                    "rect": {"x": 24.0, "y": 42.0, "w": 140.0, "h": 70.0},
                    "rgbaNormalized": [0.5, 0.2509804, 0.1254902, 0.7509804],
                    "angle": 50,
                    "clip": {"x": 9.0, "y": 12.0, "w": 100.0, "h": 50.0},
                },
                precision=1e-6,
            ),
        ],
    )

    traces["properties"] = envelope(
        "properties",
        [
            provenance(
                "src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java",
                "LuaSkinLoader.serializeLuaScript",
                "model properties dispatch functions, numeric IDs, recognized names, and script strings distinctly",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java",
                "OptionFunction/NumberFunction/FloatNumberFunction/TextFunction/TimerFunction/EventExecFunction/EventIndexFunction",
                "direct main_state calls use their named factories or direct state APIs",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/property/TimerPropertyFactory.java",
                "TimerPropertyFactory.getTimerProperty",
                "negative timer IDs are unsupported",
            ),
        ],
        [
            case(
                "model-property-dispatch",
                {
                    "values": ["lua-function", 7, "recognized-name", "return function() return 9 end", True],
                    "timerIds": [-1, 0],
                },
                {
                    "forms": {
                        "function": "runtime-callback",
                        "numeric": "built-in-id",
                        "recognizedString": "built-in-name",
                        "unrecognizedString": "runtime-script",
                        "other": "unsupported-null",
                    },
                    "timerNegativeId": "unsupported-null",
                    "timerZeroId": "built-in-id",
                },
            ),
            case(
                "direct-main-state-dispatch",
                {
                    "functions": ["option", "number", "float_number", "text", "event_index", "timer", "event_exec"],
                    "unknownId": 2147483647,
                },
                {
                    "factories": {
                        "option": "BooleanPropertyFactory.getBooleanProperty",
                        "number": "IntegerPropertyFactory.getIntegerProperty",
                        "float_number": "FloatPropertyFactory.getRateProperty",
                        "text": "StringPropertyFactory.getStringProperty",
                        "event_index": "IntegerPropertyFactory.getImageIndexProperty",
                        "timer": "MainState.timer.getMicroTimer",
                        "event_exec": "MainState.executeEvent",
                    },
                    "unknownDirectLookup": "dereference-error",
                },
            ),
        ],
    )

    traces["timers-events"] = envelope(
        "timers-events",
        [
            provenance(
                "src/bms/player/beatoraja/skin/CustomTimer.java",
                "CustomTimer.update/getMicroTimer",
                "active timer callback values are cached by the once-per-frame update",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/CustomEvent.java",
                "CustomEvent.execute/update",
                "automatic conditions observe current timers and enforce minimum intervals",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/Skin.java",
                "Skin.updateCustomObjects",
                "all custom timers update before all automatic custom events",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java",
                "SkinLuaAccessor.loadEvent",
                "Lua event functions support exactly zero, one, or two declared arguments",
            ),
            provenance(
                "lib/gdx.jar",
                "com.badlogic.gdx.utils.IntMap.put/Entries",
                "synthetic nonempty maps iterate backing-table order and collision eviction uses MathUtils.random",
            ),
        ],
        [
            case(
                "timer-off-sentinel",
                {"off": -9223372036854775808, "eventTimestampMicros": 0},
                {"offValue": -9223372036854775808, "eventAtZeroIsOn": True, "offElapsedMicros": 0},
            ),
            case(
                "custom-timer-once-per-frame",
                {"frame1Callback": 123456, "frame2Callback": 234567, "readsPerFrame": 2},
                {"readsWithinFrame": [123456, 123456], "nextFrameRead": 234567},
                call_order=["frame1:update", "frame1:read", "frame1:read", "frame2:update", "frame2:read"],
                call_count={"frame1TimerFunction": 1, "frame2TimerFunction": 1},
            ),
            case(
                "timer-phase-before-event-phase",
                {"timerIdsInBackingOrder": [17, 3], "eventIdsInBackingOrder": [9, 4], "updatedValue": 7000},
                {"eventObservedTimerValue": 7000, "phaseOrder": ["customTimers", "customEvents"]},
                call_order=["timer:17", "timer:3", "event:9", "event:4"],
                call_count={"timer:17": 1, "timer:3": 1, "event:9": 1, "event:4": 1},
            ),
            case(
                "zero-one-two-argument-events",
                {"zero": [], "one": [11], "two": [11, 22], "three": [1, 2, 3]},
                {"observedArguments": [[], [11], [11, 22]], "threeArgumentFunction": "unsupported-null"},
                call_order=["zeroArg", "oneArg", "twoArg"],
                call_count={"zeroArg": 1, "oneArg": 1, "twoArg": 1},
            ),
            case(
                "libgdx-intmap-backing-order",
                {
                    "traceSetup": {
                        "initialCapacity": 4,
                        "loadFactor": 0.8,
                        "randomImplementation": "java.util.Random",
                        "randomSeed": integer(facts, "intmap.seed"),
                        "randomStateBefore": "setSeed-before-first-put",
                        "randomStateAfter": "captured-after-final-put",
                        "randomDraws": random_draws,
                        "capacityAfterEachOperation": capacity_history,
                        "operations": [
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
                        "collisionAndResizeExercised": bool(random_draws) and len(set(capacity_history)) > 1,
                    },
                },
                {
                    "iterationKeys": iteration_keys,
                    "replacementValue": facts["intmap.replacement"],
                    "size": integer(facts, "intmap.size"),
                    "selectedConfiguredMaps": {"customTimers": 0, "customEvents": 0},
                    "scope": "trace-specific-not-universal-order",
                },
                call_order=iteration_keys,
                call_count={"uniqueInsertions": 12, "replacements": 1, "rngDraws": len(random_draws)},
            ),
        ],
    )

    traces["legacy-lua-upstream"] = envelope(
        "legacy-lua-upstream",
        [
            provenance(
                "src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java",
                "SkinLuaAccessor.createStandardGlobals/RestrictedIoLib/execFile",
                "standard LuaJ dofile and io handle shapes are available in the pinned accessor",
            ),
            provenance(
                "src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java",
                "LegacySkinLuaApi.install/BindClassFunction/NewFunction/fileFacade/gdxFacade",
                "the pinned upstream facade preloads luajava and exposes selected File and Gdx shapes among broader branches",
            ),
        ],
        [
            case(
                "standard-file-call-shapes",
                {"dofileArguments": ["relative-text-chunk"], "ioOpenModes": ["default", "r", "w", "a"]},
                {
                    "dofileReturn": [facts["files.dofileFirst"], integer(facts, "files.dofileSecond")],
                    "ioOpenModes": ["default", "r", "w", "a"],
                    "handleMethods": ["lines", "write", "close"],
                    "firstLine": facts["files.firstLine"],
                    "explicitReadFirstLine": facts["files.explicitReadFirstLine"],
                    "zeroArgumentWriteReturnsSameHandle": boolean(facts, "files.zeroWriteSameHandle"),
                    "chainedWriteReturnsSameHandle": boolean(facts, "files.chainedWriteSameHandle"),
                    "appendReturnsSameHandle": boolean(facts, "files.appendSameHandle"),
                    "outputContent": facts["files.outputContent"],
                    "closeReturnsTrue": all(
                        boolean(facts, key)
                        for key in (
                            "files.writeClose",
                            "files.readClose",
                            "files.explicitReadClose",
                            "files.appendClose",
                        )
                    ),
                },
                call_order=[
                    "dofile",
                    "io.open:default",
                    "lines:default",
                    "close:default",
                    "io.open:w",
                    "write:zero",
                    "write:multiple",
                    "close:w",
                    "io.open:r",
                    "lines:r",
                    "close:r",
                    "io.open:a",
                    "write:a",
                    "close:a",
                ],
                call_count={"dofile": 1, "ioOpen": 4, "lines": 2, "write": 3, "close": 4},
            ),
            case(
                "legacy-luajava-selected-facts",
                {
                    "requires": ["luajava", "luajava"],
                    "classRequests": ["java.io.File", "com.badlogic.gdx.Gdx"],
                    "fileArguments": ["File-token", "relative-path"],
                },
                {
                    "repeatedRequireSameTable": boolean(facts, "legacy.repeatedRequireSameTable"),
                    "requireUsesInstalledTable": boolean(facts, "legacy.requireUsesInstalledTable"),
                    "bindClasses": ["java.io.File", "com.badlogic.gdx.Gdx"],
                    "fileConstructorArguments": ["File-token", "relative-path"],
                    "listShape": "normalized-relative-path-array-or-nil",
                    "gdxAppPresent": boolean(facts, "legacy.gdxAppPresent"),
                    "upstreamBroaderBranches": [
                        "newInstance",
                        "URL-and-HTTP",
                        "controllers-and-input",
                        "debug-getmetatable",
                    ],
                },
                call_order=["install", "require:first", "require:second", "bindClass:File", "new:File", "listFiles", "bindClass:Gdx", "read:Gdx.app"],
                call_count={"require": 2, "bindClass": 2, "new": 1, "listFiles": 1, "gdxAppRead": 1},
            ),
        ],
    )
    return traces


def serialized(trace: dict) -> bytes:
    return (json.dumps(trace, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode("utf-8")


def write_or_verify(traces: dict[str, dict], output_dir: Path, verify: bool) -> None:
    if verify:
        failures = []
        for kind, filename in TRACE_FILENAMES.items():
            path = output_dir / filename
            expected = serialized(traces[kind])
            if not path.is_file():
                failures.append(f"missing {path}")
            elif path.read_bytes() != expected:
                failures.append(f"trace differs: {path}")
        if failures:
            raise RuntimeError("trace verification failed:\n" + "\n".join(failures))
        return

    output_dir.mkdir(parents=True, exist_ok=True)
    for kind, filename in TRACE_FILENAMES.items():
        path = output_dir / filename
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_bytes(serialized(traces[kind]))
        temporary.replace(path)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--beatoraja-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--verify", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        reference_root = arguments.beatoraja_root.expanduser().resolve()
        validate_reference(reference_root)
        facts = run_java_driver(reference_root)
        traces = build_traces(facts)
        write_or_verify(traces, arguments.output_dir, arguments.verify)
        action = "verified" if arguments.verify else "wrote"
        print(f"Beatoraja skin traces {action}: {PINNED_COMMIT}")
        return 0
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
