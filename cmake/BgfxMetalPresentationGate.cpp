#include "BgfxMetalPresentationGate.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <mutex>

namespace asobmashow { namespace metalPresentation {
namespace {
struct Gate
{
    std::mutex mutex;
    std::condition_variable changed;
    std::uint64_t pending[2]{};
    std::uint64_t counter = 0;
    std::uint64_t generation = 0;

    void reset()
    {
        pending[0] = pending[1] = 0;
        ++generation;
        changed.notify_all();
    }
};
Gate& gate()
{
    // Metal may deliver a presented handler after renderer/application teardown.
    static Gate* instance = new Gate;
    return *instance;
}
thread_local Predicate currentPredicate = nullptr;
thread_local void* currentContext = nullptr;
}

Scope::Scope(Predicate predicate, void* context)
    : previousPredicate(currentPredicate), previousContext(currentContext)
{
    currentPredicate = predicate;
    currentContext = context;
    if (!predicate) cancel();
}
Scope::~Scope()
{
    currentPredicate = previousPredicate;
    currentContext = previousContext;
}

std::uint64_t acquire()
{
    auto& state = gate();
    const auto predicate = currentPredicate;
    void* const context = currentContext;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    std::unique_lock<std::mutex> lock(state.mutex);
    const auto generation = state.generation;
    for (;;)
    {
        lock.unlock();
        const bool keepRunning = predicate && predicate(context);
        lock.lock();
        // An external reset owns the new generation. An old waiter must not
        // reserve or clear any of its slots, including after a false predicate.
        if (generation != state.generation) return 0;
        if (!keepRunning || std::chrono::steady_clock::now() >= deadline)
        {
            state.reset();
            return 0;
        }
        for (auto& token : state.pending)
        {
            if (token == 0)
            {
                // Never reuse an identity, even in the theoretical overflow case.
                if (state.counter == std::numeric_limits<std::uint64_t>::max()) return 0;
                token = ++state.counter;
                return token;
            }
        }
        state.changed.wait_until(lock, std::min(deadline,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(2)));
    }
}

void complete(std::uint64_t token)
{
    if (!token) return;
    auto& state = gate();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (auto& pending : state.pending)
    {
        if (pending == token)
        {
            pending = 0;
            state.changed.notify_all();
            return;
        }
    }
}

void cancel()
{
    auto& state = gate();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.reset();
}
} }
