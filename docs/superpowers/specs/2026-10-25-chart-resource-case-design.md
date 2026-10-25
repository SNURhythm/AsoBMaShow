# Case-insensitive chart resource lookup

Implement the cached fallback approved in conversation: native exact matches win;
otherwise resolve each path component using ASCII case folding, preserving all
other bytes. Reject ambiguous folded matches. Keep extension priority unchanged.
Audio, BGA and artwork share resource lookup; filesystem identity, archive
maintenance, and write/delete operations keep their existing semantics.

Approaches: repeated scans are unnecessarily expensive; eager indexing penalizes
correctly spelled charts. Use lazy directory-name indexes with bounded retained
memory, native directory change stamps, and thread-safe reuse. Oversized listings
must remain correct without retaining unbounded indexes. Do not read payloads.
Android SAF caches child names and document IDs and clears them at the existing
transient-cache lifecycle boundary. Archive lookup behavior stays unchanged.

Verify mismatched files and nested directories, extension fallback, exact versus
ambiguous names, non-ASCII names, cache refresh, concurrency, and SAF query reuse.

Native indexes retain at most 64 directories, 65,536 folded names, and 4 MiB of
name/path text (container overhead is additional and bounded by the name count).
SAF uses the same directory/name limits and 2 Mi characters of name/ID text.
Cancellation is checked during native listing and between extension attempts;
waiting for the shared native cache lock is cancellable too.
