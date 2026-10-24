# Readme translation implementation

Spec: ../specs/2026-10-24-readme-translation-design.md

Constraints: current checkout; no deployment; preserve unrelated files; scoped tests;
commit and push verified work. Use native on-device APIs, never cloud inference.

1. Add a request service with bounded UTF-8 chunks and request-ID based delivery.
   Test chunk preservation and stale/cancelled completion before implementation.
2. Implement Android ML Kit bridge with model consent, cancellation and resource cleanup.
3. Implement Apple SwiftUI TranslationSession host with high-fidelity preference,
   native dismissal and runtime availability checks; integrate Swift builds.
4. Connect localized reader actions, target selection, original/translated modes,
   copy feedback and status; extend meaningful existing UI coverage.
5. Build focused tests, desktop, Android and Apple bridge; exercise real-device UX;
   review final diff, fix findings, commit and push to existing PR.

Ruling: execute inline in the current checkout as required by repository instructions.
Use one final independent review, as required by the execution skill.

## Execution notes

- Implemented request IDs, bounded UTF-8 chunks, clipboard/mode/target controls,
  ML Kit model consent and SwiftUI TranslationSession hosting.
- Device evidence changed source detection to per-segment detection: an English
  heading must not cause Korean notes to be returned unchanged. Undetected or
  unsupported segments (often author names) are preserved.
- Independent review found a UTF-16 versus UTF-8 output-limit mismatch. Both
  backends now budget every append in UTF-8, including passthrough text, and
  validate completion. Java/Swift boundary tests and a C++ delivery guard cover it.
- Mobile layout testing found that DropdownView has a larger touch minimum and
  hidden views retain Yoga space. The settings row now sizes naturally; optional
  controls/status leave layout when hidden.
- Android badge loading uses bounded APK asset reads through the existing archive
  reader because APK assets are not visible through std::ifstream.

## Verification

- Desktop main and unsigned iPhoneOS application builds passed; signed Android
  release builds passed without distribution.
- Six focused CTests cover request cancellation/output limits/chunks, Swift output
  bounds, readme loading, mobile layout, Android storage and localization.
- Android JUnit checks UTF-8 byte limits and passthrough overflow.
- A real macOS harness using the production bridge translated an English heading
  plus Korean and Japanese notes into English with the high-fidelity preference.
- Galaxy S20 FE walkthrough verified model consent and cancellation, complete
  translation, Google attribution, original/translated selection, scrolling and
  successful clipboard feedback. Language-model downloads are retained by ML Kit.
- Translation framework weak linking was verified with a macOS 13 deployment
  target. No physical iPhone translation run or Windows full build was performed.
