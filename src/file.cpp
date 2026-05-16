// SPDX-License-Identifier: MIT
#include "osession/osession.hpp"

#include <miniz.h>
#include <sqlite3.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace osession {

namespace {

// FNV-1a 64-bit - fast, no extra dep. Plan dedup only needs collision
// resistance against accidental matches in a single capture; a 64-bit
// hash is plenty.
uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string to_hex(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx",
                  static_cast<unsigned long long>(v));
    return std::string(buf, 16);
}

std::string deflate_compress(std::string_view in) {
    size_t out_size = 0;
    void* buf = tdefl_compress_mem_to_heap(
        in.data(), in.size(), &out_size, TDEFL_DEFAULT_MAX_PROBES);
    if (!buf) throw Error("compress failed");
    std::string out(static_cast<char*>(buf), out_size);
    mz_free(buf);
    return out;
}

std::string deflate_decompress(std::string_view in) {
    size_t out_size = 0;
    void* buf = tinfl_decompress_mem_to_heap(
        in.data(), in.size(), &out_size, 0);
    if (!buf) throw Error("decompress failed");
    std::string out(static_cast<char*>(buf), out_size);
    mz_free(buf);
    return out;
}

struct Stmt {
    sqlite3_stmt* s = nullptr;
    Stmt() = default;
    Stmt(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK) {
            std::string msg = std::string("prepare: ") + sqlite3_errmsg(db);
            throw Error(msg + " for SQL: " + sql);
        }
    }
    ~Stmt() { if (s) sqlite3_finalize(s); }
    Stmt(Stmt&& o) noexcept : s(o.s) { o.s = nullptr; }
    Stmt& operator=(Stmt&& o) noexcept {
        if (s) sqlite3_finalize(s);
        s = o.s;
        o.s = nullptr;
        return *this;
    }
    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
};

constexpr const char* kSchema = R"sql(
CREATE TABLE IF NOT EXISTS meta (
    key   TEXT PRIMARY KEY,
    value TEXT
);
-- plans is a dedup index only. The actual XML lives in
-- plan_snapshots.original_xml; what's stored here is a hash of the
-- normalized shape so writers detect "I've seen this plan before"
-- cheaply. Multiple plan_snapshots rows can reference the same
-- plan_id (same plan shape, captured at different moments).
CREATE TABLE IF NOT EXISTS plans (
    plan_id           INTEGER PRIMARY KEY AUTOINCREMENT,
    plan_hash         TEXT UNIQUE NOT NULL,
    uncompressed_size INTEGER NOT NULL
);
-- Per-version session item - one row per Get-Actual-Plan run, one row
-- per item in a converted pesession. Mirrors pesession's PeSessionMeta
-- + Intercerve.SqlServer.ConnectionParameters so reopening recovers
-- the connection identity (server, db, auth, version) without a live
-- handle.
CREATE TABLE IF NOT EXISTS items (
    item_id           INTEGER PRIMARY KEY,
    instance          TEXT,
    database          TEXT,
    login             TEXT,
    created_utc       TEXT,
    total_time        TEXT,
    actual_rows       INTEGER,
    plan_type         TEXT,
    batch_text        TEXT,
    index_analyzer_gz BLOB,
    auth_type         TEXT,
    server_version    TEXT
);
CREATE TABLE IF NOT EXISTS statements (
    item_id          INTEGER NOT NULL,
    statement_id     INTEGER NOT NULL,
    text             TEXT,
    plan_id          INTEGER REFERENCES plans(plan_id),
    sql_handle_hex   TEXT,
    plan_handle_hex  TEXT,
    query_hash       TEXT,
    plan_hash        TEXT,
    -- Call-chain provenance: sourced from StmtSimple/ParentObjectId
    -- in script captures, sp_statement_completed columns in live
    -- captures. 0 / empty when not captured.
    parent_object_id INTEGER NOT NULL DEFAULT 0,
    nest_level       INTEGER NOT NULL DEFAULT 0,
    object_name      TEXT,
    line_number      INTEGER NOT NULL DEFAULT 0,
    source_offset    INTEGER NOT NULL DEFAULT 0,
    PRIMARY KEY (item_id, statement_id)
);
CREATE INDEX IF NOT EXISTS idx_stmt_item ON statements(item_id);
CREATE INDEX IF NOT EXISTS idx_stmt_planhash ON statements(plan_hash);
CREATE TABLE IF NOT EXISTS runtime (
    item_id      INTEGER NOT NULL,
    statement_id INTEGER NOT NULL,
    node_id      INTEGER,
    ts_us        INTEGER,
    cpu_us       INTEGER,
    elapsed_us   INTEGER,
    reads        INTEGER,
    writes       INTEGER,
    row_count    INTEGER
);
CREATE INDEX IF NOT EXISTS idx_runtime_item_stmt
    ON runtime(item_id, statement_id);
-- Byte-exact original <ShowPlanXML> snapshot. plan_id is the deduped
-- shape from `plans`; original_xml is the snapshot the source capture
-- recorded (with per-sample RunTimeInformation / QueryTimeStats /
-- parameter values intact). Keeping both makes the conversion lossless:
-- `plan_xml` indexes the shape, `original_xml` lets us hand the original
-- bytes back if a caller needs them.
CREATE TABLE IF NOT EXISTS plan_snapshots (
    snapshot_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id       INTEGER NOT NULL,
    snapshot_idx  INTEGER NOT NULL,
    plan_id       INTEGER NOT NULL REFERENCES plans(plan_id),
    captured_us   INTEGER,
    original_xml  BLOB NOT NULL,
    original_size INTEGER NOT NULL,
    UNIQUE(item_id, snapshot_idx)
);
CREATE INDEX IF NOT EXISTS idx_plan_snapshots_item
    ON plan_snapshots(item_id, snapshot_idx);
-- One row per (item, statement) pair, with every TraceRowEx for that
-- pair packed into a deflated blob. Events whose PlanHandle didn't
-- resolve to a statement get statement_id = -1 and the reader falls
-- back to source-position attribution via trace_pos inside the blob.
-- Per-record layout is documented above the kTraceRecordSize constant
-- in file.cpp (116 bytes / record, little-endian, deflated with miniz).
-- A (item, statement) bucket may have many rows across the lifetime
-- of a write - live capture appends one per poll batch; the converter
-- writes one big row up front. The reader concatenates rows in
-- stream_id order to reconstruct chronology.
CREATE TABLE IF NOT EXISTS trace_streams (
    stream_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    item_id      INTEGER NOT NULL,
    statement_id INTEGER NOT NULL,    -- -1 if PlanHandle did not resolve
    event_count  INTEGER NOT NULL,
    events_blob  BLOB NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_trace_streams_item_stmt
    ON trace_streams(item_id, statement_id);
-- Resolves sp_statement_completed.object_id to "schema.name". Read
-- alongside trace_streams so a consumer can attach an object label
-- to each event.
CREATE TABLE IF NOT EXISTS objects (
    object_id INTEGER PRIMARY KEY,
    name      TEXT NOT NULL
);
-- Captured rowsets per statement per version (item). One row per
-- result set the statement emitted - a statement that produced no
-- rows (INSERT, DECLARE) has zero rows in this table. PK includes
-- item_id so two versions of the same script can't bleed into each
-- other. FK CASCADE keeps the file consistent if a row in items is
-- ever deleted.
--
-- columns_blob: length-prefixed UTF-8 column names (int32 len; bytes).
-- rows_blob:    miniz-deflated payload of length-prefixed UTF-8 cells,
--               row-major (int32 len; bytes). Length == -1 is reserved
--               for NULL but not currently emitted by writers.
CREATE TABLE IF NOT EXISTS statement_results (
    item_id      INTEGER NOT NULL,
    statement_id INTEGER NOT NULL,
    set_idx      INTEGER NOT NULL,
    column_count INTEGER NOT NULL,
    row_count    INTEGER NOT NULL,
    columns_blob BLOB NOT NULL,
    rows_blob    BLOB NOT NULL,
    PRIMARY KEY (item_id, statement_id, set_idx),
    FOREIGN KEY (item_id) REFERENCES items(item_id) ON DELETE CASCADE
);
CREATE INDEX IF NOT EXISTS idx_stmt_results_item_stmt
    ON statement_results(item_id, statement_id);
CREATE TABLE IF NOT EXISTS waits (
    item_id      INTEGER NOT NULL,
    statement_id INTEGER NOT NULL,
    wait_type    TEXT,
    duration_ms  INTEGER,
    signal_ms    INTEGER
);
CREATE INDEX IF NOT EXISTS idx_waits_item_stmt
    ON waits(item_id, statement_id);
)sql";

}  // namespace

struct File::Impl {
    sqlite3* db = nullptr;
    bool writable = false;       // true for Write / ReadWrite opens
    Stmt insert_plan;
    Stmt lookup_plan;
    Stmt insert_item;
    Stmt insert_stmt;
    Stmt insert_rt;
    Stmt insert_wait;
    Stmt insert_snap;
    Stmt insert_trace_stream;
    Stmt get_snap_xml;
    Stmt set_meta;
    Stmt get_meta;
    ~Impl() {
        if (db) {
            // Finalize prepared statements before any close - close
            // returns SQLITE_BUSY if statements are still active.
            insert_plan         = Stmt{};
            lookup_plan         = Stmt{};
            insert_item         = Stmt{};
            insert_stmt         = Stmt{};
            insert_rt           = Stmt{};
            insert_wait         = Stmt{};
            insert_snap         = Stmt{};
            insert_trace_stream = Stmt{};
            get_snap_xml        = Stmt{};
            set_meta            = Stmt{};
            get_meta            = Stmt{};
            // Only writable opens checkpoint. Read-only opens can't
            // touch the SHM/WAL files (SQLITE_IOERR) and don't need to:
            // any pending WAL would be checkpointed by whoever opened
            // the file in write mode.
            if (writable) {
                int rc = sqlite3_wal_checkpoint_v2(db, nullptr,
                    SQLITE_CHECKPOINT_TRUNCATE, nullptr, nullptr);
                if (rc != SQLITE_OK) {
                    std::fprintf(stderr,
                        "osession: wal checkpoint returned %d (%s)\n",
                        rc, sqlite3_errmsg(db));
                }
            }
            sqlite3_close(db);
            db = nullptr;
        }
    }
};

File::File(const std::string& path, Mode mode)
    : impl_(std::make_unique<Impl>()) {
    int flags = (mode == Mode::Read)
                  ? SQLITE_OPEN_READONLY
                  : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    impl_->writable = (mode != Mode::Read);
    int rc = sqlite3_open_v2(path.c_str(), &impl_->db, flags, nullptr);
    if (rc != SQLITE_OK) {
        std::string err = impl_->db ? sqlite3_errmsg(impl_->db) : "open failed";
        if (impl_->db) sqlite3_close(impl_->db);
        impl_->db = nullptr;
        throw Error("Cannot open " + path + ": " + err);
    }
    sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->db, "PRAGMA foreign_keys=ON;",  nullptr, nullptr, nullptr);
    sqlite3_exec(impl_->db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    if (mode != Mode::Read) {
        char* errmsg = nullptr;
        if (sqlite3_exec(impl_->db, kSchema, nullptr, nullptr, &errmsg)
                != SQLITE_OK) {
            std::string e = errmsg ? errmsg : "schema error";
            sqlite3_free(errmsg);
            throw Error("schema setup: " + e);
        }
        // Bootstrap rows that identify the writer + format. INSERT
        // OR IGNORE so opening an existing file is idempotent: if the
        // keys are already there, the writer's values aren't clobbered.
        std::string bootstrap =
            "INSERT OR IGNORE INTO meta(key,value) VALUES "
            "('app','calliper'),"
            "('format_version','" + std::to_string(kFormatVersion) + "');";
        sqlite3_exec(impl_->db, bootstrap.c_str(),
                     nullptr, nullptr, nullptr);
    }

    impl_->insert_plan = Stmt(impl_->db,
        "INSERT INTO plans(plan_hash, uncompressed_size) VALUES (?, ?)");
    impl_->lookup_plan = Stmt(impl_->db,
        "SELECT plan_id FROM plans WHERE plan_hash = ?");
    impl_->insert_item = Stmt(impl_->db,
        "INSERT OR REPLACE INTO items(item_id, instance, database, login, "
        "created_utc, total_time, actual_rows, plan_type, batch_text, "
        "index_analyzer_gz, auth_type, server_version) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    impl_->insert_stmt = Stmt(impl_->db,
        "INSERT OR REPLACE INTO statements(item_id, statement_id, text, "
        "plan_id, sql_handle_hex, plan_handle_hex, query_hash, plan_hash, "
        "parent_object_id, nest_level, object_name, line_number, "
        "source_offset) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    impl_->insert_rt = Stmt(impl_->db,
        "INSERT INTO runtime(item_id, statement_id, node_id, ts_us, cpu_us, "
        "elapsed_us, reads, writes, row_count) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    impl_->insert_wait = Stmt(impl_->db,
        "INSERT INTO waits(item_id, statement_id, wait_type, duration_ms, "
        "signal_ms) VALUES (?, ?, ?, ?, ?)");
    impl_->insert_snap = Stmt(impl_->db,
        "INSERT INTO plan_snapshots(item_id, snapshot_idx, plan_id, "
        "captured_us, original_xml, original_size) "
        "VALUES (?, ?, ?, ?, ?, ?)");
    impl_->insert_trace_stream = Stmt(impl_->db,
        "INSERT INTO trace_streams(item_id, statement_id, event_count, "
        "events_blob) VALUES (?, ?, ?, ?)");
    impl_->get_snap_xml = Stmt(impl_->db,
        "SELECT original_xml, original_size FROM plan_snapshots "
        "WHERE snapshot_id = ?");
    impl_->set_meta = Stmt(impl_->db,
        "INSERT OR REPLACE INTO meta(key, value) VALUES (?, ?)");
    impl_->get_meta = Stmt(impl_->db,
        "SELECT value FROM meta WHERE key = ?");
}

File::~File() = default;
File::File(File&&) noexcept = default;
File& File::operator=(File&&) noexcept = default;

int64_t File::add_plan(std::string_view plan_xml) {
    std::string hash = to_hex(fnv1a64(plan_xml));
    sqlite3_stmt* look = impl_->lookup_plan.s;
    sqlite3_reset(look);
    sqlite3_bind_text(look, 1, hash.c_str(),
                      static_cast<int>(hash.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(look) == SQLITE_ROW) {
        return sqlite3_column_int64(look, 0);
    }
    sqlite3_stmt* ins = impl_->insert_plan.s;
    sqlite3_reset(ins);
    sqlite3_bind_text(ins, 1, hash.c_str(),
                      static_cast<int>(hash.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(ins, 2, static_cast<int64_t>(plan_xml.size()));
    if (sqlite3_step(ins) != SQLITE_DONE) {
        throw Error(std::string("insert plan: ") + sqlite3_errmsg(impl_->db));
    }
    return sqlite3_last_insert_rowid(impl_->db);
}

void File::add_item(const ItemMeta& m) {
    sqlite3_stmt* s = impl_->insert_item.s;
    sqlite3_reset(s);
    sqlite3_bind_int  (s, 1, m.file_number);
    sqlite3_bind_text (s, 2, m.instance.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 3, m.database.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 4, m.login.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 5, m.created_utc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 6, m.total_time.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 7, m.actual_rows);
    char pt[2] = {m.plan_type, 0};
    sqlite3_bind_text (s, 8, pt, -1, SQLITE_TRANSIENT);
    if (m.batch_text.empty()) sqlite3_bind_null(s, 9);
    else sqlite3_bind_text (s, 9, m.batch_text.c_str(),
                            static_cast<int>(m.batch_text.size()),
                            SQLITE_TRANSIENT);
    if (m.index_analyzer_gz.empty()) sqlite3_bind_null(s, 10);
    else sqlite3_bind_blob (s, 10, m.index_analyzer_gz.data(),
                            static_cast<int>(m.index_analyzer_gz.size()),
                            SQLITE_TRANSIENT);
    if (m.auth_type.empty()) sqlite3_bind_null(s, 11);
    else sqlite3_bind_text (s, 11, m.auth_type.c_str(),
                            -1, SQLITE_TRANSIENT);
    if (m.server_version.empty()) sqlite3_bind_null(s, 12);
    else sqlite3_bind_text (s, 12, m.server_version.c_str(),
                            -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_DONE) {
        throw Error(std::string("insert item: ") + sqlite3_errmsg(impl_->db));
    }
}

void File::add_statement(const Statement& st) {
    sqlite3_stmt* s = impl_->insert_stmt.s;
    sqlite3_reset(s);
    sqlite3_bind_int  (s, 1, st.item);
    sqlite3_bind_int  (s, 2, st.statement_id);
    sqlite3_bind_text (s, 3, st.text.c_str(),            -1, SQLITE_TRANSIENT);
    if (st.plan_id > 0) sqlite3_bind_int64(s, 4, st.plan_id);
    else                sqlite3_bind_null(s, 4);
    sqlite3_bind_text (s, 5, st.sql_handle_hex.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 6, st.plan_handle_hex.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 7, st.query_hash.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text (s, 8, st.plan_hash.c_str(),       -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 9,  st.parent_object_id);
    sqlite3_bind_int  (s, 10, st.nest_level);
    sqlite3_bind_text (s, 11, st.object_name.c_str(),    -1, SQLITE_TRANSIENT);
    sqlite3_bind_int  (s, 12, st.line_number);
    sqlite3_bind_int64(s, 13, st.source_offset);
    if (sqlite3_step(s) != SQLITE_DONE) {
        throw Error(std::string("insert statement: ") +
                    sqlite3_errmsg(impl_->db));
    }
}

void File::add_runtime(const RuntimeSample& r) {
    sqlite3_stmt* s = impl_->insert_rt.s;
    sqlite3_reset(s);
    sqlite3_bind_int  (s, 1, r.item);
    sqlite3_bind_int  (s, 2, r.statement_id);
    sqlite3_bind_int  (s, 3, r.node_id);
    sqlite3_bind_int64(s, 4, r.ts_us);
    sqlite3_bind_int64(s, 5, r.cpu_us);
    sqlite3_bind_int64(s, 6, r.elapsed_us);
    sqlite3_bind_int64(s, 7, r.reads);
    sqlite3_bind_int64(s, 8, r.writes);
    sqlite3_bind_int64(s, 9, r.row_count);
    if (sqlite3_step(s) != SQLITE_DONE) {
        throw Error(std::string("insert runtime: ") +
                    sqlite3_errmsg(impl_->db));
    }
}

int64_t File::add_plan_snapshot(PlanSnapshot snap,
                                std::string_view original_xml) {
    std::string comp = deflate_compress(original_xml);
    sqlite3_stmt* s = impl_->insert_snap.s;
    sqlite3_reset(s);
    sqlite3_bind_int  (s, 1, snap.item);
    sqlite3_bind_int  (s, 2, snap.snapshot_idx);
    sqlite3_bind_int64(s, 3, snap.plan_id);
    sqlite3_bind_int64(s, 4, snap.captured_us);
    sqlite3_bind_blob (s, 5, comp.data(),
                       static_cast<int>(comp.size()), SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 6, static_cast<int64_t>(original_xml.size()));
    if (sqlite3_step(s) != SQLITE_DONE) {
        throw Error(std::string("insert plan_snapshot: ") +
                    sqlite3_errmsg(impl_->db));
    }
    return sqlite3_last_insert_rowid(impl_->db);
}

namespace {

// Trace-event record layout, written little-endian byte-by-byte so the
// format is endian-independent at rest. Layout (116 bytes / record):
//   int32 trace_pos
//   int64 ts_us, cpu_us, elapsed_us
//   int64 udf_cpu_us, udf_elapsed_us
//   int64 reads, writes, row_count
//   uint64 start_dt_raw, end_dt_raw         (.NET DateTime raw bits)
//   int64 object_id                          (sp_statement_completed)
//   int32 nest_level, line_number
//   int64 offset_bytes, offset_end_bytes
// trace_pos(4) + 8 int64 metrics + 2 int64 dt + object_id(8)
// + nest(4) + line(4) + offset(8) + offset_end(8) = 116.
constexpr size_t kTraceRecordSize = 4 + 8 * 8 + 8 * 2 + 8 + 4 + 4 + 8 + 8;

void put_le32(std::string& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(v >> (8 * i)));
}
void put_le64(std::string& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>(v >> (8 * i)));
}
uint32_t get_le32(const unsigned char* p) {
    return uint32_t(p[0])      | uint32_t(p[1]) << 8
         | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t get_le64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

std::string serialize_trace_batch(const std::vector<TraceEvent>& evs) {
    std::string out;
    out.reserve(evs.size() * kTraceRecordSize);
    for (const auto& e : evs) {
        put_le32(out, static_cast<uint32_t>(e.trace_pos));
        put_le64(out, static_cast<uint64_t>(e.ts_us));
        put_le64(out, static_cast<uint64_t>(e.cpu_us));
        put_le64(out, static_cast<uint64_t>(e.elapsed_us));
        put_le64(out, static_cast<uint64_t>(e.udf_cpu_us));
        put_le64(out, static_cast<uint64_t>(e.udf_elapsed_us));
        put_le64(out, static_cast<uint64_t>(e.reads));
        put_le64(out, static_cast<uint64_t>(e.writes));
        put_le64(out, static_cast<uint64_t>(e.row_count));
        put_le64(out, e.start_dt_raw);
        put_le64(out, e.end_dt_raw);
        put_le64(out, static_cast<uint64_t>(e.object_id));
        put_le32(out, static_cast<uint32_t>(e.nest_level));
        put_le32(out, static_cast<uint32_t>(e.line_number));
        put_le64(out, static_cast<uint64_t>(e.offset_bytes));
        put_le64(out, static_cast<uint64_t>(e.offset_end_bytes));
    }
    return out;
}

void deserialize_trace_batch(std::string_view raw,
                             int item, int statement_id,
                             std::vector<TraceEvent>& out) {
    if (raw.empty() || raw.size() % kTraceRecordSize != 0) return;
    const size_t n = raw.size() / kTraceRecordSize;
    out.reserve(out.size() + n);
    const unsigned char* p = reinterpret_cast<const unsigned char*>(raw.data());
    for (size_t i = 0; i < n; ++i, p += kTraceRecordSize) {
        TraceEvent e;
        e.item            = item;
        e.statement_id    = statement_id;
        e.trace_pos       = static_cast<int32_t>(get_le32(p));
        e.ts_us           = static_cast<int64_t>(get_le64(p + 4));
        e.cpu_us          = static_cast<int64_t>(get_le64(p + 12));
        e.elapsed_us      = static_cast<int64_t>(get_le64(p + 20));
        e.udf_cpu_us      = static_cast<int64_t>(get_le64(p + 28));
        e.udf_elapsed_us  = static_cast<int64_t>(get_le64(p + 36));
        e.reads           = static_cast<int64_t>(get_le64(p + 44));
        e.writes          = static_cast<int64_t>(get_le64(p + 52));
        e.row_count       = static_cast<int64_t>(get_le64(p + 60));
        e.start_dt_raw    = get_le64(p + 68);
        e.end_dt_raw      = get_le64(p + 76);
        e.object_id        = static_cast<int64_t>(get_le64(p + 84));
        e.nest_level       = static_cast<int32_t>(get_le32(p + 92));
        e.line_number      = static_cast<int32_t>(get_le32(p + 96));
        e.offset_bytes     = static_cast<int64_t>(get_le64(p + 100));
        e.offset_end_bytes = static_cast<int64_t>(get_le64(p + 108));
        out.push_back(std::move(e));
    }
}

}  // namespace

void File::add_trace_events(int item_id, int statement_id,
                            const std::vector<TraceEvent>& events) {
    if (events.empty()) return;
    std::string raw = serialize_trace_batch(events);
    std::string comp = deflate_compress(raw);
    sqlite3_stmt* s = impl_->insert_trace_stream.s;
    sqlite3_reset(s);
    sqlite3_bind_int  (s, 1, item_id);
    sqlite3_bind_int  (s, 2, statement_id);
    sqlite3_bind_int64(s, 3, static_cast<int64_t>(events.size()));
    sqlite3_bind_blob (s, 4, comp.data(),
                       static_cast<int>(comp.size()), SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_DONE) {
        throw Error(std::string("insert trace_stream: ") +
                    sqlite3_errmsg(impl_->db));
    }
}

void File::add_wait(const WaitAggregate& w) {
    sqlite3_stmt* s = impl_->insert_wait.s;
    sqlite3_reset(s);
    sqlite3_bind_int  (s, 1, w.item);
    sqlite3_bind_int  (s, 2, w.statement_id);
    sqlite3_bind_text (s, 3, w.wait_type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(s, 4, w.duration_ms);
    sqlite3_bind_int64(s, 5, w.signal_ms);
    if (sqlite3_step(s) != SQLITE_DONE) {
        throw Error(std::string("insert wait: ") +
                    sqlite3_errmsg(impl_->db));
    }
}

void File::add_object_name(int64_t object_id, const std::string& name) {
    if (object_id == 0 || name.empty()) return;
    // INSERT OR IGNORE - first writer wins. Names don't change for a
    // given object_id within a capture session.
    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(impl_->db,
        "INSERT OR IGNORE INTO objects(object_id, name) VALUES (?, ?)",
        -1, &s, nullptr);
    if (!s) return;
    sqlite3_bind_int64(s, 1, object_id);
    sqlite3_bind_text (s, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

namespace {
// Length-prefixed UTF-8 strings, packed back to back.
// Per cell: int32 little-endian length followed by N bytes.
// Length == -1 reserved for NULL; writers don't emit it yet.
void put_lp_string(std::string& out, const std::string& s) {
    int32_t n = static_cast<int32_t>(s.size());
    char buf[4];
    for (int i = 0; i < 4; ++i) buf[i] = static_cast<char>(n >> (8 * i));
    out.append(buf, 4);
    out.append(s);
}

bool read_lp_string(const unsigned char* p, size_t total, size_t& pos,
                     std::string& out) {
    if (pos + 4 > total) return false;
    int32_t n = static_cast<int32_t>(
        uint32_t(p[pos])       | uint32_t(p[pos + 1]) << 8
      | uint32_t(p[pos + 2]) << 16 | uint32_t(p[pos + 3]) << 24);
    pos += 4;
    if (n < 0) {
        // NULL sentinel - surfaced as empty string for now.
        out.clear();
        return true;
    }
    if (pos + static_cast<size_t>(n) > total) return false;
    out.assign(reinterpret_cast<const char*>(p + pos),
                static_cast<size_t>(n));
    pos += static_cast<size_t>(n);
    return true;
}
}  // namespace

void File::add_result_set(const ResultSetData& rs) {
    if (rs.column_count <= 0) return;  // nothing useful to store
    std::string cols_blob;
    cols_blob.reserve(rs.column_names.size() * 32);
    for (const auto& n : rs.column_names) put_lp_string(cols_blob, n);

    std::string rows_raw;
    rows_raw.reserve(rs.rows.size() * 16);
    for (const auto& cell : rs.rows) put_lp_string(rows_raw, cell);
    // Deflate the rows blob; large result sets compress 5-10x. The
    // column blob is small and doesn't usefully compress.
    std::string rows_blob = deflate_compress(rows_raw);

    sqlite3_stmt* s = nullptr;
    sqlite3_prepare_v2(impl_->db,
        "INSERT OR REPLACE INTO statement_results"
        "(item_id, statement_id, set_idx, column_count, row_count, "
        "columns_blob, rows_blob) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)",
        -1, &s, nullptr);
    if (!s) {
        throw Error(std::string("prepare add_result_set: ") +
                    sqlite3_errmsg(impl_->db));
    }
    sqlite3_bind_int  (s, 1, rs.item);
    sqlite3_bind_int  (s, 2, rs.statement_id);
    sqlite3_bind_int  (s, 3, rs.set_idx);
    sqlite3_bind_int  (s, 4, rs.column_count);
    sqlite3_bind_int  (s, 5, rs.row_count);
    sqlite3_bind_blob (s, 6, cols_blob.data(),
                       static_cast<int>(cols_blob.size()),
                       SQLITE_TRANSIENT);
    sqlite3_bind_blob (s, 7, rows_blob.data(),
                       static_cast<int>(rows_blob.size()),
                       SQLITE_TRANSIENT);
    int rc = sqlite3_step(s);
    sqlite3_finalize(s);
    if (rc != SQLITE_DONE) {
        throw Error(std::string("insert statement_results: ") +
                    sqlite3_errmsg(impl_->db));
    }
}

void File::begin_transaction() {
    sqlite3_exec(impl_->db, "BEGIN TRANSACTION", nullptr, nullptr, nullptr);
}

void File::commit() {
    sqlite3_exec(impl_->db, "COMMIT", nullptr, nullptr, nullptr);
}

namespace {
std::string col_text(sqlite3_stmt* s, int c) {
    const unsigned char* t = sqlite3_column_text(s, c);
    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string{};
}
}

std::vector<ItemMeta> File::items() const {
    std::vector<ItemMeta> out;
    Stmt q(impl_->db,
        "SELECT item_id, instance, database, login, created_utc, "
        "total_time, actual_rows, plan_type, batch_text, "
        "index_analyzer_gz, auth_type, server_version "
        "FROM items ORDER BY item_id");
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        ItemMeta m;
        m.file_number = sqlite3_column_int(q.s, 0);
        m.instance    = col_text(q.s, 1);
        m.database    = col_text(q.s, 2);
        m.login       = col_text(q.s, 3);
        m.created_utc = col_text(q.s, 4);
        m.total_time  = col_text(q.s, 5);
        m.actual_rows = sqlite3_column_int64(q.s, 6);
        std::string pt = col_text(q.s, 7);
        m.plan_type   = pt.empty() ? '?' : pt[0];
        m.batch_text  = col_text(q.s, 8);
        const void* gz = sqlite3_column_blob(q.s, 9);
        int gz_size = sqlite3_column_bytes(q.s, 9);
        if (gz && gz_size > 0) {
            m.index_analyzer_gz.assign(static_cast<const char*>(gz),
                                       static_cast<size_t>(gz_size));
        }
        m.auth_type      = col_text(q.s, 10);
        m.server_version = col_text(q.s, 11);
        out.push_back(std::move(m));
    }
    return out;
}

std::vector<Statement> File::statements(int item) const {
    std::vector<Statement> out;
    Stmt q(impl_->db,
        "SELECT item_id, statement_id, text, plan_id, sql_handle_hex, "
        "plan_handle_hex, query_hash, plan_hash, parent_object_id, "
        "nest_level, object_name, line_number, source_offset "
        "FROM statements WHERE item_id = ? ORDER BY statement_id");
    sqlite3_bind_int(q.s, 1, item);
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        Statement st;
        st.item              = sqlite3_column_int  (q.s, 0);
        st.statement_id      = sqlite3_column_int  (q.s, 1);
        st.text              = col_text(q.s, 2);
        st.plan_id           = sqlite3_column_int64(q.s, 3);
        st.sql_handle_hex    = col_text(q.s, 4);
        st.plan_handle_hex   = col_text(q.s, 5);
        st.query_hash        = col_text(q.s, 6);
        st.plan_hash         = col_text(q.s, 7);
        st.parent_object_id  = sqlite3_column_int64(q.s, 8);
        st.nest_level        = sqlite3_column_int  (q.s, 9);
        st.object_name       = col_text(q.s, 10);
        st.line_number       = sqlite3_column_int  (q.s, 11);
        st.source_offset     = sqlite3_column_int64(q.s, 12);
        out.push_back(std::move(st));
    }
    return out;
}

std::vector<RuntimeSample> File::runtime(int item, int statement_id) const {
    std::vector<RuntimeSample> out;
    std::string sql =
        "SELECT item_id, statement_id, node_id, ts_us, cpu_us, elapsed_us, "
        "reads, writes, row_count FROM runtime WHERE item_id = ?";
    if (statement_id >= 0) sql += " AND statement_id = ?";
    sql += " ORDER BY ts_us, node_id";
    Stmt q(impl_->db, sql.c_str());
    sqlite3_bind_int(q.s, 1, item);
    if (statement_id >= 0) sqlite3_bind_int(q.s, 2, statement_id);
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        RuntimeSample r;
        r.item         = sqlite3_column_int  (q.s, 0);
        r.statement_id = sqlite3_column_int  (q.s, 1);
        r.node_id      = sqlite3_column_int  (q.s, 2);
        r.ts_us        = sqlite3_column_int64(q.s, 3);
        r.cpu_us       = sqlite3_column_int64(q.s, 4);
        r.elapsed_us   = sqlite3_column_int64(q.s, 5);
        r.reads        = sqlite3_column_int64(q.s, 6);
        r.writes       = sqlite3_column_int64(q.s, 7);
        r.row_count    = sqlite3_column_int64(q.s, 8);
        out.push_back(r);
    }
    return out;
}

std::vector<PlanSnapshot> File::plan_snapshots(int item) const {
    std::vector<PlanSnapshot> out;
    Stmt q(impl_->db,
        "SELECT snapshot_id, item_id, snapshot_idx, plan_id, captured_us "
        "FROM plan_snapshots WHERE item_id = ? ORDER BY snapshot_idx");
    sqlite3_bind_int(q.s, 1, item);
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        PlanSnapshot s;
        s.snapshot_id  = sqlite3_column_int64(q.s, 0);
        s.item         = sqlite3_column_int  (q.s, 1);
        s.snapshot_idx = sqlite3_column_int  (q.s, 2);
        s.plan_id      = sqlite3_column_int64(q.s, 3);
        s.captured_us  = sqlite3_column_int64(q.s, 4);
        out.push_back(s);
    }
    return out;
}

std::string File::plan_snapshot_xml(int64_t snapshot_id) const {
    sqlite3_stmt* s = impl_->get_snap_xml.s;
    sqlite3_reset(s);
    sqlite3_bind_int64(s, 1, snapshot_id);
    if (sqlite3_step(s) != SQLITE_ROW) return {};
    const void* blob = sqlite3_column_blob(s, 0);
    int blob_size = sqlite3_column_bytes(s, 0);
    return deflate_decompress(std::string_view(
        static_cast<const char*>(blob), static_cast<size_t>(blob_size)));
}

std::vector<TraceEvent> File::trace_events(int item, int statement_id) const {
    std::vector<TraceEvent> out;
    std::string sql =
        "SELECT statement_id, event_count, events_blob FROM trace_streams "
        "WHERE item_id = ?";
    if (statement_id >= 0) sql += " AND statement_id = ?";
    // ORDER BY (statement_id, stream_id) so chunks for the same
    // statement come back in insertion order - preserves chronology
    // for the live-capture append pattern.
    sql += " ORDER BY statement_id, stream_id";
    Stmt q(impl_->db, sql.c_str());
    sqlite3_bind_int(q.s, 1, item);
    if (statement_id >= 0) sqlite3_bind_int(q.s, 2, statement_id);
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        int sid = sqlite3_column_int(q.s, 0);
        const void* blob = sqlite3_column_blob(q.s, 2);
        int blob_size = sqlite3_column_bytes(q.s, 2);
        std::string raw = deflate_decompress(std::string_view(
            static_cast<const char*>(blob),
            static_cast<size_t>(blob_size)));
        deserialize_trace_batch(raw, item, sid, out);
    }
    return out;
}

std::vector<WaitAggregate> File::waits(int item, int statement_id) const {
    std::vector<WaitAggregate> out;
    std::string sql =
        "SELECT item_id, statement_id, wait_type, duration_ms, signal_ms "
        "FROM waits WHERE item_id = ?";
    if (statement_id >= 0) sql += " AND statement_id = ?";
    sql += " ORDER BY duration_ms DESC";
    Stmt q(impl_->db, sql.c_str());
    sqlite3_bind_int(q.s, 1, item);
    if (statement_id >= 0) sqlite3_bind_int(q.s, 2, statement_id);
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        WaitAggregate w;
        w.item         = sqlite3_column_int  (q.s, 0);
        w.statement_id = sqlite3_column_int  (q.s, 1);
        w.wait_type    = col_text(q.s, 2);
        w.duration_ms  = sqlite3_column_int64(q.s, 3);
        w.signal_ms    = sqlite3_column_int64(q.s, 4);
        out.push_back(std::move(w));
    }
    return out;
}

std::vector<std::pair<int64_t, std::string>> File::object_names() const {
    std::vector<std::pair<int64_t, std::string>> out;
    Stmt q(impl_->db, "SELECT object_id, name FROM objects");
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(q.s, 0);
        std::string n = col_text(q.s, 1);
        if (id != 0 && !n.empty()) out.emplace_back(id, std::move(n));
    }
    return out;
}

std::vector<File::ResultSetData> File::result_sets(
        int item, int statement_id) const {
    std::vector<ResultSetData> out;
    std::string sql =
        "SELECT statement_id, set_idx, column_count, row_count, "
        "columns_blob, rows_blob "
        "FROM statement_results WHERE item_id = ?";
    if (statement_id >= 0) sql += " AND statement_id = ?";
    sql += " ORDER BY statement_id, set_idx";
    Stmt q(impl_->db, sql.c_str());
    sqlite3_bind_int(q.s, 1, item);
    if (statement_id >= 0) sqlite3_bind_int(q.s, 2, statement_id);
    while (sqlite3_step(q.s) == SQLITE_ROW) {
        ResultSetData rs;
        rs.item         = item;
        rs.statement_id = sqlite3_column_int(q.s, 0);
        rs.set_idx      = sqlite3_column_int(q.s, 1);
        rs.column_count = sqlite3_column_int(q.s, 2);
        rs.row_count    = sqlite3_column_int(q.s, 3);
        const void* cb  = sqlite3_column_blob(q.s, 4);
        int cb_n        = sqlite3_column_bytes(q.s, 4);
        const void* rb  = sqlite3_column_blob(q.s, 5);
        int rb_n        = sqlite3_column_bytes(q.s, 5);

        // Columns blob - never compressed; small.
        if (cb && cb_n > 0) {
            const unsigned char* p = static_cast<const unsigned char*>(cb);
            size_t pos = 0;
            rs.column_names.reserve(rs.column_count);
            for (int c = 0; c < rs.column_count; ++c) {
                std::string name;
                if (!read_lp_string(p, static_cast<size_t>(cb_n), pos, name))
                    break;
                rs.column_names.push_back(std::move(name));
            }
        }

        std::string raw;
        if (rb && rb_n > 0) {
            raw = deflate_decompress(std::string_view(
                static_cast<const char*>(rb), static_cast<size_t>(rb_n)));
        }
        const size_t total_cells = static_cast<size_t>(rs.row_count) *
                                    static_cast<size_t>(rs.column_count);
        rs.rows.reserve(total_cells);
        if (!raw.empty()) {
            const unsigned char* p =
                reinterpret_cast<const unsigned char*>(raw.data());
            size_t pos = 0;
            for (size_t i = 0; i < total_cells; ++i) {
                std::string cell;
                if (!read_lp_string(p, raw.size(), pos, cell)) break;
                rs.rows.push_back(std::move(cell));
            }
        }
        out.push_back(std::move(rs));
    }
    return out;
}

std::string File::meta(const std::string& key) const {
    sqlite3_stmt* s = impl_->get_meta.s;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW) {
        return col_text(s, 0);
    }
    return {};
}

void File::set_meta(const std::string& key, const std::string& value) {
    sqlite3_stmt* s = impl_->set_meta.s;
    sqlite3_reset(s);
    sqlite3_bind_text(s, 1, key.c_str(),   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) != SQLITE_DONE) {
        throw Error(std::string("set_meta: ") + sqlite3_errmsg(impl_->db));
    }
}

}  // namespace osession
