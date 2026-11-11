#ifndef UMI_CELL_PROBE_WIRE_H
#define UMI_CELL_PROBE_WIRE_H

#include <arpa/inet.h>
#ifdef __APPLE__
#include <nameser.h>
#else
#include <arpa/nameser.h>
#endif
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#define CELL_PROBE_HOST "captive.apple.com"

static bool cell_probe_enabled(const char *args)
{
	const char prefix[] = "sdx55_data_call=";
	bool enabled = false;
	while (*args) {
		args += strspn(args, " \t");
		size_t n = strcspn(args, " \t");
		if (n >= sizeof(prefix)-1 && !memcmp(args, prefix, sizeof(prefix)-1)) {
			if (enabled || n != sizeof(prefix) || args[n-1] != '1') return false;
			enabled = true;
		}
		args += n;
	}
	return enabled;
}

static bool cell_probe_public_ip(struct in_addr ip)
{
	uint32_t a = ntohl(ip.s_addr);
	unsigned octet = a >> 24;
	return octet != 0 && octet != 10 && octet != 127 && octet < 224 &&
	    (a & 0xffc00000u) != 0x64400000u && (a >> 16) != 0xa9fe &&
	    (a >> 20) != 0xac1 && (a >> 16) != 0xc0a8 &&
	    (a >> 17) != (0xc6120000u >> 17) && (a >> 8) != 0xc00000 &&
	    (a >> 8) != 0xc00002 && (a >> 8) != 0xc05863 &&
	    (a >> 8) != 0xc63364 && (a >> 8) != 0xcb0071;
}

static bool cell_probe_dns(const uint8_t *packet, size_t len, uint16_t xid, struct in_addr *ip)
{
	ns_msg msg;
	ns_rr rr;
	if (len < 12 || len > 4096 || ns_initparse(packet, (int)len, &msg) != 0 ||
	    ns_msg_id(msg) != xid || !ns_msg_getflag(msg, ns_f_qr) || ns_msg_getflag(msg, ns_f_tc) ||
	    ns_msg_getflag(msg, ns_f_opcode) != 0 || ns_msg_getflag(msg, ns_f_rcode) != 0 ||
	    ns_msg_count(msg, ns_s_qd) != 1 || ns_parserr(&msg, ns_s_qd, 0, &rr) != 0 ||
	    strcasecmp(ns_rr_name(rr), CELL_PROBE_HOST) || ns_rr_type(rr) != ns_t_a ||
	    ns_rr_class(rr) != ns_c_in) return false;

	char current[NS_MAXDNAME] = CELL_PROBE_HOST, next[NS_MAXDNAME];
	for (unsigned hop = 0; hop < 8; hop++) {
		next[0] = 0;
		for (int i = 0; i < ns_msg_count(msg, ns_s_an); i++) {
			if (ns_parserr(&msg, ns_s_an, i, &rr) != 0) return false;
			if (ns_rr_class(rr) != ns_c_in || strcasecmp(ns_rr_name(rr), current)) continue;
			if (ns_rr_type(rr) == ns_t_a && ns_rr_rdlen(rr) == sizeof(*ip)) {
				memcpy(ip, ns_rr_rdata(rr), sizeof(*ip));
				if (cell_probe_public_ip(*ip)) return true;
			} else if (ns_rr_type(rr) == ns_t_cname) {
				int used = ns_name_uncompress(packet, packet+len, ns_rr_rdata(rr), next, sizeof(next));
				if (used <= 0 || used != ns_rr_rdlen(rr)) return false;
			}
		}
		if (!next[0]) return false;
		strcpy(current, next);
	}
	return false;
}

static bool cell_probe_http(const char *response)
{
	const char expected[] = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
	if (strncmp(response,"HTTP/1.0 200 ",13) && strncmp(response,"HTTP/1.1 200 ",13)) return false;
	const char *body = strstr(response,"\r\n\r\n");
	if (!body) return false;
	body += 4;
	size_t n = sizeof(expected)-1;
	if (strncasecmp(body, expected, n)) return false;
	body += n;
	return body[strspn(body, "\r\n \t")] == '\0';
}
#endif
