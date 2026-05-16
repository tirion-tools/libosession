# libosession

Compact, queryable container for SQL Server plan / runtime capture data. A `.osession` file is a single SQLite database with a fixed schema designed for the openplan / Plan Explorer use case.

Replaces `.pesession`'s NRBF blobs with:

- **Plan-XML dedup** by hash. Long captures that snapshot the same plan repeatedly collapse to one row in `plans`.
- **Lossless round-trip.** Every captured `<ShowPlanXML>` block is preserved byte-exact in `plan_snapshots.original_xml` (deflated), so re-emitting the source `.pesession` is possible without information loss.
- **Indexed rows** for items / statements / runtime samples / waits via SQLite. Random access on `(item, statement)` is O(log n) instead of "stream-parse the whole NRBF blob".
- **deflate-compressed payloads** (plan XML, trace-event blobs, result rowsets) via vendored miniz.
- **WAL mode** so live capture can stream rows in without rewriting the file.
- **Inspectable**: `sqlite3 myfile.osession 'select count(*) from runtime'`.
- **Foreign-key cascades.** Deleting an item via `items.item_id` removes its rowsets, plan_snapshots, statements, runtime samples, trace_streams and waits.

## Schema

| table               | role                                                                                       |
|---------------------|--------------------------------------------------------------------------------------------|
| `meta`              | key/value: `app` marker, `format_version`, source path, capture time stamps                |
| `plans`             | deduped plan-shape index keyed by FNV-1a 64-bit hash of the normalized text                |
| `plan_snapshots`    | byte-exact original `<ShowPlanXML>` per item (deflated). FK → `plans.plan_id`              |
| `items`             | one row per session item / per Get-Actual-Plan "version". Identity + auth + server version |
| `statements`        | `(item, statement_id)`: text, plan FK, hashes, **call-chain provenance**                   |
| `statement_results` | captured rowsets per `(item, statement, set_idx)`. FK → `items` with CASCADE               |
| `runtime`           | per-`(item, statement, node, ts)` cpu / elapsed / IO / rows                                |
| `trace_streams`     | deflated TraceRowEx blobs keyed by `(item, statement)`. Statement -1 = unresolved          |
| `objects`           | `object_id → "schema.name"` lookup table for trace-event labelling                         |
| `waits`             | per-`(item, statement)` wait_type + duration + signal                                      |

Indexes on `(item_id, statement_id)` for runtime / waits / trace_streams / statement_results, `plan_hash` on statements for cross-item plan history, `(item_id, snapshot_idx)` on plan_snapshots.

### Connection identity on `items`
Mirrors pesession's `Intercerve.SqlServer.ConnectionParameters`: `instance`, `database`, `login`, plus `auth_type` ("Integrated" / "SQL Auth") and `server_version` (the `@@VERSION` string). Captured at the time the version ran so reopening the file recovers what the consumer was connected to without a live handle.

### Call-chain provenance on `statements`
`parent_object_id` (sys.objects.object_id of the emitting module), `nest_level`, `object_name`, `line_number`, `source_offset`. Enables "at: schema.proc, Line: N, Nest Level: M, Start Offset: X" call-chain display the same way SQL Sentry shows it.

### TraceEvent record layout (`trace_streams.events_blob`)
116-byte fixed records, little-endian, miniz-deflated:

```
int32  trace_pos
int64  ts_us cpu_us elapsed_us
int64  udf_cpu_us udf_elapsed_us
int64  reads writes row_count
uint64 start_dt_raw end_dt_raw      (.NET DateTime raw bits, top 2 = Kind)
int64  object_id                    (sp_statement_completed.object_id)
int32  nest_level line_number
int64  offset_bytes offset_end_bytes
```

### Result-set blobs (`statement_results.rows_blob`)
Length-prefixed UTF-8 cells, row-major, miniz-deflated. `int32 -1` is reserved for NULL; writers don't emit it yet (NULL stored as empty string today).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

System dependency: `libsqlite3-dev` on Linux (`sudo apt install libsqlite3-dev`). On Windows the installer bundles `sqlite3.dll`.

## Use from CMake

```cmake
add_subdirectory(extern/libosession)
target_link_libraries(myapp PRIVATE osession::osession)
```

```cpp
#include <osession/osession.hpp>

osession::File f("session.osession", osession::File::Mode::Write);
f.begin_transaction();

// Plan shape (deduped) + original XML.
int64_t plan_id = f.add_plan(normalized_xml);
osession::PlanSnapshot snap{};
snap.item = 1;
snap.plan_id = plan_id;
f.add_plan_snapshot(snap, original_xml);

// Per-version identity (everything pesession's ConnectionParameters
// carried, plus the user's submitted batch text).
osession::ItemMeta meta{};
meta.file_number    = 1;
meta.instance       = "PRODSQL";
meta.database       = "AdventureWorks";
meta.login          = "domain\\alice";
meta.auth_type      = "Integrated";
meta.server_version = "15.0.4188.2";
meta.batch_text     = "EXEC dbo.SomeProc @x = 1;";
f.add_item(meta);

osession::Statement st{};
st.item             = 1;
st.statement_id     = 0;
st.plan_id          = plan_id;
st.text             = "SELECT * FROM Sales.Orders ...";
st.parent_object_id = 1234567;
st.nest_level       = 2;
st.object_name      = "dbo.SomeProc";
f.add_statement(st);
f.add_object_name(1234567, "dbo.SomeProc");

f.commit();
```

## Read API

```cpp
osession::File f(path, osession::File::Mode::Read);
for (auto& item : f.items()) {
    for (auto& stmt : f.statements(item.file_number)) { /* ... */ }
    for (auto& rt   : f.runtime  (item.file_number)) { /* ... */ }
    for (auto& ev   : f.trace_events(item.file_number)) { /* ... */ }
    for (auto& rs   : f.result_sets(item.file_number)) { /* ... */ }
    for (auto& w    : f.waits    (item.file_number)) { /* ... */ }
}
```

All readers accept an optional `statement_id` filter so a UI can fetch just the selected statement's data without scanning the whole item.

## Status

1.0. Schema is stable. Files carry `format_version` in `meta` so readers can check compatibility (`osession::kFormatVersion` in C++).

## License

MIT. Links SQLite (public domain). Vendors miniz (MIT) under `third_party/miniz/`.
