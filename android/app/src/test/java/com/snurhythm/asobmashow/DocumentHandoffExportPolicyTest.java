package com.snurhythm.asobmashow;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class DocumentHandoffExportPolicyTest {
    @Test
    public void unknownAndNonemptyDestinationsAreNeverTouchedOrRestored() {
        DocumentHandoffExportPolicy.Decision unknown =
                DocumentHandoffExportPolicy.decide(-1);
        DocumentHandoffExportPolicy.Decision nonempty =
                DocumentHandoffExportPolicy.decide(8);
        assertEquals(DocumentHandoffExportPolicy.Decision.REFUSE_UNKNOWN, unknown);
        assertEquals(DocumentHandoffExportPolicy.Decision.REFUSE_NONEMPTY, nonempty);
        assertFalse(DocumentHandoffExportPolicy.shouldRestoreEmpty(unknown, false));
        assertFalse(DocumentHandoffExportPolicy.shouldRestoreEmpty(nonempty, false));
    }

    @Test
    public void acceptedEmptyDestinationIsRestoredOnlyAfterWriteAttempt() {
        DocumentHandoffExportPolicy.Decision empty =
                DocumentHandoffExportPolicy.decide(0);
        assertEquals(DocumentHandoffExportPolicy.Decision.ACCEPT_EMPTY, empty);
        assertFalse(DocumentHandoffExportPolicy.shouldRestoreEmpty(empty, false));
        assertTrue(DocumentHandoffExportPolicy.shouldRestoreEmpty(empty, true));
    }
}
