#pragma once

#include "RAII.h"

#include <archive.h>

using ArchiveReadHandle = UniqueResource<archive, archive_read_free>;
using ArchiveWriteHandle = UniqueResource<archive, archive_write_free>;

inline ArchiveReadHandle makeArchiveReadHandle() {
  return ArchiveReadHandle(archive_read_new());
}

inline ArchiveWriteHandle makeArchiveWriteHandle() {
  return ArchiveWriteHandle(archive_write_new());
}
