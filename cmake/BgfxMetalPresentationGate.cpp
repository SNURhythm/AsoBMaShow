#include "BgfxMetalPresentationGate.h"
#include <algorithm>
#include <condition_variable>
#include <mutex>

namespace asobmashow::metalPresentation {
namespace {
struct Gate {
    std::mutex mutex;
    std::condition_variable changed;
    Token sequence = 0;
    Token pending = 0;
    bool presented = false;
};

Gate& gate()
{
    // Metal may deliver a callback after bgfx shutdown. Keep both the gate
    // and token identity alive across shutdown/resume and static destruction.
    static Gate* value = new Gate;
    return *value;
}
}

Token begin()
{
    auto& state = gate();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pending = ++state.sequence;
    state.presented = false;
    state.changed.notify_all();
    return state.pending;
}

void complete(Token token)
{
    auto& state = gate();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (token != 0 && state.pending == token) {
        state.presented = true;
        state.changed.notify_all();
    }
}

void cancel()
{
    auto& state = gate();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.pending = 0;
    state.changed.notify_all();
}

WaitResult wait(bool (*keepWaiting)(void*), void* context,
                std::chrono::milliseconds timeout)
{
    using namespace std::chrono;
    const auto deadline = steady_clock::now() + std::clamp(timeout, milliseconds(0), milliseconds(50));
    auto& state = gate();
    std::unique_lock<std::mutex> lock(state.mutex);
    const Token token = state.pending;
    if (token == 0) return WaitResult::Completed;
    for (;;) {
        if (state.pending != token) return WaitResult::Superseded;
        // External lifecycle/export checks must never run under the gate lock.
        lock.unlock();
        const bool active = !keepWaiting || keepWaiting(context);
        lock.lock();
        if (state.pending != token) return WaitResult::Superseded;
        if (!active || state.presented || steady_clock::now() >= deadline) {
            state.pending = 0;
            return !active ? WaitResult::Cancelled
                : state.presented ? WaitResult::Completed : WaitResult::TimedOut;
        }
        state.changed.wait_until(lock, std::min(deadline, steady_clock::now() + milliseconds(2)));
    }
}
}
