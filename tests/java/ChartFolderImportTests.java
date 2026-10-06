package com.snurhythm.asobmashow;

import java.io.ByteArrayInputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

public final class ChartFolderImportTests {
    private static File temporary;
    private static int nextOutput;

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static final class Node {
        ChartFolderImport.Entry entry;
        final List<Node> children = new ArrayList<>();
        byte[] bytes;
        Node(String id, String name, boolean directory, byte[] bytes) {
            this.bytes = bytes;
            entry = new ChartFolderImport.Entry(id, name, directory,
                    directory ? -1 : bytes.length, 100);
        }
    }

    private interface OpenHook { void run(Node node) throws IOException; }
    private interface DeleteHook { void run(Node node) throws IOException; }
    private interface ListHook { void run(Node node, int count) throws IOException; }

    private static final class FakeSource implements ChartFolderImport.Source {
        final Node root = new Node("root", "Charts", true, null);
        final Map<String, Node> nodes = new LinkedHashMap<>();
        final Map<String, Integer> listings = new LinkedHashMap<>();
        final List<String> events = new ArrayList<>();
        OpenHook opening = node -> {};
        DeleteHook deleting = node -> {};
        ListHook listing = (node, count) -> {};
        ListHook listed = (node, count) -> {};
        FakeSource() { nodes.put("root", root); }
        Node dir(Node parent, String id, String name) {
            Node node = new Node(id, name, true, null);
            parent.children.add(node);
            nodes.put(id, node);
            return node;
        }
        Node file(Node parent, String id, String name, byte[] bytes) {
            Node node = new Node(id, name, false, bytes);
            parent.children.add(node);
            nodes.put(id, node);
            return node;
        }
        Node file(Node parent, String id, String name) {
            return file(parent, id, name, new byte[] {1, 2, 3});
        }
        @Override public ChartFolderImport.Entry root() { return root.entry; }
        @Override public List<ChartFolderImport.Entry> children(ChartFolderImport.Entry directory)
                throws IOException {
            Node node = nodes.get(directory.id);
            int count = listings.getOrDefault(directory.id, 0) + 1;
            listings.put(directory.id, count);
            listing.run(node, count);
            List<ChartFolderImport.Entry> result = new ArrayList<>();
            for (Node child : node.children) result.add(child.entry);
            listed.run(node, count);
            return result;
        }
        @Override public InputStream open(ChartFolderImport.Entry entry) throws IOException {
            Node node = nodes.get(entry.id);
            events.add("open:" + entry.id);
            opening.run(node);
            return new ByteArrayInputStream(node.bytes);
        }
        @Override public void delete(ChartFolderImport.Entry directory) throws IOException {
            Node node = nodes.get(directory.id);
            events.add("delete:" + directory.id);
            deleting.run(node);
            for (Node other : nodes.values()) other.children.remove(node);
            node.children.clear();
        }
    }

    private static File output() { return new File(temporary, "output-" + nextOutput++); }
    private static ChartImportCopyControl control() {
        return new ChartImportCopyControl(() -> 1, () -> false);
    }
    private static ChartFolderImport.Result run(FakeSource source, File output, boolean move) {
        return ChartFolderImport.run(source, output, move, control(),
                (copied, total, bytes, totalBytes, name, phase) -> {}, () -> {});
    }
    private static void contents(File file, byte[] expected) throws IOException {
        require(file.isFile() && Arrays.equals(Files.readAllBytes(file.toPath()), expected),
                "Copied bytes missing or changed: " + file);
    }

    private static void testCopyKeepsSourceAndCountsChunks() throws Exception {
        FakeSource source = new FakeSource();
        Node album = source.dir(source.root, "album", "Album");
        source.dir(source.root, "empty", "Empty");
        byte[] large = new byte[2 * 1024 * 1024 + 7];
        Arrays.fill(large, (byte) 42);
        source.file(album, "music", "music.wav", large);
        source.file(source.root, "chart", "chart.bms");
        List<long[]> progress = new ArrayList<>();
        AtomicBoolean dirty = new AtomicBoolean();
        File output = output();
        ChartFolderImport.Result result = ChartFolderImport.run(source, output, false, control(),
                (copied, total, bytes, totalBytes, name, phase) ->
                        progress.add(new long[] {copied, total, bytes, totalBytes, phase.ordinal()}),
                () -> dirty.set(true));
        require(result.complete && result.retainedOutput && result.error.isEmpty(), "Copy must succeed");
        contents(new File(output, "Album/music.wav"), large);
        contents(new File(output, "chart.bms"), new byte[] {1, 2, 3});
        require(new File(output, "Empty").isDirectory(), "Empty directories must be copied");
        require(source.events.stream().noneMatch(event -> event.startsWith("delete:")) && !dirty.get(),
                "Copy must never remove source or mark deletion pending");
        require(source.listings.values().stream().allMatch(count -> count == 1),
                "Copy must discover each directory only once");
        require(progress.get(0)[0] == 0 && progress.get(0)[1] == 0
                && progress.get(0)[4] == ChartFolderImport.Phase.COUNTING.ordinal(),
                "Counting must begin at zero");
        require(progress.stream().anyMatch(p -> p[0] == 0 && p[2] > 0 && p[2] < large.length),
                "Progress must advance in chunks before a large file completes");
        long[] end = progress.get(progress.size() - 1);
        require(end[0] == 2 && end[1] == 2 && end[2] == large.length + 3
                && end[3] == large.length + 3, "Final counts and byte totals must match copied data");
    }

    private static FakeSource twoAlbums() {
        FakeSource source = new FakeSource();
        source.file(source.root, "root-chart", "root.bms");
        Node first = source.dir(source.root, "first", "First");
        source.file(first, "first-chart", "chart.bms");
        Node second = source.dir(source.root, "second", "Second");
        source.file(second, "second-chart", "chart.bms");
        return source;
    }

    private static void testMoveDeletesCompletedSubfoldersPromptly() throws Exception {
        FakeSource source = twoAlbums();
        List<String> marks = new ArrayList<>();
        File output = output();
        source.deleting = node -> {
            require(!marks.isEmpty(), "Recovery marker must be stored before provider deletion");
            if (node.entry.id.equals("first")) {
                contents(new File(output, "First/chart.bms"), new byte[] {1, 2, 3});
                require(!source.events.contains("open:second-chart"), "First folder must release space immediately");
            }
        };
        ChartFolderImport.Result result = ChartFolderImport.run(source, output, true, control(),
                (copied, total, bytes, totalBytes, name, phase) -> {}, () -> marks.add("dirty"));
        require(result.complete && result.retainedOutput, "Move should complete");
        require(source.events.equals(Arrays.asList("open:first-chart", "delete:first", "open:second-chart",
                "delete:second", "open:root-chart", "delete:root")),
                "Move must copy/delete in postorder before copying parent files: " + source.events);
    }

    private static void testLaterFailureRetainsEarlierMovedContent() throws Exception {
        FakeSource source = twoAlbums();
        source.opening = node -> {
            if (node.entry.id.equals("second-chart")) throw new IOException("Cannot read second album");
        };
        File output = output();
        ChartFolderImport.Result result = run(source, output, true);
        require(!result.complete && result.retainedOutput && result.error.contains("Cannot read"),
                "Later failure must retain a recoverable partial move");
        contents(new File(output, "First/chart.bms"), new byte[] {1, 2, 3});
        require(!new File(output, "Second/chart.bms").exists(), "Failed file must be removed");
        require(source.root.children.contains(source.nodes.get("second"))
                && !source.root.children.contains(source.nodes.get("first")),
                "Only completed moved directories may disappear from the source");
        require(!source.events.contains("delete:root"), "An incomplete parent must remain");
    }

    private static void testCancellationRemovesCurrentPartialFile() throws Exception {
        FakeSource source = twoAlbums();
        source.nodes.get("second-chart").bytes = new byte[3 * 1024 * 1024];
        source.nodes.get("second-chart").entry = new ChartFolderImport.Entry("second-chart", "chart.bms",
                false, 3 * 1024 * 1024, 100);
        AtomicBoolean cancelled = new AtomicBoolean();
        File output = output();
        ChartFolderImport.Result result = ChartFolderImport.run(source, output, true,
                new ChartImportCopyControl(() -> 1, cancelled::get),
                (copied, total, bytes, totalBytes, name, phase) -> {
                    if (bytes > 3 && copied == 1) cancelled.set(true);
                }, () -> {});
        require(!result.complete && result.retainedOutput, "Cancellation after first move must retain output");
        contents(new File(output, "First/chart.bms"), new byte[] {1, 2, 3});
        require(!new File(output, "Second/chart.bms").exists(), "Cancelled partial file must be cleaned");
        require(!source.events.contains("delete:second"), "Cancel must preserve unfinished source");
    }

    private static void testPartialProviderDeleteFailureRetainsEveryCopy() throws Exception {
        FakeSource source = new FakeSource();
        source.file(source.root, "one", "one.bms");
        source.file(source.root, "two", "two.bms");
        source.deleting = node -> {
            node.children.remove(0);
            throw new IOException("Provider deleted one file then failed");
        };
        File output = output();
        ChartFolderImport.Result result = run(source, output, true);
        require(!result.complete && result.retainedOutput, "Even a throwing first delete may remove originals");
        contents(new File(output, "one.bms"), new byte[] {1, 2, 3});
        contents(new File(output, "two.bms"), new byte[] {1, 2, 3});
    }

    private static void testFailureBeforeDeletionCleansOwnedOutput() throws Exception {
        for (boolean move : new boolean[] {false, true}) {
            FakeSource source = new FakeSource();
            source.file(source.root, "short", "short.wav");
            source.nodes.get("short").bytes = new byte[] {1};
            File output = output();
            ChartFolderImport.Result result = run(source, output, move);
            require(!result.complete && !result.retainedOutput && !output.exists(),
                    "Length mismatch must clean destination while all originals are intact");
            require(!source.events.contains("delete:root"), "Length mismatch must never remove source");
        }
    }

    private static void testChangedSourcePreventsDeletion() throws Exception {
        for (String change : new String[] {"new", "size", "modified", "name", "missing", "duplicate"}) {
            FakeSource source = new FakeSource();
            Node file = source.file(source.root, "chart", "chart.bms");
            source.listing = (node, count) -> {
                if (count == 2) {
                    if (change.equals("new")) source.file(node, "new", "new.bms");
                    else if (change.equals("missing")) node.children.clear();
                    else if (change.equals("duplicate")) node.children.add(file);
                    else file.entry = new ChartFolderImport.Entry("chart",
                            change.equals("name") ? "renamed.bms" : "chart.bms", false,
                            change.equals("size") ? 4 : 3, change.equals("modified") ? 101 : 100);
                }
            };
            File output = output();
            ChartFolderImport.Result result = run(source, output, true);
            require(!result.complete && !result.retainedOutput && !output.exists(),
                    "Source mutation must abort safe move: " + change);
            require(!source.events.contains("delete:root"), "Changed source must remain: " + change);
        }
    }

    private static void testUnsafePlansRejectedBeforeAnyDeletion() throws Exception {
        for (String name : new String[] {"../escape", "/absolute", ".", "..", "a/b", "a\\b", "", "nul\u0000"}) {
            FakeSource source = new FakeSource();
            source.file(source.root, "bad", name);
            File output = output();
            ChartFolderImport.Result result = run(source, output, true);
            require(!result.complete && !output.exists() && source.events.isEmpty(),
                    "Unsafe filename must fail before copying: " + name);
        }
        FakeSource duplicate = new FakeSource();
        Node one = duplicate.dir(duplicate.root, "one", "one");
        duplicate.root.children.add(one);
        require(!run(duplicate, output(), true).complete, "Duplicate IDs must be rejected");
        FakeSource cycle = new FakeSource();
        cycle.root.children.add(cycle.root);
        require(!run(cycle, output(), true).complete, "Cycles must be rejected");
        FakeSource deep = new FakeSource();
        Node parent = deep.root;
        for (int index = 0; index < 300; index++) parent = deep.dir(parent, "d" + index, "d");
        require(!run(deep, output(), true).complete, "Abnormally deep source trees must be rejected");
    }

    private static void testConflictingNamesNeverOverwrite() throws Exception {
        FakeSource source = new FakeSource();
        source.file(source.root, "first", "same.bms", new byte[] {1});
        source.file(source.root, "second", "same.bms", new byte[] {2});
        source.file(source.root, "third", "same (2).bms", new byte[] {3});
        source.dir(source.root, "dir", "same.bms");
        File output = output();
        require(run(source, output, false).complete, "Duplicate display names must be disambiguated");
        File[] children = output.listFiles();
        require(children != null && children.length == 4, "No collision may overwrite another entry");
        List<Byte> bytes = new ArrayList<>();
        for (File file : children) if (file.isFile()) bytes.add(Files.readAllBytes(file.toPath())[0]);
        require(bytes.size() == 3 && bytes.contains((byte) 1) && bytes.contains((byte) 2)
                && bytes.contains((byte) 3), "All colliding files must survive");
        File occupied = output();
        require(occupied.mkdir(), "Create occupied destination");
        Files.write(new File(occupied, "precious").toPath(), new byte[] {9});
        require(!run(source, occupied, true).complete, "Existing destination must be refused");
        contents(new File(occupied, "precious"), new byte[] {9});
    }

    private static void testUnknownSizeAndEmptyRoot() throws Exception {
        FakeSource empty = new FakeSource();
        File emptyOutput = output();
        require(run(empty, emptyOutput, true).complete && emptyOutput.isDirectory()
                && empty.events.equals(Arrays.asList("delete:root")), "Empty root must move successfully");
        FakeSource unknown = new FakeSource();
        Node file = unknown.file(unknown.root, "unknown", "unknown.bms");
        file.entry = new ChartFolderImport.Entry("unknown", "unknown.bms", false, -1, -1);
        List<Long> totals = new ArrayList<>();
        File output = output();
        ChartFolderImport.Result result = ChartFolderImport.run(unknown, output, false, control(),
                (copied, total, bytes, totalBytes, name, phase) -> {
                    if (phase != ChartFolderImport.Phase.COUNTING) totals.add(totalBytes);
                }, () -> {});
        require(result.complete && totals.stream().allMatch(total -> total == -1),
                "Unknown sizes must remain unknown, with actual copied bytes still tracked");
        contents(new File(output, "unknown.bms"), new byte[] {1, 2, 3});
    }

    private static void testMoveRejectsMissingFileMetadataBeforeCopying() throws Exception {
        for (long[] metadata : new long[][] {{-1, -1}, {-1, 100}, {3, -1}, {3, 0}}) {
            FakeSource source = twoAlbums();
            Node file = source.nodes.get("second-chart");
            file.entry = new ChartFolderImport.Entry(file.entry.id, file.entry.name, false,
                    metadata[0], metadata[1]);
            File output = output();
            ChartFolderImport.Result result = run(source, output, true);
            require(!result.complete && !result.retainedOutput && !output.exists(),
                    "Move must reject unavailable file metadata before creating any destination");
            require(source.events.isEmpty() && source.root.children.size() == 3,
                    "A later file with unknown metadata must not allow an earlier subtree to be moved");
            require(result.error.contains("Copy"), "Explain the safe Copy alternative");
            File copied = output();
            require(run(source, copied, false).complete, "Copy must still accept missing metadata");
            contents(new File(copied, "Second/chart.bms"), new byte[] {1, 2, 3});
            require(source.events.stream().noneMatch(event -> event.startsWith("delete:")),
                    "Copy with unknown metadata must preserve source files");
        }
    }

    private static void testMetadataLossBeforeDeletionPreservesSource() throws Exception {
        for (long[] metadata : new long[][] {{-1, 100}, {3, -1}, {3, 0}}) {
            FakeSource source = new FakeSource();
            Node file = source.file(source.root, "chart", "chart.bms");
            File output = output();
            ChartFolderImport.Result result = ChartFolderImport.run(source, output, true, control(),
                    (copied, total, bytes, totalBytes, name, phase) -> {}, () -> {
                        file.bytes = new byte[] {4, 5, 6};
                        file.entry = new ChartFolderImport.Entry("chart", "chart.bms", false,
                                metadata[0], metadata[1]);
                    });
            require(!result.complete && !result.retainedOutput && !output.exists(),
                    "Metadata disappearing after copy must prevent deletion");
            require(source.root.children.size() == 1 && Arrays.equals(file.bytes, new byte[] {4, 5, 6})
                    && !source.events.contains("delete:root"),
                    "The newer source contents must survive an uncertain final validation");
        }
    }

    private static void testCancellationImmediatelyBeforeDeletion() throws Exception {
        FakeSource source = new FakeSource();
        source.file(source.root, "chart", "chart.bms");
        AtomicBoolean cancelled = new AtomicBoolean();
        File output = output();
        ChartFolderImport.Result result = ChartFolderImport.run(source, output, true,
                new ChartImportCopyControl(() -> 1, cancelled::get),
                (copied, total, bytes, totalBytes, name, phase) -> {
                    if (phase == ChartFolderImport.Phase.REMOVING_SOURCE) cancelled.set(true);
                }, () -> {});
        require(!result.complete && !result.retainedOutput && !output.exists()
                && !source.events.contains("delete:root"), "Cancellation before provider call must preserve source");
    }

    private static void testNestedMoveRemovesChildrenBeforeParentFiles() throws Exception {
        FakeSource source = new FakeSource();
        Node album = source.dir(source.root, "album", "Album");
        source.file(album, "album-chart", "album.bms");
        Node samples = source.dir(album, "samples", "Samples");
        source.file(samples, "sample", "sample.wav");
        File output = output();
        require(run(source, output, true).complete, "Nested move should complete");
        require(source.events.equals(Arrays.asList("open:sample", "delete:samples", "open:album-chart",
                "delete:album", "delete:root")), "Nested sources must be removed in postorder");
        contents(new File(output, "Album/Samples/sample.wav"), new byte[] {1, 2, 3});
        contents(new File(output, "Album/album.bms"), new byte[] {1, 2, 3});
    }

    private static void testRootMutationAfterChildMoveRetainsOutput() throws Exception {
        FakeSource source = twoAlbums();
        source.listing = (node, count) -> {
            if (node == source.root && count == 2) source.file(node, "new", "new.bms");
        };
        File output = output();
        ChartFolderImport.Result result = run(source, output, true);
        require(!result.complete && result.retainedOutput, "Late source mutation must retain moved content");
        contents(new File(output, "First/chart.bms"), new byte[] {1, 2, 3});
        contents(new File(output, "Second/chart.bms"), new byte[] {1, 2, 3});
        contents(new File(output, "root.bms"), new byte[] {1, 2, 3});
        require(!source.events.contains("delete:root") && source.root.children.size() == 2,
                "Root must retain its original direct file and newly added file");
    }

    private static void testFailedRecoveryMarkerPreventsDeletion() throws Exception {
        FakeSource source = new FakeSource();
        source.file(source.root, "chart", "chart.bms");
        File output = output();
        ChartFolderImport.Result result = ChartFolderImport.run(source, output, true, control(),
                (copied, total, bytes, totalBytes, name, phase) -> {},
                () -> { throw new IllegalStateException("Cannot save recovery marker"); });
        require(!result.complete && !result.retainedOutput && !output.exists()
                && !source.events.contains("delete:root"), "Missing recovery marker must prevent deletion");
    }

    private static void testDiscoveryFailureDoesNotStartMoving() throws Exception {
        FakeSource source = twoAlbums();
        source.listing = (node, count) -> {
            if (node.entry.id.equals("second")) throw new IOException("Cannot list folder");
        };
        File output = output();
        require(!run(source, output, true).complete && !output.exists() && source.events.isEmpty(),
                "A complete plan must be discovered before any copying or deletion");
    }

    private static void testCancelledCopyCleansAllDestination() throws Exception {
        FakeSource source = new FakeSource();
        source.file(source.root, "large", "large.wav", new byte[3 * 1024 * 1024]);
        AtomicBoolean cancelled = new AtomicBoolean();
        File output = output();
        ChartFolderImport.Result result = ChartFolderImport.run(source, output, false,
                new ChartImportCopyControl(() -> 1, cancelled::get),
                (copied, total, bytes, totalBytes, name, phase) -> {
                    if (bytes > 0) cancelled.set(true);
                }, () -> {});
        require(!result.complete && !result.retainedOutput && !output.exists()
                && source.root.children.size() == 1, "Cancelled Copy must remove its partial destination");
    }

    private static void testDestinationSymlinkCannotRedirectTransfer() throws Exception {
        FakeSource source = new FakeSource();
        source.file(source.root, "chart", "chart.bms");
        File outside = output();
        File output = output();
        Files.createSymbolicLink(output.toPath(), outside.toPath());
        ChartFolderImport.Result result = run(source, output, true);
        require(!result.complete && !outside.exists() && source.events.isEmpty(),
                "An existing destination symlink must never redirect copying or authorize source removal");
        Files.delete(output.toPath());
    }

    private static void testPauseAfterValidationRequiresFreshSourceListing() throws Exception {
        FakeSource source = new FakeSource();
        AtomicInteger state = new AtomicInteger(1);
        AtomicBoolean pauseAfterListing = new AtomicBoolean();
        CountDownLatch paused = new CountDownLatch(1);
        source.listed = (node, count) -> {
            if (count == 2) pauseAfterListing.set(true);
        };
        ChartImportCopyControl control = new ChartImportCopyControl(() -> {
            if (pauseAfterListing.getAndSet(false)) state.set(0);
            if (state.get() == 0) paused.countDown();
            return state.get();
        }, () -> false);
        File output = output();
        AtomicReference<ChartFolderImport.Result> result = new AtomicReference<>();
        Thread worker = new Thread(() -> result.set(ChartFolderImport.run(source, output, true, control,
                (copied, total, bytes, totalBytes, name, phase) -> {}, () -> {})));
        worker.start();
        try {
            require(paused.await(2, TimeUnit.SECONDS), "Move must observe the pause after its final empty listing");
            source.file(source.root, "new", "new.bms");
            state.set(1);
            worker.join(2000);
            require(!worker.isAlive(), "Move must finish after resume");
            require(!result.get().complete && source.root.children.size() == 1
                    && !source.events.contains("delete:root"),
                    "Resume must revalidate source instead of deleting a newly added file");
            require(!result.get().retainedOutput && !output.exists(), "No deletion attempt must mean safe cleanup");
        } finally {
            worker.interrupt();
            worker.join(2000);
        }
    }

    private static void testPauseInsideProviderListingRequiresFreshSnapshot() throws Exception {
        FakeSource source = twoAlbums();
        AtomicInteger state = new AtomicInteger(1);
        CountDownLatch paused = new CountDownLatch(1);
        ChartImportCopyControl control = new ChartImportCopyControl(() -> {
            if (state.get() == 0) paused.countDown();
            return state.get();
        }, () -> false);
        source.listed = (node, count) -> {
            if (node.entry.id.equals("second") && count == 2) {
                state.set(0);
                control.checkpoint();
            }
        };
        File output = output();
        AtomicReference<ChartFolderImport.Result> result = new AtomicReference<>();
        Thread worker = new Thread(() -> result.set(ChartFolderImport.run(source, output, true, control,
                (copied, total, bytes, totalBytes, name, phase) -> {}, () -> {})));
        worker.start();
        try {
            require(paused.await(2, TimeUnit.SECONDS), "Provider must pause with its old listing snapshot");
            source.file(source.nodes.get("second"), "new", "new.bms");
            state.set(1);
            worker.join(2000);
            require(!worker.isAlive(), "Move must finish after provider resumes");
            require(!result.get().complete && result.get().retainedOutput
                    && source.nodes.get("second").children.size() == 2
                    && !source.events.contains("delete:second"),
                    "A pause within provider iteration must invalidate its source snapshot");
            contents(new File(output, "First/chart.bms"), new byte[] {1, 2, 3});
        } finally {
            worker.interrupt();
            worker.join(2000);
        }
    }

    private static void testPauseDuringValidationCanResumeUnchangedMove() throws Exception {
        FakeSource source = new FakeSource();
        source.file(source.root, "chart", "chart.bms");
        AtomicInteger state = new AtomicInteger(1);
        CountDownLatch paused = new CountDownLatch(1);
        ChartImportCopyControl control = new ChartImportCopyControl(() -> {
            if (state.get() == 0) paused.countDown();
            return state.get();
        }, () -> false);
        source.listed = (node, count) -> {
            if (count == 2) {
                state.set(0);
                control.checkpoint();
            }
        };
        File output = output();
        AtomicReference<ChartFolderImport.Result> result = new AtomicReference<>();
        Thread worker = new Thread(() -> result.set(ChartFolderImport.run(source, output, true, control,
                (copied, total, bytes, totalBytes, name, phase) -> {}, () -> {})));
        worker.start();
        try {
            require(paused.await(2, TimeUnit.SECONDS), "Provider must pause during final validation");
            require(!source.events.contains("delete:root"), "Paused move must not delete source");
            state.set(1);
            worker.join(2000);
            require(!worker.isAlive() && result.get().complete && source.listings.get("root") == 3,
                    "Unchanged source must be freshly listed and moved after resume");
            contents(new File(output, "chart.bms"), new byte[] {1, 2, 3});
        } finally {
            worker.interrupt();
            worker.join(2000);
        }
    }

    private static void cleanup(File file) throws IOException {
        if (file.isDirectory()) for (File child : file.listFiles()) cleanup(child);
        Files.deleteIfExists(file.toPath());
    }

    public static void main(String[] args) throws Exception {
        temporary = Files.createTempDirectory("chart-folder-import-").toFile();
        try {
            testCopyKeepsSourceAndCountsChunks();
            testMoveDeletesCompletedSubfoldersPromptly();
            testLaterFailureRetainsEarlierMovedContent();
            testCancellationRemovesCurrentPartialFile();
            testPartialProviderDeleteFailureRetainsEveryCopy();
            testFailureBeforeDeletionCleansOwnedOutput();
            testChangedSourcePreventsDeletion();
            testUnsafePlansRejectedBeforeAnyDeletion();
            testConflictingNamesNeverOverwrite();
            testUnknownSizeAndEmptyRoot();
            testMoveRejectsMissingFileMetadataBeforeCopying();
            testMetadataLossBeforeDeletionPreservesSource();
            testCancellationImmediatelyBeforeDeletion();
            testNestedMoveRemovesChildrenBeforeParentFiles();
            testRootMutationAfterChildMoveRetainsOutput();
            testFailedRecoveryMarkerPreventsDeletion();
            testDiscoveryFailureDoesNotStartMoving();
            testCancelledCopyCleansAllDestination();
            testDestinationSymlinkCannotRedirectTransfer();
            testPauseAfterValidationRequiresFreshSourceListing();
            testPauseInsideProviderListingRequiresFreshSnapshot();
            testPauseDuringValidationCanResumeUnchangedMove();
            System.out.println("22 chart folder import tests passed");
        } finally {
            cleanup(temporary);
        }
    }
}
