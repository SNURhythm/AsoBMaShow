package com.snurhythm.asobmashow;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import java.nio.file.Path;
import org.junit.Test;

public class DocumentHandoffImportPathPolicyTest {
    @Test
    public void acceptsOnlyIssuedUuidDirectoryAndImportLeaf() {
        Path base = Path.of("/private/cache/document-handoff");
        Path valid = base.resolve("12345678-1234-1234-1234-123456789abc")
                .resolve("imported-document.zip");
        assertTrue(DocumentHandoffImportPathPolicy.isIssuedPath(base, valid));
        assertFalse(DocumentHandoffImportPathPolicy.isIssuedPath(
                base, valid.resolveSibling("other.zip")));
        assertFalse(DocumentHandoffImportPathPolicy.isIssuedPath(
                base, valid.getParent().resolve("nested")
                        .resolve("imported-document.zip")));
        assertFalse(DocumentHandoffImportPathPolicy.isIssuedPath(
                base, base.resolve("not-a-uuid")
                        .resolve("imported-document.zip")));
    }
}
