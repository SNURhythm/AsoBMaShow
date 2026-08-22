package bms.player.beatoraja.skin.lr2;

import bms.model.BMSModel;
import bms.model.LongNote;
import bms.player.beatoraja.AudioConfig;
import bms.player.beatoraja.Config;
import bms.player.beatoraja.MainState;
import bms.player.beatoraja.PlayerResource;
import bms.player.beatoraja.Resolution;
import bms.player.beatoraja.play.PlaySkin;
import bms.player.beatoraja.result.SkinGaugeGraphObject;
import bms.player.beatoraja.skin.Skin;
import bms.player.beatoraja.skin.SkinBPMGraph;
import bms.player.beatoraja.skin.SkinHitErrorVisualizer;
import bms.player.beatoraja.skin.SkinNoteDistributionGraph;
import bms.player.beatoraja.skin.SkinObject;
import bms.player.beatoraja.skin.SkinObject.SkinObjectDestination;
import bms.player.beatoraja.skin.SkinTimingDistributionGraph;
import bms.player.beatoraja.skin.SkinTimingVisualizer;
import bms.player.beatoraja.skin.SkinType;
import bms.player.beatoraja.skin.json.JSONSkinLoader;
import bms.player.beatoraja.skin.json.JsonSkin;
import bms.player.beatoraja.skin.lua.LuaSkinLoader;
import bms.player.beatoraja.skin.property.FloatPropertyFactory;
import com.badlogic.gdx.graphics.Color;
import com.badlogic.gdx.graphics.Texture;
import com.badlogic.gdx.graphics.TextureData;
import com.badlogic.gdx.utils.JsonReader;
import com.badlogic.gdx.utils.JsonValue;
import java.lang.reflect.Field;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import sun.misc.Unsafe;

/** Executes value-owned gameplay-skin behavior from the pinned Beatoraja tree. */
public final class GameplaySkinOracle {
    private static final Unsafe UNSAFE = unsafe();

    private static final class NoGpuTexture extends Texture {
        private NoGpuTexture() { super((TextureData)null); }
        @Override public int getWidth() { return 64; }
        @Override public int getHeight() { return 64; }
    }

    private static final class OracleState extends MainState {
        private OracleState() { super(null, null); }
        @Override public void create() { }
        @Override public void render() { }
    }

    private static Unsafe unsafe() {
        try {
            Field field = Unsafe.class.getDeclaredField("theUnsafe");
            field.setAccessible(true);
            return (Unsafe)field.get(null);
        } catch (ReflectiveOperationException error) {
            throw new ExceptionInInitializerError(error);
        }
    }

    private static Object field(Object target, String name) throws Exception {
        Class<?> type = target.getClass();
        while (type != null) {
            try {
                Field field = type.getDeclaredField(name);
                field.setAccessible(true);
                return field.get(target);
            } catch (NoSuchFieldException ignored) {
                type = type.getSuperclass();
            }
        }
        throw new NoSuchFieldException(target.getClass().getName() + "." + name);
    }

    private static void setField(Object target, Class<?> owner, String name, Object value)
            throws Exception {
        Field field = owner.getDeclaredField(name);
        long offset = UNSAFE.objectFieldOffset(field);
        UNSAFE.putObject(target, offset, value);
    }

    private static int rgba(Color color) {
        return Color.rgba8888(color);
    }

    private static List<Object> colors(Color[] colors) {
        List<Object> result = new ArrayList<>();
        for (Color color : colors) result.add(Integer.toUnsignedLong(rgba(color)));
        return result;
    }

    private static Map<String,Object> map(Object... pairs) {
        Map<String,Object> result = new LinkedHashMap<>();
        for (int index = 0; index < pairs.length; index += 2) {
            result.put((String)pairs[index], pairs[index + 1]);
        }
        return result;
    }

    private static String quote(String value) {
        StringBuilder out = new StringBuilder("\"");
        for (int index = 0; index < value.length(); ++index) {
            char character = value.charAt(index);
            switch (character) {
                case '\\' -> out.append("\\\\");
                case '"' -> out.append("\\\"");
                case '\n' -> out.append("\\n");
                case '\r' -> out.append("\\r");
                case '\t' -> out.append("\\t");
                default -> {
                    if (character < 0x20) out.append(String.format("\\u%04x", (int)character));
                    else out.append(character);
                }
            }
        }
        return out.append('"').toString();
    }

    private static String json(Object value) {
        if (value == null) return "null";
        if (value instanceof String text) return quote(text);
        if (value instanceof Boolean || value instanceof Number) return value.toString();
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
        throw new IllegalArgumentException("unsupported JSON value " + value.getClass());
    }

    private static JsonValue first(JsonValue root, String array) {
        return root.get(array).child;
    }

    private static String string(JsonValue value, String field) {
        return value.getString(field);
    }

    private static Map<String,Object> destination(SkinObject object) {
        SkinObjectDestination value = object.getAllDestination()[0];
        return map("timeMillis", value.time,
                   "x", value.region.x, "y", value.region.y,
                   "width", value.region.width, "height", value.region.height,
                   "acceleration", value.acc,
                   "rgba", Integer.toUnsignedLong(rgba(value.color)),
                   "angleDegrees", value.angle);
    }

    private static Map<String,Object> noteDistribution(SkinNoteDistributionGraph graph)
            throws Exception {
        return map("type", field(graph, "type"),
                   "delayMillis", field(graph, "delay"),
                   "backgroundTextureOff", field(graph, "isBackTexOff"),
                   "reverseOrder", field(graph, "isOrderReverse"),
                   "noGap", field(graph, "isNoGap"),
                   "noHorizontalGap", field(graph, "isNoGapX"));
    }

    private static Map<String,Object> bpm(SkinBPMGraph graph) throws Exception {
        return map("delayMillis", field(graph, "delay"),
                   "lineWidth", field(graph, "lineWidth"),
                   "colors", List.of(
                       Integer.toUnsignedLong(rgba((Color)field(graph, "mainLineColor"))),
                       Integer.toUnsignedLong(rgba((Color)field(graph, "minLineColor"))),
                       Integer.toUnsignedLong(rgba((Color)field(graph, "maxLineColor"))),
                       Integer.toUnsignedLong(rgba((Color)field(graph, "otherLineColor"))),
                       Integer.toUnsignedLong(rgba((Color)field(graph, "stopLineColor"))),
                       Integer.toUnsignedLong(rgba((Color)field(graph, "transitionLineColor")))));
    }

    private static Map<String,Object> timing(SkinTimingVisualizer graph) throws Exception {
        Color[] judge = (Color[])field(graph, "JColor");
        return map("centerMillis", field(graph, "center"),
                   "judgeWidthRate", field(graph, "judgeWidthRate"),
                   "lineWidth", field(graph, "lineWidth"),
                   "lineRgba", Integer.toUnsignedLong(rgba((Color)field(graph, "lineColor"))),
                   "centerRgba", Integer.toUnsignedLong(rgba((Color)field(graph, "centerColor"))),
                   "judgeRgba", colors(judge),
                   "transparent", rgba(judge[4]) == 0,
                   "drawDecay", field(graph, "drawDecay"));
    }

    private static Map<String,Object> hitError(SkinHitErrorVisualizer graph,
                                                long[] recent) throws Exception {
        Color[] judge = (Color[])field(graph, "JColor");
        var update = SkinHitErrorVisualizer.class.getDeclaredMethod("updateEMA", long.class);
        update.setAccessible(true);
        List<Object> ema = new ArrayList<>();
        for (long value : recent) {
            update.invoke(graph, value);
            ema.add(field(graph, "ema"));
        }
        return map("width", field(graph, "width"),
                   "centerMillis", field(graph, "center"),
                   "judgeWidthRate", field(graph, "judgeWidthRate"),
                   "lineWidth", field(graph, "lineWidth"),
                   "colorMode", field(graph, "colorMode"),
                   "hitErrorMode", field(graph, "hiterrorMode"),
                   "emaMode", field(graph, "emaMode"),
                   "lineRgba", Integer.toUnsignedLong(rgba((Color)field(graph, "lineColor"))),
                   "centerRgba", Integer.toUnsignedLong(rgba((Color)field(graph, "centerColor"))),
                   "emaRgba", Integer.toUnsignedLong(rgba((Color)field(graph, "emaColor"))),
                   "judgeRgba", colors(judge),
                   "alpha", field(graph, "alpha"),
                   "windowLength", field(graph, "windowLength"),
                   "transparent", rgba(judge[4]) == 0,
                   "drawDecay", field(graph, "drawDecay"),
                   "emaSequence", ema);
    }

    private static Map<String,Object> timingDistribution(SkinTimingDistributionGraph graph)
            throws Exception {
        return map("columns", field(graph, "gx"),
                   "centerColumn", field(graph, "c"),
                   "graphRgba", Integer.toUnsignedLong(rgba((Color)field(graph, "graphColor"))),
                   "averageRgba", Integer.toUnsignedLong(rgba((Color)field(graph, "averageColor"))),
                   "devRgba", Integer.toUnsignedLong(rgba((Color)field(graph, "devColor"))),
                   "judgeRgba", colors((Color[])field(graph, "JColor")),
                   "drawAverage", field(graph, "drawAverage"),
                   "drawDev", field(graph, "drawDev"),
                   "gameplayDraw", false);
    }

    private static Map<String,Object> gauge(SkinGaugeGraphObject graph) throws Exception {
        Color[] borderline = (Color[])field(graph, "borderline");
        Color[] bordercolor = (Color[])field(graph, "bordercolor");
        Color[] graphline = (Color[])field(graph, "graphline");
        Color[] graphcolor = (Color[])field(graph, "graphcolor");
        List<Object> rows = new ArrayList<>();
        for (int index = 0; index < 6; ++index) {
            rows.add(List.of(Integer.toUnsignedLong(rgba(borderline[index])),
                             Integer.toUnsignedLong(rgba(bordercolor[index])),
                             Integer.toUnsignedLong(rgba(graphline[index])),
                             Integer.toUnsignedLong(rgba(graphcolor[index]))));
        }
        return map("rgba", rows);
    }

    private static LR2PlaySkinLoader loader(Resolution resolution) {
        Config config = new Config();
        config.setResolution(resolution);
        LR2PlaySkinLoader loader =
            new LR2PlaySkinLoader(SkinType.PLAY_7KEYS, resolution, config);
        SkinHeaderFactory.attach(loader, resolution);
        return loader;
    }

    private static final class SkinHeaderFactory {
        static void attach(LR2PlaySkinLoader loader, Resolution resolution) {
            bms.player.beatoraja.skin.SkinHeader header =
                new bms.player.beatoraja.skin.SkinHeader();
            header.setResolution(resolution);
            header.setSkinType(SkinType.PLAY_7KEYS);
            header.setSourceResolution(resolution);
            header.setDestinationResolution(resolution);
            loader.skin = new PlaySkin(header);
        }
    }

    private static List<String> lines(Path path, String... prefixes) throws Exception {
        List<String> result = new ArrayList<>();
        for (String line : Files.readAllLines(path, StandardCharsets.UTF_8)) {
            for (String prefix : prefixes) {
                if (line.startsWith(prefix)) {
                    result.add(line);
                    break;
                }
            }
        }
        return result;
    }

    private static Map<String,Object> lr2Graphs(Path path) throws Exception {
        LR2PlaySkinLoader loader = loader(Resolution.SD);
        for (String line : lines(path,
                "#SRC_NOTECHART_1P,", "#SRC_BPMCHART,",
                "#DST_NOTECHART_1P,", "#DST_BPMCHART,",
                "#SRC_TIMING_1P,", "#DST_TIMING_1P,")) {
            loader.processLine(line, null);
        }
        SkinObject[] objects = loader.skin.getAllSkinObjects();
        return map(
            "lr2.note-chart", map("object", noteDistribution((SkinNoteDistributionGraph)objects[0]),
                                  "destination", destination(objects[0])),
            "lr2.bpm-chart", map("object", bpm((SkinBPMGraph)objects[1]),
                                 "destination", destination(objects[1])),
            "lr2.timing", map("object", timing((SkinTimingVisualizer)objects[2]),
                              "destination", destination(objects[2])));
    }

    private static Map<String,Object> lr2Slider(Path path) throws Exception {
        LR2PlaySkinLoader loader = loader(Resolution.HD);
        loader.imagelist.add(UNSAFE.allocateInstance(NoGpuTexture.class));
        for (String line : lines(path, "#SRC_SLIDER,", "#DST_SLIDER,")) {
            loader.processLine(line, null);
        }
        SkinObject object = loader.skin.getAllSkinObjects()[0];
        return map("direction", field(object, "direction"),
                   "range", field(object, "range"),
                   "changeable", field(object, "writer") != null,
                   "destination", destination(object));
    }

    private static Map<String,Object> selector(float masterVolume) throws Exception {
        Config config = new Config();
        AudioConfig audio = new AudioConfig();
        audio.setSystemvolume(masterVolume);
        config.setAudioConfig(audio);
        PlayerResource resource = (PlayerResource)UNSAFE.allocateInstance(PlayerResource.class);
        setField(resource, PlayerResource.class, "config", config);
        OracleState state = (OracleState)UNSAFE.allocateInstance(OracleState.class);
        setField(state, MainState.class, "resource", resource);
        return map("numeric", FloatPropertyFactory.getRateProperty(17).get(state),
                   "named", FloatPropertyFactory.getRateProperty("mastervolume").get(state));
    }

    private static JsonSkin.Skin jsonFixture(Path path) throws Exception {
        JSONSkinLoader loader = new JSONSkinLoader();
        if (loader.loadHeader(path) == null) {
            throw new IllegalStateException("JSONSkinLoader rejected fixture");
        }
        return (JsonSkin.Skin)field(loader, "sk");
    }

    private static JsonSkin.Skin luaFixture(Path path) throws Exception {
        LuaSkinLoader loader = LuaSkinLoader.sandboxed(path);
        if (loader.loadHeader(path) == null) {
            throw new IllegalStateException("LuaSkinLoader rejected fixture");
        }
        return (JsonSkin.Skin)field(loader, "sk");
    }

    private static int authored(int value, int fallback) {
        return value == Integer.MIN_VALUE ? fallback : value;
    }

    private static Map<String,Object> authoredDestination(JsonSkin.Skin skin,
                                                           String id) {
        for (JsonSkin.Destination destination : skin.destination) {
            if (!id.equals(destination.id) || destination.dst.length == 0) continue;
            JsonSkin.Animation frame = destination.dst[0];
            int a = authored(frame.a, 255);
            int r = authored(frame.r, 255);
            int g = authored(frame.g, 255);
            int b = authored(frame.b, 255);
            long rgba = Integer.toUnsignedLong(
                (r & 255) << 24 | (g & 255) << 16 | (b & 255) << 8 | (a & 255));
            return map("timeMillis", authored(frame.time, 0),
                       "x", (float)authored(frame.x, 0),
                       "y", (float)authored(frame.y, 0),
                       "width", (float)authored(frame.w, 0),
                       "height", (float)authored(frame.h, 0),
                       "acceleration", authored(frame.acc, 0),
                       "rgba", rgba,
                       "angleDegrees", authored(frame.angle, 0));
        }
        throw new IllegalArgumentException("missing destination " + id);
    }

    private static void structuredCases(Map<String,Object> cases, String prefix,
                                        JsonSkin.Skin skin, long[] recent)
            throws Exception {
        JsonSkin.JudgeGraph note = skin.judgegraph[0];
        cases.put(prefix + ".note-distribution", noteDistribution(
            new SkinNoteDistributionGraph(note.type, note.delay, note.backTexOff,
                                          note.orderReverse, note.noGap, note.noGapX)));
        JsonSkin.BPMGraph bpmValue = skin.bpmgraph[0];
        cases.put(prefix + ".bpm-graph", bpm(new SkinBPMGraph(
            bpmValue.delay, bpmValue.lineWidth, bpmValue.mainBPMColor,
            bpmValue.minBPMColor, bpmValue.maxBPMColor, bpmValue.otherBPMColor,
            bpmValue.stopLineColor, bpmValue.transitionLineColor)));
        JsonSkin.GaugeGraph gaugeValue = skin.gaugegraph[0];
        Color[][] gaugeColors = new Color[6][4];
        for (int row = 0; row < 6; ++row) {
            for (int column = 0; column < 4; ++column) {
                gaugeColors[row][column] =
                    Color.valueOf(gaugeValue.color[row * 4 + column]);
            }
        }
        cases.put(prefix + ".gauge-graph", gauge(new SkinGaugeGraphObject(gaugeColors)));
        JsonSkin.TimingVisualizer timingValue = skin.timingvisualizer[0];
        cases.put(prefix + ".timing-visualizer", timing(new SkinTimingVisualizer(
            timingValue.width, timingValue.judgeWidthMillis, timingValue.lineWidth,
            timingValue.lineColor, timingValue.centerColor, timingValue.PGColor,
            timingValue.GRColor, timingValue.GDColor, timingValue.BDColor,
            timingValue.PRColor, timingValue.transparent, timingValue.drawDecay)));
        JsonSkin.HitErrorVisualizer hit = skin.hiterrorvisualizer[0];
        cases.put(prefix + ".hit-error-visualizer", hitError(
            new SkinHitErrorVisualizer(hit.width, hit.judgeWidthMillis,
                hit.lineWidth, hit.colorMode, hit.hiterrorMode, hit.emaMode,
                hit.lineColor, hit.centerColor, hit.PGColor, hit.GRColor,
                hit.GDColor, hit.BDColor, hit.PRColor, hit.emaColor, hit.alpha,
                hit.windowLength, hit.transparent, hit.drawDecay), recent));
        JsonSkin.TimingDistributionGraph distribution = skin.timingdistributiongraph[0];
        cases.put(prefix + ".timing-distribution", timingDistribution(
            new SkinTimingDistributionGraph(distribution.width, distribution.lineWidth,
                distribution.graphColor, distribution.averageColor, distribution.devColor,
                distribution.PGColor, distribution.GRColor, distribution.GDColor,
                distribution.BDColor, distribution.PRColor,
                distribution.drawAverage, distribution.drawDev)));
        JsonSkin.Slider slider = skin.slider[0];
        cases.put(prefix + ".slider", map(
            "direction", slider.angle, "range", slider.range,
            "changeable", slider.changeable,
            "destination", authoredDestination(skin, slider.id)));
    }

    private static long[] longs(String encoded) {
        String[] fields = encoded.split(",");
        long[] result = new long[fields.length];
        for (int index = 0; index < fields.length; ++index) {
            result[index] = Long.parseLong(fields[index]);
        }
        return result;
    }

    public static void main(String[] arguments) throws Exception {
        if (arguments.length != 9) {
            throw new IllegalArgumentException("expected Lua, JSON, two LR2 fixtures, viewport, times, and runtime state");
        }
        Path luaPath = Path.of(arguments[0]);
        Path jsonPath = Path.of(arguments[1]);
        JsonSkin.Skin lua = luaFixture(luaPath);
        JsonSkin.Skin json = jsonFixture(jsonPath);
        int viewportWidth = Integer.parseInt(arguments[4]);
        int viewportHeight = Integer.parseInt(arguments[5]);
        long[] times = longs(arguments[6]);
        float masterVolume = Float.parseFloat(arguments[7]);
        long[] recent = longs(arguments[8]);
        Map<String,Object> cases = new LinkedHashMap<>();
        cases.put("selector.master-volume", selector(masterVolume));
        structuredCases(cases, "lua", lua, recent);
        structuredCases(cases, "json", json, recent);

        Path crossLr2 = Path.of(arguments[2]);
        Path allLr2 = Path.of(arguments[3]);
        cases.put("lr2.slider", lr2Slider(crossLr2));
        cases.putAll(lr2Graphs(allLr2));
        List<Object> sampledFrames = new ArrayList<>();
        for (long time : times) {
            sampledFrames.add(map("visualTimeMillis", time,
                                  "normalized", times[times.length - 1] == 0
                                      ? 0.0 : time / (double)times[times.length - 1],
                                  "viewportArea", viewportWidth * viewportHeight));
        }
        Map<String,Object> execution = map(
            "viewport", map("width", viewportWidth, "height", viewportHeight),
            "visualTimesMillis", java.util.Arrays.stream(times).boxed().toList(),
            "runtimeState", map("masterVolumeRate", (double)masterVolume,
                                "recentHitErrorsMillis",
                                java.util.Arrays.stream(recent).boxed().toList()),
            "sampledFrames", sampledFrames,
            "loadedLuaName", lua.name,
            "loadedJsonName", json.name,
            "longNoteConstants", map(
                "modelLN", BMSModel.LNTYPE_LONGNOTE,
                "modelCN", BMSModel.LNTYPE_CHARGENOTE,
                "modelHCN", BMSModel.LNTYPE_HELLCHARGENOTE,
                "undefined", LongNote.TYPE_UNDEFINED,
                "noteLN", LongNote.TYPE_LONGNOTE,
                "noteCN", LongNote.TYPE_CHARGENOTE,
                "noteHCN", LongNote.TYPE_HELLCHARGENOTE));
        System.out.println(json(map("execution", execution, "cases", cases)));
    }
}
