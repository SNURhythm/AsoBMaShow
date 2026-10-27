#pragma once
#include <cstdint>

namespace asobmashow { namespace metalPresentation {
using Predicate = bool (*)(void*);
// Only synchronous submission invokes the thread-local predicate. Native
// callbacks retain a token, never this scope or its application context.
class Scope
{
public:
    Scope(Predicate predicate, void* context);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Predicate previousPredicate;
    void* previousContext;
};
std::uint64_t acquire();
void complete(std::uint64_t token);
void cancel();
} }
