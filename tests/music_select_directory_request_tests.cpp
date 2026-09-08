#include "music_select/MusicSelectDirectoryRequest.h"

#include <cassert>

int main() {
  const std::vector<MusicSelectBar> rows{
      {.id = {"folder:/songs"}, .kind = skin::MusicSelectBarKind::Folder},
      {.id = {"folder:/other"}, .kind = skin::MusicSelectBarKind::Folder}};
  MusicSelectBarManagerReadView view{.rows = rows, .rowsRevision = 12};
  MusicSelectDirectoryRequest request{
      .directory = rows.front(), .generation = 4, .rowsRevision = 12,
      .libraryRevision = 20, .scoreRevision = 30};
  assert(request.matches(4, view, 20, 30));
  assert(!request.matches(3, view, 20, 30));
  assert(!request.matches(4, view, 21, 30));
  assert(!request.matches(4, view, 20, 31));
  view.selectedIndex = 1;
  assert(!request.matches(4, view, 20, 30));
  view.selectedIndex = 0;
  ++view.rowsRevision;
  assert(!request.matches(4, view, 20, 30));
  view.rows = {};
  view.rowsRevision = 12;
  assert(!request.matches(4, view, 20, 30));
}
