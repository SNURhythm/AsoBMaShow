package com.snurhythm.asobmashow;

import static org.junit.Assert.assertArrayEquals;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

import org.junit.Test;

public class DocumentHandoffMimePolicyTest {
    @Test
    public void zipImportIncludesCustomAndProviderFallbackTypes() {
        assertEquals("*/*", DocumentHandoffMimePolicy.importIntentType(
                "application/zip"));
        assertArrayEquals(new String[] {
                "application/zip",
                "application/x-zip-compressed",
                "application/vnd.snurhythm.asobmashow.profile+zip",
                "application/octet-stream"
        }, DocumentHandoffMimePolicy.importExtraMimeTypes("application/zip"));

        assertEquals("*/*", DocumentHandoffMimePolicy.importIntentType(
                "application/vnd.snurhythm.asobmashow.profile+zip"));
    }

    @Test
    public void unrelatedDocumentTypesKeepTheirExactFilter() {
        assertEquals("application/json",
                DocumentHandoffMimePolicy.importIntentType("application/json"));
        assertNull(DocumentHandoffMimePolicy.importExtraMimeTypes(
                "application/json"));
    }
}
