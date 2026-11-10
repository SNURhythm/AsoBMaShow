package com.snurhythm.asobmashow;

import java.io.*;
import java.net.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

// No Activity or SDL dependency: the same transport runs under either OS host.
final class BmsDownloadTransport {
    static final class HttpFailure extends IOException {
        final int status;
        HttpFailure(int status) { super("Download failed (HTTP " + status + ")."); this.status = status; }
    }
    interface Listener {
        boolean cancelled();
        void progress(long bytes, long total);
    }
    private static final Pattern RANGE = Pattern.compile("bytes ([0-9]+)-([0-9]+)/([0-9]+)");
    private static URL httpUrl(String value) throws IOException {
        URL url = new URL(value);
        if (!(url.getProtocol().equals("https") || url.getProtocol().equals("http")) ||
                url.getHost().isEmpty() || url.getUserInfo() != null)
            throw new IOException("Unsupported download URL.");
        return url;
    }
    private static boolean sameOrigin(URL a, URL b) {
        return a.getProtocol().equalsIgnoreCase(b.getProtocol()) && a.getHost().equalsIgnoreCase(b.getHost()) &&
                (a.getPort() == -1 ? a.getDefaultPort() : a.getPort()) ==
                (b.getPort() == -1 ? b.getDefaultPort() : b.getPort());
    }
    static boolean allowedRedirect(String from, String to) {
        try {
            URL a = httpUrl(from), b = httpUrl(to);
            return !a.getProtocol().equals("https") || b.getProtocol().equals("https");
        } catch (IOException error) { return false; }
    }
    private static void checkpoint(File meta, Properties state, long bytes, FileOutputStream output) throws IOException {
        output.getFD().sync();
        state.setProperty("bytes", Long.toString(bytes));
        File temp = new File(meta + ".new");
        try (FileOutputStream out = new FileOutputStream(temp)) { state.store(out, null); out.getFD().sync(); }
        Files.move(temp.toPath(), meta.toPath(), StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
    }
    private static boolean completed(String source, File part, Properties state) {
        return source.equals(state.getProperty("url")) && "true".equals(state.getProperty("complete")) &&
                Long.toString(part.length()).equals(state.getProperty("bytes")) && part.isFile();
    }
    static boolean completed(String source, File part) throws IOException {
        File meta = new File(part + ".resume");
        if (!meta.isFile() || meta.length() >= 32768) return false;
        Properties state = new Properties();
        try (InputStream input = new FileInputStream(meta)) { state.load(input); }
        return completed(source, part, state);
    }
    // A retried native attempt can be admitted before the previous worker has
    // closed its stream. Serialize writers so the retained resume file has one owner.
    static synchronized void download(String source, Map<String, String> headers, File part, Listener listener) throws IOException {
        URL initial = httpUrl(source), url = initial;
        File meta = new File(part + ".resume");
        Properties state = new Properties();
        if (meta.isFile() && meta.length() < 32768) {
            try (InputStream input = new FileInputStream(meta)) { state.load(input); }
        }
        if (completed(source, part, state)) return;
        long offset = 0;
        String validator = state.getProperty("validator", "");
        try {
            if (source.equals(state.getProperty("url")) && !validator.isEmpty())
                offset = Math.min(part.length(), Math.max(0, Long.parseLong(state.getProperty("bytes", "0"))));
        } catch (NumberFormatException ignored) { offset = 0; }
        try (RandomAccessFile file = new RandomAccessFile(part, "rw")) { file.setLength(offset); }
        boolean scopedHeaders = true;
        for (int redirects = 0; redirects <= 10; ++redirects) {
            if (listener.cancelled()) throw new IOException("Download interrupted. Retry to continue.");
            HttpURLConnection connection = (HttpURLConnection) url.openConnection();
            connection.setInstanceFollowRedirects(false);
            connection.setConnectTimeout(15000); connection.setReadTimeout(15000);
            connection.setRequestProperty("Accept-Encoding", "identity");
            connection.setRequestProperty("User-Agent", "AsoBMaShow");
            for (Map.Entry<String, String> entry : headers.entrySet()) {
                String key = entry.getKey();
                if (scopedHeaders || key.equalsIgnoreCase("User-Agent") || key.equalsIgnoreCase("Accept"))
                    connection.setRequestProperty(key, entry.getValue());
            }
            if (offset > 0 && url.toString().equals(state.getProperty("effectiveUrl"))) {
                connection.setRequestProperty("Range", "bytes=" + offset + "-");
                connection.setRequestProperty("If-Range", validator);
            }
            try {
                int status = connection.getResponseCode();
                if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
                    String location = connection.getHeaderField("Location");
                    if (location == null) throw new IOException("Missing download redirect.");
                    URL next = new URL(url, location);
                    if (!allowedRedirect(url.toString(), next.toString())) throw new IOException("Unsafe download redirect.");
                    scopedHeaders &= sameOrigin(url, next);
                    url = next;
                    continue;
                }
                if (status == 416 && offset > 0) {
                    offset = 0; url = initial; scopedHeaders = true;
                    continue;
                }
                if (status != 200 && status != 206) throw new HttpFailure(status);
                long length = connection.getContentLengthLong(), total = length;
                String etag = connection.getHeaderField("ETag");
                String modified = connection.getHeaderField("Last-Modified");
                String nextValidator = etag != null && !etag.startsWith("W/") ? etag : modified;
                if (status == 206) {
                    Matcher range = RANGE.matcher(Objects.toString(connection.getHeaderField("Content-Range"), ""));
                    if (offset == 0 || !range.matches()) throw new IOException("Invalid resumed download response.");
                    try {
                        long start = Long.parseLong(range.group(1)), end = Long.parseLong(range.group(2));
                        total = Long.parseLong(range.group(3));
                        if (start != offset || end < start || end != total - 1 ||
                                (length >= 0 && length != total - start) ||
                                (nextValidator != null && !validator.equals(nextValidator)))
                            throw new IOException("Download changed while resuming.");
                    } catch (NumberFormatException error) { throw new IOException("Invalid download length."); }
                } else { offset = 0; }
                String encoding = connection.getHeaderField("Content-Encoding");
                if (encoding != null && !encoding.equalsIgnoreCase("identity")) throw new IOException("Unsupported archive transfer encoding.");
                final long reserve = 512L * 1024 * 1024;
                long free = part.getParentFile().getUsableSpace();
                if (free < reserve || (total > 0 && total - offset > free - reserve))
                    throw new IOException("Not enough storage for the download reserve.");
                state.clear(); state.setProperty("url", source);
                state.setProperty("effectiveUrl", url.toString());
                state.setProperty("validator", nextValidator == null ? "" : nextValidator);
                long bytes = offset, checkpointBytes = offset;
                try (InputStream input = connection.getInputStream(); FileOutputStream output = new FileOutputStream(part, offset > 0)) {
                    checkpoint(meta, state, bytes, output);
                    try {
                        byte[] buffer = new byte[65536];
                        while (true) {
                            if (listener.cancelled()) throw new IOException("Download interrupted. Retry to continue.");
                            int count = input.read(buffer);
                            if (count < 0) break;
                            if (total >= 0 && count > total - bytes) throw new IOException("Download exceeded its declared length.");
                            output.write(buffer, 0, count); bytes += count;
                            listener.progress(bytes, Math.max(0, total));
                            if (bytes - checkpointBytes >= 4 * 1024 * 1024) {
                                if (part.getParentFile().getUsableSpace() < reserve)
                                    throw new IOException("Not enough storage for the download reserve.");
                                checkpoint(meta, state, bytes, output); checkpointBytes = bytes;
                            }
                        }
                        if (total >= 0 && bytes != total) throw new IOException("Download ended early. Retry to continue.");
                        if (listener.cancelled()) throw new IOException("Download interrupted. Retry to continue.");
                        state.setProperty("complete", "true");
                    } finally { checkpoint(meta, state, bytes, output); }
                }
                return;
            } finally { connection.disconnect(); }
        }
        throw new IOException("Too many download redirects.");
    }
}
