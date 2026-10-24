# Chart readme in the Reveal menu

Add **Readme** to the built-in and custom selector Reveal/chart menus so players
can read the artist's bundled notes without leaving the chart selection screen.

Beatoraja 0.8.9 event 17 lists immediate non-directory `.txt` siblings of the
selected chart, ignoring extension case, and opens all matches in the OS viewer.
It does not require a `readme` basename or traverse parents/subdirectories.
Source: https://github.com/exch-bms2/beatoraja/blob/83574048ba489082eb46df7fe9806b464bf1f526/src/bms/player/beatoraja/skin/property/EventFactory.java#L234-L251

## Approach

Use an in-app reader with a filename dropdown, scrollable text, Close, loading,
empty and failure states. The user selected this approach.
The alternative is the existing desktop OS opener, which cannot provide the
same experience on mobile or avoid opening every discovered document at once.
The user also requested custom-skin wiring: route event 17 (`open_document`)
and its existing readme shortcut to this same reader on all platforms.

Discover regular text siblings on disk, archive-index entries in the chart's
inner directory, and direct Android SAF children. Sort filenames consistently.
Discovery reads metadata only; loading reads only the selected document. Do this
off the UI thread with cancellation and no references to scene objects. Closing
or changing charts must never publish stale work or block on archive I/O.

Decode UTF-8 (with optional BOM), BOM-marked UTF-16, and legacy CP932/CP949. Automatic decoding tries Unicode, then strict Japanese,
then Korean decoding; an Encoding dropdown resolves ambiguous legacy bytes. Preserve
line breaks and readable spacing. Render wrapped rows through a virtualized list
so a long document cannot create one enormous GPU texture. Limit each document
to 1 MiB and discovery to 1024 matches; report limits rather than silently hiding
content. No scanning/schema changes, archive extraction, or parser changes.
