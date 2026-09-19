#pragma once

#include "../../replay/BestReplayResolver.h"

#include <functional>
#include <string>

class ReplayRecordTask;

namespace replay {

using BestReplayResolverFactory = std::function<BestReplayResolver()>;
using BestReplayLoaded = std::function<void(const ReplayData &)>;

// Cancel/join any previous load before admitting this request. Resolver
// construction and loading run on the task worker. Only a successful,
// uncancelled replay queues onLoaded for the scene's takeCompletion caller.
// Missing/unreadable replays leave the scene's proportional fallback intact.
// The task owns callback captures; its scene must cancel/join before destroying
// callback dependencies. Resolver dependencies must also outlive the task.
void startBestReplayLoad(ReplayRecordTask &task,
                         BestReplayResolverFactory makeResolver,
                         std::string attemptId,
                         std::filesystem::path chartPath,
                         BestReplayLoaded onLoaded);

} // namespace replay
