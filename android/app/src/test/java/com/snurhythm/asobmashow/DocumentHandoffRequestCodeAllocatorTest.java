package com.snurhythm.asobmashow;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

import org.junit.Test;

public class DocumentHandoffRequestCodeAllocatorTest {
    @Test
    public void allocatorSaturatesWithoutWrappingOrReusingCodes() {
        DocumentHandoffRequestCodeAllocator allocator =
                new DocumentHandoffRequestCodeAllocator(0xfffd, 0xffff);
        assertEquals(0xfffd, allocator.allocate());
        assertEquals(0xfffe, allocator.allocate());
        assertEquals(0xffff, allocator.allocate());
        for (int attempt = 0; attempt < 1000; attempt++) {
            assertEquals(-1, allocator.allocate());
        }
        assertTrue(allocator.wasIssued(0xfffd));
        assertTrue(allocator.wasIssued(0xffff));
        assertFalse(allocator.wasIssued(0x0000));
        assertFalse(allocator.wasIssued(0x10000));
    }
}
