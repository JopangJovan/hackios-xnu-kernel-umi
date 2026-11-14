/* Runs on the SDX55 mission worker. IMS owns a WDS client and QMAP mux
 * separate from the ordinary IPv4 session. QRTR callbacks only stage work. */
static struct {
	bool advertised, pending, active, opened, awaiting_address;
	uint32_t node, port, handle;
	uint16_t indication_txn;
	uint64_t address_deadline, retry_at;
	struct sdx55_ims_request inbox, origin;
	struct sdx55_cell_endpoint wds;
} ims_dcm;

static struct {
	bool pending, done;
	uint16_t txn, message;
	int error;
	struct sdx55_ims_wds_reply reply;
} ims_wds_query;

static void
sdx55_ims_receive(uint32_t local, uint32_t node, uint32_t port, const uint8_t *q, size_t n)
{
	if (!ims_dcm.advertised || !node || node != umi_sdx55_dms_node || !port || n < 7) return;
	if (local == SDX55_IMS_WDS_PORT) {
		if (!ims_wds_query.pending || ims_wds_query.done || node != ims_dcm.wds.node || port != ims_dcm.wds.port ||
		    q[0] != QMI_RESPONSE || sdx55_cell_le16(q+1) != ims_wds_query.txn ||
		    sdx55_cell_le16(q+3) != ims_wds_query.message) return;
		ims_wds_query.error = sdx55_ims_wds_decode(ims_wds_query.message, ims_wds_query.txn, q, n, &ims_wds_query.reply);
		ims_wds_query.done = true;
		return;
	}
	if (ims_dcm.pending || q[0] != QMI_REQUEST) return;
	struct sdx55_ims_request r;
	int error = sdx55_ims_dcm_request(q, n, &r);
	if (error) {
		hakios_tl("UMI SDX55: IMS DCM ignored message=%#x errno=%d\n", sdx55_cell_le16(q+3), error);
		return;
	}
	if (ims_dcm.active && (node != ims_dcm.node || port != ims_dcm.port)) return;
	ims_dcm.node = node;
	ims_dcm.port = port;
	ims_dcm.inbox = r;
	ims_dcm.pending = true;
}

static int
sdx55_ims_dcm_send(struct umi_mhi *host, const struct sdx55_ims_request *r, bool indication,
    uint16_t error, const char *address)
{
	uint32_t packet[28] = {1, 1, 1, SDX55_IMSDCM_PORT, 0, 0, ims_dcm.node, ims_dcm.port};
	uint16_t txn = indication ? ++ims_dcm.indication_txn : r->txn;
	size_t n = sdx55_ims_dcm_reply(r, indication, txn, error, address, (uint8_t *)(packet+8), sizeof(packet)-32);
	if (!n) return EINVAL;
	packet[5] = (uint32_t)n;
	return umi_mhi_send_qrtr_packet(host, packet, 32+((n+3)&~3u), 1000, UINT16_MAX);
}

static int
sdx55_ims_wds(struct umi_mhi *host, uint16_t message, unsigned argument, struct sdx55_ims_wds_reply *reply)
{
	*reply = (struct sdx55_ims_wds_reply){0};
	const struct sdx55_cell_endpoint *wds = &ims_dcm.wds;
	if (host->poisoned || !sdx55_cell_endpoint_current(wds)) return ENXIO;
	uint32_t packet[24] = {1, 1, 1, SDX55_IMS_WDS_PORT, 0, 0, wds->node, wds->port};
	uint16_t txn = ims_wds_query.txn + 1;
	size_t n = sdx55_ims_wds_request(message, txn, SDX55_PCIE_DATA_IFACE, argument, (uint8_t *)(packet+8), sizeof(packet)-32);
	if (!n) return EINVAL;
	ims_wds_query = (typeof(ims_wds_query)){.txn=txn, .message=message, .pending=true};
	packet[5] = (uint32_t)n;
	ims_dcm.opened = true;
	int error = umi_mhi_send_qrtr_packet(host, packet, 32+((n+3)&~3u), 1000, UINT16_MAX);
	unsigned timeout = message == SDX55_WDS_START ? 5000 : 1500;
	for (unsigned ms = 0; !error && !ims_wds_query.done && ms < timeout; ms += 10) {
		error = umi_sdx55_poll_loc(host, 10);
		if (!error) error = sdx55_flush_flow_acks(host);
		if (!error && !sdx55_cell_endpoint_current(wds)) error = ENXIO;
	}
	ims_wds_query.pending = false;
	*reply = ims_wds_query.reply;
	if (!error) error = ims_wds_query.done ? ims_wds_query.error : ETIMEDOUT;
	hakios_tl("UMI SDX55: IMS WDS message=%#x errno=%d qmi=%u\n", message, error, reply->qmi_error);
	return error;
}

static void
sdx55_ims_dcm_close(struct umi_mhi *host)
{
	if (ims_dcm.handle) {
		struct sdx55_ims_wds_reply reply;
		(void)sdx55_ims_wds(host, SDX55_WDS_STOP, ims_dcm.handle, &reply);
	}
	if (ims_dcm.opened && !host->poisoned && umi_sdx55_peer) {
		uint32_t bye[] = {1, 6, 1, SDX55_IMS_WDS_PORT, 0, 20, umi_sdx55_peer, 0xfffffffe,
		    6, 1, SDX55_IMS_WDS_PORT, 0, 0};
		(void)umi_mhi_send_qrtr_packet(host, bye, sizeof(bye), 1000, UINT16_MAX);
	}
	ims_dcm.handle = 0;
	ims_dcm.opened = ims_dcm.active = ims_dcm.awaiting_address = false;
}

static void
sdx55_ims_dcm_complete(struct umi_mhi *host, const uint8_t *ip)
{
	char text[40];
	if (ip) {
		snprintf(text, sizeof(text), "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
		    ip[0], ip[1], ip[2], ip[3], ip[4], ip[5], ip[6], ip[7],
		    ip[8], ip[9], ip[10], ip[11], ip[12], ip[13], ip[14], ip[15]);
	}
	int error = sdx55_ims_dcm_send(host, &ims_dcm.origin, true, ip ? 0 : 13, ip ? text : NULL);
	hakios_tl("UMI SDX55: IMS DCM activation indication IPv6=%d errno=%d\n", ip != NULL, error);
	ims_dcm.awaiting_address = false;
	if (!ip || error) sdx55_ims_dcm_close(host);
}

static void
sdx55_ims_dcm_tick(struct umi_mhi *host, unsigned subscription)
{
	if (!ims_dcm.advertised || host->poisoned || !subscription) return;
	if (ims_dcm.pending) {
		const struct sdx55_ims_request r = ims_dcm.inbox;
		ims_dcm.pending = false;
		if (r.message == SDX55_IMS_DEACTIVATE) {
			bool owned = ims_dcm.active && r.id == SDX55_IMS_PDP_ID;
			(void)sdx55_ims_dcm_send(host, &r, false, owned ? 0 : 48, NULL);
			if (owned) sdx55_ims_dcm_close(host);
			return;
		}
		bool duplicate = ims_dcm.active && ims_dcm.origin.sequence == r.sequence && ims_dcm.origin.instance == r.instance;
		unsigned reject = r.instance != subscription ? 48 : ims_dcm.active && !duplicate ? 23 : 0;
		int error = sdx55_ims_dcm_send(host, &r, false, reject, NULL);
		if (error || reject || duplicate) return;
		hakios_tl("UMI SDX55: IMS DCM activate subscription=%u profile=%u mux=%u\n", subscription, r.profile, SDX55_IMS_WDS_MUX);
		ims_dcm.active = true;
		ims_dcm.origin = r;
		ims_dcm.wds = sdx55_services[CELL_WDS];
		struct sdx55_ims_wds_reply reply;
		error = sdx55_ims_wds(host, SDX55_WDS_BIND_MUX, 0, &reply);
		if (!error) error = sdx55_ims_wds(host, SDX55_WDS_BIND_SUBSCRIPTION, subscription, &reply);
		if (!error) error = sdx55_ims_wds(host, SDX55_WDS_SET_FAMILY, 0, &reply);
		if (!error) error = sdx55_ims_wds(host, SDX55_WDS_START, r.profile, &reply);
		if (error) { sdx55_ims_dcm_complete(host, NULL); return; }
		ims_dcm.handle = reply.handle;
		ims_dcm.awaiting_address = true;
		ims_dcm.retry_at = 0;
		clock_interval_to_deadline(5, NSEC_PER_SEC, &ims_dcm.address_deadline);
	}
	if (ims_dcm.awaiting_address && mach_absolute_time() >= ims_dcm.retry_at) {
		struct sdx55_ims_wds_reply reply;
		int error = sdx55_ims_wds(host, SDX55_WDS_SETTINGS, 0, &reply);
		if (!error) sdx55_ims_dcm_complete(host, reply.address);
		else if (mach_absolute_time() >= ims_dcm.address_deadline || error == ENXIO || error == EPROTO)
			sdx55_ims_dcm_complete(host, NULL);
		else clock_interval_to_deadline(500, NSEC_PER_MSEC, &ims_dcm.retry_at);
	}
}

static void
sdx55_ims_dcm_announce(struct umi_mhi *host)
{
	uint32_t enabled = 0;
	if (!PE_parse_boot_argn("sdx55_voice", &enabled, sizeof(enabled)) || enabled != 1) return;
	uint32_t announce[] = {1, 4, 1, 0xfffffffe, 0, 20, umi_sdx55_peer,
	    0xfffffffe, 4, SDX55_IMSDCM_SERVICE, 1, 1, SDX55_IMSDCM_PORT};
	ims_dcm.advertised = true;
	int error = umi_mhi_send_qrtr_packet(host, announce, sizeof(announce), 1000, UINT16_MAX);
	if (error) ims_dcm.advertised = false;
	hakios_tl("UMI SDX55: IMS DCM advertise service=%u errno=%d\n", SDX55_IMSDCM_SERVICE, error);
}
