#include "BgfxMetalPresentationGate.h"
#include <cstdlib>
#include <future>
#include <iostream>

using namespace asobmashow::metalPresentation;
using namespace std::chrono_literals;
static void require(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::abort(); }
}
static bool yes(void*) { return true; }

int main()
{
    require(acquire() == 0, "unscoped acquisition must bypass");
    Scope scope(yes, nullptr);
    const auto a = acquire(), b = acquire();
    require(a && b && a != b, "first two slots must be available");
    std::promise<void> polled, resume;
    auto resumed = resume.get_future().share();
    auto waiter = std::async(std::launch::async, [&] {
        bool first = true;
        struct Poll { bool& first; std::promise<void>& polled; std::shared_future<void>& resumed; } poll{first, polled, resumed};
        Scope waiting([](void* ptr) {
            auto& p = *static_cast<Poll*>(ptr);
            if (p.first) { p.first = false; p.polled.set_value(); p.resumed.wait(); }
            return true;
        }, &poll);
        return acquire();
    });
    require(polled.get_future().wait_for(1s) == std::future_status::ready, "third must poll");
    require(waiter.wait_for(0ms) == std::future_status::timeout, "third did not block");
    complete(b); // Out of order; predicate must not own the gate mutex.
    resume.set_value();
    const auto c = waiter.get();
    require(c > b, "out-of-order callback did not free slot");
    complete(b); complete(0);
    const auto start = std::chrono::steady_clock::now();
    require(acquire() == 0, "duplicate callback created an extra slot");
    const auto elapsed = std::chrono::steady_clock::now() - start;
    require(elapsed >= 50ms && elapsed < 500ms, "missing callback wait must be capped at 50ms");
    const auto d = acquire(), e = acquire();
    require(d > c && e > d, "timeout must clear reservations without reusing tokens");
    complete(a); complete(c);
    int polls = 0;
    {
        Scope cancelling([](void* p) { return ++*static_cast<int*>(p) < 2; }, &polls);
        require(acquire() == 0 && polls == 2, "waiting predicate must be polled again");
    }
    require(acquire() > e, "cancelled slots not recovered");
    cancel();
    acquire(); acquire();
    // Reset while the old waiter is outside the mutex. It must neither clear
    // nor reserve a slot in the new generation, even when its predicate fails.
    std::promise<void> oldPolled, resumeOld;
    auto oldResumed = resumeOld.get_future().share();
    auto oldWaiter = std::async(std::launch::async, [&] {
        struct Poll { std::promise<void>& polled; std::shared_future<void>& resumed; } poll{oldPolled, oldResumed};
        Scope waiting([](void* ptr) {
            auto& p = *static_cast<Poll*>(ptr);
            p.polled.set_value();
            p.resumed.wait();
            return false;
        }, &poll);
        return acquire();
    });
    require(oldPolled.get_future().wait_for(1s) == std::future_status::ready, "old waiter did not poll");
    cancel();
    const auto fresh = acquire();
    acquire();
    resumeOld.set_value();
    require(oldWaiter.get() == 0, "cancelled waiter must bypass");
    require(fresh > e, "reset reused token identity");
    polls = 0;
    {
        Scope cancelling([](void* p) { return ++*static_cast<int*>(p) < 2; }, &polls);
        require(acquire() == 0 && polls == 2, "old waiter cleared new generation");
    }
    acquire(); acquire();
    {
        Scope discard(nullptr, nullptr);
        require(acquire() == 0, "discard scope must bypass and cancel");
    }
    const auto restored = acquire();
    require(restored != 0, "scope must restore outer predicate");
    complete(restored); // Same completion operation used for nil nextDrawable.
    require(acquire() && acquire(), "nil acquisition leaked its permit");
    cancel();
    std::cout << "two-slot acquisition gate contracts passed\n";
}
