# Chart source syntax colors

Add syntax highlighting to the existing read-only native chart source viewer on
iOS, Android, macOS and Windows. Keep practice backgrounds, manual selection,
copy, direct highlight jumps and focus preservation unchanged.

The tokenizer lives in this application. Neither `bms-parser-cpp` nor the normal
chart parse/scan path changes. It runs only when opening a source document, on
the application worker, and emits sorted non-overlapping UTF-16 spans. It makes
linear passes over decoded UTF-8 without regex, copied token strings, or one
token per note cell. A whole channel payload is one span. Styles distinguish
directives, control flow, resource/channel IDs, numbers, text, data and comments.
Empty `00` cells use a muted color. Only viewport painting splits data into
coalesced empty/populated runs, preserving two-character cell alignment and
sharing the existing foreground-run budget. The cached payload remains one span.

Native foreground attributes are limited to the visible text plus bounded
overscan. Scrolling uses binary search into cached spans; theme changes reuse
the same spans. Clear retired foreground attributes so native span storage does
not grow with scrolling. Preserve background attributes and selection. Clamp
painting to 16,384 UTF-16 units and 4,096 syntax spans per update; normal text
remains readable outside that window. Native text controls already have their
own full-document storage/layout costs, which this change must not multiply by
installing an attribute object for every token in a large file.

Use the existing app palette in both themes. The no-native-surface fallback
stays readable with its existing renderer; it does not acquire a second syntax
rendering system. Tokenization can be skipped when no native surface is created.

Alternatives considered: extending the parser was explicitly declined; regex
and per-note-cell formatting add avoidable work; styling every token eagerly
would retain large native attribute tables. A separate scanner and bounded
native styling keep chart loading unchanged and bound scrolling work.

Verify Unicode/CRLF offsets, directives and filenames, long data lines, dense
short-line files, viewport lookup near the end of a large document, selection
and practice-highlight preservation, repeated scrolling and theme/resize changes.
Record optimized tokenizer timing and native formatting bounds using synthetic
large-text fixtures without imposing machine-sensitive timing thresholds.
