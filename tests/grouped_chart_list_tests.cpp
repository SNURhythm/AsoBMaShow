#include "../src/library/GroupedChartListModel.h"
#include "../src/library/StorageArchiveList.h"
#include <cassert>
#include "../src/view/StoragePolicyEditorState.h"

using music_playlist::MusicTrack;
MusicTrack group(std::string id, std::string title) {
  MusicTrack track;
  track.trackId = track.groupId = std::move(id);
  track.title = std::move(title);
  track.groupRepresentative = true;
  return track;
}
int main() {
  using namespace storage_management;
  StoragePreferences preferences;
  preferences.manual.audio = AudioAction::LosslessFlac;
  preferences.afterDownload = true;
  DownloadStoragePolicyEditorState editor;
  StoragePolicy destructive;
  destructive.bga = BgaAction::Delete;
  editor.ChangePolicy(preferences, destructive);
  assert(!preferences.afterDownload);
  assert(preferences.manual.audio == AudioAction::LosslessFlac);
  assert(!editor.ToggleAutomation(preferences));
  assert(editor.AwaitingAcknowledgment() && !preferences.afterDownload);
  assert(editor.ToggleAutomation(preferences));
  assert(preferences.afterDownload && !editor.AwaitingAcknowledgment());
  // Editing a cap on an already acknowledged policy does not re-prompt.
  destructive.maxFps = 30;
  editor.ChangePolicy(preferences, destructive);
  assert(preferences.afterDownload);
  assert(editor.ToggleAutomation(preferences) && !preferences.afterDownload);
  assert(!editor.ToggleAutomation(preferences));
  editor.ChangePolicy(preferences, {});
  assert(!editor.AwaitingAcknowledgment());
  assert(editor.ToggleAutomation(preferences) && preferences.afterDownload);

  auto a = group("folder-a", "Alpha");
  auto b = group("folder-b", "Beta");
  auto child = a;
  child.trackId = "chart-a";
  child.groupRepresentative = false;
  child.expandedChart = true;
  child.title = "Another difficulty";
  child.representativeChart.SHA256 = "HASHneedle";
  GroupedChartListModel model;
  model.SetGroups({a, b});
  model.SetChildren(a.groupId, {child});
  model.SetQuery("  ALPHA  ");
  assert(model.Rows().size() == 1 && model.Rows()[0].trackId == a.trackId);
  model.SetExpanded(a.groupId, true);
  assert(model.Rows().empty()); // Expanded children replace the representative.
  model.SetQuery("hashNEEDLE");
  assert(model.Rows().size() == 1 && model.Rows()[0].expandedChart);
  model.SetExpanded(a.groupId, false);
  assert(model.Rows().empty()); // Collapsed groups do not search unloaded children.
  model.SetExpanded(b.groupId, true);
  model.SetQuery("beta");
  assert(model.Rows().size() == 1); // No child cache: retain representative.

  StorageGroupSelection selection;
  selection.SetGroups({a, b});
  selection.Toggle(a);
  model.SetQuery("beta");
  assert(selection.Count() == 1 && selection.HiddenCount(model.Rows()) == 1);
  selection.Toggle(child);
  assert(selection.Count() == 0); // Child checkbox always addresses the whole folder.
  selection.Toggle(child);
  selection.SetGroups({b, a});
  assert(selection.SelectedGroups().size() == 1);
  assert(selection.SelectedGroups()[0].groupId == a.groupId);
  a.title = "Updated metadata";
  selection.SetGroups({a, b});
  assert(selection.SelectedGroups()[0].title == a.title);
  model.SetQuery("");
  model.SetExpanded(a.groupId, true);
  assert(selection.HiddenCount(model.Rows()) == 0);
  selection.SetGroups({b});
  assert(selection.Count() == 0); // Removed folders cannot linger as mutation targets.

  // Interleaved songs stay together by full archive identity, not basename.
  auto c = group("folder-c", "Gamma"), loose = group("local", "Local");
  StorageGroupArchives archives{{a.groupId, "/one/pack.zip"}, {b.groupId, "/two/pack.zip"},
      {c.groupId, "/one/pack.zip"}};
  selection.SetGroups({a, b, c, loose});
  selection.Toggle(a);
  auto rows = MakeStorageArchiveRows({a, b, loose, c}, archives, selection);
  assert(rows.size() == 6);
  assert(rows[0].archiveHeader && rows[0].archivePath == "/one/pack.zip");
  assert(rows[0].songCount == 2 && rows[0].selectedCount == 1);
  assert(rows[0].visibleCount == 2 && rows[0].visibleSelectedCount == 1);
  assert(rows[1].track.groupId == a.groupId && rows[2].track.groupId == c.groupId);
  assert(rows[3].archiveHeader && rows[3].archivePath == "/two/pack.zip");
  assert(rows[4].track.groupId == b.groupId && rows[5].track.groupId == loose.groupId);
  assert(rows[0].IndentLevel() == 0 && rows[1].IndentLevel() == 1 && rows[5].IndentLevel() == 0);

  // Filtered batch actions never add hidden songs or count chart variants twice.
  auto child2 = child;
  child2.trackId = "chart-a-2";
  rows = MakeStorageArchiveRows({child, child2, b}, archives, selection);
  assert(rows[0].songCount == 2 && rows[0].visibleCount == 1 && rows[0].visibleSelectedCount == 1);
  assert(rows[1].IndentLevel() == 2 && rows[2].IndentLevel() == 2);
  ToggleStorageArchiveSelection("/one/pack.zip", {child, child2, b}, archives, selection);
  assert(selection.Count() == 0);
  ToggleStorageArchiveSelection("/one/pack.zip", {child, child2, b}, archives, selection);
  assert(selection.Count() == 1 && selection.Contains(a.groupId) && !selection.Contains(c.groupId));
  ToggleStorageArchiveSelection("/one/pack.zip", {a, b, c}, archives, selection);
  assert(selection.Count() == 2 && selection.Contains(c.groupId) && !selection.Contains(b.groupId));
  selection.Toggle(child); // A single song can still be deselected within a batch.
  assert(selection.Count() == 1 && selection.Contains(c.groupId));
  rows = MakeStorageArchiveRows({a}, archives, selection);
  assert(rows[0].selectedCount == 1 && rows[0].visibleSelectedCount == 0);
  ToggleStorageArchiveSelection("/one/pack.zip", {a}, archives, selection);
  ToggleStorageArchiveSelection("/one/pack.zip", {a}, archives, selection);
  assert(selection.Count() == 1 && selection.Contains(c.groupId)); // Hidden selection survives.
  assert(MakeStorageArchiveRows({}, archives, selection).empty());
  loose.expandedChart = true;
  rows = MakeStorageArchiveRows({loose}, archives, selection);
  assert(rows.size() == 1 && rows[0].IndentLevel() == 1);
}
