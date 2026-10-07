package com.snurhythm.asobmashow;

import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

public final class SkinDirectoryImportTests {
    static class Source implements SkinDirectoryImport.Source {
        Map<String, List<SkinDirectoryImport.Entry>> entries = new HashMap<>();
        volatile boolean cancelled;
        public SkinDirectoryImport.Entry root() { return entry("root", "My Skin", true, 0); }
        public List<SkinDirectoryImport.Entry> children(SkinDirectoryImport.Entry directory, long remaining) throws IOException {
            List<SkinDirectoryImport.Entry> result = entries.getOrDefault(directory.id, List.of());
            if (result.size() > remaining) throw new IOException("entry limit");
            return result;
        }
        public InputStream open(SkinDirectoryImport.Entry file) { return new ByteArrayInputStream(new byte[]{1,2,3}); }
        public void checkpoint() throws IOException { if (cancelled) throw new IOException("cancelled"); }
    }
    static SkinDirectoryImport.Entry entry(String id, String name, boolean directory, long size) {
        return new SkinDirectoryImport.Entry(id, name, directory, size);
    }
    static SkinDirectoryImport.Limits limits(long bytes, long entries, long depth, long path, long file) {
        return new SkinDirectoryImport.Limits(bytes, entries, depth, path, file);
    }
    static void require(boolean value) { if (!value) throw new AssertionError(); }
    static void reject(Source source, SkinDirectoryImport.Limits limits) throws Exception {
        Path temp = Files.createTempDirectory("skin-directory-test");
        Path output = temp.resolve("copy");
        try {
            try { SkinDirectoryImport.copy(source, output, limits); throw new AssertionError("accepted invalid tree"); }
            catch (IOException expected) { require(!Files.exists(output)); }
        } finally { SkinDirectoryImport.removeTree(temp); }
    }
    static void concurrentFilesKeepExactContentsAndProgress() throws Exception {
        CountDownLatch readers = new CountDownLatch(Math.min(2, Runtime.getRuntime().availableProcessors()));
        AtomicInteger active = new AtomicInteger();
        AtomicInteger maximum = new AtomicInteger();
        Source source = new Source() {
            public InputStream open(SkinDirectoryImport.Entry file) {
                int count = active.incrementAndGet();
                maximum.accumulateAndGet(count, Math::max);
                return new ByteArrayInputStream(new byte[]{(byte)Integer.parseInt(file.id)}) {
                    boolean first = true;
                    @Override public synchronized int read(byte[] bytes, int offset, int length) {
                        if (first) {
                            first = false;
                            readers.countDown();
                            try { require(readers.await(2, TimeUnit.SECONDS)); }
                            catch (InterruptedException error) { throw new AssertionError(error); }
                        }
                        return super.read(bytes, offset, length);
                    }
                    @Override public void close() { active.decrementAndGet(); }
                };
            }
        };
        List<SkinDirectoryImport.Entry> files = new ArrayList<>();
        for (int i = 1; i <= 20; i++) files.add(entry("" + i, "file-" + i, false, -1));
        source.entries.put("root", files);
        Path root = Files.createTempDirectory("skin-parallel-");
        List<long[]> progress = new ArrayList<>();
        try {
            SkinDirectoryImport.copy(source, root.resolve("copy"), limits(20,20,1,64,1),
                    (bytes, count) -> progress.add(new long[]{bytes, count}));
            require(maximum.get() >= Math.min(2, Runtime.getRuntime().availableProcessors())
                    && maximum.get() <= Math.min(8, Runtime.getRuntime().availableProcessors()) && active.get() == 0);
            for (int i = 1; i <= 20; i++) require(Arrays.equals(
                    Files.readAllBytes(root.resolve("copy/file-" + i)), new byte[]{(byte)i}));
            long bytes = 0, count = 0;
            for (long[] update : progress) {
                require(update[0] >= bytes && update[1] >= count);
                bytes = update[0]; count = update[1];
            }
            require(bytes == 20 && count == 20);
        } finally { SkinDirectoryImport.removeTree(root); }
    }
    public static void main(String[] args) throws Exception {
        concurrentFilesKeepExactContentsAndProgress();
        Source source = new Source();
        source.entries.put("root", List.of(entry("folder", "이미지", true, 0), entry("lua", "skin.lua", false, -1)));
        source.entries.put("folder", List.of(entry("image", "note.png", false, 3)));
        Path temp = Files.createTempDirectory("skin-directory-test");
        try {
            List<long[]> progress = new ArrayList<>();
            require(SkinDirectoryImport.copy(source, temp.resolve("copy"), limits(6,3,2,64,3),
                    (bytes, files) -> progress.add(new long[]{bytes, files})).equals("My Skin"));
            require(progress.get(0)[0] == 0 && progress.get(0)[1] == 0);
            require(progress.stream().anyMatch(p -> p[0] == 3 && p[1] == 0));
            require(progress.get(progress.size() - 1)[0] == 6);
            require(progress.get(progress.size() - 1)[1] == 2);
            require(Arrays.equals(Files.readAllBytes(temp.resolve("copy/이미지/note.png")), new byte[]{1,2,3}));
        } finally { SkinDirectoryImport.removeTree(temp); }
        reject(source, limits(5,3,2,64,3)); // actual aggregate size, including unknown sizes
        reject(source, limits(6,2,2,64,3));
        reject(source, limits(6,3,1,64,3));
        reject(source, limits(6,3,2,10,3)); // UTF-8 path size
        reject(source, limits(6,3,2,64,2));
        for (String name : List.of("..", "../outside", "a/b", "a\\b", "bad\nname", "")) {
            Source unsafe = new Source();
            unsafe.entries.put("root", List.of(entry("bad", name, false, 3)));
            reject(unsafe, limits(9,9,9,99,9));
        }
        Source duplicate = new Source();
        duplicate.entries.put("root", List.of(entry("1", "same", false, 3), entry("2", "same", false, 3)));
        reject(duplicate, limits(9,9,9,99,9));
        Source cycle = new Source();
        cycle.entries.put("root", List.of(entry("root", "cycle", true, 0)));
        reject(cycle, limits(9,9,9,99,9));
        Source cancel = new Source() {
            public InputStream open(SkinDirectoryImport.Entry file) { cancelled = true; return super.open(file); }
        };
        cancel.entries.put("root", List.of(entry("1", "file", false, 3)));
        reject(cancel, limits(9,9,9,99,9));
        Path outside = Files.createTempDirectory("skin-outside");
        Path owned = Files.createTempDirectory("skin-owned");
        try {
            Files.write(outside.resolve("keep"), new byte[]{1});
            Files.createSymbolicLink(owned.resolve("link"), outside);
            SkinDirectoryImport.removeTree(owned);
            require(Files.exists(outside.resolve("keep")));
        } finally { SkinDirectoryImport.removeTree(outside); SkinDirectoryImport.removeTree(owned); }
        System.out.println("PASS skin directory import: exact names, bounds, unsafe paths, duplicates, cycles, cancellation, safe cleanup");
    }
}
