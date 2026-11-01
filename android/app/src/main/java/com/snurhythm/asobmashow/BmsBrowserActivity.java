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
import java.util.*;

public final class BmsBrowserActivity extends Activity {
    private static boolean webStorageConfigured;
    private static final String GINGER = "https://gingerrush.com/";
    private static final String MANBOW = "https://manbow.nothing.sh/event/event.cgi";
    private final ArrayDeque<WebView> pages = new ArrayDeque<>();
    private LinearLayout layout;
    private SearchView address;
    private MenuItem importTable, backItem, forwardItem, reloadItem;
    private ProgressBar progress;
    private WebView loadingPage;
    // Resume within this process only; a new app launch starts at Manbow.
    private static String lastPage = MANBOW;
    private String tableUrl = "", tablePageUrl;
    private long tableProbe;

    private static boolean http(String url) {
        if (url == null || url.length() > 16384) return false;
        Uri uri = Uri.parse(url);
        return ("https".equals(uri.getScheme()) || "http".equals(uri.getScheme())) &&
                uri.getHost() != null && uri.getUserInfo() == null;
    }
    private WebView page() { return pages.peekLast(); }
    private MenuItem action(Menu menu, String label, Runnable run) {
        return menu.add(label).setOnMenuItemClickListener(item -> { run.run(); return true; });
    }
    private void updateControls() {
        WebView web = page(); if (web == null) return;
        backItem.setEnabled(web.canGoBack() || pages.size() > 1);
        forwardItem.setEnabled(web.canGoForward());
        reloadItem.setTitle(loadingPage == web ? "Stop loading" : "Reload");
        reloadItem.setIcon(loadingPage == web ? android.R.drawable.ic_menu_close_clear_cancel : R.drawable.ic_browser_reload);
        progress.setVisibility(loadingPage == web ? View.VISIBLE : View.INVISIBLE);
        progress.setProgress(web.getProgress());
        if (!address.hasFocus()) address.setQuery(web.getUrl(), false);
    }
    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        BmsDownloads.initialize(this);
        synchronized (BmsBrowserActivity.class) {
            if (!webStorageConfigured) {
                WebView.setDataDirectorySuffix("bms_browser"); webStorageConfigured = true;
            }
        }
        layout = new LinearLayout(this); layout.setOrientation(LinearLayout.VERTICAL);
        layout.setFitsSystemWindows(true);
        Toolbar controls = new Toolbar(this);
        controls.setNavigationIcon(android.R.drawable.ic_menu_close_clear_cancel);
        controls.setNavigationContentDescription("Close browser");
        controls.setNavigationOnClickListener(view -> finish());
        Menu menu = controls.getMenu();
        SubMenu sites = menu.addSubMenu("Sites");
        action(sites, "Digital Emergency Exit", () -> load(MANBOW));
        action(sites, "GingerRush", () -> load(GINGER));
        action(sites, "BMS Search", () -> load("https://bmssearch.net/"));
        action(sites, "horieyuuka.github.io", () -> load("https://horieyuuka.github.io/"));
        importTable = action(menu, "Import table", this::importCurrentTable);
        importTable.setEnabled(false);
        action(menu, "Downloads", () -> { nativeShowDownloads(); finish(); });
        action(menu, "Open externally", () -> external(page().getUrl()));
        address = new SearchView(this); address.setIconifiedByDefault(false);
        address.setQueryHint("Search or enter URL"); address.setContentDescription("Search or website address");
        address.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_VARIATION_WEB_EDIT_TEXT | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        address.setImeOptions(EditorInfo.IME_ACTION_GO); address.setMaxWidth(Integer.MAX_VALUE);
        address.setOnQueryTextListener(new SearchView.OnQueryTextListener() {
            @Override public boolean onQueryTextChange(String value) { return false; }
            @Override public boolean onQueryTextSubmit(String value) {
                String url = nativeAddressTarget(value); if (url.isEmpty()) return true;
                address.clearFocus(); load(url); return true;
            }
        });
        controls.addView(address, new Toolbar.LayoutParams(-1, -2, Gravity.CENTER_VERTICAL)); layout.addView(controls);
        Toolbar navigation = new Toolbar(this);
        backItem = action(navigation.getMenu(), "Back", this::back);
        backItem.setIcon(R.drawable.ic_browser_back).setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        forwardItem = action(navigation.getMenu(), "Forward", () -> { if (page().canGoForward()) page().goForward(); });
        forwardItem.setIcon(R.drawable.ic_browser_forward).setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        reloadItem = action(navigation.getMenu(), "Reload", () -> {
            if (loadingPage == page()) { page().stopLoading(); loadingPage = null; updateControls(); }
            else page().reload();
        });
        reloadItem.setIcon(R.drawable.ic_browser_reload).setShowAsAction(MenuItem.SHOW_AS_ACTION_ALWAYS);
        layout.addView(navigation);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(100);
        layout.addView(progress, new LinearLayout.LayoutParams(-1, Math.max(1, (int)(2 * getResources().getDisplayMetrics().density))));
        setContentView(layout);
        attach(newPage());
        address.clearFocus(); load(lastPage);
    }
    private void attach(WebView web) {
        if (page() != null) { layout.removeView(page()); page().onPause(); }
        pages.add(web);
        clearTable();
        layout.addView(web, new LinearLayout.LayoutParams(-1, 0, 1)); updateControls();
    }
    private void load(String url) {
        if (!http(url)) { Toast.makeText(this, "Use an HTTP or HTTPS page.", Toast.LENGTH_SHORT).show(); return; }
        address.clearFocus(); address.setQuery(url, false);
        clearTable();
        page().loadUrl(url);
    }
    private WebView newPage() {
        WebView web = new WebView(this);
        WebSettings settings = web.getSettings();
        // Keep the installed Chromium/device versions, without legacy WebView
        // markers that make sites fall back to an older or embedded-client UI.
        settings.setUserAgentString(settings.getUserAgentString()
                .replace("; wv)", ")").replace(" Version/4.0 Chrome/", " Chrome/"));
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
                requests.observed(request.getUrl().toString(), request.getMethod(),
                        request.isForMainFrame() || BmsBrowserRequestPolicy.navigationHeaders(request.getRequestHeaders()));
                return null;
            }
            @Override public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest request) {
                if (http(request.getUrl().toString())) {
                    requests.observed(request.getUrl().toString(), request.getMethod(), true);
                    return false;
                }
                if ("about:blank".equals(request.getUrl().toString())) return false;
                // Browser checks such as Turnstile use inline child documents.
                if (!request.isForMainFrame() && "about:srcdoc".equals(request.getUrl().toString())) return false;
                if (request.isForMainFrame()) unsupported(view.getUrl());
                return true;
            }
            @Override public void onPageCommitVisible(WebView view, String url) { requests.pageCommitted(); }
            @Override public void onPageStarted(WebView view, String url, android.graphics.Bitmap favicon) {
                loading[0] = true;
                if (view == page()) { loadingPage = view; clearTable(); updateControls(); }
            }
            @Override public void doUpdateVisitedHistory(WebView view, String url, boolean reload) {
                updatePage(view, url, !loading[0]);
            }
            @Override public void onPageFinished(WebView view, String url) {
                if (!Objects.equals(url, view.getUrl())) return;
                loading[0] = false;
                if (loadingPage == view) loadingPage = null;
                updatePage(view, url, true);
            }
            private void updatePage(WebView view, String url, boolean inspectDocument) {
                if (view != page() || !Objects.equals(url, view.getUrl())) return;
                updateControls(); refreshTable(view, inspectDocument);
                if (http(url)) lastPage = url;
            }
        });
        web.setWebChromeClient(new WebChromeClient() {
            @Override public void onProgressChanged(WebView view, int value) { if (view == page()) updateControls(); }
            @Override public boolean onCreateWindow(WebView view, boolean dialog, boolean userGesture, Message result) {
                if (!userGesture || pages.size() >= 4) return false;
                WebView child = newPage(); attach(child);
                ((WebView.WebViewTransport) result.obj).setWebView(child); result.sendToTarget(); return true;
            }
            @Override public void onCloseWindow(WebView window) { if (page() == window) closePopup(); }
        });
        web.setDownloadListener((url, userAgent, disposition, mime, length) -> {
            if (!http(url) || !requests.consume(url)) { unsupported(web.getUrl()); return; }
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
        loadingPage = page().getProgress() < 100 ? page() : null;
        updateControls(); refreshTable(page(), page().getProgress() == 100);
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
    private static native String nativeAddressTarget(String input);
    private static native String nativeTableUrl(String url, String detected);
    private static native String nativeTableDetectionScript();
    private static native boolean nativeImportTable(String url);
    private static native String nativeDownload(String url, String filename, String agent, String cookie, String referrer);
    private static native void nativeForeground(boolean foreground);
    private static native void nativeShowDownloads();
}
