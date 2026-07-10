package com.snurhythm.asobmashow;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotEquals;

import org.junit.Test;

public class DocumentHandoffUriKeyPolicyTest {
    @Test
    public void uriAliasesUseProviderAuthorityAndDocumentIdIdentity() {
        String canonical = DocumentHandoffUriKeyPolicy.key(
                "provider.example", "root:profiles/player");
        String aliasWithDifferentPathAndQuery = DocumentHandoffUriKeyPolicy.key(
                "provider.example", "root:profiles/player");
        assertEquals(canonical, aliasWithDifferentPathAndQuery);
        assertNotEquals(canonical, DocumentHandoffUriKeyPolicy.key(
                "other.example", "root:profiles/player"));
        assertNotEquals(canonical, DocumentHandoffUriKeyPolicy.key(
                "provider.example", "root:profiles/other"));
    }
}
