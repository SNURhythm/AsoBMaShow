package com.snurhythm.asobmashow;

/** Fail-closed policy for provider destinations returned by ACTION_CREATE_DOCUMENT. */
final class DocumentHandoffExportPolicy {
    enum Decision {
        ACCEPT_EMPTY,
        REFUSE_UNKNOWN,
        REFUSE_NONEMPTY
    }

    static Decision decide(long existingSize) {
        if (existingSize < 0) {
            return Decision.REFUSE_UNKNOWN;
        }
        if (existingSize != 0) {
            return Decision.REFUSE_NONEMPTY;
        }
        return Decision.ACCEPT_EMPTY;
    }

    static boolean shouldRestoreEmpty(Decision decision, boolean touched) {
        return decision == Decision.ACCEPT_EMPTY && touched;
    }

    private DocumentHandoffExportPolicy() {}
}
