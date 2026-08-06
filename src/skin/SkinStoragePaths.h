#pragma once

#include "package/SkinPackageTypes.h"

#include <filesystem>
#include <optional>

namespace skin {

struct SkinProfileId;

struct SkinStorageRoots {
  std::filesystem::path visiblePackages;
  std::filesystem::path privateRevisions;
  std::filesystem::path privateCatalog;
  std::filesystem::path profileOverlays;
  // A live-source store keeps Documents/Skins as the sole package payload
  // location. privateRevisions remains only for backwards-compatible callers;
  // no revision tree is created when this is set.
  bool liveSources = false;
};

SkinStorageRoots deriveSkinStorageRoots(std::filesystem::path visiblePackages,
                                        std::filesystem::path privateRoot);

SkinStorageRoots defaultSkinStorageRoots();

struct SkinPrivateOverlayPathResult {
  std::optional<std::filesystem::path> root;
  std::optional<SkinDiagnostic> failure;
};

SkinPrivateOverlayPathResult
deriveSkinPrivateOverlayRoot(const SkinStorageRoots &, const SkinProfileId &,
                             const SkinEntryId &);

} // namespace skin
