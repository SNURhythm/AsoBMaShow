# Adaptive IR Polling Design

## Goal

Reduce the visible delay after Bokutachi accepts a score with HTTP 202 without
reposting the score or polling the remote service continuously.

## Polling Policy

The first remote-status poll is due 200 ms after the accepted POST. If the
import remains ongoing, subsequent polls use delays of 1, 2, 3, 5, and 10
seconds. Further ongoing responses remain capped at 10 seconds.

The polling stage is stored with the durable outbox row independently from the
total request-attempt count and transient-failure count. This guarantees that
earlier POST failures do not skip the 200 ms first poll, and that crash recovery
does not reset an established remote polling cadence.

Manual Retry on an awaiting row keeps its existing remote job and polling stage,
makes the next poll immediately due, and wakes the worker. It never repeats the
POST or restarts the adaptive schedule.

Transient HTTP or transport failures retain the existing retry/backoff policy.
The adaptive schedule advances only after an `ongoing` remote response. A new
202 response initializes the polling stage for its newly accepted remote job.

## Persistence

The replay database schema gains a non-negative `remote_poll_count` column with
a default of zero. Existing rows migrate with zero, which safely gives an
already-queued job the shortest poll after its currently persisted due time.
The count is reset when a row no longer represents an awaiting remote job.

No API key, authorization value, or new server response data is persisted.

## Testing

Service tests verify the 200 ms initial delay, every adaptive step, the 10-second
cap, immediate manual polling without reposting, and independence from earlier
request failures. Repository tests verify poll-count persistence, mutation,
retry preservation, and schema migration from the current database version.

Verification includes focused IR service and repository tests, the complete
configured CTest suite, `git diff --check`, and the desktop `main` build.
