// Regenerates the stock fixtures with the pinned Beatoraja classes.
//
// Compile Beatoraja commit 5f46fe198e88abbefe9215ca2de397aef8f54bd8,
// compile this file against its build output and lib/*, then run it with the
// destination fixture directory as argv[0]. This intentionally imports
// Beatoraja's ReplayData.shrink() and libGDX Json serializer instead of
// duplicating either implementation here.

import bms.player.beatoraja.PlayConfig;
import bms.player.beatoraja.ReplayData;
import bms.player.beatoraja.input.KeyInputLog;
import com.badlogic.gdx.utils.Json;
import com.badlogic.gdx.utils.JsonWriter.OutputType;

import java.io.BufferedOutputStream;
import java.io.BufferedInputStream;
import java.io.OutputStreamWriter;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.zip.GZIPOutputStream;
import java.util.zip.GZIPInputStream;

public final class BeatorajaFixtureGenerator {
    private static final String SHA_A =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    private static final String SHA_B =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

    private static ReplayData stage(String sha, long timeOffset, int option,
                                    long optionSeed, float laneCover) {
        ReplayData replay = new ReplayData();
        replay.player = "beatoraja-fixture";
        replay.sha256 = sha;
        replay.mode = 1;
        replay.keylog = new KeyInputLog[] {
            new KeyInputLog(timeOffset + 1_000, 0, true),
            new KeyInputLog(timeOffset + 1_500, 0, false),
            new KeyInputLog(timeOffset + 2_000, 7, true),
            new KeyInputLog(timeOffset + 2_500, 7, false),
            new KeyInputLog(timeOffset + 3_000, 8, true),
            new KeyInputLog(timeOffset + 3_500, 8, false),
        };
        replay.gauge = 3;
        replay.laneShufflePattern = new int[][] {
            new int[] {2, 5, 1, 4, 0, 6, 3, 7}
        };
        replay.rand = new int[] {4, 2, 7};
        replay.date = 1_725_000_000L;
        replay.sevenToNinePattern = 0;
        replay.randomoption = option;
        replay.randomoptionseed = optionSeed;
        replay.randomoption2 = 0;
        replay.randomoption2seed = -1;
        replay.doubleoption = 0;
        replay.config = new PlayConfig();
        replay.config.setHispeed(1.75f);
        replay.config.setDuration(650);
        replay.config.setLanecover(laneCover);
        replay.config.setEnablelanecover(true);
        return replay;
    }

    private static byte[] keyRecords(KeyInputLog[] logs) {
        ByteBuffer bytes = ByteBuffer.allocate(logs.length * 9)
            .order(ByteOrder.LITTLE_ENDIAN);
        for (KeyInputLog log : logs) {
            bytes.put((byte)((log.getKeycode() + 1)
                * (log.isPressed() ? 1 : -1)));
            bytes.putLong(log.getTime());
        }
        return bytes.array();
    }

    private static void write(Path path, ReplayData... replay) throws Exception {
        for (ReplayData stage : replay) {
            stage.shrink();
        }
        Json json = new Json();
        json.setOutputType(OutputType.json);
        String document = replay.length == 1
            ? json.prettyPrint(replay[0])
            : json.prettyPrint(replay);
        try (OutputStreamWriter writer = new OutputStreamWriter(
                new BufferedOutputStream(new GZIPOutputStream(
                    Files.newOutputStream(path))), StandardCharsets.UTF_8)) {
            writer.write(document);
        }
    }

    public static void main(String[] args) throws Exception {
        if (args.length == 2 && args[0].equals("--verify")) {
            Json json = new Json();
            json.setIgnoreUnknownFields(true);
            ReplayData replay;
            try (BufferedInputStream input = new BufferedInputStream(
                    new GZIPInputStream(Files.newInputStream(Path.of(args[1]))))) {
                replay = json.fromJson(ReplayData.class, input);
            }
            if (replay == null || !replay.validate()
                    || !SHA_A.equals(replay.sha256) || replay.mode != 2
                    || replay.gauge != 4 || replay.keylog.length != 6
                    || replay.config == null
                    || replay.config.getLanecover() != 0.37f
                    || !replay.config.isEnablelanecover()) {
                throw new IllegalStateException(
                    "Aso replay was not readable as stock Beatoraja ReplayData");
            }
            System.out.println("Pinned Beatoraja accepted the Aso replay");
            return;
        }
        if (args.length != 1) {
            throw new IllegalArgumentException("fixture output directory required");
        }
        Path output = Path.of(args[0]);
        Files.createDirectories(output);

        ReplayData chart = stage(SHA_A, 0, 2, 0x123456L, 0.37f);
        Files.write(output.resolve("beatoraja-keyinput.bin"),
                    keyRecords(chart.keylog));
        write(output.resolve("beatoraja-chart.brd"), chart);

        ReplayData first = stage(SHA_A, 10_000, 1, 0x234567L, 0.25f);
        ReplayData second = stage(SHA_B, 20_000, 3, 0x345678L, 0.50f);
        write(output.resolve("beatoraja-course.brd"), first, second);
    }
}
