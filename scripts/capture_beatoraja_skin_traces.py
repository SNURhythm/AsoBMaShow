#!/usr/bin/env python3
"""Capture differential traces by executing pinned Beatoraja source methods.

The live harness compiles selected original source files from an explicit,
clean, pinned Beatoraja checkout. Unrelated application services are replaced
with narrow test doubles under a Python-owned temporary root. Java owns every
case input, expected value, precision, call order, and call count; Python only
validates, normalizes, envelopes, and serializes those case payloads.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path


PINNED_COMMIT = "c2ed5db1a46145ed10790c3872f717e95b59db9d"
ROOT = Path(__file__).resolve().parents[1]
MANIFEST_PATH = ROOT / "tests/fixtures/beatoraja_skin/reference_manifest.json"
TWO_PHASE_ENTRY = ROOT / "tests/fixtures/beatoraja_skin/lua/two_phase/entry.luaskin"

TRACE_FILENAMES = {
    "lua-language": "lua_language_v1.json",
    "destination": "destination_v1.json",
    "properties": "properties_v1.json",
    "timers-events": "timers_events_v1.json",
    "legacy-lua-upstream": "legacy_lua_upstream_v1.json",
}

EXPECTED_CASE_KEYS = (
    "lua-language/two-phase-require-before-branch",
    "lua-language/lua-value-conversions",
    "lua-language/bit32-operations",
    "destination/timer-off-suppresses-draw",
    "destination/loop-and-easing-vectors",
    "destination/color-angle-clip-offset-interpolation",
    "properties/model-property-dispatch",
    "properties/direct-main-state-dispatch",
    "timers-events/timer-off-sentinel",
    "timers-events/custom-timer-once-per-frame",
    "timers-events/timer-phase-before-event-phase",
    "timers-events/zero-one-two-argument-events",
    "timers-events/libgdx-intmap-backing-order",
    "legacy-lua-upstream/standard-file-call-shapes",
    "legacy-lua-upstream/legacy-luajava-selected-facts",
)

PINNED_SOURCE_UNITS = (
    "src/bms/player/beatoraja/DisposableObject.java",
    "src/bms/player/beatoraja/skin/StretchType.java",
    "src/bms/player/beatoraja/skin/SkinObject.java",
    "src/bms/player/beatoraja/skin/Skin.java",
    "src/bms/player/beatoraja/skin/CustomTimer.java",
    "src/bms/player/beatoraja/skin/CustomEvent.java",
    "src/bms/player/beatoraja/skin/property/BooleanProperty.java",
    "src/bms/player/beatoraja/skin/property/IntegerProperty.java",
    "src/bms/player/beatoraja/skin/property/FloatProperty.java",
    "src/bms/player/beatoraja/skin/property/StringProperty.java",
    "src/bms/player/beatoraja/skin/property/FloatWriter.java",
    "src/bms/player/beatoraja/skin/property/StringWriter.java",
    "src/bms/player/beatoraja/skin/property/Event.java",
    "src/bms/player/beatoraja/skin/property/TimerProperty.java",
    "src/bms/player/beatoraja/skin/property/TimerPropertyFactory.java",
    "src/bms/player/beatoraja/skin/lua/LuaApiExporter.java",
    "src/bms/player/beatoraja/skin/lua/SkinLuaPathResolver.java",
    "src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java",
    "src/bms/player/beatoraja/skin/lua/MainStateAccessor.java",
    "src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java",
    "src/bms/player/beatoraja/skin/lua/TimerUtility.java",
    "src/bms/player/beatoraja/skin/lua/EventUtility.java",
    "src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java",
    "src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java",
)

REFERENCE_MARKERS = {
    "src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java": (
        "SkinHeader loadHeader(Path p)",
        "<T> T fromLuaValue(Class<T> cls, LuaValue lv)",
        "serializeLuaScript",
    ),
    "src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java": (
        "createStandardGlobals",
        "public LuaValue execFile(Path path)",
        "public Event loadEvent(LuaFunction function)",
    ),
    "src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java": (
        "globals.package_.setIsLoaded(\"luajava\", luajava)",
        "private static LuaTable fileFacade",
        "private static LuaTable gdxFacade()",
    ),
    "src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java": (
        "private static final long TIMER_OFF_VALUE = Long.MIN_VALUE",
        "class EventExecFunction",
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
    "src/bms/player/beatoraja/skin/CustomTimer.java": (
        "private long time = Long.MIN_VALUE",
        "public void update(MainState state)",
    ),
    "src/bms/player/beatoraja/skin/CustomEvent.java": (
        "private long lastExecuteTime = Long.MIN_VALUE",
        "public void update(MainState state)",
    ),
    "src/bms/player/beatoraja/skin/property/TimerProperty.java": (
        "return getMicro(state) == Long.MIN_VALUE",
    ),
    "src/bms/player/beatoraja/skin/property/TimerPropertyFactory.java": (
        "if (timerId < 0)",
    ),
}

PROVENANCE = {
    "lua-language": (
        ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.fromLuaValue/serializeLuaScript", "actual pinned loader converts Lua values and dispatches model properties"),
        ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.createStandardGlobals/execFile", "actual pinned accessor retains package and global state across two-phase execution"),
    ),
    "destination": (
        ("src/bms/player/beatoraja/skin/SkinObject.java", "SkinObject.prepare/prepareRegion/getRate/prepareColor/prepareAngle/prepareClip", "actual pinned object evaluates timer, loop, easing, geometry, normalized color, angle, clip, and offset behavior"),
        ("src/bms/player/beatoraja/skin/property/TimerProperty.java", "TimerProperty.isOff", "actual pinned timer sentinel suppresses destination drawing"),
    ),
    "properties": (
        ("src/bms/player/beatoraja/skin/lua/LuaSkinLoader.java", "LuaSkinLoader.fromLuaValue/serializeLuaScript", "actual pinned model property dispatch distinguishes functions, IDs, names, scripts, and unsupported values"),
        ("src/bms/player/beatoraja/skin/lua/MainStatePropertyLuaApiExporter.java", "MainStatePropertyLuaApiExporter.export", "actual pinned direct host functions call their matching factories and state methods"),
        ("src/bms/player/beatoraja/skin/property/TimerPropertyFactory.java", "TimerPropertyFactory.getTimerProperty", "actual pinned factory rejects negative timer IDs"),
    ),
    "timers-events": (
        ("src/bms/player/beatoraja/skin/CustomTimer.java", "CustomTimer.update/getMicroTimer", "actual pinned custom timer caches one callback result per update"),
        ("src/bms/player/beatoraja/skin/CustomEvent.java", "CustomEvent.update/execute", "actual pinned custom event evaluates conditions and action arguments"),
        ("src/bms/player/beatoraja/skin/Skin.java", "Skin.updateCustomObjects", "actual pinned skin updates timer map before event map"),
        ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.loadEvent", "actual pinned accessor adapts zero, one, and two argument Lua functions"),
        ("lib/gdx.jar", "com.badlogic.gdx.utils.IntMap.put/Entries", "actual pinned IntMap records requested/backing capacities, collision RNG state, replacement sequence, and resulting backing order"),
    ),
    "legacy-lua-upstream": (
        ("src/bms/player/beatoraja/skin/lua/SkinLuaAccessor.java", "SkinLuaAccessor.RestrictedIoLib/execFile", "actual pinned accessor supplies standard dofile and restricted io handle shapes"),
        ("src/bms/player/beatoraja/skin/lua/LegacySkinLuaApi.java", "LegacySkinLuaApi.install/BindClassFunction/NewFunction/fileFacade/gdxFacade", "actual pinned legacy facade supplies repeated require identity, File construction/listing, and a Gdx table without app"),
    ),
}


STUB_SOURCES = {
    "bms/player/beatoraja/Resolution.java": r'''
package bms.player.beatoraja;
public final class Resolution {
    public final int width, height;
    public Resolution(int width, int height) { this.width = width; this.height = height; }
}
''',
}


JAVA_DRIVER = r'''
package bms.player.beatoraja.skin.lua;

import bms.player.beatoraja.MainState;
import bms.player.beatoraja.skin.CustomEvent;
import bms.player.beatoraja.skin.CustomTimer;
import bms.player.beatoraja.skin.Skin;
import bms.player.beatoraja.skin.SkinHeader;
import bms.player.beatoraja.skin.SkinObject;
import bms.player.beatoraja.skin.property.*;
import com.badlogic.gdx.math.MathUtils;
import com.badlogic.gdx.math.Rectangle;
import com.badlogic.gdx.utils.IntMap;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Random;
import org.luaj.vm2.LuaFunction;
import org.luaj.vm2.LuaTable;
import org.luaj.vm2.LuaValue;
import org.luaj.vm2.lib.OneArgFunction;
import org.luaj.vm2.lib.ThreeArgFunction;
import org.luaj.vm2.lib.TwoArgFunction;
import org.luaj.vm2.lib.ZeroArgFunction;

public final class TraceDriver {
    private static String mutation;

    public static final class ConversionModel {
        public boolean booleanValue;
        public int integerValue;
        public float floatValue;
        public String stringValue;
    }

    private static final class TraceObject extends SkinObject {
        @Override public void draw(Skin.SkinObjectRenderer renderer) { }
        @Override public void dispose() { }
    }

    private static final class RecordingJavaRandom extends Random {
        private static final long MULTIPLIER = 0x5DEECE66DL;
        private static final long ADDEND = 0xBL;
        private static final long MASK = (1L << 48) - 1;
        private long lcgState;
        final List<Object> draws = new ArrayList<>();

        RecordingJavaRandom(long seed) {
            super(0L);
            lcgState = (seed ^ MULTIPLIER) & MASK;
        }

        @Override protected synchronized int next(int bits) {
            lcgState = (lcgState * MULTIPLIER + ADDEND) & MASK;
            return (int)(lcgState >>> (48 - bits));
        }

        @Override public int nextInt(int bound) {
            int result = super.nextInt(bound);
            draws.add(map("bound", bound, "result", result));
            return result;
        }

        String stateDigest() throws Exception {
            return sha256("java.util.Random-lcg48:" + Long.toUnsignedString(lcgState));
        }
    }

    private static boolean mutate(String caseKey) { return caseKey.equals(mutation); }

    private static Map<String,Object> map(Object... pairs) {
        if ((pairs.length & 1) != 0) throw new IllegalArgumentException("odd map pairs");
        Map<String,Object> value = new LinkedHashMap<>();
        for (int index = 0; index < pairs.length; index += 2) {
            value.put((String)pairs[index], pairs[index + 1]);
        }
        return value;
    }

    private static List<Object> list(Object... values) {
        List<Object> result = new ArrayList<>();
        for (Object value : values) result.add(value);
        return result;
    }

    private static String json(Object value) {
        if (value == null) return "null";
        if (value instanceof String string) return quote(string);
        if (value instanceof Boolean || value instanceof Integer || value instanceof Long
                || value instanceof Float || value instanceof Double) return value.toString();
        if (value instanceof Map<?,?> source) {
            StringBuilder out = new StringBuilder("{");
            boolean first = true;
            for (Map.Entry<?,?> entry : source.entrySet()) {
                if (!first) out.append(',');
                first = false;
                out.append(quote((String)entry.getKey())).append(':').append(json(entry.getValue()));
            }
            return out.append('}').toString();
        }
        if (value instanceof Iterable<?> source) {
            StringBuilder out = new StringBuilder("[");
            boolean first = true;
            for (Object item : source) {
                if (!first) out.append(',');
                first = false;
                out.append(json(item));
            }
            return out.append(']').toString();
        }
        throw new IllegalArgumentException("unsupported json value " + value.getClass());
    }

    private static String quote(String value) {
        StringBuilder out = new StringBuilder("\"");
        for (int index = 0; index < value.length(); index++) {
            char c = value.charAt(index);
            switch (c) {
                case '\\' -> out.append("\\\\");
                case '"' -> out.append("\\\"");
                case '\n' -> out.append("\\n");
                case '\r' -> out.append("\\r");
                case '\t' -> out.append("\\t");
                default -> {
                    if (c < 0x20) out.append(String.format("\\u%04x", (int)c));
                    else out.append(c);
                }
            }
        }
        return out.append('"').toString();
    }

    private static void emitCase(
            String kind, String name, Map<String,Object> input, Map<String,Object> expected,
            Number precision, List<Object> callOrder, Map<String,Object> callCount) {
        Map<String,Object> payload = map(
                "name", name, "input", input, "expected", expected, "precision", precision);
        if (callOrder != null) payload.put("callOrder", callOrder);
        if (callCount != null) payload.put("callCount", callCount);
        String encoded = Base64.getEncoder().encodeToString(json(payload).getBytes(StandardCharsets.UTF_8));
        System.out.println("FACT\t" + kind + "/" + name + "\t" + encoded);
    }

    private static String sha256(String value) throws Exception {
        byte[] digest = MessageDigest.getInstance("SHA-256").digest(value.getBytes(StandardCharsets.UTF_8));
        StringBuilder out = new StringBuilder();
        for (byte item : digest) out.append(String.format("%02x", item & 0xff));
        return out.toString();
    }

    private static long privateLong(Object target, String fieldName) throws Exception {
        Field field = target.getClass().getSuperclass().getDeclaredField(fieldName);
        field.setAccessible(true);
        return field.getLong(target);
    }

    private static int intMapCapacity(IntMap<?> map) throws Exception {
        Field field = IntMap.class.getDeclaredField("capacity");
        field.setAccessible(true);
        return field.getInt(map);
    }

    private static void captureTwoPhase(Path entry) {
        String key = "lua-language/two-phase-require-before-branch";
        SkinLuaAccessor accessor = new SkinLuaAccessor(false);
        accessor.setDirectory(entry.getParent());
        if (mutate(key)) accessor.exec("__trace_header_mutation='header-mutated-probe'; return true");
        LuaTable header = accessor.execFile(entry).checktable();
        LuaTable configured;
        accessor.exec("skin_config={}; return true");
        configured = accessor.execFile(entry).checktable();
        SkinLuaAccessor fresh = new SkinLuaAccessor(false);
        fresh.setDirectory(entry.getParent());
        LuaTable freshFacts = fresh.exec(
                "return {loaded=package.loaded.shared~=nil,count=__trace_shared_load_count or 0," +
                "global_value=__trace_two_phase_global}").checktable();
        emitCase("lua-language", "two-phase-require-before-branch",
                map("entry", "lua/two_phase/entry.luaskin", "requiredModule", "shared",
                        "requirePrecedesSkinConfigBranch", true,
                        "headerMutation", mutate(key) ? "header-mutated-probe" : "header-mutated"),
                map("packageLoaded", configured.get("package_loaded").toboolean(),
                        "header", map("packageLoaded", header.get("package_loaded").toboolean(),
                                "moduleLoadCount", header.get("module_load_count").toint(),
                                "globalMutation", header.get("mutation").tojstring()),
                        "configuredSameState", map(
                                "packageLoaded", configured.get("package_loaded").toboolean(),
                                "moduleLoadCount", configured.get("module_load_count").toint(),
                                "sameModuleTable", configured.get("same_module_table").toboolean(),
                                "observedHeaderMutation", configured.get("observed_header_mutation").tojstring(),
                                "observedGlobalMutation", configured.get("observed_global_mutation").tojstring()),
                        "freshCatalogBeforeExecution", map(
                                "packageLoaded", freshFacts.get("loaded").toboolean(),
                                "moduleLoadCount", freshFacts.get("count").toint(),
                                "globalMutation", freshFacts.get("global_value").isnil() ? null : freshFacts.get("global_value").tojstring())),
                0, list("header", "configured-same-state"),
                map("entryExecutions", 2, "sharedModuleLoads", header.get("module_load_count").toint()));
    }

    private static void captureLanguage() {
        SkinLuaAccessor accessor = new SkinLuaAccessor(false);
        LuaSkinLoader loader = new LuaSkinLoader();
        String conversionKey = "lua-language/lua-value-conversions";
        double integerSource = mutate(conversionKey) ? 18.9 : 17.9;
        LuaTable table = accessor.exec("return {booleanValue=true,integerValue=" + integerSource
                + ",floatValue=12.5,stringValue=42,ordered={[1]='first',[2]='second',named='named'}}")
                .checktable();
        ConversionModel model = loader.fromLuaValue(ConversionModel.class, table);
        String[] ordered = loader.fromLuaValue(String[].class, table.get("ordered"));
        String[] empty = loader.fromLuaValue(String[].class, LuaValue.valueOf("not-a-table"));
        List<Object> orderedValues = new ArrayList<>();
        for (String value : ordered) orderedValues.add(value);
        emitCase("lua-language", "lua-value-conversions",
                map("boolean", true, "integerFromNumber", integerSource, "float", 12.5,
                        "stringSource", 42, "orderedTable", map("1", "first", "2", "second", "named", "named"),
                        "arraySource", "not-a-table"),
                map("boolean", model.booleanValue, "integer", model.integerValue,
                        "float", model.floatValue, "stringFromNumber", model.stringValue,
                        "nonTableArrayLength", empty.length, "orderedTableValues", orderedValues),
                0.0000001, null, null);

        String bitKey = "lua-language/bit32-operations";
        int bandLeft = mutate(bitKey) ? 0xffe0 : 0xfff0;
        LuaTable bit = accessor.exec("return {band=bit32.band(" + bandLeft + ",0x00ff),"
                + "bor=bit32.bor(0xf00,0x0ff),bxor=bit32.bxor(0xf0,0x0f),bnot=bit32.bnot(0),"
                + "lshift=bit32.lshift(1,31),rshift=bit32.rshift(0xfffffffe,1),"
                + "arshift=bit32.arshift(0xfffffffe,1),extract=bit32.extract(0xf0,4,4),"
                + "replace=bit32.replace(0xa505,0xa,4,4)}").checktable();
        emitCase("lua-language", "bit32-operations",
                map("band", list(bandLeft, 255), "bor", list(3840,255), "bxor", list(240,15),
                        "bnot", 0, "lshift", list(1,31), "rshift", list(4294967294L,1),
                        "arshift", list(4294967294L,1), "extract", list(240,4,4),
                        "replace", list(42245,10,4,4)),
                map("band", bit.get("band").tolong(), "bor", bit.get("bor").tolong(),
                        "bxor", bit.get("bxor").tolong(), "bnot", bit.get("bnot").tolong(),
                        "lshift", bit.get("lshift").tolong(), "rshift", bit.get("rshift").tolong(),
                        "arshift", bit.get("arshift").tolong(), "extract", bit.get("extract").tolong(),
                        "replace", bit.get("replace").tolong()),
                0, null, null);
    }

    private static TraceObject easingObject(int acceleration) {
        TraceObject object = new TraceObject();
        object.setDestination(0,0,0,1,1,acceleration,255,255,255,255,0,0,0,0,0,(TimerProperty)null,new int[0]);
        object.setDestination(1000,1,0,1,1,acceleration,255,255,255,255,0,0,0,0,0,(TimerProperty)null,new int[0]);
        return object;
    }

    private static void captureDestination() throws Exception {
        MainState state = new MainState();
        String timerKey = "destination/timer-off-suppresses-draw";
        long timerValue = mutate(timerKey) ? 0 : Long.MIN_VALUE;
        TraceObject timerObject = new TraceObject();
        TimerProperty destinationTimer = ignored -> timerValue;
        timerObject.setDestination(0,0,0,10,10,0,255,255,255,255,0,0,0,0,0,destinationTimer,new int[0]);
        timerObject.prepare(500, state);
        emitCase("destination", "timer-off-suppresses-draw",
                map("nowMillis",500,"timerStartMicros",timerValue), map("draw",timerObject.draw), 0, null, null);

        String loopKey = "destination/loop-and-easing-vectors";
        long sample = mutate(loopKey) ? 1251 : 1250;
        TraceObject loop = easingObject(0);
        Field loopField = SkinObject.class.getDeclaredField("dstloop");
        loopField.setAccessible(true);
        loopField.setInt(loop, 400);
        loop.prepare(sample, state);
        TraceObject stop = easingObject(0);
        loopField.setInt(stop, -1);
        stop.prepare(1001, state);
        List<Object> rates = new ArrayList<>();
        for (int acceleration : new int[]{0,1,2,3}) {
            TraceObject object = easingObject(acceleration);
            object.prepare(250, state);
            rates.add(object.region.x);
        }
        emitCase("destination", "loop-and-easing-vectors",
                map("endTimeMillis",1000,"loopPointMillis",400,"loopSampleMillis",sample,
                        "stopSampleMillis",1001,"intervalFraction",0.25),
                map("stopDraw",stop.draw,"wrappedTimeMillis",privateLong(loop,"nowtime"),
                        "easingRates",map("linear",rates.get(0),"easeIn",rates.get(1),
                                "easeOut",rates.get(2),"step",rates.get(3))),
                0.0000001, null, null);

        String geometryKey = "destination/color-angle-clip-offset-interpolation";
        long geometryTime = mutate(geometryKey) ? 501 : 500;
        TraceObject geometry = new TraceObject();
        geometry.setDestination(0,10,20,100,50,0,128,0,0,0,0,0,20,0,0,(TimerProperty)null,0,0,0,1);
        geometry.setDestination(1000,30,60,140,70,0,255,255,128,64,0,0,60,0,0,(TimerProperty)null,0,0,0,1);
        geometry.setDestinationClip(0,new Rectangle(0,0,60,30));
        geometry.setDestinationClip(1000,new Rectangle(10,20,100,50));
        SkinObject.SkinOffset offset = new SkinObject.SkinOffset();
        offset.x=14; offset.y=7; offset.w=20; offset.h=10; offset.r=10; offset.a=0;
        state.setOffsetValue(1,offset);
        geometry.prepare(geometryTime,state);
        Rectangle clip=geometry.getClip();
        emitCase("destination", "color-angle-clip-offset-interpolation",
                map("sampleMillis",geometryTime,"from",map("rect",map("x",10,"y",20,"w",100,"h",50),
                                "rgba",list(0,0,0,128),"angle",20,"clip",map("x",0,"y",0,"w",60,"h",30)),
                        "to",map("rect",map("x",30,"y",60,"w",140,"h",70),
                                "rgba",list(255,128,64,255),"angle",60,"clip",map("x",10,"y",20,"w",100,"h",50)),
                        "offset",map("x",14,"y",7,"w",20,"h",10,"r",10,"a",0),"relative",false),
                map("rect",map("x",geometry.region.x,"y",geometry.region.y,"w",geometry.region.width,"h",geometry.region.height),
                        "rgbaNormalized",list(geometry.color.r,geometry.color.g,geometry.color.b,geometry.color.a),
                        "angle",geometry.angle,"clip",map("x",clip.x,"y",clip.y,"w",clip.width,"h",clip.height)),
                0.000001, null, null);
    }

    private static void captureProperties() {
        MainState state = new MainState();
        SkinLuaAccessor accessor = new SkinLuaAccessor(false);
        LuaSkinLoader loader = new LuaSkinLoader();
        String modelKey = "properties/model-property-dispatch";
        int numericId = mutate(modelKey) ? 8 : 7;
        BooleanProperty functionProperty = loader.fromLuaValue(BooleanProperty.class,
                accessor.exec("return function() return true end"));
        BooleanPropertyFactory.lastOrigin=null;
        BooleanProperty numericProperty = loader.fromLuaValue(BooleanProperty.class,LuaValue.valueOf(numericId));
        String numericOrigin=BooleanPropertyFactory.lastOrigin;
        BooleanPropertyFactory.lastOrigin=null;
        BooleanProperty namedProperty = loader.fromLuaValue(BooleanProperty.class,LuaValue.valueOf("recognized-name"));
        String namedOrigin=BooleanPropertyFactory.lastOrigin;
        BooleanPropertyFactory.lastOrigin=null;
        BooleanProperty scriptProperty = loader.fromLuaValue(BooleanProperty.class,LuaValue.valueOf("function() return false end"));
        BooleanProperty unsupported = loader.fromLuaValue(BooleanProperty.class,LuaValue.TRUE);
        TimerProperty negative = loader.fromLuaValue(TimerProperty.class,LuaValue.valueOf(-1));
        TimerProperty zero = loader.fromLuaValue(TimerProperty.class,LuaValue.valueOf(0));
        emitCase("properties", "model-property-dispatch",
                map("values",list("lua-function",numericId,"recognized-name","function() return false end",true),
                        "timerIds",list(-1,0)),
                map("forms",map("function","runtime-callback","numeric","built-in-id",
                                "recognizedString","built-in-name","unrecognizedString","runtime-script","other","unsupported-null"),
                        "observed",map("function",functionProperty.get(state),"numeric",numericProperty.get(state),
                                "numericOrigin",numericOrigin,"recognizedString",namedProperty.get(state),
                                "recognizedOrigin",namedOrigin,"unrecognizedString",scriptProperty.get(state),
                                "otherIsNull",unsupported==null),
                        "timerNegativeId",negative==null?"unsupported-null":"supported",
                        "timerZeroId",zero!=null?"built-in-id":"unsupported-null"),
                0, null, null);

        String directKey = "properties/direct-main-state-dispatch";
        int id=mutate(directKey)?8:7;
        state.timer.setMicroTimer(id,777000+id);
        LuaTable direct=new LuaTable();
        new MainStatePropertyLuaApiExporter(state).export(direct);
        boolean option=direct.get("option").call(LuaValue.valueOf(id)).toboolean();
        int number=direct.get("number").call(LuaValue.valueOf(id)).toint();
        double floatNumber=direct.get("float_number").call(LuaValue.valueOf(id)).todouble();
        String text=direct.get("text").call(LuaValue.valueOf(id)).tojstring();
        int eventIndex=direct.get("event_index").call(LuaValue.valueOf(id)).toint();
        long timer=direct.get("timer").call(LuaValue.valueOf(id)).tolong();
        direct.get("event_exec").call(LuaValue.valueOf(id),LuaValue.valueOf(11),LuaValue.valueOf(22));
        String unknown;
        try { direct.get("option").call(LuaValue.valueOf(Integer.MAX_VALUE)); unknown="no-error"; }
        catch(Throwable error) { unknown=error.getClass().getSimpleName(); }
        emitCase("properties", "direct-main-state-dispatch",
                map("functions",list("option","number","float_number","text","event_index","timer","event_exec"),
                        "id",id,"unknownId",Integer.MAX_VALUE),
                map("factories",map("option","BooleanPropertyFactory.getBooleanProperty",
                                "number","IntegerPropertyFactory.getIntegerProperty",
                                "float_number","FloatPropertyFactory.getRateProperty",
                                "text","StringPropertyFactory.getStringProperty",
                                "event_index","IntegerPropertyFactory.getImageIndexProperty",
                                "timer","MainState.timer.getMicroTimer","event_exec","MainState.executeEvent"),
                        "observed",map("option",option,"number",number,"float_number",floatNumber,"text",text,
                                "event_index",eventIndex,"timer",timer,"event",list(state.lastEventId,state.lastEventArg1,state.lastEventArg2)),
                        "unknownDirectLookup",unknown),
                0.000001,null,null);
    }

    private static void captureTimersEvents(int selectedTimers,int selectedEvents) throws Exception {
        MainState state=new MainState();
        String sentinelKey="timers-events/timer-off-sentinel";
        long sentinel=mutate(sentinelKey)?Long.MIN_VALUE+1:Long.MIN_VALUE;
        TimerProperty property=ignored->sentinel;
        TimerUtility timerUtility=new TimerUtility(state);
        LuaTable timerTable=new LuaTable(); timerUtility.export(timerTable);
        emitCase("timers-events","timer-off-sentinel",
                map("off",sentinel,"eventTimestampMicros",0),
                map("offValue",sentinel,"isOff",property.isOff(state),"isOn",property.isOn(state),
                        "eventAtZeroIsOn",timerTable.get("is_timer_on").call(LuaValue.ZERO).toboolean(),
                        "offElapsedMicros",timerTable.get("now_timer").call(LuaValue.valueOf(sentinel)).tolong()),
                0,null,null);

        String cacheKey="timers-events/custom-timer-once-per-frame";
        long first=mutate(cacheKey)?123457:123456, second=mutate(cacheKey)?234568:234567;
        final long[] next={first}; final int[] calls={0};
        CustomTimer cached=new CustomTimer(7,ignored->{calls[0]++;return next[0];});
        cached.update(state); long read1=cached.getMicroTimer(),read2=cached.getMicroTimer(); int frame1=calls[0];
        next[0]=second; cached.update(state); long read3=cached.getMicroTimer(); int frame2=calls[0]-frame1;
        emitCase("timers-events","custom-timer-once-per-frame",
                map("frame1Callback",first,"frame2Callback",second,"readsPerFrame",2),
                map("readsWithinFrame",list(read1,read2),"nextFrameRead",read3),0,
                list("frame1:update","frame1:read","frame1:read","frame2:update","frame2:read"),
                map("frame1TimerFunction",frame1,"frame2TimerFunction",frame2));

        String phaseKey="timers-events/timer-phase-before-event-phase";
        long updated=mutate(phaseKey)?7001:7000;
        Skin skin=new Skin(new SkinHeader()); List<String> order=new ArrayList<>(); final long[] observed={Long.MIN_VALUE};
        skin.addCustomTimer(new CustomTimer(17,ignored->{order.add("timer:17");return updated;}));
        skin.addCustomTimer(new CustomTimer(3,ignored->{order.add("timer:3");return 3000;}));
        skin.addCustomEvent(new CustomEvent(9,(s,a,b)->{order.add("event:9");observed[0]=skin.getMicroCustomTimer(17);},
                new BooleanProperty(){public boolean isStatic(MainState s){return false;} public boolean get(MainState s){return true;}},0));
        skin.addCustomEvent(new CustomEvent(4,(s,a,b)->order.add("event:4"),
                new BooleanProperty(){public boolean isStatic(MainState s){return false;} public boolean get(MainState s){return true;}},0));
        skin.updateCustomObjects(state);
        List<Object> phaseOrder=new ArrayList<>();phaseOrder.addAll(order);
        emitCase("timers-events","timer-phase-before-event-phase",
                map("updatedValue",updated,"configuredMaps",map("customTimers",2,"customEvents",2)),
                map("eventObservedTimerValue",observed[0],"phaseOrder",list("customTimers","customEvents")),0,
                phaseOrder,map("timerCallbacks",2,"eventConditions",2,"eventActions",2));

        String argsKey="timers-events/zero-one-two-argument-events";
        int arg1=mutate(argsKey)?12:11,arg2=mutate(argsKey)?23:22;
        List<Object> observedArgs=new ArrayList<>(); SkinLuaAccessor accessor=new SkinLuaAccessor(false);
        LuaFunction zero=new ZeroArgFunction(){public LuaValue call(){observedArgs.add(list());return LuaValue.NIL;}};
        LuaFunction one=new OneArgFunction(){public LuaValue call(LuaValue a){observedArgs.add(list(a.toint()));return LuaValue.NIL;}};
        LuaFunction two=new TwoArgFunction(){public LuaValue call(LuaValue a,LuaValue b){observedArgs.add(list(a.toint(),b.toint()));return LuaValue.NIL;}};
        LuaFunction three=new ThreeArgFunction(){public LuaValue call(LuaValue a,LuaValue b,LuaValue c){return LuaValue.NIL;}};
        Event e0=accessor.loadEvent(zero),e1=accessor.loadEvent(one),e2=accessor.loadEvent(two),e3=accessor.loadEvent(three);
        e0.exec(state,arg1,arg2);e1.exec(state,arg1,arg2);e2.exec(state,arg1,arg2);
        emitCase("timers-events","zero-one-two-argument-events",
                map("zero",list(),"one",list(arg1),"two",list(arg1,arg2),"three",list(1,2,3)),
                map("observedArguments",observedArgs,"threeArgumentFunction",e3==null?"unsupported-null":"supported"),0,
                list("zeroArg","oneArg","twoArg"),map("zeroArg",1,"oneArg",1,"twoArg",1));

        captureIntMap(selectedTimers,selectedEvents);
    }

    private static void captureIntMap(int selectedTimers,int selectedEvents) throws Exception {
        String key="timers-events/libgdx-intmap-backing-order";
        long seed=mutate(key)?1592594997L:1592594996L;
        RecordingJavaRandom random=new RecordingJavaRandom(seed); MathUtils.random=random;
        IntMap<String> values=new IntMap<>(4,0.8f); List<Object> capacities=new ArrayList<>();
        capacities.add(intMapCapacity(values));
        List<Object> operations=new ArrayList<>();
        int[] keys={1,217,545,761,217,977,1305,1521,1737,1953,2281,2497,2713};
        String[] labels={"one","two-one-seven","five-four-five","seven-six-one","TWO-ONE-SEVEN","nine-seven-seven","thirteen-oh-five","fifteen-twenty-one","seventeen-thirty-seven","nineteen-fifty-three","twenty-two-eighty-one","twenty-four-ninety-seven","twenty-seven-thirteen"};
        String before=random.stateDigest();
        for(int index=0;index<keys.length;index++){
            operations.add(map("operation","put","key",keys[index],"value",labels[index]));
            values.put(keys[index],labels[index]);capacities.add(intMapCapacity(values));
        }
        String after=random.stateDigest(); List<Object> iteration=new ArrayList<>();
        for(IntMap.Entry<String> entry:values)iteration.add("key:"+entry.key);
        emitCase("timers-events","libgdx-intmap-backing-order",
                map("traceSetup",map("requestedCapacity",4,"initialBackingCapacity",capacities.get(0),"loadFactor",0.8,
                                "randomImplementation","java.util.Random-compatible-lcg48","randomSeed",seed,
                                "randomStateBeforeSha256",before,"randomStateAfterSha256",after,
                                "randomDraws",random.draws,"capacityAfterEachOperation",capacities,"operations",operations,
                                "collisionAndResizeExercised",!random.draws.isEmpty()&&capacities.contains(16))),
                map("iterationKeys",iteration,"replacementValue",values.get(217),"size",values.size,
                        "selectedConfiguredMaps",map("customTimers",selectedTimers,"customEvents",selectedEvents),
                        "scope","trace-specific-not-universal-order"),0,iteration,
                map("uniqueInsertions",12,"replacements",1,"rngDraws",random.draws.size()));
    }

    private static void captureLegacy(Path work) throws Exception {
        String fileKey="legacy-lua-upstream/standard-file-call-shapes";
        String firstValue=mutate(fileKey)?"beta":"alpha";int secondValue=mutate(fileKey)?8:7;
        Path returns=work.resolve("returns.lua"),input=work.resolve("input.txt"),output=work.resolve("output.txt");
        Files.writeString(returns,"return '"+firstValue+"', "+secondValue+"\n");
        Files.writeString(input,"first-line\nsecond-line\n");
        SkinLuaAccessor accessor=new SkinLuaAccessor(false);accessor.setDirectory(work);
        String setup="trace_returns="+quoteLua(returns.toString())+";trace_input="+quoteLua(input.toString())+";trace_output="+quoteLua(output.toString())+";";
        LuaTable files=accessor.exec(setup+
                "local a,b=dofile(trace_returns);local d=io.open(trace_input);local first=d:lines()();local dc=d:close();"+
                "local w=io.open(trace_output,'w');local zero=(w:write()==w);local chain=(w:write(a,':',b)==w);local wc=w:close();"+
                "local r=io.open(trace_input,'r');local explicit=r:lines()();local rc=r:close();"+
                "local ap=io.open(trace_output,'a');local apsame=(ap:write(':tail')==ap);local ac=ap:close();"+
                "return {a=a,b=b,first=first,dc=dc,zero=zero,chain=chain,wc=wc,explicit=explicit,rc=rc,apsame=apsame,ac=ac}").checktable();
        emitCase("legacy-lua-upstream","standard-file-call-shapes",
                map("dofileArguments",list("relative-text-chunk"),"ioOpenModes",list("default","r","w","a"),
                        "returnFixture",list(firstValue,secondValue)),
                map("dofileReturn",list(files.get("a").tojstring(),files.get("b").toint()),
                        "ioOpenModes",list("default","r","w","a"),"handleMethods",list("lines","write","close"),
                        "firstLine",files.get("first").tojstring(),"explicitReadFirstLine",files.get("explicit").tojstring(),
                        "zeroArgumentWriteReturnsSameHandle",files.get("zero").toboolean(),
                        "chainedWriteReturnsSameHandle",files.get("chain").toboolean(),
                        "appendReturnsSameHandle",files.get("apsame").toboolean(),"outputContent",Files.readString(output),
                        "closeReturnsTrue",files.get("dc").toboolean()&&files.get("wc").toboolean()&&files.get("rc").toboolean()&&files.get("ac").toboolean()),0,
                list("dofile","io.open:default","lines:default","close:default","io.open:w","write:zero","write:multiple","close:w","io.open:r","lines:r","close:r","io.open:a","write:a","close:a"),
                map("dofile",1,"ioOpen",4,"lines",2,"write",3,"close",4));

        String legacyKey="legacy-lua-upstream/legacy-luajava-selected-facts";
        Path directory=work.resolve("legacy-list");Files.createDirectory(directory);Files.writeString(directory.resolve("one.txt"),"1");
        if(mutate(legacyKey))Files.writeString(directory.resolve("two.txt"),"2");
        LuaTable legacy=accessor.exec(
                "local first=require('luajava');local second=require('luajava');local File=first.bindClass('java.io.File');"+
                "local object=first.new(File,'legacy-list');local listed=object:listFiles();local Gdx=first.bindClass('com.badlogic.gdx.Gdx');"+
                "return {same=first==second,installed=first==package.loaded.luajava,list=listed,app=Gdx.app,graphics=Gdx.graphics,input=Gdx.input}").checktable();
        LuaTable listed=legacy.get("list").checktable();List<Object> normalized=new ArrayList<>();
        for(int index=1;index<=listed.length();index++)normalized.add(Path.of(listed.get(index).tojstring()).getFileName().toString());
        normalized.sort((a,b)->a.toString().compareTo(b.toString()));
        emitCase("legacy-lua-upstream","legacy-luajava-selected-facts",
                map("requires",list("luajava","luajava"),"classRequests",list("java.io.File","com.badlogic.gdx.Gdx"),
                        "fileArguments",list("File-token","relative-path"),"syntheticListEntryCount",normalized.size()),
                map("repeatedRequireSameTable",legacy.get("same").toboolean(),"requireUsesInstalledTable",legacy.get("installed").toboolean(),
                        "bindClasses",list("java.io.File","com.badlogic.gdx.Gdx"),"fileConstructorArguments",list("File-token","relative-path"),
                        "listShape","normalized-relative-path-array-or-nil","normalizedList",normalized,
                        "gdxAppPresent",!legacy.get("app").isnil(),"gdxFacadeMembers",list("graphics","input"),
                        "upstreamBroaderBranches",list("newInstance","URL-and-HTTP","controllers-and-input","debug-getmetatable")),0,
                list("install","require:first","require:second","bindClass:File","new:File","listFiles","bindClass:Gdx","read:Gdx.app"),
                map("require",2,"bindClass",2,"new",1,"listFiles",1,"gdxAppRead",1));
    }

    private static String quoteLua(String value) { return "'"+value.replace("\\","\\\\").replace("'","\\'")+"'"; }

    public static void main(String[] args) throws Exception {
        Path entry=Path.of(args[0]).toAbsolutePath().normalize();
        Path work=Path.of(args[1]).toAbsolutePath().normalize();
        int selectedTimers=Integer.parseInt(args[2]),selectedEvents=Integer.parseInt(args[3]);
        mutation="-".equals(args[4])?"":args[4];
        Files.createDirectories(work);
        captureTwoPhase(entry);
        captureLanguage();
        captureDestination();
        captureProperties();
        captureTimersEvents(selectedTimers,selectedEvents);
        captureLegacy(work);
    }
}
'''


def canonical_json_sha256(value: object) -> str:
    encoded = json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(encoded).hexdigest()


def load_reference_manifest(path: Path) -> dict:
    try:
        manifest = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"invalid ReferenceManifestV1: {error}") from error
    if manifest.get("schemaVersion") != 1:
        raise ValueError("manifest schemaVersion must be 1")
    if manifest.get("beatorajaCommit") != PINNED_COMMIT:
        raise ValueError("manifest Beatoraja commit does not match the pinned reference")
    selected = manifest.get("selectedCustomObjectMaps")
    surface = (
        manifest.get("selectedFileIoSurface", {})
        .get("configuredModelEvidence", {})
        .get("customObjectMaps")
    )
    if not isinstance(selected, dict) or selected != surface:
        raise ValueError("manifest selected custom object maps are not relationally equal")
    if set(selected) != {"customTimers", "customEvents"}:
        raise ValueError("manifest selected custom object maps have an invalid shape")
    if any(not isinstance(value, int) or value < 0 for value in selected.values()):
        raise ValueError("manifest selected custom object map counts are invalid")
    return manifest


def run_bounded(
    *arguments: str,
    cwd: Path | None = None,
    timeout_seconds: float,
    max_output_bytes: int,
) -> subprocess.CompletedProcess[str]:
    if timeout_seconds <= 0 or max_output_bytes <= 0:
        raise ValueError("subprocess bounds must be positive")
    with tempfile.TemporaryDirectory(prefix="asobmashow-bounded-process-") as temporary:
        output_path = Path(temporary) / "combined-output.bin"
        with output_path.open("wb") as output:
            process = subprocess.Popen(
                list(arguments),
                cwd=cwd,
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=subprocess.STDOUT,
            )
            deadline = time.monotonic() + timeout_seconds
            while process.poll() is None:
                output.flush()
                size = output_path.stat().st_size
                if size > max_output_bytes:
                    process.kill()
                    process.wait()
                    raise RuntimeError(
                        f"subprocess output limit exceeded "
                        f"({size} > {max_output_bytes} bytes): {arguments[0]}"
                    )
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    process.kill()
                    process.wait()
                    raise RuntimeError(
                        f"subprocess timed out after {timeout_seconds} seconds: {arguments[0]}"
                    )
                time.sleep(min(0.01, remaining))
            returncode = process.returncode
        size = output_path.stat().st_size
        with output_path.open("rb") as output:
            raw = output.read(max_output_bytes + 1)
        if size > max_output_bytes or len(raw) > max_output_bytes:
            raise RuntimeError(
                f"subprocess output limit exceeded ({size} > {max_output_bytes} bytes): {arguments[0]}"
            )
        text = raw.decode("utf-8", errors="replace")
        return subprocess.CompletedProcess(list(arguments), returncode, text, "")


def validate_reference(root: Path) -> None:
    if not root.is_dir():
        raise RuntimeError(f"Beatoraja root is not a directory: {root}")
    head = run_bounded(
        "git", "-C", str(root), "rev-parse", "HEAD",
        timeout_seconds=10,
        max_output_bytes=4096,
    )
    if head.returncode != 0 or head.stdout.strip() != PINNED_COMMIT:
        raise RuntimeError(
            f"Beatoraja reference must be {PINNED_COMMIT}; observed {head.stdout.strip() or 'unavailable'}"
        )
    status = run_bounded(
        "git", "-C", str(root), "status", "--porcelain",
        timeout_seconds=10,
        max_output_bytes=65536,
    )
    if status.returncode != 0 or status.stdout.strip():
        raise RuntimeError("Beatoraja reference must be clean")
    for relative in PINNED_SOURCE_UNITS:
        if not (root / relative).is_file():
            raise RuntimeError(f"missing pinned source unit: {relative}")
    for relative, markers in REFERENCE_MARKERS.items():
        text = (root / relative).read_text(encoding="utf-8")
        for marker in markers:
            if marker not in text:
                raise RuntimeError(f"missing pinned source marker {relative}: {marker}")
    for jar in ("gdx.jar", "luaj-jse-3.0.2-custom.jar", "gdx-controllers.jar"):
        if not (root / "lib" / jar).is_file():
            raise RuntimeError(f"missing pinned runtime library: lib/{jar}")


def _reject_duplicate_json_fields(pairs: list[tuple[str, object]]) -> dict:
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError(f"duplicate JSON field: {key}")
        value[key] = item
    return value


def parse_driver_output(raw: bytes, expected_keys: set[str]) -> dict[str, dict]:
    try:
        text = raw.decode("ascii")
    except UnicodeDecodeError as error:
        raise ValueError("driver output is not strict ASCII") from error
    payloads: dict[str, dict] = {}
    for line_number, line in enumerate(text.splitlines(), 1):
        fields = line.split("\t")
        if len(fields) != 3 or fields[0] != "FACT":
            raise ValueError(f"unexpected driver output at line {line_number}")
        key, encoded = fields[1], fields[2]
        if key in payloads:
            raise ValueError(f"duplicate driver fact: {key}")
        if key not in expected_keys:
            raise ValueError(f"unexpected driver fact: {key}")
        try:
            decoded = base64.b64decode(encoded, validate=True)
            payload = json.loads(
                decoded.decode("utf-8"),
                object_pairs_hook=_reject_duplicate_json_fields,
            )
        except (ValueError, UnicodeDecodeError, json.JSONDecodeError) as error:
            raise ValueError(f"invalid driver fact payload {key}: {error}") from error
        required = {"input", "expected", "precision"}
        optional = {"name", "callOrder", "callCount"}
        if not isinstance(payload, dict) or not required.issubset(payload):
            raise ValueError(f"driver fact has an incomplete TraceCase: {key}")
        if set(payload) - required - optional:
            raise ValueError(f"driver fact has extra TraceCase fields: {key}")
        if "name" in payload and payload["name"] != key.split("/", 1)[1]:
            raise ValueError(f"driver fact name mismatch: {key}")
        if not isinstance(payload["input"], dict) or not isinstance(payload["expected"], dict):
            raise ValueError(f"driver fact input/expected must be objects: {key}")
        if isinstance(payload["precision"], bool) or not isinstance(payload["precision"], (int, float)):
            raise ValueError(f"driver fact precision must be numeric: {key}")
        if "callOrder" in payload and (
            not isinstance(payload["callOrder"], list)
            or not all(isinstance(item, str) for item in payload["callOrder"])
        ):
            raise ValueError(f"driver fact callOrder must be a string array: {key}")
        if "callCount" in payload and (
            not isinstance(payload["callCount"], dict)
            or not all(
                isinstance(item, int) and not isinstance(item, bool) and item >= 0
                for item in payload["callCount"].values()
            )
        ):
            raise ValueError(f"driver fact callCount must contain nonnegative integers: {key}")
        payloads[key] = payload
    missing = expected_keys - set(payloads)
    if missing:
        raise ValueError("missing driver facts: " + ", ".join(sorted(missing)))
    return payloads


def _write_harness_sources(root: Path) -> tuple[Path, list[Path]]:
    source_root = root / "harness-src"
    sources: list[Path] = []
    for relative, content in STUB_SOURCES.items():
        path = source_root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content.strip() + "\n", encoding="utf-8")
        sources.append(path)
    driver = source_root / "bms/player/beatoraja/skin/lua/TraceDriver.java"
    driver.parent.mkdir(parents=True, exist_ok=True)
    driver.write_text(JAVA_DRIVER.strip() + "\n", encoding="utf-8")
    sources.append(driver)
    return source_root, sources


def _compile_harness(reference_root: Path, temporary_root: Path) -> tuple[Path, str]:
    classes = temporary_root / "classes"
    classes.mkdir()
    _, harness_sources = _write_harness_sources(temporary_root)
    pinned_sources = [reference_root / relative for relative in PINNED_SOURCE_UNITS]
    library_wildcard = str(reference_root / "lib" / "*")
    compile_result = run_bounded(
        "javac",
        "-encoding", "UTF-8",
        "-cp", library_wildcard,
        "-sourcepath", "",
        "-d", str(classes),
        *(str(path) for path in [*pinned_sources, *harness_sources]),
        cwd=reference_root,
        timeout_seconds=60,
        max_output_bytes=1024 * 1024,
    )
    if compile_result.returncode != 0:
        raise RuntimeError(f"trace driver compilation failed:\n{compile_result.stdout}")
    return classes, os.pathsep.join((str(classes), library_wildcard))


def _selected_map_arguments(manifest: dict) -> tuple[str, str]:
    selected = manifest["selectedCustomObjectMaps"]
    return str(selected["customTimers"]), str(selected["customEvents"])


def _execute_driver(
    classpath: str,
    temporary_root: Path,
    manifest: dict,
    mutation_case: str | None,
) -> dict[str, dict]:
    variant = "baseline" if mutation_case is None else hashlib.sha256(
        mutation_case.encode("utf-8")
    ).hexdigest()[:16]
    work = temporary_root / "driver-work" / variant
    work.mkdir(parents=True)
    selected_timers, selected_events = _selected_map_arguments(manifest)
    result = run_bounded(
        "java",
        "-cp", classpath,
        "bms.player.beatoraja.skin.lua.TraceDriver",
        str(TWO_PHASE_ENTRY),
        str(work),
        selected_timers,
        selected_events,
        mutation_case or "-",
        cwd=ROOT,
        timeout_seconds=20,
        max_output_bytes=2 * 1024 * 1024,
    )
    if result.returncode != 0:
        raise RuntimeError(f"trace driver execution failed:\n{result.stdout}")
    return parse_driver_output(result.stdout.encode("utf-8"), set(EXPECTED_CASE_KEYS))


def run_java_driver(reference_root: Path, manifest: dict) -> dict[str, dict]:
    with tempfile.TemporaryDirectory(prefix="asobmashow-beatoraja-traces-") as temporary:
        temporary_root = Path(temporary)
        _, classpath = _compile_harness(reference_root, temporary_root)
        return _execute_driver(classpath, temporary_root, manifest, None)


def verify_live_case_coupling(reference_root: Path, manifest: dict) -> dict[str, list[str]]:
    reference_root = reference_root.expanduser().resolve()
    validate_reference(reference_root)
    with tempfile.TemporaryDirectory(prefix="asobmashow-beatoraja-coupling-") as temporary:
        temporary_root = Path(temporary)
        _, classpath = _compile_harness(reference_root, temporary_root)
        baseline = _execute_driver(classpath, temporary_root, manifest, None)
        changed_by_probe: dict[str, list[str]] = {}
        for case_key in EXPECTED_CASE_KEYS:
            mutated = _execute_driver(classpath, temporary_root, manifest, case_key)
            changed_by_probe[case_key] = [
                key for key in EXPECTED_CASE_KEYS if mutated[key] != baseline[key]
            ]
        return changed_by_probe


def _provenance(kind: str) -> list[dict[str, str]]:
    return [
        {
            "commit": PINNED_COMMIT,
            "path": path,
            "symbol": symbol,
            "behavior": behavior,
        }
        for path, symbol, behavior in PROVENANCE[kind]
    ]


def assemble_traces(case_payloads: dict[str, dict], manifest: dict) -> dict[str, dict]:
    if set(case_payloads) != set(EXPECTED_CASE_KEYS):
        missing = set(EXPECTED_CASE_KEYS) - set(case_payloads)
        extra = set(case_payloads) - set(EXPECTED_CASE_KEYS)
        raise ValueError(f"driver case set mismatch; missing={sorted(missing)} extra={sorted(extra)}")
    selected = manifest.get("selectedCustomObjectMaps")
    surface = (
        manifest.get("selectedFileIoSurface", {})
        .get("configuredModelEvidence", {})
        .get("customObjectMaps")
    )
    if selected != surface:
        raise ValueError("selected custom object maps are not relationally equal")
    intmap = case_payloads["timers-events/libgdx-intmap-backing-order"]
    if intmap.get("expected", {}).get("selectedConfiguredMaps") != selected:
        raise ValueError("driver selected custom object maps do not match ReferenceManifestV1")
    traces: dict[str, dict] = {}
    for kind in TRACE_FILENAMES:
        cases = [
            case_payloads[key]
            for key in EXPECTED_CASE_KEYS
            if key.startswith(kind + "/")
        ]
        traces[kind] = {
            "schemaVersion": 1,
            "kind": kind,
            "referenceCommit": PINNED_COMMIT,
            "provenance": _provenance(kind),
            "cases": cases,
        }
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
    parser.add_argument("--manifest", type=Path, default=MANIFEST_PATH)
    parser.add_argument("--verify", action="store_true")
    parser.add_argument("--verify-coupling", action="store_true")
    return parser.parse_args()


def main() -> int:
    arguments = parse_arguments()
    try:
        reference_root = arguments.beatoraja_root.expanduser().resolve()
        validate_reference(reference_root)
        manifest = load_reference_manifest(arguments.manifest)
        if arguments.verify_coupling:
            changed = verify_live_case_coupling(reference_root, manifest)
            failures = {key: values for key, values in changed.items() if values != [key]}
            if failures:
                raise RuntimeError(f"live case coupling verification failed: {failures}")
        cases = run_java_driver(reference_root, manifest)
        traces = assemble_traces(cases, manifest)
        write_or_verify(traces, arguments.output_dir, arguments.verify)
        action = "verified" if arguments.verify else "wrote"
        print(f"Beatoraja skin traces {action}: {PINNED_COMMIT}")
        return 0
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


STUB_SOURCES.update({
    "bms/player/beatoraja/MainController.java": r'''
package bms.player.beatoraja;
import bms.player.beatoraja.skin.SkinObject.SkinOffset;
public class MainController {
    public static boolean debug = false;
    private final TraceConfig config = new TraceConfig();
    private final TraceInputProcessor input = new TraceInputProcessor();
    public TraceConfig getConfig() { return config; }
    public TraceInputProcessor getInputProcessor() { return input; }
    public SkinOffset getOffset(int id) { return null; }
    public static final class TraceInputProcessor {
        public int getMouseX() { return 0; }
        public int getMouseY() { return 0; }
    }
    public static final class TraceConfig {
        private final TraceAudioConfig audio = new TraceAudioConfig();
        public int getPrepareFramePerSecond() { return 60; }
        public TraceAudioConfig getAudioConfig() { return audio; }
    }
    public static final class TraceAudioConfig {
        private float system, key, bg;
        public float getSystemvolume() { return system; }
        public float getKeyvolume() { return key; }
        public float getBgvolume() { return bg; }
        public void setSystemvolume(float v) { system = v; }
        public void setKeyvolume(float v) { key = v; }
        public void setBgvolume(float v) { bg = v; }
    }
}
''',
    "bms/player/beatoraja/MainState.java": r'''
package bms.player.beatoraja;
import java.util.HashMap;
import java.util.Map;
import bms.player.beatoraja.skin.SkinObject.SkinOffset;
public class MainState {
    public final MainController main = new MainController();
    public final TraceTimer timer = new TraceTimer();
    private final TraceScore score = new TraceScore();
    private final Map<Integer, SkinOffset> offsets = new HashMap<>();
    public int lastEventId = Integer.MIN_VALUE, lastEventArg1, lastEventArg2, eventCalls;
    public TraceScore getScoreDataProperty() { return score; }
    public int getJudgeCount(int judge, boolean fast) { return judge + (fast ? 100 : 200); }
    public SkinOffset getOffsetValue(int id) { return offsets.get(id); }
    public void setOffsetValue(int id, SkinOffset offset) { offsets.put(id, offset); }
    public void executeEvent(int id) { executeEvent(id, 0, 0); }
    public void executeEvent(int id, int arg) { executeEvent(id, arg, 0); }
    public void executeEvent(int id, int arg1, int arg2) {
        lastEventId = id; lastEventArg1 = arg1; lastEventArg2 = arg2; eventCalls++;
    }
    public static final class TraceScore {
        public double getNowRate() { return 0.1; }
        public int getNowEXScore() { return 2; }
        public double getNowBestScoreRate() { return 0.3; }
        public int getBestScore() { return 4; }
        public double getRivalScoreRate() { return 0.5; }
        public int getRivalScore() { return 6; }
    }
    public static final class TraceTimer {
        private final Map<Integer, Long> values = new HashMap<>();
        private long nowMicro;
        public long getMicroTimer(int id) { return values.getOrDefault(id, Long.MIN_VALUE); }
        public long getTimer(int id) { long v=getMicroTimer(id); return v == Long.MIN_VALUE ? v : v / 1000; }
        public long getNowTime(int id) { long v=getMicroTimer(id); return v == Long.MIN_VALUE ? 0 : getNowTime() - v / 1000; }
        public boolean isTimerOn(int id) { return getMicroTimer(id) != Long.MIN_VALUE; }
        public void setMicroTimer(int id, long value) { values.put(id, value); }
        public long getNowMicroTime() { return nowMicro; }
        public long getNowTime() { return nowMicro / 1000; }
        public void setNowMicroTime(long value) { nowMicro = value; }
    }
}
''',
    "bms/player/beatoraja/Config.java": r'''
package bms.player.beatoraja;
public class Config { }
''',
    "bms/player/beatoraja/SkinConfig.java": r'''
package bms.player.beatoraja;
public class SkinConfig {
    public static class Property {
        private FilePath[] files = new FilePath[0];
        private Offset[] offsets = new Offset[0];
        public FilePath[] getFile() { return files; }
        public Offset[] getOffset() { return offsets; }
    }
    public static class FilePath { public String name = "", path = ""; }
    public static class Offset { public String name = ""; public int x,y,w,h,r,a; }
}
''',
    "bms/player/beatoraja/ShaderManager.java": r'''
package bms.player.beatoraja;
import com.badlogic.gdx.graphics.glutils.ShaderProgram;
public final class ShaderManager { public static ShaderProgram getShader(String name) { return null; } }
''',
    "bms/player/beatoraja/skin/SkinType.java": r'''
package bms.player.beatoraja.skin;
public enum SkinType {
    PLAY_5KEYS, PLAY_7KEYS, PLAY_9KEYS, PLAY_10KEYS, PLAY_14KEYS, PLAY_24KEYS, PLAY_24KEYS_DOUBLE, OTHER
}
''',
    "bms/player/beatoraja/skin/SkinProperty.java": r'''
package bms.player.beatoraja.skin;
public final class SkinProperty { public static final int OFFSET_ALL=1, OFFSET_MAX=10000; }
''',
    "bms/player/beatoraja/skin/SkinPropertyMapper.java": r'''
package bms.player.beatoraja.skin;
public final class SkinPropertyMapper {
    public static boolean isTimerWritableBySkin(int id) { return id >= 0; }
    public static boolean isEventRunnableBySkin(int id) { return id >= 0; }
    public static boolean isCustomEventId(int id) { return false; }
}
''',
    "bms/player/beatoraja/skin/SkinHeader.java": r'''
package bms.player.beatoraja.skin;
import bms.player.beatoraja.Resolution;
import bms.player.beatoraja.SkinConfig;
public class SkinHeader {
    private Resolution source = new Resolution(1280,720), destination = new Resolution(1280,720);
    public Resolution getSourceResolution() { return source; }
    public Resolution getDestinationResolution() { return destination; }
    public void setSkinConfigProperty(SkinConfig.Property property) { }
    public CustomFile[] getCustomFiles() { return new CustomFile[0]; }
    public CustomOption[] getCustomOptions() { return new CustomOption[0]; }
    public CustomOffset[] getCustomOffsets() { return new CustomOffset[0]; }
    public static class CustomFile { public String path=""; public String getSelectedFilename(){return null;} }
    public static class CustomOption { public String name=""; public int getSelectedOption(){return 0;} }
    public static class CustomOffset { public String name=""; }
}
''',
    "bms/player/beatoraja/play/BMSPlayer.java": r'''
package bms.player.beatoraja.play;
import bms.player.beatoraja.MainState;
import bms.player.beatoraja.skin.SkinType;
public class BMSPlayer extends MainState {
    private final Gauge gauge = new Gauge();
    public Gauge getGauge() { return gauge; }
    public SkinType getSkinType() { return SkinType.PLAY_7KEYS; }
    public static final class Gauge { public float getValue(){return 0.75f;} public int getType(){return 2;} }
}
''',
    "bms/player/beatoraja/skin/SkinNumber.java": r'''
package bms.player.beatoraja.skin;
public class SkinNumber extends SkinObject { public void draw(Skin.SkinObjectRenderer r){} public void dispose(){} }
''',
    "bms/player/beatoraja/skin/SkinImage.java": r'''
package bms.player.beatoraja.skin;
import com.badlogic.gdx.graphics.g2d.TextureRegion;
public class SkinImage extends SkinObject { public SkinImage(TextureRegion r){} public void draw(Skin.SkinObjectRenderer r){} public void dispose(){} }
''',
    "bms/player/beatoraja/skin/SkinText.java": r'''
package bms.player.beatoraja.skin;
import com.badlogic.gdx.math.Rectangle;
public class SkinText extends SkinObject { public boolean isEditable(){return false;} public Rectangle getInputBounds(){return new Rectangle();} public void draw(Skin.SkinObjectRenderer r){} public void dispose(){} }
''',
    "bms/player/beatoraja/skin/SkinSlider.java": r'''
package bms.player.beatoraja.skin;
public class SkinSlider extends SkinObject { public void draw(Skin.SkinObjectRenderer r){} public void dispose(){} }
''',
    "bms/player/beatoraja/skin/SkinTextInput.java": r'''
package bms.player.beatoraja.skin;
import bms.player.beatoraja.MainState;
public class SkinTextInput { public void commitIfOutside(int x,int y){} public void focus(MainState s,SkinText t){} public void dispose(){} }
''',
    "bms/player/beatoraja/skin/property/BooleanPropertyFactory.java": r'''
package bms.player.beatoraja.skin.property;
public final class BooleanPropertyFactory {
    public static String lastOrigin;
    public static BooleanProperty getBooleanProperty(int id) {
        lastOrigin="boolean-id:"+id;
        if(id==Integer.MAX_VALUE) return null;
        return new BooleanProperty(){ public boolean isStatic(bms.player.beatoraja.MainState s){return true;} public boolean get(bms.player.beatoraja.MainState s){return id%2!=0;} };
    }
    public static BooleanProperty getBooleanProperty(String name) {
        lastOrigin="boolean-name:"+name;
        return "recognized-name".equals(name) ? new BooleanProperty(){ public boolean isStatic(bms.player.beatoraja.MainState s){return true;} public boolean get(bms.player.beatoraja.MainState s){return true;} } : null;
    }
}
''',
    "bms/player/beatoraja/skin/property/IntegerPropertyFactory.java": r'''
package bms.player.beatoraja.skin.property;
public final class IntegerPropertyFactory {
    public static String lastOrigin;
    public static IntegerProperty getIntegerProperty(int id){lastOrigin="integer-id:"+id; return id==Integer.MAX_VALUE?null:s->1000+id;}
    public static IntegerProperty getIntegerProperty(String name){lastOrigin="integer-name:"+name; return "recognized-name".equals(name)?s->2000:null;}
    public static IntegerProperty getImageIndexProperty(int id){lastOrigin="image-index-id:"+id; return id==Integer.MAX_VALUE?null:s->3000+id;}
}
''',
    "bms/player/beatoraja/skin/property/FloatPropertyFactory.java": r'''
package bms.player.beatoraja.skin.property;
public final class FloatPropertyFactory {
    public static String lastOrigin;
    public static FloatProperty getRateProperty(int id){lastOrigin="float-id:"+id; return id==Integer.MAX_VALUE?null:s->4000f+id;}
    public static FloatProperty getRateProperty(String name){lastOrigin="float-name:"+name; return "recognized-name".equals(name)?s->5000f:null;}
    public static FloatWriter getRateWriter(int id){return (s,v)->{};}
    public static FloatWriter getRateWriter(String name){return null;}
}
''',
    "bms/player/beatoraja/skin/property/StringPropertyFactory.java": r'''
package bms.player.beatoraja.skin.property;
public final class StringPropertyFactory {
    public static String lastOrigin;
    public static StringProperty getStringProperty(int id){lastOrigin="string-id:"+id; return id==Integer.MAX_VALUE?null:s->"string:"+id;}
    public static StringProperty getStringProperty(String name){lastOrigin="string-name:"+name; return "recognized-name".equals(name)?s->"recognized":null;}
    public static StringWriter getStringWriter(String name){return null;}
}
''',
    "bms/player/beatoraja/skin/property/EventFactory.java": r'''
package bms.player.beatoraja.skin.property;
import bms.player.beatoraja.MainState;
public final class EventFactory {
    public interface Zero { void exec(MainState state); }
    public interface One { void exec(MainState state,int arg1); }
    public interface Two { void exec(MainState state,int arg1,int arg2); }
    public static Event createZeroArgEvent(Zero f){return (s,a,b)->f.exec(s);}
    public static Event createOneArgEvent(One f){return (s,a,b)->f.exec(s,a);}
    public static Event createTwoArgEvent(Two f){return (s,a,b)->f.exec(s,a,b);}
    public static Event getEvent(int id){return (s,a,b)->s.executeEvent(id,a,b);}
    public static Event getEvent(String name){return "recognized-name".equals(name)?(s,a,b)->{}:null;}
}
''',
    "bms/player/beatoraja/skin/json/JsonSkin.java": r'''
package bms.player.beatoraja.skin.json;
import bms.player.beatoraja.skin.property.BooleanProperty;
public final class JsonSkin {
    public static class Skin { public int type=-1; public String name,author; }
    public static class DestinationOption { public int id; public BooleanProperty property; public DestinationOption(int i){id=i;} public DestinationOption(BooleanProperty p){property=p;} }
}
''',
    "bms/player/beatoraja/skin/json/JSONSkinLoader.java": r'''
package bms.player.beatoraja.skin.json;
import java.io.File;
import java.nio.file.Path;
import bms.player.beatoraja.*;
import bms.player.beatoraja.skin.*;
import bms.player.beatoraja.skin.lua.SkinLuaAccessor;
import com.badlogic.gdx.utils.ObjectMap;
public class JSONSkinLoader {
    protected final SkinLuaAccessor lua;
    protected JsonSkin.Skin sk;
    protected ObjectMap<String,String> filemap=new ObjectMap<>();
    public JSONSkinLoader(SkinLuaAccessor lua){this.lua=lua;}
    public JSONSkinLoader(MainState state,Config c,SkinLuaAccessor lua){this.lua=lua;}
    public SkinHeader loadHeader(Path p){return null;}
    public Skin loadSkin(Path p,SkinType type,SkinConfig.Property property){return null;}
    public Skin load(Path p,SkinType type,SkinConfig.Property property){return null;}
    protected SkinHeader loadJsonSkinHeader(JsonSkin.Skin skin,Path p){return new SkinHeader();}
    protected Skin loadJsonSkin(SkinHeader h,JsonSkin.Skin s,SkinType t,SkinConfig.Property p,Path path){return null;}
    protected File getPath(String path,ObjectMap<String,String> map){return new File(path);}
}
''',
    "bms/player/beatoraja/skin/lua/SkinFileLuaApiExporter.java": r'''
package bms.player.beatoraja.skin.lua;
import org.luaj.vm2.LuaTable;
class SkinFileLuaApiExporter implements LuaApiExporter { SkinFileLuaApiExporter(SkinLuaPathResolver r){} public void export(LuaTable t){} }
''',
    "bms/player/beatoraja/skin/lua/SkinHttpLuaApiExporter.java": r'''
package bms.player.beatoraja.skin.lua;
import org.luaj.vm2.LuaTable;
class SkinHttpLuaApiExporter implements LuaApiExporter { public void export(LuaTable t){} }
''',
    "bms/player/beatoraja/skin/lua/SkinAudioLuaApiExporter.java": r'''
package bms.player.beatoraja.skin.lua;
import org.luaj.vm2.LuaTable;
import bms.player.beatoraja.MainState;
class SkinAudioLuaApiExporter implements LuaApiExporter { SkinAudioLuaApiExporter(MainState s,SkinLuaPathResolver r){} public void export(LuaTable t){} }
''',
})


if __name__ == "__main__":
    raise SystemExit(main())
