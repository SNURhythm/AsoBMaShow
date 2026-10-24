package com.snurhythm.asobmashow;

import java.util.*;

public final class AndroidReadmeListingFixture {
    private static final String ERROR_PREFIX = "__ERROR__:";
    private final Map<String, String> transientDocumentIdCache = new HashMap<>();
    private final Resolver resolver = new Resolver();
    private String parentId = "song-id";
    private String queriedDirectory;
    private static final class Uri {
        final String value;
        Uri(String value) { this.value = value; }
        static Uri parse(String value) { return new Uri(value); }
    }
    private static final class Document {
        static final String COLUMN_DOCUMENT_ID = "id", COLUMN_DISPLAY_NAME = "name", COLUMN_MIME_TYPE = "mime";
        static final String MIME_TYPE_DIR = "directory";
    }
    private static final class DocumentsContract {
        static Uri buildChildDocumentsUriUsingTree(Uri tree, String id) { return new Uri(tree.value + "/" + id); }
    }
    private static final class Cursor implements AutoCloseable {
        final List<String[]> rows = new ArrayList<>();
        int index = -1;
        boolean closed;
        Cursor add(String id, String name, String mime) { rows.add(new String[]{id, name, mime}); return this; }
        boolean moveToNext() { return ++index < rows.size(); }
        String getString(int column) { return rows.get(index)[column]; }
        public void close() { closed = true; }
    }
    private static final class Resolver {
        Cursor cursor;
        Uri queried;
        Cursor query(Uri uri, String[] columns, Object a, Object b, Object c) { queried = uri; return cursor; }
    }
    // JSON serialization is Android's library; capture the values passed to it.
    private static final class JsonNames {
        static List<String> last;
        final List<String> values = new ArrayList<>();
        JsonNames() { last = values; }
        int length() { return values.size(); }
        void put(String value) { values.add(value); }
        public String toString() { return "names"; }
    }
    private String normalizeRelativePath(String path) { return path; }
    private String resolveDocumentId(Uri tree, String directory) { queriedDirectory = directory; return parentId; }
    private Resolver getContentResolver() { return resolver; }
    private void requireChartFilesCursor(Cursor cursor) { if (cursor == null) throw new IllegalStateException("No cursor"); }
    private String cacheKey(Uri tree, String path) { return tree.value + ":" + path; }
    // PRODUCTION_METHOD
    private static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) {
        var fixture = new AndroidReadmeListingFixture();
        var cursor = new Cursor().add("one", "Artist.TXT", "text/plain")
                .add("directory", "nested.txt", Document.MIME_TYPE_DIR)
                .add("two", "日本語\nnotes.txt", "application/octet-stream")
                .add("audio", "song.wav", "audio/wav").add("bad", "sub/file.txt", "text/plain");
        fixture.resolver.cursor = cursor;
        check(fixture.listTreeTextFiles("tree", "song").equals("names"), "Listing succeeds");
        check(JsonNames.last.equals(List.of("Artist.TXT", "日本語\nnotes.txt")), "Only immediate non-directory text names, case insensitive");
        check(fixture.queriedDirectory.equals("song") && fixture.resolver.queried.value.equals("tree/song-id"), "Query resolves the selected chart directory");
        check(fixture.transientDocumentIdCache.get("tree:song/日本語\nnotes.txt").equals("two"), "Selected file can reuse its document ID");
        check(cursor.closed, "Cursor closes");
        cursor = new Cursor();
        for (int i = 0; i < 1100; ++i) cursor.add("id" + i, i + ".txt", "text/plain");
        fixture.resolver.cursor = cursor;
        fixture.listTreeTextFiles("tree", "song");
        check(JsonNames.last.size() == 1025 && cursor.index == 1024 && cursor.closed, "Return one overflow item and stop enumeration for an explicit count-limit error");
        fixture.resolver.cursor = null;
        check(fixture.listTreeTextFiles("tree", "song").startsWith(ERROR_PREFIX), "Provider errors are not an empty directory");
        fixture.parentId = null;
        check(fixture.listTreeTextFiles("tree", "song").startsWith(ERROR_PREFIX), "Missing directory reports failure");
    }
}
