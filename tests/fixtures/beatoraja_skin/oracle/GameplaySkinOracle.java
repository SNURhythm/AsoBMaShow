package bms.player.beatoraja.skin.lr2;

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

    private static Map<String,Object> hitError(SkinHitErrorVisualizer graph) throws Exception {
        Color[] judge = (Color[])field(graph, "JColor");
        var update = SkinHitErrorVisualizer.class.getDeclaredMethod("updateEMA", long.class);
        update.setAccessible(true);
        List<Object> ema = new ArrayList<>();
        for (long value : new long[]{40, -20, 10}) {
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

    private static Map<String,Object> selector() throws Exception {
        Config config = new Config();
        AudioConfig audio = new AudioConfig();
        audio.setSystemvolume(0.5f);
        config.setAudioConfig(audio);
        PlayerResource resource = (PlayerResource)UNSAFE.allocateInstance(PlayerResource.class);
        setField(resource, PlayerResource.class, "config", config);
        OracleState state = (OracleState)UNSAFE.allocateInstance(OracleState.class);
        setField(state, MainState.class, "resource", resource);
        return map("numeric", FloatPropertyFactory.getRateProperty(17).get(state),
                   "named", FloatPropertyFactory.getRateProperty("mastervolume").get(state));
    }

    public static void main(String[] arguments) throws Exception {
        if (arguments.length != 3) {
            throw new IllegalArgumentException("expected JSON, cross-format LR2, and all-command LR2 fixtures");
        }
        JsonValue root = new JsonReader().parse(Files.readString(Path.of(arguments[0])));
        Map<String,Object> cases = new LinkedHashMap<>();
        cases.put("selector.master-volume", selector());

        JsonValue note = first(root, "judgegraph");
        cases.put("json.note-distribution", noteDistribution(new SkinNoteDistributionGraph(
            note.getInt("type"), note.getInt("delay"), note.getInt("backTexOff"),
            note.getInt("orderReverse"), note.getInt("noGap"), note.getInt("noGapX"))));

        JsonValue bpm = first(root, "bpmgraph");
        cases.put("json.bpm-graph", bpm(new SkinBPMGraph(
            bpm.getInt("delay"), bpm.getInt("lineWidth"), string(bpm, "mainBPMColor"),
            string(bpm, "minBPMColor"), string(bpm, "maxBPMColor"),
            string(bpm, "otherBPMColor"), string(bpm, "stopLineColor"),
            string(bpm, "transitionLineColor"))));

        JsonValue gauge = first(root, "gaugegraph");
        Color[][] gaugeColors = new Color[6][4];
        JsonValue color = gauge.get("color").child;
        for (int row = 0; row < 6; ++row) {
            for (int column = 0; column < 4; ++column) {
                gaugeColors[row][column] = Color.valueOf(color.asString());
                color = color.next;
            }
        }
        cases.put("json.gauge-graph", gauge(new SkinGaugeGraphObject(gaugeColors)));

        JsonValue timing = first(root, "timingvisualizer");
        cases.put("json.timing-visualizer", timing(new SkinTimingVisualizer(
            timing.getInt("width"), timing.getInt("judgeWidthMillis"),
            timing.getInt("lineWidth"), string(timing, "lineColor"),
            string(timing, "centerColor"), string(timing, "PGColor"),
            string(timing, "GRColor"), string(timing, "GDColor"),
            string(timing, "BDColor"), string(timing, "PRColor"),
            timing.getInt("transparent"), timing.getInt("drawDecay"))));

        JsonValue hit = first(root, "hiterrorvisualizer");
        cases.put("json.hit-error-visualizer", hitError(new SkinHitErrorVisualizer(
            hit.getInt("width"), hit.getInt("judgeWidthMillis"), hit.getInt("lineWidth"),
            hit.getInt("colorMode"), hit.getInt("hiterrorMode"), hit.getInt("emaMode"),
            string(hit, "lineColor"), string(hit, "centerColor"), string(hit, "PGColor"),
            string(hit, "GRColor"), string(hit, "GDColor"), string(hit, "BDColor"),
            string(hit, "PRColor"), string(hit, "emaColor"), hit.getFloat("alpha"),
            hit.getInt("windowLength"), hit.getInt("transparent"), hit.getInt("drawDecay"))));

        JsonValue distribution = first(root, "timingdistributiongraph");
        cases.put("json.timing-distribution", timingDistribution(new SkinTimingDistributionGraph(
            distribution.getInt("width"), distribution.getInt("lineWidth"),
            string(distribution, "graphColor"), string(distribution, "averageColor"),
            string(distribution, "devColor"), string(distribution, "PGColor"),
            string(distribution, "GRColor"), string(distribution, "GDColor"),
            string(distribution, "BDColor"), string(distribution, "PRColor"),
            distribution.getInt("drawAverage"), distribution.getInt("drawDev"))));

        Path crossLr2 = Path.of(arguments[1]);
        Path allLr2 = Path.of(arguments[2]);
        cases.put("lr2.slider", lr2Slider(crossLr2));
        cases.putAll(lr2Graphs(allLr2));
        System.out.println(json(cases));
    }
}
