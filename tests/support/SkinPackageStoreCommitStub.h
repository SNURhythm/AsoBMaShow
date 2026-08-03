#pragma once

#include "skin/package/SkinPackageStore.h"

#include <cstddef>

namespace skin::test_support {

// Temporary link support for the coordinator's focused tests. Task 7's store
// lane replaces these definitions with the real CAS implementation before the
// coordinator is integrated into the application target.
void resetStore(SkinPackageStore &);
void setNextActivationDisposition(SkinPackageStore &,
                                  ActivationCommitDisposition);
void throwOnNextProfileRemoval(SkinPackageStore &);
std::size_t removedProfileCount(const SkinPackageStore &);

} // namespace skin::test_support
