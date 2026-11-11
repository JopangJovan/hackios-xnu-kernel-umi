#!/usr/bin/env python3
"""Exercise read-only QRTR QMI card and network status directly on the wire."""
import subprocess
import tempfile
from pathlib import Path

device = Path(__file__).resolve().parents[1]
include = device / "overlays/xnu/files/bsd/dev"
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "umi_sdx55_cell.h"

int main(void)
{
    uint8_t out[7] = {0};
    const uint8_t req[] = {0, 0x34, 0x12, 0x2f, 0, 0, 0};
    assert(!sdx55_cell_request(0x30, 1, out, sizeof(out))); /* UIM power off not allowed */
    assert(!sdx55_cell_request(0x2f, 1, out, sizeof(out) - 1));
    assert(sdx55_cell_request(0x2f, 0x1234, out, sizeof(out)) == 7 && !memcmp(out, req, 7));
    assert(sdx55_cell_request(0x24, 1, out, sizeof(out)) == 7);
    assert(!memcmp(out, (uint8_t[]){0,1,0,0x24,0,0,0}, 7));
    assert(sdx55_cell_request(0x4f, 1, out, sizeof(out)) == 7);

    /* Serving System and Data Service Capability TLVs from libqmi's
     * test_generated_nas_get_serving_system, without QMUX or network IDs. */
    uint8_t nas[] = {2,1,0,0x24,0,23,0, 2,4,0,0,0,0,0,
                     1,6,0,1,1,1,2,1,5, 0x11,4,0,3,3,4,5};
    struct sdx55_cell_status status = {0};
    assert(!sdx55_cell_decode(0x24, 1, nas, sizeof(nas), &status));
    assert(status.registration == 1 && status.ps_attach == 1);
    assert(status.radio_mask == (1u<<5) && status.data_mask == ((1u<<3)|(1u<<4)|(1u<<5)));
    nas[17] = 2; nas[19] = 2; /* searching, detached */
    assert(!sdx55_cell_decode(0x24, 1, nas, sizeof(nas), &status));
    assert(status.registration == 2 && status.ps_attach == 2);
    assert(sdx55_cell_decode(0x24, 2, nas, sizeof(nas), &status) == EINVAL);
    assert(sdx55_cell_decode(0x2f, 1, nas, sizeof(nas), &status) == EINVAL);
    nas[0] = 4;
    assert(sdx55_cell_decode(0x24, 1, nas, sizeof(nas), &status) == EINVAL);
    nas[0] = 2; nas[21] = 2; /* count claims more radio interfaces than fit */
    assert(sdx55_cell_decode(0x24, 1, nas, sizeof(nas), &status) == EINVAL);
    uint8_t lte[] = {2,1,0,0x24,0,35,0, 2,4,0,0,0,0,0,
        1,6,0,1,1,1,2,1,8, 0x27,5,0,0xcc,1,1,0,1,
        0x12,8,0,0xcc,1,1,0,3,'L','T','E'};
    assert(!sdx55_cell_decode(0x24,1,lte,sizeof(lte),&status));
    assert(!strcmp(status.plmn,"460001") && !strcmp(status.operator_name,"LTE"));
    uint8_t signal[] = {2,1,0,0x4f,0,23,0, 2,4,0,0,0,0,0,
        0x14,6,0,0,0,0xa4,0xff,0,0, 0x17,4,0,0x98,0xff,0,0};
    assert(!sdx55_cell_decode(0x4f,1,signal,sizeof(signal),&status));
    assert(status.lte_rsrp == -92 && status.nr_rsrp == -104);
    uint8_t plmn_request[22];
    assert(sdx55_cell_plmn_request(3,460 | (1u<<10),plmn_request,sizeof(plmn_request))==22);
    assert(!memcmp(plmn_request,(uint8_t[]){0,3,0,0x44,0,15,0,1,4,0,0xcc,1,1,0,
        0x11,1,0,0,0x12,1,0,1},22));
    uint8_t eons[] = {2,3,0,0x44,0,25,0, 2,4,0,0,0,0,0,
        0x10,15,0, 1,1,'X', 1,0,0,2,'N','R', 4,0,0,2,0x2d,0x4e};
    assert(!sdx55_cell_decode(0x44,3,eons,sizeof(eons),&status));
    assert(!strcmp(status.operator_name,"\xe4\xb8\xad"));
    eons[sizeof(eons)-1]=0xd8; /* A surrogate in the long name falls back to the short name. */
    assert(!sdx55_cell_decode(0x44,3,eons,sizeof(eons),&status) && !strcmp(status.operator_name,"NR"));

    /* Android SDX55 on NR SA: legacy 0x24 searches, but the bound NR line
     * has available service with a valid PS domain. True status can be 0. */
    uint8_t nr[52] = {2,1,0,0x4d,0,45,0,2,4,0,0,0,0,0,0x4a,3,0,2,0,0,0x4b,29,0,1,2};
    nr[42]=1;
    memcpy(nr+43,"46001\xff",6);
    assert(!sdx55_cell_decode(0x4d,1,nr,sizeof(nr),&status) && sdx55_cell_nr_packet_service(&status));
    assert(!strcmp(status.plmn,"46001"));
    uint8_t lte_system[52] = {2,1,0,0x4d,0,45,0,2,4,0,0,0,0,0,0x14,3,0,2,2,1,0x19,29,0,1,2};
    lte_system[42]=1;
    memcpy(lte_system+43,"460001",6);
    assert(!sdx55_cell_decode(0x4d,1,lte_system,sizeof(lte_system),&status));
    assert(status.lte_service==2 && !status.lte_forbidden && !strcmp(status.lte_plmn,"460001"));
    nr[17]=1; /* limited service */
    assert(!sdx55_cell_decode(0x4d,1,nr,sizeof(nr),&status) && !sdx55_cell_nr_packet_service(&status));
    nr[17]=2;nr[23]=0; /* domain advertised invalid */
    assert(!sdx55_cell_decode(0x4d,1,nr,sizeof(nr),&status) && !sdx55_cell_nr_packet_service(&status));
    nr[23]=1;nr[24]=1; /* CS only */
    assert(!sdx55_cell_decode(0x4d,1,nr,sizeof(nr),&status) && !sdx55_cell_nr_packet_service(&status));
    nr[24]=2;nr[29]=1;nr[30]=1; /* access forbidden */
    assert(!sdx55_cell_decode(0x4d,1,nr,sizeof(nr),&status) && !sdx55_cell_nr_packet_service(&status));
    nr[5]--;nr[21]--;
    assert(sdx55_cell_decode(0x4d,1,nr,sizeof(nr)-1,&status)==EINVAL);
    /* Qualcomm UIM Card Status. Provisioning points at slot 2, app 2.
     * Slot 1 is absent. Skip slot 2's unrelated app and its variable AID. */
    uint8_t card[] = {
        2,0x34,0x12,0x2f,0,60,0, 2,4,0,0,0,0,0,
        0x10,50,0, 1,1, 0xff,0xff, 0xff,0xff, 0xff,0xff, 2,
        0,0,0,0,0,0, 1,0,0,0,0,2,
        5,7,0,0,0,0,1,0xaa,0,0,0,0,0,0,0,
        2,2,0,0,0,0,0,0,0,0,0,0,0,0
    };
    assert(!sdx55_cell_decode(0x2f, 0x1234, card, sizeof(card), &status));
    assert(status.cards == 2 && status.present_cards == 1 && status.gw[0].slot == 1);
    assert(status.slots[0].card_state==0 && !status.slots[0].applications);
    assert(status.slots[1].card_state==1 && status.slots[1].applications==2 && !status.slots[1].ready_gw);
    assert(status.gw[0].app_index==1 && !status.error_cards);
    assert(status.gw[0].card_state == 1 && status.gw[0].app_type == 2 && status.gw[0].app_state == 2); /* PIN required */
    card[54] = 7; /* primary USIM becomes ready */
    assert(!sdx55_cell_decode(0x2f, 0x1234, card, sizeof(card), &status) && status.gw[0].app_state == 7);
    assert(status.slots[1].ready_gw==1);
    card[17] = 3; /* out-of-range app index */
    assert(sdx55_cell_decode(0x2f, 0x1234, card, sizeof(card), &status) == EINVAL);
    card[17] = card[18] = 0xff; /* unprovisioned: never assume present slot is primary */
    assert(!sdx55_cell_decode(0x2f, 0x1234, card, sizeof(card), &status));
    assert(status.present_cards == 1 && status.gw[0].slot == UINT8_MAX && status.gw[0].app_state == UINT8_MAX);
    /* UMI stock capture: only the secondary 3GPP session is provisioned.
     * The card and USIM app can be on slot index 1 for either subscription. */
    card[21]=1;card[22]=1;
    assert(!sdx55_cell_decode(0x2f,0x1234,card,sizeof(card),&status));
    uint8_t selected=0;
    assert(!sdx55_cell_subscription(&status,&selected) && selected==2 && status.gw[1].slot==1);
    card[54]=2; /* PIN required */
    assert(!sdx55_cell_decode(0x2f,0x1234,card,sizeof(card),&status));
    assert(sdx55_cell_subscription(&status,&selected)==EAGAIN);
    card[54]=7;card[17]=1;card[18]=1; /* ambiguous if both GW sessions reference a single USIM */
    assert(!sdx55_cell_decode(0x2f,0x1234,card,sizeof(card),&status));
    assert(sdx55_cell_subscription(&status,&selected)==EBUSY);
    card[21]=card[22]=0xff;
    assert(!sdx55_cell_decode(0x2f,0x1234,card,sizeof(card),&status));
    assert(!sdx55_cell_subscription(&status,&selected) && selected==1);
    status.present_cards=2;assert(sdx55_cell_subscription(&status,&selected)==EBUSY);
    card[17]=card[18]=0xff;
    assert(!sdx55_cell_decode(0x2f,0x1234,card,sizeof(card),&status));
    uint8_t bind[14];
    assert(sdx55_cell_bind_request(0x45,1,2,bind,sizeof(bind))==11);
    assert(!memcmp(bind,(uint8_t[]){0,1,0,0x45,0,4,0,1,1,0,1},11));
    assert(sdx55_cell_bind_request(0xaf,1,2,bind,sizeof(bind))==14);
    assert(!memcmp(bind,(uint8_t[]){0,1,0,0xaf,0,7,0,1,4,0,2,0,0,0},14));
    assert(!sdx55_cell_bind_request(0xaf,1,0,bind,sizeof(bind)));
    assert(!sdx55_cell_bind_request(0x45,1,3,bind,sizeof(bind)));
    assert(!sdx55_cell_bind_request(0xaf,1,1,bind,13));
    card[44] = 0xff; /* truncated AID */
    assert(sdx55_cell_decode(0x2f, 0x1234, card, sizeof(card), &status) == EINVAL);
    assert(status.gw[0].slot == UINT8_MAX && status.present_cards == 1); /* no partial update */

    /* A not-yet-provisioned USIM can still require PIN1. Read the state
     * after its variable-length AID. */
    uint8_t detected[] = {2,9,0,0x2f,0,43,0, 2,4,0,0,0,0,0,
        0x10,33,0, 255,255,255,255,255,255,255,255,1,
        1,3,0,0,0,1, 2,2,0,0,0,0,4, 0xaa,0xbb,0xcc,0xdd, 1,1,3,10,0,3,10};
    assert(!sdx55_cell_decode(0x2f,9,detected,sizeof(detected),&status));
    assert(status.gw[0].slot==255 && status.gw[1].slot==255 && status.slots[0].upin_state==3);
    assert(status.slots[0].app[0].type==2 && status.slots[0].app[0].state==2);
    assert(status.slots[0].app[0].upin_replaces==1 && status.slots[0].app[0].pin1_state==1);
    uint8_t provision[32];
    assert(!sdx55_uim_provision_request(1,1,&status,provision,sizeof(provision)));
    status.slots[1]=status.slots[0];
    status.slots[0].card_state=2;
    status.slots[1].app[0].state=1;
    status.slots[1].app[0].pin1_state=0;
    status.slots[1].upin_state=0;
    status.cards=2;
    assert(sdx55_uim_provision_request(1,2,&status,provision,sizeof(provision))==21);
    assert(!memcmp(provision,(uint8_t[]){0,1,0,0x38,0,14,0,1,2,0,2,1,0x10,6,0,2,4,0xaa,0xbb,0xcc,0xdd},21));
    assert(sdx55_uim_provision_request(2,2|SDX55_UIM_DEACTIVATE,&status,provision,sizeof(provision))==12);
    assert(!memcmp(provision,(uint8_t[]){0,2,0,0x38,0,5,0,1,2,0,2,0},12));
    uint8_t config[14]={0};
    assert(sdx55_cell_request(0x3a,2,config,sizeof(config))==14);
    assert(!memcmp(config,(uint8_t[]){0,2,0,0x3a,0,7,0,0x10,4,0,5,0,0,0},14));
    assert(!sdx55_cell_request(0x38,2,config,sizeof(config)));
    uint8_t cfg_rsp[]={2,2,0,0x3a,0,15,0,2,4,0,0,0,0,0,0x10,1,0,0,0x12,1,0,1};
    assert(!sdx55_cell_decode(0x3a,2,cfg_rsp,sizeof(cfg_rsp),&status));
    assert(!status.auto_select && status.halt_subscription==1);
    cfg_rsp[17]=2;
    assert(sdx55_cell_decode(0x3a,2,cfg_rsp,sizeof(cfg_rsp),&status)==EINVAL);
    uint8_t absent[] = {2,1,0,0x2f,0,25,0, 0x10,15,0,
                        0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,1,0,0,0,0,0,0, 2,4,0,0,0,0,0};
    assert(!sdx55_cell_decode(0x2f, 1, absent, sizeof(absent), &status));
    assert(status.cards == 1 && !status.present_cards && status.gw[0].slot == UINT8_MAX);
    assert(sdx55_cell_subscription(&status,&selected)==ENODEV);
    uint8_t failure[] = {2,1,0,0x2f,0,7,0, 2,4,0,1,0,0x34,0};
    assert(sdx55_cell_decode(0x2f, 1, failure, sizeof(failure), &status) == EIO && status.service_error == 0x34);
    failure[10] = 0;
    assert(sdx55_cell_decode(0x2f, 1, failure, sizeof(failure), &status) == EINVAL);
    failure[12] = 0; /* missing status TLV after success */
    assert(sdx55_cell_decode(0x2f, 1, failure, sizeof(failure), &status) == EINVAL);
    assert(sdx55_cell_decode(0x2f, 1, absent, sizeof(absent)-1, &status) == EINVAL);
    uint8_t duplicate[sizeof(absent)+7];
    memcpy(duplicate, absent, sizeof(absent));
    memcpy(duplicate+sizeof(absent), absent+sizeof(absent)-7, 7);
    duplicate[5] += 7;
    assert(sdx55_cell_decode(0x2f, 1, duplicate, sizeof(duplicate), &status) == EINVAL);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "cell.c"
    exe = Path(tmp) / "cell"
    c.write_text(fixture)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(include),
                    str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], cwd=tmp, check=True)
print("PASS: SDX55 UIM primary card and NAS registration/PS read-only wire queries")
