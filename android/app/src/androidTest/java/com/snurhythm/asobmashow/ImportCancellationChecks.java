package com.snurhythm.asobmashow;

import android.content.ContentProvider;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.pm.ProviderInfo;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileNotFoundException;
import java.nio.file.Files;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.io.IOException;
import java.util.concurrent.*;

final class ImportCancellationChecks {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static class BlockingProvider extends ContentProvider {
        final CountDownLatch opened = new CountDownLatch(2);
        final CountDownLatch cancelled = new CountDownLatch(2);
        final CountDownLatch cleanup = new CountDownLatch(1);
        public boolean onCreate() { return true; }
        public String getType(Uri uri) { return "application/octet-stream"; }
        public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
        public int update(Uri uri, ContentValues values, String where, String[] args) { throw new UnsupportedOperationException(); }
        public int delete(Uri uri, String where, String[] args) { throw new UnsupportedOperationException(); }
        public Cursor query(Uri uri, String[] columns, String where, String[] args, String order) {
            MatrixCursor cursor = new MatrixCursor(columns);
            Object[] values = new Object[columns.length];
            for (int i = 0; i < columns.length; ++i) {
                if (columns[i].equals(DocumentsContract.Document.COLUMN_DOCUMENT_ID)) values[i] = "fixture";
                else if (columns[i].equals(DocumentsContract.Document.COLUMN_DISPLAY_NAME)) values[i] = "fixture";
                else if (columns[i].equals(DocumentsContract.Document.COLUMN_MIME_TYPE)) values[i] = DocumentsContract.Document.MIME_TYPE_DIR;
                else values[i] = 1L;
            }
            cursor.addRow(values);
            return cursor;
        }
        @Override public ParcelFileDescriptor openFile(Uri uri, String mode, CancellationSignal signal)
                throws FileNotFoundException {
            require(signal != null, "Provider open needs cancellation");
            signal.setOnCancelListener(cancelled::countDown);
            opened.countDown();
            try {
                while (!signal.isCanceled() && !cleanup.await(10, TimeUnit.MILLISECONDS)) { }
            } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
            signal.throwIfCanceled();
            throw new FileNotFoundException("Test cleanup");
        }
    }
    private static void blockedChartQuery(Context context, int blockedQuery) throws Exception {
        CountDownLatch queried = new CountDownLatch(1);
        CountDownLatch queryCancelled = new CountDownLatch(1);
        CountDownLatch queryCleanup = new CountDownLatch(1);
        AtomicInteger queryCount = new AtomicInteger();
        BlockingProvider provider = new BlockingProvider() {
            @Override public Cursor query(Uri uri, String[] columns, String where, String[] args,
                                          String order, CancellationSignal signal) {
                if (queryCount.incrementAndGet() == blockedQuery) {
                    require(signal != null, "Blocked query needs its own cancellation signal");
                    signal.setOnCancelListener(queryCancelled::countDown);
                    queried.countDown();
                    while (!signal.isCanceled() && queryCleanup.getCount() > 0) {
                        java.util.concurrent.locks.LockSupport.parkNanos(TimeUnit.MILLISECONDS.toNanos(5));
                    }
                    signal.throwIfCanceled();
                }
                if (uri.getPath().endsWith("/children")) return new MatrixCursor(columns);
                return super.query(uri, columns, where, args, order);
            }
        };
        ProviderInfo info = new ProviderInfo();
        info.authority = "import-query-cancellation-fixture";
        provider.attachInfo(context, info);
        AtomicInteger state = new AtomicInteger(1);
        ChartImportCopyControl control = new ChartImportCopyControl(state::get, () -> false);
        SafChartFolderSource source = new SafChartFolderSource(ContentResolver.wrap(provider),
                DocumentsContract.buildTreeDocumentUri(info.authority, "fixture"), control);
        File staging = Files.createTempDirectory(context.getCacheDir().toPath(), "cancel-query-").toFile();
        File output = new File(staging, "copy");
        AtomicReference<ChartFolderImport.Result> result = new AtomicReference<>();
        Thread coordinator = new Thread(() -> result.set(ChartFolderImport.run(source, output, true, control,
                (files, total, bytes, totalBytes, name, phase) -> {}, () -> {})));
        coordinator.start();
        try {
            require(queried.await(3, TimeUnit.SECONDS), "Chart query never blocked: " + blockedQuery);
            state.set(-1);
            require(queryCancelled.await(2, TimeUnit.SECONDS), "Native cancellation missed blocked chart query: " + blockedQuery);
            coordinator.join(3000);
            require(!coordinator.isAlive() && result.get() != null && !result.get().complete && !output.exists(),
                    "Blocked chart cancellation failed to drain and remove staging");
        } finally {
            queryCleanup.countDown();
            state.set(-1);
            coordinator.interrupt();
            coordinator.join(3000);
            require(!coordinator.isAlive(), "Chart query did not drain before cleanup");
            SkinDirectoryImport.removeTree(staging.toPath());
        }
    }

    private static void directoryDurability(Context context) throws Exception {
        File documents = AsoBMaShowDocumentsProvider.documentsDirectory(context);
        require(documents.isDirectory() || documents.mkdirs(), "Cannot initialize Documents for durability check");
        for (File base : new File[]{context.getFilesDir(), documents}) {
            File staging = Files.createTempDirectory(base.toPath(), "directory-durability-").toFile();
            try {
                File nested = new File(staging, "empty");
                require(nested.mkdir(), "Cannot create empty durability fixture");
                try (FileOutputStream stream = new FileOutputStream(new File(staging, "file"))) {
                    stream.write(new byte[]{1, 2, 3});
                    stream.getFD().sync();
                }
                ChartFolderImport.syncDirectory(nested);
                ChartFolderImport.syncDirectory(staging);
                ChartFolderImport.syncDirectory(base);
                if (base.equals(documents)) ChartFolderImport.syncDirectory(base.getParentFile());
            } finally { SkinDirectoryImport.removeTree(staging.toPath()); }
        }
    }

    static void run(Context context) throws Exception {
        directoryDurability(context);
        for (int query = 1; query <= 3; query++) blockedChartQuery(context, query);
        for (boolean skin : new boolean[]{true, false}) {
            BlockingProvider provider = new BlockingProvider();
            ProviderInfo info = new ProviderInfo();
            info.authority = "import-cancellation-fixture";
            provider.attachInfo(context, info);
            ContentResolver resolver = ContentResolver.wrap(provider);
            Uri tree = DocumentsContract.buildTreeDocumentUri(info.authority, "fixture");
            CancellationSignal userCancellation = new CancellationSignal();
            SafSkinDirectorySource skinSource = new SafSkinDirectorySource(resolver, tree, userCancellation,
                    new SafSkinDirectorySource.IoControl() {
                        public void checkpoint() { }
                        public void descriptor(ParcelFileDescriptor descriptor) { }
                    });
            SafChartFolderSource chartSource = new SafChartFolderSource(resolver, tree,
                    new ChartImportCopyControl(() -> 1, () -> false));
            ExecutorService threads = Executors.newFixedThreadPool(2);
            try {
                for (int i = 0; i < 2; ++i) threads.submit(() -> {
                    try {
                        if (skin) skinSource.open(new SkinDirectoryImport.Entry("file", "file", false, 1));
                        else chartSource.open(new ChartFolderImport.Entry("file", "file", false, 1, 1));
                    } catch (IOException | RuntimeException expected) { }
                });
                require(provider.opened.await(3, TimeUnit.SECONDS), "Parallel provider opens never started");
                // A completed query must not detach either pending open's remote signal.
                if (skin) { skinSource.root(); skinSource.cancel(); }
                else { chartSource.root(); chartSource.cancel(); }
                require(provider.cancelled.await(2, TimeUnit.SECONDS), "Cancellation missed a blocked provider open");
                require(!userCancellation.isCanceled(), "Internal failure cleanup became user cancellation");
                try {
                    if (skin) skinSource.open(new SkinDirectoryImport.Entry("late", "late", false, 1));
                    else chartSource.open(new ChartFolderImport.Entry("late", "late", false, 1, 1));
                    throw new AssertionError("Accepted a provider call after cancellation");
                } catch (android.os.OperationCanceledException expected) { }
            } finally {
                provider.cleanup.countDown();
                threads.shutdownNow();
                require(threads.awaitTermination(3, TimeUnit.SECONDS), "Provider readers did not drain");
            }
        }
        CancellationSignal user = new CancellationSignal();
        SafImportCancellation group = new SafImportCancellation();
        user.setOnCancelListener(group::cancel);
        try (SafImportCancellation.Operation first = group.begin();
             SafImportCancellation.Operation second = group.begin()) {
            user.cancel();
            require(first.signal.isCanceled() && second.signal.isCanceled(), "User cancellation did not fan out");
        }
    }
}
