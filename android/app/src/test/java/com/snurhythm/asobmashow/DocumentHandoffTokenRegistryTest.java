package com.snurhythm.asobmashow;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class DocumentHandoffTokenRegistryTest {
    @Test
    public void cancelBeforeActivationIsConsumedAndRetired() {
        DocumentHandoffTokenRegistry registry = new DocumentHandoffTokenRegistry();
        assertTrue(registry.register("41"));
        assertTrue(registry.cancel("41"));
        assertEquals(DocumentHandoffTokenRegistry.Activation.CANCELLED,
                registry.activate("41"));
        registry.retire("41");

        assertFalse(registry.cancel("41"));
        assertEquals(0, registry.sizeForTesting());
    }

    @Test
    public void lateRetiredCancellationCannotAffectNewOperation() {
        DocumentHandoffTokenRegistry registry = new DocumentHandoffTokenRegistry();
        assertTrue(registry.register("100"));
        assertEquals(DocumentHandoffTokenRegistry.Activation.ACTIVE,
                registry.activate("100"));
        registry.finish("100");
        registry.retire("100");

        assertTrue(registry.register("101"));
        assertEquals(DocumentHandoffTokenRegistry.Activation.ACTIVE,
                registry.activate("101"));
        assertFalse(registry.cancel("100"));
        assertEquals(1, registry.sizeForTesting());
    }

    @Test
    public void repeatedStaleCancellationDoesNotGrowStorage() {
        DocumentHandoffTokenRegistry registry = new DocumentHandoffTokenRegistry();
        for (int i = 0; i < 1000; i++) {
            assertFalse(registry.cancel(Integer.toString(i)));
        }
        assertEquals(0, registry.sizeForTesting());
    }
}
