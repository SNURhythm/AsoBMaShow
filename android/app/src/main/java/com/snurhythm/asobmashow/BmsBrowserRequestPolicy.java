package com.snurhythm.asobmashow;

// Redirect targets are not consistently reported by WebView. Only the current
// main-frame request supplies method evidence; a previous visit cannot grant it.
final class BmsBrowserRequestPolicy {
    private String method = "";
    synchronized void started(String value) { method = value == null ? "" : value; }
    synchronized boolean replayable() { return "GET".equals(method); }
}
