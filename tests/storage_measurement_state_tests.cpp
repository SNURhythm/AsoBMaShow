#include "../src/storage/StorageMeasurementState.h"
#include "StorageTestSupport.h"
#include <iostream>
using namespace storage_management;
using storage_test::require;
namespace {
void subsetApplyNeverStartsFullMeasurement() {
  const std::vector<FolderSelection> groups{{"parent", "/library"}, {"selected", "/library/song"},
      {"other", "/library/other"}};
  StorageMeasurementRows rows{
      {"parent", {.groupId = "parent", .totalBytes = 120}},
      {"selected", {.groupId = "selected", .totalBytes = 100, .audioBytes = 100, .sourceIdentity = "before", .generation = 7}},
      {"other", {.groupId = "other", .totalBytes = 20}}};
  StorageReport report{.changedRoots = {"/library/song"}, .groupDeltas = {
      {.groupId = "selected", .bytesBefore = 100, .bytesAfter = 60, .audioBefore = 100, .audioAfter = 60,
       .baselineBytes = 100, .sourceIdentity = "before", .nextSourceIdentity = "after", .baselineGeneration = 7}}};
  const auto refresh = ApplyStorageReportMeasurements(rows, groups, report);
  require(rows.at("selected").totalBytes == 60 && rows.at("selected").audioBytes == 60,
      "subset conversion applies known committed deltas without a new size walk");
  require(refresh.empty() && !rows.at("parent").totalBytes && rows.at("other").totalBytes == 20,
      "uncertain ancestors remain unavailable without recursively refreshing unrelated siblings");
  rows.at("selected").generation = 8;
  require(ApplyStorageReportMeasurements(rows, groups, report).contains("selected") && !rows.at("selected").totalBytes,
      "stale report generation cannot change a newer measurement");
}
void jobRefreshPreservesUnrelatedSizes() {
  StorageMeasurementRows rows{{"unchanged", {.groupId = "unchanged", .totalBytes = 42}},
      {"changed", {.groupId = "changed", .totalBytes = 12}}};
  std::map<std::string, StorageGroupSource> previous{
      {"unchanged", {{"unchanged", "/one"}, "v1"}}, {"changed", {{"changed", "/two"}, "v1"}}};
  const std::vector<StorageGroupSource> current{{{"unchanged", "/one"}, "v1"},
      {{"changed", "/two"}, "v2"}, {{"added", "/three"}, "v1"}};
  const auto refresh = ReconcileStorageMeasurements(rows, previous, current, false);
  require(refresh == std::set<std::string>{"changed", "added"} && rows.size() == 1 && rows.at("unchanged").totalBytes == 42,
      "job-related list reload retains completed sizes for unchanged sources");
  require(ReconcileStorageMeasurements(rows, previous, current, true).size() == 3 && rows.empty(),
      "explicit refresh may invalidate the full measurement scope");
}
void sourceChangesInvalidateContainingRollups() {
  StorageMeasurementRows rows{{"parent", {.groupId = "parent", .totalBytes = 100}},
      {"sibling", {.groupId = "sibling", .totalBytes = 50}}};
  std::map<std::string, StorageGroupSource> previous{
      {"parent", {{"parent", "/parent"}, "same"}}, {"sibling", {{"sibling", "/sibling"}, "same"}}};
  const std::vector<StorageGroupSource> current{{{"parent", "/parent"}, "same"},
      {{"sibling", "/sibling"}, "same"}, {{"child", "/parent/child"}, "new"}};
  const auto refresh = ReconcileStorageMeasurements(rows, previous, current, false);
  require(refresh == std::set<std::string>{"child"} && !rows.at("parent").totalBytes && rows.at("sibling").totalBytes == 50,
      "new nested source invalidates its containing totals only");
}

void dirtyScopesInvalidateStableRepresentative() {
  StorageMeasurementRows rows{{"parent", {.groupId = "parent", .totalBytes = 100}},
      {"song", {.groupId = "song", .totalBytes = 60}}, {"other", {.groupId = "other", .totalBytes = 40}}};
  const std::vector<StorageGroupSource> current{{{"parent", "/library"}, "same"},
      {{"song", "/library/song"}, "same"}, {{"other", "/library/other"}, "same"}};
  std::map<std::string, StorageGroupSource> previous;
  for (const auto &source : current) previous[source.selection.groupId] = source;
  const auto refresh = ReconcileStorageMeasurements(rows, previous, current, false, {"/library/song"});
  require(refresh == std::set<std::string>{"song"} && !rows.contains("song") &&
      !rows.at("parent").totalBytes && rows.at("other").totalBytes == 40,
      "secondary chart/resource-only revision invalidates its scope with stable representative and count");
  require(ReconcileStorageMeasurements(rows, previous, current, true).size() == 3 && rows.empty(),
      "unknown revision conservatively invalidates every source");
}

void invalidationUsesSourceCaseRules() {
  storage_test::TempDirectory temp;
  const auto library = temp.root / "library";
  std::filesystem::create_directory(library);
  for (const auto kind : {StorageSourceKind::Local, StorageSourceKind::Archive}) {
    const std::filesystem::path base = kind == StorageSourceKind::Local ? library : library / "pack.zip";
    const std::vector<FolderSelection> groups{
        {.groupId = "parent", .path = base, .sourceKind = kind},
        {.groupId = "upper", .path = base / "Song", .sourceKind = kind},
        {.groupId = "lower", .path = base / "song", .sourceKind = kind}};
    const StorageMeasurementRows original{{"parent", {.groupId = "parent", .totalBytes = 100}},
        {"upper", {.groupId = "upper", .totalBytes = 60}}, {"lower", {.groupId = "lower", .totalBytes = 40}}};
    std::set<std::string> expected{"upper"};
#ifdef _WIN32
    if (kind == StorageSourceKind::Local) expected.insert("lower");
#endif
    auto rows = original;
    const StorageReport report{.changedRoots = {base / "Song"}};
    require(ApplyStorageReportMeasurements(rows, groups, report) == expected && !rows.at("parent").totalBytes &&
        !rows.at("upper").totalBytes && rows.at("lower").totalBytes.has_value() == !expected.contains("lower"),
        "Apply invalidates native local aliases while preserving case-distinct archive folders");
    std::vector<StorageGroupSource> current;
    std::map<std::string, StorageGroupSource> previous;
    for (const auto &group : groups) {
      current.push_back({group, "same"}); previous[group.groupId] = current.back();
    }
    rows = original;
    require(ReconcileStorageMeasurements(rows, previous, current, false, report.changedRoots) == expected &&
        !rows.at("parent").totalBytes && !rows.contains("upper") && rows.contains("lower") == !expected.contains("lower"),
        "dirty-scope refresh uses local filesystem casing and exact archive entry casing");
  }
}
}
int main() {
  try { subsetApplyNeverStartsFullMeasurement(); jobRefreshPreservesUnrelatedSizes(); sourceChangesInvalidateContainingRollups(); dirtyScopesInvalidateStableRepresentative(); invalidationUsesSourceCaseRules(); }
  catch (const std::exception &error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
