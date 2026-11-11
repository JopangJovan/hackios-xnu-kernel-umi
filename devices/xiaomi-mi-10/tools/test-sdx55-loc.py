#!/usr/bin/env python3
"""Exercise SDX55 GNSS publication, LOC codecs and stock modem control exchanges."""
import subprocess
import sys
import tempfile
from pathlib import Path

bsd = Path(__file__).resolve().parents[1] / "overlays/xnu/files/bsd"
source = (bsd / "dev/hakios.input.c").read_text()
def section(start, end):
    return source[source.index(start):source.index(end, source.index(start))]

fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stddef.h>
#include <sys/hakios.input.h>
#include <string.h>
#include <sys/hakios.qmi.h>
typedef unsigned clock_sec_t, clock_usec_t;
static void clock_get_calendar_microtime(clock_sec_t *sec, clock_usec_t *usec)
{
    *sec = 1700000000;
    *usec = 0;
}
'''
fixture += section("#define SLPI_LOC_SERVICE", "/* LOC_POSITION_REPORT indication")
fixture += section("static const struct qmi_elem_info slpi_loc_response_ei", "static LCK_GRP_DECLARE(slpi_loc_lck_grp")
fixture += section("struct slpi_loc_position_report {", "static const struct qmi_elem_info slpi_loc_position_report_ei")
fixture += section("static const struct qmi_elem_info slpi_loc_position_report_ei", "/* the mandatory result TLV")
fixture += r"""
#define lck_mtx_lock(m) ((void)0)
#define lck_mtx_unlock(m) ((void)0)
#define mach_absolute_time() 123
#define hakios_tl(...) ((void)0)
"""
fixture += section("static struct hakios_loc_state slpi_loc_state", "static double\nslpi_loc_bits_to_double")
fixture += section("static double\nslpi_loc_bits_to_double", "int\nhakios_loc_request")
fixture += section("static void\nsdx55_loc_metric(", "static int\nsdx55_fix_sysctl")
fixture += r"""
#define __unused __attribute__((unused))
struct sysctl_oid;
struct sysctl_req { void *out; size_t len; const unsigned *p; };
static int proc_suser(const unsigned *uid) { return *uid == 0 ? 0 : EPERM; }
static int SYSCTL_OUT(struct sysctl_req *req, const void *p, size_t n)
{
    assert(n <= req->len);
    memcpy(req->out, p, n);
    return 0;
}
#define SYSCTL_PROC(...)
"""
fixture += section("static int\nsdx55_fix_sysctl", "static const struct qmi_msg_handler slpi_sns_qmi_handlers[]")
fixture += section("int\nhakios_loc_request", "/* SDX55 LOC_POSITION_REPORT")
pcie = (bsd / "dev/umi_pcie2.c").read_text()
fixture += r'''
static struct {
    unsigned nmea_reports, gsv_reports, max_gsv, max_bds_gsv, sv_reports, max_sv;
    unsigned max_tracked, max_bds_tracked;
    uint32_t peak_snr_bits;
} umi_sdx55_loc;
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#undef hakios_tl
static char location_log[256];
#define hakios_tl(...) ((void)snprintf(location_log,sizeof(location_log),__VA_ARGS__))
'''
fixture += pcie[pcie.index("static int\nsdx55_pdm_response("):pcie.index("static uint32_t\nsdx55_le32(")]
fixture += pcie[pcie.index("static uint32_t\nsdx55_le32("):pcie.index("static void\numi_sdx55_qrtr_packet(void *context, const void *data, size_t size)\n{")]
fixture += pcie[pcie.index("#define SDX55_DMS_PORT"):pcie.index("} umi_sdx55_dms;") + len("} umi_sdx55_dms;")]
fixture += r'''
struct umi_mhi { uint8_t mode; unsigned reads, writes, refusals; uint16_t refusal_code; };
static int umi_sdx55_poll_loc(struct umi_mhi *host, unsigned ms) { return 0; }
static int umi_mhi_send_qrtr_packet(struct umi_mhi *host, uint32_t *packet,
    size_t size, unsigned timeout, uint16_t unused)
{
    const uint8_t *qmi = (uint8_t *)(packet + 8);
    assert(packet[3] == SDX55_DMS_PORT && packet[6] == 3 && packet[7] == 65);
    assert(qmi[0] == 0 && (qmi[1] | qmi[2] << 8) == umi_sdx55_dms.txn);
    if (qmi[3] == SDX55_DMS_BIND) {
        const uint8_t bind[] = {0x54,0,7,0,1,4,0,1,0,0,0};
        assert(packet[5] == 14 && size == 48 && !memcmp(qmi + 3,bind,sizeof(bind)));
    } else if (qmi[3] == SDX55_DMS_SET_MODE) {
        assert(size == 44 && packet[5] == 11 && !qmi[4] && qmi[5] == 4 && !qmi[6]);
        assert(qmi[7] == 1 && qmi[8] == 1 && qmi[9] == 0);
        if (host->mode == 5) assert(qmi[10] == SDX55_DMS_LOW_POWER);
        assert(qmi[10] != 5);
        host->writes++;
        if (host->refusals) {
            host->refusals--;
            umi_sdx55_dms.replied = true;
            umi_sdx55_dms.error = EIO;
            umi_sdx55_dms.service_error = host->refusal_code;
            return 0;
        }
        host->mode = qmi[10];
    } else {
        assert(qmi[3] == SDX55_DMS_GET_MODE && size == 40 && packet[5] == 7);
        assert(!qmi[4] && !qmi[5] && !qmi[6]);
        umi_sdx55_dms.mode = host->mode;
        host->reads++;
    }
    umi_sdx55_dms.replied = true;
    umi_sdx55_dms.error = 0;
    return 0;
}
'''
fixture += pcie[pcie.index("static int\numi_sdx55_dms_command("):pcie.index("static int\numi_sdx55_loc_exchange(")]
fixture += r'''
/* A complete POSITION_REPORT with coordinates 1 and 2 degrees. */
static bool position_report(uint8_t status, uint8_t session, const uint8_t *quality, size_t length)
{
    uint8_t packet[256] = {4,1,0,0x24,0,0,0};
    const uint8_t coordinates[] = {
        1,4,0,status,0,0,0, 2,1,0,session,
        0x10,8,0,0,0,0,0,0,0,0xf0,0x3f, 0x11,8,0,0,0,0,0,0,0,0,0x40
    };
    const size_t body = sizeof(coordinates) + length;
    assert(7 + body <= sizeof(packet));
    packet[5] = body;
    memcpy(packet + 7, coordinates, sizeof(coordinates));
    if (length) memcpy(packet + 7 + sizeof(coordinates), quality, length);
    return hakios_loc_indication(packet, 7 + body);
}

int main(void)
{
    uint8_t mask[] = {0x23,4,0,2,0,0,0};
    const uint8_t cell_with_list[] = {0x23,4,0,2,0,0,0, 0x2c,3,0,1,19,0};
    uint8_t only_list[] = {0x2c,3,0,1,19,0};
    assert(!position_report(0, 1, mask, sizeof(mask)) && !slpi_loc_state.valid);
    assert(!position_report(0, 1, cell_with_list, sizeof(cell_with_list)));
    mask[3] = 1; /* satellite */
    assert(position_report(0, 1, mask, sizeof(mask)) && slpi_loc_state.fixes == 1);
    assert(slpi_loc_state.latitude == 1 && slpi_loc_state.longitude == 2);
    mask[3] = 3; /* satellite and cell */
    assert(position_report(0, 1, mask, sizeof(mask)));
    mask[3] = 0x80; /* GNSS/network hybrid */
    assert(position_report(0, 1, mask, sizeof(mask)));
    assert(!position_report(1, 1, mask, sizeof(mask))); /* intermediate */
    assert(!position_report(0, 0, mask, sizeof(mask))); /* other session */
    assert(!position_report(0, 1, NULL, 0));
    assert(position_report(0, 1, only_list, sizeof(only_list)));
    only_list[3] = 2; /* count disagrees with TLV size */
    assert(!position_report(0, 1, only_list, sizeof(only_list)));
    assert(slpi_loc_state.fixes == 4);
    const uint8_t duplicated_lat[] = {
        0x23,4,0,1,0,0,0, 0x10,8,0,0,0,0,0,0,0,0,0,
    };
    assert(!position_report(0, 1, duplicated_lat, sizeof(duplicated_lat)));
    assert(slpi_loc_state.fixes == 4);
    _Static_assert(sizeof(struct hakios_loc_state) == 48, "deployed location v1 reader ABI");
    const uint8_t measured[] = {
        0x23,4,0,1,0,0,0, 0x2c,5,0,2,19,0,7,0,
        0x12,4,0,0,0,0x48,0x41, 0x25,8,0,0x7b,0x68,0xe5,0xcf,0x8b,1,0,0,
    };
    assert(position_report(0, 1, measured, sizeof(measured)));
    assert(sdx55_geodetic_state.fix.size == sizeof(struct hakios_sdx55_fix) && sdx55_geodetic_state.fix.version == HAKIOS_SDX55_FIX_VERSION);
    assert(sdx55_geodetic_state.fix.fixes == 5 && sdx55_geodetic_state.fix.received_ticks == 123);
    assert(sdx55_geodetic_state.fix.latitude == 1 && sdx55_geodetic_state.fix.longitude == 2);
    assert(sdx55_geodetic_state.fix.utc_ms == 1700000000123 && sdx55_geodetic_state.fix.horizontal_uncertainty_m == 12.5f);
    assert((sdx55_geodetic_state.fix.flags & (HAKIOS_SDX55_FIX_HAS_UTC | HAKIOS_SDX55_FIX_HAS_HUNC)) ==
        (HAKIOS_SDX55_FIX_HAS_UTC | HAKIOS_SDX55_FIX_HAS_HUNC));
    assert((sdx55_geodetic_state.fix.flags & HAKIOS_SDX55_FIX_HAS_TECHNOLOGY) && sdx55_geodetic_state.fix.technology == 1 && sdx55_geodetic_state.fix.used_sv == 2);
    assert(!position_report(1, 1, measured, sizeof(measured)) && sdx55_geodetic_state.fix.fixes == 5);
    struct hakios_sdx55_fix sample;
    struct sysctl_req read_measurement = { &sample, sizeof(sample) };
    assert(sdx55_fix_sysctl(NULL, NULL, 0, &read_measurement) == 0);
    assert(sample.utc_ms == 1700000000123 && sample.horizontal_uncertainty_m == 12.5f);
    /* A report from another source must not overwrite the modem measurement. */
    struct slpi_loc_position_report slpi = { .latitude_present = 1, .longitude_present = 1 };
    assert(slpi_loc_publish(&slpi, 0, NULL));
    assert(slpi_loc_state.latitude == 0 && sdx55_geodetic_state.fix.latitude == 1 && sdx55_geodetic_state.fix.fixes == 5);
    /* Optional values are not carried forward to the next accepted fix. */
    only_list[3] = 1;
    assert(position_report(0, 1, only_list, sizeof(only_list)));
    assert(sdx55_geodetic_state.fix.flags == 0 && !sdx55_geodetic_state.fix.utc_ms && sdx55_geodetic_state.fix.used_sv == 1);
    assert(!sdx55_geodetic_state.fix.horizontal_uncertainty_m && !sdx55_geodetic_state.fix.technology);
    uint8_t invalid_radius[] = {0x23,4,0,1,0,0,0, 0x12,4,0,0,0,0x80,0x7f};
    assert(position_report(0, 1, invalid_radius, sizeof(invalid_radius)));
    assert(!(sdx55_geodetic_state.fix.flags & HAKIOS_SDX55_FIX_HAS_HUNC)); /* infinity */
    invalid_radius[13] = 0xbf; /* -1 metre */
    assert(position_report(0, 1, invalid_radius, sizeof(invalid_radius)));
    assert(!(sdx55_geodetic_state.fix.flags & HAKIOS_SDX55_FIX_HAS_HUNC));
    uint64_t saved_fixes = sdx55_geodetic_state.fix.fixes;
    uint8_t duplicated_time[sizeof(measured) + 11];
    memcpy(duplicated_time, measured, sizeof(measured));
    memcpy(duplicated_time + sizeof(measured), measured + sizeof(measured) - 11, 11);
    assert(!position_report(0, 1, duplicated_time, sizeof(duplicated_time)));
    const uint8_t bad_utc_width[] = {0x23,4,0,1,0,0,0, 0x25,1,0,123};
    assert(!position_report(0, 1, bad_utc_width, sizeof(bad_utc_width)));
    assert(sdx55_geodetic_state.fix.fixes == saved_fixes);
    const uint8_t native_measurements[] = {
        0x23,4,0,1,0,0,0,
        0x13,4,0,0,0,0x20,0x40, 0x14,4,0,0,0,0x90,0x40, /* 2.5 and 4.5 m */
        0x15,4,0,0,0,0xb4,0x42, /* 90 degrees */
        0x1a,4,0,0,0,0x28,0xc1, /* -10.5 m WGS84 ellipsoid */
        0x1b,4,0,0,0,0x80,0x3f, /* distinct MSL altitude, not WGS84 */
        0x1c,4,0,0,0,0xf0,0x40, /* 7.5 m */
        0x27,6,0,0x81,9,0x15,0xcd,0x5b,0x07, 0x2d,1,0,1,
    };
    assert(position_report(0, 1, native_measurements, sizeof(native_measurements)));
    assert(sdx55_geodetic_state.fix.version == 2 && sizeof(struct hakios_sdx55_fix) == 96);
    assert(sdx55_fix_sysctl(NULL, NULL, 0, &read_measurement) == 0);
    const uint32_t native_flags = HAKIOS_SDX55_FIX_HAS_SEMI_MINOR |
        HAKIOS_SDX55_FIX_HAS_SEMI_MAJOR | HAKIOS_SDX55_FIX_HAS_ELLIPSE_AZIMUTH |
        HAKIOS_SDX55_FIX_HAS_ELLIPSOID_ALTITUDE | HAKIOS_SDX55_FIX_HAS_VUNC |
        HAKIOS_SDX55_FIX_HAS_GPS_TIME | HAKIOS_SDX55_FIX_HAS_ALTITUDE_ASSUMED;
    assert(sample.flags == (HAKIOS_SDX55_FIX_HAS_TECHNOLOGY | native_flags));
    assert(sample.semi_minor_m == 2.5f && sample.semi_major_m == 4.5f && sample.ellipse_azimuth_deg == 90);
    assert(sample.altitude_ellipsoid_m == -10.5f && sample.vertical_uncertainty_m == 7.5f);
    assert(sample.gps_week == 2433 && sample.gps_tow_ms == 123456789 && sample.altitude_assumed == 1);
    assert(position_report(0, 1, only_list, sizeof(only_list)));
    assert(sdx55_fix_sysctl(NULL, NULL, 0, &read_measurement) == 0);
    assert(!sample.flags && !sample.semi_minor_m && !sample.semi_major_m && !sample.ellipse_azimuth_deg);
    assert(!sample.altitude_ellipsoid_m && !sample.vertical_uncertainty_m &&
        !sample.gps_week && !sample.gps_tow_ms && !sample.altitude_assumed);
    const uint8_t invalid_native[] = {
        0x23,4,0,1,0,0,0, 0x13,4,0,0,0,0x80,0xbf, /* negative minor */
        0x14,4,0,0,0,0xc0,0x7f, /* NaN major */
        0x15,4,0,0,0,0x35,0x43, /* 181 degrees */
        0x1a,4,0,0,0,0xfa,0xc3, /* -500 inclusive */
        0x1c,4,0,0,0,0x80,0x7f, /* infinite vertical uncertainty */
        0x27,6,0,0x81,9,0,0x84,0x0c,0x24, /* next week, out of range */
        0x2d,1,0,2,
    };
    assert(position_report(0, 1, invalid_native, sizeof(invalid_native)));
    assert(sdx55_geodetic_state.fix.flags == (HAKIOS_SDX55_FIX_HAS_TECHNOLOGY | HAKIOS_SDX55_FIX_HAS_ELLIPSOID_ALTITUDE));
    assert(sdx55_geodetic_state.fix.altitude_ellipsoid_m == -500);
    const uint8_t short_gps[] = {0x23,4,0,1,0,0,0, 0x27,5,0,0x81,9,0,0,0};
    assert(!position_report(0, 1, short_gps, sizeof(short_gps)));
    const uint8_t twice_minor[] = {0x23,4,0,1,0,0,0, 0x13,4,0,0,0,0,0, 0x13,4,0,0,0,0,0};
    assert(!position_report(0, 1, twice_minor, sizeof(twice_minor)));
    const uint8_t duplicate_msl[] = {
        0x23,4,0,1,0,0,0, 0x1b,4,0,0,0,0x80,0x3f, 0x1b,4,0,0,0,0,0,
    };
    assert(!position_report(0, 1, duplicate_msl, sizeof(duplicate_msl)));
    const uint8_t short_reliability[] = {0x23,4,0,1,0,0,0, 0x17,1,0,4};
    assert(!position_report(0, 1, short_reliability, sizeof(short_reliability)));
    _Static_assert(sizeof(struct hakios_sdx55_fix) == 96, "deployed v2 reader ABI");
    const uint8_t geodetic_tlvs[] = {
        0x23,4,0,1,0,0,0, 0x1a,4,0,0,0,0x28,0xc1, /* -10.5 m ellipsoidal */
        0x1b,4,0,0,0,0x34,0xc2, /* -45.0 m above mean sea level */
        0x17,4,0,3,0,0,0,       /* QMI horizontal reliability MEDIUM */
    };
    struct hakios_sdx55_geodetic_fix geo;
    const unsigned root_uid = 0, mobile_uid = 501;
    struct sysctl_req read_geo = { &geo, sizeof(geo), &mobile_uid };
    memset(&geo, 0xa5, sizeof(geo));
    assert(sdx55_geodetic_fix_sysctl(NULL, NULL, 0, &read_geo) == EPERM);
    assert(geo.size == 0xa5a5a5a5); /* denied before any copy */
    read_geo.p = NULL;
    assert(sdx55_geodetic_fix_sysctl(NULL, NULL, 0, &read_geo) == EPERM);
    read_geo.p = &root_uid;
    for (unsigned i = 0; i < 2; i++) {
        assert(position_report(0, 1, geodetic_tlvs, sizeof(geodetic_tlvs)));
        assert(!position_report(1, 1, only_list, sizeof(only_list)));
        assert(slpi_loc_publish(&slpi, 0, NULL));
        assert(sdx55_geodetic_fix_sysctl(NULL, NULL, 0, &read_geo) == 0);
        assert(sdx55_fix_sysctl(NULL, NULL, 0, &read_measurement) == 0);
        assert(geo.size == sizeof(geo) && geo.version == HAKIOS_SDX55_GEODETIC_VERSION);
        assert(!memcmp(&geo.fix, &sample, sizeof(sample)));
        assert(geo.flags == (HAKIOS_SDX55_GEODETIC_HAS_MSL | HAKIOS_SDX55_GEODETIC_HAS_HREL));
        assert(geo.fix.altitude_ellipsoid_m - geo.altitude_msl_m == 34.5f);
        assert(geo.horizontal_reliability == 3);
    }
    uint8_t invalid_geodetic[] = {0x23,4,0,1,0,0,0, 0x1b,4,0,0,0,0xc0,0x7f, 0x17,4,0,5,0,0,0};
    assert(position_report(0, 1, invalid_geodetic, sizeof(invalid_geodetic)));
    assert(sdx55_geodetic_fix_sysctl(NULL, NULL, 0, &read_geo) == 0);
    assert(!geo.flags && !geo.altitude_msl_m && !geo.horizontal_reliability);
    invalid_geodetic[17] = 0; /* explicitly reported NOT_SET differs from an omitted TLV */
    assert(position_report(0, 1, invalid_geodetic, sizeof(invalid_geodetic)));
    assert(sdx55_geodetic_fix_sysctl(NULL, NULL, 0, &read_geo) == 0);
    assert(geo.flags == HAKIOS_SDX55_GEODETIC_HAS_HREL && !geo.horizontal_reliability);
    struct umi_mhi host = {.mode = SDX55_DMS_OFFLINE};
    umi_sdx55_dms_node = 3;
    umi_sdx55_dms_port = 65;
    assert(umi_sdx55_dms_change_mode(&host, SDX55_DMS_ONLINE) == 0 && host.mode == 0);
    assert(umi_sdx55_dms_change_mode(&host, SDX55_DMS_OFFLINE) == 0 && host.mode == 3);
    assert(host.reads == 2 && host.writes == 2);
    int restore_mode = -1;
    host.refusals = 2;
    host.refusal_code = SDX55_DMS_NOT_READY;
    assert(umi_sdx55_dms_online(&host, &restore_mode) == 0);
    assert(host.mode == 0 && restore_mode == 3 && host.writes == 6);
    host.mode = SDX55_DMS_LOW_POWER;
    restore_mode = -1;
    host.refusals = 20;
    assert(umi_sdx55_dms_online(&host, &restore_mode) == ETIMEDOUT);
    assert(restore_mode == -1 && host.mode == SDX55_DMS_LOW_POWER);
    host.mode = SDX55_DMS_OFFLINE;
    host.refusal_code = SDX55_DMS_INVALID_TRANSITION;
    host.refusals = 20;
    unsigned writes = host.writes;
    assert(umi_sdx55_dms_online(&host, &restore_mode) == ETIMEDOUT);
    assert(restore_mode == -1 && host.mode == 3 && host.writes == writes + 2);
    host.mode = 5;
    host.refusals = 1;
    host.refusal_code = SDX55_DMS_NOT_READY;
    restore_mode = -1;
    writes = host.writes;
    assert(umi_sdx55_dms_online(&host, &restore_mode) == 0);
    assert(host.mode == SDX55_DMS_ONLINE && host.writes == writes+3);
    assert(restore_mode == SDX55_DMS_LOW_POWER);
    assert(umi_sdx55_dms_change_mode(&host, restore_mode) == 0 && host.mode == 1);
    uint8_t orbits[1025], encoded[1100];
    for (unsigned i=0; i<sizeof(orbits); i++) orbits[i]=(uint8_t)(i+1);
    const uint8_t tail[] = {0,9,0,0xa7,0,23,0, 1,4,0,1,4,0,0, 2,2,0,2,0,
        3,2,0,2,0, 4,3,0,1,0,1};
    assert(sdx55_encode_orbit_part(9,orbits,sizeof(orbits),2,encoded,sizeof(encoded)) == sizeof(tail));
    assert(!memcmp(encoded,tail,sizeof(tail)));
    assert(sdx55_encode_orbit_part(8,orbits,sizeof(orbits),1,encoded,sizeof(encoded)) == 1053);
    assert(encoded[27]==0 && encoded[28]==4 && !memcmp(encoded+29,orbits,1024));
    assert(sdx55_encode_orbit_part(1,orbits,0,1,encoded,sizeof(encoded)) < 0);
    assert(sdx55_encode_orbit_part(1,orbits,sizeof(orbits),0,encoded,sizeof(encoded)) < 0);
    assert(sdx55_encode_orbit_part(1,orbits,sizeof(orbits),3,encoded,sizeof(encoded)) < 0);
    assert(sdx55_encode_orbit_part(1,orbits,1024,1,encoded,32) < 0);
    uint32_t orbit_status = UINT32_MAX;
    uint8_t ack[] = {1,4,0,0,0,0,0, 0x10,2,0,9,0};
    assert(sdx55_orbit_ack(ack,sizeof(ack),9,&orbit_status) && orbit_status == 0);
    orbit_status = 6;
    assert(!sdx55_orbit_ack(ack,sizeof(ack),10,&orbit_status) && orbit_status == 6);
    assert(!sdx55_orbit_ack(ack,sizeof(ack)-1,9,&orbit_status) && orbit_status == 6);
    assert(!sdx55_orbit_ack(ack+7,5,9,&orbit_status) && orbit_status == 6);
    ack[3] = 3;
    assert(sdx55_orbit_ack(ack,7,9,&orbit_status) && orbit_status == 3); /* optional echoed part */
    const uint8_t quality[] = {0x23,4,0,1,0,0,0,0x2c,5,0,2,19,0,7,0};
    struct hakios_sdx55_geodetic_fix q;
    assert(sdx55_loc_quality(quality,sizeof(quality),&q) && q.fix.technology == 1 && q.fix.used_sv == 2);
    assert(!sdx55_loc_quality(quality,sizeof(quality)-1,&q));
    assert(sdx55_loc_quality(NULL,0,&q) && q.fix.used_sv == -1 && !q.fix.flags);
    uint8_t svs[1+28] = {1};
    svs[1]=0x89; svs[5]=6; svs[12]=3; svs[28]=0x42;
    struct sdx55_sv_quality sv = {0};
    assert(sdx55_sv_list(svs,sizeof(svs),false,true,&sv));
    assert(sv.tracked==1 && sv.bds_tracked==1 && sv.signal==1 && sv.peak_bits==0x42000000);
    /* Match a tracked BeiDou SV and distinguish "no ephemeris" from an omitted field. */
    svs[1] = 0x9b; svs[9] = 205; svs[16] = 2;
    location_log[0] = 0;
    assert(sdx55_sv_list(svs,sizeof(svs),false,true,&sv));
    assert(strstr(location_log, "system=6 svid=205 tracked=1 nav=2 snr_bits=0x42000000"));
    svs[1] = 0x8b;
    assert(sdx55_sv_list(svs,sizeof(svs),false,true,&sv));
    assert(strstr(location_log, "system=6 svid=205 tracked=1 nav=-1"));
    location_log[0] = 0;
    svs[1]=0; assert(sdx55_sv_list(svs,sizeof(svs),false,true,&sv));
    assert(!sv.tracked && !sv.bds_tracked && !sv.signal && !location_log[0]);
    assert(!sdx55_sv_list(svs,sizeof(svs)-1,false,true,&sv));
    #define GSV(s) sdx55_nmea_gsv((const uint8_t *)(s), sizeof(s) - 1)
    assert(GSV("$GBGSV,2,1,07,09") == 7);
    assert(GSV("$BDGSV,1,1,00*00") == 0);
    assert(GSV("$GPGSV,1,1,1") < 0 && GSV("$GPGSV,1,,03,*00") < 0);
    assert(GSV("$GPGSA,1,1,04,*00") < 0 && GSV("$GPGSV,1,1,9999,") < 0);
    uint8_t bds[] = {1,0,0,'$','G','B','G','S','V',',','2',',','1',',','0','7',',','*'};
    bds[1] = sizeof(bds) - 3;
    sdx55_loc_event(0x26, bds, sizeof(bds) - 1);
    assert(!umi_sdx55_loc.nmea_reports);
    sdx55_loc_event(0x26, bds, sizeof(bds));
    assert(umi_sdx55_loc.gsv_reports == 1 && umi_sdx55_loc.max_bds_gsv == 7);
    bds[4] = 'G';
    bds[5] = 'P';
    bds[15] = '9';
    sdx55_loc_event(0x26, bds, sizeof(bds));
    assert(umi_sdx55_loc.max_gsv == 9 && umi_sdx55_loc.max_bds_gsv == 7);
    /* Expanded TLV 0x11: 28-byte svInfo + uint8_t gloFrequency per entry.
     * Field values reflect UMI's QMI system enum, independent of Android GnssStatus. */
    uint8_t ext_sv[3+1+2*29] = {0x11,59,0,2};
    memcpy(ext_sv+4,svs+1,28);
    ext_sv[32] = 11;
    ext_sv[33]=0x9b; ext_sv[37]=6; ext_sv[41]=205; ext_sv[44]=3;
    ext_sv[48]=2; ext_sv[60]=0x42; ext_sv[61]=99;
    sdx55_loc_event(0x25,ext_sv,sizeof(ext_sv));
    assert(umi_sdx55_loc.sv_reports == 1 && umi_sdx55_loc.max_sv == 2);
    assert(umi_sdx55_loc.max_bds_tracked == 1 && umi_sdx55_loc.peak_snr_bits == 0x42000000);
    uint8_t basic_empty_and_ext[4+sizeof(ext_sv)] = {0x10,1,0,0};
    memcpy(basic_empty_and_ext+4,ext_sv,sizeof(ext_sv));
    ext_sv[4]=0x89; ext_sv[8]=1; ext_sv[15]=3; ext_sv[30]=0x20; ext_sv[31]=0x42;
    sdx55_loc_event(0x25,ext_sv,sizeof(ext_sv)); /* unsampled second report */
    assert(umi_sdx55_loc.max_tracked == 2 && umi_sdx55_loc.peak_snr_bits == 0x42200000);
    sdx55_loc_event(0x25,basic_empty_and_ext,sizeof(basic_empty_and_ext));
    assert(umi_sdx55_loc.sv_reports == 3);
    sdx55_loc_event(0x25,ext_sv,sizeof(ext_sv)-1);
    ext_sv[3]=255;
    sdx55_loc_event(0x25,ext_sv,sizeof(ext_sv));
    assert(umi_sdx55_loc.sv_reports == 3 && umi_sdx55_loc.max_sv == 2);
    uint8_t too_many[1+177*29] = {177};
    assert(!sdx55_sv_list(too_many,sizeof(too_many),true,false,&sv));
    too_many[0] = 81;
    assert(!sdx55_sv_list(too_many,1+81*28,false,false,&sv));
    /* Qualcomm expanded NMEA has an empty mandatory TLV and a string in 0x10. */
    uint8_t expanded_sentence[] = {1,0,0,0x10,15,0,'$','G','B','G','S','V',',','1',',','1',',','0','3',',','*'};
    unsigned before_extended = umi_sdx55_loc.gsv_reports;
    sdx55_loc_event(0x26,expanded_sentence,sizeof(expanded_sentence));
    assert(umi_sdx55_loc.gsv_reports == before_extended + 1);
    assert(umi_sdx55_loc.max_bds_gsv == 7);
    sdx55_loc_event(0x26,expanded_sentence,sizeof(expanded_sentence)-1);
    assert(umi_sdx55_loc.gsv_reports == before_extended + 1);
    uint8_t packet[128];
    const uint8_t req0[] = {0x0, 0x1, 0x0, 0x21, 0x0, 0x16, 0x0, 0x1, 0x13, 0x0, 0x74, 0x6d, 0x73, 0x2f, 0x70, 0x64, 0x64, 0x75, 0x6d, 0x70, 0x5f, 0x64, 0x69, 0x73, 0x61, 0x62, 0x6c, 0x65, 0x64};
    const uint8_t resp0[] = {0x2, 0x1, 0x0, 0x21, 0x0, 0x15, 0x0, 0x2, 0x4, 0x0, 0x0, 0x0, 0x0, 0x0, 0x10, 0x2, 0x0, 0x0, 0x0, 0x11, 0x2, 0x0, 0x1, 0x0, 0x12, 0x1, 0x0, 0x0};
    assert(sdx55_pdm_response(req0, sizeof(req0), packet,sizeof(packet)) == sizeof(resp0));
    assert(!memcmp(packet,resp0,sizeof(resp0)));
    const uint8_t req2[] = {0x0, 0x2, 0x0, 0x21, 0x0, 0xc, 0x0, 0x1, 0x9, 0x0, 0x61, 0x76, 0x73, 0x2f, 0x61, 0x75, 0x64, 0x69, 0x6f};
    const uint8_t resp2[] = {0x2, 0x2, 0x0, 0x21, 0x0, 0x30, 0x0, 0x2, 0x4, 0x0, 0x0, 0x0, 0x0, 0x0, 0x10, 0x2, 0x0, 0x1, 0x0, 0x11, 0x2, 0x0, 0x1, 0x0, 0x12, 0x1c, 0x0, 0x1, 0x11, 0x6d, 0x73, 0x6d, 0x2f, 0x61, 0x64, 0x73, 0x70, 0x2f, 0x61, 0x75, 0x64, 0x69, 0x6f, 0x5f, 0x70, 0x64, 0x4a, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0};
    assert(sdx55_pdm_response(req2, sizeof(req2), packet,sizeof(packet)) == sizeof(resp2));
    assert(!memcmp(packet,resp2,sizeof(resp2)));
    const uint8_t standalone_mode[] = {0,1,0,0x4a,0,7,0,1,4,0,4,0,0,0};
    assert(hakios_loc_request(0x4a,1,packet,sizeof(packet),4,NULL) == sizeof(standalone_mode));
    assert(!memcmp(packet,standalone_mode,sizeof(standalone_mode)));
    const uint16_t messages[] = {0x21, 0x22, 0x23, 0x3b, 0xba, 0xbf, 0x37};
    for (unsigned i = 0; i < sizeof(messages) / sizeof(messages[0]); i++) {
        int size = hakios_loc_request(messages[i], 1, packet, sizeof(packet), 0, NULL);
        assert(size > 0 && (size_t)size <= sizeof(packet));
        for (int n = 0; n < size; n++) printf("%02x", packet[n]);
        puts("");
    }
    uint8_t response[] = {2,1,0,0x22,0,7,0,2,4,0,1,0,46,0};
    uint16_t error = UINT16_MAX;
    assert(hakios_loc_result(response,sizeof(response),&error) == EIO && error == 46);
    response[10] = response[12] = 0;
    assert(hakios_loc_result(response,sizeof(response),&error) == 0 && !error);
    response[7] = 3; /* missing mandatory result */
    assert(hakios_loc_result(response,sizeof(response),&error) != 0);
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / "loc.c"
    exe = Path(tmp) / "loc"
    c.write_text(fixture)
    dead_code_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    subprocess.run(["cc", "-std=gnu11", "-ffunction-sections", "-fdata-sections",
                    dead_code_flag, "-pthread", "-I", str(bsd), str(c),
                    str(bsd / "dev/hakios.qmi.c"), "-o", str(exe)], check=True)
    actual = subprocess.check_output([str(exe)], text=True).splitlines()
# Check the five selected LOC events and the one-second fix interval.
expected = [
    "00010021001d0001080087010000000000001004004d48414c1104000100000012010000",
    "00010022003200010100011004000100000011040003000000130400e8030000"
    "15040002000000160400e80300001a080002000000e8030000",
    "0001002300040001010001",
    "0001003b00070010040003000000",
    "000100ba000700010400efcdabba",
    "000100bf000000",
    "00010037000000",
]
assert actual == expected, (actual, expected)
print("PASS: SDX55 GNSS publication and UMI LOC codecs, basic and expanded SV/NMEA, DMS mode transitions, stock PD replies")
