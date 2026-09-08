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
