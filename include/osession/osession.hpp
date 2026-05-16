// SPDX-License-Identifier: MIT
// libosession - compact, queryable container for SQL Server plan / runtime
// capture data. A .osession file is a SQLite database with a fixed schema
// optimised for the openplan / Plan Explorer use case: plan-XML dedup,
// indexed runtime samples, queryable wait stats.
//
// Designed as a replacement for .pesession's NRBF blobs - same data, ~10x
// smaller and random-accessible per item/statement.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace osession {

// Bumped whenever the on-disk schema changes in a way readers need to
// notice. Writers stamp this into meta('format_version') at create.
// Readers consult meta('format_version') and decide whether they can
// handle the file (forward-compatible reads on the same major version,
// hard fail on a newer major, soft warn on a newer minor).
inline constexpr int kFormatVersion = 1;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Mirrors .pesession's PeSessionMeta. batch_text is the SQL the user
// submitted that produced this item's plans (the original script,
// preserved for display). index_analyzer_gz is the gzipped JSON
// payload from QueryAnalyzerInput.IndexAnalyzerResultsGz, stored
// verbatim so the consumer decompresses + parses on demand.
struct ItemMeta {
    int file_number = 0;
    std::string instance;
    std::string database;
    std::string login;
    std::string created_utc;
    std::string total_time;
    int64_t actual_rows = 0;
    char plan_type = '?';
    std::string batch_text;
    std::string index_analyzer_gz;
    // Connection-level identity. Captured at the time the version
    // ran, so reopening recovers the original auth method + server
    // build without a live handle.
    std::string auth_type;        // "Integrated" / "SQL Auth" / ...
    std::string server_version;   // @@VERSION or pesession's _Version
};

struct Statement {
    int item = 0;
    int statement_id = 0;
    std::string text;
    int64_t plan_id = 0;            // FK into the plans table
    std::string sql_handle_hex;
    std::string plan_handle_hex;
    std::string query_hash;
    std::string plan_hash;
    // Call-chain provenance. Sourced from StmtSimple/ParentObjectId
    // for script captures, sp_statement_completed columns for live
    // captures. Zero / empty when not captured.
    int64_t parent_object_id = 0;
    int     nest_level = 0;
    std::string object_name;
    int     line_number = 0;
    int64_t source_offset = 0;
};

struct RuntimeSample {
    int item = 0;
    int statement_id = 0;
    int node_id = -1;
    int64_t ts_us = 0;              // optional capture timestamp
    int64_t cpu_us = 0;
    int64_t elapsed_us = 0;
    int64_t reads = 0;
    int64_t writes = 0;
    int64_t row_count = 0;
};

struct WaitAggregate {
    int item = 0;
    int statement_id = 0;
    std::string wait_type;
    int64_t duration_ms = 0;
    int64_t signal_ms = 0;
};

// One captured <ShowPlanXML> instance. The plans table deduplicates by
// the shape of the XML; plan_snapshots preserves every original snapshot
// so the source .pesession can be reconstructed without loss.
struct PlanSnapshot {
    int64_t snapshot_id = 0;        // set by add_plan_snapshot on insert
    int item = 0;
    int snapshot_idx = 0;           // position within this item's blocks
    int64_t plan_id = 0;            // FK into plans (deduped shape)
    int64_t captured_us = 0;        // optional source timestamp
};

// One TraceRowEx event from a .queryanalysis NRBF stream - kept
// verbatim so per-execution analysis is possible; consumers can
// aggregate to per-statement at read time.
//
// trace_pos is the event's position in its source item's trace stream,
// preserved so positional-fallback attribution (when PlanHandle didn't
// resolve to a statement) survives the round-trip into per-statement
// blobs.
struct TraceEvent {
    int item = 0;
    int statement_id = -1;
    int32_t trace_pos = 0;
    int64_t ts_us = 0;
    int64_t cpu_us = 0;
    int64_t elapsed_us = 0;
    int64_t udf_cpu_us = 0;
    int64_t udf_elapsed_us = 0;
    int64_t reads = 0;
    int64_t writes = 0;
    int64_t row_count = 0;
    // Raw .NET DateTime bits from the source TraceRowEx (StartTimeUtc /
    // EndTimeUtc). Top 2 bits are the DateTimeKind, lower 62 are Ticks
    // (100ns since year 1). 0 = not captured. Stored as raw bits so
    // every consumer applies the same conversion.
    uint64_t start_dt_raw = 0;
    uint64_t end_dt_raw   = 0;
    // Provenance fields from sp_statement_completed (live XE capture)
    // or TraceRowEx (pesession). object_id resolves to "schema.name"
    // via the `objects` side-table - keeps this blob fixed-size.
    int64_t object_id        = 0;
    int32_t nest_level       = 0;
    int32_t line_number      = 0;
    int64_t offset_bytes     = 0;
    int64_t offset_end_bytes = 0;
};

class File {
public:
    enum class Mode { Read, Write, ReadWrite };

    File(const std::string& path, Mode mode);
    ~File();
    File(File&&) noexcept;
    File& operator=(File&&) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    // ── Writers ────────────────────────────────────────────────────────────
    // Register (or dedup, by FNV-1a hash of the normalized text) a plan
    // shape and return its row id. The XML itself is NOT stored here -
    // only the hash + original byte length. Use add_plan_snapshot for
    // byte-exact storage of the captured XML.
    int64_t add_plan(std::string_view normalized_plan_xml);

    // Record a captured XML snapshot with byte-exact original text, so
    // the source .pesession can be reconstructed without loss. `plan_id`
    // is the row from add_plan() that holds the deduped shape.
    int64_t add_plan_snapshot(PlanSnapshot snap,
                              std::string_view original_xml);

    void add_item(const ItemMeta& meta);
    void add_statement(const Statement& stmt);
    void add_runtime(const RuntimeSample& sample);

    // Write all trace events for one (item, statement) pair as a single
    // deflated blob in trace_streams. Callers buffer events per
    // (item, statement) bucket and flush here. statement_id == -1 is
    // reserved for events whose PlanHandle could not be resolved at
    // conversion time.
    void add_trace_events(int item_id, int statement_id,
                          const std::vector<TraceEvent>& events);

    void add_wait(const WaitAggregate& wait);

    // Resolves a sys.objects.object_id to "schema.name". Idempotent -
    // safe to call repeatedly with the same id (first writer wins).
    void add_object_name(int64_t object_id, const std::string& name);

    // One captured result set, written per (item, statement, set_idx).
    // `rows` is row-major flat: rows[r*column_count + c]. NULL cells
    // are currently stored as empty strings - the on-disk format
    // reserves a sentinel (length == -1) for future NULL preservation.
    struct ResultSetData {
        int item = 0;
        int statement_id = 0;
        int set_idx = 0;
        int column_count = 0;
        int row_count    = 0;
        std::vector<std::string> column_names;
        std::vector<std::string> rows;
    };
    void add_result_set(const ResultSetData& rs);

    // Wrap a batch of add_* calls in a single transaction for speed.
    void begin_transaction();
    void commit();

    // ── Readers ────────────────────────────────────────────────────────────
    std::vector<ItemMeta> items() const;
    std::vector<Statement> statements(int item) const;
    // Snapshots captured for an item, ordered by snapshot_idx.
    std::vector<PlanSnapshot> plan_snapshots(int item) const;
    // Byte-exact original XML for a snapshot, or empty if unknown.
    std::string plan_snapshot_xml(int64_t snapshot_id) const;
    // Pass statement_id < 0 for "all statements in the item".
    std::vector<RuntimeSample> runtime(int item, int statement_id = -1) const;
    // Expands the deflated trace_streams blobs into TraceEvent records.
    // Pass statement_id < 0 for "all statements". The returned vector
    // is ordered by (statement_id, trace_pos).
    std::vector<TraceEvent> trace_events(int item, int statement_id = -1) const;
    std::vector<WaitAggregate> waits(int item, int statement_id = -1) const;

    // (object_id -> "schema.name") map written by add_object_name.
    std::vector<std::pair<int64_t, std::string>> object_names() const;

    // All captured rowsets for an item, optionally filtered to one
    // statement. Returns them sorted by (statement_id, set_idx) so
    // the order on disk = order at capture time.
    std::vector<ResultSetData> result_sets(int item,
                                            int statement_id = -1) const;

    // Misc
    std::string meta(const std::string& key) const;
    void set_meta(const std::string& key, const std::string& value);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace osession
