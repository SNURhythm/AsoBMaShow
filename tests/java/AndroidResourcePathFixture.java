package com.snurhythm.asobmashow;

import java.util.*;
import java.util.concurrent.*;

public final class AndroidResourcePathFixture {
    private static final String ERROR_PREFIX = "ERROR:";
    private static final class JsonNames {
        final List<String> names = new ArrayList<>();
        void put(String name) { names.add(name); }
        int length() { return names.size(); }
    }
    private void requireChartFilesCursor(Cursor cursor) { if (cursor == null) throw new IllegalStateException(); }
    private final ConcurrentHashMap<String, String> documentIdCache = new ConcurrentHashMap<>();
    private final ConcurrentHashMap<String, String> transientDocumentIdCache = new ConcurrentHashMap<>();
    // CACHE_FIELD
    private final Resolver resolver = new Resolver();
    private static final class Uri {
        final String value;
        Uri(String value) { this.value = value; }
        static Uri parse(String value) { return new Uri(value); }
        public String toString() { return value; }
    }
    private static final class Document {
        static final String COLUMN_DOCUMENT_ID = "id", COLUMN_DISPLAY_NAME = "name", COLUMN_MIME_TYPE = "mime", MIME_TYPE_DIR = "directory";
    }
    private static final class DocumentsContract {
        static String getTreeDocumentId(Uri tree) { return "root"; }
        static Uri buildChildDocumentsUriUsingTree(Uri tree, String id) { return new Uri(tree.value + ":" + id); }
    }
    private static final class Cursor implements AutoCloseable {
        final List<String[]> rows;
        int index = -1;
        Cursor(List<String[]> rows) { this.rows = rows; }
        int getColumnIndexOrThrow(String name) { return name.equals("id") ? 0 : 1; }
        boolean moveToNext() { return ++index < rows.size(); }
        String getString(int column) { return column == 2 ? "file" : rows.get(index)[column]; }
        public void close() {}
    }
    private static final class Resolver {
        final Map<String, List<String[]>> children = new HashMap<>();
        int queries;
        String gatedUri;
        CountDownLatch queryEntered, releaseQuery;
        Cursor query(Uri uri, String[] columns, Object a, Object b, Object c) {
            ++queries;
            var rows = children.get(uri.value);
            if (uri.value.equals(gatedUri)) {
                queryEntered.countDown();
                try { if (!releaseQuery.await(5, TimeUnit.SECONDS)) throw new AssertionError("query gate timeout"); }
                catch (InterruptedException e) { throw new AssertionError(e); }
            }
            return rows == null ? null : new Cursor(rows);
        }
    }
    private Resolver getContentResolver() { return resolver; }
    private String cacheKey(Uri tree, String path) { return tree.value + ":" + path; }
    // PRODUCTION_METHODS
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    private static void testClearDuringTextListing() throws Exception {
        var f = new AndroidResourcePathFixture();
        var tree = new Uri("tree");
        f.resolver.children.put("tree:root", List.<String[]>of(new String[]{"sounds", "Sounds"}));
        f.resolver.children.put("tree:sounds", List.<String[]>of(new String[]{"old", "Readme.TXT"}));
        check("sounds".equals(f.resolveDocumentId(tree, "Sounds")), "Resolve directory before listing");
        f.resolver.gatedUri = "tree:sounds";
        f.resolver.queryEntered = new CountDownLatch(1);
        f.resolver.releaseQuery = new CountDownLatch(1);
        Thread listing = new Thread(() -> f.listTreeTextFiles("tree", "Sounds"));
        listing.start();
        check(f.resolver.queryEntered.await(5, TimeUnit.SECONDS), "Listing acquired old provider snapshot");
        Thread clearing = new Thread(f::clearTransientTreeFileCache);
        clearing.start();
        // Let clear either complete or block on the listing's transaction.
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (clearing.isAlive() && clearing.getState() != Thread.State.BLOCKED && System.nanoTime() < deadline)
            Thread.yield();
        check(!clearing.isAlive() || clearing.getState() == Thread.State.BLOCKED, "Clear reached its synchronization boundary");
        f.resolver.children.put("tree:sounds", List.<String[]>of(new String[]{"new", "Readme.TXT"}));
        f.resolver.releaseQuery.countDown();
        listing.join(5000);
        clearing.join(5000);
        check(!listing.isAlive() && !clearing.isAlive(), "Listing and clear finish");
        f.resolver.gatedUri = null;
        check("new".equals(f.resolveDocumentId(tree, "Sounds/Readme.TXT")), "In-flight listing cannot resurrect aliases after clear");
    }

    public static void main(String[] args) throws Exception {
        testClearDuringTextListing();
        var f = new AndroidResourcePathFixture();
        var tree = new Uri("tree");
        f.resolver.children.put("tree:root", List.<String[]>of(new String[]{"sounds", "Sounds"}));
        f.resolver.children.put("tree:sounds", new ArrayList<>(List.of(
                new String[]{"kick", "Kick.WAV"}, new String[]{"image", "音A.PNG"},
                new String[]{"one", "Clap.wav"}, new String[]{"two", "CLAP.wav"})));
        check("kick".equals(f.resolveDocumentId(tree, "sounds/kick.wav")), "Nested ASCII case fallback resolves resource");
        check("image".equals(f.resolveDocumentId(tree, "SOUNDS/音a.png")), "Non-ASCII bytes are preserved");
        check("one".equals(f.resolveDocumentId(tree, "Sounds/Clap.wav")), "Exact first spelling wins");
        check("two".equals(f.resolveDocumentId(tree, "Sounds/CLAP.wav")), "Exact second spelling wins");
        check(f.resolveDocumentId(tree, "sounds/clap.wav") == null, "Ambiguous folded match fails");
        check(f.resolver.queries == 2, "One provider listing per directory for all references");
        for (int i = 0; i < 1000; ++i) check(f.resolveDocumentId(tree, "sounds/absent" + i + ".wav") == null, "Missing resource");
        check(f.resolver.queries == 2, "Negative lookups reuse the complete directory index");
        f.resolver.children.get("tree:sounds").add(new String[]{"new", "New.WAV"});
        f.clearTransientTreeFileCache();
        check("new".equals(f.resolveDocumentId(tree, "sounds/new.wav")), "Next preparation sees provider changes");
        f.clearTransientTreeFileCache();
        int before = f.resolver.queries;
        check(f.cacheDirectChildren(tree, "sounds", "Sounds", f.transientDocumentIdCache) == 5, "Prewarm counts children");
        check("kick".equals(f.findChildDocumentId(tree, "sounds", "KICK.WAV")), "Prewarm feeds folded lookup");
        check(f.resolver.queries == before + 1, "Prewarm needs no redundant query");
        f.clearTransientTreeFileCache();
        before = f.resolver.queries;
        var pool = Executors.newFixedThreadPool(6);
        try {
            var tasks = new ArrayList<Callable<String>>();
            for (int i = 0; i < 24; ++i) tasks.add(() -> f.resolveDocumentId(tree, "sounds/kick.wav"));
            for (var result : pool.invokeAll(tasks)) check("kick".equals(result.get()), "Concurrent lookup succeeds");
        } finally { pool.shutdownNow(); }
        check(f.resolver.queries == before + 2, "Concurrent requests share directory queries");
        // Oversized directory indexes retain bounded memory but still finish
        // resolving names at the end of the provider's listing.
        var large = new ArrayList<String[]>();
        String padding = "x".repeat(230);
        for (int i = 0; i < 6000; ++i) large.add(new String[]{"id" + i, padding + i + ".wav"});
        large.add(new String[]{"last", "Last.WAV"});
        f.resolver.children.put("tree:large", large);
        check("last".equals(f.findChildDocumentId(tree, "large", "last.wav")), "Oversized listing still resolves its final name");
        large.add(new String[]{"collision", "LAST.wav"});
        check(f.findChildDocumentId(tree, "large", "last.wav") == null, "Oversized listing still detects ambiguity");
        check("last".equals(f.findChildDocumentId(tree, "large", "Last.WAV")), "Oversized listing preserves exact match priority");
        for (int i = 0; i < 70; ++i) {
            String id = "dir" + i;
            f.resolver.children.put("tree:" + id, List.<String[]>of(new String[]{id, "File.WAV"}));
            check(id.equals(f.findChildDocumentId(tree, id, "file.wav")), "Bounded cache resolves many directories");
        }
        check("kick".equals(f.findChildDocumentId(tree, "sounds", "kick.wav")), "Evicted directories reload correctly");
        var other = new Uri("other");
        f.resolver.children.put("other:root", List.<String[]>of(new String[]{"different", "Sounds"}));
        check("different".equals(f.resolveDocumentId(other, "sounds")), "Different tree with same IDs is isolated");
        f.clearTransientTreeFileCache();
        f.resolver.children.remove("tree:sounds");
        check(f.resolveDocumentId(tree, "sounds/kick.wav") == null, "Failed listings do not return cached aliases");
        f.resolver.children.put("tree:sounds", List.<String[]>of(new String[]{"restored", "Kick.WAV"}));
        check("restored".equals(f.resolveDocumentId(tree, "sounds/kick.wav")), "Failed listing is retried");
    }
}
