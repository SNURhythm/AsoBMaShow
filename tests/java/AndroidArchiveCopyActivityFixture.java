package com.snurhythm.asobmashow;

import java.io.InputStream;
import java.io.IOException;
import java.io.ByteArrayInputStream;
import java.nio.file.Files;
import java.util.Arrays;

public final class AndroidArchiveCopyActivityFixture {
    public static void main(String[] arguments) throws Exception {
        File root = new File(arguments[0]);
        CopyActivity activity = new CopyActivity(root);
        File inbox = new File(root, "archive_imports/inbox");
        inbox.mkdirs();
        File preserved = new File(inbox, "chart.zip");
        Files.write(preserved.toPath(), new byte[] {9});
        for (String scenario : new String[] {"open", "null", "read", "write", "input-close",
                                             "output-close", "cancel", "space"}) {
            for (int attempt = 0; attempt < 3; ++attempt) {
                CopyActivity.scenario = scenario;
                FileOutputStream.written = 0;
                ChartImportCopyControl control = new ChartImportCopyControl(
                        () -> scenario.equals("cancel") && FileOutputStream.written > 0 ? -1 : 1,
                        () -> false);
                try {
                    activity.attempt(control);
                    throw new AssertionError("Expected owned-copy failure: " + scenario);
                } catch (IOException expected) {
                } catch (Exception expected) {
                    if (!scenario.equals("null")) throw expected;
                }
                File[] remaining = inbox.listFiles();
                if (remaining == null || remaining.length != 1
                        || !remaining[0].equals(preserved)
                        || !Arrays.equals(Files.readAllBytes(preserved.toPath()), new byte[] {9})) {
                    throw new AssertionError("Failed copy leaked data or removed unowned file: " + scenario);
                }
                if (!scenario.equals("open") && !scenario.equals("null")
                        && FileOutputStream.written == 0) {
                    throw new AssertionError("Fixture did not exercise partial-file cleanup: " + scenario);
                }
            }
            System.out.println("PASS owned archive cleanup " + scenario);
        }
        CopyActivity.scenario = "success";
        FileOutputStream.written = 0;
        String path = activity.attempt(new ChartImportCopyControl(() -> 1, () -> false));
        if (!Arrays.equals(Files.readAllBytes(new File(path).toPath()), CopyActivity.bytes)) {
            throw new AssertionError("Successful archive contents changed");
        }
        System.out.println("PASS owned archive success");
    }
}

class CopyActivity {
    static String scenario;
    static final byte[] bytes = new byte[2 * 1024 * 1024];
    final File root;
    CopyActivity(File root) { this.root = root; }
    File getFilesDir() { return root; }
    CopyResolver getContentResolver() { return new CopyResolver(); }
    String attempt(ChartImportCopyControl control) throws Exception {
        return copyArchiveUriToInternalStorage(new Uri(), "chart.zip", control);
    }
    ACTIVITY_METHODS
}

class CopyResolver {
    InputStream openInputStream(Uri uri) throws IOException {
        if (CopyActivity.scenario.equals("open")) throw new IOException("open fixture");
        if (CopyActivity.scenario.equals("null")) return null;
        return new InputStream() {
            final ByteArrayInputStream source = new ByteArrayInputStream(CopyActivity.bytes);
            public int read() throws IOException { return source.read(); }
            public int read(byte[] buffer) throws IOException {
                if (CopyActivity.scenario.equals("read") && FileOutputStream.written > 0) {
                    throw new IOException("read fixture");
                }
                return source.read(buffer);
            }
            public void close() throws IOException {
                source.close();
                if (CopyActivity.scenario.equals("input-close")) throw new IOException("input close fixture");
            }
        };
    }
}

class Uri {}

class File extends java.io.File {
    File(String path) { super(path); }
    File(java.io.File parent, String path) { super(parent, path); }
    public long getUsableSpace() {
        return CopyActivity.scenario.equals("space") && FileOutputStream.written > 0
                ? 256L * 1024 * 1024 : Long.MAX_VALUE;
    }
    public File[] listFiles() {
        java.io.File[] files = super.listFiles();
        if (files == null) return null;
        return Arrays.stream(files).map(file -> new File(file.getPath())).toArray(File[]::new);
    }
}

class FileOutputStream extends java.io.FileOutputStream {
    static long written;
    FileOutputStream(File output) throws IOException { super(output); }
    public void write(byte[] bytes, int offset, int count) throws IOException {
        if (CopyActivity.scenario.equals("write") && written > 0) {
            super.write(bytes, offset, 1);
            throw new IOException("write fixture");
        }
        super.write(bytes, offset, count);
        written += count;
    }
    public void close() throws IOException {
        super.close();
        if (CopyActivity.scenario.equals("output-close")) throw new IOException("output close fixture");
    }
}
