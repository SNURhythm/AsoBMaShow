package com.snurhythm.asobmashow;

import java.util.LinkedHashSet;
import java.util.Map;

// DownloadListener supplies a URL but no method/frame identity. Evidence must
// belong to that navigation, never to the main page or a historical visit.
final class BmsBrowserRequestPolicy {
    private final LinkedHashSet<String> getNavigations = new LinkedHashSet<>();
    private boolean ambiguous;

    // WebView may omit Sec-Fetch-* and omit the upgrade header for HSTS hosts.
    // Document Accept also identifies form navigations; ambiguous HTML XHRs
    // conservatively use the external fallback.
    static boolean navigationHeaders(Map<String, String> headers) {
        for (Map.Entry<String, String> header : headers.entrySet()) {
            if (("Upgrade-Insecure-Requests".equalsIgnoreCase(header.getKey()) && "1".equals(header.getValue())) ||
                    ("Sec-Fetch-Mode".equalsIgnoreCase(header.getKey()) && "navigate".equalsIgnoreCase(header.getValue())) ||
                    ("Accept".equalsIgnoreCase(header.getKey()) && header.getValue().toLowerCase(java.util.Locale.ROOT).contains("text/html"))) return true;
        }
        return false;
    }
    synchronized void observed(String url, String method, boolean navigation) {
        if (!navigation) return;
        if (!"GET".equals(method)) {
            // Redirect targets of POST are not necessarily intercepted. Keep
            // ambiguity until a new document commits, even if another GET to
            // the same target starts before the older attachment callback.
            ambiguous = true;
            getNavigations.clear();
        } else if (!ambiguous && url != null && url.length() <= 16384) {
            if (getNavigations.size() >= 64) getNavigations.clear();
            getNavigations.add(url);
        }
    }
    synchronized void pageCommitted() { ambiguous = false; getNavigations.clear(); }
    synchronized boolean consume(String url) { return !ambiguous && getNavigations.remove(url); }
}
