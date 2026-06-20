#pragma once

#include "RAII.h"

#include <archive.h>

using ArchiveReadHandle = UniqueResource<archive, archive_read_free>;

inline ArchiveReadHandle makeArchiveReadHandle() {
  return ArchiveReadHandle(archive_read_new());
}
