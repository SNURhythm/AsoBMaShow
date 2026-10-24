package com.snurhythm.asobmashow;

import java.nio.charset.StandardCharsets;

final class ReadmeTranslationOutput {
    private final int maximumBytes;
    private final StringBuilder text = new StringBuilder();
    private int bytes;
    ReadmeTranslationOutput(int maximumBytes) { this.maximumBytes = maximumBytes; }
    boolean append(String value) {
        int size = value.getBytes(StandardCharsets.UTF_8).length;
        if (size > maximumBytes - bytes) return false;
        text.append(value);
        bytes += size;
        return true;
    }
    void clear() { text.setLength(0); bytes = 0; }
    @Override public String toString() { return text.toString(); }
}
