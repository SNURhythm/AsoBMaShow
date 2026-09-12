package com.snurhythm.asobmashow;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.net.URLConnection;
import java.net.URLStreamHandler;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;

class AsoBMaShowActivity {
    private static native boolean nativeDownloadUrlTextCheckpoint(long token);
    private static native void cancel();
    private static native int registeredBridges();
    private native String request(String url, boolean cancellable);
    private native String requestBridge(String url, boolean cancellable);
    private static final List<Connection> connections = new ArrayList<>();
    private static final CountDownLatch blocked = new CountDownLatch(1);
    private static String scenario;

    static class Looper {
        static Object myLooper() { return null; }
        static Object getMainLooper() { return Looper.class; }
    }

    ACTIVITY_METHODS

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    static class Connection extends HttpURLConnection {
        final CountDownLatch disconnected = new CountDownLatch(1);
        final CountDownLatch earlyDisconnect = new CountDownLatch(1);
        final AtomicBoolean engineStarted = new AtomicBoolean(false);
        final int number;

        Connection(URL url) {
            super(url);
            number = connections.size();
            connections.add(this);
        }

        @Override public void connect() { engineStarted.set(true); }
        @Override public boolean usingProxy() { return false; }
        @Override public void disconnect() {
            if (!engineStarted.get()) {
                earlyDisconnect.countDown();
                return;
            }
            disconnected.countDown();
        }

        private void stall(String phase) throws IOException {
            if (!scenario.equals(phase) &&
                    !(scenario.equals("redirect") && number == 1 && phase.equals("headers"))) return;
            blocked.countDown();
            try {
                if (!disconnected.await(5, TimeUnit.SECONDS)) {
                    throw new IOException("Fixture stalled connection timed out.");
                }
            } catch (InterruptedException exception) {
                throw new IOException(exception);
            }
            throw new IOException("Socket closed.");
        }

        @Override public OutputStream getOutputStream() throws IOException {
            require(getRequestMethod().equals("POST"), "POST method was not preserved");
            require(getDoOutput() && fixedContentLength == 0, "Expected explicit empty POST");
            if (scenario.equals("preconnect")) {
                blocked.countDown();
                try {
                    require(earlyDisconnect.await(2, TimeUnit.SECONDS),
                            "Cancellation did not attempt disconnect before engine initialization");
                } catch (InterruptedException exception) {
                    throw new IOException(exception);
                }
            }
            engineStarted.set(true);
            stall("preconnect");
            stall("output");
            return new ByteArrayOutputStream();
        }

        @Override public int getResponseCode() throws IOException {
            require(!getInstanceFollowRedirects(), "Automatic redirects bypass security checks");
            engineStarted.set(true);
            stall("headers");
            if (scenario.equals("io-error")) throw new IOException("Fixture network failure");
            if (scenario.equals("http-error")) return 503;
            if (scenario.equals("redirect-limit")) return 307;
            if (number == 0) {
                if (scenario.equals("redirect-302")) return 302;
                if (scenario.startsWith("redirect") || scenario.equals("downgrade")) return 307;
            }
            return 200;
        }

        @Override public String getHeaderField(String name) {
            require(name.equals("Location"), "Unexpected header lookup");
            if (scenario.equals("downgrade")) return "http://fixture/next";
            if (scenario.equals("redirect-scheme")) return "file:///tmp/metadata";
            return "/next";
        }

        @Override public InputStream getInputStream() {
            if (scenario.equals("body")) {
                return new InputStream() {
                    @Override public int read() throws IOException {
                        stall("body");
                        return -1;
                    }
                };
            }
            if (scenario.equals("too-large")) {
                return new ByteArrayInputStream(new byte[16 * 1024 * 1024 + 1]);
            }
            return new ByteArrayInputStream("{\"title\":\"곡🎵\"}".getBytes(StandardCharsets.UTF_8));
        }
    }

    public static void main(String[] args) throws Exception {
        System.load(args[0]);
        scenario = args[1];
        boolean directBridge = scenario.startsWith("bridge-");
        if (directBridge) scenario = scenario.substring("bridge-".length());
        URL.setURLStreamHandlerFactory(protocol -> {
            if (!protocol.equals("http") && !protocol.equals("https")) return null;
            return new URLStreamHandler() {
                @Override protected URLConnection openConnection(URL url) {
                    return new Connection(url);
                }
            };
        });
        AsoBMaShowActivity activity = new AsoBMaShowActivity();
        AtomicReference<String> result = new AtomicReference<>();
        AtomicReference<Throwable> failure = new AtomicReference<>();
        if (scenario.equals("pre-cancelled")) cancel();
        String url = scenario.equals("bad-scheme") ? "file:///tmp/metadata" : "https://fixture/start";
        Thread worker = new Thread(() -> {
            try {
                result.set(directBridge
                        ? activity.requestBridge(url, !scenario.equals("uncancellable"))
                        : activity.request(url, !scenario.equals("uncancellable")));
            } catch (Throwable exception) {
                failure.set(exception);
            }
        });
        worker.setDaemon(true);
        worker.start();
        boolean stalled = List.of("preconnect", "output", "headers", "body", "redirect").contains(scenario);
        if (stalled) {
            require(blocked.await(2, TimeUnit.SECONDS), "POST did not reach stalled " + scenario);
            cancel();
        }
        worker.join(1500);
        require(!worker.isAlive(), "Cancelled POST stayed blocked at " + scenario);
        require(failure.get() == null, "Worker threw " + failure.get());
        String response = result.get();
        if (stalled || scenario.equals("pre-cancelled")) {
            require("ERROR:Lookup cancelled.".equals(response), "Cancellation misclassified: " + response);
            if (stalled) require(connections.size() == (scenario.equals("redirect") ? 2 : 1),
                    "Cancelled POST was retried");
        } else if (scenario.equals("http-error")) {
            require(response.startsWith("ERROR:HTTP 503"), response);
        } else if (scenario.equals("io-error")) {
            require(response.equals("ERROR:Fixture network failure"), response);
        } else if (scenario.equals("too-large")) {
            require(response.contains("too large"), "Response bound was bypassed: " + response);
        } else if (scenario.equals("downgrade")) {
            require(response.contains("insecure HTTP") && connections.size() == 1, response);
        } else if (scenario.equals("bad-scheme") || scenario.equals("redirect-scheme")) {
            require(response.contains("must use HTTP or HTTPS"), response);
        } else if (scenario.equals("redirect-limit")) {
            require(response.contains("Too many redirects") && connections.size() == 9, response);
        } else {
            require(response.equals("{\"title\":\"곡🎵\"}"), "Unexpected response: " + response);
        }
        if (scenario.equals("pre-cancelled")) require(connections.isEmpty(), "Pre-cancelled POST opened a connection");
        if (scenario.equals("redirect-302") || scenario.equals("redirect-307")) {
            require(connections.size() == 2, "Redirect did not open the target");
            require(connections.get(1).getRequestMethod().equals(
                    scenario.equals("redirect-302") ? "GET" : "POST"), "Redirect method changed");
        }
        for (Connection connection : connections) {
            require(connection.disconnected.getCount() == 0, "Connection leaked at " + connection.number);
        }
        require(registeredBridges() == 0, "Native checkpoint bridge leaked");
        for (Thread thread : Thread.getAllStackTraces().keySet()) {
            require(!thread.getName().equals("MetadataPostCancellationMonitor") || !thread.isAlive(),
                    "Monitor outlived its native checkpoint bridge");
        }
        System.out.println("PASS " + args[1]);
    }
}
