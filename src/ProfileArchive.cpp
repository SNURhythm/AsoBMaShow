#include "ProfileArchive.h"

#include "AppSettingsStore.h"
#include "ArchiveRAII.h"
#include "AtomicFile.h"
#include "FileChecksum.h"
#include "ProfileDatabaseTools.h"
#include "ReplayDBHelper.h"
#include "ScoreDBHelper.h"
#include "input/InputProfile.h"
#include "input/InputProfileStore.h"
#include "practice/PracticePresetStore.h"

#include "../yoga/lib/nlohmann/json.hpp"

#include <archive_entry.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <AclAPI.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using Json = nlohmann::json;

constexpr std::string_view kApplicationVersion = "1.0";
constexpr auto kStaleWorkspaceAge = std::chrono::hours(24);
constexpr std::array<std::string_view, 6> kMemberNames = {
    "manifest.json", "settings.json", "input.json",
    "scores.db",     "replays.db",    "checksums.sha256"};
constexpr std::array<std::string_view, 10> kVersionOneManifestKeys = {
    "createdAt",
    "formatVersion",
    "inputSchemaVersion",
    "profileDisplayName",
    "profileSchemaVersion",
    "profileUuid",
    "replaySchemaVersion",
    "scoreSchemaVersion",
    "settingsSchemaVersion",
    "sourceApplicationVersion"};
constexpr std::array<std::string_view, 11> kVersionTwoManifestKeys = {
    "createdAt",
    "formatVersion",
    "inputSchemaVersion",
    "practiceSchemaVersion",
    "profileDisplayName",
    "profileSchemaVersion",
    "profileUuid",
    "replaySchemaVersion",
    "scoreSchemaVersion",
    "settingsSchemaVersion",
    "sourceApplicationVersion"};

ProfileArchiveResult failure(ProfileError error, std::string message) {
  return {
      .error = error, .message = std::move(message), .profile = std::nullopt};
}

ProfileArchiveResult success(PlayerProfile profile, std::string message = {}) {
  return {.error = ProfileError::None,
          .message = std::move(message),
          .profile = std::move(profile)};
}

std::string archiveError(archive *handle, std::string_view fallback) {
  const char *detail =
      handle == nullptr ? nullptr : archive_error_string(handle);
  return detail == nullptr ? std::string(fallback) : std::string(detail);
}

bool randomSuffix(std::string &suffix, std::string &errorMessage) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  try {
    std::random_device entropy;
    suffix.clear();
    suffix.reserve(32);
    for (int index = 0; index < 32; ++index) {
      suffix.push_back(kHex[entropy() % kHex.size()]);
    }
    return true;
  } catch (const std::exception &exception) {
    errorMessage = "unable to obtain entropy for a temporary path: " +
                   std::string(exception.what());
    return false;
  }
}

std::filesystem::path siblingCandidate(const std::filesystem::path &path,
                                       std::string_view tag,
                                       std::string_view suffix) {
  const auto parent = path.parent_path().empty() ? std::filesystem::path(".")
                                                 : path.parent_path();
  const std::filesystem::path nativeSuffix(std::string(tag) +
                                           std::string(suffix));
  return parent / std::filesystem::path(path.filename().native() +
                                        nativeSuffix.native());
}

bool classifyPathWithin(const std::filesystem::path &root,
                        const std::filesystem::path &candidate, bool &within,
                        std::string &errorMessage) {
  std::error_code error;
  const auto resolvedRoot = std::filesystem::weakly_canonical(root, error);
  if (error) {
    errorMessage =
        "unable to resolve managed application root: " + error.message();
    return false;
  }
  const auto resolvedCandidate =
      std::filesystem::weakly_canonical(candidate, error);
  if (error) {
    errorMessage = "unable to resolve export destination: " + error.message();
    return false;
  }
  auto rootPart = resolvedRoot.begin();
  auto candidatePart = resolvedCandidate.begin();
  for (; rootPart != resolvedRoot.end(); ++rootPart, ++candidatePart) {
    bool equal = candidatePart != resolvedCandidate.end();
    if (equal) {
#ifdef _WIN32
      const auto rootText = rootPart->native();
      const auto candidateText = candidatePart->native();
      equal = CompareStringOrdinal(
                  rootText.c_str(), static_cast<int>(rootText.size()),
                  candidateText.c_str(), static_cast<int>(candidateText.size()),
                  TRUE) == CSTR_EQUAL;
#else
      equal = *candidatePart == *rootPart;
#endif
    }
    if (!equal) {
      within = false;
      return true;
    }
  }
  within = true;
  return true;
}

bool hasRandomArtifactSuffix(const std::filesystem::path::string_type &filename,
                             const std::filesystem::path::string_type &prefix) {
  if (filename.size() != prefix.size() + 32 ||
      filename.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  using PathCharacter = std::filesystem::path::value_type;
  for (std::size_t index = prefix.size(); index < filename.size(); ++index) {
    const auto character = filename[index];
    const bool digit = character >= static_cast<PathCharacter>('0') &&
                       character <= static_cast<PathCharacter>('9');
    const bool lowerHex = character >= static_cast<PathCharacter>('a') &&
                          character <= static_cast<PathCharacter>('f');
    if (!digit && !lowerHex) {
      return false;
    }
  }
  return true;
}

#ifdef _WIN32
class PrivateSecurityAttributes {
public:
  PrivateSecurityAttributes() = default;
  PrivateSecurityAttributes(const PrivateSecurityAttributes &) = delete;
  PrivateSecurityAttributes &
  operator=(const PrivateSecurityAttributes &) = delete;
  ~PrivateSecurityAttributes() {
    if (accessControlList_ != nullptr) {
      LocalFree(accessControlList_);
    }
    if (token_ != nullptr) {
      CloseHandle(token_);
    }
  }

  bool initialize(std::string &errorMessage) {
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token_)) {
      errorMessage = "unable to open process token for private ACL: " +
                     std::to_string(GetLastError());
      return false;
    }
    DWORD bytes = 0;
    GetTokenInformation(token_, TokenUser, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) {
      errorMessage = "unable to size process token user: " +
                     std::to_string(GetLastError());
      return false;
    }
    tokenUser_.resize(bytes);
    if (!GetTokenInformation(token_, TokenUser, tokenUser_.data(), bytes,
                             &bytes)) {
      errorMessage = "unable to read process token user: " +
                     std::to_string(GetLastError());
      return false;
    }
    auto *tokenUser = reinterpret_cast<TOKEN_USER *>(tokenUser_.data());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(tokenUser->User.Sid);
    const DWORD aclResult =
        SetEntriesInAclW(1, &access, nullptr, &accessControlList_);
    if (aclResult != ERROR_SUCCESS) {
      errorMessage =
          "unable to build private ACL: " + std::to_string(aclResult);
      return false;
    }
    if (!InitializeSecurityDescriptor(&descriptor_,
                                      SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor_, TRUE, accessControlList_,
                                   FALSE) ||
        !SetSecurityDescriptorControl(&descriptor_, SE_DACL_PROTECTED,
                                      SE_DACL_PROTECTED)) {
      errorMessage = "unable to initialize private security descriptor: " +
                     std::to_string(GetLastError());
      return false;
    }
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = &descriptor_;
    attributes_.bInheritHandle = FALSE;
    return true;
  }

  [[nodiscard]] SECURITY_ATTRIBUTES *get() { return &attributes_; }

private:
  HANDLE token_ = nullptr;
  std::vector<std::byte> tokenUser_;
  PACL accessControlList_ = nullptr;
  SECURITY_DESCRIPTOR descriptor_{};
  SECURITY_ATTRIBUTES attributes_{};
};
#endif

bool artifactIsStale(const std::filesystem::path &path, bool &stale,
                     std::string &errorMessage) {
  std::error_code error;
  const auto timestamp = std::filesystem::last_write_time(path, error);
  if (error) {
    errorMessage =
        "unable to inspect temporary artifact age: " + error.message();
    return false;
  }
  stale = timestamp <
          std::filesystem::file_time_type::clock::now() - kStaleWorkspaceAge;
  return true;
}

bool cleanupStaleImportWorkspaces(std::string &errorMessage) {
  const auto temporaryRoot = std::filesystem::temp_directory_path();
  const auto prefix =
      std::filesystem::path("asobmashow-profile-import-").native();
  std::error_code error;
  std::filesystem::directory_iterator iterator(temporaryRoot, error);
  if (error) {
    errorMessage =
        "unable to enumerate profile import workspaces: " + error.message();
    return false;
  }
  for (const auto &entry : iterator) {
    if (!hasRandomArtifactSuffix(entry.path().filename().native(), prefix)) {
      continue;
    }
    bool stale = false;
    if (!artifactIsStale(entry.path(), stale, errorMessage)) {
      return false;
    }
    if (!stale) {
      continue;
    }
    std::filesystem::remove_all(entry.path(), error);
    if (error) {
      errorMessage =
          "unable to remove stale profile import workspace: " + error.message();
      return false;
    }
  }
  return true;
}

bool cleanupStaleExportArtifacts(const std::filesystem::path &destination,
                                 std::string &errorMessage) {
  const auto parent = destination.parent_path().empty()
                          ? std::filesystem::path(".")
                          : destination.parent_path();
  const auto base = destination.filename().native();
  const std::array prefixes = {base + std::filesystem::path(".work-").native(),
                               base + std::filesystem::path(".tmp-").native(),
                               base +
                                   std::filesystem::path(".backup-").native()};
  std::error_code error;
  std::filesystem::directory_iterator iterator(parent, error);
  if (error) {
    errorMessage =
        "unable to enumerate profile export artifacts: " + error.message();
    return false;
  }
  const auto destinationStatus =
      std::filesystem::symlink_status(destination, error);
  if (error &&
      error != std::make_error_code(std::errc::no_such_file_or_directory)) {
    errorMessage =
        "unable to inspect profile export destination: " + error.message();
    return false;
  }
  const bool destinationHasRegularFile =
      !error && std::filesystem::is_regular_file(destinationStatus);
  for (const auto &entry : iterator) {
    std::size_t artifactType = prefixes.size();
    for (std::size_t index = 0; index < prefixes.size(); ++index) {
      if (hasRandomArtifactSuffix(entry.path().filename().native(),
                                  prefixes[index])) {
        artifactType = index;
        break;
      }
    }
    if (artifactType == prefixes.size() ||
        (artifactType == 2 && !destinationHasRegularFile)) {
      continue;
    }
    bool stale = false;
    if (!artifactIsStale(entry.path(), stale, errorMessage)) {
      return false;
    }
    if (!stale) {
      continue;
    }
    if (artifactType == 0) {
      std::filesystem::remove_all(entry.path(), error);
    } else {
      std::filesystem::remove(entry.path(), error);
    }
    if (error) {
      errorMessage =
          "unable to remove stale profile export artifact: " + error.message();
      return false;
    }
  }
  return true;
}

bool createPrivateDirectoryNative(const std::filesystem::path &path,
                                  bool &alreadyExists,
                                  std::string &errorMessage) {
  alreadyExists = false;
#ifdef _WIN32
  PrivateSecurityAttributes security;
  if (!security.initialize(errorMessage)) {
    return false;
  }
  if (CreateDirectoryW(path.c_str(), security.get())) {
    return true;
  }
  const DWORD createError = GetLastError();
  if (createError == ERROR_ALREADY_EXISTS || createError == ERROR_FILE_EXISTS) {
    alreadyExists = true;
    return false;
  }
  errorMessage = "unable to create private temporary directory: " +
                 std::to_string(createError);
#else
  if (::mkdir(path.c_str(), 0700) == 0) {
    return true;
  }
  if (errno == EEXIST) {
    alreadyExists = true;
    return false;
  }
  errorMessage = "unable to create private temporary directory: " +
                 std::string(std::strerror(errno));
#endif
  return false;
}

bool createPrivateDirectory(const std::filesystem::path &path,
                            std::string &errorMessage) {
  bool alreadyExists = false;
  if (!createPrivateDirectoryNative(path, alreadyExists, errorMessage)) {
    if (alreadyExists) {
      errorMessage = "private temporary directory already exists";
    }
    return false;
  }
  return true;
}

bool createPrivateSiblingDirectory(const std::filesystem::path &path,
                                   std::string_view tag,
                                   std::filesystem::path &created,
                                   std::string &errorMessage) {
  for (int attempt = 0; attempt < 128; ++attempt) {
    std::string suffix;
    if (!randomSuffix(suffix, errorMessage)) {
      return false;
    }
    const auto candidate = siblingCandidate(path, tag, suffix);
    bool alreadyExists = false;
    if (createPrivateDirectoryNative(candidate, alreadyExists, errorMessage)) {
      created = candidate;
      return true;
    }
    if (alreadyExists) {
      continue;
    }
    return false;
  }
  errorMessage = "unable to allocate a collision-free temporary directory";
  return false;
}

int closeDescriptor(int descriptor) {
#ifdef _WIN32
  return ::_close(descriptor);
#else
  return ::close(descriptor);
#endif
}

class ExclusiveFileDescriptor {
public:
  ExclusiveFileDescriptor() = default;
  explicit ExclusiveFileDescriptor(int descriptor) : descriptor_(descriptor) {}
  ExclusiveFileDescriptor(const ExclusiveFileDescriptor &) = delete;
  ExclusiveFileDescriptor &operator=(const ExclusiveFileDescriptor &) = delete;
  ExclusiveFileDescriptor(ExclusiveFileDescriptor &&other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}
  ExclusiveFileDescriptor &operator=(ExclusiveFileDescriptor &&other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0) {
        closeDescriptor(descriptor_);
      }
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }
  ~ExclusiveFileDescriptor() {
    if (descriptor_ >= 0) {
      closeDescriptor(descriptor_);
    }
  }

  [[nodiscard]] int get() const { return descriptor_; }

  bool sync(std::string &errorMessage) const {
    if (descriptor_ < 0) {
      errorMessage = "exclusive temporary file is not open";
      return false;
    }
#ifdef _WIN32
    const int result = ::_commit(descriptor_);
#else
    const int result = ::fsync(descriptor_);
#endif
    if (result != 0) {
      errorMessage = "unable to sync exclusive temporary file: " +
                     std::string(std::strerror(errno));
      return false;
    }
    return true;
  }

  bool close(std::string &errorMessage) {
    if (descriptor_ < 0) {
      return true;
    }
    const int descriptor = std::exchange(descriptor_, -1);
    if (closeDescriptor(descriptor) != 0) {
      errorMessage = "unable to close exclusive temporary file: " +
                     std::string(std::strerror(errno));
      return false;
    }
    return true;
  }

private:
  int descriptor_ = -1;
};

int openExclusiveFile(const std::filesystem::path &path) {
#ifdef _WIN32
  std::string ignored;
  PrivateSecurityAttributes security;
  if (!security.initialize(ignored)) {
    errno = EACCES;
    return -1;
  }
  HANDLE handle = CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, security.get(), CREATE_NEW,
      FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD createError = GetLastError();
    errno =
        createError == ERROR_FILE_EXISTS || createError == ERROR_ALREADY_EXISTS
            ? EEXIST
            : EACCES;
    return -1;
  }
  const int descriptor = ::_open_osfhandle(reinterpret_cast<intptr_t>(handle),
                                           _O_RDWR | _O_BINARY | _O_NOINHERIT);
  if (descriptor < 0) {
    CloseHandle(handle);
  }
  return descriptor;
#else
  int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return ::open(path.c_str(), flags, 0600);
#endif
}

bool createExclusiveSiblingFile(const std::filesystem::path &path,
                                std::string_view tag,
                                std::filesystem::path &created,
                                ExclusiveFileDescriptor &descriptor,
                                std::string &errorMessage) {
  for (int attempt = 0; attempt < 128; ++attempt) {
    std::string suffix;
    if (!randomSuffix(suffix, errorMessage)) {
      return false;
    }
    const auto candidate = siblingCandidate(path, tag, suffix);
    const int opened = openExclusiveFile(candidate);
    if (opened >= 0) {
      created = candidate;
      descriptor = ExclusiveFileDescriptor(opened);
      return true;
    }
    bool collision = errno == EEXIST;
#ifdef ELOOP
    collision = collision || errno == ELOOP;
#endif
    if (collision) {
      continue;
    }
    errorMessage = "unable to create exclusive temporary file: " +
                   std::string(std::strerror(errno));
    return false;
  }
  errorMessage = "unable to allocate a collision-free temporary file";
  return false;
}

bool isValidUtf8(std::string_view value) {
  std::size_t index = 0;
  while (index < value.size()) {
    const unsigned char first = static_cast<unsigned char>(value[index]);
    std::uint32_t codePoint = 0;
    std::size_t length = 0;
    if (first < 0x80U) {
      codePoint = first;
      length = 1;
    } else if (first >= 0xc2U && first <= 0xdfU) {
      codePoint = first & 0x1fU;
      length = 2;
    } else if (first >= 0xe0U && first <= 0xefU) {
      codePoint = first & 0x0fU;
      length = 3;
    } else if (first >= 0xf0U && first <= 0xf4U) {
      codePoint = first & 0x07U;
      length = 4;
    } else {
      return false;
    }
    if (index + length > value.size()) {
      return false;
    }
    for (std::size_t continuation = 1; continuation < length; ++continuation) {
      const unsigned char byte =
          static_cast<unsigned char>(value[index + continuation]);
      if ((byte & 0xc0U) != 0x80U) {
        return false;
      }
      codePoint = (codePoint << 6U) | (byte & 0x3fU);
    }
    if ((length == 2 && codePoint < 0x80U) ||
        (length == 3 && codePoint < 0x800U) ||
        (length == 4 && codePoint < 0x10000U) ||
        (codePoint >= 0xd800U && codePoint <= 0xdfffU) ||
        codePoint > 0x10ffffU) {
      return false;
    }
    index += length;
  }
  return true;
}

bool isUuid(std::string_view value) {
  if (value.size() != 36) {
    return false;
  }
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8 || index == 13 || index == 18 || index == 23) {
      if (value[index] != '-') {
        return false;
      }
    } else if (std::isxdigit(static_cast<unsigned char>(value[index])) == 0) {
      return false;
    }
  }
  return true;
}

bool isPracticeMember(std::string_view name) {
  constexpr std::string_view prefix = "practice/";
  return name.starts_with(prefix) &&
         practice::classifyPresetFilename(name.substr(prefix.size())) ==
             practice::PresetFileKind::Primary;
}

bool isKnownMember(std::string_view name) {
  return std::ranges::find(kMemberNames, name) != kMemberNames.end() ||
         isPracticeMember(name);
}

std::vector<std::string>
archiveMemberNames(const std::filesystem::path &sourceDirectory,
                   std::string &errorMessage) {
  std::vector<std::string> names = {"manifest.json", "settings.json",
                                    "input.json", "scores.db", "replays.db"};
  std::error_code error;
  const auto practiceDirectory = sourceDirectory / "practice";
  if (std::filesystem::exists(practiceDirectory, error)) {
    std::filesystem::directory_iterator iterator(practiceDirectory, error);
    if (error) {
      errorMessage =
          "unable to enumerate staged practice data: " + error.message();
      return {};
    }
    std::vector<std::string> practiceNames;
    for (const auto &entry : iterator) {
      const std::string name = "practice/" + entry.path().filename().string();
      const auto status = std::filesystem::symlink_status(entry.path(), error);
      if (error || !std::filesystem::is_regular_file(status) ||
          std::filesystem::is_symlink(status) || !isPracticeMember(name)) {
        errorMessage = "staged practice data contains an unsafe entry";
        return {};
      }
      practiceNames.push_back(name);
    }
    std::ranges::sort(practiceNames);
    names.insert(names.end(), practiceNames.begin(), practiceNames.end());
  } else if (error) {
    errorMessage = "unable to inspect staged practice data: " + error.message();
    return {};
  }
  names.emplace_back("checksums.sha256");
  return names;
}

bool checkedAdd(std::uint64_t &total, std::uint64_t amount) {
  if (amount > std::numeric_limits<std::uint64_t>::max() - total) {
    return false;
  }
  total += amount;
  return ProfileArchiveSizePolicy::totalSizeAllowed(total);
}

bool writeTextFile(const std::filesystem::path &path, std::string_view contents,
                   std::string &errorMessage) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    errorMessage = "unable to open file for writing: " + path.string();
    return false;
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!output) {
    errorMessage = "unable to write file: " + path.string();
    return false;
  }
  output.close();
  if (!output) {
    errorMessage = "unable to close file: " + path.string();
    return false;
  }
  return true;
}

std::optional<std::string> readMetadataFile(const std::filesystem::path &path,
                                            std::string &errorMessage) {
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    errorMessage = "unable to inspect metadata file: " + error.message();
    return std::nullopt;
  }
  if (size > ProfileArchiveSizePolicy::kMaximumMetadataBytes) {
    errorMessage = "archive metadata exceeds its size limit";
    return std::nullopt;
  }
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    errorMessage = "unable to open metadata file: " + path.string();
    return std::nullopt;
  }
  std::string contents(static_cast<std::size_t>(size), '\0');
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  if (input.gcount() != static_cast<std::streamsize>(contents.size()) ||
      input.bad()) {
    errorMessage = "unable to read metadata file: " + path.string();
    return std::nullopt;
  }
  return contents;
}

bool copyFileStreaming(const std::filesystem::path &source,
                       const std::filesystem::path &destination,
                       std::string_view memberName, std::string &errorMessage) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(source, error);
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    errorMessage = "source is not a safe regular file: " + source.string();
    return false;
  }
  const auto size = std::filesystem::file_size(source, error);
  if (error || !ProfileArchiveSizePolicy::memberSizeAllowed(memberName, size)) {
    errorMessage =
        error ? "unable to inspect source file size: " + error.message()
              : "source file exceeds archive size limit";
    return false;
  }
  std::ifstream input(source, std::ios::binary);
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  if (!input || !output) {
    errorMessage = "unable to open profile component for copying";
    return false;
  }
  std::array<char, 64 * 1024> buffer{};
  std::uint64_t copied = 0;
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      copied += static_cast<std::uint64_t>(count);
      if (!ProfileArchiveSizePolicy::memberSizeAllowed(memberName, copied)) {
        errorMessage = "profile component grew beyond archive size limit";
        return false;
      }
      output.write(buffer.data(), count);
      if (!output) {
        errorMessage = "unable to write copied profile component";
        return false;
      }
    }
  }
  if (!input.eof()) {
    errorMessage = "unable to read profile component";
    return false;
  }
  output.close();
  if (!output) {
    errorMessage = "unable to close copied profile component";
    return false;
  }
  return copied == size;
}

Json manifestJson(const ProfileArchiveManifest &manifest) {
  return {{"createdAt", manifest.createdAt},
          {"formatVersion", manifest.formatVersion},
          {"inputSchemaVersion", manifest.inputSchemaVersion},
          {"practiceSchemaVersion", manifest.practiceSchemaVersion},
          {"profileDisplayName", manifest.profileDisplayName},
          {"profileSchemaVersion", manifest.profileSchemaVersion},
          {"profileUuid", manifest.profileUuid},
          {"replaySchemaVersion", manifest.replaySchemaVersion},
          {"scoreSchemaVersion", manifest.scoreSchemaVersion},
          {"settingsSchemaVersion", manifest.settingsSchemaVersion},
          {"sourceApplicationVersion", manifest.sourceApplicationVersion}};
}

struct ManifestParseResult {
  ProfileError error = ProfileError::None;
  std::string message;
  std::optional<ProfileArchiveManifest> manifest;
};

ManifestParseResult parseManifest(std::string_view contents) {
  if (!isValidUtf8(contents)) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "archive manifest is not valid UTF-8"};
  }
  const Json document = Json::parse(contents, nullptr, false);
  if (document.is_discarded() || !document.is_object() ||
      !document.contains("formatVersion") ||
      !document.at("formatVersion").is_number_integer()) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "archive manifest is invalid"};
  }
  int encodedFormatVersion = 0;
  try {
    encodedFormatVersion = document.at("formatVersion").get<int>();
  } catch (const Json::exception &error) {
    return {.error = ProfileError::IntegrityFailure,
            .message =
                std::string("archive manifest is invalid: ") + error.what()};
  }
  if (encodedFormatVersion > ProfileArchiveManifest::kFormatVersion) {
    return {.error = ProfileError::FutureVersion,
            .message = "archive requires a newer application version"};
  }
  const auto requiredKeys =
      encodedFormatVersion == 1
          ? std::span<const std::string_view>(kVersionOneManifestKeys)
          : std::span<const std::string_view>(kVersionTwoManifestKeys);
  if ((encodedFormatVersion != 1 &&
       encodedFormatVersion != ProfileArchiveManifest::kFormatVersion) ||
      document.size() != requiredKeys.size()) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "archive manifest contains unsupported metadata"};
  }
  for (const std::string_view key : requiredKeys) {
    if (!document.contains(std::string(key))) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive manifest is missing required fields"};
    }
  }
  try {
    ProfileArchiveManifest manifest;
    manifest.createdAt = document.at("createdAt").get<std::string>();
    manifest.formatVersion = document.at("formatVersion").get<int>();
    manifest.inputSchemaVersion = document.at("inputSchemaVersion").get<int>();
    manifest.practiceSchemaVersion =
        manifest.formatVersion >= 2
            ? document.at("practiceSchemaVersion").get<int>()
            : 0;
    manifest.profileDisplayName =
        document.at("profileDisplayName").get<std::string>();
    manifest.profileSchemaVersion =
        document.at("profileSchemaVersion").get<int>();
    manifest.profileUuid = document.at("profileUuid").get<std::string>();
    manifest.replaySchemaVersion =
        document.at("replaySchemaVersion").get<int>();
    manifest.scoreSchemaVersion = document.at("scoreSchemaVersion").get<int>();
    manifest.settingsSchemaVersion =
        document.at("settingsSchemaVersion").get<int>();
    manifest.sourceApplicationVersion =
        document.at("sourceApplicationVersion").get<std::string>();

    if (manifest.formatVersion > ProfileArchiveManifest::kFormatVersion ||
        manifest.profileSchemaVersion > kPlayerProfileSchemaVersion ||
        manifest.settingsSchemaVersion >
            AppSettingsStore::kCurrentSchemaVersion ||
        manifest.inputSchemaVersion > InputProfile::kSchemaVersion ||
        manifest.practiceSchemaVersion > 1 ||
        manifest.scoreSchemaVersion > ScoreDBHelper::kCurrentSchemaVersion ||
        manifest.replaySchemaVersion > ReplayDBHelper::kCurrentSchemaVersion) {
      return {.error = ProfileError::FutureVersion,
              .message = "archive requires a newer application version"};
    }
    if (manifest.scoreSchemaVersion < 4) {
      return {.error = ProfileError::IntegrityFailure,
              .message =
                  "score database schemas older than version 4 are unsupported "
                  "for portable import because their migration requires chart "
                  "library context"};
    }
    if ((manifest.formatVersion != 1 &&
         manifest.formatVersion != ProfileArchiveManifest::kFormatVersion) ||
        manifest.profileSchemaVersion < 0 ||
        manifest.settingsSchemaVersion < 0 || manifest.inputSchemaVersion < 0 ||
        manifest.practiceSchemaVersion < 0 ||
        (manifest.formatVersion >= 2 && manifest.practiceSchemaVersion != 1) ||
        manifest.replaySchemaVersion < 0 ||
        manifest.sourceApplicationVersion.empty() ||
        !isUuid(manifest.profileUuid) || manifest.profileDisplayName.empty() ||
        manifest.createdAt.empty()) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive manifest contains unsupported metadata"};
    }
    return {.manifest = std::move(manifest)};
  } catch (const Json::exception &error) {
    return {.error = ProfileError::IntegrityFailure,
            .message =
                std::string("archive manifest is invalid: ") + error.what()};
  }
}

std::string canonicalChecksums(const std::filesystem::path &directory,
                               std::span<const std::string> memberNames,
                               std::string &errorMessage) {
  std::string result;
  for (const std::string_view name : memberNames) {
    if (name == "checksums.sha256") {
      continue;
    }
    const auto digest =
        file_checksum::sha256File(directory / std::string(name), errorMessage);
    if (!digest) {
      return {};
    }
    result += *digest;
    result += "  ";
    result += name;
    result += '\n';
  }
  return result;
}

bool constantTimeEqual(std::string_view left, std::string_view right) {
  std::size_t difference = left.size() ^ right.size();
  const std::size_t shared = std::min(left.size(), right.size());
  for (std::size_t index = 0; index < shared; ++index) {
    difference |= static_cast<unsigned char>(left[index]) ^
                  static_cast<unsigned char>(right[index]);
  }
  return difference == 0;
}

bool removePath(const std::filesystem::path &path, std::string &errorMessage) {
  std::error_code error;
  std::filesystem::remove(path, error);
  if (error) {
    errorMessage =
        "unable to remove '" + path.string() + "': " + error.message();
    return false;
  }
  return true;
}

bool writeToDescriptor(int descriptor, const char *data, std::size_t size,
                       std::string &errorMessage) {
  std::size_t offset = 0;
  while (offset < size) {
#ifdef _WIN32
    const unsigned int chunk = static_cast<unsigned int>(
        std::min<std::size_t>(size - offset, 1U << 30U));
    const int written = ::_write(descriptor, data + offset, chunk);
#else
    const ssize_t written = ::write(descriptor, data + offset, size - offset);
#endif
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      errorMessage = "unable to write exclusive temporary file: " +
                     std::string(std::strerror(errno));
      return false;
    }
    if (written == 0) {
      errorMessage = "unable to write exclusive temporary file: no progress";
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool copyToDescriptor(const std::filesystem::path &source, int descriptor,
                      std::uint64_t expectedBytes, std::string &copiedDigest,
                      std::string &errorMessage) {
  std::ifstream input(source, std::ios::binary);
  if (!input) {
    errorMessage = "unable to open existing profile archive for backup";
    return false;
  }
  file_checksum::Sha256 checksum;
  std::uint64_t copiedBytes = 0;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count > 0) {
      if (count > expectedBytes - copiedBytes) {
        errorMessage = "existing profile archive changed or exceeds the "
                       "rollback backup size limit";
        return false;
      }
      checksum.update(std::as_bytes(std::span(buffer.data(), count)));
      if (!writeToDescriptor(descriptor, buffer.data(), count, errorMessage)) {
        return false;
      }
      copiedBytes += count;
    }
  }
  if (!input.eof()) {
    errorMessage = "unable to finish reading existing profile archive";
    return false;
  }
  if (copiedBytes != expectedBytes) {
    errorMessage =
        "existing profile archive changed while preparing its backup";
    return false;
  }
  copiedDigest = checksum.finalHex();
  return true;
}

bool writeZip(const std::filesystem::path &sourceDirectory,
              int archiveDescriptor, std::string &errorMessage) {
  auto writer = makeArchiveWriteHandle();
  if (!writer) {
    errorMessage = "unable to allocate archive writer";
    return false;
  }
  if (archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
      archive_write_set_options(writer.get(), "zip:compression=store") !=
          ARCHIVE_OK ||
      archive_write_set_bytes_per_block(writer.get(), 0) != ARCHIVE_OK ||
      archive_write_open_fd(writer.get(), archiveDescriptor) != ARCHIVE_OK) {
    errorMessage = archiveError(writer.get(), "unable to open archive writer");
    return false;
  }

  using EntryHandle =
      std::unique_ptr<archive_entry, decltype(&archive_entry_free)>;
  std::array<char, 64 * 1024> buffer{};
  const auto memberNames = archiveMemberNames(sourceDirectory, errorMessage);
  if (!errorMessage.empty()) {
    return false;
  }
  for (const std::string_view name : memberNames) {
    const auto source = sourceDirectory / std::string(name);
    std::error_code sizeError;
    const auto size = std::filesystem::file_size(source, sizeError);
    if (sizeError || !ProfileArchiveSizePolicy::memberSizeAllowed(name, size)) {
      errorMessage =
          sizeError ? "unable to inspect archive member: " + sizeError.message()
                    : "archive member exceeds size limit";
      return false;
    }
    EntryHandle entry(archive_entry_new(), archive_entry_free);
    if (!entry) {
      errorMessage = "unable to allocate archive entry";
      return false;
    }
    archive_entry_set_pathname(entry.get(), std::string(name).c_str());
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_uid(entry.get(), 0);
    archive_entry_set_gid(entry.get(), 0);
    archive_entry_set_mtime(entry.get(), 0, 0);
    archive_entry_unset_atime(entry.get());
    archive_entry_unset_ctime(entry.get());
    archive_entry_unset_birthtime(entry.get());
    archive_entry_set_size(entry.get(), static_cast<la_int64_t>(size));
    if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK) {
      errorMessage = archiveError(writer.get(), "unable to write ZIP header");
      return false;
    }
    std::ifstream input(source, std::ios::binary);
    if (!input) {
      errorMessage = "unable to read archive member: " + source.string();
      return false;
    }
    while (input) {
      input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
      std::size_t remaining = static_cast<std::size_t>(input.gcount());
      std::size_t offset = 0;
      while (remaining > 0) {
        const la_ssize_t written =
            archive_write_data(writer.get(), buffer.data() + offset, remaining);
        if (written <= 0) {
          errorMessage =
              archiveError(writer.get(), "unable to write ZIP member data");
          return false;
        }
        offset += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
      }
    }
    if (!input.eof()) {
      errorMessage = "unable to finish reading archive member";
      return false;
    }
    if (archive_write_finish_entry(writer.get()) != ARCHIVE_OK) {
      errorMessage = archiveError(writer.get(), "unable to finish ZIP entry");
      return false;
    }
  }
  if (archive_write_close(writer.get()) != ARCHIVE_OK) {
    errorMessage = archiveError(writer.get(), "unable to close ZIP archive");
    return false;
  }
  return true;
}

struct ArchiveValidationResult {
  ProfileError error = ProfileError::None;
  std::string message;
  std::optional<ProfileArchiveManifest> manifest;

  [[nodiscard]] bool ok() const { return error == ProfileError::None; }
};

int openArchiveReader(archive *reader, const std::filesystem::path &path) {
#ifdef _WIN32
  return archive_read_open_filename_w(reader, path.c_str(), 64 * 1024);
#else
  return archive_read_open_filename(reader, path.c_str(), 64 * 1024);
#endif
}

ArchiveValidationResult
validateArchive(const std::filesystem::path &archivePath,
                const std::filesystem::path &extractDirectory,
                const ProfileArchiveValidationOperations &validation) {
  std::error_code filesystemError;
  const auto archiveStatus =
      std::filesystem::symlink_status(archivePath, filesystemError);
  if (filesystemError || !std::filesystem::is_regular_file(archiveStatus)) {
    return {.error = ProfileError::IoFailure,
            .message = "profile archive is missing or is not a regular file"};
  }
  std::string directoryError;
  if (!createPrivateDirectory(extractDirectory, directoryError)) {
    return {.error = ProfileError::IoFailure,
            .message = "unable to create archive extraction directory: " +
                       directoryError};
  }

  auto reader = makeArchiveReadHandle();
  if (!reader) {
    return {.error = ProfileError::IoFailure,
            .message = "unable to allocate archive reader"};
  }
  if (archive_read_support_filter_none(reader.get()) != ARCHIVE_OK ||
      archive_read_support_format_zip(reader.get()) != ARCHIVE_OK ||
      openArchiveReader(reader.get(), archivePath) != ARCHIVE_OK) {
    return {.error = ProfileError::IntegrityFailure,
            .message = archiveError(reader.get(),
                                    "unable to open portable profile ZIP")};
  }

  std::set<std::string, std::less<>> seen;
  std::vector<std::string> memberNames;
  std::uint64_t declaredTotal = 0;
  std::uint64_t actualTotal = 0;
  std::array<char, 64 * 1024> buffer{};
  while (true) {
    archive_entry *entry = nullptr;
    const int status = archive_read_next_header(reader.get(), &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status != ARCHIVE_OK || entry == nullptr) {
      return {.error = ProfileError::IntegrityFailure,
              .message =
                  archiveError(reader.get(), "unable to read archive entry")};
    }
    if ((archive_format(reader.get()) & ARCHIVE_FORMAT_BASE_MASK) !=
        ARCHIVE_FORMAT_ZIP) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "portable profile is not a ZIP archive"};
    }
    const char *rawName = archive_entry_pathname(entry);
    if (rawName == nullptr) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive entry has no name"};
    }
    const std::string name(rawName);
    if (!isValidUtf8(name) || !isKnownMember(name) ||
        !seen.insert(name).second) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive contains an invalid, duplicate, or "
                         "unexpected member name"};
    }
    memberNames.push_back(name);
    if (archive_entry_filetype(entry) != AE_IFREG ||
        archive_entry_symlink(entry) != nullptr ||
        archive_entry_hardlink(entry) != nullptr ||
        archive_entry_sparse_count(entry) != 0 ||
        archive_entry_is_encrypted(entry) > 0) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive members must be unencrypted regular files"};
    }
    if (archive_entry_size_is_set(entry) == 0 ||
        archive_entry_size(entry) < 0) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive member has no valid declared size"};
    }
    const auto declared = static_cast<std::uint64_t>(archive_entry_size(entry));
    if (!ProfileArchiveSizePolicy::additionAllowed(name, 0, declaredTotal,
                                                   declared) ||
        !validation.declaredSizeAllowed(name, 0, declaredTotal, declared)) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive declared size exceeds the safety limit"};
    }
    declaredTotal += declared;

    const auto outputPath = extractDirectory / name;
    if (isPracticeMember(name)) {
      std::error_code directoryError;
      std::filesystem::create_directory(extractDirectory / "practice",
                                        directoryError);
      if (directoryError &&
          !std::filesystem::is_directory(extractDirectory / "practice")) {
        return {.error = ProfileError::IoFailure,
                .message = "unable to create extracted practice directory"};
      }
    }
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      return {.error = ProfileError::IoFailure,
              .message = "unable to create extracted archive member"};
    }
    std::uint64_t actual = 0;
    while (true) {
      const la_ssize_t count =
          archive_read_data(reader.get(), buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        return {.error = ProfileError::IntegrityFailure,
                .message = archiveError(reader.get(),
                                        "unable to read archive member")};
      }
      const auto chunk = static_cast<std::uint64_t>(count);
      if (!ProfileArchiveSizePolicy::additionAllowed(name, actual, actualTotal,
                                                     chunk) ||
          !validation.streamedSizeAllowed(name, actual, actualTotal, chunk)) {
        return {.error = ProfileError::IntegrityFailure,
                .message = "archive stream exceeds the safety limit"};
      }
      actual += chunk;
      actualTotal += chunk;
      output.write(buffer.data(), count);
      if (!output) {
        return {.error = ProfileError::IoFailure,
                .message = "unable to write extracted archive member"};
      }
    }
    output.close();
    if (!output || actual != declared) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive member size does not match its declaration"};
    }
  }
  for (const std::string_view name : kMemberNames) {
    if (!seen.contains(name)) {
      return {.error = ProfileError::IntegrityFailure,
              .message = "archive is missing a required member"};
    }
  }
  if (archive_read_has_encrypted_entries(reader.get()) > 0) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "encrypted profile archives are not supported"};
  }

  std::string errorMessage;
  const auto checksums =
      readMetadataFile(extractDirectory / "checksums.sha256", errorMessage);
  if (!checksums || !isValidUtf8(*checksums)) {
    return {.error = ProfileError::IntegrityFailure,
            .message = errorMessage.empty()
                           ? "checksum manifest is not valid UTF-8"
                           : errorMessage};
  }
  const std::string expected =
      canonicalChecksums(extractDirectory, memberNames, errorMessage);
  if (!errorMessage.empty()) {
    return {.error = ProfileError::IoFailure, .message = errorMessage};
  }
  if (!constantTimeEqual(*checksums, expected)) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "archive checksum verification failed"};
  }

  const auto manifestContents =
      readMetadataFile(extractDirectory / "manifest.json", errorMessage);
  const auto settingsContents =
      readMetadataFile(extractDirectory / "settings.json", errorMessage);
  const auto inputContents =
      readMetadataFile(extractDirectory / "input.json", errorMessage);
  if (!manifestContents || !settingsContents || !inputContents) {
    return {.error = ProfileError::IntegrityFailure, .message = errorMessage};
  }
  if (!isValidUtf8(*settingsContents) || !isValidUtf8(*inputContents)) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "profile JSON metadata is not valid UTF-8"};
  }
  ManifestParseResult manifest = parseManifest(*manifestContents);
  if (manifest.error != ProfileError::None || !manifest.manifest) {
    return {.error = manifest.error, .message = std::move(manifest.message)};
  }
  const bool hasPracticeMembers =
      std::ranges::any_of(memberNames, [](std::string_view name) {
        return isPracticeMember(name);
      });
  if (manifest.manifest->formatVersion == 1 && hasPracticeMembers) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "version-one archive cannot contain practice data"};
  }
  for (const std::string &name : memberNames) {
    if (!isPracticeMember(name)) {
      continue;
    }
    const auto practiceValidation = practice::validatePresetFile(
        extractDirectory / name, manifest.manifest->practiceSchemaVersion);
    if (!practiceValidation.valid()) {
      const std::string detail = practiceValidation.diagnostics.empty()
                                     ? "archive practice preset is invalid"
                                     : practiceValidation.diagnostics.front();
      return {.error = practiceValidation.status ==
                               versioned_json::LoadStatus::FutureVersion
                           ? ProfileError::FutureVersion
                           : ProfileError::IntegrityFailure,
              .message = detail};
    }
  }

  const auto settings =
      AppSettingsStore::Load(extractDirectory / "settings.json");
  if (settings.status == AppSettingsLoadStatus::FutureVersion) {
    return {.error = ProfileError::FutureVersion,
            .message = "archive settings are newer than supported"};
  }
  if (settings.status != AppSettingsLoadStatus::Loaded) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "archive settings are invalid"};
  }
  const auto input = InputProfileStore::load(extractDirectory / "input.json");
  if (input.status == InputProfileLoadStatus::FutureVersion) {
    return {.error = ProfileError::FutureVersion,
            .message = "archive input profile is newer than supported"};
  }
  if (input.status != InputProfileLoadStatus::Loaded) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "archive input profile is invalid"};
  }

  const Json settingsDocument = Json::parse(*settingsContents, nullptr, false);
  const Json inputDocument = Json::parse(*inputContents, nullptr, false);
  if (settingsDocument.is_discarded() || inputDocument.is_discarded() ||
      !settingsDocument.is_object() || !inputDocument.is_object() ||
      !settingsDocument.contains("schemaVersion") ||
      !inputDocument.contains("schemaVersion") ||
      !settingsDocument.at("schemaVersion").is_number_integer() ||
      !inputDocument.at("schemaVersion").is_number_integer() ||
      settingsDocument.at("schemaVersion").get<int>() !=
          manifest.manifest->settingsSchemaVersion ||
      inputDocument.at("schemaVersion").get<int>() !=
          manifest.manifest->inputSchemaVersion) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "manifest and JSON component versions do not match"};
  }

  const auto scoreVersion =
      sqliteDatabaseUserVersion(extractDirectory / "scores.db", errorMessage);
  const auto replayVersion =
      sqliteDatabaseUserVersion(extractDirectory / "replays.db", errorMessage);
  if (!scoreVersion || !replayVersion) {
    return {.error = ProfileError::IntegrityFailure, .message = errorMessage};
  }
  if (*scoreVersion > ScoreDBHelper::kCurrentSchemaVersion ||
      *replayVersion > ReplayDBHelper::kCurrentSchemaVersion) {
    return {.error = ProfileError::FutureVersion,
            .message = "archive database is newer than supported"};
  }
  if (*scoreVersion != manifest.manifest->scoreSchemaVersion ||
      *replayVersion != manifest.manifest->replaySchemaVersion) {
    return {.error = ProfileError::IntegrityFailure,
            .message = "manifest and database component versions do not match"};
  }
  if (*scoreVersion < 4) {
    return {
        .error = ProfileError::IntegrityFailure,
        .message =
            "score database schemas older than version 4 are unsupported for "
            "portable import because their migration requires chart library "
            "context"};
  }
  if (*replayVersion < 0 ||
      !sqliteIntegrityCheck(extractDirectory / "scores.db", errorMessage) ||
      !sqliteIntegrityCheck(extractDirectory / "replays.db", errorMessage)) {
    return {.error = ProfileError::IntegrityFailure,
            .message = errorMessage.empty()
                           ? "archive database schema is not current"
                           : errorMessage};
  }
  return {.manifest = std::move(manifest.manifest)};
}
} // namespace

bool ProfileArchiveSizePolicy::memberSizeAllowed(std::string_view memberName,
                                                 std::uint64_t bytes) {
  if (memberName == "scores.db" || memberName == "replays.db") {
    return bytes <= kMaximumDatabaseBytes;
  }
  return isKnownMember(memberName) && bytes <= kMaximumMetadataBytes;
}

bool ProfileArchiveSizePolicy::totalSizeAllowed(std::uint64_t bytes) {
  return bytes <= kMaximumTotalBytes;
}

bool ProfileArchiveSizePolicy::additionAllowed(std::string_view memberName,
                                               std::uint64_t currentMemberBytes,
                                               std::uint64_t currentTotalBytes,
                                               std::uint64_t additionalBytes) {
  if (additionalBytes >
          std::numeric_limits<std::uint64_t>::max() - currentMemberBytes ||
      additionalBytes >
          std::numeric_limits<std::uint64_t>::max() - currentTotalBytes) {
    return false;
  }
  return memberSizeAllowed(memberName, currentMemberBytes + additionalBytes) &&
         totalSizeAllowed(currentTotalBytes + additionalBytes);
}

ProfileArchiveService::ProfileArchiveService(
    PlayerProfileManager &manager, ProfileArchiveDependencies dependencies)
    : manager_(manager), dependencies_(std::move(dependencies)) {
  if (!dependencies_.filesystem.syncFile) {
    dependencies_.filesystem.syncFile = atomic_file::syncFile;
  }
  if (!dependencies_.filesystem.syncDirectory) {
    dependencies_.filesystem.syncDirectory = atomic_file::syncDirectory;
  }
  if (!dependencies_.filesystem.durableRename) {
    dependencies_.filesystem.durableRename = atomic_file::renameDurably;
  }
  if (!dependencies_.filesystem.removePath) {
    dependencies_.filesystem.removePath = removePath;
  }
  if (!dependencies_.validation.declaredSizeAllowed) {
    dependencies_.validation.declaredSizeAllowed =
        ProfileArchiveSizePolicy::additionAllowed;
  }
  if (!dependencies_.validation.streamedSizeAllowed) {
    dependencies_.validation.streamedSizeAllowed =
        ProfileArchiveSizePolicy::additionAllowed;
  }
  cleanupStaleImportWorkspaces(startupCleanupError_);
}

ProfileArchiveResult
ProfileArchiveService::Export(std::string_view profileId,
                              const std::filesystem::path &destination) {
  const ProfileResult validated = manager_.validateProfile(profileId);
  if (!validated.ok() || !validated.profile) {
    return {.error = validated.error,
            .message = validated.message,
            .profile = std::nullopt};
  }
  if (destination.empty() || destination.filename().empty()) {
    return failure(ProfileError::IoFailure,
                   "profile export destination is invalid");
  }
  const auto parent = destination.parent_path().empty()
                          ? std::filesystem::path(".")
                          : destination.parent_path();
  std::error_code filesystemError;
  const auto parentStatus =
      std::filesystem::symlink_status(parent, filesystemError);
  if (filesystemError || !std::filesystem::is_directory(parentStatus)) {
    return failure(ProfileError::IoFailure,
                   "profile export directory is missing");
  }
  bool managedDestination = false;
  std::string managedPathError;
  if (!classifyPathWithin(manager_.applicationDataRoot(), destination,
                          managedDestination, managedPathError)) {
    return failure(ProfileError::IoFailure, managedPathError);
  }
  if (managedDestination) {
    return failure(ProfileError::IoFailure,
                   "profile exports cannot replace managed application data");
  }
  std::string staleCleanupError;
  if (!cleanupStaleExportArtifacts(destination, staleCleanupError)) {
    return failure(ProfileError::IoFailure, staleCleanupError);
  }
  std::filesystem::path temporaryArchive;
  std::filesystem::path workspace;
  std::string errorMessage;
  if (!createPrivateSiblingDirectory(destination, ".work-", workspace,
                                     errorMessage)) {
    return failure(ProfileError::IoFailure,
                   "unable to allocate profile export workspace: " +
                       errorMessage);
  }
  auto cleanup = makeScopeExit([&] {
    std::error_code ignored;
    if (!temporaryArchive.empty()) {
      std::filesystem::remove(temporaryArchive, ignored);
    }
    if (!workspace.empty()) {
      std::filesystem::remove_all(workspace, ignored);
    }
  });

  const PlayerProfilePaths source = manager_.pathsFor(profileId);
  std::filesystem::create_directory(workspace / "practice", filesystemError);
  if (filesystemError) {
    return failure(ProfileError::IoFailure,
                   "unable to stage practice export directory: " +
                       filesystemError.message());
  }
  if (!copyFileStreaming(source.settingsJson, workspace / "settings.json",
                         "settings.json", errorMessage) ||
      !copyFileStreaming(source.inputJson, workspace / "input.json",
                         "input.json", errorMessage) ||
      !snapshotSqliteDatabase(source.scoresDb, workspace / "scores.db",
                              errorMessage) ||
      !snapshotSqliteDatabase(source.replaysDb, workspace / "replays.db",
                              errorMessage)) {
    return failure(ProfileError::IoFailure,
                   "unable to stage profile export: " + errorMessage);
  }
  if (std::filesystem::exists(source.practiceDirectory, filesystemError)) {
    std::filesystem::directory_iterator iterator(source.practiceDirectory,
                                                 filesystemError);
    if (filesystemError) {
      return failure(ProfileError::IoFailure,
                     "unable to enumerate practice data for export: " +
                         filesystemError.message());
    }
    std::vector<std::filesystem::path> practiceFiles;
    for (const auto &entry : iterator) {
      practiceFiles.push_back(entry.path());
    }
    std::ranges::sort(practiceFiles);
    for (const auto &practiceFile : practiceFiles) {
      const std::string memberName =
          "practice/" + practiceFile.filename().string();
      const practice::PresetFileKind kind =
          practice::classifyPresetFilename(practiceFile.filename().string());
      if (kind == practice::PresetFileKind::AtomicSidecar) {
        const auto status =
            std::filesystem::symlink_status(practiceFile, filesystemError);
        const auto size =
            filesystemError
                ? 0
                : std::filesystem::file_size(practiceFile, filesystemError);
        if (filesystemError || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status) ||
            size > ProfileArchiveSizePolicy::kMaximumMetadataBytes) {
          return failure(ProfileError::IoFailure,
                         "unable to validate practice backup sidecar for "
                         "export");
        }
        continue;
      }
      if (kind != practice::PresetFileKind::Primary ||
          !copyFileStreaming(practiceFile,
                             workspace / "practice" / practiceFile.filename(),
                             memberName, errorMessage)) {
        return failure(ProfileError::IoFailure,
                       "unable to stage practice export: " + errorMessage);
      }
    }
  } else if (filesystemError) {
    return failure(ProfileError::IoFailure,
                   "unable to inspect practice data for export: " +
                       filesystemError.message());
  }
  for (const std::string_view database : {"scores.db", "replays.db"}) {
    const auto size = std::filesystem::file_size(
        workspace / std::string(database), filesystemError);
    if (filesystemError ||
        !ProfileArchiveSizePolicy::memberSizeAllowed(database, size)) {
      return failure(ProfileError::IntegrityFailure,
                     "profile database exceeds the export size limit");
    }
  }

  ProfileArchiveManifest manifest;
  manifest.sourceApplicationVersion = std::string(kApplicationVersion);
  manifest.profileUuid = validated.profile->id;
  manifest.profileDisplayName = validated.profile->displayName;
  manifest.createdAt = validated.profile->createdAt;
  manifest.profileSchemaVersion = kPlayerProfileSchemaVersion;
  manifest.settingsSchemaVersion = AppSettingsStore::kCurrentSchemaVersion;
  manifest.inputSchemaVersion = InputProfile::kSchemaVersion;
  manifest.practiceSchemaVersion = 1;
  manifest.scoreSchemaVersion = ScoreDBHelper::kCurrentSchemaVersion;
  manifest.replaySchemaVersion = ReplayDBHelper::kCurrentSchemaVersion;
  if (!writeTextFile(workspace / "manifest.json",
                     manifestJson(manifest).dump(2) + "\n", errorMessage)) {
    return failure(ProfileError::IoFailure, errorMessage);
  }
  const auto stagedMemberNames = archiveMemberNames(workspace, errorMessage);
  const std::string checksums =
      errorMessage.empty()
          ? canonicalChecksums(workspace, stagedMemberNames, errorMessage)
          : std::string{};
  if (!errorMessage.empty() ||
      !writeTextFile(workspace / "checksums.sha256", checksums, errorMessage)) {
    return failure(ProfileError::IoFailure,
                   "unable to write archive checksums: " + errorMessage);
  }

  std::uint64_t total = 0;
  for (const std::string_view name : stagedMemberNames) {
    const auto size = std::filesystem::file_size(workspace / std::string(name),
                                                 filesystemError);
    if (filesystemError ||
        !ProfileArchiveSizePolicy::memberSizeAllowed(name, size) ||
        !checkedAdd(total, size)) {
      return failure(ProfileError::IntegrityFailure,
                     "profile export exceeds the size limit");
    }
  }
  ExclusiveFileDescriptor temporaryDescriptor;
  if (!createExclusiveSiblingFile(destination, ".tmp-", temporaryArchive,
                                  temporaryDescriptor, errorMessage) ||
      !writeZip(workspace, temporaryDescriptor.get(), errorMessage) ||
      !temporaryDescriptor.sync(errorMessage) ||
      !temporaryDescriptor.close(errorMessage) ||
      !dependencies_.filesystem.syncFile(temporaryArchive, errorMessage)) {
    return failure(ProfileError::IoFailure,
                   "unable to create durable profile archive: " + errorMessage);
  }
  if (dependencies_.beforeExportPhase &&
      !dependencies_.beforeExportPhase(
          ProfileArchiveExportPhase::TemporaryArchiveWritten, errorMessage)) {
    return failure(ProfileError::IoFailure, errorMessage);
  }

  const auto verificationDirectory = workspace / "verification";
  const ArchiveValidationResult verified = validateArchive(
      temporaryArchive, verificationDirectory, dependencies_.validation);
  if (!verified.ok() || !verified.manifest ||
      verified.manifest->profileUuid != validated.profile->id) {
    return failure(verified.ok() ? ProfileError::IntegrityFailure
                                 : verified.error,
                   verified.ok() ? "exported archive verification mismatch"
                                 : verified.message);
  }
  const auto destinationStatus =
      std::filesystem::symlink_status(destination, filesystemError);
  const bool hadDestination =
      !filesystemError &&
      destinationStatus.type() != std::filesystem::file_type::not_found;
  if (filesystemError &&
      filesystemError !=
          std::make_error_code(std::errc::no_such_file_or_directory)) {
    return failure(ProfileError::IoFailure,
                   "unable to inspect profile export destination: " +
                       filesystemError.message());
  }
  if (hadDestination && !std::filesystem::is_regular_file(destinationStatus)) {
    return failure(ProfileError::IoFailure,
                   "profile export destination is not a regular file");
  }
  std::uint64_t existingDestinationBytes = 0;
  if (hadDestination) {
    const auto size = std::filesystem::file_size(destination, filesystemError);
    if (filesystemError ||
        size > ProfileArchiveSizePolicy::kMaximumExistingArchiveBytes) {
      return failure(
          ProfileError::IoFailure,
          filesystemError
              ? "unable to inspect existing profile archive size: " +
                    filesystemError.message()
              : "existing profile archive exceeds the rollback backup limit");
    }
    existingDestinationBytes = static_cast<std::uint64_t>(size);
  }
  std::filesystem::path backup;
  auto cleanupBackup = makeScopeExit([&] {
    std::error_code ignored;
    if (!backup.empty()) {
      std::filesystem::remove(backup, ignored);
    }
  });
  auto rollback = [&](std::string message) {
    std::string rollbackError;
    if (hadDestination) {
      if (!dependencies_.filesystem.durableRename(backup, destination,
                                                  rollbackError)) {
        message += "; unable to restore prior export: " + rollbackError;
        // Retain the durable old copy when restoration itself fails.
        cleanupBackup.dismiss();
      }
    } else {
      bool currentExists = false;
      std::error_code existsError;
      currentExists = std::filesystem::exists(destination, existsError);
      if (existsError) {
        message +=
            "; unable to inspect failed export: " + existsError.message();
      } else if (currentExists && !dependencies_.filesystem.removePath(
                                      destination, rollbackError)) {
        message += "; unable to remove failed export: " + rollbackError;
      }
    }
    rollbackError.clear();
    if (!dependencies_.filesystem.syncDirectory(parent, rollbackError)) {
      message += "; unable to sync export rollback: " + rollbackError;
    }
    return failure(ProfileError::IoFailure, std::move(message));
  };
  if (hadDestination) {
    ExclusiveFileDescriptor backupDescriptor;
    std::string copiedDigest;
    if (!createExclusiveSiblingFile(destination, ".backup-", backup,
                                    backupDescriptor, errorMessage) ||
        !copyToDescriptor(destination, backupDescriptor.get(),
                          existingDestinationBytes, copiedDigest,
                          errorMessage) ||
        !backupDescriptor.sync(errorMessage) ||
        !backupDescriptor.close(errorMessage) ||
        !dependencies_.filesystem.syncFile(backup, errorMessage)) {
      return failure(ProfileError::IoFailure,
                     "unable to create a durable profile archive backup: " +
                         errorMessage);
    }
    const auto currentSize =
        std::filesystem::file_size(destination, filesystemError);
    const auto currentDigest = file_checksum::sha256File(
        destination, errorMessage, existingDestinationBytes);
    if (filesystemError || currentSize != existingDestinationBytes ||
        !currentDigest || *currentDigest != copiedDigest) {
      return failure(
          ProfileError::IoFailure,
          "existing profile archive changed while preparing rollback");
    }
    if (!dependencies_.filesystem.syncDirectory(parent, errorMessage)) {
      return failure(ProfileError::IoFailure,
                     "unable to sync profile archive backup: " + errorMessage);
    }
  }
  if (!dependencies_.filesystem.durableRename(temporaryArchive, destination,
                                              errorMessage)) {
    return rollback("unable to replace profile archive: " + errorMessage);
  }
  if (!dependencies_.filesystem.syncDirectory(parent, errorMessage)) {
    return rollback("unable to sync committed profile archive: " +
                    errorMessage);
  }
  // The new destination is the durable commit point. Old-backup cleanup must
  // not turn a completed export into a reported failure.
  std::string cleanupWarning;
  if (hadDestination) {
    if (!dependencies_.filesystem.removePath(backup, errorMessage)) {
      cleanupWarning =
          "profile exported, but its rollback backup could not be removed: " +
          errorMessage;
    } else if (!dependencies_.filesystem.syncDirectory(parent, errorMessage)) {
      cleanupWarning =
          "profile exported, but backup cleanup could not be synced: " +
          errorMessage;
    }
    cleanupBackup.dismiss();
  }
  std::error_code workspaceCleanupError;
  std::filesystem::remove_all(workspace, workspaceCleanupError);
  cleanup.dismiss();
  if (workspaceCleanupError) {
    if (!cleanupWarning.empty()) {
      cleanupWarning += "; ";
    }
    cleanupWarning += "export workspace cleanup is deferred: " +
                      workspaceCleanupError.message();
  }
  return success(*validated.profile, std::move(cleanupWarning));
}

ProfileArchiveResult
ProfileArchiveService::Import(const std::filesystem::path &archivePath,
                              const ProfileImportOptions &options) {
  if (!startupCleanupError_.empty()) {
    std::string retryError;
    if (!cleanupStaleImportWorkspaces(retryError)) {
      return failure(ProfileError::IoFailure,
                     "unable to recover stale import workspaces: " +
                         retryError);
    }
    startupCleanupError_.clear();
  }
  if (options.mode == ProfileImportMode::Overwrite &&
      !options.overwriteProfileId) {
    return failure(ProfileError::NotFound,
                   "overwrite import requires a target profile ID");
  }
  if (options.mode == ProfileImportMode::CreateWithNewId &&
      options.overwriteProfileId) {
    return failure(ProfileError::IntegrityFailure,
                   "create import cannot specify an overwrite target");
  }
  std::string staleCleanupError;
  if (!cleanupStaleImportWorkspaces(staleCleanupError)) {
    return failure(ProfileError::IoFailure, staleCleanupError);
  }
  std::filesystem::path workspace;
  std::string workspaceError;
  if (!createPrivateSiblingDirectory(std::filesystem::temp_directory_path() /
                                         "asobmashow-profile-import",
                                     "-", workspace, workspaceError)) {
    return failure(ProfileError::IoFailure,
                   "unable to allocate profile import workspace: " +
                       workspaceError);
  }
  if (dependencies_.importWorkspaceCreated) {
    dependencies_.importWorkspaceCreated(workspace);
  }
  auto cleanup = makeScopeExit([&] {
    std::error_code ignored;
    std::filesystem::remove_all(workspace, ignored);
  });
  const auto extracted = workspace / "extracted";
  const ArchiveValidationResult validated =
      validateArchive(archivePath, extracted, dependencies_.validation);
  if (!validated.ok() || !validated.manifest) {
    return failure(validated.error, validated.message);
  }

  PlayerProfile profile{.schemaVersion = kPlayerProfileSchemaVersion,
                        .id = validated.manifest->profileUuid,
                        .displayName = validated.manifest->profileDisplayName,
                        .createdAt = validated.manifest->createdAt,
                        .lastUsedAt = validated.manifest->createdAt};
  const auto loadedSettings =
      AppSettingsStore::Load(extracted / "settings.json");
  const auto loadedInput = InputProfileStore::load(extracted / "input.json");
  const std::optional<std::string> overwrite =
      options.mode == ProfileImportMode::Overwrite ? options.overwriteProfileId
                                                   : std::nullopt;
  const ProfileResult installed = manager_.installProfile(
      std::move(profile), overwrite,
      [&](const PlayerProfilePaths &staging, std::string &errorMessage) {
        std::error_code practiceError;
        std::filesystem::create_directory(staging.practiceDirectory,
                                          practiceError);
        if (practiceError) {
          errorMessage = "unable to create imported practice directory: " +
                         practiceError.message();
          return false;
        }
        const auto extractedPractice = extracted / "practice";
        if (validated.manifest->formatVersion >= 2 &&
            std::filesystem::exists(extractedPractice, practiceError)) {
          std::filesystem::directory_iterator iterator(extractedPractice,
                                                       practiceError);
          if (practiceError) {
            errorMessage = "unable to enumerate imported practice data: " +
                           practiceError.message();
            return false;
          }
          for (const auto &entry : iterator) {
            const std::string memberName =
                "practice/" + entry.path().filename().string();
            if (!isPracticeMember(memberName) ||
                !copyFileStreaming(entry.path(),
                                   staging.practiceDirectory /
                                       entry.path().filename(),
                                   memberName, errorMessage)) {
              return false;
            }
          }
        } else if (practiceError) {
          errorMessage = "unable to inspect imported practice data: " +
                         practiceError.message();
          return false;
        }
        const bool settingsWritten =
            validated.manifest->settingsSchemaVersion ==
                    AppSettingsStore::kCurrentSchemaVersion
                ? copyFileStreaming(extracted / "settings.json",
                                    staging.settingsJson, "settings.json",
                                    errorMessage)
                : AppSettingsStore::Save(staging.settingsJson,
                                         loadedSettings.settings, errorMessage);
        const bool inputWritten =
            validated.manifest->inputSchemaVersion ==
                    InputProfile::kSchemaVersion
                ? copyFileStreaming(extracted / "input.json", staging.inputJson,
                                    "input.json", errorMessage)
                : InputProfileStore::saveAtomic(
                      staging.inputJson, loadedInput.profile, errorMessage);
        return settingsWritten && inputWritten &&
               snapshotSqliteDatabase(extracted / "scores.db", staging.scoresDb,
                                      errorMessage) &&
               snapshotSqliteDatabase(extracted / "replays.db",
                                      staging.replaysDb, errorMessage);
      });
  if (!installed.ok() || !installed.profile) {
    return {.error = installed.error,
            .message = installed.message,
            .profile = std::nullopt};
  }
  std::string cleanupWarning = installed.message;
  std::error_code workspaceCleanupError;
  std::filesystem::remove_all(workspace, workspaceCleanupError);
  cleanup.dismiss();
  if (workspaceCleanupError) {
    if (!cleanupWarning.empty()) {
      cleanupWarning += "; ";
    }
    cleanupWarning += "profile imported, but workspace cleanup is deferred: " +
                      workspaceCleanupError.message();
  }
  return success(*installed.profile, std::move(cleanupWarning));
}
