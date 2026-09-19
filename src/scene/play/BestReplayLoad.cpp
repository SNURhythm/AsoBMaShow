#include "BestReplayLoad.h"
#include "../ReplayRecordTask.h"

#include <utility>

namespace replay {

void startBestReplayLoad(ReplayRecordTask &task,
                         BestReplayResolverFactory makeResolver,
                         std::string attemptId,
                         std::filesystem::path chartPath,
                         BestReplayLoaded onLoaded) {
  task.cancelAndWait();
  task.start([&task, makeResolver = std::move(makeResolver),
              attemptId = std::move(attemptId), chartPath = std::move(chartPath),
              onLoaded = std::move(onLoaded)](auto cancelled) mutable {
    auto resolver = makeResolver();
    auto loaded = resolver.load(attemptId, chartPath, *cancelled);
    if (cancelled->load() || loaded == nullptr) {
      return;
    }
    task.publish([loaded = std::move(loaded), onLoaded = std::move(onLoaded)] {
      onLoaded(*loaded);
    });
  });
}

} // namespace replay
