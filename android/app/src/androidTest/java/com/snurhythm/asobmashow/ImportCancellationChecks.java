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
import java.io.FileNotFoundException;
import java.io.IOException;
import java.util.concurrent.*;

final class ImportCancellationChecks {
    private static void require(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }
    private static final class BlockingProvider extends ContentProvider {
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
    static void run(Context context) throws Exception {
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
