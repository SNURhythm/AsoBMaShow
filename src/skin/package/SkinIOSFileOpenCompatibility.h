#pragma once

#include "SkinPathPolicy.h"

// Keep the existing desktop POSIX hardening intact. On iOS, the skin root is
// intentionally a Files-editable Documents location, so every skin-service
// open uses the neutral flag returned by skinOpenNoFollowFlag().
#if (TARGET_OS_IOS || TARGET_OS_SIMULATOR) && defined(O_NOFOLLOW)
#undef O_NOFOLLOW
#define O_NOFOLLOW skin::skinOpenNoFollowFlag()
#endif
