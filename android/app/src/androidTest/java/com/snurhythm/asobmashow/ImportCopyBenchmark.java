package com.snurhythm.asobmashow;

import android.app.Instrumentation;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;
import java.io.*;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.concurrent.*;

/** Explicit, read-only-source device benchmark; all outputs are owned temporary files. */
final class ImportCopyBenchmark {
    // Instrumentation substitutes this Activity for the SDL Activity. The copy
    // benchmark gets a real picker grant without starting a competing renderer.
    static final class PickerActivity extends Activity {
        final CountDownLatch selected = new CountDownLatch(1);
        Uri tree;
        @Override protected void onCreate(Bundle state) {
            super.onCreate(state);
            Intent picker = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE)
                    .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            picker.putExtra(DocumentsContract.EXTRA_INITIAL_URI,
                    (Uri)getIntent().getParcelableExtra("benchmark-tree"));
            startActivityForResult(picker, 1);
        }
        @Override protected void onActivityResult(int request, int result, Intent data) {
            super.onActivityResult(request, result, data);
            if (result == RESULT_OK && data != null) tree = data.getData();
            selected.countDown();
        }
    }
    private static final class Item {
        final SkinDirectoryImport.Entry entry;
        final String path;
        byte[] digest;
        Item(SkinDirectoryImport.Entry entry, String path) { this.entry = entry; this.path = path; }
    }
    static void run(Context context, Instrumentation instrumentation, Bundle arguments) throws Exception {
        Uri tree = DocumentsContract.buildTreeDocumentUri("com.android.externalstorage.documents",
                arguments.getString("source", "primary:Download/Skins/simple-play-simple"));
        Path base = Files.createTempDirectory(context.getCacheDir().toPath(), "import-copy-benchmark-");
        try {
            // Use the ordinary picker grant, exactly as a user import does.
            PickerActivity activity = (PickerActivity)instrumentation.startActivitySync(
                    new Intent(context, AsoBMaShowActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                            .putExtra("benchmark-tree", tree));
            if (!activity.selected.await(120, TimeUnit.SECONDS) || !tree.equals(activity.tree))
                throw new IOException("Select the requested benchmark source folder.");
            instrumentation.runOnMainSync(() -> activity.moveTaskToBack(true));
            SafSkinDirectorySource source = new SafSkinDirectorySource(context.getContentResolver(), tree,
                    new CancellationSignal(), new SafSkinDirectorySource.IoControl() {
                        public void checkpoint() { }
                        public void descriptor(ParcelFileDescriptor descriptor) { }
                    });
            List<Item> items = new ArrayList<>();
            long start = System.nanoTime();
            enumerate(source, source.root(), "", items, new HashSet<>());
            long listingNanos = System.nanoTime() - start;
            long bytes = 0;
            for (Item item : items) {
                bytes += item.entry.size;
                try (InputStream input = source.open(item.entry)) { item.digest = digest(input); }
            }
            Log.i("ImportBench", "files=" + items.size() + " bytes=" + bytes + " listing_ms=" + ms(listingNanos)
                    + " processors=" + Runtime.getRuntime().availableProcessors());
            SkinDirectoryImport.Limits limits = new SkinDirectoryImport.Limits(
                    8L << 30, 20000, 256, 4096, 8L << 30);
            Path output = base.resolve("skin");
            start = System.nanoTime();
            SkinDirectoryImport.copy(source, output, limits);
            Log.i("ImportBench", "engine=skin ms=" + ms(System.nanoTime() - start));
            verify(output, items);
            SkinDirectoryImport.removeTree(output);
            output = base.resolve("chart");
            ChartImportCopyControl control = new ChartImportCopyControl(() -> 1, () -> false);
            start = System.nanoTime();
            ChartFolderImport.Result result = ChartFolderImport.run(
                    new SafChartFolderSource(context.getContentResolver(), tree, control), output.toFile(),
                    false, control, (files, totalFiles, copied, total, name, phase) -> {}, () -> {});
            if (!result.complete) throw new IOException(result.error);
            Log.i("ImportBench", "engine=chart ms=" + ms(System.nanoTime() - start));
            verify(output, items);
            SkinDirectoryImport.removeTree(output);
            for (int bufferSize : new int[]{1024 * 1024}) {
                for (int workers : new int[]{4, 8, 8, 4}) {
                    output = Files.createDirectory(base.resolve("probe"));
                    for (Item item : items) Files.createDirectories(output.resolve(item.path).getParent());
                    ExecutorService pool = Executors.newFixedThreadPool(workers);
                    ThreadLocal<byte[]> buffers = ThreadLocal.withInitial(() -> new byte[bufferSize]);
                    List<Future<?>> futures = new ArrayList<>();
                    final Path destination = output;
                    start = System.nanoTime();
                    try {
                        for (Item item : items) futures.add(pool.submit(() -> {
                            try (InputStream input = source.open(item.entry);
                                 OutputStream out = Files.newOutputStream(destination.resolve(item.path),
                                         StandardOpenOption.CREATE_NEW)) {
                                byte[] buffer = buffers.get();
                                int count;
                                while ((count = input.read(buffer)) >= 0) {
                                    if (count > 0) out.write(buffer, 0, count);
                                }
                            } catch (IOException error) { throw new UncheckedIOException(error); }
                        }));
                        for (Future<?> future : futures) future.get();
                        Log.i("ImportBench", "workers=" + workers + " buffer=" + bufferSize
                                + " ms=" + ms(System.nanoTime() - start));
                    } finally {
                        pool.shutdownNow();
                        if (!pool.awaitTermination(30, TimeUnit.SECONDS))
                            throw new IOException("Copy workers did not finish");
                    }
                    verify(output, items);
                    SkinDirectoryImport.removeTree(output);
                }
            }
        } finally {
            SkinDirectoryImport.removeTree(base);
        }
    }
    private static void enumerate(SkinDirectoryImport.Source source, SkinDirectoryImport.Entry directory,
                                  String prefix, List<Item> items, Set<String> seen) throws IOException {
        if (!seen.add(directory.id) || seen.size() > 20000) throw new IOException("Unbounded fixture");
        for (SkinDirectoryImport.Entry entry : source.children(directory, 20000 - seen.size())) {
            if (entry.name.contains("/") || entry.name.equals("..")) throw new IOException("Unsafe fixture");
            String path = prefix + entry.name;
            if (entry.directory) enumerate(source, entry, path + "/", items, seen);
            else {
                if (!seen.add(entry.id) || seen.size() > 20000) throw new IOException("Unbounded fixture");
                items.add(new Item(entry, path));
            }
        }
    }
    private static byte[] digest(InputStream input) throws Exception {
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        byte[] buffer = new byte[64 * 1024];
        int count;
        while ((count = input.read(buffer)) >= 0) if (count > 0) digest.update(buffer, 0, count);
        return digest.digest();
    }
    private static void verify(Path output, List<Item> items) throws Exception {
        for (Item item : items) try (InputStream input = Files.newInputStream(output.resolve(item.path))) {
            if (!Arrays.equals(item.digest, digest(input))) throw new IOException("Copy mismatch: " + item.path);
        }
    }
    private static long ms(long nanos) { return TimeUnit.NANOSECONDS.toMillis(nanos); }
}
