#include "SkinStoragePaths.h"

#include "../FileChecksum.h"
#include "../Utils.h"
#include "../targets.h"
#include "SkinProfileSettings.h"
#include "package/SkinPathPolicy.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#if TARGET_OS_ANDROID
#include "../AndroidNatives.h"
#endif

#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
#include "../iOSNatives.hpp"
#endif

namespace skin {
namespace {

constexpr std::string_view kOverlayIdentityMagic = "ASOBMSKIN-OVERLAY-V1";

SkinPrivateOverlayPathResult invalidOverlayIdentity() {
  return {.failure = SkinDiagnostic{
              .code = "skin_overlay_identity_invalid",
              .message = "private skin overlay identity is invalid",
              .severity = DiagnosticSeverity::Error,
          }};
}

void appendU32(std::string &framed, std::uint32_t value) {
  for (int shift : {24, 16, 8, 0}) {
    framed.push_back(static_cast<char>((value >> shift) & 0xffU));
  }
}

void appendField(std::string &framed, std::string_view value) {
  appendU32(framed, static_cast<std::uint32_t>(value.size()));
  framed.append(value);
}

} // namespace

SkinStorageRoots deriveSkinStorageRoots(std::filesystem::path visiblePackages,
                                        std::filesystem::path privateRoot) {
  privateRoot = privateRoot.lexically_normal();
  if (privateRoot.empty() || !privateRoot.is_absolute()) {
    return {.visiblePackages = std::move(visiblePackages)};
  }
  return {.visiblePackages = std::move(visiblePackages),
          .privateRevisions = privateRoot / "revisions",
          .privateCatalog = privateRoot / "catalog",
          .profileOverlays = privateRoot / "profileOverlays"};
}

#if !defined(ASOBMASHOW_SKIN_STORAGE_PATHS_NO_PLATFORM_DEFAULTS)
SkinStorageRoots defaultSkinStorageRoots() {
  const std::filesystem::path visible = Utils::GetDocumentsPath("Skins");
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  // Skin payloads are read directly from Documents/Skins so Files edits are
  // immediately authoritative and are never duplicated under Documents.
  const std::filesystem::path support = GetIOSApplicationSupportPath();
  auto roots = deriveSkinStorageRoots(
      visible, support.empty() ? std::filesystem::path{} : support / "Skins");
  roots.liveSources = true;
  return roots;
#elif TARGET_OS_ANDROID
  std::filesystem::path privateRoot;
  const std::filesystem::path internalFiles = GetAndroidInternalFilesDir();
  if (!internalFiles.empty()) {
    privateRoot = internalFiles / ".asobmashow-private" / "Skins";
  }
  auto roots = deriveSkinStorageRoots(visible, privateRoot);
  roots.liveSources = true;
  return roots;
#else
  std::filesystem::path privateRoot;
  const std::filesystem::path applicationRoot = Utils::GetDocumentsPath();
  if (!applicationRoot.empty()) {
    privateRoot = applicationRoot / ".asobmashow-private" / "Skins";
  }
  auto roots = deriveSkinStorageRoots(visible, privateRoot);
  roots.liveSources = true;
  return roots;
#endif
}
#endif

SkinPrivateOverlayPathResult
deriveSkinPrivateOverlayRoot(const SkinStorageRoots &roots,
                             const SkinProfileId &profile,
                             const SkinEntryId &entry) {
  const auto validatedProfile = makeSkinProfileId(profile.opaque);
  if (!validatedProfile) {
    return invalidOverlayIdentity();
  }
  std::string normalizedProfile = validatedProfile->opaque;
  std::transform(normalizedProfile.begin(), normalizedProfile.end(),
                 normalizedProfile.begin(), [](unsigned char value) {
                   if (value >= 'A' && value <= 'F') {
                     return static_cast<char>(value - 'A' + 'a');
                   }
                   return static_cast<char>(value);
                 });

  const auto package = normalizePackageId(entry.package.directoryName);
  if (!package.package ||
      package.package->collisionKey != entry.package.collisionKey) {
    return invalidOverlayIdentity();
  }
  const auto normalizedEntry =
      normalizeEntryPath(*package.package, entry.packageRelativePath);
  if (!normalizedEntry.entry ||
      normalizedEntry.entry->collisionKey != entry.collisionKey) {
    return invalidOverlayIdentity();
  }

  const std::filesystem::path base = roots.profileOverlays.lexically_normal();
  if (base.empty() || !base.is_absolute()) {
    return invalidOverlayIdentity();
  }

  std::string framed;
  framed.reserve(kOverlayIdentityMagic.size() + normalizedProfile.size() +
                 package.package->collisionKey.size() +
                 normalizedEntry.entry->collisionKey.size() + 13);
  framed.append(kOverlayIdentityMagic);
  framed.push_back('\0');
  appendField(framed, normalizedProfile);
  appendField(framed, package.package->collisionKey);
  appendField(framed, normalizedEntry.entry->collisionKey);

  const std::filesystem::path root = base / file_checksum::sha256(framed);
  if (root.parent_path() != base) {
    return invalidOverlayIdentity();
  }
  return {.root = root};
}

} // namespace skin
