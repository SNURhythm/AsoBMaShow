package com.snurhythm.asobmashow;

import java.io.*;
import java.nio.file.*;
import java.util.*;

public final class SkinDirectoryImportTests {
    static class Source implements SkinDirectoryImport.Source {
        Map<String, List<SkinDirectoryImport.Entry>> entries = new HashMap<>();
        boolean cancelled;
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
    public static void main(String[] args) throws Exception {
        Source source = new Source();
        source.entries.put("root", List.of(entry("folder", "이미지", true, 0), entry("lua", "skin.lua", false, -1)));
        source.entries.put("folder", List.of(entry("image", "note.png", false, 3)));
        Path temp = Files.createTempDirectory("skin-directory-test");
        try {
            require(SkinDirectoryImport.copy(source, temp.resolve("copy"), limits(6,3,2,64,3)).equals("My Skin"));
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
