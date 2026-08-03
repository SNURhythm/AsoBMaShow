#include <iostream>
#include <string>
#include <string_view>

#include "skin/SkinPresentationTypes.h"
#include "skin/package/SkinPathPolicy.h"

namespace {

int failures = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testFilenameNormalizationRejectsInvalidText() {
  using namespace skin;
  expect(!normalizeSkinSourceNameNfc("\xFF").value.has_value(),
         "invalid UTF-8 is rejected");
  expect(!normalizeSkinSourceNameNfc(std::string("contains\0nul", 12)).value.has_value(),
         "embedded NUL is rejected");
  expect(!normalizeSkinSourceNameNfc("").value.has_value(),
         "empty source name is rejected");
  expect(!normalizeSkinSourceNameNfc(".").value.has_value(),
         "dot source name is rejected");
  expect(!normalizeSkinSourceNameNfc("..").value.has_value(),
         "dotdot source name is rejected");
}

void testPackageIdentityPreservesNfcAndUsesFullCaseFold() {
  using namespace skin;
  const auto composed = normalizePackageId("Stra\xC3\x9F" "e");
  const auto uppercase = normalizePackageId("STRASSE");
  const auto decomposed = normalizePackageId("Cafe\xCC\x81");
  expect(composed.package.has_value(), "Unicode package name is accepted");
  expect(uppercase.package.has_value(), "uppercase package name is accepted");
  expect(decomposed.package.has_value(), "decomposed package name is accepted");
  if (composed.package && uppercase.package) {
    expect(composed.package->collisionKey == uppercase.package->collisionKey,
           "full casefold collides Straße and STRASSE");
    expect(composed.package->directoryName == "Stra\xC3\x9F" "e",
           "package retains authored NFC spelling");
  }
  if (decomposed.package) {
    expect(decomposed.package->directoryName == "Caf\xC3\xA9",
           "package spelling is stored as NFC");
  }
}

void testPackageNameRejectsPathAndSizeViolations() {
  using namespace skin;
  expect(!normalizePackageId("folder/name").package.has_value(),
         "package must be a direct-child name");
  expect(!normalizePackageId("folder\\name").package.has_value(),
         "backslash package separator is rejected");
  expect(!normalizePackageId("/absolute").package.has_value(),
         "absolute package path is rejected");
  expect(!normalizePackageId("C:/drive").package.has_value(),
         "drive package path is rejected");
  expect(!normalizePackageId("//server/share").package.has_value(),
         "UNC package path is rejected");
  expect(!normalizePackageId(std::string(129, 'x')).package.has_value(),
         "package byte limit is enforced");
}

void testEntryIdentityStaysPackageRelative() {
  using namespace skin;
  const auto package = normalizePackageId("ModernChic").package;
  expect(package.has_value(), "fixture package is valid");
  if (!package) {
    return;
  }
  const auto entry = normalizeEntryPath(*package, "play/play7.luaskin");
  expect(entry.entry.has_value(), "package-relative entry is accepted");
  if (entry.entry) {
    expect(entry.entry->package == *package,
           "entry keeps its typed package identity");
    expect(entry.entry->packageRelativePath == "play/play7.luaskin",
           "entry contains only its package-relative path");
    expect(installedRelativePath(*entry.entry) == "ModernChic/play/play7.luaskin",
           "installed path is virtual package plus entry path only");
  }
}

void testEntryRejectsUnsafeAndOversizedPaths() {
  using namespace skin;
  const auto package = normalizePackageId("ModernChic").package;
  if (!package) {
    ++failures;
    return;
  }
  for (const std::string_view path : {"", "/absolute.luaskin", "C:/drive.luaskin",
                                      "//server/share.luaskin", "./entry.luaskin",
                                      "dir/../entry.luaskin", "dir//entry.luaskin",
                                      "dir\\entry.luaskin"}) {
    expect(!normalizeEntryPath(*package, path).entry.has_value(),
           "unsafe virtual entry path is rejected");
  }
  expect(!normalizeEntryPath(*package, std::string("entry\0nul.luaskin", 17))
              .entry.has_value(),
         "entry path containing NUL is rejected");
  std::string tooDeep;
  for (int component = 0; component < 65; ++component) {
    if (!tooDeep.empty()) {
      tooDeep += '/';
    }
    tooDeep += 'x';
  }
  expect(!normalizeEntryPath(*package, tooDeep).entry.has_value(),
         "entry component limit is enforced");
  expect(!normalizeEntryPath(*package, std::string(1025, 'x')).entry.has_value(),
         "entry byte limit is enforced");
}

void testRevisionContainmentUsesOnlyTheEntryRelativePath() {
  using namespace skin;
  const auto package = normalizePackageId("ModernChic").package;
  if (!package) {
    ++failures;
    return;
  }
  const auto entry = normalizeEntryPath(*package, "play/play7.luaskin").entry;
  if (!entry) {
    ++failures;
    return;
  }
  const std::string injectedRevisionRoot = "/private/runtime/revisions/opaque";
  const std::string openPath =
      injectedRevisionRoot + "/" + entry->packageRelativePath;
  expect(openPath == "/private/runtime/revisions/opaque/play/play7.luaskin",
         "runtime containment appends only package-relative entry spelling");
  expect(installedRelativePath(*entry).find(injectedRevisionRoot) ==
             std::string::npos,
         "public installed identity never contains the revision root");
}

void testEntryNormalizesNfcWithoutChangingAuthoredPathShape() {
  using namespace skin;
  const auto package = normalizePackageId("ModernChic").package;
  if (!package) {
    ++failures;
    return;
  }
  const auto composed = normalizeEntryPath(*package, "play/Caf\xC3\xA9.luaskin");
  const auto decomposed = normalizeEntryPath(*package, "play/Cafe\xCC\x81.luaskin");
  expect(composed.entry.has_value() && decomposed.entry.has_value(),
         "both NFC spellings are valid entries");
  if (composed.entry && decomposed.entry) {
    expect(composed.entry->packageRelativePath == "play/Caf\xC3\xA9.luaskin",
           "authored NFC entry spelling is retained");
    expect(decomposed.entry->packageRelativePath == "play/Caf\xC3\xA9.luaskin",
           "decomposed entry spelling becomes NFC");
    expect(composed.entry->collisionKey == decomposed.entry->collisionKey,
           "NFC-equivalent entries share a collision key");
  }
}

void testFloatWriterIdIsAStrongTruthyToken() {
  using namespace skin;
  const SkinFloatWriterId empty{};
  const SkinFloatWriterId writer{7};
  expect(!static_cast<bool>(empty), "zero writer token is false");
  expect(static_cast<bool>(writer), "nonzero writer token is true");
  expect(writer != empty, "writer token compares by its typed value");
}

} // namespace

int main() {
  testFilenameNormalizationRejectsInvalidText();
  testPackageIdentityPreservesNfcAndUsesFullCaseFold();
  testPackageNameRejectsPathAndSizeViolations();
  testEntryIdentityStaysPackageRelative();
  testEntryRejectsUnsafeAndOversizedPaths();
  testEntryNormalizesNfcWithoutChangingAuthoredPathShape();
  testRevisionContainmentUsesOnlyTheEntryRelativePath();
  testFloatWriterIdIsAStrongTruthyToken();
  return failures == 0 ? 0 : 1;
}
