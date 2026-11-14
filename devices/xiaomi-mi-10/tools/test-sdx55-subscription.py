#!/usr/bin/env python3
"""Exercise SIM selection and registration on the live cellular worker path."""
from pathlib import Path
import subprocess
import tempfile

inc = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd/dev"
source = (inc / "umi_pcie2.c").read_text()
routes = source[source.index("#define SDX55_CELL_PORT"):source.index("static void\nsdx55_cell_announce")]
wait = source[source.index("static int\nsdx55_wait_registration"):source.index("static int\nsdx55_program_format")]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include "umi_sdx55_wds.h"
#include "umi_sdx55_wan.h"
#define hakios_tl(...) (void)0
struct umi_mhi { int unused; };
''' + routes + r'''
enum {ABSENT,PRIMARY,SECONDARY,AMBIGUOUS,PIN_REQUIRED,FAIL_WDS,CHANGED,NO_PS,NR_FORBIDDEN,LATE_CARD,DISAPPEARS,MOVED_SLOT,NEW_APP,UNREADY_THEN_EMPTY,NO_STATUS,EMPTY_THEN_UNKNOWN,CARD_ERROR,DETECTED};
static unsigned scenario, round_no, nas_client, wds_client, nas_calls, nr_calls, config_queries, activations, registrations;
#define NSEC_PER_SEC 1000000000
#define clock_interval_to_deadline(s,n,d) (*(d)=round_no+3)
#define mach_absolute_time() (round_no)
static int sdx55_radio_initializing(unsigned code){ return code==74; }
static int umi_sdx55_poll_loc(struct umi_mhi *host,unsigned ms){round_no++;return 0;}
static int sdx55_read_cell_service(struct umi_mhi *host,const struct sdx55_cell_endpoint *ep){
    struct sdx55_cell_status *s=&sdx55_query.cell;
    *s=(struct sdx55_cell_status){.gw={{.slot=255},{.slot=255}}};
    if(ep->service==11){
        s->cards=2;
        if(scenario==NO_STATUS || (scenario==EMPTY_THEN_UNKNOWN && round_no)){sdx55_query.service_error=74;return EIO;}
        if(scenario==CARD_ERROR){s->slots[1].card_state=2;s->error_cards=1;return 0;}
        if(scenario==ABSENT || scenario==EMPTY_THEN_UNKNOWN || (scenario==LATE_CARD && round_no<2) ||
            ((scenario==DISAPPEARS || scenario==UNREADY_THEN_EMPTY) && round_no))return 0;
        s->present_cards=scenario==AMBIGUOUS ? 2 : 1;
        if(scenario==DETECTED && !round_no){
            s->slots[1].card_state=1;
            s->slots[1].applications=1;
            s->slots[1].app[0]=(struct sdx55_uim_app){.type=2,.state=1,.aid_length=1,.aid={1}};
            return 0;
        }
        unsigned which=(scenario==PRIMARY || (scenario==CHANGED && round_no)) ? 0 : 1;
        s->gw[which]=(struct sdx55_gw_app){.slot=(scenario==MOVED_SLOT&&round_no)?0:1,.card_state=1,.app_type=2,
            .app_state=(scenario==PIN_REQUIRED||scenario==UNREADY_THEN_EMPTY)?2:7,.app_index=(scenario==NEW_APP&&round_no)?1:0};
        return 0;
    }
    assert(ep->service==3 && nas_client && nas_client==wds_client);
    nas_calls++;
    s->registration=scenario==PRIMARY ? 1 : 2;
    s->ps_attach=scenario==PRIMARY ? 1 : 2;
    return 0;
}
static int sdx55_cell_command(struct umi_mhi *host,const struct sdx55_cell_endpoint *ep,
    uint16_t msg,uint32_t argument,const struct sdx55_wan_status *format){
    if(msg==SDX55_UIM_PROVISION){
        assert(scenario==DETECTED && ep->service==11 && argument==2 && !activations && !nas_client);
        activations++;return 0;
    }
    if(msg==SDX55_NAS_REGISTER_AUTO){
        assert(scenario==DETECTED && ep->service==3 && nas_client==2 && !registrations);
        registrations++;return 0;
    }
    if(msg==0x3a){
        assert(ep->service==11 && !wds_client && !nas_client && round_no>=3);
        config_queries++;return EIO; /* optional GET must not change the gate */
    }
    if(msg==SDX55_NAS_BIND_SUBSCRIPTION){
        assert(ep->service==3 && !nas_client && !wds_client);
        assert(argument==(scenario==PRIMARY ? 1u : 2u));
        nas_client=argument;return 0;
    }
    if(msg==SDX55_WDS_BIND_SUBSCRIPTION){
        assert(ep->service==1 && argument==nas_client && !wds_client);
        if(scenario==FAIL_WDS){sdx55_query.service_error=48;return EIO;}
        wds_client=argument;return 0;
    }
    assert(msg==SDX55_NAS_GET_SYSTEM_INFO && ep->service==3 && nas_calls);
    nr_calls++;
    sdx55_query.cell.nr_service=2;
    sdx55_query.cell.nr_true_service=0;
    sdx55_query.cell.nr_domain=(scenario==NO_PS || scenario==CHANGED || scenario==DISAPPEARS || scenario==MOVED_SLOT || scenario==NEW_APP || (scenario==LATE_CARD && round_no<4)) ? 1 : 2;
    sdx55_query.cell.nr_forbidden=scenario==NR_FORBIDDEN || (scenario==DETECTED && !registrations);
    return 0;
}
''' + wait + r'''
static int run(unsigned which) {
    scenario=which;round_no=nas_client=wds_client=nas_calls=nr_calls=config_queries=activations=registrations=0;
    struct sdx55_data_routes routes={sdx55_services[CELL_WDS],sdx55_services[CELL_WDA]};
    struct umi_mhi host={0};
    unsigned provisioned=0;
    int result=sdx55_wait_registration(&host,&routes,&provisioned);
    assert(provisioned==(which==DETECTED?2u:0u));
    return result;
}
int main(void){
    for(unsigned i=0;i<4;i++){sdx55_services[i].node=3;sdx55_services[i].port=52+i;}
    assert(run(ABSENT)==ENODEV && round_no==3 && !nas_client && !wds_client);
    assert(!run(LATE_CARD) && round_no==4 && wds_client==2 && nr_calls==3);
    assert(run(DISAPPEARS)==ENXIO && round_no==1 && wds_client==2 && nr_calls==1);
    assert(run(MOVED_SLOT)==ENXIO && round_no==1 && wds_client==2 && nr_calls==1);
    assert(run(NEW_APP)==ENXIO && round_no==1 && wds_client==2 && nr_calls==1);
    assert(run(UNREADY_THEN_EMPTY)==ETIMEDOUT && round_no==3 && !nas_client);
    assert(run(EMPTY_THEN_UNKNOWN)==ETIMEDOUT && round_no==3 && !nas_client);
    assert(run(NO_STATUS)==ETIMEDOUT && round_no==3 && !nas_client);
    assert(run(CARD_ERROR)==ETIMEDOUT && round_no==3 && !nas_client);
    assert(run(AMBIGUOUS)==EBUSY && !nas_client && !wds_client);
    assert(run(PIN_REQUIRED)==ETIMEDOUT && !nas_client && !wds_client && config_queries==1);
    assert(!run(PRIMARY) && wds_client==1 && nas_calls==1 && !nr_calls);
    assert(!run(SECONDARY) && wds_client==2 && nr_calls==1);
    assert(!run(DETECTED) && activations==1 && registrations==1 && wds_client==2 && nr_calls==2);
    assert(run(FAIL_WDS)==EIO && !wds_client && !nas_calls);
    assert(run(CHANGED)==ENXIO && wds_client==2 && nas_calls==1);
    assert(run(NO_PS)==ETIMEDOUT && nr_calls==3);
    assert(run(NR_FORBIDDEN)==ETIMEDOUT && nr_calls==3);
}
'''
with tempfile.TemporaryDirectory() as directory:
    src = Path(directory) / "subscription.c"
    src.write_text(fixture)
    exe = src.with_suffix("")
    subprocess.run(["cc", "-std=gnu11", "-I", str(inc), str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("PASS: per-client 3GPP subscription binding and NR SA packet-domain registration")
