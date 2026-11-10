package com.snurhythm.asobmashow;

import com.sun.net.httpserver.HttpServer;
import java.net.InetSocketAddress;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.concurrent.atomic.*;
import java.util.concurrent.*;

public final class BmsDownloadTransportTest {
    static void check(boolean value, String message) { if (!value) throw new AssertionError(message); }
    public static void main(String[] args) throws Exception {
        Path root = Paths.get(args[0]);
        byte[] payload = new byte[256 * 1024];
        new Random(42).nextBytes(payload);
        HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        HttpServer other = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
        AtomicBoolean resumed = new AtomicBoolean(), leaked = new AtomicBoolean();
        AtomicReference<String> version = new AtomicReference<>("\"v1\"");
        server.createContext("/archive", exchange -> {
            String range = exchange.getRequestHeaders().getFirst("Range");
            String validator = exchange.getRequestHeaders().getFirst("If-Range");
            int offset = 0;
            if (range != null && version.get().equals(validator)) {
                offset = Integer.parseInt(range.substring(6, range.length() - 1));
                resumed.set(offset > 0);
                exchange.getResponseHeaders().set("Content-Range", "bytes " + offset + "-" + (payload.length-1) + "/" + payload.length);
            }
            exchange.getResponseHeaders().set("ETag", version.get());
            exchange.sendResponseHeaders(offset > 0 ? 206 : 200, payload.length-offset);
            try (OutputStream out = exchange.getResponseBody()) { out.write(payload, offset, payload.length-offset); }
        });
        other.createContext("/file", exchange -> {
            leaked.set(exchange.getRequestHeaders().getFirst("Cookie") != null ||
                       exchange.getRequestHeaders().getFirst("Authorization") != null ||
                       exchange.getRequestHeaders().getFirst("Referer") != null);
            exchange.sendResponseHeaders(200, payload.length);
            try (OutputStream out = exchange.getResponseBody()) { out.write(payload); }
        });
        server.createContext("/redirect", exchange -> {
            exchange.getResponseHeaders().set("Location", "http://127.0.0.1:" + other.getAddress().getPort() + "/file");
            exchange.sendResponseHeaders(302, -1); exchange.close();
        });
        server.start(); other.start();
        try {
            String url = "http://127.0.0.1:" + server.getAddress().getPort() + "/archive";
            File part = root.resolve("transfer.part").toFile();
            AtomicBoolean cancel = new AtomicBoolean();
            BmsDownloadTransport.Listener listener = new BmsDownloadTransport.Listener() {
                public boolean cancelled() { return cancel.get(); }
                public void progress(long bytes, long total) { if (bytes >= 65536) cancel.set(true); }
            };
            try { BmsDownloadTransport.download(url, Map.of(), part, listener); throw new AssertionError("cancellation ignored"); }
            catch (IOException expected) { check(part.length() > 0 && part.length() < payload.length, "partial download retained"); }
            BmsDownloadTransport.Listener live = new BmsDownloadTransport.Listener() {
                public boolean cancelled() { return false; }
                public void progress(long bytes, long total) {}
            };
            BmsDownloadTransport.download(url, Map.of(), part, live);
            check(resumed.get(), "retry uses validator and range");
            check(BmsDownloadTransport.completed(url, part), "completed receipt permits recovery without network");
            check(Arrays.equals(Files.readAllBytes(part.toPath()), payload), "resumed bytes are exact");
            // A changed representation must replace, not append to, an earlier prefix.
            Files.write(part.toPath(), Arrays.copyOf(payload, 123));
            version.set("\"v2\""); resumed.set(false);
            BmsDownloadTransport.download(url, Map.of(), part, live);
            check(!resumed.get() && Arrays.equals(Files.readAllBytes(part.toPath()), payload), "changed validator restarts safely");
            BmsDownloadTransport.download(url.replace("archive", "redirect"),
                    Map.of("Cookie", "private=value", "Authorization", "secret", "Referer", url),
                    root.resolve("redirect.part").toFile(), live);
            check(!leaked.get(), "cross-origin redirect leaked credentials");
            check(!BmsDownloadTransport.allowedRedirect("https://example.com/a", "http://example.com/a"), "HTTPS downgrade accepted");
            check(!BmsDownloadTransport.allowedRedirect(url, "file:///etc/passwd"), "non-http redirect accepted");
            // A new native attempt can be admitted while the previous worker
            // is still unwinding. Never let two writers share its resume files.
            CountDownLatch firstWriting = new CountDownLatch(1), releaseFirst = new CountDownLatch(1);
            AtomicBoolean secondWriting = new AtomicBoolean(), stopFirst = new AtomicBoolean();
            AtomicReference<Throwable> threadError = new AtomicReference<>();
            File shared = root.resolve("race.part").toFile();
            Thread first = new Thread(() -> {
                try {
                    BmsDownloadTransport.download(url, Map.of(), shared, new BmsDownloadTransport.Listener() {
                        public boolean cancelled() { return stopFirst.get(); }
                        public void progress(long bytes, long total) {
                            firstWriting.countDown();
                            try { releaseFirst.await(); } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
                        }
                    });
                } catch (IOException expected) { if (!stopFirst.get()) threadError.set(expected); }
            });
            Thread second = new Thread(() -> {
                try {
                    BmsDownloadTransport.download(url, Map.of(), shared, new BmsDownloadTransport.Listener() {
                        public boolean cancelled() { return false; }
                        public void progress(long bytes, long total) { secondWriting.set(true); }
                    });
                } catch (IOException error) { threadError.set(error); }
            });
            first.start();
            check(firstWriting.await(3, TimeUnit.SECONDS), "first writer started");
            second.start();
            boolean overlapped;
            try {
                long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
                while (!secondWriting.get() && second.getState() != Thread.State.BLOCKED && System.nanoTime() < deadline)
                    Thread.yield();
                overlapped = secondWriting.get() || second.getState() != Thread.State.BLOCKED;
            } finally {
                stopFirst.set(true); releaseFirst.countDown(); first.join(3000); second.join(3000);
            }
            check(!overlapped, "retry wrote the retained file before old transport stopped");
            check(threadError.get() == null && !first.isAlive() && !second.isAlive(), "serialized transfers finished");
            check(Arrays.equals(Files.readAllBytes(shared.toPath()), payload), "serialized retry preserved exact bytes");
            System.out.println("PASS: cancellation, validator resume, representation replacement, redirect credential scope");
        } finally { server.stop(0); other.stop(0); }
    }
}
