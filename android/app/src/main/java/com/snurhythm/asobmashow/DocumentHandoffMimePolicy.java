package com.snurhythm.asobmashow;

final class DocumentHandoffMimePolicy {
    private static final String ZIP = "application/zip";
    private static final String LEGACY_ZIP = "application/x-zip-compressed";
    private static final String PROFILE_ZIP =
            "application/vnd.snurhythm.asobmashow.profile+zip";
    private static final String OPAQUE_BINARY = "application/octet-stream";

    private DocumentHandoffMimePolicy() {
    }

    static String importIntentType(String requestedMimeType) {
        return isProfileZipType(requestedMimeType) ? "*/*" : requestedMimeType;
    }

    static String[] importExtraMimeTypes(String requestedMimeType) {
        if (!isProfileZipType(requestedMimeType)) {
            return null;
        }
        return new String[] {ZIP, LEGACY_ZIP, PROFILE_ZIP, OPAQUE_BINARY};
    }

    private static boolean isProfileZipType(String mimeType) {
        return ZIP.equalsIgnoreCase(mimeType) ||
                LEGACY_ZIP.equalsIgnoreCase(mimeType) ||
                PROFILE_ZIP.equalsIgnoreCase(mimeType);
    }
}
