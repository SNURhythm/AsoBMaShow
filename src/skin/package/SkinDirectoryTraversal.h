#pragma once

#if !defined(_WIN32)
#include <fcntl.h>

namespace skin {

// Platform parents may permit traversal without directory listing (notably
// Android / and /data). Only the final app-owned directory needs a readable
// descriptor. O_DIRECTORY and O_NOFOLLOW remain the caller's responsibility.
inline int skinAncestorDirectoryOpenFlag() noexcept {
#if defined(O_PATH)
  return O_PATH;
#elif defined(O_SEARCH)
  return O_SEARCH;
#else
  return O_RDONLY;
#endif
}

} // namespace skin
#endif
