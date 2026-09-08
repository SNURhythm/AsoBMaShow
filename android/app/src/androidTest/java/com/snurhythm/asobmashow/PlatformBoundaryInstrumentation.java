package com.snurhythm.asobmashow;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.security.NetworkSecurityPolicy;
import android.system.Os;
import android.util.Base64;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.net.HttpURLConnection;
import java.security.KeyStore;
import java.security.cert.CertificateFactory;
import java.util.concurrent.atomic.AtomicReference;
import javax.net.ssl.HttpsURLConnection;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLSocketFactory;
import javax.net.ssl.TrustManagerFactory;

public final class PlatformBoundaryInstrumentation extends Instrumentation {
    private Bundle arguments;

    @Override
    public void onCreate(Bundle arguments) {
        super.onCreate(arguments);
        this.arguments = arguments;
        start();
    }

    @Override
    public void onStart() {
        Bundle result = new Bundle();
        SSLSocketFactory originalFactory = HttpsURLConnection.getDefaultSSLSocketFactory();
        try {
            File cacheDirectory = getTargetContext().getCacheDir();
            require(cacheDirectory.getAbsolutePath().equals(Os.getenv("SQLITE_TMPDIR")),
                    "Target application did not configure SQLite private temporary storage");
            File temporary = File.createTempFile("sqlite-boundary-", ".tmp", cacheDirectory);
            require(temporary.delete(), "Could not remove owned SQLite storage probe");
            if ("seed-saf".equals(arguments.getString("mode"))) {
                ContentValues values = new ContentValues();
                values.put(MediaStore.MediaColumns.DISPLAY_NAME, "smoke.bms");
                values.put(MediaStore.MediaColumns.MIME_TYPE, "application/octet-stream");
                values.put(MediaStore.MediaColumns.RELATIVE_PATH, "Download/AsoBMaShowTask5Saf");
                Uri uri = getTargetContext().getContentResolver().insert(
                        MediaStore.Downloads.EXTERNAL_CONTENT_URI, values);
                require(uri != null, "Could not seed the isolated test user's chart");
                try (OutputStream output = getTargetContext().getContentResolver().openOutputStream(uri)) {
                    output.write("#PLAYER 1\n#TITLE Task5 SAF Smoke\n#BPM 120\n#RANK 2\n#00111:01\n".getBytes("UTF-8"));
                }
                result.putString("result", "PASS seeded SAF chart");
                finish(Activity.RESULT_OK, result);
                return;
            }
            if ("saf".equals(arguments.getString("mode"))) {
                verifyPersistedTree();
                result.putString("result", "PASS persisted SAF indexing and read");
                finish(Activity.RESULT_OK, result);
                return;
            }
            AtomicReference<AsoBMaShowActivity> activity = new AtomicReference<>();
            runOnMainSync(() -> activity.set(new AsoBMaShowActivity()));
            Method open = AsoBMaShowActivity.class.getDeclaredMethod(
                    "openHttpConnection", String.class, String.class, int.class);
            open.setAccessible(true);
            String http = "http://localhost:" + arguments.getString("httpPort");
            String https = "https://localhost:" + arguments.getString("httpsPort");
            require("table-fixture".equals(read(open, activity.get(), http + "/table.json")),
                    "Legacy HTTP table retrieval failed");
            require(NetworkSecurityPolicy.getInstance().isCleartextTrafficPermitted("localhost"),
                    "Assembled target policy rejects public-content HTTP");
            require("archive-fixture".equals(read(open, activity.get(), http + "/chart.zip")),
                    "Legacy HTTP archive retrieval failed");
            configureFixtureTrust(arguments.getString("fixtureCa"));
            require("table-fixture".equals(read(open, activity.get(), https + "/table.json")),
                    "HTTPS control failed");
            require("archive-fixture".equals(read(open, activity.get(), http + "/upgrade")),
                    "HTTP to HTTPS upgrade failed");
            try {
                read(open, activity.get(), https + "/downgrade");
                throw new AssertionError("HTTPS to HTTP downgrade was allowed");
            } catch (IOException expected) {
                require(expected.getMessage().contains("insecure HTTP"),
                        "Downgrade must fail at application redirect policy, not a network setup error");
            }
            Method direct = AsoBMaShowActivity.class.getDeclaredMethod("directPathForTree", Uri.class);
            direct.setAccessible(true);
            if (Build.VERSION.SDK_INT < 30 || !BuildConfig.ASOBMSHOW_MANAGE_EXTERNAL_STORAGE) {
                require("".equals(direct.invoke(activity.get(), Uri.parse(
                                "content://com.android.externalstorage.documents/tree/primary%3ACharts"))),
                        "SAF-only target replaced its URI grant with an unauthorized raw path");
            }
            result.putString("result", "PASS platform boundaries " + BuildConfig.FLAVOR);
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            result.putString("result", "FAIL " + error.toString());
            finish(Activity.RESULT_CANCELED, result);
        } finally {
            HttpsURLConnection.setDefaultSSLSocketFactory(originalFactory);
        }
    }

    private static String read(Method open, AsoBMaShowActivity activity, String url) throws Exception {
        HttpURLConnection connection;
        try {
            connection = (HttpURLConnection) open.invoke(activity, url, "GET", 4);
        } catch (InvocationTargetException error) {
            if (error.getCause() instanceof Exception) throw (Exception) error.getCause();
            throw error;
        }
        try (InputStream input = connection.getInputStream()) {
            ByteArrayOutputStream output = new ByteArrayOutputStream();
            byte[] buffer = new byte[1024];
            int count;
            while ((count = input.read(buffer)) >= 0) output.write(buffer, 0, count);
            return output.toString("UTF-8");
        } finally {
            connection.disconnect();
        }
    }

    private static void configureFixtureTrust(String encodedCertificate) throws Exception {
        KeyStore store = KeyStore.getInstance(KeyStore.getDefaultType());
        store.load(null);
        store.setCertificateEntry("loopback-fixture", CertificateFactory.getInstance("X.509")
                .generateCertificate(new ByteArrayInputStream(Base64.decode(encodedCertificate, Base64.DEFAULT))));
        TrustManagerFactory factory = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
        factory.init(store);
        SSLContext context = SSLContext.getInstance("TLS");
        context.init(null, factory.getTrustManagers(), null);
        HttpsURLConnection.setDefaultSSLSocketFactory(context.getSocketFactory());
    }

    private static void require(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private void verifyPersistedTree() throws Exception {
        String tree = "content://com.android.externalstorage.documents/tree/primary%3ADownload%2FAsoBMaShowTask5Saf";
        AtomicReference<ContextActivity> activity = new AtomicReference<>();
        runOnMainSync(() -> activity.set(new ContextActivity(getTargetContext())));
        if ("1".equals(arguments.getString("deliverSelection"))) {
            Field requestsField = AsoBMaShowActivity.class.getDeclaredField("folderPickerRequests");
            requestsField.setAccessible(true);
            NativeFolderPickerRequests requests = (NativeFolderPickerRequests) requestsField.get(activity.get());
            NativeFolderPickerRequests.Request request = requests.begin(false, () -> false);
            Intent returned = new Intent();
            returned.setData(Uri.parse(tree));
            returned.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
            runOnMainSync(() -> activity.get().onActivityResult(request.code, Activity.RESULT_OK, returned));
            require((tree + "\nDownload_AsoBMaShowTask5Saf\n").equals(requests.await(request)),
                    "Actual activity handoff did not retain its authorized SAF root");
        }
        boolean persisted = getTargetContext().getContentResolver().getPersistedUriPermissions()
                .stream().anyMatch(permission -> permission.isReadPermission()
                        && tree.equals(permission.getUri().toString()));
        require(persisted, "The actual folder picker did not persist its read grant");
        String listed = activity.get().listChartFiles(tree, "@androidtree@/task5");
        require("@androidtree@/task5/smoke.bms\t0\n".equals(listed),
                "Actual tree indexing failed: " + listed);
        int descriptor = activity.get().openTreeFileDescriptor(tree, "smoke.bms");
        require(descriptor >= 0, "Actual tree file open failed");
        try (InputStream input = new ParcelFileDescriptor.AutoCloseInputStream(
                ParcelFileDescriptor.adoptFd(descriptor))) {
            byte[] bytes = new byte[1024];
            int count = input.read(bytes);
            require(count > 0 && new String(bytes, 0, count, "UTF-8").contains("#TITLE Task5 SAF Smoke"),
                    "SAF bridge did not return the selected chart bytes");
        }
    }

    private static final class ContextActivity extends AsoBMaShowActivity {
        ContextActivity(Context context) { attachBaseContext(context); }
    }
}
