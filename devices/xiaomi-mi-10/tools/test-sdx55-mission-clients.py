#!/usr/bin/env python3
"""Exercise scheduling of the sequential cellular and GNSS mission clients."""
import subprocess
import tempfile
from pathlib import Path

driver = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev/umi_pcie2.c"
source = driver.read_text()
start = source.index("static int\numi_sdx55_run_mission_clients(")
end = source.index("static int\numi_sdx55_mission_session(", start)
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "clients.c"
    c.write_text(r"""
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
struct umi_mhi { bool poisoned; };
static unsigned umi_sdx55_loc_port, umi_sdx55_qrtr_frames;
static struct { unsigned fix,sv_reports,max_sv,nmea_reports,gsv_reports,max_gsv,max_bds_gsv; } umi_sdx55_loc;
static int cell_result, loc_result, loc_calls, cell_calls;
static bool cell_ready, transport_error;
#define sdx55_status_session(active) ((void)0)
#define hakios_tl(...) ((void)0)
static bool sdx55_cell_discovered(void) { return cell_ready; }
static int umi_sdx55_cell_session(struct umi_mhi *h) {
    assert(!loc_calls); ++cell_calls; h->poisoned = transport_error; return cell_result;
}
static int umi_sdx55_loc_session(struct umi_mhi *h) { assert(!h->poisoned); ++loc_calls; return loc_result; }
""" + source[start:end] + r"""
static void run(bool cell, bool loc, int expected, int ncell, int nloc) {
    struct umi_mhi host = {0}; cell_calls = loc_calls = 0;
    assert(umi_sdx55_run_mission_clients(&host, cell, loc) == expected);
    assert(cell_calls == ncell && loc_calls == nloc);
}
int main(void) {
    cell_ready = true; umi_sdx55_loc_port = 1;
    run(true, false, 0, 1, 0); run(false, true, 0, 0, 1); run(true, true, 0, 1, 1);
    cell_result = ENODEV; run(true, true, ENODEV, 1, 1);
    transport_error = true; run(true, true, ENODEV, 1, 0);
    transport_error = false; cell_ready = false; run(true, true, ETIMEDOUT, 0, 1);
    cell_ready = true; cell_result = 0; umi_sdx55_loc_port = 0;
    run(true, true, ETIMEDOUT, 1, 0);
}
""")
    exe = Path(tmp) / "clients"
    subprocess.run(["cc", "-std=c11", "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([exe], check=True)
print("sequential mission client scheduling passed")
