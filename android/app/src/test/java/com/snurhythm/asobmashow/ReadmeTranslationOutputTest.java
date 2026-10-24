package com.snurhythm.asobmashow;
import org.junit.Test;
import static org.junit.Assert.*;

public class ReadmeTranslationOutputTest {
    @Test public void countsUtf8AndRejectsPassthroughOverflow() {
        ReadmeTranslationOutput output = new ReadmeTranslationOutput(8);
        assertTrue(output.append("한🙂")); // 3 + 4 bytes, not three UTF-16 units.
        assertFalse(output.append("\n\n"));
        assertEquals("한🙂", output.toString());
        assertTrue(output.append("\n"));
        assertFalse(output.append("a"));
        output.clear();
        assertTrue(output.append("12345678"));
        assertFalse(output.append("字"));
    }
}
