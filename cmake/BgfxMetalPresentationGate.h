#pragma once

#include <chrono>
#include <cstdint>

namespace asobmashow::metalPresentation {
using Token = std::uint64_t;
enum class WaitResult { Completed, Cancelled, TimedOut, Superseded };
// One primary drawable is pending at a time. New submissions supersede old ones.
Token begin();
void complete(Token token);
void cancel();
// Consumes the pending token on completion, cancellation or timeout. The total
// deadline is capped at 50 ms; a nonblocking predicate is polled at most 2 ms
// apart, outside the gate mutex. A superseded waiter leaves the new token intact.
WaitResult wait(bool (*keepWaiting)(void*), void* context,
                std::chrono::milliseconds timeout = std::chrono::milliseconds(50));
}
