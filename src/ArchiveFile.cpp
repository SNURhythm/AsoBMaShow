#include "ArchiveFile.h"

#include <SDL2/SDL.h>
#if __has_include(<TargetConditionals.h>)
#include <TargetConditionals.h>
#endif
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <clocale>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <utility>

#if __has_include(<archive.h>) && __has_include(<archive_entry.h>)
#include <archive.h>
#include <archive_entry.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE 0
#endif

#if __has_include(<unarr.h>) && \
    !(defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE)
#include <unarr.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_UNARR 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_UNARR 0
#endif

#if __has_include(<7zip/CPP/7zip/Archive/IArchive.h>) &&                  \
    __has_include(<7zip/CPP/7zip/IStream.h>) &&                           \
    __has_include(<7zip/CPP/Common/MyCom.h>)
#include <7zip/CPP/7zip/Archive/IArchive.h>
#include <7zip/CPP/7zip/IStream.h>
#include <7zip/CPP/Common/MyCom.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP 0
#endif

#if __has_include(<iconv.h>)
#include <iconv.h>
#define ASOBMSHOW_ARCHIVEFILE_HAS_ICONV 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_ICONV 0
#endif

#if __has_include("../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h")
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ARCHIVE_WRITING_APIS
#include "../bgfx/bimg/3rdparty/tinyexr/deps/miniz/miniz.h"
#define ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ 1
#else
#define ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ 0
#endif

namespace archive_file {
namespace {

std::string lowerCopy(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

std::string replaceAll(std::string value, std::string_view needle,
                       std::string_view replacement) {
  if (needle.empty()) {
    return value;
  }
  size_t pos = 0;
  while ((pos = value.find(needle, pos)) != std::string::npos) {
    value.replace(pos, needle.size(), replacement);
    pos += replacement.size();
  }
  return value;
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

std::string archiveExtensionFromPath(const std::filesystem::path &path) {
  std::string name = lowerCopy(path.filename().string());
  static constexpr std::array<std::string_view, 22> kArchiveExtensions = {
      ".tar.bz2", ".tar.gz", ".tar.xz", ".tar.zst", ".tbz2", ".tgz",
      ".txz",     ".tzst",   ".zip",    ".zipx",    ".cbz",  ".7z",
      ".cb7",     ".rar",    ".cbr",    ".lzh",     ".lha",  ".tar",
      ".bz2",     ".gz",     ".xz",     ".zst",
  };
  for (std::string_view extension : kArchiveExtensions) {
    if (endsWith(name, extension)) {
      return std::string(extension);
    }
  }
  return "";
}

bool hasZipArchiveExtension(const std::filesystem::path &path) {
  const std::string extension = archiveExtensionFromPath(path);
  return extension == ".zip" || extension == ".cbz";
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
bool hasRarArchiveExtension(const std::filesystem::path &path) {
  const std::string extension = archiveExtensionFromPath(path);
  return extension == ".rar" || extension == ".cbr";
}
#endif

bool hasSevenZipArchiveExtension(const std::filesystem::path &path) {
  const std::string extension = archiveExtensionFromPath(path);
  return extension == ".7z" || extension == ".cb7" || extension == ".rar" ||
         extension == ".cbr" || extension == ".lzh" || extension == ".lha" ||
         extension == ".zipx";
}

enum class ArchiveIndexBackend {
  Unknown,
  MinizZip,
  UnarrRar,
  SevenZip,
  LibArchive,
};

enum class RarSignature {
  Unknown,
  Rar4,
  Rar5,
};

RarSignature rarSignature(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return RarSignature::Unknown;
  }

  std::array<unsigned char, 8> marker{};
  file.read(reinterpret_cast<char *>(marker.data()),
            static_cast<std::streamsize>(marker.size()));
  const std::streamsize readSize = file.gcount();
  if (readSize >= 7 && marker[0] == 'R' && marker[1] == 'a' &&
      marker[2] == 'r' && marker[3] == '!' && marker[4] == 0x1a &&
      marker[5] == 0x07 && marker[6] == 0x00) {
    return RarSignature::Rar4;
  }
  if (readSize >= 8 && marker[0] == 'R' && marker[1] == 'a' &&
      marker[2] == 'r' && marker[3] == '!' && marker[4] == 0x1a &&
      marker[5] == 0x07 && marker[6] == 0x01 && marker[7] == 0x00) {
    return RarSignature::Rar5;
  }
  return RarSignature::Unknown;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
enum class SevenZipFormat : unsigned char {
  Zip = 0x01,
  Rar = 0x03,
  Lzh = 0x06,
  SevenZip = 0x07,
  Rar5 = 0xcc,
};

GUID sevenZipFormatGuid(SevenZipFormat format) {
  return {0x23170F69,
          0x40C1,
          0x278A,
          {0x10, 0x00, 0x00, 0x01, 0x10,
           static_cast<unsigned char>(format), 0x00, 0x00}};
}

std::vector<SevenZipFormat>
sevenZipFormatCandidates(const std::filesystem::path &path) {
  const std::string extension = archiveExtensionFromPath(path);
  if (extension == ".7z" || extension == ".cb7") {
    return {SevenZipFormat::SevenZip};
  }
  if (extension == ".rar" || extension == ".cbr") {
    // 7-Zip RAR support is decompression-only code under LGPL plus the
    // unRAR restriction; do not use it for RAR-compatible archive creation.
    switch (rarSignature(path)) {
    case RarSignature::Rar4:
      return {SevenZipFormat::Rar};
    case RarSignature::Rar5:
      return {SevenZipFormat::Rar5};
    case RarSignature::Unknown:
      return {SevenZipFormat::Rar, SevenZipFormat::Rar5};
    }
  }
  if (extension == ".lzh" || extension == ".lha") {
    return {SevenZipFormat::Lzh};
  }
  if (extension == ".zipx") {
    return {SevenZipFormat::Zip};
  }
  return {};
}
#endif

std::string normalizeEntryName(std::string value) {
  value = replaceAll(std::move(value), "\\", "/");
  std::filesystem::path path(value);
  path = path.lexically_normal();
  std::string normalized = path.generic_string();
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  if (normalized == ".") {
    normalized.clear();
  }
  return normalized;
}

bool safeEntryPath(const std::string &name, std::filesystem::path &outPath) {
  if (name.empty() || name.find('\0') != std::string::npos) {
    return false;
  }
  const std::string normalized = normalizeEntryName(name);
  std::filesystem::path relative(normalized);
  if (relative.empty() || relative.is_absolute() || relative.has_root_path()) {
    return false;
  }
  for (const auto &part : relative) {
    if (part == ".." || part == ".") {
      return false;
    }
  }
  outPath = relative;
  return true;
}

bool pathIsInsideFolder(const std::filesystem::path &path,
                        const std::filesystem::path &folderPath) {
  const std::string normalized = normalizeEntryName(path.generic_string());
  const std::string folder = normalizeEntryName(folderPath.generic_string());
  if (folder.empty()) {
    return !normalized.empty();
  }
  return normalized == folder ||
         (normalized.size() > folder.size() &&
          normalized.compare(0, folder.size(), folder) == 0 &&
          normalized[folder.size()] == '/');
}

std::string archiveKey(const std::filesystem::path &path) {
  return path_t_to_utf8(fspath_to_path_t(path.lexically_normal()));
}

bool fileState(const std::filesystem::path &path, std::uintmax_t &size,
               std::filesystem::file_time_type &mtime) {
  std::error_code error;
  size = std::filesystem::file_size(path, error);
  if (error) {
    return false;
  }
  mtime = std::filesystem::last_write_time(path, error);
  return !error;
}

std::string fileStateKey(const std::filesystem::path &path) {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(path, size, mtime)) {
    return "missing";
  }

  std::ostringstream out;
  const auto mtimeNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              mtime.time_since_epoch())
                              .count();
  out << size << ':' << mtimeNanos;
  return out.str();
}

struct CachedIndex {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  ArchiveIndexBackend backend = ArchiveIndexBackend::Unknown;
  unsigned char sevenZipFormat = 0;
  std::vector<Entry> entries;
  std::unordered_map<std::string, std::size_t> exact;
  std::unordered_map<std::string, std::size_t> lower;
};

std::mutex gIndexMutex;
std::unordered_map<std::string, std::shared_ptr<const CachedIndex>> gIndexCache;

constexpr std::size_t kDebugLogMaxLines = 500;
std::mutex gDebugLogMutex;
std::deque<std::string> gDebugLogLines;
std::uint64_t gDebugLogRevision = 0;

std::string pathForLog(const std::filesystem::path &path) {
  return path_t_to_utf8(fspath_to_path_t(path));
}

std::string byteCountForLog(std::uintmax_t bytes) {
  constexpr std::uintmax_t kib = 1024;
  constexpr std::uintmax_t mib = kib * 1024;
  constexpr std::uintmax_t gib = mib * 1024;
  if (bytes >= gib) {
    return std::to_string(bytes / gib) + " GiB";
  }
  if (bytes >= mib) {
    return std::to_string(bytes / mib) + " MiB";
  }
  if (bytes >= kib) {
    return std::to_string(bytes / kib) + " KiB";
  }
  return std::to_string(bytes) + " B";
}

void appendDebugLogLineImpl(std::string message) {
  if (message.empty()) {
    return;
  }
  if (message.size() > 6000) {
    message.resize(6000);
    message += "...";
  }

  std::ostringstream line;
  line << '[' << SDL_GetTicks64() << "ms] " << message;

  std::lock_guard<std::mutex> lock(gDebugLogMutex);
  gDebugLogLines.push_back(line.str());
  while (gDebugLogLines.size() > kDebugLogMaxLines) {
    gDebugLogLines.pop_front();
  }
  ++gDebugLogRevision;
}

std::string backendName(ArchiveIndexBackend backend) {
  switch (backend) {
  case ArchiveIndexBackend::MinizZip:
    return "miniz ZIP";
  case ArchiveIndexBackend::UnarrRar:
    return "unarr RAR";
  case ArchiveIndexBackend::SevenZip:
    return "7-Zip SDK";
  case ArchiveIndexBackend::LibArchive:
    return "libarchive";
  case ArchiveIndexBackend::Unknown:
  default:
    return "unknown";
  }
}

bool readRegularFile(const std::filesystem::path &path,
                     std::vector<unsigned char> &bytes,
                     std::string *errorMessage) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open file: " + path.string();
    }
    return false;
  }
  file.seekg(0, std::ios::end);
  const auto size = file.tellg();
  if (size < 0) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read file size: " + path.string();
    }
    return false;
  }
  file.seekg(0, std::ios::beg);
  bytes.resize(static_cast<size_t>(size));
  if (!bytes.empty()) {
    file.read(reinterpret_cast<char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!file) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not read file: " + path.string();
      }
      return false;
    }
  }
  return true;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
struct UnarrStreamHandle {
  ~UnarrStreamHandle() {
    if (stream != nullptr) {
      ar_close(stream);
    }
  }

  ar_stream *stream = nullptr;
};

struct UnarrArchiveHandle {
  ~UnarrArchiveHandle() {
    if (archive != nullptr) {
      ar_close_archive(archive);
    }
  }

  ar_archive *archive = nullptr;
};

constexpr unsigned char kRarMainHeader = 0x73;
constexpr unsigned char kRarFileHeader = 0x74;
constexpr unsigned short kRarMainSolidFlag = 1u << 3;
constexpr unsigned short kRarFileSolidFlag = 1u << 4;

unsigned short readUInt16Le(const unsigned char *data) {
  return static_cast<unsigned short>(data[0] |
                                     (static_cast<unsigned short>(data[1]) << 8));
}

bool readRar4BaseHeader(std::ifstream &file, std::int64_t offset,
                        unsigned char &type, unsigned short &flags,
                        unsigned short &size) {
  if (offset < 0) {
    return false;
  }
  unsigned char header[7] = {};
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file.read(reinterpret_cast<char *>(header), sizeof(header))) {
    return false;
  }
  type = header[2];
  flags = readUInt16Le(header + 3);
  size = readUInt16Le(header + 5);
  return size >= sizeof(header);
}

bool readRar4MainSolidFlag(std::ifstream &file, bool &mainSolid) {
  unsigned char type = 0;
  unsigned short flags = 0;
  unsigned short size = 0;
  if (!readRar4BaseHeader(file, 7, type, flags, size) ||
      type != kRarMainHeader) {
    return false;
  }
  mainSolid = (flags & kRarMainSolidFlag) != 0;
  return true;
}

bool readRar4EntrySolidFlagAtOffset(std::ifstream &file, std::int64_t offset,
                                    bool mainSolid, bool &solid) {
  unsigned char type = 0;
  unsigned short flags = 0;
  unsigned short size = 0;
  if (!readRar4BaseHeader(file, offset, type, flags, size) ||
      type != kRarFileHeader) {
    return false;
  }

  unsigned char compressedSize[4] = {};
  unsigned char fileData[21] = {};
  if (!file.read(reinterpret_cast<char *>(compressedSize),
                 sizeof(compressedSize)) ||
      !file.read(reinterpret_cast<char *>(fileData), sizeof(fileData))) {
    return false;
  }

  const unsigned char unpackVersion = fileData[13];
  solid = unpackVersion < 20 ? mainSolid : (flags & kRarFileSolidFlag) != 0;
  return true;
}

bool openUnarrRarArchive(const std::filesystem::path &archivePath,
                         UnarrStreamHandle &stream,
                         UnarrArchiveHandle &archive,
                         std::string *errorMessage) {
  const std::string archiveText = path_t_to_utf8(fspath_to_path_t(archivePath));
  stream.stream = ar_open_file(archiveText.c_str());
  if (stream.stream == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr could not open archive file.";
    }
    return false;
  }

  archive.archive = ar_open_rar_archive(stream.stream);
  if (archive.archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr could not open RAR4 archive.";
    }
    return false;
  }
  return true;
}

bool listUnarrRarEntries(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         bool &containsSolidEntries,
                         std::string *errorMessage) {
  entries.clear();
  containsSolidEntries = false;

  UnarrStreamHandle stream;
  UnarrArchiveHandle archive;
  if (!openUnarrRarArchive(archivePath, stream, archive, errorMessage)) {
    return false;
  }

  std::ifstream headerFile(archivePath, std::ios::binary);
  bool mainSolid = false;
  if (!headerFile || !readRar4MainSolidFlag(headerFile, mainSolid)) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr RAR header inspection failed.";
    }
    return false;
  }

  std::size_t order = 0;
  while (ar_parse_entry(archive.archive)) {
    const char *entryName = ar_entry_get_name(archive.archive);
    if (entryName == nullptr || entryName[0] == '\0') {
      continue;
    }

    std::filesystem::path relativePath;
    if (!safeEntryPath(entryName, relativePath)) {
      continue;
    }

    const std::int64_t offset =
        static_cast<std::int64_t>(ar_entry_get_offset(archive.archive));
    bool solid = false;
    if (!readRar4EntrySolidFlagAtOffset(headerFile, offset, mainSolid, solid)) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr RAR entry header inspection failed.";
      }
      entries.clear();
      return false;
    }
    containsSolidEntries = containsSolidEntries || solid;

    entries.push_back({
        .path = relativePath,
        .directory = false,
        .size = static_cast<std::uint64_t>(ar_entry_get_size(archive.archive)),
        .order = order++,
        .offset = offset,
        .solid = solid,
    });
  }

  if (!ar_at_eof(archive.archive)) {
    if (errorMessage != nullptr) {
      *errorMessage = "unarr RAR index did not reach archive end.";
    }
    entries.clear();
    return false;
  }

  appendDebugLogLineImpl("Indexed RAR with unarr random offsets: " +
                         pathForLog(archivePath) +
                         " entries=" + std::to_string(entries.size()) +
                         " solid=" +
                         (containsSolidEntries ? "yes" : "no"));
  return true;
}
#endif

void appendUtf8CodePoint(std::string &output, std::uint32_t codePoint) {
  if (codePoint <= 0x7f) {
    output.push_back(static_cast<char>(codePoint));
    return;
  }
  if (codePoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    return;
  }
  if (codePoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    return;
  }
  if (codePoint <= 0x10ffff) {
    output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
    return;
  }
  appendUtf8CodePoint(output, 0xfffd);
}

std::string wideStringToUtf8(const wchar_t *input, std::size_t length) {
  std::string output;
  if (input == nullptr || length == 0) {
    return output;
  }
  output.reserve(length);
  for (std::size_t i = 0; i < length; ++i) {
    std::uint32_t codePoint = static_cast<std::uint32_t>(input[i]);
    if constexpr (sizeof(wchar_t) == 2) {
      if (codePoint >= 0xd800 && codePoint <= 0xdbff && i + 1 < length) {
        const std::uint32_t low = static_cast<std::uint32_t>(input[i + 1]);
        if (low >= 0xdc00 && low <= 0xdfff) {
          codePoint =
              0x10000 + (((codePoint - 0xd800) << 10) | (low - 0xdc00));
          ++i;
        } else {
          codePoint = 0xfffd;
        }
      } else if (codePoint >= 0xdc00 && codePoint <= 0xdfff) {
        codePoint = 0xfffd;
      }
    }
    appendUtf8CodePoint(output, codePoint);
  }
  return output;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP

extern "C" HRESULT WINAPI CreateObject(const GUID *clsID,
                                        const GUID *interfaceID, void **out);

std::string sevenZipResultMessage(HRESULT result) {
  return "7-Zip SDK error: " + std::to_string(static_cast<long long>(result));
}

struct SevenZipPropVariant : PROPVARIANT {
  SevenZipPropVariant() { std::memset(this, 0, sizeof(PROPVARIANT)); }
  ~SevenZipPropVariant() { VariantClear(this); }
};

std::optional<std::string> sevenZipStringProperty(IInArchive *archive,
                                                  UInt32 index,
                                                  PROPID property) {
  if (archive == nullptr) {
    return std::nullopt;
  }
  SevenZipPropVariant value;
  if (archive->GetProperty(index, property, &value) != S_OK ||
      value.vt != VT_BSTR || value.bstrVal == nullptr) {
    return std::nullopt;
  }
  return wideStringToUtf8(value.bstrVal, SysStringLen(value.bstrVal));
}

bool sevenZipBoolProperty(IInArchive *archive, UInt32 index, PROPID property,
                          bool defaultValue = false) {
  SevenZipPropVariant value;
  if (archive == nullptr ||
      archive->GetProperty(index, property, &value) != S_OK) {
    return defaultValue;
  }
  if (value.vt == VT_BOOL) {
    return value.boolVal != VARIANT_FALSE;
  }
  if (value.vt == VT_UI4) {
    return value.ulVal != 0;
  }
  if (value.vt == VT_UI8) {
    return value.uhVal.QuadPart != 0;
  }
  return defaultValue;
}

std::uint64_t sevenZipUInt64Property(IInArchive *archive, UInt32 index,
                                     PROPID property,
                                     std::uint64_t defaultValue = 0) {
  SevenZipPropVariant value;
  if (archive == nullptr ||
      archive->GetProperty(index, property, &value) != S_OK) {
    return defaultValue;
  }
  if (value.vt == VT_UI8) {
    return static_cast<std::uint64_t>(value.uhVal.QuadPart);
  }
  if (value.vt == VT_I8) {
    return value.hVal.QuadPart > 0
               ? static_cast<std::uint64_t>(value.hVal.QuadPart)
               : defaultValue;
  }
  if (value.vt == VT_UI4) {
    return static_cast<std::uint64_t>(value.ulVal);
  }
  if (value.vt == VT_I4) {
    return value.lVal > 0 ? static_cast<std::uint64_t>(value.lVal)
                          : defaultValue;
  }
  return defaultValue;
}

class SevenZipInFileStream final : public IInStream, public IStreamGetSize {
public:
  SevenZipInFileStream(const std::filesystem::path &path, std::uintmax_t size)
      : size_(static_cast<UInt64>(size)) {
    file_.rdbuf()->pubsetbuf(buffer_.data(),
                             static_cast<std::streamsize>(buffer_.size()));
    file_.open(path, std::ios::binary);
  }

  bool isOpen() const { return file_.is_open(); }

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_IInStream) {
      *outObject = static_cast<IInStream *>(this);
    } else if (iid == IID_ISequentialInStream) {
      *outObject =
          static_cast<ISequentialInStream *>(static_cast<IInStream *>(this));
    } else if (iid == IID_IStreamGetSize) {
      *outObject = static_cast<IStreamGetSize *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(Read)(void *data, UInt32 size, UInt32 *processedSize) throw()
      override {
    if (processedSize != nullptr) {
      *processedSize = 0;
    }
    if (size == 0) {
      return S_OK;
    }
    if (data == nullptr || !file_) {
      return E_FAIL;
    }
    file_.read(reinterpret_cast<char *>(data),
               static_cast<std::streamsize>(size));
    const std::streamsize readSize = file_.gcount();
    if (processedSize != nullptr) {
      *processedSize = static_cast<UInt32>(std::max<std::streamsize>(
          0, std::min<std::streamsize>(readSize, size)));
    }
    if (readSize > 0 || file_.eof()) {
      return S_OK;
    }
    return file_.bad() ? E_FAIL : S_OK;
  }

  STDMETHOD(Seek)(Int64 offset, UInt32 seekOrigin,
                  UInt64 *newPosition) throw() override {
    if (!file_) {
      return E_FAIL;
    }

    std::ios_base::seekdir direction = std::ios::beg;
    if (seekOrigin == STREAM_SEEK_CUR) {
      direction = std::ios::cur;
    } else if (seekOrigin == STREAM_SEEK_END) {
      direction = std::ios::end;
    } else if (seekOrigin != STREAM_SEEK_SET) {
      return STG_E_INVALIDFUNCTION;
    }

    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset), direction);
    if (!file_) {
      return STG_E_INVALIDFUNCTION;
    }
    if (newPosition != nullptr) {
      const auto position = file_.tellg();
      if (position < 0) {
        return E_FAIL;
      }
      *newPosition = static_cast<UInt64>(position);
    }
    return S_OK;
  }

  STDMETHOD(GetSize)(UInt64 *size) throw() override {
    if (size == nullptr) {
      return E_FAIL;
    }
    *size = size_;
    return S_OK;
  }

private:
  static constexpr std::size_t kInputBufferSize = 1u << 20;
  std::array<char, kInputBufferSize> buffer_{};
  std::ifstream file_;
  UInt64 size_ = 0;
  ULONG refCount_ = 0;
};

class SevenZipMemoryOutStream final : public ISequentialOutStream {
public:
  explicit SevenZipMemoryOutStream(std::vector<unsigned char> &bytes)
      : bytes_(bytes) {}

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) {
      *outObject = static_cast<ISequentialOutStream *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(Write)(const void *data, UInt32 size,
                   UInt32 *processedSize) throw() override {
    if (processedSize != nullptr) {
      *processedSize = 0;
    }
    if (size == 0) {
      return S_OK;
    }
    if (data == nullptr) {
      return E_FAIL;
    }
    const auto *bytes = static_cast<const unsigned char *>(data);
    bytes_.insert(bytes_.end(), bytes, bytes + size);
    if (processedSize != nullptr) {
      *processedSize = size;
    }
    return S_OK;
  }

private:
  std::vector<unsigned char> &bytes_;
  ULONG refCount_ = 0;
};

class SevenZipExtractCallback final : public IArchiveExtractCallback {
public:
  explicit SevenZipExtractCallback(
      std::unordered_map<UInt32, FileData *> targets)
      : targets_(std::move(targets)) {}

  STDMETHOD(QueryInterface)(REFIID iid, void **outObject) throw() override {
    if (outObject == nullptr) {
      return E_FAIL;
    }
    *outObject = nullptr;
    if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback) {
      *outObject = static_cast<IArchiveExtractCallback *>(this);
    } else if (iid == IID_IProgress) {
      *outObject = static_cast<IProgress *>(this);
    } else {
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  STDMETHOD_(ULONG, AddRef)() throw() override { return ++refCount_; }

  STDMETHOD_(ULONG, Release)() throw() override {
    const ULONG refCount = --refCount_;
    if (refCount == 0) {
      delete this;
    }
    return refCount;
  }

  STDMETHOD(SetTotal)(UInt64) throw() override { return S_OK; }
  STDMETHOD(SetCompleted)(const UInt64 *) throw() override { return S_OK; }

  STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream **outStream,
                       Int32 askExtractMode) throw() override {
    if (outStream == nullptr) {
      return E_FAIL;
    }
    *outStream = nullptr;
    currentTarget_ = nullptr;
    if (askExtractMode != NArchive::NExtract::NAskMode::kExtract) {
      return S_OK;
    }
    const auto it = targets_.find(index);
    if (it == targets_.end() || it->second == nullptr) {
      return S_OK;
    }

    currentTarget_ = it->second;
    currentTarget_->bytes.clear();
    auto *stream = new SevenZipMemoryOutStream(currentTarget_->bytes);
    ISequentialOutStream *streamInterface = stream;
    streamInterface->AddRef();
    *outStream = streamInterface;
    return S_OK;
  }

  STDMETHOD(PrepareOperation)(Int32) throw() override { return S_OK; }

  STDMETHOD(SetOperationResult)(Int32 opRes) throw() override {
    if (currentTarget_ != nullptr &&
        opRes != NArchive::NExtract::NOperationResult::kOK) {
      failed_ = true;
      operationResult_ = opRes;
    }
    currentTarget_ = nullptr;
    return S_OK;
  }

  bool failed() const { return failed_; }
  Int32 operationResult() const { return operationResult_; }

private:
  std::unordered_map<UInt32, FileData *> targets_;
  FileData *currentTarget_ = nullptr;
  ULONG refCount_ = 0;
  bool failed_ = false;
  Int32 operationResult_ = NArchive::NExtract::NOperationResult::kOK;
};

struct SevenZipArchiveState {
  ~SevenZipArchiveState() {
    std::lock_guard<std::mutex> lock(mutex);
    if (archive.Interface() != nullptr) {
      archive->Close();
    }
  }

  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  unsigned char formatId = 0;
  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> stream;
  std::mutex mutex;
  std::uint64_t lastUse = 0;
};

constexpr std::size_t kMaxOpenSevenZipArchives = 4;
std::mutex gSevenZipArchiveMutex;
std::unordered_map<std::string, std::shared_ptr<SevenZipArchiveState>>
    gSevenZipArchiveCache;
std::uint64_t gSevenZipArchiveUseCounter = 0;

bool openSevenZipArchive(const std::filesystem::path &archivePath,
                         CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream,
                         SevenZipFormat &formatUsed,
                         std::string *errorMessage);
bool openSevenZipArchive(const std::filesystem::path &archivePath,
                         unsigned char formatId,
                         CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream,
                         std::string *errorMessage);

bool openSevenZipArchiveWithFormat(const std::filesystem::path &archivePath,
                                   SevenZipFormat format,
                                   CMyComPtr<IInArchive> &archive,
                                   CMyComPtr<IInStream> &stream,
                                   std::string *errorMessage) {
  archive.Release();
  stream.Release();

  IInArchive *rawArchive = nullptr;
  const GUID formatId = sevenZipFormatGuid(format);
  HRESULT result =
      CreateObject(&formatId, &IID_IInArchive,
                   reinterpret_cast<void **>(&rawArchive));
  if (result != S_OK || rawArchive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }
  CMyComPtr<IInArchive> archiveHandle;
  archiveHandle.Attach(rawArchive);

  std::uintmax_t archiveSize = 0;
  std::filesystem::file_time_type ignoredMtime{};
  if (!fileState(archivePath, archiveSize, ignoredMtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + archivePath.string();
    }
    return false;
  }

  auto *fileStream = new SevenZipInFileStream(archivePath, archiveSize);
  if (!fileStream->isOpen()) {
    delete fileStream;
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open archive file: " + archivePath.string();
    }
    return false;
  }

  IInStream *streamInterface = fileStream;
  streamInterface->AddRef();
  CMyComPtr<IInStream> streamHandle;
  streamHandle.Attach(streamInterface);

  UInt64 maxCheckStartPosition = 0;
  result = archiveHandle->Open(streamHandle, &maxCheckStartPosition, nullptr);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  archive = archiveHandle;
  stream = streamHandle;
  return true;
}

void trimSevenZipArchiveCacheLocked() {
  while (gSevenZipArchiveCache.size() > kMaxOpenSevenZipArchives) {
    auto evictIt = gSevenZipArchiveCache.end();
    for (auto it = gSevenZipArchiveCache.begin();
         it != gSevenZipArchiveCache.end(); ++it) {
      if (it->second == nullptr || it->second.use_count() > 1) {
        continue;
      }
      if (evictIt == gSevenZipArchiveCache.end() ||
          it->second->lastUse < evictIt->second->lastUse) {
        evictIt = it;
      }
    }
    if (evictIt == gSevenZipArchiveCache.end()) {
      return;
    }
    appendDebugLogLineImpl("Closing cached 7-Zip archive: " + evictIt->first);
    gSevenZipArchiveCache.erase(evictIt);
  }
}

std::shared_ptr<SevenZipArchiveState> openCachedSevenZipArchive(
    const std::filesystem::path &archivePath, unsigned char requestedFormatId,
    bool *cacheHit, long long *openMs, std::string *errorMessage) {
  if (cacheHit != nullptr) {
    *cacheHit = false;
  }
  if (openMs != nullptr) {
    *openMs = 0;
  }

  std::uintmax_t archiveSize = 0;
  std::filesystem::file_time_type archiveMtime{};
  if (!fileState(archivePath, archiveSize, archiveMtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + archivePath.string();
    }
    return nullptr;
  }

  const std::string key = archiveKey(archivePath);
  std::lock_guard<std::mutex> cacheLock(gSevenZipArchiveMutex);
  const auto cachedIt = gSevenZipArchiveCache.find(key);
  if (cachedIt != gSevenZipArchiveCache.end()) {
    const auto &cached = cachedIt->second;
    if (cached != nullptr && cached->size == archiveSize &&
        cached->mtime == archiveMtime &&
        (requestedFormatId == 0 || cached->formatId == requestedFormatId)) {
      cached->lastUse = ++gSevenZipArchiveUseCounter;
      if (cacheHit != nullptr) {
        *cacheHit = true;
      }
      return cached;
    }
    gSevenZipArchiveCache.erase(cachedIt);
    appendDebugLogLineImpl("Invalidated cached 7-Zip archive: " + key);
  }

  using Clock = std::chrono::steady_clock;
  const auto start = Clock::now();
  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> stream;
  SevenZipFormat formatUsed = SevenZipFormat::SevenZip;
  const bool opened =
      requestedFormatId == 0
          ? openSevenZipArchive(archivePath, archive, stream, formatUsed,
                                errorMessage)
          : openSevenZipArchive(archivePath, requestedFormatId, archive, stream,
                                errorMessage);
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            start)
          .count();
  if (openMs != nullptr) {
    *openMs = elapsed;
  }
  if (!opened) {
    return nullptr;
  }
  if (requestedFormatId != 0) {
    formatUsed = static_cast<SevenZipFormat>(requestedFormatId);
  }

  auto state = std::make_shared<SevenZipArchiveState>();
  state->size = archiveSize;
  state->mtime = archiveMtime;
  state->formatId = static_cast<unsigned char>(formatUsed);
  state->archive = archive;
  state->stream = stream;
  state->lastUse = ++gSevenZipArchiveUseCounter;
  gSevenZipArchiveCache[key] = state;
  appendDebugLogLineImpl("Opened cached 7-Zip archive: " + key +
                         " openMs=" + std::to_string(elapsed));
  trimSevenZipArchiveCacheLocked();
  return state;
}

bool openSevenZipArchive(const std::filesystem::path &archivePath,
                         CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream,
                         SevenZipFormat &formatUsed,
                         std::string *errorMessage) {
  const auto candidates = sevenZipFormatCandidates(archivePath);
  if (candidates.empty()) {
    return false;
  }

  std::string lastError;
  for (SevenZipFormat format : candidates) {
    std::string currentError;
    if (openSevenZipArchiveWithFormat(archivePath, format, archive, stream,
                                      &currentError)) {
      formatUsed = format;
      return true;
    }
    lastError = std::move(currentError);
  }

  if (errorMessage != nullptr) {
    *errorMessage = lastError.empty() ? "Could not open archive with 7-Zip SDK."
                                      : lastError;
  }
  return false;
}

bool openSevenZipArchive(const std::filesystem::path &archivePath,
                         unsigned char formatId,
                         CMyComPtr<IInArchive> &archive,
                         CMyComPtr<IInStream> &stream,
                         std::string *errorMessage) {
  return openSevenZipArchiveWithFormat(
      archivePath, static_cast<SevenZipFormat>(formatId), archive, stream,
      errorMessage);
}

bool listSevenZipEntries(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         unsigned char &formatUsed,
                         std::string *errorMessage) {
  entries.clear();

  long long openMs = 0;
  const auto archiveState = openCachedSevenZipArchive(
      archivePath, 0, nullptr, &openMs, errorMessage);
  if (archiveState == nullptr) {
    return false;
  }
  formatUsed = archiveState->formatId;

  std::lock_guard<std::mutex> archiveLock(archiveState->mutex);
  IInArchive *archive = archiveState->archive.Interface();
  UInt32 itemCount = 0;
  HRESULT result = archive->GetNumberOfItems(&itemCount);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  entries.reserve(itemCount);
  std::size_t skippedUnnamed = 0;
  std::size_t skippedUnsafe = 0;
  std::size_t skippedEncrypted = 0;
  std::size_t solidEntries = 0;
  for (UInt32 index = 0; index < itemCount; ++index) {
    auto entryName = sevenZipStringProperty(archive, index, kpidPath);
    if (!entryName.has_value() || entryName->empty()) {
      entryName = sevenZipStringProperty(archive, index, kpidName);
    }
    if (!entryName.has_value() || entryName->empty()) {
      ++skippedUnnamed;
      continue;
    }

    std::filesystem::path relativePath;
    if (!safeEntryPath(*entryName, relativePath)) {
      ++skippedUnsafe;
      continue;
    }

    const bool directory =
        sevenZipBoolProperty(archive, index, kpidIsDir, false);
    const bool solid = !directory &&
                       sevenZipBoolProperty(archive, index, kpidSolid, false);
    if (!directory &&
        sevenZipBoolProperty(archive, index, kpidEncrypted, false)) {
      ++skippedEncrypted;
      continue;
    }
    if (solid) {
      ++solidEntries;
    }

    entries.push_back({
        .path = relativePath,
        .directory = directory,
        .size = sevenZipUInt64Property(archive, index, kpidSize),
        .order = static_cast<std::size_t>(index),
        .offset = -1,
        .solid = solid,
    });
  }

  appendDebugLogLineImpl("Indexed 7-Zip archive from cached open handle: " +
                         pathForLog(archivePath) +
                         " entries=" + std::to_string(entries.size()) +
                         " solidEntries=" + std::to_string(solidEntries) +
                         " formatId=" + std::to_string(formatUsed) +
                         " openMs=" + std::to_string(openMs));
  if (skippedUnnamed > 0 || skippedUnsafe > 0 || skippedEncrypted > 0) {
    appendDebugLogLineImpl(
        "7-Zip skipped entries while indexing " + pathForLog(archivePath) +
        ": unnamed=" + std::to_string(skippedUnnamed) +
        " unsafe=" + std::to_string(skippedUnsafe) +
        " encrypted=" + std::to_string(skippedEncrypted));
  }
  return true;
}

#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE

bool isValidUtf8(const std::string &value) {
  const auto *bytes = reinterpret_cast<const unsigned char *>(value.data());
  size_t i = 0;
  while (i < value.size()) {
    const unsigned char c = bytes[i];
    if (c <= 0x7f) {
      ++i;
      continue;
    }
    if (c >= 0xc2 && c <= 0xdf) {
      if (i + 1 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80) {
        return false;
      }
      i += 2;
      continue;
    }
    if (c == 0xe0) {
      if (i + 2 >= value.size() || bytes[i + 1] < 0xa0 ||
          bytes[i + 1] > 0xbf || (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if ((c >= 0xe1 && c <= 0xec) || c == 0xee || c == 0xef) {
      if (i + 2 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80 ||
          (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if (c == 0xed) {
      if (i + 2 >= value.size() || bytes[i + 1] < 0x80 ||
          bytes[i + 1] > 0x9f || (bytes[i + 2] & 0xc0) != 0x80) {
        return false;
      }
      i += 3;
      continue;
    }
    if (c == 0xf0) {
      if (i + 3 >= value.size() || bytes[i + 1] < 0x90 ||
          bytes[i + 1] > 0xbf || (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    if (c >= 0xf1 && c <= 0xf3) {
      if (i + 3 >= value.size() || (bytes[i + 1] & 0xc0) != 0x80 ||
          (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    if (c == 0xf4) {
      if (i + 3 >= value.size() || bytes[i + 1] < 0x80 ||
          bytes[i + 1] > 0x8f || (bytes[i + 2] & 0xc0) != 0x80 ||
          (bytes[i + 3] & 0xc0) != 0x80) {
        return false;
      }
      i += 4;
      continue;
    }
    return false;
  }
  return true;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_ICONV
std::optional<std::string> convertTextToUtf8(const std::string &input,
                                             const char *fromEncoding) {
  iconv_t converter = iconv_open("UTF-8", fromEncoding);
  if (converter == reinterpret_cast<iconv_t>(-1)) {
    return std::nullopt;
  }

  std::string output(std::max<size_t>(input.size() * 4, 32), '\0');
  char *inputPtr = const_cast<char *>(input.data());
  size_t inputBytes = input.size();
  size_t outputOffset = 0;

  while (inputBytes > 0) {
    char *outputPtr = output.data() + outputOffset;
    size_t outputBytes = output.size() - outputOffset;
    const size_t status =
        iconv(converter, &inputPtr, &inputBytes, &outputPtr, &outputBytes);
    outputOffset = output.size() - outputBytes;
    if (status != static_cast<size_t>(-1)) {
      continue;
    }
    if (errno != E2BIG) {
      iconv_close(converter);
      return std::nullopt;
    }
    output.resize(output.size() * 2);
  }

  iconv_close(converter);
  output.resize(outputOffset);
  if (!isValidUtf8(output)) {
    return std::nullopt;
  }
  return output;
}
#endif

std::string wideToUtf8(const wchar_t *input) {
  if (input == nullptr) {
    return "";
  }
  return wideStringToUtf8(input, std::wcslen(input));
}

std::string entryPathnameUtf8(archive_entry *entry) {
  if (entry == nullptr) {
    return "";
  }
  if (const char *utf8Name = archive_entry_pathname_utf8(entry);
      utf8Name != nullptr && utf8Name[0] != '\0') {
    return utf8Name;
  }
  if (const wchar_t *wideName = archive_entry_pathname_w(entry);
      wideName != nullptr && wideName[0] != L'\0') {
    return wideToUtf8(wideName);
  }

  const char *rawName = archive_entry_pathname(entry);
  if (rawName == nullptr || rawName[0] == '\0') {
    return "";
  }

  const std::string raw(rawName);
  if (isValidUtf8(raw)) {
    return raw;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_ICONV
  for (const char *encoding : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    if (const auto converted = convertTextToUtf8(raw, encoding)) {
      return *converted;
    }
  }
#endif
  return raw;
}

struct ArchiveEntryInfo {
  std::filesystem::path relativePath;
  bool directory = false;
  bool regular = false;
  std::uint64_t size = 0;
};

bool archiveEntryInfo(archive_entry *entry, const std::string &entryName,
                      ArchiveEntryInfo &info) {
  if (entry == nullptr) {
    return false;
  }

  std::filesystem::path relativePath;
  if (!safeEntryPath(entryName, relativePath)) {
    return false;
  }

  const auto fileType = archive_entry_filetype(entry);
  const bool typeIsSet = archive_entry_filetype_is_set(entry) != 0;
  const bool nameLooksDirectory =
      !entryName.empty() &&
      (entryName.back() == '/' || entryName.back() == '\\');
  const bool directory =
      fileType == AE_IFDIR ||
      ((!typeIsSet || fileType == 0) && nameLooksDirectory);
  const bool regular =
      !directory && (!typeIsSet || fileType == 0 || fileType == AE_IFREG);
  if (!directory && !regular) {
    return false;
  }

  info = {
      .relativePath = relativePath,
      .directory = directory,
      .regular = regular,
      .size = archive_entry_size_is_set(entry)
                  ? static_cast<std::uint64_t>(archive_entry_size(entry))
                  : 0,
  };
  return true;
}

bool localeNameLooksArchiveCompatible(const char *name) {
  if (name == nullptr) {
    return false;
  }
  const std::string lower = lowerCopy(name);
  return lower.find("utf") != std::string::npos ||
         lower.find("sjis") != std::string::npos ||
         lower.find("shift") != std::string::npos ||
         lower.find("932") != std::string::npos;
}

void ensureArchiveFilenameLocale() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    if (localeNameLooksArchiveCompatible(std::setlocale(LC_CTYPE, nullptr))) {
      return;
    }
    for (const char *candidate :
         {"", "C.UTF-8", "en_US.UTF-8", "ja_JP.UTF-8", "ko_KR.UTF-8",
          "UTF-8", "ja_JP.SJIS", "ja_JP.Shift_JIS", "ja_JP.CP932",
          "Shift_JIS", "CP932", "SJIS"}) {
      const char *selected = std::setlocale(LC_CTYPE, candidate);
      if (localeNameLooksArchiveCompatible(selected)) {
        return;
      }
    }
  });
}

std::string archiveErrorString(archive *archiveHandle,
                               const std::string &fallback) {
  if (archiveHandle == nullptr ||
      archive_error_string(archiveHandle) == nullptr) {
    return fallback;
  }
  return archive_error_string(archiveHandle);
}

bool trySetArchiveHeaderCharset(archive *archiveHandle, const char *charset) {
  if (archiveHandle == nullptr || charset == nullptr || charset[0] == '\0') {
    return false;
  }

  bool applied = false;
  for (const char *format :
       {"zip", "7zip", "rar", "lha", "tar", "cab", "cpio"}) {
    const int status =
        archive_read_set_option(archiveHandle, format, "hdrcharset", charset);
    if (status == ARCHIVE_OK || status == ARCHIVE_WARN) {
      applied = true;
    }
  }
  return applied;
}

void configureArchiveReader(archive *archiveHandle) {
  archive_read_support_filter_all(archiveHandle);
  archive_read_support_format_all(archiveHandle);
  for (const char *charset : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
    if (trySetArchiveHeaderCharset(archiveHandle, charset)) {
      break;
    }
  }
}

archive *openArchive(const std::filesystem::path &archivePath,
                     std::string *errorMessage) {
  ensureArchiveFilenameLocale();
  archive *archiveHandle = archive_read_new();
  if (archiveHandle == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not initialize archive reader.";
    }
    return nullptr;
  }
  configureArchiveReader(archiveHandle);

  const std::string archiveText = path_t_to_utf8(fspath_to_path_t(archivePath));
  const int status =
      archive_read_open_filename(archiveHandle, archiveText.c_str(), 10240);
  if (status != ARCHIVE_OK) {
    if (errorMessage != nullptr) {
      *errorMessage =
          "Could not open archive: " + archiveErrorString(archiveHandle, "");
    }
    archive_read_free(archiveHandle);
    return nullptr;
  }
  return archiveHandle;
}

bool listEntriesUncached(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         std::string *errorMessage) {
  entries.clear();
  archive *archiveHandle = openArchive(archivePath, errorMessage);
  if (archiveHandle == nullptr) {
    return false;
  }

  archive_entry *entry = nullptr;
  for (;;) {
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Could not read archive: " + archiveErrorString(archiveHandle, "");
      }
      archive_read_free(archiveHandle);
      return false;
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    entries.push_back({
        .path = info.relativePath,
        .directory = info.directory,
        .size = info.size,
        .order = entries.size(),
    });
    archive_read_data_skip(archiveHandle);
  }

  archive_read_free(archiveHandle);
  return true;
}

bool readArchiveEntry(const std::filesystem::path &archivePath,
                      const std::filesystem::path &innerPath,
                      std::vector<unsigned char> &bytes,
                      std::string *errorMessage) {
  bytes.clear();
  const std::string target = normalizeEntryName(innerPath.generic_string());
  if (target.empty()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry path is empty.";
    }
    return false;
  }

  archive *archiveHandle = openArchive(archivePath, errorMessage);
  if (archiveHandle == nullptr) {
    return false;
  }

  archive_entry *entry = nullptr;
  for (;;) {
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Could not read archive: " + archiveErrorString(archiveHandle, "");
      }
      archive_read_free(archiveHandle);
      return false;
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    std::filesystem::path relativePath;
    if (!safeEntryPath(entryPathnameUtf8(entry), relativePath)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }
    if (normalizeEntryName(relativePath.generic_string()) != target) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0) {
      bytes.reserve(static_cast<size_t>(archive_entry_size(entry)));
    }
    std::array<unsigned char, 64 * 1024> buffer{};
    for (;;) {
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        archive_read_free(archiveHandle);
        return true;
      }
      if (count < 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Could not read archive entry: " +
                          archiveErrorString(archiveHandle, "");
        }
        archive_read_free(archiveHandle);
        return false;
      }
      bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + count);
    }
  }

  if (errorMessage != nullptr) {
    *errorMessage = "Archive entry not found: " + target;
  }
  archive_read_free(archiveHandle);
  return false;
}

bool readArchiveEntriesUncached(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  std::unordered_map<std::string, std::filesystem::path> targets;
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (!target.empty()) {
      targets.emplace(target, innerPath);
    }
  }
  if (targets.empty()) {
    return true;
  }

  archive *archiveHandle = openArchive(archivePath, errorMessage);
  if (archiveHandle == nullptr) {
    return false;
  }

  archive_entry *entry = nullptr;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::size_t entryOrder = 0;
  while (!targets.empty()) {
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "Could not read archive: " + archiveErrorString(archiveHandle, "");
      }
      archive_read_free(archiveHandle);
      return false;
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::size_t currentOrder = entryOrder++;
    if (range.has_value()) {
      if (currentOrder < range->start) {
        archive_read_data_skip(archiveHandle);
        continue;
      }
      if (currentOrder > range->end) {
        archive_read_data_skip(archiveHandle);
        break;
      }
    }

    if (!info.regular) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::string normalized =
        normalizeEntryName(info.relativePath.generic_string());
    const auto targetIt = targets.find(normalized);
    if (targetIt == targets.end()) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    FileData file;
    file.path = info.relativePath;
    if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0) {
      file.bytes.reserve(static_cast<size_t>(archive_entry_size(entry)));
    }
    for (;;) {
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        if (errorMessage != nullptr) {
          *errorMessage = "Could not read archive entry: " +
                          archiveErrorString(archiveHandle, "");
        }
        archive_read_free(archiveHandle);
        return false;
      }
      file.bytes.insert(file.bytes.end(), buffer.begin(),
                        buffer.begin() + count);
    }

    files.push_back(std::move(file));
    targets.erase(targetIt);
  }

  archive_read_free(archiveHandle);
  return true;
}

#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
bool listZipEntries(const std::filesystem::path &archivePath,
                    std::vector<Entry> &entries, std::string *errorMessage);
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
bool listUnarrRarEntries(const std::filesystem::path &archivePath,
                         std::vector<Entry> &entries,
                         bool &containsSolidEntries,
                         std::string *errorMessage);
#endif

void buildIndexLookups(CachedIndex &index) {
  index.exact.clear();
  index.lower.clear();
  index.exact.reserve(index.entries.size());
  index.lower.reserve(index.entries.size());
  for (std::size_t i = 0; i < index.entries.size(); ++i) {
    const Entry &entry = index.entries[i];
    if (entry.directory) {
      continue;
    }
    const std::string normalized =
        normalizeEntryName(entry.path.generic_string());
    if (normalized.empty()) {
      continue;
    }
    index.exact.emplace(normalized, i);
    index.lower.emplace(lowerCopy(normalized), i);
  }
}

std::shared_ptr<const CachedIndex>
cachedIndexForArchiveIfFresh(const std::filesystem::path &archivePath) {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    return nullptr;
  }

  const std::string key = archiveKey(archivePath);
  std::lock_guard<std::mutex> lock(gIndexMutex);
  const auto it = gIndexCache.find(key);
  if (it != gIndexCache.end() && it->second != nullptr &&
      it->second->size == size && it->second->mtime == mtime) {
    return it->second;
  }
  return nullptr;
}

std::shared_ptr<const CachedIndex>
cachedIndexForArchive(const std::filesystem::path &archivePath,
                      std::string *errorMessage) {
  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + archivePath.string();
    }
    return nullptr;
  }

  const std::string key = archiveKey(archivePath);
  bool hadCachedIndex = false;
  {
    std::lock_guard<std::mutex> lock(gIndexMutex);
    const auto it = gIndexCache.find(key);
    if (it != gIndexCache.end() && it->second != nullptr &&
        it->second->size == size && it->second->mtime == mtime) {
      return it->second;
    }
    hadCachedIndex = it != gIndexCache.end() && it->second != nullptr;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ || ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP || \
    ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  appendDebugLogLineImpl((hadCachedIndex ? "Archive changed; rebuilding index: "
                                         : "Indexing archive: ") +
                         pathForLog(archivePath) + " (" +
                         byteCountForLog(size) + ")");
  auto loaded = std::make_shared<CachedIndex>();
  loaded->size = size;
  loaded->mtime = mtime;
  bool loadedEntries = false;
#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (hasZipArchiveExtension(archivePath) &&
      listZipEntries(archivePath, loaded->entries, &zipError)) {
    loaded->backend = ArchiveIndexBackend::MinizZip;
    loadedEntries = true;
  } else if (hasZipArchiveExtension(archivePath) && !zipError.empty()) {
    appendDebugLogLineImpl("miniz ZIP index failed: " +
                           pathForLog(archivePath) + ": " + zipError);
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  std::string unarrError;
  if (!loadedEntries && hasRarArchiveExtension(archivePath)) {
    const RarSignature signature = rarSignature(archivePath);
    if (signature == RarSignature::Rar4) {
      std::vector<Entry> unarrEntries;
      bool containsSolidEntries = false;
      if (listUnarrRarEntries(archivePath, unarrEntries, containsSolidEntries,
                              &unarrError)) {
        if (!containsSolidEntries) {
          loaded->entries = std::move(unarrEntries);
          loaded->backend = ArchiveIndexBackend::UnarrRar;
          loadedEntries = true;
        } else {
          appendDebugLogLineImpl(
              "RAR archive is solid; random-access unarr backend disabled: " +
              pathForLog(archivePath));
        }
      } else if (!unarrError.empty()) {
        appendDebugLogLineImpl("unarr RAR4 index failed: " +
                               pathForLog(archivePath) + ": " + unarrError);
      }
    } else if (signature == RarSignature::Rar5) {
      appendDebugLogLineImpl("RAR5 archive detected; using 7-Zip backend: " +
                             pathForLog(archivePath));
    } else {
      appendDebugLogLineImpl("unarr RAR index skipped: " +
                             pathForLog(archivePath) +
                             ": unknown RAR signature");
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  std::string sevenZipError;
  if (!loadedEntries && hasSevenZipArchiveExtension(archivePath) &&
      listSevenZipEntries(archivePath, loaded->entries,
                          loaded->sevenZipFormat, &sevenZipError)) {
    loaded->backend = ArchiveIndexBackend::SevenZip;
    loadedEntries = true;
  } else if (!loadedEntries && hasSevenZipArchiveExtension(archivePath) &&
             !sevenZipError.empty()) {
    appendDebugLogLineImpl("7-Zip index failed: " + pathForLog(archivePath) +
                           ": " + sevenZipError);
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipError;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  std::string libarchiveError;
  if (!loadedEntries &&
      listEntriesUncached(archivePath, loaded->entries, &libarchiveError)) {
    loaded->backend = ArchiveIndexBackend::LibArchive;
    loadedEntries = true;
  } else if (!loadedEntries && !libarchiveError.empty()) {
    appendDebugLogLineImpl("libarchive index failed: " +
                           pathForLog(archivePath) + ": " + libarchiveError);
    if (errorMessage != nullptr) {
      *errorMessage = libarchiveError;
    }
  }
#endif
  if (!loadedEntries) {
    appendDebugLogLineImpl("Archive indexing failed: " +
                           pathForLog(archivePath));
    return nullptr;
  }
  buildIndexLookups(*loaded);
  appendDebugLogLineImpl("Indexed archive with " + backendName(loaded->backend) +
                         ": " + pathForLog(archivePath) + " entries=" +
                         std::to_string(loaded->entries.size()));

  {
    std::lock_guard<std::mutex> lock(gIndexMutex);
    gIndexCache[key] = loaded;
  }
  return loaded;
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  return nullptr;
#endif
}

const Entry *findIndexedEntry(const CachedIndex &index,
                              const std::filesystem::path &innerPath) {
  const std::string target = normalizeEntryName(innerPath.generic_string());
  if (target.empty()) {
    return nullptr;
  }

  const auto exactIt = index.exact.find(target);
  if (exactIt != index.exact.end() && exactIt->second < index.entries.size()) {
    return &index.entries[exactIt->second];
  }

  const auto lowerIt = index.lower.find(lowerCopy(target));
  if (lowerIt != index.lower.end() && lowerIt->second < index.entries.size()) {
    return &index.entries[lowerIt->second];
  }

  return nullptr;
}

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
struct CachedReadTarget {
  std::filesystem::path entryPath;
  std::size_t order = 0;
};

bool readArchiveEntriesByCachedOrder(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  const auto index = cachedIndexForArchiveIfFresh(archivePath);
  if (index == nullptr) {
    return false;
  }

  std::vector<CachedReadTarget> targets;
  targets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    targets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
    });
  }
  if (targets.empty()) {
    return true;
  }

  std::sort(targets.begin(), targets.end(),
            [](const CachedReadTarget &a, const CachedReadTarget &b) {
              return a.order < b.order;
            });
  targets.erase(std::unique(targets.begin(), targets.end(),
                            [](const CachedReadTarget &a,
                               const CachedReadTarget &b) {
                              return a.order == b.order;
                            }),
                targets.end());

  archive *archiveHandle = openArchive(archivePath, errorMessage);
  if (archiveHandle == nullptr) {
    return false;
  }

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    files.clear();
    archive_read_free(archiveHandle);
    return false;
  };

  archive_entry *entry = nullptr;
  std::array<unsigned char, 64 * 1024> buffer{};
  std::size_t entryOrder = 0;
  std::size_t targetIndex = 0;
  while (targetIndex < targets.size()) {
    const int status = archive_read_next_header(archiveHandle, &entry);
    if (status == ARCHIVE_EOF) {
      break;
    }
    if (status == ARCHIVE_RETRY) {
      continue;
    }
    if (status < ARCHIVE_WARN) {
      return fail("Could not read archive: " +
                  archiveErrorString(archiveHandle, ""));
    }
    if (entry == nullptr) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    ArchiveEntryInfo info;
    if (!archiveEntryInfo(entry, entryPathnameUtf8(entry), info)) {
      archive_read_data_skip(archiveHandle);
      continue;
    }

    const std::size_t currentOrder = entryOrder++;
    const CachedReadTarget &target = targets[targetIndex];
    if (currentOrder < target.order) {
      archive_read_data_skip(archiveHandle);
      continue;
    }
    if (currentOrder > target.order) {
      return fail("Cached archive entry order did not match archive stream.");
    }
    if (!info.regular) {
      return fail("Cached archive entry is not a regular file.");
    }

    const std::string actual =
        normalizeEntryName(info.relativePath.generic_string());
    const std::string expected =
        normalizeEntryName(target.entryPath.generic_string());
    if (actual != expected) {
      return fail("Cached archive entry path did not match archive stream.");
    }

    FileData file;
    file.path = target.entryPath;
    if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0) {
      file.bytes.reserve(static_cast<size_t>(archive_entry_size(entry)));
    }
    for (;;) {
      const la_ssize_t count =
          archive_read_data(archiveHandle, buffer.data(), buffer.size());
      if (count == 0) {
        break;
      }
      if (count < 0) {
        return fail("Could not read archive entry: " +
                    archiveErrorString(archiveHandle, ""));
      }
      file.bytes.insert(file.bytes.end(), buffer.begin(),
                        buffer.begin() + count);
    }

    files.push_back(std::move(file));
    ++targetIndex;
  }

  archive_read_free(archiveHandle);
  return targetIndex == targets.size();
}
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
struct ZipReadTarget {
  std::string normalized;
  std::filesystem::path entryPath;
  std::size_t order = 0;
  std::uint64_t size = 0;
};

std::optional<std::string> minizFilename(mz_zip_archive *archive,
                                         mz_uint fileIndex) {
  const mz_uint size = mz_zip_reader_get_filename(archive, fileIndex, nullptr, 0);
  if (size == 0) {
    return std::nullopt;
  }
  std::string filename(size, '\0');
  if (mz_zip_reader_get_filename(archive, fileIndex, filename.data(), size) ==
      0) {
    return std::nullopt;
  }
  if (!filename.empty() && filename.back() == '\0') {
    filename.pop_back();
  }
  return filename;
}

enum class ZipNameMatch { Matches, Mismatches, Unknown };

std::optional<std::string> normalizedZipEntryName(const std::string &filename,
                                                  bool *knownMismatch) {
  if (knownMismatch != nullptr) {
    *knownMismatch = false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE && ASOBMSHOW_ARCHIVEFILE_HAS_ICONV
  if (!isValidUtf8(filename)) {
    std::filesystem::path convertedPath;
    bool convertedAny = false;
    for (const char *encoding : {"CP932", "SHIFT_JIS", "SHIFT-JIS", "SJIS"}) {
      const auto converted = convertTextToUtf8(filename, encoding);
      if (!converted.has_value()) {
        continue;
      }
      convertedAny = true;
      if (safeEntryPath(*converted, convertedPath)) {
        return normalizeEntryName(convertedPath.generic_string());
      }
    }
    if (knownMismatch != nullptr) {
      *knownMismatch = convertedAny;
    }
    return std::nullopt;
  }
#elif ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  if (!isValidUtf8(filename)) {
    return std::nullopt;
  }
#endif

  std::filesystem::path minizPath;
  if (safeEntryPath(filename, minizPath)) {
    return normalizeEntryName(minizPath.generic_string());
  }

  if (knownMismatch != nullptr) {
    *knownMismatch = true;
  }
  return std::nullopt;
}

ZipNameMatch compareZipEntryName(const std::string &filename,
                                 const std::string &target) {
  bool knownMismatch = false;
  const auto normalized = normalizedZipEntryName(filename, &knownMismatch);
  if (normalized.has_value() && *normalized == target) {
    return ZipNameMatch::Matches;
  }
  return knownMismatch ? ZipNameMatch::Mismatches : ZipNameMatch::Unknown;
}

bool listZipEntries(const std::filesystem::path &archivePath,
                    std::vector<Entry> &entries, std::string *errorMessage) {
  entries.clear();

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = path_t_to_utf8(fspath_to_path_t(archivePath));
  if (!mz_zip_reader_init_file_v2(&archive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    entries.clear();
    mz_zip_reader_end(&archive);
    return false;
  };

  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  entries.reserve(fileCount);
  for (mz_uint fileIndex = 0; fileIndex < fileCount; ++fileIndex) {
    const auto filename = minizFilename(&archive, fileIndex);
    if (!filename.has_value()) {
      return fail("Could not read ZIP central directory filename.");
    }

    const auto normalized = normalizedZipEntryName(*filename, nullptr);
    if (!normalized.has_value() || normalized->empty()) {
      continue;
    }

    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, fileIndex, &stat)) {
      return fail("Could not read ZIP central directory entry.");
    }

    entries.push_back({
        .path = std::filesystem::path(*normalized),
        .directory = stat.m_is_directory != 0,
        .size = stat.m_uncomp_size,
        .order = static_cast<std::size_t>(fileIndex),
    });
  }

  mz_zip_reader_end(&archive);
  return true;
}

bool readZipEntryByFileIndex(mz_zip_archive *archive, mz_uint fileIndex,
                             const std::filesystem::path &entryPath,
                             FileData &file, std::string *errorMessage) {
  mz_zip_archive_file_stat stat{};
  if (!mz_zip_reader_file_stat(archive, fileIndex, &stat)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not read ZIP central directory entry.";
    }
    return false;
  }
  if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry is not supported by direct reader.";
    }
    return false;
  }
  if (stat.m_uncomp_size >
      static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
    if (errorMessage != nullptr) {
      *errorMessage = "ZIP entry is too large to read into memory.";
    }
    return false;
  }

  file.path = entryPath;
  file.bytes.resize(static_cast<std::size_t>(stat.m_uncomp_size));
  if (!mz_zip_reader_extract_to_mem(archive, fileIndex, file.bytes.data(),
                                    file.bytes.size(), 0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not extract ZIP entry by index.";
    }
    return false;
  }
  return true;
}

bool readZipEntriesByName(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    std::vector<FileData> &files, std::string *errorMessage) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  std::unordered_map<std::string, std::filesystem::path> exactTargets;
  std::unordered_map<std::string, std::filesystem::path> lowerTargets;
  exactTargets.reserve(innerPaths.size());
  lowerTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (target.empty()) {
      continue;
    }
    exactTargets.emplace(target, innerPath);
    lowerTargets.emplace(lowerCopy(target), innerPath);
  }
  if (exactTargets.empty()) {
    return true;
  }

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = path_t_to_utf8(fspath_to_path_t(archivePath));
  if (!mz_zip_reader_init_file_v2(&archive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    files.clear();
    mz_zip_reader_end(&archive);
    return false;
  };

  std::vector<ZipReadTarget> readTargets;
  readTargets.reserve(exactTargets.size());
  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  for (mz_uint fileIndex = 0; fileIndex < fileCount && !exactTargets.empty();
       ++fileIndex) {
    const auto filename = minizFilename(&archive, fileIndex);
    if (!filename.has_value()) {
      continue;
    }

    const auto normalized = normalizedZipEntryName(*filename, nullptr);
    if (!normalized.has_value()) {
      continue;
    }

    auto targetIt = exactTargets.find(*normalized);
    if (targetIt == exactTargets.end()) {
      const auto lowerIt = lowerTargets.find(lowerCopy(*normalized));
      if (lowerIt == lowerTargets.end()) {
        continue;
      }
      targetIt = exactTargets.find(normalizeEntryName(
          lowerIt->second.generic_string()));
      if (targetIt == exactTargets.end()) {
        continue;
      }
    }

    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, fileIndex, &stat)) {
      return fail("Could not read ZIP central directory entry.");
    }
    if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
      return fail("ZIP entry is not supported by direct reader.");
    }
    if (stat.m_uncomp_size >
        static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
      return fail("ZIP entry is too large to read into memory.");
    }

    readTargets.push_back({
        .normalized = *normalized,
        .entryPath = targetIt->second,
        .order = static_cast<std::size_t>(fileIndex),
        .size = static_cast<std::uint64_t>(stat.m_uncomp_size),
    });
    lowerTargets.erase(lowerCopy(targetIt->first));
    exactTargets.erase(targetIt);
  }

  for (const ZipReadTarget &target : readTargets) {
    FileData file;
    std::string readError;
    if (!readZipEntryByFileIndex(&archive, static_cast<mz_uint>(target.order),
                                 target.entryPath, file, &readError)) {
      return fail(readError.empty() ? "Could not extract ZIP entry by name."
                                    : readError);
    }
    files.push_back(std::move(file));
  }

  mz_zip_reader_end(&archive);
  return true;
}

bool readZipEntriesByIndex(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  std::unordered_map<std::string, std::filesystem::path> targets;
  for (const auto &innerPath : innerPaths) {
    const std::string target = normalizeEntryName(innerPath.generic_string());
    if (!target.empty()) {
      targets.emplace(target, innerPath);
    }
  }
  if (targets.empty()) {
    return true;
  }

  const auto index = cachedIndexForArchive(archivePath, errorMessage);
  if (index == nullptr) {
    return false;
  }

  std::vector<ZipReadTarget> readTargets;
  readTargets.reserve(targets.size());
  for (const auto &targetPair : targets) {
    const std::filesystem::path &requestedPath = targetPair.second;
    const Entry *entry = findIndexedEntry(*index, requestedPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }

    readTargets.push_back({
        .normalized = normalizeEntryName(entry->path.generic_string()),
        .entryPath = entry->path,
        .order = entry->order,
        .size = entry->size,
    });
  }
  if (readTargets.empty()) {
    return true;
  }
  std::sort(readTargets.begin(), readTargets.end(),
            [](const ZipReadTarget &a, const ZipReadTarget &b) {
              return a.order < b.order;
            });

  mz_zip_archive archive{};
  mz_zip_zero_struct(&archive);
  const std::string archiveText = path_t_to_utf8(fspath_to_path_t(archivePath));
  if (!mz_zip_reader_init_file_v2(&archive, archiveText.c_str(),
                                  MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY, 0,
                                  0)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not open ZIP central directory.";
    }
    return false;
  }

  auto fail = [&](const std::string &message) {
    if (errorMessage != nullptr) {
      *errorMessage = message;
    }
    files.clear();
    mz_zip_reader_end(&archive);
    return false;
  };

  const mz_uint fileCount = mz_zip_reader_get_num_files(&archive);
  for (const ZipReadTarget &target : readTargets) {
    if (target.order >= static_cast<std::size_t>(fileCount)) {
      return fail("ZIP index is out of range.");
    }

    const mz_uint fileIndex = static_cast<mz_uint>(target.order);
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&archive, fileIndex, &stat)) {
      return fail("Could not read ZIP central directory entry.");
    }
    if (stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported) {
      return fail("ZIP entry is not supported by direct reader.");
    }
    if (stat.m_uncomp_size != target.size) {
      return fail("ZIP central directory size did not match archive index.");
    }
    const auto filename = minizFilename(&archive, fileIndex);
    if (!filename.has_value()) {
      return fail("Could not read ZIP central directory filename.");
    }

    if (compareZipEntryName(*filename, target.normalized) ==
        ZipNameMatch::Mismatches) {
      return fail("ZIP central directory order did not match archive index.");
    }
    if (stat.m_uncomp_size >
        static_cast<mz_uint64>(std::numeric_limits<std::size_t>::max())) {
      return fail("ZIP entry is too large to read into memory.");
    }

    FileData file;
    file.path = target.entryPath;
    file.bytes.resize(static_cast<std::size_t>(stat.m_uncomp_size));
    if (!mz_zip_reader_extract_to_mem(&archive, fileIndex, file.bytes.data(),
                                      file.bytes.size(), 0)) {
      return fail("Could not extract ZIP entry by index.");
    }
    files.push_back(std::move(file));
  }

  mz_zip_reader_end(&archive);
  return true;
}
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
struct UnarrRarReadTarget {
  std::filesystem::path entryPath;
  std::size_t order = 0;
  std::int64_t offset = -1;
  std::uint64_t size = 0;
};

bool readUnarrRarEntriesByOffset(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  const auto index = cachedIndexForArchive(archivePath, errorMessage);
  if (index == nullptr || index->backend != ArchiveIndexBackend::UnarrRar) {
    return false;
  }

  std::vector<UnarrRarReadTarget> readTargets;
  readTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory || entry->offset < 0) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    if (entry->solid) {
      if (errorMessage != nullptr) {
        *errorMessage =
            "RAR entry is solid; random-access extraction is impossible.";
      }
      files.clear();
      return false;
    }
    readTargets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
        .offset = entry->offset,
        .size = entry->size,
    });
  }
  if (readTargets.empty()) {
    return true;
  }

  std::sort(readTargets.begin(), readTargets.end(),
            [](const UnarrRarReadTarget &a, const UnarrRarReadTarget &b) {
              return a.offset < b.offset;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const UnarrRarReadTarget &a,
                                   const UnarrRarReadTarget &b) {
                                  return a.offset == b.offset;
                                }),
                    readTargets.end());

  UnarrStreamHandle stream;
  UnarrArchiveHandle archive;
  if (!openUnarrRarArchive(archivePath, stream, archive, errorMessage)) {
    return false;
  }

  using Clock = std::chrono::steady_clock;
  const auto readStart = Clock::now();
  appendDebugLogLineImpl("Starting unarr RAR random extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " firstOffset=" +
                         std::to_string(readTargets.front().offset) +
                         " lastOffset=" +
                         std::to_string(readTargets.back().offset));

  files.reserve(readTargets.size());
  for (const UnarrRarReadTarget &target : readTargets) {
    if (!ar_parse_entry_at(archive.archive,
                           static_cast<off64_t>(target.offset))) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr could not seek to RAR entry offset.";
      }
      files.clear();
      return false;
    }

    const char *actualName = ar_entry_get_name(archive.archive);
    std::filesystem::path actualPath;
    if (actualName == nullptr || !safeEntryPath(actualName, actualPath) ||
        normalizeEntryName(actualPath.generic_string()) !=
            normalizeEntryName(target.entryPath.generic_string())) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr RAR offset did not match cached entry path.";
      }
      files.clear();
      return false;
    }

    const size_t entrySize = ar_entry_get_size(archive.archive);
    if (static_cast<std::uint64_t>(entrySize) != target.size) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr RAR entry size did not match cached index.";
      }
      files.clear();
      return false;
    }

    FileData file;
    file.path = target.entryPath;
    file.bytes.resize(entrySize);
    if (entrySize > 0 &&
        !ar_entry_uncompress(archive.archive, file.bytes.data(), entrySize)) {
      if (errorMessage != nullptr) {
        *errorMessage = "unarr could not extract RAR entry.";
      }
      files.clear();
      return false;
    }
    files.push_back(std::move(file));
  }

  const auto readMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            readStart)
          .count();
  appendDebugLogLineImpl("Finished unarr RAR random extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(readTargets.size()) +
                         " files=" + std::to_string(files.size()) +
                         " extractMs=" + std::to_string(readMs));
  return true;
}
#endif

#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
struct SevenZipReadTarget {
  std::filesystem::path entryPath;
  std::size_t order = 0;
  std::uint64_t size = 0;
  bool solid = false;
};

std::size_t solidReplayStartOrderForTarget(const CachedIndex &index,
                                           const SevenZipReadTarget &target) {
  if (!target.solid) {
    return target.order;
  }

  auto it = std::lower_bound(
      index.entries.begin(), index.entries.end(), target.order,
      [](const Entry &entry, std::size_t order) { return entry.order < order; });
  if (it == index.entries.end() || it->order != target.order) {
    return target.order;
  }

  while (it != index.entries.begin() && it->solid) {
    --it;
  }
  return it->order;
}

void sevenZipSolidReplayEstimate(const CachedIndex &index,
                                 const std::vector<SevenZipReadTarget> &targets,
                                 std::size_t &replayStartOrder,
                                 std::size_t &replayEntryCount,
                                 std::uint64_t &replayUnpackedBytes) {
  replayStartOrder = targets.empty() ? 0 : targets.front().order;
  replayEntryCount = 0;
  replayUnpackedBytes = 0;
  if (targets.empty()) {
    return;
  }

  bool hasSolidTarget = false;
  for (const SevenZipReadTarget &target : targets) {
    if (!target.solid) {
      continue;
    }
    hasSolidTarget = true;
    replayStartOrder = std::min(
        replayStartOrder, solidReplayStartOrderForTarget(index, target));
  }
  if (!hasSolidTarget) {
    return;
  }

  const std::size_t replayEndOrder = targets.back().order;
  for (const Entry &entry : index.entries) {
    if (entry.order < replayStartOrder || entry.order > replayEndOrder) {
      continue;
    }
    ++replayEntryCount;
    if (!entry.directory) {
      replayUnpackedBytes += entry.size;
    }
  }
}

bool readSevenZipEntriesByIndex(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const std::optional<EntryRange> &range, std::vector<FileData> &files,
    std::string *errorMessage) {
  files.clear();
  if (innerPaths.empty()) {
    return true;
  }

  const auto index = cachedIndexForArchive(archivePath, errorMessage);
  if (index == nullptr || index->backend != ArchiveIndexBackend::SevenZip ||
      index->sevenZipFormat == 0) {
    return false;
  }

  std::vector<SevenZipReadTarget> readTargets;
  readTargets.reserve(innerPaths.size());
  for (const auto &innerPath : innerPaths) {
    const Entry *entry = findIndexedEntry(*index, innerPath);
    if (entry == nullptr || entry->directory) {
      continue;
    }
    if (entry->order > std::numeric_limits<UInt32>::max()) {
      continue;
    }
    if (range.has_value() &&
        (entry->order < range->start || entry->order > range->end)) {
      continue;
    }
    readTargets.push_back({
        .entryPath = entry->path,
        .order = entry->order,
        .size = entry->size,
        .solid = entry->solid,
    });
  }
  if (readTargets.empty()) {
    return true;
  }

  std::sort(readTargets.begin(), readTargets.end(),
            [](const SevenZipReadTarget &a, const SevenZipReadTarget &b) {
              return a.order < b.order;
            });
  readTargets.erase(std::unique(readTargets.begin(), readTargets.end(),
                                [](const SevenZipReadTarget &a,
                                   const SevenZipReadTarget &b) {
                                  return a.order == b.order;
                                }),
                    readTargets.end());

  std::size_t predictedSolidReplayStart = 0;
  std::size_t predictedSolidReplayEntries = 0;
  std::uint64_t predictedSolidReplayBytes = 0;
  sevenZipSolidReplayEstimate(*index, readTargets, predictedSolidReplayStart,
                              predictedSolidReplayEntries,
                              predictedSolidReplayBytes);

  bool archiveCacheHit = false;
  long long openMs = 0;
  const auto archiveState = openCachedSevenZipArchive(
      archivePath, index->sevenZipFormat, &archiveCacheHit, &openMs,
      errorMessage);
  if (archiveState == nullptr) {
    return false;
  }

  std::lock_guard<std::mutex> archiveLock(archiveState->mutex);
  IInArchive *archive = archiveState->archive.Interface();
  if (archive == nullptr) {
    if (errorMessage != nullptr) {
      *errorMessage = "Cached 7-Zip archive is unavailable.";
    }
    return false;
  }
  UInt32 itemCount = 0;
  HRESULT result = archive->GetNumberOfItems(&itemCount);
  if (result != S_OK) {
    if (errorMessage != nullptr) {
      *errorMessage = sevenZipResultMessage(result);
    }
    return false;
  }

  std::vector<UInt32> itemIndices;
  itemIndices.reserve(readTargets.size());
  std::unordered_map<UInt32, FileData *> outputTargets;
  outputTargets.reserve(readTargets.size());
  files.reserve(readTargets.size());
  std::size_t solidTargets = 0;

  for (const SevenZipReadTarget &target : readTargets) {
    const auto itemIndex = static_cast<UInt32>(target.order);
    if (itemIndex >= itemCount) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index is out of range.";
      }
      files.clear();
      return false;
    }

    auto actualPath = sevenZipStringProperty(archive, itemIndex, kpidPath);
    if (!actualPath.has_value() || actualPath->empty()) {
      actualPath = sevenZipStringProperty(archive, itemIndex, kpidName);
    }
    std::filesystem::path relativePath;
    if (!actualPath.has_value() ||
        !safeEntryPath(*actualPath, relativePath) ||
        normalizeEntryName(relativePath.generic_string()) !=
            normalizeEntryName(target.entryPath.generic_string())) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive index did not match cached entry path.";
      }
      files.clear();
      return false;
    }
    if (sevenZipBoolProperty(archive, itemIndex, kpidIsDir, false) ||
        sevenZipBoolProperty(archive, itemIndex, kpidEncrypted, false)) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive entry is not extractable.";
      }
      files.clear();
      return false;
    }
    const bool actualSolid =
        sevenZipBoolProperty(archive, itemIndex, kpidSolid, false);
    if (actualSolid != target.solid) {
      if (errorMessage != nullptr) {
        *errorMessage = "7-Zip archive solid flag did not match cached index.";
      }
      files.clear();
      return false;
    }
    if (target.solid) {
      ++solidTargets;
    }

    FileData file;
    file.path = target.entryPath;
    if (target.size > 0 &&
        target.size <=
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
      file.bytes.reserve(static_cast<std::size_t>(target.size));
    }
    files.push_back(std::move(file));
    itemIndices.push_back(itemIndex);
    outputTargets.emplace(itemIndex, &files.back());
  }

  auto *callback = new SevenZipExtractCallback(std::move(outputTargets));
  IArchiveExtractCallback *callbackInterface = callback;
  callbackInterface->AddRef();
  CMyComPtr<IArchiveExtractCallback> callbackHandle;
  callbackHandle.Attach(callbackInterface);

  appendDebugLogLineImpl("Starting 7-Zip extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(itemIndices.size()) +
                         " firstOrder=" +
                         std::to_string(readTargets.front().order) +
                         " lastOrder=" +
                         std::to_string(readTargets.back().order) +
                         " solidTargets=" +
                         std::to_string(solidTargets) +
                         (solidTargets > 0
                              ? " solidReplayStart=" +
                                    std::to_string(predictedSolidReplayStart) +
                                    " solidReplayEntries=" +
                                    std::to_string(predictedSolidReplayEntries) +
                                    " solidReplayUnpacked=" +
                                    byteCountForLog(predictedSolidReplayBytes) +
                                    " note=solid-archives-decode-sequentially"
                              : "") +
                         " archiveCache=" +
                         (archiveCacheHit ? "hit" : "miss") +
                         " openMs=" + std::to_string(openMs));
  using Clock = std::chrono::steady_clock;
  const auto extractStart = Clock::now();
  result =
      archive->Extract(itemIndices.data(), static_cast<UInt32>(itemIndices.size()),
                       0, callbackHandle);
  const auto extractMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                            extractStart)
          .count();
  if (result != S_OK || callback->failed()) {
    if (errorMessage != nullptr) {
      *errorMessage =
          result != S_OK
              ? sevenZipResultMessage(result)
              : "7-Zip archive extraction failed with operation result: " +
                    std::to_string(callback->operationResult());
    }
    files.clear();
    return false;
  }

  appendDebugLogLineImpl("Finished 7-Zip extraction: " +
                         pathForLog(archivePath) +
                         " targets=" + std::to_string(itemIndices.size()) +
                         " files=" + std::to_string(files.size()) +
                         " extractMs=" + std::to_string(extractMs));
  return true;
}
#endif

std::optional<std::filesystem::path>
resolveInnerPath(const std::filesystem::path &archivePath,
                 const std::filesystem::path &innerPath) {
  const auto index = cachedIndexForArchive(archivePath, nullptr);
  if (index == nullptr) {
    return std::nullopt;
  }
  if (const Entry *entry = findIndexedEntry(*index, innerPath);
      entry != nullptr && !entry->directory) {
    return entry->path;
  }
  return std::nullopt;
}

std::uint64_t fnv1a64(const std::string &value) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string hex64(std::uint64_t value) {
  constexpr char digits[] = "0123456789abcdef";
  std::string out(16, '0');
  for (int i = 15; i >= 0; --i) {
    out[i] = digits[value & 0xf];
    value >>= 4;
  }
  return out;
}

std::filesystem::path archiveCacheRoot() {
  return std::filesystem::temp_directory_path() / "AsoBMaShowArchiveCache";
}

} // namespace

bool isArchiveSupportAvailable() {
  return ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ != 0 ||
         ASOBMSHOW_ARCHIVEFILE_HAS_UNARR != 0 ||
         ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE != 0 ||
         ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP != 0;
}

bool hasSupportedArchiveExtension(const std::filesystem::path &path) {
  return !archiveExtensionFromPath(path).empty();
}

void appendDebugLogLine(const std::string &message) {
  appendDebugLogLineImpl(message);
}

std::uint64_t debugLogRevision() {
  std::lock_guard<std::mutex> lock(gDebugLogMutex);
  return gDebugLogRevision;
}

std::string debugLogText() {
  std::lock_guard<std::mutex> lock(gDebugLogMutex);
  if (gDebugLogLines.empty()) {
    return "No parsing logs yet.";
  }

  std::string text;
  for (const std::string &line : gDebugLogLines) {
    if (!text.empty()) {
      text += '\n';
    }
    text += line;
  }
  return text;
}

bool splitVirtualPath(const std::filesystem::path &path,
                      std::filesystem::path &archivePath,
                      std::filesystem::path &innerPath) {
  archivePath.clear();
  innerPath.clear();
  if (path.empty()) {
    return false;
  }

  std::filesystem::path current;
  bool foundArchive = false;
  for (const auto &part : path.lexically_normal()) {
    if (!foundArchive) {
      current /= part;
      if (hasSupportedArchiveExtension(current)) {
        std::error_code error;
        if (std::filesystem::is_regular_file(current, error) && !error) {
          archivePath = current;
          foundArchive = true;
        }
      }
      continue;
    }
    innerPath /= part;
  }

  if (!foundArchive || innerPath.empty()) {
    archivePath.clear();
    innerPath.clear();
    return false;
  }
  return true;
}

bool isVirtualPath(const std::filesystem::path &path) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  return splitVirtualPath(path, archivePath, innerPath);
}

std::filesystem::path makeVirtualPath(const std::filesystem::path &archivePath,
                                      const std::filesystem::path &innerPath) {
  if (innerPath.empty()) {
    return archivePath;
  }
  return archivePath / innerPath;
}

bool listEntries(const std::filesystem::path &archivePath,
                 std::vector<Entry> &entries, std::string *errorMessage) {
  entries.clear();
  if (!isArchiveSupportAvailable() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive format is not supported for direct browsing.";
    }
    return false;
  }

  const auto index = cachedIndexForArchive(archivePath, errorMessage);
  if (index == nullptr) {
    return false;
  }
  entries = index->entries;
  return true;
}

bool readArchiveEntries(const std::filesystem::path &archivePath,
                        const std::vector<std::filesystem::path> &innerPaths,
                        std::vector<FileData> &files,
                        std::string *errorMessage) {
  files.clear();
  if (!isArchiveSupportAvailable() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive format is not supported for direct browsing.";
    }
    appendDebugLogLineImpl("Archive batch read unsupported: " +
                           pathForLog(archivePath));
    return false;
  }

  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + archivePath.string();
    }
    appendDebugLogLineImpl("Archive batch read unavailable: " +
                           pathForLog(archivePath));
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (hasZipArchiveExtension(archivePath) &&
      readZipEntriesByIndex(archivePath, innerPaths, std::nullopt, files,
                            &zipError)) {
    appendDebugLogLineImpl("Read archive batch via miniz ZIP: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()));
    return true;
  }
  if (hasZipArchiveExtension(archivePath) && !zipError.empty()) {
    appendDebugLogLineImpl("miniz ZIP batch read failed: " +
                           pathForLog(archivePath) + ": " + zipError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  std::string unarrError;
  if (hasRarArchiveExtension(archivePath) &&
      readUnarrRarEntriesByOffset(archivePath, innerPaths, std::nullopt, files,
                                  &unarrError)) {
    appendDebugLogLineImpl("Read archive batch via unarr RAR random access: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()));
    return true;
  }
  if (hasRarArchiveExtension(archivePath) && !unarrError.empty()) {
    appendDebugLogLineImpl("unarr RAR batch read failed: " +
                           pathForLog(archivePath) + ": " + unarrError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  std::string sevenZipError;
  if (hasSevenZipArchiveExtension(archivePath) &&
      readSevenZipEntriesByIndex(archivePath, innerPaths, std::nullopt, files,
                                 &sevenZipError)) {
    appendDebugLogLineImpl("Read archive batch via 7-Zip SDK: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()));
    return true;
  }
  if (hasSevenZipArchiveExtension(archivePath) && !sevenZipError.empty()) {
    appendDebugLogLineImpl("7-Zip batch read failed: " +
                           pathForLog(archivePath) + ": " + sevenZipError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  std::string cachedOrderError;
  if (readArchiveEntriesByCachedOrder(archivePath, innerPaths, std::nullopt,
                                      files, &cachedOrderError)) {
    appendDebugLogLineImpl("Read archive batch via cached libarchive order: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()));
    return true;
  }
  if (!cachedOrderError.empty()) {
    appendDebugLogLineImpl("Cached-order archive batch read failed: " +
                           pathForLog(archivePath) + ": " + cachedOrderError);
  }
  files.clear();
  const bool read =
      readArchiveEntriesUncached(archivePath, innerPaths, std::nullopt, files,
                                 errorMessage);
  appendDebugLogLineImpl(
      std::string(read ? "Read archive batch via libarchive scan: "
                       : "Archive batch read failed: ") +
      pathForLog(archivePath) + " targets=" +
      std::to_string(innerPaths.size()) + " files=" +
      std::to_string(files.size()) +
      ((!read && errorMessage != nullptr && !errorMessage->empty())
           ? ": " + *errorMessage
           : ""));
  return read;
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  appendDebugLogLineImpl("Archive support is not compiled in for batch read: " +
                         pathForLog(archivePath));
  return false;
#endif
}

bool readArchiveEntriesInRange(
    const std::filesystem::path &archivePath,
    const std::vector<std::filesystem::path> &innerPaths,
    const EntryRange &range, std::vector<FileData> &files,
    std::string *errorMessage) {
  files.clear();
  if (range.end < range.start) {
    return true;
  }
  if (!isArchiveSupportAvailable() || !hasSupportedArchiveExtension(archivePath)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive format is not supported for direct browsing.";
    }
    appendDebugLogLineImpl("Archive ranged read unsupported: " +
                           pathForLog(archivePath));
    return false;
  }

  std::uintmax_t size = 0;
  std::filesystem::file_time_type mtime{};
  if (!fileState(archivePath, size, mtime)) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive file is unavailable: " + archivePath.string();
    }
    appendDebugLogLineImpl("Archive ranged read unavailable: " +
                           pathForLog(archivePath));
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  std::string zipError;
  if (hasZipArchiveExtension(archivePath) &&
      readZipEntriesByIndex(archivePath, innerPaths, std::nullopt, files,
                            &zipError)) {
    appendDebugLogLineImpl("Read archive range via miniz ZIP: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()) +
                           " range=" + std::to_string(range.start) + "-" +
                           std::to_string(range.end));
    return true;
  }
  if (hasZipArchiveExtension(archivePath) && !zipError.empty()) {
    appendDebugLogLineImpl("miniz ZIP ranged read failed: " +
                           pathForLog(archivePath) + ": " + zipError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  std::string unarrError;
  if (hasRarArchiveExtension(archivePath) &&
      readUnarrRarEntriesByOffset(archivePath, innerPaths, std::nullopt, files,
                                  &unarrError)) {
    appendDebugLogLineImpl("Read archive range via unarr RAR random access: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()) +
                           " range=" + std::to_string(range.start) + "-" +
                           std::to_string(range.end));
    return true;
  }
  if (hasRarArchiveExtension(archivePath) && !unarrError.empty()) {
    appendDebugLogLineImpl("unarr RAR ranged read failed: " +
                           pathForLog(archivePath) + ": " + unarrError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  std::string sevenZipError;
  if (hasSevenZipArchiveExtension(archivePath) &&
      readSevenZipEntriesByIndex(archivePath, innerPaths, std::nullopt, files,
                                 &sevenZipError)) {
    appendDebugLogLineImpl("Read archive range via 7-Zip SDK: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()) +
                           " range=" + std::to_string(range.start) + "-" +
                           std::to_string(range.end));
    return true;
  }
  if (hasSevenZipArchiveExtension(archivePath) && !sevenZipError.empty()) {
    appendDebugLogLineImpl("7-Zip ranged read failed: " +
                           pathForLog(archivePath) + ": " + sevenZipError);
  }
  files.clear();
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  std::string cachedOrderError;
  if (readArchiveEntriesByCachedOrder(archivePath, innerPaths, range, files,
                                      &cachedOrderError)) {
    appendDebugLogLineImpl("Read archive range via cached libarchive order: " +
                           pathForLog(archivePath) +
                           " targets=" + std::to_string(innerPaths.size()) +
                           " files=" + std::to_string(files.size()) +
                           " range=" + std::to_string(range.start) + "-" +
                           std::to_string(range.end));
    return true;
  }
  if (!cachedOrderError.empty()) {
    appendDebugLogLineImpl("Cached-order archive ranged read failed: " +
                           pathForLog(archivePath) + ": " + cachedOrderError);
  }
  files.clear();
  const bool read =
      readArchiveEntriesUncached(archivePath, innerPaths, range, files,
                                 errorMessage);
  appendDebugLogLineImpl(
      std::string(read ? "Read archive range via libarchive scan: "
                       : "Archive ranged read failed: ") +
      pathForLog(archivePath) + " targets=" +
      std::to_string(innerPaths.size()) + " files=" +
      std::to_string(files.size()) + " range=" +
      std::to_string(range.start) + "-" + std::to_string(range.end) +
      ((!read && errorMessage != nullptr && !errorMessage->empty())
           ? ": " + *errorMessage
           : ""));
  return read;
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  appendDebugLogLineImpl("Archive support is not compiled in for ranged read: " +
                         pathForLog(archivePath));
  return false;
#endif
}

std::optional<EntryRange>
entryRangeForFolder(const std::filesystem::path &folderPath) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(folderPath, archivePath, innerPath)) {
    std::error_code error;
    if (!hasSupportedArchiveExtension(folderPath) ||
        !std::filesystem::is_regular_file(folderPath, error) || error) {
      return std::nullopt;
    }
    archivePath = folderPath;
    innerPath.clear();
  }

  const auto index = cachedIndexForArchive(archivePath, nullptr);
  if (index == nullptr || index->entries.empty()) {
    return std::nullopt;
  }
  if (innerPath.empty()) {
    return EntryRange{.start = index->entries.front().order,
                      .end = index->entries.back().order};
  }

  EntryRange range;
  bool found = false;
  for (const Entry &entry : index->entries) {
    if (!pathIsInsideFolder(entry.path, innerPath)) {
      continue;
    }
    if (!found) {
      range.start = entry.order;
      range.end = entry.order;
      found = true;
      continue;
    }
    range.start = std::min(range.start, entry.order);
    range.end = std::max(range.end, entry.order);
  }
  if (!found) {
    return std::nullopt;
  }
  return range;
}

bool exists(const std::filesystem::path &path) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    std::error_code error;
    return std::filesystem::exists(path, error) && !error;
  }
  return resolveInnerPath(archivePath, innerPath).has_value();
}

bool readFile(const std::filesystem::path &path,
              std::vector<unsigned char> &bytes, std::string *errorMessage) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    return readRegularFile(path, bytes, errorMessage);
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  if (hasZipArchiveExtension(archivePath)) {
    std::vector<FileData> files;
    if (readZipEntriesByName(archivePath, {innerPath}, files, nullptr) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }
#endif

  const auto resolvedInner = resolveInnerPath(archivePath, innerPath);
  if (!resolvedInner.has_value()) {
    if (errorMessage != nullptr) {
      *errorMessage = "Archive entry not found: " + innerPath.generic_string();
    }
    appendDebugLogLineImpl("Archive entry not found: " +
                           pathForLog(makeVirtualPath(archivePath,
                                                      innerPath)));
    return false;
  }

#if ASOBMSHOW_ARCHIVEFILE_HAS_MINIZ
  if (hasZipArchiveExtension(archivePath)) {
    std::vector<FileData> files;
    if (readZipEntriesByIndex(archivePath, {*resolvedInner}, std::nullopt,
                              files, nullptr) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_UNARR
  if (hasRarArchiveExtension(archivePath)) {
    std::vector<FileData> files;
    if (readUnarrRarEntriesByOffset(archivePath, {*resolvedInner}, std::nullopt,
                                    files, nullptr) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_SEVENZIP
  if (hasSevenZipArchiveExtension(archivePath)) {
    std::vector<FileData> files;
    if (readSevenZipEntriesByIndex(archivePath, {*resolvedInner}, std::nullopt,
                                   files, nullptr) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }
#endif
#if ASOBMSHOW_ARCHIVEFILE_HAS_LIBARCHIVE
  if (const auto range =
          entryRangeForFolder(makeVirtualPath(archivePath,
                                              resolvedInner->parent_path()))) {
    std::vector<FileData> files;
    std::string rangeError;
    if (readArchiveEntriesInRange(archivePath, {*resolvedInner}, *range, files,
                                  &rangeError) &&
        !files.empty()) {
      bytes = std::move(files.front().bytes);
      return true;
    }
  }

  return readArchiveEntry(archivePath, *resolvedInner, bytes, errorMessage);
#else
  if (errorMessage != nullptr) {
    *errorMessage = "Archive support is not compiled in.";
  }
  return false;
#endif
}

std::string cacheKeyForPath(const std::filesystem::path &path) {
  const std::filesystem::path normalized = path.lexically_normal();
  std::string key = path_t_to_utf8(fspath_to_path_t(normalized));

  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (splitVirtualPath(path, archivePath, innerPath)) {
    key += "|archive:";
    key += archiveKey(archivePath);
    key += '|';
    key += fileStateKey(archivePath);
    return key;
  }

  key += "|file:";
  key += fileStateKey(normalized);
  return key;
}

std::optional<std::filesystem::path>
findFileWithExtensions(const std::filesystem::path &basePath,
                       const std::vector<std::string_view> &extensions) {
  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(basePath, archivePath, innerPath)) {
    if (archive_file::exists(basePath)) {
      return basePath;
    }
    for (std::string_view ext : extensions) {
      std::filesystem::path candidate = basePath;
      candidate.replace_extension(std::string(ext));
      if (archive_file::exists(candidate)) {
        return candidate;
      }
    }
    return std::nullopt;
  }

  if (const auto resolved = resolveInnerPath(archivePath, innerPath)) {
    return makeVirtualPath(archivePath, *resolved);
  }
  for (std::string_view ext : extensions) {
    std::filesystem::path candidateInner = innerPath;
    candidateInner.replace_extension(std::string(ext));
    if (const auto resolved = resolveInnerPath(archivePath, candidateInner)) {
      return makeVirtualPath(archivePath, *resolved);
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path>
materializeFile(const std::filesystem::path &path, std::string *errorMessage) {
  if (!isVirtualPath(path)) {
    return path;
  }

  std::vector<unsigned char> bytes;
  if (!readFile(path, bytes, errorMessage)) {
    return std::nullopt;
  }
  return materializeFileBytes(path, bytes, errorMessage);
}

std::optional<std::filesystem::path>
materializeFileBytes(const std::filesystem::path &path,
                     const std::vector<unsigned char> &bytes,
                     std::string *errorMessage) {
  const std::string key = cacheKeyForPath(path);
  std::filesystem::path cacheRoot = archiveCacheRoot();
  std::error_code error;
  std::filesystem::create_directories(cacheRoot, error);
  if (error) {
    if (errorMessage != nullptr) {
      *errorMessage = "Could not create archive cache: " + error.message();
    }
    return std::nullopt;
  }

  std::filesystem::path output =
      cacheRoot / (hex64(fnv1a64(key)) + path.extension().string());
  bool needsWrite = true;
  if (std::filesystem::exists(output, error) && !error) {
    std::uintmax_t size = std::filesystem::file_size(output, error);
    needsWrite = error || size != bytes.size();
  }
  if (needsWrite) {
    std::ofstream file(output, std::ios::binary | std::ios::trunc);
    if (!file) {
      if (errorMessage != nullptr) {
        *errorMessage = "Could not create cached archive entry: " +
                        output.string();
      }
      return std::nullopt;
    }
    if (!bytes.empty()) {
      file.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    }
  }
  return output;
}

void parseChart(bms_parser::Parser &parser, const std::filesystem::path &path,
                bms_parser::Chart **chart, bool addReadyMeasure,
                bool metaOnly, std::atomic_bool &cancelled) {
  if (chart == nullptr) {
    return;
  }
  *chart = nullptr;

  std::filesystem::path archivePath;
  std::filesystem::path innerPath;
  if (!splitVirtualPath(path, archivePath, innerPath)) {
    parser.Parse(path, chart, addReadyMeasure, metaOnly, cancelled);
    return;
  }

  std::vector<unsigned char> bytes;
  std::string errorMessage;
  appendDebugLogLineImpl("Reading archive chart: " + pathForLog(path));
  if (!readFile(path, bytes, &errorMessage)) {
    SDL_Log("Failed to read chart from archive %s: %s",
            path_t_to_utf8(fspath_to_path_t(path)).c_str(),
            errorMessage.c_str());
    appendDebugLogLineImpl("Failed to read archive chart: " +
                           pathForLog(path) + ": " + errorMessage);
    return;
  }

  parser.Parse(bytes, chart, addReadyMeasure, metaOnly, cancelled);
  if (*chart != nullptr) {
    (*chart)->Meta.BmsPath = path;
    (*chart)->Meta.Folder =
        makeVirtualPath(archivePath, innerPath.parent_path());
    appendDebugLogLineImpl("Parsed archive chart: " + pathForLog(path) +
                           " measures=" +
                           std::to_string((*chart)->Measures.size()));
  } else if (cancelled.load()) {
    appendDebugLogLineImpl("Archive chart parse cancelled: " +
                           pathForLog(path));
  } else {
    appendDebugLogLineImpl("Archive chart parser returned null: " +
                           pathForLog(path));
  }
}

} // namespace archive_file
