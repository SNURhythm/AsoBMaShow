#include "SkinPackageCatalog.h"

#include <mutex>
#include <utility>

namespace skin {

struct SkinPackageCatalog::Impl {
  explicit Impl(std::filesystem::path rootPath)
      : root(std::move(rootPath)),
        current(std::make_shared<SkinPackageCatalogSnapshot>()) {}

  std::filesystem::path root;
  mutable std::mutex mutex;
  std::shared_ptr<const SkinPackageCatalogSnapshot> current;
  bool stopped = false;
};

SkinPackageCatalog::SkinPackageCatalog(std::filesystem::path privateCatalogRoot)
    : impl_(std::make_unique<Impl>(std::move(privateCatalogRoot))) {}

SkinPackageCatalog::~SkinPackageCatalog() { shutdown(); }

void SkinPackageCatalog::recover() {
  // Deliberate Task 7 RED scaffold. Recovery and journal replay are specified
  // by skin_package_store_red_tests and implemented in the next phase.
}

std::shared_ptr<const SkinPackageCatalogSnapshot>
SkinPackageCatalog::snapshot() const noexcept {
  std::scoped_lock lock(impl_->mutex);
  return impl_->current;
}

void SkinPackageCatalog::flush() {}

void SkinPackageCatalog::shutdown() noexcept {
  if (!impl_) {
    return;
  }
  std::scoped_lock lock(impl_->mutex);
  impl_->stopped = true;
}

} // namespace skin
