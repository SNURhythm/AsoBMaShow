# Find BMS transport limits

Find BMS separates metadata response retention, compressed archive transfer,
and archive extraction budgets. A metadata limit is not an archive size limit.

## Desktop metadata

GET and POST retain at most **16 MiB (16,777,216 bytes)** of response body.
Their real curl receive callback rejects an oversized chunk before appending,
including overflow in `size * nmemb`. The body budget applies to successful
responses, HTTP error bodies, and responses without a declared Content-Length.
Cancellation interrupts both body reception and idle curl progress checks.
Failures return an error rather than a partially accumulated successful body.

Internal callers can inject a smaller response limit for bounded tests. The
existing iOS/Android metadata APIs remain separate: their shared Find BMS call
signatures do not establish a streamed mobile metadata budget. Existing mobile
GET checkpoint support is preserved; mobile POST checks cancellation before and
after its native request.

## iOS archives

The Find BMS archive caller supplies an **8 GiB (8,589,934,592 bytes)** compressed
download limit. NSURLSession downloads into a file. The native delegate checks
reported byte counts and independently checks the completed file before staging
and publication. It does not read the archive into a complete NSData or vector.
NSURLSession can write a progress chunk before the delegate observes it; the
policy rejects/cancels and prevents oversized publication, rather than claiming
an exact upper bound on transient OS-managed spool bytes.

The completed temporary file is moved into an exclusively created private
staging directory. A cross-volume move falls back to a **64 KiB (65,536 bytes)**
read/write buffer, with cancellation and byte admission before writing. Only a
completed admitted transfer replaces the caller-owned private archive path.
This preserves Google Drive's second download into the same attempt path while
leaving the previous file untouched on failure. Failure cleanup removes only
the newly owned staging directory, never the existing destination.

The synchronous bridge polls cancellation in 100 ms wait slices with the
existing 190-second overall deadline. Callback context is detached before the
bridge returns, and owned abort/staging state prevents late callbacks from
publishing or recreating cleaned files. Cancellation observed before the final
publication check fails the transfer; a cancellation racing after that check
may lose to atomic publication.

HTTP(S)-only URLs, initial-HTTPS downgrade rejection, and the separate
authenticated IR transport restrictions remain unchanged. Archive extraction
retains its separate existing budgets and cancellation behavior.

The host tests use tiny injected limits, real loopback curl/NSURLSession
requests, and bounded filesystem fault injection. macOS Foundation tests are
not iOS simulator/device runtime or whole-process memory measurements.
