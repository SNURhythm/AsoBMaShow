# Readme copy and on-device translation

Add Copy and Translate to the existing chart readme modal. Copy exports the full
currently displayed decoded text, independent of wrapping or scrolling. Preserve
original text and provide Original/Translated controls after translation.

Translation defaults to the app language, with a target selector. It is explicitly
requested, asynchronous and cancellable. Closing the reader, loading another file,
changing encoding or target invalidates outstanding results. Do not write translations
to chart files. Bound work per inference and preserve line breaks.

Android uses ML Kit translation and bundled language identification. Download missing
models only after a user-facing prompt; inference remains on device. Show Google
Translate attribution. Apple uses a small SwiftUI host for TranslationSession and
system model consent on iOS 18/macOS 15 or later. Prefer `.highFidelity` on 26.4+
with the framework's automatic fallback; retain older on-device support. Do not raise
minimum OS versions. Other platforms keep Copy and explain translation availability.

Validate request lifetime, text chunk boundaries, reader behavior, builds on Android
and Apple, and real-device translation where hardware/model availability permits.
