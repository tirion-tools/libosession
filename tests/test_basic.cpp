// SPDX-License-Identifier: MIT
// Round-trip: write a small osession, read it back, verify everything.

#include "osession/osession.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        std::exit(1); \
    } \
} while (0)

int main() {
    auto path = std::filesystem::temp_directory_path() / "osession_test.osession";
    std::filesystem::remove(path);

    // ── Write
    {
        osession::File f(path.string(), osession::File::Mode::Write);
        f.set_meta("source_pesession", "/tmp/test.pesession");

        f.begin_transaction();

        const std::string xml_a =
            "<ShowPlanXML>plan A: select * from t</ShowPlanXML>";
        const std::string xml_b =
            "<ShowPlanXML>plan B: select * from u</ShowPlanXML>";
        int64_t pa1 = f.add_plan(xml_a);
        int64_t pa2 = f.add_plan(xml_a);   // duplicate - should return same id
        int64_t pb  = f.add_plan(xml_b);
        CHECK(pa1 == pa2);
        CHECK(pa1 != pb);

        osession::ItemMeta m;
        m.file_number = 1;
        m.instance = "PRODSQL";
        m.database = "AdventureWorks";
        m.login = "user1";
        m.plan_type = 'A';
        m.actual_rows = 12345;
        m.total_time = "00:01:23";
        f.add_item(m);

        osession::Statement s;
        s.item = 1;
        s.statement_id = 1;
        s.text = "SELECT * FROM t";
        s.plan_id = pa1;
        s.plan_hash = "abc123";
        f.add_statement(s);

        osession::RuntimeSample r;
        r.item = 1;
        r.statement_id = 1;
        r.node_id = 0;
        r.cpu_us = 1500;
        r.elapsed_us = 4200;
        r.reads = 800;
        r.writes = 2;
        r.row_count = 9876;
        f.add_runtime(r);

        osession::WaitAggregate w;
        w.item = 1;
        w.statement_id = 1;
        w.wait_type = "CXPACKET";
        w.duration_ms = 555;
        w.signal_ms = 33;
        f.add_wait(w);

        // Lossless additions: capture two snapshots of plan A so we can
        // verify dedup-on-shape coexists with byte-exact reconstruction.
        const std::string snap_xml_1 =
            "<ShowPlanXML><RunTime cpu=\"100\"/>plan A: select * from t"
            "</ShowPlanXML>";
        const std::string snap_xml_2 =
            "<ShowPlanXML><RunTime cpu=\"250\"/>plan A: select * from t"
            "</ShowPlanXML>";
        osession::PlanSnapshot snap;
        snap.item = 1;
        snap.snapshot_idx = 0;
        snap.plan_id = pa1;
        int64_t snap_id_1 = f.add_plan_snapshot(snap, snap_xml_1);
        snap.snapshot_idx = 1;
        int64_t snap_id_2 = f.add_plan_snapshot(snap, snap_xml_2);
        CHECK(snap_id_1 != snap_id_2);

        // Two trace events for the same statement, batched into one
        // deflated blob in trace_streams.
        std::vector<osession::TraceEvent> evs;
        {
            osession::TraceEvent ev;
            ev.trace_pos = 0;
            ev.cpu_us = 100;
            ev.elapsed_us = 200;
            ev.reads = 10;
            ev.row_count = 1;
            evs.push_back(ev);
            ev.trace_pos = 1;
            ev.cpu_us = 150;
            ev.elapsed_us = 300;
            ev.reads = 20;
            ev.row_count = 1;
            evs.push_back(ev);
        }
        f.add_trace_events(1, 1, evs);

        f.commit();
    }

    // ── Read
    {
        osession::File f(path.string(), osession::File::Mode::Read);
        CHECK(f.meta("format_version") == "2");
        CHECK(f.meta("source_pesession") == "/tmp/test.pesession");

        auto items = f.items();
        CHECK(items.size() == 1);
        CHECK(items[0].file_number == 1);
        CHECK(items[0].instance == "PRODSQL");
        CHECK(items[0].plan_type == 'A');
        CHECK(items[0].actual_rows == 12345);

        auto stmts = f.statements(1);
        CHECK(stmts.size() == 1);
        CHECK(stmts[0].statement_id == 1);
        CHECK(stmts[0].text == "SELECT * FROM t");
        CHECK(stmts[0].plan_id > 0);

        auto rt = f.runtime(1, 1);
        CHECK(rt.size() == 1);
        CHECK(rt[0].cpu_us == 1500);
        CHECK(rt[0].row_count == 9876);

        auto ws = f.waits(1, 1);
        CHECK(ws.size() == 1);
        CHECK(ws[0].wait_type == "CXPACKET");
        CHECK(ws[0].duration_ms == 555);

        auto snaps = f.plan_snapshots(1);
        CHECK(snaps.size() == 2);
        CHECK(snaps[0].snapshot_idx == 0);
        CHECK(snaps[1].snapshot_idx == 1);
        std::string s1 = f.plan_snapshot_xml(snaps[0].snapshot_id);
        std::string s2 = f.plan_snapshot_xml(snaps[1].snapshot_id);
        CHECK(s1 ==
              "<ShowPlanXML><RunTime cpu=\"100\"/>plan A: select * from t"
              "</ShowPlanXML>");
        CHECK(s2 ==
              "<ShowPlanXML><RunTime cpu=\"250\"/>plan A: select * from t"
              "</ShowPlanXML>");

        auto events = f.trace_events(1, 1);
        CHECK(events.size() == 2);
        CHECK(events[0].trace_pos == 0);
        CHECK(events[0].cpu_us == 100);
        CHECK(events[1].trace_pos == 1);
        CHECK(events[1].cpu_us == 150);
    }

    std::filesystem::remove(path);
    std::printf("osession round-trip OK\n");
    return 0;
}
