package com.snurhythm.asobmashow;

import android.app.*;
import android.content.*;
import android.net.Uri;
import android.os.*;
import android.view.*;
import android.view.inputmethod.EditorInfo;
import android.webkit.*;
import android.widget.*;
import org.json.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

public final class BmsBrowserActivity extends Activity {
    private static boolean webStorageConfigured;
    private static final String GINGER = "https://gingerrush.com/";
    private static final String MANBOW = "https://manbow.nothing.sh/event/event.cgi";
    private final ArrayDeque<WebView> pages = new ArrayDeque<>();
    private LinearLayout layout;
    private EditText address;
    private Button importTable;
    private File lastPage;
    private String tableUrl = "", tablePageUrl;
    private long tableProbe;

    private static boolean http(String url) {
        if (url == null || url.length() > 16384) return false;
        Uri uri = Uri.parse(url);
        return ("https".equals(uri.getScheme()) || "http".equals(uri.getScheme())) &&
                uri.getHost() != null && uri.getUserInfo() == null;
    }
    private WebView page() { return pages.peekLast(); }
    private Button button(LinearLayout row, String label, Runnable action) {
        Button button = new Button(this); button.setText(label); button.setAllCaps(false);
        button.setOnClickListener(view -> action.run()); row.addView(button); return button;
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        BmsDownloads.initialize(this);
        synchronized (BmsBrowserActivity.class) {
            if (!webStorageConfigured) {
                WebView.setDataDirectorySuffix("bms_browser"); webStorageConfigured = true;
            }
        }
        lastPage = new File(getNoBackupFilesDir(), "bms-browser-url");
        layout = new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL);
        layout.setFitsSystemWindows(true);
        HorizontalScrollView controls = new HorizontalScrollView(this);
        controls.setHorizontalScrollBarEnabled(false);
        LinearLayout row = new LinearLayout(this);
        button(row, "Close", this::finish);
        button(row, "Back", this::back);
        button(row, "Forward", () -> { if (page().canGoForward()) page().goForward(); });
        button(row, "Reload", () -> page().reload());
        button(row, "GingerRush", () -> load(GINGER));
        button(row, "Manbow", () -> load(MANBOW));
        importTable = button(row, "Import table", this::importCurrentTable);
        button(row, "Downloads", () -> { nativeShowDownloads(); finish(); });
        button(row, "Open externally", () -> external(page().getUrl()));
        controls.addView(row); layout.addView(controls);
        address = new EditText(this); address.setSingleLine(true);
        address.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_URI);
        address.setImeOptions(EditorInfo.IME_ACTION_GO);
        address.setTextSize(14);
        address.setOnEditorActionListener((view, action, event) -> {
            boolean enter = event != null && event.getKeyCode() == KeyEvent.KEYCODE_ENTER;
            if (action != EditorInfo.IME_ACTION_GO && !enter) return false;
            if (enter && event.getAction() != KeyEvent.ACTION_DOWN) return true;
            String url = address.getText().toString().trim();
            if (!url.contains("://")) url = "https://" + url;
            load(url); return true;
        });
        layout.addView(address);
        setContentView(layout);
        attach(newPage());
        String initial = GINGER;
        if (lastPage.isFile() && lastPage.length() <= 16384) {
            try { initial = new String(java.nio.file.Files.readAllBytes(lastPage.toPath()), StandardCharsets.UTF_8); }
            catch (IOException ignored) {}
        }
        load(http(initial) ? initial : GINGER);
    }
    private void attach(WebView web) {
        if (page() != null) { layout.removeView(page()); page().onPause(); }
        pages.add(web);
        clearTable();
        layout.addView(web, new LinearLayout.LayoutParams(-1, 0, 1));
    }
    private void load(String url) {
        if (!http(url)) { Toast.makeText(this, "Use an HTTP or HTTPS page.", Toast.LENGTH_SHORT).show(); return; }
        clearTable();
        page().loadUrl(url);
    }
    private WebView newPage() {
        WebView web = new WebView(this);
        WebSettings settings = web.getSettings();
        settings.setJavaScriptEnabled(true); settings.setDomStorageEnabled(true);
        settings.setAllowFileAccess(false); settings.setAllowContentAccess(false);
        settings.setMixedContentMode(WebSettings.MIXED_CONTENT_NEVER_ALLOW);
        settings.setSupportMultipleWindows(true); settings.setJavaScriptCanOpenWindowsAutomatically(false);
        CookieManager.getInstance().setAcceptThirdPartyCookies(web, false);
        // Record the method from WebView's actual request, not from its filename.
        final BmsBrowserRequestPolicy requests = new BmsBrowserRequestPolicy();
        final boolean[] loading = {false};
        web.setWebViewClient(new WebViewClient() {
            @Override public WebResourceResponse shouldInterceptRequest(WebView view, WebResourceRequest request) {
                if (request.isForMainFrame()) requests.started(request.getMethod());
                return null;
            }
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                if (http(request.getUrl().toString())) return false;
                if (request.isForMainFrame()) unsupported(view.getUrl());
                return true;
            }
            @Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap favicon) {
                loading[0] = true;
                if (view == page()) clearTable();
            }
            @Override public void doUpdateVisitedHistory(WebView view, String url, boolean reload) {
                updatePage(view, url, !loading[0]);
            }
            @Override public void onPageFinished(WebView view, String url) {
                if (!Objects.equals(url, view.getUrl())) return;
                loading[0] = false;
                updatePage(view, url, true);
            }
            private void updatePage(WebView view, String url, boolean inspectDocument) {
                if (view != page() || !Objects.equals(url, view.getUrl())) return;
                address.setText(url); refreshTable(view, inspectDocument);
                if (http(url)) {
                    try {
                        android.util.AtomicFile file = new android.util.AtomicFile(lastPage);
                        FileOutputStream output = file.startWrite();
                        try { output.write(url.getBytes(StandardCharsets.UTF_8)); file.finishWrite(output); }
                        catch (IOException error) { file.failWrite(output); }
                    } catch (IOException ignored) {}
                }
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override public boolean onCreateWindow(WebView view, boolean dialog, boolean userGesture, Message result) {
                if (!userGesture || pages.size() >= 4) return false;
                WebView child = newPage(); attach(child);
                ((WebView.WebViewTransport) result.obj).setWebView(child); result.sendToTarget(); return true;
            }
            @Override public void onCloseWindow(WebView window) { if (page() == window) closePopup(); }
        });
        web.setDownloadListener((url, userAgent, disposition, mime, length) -> {
            if (!http(url) || !requests.replayable()) { unsupported(web.getUrl()); return; }
            String filename = URLUtil.guessFileName(url, disposition, mime);
            String referrer = web.getUrl();
            new AlertDialog.Builder(this).setTitle("Download BMS archive?")
                    .setMessage(filename + "\n\nThe download can continue in the background. Import finishes when the app is open. You can cancel in Downloads or the notification.")
                    .setNegativeButton("Cancel", null).setPositiveButton("Download", (dialog, which) -> {
                String cookie = CookieManager.getInstance().getCookie(url);
                String error = nativeDownload(url, filename, userAgent == null ? "" : userAgent,
                        cookie == null ? "" : cookie, http(referrer) ? referrer : "");
                if (error.isEmpty()) Toast.makeText(this, "Added to Downloads", Toast.LENGTH_LONG).show();
                else new AlertDialog.Builder(this).setMessage(error).setPositiveButton("OK", null).show();
            }).show();
        });
        return web;
    }
    private void unsupported(String source) {
        new AlertDialog.Builder(this).setTitle("Open this download externally")
                .setMessage("This request cannot be repeated safely by the download service. Open the source page in your browser and choose the download there.")
                .setNegativeButton("Cancel", null).setPositiveButton("Open externally", (dialog, which) -> external(source)).show();
    }
    private void external(String url) {
        if (!http(url)) return;
        try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url))); }
        catch (ActivityNotFoundException error) { Toast.makeText(this, "No browser is available.", Toast.LENGTH_LONG).show(); }
    }
    private void clearTable() {
        ++tableProbe; tableUrl = ""; tablePageUrl = null; importTable.setEnabled(false);
    }
    private void refreshTable(WebView web, boolean inspectDocument) {
        clearTable();
        String url = web.getUrl();
        if (!http(url)) return;
        tablePageUrl = url;
        tableUrl = nativeTableUrl(url, "");
        importTable.setEnabled(!tableUrl.isEmpty());
        if (!inspectDocument) return;
        long probe = tableProbe;
        web.evaluateJavascript(nativeTableDetectionScript(), result -> {
            if (probe != tableProbe || web != page() || !Objects.equals(url, web.getUrl())) return;
            try {
                JSONObject detected = new JSONObject(result);
                if (!url.equals(detected.optString("pageUrl"))) return;
                tableUrl = nativeTableUrl(url, detected.optString("tableUrl", ""));
                importTable.setEnabled(!tableUrl.isEmpty());
            } catch (JSONException ignored) {}
        });
    }
    private void importCurrentTable() {
        String url = tableUrl;
        if (url.isEmpty() || !Objects.equals(tablePageUrl, page().getUrl())) return;
        new AlertDialog.Builder(this).setTitle("Import this table?")
                .setMessage("Adds or updates the selected difficulty table. Its charts will not be downloaded.")
                .setNegativeButton("Cancel", null).setPositiveButton("Import table", (dialog, which) -> {
                    if (nativeImportTable(url)) finish();
                }).show();
    }
    private boolean closePopup() {
        if (pages.size() < 2) return false;
        WebView child = pages.removeLast(); layout.removeView(child); child.destroy();
        layout.addView(page(), new LinearLayout.LayoutParams(-1, 0, 1)); page().onResume();
        address.setText(page().getUrl()); refreshTable(page(), page().getProgress() == 100);
        return true;
    }
    private void back() { if (page().canGoBack()) page().goBack(); else if (!closePopup()) finish(); }
    @Override public void onBackPressed() { back(); }
    @Override protected void onResume() {
        super.onResume(); if (page() != null) page().onResume(); nativeForeground(true);
    }
    @Override protected void onPause() {
        nativeForeground(false); if (page() != null) page().onPause(); super.onPause();
    }
    @Override protected void onDestroy() {
        clearTable();
        for (WebView web : pages) { layout.removeView(web); web.destroy(); }
        pages.clear(); super.onDestroy();
    }
    private static native String nativeTableUrl(String url, String detected);
    private static native String nativeTableDetectionScript();
    private static native boolean nativeImportTable(String url);
    private static native String nativeDownload(String url, String filename, String agent, String cookie, String referrer);
    private static native void nativeForeground(boolean foreground);
    private static native void nativeShowDownloads();
}
