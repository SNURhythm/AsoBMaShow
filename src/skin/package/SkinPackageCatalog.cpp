#include "SkinPackageCatalog.h"

#include "../../AtomicFile.h"
#include "../../FileChecksum.h"
#include "../../targets.h"
#include "../beatoraja/SkinDiagnosticHistory.h"
#include "SkinPathPolicy.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "SkinIOSFileOpenCompatibility.h"

namespace skin {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

constexpr int kCatalogSchemaVersion = 1;
constexpr std::uint64_t kMaximumCatalogBytes = 32ULL * 1024 * 1024;
constexpr int kDiagnosticHistorySchemaVersion = 1;
constexpr std::string_view kDiagnosticHistoryFile = "diagnostic-history.json";

bool ensureDirectoryNoFollow(const fs::path &directory) {
#if TARGET_OS_IOS || TARGET_OS_SIMULATOR
  std::error_code error;
  if (directory.empty()) {
    return false;
  }
  fs::create_directories(directory, error);
  return !error && fs::is_directory(directory, error) && !error;
#else
  try {
    std::error_code error;
    const fs::path absolute = fs::absolute(directory, error).lexically_normal();
    if (error || !absolute.is_absolute() || absolute.root_path().empty()) {
      return false;
    }
#if defined(_WIN32)
    fs::path current = absolute.root_path();
    std::vector<HANDLE> retained;
    HANDLE root = CreateFileW(
        current.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (root == INVALID_HANDLE_VALUE) {
      return false;
    }
    retained.push_back(root);
    const fs::path relative = absolute.lexically_relative(current);
    for (const fs::path &component : relative) {
      if (component.empty() || component == "." || component == "..") {
        for (HANDLE handle : retained) {
          CloseHandle(handle);
        }
        return false;
      }
      current /= component;
      if (!CreateDirectoryW(current.c_str(), nullptr) &&
          GetLastError() != ERROR_ALREADY_EXISTS) {
        for (HANDLE handle : retained) {
          CloseHandle(handle);
        }
        return false;
      }
      HANDLE handle = CreateFileW(
          current.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      FILE_ATTRIBUTE_TAG_INFO attributes{};
      const bool valid =
          handle != INVALID_HANDLE_VALUE &&
          GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
                                       &attributes, sizeof(attributes)) &&
          (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
          (attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      if (!valid) {
        if (handle != INVALID_HANDLE_VALUE) {
          CloseHandle(handle);
        }
        for (HANDLE retainedHandle : retained) {
          CloseHandle(retainedHandle);
        }
        return false;
      }
      retained.push_back(handle);
    }
    for (HANDLE handle : retained) {
      CloseHandle(handle);
    }
    return true;
#else
    int current = ::open(absolute.root_path().c_str(),
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (current < 0) {
      return false;
    }
    const fs::path relative = absolute.lexically_relative(absolute.root_path());
    for (const fs::path &component : relative) {
      if (component.empty() || component == "." || component == "..") {
        ::close(current);
        return false;
      }
      int next = ::openat(current, component.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (next < 0 && errno == ENOENT &&
          ::mkdirat(current, component.c_str(), 0700) == 0) {
        next = ::openat(current, component.c_str(),
                        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      }
      ::close(current);
      if (next < 0) {
        return false;
      }
      current = next;
    }
    ::close(current);
    return true;
#endif
  } catch (...) {
    return false;
  }
#endif
}

std::optional<std::string> readBoundedCatalogFile(const fs::path &path,
                                                  bool &missing) {
  missing = false;
#if defined(_WIN32)
  HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    missing = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    return std::nullopt;
  }
  FILE_ATTRIBUTE_TAG_INFO tags{};
  LARGE_INTEGER size{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tags,
                                    sizeof(tags)) ||
      (tags.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
      (tags.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      GetFileType(handle) != FILE_TYPE_DISK || !GetFileSizeEx(handle, &size) ||
      size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > kMaximumCatalogBytes) {
    CloseHandle(handle);
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD amount = 0;
    if (!ReadFile(handle, bytes.data() + offset,
                  static_cast<DWORD>(
                      std::min<std::size_t>(bytes.size() - offset, MAXDWORD)),
                  &amount, nullptr) ||
        amount == 0) {
      CloseHandle(handle);
      return std::nullopt;
    }
    offset += amount;
  }
  char extra = 0;
  DWORD extraAmount = 0;
  const bool exact = ReadFile(handle, &extra, 1, &extraAmount, nullptr) != 0 &&
                     extraAmount == 0;
  CloseHandle(handle);
  if (!exact) {
    return std::nullopt;
  }
#else
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    missing = errno == ENOENT;
    return std::nullopt;
  }
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_nlink != 1 || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > kMaximumCatalogBytes) {
    ::close(descriptor);
    return std::nullopt;
  }
  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t amount =
        ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (amount < 0 && errno == EINTR) {
      continue;
    }
    if (amount <= 0) {
      ::close(descriptor);
      return std::nullopt;
    }
    offset += static_cast<std::size_t>(amount);
  }
  char extra = 0;
  const ssize_t extraAmount = ::read(descriptor, &extra, 1);
  ::close(descriptor);
  if (extraAmount != 0) {
    return std::nullopt;
  }
#endif
  return bytes;
}

bool lowercaseSha256(std::string_view digest) {
  return digest.size() == 64 &&
         std::ranges::all_of(digest, [](unsigned char character) {
           return std::isdigit(character) != 0 ||
                  (character >= 'a' && character <= 'f');
         });
}

SkinDiagnostic catalogDiagnostic(std::string code, std::string message) {
  return {.code = std::move(code),
          .message = std::move(message),
          .severity = DiagnosticSeverity::Error};
}

std::string validationName(SkinValidationDisposition value) {
  switch (value) {
  case SkinValidationDisposition::SelectableGameplay:
    return "selectableGameplay";
  case SkinValidationDisposition::UnavailableType:
    return "unavailableType";
  case SkinValidationDisposition::Invalid:
    return "invalid";
  }
  return "invalid";
}

std::optional<SkinValidationDisposition> parseValidation(const Json &value) {
  if (!value.is_string()) {
    return std::nullopt;
  }
  const std::string encoded = value.get<std::string>();
  if (encoded == "selectableGameplay" || encoded == "selectable7Key") {
    return SkinValidationDisposition::SelectableGameplay;
  }
  if (encoded == "unavailableType") {
    return SkinValidationDisposition::UnavailableType;
  }
  if (encoded == "invalid") {
    return SkinValidationDisposition::Invalid;
  }
  return std::nullopt;
}

OrderedJson encodePackage(const SkinPackageId &package) {
  OrderedJson result = OrderedJson::object();
  result["directoryName"] = package.directoryName;
  result["collisionKey"] = package.collisionKey;
  return result;
}

OrderedJson encodeEntryId(const SkinEntryId &entry) {
  OrderedJson result = OrderedJson::object();
  result["package"] = encodePackage(entry.package);
  result["packageRelativePath"] = entry.packageRelativePath;
  result["collisionKey"] = entry.collisionKey;
  return result;
}

OrderedJson encodeDiagnostic(const SkinDiagnostic &diagnostic) {
  OrderedJson result = OrderedJson::object();
  result["code"] = diagnostic.code;
  result["message"] = diagnostic.message;
  result["virtualPath"] = diagnostic.virtualPath;
  result["severity"] = static_cast<std::uint8_t>(diagnostic.severity);
  if (diagnostic.source) {
    OrderedJson source = OrderedJson::object();
    source["virtualPath"] = diagnostic.source->virtualPath;
    source["line"] = diagnostic.source->line;
    source["column"] = diagnostic.source->column;
    result["source"] = std::move(source);
  }
  return result;
}

OrderedJson encodeMetadata(const SkinEntryMetadataSnapshot &metadata) {
  OrderedJson result = OrderedJson::object();
  result["displayName"] = metadata.displayName;
  result["author"] = metadata.author;
  result["skinType"] = metadata.skinType;
  result["authoredWidth"] = metadata.authoredWidth;
  result["authoredHeight"] = metadata.authoredHeight;
  result["categories"] = OrderedJson::array();
  for (const auto &category : metadata.categories) {
    result["categories"].push_back(
        OrderedJson{{"name", category.name}, {"items", category.items}});
  }
  result["options"] = OrderedJson::array();
  for (const auto &option : metadata.options) {
    OrderedJson encoded = OrderedJson::object();
    encoded["category"] = option.category;
    encoded["name"] = option.name;
    encoded["choices"] = OrderedJson::array();
    for (const auto &choice : option.choices) {
      encoded["choices"].push_back(
          OrderedJson{{"label", choice.label}, {"value", choice.value}});
    }
    encoded["defaultLabel"] = option.defaultLabel;
    result["options"].push_back(std::move(encoded));
  }
  result["files"] = OrderedJson::array();
  for (const auto &file : metadata.files) {
    result["files"].push_back(OrderedJson{{"category", file.category},
                                          {"name", file.name},
                                          {"pattern", file.pattern},
                                          {"defaultValue", file.defaultValue},
                                          {"choices", file.choices}});
  }
  result["offsets"] = OrderedJson::array();
  for (const auto &offset : metadata.offsets) {
    result["offsets"].push_back(
        OrderedJson{{"category", offset.category},
                    {"name", offset.name},
                    {"id", offset.id},
                    {"permissions", offset.permissions}});
  }
  return result;
}

OrderedJson encodeCatalog(const SkinPackageCatalogSnapshot &snapshot) {
  OrderedJson result = OrderedJson::object();
  result["schemaVersion"] = kCatalogSchemaVersion;
  result["catalogGeneration"] = snapshot.catalogGeneration;
  result["sourceGeneration"] = snapshot.sourceGeneration;
  result["packages"] = OrderedJson::array();
  for (const SkinPackageId &package : snapshot.packages) {
    result["packages"].push_back(encodePackage(package));
  }
  result["entries"] = OrderedJson::array();
  for (const SkinCatalogEntrySnapshot &entry : snapshot.entries) {
    OrderedJson encoded = OrderedJson::object();
    encoded["entry"] = encodeEntryId(entry.entry);
    encoded["revisionDigest"] = entry.revisionDigest;
    encoded["validation"] = validationName(entry.validation);
    if (entry.metadata) {
      encoded["metadata"] = encodeMetadata(*entry.metadata);
    }
    encoded["validatedConfigurationDigests"] =
        entry.validatedConfigurationDigests;
    if (!entry.diagnostics.empty()) {
      encoded["diagnostics"] = OrderedJson::array();
      for (const SkinDiagnostic &diagnostic : entry.diagnostics) {
        encoded["diagnostics"].push_back(encodeDiagnostic(diagnostic));
      }
    }
    result["entries"].push_back(std::move(encoded));
  }
  return result;
}

bool boundedText(std::string_view value) {
  return value.size() <= SkinPackagePolicy::maxPathBytes &&
         value.find('\0') == std::string_view::npos;
}

bool safeVirtualPath(std::string_view value) {
  if (!boundedText(value)) {
    return false;
  }
  if (value.empty()) {
    return true;
  }
  if (value.starts_with('/') || value.starts_with('\\') ||
      value.find('\\') != std::string_view::npos ||
      value.find(':') != std::string_view::npos) {
    return false;
  }
  std::size_t start = 0;
  while (start <= value.size()) {
    const std::size_t separator = value.find('/', start);
    const std::string_view component = value.substr(
        start, separator == std::string_view::npos ? value.size() - start
                                                   : separator - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    if (separator == std::string_view::npos) {
      break;
    }
    start = separator + 1;
  }
  return true;
}

bool uniqueBoundedStrings(std::vector<std::string> &values, std::size_t maximum,
                          bool requireDigest = false) {
  if (values.size() > maximum) {
    return false;
  }
  std::ranges::sort(values);
  if (std::ranges::adjacent_find(values) != values.end()) {
    return false;
  }
  return std::ranges::all_of(values, [&](const std::string &value) {
    return boundedText(value) && (!requireDigest || lowercaseSha256(value));
  });
}

bool validateMetadata(SkinEntryMetadataSnapshot &metadata) {
  constexpr std::size_t maximumDeclarations = 256;
  if (!boundedText(metadata.displayName) || !boundedText(metadata.author) ||
      metadata.categories.size() > maximumDeclarations ||
      metadata.options.size() > maximumDeclarations ||
      metadata.files.size() > maximumDeclarations ||
      metadata.offsets.size() > maximumDeclarations) {
    return false;
  }
  for (auto &category : metadata.categories) {
    if (!boundedText(category.name) ||
        !uniqueBoundedStrings(category.items, maximumDeclarations)) {
      return false;
    }
  }
  for (auto &option : metadata.options) {
    if (!boundedText(option.category) || !boundedText(option.name) ||
        !boundedText(option.defaultLabel) ||
        option.choices.size() > maximumDeclarations) {
      return false;
    }
    for (const auto &choice : option.choices) {
      if (!boundedText(choice.label)) {
        return false;
      }
    }
  }
  for (auto &file : metadata.files) {
    if (!boundedText(file.category) || !boundedText(file.name) ||
        !boundedText(file.pattern) || !boundedText(file.defaultValue) ||
        !uniqueBoundedStrings(file.choices, maximumDeclarations)) {
      return false;
    }
  }
  return std::ranges::all_of(metadata.offsets, [](const auto &offset) {
    return boundedText(offset.category) && boundedText(offset.name);
  });
}

bool validateDiagnostic(const SkinDiagnostic &diagnostic) {
  return boundedText(diagnostic.code) && boundedText(diagnostic.message) &&
         safeVirtualPath(diagnostic.virtualPath) &&
         (!diagnostic.source ||
          safeVirtualPath(diagnostic.source->virtualPath));
}

bool validateAndCanonicalize(SkinPackageCatalogSnapshot &snapshot) {
  constexpr std::size_t maximumDeclarations = 256;
  if (snapshot.catalogGeneration == std::numeric_limits<std::uint64_t>::max() ||
      snapshot.sourceGeneration == std::numeric_limits<std::uint64_t>::max() ||
      snapshot.packages.size() > SkinPackagePolicy::maxFiles ||
      snapshot.entries.size() > SkinPackagePolicy::maxFiles) {
    return false;
  }
  std::set<std::string, std::less<>> packageKeys;
  std::map<std::string, SkinPackageId, std::less<>> packages;
  for (const SkinPackageId &package : snapshot.packages) {
    const auto normalized = normalizePackageId(package.directoryName);
    if (!normalized.package || *normalized.package != package ||
        !packageKeys.insert(package.collisionKey).second) {
      return false;
    }
    packages.emplace(package.collisionKey, package);
  }
  std::set<std::string, std::less<>> entryKeys;
  for (SkinCatalogEntrySnapshot &entry : snapshot.entries) {
    const auto normalized = normalizeEntryPath(entry.entry.package,
                                               entry.entry.packageRelativePath);
    const auto package = packages.find(entry.entry.package.collisionKey);
    if (!normalized.entry || *normalized.entry != entry.entry ||
        package == packages.end() || package->second != entry.entry.package ||
        !entryKeys.insert(entry.entry.collisionKey).second ||
        !lowercaseSha256(entry.revisionDigest) ||
        !uniqueBoundedStrings(entry.validatedConfigurationDigests,
                              maximumDeclarations, true) ||
        entry.diagnostics.size() > maximumDeclarations ||
        (entry.metadata && !validateMetadata(*entry.metadata)) ||
        !std::ranges::all_of(entry.diagnostics, validateDiagnostic)) {
      return false;
    }
  }
  std::ranges::sort(snapshot.packages, {}, &SkinPackageId::collisionKey);
  std::ranges::sort(snapshot.entries, {},
                    [](const SkinCatalogEntrySnapshot &entry) {
                      return entry.entry.collisionKey;
                    });
  return true;
}

bool encodedCatalogBytes(SkinPackageCatalogSnapshot snapshot,
                         std::string &bytes) {
  try {
    if (!validateAndCanonicalize(snapshot)) {
      return false;
    }
    bytes = encodeCatalog(snapshot).dump() + "\n";
    return bytes.size() <= kMaximumCatalogBytes;
  } catch (...) {
    bytes.clear();
    return false;
  }
}

bool getString(const Json &object, std::string_view key, std::string &value) {
  const auto iterator = object.find(std::string(key));
  if (iterator == object.end() || !iterator->is_string()) {
    return false;
  }
  value = iterator->get<std::string>();
  return value.size() <= SkinPackagePolicy::maxPathBytes &&
         value.find('\0') == std::string::npos;
}

bool decodeStringArray(const Json &encoded, std::size_t maximum,
                       std::vector<std::string> &values) {
  if (!encoded.is_array() || encoded.size() > maximum) {
    return false;
  }
  std::set<std::string, std::less<>> unique;
  for (const Json &item : encoded) {
    if (!item.is_string()) {
      return false;
    }
    std::string value = item.get<std::string>();
    if (value.size() > SkinPackagePolicy::maxPathBytes ||
        value.find('\0') != std::string::npos || !unique.insert(value).second) {
      return false;
    }
    values.push_back(std::move(value));
  }
  return true;
}

bool decodePackage(const Json &encoded, SkinPackageId &package) {
  if (!encoded.is_object() ||
      !getString(encoded, "directoryName", package.directoryName) ||
      !getString(encoded, "collisionKey", package.collisionKey)) {
    return false;
  }
  const auto normalized = normalizePackageId(package.directoryName);
  return normalized.package && *normalized.package == package;
}

bool decodeEntryId(const Json &encoded, SkinEntryId &entry) {
  if (!encoded.is_object()) {
    return false;
  }
  const auto package = encoded.find("package");
  if (package == encoded.end() || !decodePackage(*package, entry.package) ||
      !getString(encoded, "packageRelativePath", entry.packageRelativePath) ||
      !getString(encoded, "collisionKey", entry.collisionKey)) {
    return false;
  }
  const auto normalized =
      normalizeEntryPath(entry.package, entry.packageRelativePath);
  return normalized.entry && *normalized.entry == entry;
}

bool decodeDiagnostic(const Json &encoded, SkinDiagnostic &diagnostic) {
  if (!encoded.is_object() || !getString(encoded, "code", diagnostic.code) ||
      !getString(encoded, "message", diagnostic.message) ||
      !getString(encoded, "virtualPath", diagnostic.virtualPath)) {
    return false;
  }
  const auto severity = encoded.find("severity");
  if (severity == encoded.end() || !severity->is_number_unsigned()) {
    return false;
  }
  const auto value = severity->get<std::uint64_t>();
  if (value > static_cast<std::uint64_t>(DiagnosticSeverity::Error)) {
    return false;
  }
  diagnostic.severity = static_cast<DiagnosticSeverity>(value);
  const auto source = encoded.find("source");
  if (source != encoded.end()) {
    if (!source->is_object()) {
      return false;
    }
    SkinSourceLocation location;
    const auto line = source->find("line");
    const auto column = source->find("column");
    if (!getString(*source, "virtualPath", location.virtualPath) ||
        line == source->end() || !line->is_number_unsigned() ||
        column == source->end() || !column->is_number_unsigned()) {
      return false;
    }
    location.line = line->get<std::uint32_t>();
    location.column = column->get<std::uint32_t>();
    diagnostic.source = std::move(location);
  }
  return true;
}

bool validDiagnosticHistoryRecord(const SkinDiagnosticHistoryRecord &record) {
  const auto normalized =
      normalizeEntryPath(record.entry.package, record.entry.packageRelativePath);
  return record.recordSerial != 0 && normalized.entry &&
         *normalized.entry == record.entry &&
         lowercaseSha256(record.revisionDigest) &&
         lowercaseSha256(record.configurationDigest) &&
         static_cast<std::uint8_t>(record.phase) <=
             static_cast<std::uint8_t>(SkinDiagnosticPhase::FrameFallback) &&
         validateDiagnostic(record.diagnostic);
}

bool validDiagnosticHistoryRecords(
    std::span<const SkinDiagnosticHistoryRecord> records) {
  if (records.size() > SkinDiagnosticHistory::maxGlobalRecords) {
    return false;
  }
  std::uint64_t previousSerial = 0;
  std::map<SkinEntryId, std::size_t> perEntry;
  for (const auto &record : records) {
    if (!validDiagnosticHistoryRecord(record) ||
        record.recordSerial <= previousSerial ||
        ++perEntry[record.entry] > SkinDiagnosticHistory::maxRecordsPerEntry) {
      return false;
    }
    previousSerial = record.recordSerial;
  }
  return true;
}

OrderedJson encodeDiagnosticHistory(
    std::span<const SkinDiagnosticHistoryRecord> records) {
  OrderedJson document = OrderedJson::object();
  document["schemaVersion"] = kDiagnosticHistorySchemaVersion;
  document["records"] = OrderedJson::array();
  for (const auto &record : records) {
    OrderedJson encoded = OrderedJson::object();
    encoded["recordSerial"] = record.recordSerial;
    encoded["entry"] = encodeEntryId(record.entry);
    encoded["revisionDigest"] = record.revisionDigest;
    encoded["configurationDigest"] = record.configurationDigest;
    encoded["phase"] = static_cast<std::uint8_t>(record.phase);
    encoded["diagnostic"] = encodeDiagnostic(record.diagnostic);
    if (record.luaLine) {
      encoded["luaLine"] = *record.luaLine;
    }
    if (record.frameSerial) {
      encoded["frameSerial"] = *record.frameSerial;
    }
    document["records"].push_back(std::move(encoded));
  }
  return document;
}

bool encodedDiagnosticHistoryBytes(
    std::span<const SkinDiagnosticHistoryRecord> records, std::string &bytes) {
  try {
    if (!validDiagnosticHistoryRecords(records)) {
      return false;
    }
    bytes = encodeDiagnosticHistory(records).dump() + "\n";
    return bytes.size() <= kMaximumCatalogBytes;
  } catch (...) {
    bytes.clear();
    return false;
  }
}

bool decodeDiagnosticHistory(const Json &document,
                             std::vector<SkinDiagnosticHistoryRecord> &records) {
  try {
    if (!document.is_object()) {
      return false;
    }
    const auto schemaVersion = document.find("schemaVersion");
    const auto encodedRecords = document.find("records");
    if (schemaVersion == document.end() ||
        !schemaVersion->is_number_integer() ||
        schemaVersion->get<int>() != kDiagnosticHistorySchemaVersion ||
        encodedRecords == document.end() || !encodedRecords->is_array() ||
        encodedRecords->size() > SkinDiagnosticHistory::maxGlobalRecords) {
      return false;
    }
    records.clear();
    records.reserve(encodedRecords->size());
    for (const Json &encoded : *encodedRecords) {
      if (!encoded.is_object()) {
        return false;
      }
      SkinDiagnosticHistoryRecord record;
      const auto serial = encoded.find("recordSerial");
      const auto entry = encoded.find("entry");
      const auto phase = encoded.find("phase");
      const auto diagnostic = encoded.find("diagnostic");
      if (serial == encoded.end() || !serial->is_number_unsigned() ||
          entry == encoded.end() || !decodeEntryId(*entry, record.entry) ||
          !getString(encoded, "revisionDigest", record.revisionDigest) ||
          !getString(encoded, "configurationDigest", record.configurationDigest) ||
          phase == encoded.end() || !phase->is_number_unsigned() ||
          diagnostic == encoded.end() || !decodeDiagnostic(*diagnostic, record.diagnostic)) {
        return false;
      }
      const auto phaseValue = phase->get<std::uint64_t>();
      if (phaseValue > static_cast<std::uint64_t>(SkinDiagnosticPhase::FrameFallback)) {
        return false;
      }
      record.recordSerial = serial->get<std::uint64_t>();
      record.phase = static_cast<SkinDiagnosticPhase>(phaseValue);
      const auto luaLine = encoded.find("luaLine");
      if (luaLine != encoded.end()) {
        if (!luaLine->is_number_unsigned() ||
            luaLine->get<std::uint64_t>() >
                std::numeric_limits<std::uint32_t>::max()) {
          return false;
        }
        record.luaLine = luaLine->get<std::uint32_t>();
      }
      const auto frameSerial = encoded.find("frameSerial");
      if (frameSerial != encoded.end()) {
        if (!frameSerial->is_number_unsigned()) {
          return false;
        }
        record.frameSerial = frameSerial->get<std::uint64_t>();
      }
      records.push_back(std::move(record));
    }
    return validDiagnosticHistoryRecords(records);
  } catch (...) {
    records.clear();
    return false;
  }
}

bool decodeMetadata(const Json &encoded, SkinEntryMetadataSnapshot &metadata) {
  if (!encoded.is_object() ||
      !getString(encoded, "displayName", metadata.displayName) ||
      !getString(encoded, "author", metadata.author)) {
    return false;
  }
  const auto skinType = encoded.find("skinType");
  const auto width = encoded.find("authoredWidth");
  const auto height = encoded.find("authoredHeight");
  const auto categories = encoded.find("categories");
  const auto options = encoded.find("options");
  const auto files = encoded.find("files");
  const auto offsets = encoded.find("offsets");
  if (skinType == encoded.end() || !skinType->is_number_integer() ||
      width == encoded.end() || !width->is_number_integer() ||
      height == encoded.end() || !height->is_number_integer() ||
      categories == encoded.end() || !categories->is_array() ||
      options == encoded.end() || !options->is_array() ||
      files == encoded.end() || !files->is_array() ||
      offsets == encoded.end() || !offsets->is_array()) {
    return false;
  }
  constexpr std::size_t maxDeclarations = 256;
  if (categories->size() > maxDeclarations ||
      options->size() > maxDeclarations || files->size() > maxDeclarations ||
      offsets->size() > maxDeclarations) {
    return false;
  }
  metadata.skinType = skinType->get<int>();
  metadata.authoredWidth = width->get<int>();
  metadata.authoredHeight = height->get<int>();
  for (const Json &item : *categories) {
    SkinCatalogCategoryDeclaration category;
    const auto values = item.find("items");
    if (!item.is_object() || !getString(item, "name", category.name) ||
        values == item.end() || !values->is_array()) {
      return false;
    }
    if (values->size() > maxDeclarations) {
      return false;
    }
    if (!decodeStringArray(*values, maxDeclarations, category.items)) {
      return false;
    }
    metadata.categories.push_back(std::move(category));
  }
  for (const Json &item : *options) {
    SkinCatalogOptionDeclaration option;
    const auto choices = item.find("choices");
    if (!item.is_object() || !getString(item, "category", option.category) ||
        !getString(item, "name", option.name) ||
        !getString(item, "defaultLabel", option.defaultLabel) ||
        choices == item.end() || !choices->is_array()) {
      return false;
    }
    if (choices->size() > maxDeclarations) {
      return false;
    }
    for (const Json &choiceValue : *choices) {
      SkinCatalogOptionChoice choice;
      const auto value = choiceValue.find("value");
      if (!choiceValue.is_object() ||
          !getString(choiceValue, "label", choice.label) ||
          value == choiceValue.end() || !value->is_number_integer()) {
        return false;
      }
      choice.value = value->get<int>();
      option.choices.push_back(std::move(choice));
    }
    metadata.options.push_back(std::move(option));
  }
  for (const Json &item : *files) {
    SkinCatalogFileDeclaration file;
    const auto choices = item.find("choices");
    if (!item.is_object() || !getString(item, "category", file.category) ||
        !getString(item, "name", file.name) ||
        !getString(item, "pattern", file.pattern) ||
        !getString(item, "defaultValue", file.defaultValue) ||
        choices == item.end() || !choices->is_array()) {
      return false;
    }
    if (choices->size() > maxDeclarations) {
      return false;
    }
    if (!decodeStringArray(*choices, maxDeclarations, file.choices)) {
      return false;
    }
    metadata.files.push_back(std::move(file));
  }
  for (const Json &item : *offsets) {
    SkinCatalogOffsetDeclaration offset;
    const auto id = item.find("id");
    const auto permissions = item.find("permissions");
    if (!item.is_object() || !getString(item, "category", offset.category) ||
        !getString(item, "name", offset.name) || id == item.end() ||
        !id->is_number_integer() || permissions == item.end() ||
        !permissions->is_number_unsigned()) {
      return false;
    }
    offset.id = id->get<int>();
    const auto permissionValue = permissions->get<std::uint64_t>();
    if (permissionValue > std::numeric_limits<std::uint8_t>::max()) {
      return false;
    }
    offset.permissions = static_cast<std::uint8_t>(permissionValue);
    metadata.offsets.push_back(std::move(offset));
  }
  return true;
}

bool decodeCatalog(const Json &document, SkinPackageCatalogSnapshot &snapshot) {
  try {
    if (!document.is_object()) {
      return false;
    }
    const auto catalogGeneration = document.find("catalogGeneration");
    const auto sourceGeneration = document.find("sourceGeneration");
    const auto packages = document.find("packages");
    const auto entries = document.find("entries");
    if (catalogGeneration == document.end() ||
        !catalogGeneration->is_number_unsigned() ||
        sourceGeneration == document.end() ||
        !sourceGeneration->is_number_unsigned() || packages == document.end() ||
        !packages->is_array() || entries == document.end() ||
        !entries->is_array()) {
      return false;
    }
    if (packages->size() > SkinPackagePolicy::maxFiles ||
        entries->size() > SkinPackagePolicy::maxFiles) {
      return false;
    }
    snapshot.catalogGeneration = catalogGeneration->get<std::uint64_t>();
    snapshot.sourceGeneration = sourceGeneration->get<std::uint64_t>();
    std::set<std::string> packageIdentities;
    for (const Json &encoded : *packages) {
      SkinPackageId package;
      if (!decodePackage(encoded, package)) {
        return false;
      }
      if (!packageIdentities.insert(package.collisionKey).second) {
        return false;
      }
      snapshot.packages.push_back(std::move(package));
    }
    std::set<std::string> entryIdentities;
    for (const Json &encoded : *entries) {
      if (!encoded.is_object()) {
        return false;
      }
      SkinCatalogEntrySnapshot entry;
      const auto id = encoded.find("entry");
      const auto validation = encoded.find("validation");
      const auto configurationDigests =
          encoded.find("validatedConfigurationDigests");
      const auto parsedValidation = validation == encoded.end()
                                        ? std::nullopt
                                        : parseValidation(*validation);
      if (id == encoded.end() || !decodeEntryId(*id, entry.entry) ||
          !getString(encoded, "revisionDigest", entry.revisionDigest) ||
          !parsedValidation || configurationDigests == encoded.end() ||
          !configurationDigests->is_array()) {
        return false;
      }
      if (!entryIdentities.insert(entry.entry.collisionKey).second ||
          !packageIdentities.contains(entry.entry.package.collisionKey) ||
          !lowercaseSha256(entry.revisionDigest) ||
          configurationDigests->size() > 256) {
        return false;
      }
      entry.validation = *parsedValidation;
      if (!decodeStringArray(*configurationDigests, 256,
                             entry.validatedConfigurationDigests) ||
          !std::ranges::all_of(entry.validatedConfigurationDigests,
                               lowercaseSha256)) {
        return false;
      }
      const auto metadata = encoded.find("metadata");
      if (metadata != encoded.end()) {
        SkinEntryMetadataSnapshot value;
        if (!decodeMetadata(*metadata, value)) {
          return false;
        }
        entry.metadata = std::move(value);
      }
      const auto diagnostics = encoded.find("diagnostics");
      if (diagnostics != encoded.end()) {
        if (!diagnostics->is_array()) {
          return false;
        }
        if (diagnostics->size() > 256) {
          return false;
        }
        for (const Json &encodedDiagnostic : *diagnostics) {
          SkinDiagnostic diagnostic;
          if (!decodeDiagnostic(encodedDiagnostic, diagnostic)) {
            return false;
          }
          entry.diagnostics.push_back(std::move(diagnostic));
        }
      }
      snapshot.entries.push_back(std::move(entry));
    }
    return validateAndCanonicalize(snapshot);
  } catch (const nlohmann::json::exception &) {
    snapshot = {};
    return false;
  } catch (const std::exception &) {
    snapshot = {};
    return false;
  }
}

bool writeCatalogFile(const fs::path &path,
                      const SkinPackageCatalogSnapshot &snapshot,
                      std::string &error) {
  std::string encoded;
  if (!encodedCatalogBytes(snapshot, encoded)) {
    error = "private catalog snapshot is invalid or exceeds its size bound";
    return false;
  }
  const auto operations = atomic_file::privateFileOperations();
  return atomic_file::writeWithBackup(path, std::as_bytes(std::span(encoded)),
                                      error, &operations);
}

bool writeDiagnosticHistoryFile(
    const fs::path &path,
    std::span<const SkinDiagnosticHistoryRecord> records, std::string &error) {
  std::string encoded;
  if (!encodedDiagnosticHistoryBytes(records, encoded)) {
    error = "private diagnostic history is invalid or exceeds its size bound";
    return false;
  }
  const auto operations = atomic_file::privateFileOperations();
  return atomic_file::writeWithBackup(path, std::as_bytes(std::span(encoded)),
                                      error, &operations);
}

} // namespace

struct SkinPackageCatalog::Impl {
  struct WriteCompletion {
    bool finished = false;
    bool succeeded = false;
    std::string error;
  };

  struct WriteWork {
    SkinPackageCatalogSnapshot snapshot;
    std::uint64_t sequence = 0;
    std::shared_ptr<WriteCompletion> completion;
  };

  struct HistoryWriteWork {
    std::vector<SkinDiagnosticHistoryRecord> records;
    std::uint64_t sequence = 0;
  };

  struct Submission {
    std::uint64_t sequence = 0;
    std::shared_ptr<WriteCompletion> completion;
  };

  explicit Impl(fs::path rootPath)
      : root(std::move(rootPath)),
        current(std::make_shared<SkinPackageCatalogSnapshot>()) {
    try {
      worker = std::thread([this] { run(); });
      historyWorker = std::thread([this] { runHistory(); });
    } catch (...) {
      {
        std::scoped_lock lock(mutex);
        closing = true;
        changed.notify_all();
        historyChanged.notify_all();
      }
      if (worker.joinable()) {
        worker.join();
      }
      throw;
    }
  }

  ~Impl() { stop(); }

  void run() noexcept {
    std::unique_lock lock(mutex);
    while (true) {
      changed.wait(lock, [this] { return closing || !queue.empty(); });
      if (queue.empty() && closing) {
        break;
      }
      WriteWork work = std::move(queue.front());
      queue.pop_front();
      lock.unlock();
      bool saved = false;
      std::string error;
      try {
        const bool directoryReady = ensureDirectoryNoFollow(root);
        saved = directoryReady &&
                writeCatalogFile(root / "catalog.json", work.snapshot, error);
        if (!directoryReady) {
          error = "private catalog directory is unavailable";
        }
      } catch (...) {
        saved = false;
        error = "private catalog write raised an exception";
      }
      lock.lock();
      completedSequence = std::max(completedSequence, work.sequence);
      lastWriteSucceeded = saved;
      if (saved) {
        lastWriteError.clear();
      } else {
        lastWriteError.swap(error);
      }
      if (work.completion) {
        work.completion->succeeded = saved;
        if (saved) {
          work.completion->error.clear();
        }
        work.completion->finished = true;
      }
      completed.notify_all();
    }
  }

  void runHistory() noexcept {
    std::unique_lock lock(mutex);
    while (true) {
      historyChanged.wait(lock,
                          [this] { return closing || historyPending.has_value(); });
      if (!historyPending && closing) {
        break;
      }
      HistoryWriteWork work = std::move(*historyPending);
      historyPending.reset();
      lock.unlock();
      bool saved = false;
      std::string error;
      try {
        const bool directoryReady = ensureDirectoryNoFollow(root);
        saved = directoryReady &&
                writeDiagnosticHistoryFile(root / kDiagnosticHistoryFile,
                                           work.records, error);
        if (!directoryReady) {
          error = "private catalog directory is unavailable";
        }
      } catch (...) {
        saved = false;
        error = "private diagnostic history write raised an exception";
      }
      lock.lock();
      completedHistorySequence =
          std::max(completedHistorySequence, work.sequence);
      lastHistoryWriteSucceeded = saved;
      if (saved) {
        lastHistoryWriteError.clear();
      } else {
        lastHistoryWriteError.swap(error);
      }
      historyCompleted.notify_all();
    }
  }

  std::optional<Submission>
  enqueue(SkinPackageCatalogSnapshot snapshot, bool durable,
          bool publishImmediately,
          std::optional<std::uint64_t> expectedCatalog = std::nullopt,
          std::optional<std::uint64_t> expectedSource = std::nullopt) noexcept {
    try {
      std::string boundedEncoding;
      if (!encodedCatalogBytes(snapshot, boundedEncoding)) {
        return std::nullopt;
      }
      auto published =
          publishImmediately
              ? std::make_shared<SkinPackageCatalogSnapshot>(snapshot)
              : nullptr;
      auto completion = durable
                            ? std::make_shared<WriteCompletion>(WriteCompletion{
                                  .error = "private catalog write failed"})
                            : nullptr;
      WriteWork work{.snapshot = std::move(snapshot), .completion = completion};
      std::scoped_lock lock(mutex);
      if (closing) {
        return std::nullopt;
      }
      if ((expectedCatalog && current->catalogGeneration != *expectedCatalog) ||
          (expectedSource && current->sourceGeneration != *expectedSource)) {
        return std::nullopt;
      }
      work.sequence = submittedSequence + 1;
      if (!durable && !queue.empty() && !queue.back().completion) {
        queue.back() = std::move(work);
      } else {
        // Admit the fully-owned work before publishing its in-memory view.
        // If deque growth throws, neither current nor sequence changes.
        queue.push_back(std::move(work));
      }
      submittedSequence = queue.back().sequence;
      if (publishImmediately) {
        current = std::move(published);
      }
      changed.notify_one();
      return Submission{.sequence = submittedSequence,
                        .completion = std::move(completion)};
    } catch (...) {
      return std::nullopt;
    }
  }

  bool waitForSequence(const Submission &submission, std::string &error) {
    std::unique_lock lock(mutex);
    completed.wait(lock, [&submission, this] {
      return submission.completion->finished ||
             (closing && completedSequence >= submission.sequence);
    });
    if (!submission.completion->finished) {
      error = "private catalog closed before durable write completed";
      return false;
    }
    error = submission.completion->error;
    return submission.completion->succeeded;
  }

  bool waitForWrites(std::string &error) {
    std::unique_lock lock(mutex);
    const std::uint64_t target = submittedSequence;
    completed.wait(lock, [this, target] {
      return completedSequence >= target ||
             (closing && queue.empty() && completedSequence >= target);
    });
    error = lastWriteError;
    return target == 0 || lastWriteSucceeded;
  }

  bool enqueueDiagnosticHistory(
      std::vector<SkinDiagnosticHistoryRecord> records) noexcept {
    try {
      std::string boundedEncoding;
      if (!encodedDiagnosticHistoryBytes(records, boundedEncoding)) {
        return false;
      }
      std::scoped_lock lock(mutex);
      if (closing) {
        return false;
      }
      HistoryWriteWork work{.records = std::move(records),
                            .sequence = submittedHistorySequence + 1};
      historyPending = std::move(work);
      submittedHistorySequence = historyPending->sequence;
      historyChanged.notify_one();
      return true;
    } catch (...) {
      return false;
    }
  }

  bool waitForHistoryWrites(std::string &error) {
    std::unique_lock lock(mutex);
    const std::uint64_t target = submittedHistorySequence;
    historyCompleted.wait(lock, [this, target] {
      return completedHistorySequence >= target ||
             (closing && !historyPending && completedHistorySequence >= target);
    });
    error = lastHistoryWriteError;
    return target == 0 || lastHistoryWriteSucceeded;
  }

  void stop() noexcept {
    std::call_once(stopOnce, [this] {
      {
        std::scoped_lock lock(mutex);
        closing = true;
        changed.notify_all();
        historyChanged.notify_all();
      }
      if (worker.joinable()) {
        worker.join();
      }
      if (historyWorker.joinable()) {
        historyWorker.join();
      }
    });
  }

  fs::path root;
  mutable std::mutex mutex;
  std::condition_variable changed;
  std::condition_variable completed;
  std::condition_variable historyChanged;
  std::condition_variable historyCompleted;
  std::shared_ptr<const SkinPackageCatalogSnapshot> current;
  std::deque<WriteWork> queue;
  std::uint64_t submittedSequence = 0;
  std::uint64_t completedSequence = 0;
  bool lastWriteSucceeded = true;
  std::string lastWriteError;
  std::optional<HistoryWriteWork> historyPending;
  std::uint64_t submittedHistorySequence = 0;
  std::uint64_t completedHistorySequence = 0;
  bool lastHistoryWriteSucceeded = true;
  std::string lastHistoryWriteError;
  bool closing = false;
  std::once_flag stopOnce;
  std::thread worker;
  std::thread historyWorker;
};

SkinPackageCatalog::SkinPackageCatalog(fs::path privateCatalogRoot)
    : impl_(std::make_unique<Impl>(std::move(privateCatalogRoot))) {}

SkinPackageCatalog::~SkinPackageCatalog() { shutdown(); }

bool SkinPackageCatalog::loadSnapshotFromDisk(
    SkinPackageCatalogSnapshot &snapshot,
    std::vector<SkinDiagnostic> &diagnostics) const {
  snapshot = {};
  bool missing = false;
  const auto bytes =
      readBoundedCatalogFile(impl_->root / "catalog.json", missing);
  if (missing) {
    return true;
  }
  if (!bytes || !decodeSnapshotBytes(*bytes, snapshot)) {
    diagnostics.push_back(catalogDiagnostic(
        "skin_catalog_load_failed",
        "private skin catalog metadata is missing or malformed"));
    snapshot = {};
    return false;
  }
  return true;
}

bool SkinPackageCatalog::decodeSnapshotBytes(
    std::string_view bytes, SkinPackageCatalogSnapshot &snapshot) const {
  try {
    if (bytes.size() > kMaximumCatalogBytes) {
      return false;
    }
    const Json document = Json::parse(bytes);
    const auto schemaVersion = document.find("schemaVersion");
    if (!document.is_object() || schemaVersion == document.end() ||
        !schemaVersion->is_number_integer() ||
        schemaVersion->get<int>() != kCatalogSchemaVersion) {
      snapshot = {};
      return false;
    }
    snapshot = {};
    return decodeCatalog(document, snapshot);
  } catch (...) {
    snapshot = {};
    return false;
  }
}

bool SkinPackageCatalog::recover() {
  SkinPackageCatalogSnapshot recovered;
  std::vector<SkinDiagnostic> ignored;
  if (!loadSnapshotFromDisk(recovered, ignored)) {
    return false;
  }
  try {
    auto published =
        std::make_shared<SkinPackageCatalogSnapshot>(std::move(recovered));
    std::scoped_lock lock(impl_->mutex);
    impl_->current = std::move(published);
    return true;
  } catch (...) {
    return false;
  }
}

bool SkinPackageCatalog::replaceSnapshotDurably(
    SkinPackageCatalogSnapshot snapshot,
    std::vector<SkinDiagnostic> &diagnostics) {
  std::string boundedEncoding;
  if (!validateAndCanonicalize(snapshot) ||
      !encodedCatalogBytes(snapshot, boundedEncoding)) {
    diagnostics.push_back(catalogDiagnostic(
        "skin_catalog_snapshot_invalid",
        "private skin catalog snapshot violates its bounded typed contract"));
    return false;
  }
  auto committed = std::make_shared<SkinPackageCatalogSnapshot>(snapshot);
  const auto sequence = impl_->enqueue(std::move(snapshot), true, false);
  if (!sequence) {
    diagnostics.push_back(catalogDiagnostic(
        "skin_catalog_closed", "private skin catalog is already closed"));
    return false;
  }
  std::string error;
  if (!impl_->waitForSequence(*sequence, error)) {
    diagnostics.push_back(catalogDiagnostic(
        "skin_catalog_write_failed",
        "unable to durably replace private skin catalog metadata"));
    return false;
  }
  {
    std::scoped_lock lock(impl_->mutex);
    impl_->current = std::move(committed);
  }
  return true;
}

bool SkinPackageCatalog::replaceSnapshotAsync(
    SkinPackageCatalogSnapshot snapshot) {
  return impl_->enqueue(std::move(snapshot), false, true).has_value();
}

bool SkinPackageCatalog::replaceSnapshotAsyncIfGeneration(
    std::uint64_t expectedCatalog, std::uint64_t expectedSource,
    SkinPackageCatalogSnapshot snapshot) {
  return impl_
      ->enqueue(std::move(snapshot), false, true, expectedCatalog,
                expectedSource)
      .has_value();
}

std::string SkinPackageCatalog::snapshotDigest(
    const SkinPackageCatalogSnapshot &snapshot) const {
  std::string encoded;
  if (!encodedCatalogBytes(snapshot, encoded)) {
    return {};
  }
  return file_checksum::sha256(encoded);
}

bool SkinPackageCatalog::writeSnapshotFile(
    const fs::path &path, const SkinPackageCatalogSnapshot &snapshot,
    std::vector<SkinDiagnostic> &diagnostics) const {
  std::string message;
  if (!ensureDirectoryNoFollow(path.parent_path()) ||
      !writeCatalogFile(path, snapshot, message)) {
    diagnostics.push_back(catalogDiagnostic(
        "skin_catalog_staging_write_failed",
        "unable to durably stage private skin catalog metadata"));
    return false;
  }
  return true;
}

std::shared_ptr<const SkinPackageCatalogSnapshot>
SkinPackageCatalog::snapshot() const noexcept {
  std::scoped_lock lock(impl_->mutex);
  return impl_->current;
}

std::vector<SkinDiagnosticHistoryRecord>
SkinPackageCatalog::loadDiagnosticHistory() const {
  bool missing = false;
  const auto bytes =
      readBoundedCatalogFile(impl_->root / kDiagnosticHistoryFile, missing);
  if (missing || !bytes || bytes->size() > kMaximumCatalogBytes) {
    return {};
  }
  try {
    std::vector<SkinDiagnosticHistoryRecord> records;
    if (!decodeDiagnosticHistory(Json::parse(*bytes), records)) {
      return {};
    }
    return records;
  } catch (...) {
    return {};
  }
}

bool SkinPackageCatalog::replaceDiagnosticHistory(
    std::span<const SkinDiagnosticHistoryRecord> records) {
  try {
    std::vector<SkinDiagnosticHistoryRecord> owned(records.begin(),
                                                    records.end());
    return impl_->enqueueDiagnosticHistory(std::move(owned));
  } catch (...) {
    return false;
  }
}

void SkinPackageCatalog::flush() {
  std::string ignored;
  impl_->waitForWrites(ignored);
  impl_->waitForHistoryWrites(ignored);
}

void SkinPackageCatalog::shutdown() noexcept {
  if (impl_) {
    impl_->stop();
  }
}

} // namespace skin
