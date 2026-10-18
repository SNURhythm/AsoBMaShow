#include "StorageArchive.h"
#include "StorageArchiveDb.h"
#include "../text/Utf8.h"
#include "../archive/ArchiveEntryFilter.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#if defined(__ANDROID__)
#include "../targets.h"
#include "../AndroidNatives.h"
#elif defined(_WIN32)
#include <windows.h>
#elif __has_include(<iconv.h>)
#include <iconv.h>
#define STORAGE_ARCHIVE_ICONV 1
#endif
#include <zlib.h>

namespace storage_management {
namespace fs = std::filesystem;
using namespace archive_detail;
namespace {
FileIdentity identityOf(const fs::path &path, const std::vector<fs::path> &roots) {
  std::string error; const auto identity = ReadFileIdentity(path, error, roots);
  if (!identity) throw StorageArchiveError(StorageReason::UnsafePath, error);
  return *identity;
}
std::string pathText(const fs::path &path) { const auto utf8 = path.generic_u8string(); return {utf8.begin(), utf8.end()}; }
std::string lower(std::string text) {
  for (auto &c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A'; return text;
}
std::string decoded(std::string raw, bool utf8) {
  if (asobmashow::text::validUtf8(raw)) return raw;
  require(!utf8, "Invalid UTF-8 archive name");
#if defined(__ANDROID__)
  auto result = ConvertAndroidMs932ToUtf8(raw);
  // Java's String decoder substitutes U+FFFD for malformed input. CP932
  // cannot encode that scalar, so reject substitution before accepting a name.
  require(result && result->find("\xef\xbf\xbd") == std::string::npos, "Invalid CP932 archive name");
  return *result;
#elif defined(_WIN32)
  require(raw.size() <= INT_MAX, "Archive filename is too long");
  const auto length = static_cast<int>(raw.size());
  const auto wideLength = MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw.data(), length, nullptr, 0);
  require(wideLength > 0, "Invalid CP932 archive name");
  std::wstring wide(wideLength, L'\0');
  require(MultiByteToWideChar(932, MB_ERR_INVALID_CHARS, raw.data(), length, wide.data(), wideLength) == wideLength,
      "Cannot decode CP932 archive name");
  const auto utf8Length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideLength, nullptr, 0, nullptr, nullptr);
  require(utf8Length > 0, "Cannot encode archive filename");
  std::string result(utf8Length, '\0');
  require(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), wideLength, result.data(), utf8Length, nullptr, nullptr) == utf8Length,
      "Cannot encode archive filename");
  return result;
#elif defined(STORAGE_ARCHIVE_ICONV)
  auto converter = iconv_open("UTF-8", "CP932");
  require(converter != reinterpret_cast<iconv_t>(-1), "Cannot decode archive filename");
  std::string result(raw.size() * 4, '\0');
  char *in = raw.data(), *out = result.data(); std::size_t input = raw.size(), output = result.size();
  const auto rc = iconv(converter, &in, &input, &out, &output); iconv_close(converter);
  require(rc != std::size_t(-1) && !input, "Unsupported archive filename encoding");
  result.resize(result.size() - output); return result;
#else
  throw StorageArchiveError(StorageReason::UnsupportedFormat, "This build cannot decode legacy archive filenames");
#endif
}
void safeName(std::string &name, bool directory) {
  require(!name.empty() && name.find('\0') == std::string::npos && name.find('\\') == std::string::npos &&
      name.find(':') == std::string::npos && name.front() != '/', "Unsafe archive filename", StorageReason::UnsafePath);
  if (directory && name.back() == '/') name.pop_back();
  require(!name.empty(), "Empty archive filename", StorageReason::UnsafePath);
  const auto path = fs::u8path(name);
  for (const auto &part : path) require(part != ".." && part != "." && !part.empty(), "Unsafe archive path", StorageReason::UnsafePath);
  require(pathText(path.lexically_normal()) == name, "Non-normalized archive path", StorageReason::UnsafePath);
}
template<class Visitor> void extras(std::string_view data, Visitor visit) {
  for (std::size_t i = 0; i < data.size();) {
    require(data.size() - i >= 4, "Truncated ZIP extra field");
    const auto size = number(data, i + 2, 2);
    require(size <= data.size() - i - 4, "Invalid ZIP extra field");
    visit(number(data, i, 2), data.substr(i + 4, size)); i += size + 4;
  }
}
std::uint64_t octal(std::string_view text) {
  require(!(static_cast<unsigned char>(text[0]) & 128), "Base-256 TAR fields are not supported for in-place updates");
  std::uint64_t value = 0; bool ended = false, started = false;
  for (const auto c : text) {
    if (c == ' ' || c == '\0') { if (started || c == '\0') ended = true; continue; }
    require(!ended && c >= '0' && c <= '7' && value <= (std::uint64_t(INT64_MAX) >> 3), "Invalid TAR numeric field");
    started = true; value = value * 8 + c - '0';
  }
  return value;
}
std::string field(std::string_view data) { return std::string(data.substr(0, data.find('\0'))); }
std::uint64_t rounded(std::uint64_t bytes) { return plus(bytes, 511) / 512 * 512; }
void tarChecksum(std::string &header) {
  header.replace(148, 8, 8, ' ');
  unsigned checksum = 0; for (unsigned char c : header) checksum += c;
  char value[8]; std::snprintf(value, sizeof(value), "%06o", checksum);
  header.replace(148, 6, value, 6); header[154] = '\0'; header[155] = ' ';
}
StorageArchiveEntry row(const Statement &q) {
  return {.id = q.integer(0), .offset = q.integer(1), .span = q.integer(2), .dataOffset = q.integer(3),
      .packed = q.integer(4), .bytes = q.integer(5), .crc = q.integer(6), .method = static_cast<unsigned>(q.integer(7)),
      .flags = static_cast<unsigned>(q.integer(8)), .directory = q.integer(9) != 0,
      .name = q.bytes(10), .rawName = q.bytes(11), .metadata = q.bytes(12)};
}
}
bool FindStorageArchive(const fs::path &virtualPath, fs::path &archive, fs::path &inner,
    const std::vector<fs::path> &accessRoots) {
  archive.clear(); inner.clear();
  if (!virtualPath.is_absolute()) return false;
  for (const auto &component : virtualPath) if (component == "..") return false;
  auto prefix = virtualPath.root_path();
  bool anchored = accessRoots.empty();
  for (auto root : accessRoots) {
    if (!root.is_absolute() || root != root.lexically_normal()) continue;
    while (root != root.root_path() && root.filename().empty()) root = root.parent_path();
    if (IsWithinFolder(virtualPath, root) && (!anchored || IsWithinFolder(prefix, root))) {
      prefix = std::move(root); anchored = true;
    }
  }
  if (!anchored) return false;
  const auto relative = virtualPath.lexically_relative(prefix);
  auto component = relative.begin();
  // A bare Windows drive name or UNC server is not a filesystem directory.
  // Without a grant, start at the first full component below root_path().
  if (accessRoots.empty()) {
    if (component == relative.end() || *component == ".") return false;
    prefix /= *component++;
  }
  for (;;) {
    std::error_code error; const auto status = fs::symlink_status(prefix, error);
    if (!error && fs::is_regular_file(status)) {
      archive = prefix; inner = virtualPath.lexically_relative(prefix);
      if (inner == ".") inner.clear(); return true;
    }
    if (error || (!fs::is_directory(status) && !fs::is_symlink(status))) return false;
    if (component == relative.end()) return false;
    prefix /= *component++;
  }
}
StorageArchive::StorageArchive(const fs::path &path, const fs::path &manifest,
    const std::vector<fs::path> &roots, Checkpoint check, bool resume)
    : identity(identityOf(path, roots)), io(path, roots), db(std::make_unique<Database>(manifest, !resume)) {
  if (resume) {
    Statement q(db->value, "SELECT version,format,comment FROM control WHERE id=1");
    require(q.step() && (q.integer(0) == 1 || q.integer(0) == 2) && q.integer(1) <= 1, "Unknown archive recovery format");
    format_ = static_cast<StorageArchiveFormat>(q.integer(1)); comment = q.bytes(2); return;
  }
  db->exec("CREATE TABLE entries(id INTEGER PRIMARY KEY, offset INTEGER NOT NULL UNIQUE, span INTEGER NOT NULL,"
      " data_offset INTEGER NOT NULL, packed INTEGER NOT NULL, bytes INTEGER NOT NULL, crc INTEGER NOT NULL,"
      " method INTEGER NOT NULL, flags INTEGER NOT NULL, directory INTEGER NOT NULL, name TEXT NOT NULL COLLATE NOCASE UNIQUE,"
      " raw_name BLOB NOT NULL, metadata BLOB NOT NULL, stem TEXT NOT NULL, media INTEGER NOT NULL);"
      "CREATE INDEX entry_stems ON entries(stem, media); BEGIN;");
  try {
    require(identity.bytes >= 4, "Archive is too short");
    const auto signature = io.Read(0, 4);
    if (signature == "PK\003\004" || signature == "PK\005\006" || lower(path.extension().string()) == ".zip") ParseZip(check);
    else {
      require(lower(path.extension().string()) == ".tar", "This archive format does not support bounded in-place updates");
      format_ = StorageArchiveFormat::Tar; ParseTar(check);
    }
    std::uint64_t end = 0;
    Statement q(db->value, "SELECT offset, span FROM entries ORDER BY offset");
    while (q.step()) { active(check); require(q.integer(0) >= end, "Overlapping archive entries"); end = plus(q.integer(0), q.integer(1)); }
    require(identityOf(path, roots) == identity, "Archive changed while indexing", StorageReason::StaleInput);
    db->exec("COMMIT;");
  } catch (...) { db->exec("ROLLBACK;"); throw; }
}
StorageArchive::~StorageArchive() = default;
void StorageArchive::Add(const StorageArchiveEntry &entry, Statement &q) {
  const auto path = fs::u8path(entry.name);
  auto stem = path; stem.replace_extension();
  const auto kind = asobmshow::chart_assets::ClassifyMediaPath(path);
  q.reset();
  q.bind(1, entry.id); q.bind(2, entry.offset); q.bind(3, entry.span); q.bind(4, entry.dataOffset);
  q.bind(5, entry.packed); q.bind(6, entry.bytes); q.bind(7, entry.crc); q.bind(8, entry.method);
  q.bind(9, entry.flags); q.bind(10, entry.directory); q.text(11, entry.name); q.bind(12, entry.rawName);
  q.bind(13, entry.metadata); q.text(14, lower(pathText(stem))); q.bind(15, static_cast<unsigned>(kind));
  q.step();
}
void StorageArchive::ParseZip(Checkpoint check) {
  Statement insert(db->value, "INSERT INTO entries VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  const auto tailSize = std::min<std::uint64_t>(identity.bytes, 65557);
  const auto tail = io.Read(identity.bytes - tailSize, tailSize);
  std::size_t end = std::string::npos;
  if (tail.size() >= 22) for (std::size_t at = tail.size() - 22;; --at) {
    if (number(tail, at, 4) == 0x06054b50 && number(tail, at + 20, 2) == tail.size() - at - 22) { end = at; break; }
    if (!at) break;
  }
  require(end != std::string::npos, "Cannot locate ZIP end record");
  require(number(tail, end + 4, 2) == 0 && number(tail, end + 6, 2) == 0 &&
      number(tail, end + 8, 2) == number(tail, end + 10, 2), "Split ZIP archives cannot be updated in place");
  auto count = number(tail, end + 10, 2), size = number(tail, end + 12, 4), offset = number(tail, end + 16, 4);
  const auto endOffset = identity.bytes - tailSize + end;
  auto centralEnd = endOffset;
  if (count == 0xffff || size == 0xffffffff || offset == 0xffffffff) {
    require(endOffset >= 20, "Missing ZIP64 locator");
    const auto locator = io.Read(endOffset - 20, 20);
    require(number(locator, 0, 4) == 0x07064b50 && number(locator, 4, 4) == 0 && number(locator, 16, 4) == 1, "Unsupported ZIP64 disks");
    centralEnd = number(locator, 8, 8);
    require(plus(centralEnd, 56) <= endOffset - 20, "Invalid ZIP64 end offset");
    const auto record = io.Read(centralEnd, 56);
    require(number(record, 0, 4) == 0x06064b50 && number(record, 4, 8) == 44 &&
        plus(centralEnd, 56) == endOffset - 20 && number(record, 16, 4) == 0 && number(record, 20, 4) == 0 &&
        number(record, 24, 8) == number(record, 32, 8), "Unsupported ZIP64 end record");
    count = number(record, 32, 8); size = number(record, 40, 8); offset = number(record, 48, 8);
  }
  require(plus(offset, size) == centralEnd && count <= size / 46, "Invalid ZIP central directory extent");
  comment = tail.substr(end + 22);
  std::string centralBuffer;
  std::uint64_t bufferOffset = offset;
  auto readCentral = [&](std::uint64_t at, std::uint64_t length) {
    require(plus(at, length) <= centralEnd, "Truncated ZIP central directory");
    if (at < bufferOffset || plus(at, length) > plus(bufferOffset, centralBuffer.size())) {
      bufferOffset = at;
      centralBuffer = io.Read(at, std::min<std::uint64_t>(256 * 1024, centralEnd - at));
    }
    return centralBuffer.substr(at - bufferOffset, length);
  };
  auto cursor = offset;
  for (std::uint64_t index = 1; index <= count; ++index) {
    active(check);
    require(plus(cursor, 46) <= centralEnd, "Truncated ZIP central directory");
    auto central = readCentral(cursor, 46);
    require(number(central, 0, 4) == 0x02014b50, "Invalid ZIP central record");
    const auto nameLength = number(central, 28, 2), extraLength = number(central, 30, 2), commentLength = number(central, 32, 2);
    const auto length = 46 + nameLength + extraLength + commentLength;
    require(plus(cursor, length) <= centralEnd, "Truncated ZIP entry metadata");
    central += readCentral(cursor + 46, length - 46); cursor += length;
    StorageArchiveEntry entry{.id = index, .offset = number(central, 42, 4), .packed = number(central, 20, 4),
        .bytes = number(central, 24, 4), .crc = number(central, 16, 4), .method = static_cast<unsigned>(number(central, 10, 2)),
        .flags = static_cast<unsigned>(number(central, 8, 2)), .rawName = central.substr(46, nameLength), .metadata = central};
    require(!(entry.flags & ~0x080eU) && (entry.method == 0 || entry.method == 8), "Encrypted or unsupported ZIP compression");
    require(number(central, 34, 2) == 0, "Split ZIP entry");
    const auto mode = number(central, 38, 4) >> 16;
    require(!(mode & 0170000) || (mode & 0170000) == 0100000 || (mode & 0170000) == 0040000, "ZIP links or special entries cannot be updated", StorageReason::UnsafePath);
    bool zip64 = false;
    entry.name = decoded(entry.rawName, entry.flags & 0x800);
    extras(std::string_view(central).substr(46 + nameLength, extraLength), [&](auto tag, auto data) {
      if (tag == 1) {
        require(!zip64, "Duplicate ZIP64 extra field"); zip64 = true; std::size_t pos = 0;
        for (auto *value : {&entry.bytes, &entry.packed, &entry.offset}) if (*value == 0xffffffff) { *value = number(data, pos, 8); pos += 8; }
      }
      if (tag == 0x7075 && data.size() >= 5 && data[0] == 1 && number(data, 1, 4) == crc32(0, reinterpret_cast<const Bytef *>(entry.rawName.data()), entry.rawName.size())) {
        const std::string unicode(data.substr(5));
        require(asobmashow::text::validUtf8(unicode) && unicode == entry.name, "Ambiguous Unicode ZIP filename");
      }
    });
    require(zip64 || (entry.bytes != 0xffffffff && entry.packed != 0xffffffff && entry.offset != 0xffffffff), "Missing ZIP64 entry values");
    entry.directory = !entry.rawName.empty() && entry.rawName.back() == '/'; safeName(entry.name, entry.directory);
    require(!entry.directory || !entry.bytes, "Nonempty ZIP directory");
    require(entry.method != 0 || entry.bytes == entry.packed, "Invalid stored ZIP sizes");
    require(plus(entry.offset, 30) <= offset, "ZIP local header outside data area");
    const auto local = io.Read(entry.offset, 30);
    require(number(local, 0, 4) == 0x04034b50 && number(local, 6, 2) == entry.flags && number(local, 8, 2) == entry.method,
        "Inconsistent ZIP local header");
    const auto localName = number(local, 26, 2), localExtra = number(local, 28, 2);
    entry.dataOffset = plus(entry.offset, 30 + localName + localExtra);
    require(plus(entry.dataOffset, entry.packed) <= offset && localName == entry.rawName.size() &&
        io.Read(entry.offset + 30, localName) == entry.rawName, "Inconsistent ZIP local filename or extent");
    auto localBytes = number(local, 22, 4), localPacked = number(local, 18, 4);
    bool local64 = false;
    extras(io.Read(entry.offset + 30 + localName, localExtra), [&](auto tag, auto data) {
      if (tag == 1) {
        require(!local64, "Duplicate local ZIP64 field"); local64 = true; std::size_t pos = 0;
        for (auto *value : {&localBytes, &localPacked}) if (*value == 0xffffffff) { *value = number(data, pos, 8); pos += 8; }
      }
    });
    auto finish = plus(entry.dataOffset, entry.packed);
    if (entry.flags & 8) {
      const bool wide = local64 || entry.bytes >= 0xffffffff || entry.packed >= 0xffffffff;
      require(plus(finish, 4) <= offset, "Missing ZIP data descriptor");
      const auto marker = io.Read(finish, 4);
      const auto prefix = number(marker, 0, 4) == 0x08074b50 ? 4U : 0U;
      const auto descriptorSize = prefix + 4 + (wide ? 16 : 8);
      require(plus(finish, descriptorSize) <= offset, "Truncated ZIP data descriptor");
      const auto descriptor = io.Read(finish, descriptorSize);
      require(number(descriptor, prefix, 4) == entry.crc && number(descriptor, prefix + 4, wide ? 8 : 4) == entry.packed &&
          number(descriptor, prefix + 4 + (wide ? 8 : 4), wide ? 8 : 4) == entry.bytes, "Inconsistent ZIP data descriptor");
      finish += descriptorSize;
    } else require(localBytes == entry.bytes && localPacked == entry.packed && number(local, 14, 4) == entry.crc,
        "Inconsistent ZIP local sizes or checksum");
    entry.span = finish - entry.offset; Add(entry, insert);
  }
  require(cursor == centralEnd, "Unsupported trailing ZIP central metadata");
}
void StorageArchive::ParseTar(Checkpoint check) {
  Statement insert(db->value, "INSERT INTO entries VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
  require(identity.bytes % 512 == 0, "TAR size is not block aligned");
  std::uint64_t offset = 0, id = 0;
  while (plus(offset, 512) <= identity.bytes) {
    active(check); const auto header = io.Read(offset, 512);
    if (header == std::string(512, '\0')) {
      require(plus(offset, 1024) <= identity.bytes, "Missing TAR end blocks");
      while (offset < identity.bytes) {
        active(check); const auto amount = std::min<std::uint64_t>(512 * 1024, identity.bytes - offset);
        const auto tail = io.Read(offset, amount);
        require(tail.find_first_not_of('\0') == std::string::npos, "Concatenated TAR archives are unsupported"); offset += amount;
      }
      return;
    }
    require(header.substr(257, 6) == std::string("ustar\0", 6), "Only uncompressed USTAR entries support in-place updates");
    auto checked = header; checked.replace(148, 8, 8, ' ');
    std::uint64_t checksum = 0; for (unsigned char c : checked) checksum += c;
    require(checksum == octal(std::string_view(header).substr(148, 8)), "Invalid TAR header checksum");
    require(header[156] == '0' || header[156] == '\0' || header[156] == '5', "TAR links, sparse, GNU and PAX records cannot be updated in place");
    StorageArchiveEntry entry{.id = ++id, .offset = offset, .dataOffset = offset + 512,
        .bytes = octal(std::string_view(header).substr(124, 12)), .directory = header[156] == '5', .metadata = header};
    entry.packed = entry.bytes; entry.span = plus(512, rounded(entry.bytes));
    entry.rawName = field(std::string_view(header).substr(0, 100));
    const auto prefix = field(std::string_view(header).substr(345, 155));
    if (!prefix.empty()) entry.rawName = prefix + "/" + entry.rawName;
    entry.name = decoded(entry.rawName, false); safeName(entry.name, entry.directory);
    require(!entry.directory || !entry.bytes, "Nonempty TAR directory");
    offset = plus(offset, entry.span); require(offset <= identity.bytes, "Truncated TAR payload"); Add(entry, insert);
  }
  throw StorageArchiveError(StorageReason::UnsupportedFormat, "Missing TAR end record");
}
std::optional<StorageArchiveEntry> StorageArchive::Next(std::optional<std::uint64_t> after) const {
  Statement q(db->value, "SELECT id,offset,span,data_offset,packed,bytes,crc,method,flags,directory,name,raw_name,metadata FROM entries WHERE offset > ? ORDER BY offset LIMIT 1");
  if (after) q.bind(1, *after); else sqlite3_bind_int64(q.value, 1, -1);
  if (!q.step()) return {}; return row(q);
}
std::optional<AssetCandidate> StorageArchive::Candidate(const StorageArchiveEntry &entry, const StoragePolicy &policy, StorageDiagnostic &reason) const {
  Statement collisions(db->value, candidateCollisionSql);
  return archiveCandidate(entry, policy, reason, collisions);
}
std::optional<AssetCandidate> archive_detail::archiveCandidate(const StorageArchiveEntry &entry,
    const StoragePolicy &policy, StorageDiagnostic &reason, Statement &q) {
  reason = {}; if (entry.directory || !IsValid(policy)) return {};
  const auto path = fs::u8path(entry.name), extension = path.extension();
  if (archive_file::isSystemEntry(path)) return {};
  const auto kind = asobmshow::chart_assets::ClassifyMediaPath(path);
  const auto ext = asobmshow::chart_assets::LowerMediaExtension(path);
  AssetCandidate candidate{.source = path, .destination = path, .kind = kind, .identity = {.bytes = entry.bytes}};
  if (ext == "wav" && policy.audio == AudioAction::LosslessFlac) { candidate.operation = AssetOperation::LosslessFlac; candidate.destination.replace_extension("flac"); }
  else if ((ext == "wav" || ext == "flac") && policy.audio == AudioAction::LossyVorbis) { candidate.operation = AssetOperation::LossyVorbis; candidate.destination.replace_extension("ogg"); }
  else if (kind == asobmshow::chart_assets::MediaKind::Video && policy.bga != BgaAction::Keep) {
    candidate.operation = policy.bga == BgaAction::Delete ? AssetOperation::Delete : AssetOperation::CompressH264;
    if (candidate.operation != AssetOperation::Delete) candidate.destination.replace_extension("mp4");
  } else return {};
  if (candidate.operation != AssetOperation::Delete) {
    auto stem = path; stem.replace_extension();
    q.reset();
    q.text(1, lower(pathText(stem))); q.bind(2, static_cast<unsigned>(kind)); q.text(3, pathText(candidate.destination)); q.bind(4, entry.id); q.step();
    if (q.integer(0) > 1) { reason = {StorageReason::Collision, "Same-stem archive media alternatives are ambiguous"}; return {}; }
  }
  return candidate;
}
void StorageArchive::Extract(const StorageArchiveEntry &entry, const fs::path &output, Checkpoint check) const {
#ifdef _WIN32
  auto *raw = _wfopen(output.c_str(), L"wbx");
#else
  auto *raw = std::fopen(output.c_str(), "wbx");
#endif
  require(raw, "Cannot create archive media staging file", StorageReason::IoError);
  const std::unique_ptr<FILE, decltype(&std::fclose)> file(raw, &std::fclose);
  std::error_code error; fs::permissions(output, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, error);
  require(!error, "Cannot protect archive media staging file", StorageReason::IoError);
  std::array<char, 128 * 1024> decodedBytes{};
  z_stream stream{};
  struct Inflater { z_stream *stream; ~Inflater() { if (stream) inflateEnd(stream); } } inflater{nullptr};
  if (entry.method == 8) { require(inflateInit2(&stream, -MAX_WBITS) == Z_OK, "Cannot initialize ZIP decoder"); inflater.stream = &stream; }
  std::uint64_t position = 0, produced = 0; auto crc = crc32(0, nullptr, 0); bool ended = false;
  auto write = [&](std::string_view bytes) {
    require(bytes.size() <= entry.bytes - produced, "ZIP output exceeds declared size");
    require(std::fwrite(bytes.data(), 1, bytes.size(), raw) == bytes.size(), "Cannot stage archive payload", StorageReason::IoError);
    produced += bytes.size(); crc = crc32(crc, reinterpret_cast<const Bytef *>(bytes.data()), bytes.size());
  };
  while (position < entry.packed) {
    active(check); const auto amount = std::min<std::uint64_t>(decodedBytes.size(), entry.packed - position);
    auto input = io.Read(entry.dataOffset + position, amount); position += amount;
    if (!inflater.stream) write(input);
    else {
      stream.next_in = reinterpret_cast<Bytef *>(input.data()); stream.avail_in = input.size();
      do {
        active(check); stream.next_out = reinterpret_cast<Bytef *>(decodedBytes.data()); stream.avail_out = decodedBytes.size();
        const auto rc = inflate(&stream, Z_NO_FLUSH);
        require(rc == Z_OK || rc == Z_STREAM_END, "Invalid deflated ZIP payload");
        write(std::string_view(decodedBytes.data(), decodedBytes.size() - stream.avail_out));
        if (rc == Z_STREAM_END) { ended = true; require(!stream.avail_in && position == entry.packed, "Trailing deflated ZIP payload"); break; }
      } while (stream.avail_in || !stream.avail_out);
    }
  }
  require(produced == entry.bytes && (!inflater.stream || ended) && (format_ != StorageArchiveFormat::Zip || crc == entry.crc), "Archive payload checksum or size mismatch");
  require(std::fflush(raw) == 0, "Cannot flush staged archive payload", StorageReason::IoError);
}
std::string StorageArchive::Central(const StorageArchiveEntry &entry, std::uint64_t offset) const {
  require(format_ == StorageArchiveFormat::Zip, "Central directory requested for non-ZIP archive");
  auto header = entry.metadata.substr(0, 46);
  const auto oldName = number(entry.metadata, 28, 2), oldExtra = number(entry.metadata, 30, 2);
  std::string extra;
  extras(std::string_view(entry.metadata).substr(46 + oldName, oldExtra), [&](auto tag, auto data) {
    if (tag == 1) return;
    std::string field(4, '\0'); put(field, 0, tag, 2); put(field, 2, data.size(), 2); extra += field; extra += data;
  });
  std::string values;
  for (const auto value : {entry.bytes, entry.packed, offset}) if (value >= 0xffffffff) { const auto at = values.size(); values.resize(at + 8); put(values, at, value, 8); }
  if (!values.empty()) { std::string field(4, '\0'); put(field, 0, 1, 2); put(field, 2, values.size(), 2); extra += field + values; put(header, 6, 45, 2); }
  require(extra.size() <= 65535 && entry.rawName.size() <= 65535, "ZIP metadata is too long");
  put(header, 8, entry.flags, 2); put(header, 10, entry.method, 2); put(header, 16, entry.crc, 4);
  put(header, 20, std::min<std::uint64_t>(entry.packed, 0xffffffff), 4); put(header, 24, std::min<std::uint64_t>(entry.bytes, 0xffffffff), 4);
  put(header, 28, entry.rawName.size(), 2); put(header, 30, extra.size(), 2); put(header, 42, std::min<std::uint64_t>(offset, 0xffffffff), 4);
  return header + entry.rawName + extra + entry.metadata.substr(46 + oldName + oldExtra);
}
std::string StorageArchive::Replacement(StorageArchiveEntry &entry, std::string_view extension, std::uint64_t bytes, std::uint32_t crc) const {
  const auto dot = entry.rawName.find_last_of('.');
  require(dot != std::string::npos && entry.rawName.find('/', dot) == std::string::npos, "Missing archive media extension");
  entry.rawName.replace(dot, std::string::npos, extension);
  auto name = fs::u8path(entry.name); name.replace_extension(extension); entry.name = pathText(name);
  entry.bytes = entry.packed = bytes; entry.crc = crc; entry.method = 0; entry.flags &= 0x800;
  if (format_ == StorageArchiveFormat::Tar) {
    require(bytes < (std::uint64_t(1) << 33), "TAR replacement exceeds octal size limit");
    auto header = entry.metadata;
    header.replace(0, 100, 100, '\0'); header.replace(345, 155, 155, '\0');
    if (entry.rawName.size() <= 100) header.replace(0, entry.rawName.size(), entry.rawName);
    else {
      auto split = entry.rawName.rfind('/', 155);
      require(split != std::string::npos && entry.rawName.size() - split - 1 <= 100, "TAR replacement filename is too long");
      header.replace(345, split, entry.rawName.substr(0, split)); header.replace(0, entry.rawName.size() - split - 1, entry.rawName.substr(split + 1));
    }
    char size[12]; std::snprintf(size, sizeof(size), "%011llo", static_cast<unsigned long long>(bytes)); header.replace(124, 12, size, 12);
    tarChecksum(header); entry.metadata = header; entry.span = plus(512, rounded(bytes)); return header;
  }
  const auto oldName = number(entry.metadata, 28, 2), oldExtra = number(entry.metadata, 30, 2);
  std::string extra;
  extras(std::string_view(entry.metadata).substr(46 + oldName, oldExtra), [&](auto tag, auto data) {
    if (tag == 1 || tag == 0x7075) return;
    std::string field(4, '\0'); put(field, 0, tag, 2); put(field, 2, data.size(), 2); extra += field; extra += data;
  });
  auto central = entry.metadata.substr(0, 46);
  put(central, 28, entry.rawName.size(), 2); put(central, 30, extra.size(), 2);
  entry.metadata = central + entry.rawName + extra + entry.metadata.substr(46 + oldName + oldExtra);
  // Replacement media is already compressed, so ZIP uses a stored record.
  require(bytes < 0xffffffff, "Individual ZIP replacement exceeds staging limit");
  std::string local(30, '\0'); put(local, 0, 0x04034b50, 4); put(local, 4, 20, 2); put(local, 6, entry.flags, 2);
  local.replace(10, 4, central.substr(12, 4)); put(local, 14, crc, 4); put(local, 18, bytes, 4); put(local, 22, bytes, 4); put(local, 26, entry.rawName.size(), 2);
  local += entry.rawName; entry.span = plus(local.size(), bytes); return local;
}
}
