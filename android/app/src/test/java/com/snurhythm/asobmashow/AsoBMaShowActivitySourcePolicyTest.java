package com.snurhythm.asobmashow;

import static java.nio.charset.StandardCharsets.UTF_8;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.junit.Test;

public class AsoBMaShowActivitySourcePolicyTest {
    private static final Path ACTIVITY_SOURCE = Paths.get(
            "src", "main", "java", "com", "snurhythm", "asobmashow",
            "AsoBMaShowActivity.java");
    private static final Pattern ROLLBACK_ERROR_SINK = Pattern.compile(
            "DocumentHandoffRollbackCoordinator\\.createDefault\\(\\s*(.*?)\\);",
            Pattern.DOTALL);

    @Test
    public void rollbackFailureLoggingUsesFixedMessageWithoutThrowable()
            throws IOException {
        String source = new String(
                Files.readAllBytes(ACTIVITY_SOURCE), UTF_8);
        Matcher errorSink = ROLLBACK_ERROR_SINK.matcher(source);

        assertTrue(
                "Activity source must define the rollback error sink",
                errorSink.find());
        assertEquals(
                "Rollback diagnostics must not expose Throwable details at "
                        + "the Activity logging boundary",
                "ignored -> Log.e(\n"
                        + "                            TAG,\n"
                        + "                            \"Could not restore an empty export destination.\")",
                errorSink.group(1).trim());
    }
}
