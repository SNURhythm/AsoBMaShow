package com.snurhythm.asobmashow;

import java.util.HashMap;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.Map;

// Preparation-scoped, read-only SAF resource names. Cleared with transient IDs.
final class ResourceDirectoryCache {
    private static final int MAX_DIRECTORIES = 64, MAX_NAMES = 65536, MAX_CHARS = 2 * 1024 * 1024;
    private final LinkedHashMap<String, Directory> directories = new LinkedHashMap<>(16, 0.75f, true);
    private int names, chars;

    static String fold(String name) {
        StringBuilder result = new StringBuilder(name.length());
        for (int i = 0; i < name.length(); ++i) {
            char c = name.charAt(i);
            result.append(c >= 'A' && c <= 'Z' ? (char) (c + ('a' - 'A')) : c);
        }
        return result.toString();
    }

    static final class Directory {
        final Map<String, String> exact = new HashMap<>();
        final Map<String, String> folded = new HashMap<>();
        private final String requested, requestedFolded;
        private String exactMatch, foldedMatch;
        private boolean ambiguous;
        private int count, chars;
        private boolean retain = true;

        Directory(String requested) {
            this.requested = requested;
            requestedFolded = requested == null ? null : fold(requested);
        }

        void add(String name, String id) {
            if (name == null || name.isEmpty() || id == null) return;
            String key = fold(name);
            if (name.equals(requested)) exactMatch = id;
            if (key.equals(requestedFolded)) {
                if (foldedMatch != null) ambiguous = true;
                foldedMatch = id;
            }
            if (!retain) return;
            ++count;
            chars += 2 * name.length() + 2 * id.length();
            if (count > MAX_NAMES || chars > MAX_CHARS) {
                exact.clear();
                folded.clear();
                retain = false; // Keep resolving the requested name while streaming.
                return;
            }
            exact.put(name, id);
            // Empty ID marks a collision; exact spellings remain usable.
            folded.merge(key, id, (first, second) -> "");
        }

        String find(String name) {
            if (!retain) return name.equals(requested)
                    ? (exactMatch != null ? exactMatch : ambiguous ? null : foldedMatch) : null;
            String result = exact.get(name);
            if (result != null) return result;
            result = folded.get(fold(name));
            return result == null || result.isEmpty() ? null : result;
        }
    }

    // The Activity holds this cache's monitor across lookup/query/publication,
    // including transient alias writes, prewarming, and clearing.
    Directory get(String key) { return directories.get(key); }

    void remember(String key, Directory directory) {
        Directory old = directories.remove(key);
        if (old != null) { names -= old.count; chars -= old.chars; }
        if (!directory.retain) return;
        Iterator<Map.Entry<String, Directory>> entries = directories.entrySet().iterator();
        while (entries.hasNext() && (directories.size() >= MAX_DIRECTORIES ||
                names + directory.count > MAX_NAMES || chars + directory.chars > MAX_CHARS)) {
            Directory evicted = entries.next().getValue();
            names -= evicted.count;
            chars -= evicted.chars;
            entries.remove();
        }
        directories.put(key, directory);
        names += directory.count;
        chars += directory.chars;
    }

    void clear() { directories.clear(); names = 0; chars = 0; }
}
