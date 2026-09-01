package com.snurhythm.asobmashow;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;

public class AsoBMaShowBackupPathPolicyTest {
    @Rule
    public TemporaryFolder temporary = new TemporaryFolder();

    @Test
    public void selectsProfileDataWithoutCredentialArtifactsOrExcludedTrees()
            throws Exception {
        Path dataRoot = temporary.newFolder("data").toPath();
        Path filesRoot = Files.createDirectories(dataRoot.resolve("files"));
        Path profilesRoot = Files.createDirectories(filesRoot.resolve("profiles"));
        Path profileRoot = Files.createDirectories(
                profilesRoot.resolve("11111111-1111-4111-8111-111111111111"));
        Path cacheRoot = Files.createDirectories(dataRoot.resolve("cache"));

        Path settings = write(profileRoot.resolve("settings.json"), "{}");
        Path scores = write(profileRoot.resolve("scores.db"), "scores");
        Path replayDirectory = Files.createDirectories(profileRoot.resolve("replay"));
        Path nestedLookalike = write(
                replayDirectory.resolve("ir-credentials.json"), "not-a-credential");
        Path unrelatedLookalike = write(
                filesRoot.resolve("ir-credentials.json"), "not-a-credential");
        Path cached = write(cacheRoot.resolve("ignored.bin"), "cache");

        List<Path> credentialArtifacts = List.of(
                profileRoot.resolve("ir-credentials.json"),
                profileRoot.resolve("ir-credentials.json.tmp"),
                profileRoot.resolve("ir-credentials.json.bak"),
                profileRoot.resolve("ir-credentials.json.bak.pending"),
                profileRoot.resolve("ir-credentials.json.bak.previous"));
        for (Path artifact : credentialArtifacts) {
            write(artifact, "secret");
        }

        List<Path> selected = new ArrayList<>();
        AsoBMaShowBackupPathPolicy.visitBackupEntries(
                dataRoot,
                List.of(cacheRoot),
                List.of(profilesRoot),
                selected::add);

        assertTrue(selected.contains(profileRoot));
        assertTrue(selected.contains(settings));
        assertTrue(selected.contains(scores));
        assertTrue(selected.contains(nestedLookalike));
        assertTrue(selected.contains(unrelatedLookalike));
        assertFalse(selected.contains(cached));
        for (Path artifact : credentialArtifacts) {
            assertFalse(selected.contains(artifact));
        }
    }

    private static Path write(Path path, String contents) throws Exception {
        return Files.write(path, contents.getBytes(StandardCharsets.UTF_8));
    }
}
