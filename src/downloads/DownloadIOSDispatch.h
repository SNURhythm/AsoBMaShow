#pragma once
#include "DownloadJobs.h"
#include <dispatch/dispatch.h>

namespace downloads {
// Objective-C blocks preserve C++ reference captures. Own the job before
// enqueueing so the callback cannot outlive its request headers or paths.
inline void dispatchOwnedJob(dispatch_queue_t queue, Job job,
                             void (^callback)(const Job &)) {
  dispatch_async(queue, ^{ callback(job); });
}
}
