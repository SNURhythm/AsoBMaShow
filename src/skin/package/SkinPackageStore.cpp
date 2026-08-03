#include "SkinPackageStore.h"

#include <utility>

namespace skin {

SkinPackageStore::SkinPackageStore(
    SkinStorageRoots roots, SkinPackageCatalog &catalog,
    SkinAliasDetector &aliases, ISkinProfileSnapshotProvider &profileSnapshots)
    : roots_(std::move(roots)), catalog_(catalog), aliases_(aliases),
      profileSnapshots_(profileSnapshots) {}

SkinRecoveryResult SkinPackageStore::recoverBeforeServiceStart() {
  // Deliberate Task 7 RED scaffold. Cross-root journal replay requires this
  // store-owned root set and is specified by skin_package_store_red_tests.
  return {};
}

PreparePackageResult SkinPackageStore::prepareArchive(
    const std::filesystem::path &zip, const SkinPackageId &package,
    std::stop_token stop, SkinProgressCallback progress) {
  return SkinArchiveImporter(roots_, aliases_)
      .prepareArchive(zip, package, stop, std::move(progress));
}

PreparePackageResult SkinPackageStore::prepareFolder(
    const std::filesystem::path &folder, const SkinPackageId &package,
    std::stop_token stop, SkinProgressCallback progress) {
  return SkinArchiveImporter(roots_, aliases_)
      .prepareFolder(folder, package, stop, std::move(progress));
}

PublishPackageResult
SkinPackageStore::publish(PreparedPackage &&prepared, PackageCollisionPolicy,
                          ProfileInventorySnapshot, SkinEntryValidator &,
                          std::stop_token, SkinProgressCallback) {
  PublishPackageResult result;
  result.package = prepared.packageId();
  result.entries.assign(prepared.entries().begin(), prepared.entries().end());
  result.diagnostics.push_back(
      {.code = "skin.package.store.unimplemented",
       .message = "transactional package publication is not implemented",
       .severity = DiagnosticSeverity::Error});
  return result;
}

std::shared_ptr<const SkinPackageCatalogSnapshot>
SkinPackageStore::catalogSnapshot() const noexcept {
  return catalog_.snapshot();
}

} // namespace skin
