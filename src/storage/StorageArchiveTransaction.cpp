#include "StorageArchiveTransaction.h"
#include "StorageReportDetail.h"
#include "StorageArchiveDb.h"
#include "StoragePathIndex.h"
#include "StorageWorkScheduler.h"
#include "../AtomicFile.h"
#include "../ArchiveSourceIdentity.h"
#include "../FileChecksum.h"
#include "../Uuid.h"
#include "../utils/ParallelWork.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <chrono>
#include <exception>
#include <map>
#include <set>
#include <zlib.h>

namespace storage_management {
namespace fs = std::filesystem;
using namespace archive_detail;
namespace {
constexpr std::uint64_t chunkSize = 512 * 1024, stagingReserve = 8 * 1024 * 1024;
constexpr std::uint64_t batchBytes = 8 * 1024 * 1024;
constexpr std::size_t batchRecords = 1024;
class Timer {
  using Clock = std::chrono::steady_clock;
  std::uint64_t *elapsed;
  Clock::time_point start;
public:
  explicit Timer(std::uint64_t *elapsed) : elapsed(elapsed), start(elapsed ? Clock::now() : Clock::time_point{}) {}
  ~Timer() { Stop(); }
  void Stop() {
    if (!elapsed) return;
    *elapsed += std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
    elapsed = nullptr;
  }
};
std::string pathText(const fs::path &path) { const auto text = path.generic_u8string(); return {text.begin(), text.end()}; }
void syncDirectory(const fs::path &path) {
  std::string error; if (!atomic_file::syncDirectory(path, error)) throw StorageArchiveError(StorageReason::IoError, error);
}
void syncFile(const fs::path &path) {
  std::string error; if (!atomic_file::syncFile(path, error)) throw StorageArchiveError(StorageReason::IoError, error);
}
struct SqlTransaction {
  Database &db; bool committed = false;
  explicit SqlTransaction(Database &db) : db(db) { db.exec("BEGIN IMMEDIATE"); }
  ~SqlTransaction() { if (!committed) sqlite3_exec(db.value, "ROLLBACK", nullptr, nullptr, nullptr); }
  void Commit() { db.exec("COMMIT"); committed = true; }
};
struct Output {
  FILE *file = nullptr;
  explicit Output(const fs::path &path) {
#ifdef _WIN32
    file = _wfopen(path.c_str(), L"wbx");
#else
    file = std::fopen(path.c_str(), "wbx");
#endif
    require(file, "Cannot create archive staging file", StorageReason::IoError);
  }
  ~Output() { if (file) std::fclose(file); }
  void Write(std::string_view bytes) { require(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size(), "Cannot write archive staging file", StorageReason::IoError); }
  void Close() { auto *value = file; file = nullptr; require(std::fclose(value) == 0, "Cannot flush archive staging file", StorageReason::IoError); }
};
bool speculativeFile(const fs::path &path) {
  const auto stem = path.stem().string();
  const auto prefix = stem.starts_with("input-") ? 6u : stem.starts_with("output-") ? 7u : 0u;
  if (!prefix || stem.size() <= prefix || stem.size() > prefix + 20 || stem[prefix] < '1' || stem[prefix] > '9' ||
      !std::all_of(stem.begin() + prefix, stem.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
  return prefix == 6 ? asobmshow::chart_assets::ClassifyMediaPath(path) != asobmshow::chart_assets::MediaKind::Other :
      (path.extension() == ".flac" || path.extension() == ".ogg" || path.extension() == ".mp4");
}
bool ownedFile(const std::string &name) {
  if (name == "manifest.sqlite" || name == "manifest.sqlite-journal" || name == "replacement.bin" || name == "redo.bin") return true;
  const auto path = fs::path(name);
  if (speculativeFile(path)) return true;
  if (path.stem() == "input") return asobmshow::chart_assets::ClassifyMediaPath(path) != asobmshow::chart_assets::MediaKind::Other;
  return path.stem() == "output" && (path.extension() == ".flac" || path.extension() == ".ogg" || path.extension() == ".mp4");
}
void cleanup(fs::path journal, const StorageArchiveDependencies &deps = {}) {
  for (const auto &entry : fs::directory_iterator(journal))
    require(entry.is_regular_file() && !entry.is_symlink() && ownedFile(entry.path().filename().string()),
        "Unexpected file in archive recovery directory", StorageReason::RecoveryRequired);
  const auto name = journal.filename().string();
  if (!name.starts_with("archive-preparing-") && !name.starts_with("archive-cleanup-")) {
    const auto retired = journal.parent_path() / ("archive-cleanup-" + name.substr(8));
    fs::rename(journal, retired); journal = retired;
  }
  // Persist the cleanup-only namespace before unlinking the manifest. A crash
  // during deletion must never leave a ready journal without recovery evidence.
  syncDirectory(journal.parent_path());
  if (deps.faultPoint) deps.faultPoint("archive-cleanup-ready");
  for (const auto &entry : fs::directory_iterator(journal)) {
    fs::remove(entry.path());
    if (entry.path().filename() == "manifest.sqlite" && deps.faultPoint) deps.faultPoint("archive-manifest-unlinked");
  }
  if (deps.faultPoint) deps.faultPoint("archive-cleanup-remove");
  fs::remove(journal); syncDirectory(journal.parent_path());
}
void authority(const fs::path &path, const StoragePlatformPaths &paths) {
  require(path.is_absolute() && !StoragePathIndex(paths.allowedRoots).Ancestors(path).empty(), "Archive is outside authorized library roots", StorageReason::UnsafePath);
  require(!IsWithinFolder(path, paths.journalRoot) && !IsWithinFolder(path, paths.temporaryRoot), "Private recovery files cannot be optimized", StorageReason::UnsafePath);
  std::string error; require(IsSafeLocalPath(path, error, paths.pathAccessRoots), "Unsafe archive path", StorageReason::UnsafePath);
}
void append(StorageReport &report, FileResult result) {
  if (result.outcome == FileOutcome::Failed) ++report.failed;
  else if (result.outcome == FileOutcome::Skipped) ++report.skipped;
  else if (result.outcome == FileOutcome::Cancelled) report.cancelled = true;
  RetainStorageReportDetail(report, std::move(result));
}
struct Chunk {
  std::uint64_t id = 0, source = 0, length = 0, kind = 0, destination = 0;
  std::string hash, data;
  std::uint64_t entry = 0, part = 0;
};
class Update {
public:
  StorageArchive &archive;
  Database &db;
  fs::path journal;
  const StorageArchiveDependencies &deps;
  const std::vector<fs::path> &accessRoots;
  StorageReport report;
  StorageProgress progress;
  bool &started;
  const std::string originalSourceIdentity;
  bool recovering = false;
  struct PendingRecord {
    std::uint64_t id, output, cursor = 0;
    unsigned complete = 0;
    bool deleted = false;
  };
  std::vector<Chunk> pendingChunks;
  std::vector<PendingRecord> pendingRecords;
  std::uint64_t pendingBytes = 0;
  std::uint64_t pendingSourceEnd = 0;
  bool pendingDeletes = false;
  std::optional<std::uint64_t> copyCursor;
  ProgressCallback copyProgress;
  mutable std::map<std::string, std::unique_ptr<Statement>> statements;
  struct PendingEncoding {
    std::optional<AssetCandidate> staged;
    fs::path input, output;
    std::uint64_t inputBytes = 0, reservedBytes = 0, outputLimit = 0;
    std::atomic<bool> tooLarge = false;
    StoragePhase phase = StoragePhase::Encode;
    EncodeResult result;
    std::shared_ptr<StorageWorkBudget::Lease> reservation;
    // Destroy first: the encoder must stop touching paths and progress state.
    std::unique_ptr<StorageEncodingJob> job;
  };
  StorageWorkBudget fallbackBudget;
  StorageWorkBudget *workBudget;
  std::mutex encodingMutex;
  std::uint64_t pendingScratch = 0;
  std::optional<std::uint64_t> encodingCursor;
  bool encodingExhausted = false;
  std::atomic<bool> stopEncoding = false;
  std::unique_ptr<StoragePathIndex> encodingSelection;
  std::map<std::uint64_t, std::unique_ptr<PendingEncoding>> encodings;
  Update(StorageArchive &archive, fs::path journal, const StorageArchiveDependencies &deps, bool &started,
      const std::vector<fs::path> &accessRoots)
      : archive(archive), db(*archive.db), journal(std::move(journal)), deps(deps), accessRoots(accessRoots), started(started),
        originalSourceIdentity(deps.measurementBaselines.empty() ? std::string{} : archive_source_identity::KeyForPath(archive.io.path)),
        fallbackBudget(deps.executionLimits, [this] {
          return this->deps.availableSpace ? this->deps.availableSpace(this->journal) : std::optional<std::uint64_t>(fs::space(this->journal).available);
        }), workBudget(deps.workBudget ? deps.workBudget : &fallbackBudget) {
    archive.io.CheckPath();
    progress.archiveBytesTotal = archive.identity.bytes;
  }
  ~Update() {
    // Member destruction joins the futures. Publish an unresolved mutation
    // first so those encoders and other owners stop without waiting for us.
    if (std::uncaught_exceptions() && started) workBudget->StopForRecovery();
    stopEncoding = true;
  }
  Statement &Query(const std::string &sql) const {
    auto &query = statements[sql];
    if (!query) query = std::make_unique<Statement>(db.value, sql.c_str());
    query->reset(); return *query;
  }
  std::uint64_t Value(const char *column) const {
    const auto sql = std::string("SELECT ") + column + " FROM control WHERE id=1";
    auto &q = Query(sql); require(q.step(), "Missing archive recovery state");
    const auto value = q.integer(0); q.reset(); return value;
  }
  void Set(const char *column, std::uint64_t value) {
    const auto sql = std::string("UPDATE control SET ") + column + "=? WHERE id=1";
    auto &q = Query(sql); q.bind(1, value); q.step(); q.reset();
  }
  void CaptureSourceStamp() {
    archive.io.CheckPath();
    Statement q(db.value, "UPDATE control SET source_stamp=? WHERE id=1");
    q.text(1, archive.io.ChangeToken()); q.step();
    if (Value("version") >= 3) Set("source_size", archive.io.Identity().bytes);
  }
  void Fault(std::string_view name) { if (deps.faultPoint) deps.faultPoint(name); }
  std::string Hash(std::string_view bytes) const {
    Timer timer(deps.metrics ? &deps.metrics->hashNanoseconds : nullptr);
    return file_checksum::sha256(bytes);
  }
  void InsertChunk(std::uint64_t entry, std::uint64_t part, std::uint64_t source, std::string_view bytes, std::uint64_t kind, std::uint64_t destination = 0) {
    Statement q(db.value, "INSERT INTO chunks(entry_id,part,source,length,hash,kind,destination,done,data) VALUES(?,?,?,?,?,?,?,0,?)");
    q.bind(1, entry); q.bind(2, part); q.bind(3, source); q.bind(4, bytes.size()); q.text(5, Hash(bytes));
    q.bind(6, kind); q.bind(7, destination); q.bind(8, kind == 2 ? bytes : std::string_view{}); q.step();
  }
  void Initialize(Checkpoint check) {
    db.exec("CREATE TABLE control(id INTEGER PRIMARY KEY CHECK(id=1),version INTEGER,path TEXT,device INTEGER,file_id INTEGER,"
        "original_size INTEGER,format INTEGER,comment BLOB,ready INTEGER DEFAULT 0,started INTEGER DEFAULT 0,"
        "phase INTEGER DEFAULT 0,cursor INTEGER DEFAULT 0,stage_entry INTEGER DEFAULT 0,final_size INTEGER DEFAULT 0,source_stamp TEXT,working_size INTEGER,source_size INTEGER);"
        "CREATE TABLE chunks(id INTEGER PRIMARY KEY,entry_id INTEGER,part INTEGER,source INTEGER,length INTEGER,hash TEXT,"
        "kind INTEGER,destination INTEGER,done INTEGER,data BLOB,UNIQUE(entry_id,part));"
        "CREATE INDEX chunks_pending ON chunks(entry_id,done,part);"
        "CREATE TABLE redo(id INTEGER PRIMARY KEY,chunk INTEGER UNIQUE,target INTEGER,before_hash TEXT,scratch_offset INTEGER);"
        "ALTER TABLE entries ADD COLUMN action INTEGER DEFAULT 0;"
        "ALTER TABLE entries ADD COLUMN complete INTEGER DEFAULT 0;"
        "ALTER TABLE entries ADD COLUMN output_offset INTEGER DEFAULT 0;"
        "ALTER TABLE entries ADD COLUMN original_bytes INTEGER;"
        "ALTER TABLE entries ADD COLUMN original_span INTEGER;"
        "ALTER TABLE entries ADD COLUMN original_name TEXT;"
        "ALTER TABLE entries ADD COLUMN relocated_offset INTEGER DEFAULT 0;"
        "CREATE INDEX entries_relocated ON entries(offset) WHERE action=3;"
        "UPDATE entries SET original_bytes=bytes,original_span=span,original_name=name;");
    SqlTransaction transaction(db);
    Statement state(db.value, "INSERT INTO control(id,version,path,device,file_id,original_size,format,comment) VALUES(1,3,?,?,?,?,?,?)");
    state.text(1, pathText(archive.io.path)); state.bind(2, archive.identity.device); state.bind(3, archive.identity.fileId);
    state.bind(4, archive.identity.bytes); state.bind(5, static_cast<unsigned>(archive.Format())); state.bind(6, archive.comment); state.step();
    Set("working_size", archive.identity.bytes);
    // Preserve executable stubs and other leading ZIP bytes in place, including
    // sparse prefixes, without scanning or staging that range.
    Statement first(db.value, "SELECT offset FROM entries ORDER BY offset LIMIT 1");
    if (first.step()) Set("cursor", first.integer(0));
    // Original payloads are read only when their bounded copy batch is needed.
    // The pinned source change token guards the untouched ranges between batches.
    active(check); archive.io.CheckPath(); CaptureSourceStamp();
    Set("ready", 1); transaction.Commit(); syncFile(journal / "manifest.sqlite"); syncDirectory(journal); syncDirectory(journal.parent_path());
    if (deps.metrics) {
      deps.metrics->preparationBytesRead += archive.io.BytesRead();
      deps.metrics->preparationReadCalls += archive.io.ReadCalls();
      deps.metrics->preparationBackwardReads += archive.io.BackwardReads();
    }
    Fault("archive-ready");
  }
  std::optional<Chunk> NextChunk(std::uint64_t entry, std::uint64_t after = 0) {
    Statement q(db.value, "SELECT id,source,length,kind,destination,hash,data FROM chunks WHERE entry_id=? AND done=0 AND id>? ORDER BY part LIMIT 1");
    q.bind(1, entry); q.bind(2, after); if (!q.step()) return {};
    Chunk chunk{q.integer(0), q.integer(1), q.integer(2), q.integer(3), q.integer(4), q.bytes(5), q.bytes(6)};
    require(chunk.length && chunk.length <= chunkSize && chunk.kind <= 2 && chunk.hash.size() == 64, "Invalid archive recovery chunk"); return chunk;
  }
  std::string ReadChunkBytes(const Chunk &chunk, std::optional<std::uint64_t> capacity = {}) {
    Timer timer(deps.metrics ? &deps.metrics->readNanoseconds : nullptr);
    std::string bytes;
    if (chunk.kind == 0) {
      require(plus(chunk.source, chunk.length) <= (capacity ? *capacity : Capacity()), "Invalid original archive extent"); bytes = archive.io.Read(chunk.source, chunk.length);
    } else if (chunk.kind == 1) {
      StorageArchiveIO stage(journal / "replacement.bin", accessRoots, false);
      require(plus(chunk.source, chunk.length) <= stage.Identity().bytes, "Truncated archive replacement"); bytes = stage.Read(chunk.source, chunk.length);
    } else bytes = chunk.data;
    require(bytes.size() == chunk.length, "Archive or prepared media changed", StorageReason::StaleInput); return bytes;
  }
  std::string ReadChunk(const Chunk &chunk) {
    auto bytes = ReadChunkBytes(chunk);
    require((chunk.id == 0 && chunk.kind == 0 && chunk.hash.empty()) || Hash(bytes) == chunk.hash,
        "Archive or prepared media changed", StorageReason::StaleInput); return bytes;
  }
  void HashPayloads(const std::vector<std::string> &payloads) {
    Timer timer(deps.metrics ? &deps.metrics->hashNanoseconds : nullptr);
    const auto hash = [&](std::size_t i) {
      auto &chunk = pendingChunks[i];
      auto value = file_checksum::sha256(payloads[i]);
      if (chunk.id == 0 && chunk.kind == 0 && chunk.hash.empty()) chunk.hash = std::move(value);
      else require(value == chunk.hash, "Archive or prepared media changed", StorageReason::StaleInput);
    };
    const auto workers = pendingBytes >= 1024 * 1024 && pendingChunks.size() >= 4 ?
        std::min(4u, parallel_worker_count(pendingChunks.size())) : 1u;
    if (workers == 1) {
      for (std::size_t i = 0; i < pendingChunks.size(); ++i) hash(i);
      return;
    }
    // Workers touch disjoint hashes and already-read buffers only. Join every
    // worker and propagate failures before capacity changes, redo or writes.
    std::array<std::exception_ptr, 4> failures{};
    parallel_for_each_index(workers, [&](std::size_t worker) {
      try {
        for (auto i = worker; i < pendingChunks.size(); i += workers) hash(i);
      } catch (...) { failures[worker] = std::current_exception(); }
    });
    for (const auto &failure : failures) if (failure) std::rethrow_exception(failure);
    if (deps.metrics) {
      ++deps.metrics->parallelHashBatches;
      deps.metrics->peakHashWorkers = std::max<std::uint64_t>(deps.metrics->peakHashWorkers, workers);
    }
  }
  void Done(std::uint64_t id) {
    SqlTransaction transaction(db); Statement q(db.value, "UPDATE chunks SET done=1 WHERE id=?"); q.bind(1, id); q.step();
    db.exec("DELETE FROM redo");
    if (Value("version") >= 2) CaptureSourceStamp();
    transaction.Commit(); Fault("archive-cursor");
  }
  void WriteChunk(const Chunk &chunk) {
    if (pendingBytes + chunk.length > batchBytes || pendingChunks.size() == batchRecords) Flush();
    pendingChunks.push_back(chunk); pendingBytes += chunk.length;
  }
  void ValidateLegacyRecovery() {
    const auto identity = archive.io.Identity();
    require(identity.device == Value("device") && identity.fileId == Value("file_id") &&
        (identity.bytes == Value("original_size") || (Value("phase") >= 2 && identity.bytes == Value("final_size"))),
        "Archive identity or size changed since the interrupted update", StorageReason::StaleInput);
    Statement redo(db.value, "SELECT chunk,target,before,after FROM redo WHERE id=1");
    std::uint64_t pending = 0, target = 0; std::string before, after;
    if (redo.step()) {
      pending = redo.integer(0); target = redo.integer(1); before = redo.bytes(2); after = redo.bytes(3);
      require(started && !after.empty() && after.size() <= chunkSize && before.size() == after.size() && plus(target, after.size()) <= identity.bytes, "Invalid archive redo record");
      Statement q(db.value, "SELECT destination,length,hash,done FROM chunks WHERE id=?"); q.bind(1, pending);
      require(q.step() && q.integer(0) == target && q.integer(1) == after.size() && !q.integer(3) && q.bytes(2) == Hash(after), "Archive redo evidence is inconsistent");
      const auto current = archive.io.Read(target, after.size());
      for (std::size_t i = 0; i < current.size(); ++i) require(current[i] == before[i] || current[i] == after[i], "Archive changed outside the interrupted write", StorageReason::StaleInput);
    }
    // An overlapping write owns its saved source chunk; all other source and
    // output ranges must still match before any recovery mutation is allowed.
    Statement chunks(db.value, "SELECT id,source,length,kind,destination,hash,data,done FROM chunks ORDER BY id");
    while (chunks.step()) {
      Chunk chunk{chunks.integer(0), chunks.integer(1), chunks.integer(2), chunks.integer(3), chunks.integer(4), chunks.bytes(5), chunks.bytes(6)};
      require(chunk.length && chunk.length <= chunkSize && chunk.kind <= 2, "Invalid archive recovery chunk");
      if (chunk.id == pending) continue;
      if (chunks.integer(7)) require(plus(chunk.destination, chunk.length) <= identity.bytes &&
          Hash(archive.io.Read(chunk.destination, chunk.length)) == chunk.hash, "Previously written archive data changed", StorageReason::StaleInput);
      else ReadChunk(chunk);
    }
    if (pending) { archive.io.Write(target, after); archive.io.Sync(); Done(pending); }
  }
  void ValidateRecovery() {
    recovering = true;
    if (Value("version") == 1) {
      // Retained journals from the previous format carry full-range hashes.
      // Honor their recovery contract once before upgrading their metadata.
      ValidateLegacyRecovery();
      SqlTransaction transaction(db);
      db.exec("DROP TABLE redo; CREATE TABLE redo(id INTEGER PRIMARY KEY,chunk INTEGER UNIQUE,target INTEGER,before_hash TEXT,scratch_offset INTEGER);"
          "ALTER TABLE control ADD COLUMN source_stamp TEXT;");
      Set("version", 2); CaptureSourceStamp(); transaction.Commit(); return;
    }
    const auto identity = archive.io.Identity();
    require(identity.device == Value("device") && identity.fileId == Value("file_id") &&
        (Value("version") >= 3 ? identity.bytes == Value("source_size") :
         (identity.bytes == Value("original_size") || (Value("phase") >= 2 && identity.bytes == Value("final_size")))),
        "Archive identity or size changed since the interrupted update", StorageReason::StaleInput);
    Statement stamp(db.value, "SELECT source_stamp FROM control WHERE id=1");
    require(stamp.step() && stamp.bytes(0) == archive.io.ChangeToken(),
        "Archive changed after the last durable source checkpoint; recovery needs manual review", StorageReason::StaleInput);
    struct Redo { std::uint64_t chunk, target; std::string after; bool needsWrite; };
    std::vector<Redo> pending;
    std::uint64_t bytes = 0, end = 0;
    std::set<std::uint64_t> ids;
    std::unique_ptr<StorageArchiveIO> scratch;
    Statement redo(db.value, "SELECT chunk,target,before_hash,scratch_offset FROM redo ORDER BY target");
    while (redo.step()) {
      const auto id = redo.integer(0), target = redo.integer(1);
      Statement chunk(db.value, "SELECT destination,length,hash,done FROM chunks WHERE id=?"); chunk.bind(1, id);
      require(chunk.step() && chunk.integer(0) == target && !chunk.integer(3), "Archive redo evidence is inconsistent");
      const auto length = chunk.integer(1), offset = redo.integer(3);
      bytes = plus(bytes, length);
      require(started && length && length <= chunkSize && plus(target, length) <= identity.bytes &&
          target >= end && ids.insert(id).second && bytes <= batchBytes && pending.size() < batchRecords,
          "Invalid archive redo batch");
      end = target + length;
      if (!scratch) scratch = std::make_unique<StorageArchiveIO>(journal / "redo.bin", accessRoots, false);
      require(scratch->Identity().bytes <= 2 * batchBytes && plus(offset, 2 * length) <= scratch->Identity().bytes,
          "Invalid archive redo scratch extent");
      const auto before = scratch->Read(offset, length), after = scratch->Read(offset + length, length);
      require(redo.bytes(2) == Hash(before) && chunk.bytes(2) == Hash(after),
          "Archive redo scratch changed", StorageReason::StaleInput);
      const auto current = archive.io.Read(target, after.size());
      for (std::size_t i = 0; i < current.size(); ++i)
        require(current[i] == before[i] || current[i] == after[i], "Archive changed outside the interrupted write", StorageReason::StaleInput);
      pending.push_back({id, target, after, current != after});
    }
    if (pending.empty()) {
      return;
    }
    // The durable source token is required even with redo. A crash during a
    // source write cannot be distinguished from an unrelated off-range edit
    // without a whole-archive pass, so that ambiguous state stays fail-closed.
    for (const auto &row : pending) if (row.needsWrite) archive.io.Write(row.target, row.after);
    archive.io.Sync();
    SqlTransaction transaction(db);
    Statement done(db.value, "UPDATE chunks SET done=1 WHERE id=?");
    for (const auto &row : pending) { done.reset(); done.bind(1, row.chunk); done.step(); }
    db.exec("DELETE FROM redo"); CaptureSourceStamp(); transaction.Commit();
  }
  std::uint64_t Capacity() const { return Value(Value("version") >= 3 ? "working_size" : "original_size"); }
  void EnsureCapacity(std::uint64_t end) {
    require(end <= Capacity(), "Archive write exceeds its reserved extent");
    const auto bytes = archive.io.Identity().bytes;
    if (end <= bytes) return;
    require(started && Value("version") >= 3, "Archive growth was not authorized");
    archive.io.Truncate(end); archive.io.Sync();
    SqlTransaction transaction(db); CaptureSourceStamp(); transaction.Commit();
    Fault("archive-growth-synced");
  }
  void Start() { Set("started", 1); started = true; }
  void ValidateEntry(std::uint64_t entry) {
    archive.io.CheckPath();
    if (Value("version") >= 2) return;
    Statement q(db.value, "SELECT id,source,length,kind,destination,hash,data FROM chunks WHERE entry_id=? ORDER BY part");
    q.bind(1, entry);
    while (q.step()) ReadChunk({q.integer(0), q.integer(1), q.integer(2), q.integer(3), q.integer(4), q.bytes(5), q.bytes(6)});
  }
  void RemoveStaging() {
    for (const auto &entry : fs::directory_iterator(journal)) {
      const auto name = entry.path().filename().string();
      if (name != "manifest.sqlite" && name != "manifest.sqlite-journal" && ownedFile(name) && !speculativeFile(entry.path())) {
        require(!entry.is_symlink() && entry.is_regular_file(), "Unsafe archive staging file"); fs::remove(entry.path());
      }
    }
  }
  void RecordSkip(const StorageArchiveEntry &entry, FileOutcome outcome, StorageDiagnostic reason) {
    append(report, {.source = archive.io.path / fs::u8path(entry.name), .outcome = outcome, .reason = std::move(reason), .bytesBefore = entry.bytes});
  }
  void Prepare(StorageArchiveEntry &, const AssetCandidate &, const StoragePolicy &, Checkpoint, ProgressCallback);
  void FillEncodings(const StoragePolicy &, Checkpoint);
  void StartEncoding(PendingEncoding &, const StoragePolicy &, Checkpoint);
  void PollEncodings();
  bool ReserveOutput(PendingEncoding &, std::uint64_t);
  void RemoveEncoding(std::uint64_t);
  void ObserveScratch(std::uint64_t replacementBytes = 0);
  bool Reuse(const StorageArchiveEntry &, const AssetCandidate &, Checkpoint);
  void Flush() {
    if (pendingRecords.empty() && pendingChunks.empty()) return;
    archive.io.CheckPath();
    const auto capacity = Capacity();
    std::vector<std::string> payloads;
    payloads.reserve(pendingChunks.size());
    // Read the complete bounded batch before any overlapping destination write.
    for (const auto &chunk : pendingChunks) payloads.push_back(ReadChunkBytes(chunk, capacity));
    HashPayloads(payloads);
    bool writes = false;
    std::uint64_t writeEnd = 0;
    for (const auto &chunk : pendingChunks) writeEnd = std::max(writeEnd, plus(chunk.destination, chunk.length));
    EnsureCapacity(writeEnd);
    struct RedoRange { std::uint64_t offset = 0; std::string beforeHash; };
    std::vector<RedoRange> redoRanges(pendingChunks.size());
    const auto redoPath = journal / "redo.bin";
    std::unique_ptr<Output> redoFile;
    std::uint64_t redoOffset = 0;
    for (std::size_t i = 0; i < pendingChunks.size(); ++i) {
      const auto &chunk = pendingChunks[i];
      require(plus(chunk.destination, chunk.length) <= capacity &&
          (chunk.kind != 0 || chunk.destination <= chunk.source), "Archive compaction would overwrite unread data");
      if (chunk.kind == 0 && chunk.destination == chunk.source) continue;
      require(started || pendingDeletes, "Archive write was not authorized by durable state");
      if (!redoFile) { fs::remove(redoPath); redoFile = std::make_unique<Output>(redoPath); }
      Timer readTimer(deps.metrics ? &deps.metrics->readNanoseconds : nullptr);
      const auto before = archive.io.Read(chunk.destination, chunk.length); readTimer.Stop();
      redoRanges[i] = {redoOffset, Hash(before)};
      Timer redoTimer(deps.metrics ? &deps.metrics->redoNanoseconds : nullptr);
      redoFile->Write(before); redoFile->Write(payloads[i]); redoOffset += 2 * chunk.length; writes = true;
    }
    if (redoFile) {
      Timer timer(deps.metrics ? &deps.metrics->redoNanoseconds : nullptr);
      redoFile->Close(); syncFile(redoPath); syncDirectory(journal);
    }
    // Keep bulk redo bytes out of SQLite's rollback journal. Only bounded range
    // descriptors are committed here, after the sequential scratch file is durable.
    {
      Timer journalTimer(deps.metrics ? &deps.metrics->journalNanoseconds : nullptr);
      SqlTransaction transaction(db);
      Statement record(db.value, "UPDATE entries SET output_offset=?,action=CASE WHEN ? THEN 2 ELSE action END WHERE id=?");
      for (const auto &row : pendingRecords) {
        record.reset(); record.bind(1, row.output); record.bind(2, row.deleted); record.bind(3, row.id); record.step();
      }
      Statement insert(db.value, "INSERT INTO chunks(entry_id,part,source,length,hash,kind,destination,done,data) VALUES(?,?,?,?,?,0,?,0,X'')");
      Statement destination(db.value, "UPDATE chunks SET destination=? WHERE id=?");
      Statement redo(db.value, "INSERT INTO redo(id,chunk,target,before_hash,scratch_offset) VALUES(?,?,?,?,?)");
      for (std::size_t i = 0; i < pendingChunks.size(); ++i) {
        auto &chunk = pendingChunks[i];
        if (!chunk.id) {
          insert.reset(); insert.bind(1, chunk.entry); insert.bind(2, chunk.part); insert.bind(3, chunk.source);
          insert.bind(4, chunk.length); insert.text(5, chunk.hash); insert.bind(6, chunk.destination); insert.step();
          chunk.id = sqlite3_last_insert_rowid(db.value);
        } else {
          destination.reset(); destination.bind(1, chunk.destination); destination.bind(2, chunk.id); destination.step();
        }
        if (chunk.kind == 0 && chunk.destination == chunk.source) continue;
        redo.reset(); redo.bind(1, chunk.id); redo.bind(2, chunk.id); redo.bind(3, chunk.destination);
        redo.text(4, redoRanges[i].beforeHash); redo.bind(5, redoRanges[i].offset); redo.step();
      }
      if (pendingDeletes) Start();
      transaction.Commit();
    }
    if (pendingDeletes) Fault("archive-entry-ready");
    if (writes) Fault("archive-redo");
    // Adjacent records already have durable per-chunk redo evidence. Submit
    // their bytes together so small entries share path checks and write calls.
    // The joined buffer is bounded by the same 8 MiB batch limit.
    for (std::size_t i = 0; i < pendingChunks.size();) {
      const auto &chunk = pendingChunks[i];
      if (chunk.kind == 0 && chunk.destination == chunk.source) { ++i; continue; }
      auto end = i + 1;
      auto length = chunk.length;
      while (end < pendingChunks.size()) {
        const auto &next = pendingChunks[end];
        if (next.destination != chunk.destination + length || (next.kind == 0 && next.destination == next.source)) break;
        length += next.length; ++end;
      }
      std::string joined;
      std::string_view bytes = payloads[i];
      if (end > i + 1) {
        joined.reserve(length);
        for (auto part = i; part < end; ++part) joined += payloads[part];
        bytes = joined;
      }
      Timer writeTimer(deps.metrics ? &deps.metrics->writeNanoseconds : nullptr);
      archive.io.Write(chunk.destination, bytes); writeTimer.Stop(); Fault("archive-write");
      if (deps.metrics) { deps.metrics->bytesWritten += length; ++deps.metrics->sourceWriteRanges; }
      i = end;
    }
    if (writes) {
      Timer syncTimer(deps.metrics ? &deps.metrics->syncNanoseconds : nullptr);
      archive.io.Sync(); syncTimer.Stop();
      Timer journalTimer(deps.metrics ? &deps.metrics->journalNanoseconds : nullptr);
      SqlTransaction transaction(db); CaptureSourceStamp(); transaction.Commit();
      Fault("archive-source-synced");
    }
    {
      Timer journalTimer(deps.metrics ? &deps.metrics->journalNanoseconds : nullptr);
      SqlTransaction transaction(db);
      Statement done(db.value, "UPDATE chunks SET done=1 WHERE id=?");
      for (const auto &chunk : pendingChunks) { done.reset(); done.bind(1, chunk.id); done.step(); }
      Statement complete(db.value, "UPDATE entries SET complete=? WHERE id=?");
      Statement retire(db.value, "DELETE FROM chunks WHERE entry_id=?");
      std::optional<std::uint64_t> completedCursor;
      for (const auto &row : pendingRecords) if (row.complete) {
        complete.reset(); complete.bind(1, row.complete); complete.bind(2, row.id); complete.step();
        retire.reset(); retire.bind(1, row.id); retire.step(); completedCursor = row.cursor;
      }
      if (completedCursor) Set("cursor", *completedCursor);
      db.exec("DELETE FROM redo"); CaptureSourceStamp(); transaction.Commit();
    }
    if (deps.metrics) ++deps.metrics->durableBatches;
    if (!pendingChunks.empty()) Fault("archive-cursor");
    pendingChunks.clear(); pendingRecords.clear(); pendingBytes = 0; pendingDeletes = false;
    progress.archiveBytesDone = std::max(progress.archiveBytesDone, pendingSourceEnd);
    if (copyProgress) copyProgress(progress);
  }
  void CopyRecord(const StorageArchiveEntry &entry, std::uint64_t action, std::uint64_t originalSpan,
      bool deleted = false, bool relocated = false) {
    if (deleted) action = 2;
    else if (relocated) action = 0;
    const auto destination = (started || pendingDeletes || deleted) ? (copyCursor ? *copyCursor : Value("cursor")) : entry.offset;
    require(destination <= entry.offset, "Archive destination is beyond unread source");
    const auto record = [&] {
      if (!pendingRecords.empty() && pendingRecords.back().id == entry.id) return;
      if (pendingRecords.size() == batchRecords) Flush();
      pendingRecords.push_back({entry.id, destination, 0, 0, deleted});
      pendingDeletes |= deleted;
    };
    if (action != 2 && action != 3 && (action != 0 || destination != entry.offset)) {
      // Fresh unchanged records have no saved chunks. Recovery must still read
      // them: a partial overlapping move may already have consumed its source.
      auto *existing = action != 0 || recovering ?
          &Query("SELECT id,source,length,kind,destination,hash,data,done FROM chunks WHERE entry_id=? AND part=?") : nullptr;
      for (std::uint64_t part = 0, position = 0; position < entry.span; ++part) {
        const auto length = std::min(chunkSize, entry.span - position);
        Chunk chunk{.source = plus(entry.offset, position), .length = length, .destination = plus(destination, position),
            .entry = entry.id, .part = part};
        if (existing) {
          existing->reset(); existing->bind(1, entry.id); existing->bind(2, part);
          if (existing->step()) {
            if (existing->integer(7)) { existing->reset(); position += length; continue; }
            chunk = {existing->integer(0), existing->integer(1), existing->integer(2), existing->integer(3),
                destination + position, existing->bytes(5), existing->bytes(6), entry.id, part};
            require(chunk.length == length, "Archive chunk extent changed");
          } else require(action == 0, "Missing prepared archive chunk");
          existing->reset();
        }
        if (pendingBytes + length > batchBytes || pendingChunks.size() == batchRecords) Flush();
        record(); pendingChunks.push_back(std::move(chunk)); pendingBytes += length; position += length;
        pendingSourceEnd = relocated ? Value("original_size") : entry.offset + position;
      }
    }
    record(); copyCursor = (action == 2 || action == 3) ? destination : plus(destination, entry.span);
    pendingRecords.back().cursor = *copyCursor; pendingRecords.back().complete = relocated ? 2 : 1;
    pendingSourceEnd = relocated ? Value("original_size") : plus(entry.offset, originalSpan);
  }
  // Growing records cannot overwrite the next unread record. Append each once,
  // reclaim its original slot in the forward pass, then compact the appended
  // records in their source order. Scratch remains bounded to one media entry.
  void AppendReplacement(const StorageArchiveEntry &entry) {
    Flush();
    std::uint64_t previous = 0;
    while (auto chunk = NextChunk(entry.id, previous)) { previous = chunk->id; WriteChunk(*chunk); }
    Flush(); Fault("archive-replacement-appended");
  }
  void Relocations() {
    if (Value("version") < 3) return;
    std::optional<std::uint64_t> cursor;
    Statement next(db.value, "SELECT id,offset,span,data_offset,packed,bytes,crc,method,flags,directory,name,raw_name,metadata,complete,relocated_offset,original_span "
        "FROM entries INDEXED BY entries_relocated WHERE action=3 AND complete<2 AND offset>? ORDER BY offset LIMIT 1");
    std::uint64_t state[3]{};
    while (auto entry = nextArchiveEntry(next, cursor, state)) {
      cursor = entry->offset;
      require(state[0] == 1, "Archive replacement append is incomplete");
      entry->offset = state[1]; CopyRecord(*entry, 3, state[2], false, true);
    }
    Flush(); Fault("archive-relocations-synced");
  }
  void Entries(const StorageArchiveSummary *summary, const StoragePolicy &policy, Checkpoint check, ProgressCallback notify) {
    copyProgress = notify;
    if (summary) encodingSelection = std::make_unique<StoragePathIndex>(summary->innerRoots);
    const StoragePathIndex selected(summary ? summary->innerRoots : std::vector<fs::path>{});
    std::optional<std::uint64_t> cursor;
    Statement next(db.value, "SELECT id,offset,span,data_offset,packed,bytes,crc,method,flags,directory,name,raw_name,metadata,action,original_span "
        "FROM entries WHERE offset>? AND complete=0 ORDER BY offset LIMIT 1"), collisions(db.value, candidateCollisionSql);
    std::uint64_t state[2]{};
    while (auto entry = nextArchiveEntry(next, cursor, state)) {
      cursor = entry->offset;
      auto action = state[0];
      const bool cancelled = check && !check(); report.cancelled |= cancelled;
      if (cancelled && !started) return;
      bool deleted = false;
      if (summary && !report.cancelled && !action && !selected.Ancestors(fs::u8path(entry->name)).empty()) {
        StorageDiagnostic reason; const auto candidate = archiveCandidate(*entry, policy, reason, collisions);
        if (candidate) {
          if (candidate->operation == AssetOperation::Delete) {
            archive.io.CheckPath();
            if (!started && !pendingDeletes) Flush();
            deleted = true;
          } else if (candidate->reuseDestination) {
            Flush(); RemoveStaging();
            progress.currentPath = archive.io.path / candidate->destination; progress.phase = StoragePhase::Verify; if (notify) notify(progress);
            try { deleted = Reuse(*entry, *candidate, check); }
            catch (const StorageArchiveError &error) {
              if (error.code == StorageReason::Cancelled) { report.cancelled = true; if (!started) return; }
              else throw;
            }
          } else {
          Flush(); RemoveStaging();
          progress.currentPath = archive.io.path / candidate->source; progress.phase = StoragePhase::Encode; if (notify) notify(progress);
          try { Prepare(*entry, *candidate, policy, check, [&](const StorageProgress &value) { progress.phase = value.phase; if (notify) notify(progress); }); }
          catch (const StorageArchiveError &error) {
            if (error.code == StorageReason::Cancelled) { report.cancelled = true; if (!started) return; }
            else throw;
          }
          auto &prepared = Query("SELECT action FROM entries WHERE id=?");
          prepared.bind(1, entry->id); require(prepared.step(), "Missing prepared archive entry");
          action = prepared.integer(0); prepared.reset();
          }
          ++progress.filesDone;
        }
      }
      progress.currentPath = archive.io.path / fs::u8path(entry->name);
      progress.phase = StoragePhase::Commit; if (notify) notify(progress);
      if (action == 3) AppendReplacement(*entry);
      CopyRecord(*entry, action, state[1], deleted);
      RemoveEncoding(entry->id);
    }
    encodings.clear();
    progress.encodesActive = 0;
    Flush(); RemoveStaging();
  }
  void Trailer() {
    SqlTransaction transaction(db);
    const auto start = Value("cursor"); std::uint64_t destination = start, part = 0, count = 0; std::string buffer;
    auto flush = [&] { if (!buffer.empty()) { InsertChunk(0, part++, 0, buffer, 2, destination); destination = plus(destination, buffer.size()); buffer.clear(); } };
    auto append = [&](std::string_view bytes) {
      while (!bytes.empty()) {
        const auto amount = std::min<std::size_t>(chunkSize - buffer.size(), bytes.size()); buffer.append(bytes.substr(0, amount)); bytes.remove_prefix(amount);
        if (buffer.size() == chunkSize) flush();
      }
    };
    if (archive.Format() == StorageArchiveFormat::Tar) append(std::string(1024, '\0'));
    else {
      std::optional<std::uint64_t> cursor;
      Statement next(db.value, "SELECT id,offset,span,data_offset,packed,bytes,crc,method,flags,directory,name,raw_name,metadata,output_offset "
          "FROM entries WHERE offset>? AND action!=2 ORDER BY offset LIMIT 1");
      std::uint64_t outputOffset[1]{};
      while (auto entry = nextArchiveEntry(next, cursor, outputOffset)) {
        cursor = entry->offset; append(archive.Central(*entry, outputOffset[0])); ++count;
      }
      const auto centralSize = plus(destination, buffer.size()) - start;
      if (count >= 0xffff || centralSize >= 0xffffffff || start >= 0xffffffff) {
        std::string record(56, '\0'); put(record, 0, 0x06064b50, 4); put(record, 4, 44, 8); put(record, 12, 45, 2); put(record, 14, 45, 2);
        put(record, 24, count, 8); put(record, 32, count, 8); put(record, 40, centralSize, 8); put(record, 48, start, 8); append(record);
        std::string locator(20, '\0'); put(locator, 0, 0x07064b50, 4); put(locator, 8, start + centralSize, 8); put(locator, 16, 1, 4); append(locator);
      }
      std::string end(22, '\0'); put(end, 0, 0x06054b50, 4); put(end, 8, std::min<std::uint64_t>(count, 0xffff), 2); put(end, 10, std::min<std::uint64_t>(count, 0xffff), 2);
      put(end, 12, std::min<std::uint64_t>(centralSize, 0xffffffff), 4); put(end, 16, std::min<std::uint64_t>(start, 0xffffffff), 4); put(end, 20, archive.comment.size(), 2);
      append(end); append(archive.comment);
    }
    flush();
    if (Value("version") >= 3) Set("working_size", std::max(Capacity(), destination));
    else require(destination < Value("original_size"), "Archive rewrite does not reclaim physical space");
    Set("final_size", destination); Set("phase", 1); transaction.Commit();
  }
  void Finish() {
    if (!started) return;
    if (Value("phase") == 0) { Relocations(); Trailer(); }
    if (Value("phase") == 1) {
      std::uint64_t previous = 0;
      while (auto chunk = NextChunk(0, previous)) { previous = chunk->id; WriteChunk(*chunk); }
      Flush(); Set("phase", 2);
    }
    if (Value("phase") == 2) {
      archive.io.CheckPath(); Fault("archive-before-truncate"); archive.io.Truncate(Value("final_size")); archive.io.Sync(); Fault("archive-truncate");
      SqlTransaction transaction(db); CaptureSourceStamp(); Set("phase", 3); transaction.Commit();
      Fault("archive-truncate-synced");
    }
    if (deps.metrics) ++deps.metrics->archivesCompacted;
  }
  StorageReport Result() {
    if (!started) return report;
    require(Value("phase") == 3, "Archive update still requires recovery", StorageReason::RecoveryRequired);
    archive.io.CheckPath();
    const auto nextSourceIdentity = deps.measurementBaselines.empty() ? std::string{} : archive_source_identity::KeyForPath(archive.io.path);
    archive.io.CheckPath();
    std::vector<fs::path> groupRoots;
    for (const auto &row : deps.measurementBaselines) {
      if (row.sourceKind != StorageSourceKind::Archive || !row.totalBytes || row.reason.code != StorageReason::None ||
          originalSourceIdentity.empty() || nextSourceIdentity.empty() || row.sourceIdentity != originalSourceIdentity ||
          !IsWithinFolder(row.root, archive.io.path)) continue;
      groupRoots.push_back(row.root);
      report.groupDeltas.push_back({.groupId = row.groupId, .baselineBytes = row.totalBytes,
          .sourceIdentity = row.sourceIdentity, .nextSourceIdentity = nextSourceIdentity, .baselineGeneration = row.generation});
    }
    const StoragePathIndex groups(std::move(groupRoots));
    Statement q(db.value, "SELECT original_name,name,original_bytes,bytes,action FROM entries WHERE action!=0 ORDER BY offset");
    while (q.step()) {
      const bool deleted = q.integer(4) == 2; if (deleted) ++report.deleted; else ++report.converted;
      const auto source = archive.io.path / fs::u8path(q.bytes(0));
      const auto before = q.integer(2), after = deleted ? 0 : q.integer(3);
      const auto kind = asobmshow::chart_assets::ClassifyMediaPath(source);
      // This existing committed-entry traversal also updates cached totals;
      // it never depends on the bounded diagnostic list or rereads the index.
      for (const auto position : groups.Ancestors(source.parent_path())) {
        auto &delta = report.groupDeltas[position];
        delta.bytesBefore = plus(delta.bytesBefore, before); delta.bytesAfter = plus(delta.bytesAfter, after);
        using asobmshow::chart_assets::MediaKind;
        if (kind == MediaKind::Audio) { delta.audioBefore = plus(delta.audioBefore, before); delta.audioAfter = plus(delta.audioAfter, after); }
        if (kind == MediaKind::Video) { delta.videoBefore = plus(delta.videoBefore, before); delta.videoAfter = plus(delta.videoAfter, after); }
        if (kind == MediaKind::Image) { delta.imageBefore = plus(delta.imageBefore, before); delta.imageAfter = plus(delta.imageAfter, after); }
      }
      if (report.files.size() < 100) RetainStorageReportDetail(report, {.source = archive.io.path / fs::u8path(q.bytes(0)),
          .destination = deleted ? fs::path{} : archive.io.path / fs::u8path(q.bytes(1)), .outcome = deleted ? FileOutcome::Deleted : FileOutcome::Converted,
          .bytesBefore = q.integer(2), .bytesAfter = deleted ? 0 : q.integer(3)});
    }
    const auto before = Value("original_size"), after = Value("final_size");
    report.reclaimedBytes = before > after ? before - after : 0;
    report.addedBytes = after > before ? after - before : 0; report.changedRoots.push_back(archive.io.path); return report;
  }
};
bool Update::Reuse(const StorageArchiveEntry &entry, const AssetCandidate &candidate, Checkpoint check) {
  auto target = archive.Find(candidate.destination);
  require(target && target->id != entry.id && !target->directory, "Existing archive destination disappeared", StorageReason::StaleInput);
  // Flush has made earlier compaction durable. Resolve a retained entry at its
  // current physical position rather than extracting its obsolete source range.
  Statement state(db.value, "SELECT complete,output_offset,action FROM entries WHERE id=?");
  state.bind(1, target->id); require(state.step(), "Missing existing archive destination");
  require(state.integer(2) == 0, "Existing archive destination was changed", StorageReason::StaleInput);
  if (state.integer(0)) {
    target->dataOffset = plus(state.integer(1), target->dataOffset - target->offset);
    target->offset = state.integer(1);
  }
  const auto required = plus(target->bytes, stagingReserve);
  const auto available = deps.availableSpace ? deps.availableSpace(journal) : std::optional<std::uint64_t>(fs::space(journal).available);
  if (required > deps.scratchLimit || !available || required > *available) {
    RecordSkip(entry, FileOutcome::Skipped, {StorageReason::InsufficientSpace, "Existing archive destination exceeds the validation staging budget"});
    return false;
  }
  auto reservation = workBudget->Acquire(candidate.kind, required, check);
  if (!reservation) {
    active(check);
    RecordSkip(entry, FileOutcome::Skipped, {StorageReason::InsufficientSpace, "Insufficient space for archive destination validation"});
    return false;
  }
  if (!deps.validateExisting) {
    RecordSkip(entry, FileOutcome::Skipped, {StorageReason::CodecUnavailable, "Existing destination validation is unavailable"});
    return false;
  }
  const auto input = journal / ("input." + asobmshow::chart_assets::LowerMediaExtension(candidate.destination));
  StorageDiagnostic reason;
  try {
    archive.io.CheckPath(); archive.Extract(*target, input, check);
    auto staged = candidate; staged.destination = input;
    reason = deps.validateExisting(staged, check);
  } catch (const StorageArchiveError &error) {
    if (error.code != StorageReason::ValidationFailed) throw;
    reason = {error.code, error.what()};
  } catch (const std::exception &error) {
    reason = {StorageReason::IoError, error.what()};
  }
  RemoveStaging(); archive.io.CheckPath(); active(check);
  if (reason.code == StorageReason::Cancelled) throw StorageArchiveError(reason.code, reason.detail);
  if (reason.code != StorageReason::None) {
    RecordSkip(entry, FileOutcome::Failed, std::move(reason)); return false;
  }
  return true;
}
bool Update::ReserveOutput(PendingEncoding &pending, std::uint64_t bytes) {
  if (bytes <= pending.outputLimit) return true;
  const auto required = plus(pending.inputBytes, plus(plus(bytes, bytes), stagingReserve));
  std::lock_guard lock(encodingMutex);
  const auto extra = required - pending.reservedBytes;
  if (required > deps.scratchLimit || pendingScratch > deps.scratchLimit - extra || !pending.reservation->GrowTo(required)) return false;
  pendingScratch += extra; pending.reservedBytes = required; pending.outputLimit = bytes; return true;
}
void Update::ObserveScratch(std::uint64_t replacementBytes) {
  if (!deps.metrics) return;
  auto bytes = replacementBytes;
  for (const auto &[id, pending] : encodings) {
    for (const auto &path : {pending->input, pending->output}) {
      std::error_code error; const auto size = fs::file_size(path, error);
      if (!error) bytes = plus(bytes, size);
    }
  }
  deps.metrics->peakScratchBytes = std::max(deps.metrics->peakScratchBytes, bytes);
}
void Update::RemoveEncoding(std::uint64_t id) {
  const auto found = encodings.find(id);
  if (found == encodings.end()) return;
  auto &pending = *found->second;
  pending.job.reset(); // Join before deleting either path or releasing scratch.
  ObserveScratch();
  for (const auto &path : {pending.input, pending.output}) {
    if (path.empty()) continue;
    std::error_code error; const auto status = fs::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found) continue;
    require(!error && fs::is_regular_file(status) && !fs::is_symlink(status), "Unsafe speculative archive staging file");
    fs::remove(path);
  }
  { std::lock_guard lock(encodingMutex); pendingScratch -= pending.reservedBytes; }
  encodings.erase(found);
}
void Update::PollEncodings() {
  progress.encodesActive = 0;
  for (auto &[id, pending] : encodings) {
    if (!pending->job) continue;
    if (!pending->job->Ready()) { ++progress.encodesActive; continue; }
    try { pending->result = pending->job->Take(); }
    catch (const std::exception &error) { pending->result = {FileOutcome::Failed, {}, {StorageReason::IoError, error.what()}}; }
    catch (...) { pending->result = {FileOutcome::Failed, {}, {StorageReason::IoError, "Unhandled archive encoder failure"}}; }
    pending->job.reset();
    ++progress.encodesDone;
  }
}
void Update::FillEncodings(const StoragePolicy &policy, Checkpoint check) {
  require(!workBudget->RecoveryRequired(), "Storage work stopped for recovery", StorageReason::RecoveryRequired);
  PollEncodings();
  // Dispatch already staged work even when the read-ahead budget is full.
  for (auto &[id, pending] : encodings) StartEncoding(*pending, policy, check);
  const auto window = std::max<std::size_t>(8, deps.executionLimits.audioWorkers + StorageExecutionLimits::videoWorkers);
  // Reserved entries are bounded by aggregate scratch (each includes the
  // staging reserve). Completed outputs must not fill the encoder lookahead
  // while an earlier video holds source-order commits. Bound unreserved error
  // results separately, since they consume metadata without consuming scratch.
  auto unreserved = std::count_if(encodings.begin(), encodings.end(), [](const auto &entry) { return !entry.second->reservation; });
  if (!encodingSelection || encodingExhausted || unreserved >= window || (check && !check())) return;
  Statement next(db.value, "SELECT id,offset,span,data_offset,packed,bytes,crc,method,flags,directory,name,raw_name,metadata "
      "FROM entries WHERE offset>? AND complete=0 AND action=0 ORDER BY offset LIMIT 1"), collisions(db.value, candidateCollisionSql);
  // Bound metadata lookahead as well as media staging. A long unselected tail
  // must not delay consumption of an already completed encoder.
  for (unsigned scanned = 0; scanned < 256 && unreserved < window && (!check || check()); ++scanned) {
    const auto entry = nextArchiveEntry(next, encodingCursor);
    if (!entry) { encodingExhausted = true; return; }
    StorageDiagnostic reason;
    const auto candidate = encodingSelection->Ancestors(fs::u8path(entry->name)).empty() ? std::optional<AssetCandidate>{} :
        archiveCandidate(*entry, policy, reason, collisions);
    if (!candidate || candidate->operation == AssetOperation::Delete) { encodingCursor = entry->offset; continue; }
    // Reuse validation owns its staging synchronously. Do not reserve later
    // media across this barrier; earlier encodes are consumed before reuse.
    if (candidate->reuseDestination) return;
    // Reserve in source order, even when this codec is busy. A later staged
    // result must never occupy space needed by the next entry to commit.
    const auto required = plus(plus(entry->bytes, entry->bytes), plus(entry->bytes, stagingReserve));
    auto pending = std::make_unique<PendingEncoding>();
    pending->inputBytes = entry->bytes; pending->outputLimit = entry->bytes;
    bool waiting = false;
    {
      std::lock_guard lock(encodingMutex);
      if (required <= deps.scratchLimit) {
        if (pendingScratch > deps.scratchLimit - required) return;
        const auto available = deps.availableSpace ? deps.availableSpace(journal) : std::optional<std::uint64_t>(fs::space(journal).available);
        if (available && required <= *available) {
          auto lease = workBudget->TryReserve(candidate->kind, required, waiting);
          if (lease) pending->reservation = std::make_shared<StorageWorkBudget::Lease>(std::move(*lease));
        } else if (!encodings.empty()) return;
      }
      if (!pending->reservation && waiting) return;
      if (pending->reservation) { pendingScratch += required; pending->reservedBytes = required; }
    }
    encodingCursor = entry->offset;
    auto &item = *pending;
    encodings.emplace(entry->id, std::move(pending));
    if (!item.reservation) {
      ++unreserved;
      item.result.reason = {StorageReason::InsufficientSpace, "Archive entry exceeds the bounded media staging budget"}; continue;
    }
    item.input = journal / ("input-" + std::to_string(entry->id) + "." + asobmshow::chart_assets::LowerMediaExtension(candidate->source));
    item.output = journal / ("output-" + std::to_string(entry->id) + candidate->destination.extension().string());
    try { archive.io.CheckPath(); archive.Extract(*entry, item.input, check); }
    catch (const StorageArchiveError &error) {
      if (error.code != StorageReason::ValidationFailed) throw;
      archive.io.CheckPath();
      item.result = {FileOutcome::Failed, {}, {error.code, error.what()}};
      // Keep its reservation until the owner consumes and removes any partial input.
      item.reservation->ReleaseEncoder(); continue;
    }
    auto staged = *candidate; staged.source = item.input; staged.destination = item.output;
    std::string error; const auto identity = ReadFileIdentity(item.input, error, accessRoots);
    if (!identity) throw StorageArchiveError(StorageReason::IoError, "Cannot identify staged archive media: " + error);
    staged.identity = *identity;
    require(static_cast<bool>(deps.encode), "Archive media encoder is unavailable", StorageReason::CodecUnavailable);
    item.staged = std::move(staged);
    StartEncoding(item, policy, check);
    ObserveScratch();
  }
}
void Update::StartEncoding(PendingEncoding &item, const StoragePolicy &policy, Checkpoint check) {
  if (!item.staged || stopEncoding || (check && !check()) || !item.reservation->TryStartEncoder()) return;
  item.job = std::make_unique<StorageEncodingJob>(item.reservation, [&, staged = std::move(*item.staged), policy, check, pending = &item] {
    auto checkpoint = [&] {
      if (stopEncoding || (check && !check())) return false;
      std::error_code error; const auto size = fs::file_size(pending->output, error);
      if (!error && !ReserveOutput(*pending, size)) pending->tooLarge = true;
      return !pending->tooLarge;
    };
    return deps.encode(staged, policy, pending->output, checkpoint, [&](const StorageProgress &value) {
      std::lock_guard lock(encodingMutex); pending->phase = value.phase;
    });
  });
  item.staged.reset();
  ++progress.encodesActive;
}
void Update::Prepare(StorageArchiveEntry &entry, const AssetCandidate &candidate, const StoragePolicy &policy, Checkpoint check, ProgressCallback notify) {
  RemoveStaging();
  if (!encodingCursor || *encodingCursor < entry.offset)
    encodingCursor = entry.offset ? std::optional<std::uint64_t>(entry.offset - 1) : std::nullopt;
  while (!encodings.contains(entry.id)) {
    active(check); FillEncodings(policy, check);
    if (!encodings.contains(entry.id)) {
      require(!encodingExhausted, "Missing archive encoding candidate", StorageReason::StaleInput);
      std::unique_lock lock(encodingMutex); std::condition_variable changed;
      changed.wait_for(lock, std::chrono::milliseconds(10));
    }
  }
  auto &pending = *encodings.at(entry.id);
  while (pending.staged || pending.job) {
    active(check); FillEncodings(policy, check);
    StorageProgress value;
    { std::lock_guard lock(encodingMutex); value.phase = pending.phase; }
    if (notify) notify(value);
    if (pending.job) pending.job->Wait();
    else if (pending.staged) {
      std::unique_lock lock(encodingMutex); std::condition_variable changed;
      changed.wait_for(lock, std::chrono::milliseconds(10));
    }
  }
  const auto &encoded = pending.result;
  const auto &output = pending.output;
  if (pending.tooLarge) { RecordSkip(entry, FileOutcome::Skipped, {StorageReason::InsufficientSpace, "Encoded archive media exceeds the staging limit"}); return; }
  if (encoded.outcome != FileOutcome::Converted) { RecordSkip(entry, encoded.outcome, encoded.reason); return; }
  require(encoded.outputPath == output, "Archive encoder returned an unowned output", StorageReason::ValidationFailed);
  std::string error;
  const auto encodedIdentity = ReadFileIdentity(output, error, accessRoots); require(encodedIdentity.has_value(), "Unsafe archive encoder output", StorageReason::ValidationFailed);
  require(encodedIdentity->bytes != 0, "Empty archive encoder output", StorageReason::ValidationFailed);
  if (!ReserveOutput(pending, encodedIdentity->bytes)) {
    RecordSkip(entry, FileOutcome::Skipped, {StorageReason::InsufficientSpace, "Encoded archive media exceeds the staging limit"}); return;
  }
  if (candidate.operation != AssetOperation::LosslessFlac && encodedIdentity->bytes >= entry.bytes) {
    RecordSkip(entry, FileOutcome::Skipped, {StorageReason::NotSmaller, "Encoded archive media is not smaller"}); return;
  }
  active(check); StorageArchiveIO encodedFile(output, accessRoots, false); auto crc = crc32(0, nullptr, 0);
  for (std::uint64_t position = 0; position < encodedIdentity->bytes;) {
    active(check); auto bytes = encodedFile.Read(position, std::min(chunkSize, encodedIdentity->bytes - position));
    crc = crc32(crc, reinterpret_cast<const Bytef *>(bytes.data()), bytes.size()); position += bytes.size();
  }
  auto replacement = entry; std::string header;
  try { header = archive.Replacement(replacement, candidate.destination.extension().string(), encodedIdentity->bytes, crc); }
  catch (const StorageArchiveError &error) {
    if (error.code != StorageReason::UnsupportedFormat) throw;
    archive.io.CheckPath();
    // Keep the original entry and continue. CopyRecord removes staging after
    // encodedFile has closed, which is required on Windows.
    RecordSkip(entry, FileOutcome::Skipped, {error.code, error.what()}); return;
  }
  std::uint64_t extra = 0;
  if (archive.Format() == StorageArchiveFormat::Zip) { const auto central = archive.Central(replacement, entry.offset); if (central.size() > entry.metadata.size()) extra = central.size() - entry.metadata.size(); }
  if (candidate.operation != AssetOperation::LosslessFlac && plus(replacement.span, extra) >= entry.span) {
    // CopyRecord removes staging after encodedFile has closed on every platform.
    RecordSkip(entry, FileOutcome::Skipped, {StorageReason::NotSmaller, "Encoded media does not reduce its compressed archive record"}); return;
  }
  const bool relocate = replacement.span > entry.span;
  const auto appendOffset = Capacity();
  const auto prepared = journal / "replacement.bin"; Output file(prepared); file.Write(header);
  for (std::uint64_t position = 0; position < encodedIdentity->bytes;) {
    active(check); const auto bytes = encodedFile.Read(position, std::min(chunkSize, encodedIdentity->bytes - position)); file.Write(bytes); position += bytes.size();
  }
  const auto padding = replacement.span - header.size() - encodedIdentity->bytes; require(padding < 512, "Invalid archive replacement padding");
  file.Write(std::string(padding, '\0')); file.Close(); syncFile(prepared); syncDirectory(journal);
  require(ReadFileIdentity(output, error, accessRoots) == encodedIdentity, "Encoded archive media changed before installation", StorageReason::StaleInput);
  if (relocate) {
    const auto free = deps.availableSpace ? deps.availableSpace(archive.io.path) :
        std::optional<std::uint64_t>(fs::space(archive.io.path).available);
    if (!free || plus(replacement.span, stagingReserve) > *free) {
      RecordSkip(entry, FileOutcome::Skipped, {StorageReason::InsufficientSpace, "Insufficient space to append FLAC archive entry"}); return;
    }
  }
  ValidateEntry(entry.id);
  ObserveScratch(replacement.span);
  {
    StorageArchiveIO stage(prepared, accessRoots, false); SqlTransaction transaction(db);
    Statement remove(db.value, "DELETE FROM chunks WHERE entry_id=?"); remove.bind(1, entry.id); remove.step();
    for (std::uint64_t part = 0, position = 0; position < replacement.span; ++part) {
      const auto bytes = stage.Read(position, std::min(chunkSize, replacement.span - position)); InsertChunk(entry.id, part, position, bytes, 1, relocate ? plus(appendOffset, position) : 0); position += bytes.size();
    }
    Statement q(db.value, "UPDATE entries SET span=?,packed=?,bytes=?,crc=?,method=?,flags=?,name=?,raw_name=?,metadata=?,action=?,relocated_offset=? WHERE id=?");
    q.bind(1, replacement.span); q.bind(2, replacement.packed); q.bind(3, replacement.bytes); q.bind(4, replacement.crc);
    q.bind(5, replacement.method); q.bind(6, replacement.flags); q.text(7, replacement.name); q.bind(8, replacement.rawName); q.bind(9, replacement.metadata); q.bind(10, relocate ? 3 : 1); q.bind(11, relocate ? appendOffset : 0); q.bind(12, entry.id); q.step();
    if (relocate) Set("working_size", plus(appendOffset, replacement.span));
    Set("stage_entry", entry.id); Start(); transaction.Commit();
  }
  entry = std::move(replacement); Fault("archive-entry-ready");
}
fs::path createJournal(const StoragePlatformPaths &paths) {
  fs::create_directories(paths.journalRoot); std::string error;
  require(IsSafeLocalPath(paths.journalRoot, error, paths.pathAccessRoots), "Unsafe archive journal root", StorageReason::UnsafePath);
  const auto journal = paths.journalRoot / ("archive-preparing-" + uuid::generateV4());
  require(fs::create_directory(journal), "Cannot create archive recovery directory", StorageReason::IoError); fs::permissions(journal, fs::perms::owner_all, fs::perm_options::replace);
  auto path = paths.journalRoot;
  while (!path.empty()) { syncDirectory(path); if (path == paths.journalSyncRoot || path == path.root_path()) break; path = path.parent_path(); }
  return journal;
}
void failure(StorageReport &report, const fs::path &path, const std::exception &error, bool started) {
  const auto *archiveError = dynamic_cast<const StorageArchiveError *>(&error);
  const auto reason = started ? StorageReason::RecoveryRequired : archiveError ? archiveError->code : StorageReason::IoError;
  append(report, {.source = path, .outcome = reason == StorageReason::Cancelled ? FileOutcome::Cancelled : FileOutcome::Failed, .reason = {reason, error.what()}});
  report.recoveryRequired |= started; if (started) report.changedRoots.push_back(path);
}
}
StorageReport UpdateStorageArchive(const StorageArchiveSummary &summary, const StoragePolicy &policy,
    const StoragePlatformPaths &paths, const StorageArchiveDependencies &deps, Checkpoint check, ProgressCallback progress) {
  Timer total(deps.metrics ? &deps.metrics->totalNanoseconds : nullptr);
  StorageReport report; fs::path journal; bool started = false;
  try {
    authority(summary.path, paths); active(check); std::string error; const auto current = ReadFileIdentity(summary.path, error, paths.pathAccessRoots);
    require(current && *current == summary.identity, "Archive changed since review", StorageReason::StaleInput);
    if (progress) progress({.phase = StoragePhase::ArchivePrepare, .currentPath = summary.path, .filesTotal = summary.eligibleCount});
    active(check); journal = createJournal(paths);
    {
      Timer preparing(deps.metrics ? &deps.metrics->preparationNanoseconds : nullptr);
      StorageArchive archive(summary.path, journal / "manifest.sqlite", paths.pathAccessRoots, check);
      require(archive.identity == summary.identity, "Archive changed while starting update", StorageReason::StaleInput);
      archive.ValidateBrowsingIndex(check);
      {
        Update preparing(archive, journal, deps, started, paths.pathAccessRoots); preparing.Initialize(check);
      }
      // A preparation crash cannot have modified the source. Publish the ready
      // journal only after its database is closed, while retaining the file lock.
      archive.db.reset();
      const auto ready = journal.parent_path() / ("archive-" + journal.filename().string().substr(18));
      fs::rename(journal, ready); journal = ready; syncDirectory(journal.parent_path());
      archive.db = std::make_unique<Database>(journal / "manifest.sqlite", false);
      preparing.Stop();
      Update update(archive, journal, deps, started, paths.pathAccessRoots); update.progress.filesTotal = summary.eligibleCount;
      update.Entries(&summary, policy, check, progress); update.Finish(); report = update.Result();
      if (deps.metrics) deps.metrics->sourceBytesRead += archive.io.BytesRead();
    }
    cleanup(journal, deps); journal.clear();
  } catch (const std::exception &error) {
    report.groupDeltas.clear();
    failure(report, summary.path, error, started);
    if (!started && !journal.empty()) { try { cleanup(journal); } catch (...) { report.cleanupPending = true; } }
  }
  return report;
}
StorageReport RecoverStorageArchives(const StoragePlatformPaths &paths, Checkpoint check, const StorageArchiveDependencies &deps,
                                    StorageRecoveryProgressCallback notify) {
  StorageReport result; std::error_code error; if (!fs::exists(paths.journalRoot, error) && !error) return result;
  StorageRecoveryProgress progress;
  try {
    std::string pathError; require(IsSafeLocalPath(paths.journalRoot, pathError, paths.pathAccessRoots), "Unsafe archive recovery root", StorageReason::RecoveryRequired);
    for (const auto &entry : fs::directory_iterator(paths.journalRoot)) {
      active(check); const auto name = entry.path().filename().string(); if (!name.starts_with("archive-")) continue;
      if (name.starts_with("archive-preparing-") || name.starts_with("archive-cleanup-")) {
        const auto prefix = name.starts_with("archive-preparing-") ? 18 : 16;
        require(uuid::isCanonicalLowerV4(name.substr(prefix)) && !entry.is_symlink() && entry.is_directory(), "Invalid archive cleanup directory", StorageReason::RecoveryRequired);
        progress.phase = StorageRecoveryPhase::FinalizingArchive;
        progress.currentPath = entry.path(); progress.archiveBytesDone = 0;
        NotifyStorageRecoveryProgress(notify, progress);
        cleanup(entry.path(), deps); continue;
      }
      require(uuid::isCanonicalLowerV4(name.substr(8)) && !entry.is_symlink() && entry.is_directory(), "Invalid archive recovery directory", StorageReason::RecoveryRequired);
      const auto journal = entry.path(); fs::path source; bool started = true;
      try {
        require(ReadFileIdentity(journal / "manifest.sqlite", pathError, paths.pathAccessRoots).has_value(), "Unsafe archive manifest", StorageReason::RecoveryRequired); bool ready = false;
        {
          Database db(journal / "manifest.sqlite", false); Statement q(db.value, "SELECT version,path,ready,started FROM control WHERE id=1");
          require(q.step() && (q.integer(0) >= 1 && q.integer(0) <= 3), "Unknown archive recovery version"); source = fs::u8path(q.bytes(1)); ready = q.integer(2); started = q.integer(3);
        }
        if (ready && started) {
          require(source.is_absolute() && source == source.lexically_normal(), "Invalid archive journal path");
          if (paths.resolveRecoveryPath) source = paths.resolveRecoveryPath(source);
          require(source.is_absolute() && source == source.lexically_normal(), "Invalid relocated archive journal path");
          progress.phase = StorageRecoveryPhase::CheckingArchive;
          progress.currentPath = source; progress.archiveBytesDone = 0;
          NotifyStorageRecoveryProgress(notify, progress);
          authority(source, paths); StorageArchive archive(source, journal / "manifest.sqlite", paths.pathAccessRoots, {}, true);
          Update update(archive, journal, deps, started, paths.pathAccessRoots); update.ValidateRecovery();
          progress.phase = StorageRecoveryPhase::RestoringArchive;
          ProgressCallback copyProgress;
          if (notify) copyProgress = [&](const StorageProgress &value) {
              if (progress.phase == StorageRecoveryPhase::RestoringArchive)
                progress.currentPath = value.currentPath.empty() ? source : value.currentPath;
              progress.archiveBytesDone = value.archiveBytesDone;
              NotifyStorageRecoveryProgress(notify, progress);
            };
          update.Entries(nullptr, {}, {}, std::move(copyProgress));
          progress.phase = StorageRecoveryPhase::FinalizingArchive; progress.currentPath = source;
          NotifyStorageRecoveryProgress(notify, progress);
          update.Finish();
          auto report = update.Result(); result.converted += report.converted; result.deleted += report.deleted; AccumulateStorageBytes(result.reclaimedBytes, result.addedBytes, report.reclaimedBytes, report.addedBytes);
          for (auto &file : report.files) RetainStorageReportDetail(result, std::move(file)); result.changedRoots.push_back(source);
        }
        cleanup(journal, deps);
        if (ready && started) {
          progress.phase = StorageRecoveryPhase::Complete; progress.currentPath = source;
          ++progress.completedOperations;
          NotifyStorageRecoveryProgress(notify, progress);
        }
      } catch (const std::exception &error) { failure(result, source.empty() ? journal : source, error, true); }
    }
  } catch (const std::exception &error) {
    const auto *cancelled = dynamic_cast<const StorageArchiveError *>(&error);
    if (cancelled && cancelled->code == StorageReason::Cancelled) result.cancelled = true; else failure(result, paths.journalRoot, error, true);
  }
  return result;
}
}
