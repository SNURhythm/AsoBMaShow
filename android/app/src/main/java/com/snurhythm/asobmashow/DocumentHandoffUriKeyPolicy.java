package com.snurhythm.asobmashow;

/** Stable provider-document identity independent of URI aliases or query text. */
final class DocumentHandoffUriKeyPolicy {
    static String key(String authority, String documentId) {
        if (authority == null || authority.isEmpty() ||
                documentId == null || documentId.isEmpty()) {
            throw new IllegalArgumentException("Provider document identity is empty.");
        }
        return authority.length() + ":" + authority + documentId;
    }

    private DocumentHandoffUriKeyPolicy() {
    }
}
