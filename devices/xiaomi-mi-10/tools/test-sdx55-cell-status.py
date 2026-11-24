#!/usr/bin/env python3
"""Exercise the kernel's locked, read-only UMI cell snapshot against decoded QMI replies."""
from pathlib import Path
import subprocess
import tempfile

bsd = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd"
fixture = r"""
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "umi_sdx55_wds.h"
#include "umi_sdx55_wan.h"
#include <sys/umi_sdx55_cell_status.h>
#define LCK_GRP_DECLARE(name, grp) int name __attribute__((unused))
#define LCK_MTX_DECLARE(name, grp) int name
static void lck_mtx_lock(int *l){assert(!*l); *l=1;}
static void lck_mtx_unlock(int *l){assert(*l); *l=0;}
#define hakios_tl(...) ((void)0)
#include "umi_sdx55_cell_public.inc"
int main(void) {
    struct sdx55_cell_status c = {.cards=2};
    struct sdx55_wan_status w = {.connection_status=1};
    c.slots[1].card_state=1;
    sdx55_status_session(true);
    assert(cell_view.active && cell_view.generation==1 && !cell_view.valid);
    sdx55_status_result(11,SDX55_UIM_GET_CARD_STATUS,0,0,&c,&w);
    assert(cell_view.valid==UMI_CELL_UIM && cell_view.present_mask==2 && !cell_view.ready_mask);
    sdx55_status_result(3,SDX55_NAS_GET_SERVING_SYSTEM,0,0,&c,&w);
    sdx55_status_result(1,SDX55_WDS_GET_STATUS,0,0,&c,&w);
    assert(cell_view.valid==11 && cell_view.wds_status==1);
    sdx55_status_result(11,SDX55_UIM_GET_CARD_STATUS,0,EINVAL,&c,&w);
    assert(cell_view.valid==(UMI_CELL_NAS|UMI_CELL_WDS) && !cell_view.present_mask);
    c.slots[0].card_state=2;
    c.slots[1].ready_gw=1;
    sdx55_status_result(11,SDX55_UIM_GET_CARD_STATUS,0,0,&c,&w);
    assert(cell_view.ready_mask==2 && cell_view.present_mask==2 && cell_view.error_mask==1);
    sdx55_status_result(3,SDX55_NAS_BIND_SUBSCRIPTION,2,0,&c,&w);
    assert(!(cell_view.valid&UMI_CELL_NAS) && cell_view.nas_subscription==2);
    c.nr_service=UINT8_MAX;
    sdx55_status_result(3,SDX55_NAS_GET_SYSTEM_INFO,0,0,&c,&w);
    assert(!(cell_view.valid&UMI_CELL_NR));
    c.nr_service=2; c.nr_domain=2;
    sdx55_status_result(3,SDX55_NAS_GET_SYSTEM_INFO,0,0,&c,&w);
    assert((cell_view.valid&UMI_CELL_NR) && cell_view.nr_service==2 && cell_view.nr_domain==2);
    sdx55_status_result(3,SDX55_NAS_GET_SYSTEM_INFO,0,EIO,&c,&w);
    assert(!(cell_view.valid&UMI_CELL_NR));
    sdx55_status_result(1,SDX55_WDS_GET_STATUS,0,0,&c,&w);
    sdx55_status_lost(1);
    assert(!(cell_view.valid&UMI_CELL_WDS) && !cell_view.wds_status);
    sdx55_status_session(false);
    assert(cell_view.generation==1 && !cell_view.active && !cell_view.valid && !cell_view.ready_mask);
    sdx55_status_result(11,SDX55_UIM_GET_CARD_STATUS,0,0,&c,&w);
    assert(!cell_view.valid);
    sdx55_status_session(true);
    assert(cell_view.generation==2 && !cell_view.valid && !cell_view.nas_subscription);
}
"""
with tempfile.TemporaryDirectory() as tmp:
    source = Path(tmp) / "state.c"
    source.write_text(fixture)
    binary = Path(tmp) / "state"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I"+str(bsd),
                    "-I"+str(bsd / "dev"), source, "-o", binary], check=True)
    subprocess.run([binary], check=True)
print("PASS: read-only SDX55 cellular snapshot lifetime and field validity")
