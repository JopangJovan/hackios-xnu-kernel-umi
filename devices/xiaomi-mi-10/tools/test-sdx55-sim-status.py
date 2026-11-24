#!/usr/bin/env python3
"""Drive the read-only UIM/NAS window without running a modem or an Apple service."""
import subprocess
import tempfile
from pathlib import Path
inc = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
text = (inc / "umi_pcie2.c").read_text()
entry = "static int\nsdx55_observe_sim("
assert entry in text, "missing read-only SIM sampling entry"
helper = text[text.index(entry):text.index("static int\numi_sdx55_cell_session(", text.index(entry))]
fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <errno.h>
#include "umi_sdx55_cell.h"
#include "umi_sdx55_wan.h"
struct umi_mhi { bool poisoned; };
struct sdx55_cell_endpoint {unsigned service;};
enum {CELL_UIM,CELL_NAS,CELL_WDS,CELL_WDA};
static struct sdx55_cell_endpoint sdx55_services[]={{11},{3},{1},{26}};
static struct {struct sdx55_cell_status cell;} sdx55_query;
static unsigned ticks, samples, nas, wds, ready_at, lost_at, silent, no_card;
#define NSEC_PER_SEC 1
#define hakios_tl(...) ((void)0)
static void clock_interval_to_deadline(unsigned s,unsigned unit,uint64_t *deadline){*deadline=ticks+s*unit;}
static uint64_t mach_absolute_time(void){return ticks;}
static int umi_sdx55_poll_loc(struct umi_mhi *h, unsigned ms){ticks+=ms/1000; return h->poisoned?EIO:0;}
static int sdx55_cell_command(struct umi_mhi *h,const struct sdx55_cell_endpoint *ep,
    uint16_t msg,uint32_t arg,const struct sdx55_wan_status *format) {
    assert(!arg && !format && !h->poisoned);
    if (ep->service==11) {
        assert(msg==SDX55_UIM_GET_CARD_STATUS);
        samples++;
        if (samples==lost_at) return ENXIO;
        if (silent) return EIO;
        sdx55_query.cell.cards=2;
        sdx55_query.cell.present_cards=(!no_card && samples>=2);
        sdx55_query.cell.slots[1].card_state=(!no_card && samples>=2);
        sdx55_query.cell.slots[1].ready_gw=(ready_at && samples>=ready_at);
    } else if (ep->service==3) {
        assert(msg==SDX55_NAS_GET_SERVING_SYSTEM || msg==SDX55_NAS_GET_SYSTEM_INFO); ++nas;
    } else { assert(ep->service==1 && msg==SDX55_WDS_GET_STATUS); ++wds; }
    return 0;
}
""" + helper + r"""
int main(void) {
    struct umi_mhi h={0};
    ready_at=3;
    assert(!sdx55_observe_sim(&h) && samples==3 && nas && wds);
    ready_at=0; samples=ticks=0;
    assert(sdx55_observe_sim(&h)==ETIMEDOUT && ticks>=30 && ticks<=45);
    no_card=1; samples=ticks=0;
    assert(sdx55_observe_sim(&h)==ENODEV && ticks>=30 && ticks<=45);
    no_card=0; silent=1; samples=ticks=0;
    assert(sdx55_observe_sim(&h)!=0 && ticks>=30);
    silent=0; lost_at=2; samples=ticks=0;
    assert(sdx55_observe_sim(&h)==ENXIO && ticks<10);
    h.poisoned=true; samples=ticks=0;
    assert(sdx55_observe_sim(&h)!=0 && !samples);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)/"sim.c"
    p.write_text(fixture)
    exe=p.with_suffix("")
    subprocess.run(["cc","-std=c11","-Werror","-I"+str(inc),p,"-o",exe],check=True)
    subprocess.run([exe],check=True)
print("PASS: bounded read-only cellular observation")
