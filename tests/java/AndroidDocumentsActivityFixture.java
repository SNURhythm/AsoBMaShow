package com.snurhythm.asobmashow;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.SystemClock;
import android.provider.DocumentsContract;
import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

public final class AndroidDocumentsActivityFixture extends Context {
    private static final String SUCCESS_RESULT = "__OK__", ERROR_PREFIX = "__ERROR__:";
    private final boolean externalAvailable;
    private Intent launched;

    AndroidDocumentsActivityFixture(File root, boolean externalAvailable) {
        super(root);
        this.externalAvailable = externalAvailable;
    }
    @Override public File getExternalFilesDir(String type) {
        return externalAvailable ? super.getExternalFilesDir(type) : null;
    }
    public android.content.pm.PackageManager getPackageManager() {
        return new android.content.pm.PackageManager();
    }
    public void startActivity(Intent intent) { launched = intent; }
    private String storagePathForTree(Uri uri) { return ""; }
    private String messageForException(Exception error, String fallback) { return error.getMessage(); }

    // PRODUCTION_METHODS

    private static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    public static void main(String[] args) throws Exception {
        Path base = Files.createTempDirectory("activity-documents-").toRealPath();
        try {
            Path realFiles = Files.createDirectories(base.resolve("real/package/files"));
            Files.createSymbolicLink(base.resolve("alias"), base.resolve("real"));
            AndroidDocumentsActivityFixture activity = new AndroidDocumentsActivityFixture(
                    base.resolve("alias/package/files").toFile(), args[0].equals("external"));
            DocumentsPathPolicy policy = AsoBMaShowDocumentsProvider.initializeDocuments(activity);
            if (args[0].equals("symlink")) {
                Path outside = Files.createDirectory(base.resolve("outside"));
                Files.createSymbolicLink(realFiles.resolve("Documents/BMS"), outside);
                check(activity.openDocumentsFolder().startsWith(ERROR_PREFIX),
                        "Open in Files must reject a symlinked BMS directory");
                try {
                    activity.documentsBmsDirectory();
                    throw new AssertionError("Import helper accepted a symlinked BMS directory");
                } catch (Exception expected) {
                    check(expected instanceof IOException, "BMS symlink rejection must be an I/O error");
                }
                try (var children = Files.list(outside)) {
                    check(children.findAny().isEmpty(), "Rejected BMS symlink changed external storage");
                }
                return;
            }
            String opened = activity.openDocumentsFolder();
            check(SUCCESS_RESULT.equals(opened), "Open in Files failed: " + opened);
            check(activity.launched != null && "documents:BMS".equals(activity.launched.data.documentId),
                    "Open in Files must launch the BMS document ID");
            File bms = activity.documentsBmsDirectory();
            check(bms.equals(realFiles.resolve("Documents/BMS").toFile()),
                    "Activity import destination must use the policy's canonical root");
            for (boolean move : new boolean[]{false, true}) {
                File output = new File(bms, move ? "Move" : "Copy");
                ChartFolderImport.Result result = activity.copyTreeUriToBmsFolder(new Uri(), output, move,
                        new ChartImportCopyControl(() -> 1, () -> false), (f, t, b, tb, n, p) -> {});
                check(result.complete, "Folder import failed: " + result.error);
                check(Arrays.equals(Files.readAllBytes(output.toPath().resolve("chart.bms")),
                                SafChartFolderSource.CONTENT), "Imported bytes differ");
                check(SafChartFolderSource.deleted == move, "Source deletion must follow Copy/Move selection");
                check(policy.documentId(output).equals("documents:BMS/" + (move ? "Move" : "Copy")),
                        "Imported output is not owned by the canonical Documents root");
            }
        } finally {
            try (var paths = Files.walk(base)) {
                for (Path path : paths.sorted(Comparator.reverseOrder()).toList()) Files.delete(path);
            }
        }
    }
}
