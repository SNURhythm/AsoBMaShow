#include "BgfxMetalPresentationGate.h"
#include <cstdlib>
#include <future>
#include <iostream>

using namespace asobmashow::metalPresentation;
using namespace std::chrono_literals;

static void require(bool condition, const char* message)
{
    if (!condition) { std::cerr << message << '\n'; std::abort(); }
}

int main()
{
    require(wait(nullptr, nullptr, 0ms) == WaitResult::Completed, "no pending frame must not wait");
    auto token = begin();
    complete(token);
    require(wait(nullptr, nullptr, 0ms) == WaitResult::Completed, "completion before wait lost");

    token = begin();
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    struct Context { std::promise<void>* entered; std::shared_future<void> released; } context{&entered, released};
    auto waiter = std::async(std::launch::async, [&] {
        return wait([](void* opaque) {
            auto& ctx = *static_cast<Context*>(opaque);
            ctx.entered->set_value();
            ctx.released.wait();
            return true;
        }, &context);
    });
    require(entered.get_future().wait_for(1s) == std::future_status::ready,
            "pending wait never checked cancellation");
    require(waiter.wait_for(0ms) == std::future_status::timeout, "pending frame did not wait");
    // Must not deadlock: the predicate runs outside the gate mutex.
    complete(token);
    release.set_value();
    require(waiter.get() == WaitResult::Completed, "callback did not release pending wait");

    token = begin();
    require(wait([](void*) { return false; }, nullptr) == WaitResult::Cancelled, "cancellation ignored");
    auto fresh = begin();
    require(fresh > token, "token identity reused after cancellation");
    complete(token);
    require(wait(nullptr, nullptr, 0ms) == WaitResult::TimedOut, "cancelled callback released fresh frame");
    token = fresh;
    fresh = begin();
    complete(token);
    require(wait(nullptr, nullptr, 0ms) == WaitResult::TimedOut, "timed-out callback released fresh frame");

    token = begin();
    fresh = begin();
    complete(token);
    require(wait(nullptr, nullptr, 0ms) == WaitResult::TimedOut, "superseded callback released fresh frame");

    token = begin();
    cancel();
    fresh = begin();
    complete(token);
    require(wait(nullptr, nullptr, 0ms) == WaitResult::TimedOut, "explicit cancellation reused pending state");

    begin();
    const auto timeoutStarted = std::chrono::steady_clock::now();
    require(wait(nullptr, nullptr, 1000ms) == WaitResult::TimedOut,
            "missing callback did not time out");
    const auto timeoutElapsed = std::chrono::steady_clock::now() - timeoutStarted;
    // Leave scheduling headroom while distinguishing the 50 ms cap from the
    // requested one-second wait. A missing callback must consume its token.
    require(timeoutElapsed >= 50ms && timeoutElapsed < 500ms, "positive timeout was not capped at 50 ms");
    require(wait(nullptr, nullptr, 0ms) == WaitResult::Completed, "timeout left a pending frame");

    begin();
    int checks = 0;
    require(wait([](void* opaque) { return ++*static_cast<int*>(opaque) < 2; }, &checks)
                == WaitResult::Cancelled, "predicate not rechecked while waiting");

    begin();
    require(wait([](void*) { begin(); return false; }, nullptr) == WaitResult::Superseded,
            "old waiter consumed a new token");
    require(wait(nullptr, nullptr, 0ms) == WaitResult::TimedOut, "new pending frame lost to old waiter");
    std::cout << "presentation gate contracts passed\n";
}
