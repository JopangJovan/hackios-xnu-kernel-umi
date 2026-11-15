/* QRTR and the inbox queue are owned by the existing modem worker.
 * The views, receiver lease and queued submission use cell_status_lock. */
#define SDX55_SMS_QUEUE_COUNT 8u
#define SDX55_SMS_IMS_REGISTERED 2u
static struct sdx55_cell_endpoint sms_endpoint = {.service = SDX55_WMS_SERVICE};
static struct {
	bool enabled, opened, primary, reporting, routes_changed, ready;
	uint8_t subscription;
	uint64_t retry_at;
	struct sdx55_sms_routes saved;
	struct sdx55_sms_event queue[SDX55_SMS_QUEUE_COUNT];
	unsigned head, count;
} sms_worker;
static struct {
	bool pending, done;
	uint16_t txn, message, service_error;
	int error;
	struct sdx55_sms_routes routes;
	struct sdx55_sms_event entry;
	unsigned registration;
} sms_query;
static struct umi_sms_snapshot sms_view = {.version = UMI_SMS_VERSION};
static uint64_t sms_next, sms_deadline, sms_lease;
static unsigned sms_disposition;
static struct umi_sms_submission sms_outbox;
static struct umi_sms_send_status sms_send_view = {.version = UMI_SMS_VERSION};
static uint16_t sms_send_txn;
static uint64_t sms_send_deadline;

static void
sdx55_sms_end_send(int error, bool teardown)
{
	sms_send_deadline = 0;
	lck_mtx_lock(&cell_status_lock);
	if (sms_send_view.pending) {
		sms_send_view.error = error;
		sms_send_view.pending = 0;
	}
	bzero(&sms_outbox, sizeof(sms_outbox));
	if (teardown) {
		sms_view = (struct umi_sms_snapshot){.version = UMI_SMS_VERSION};
		sms_disposition = 0;
	}
	lck_mtx_unlock(&cell_status_lock);
}

static void
sdx55_sms_announce(uint32_t type, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	if (!sms_worker.enabled || service != SDX55_WMS_SERVICE || instance != 1 ||
	    !port || !node || node == UINT32_MAX || (umi_sdx55_dms_node && node != umi_sdx55_dms_node)) return;
	if (type == 4 && (sms_endpoint.node != node || sms_endpoint.port != port)) {
		sms_endpoint.node = node;
		sms_endpoint.port = port;
	} else if (type == 5 && sms_endpoint.node == node && sms_endpoint.port == port) {
		sms_endpoint.port = 0;
	} else return;
	sms_endpoint.generation++;
	sms_worker.opened = sms_worker.primary = sms_worker.reporting = sms_worker.routes_changed = sms_worker.ready = false;
	sms_worker.head = sms_worker.count = 0;
	bzero(sms_worker.queue, sizeof(sms_worker.queue));
	if (sms_query.pending) {
		sms_query.error = ENXIO;
		sms_query.done = true;
	}
	sdx55_sms_end_send(ENXIO, true);
}

static void
sdx55_sms_response(const uint8_t *q, size_t size)
{
	sms_query.error = sdx55_cell_body(sms_query.message, sms_query.txn, q, size, &sms_query.service_error);
	if (sms_query.error) return;
	bool data_seen = false;
	for (size_t i = 7; i < size;) {
		unsigned tag = q[i], length = sdx55_cell_le16(q + i + 1);
		const uint8_t *p = q + i + 3;
		i += 3 + length;
		if (sms_query.message == SDX55_WMS_GET_ROUTES && tag == 1) {
			sms_query.error = data_seen ? EINVAL : sdx55_sms_routes_decode(p, length, &sms_query.routes);
			data_seen = true;
		} else if (sms_query.message == SDX55_WMS_GET_ROUTES && tag == 0x10) {
			if (length != 1 || sms_query.routes.has_status_reports) {
				sms_query.error = EINVAL;
				return;
			}
			sms_query.routes.has_status_reports = true;
			sms_query.routes.status_reports = p[0];
		} else if (sms_query.message == SDX55_WMS_RAW_READ && tag == 1) {
			sms_query.error = data_seen ? EINVAL : sdx55_sms_stored_tpdu(p, length, &sms_query.entry);
			data_seen = true;
		} else if (sms_query.message == SDX55_WMS_REGISTRATION && tag == 0x10 && length == 1) {
			sms_query.registration = p[0];
		}
		if (sms_query.error) return;
	}
	if (!data_seen && (sms_query.message == SDX55_WMS_GET_ROUTES || sms_query.message == SDX55_WMS_RAW_READ))
		sms_query.error = EINVAL;
}

static void
sdx55_sms_receive(uint32_t node, uint32_t port, const uint8_t *q, size_t size)
{
	if (!sms_worker.opened || !port || node != sms_endpoint.node || port != sms_endpoint.port || size < 7) return;
	if (q[0] == QMI_RESPONSE && sms_send_deadline &&
	    sdx55_cell_le16(q + 1) == sms_send_txn && sdx55_cell_le16(q + 3) == SDX55_WMS_RAW_SEND) {
		uint16_t service_error = 0;
		int error = sdx55_cell_body(SDX55_WMS_RAW_SEND, sms_send_txn, q, size, &service_error);
		unsigned rp_cause = 0, tp_cause = 0;
		if (error == EIO) {
			for (size_t i = 7; i < size;) {
				unsigned tag = q[i], length = sdx55_cell_le16(q + i + 1);
				if (tag == 0x12 && length == 3) {
					rp_cause = sdx55_cell_le16(q + i + 3);
					tp_cause = q[i + 5];
				}
				i += 3 + length;
			}
		}
		hakios_tl("UMI SDX55: SMS send result errno=%d qmi=%u rp=%u tp=%u\n",
		    error, service_error, rp_cause, tp_cause);
		sdx55_sms_end_send(error, false);
		return;
	}
	if (q[0] == QMI_RESPONSE && sms_query.pending && !sms_query.done &&
	    sdx55_cell_le16(q + 1) == sms_query.txn && sdx55_cell_le16(q + 3) == sms_query.message) {
		sdx55_sms_response(q, size);
		sms_query.done = true;
		return;
	}
	if (q[0] != QMI_INDICATION || sdx55_cell_le16(q + 3) != SDX55_WMS_EVENT_REPORT) return;
	struct sdx55_sms_event entry;
	int error = sdx55_sms_event_decode(q, size, &entry);
	if (error) {
		hakios_tl("UMI SDX55: SMS indication ignored errno=%d\n", error);
		return;
	}
	for (unsigned i = 0; i < sms_worker.count; i++) {
		const struct sdx55_sms_event *item = &sms_worker.queue[(sms_worker.head + i) % SDX55_SMS_QUEUE_COUNT];
		if (item->transfer != entry.transfer) continue;
		if (entry.transfer ? item->transaction == entry.transaction && item->ims == entry.ims :
		    item->storage == entry.storage && item->index == entry.index) return;
	}
	/* Leave an overloaded transfer unacknowledged so the carrier can retry it. */
	if (sms_worker.count == SDX55_SMS_QUEUE_COUNT) {
		hakios_tl("UMI SDX55: SMS inbox full\n");
		return;
	}
	sms_worker.queue[(sms_worker.head + sms_worker.count++) % SDX55_SMS_QUEUE_COUNT] = entry;
	hakios_tl("UMI SDX55: SMS received transfer=%u ims=%u ack=%u bytes=%u\n",
	    entry.transfer, entry.ims, entry.ack, entry.length);
}

static int
sdx55_sms_command(struct umi_mhi *host, uint16_t message, unsigned argument,
    const struct sdx55_sms_routes *routes, const struct sdx55_sms_event *entry)
{
	if (!sms_endpoint.port || sms_endpoint.node != umi_sdx55_dms_node || host->poisoned) return ENXIO;
	uint32_t packet[25] = {1, 1, 1, SDX55_WMS_PORT, 0, 0, sms_endpoint.node, sms_endpoint.port};
	uint16_t txn = sms_query.txn + 1;
	size_t n = sdx55_sms_request(message, txn, argument, routes, entry,
	    (uint8_t *)(packet + 8), sizeof(packet) - 32);
	if (!n) return EINVAL;
	sms_query = (typeof(sms_query)){.pending = true, .txn = txn, .message = message};
	sms_worker.opened = true;
	packet[5] = n;
	int error = umi_mhi_send_qrtr_packet(host, packet, 32 + ((n + 3) & ~3u), 250, UINT16_MAX);
	for (unsigned ms = 0; !error && !sms_query.done && ms < 250; ms += 5) {
		error = umi_sdx55_poll_loc(host, 5);
		if (!error) error = sdx55_flush_flow_acks(host);
	}
	sms_query.pending = false;
	if (!error) error = sms_query.done ? sms_query.error : ETIMEDOUT;
	hakios_tl("UMI SDX55: SMS QMI message=%#x errno=%d qmi=%u\n", message, error, sms_query.service_error);
	return error;
}

static void
sdx55_sms_finish(struct umi_mhi *host)
{
	sdx55_sms_end_send(ENXIO, true);
	if (sms_worker.routes_changed)
		(void)sdx55_sms_command(host, SDX55_WMS_SET_ROUTES, 0, &sms_worker.saved, NULL);
	if (sms_worker.reporting)
		(void)sdx55_sms_command(host, SDX55_WMS_EVENT_REPORT, 0, NULL, NULL);
	if (sms_worker.primary)
		(void)sdx55_sms_command(host, SDX55_WMS_PRIMARY, 0, NULL, NULL);
	if (sms_worker.opened && !host->poisoned && umi_sdx55_peer) {
		uint32_t bye[] = {1, 6, 1, SDX55_WMS_PORT, 0, 20, umi_sdx55_peer, 0xfffffffe,
		    6, 1, SDX55_WMS_PORT, 0, 0};
		(void)umi_mhi_send_qrtr_packet(host, bye, sizeof(bye), 250, UINT16_MAX);
	}
	sms_worker.opened = sms_worker.primary = sms_worker.reporting = sms_worker.routes_changed = sms_worker.ready = false;
	sms_worker.head = sms_worker.count = 0;
	bzero(sms_worker.queue, sizeof(sms_worker.queue));
	bzero(&sms_query.entry, sizeof(sms_query.entry));
}

static bool
sdx55_sms_setup(struct umi_mhi *host)
{
	int error = sdx55_sms_command(host, SDX55_WMS_BIND, sms_worker.subscription, NULL, NULL);
	if (!error) error = sdx55_sms_command(host, SDX55_WMS_GET_ROUTES, 0, NULL, NULL);
	if (error) return false;
	sms_worker.saved = sms_query.routes;
	struct sdx55_sms_routes routes = sdx55_sms_receive_routes(&sms_worker.saved);
	bool have_default = false;
	for (unsigned i = 0; i < routes.count; i++)
		if (!routes.entries[i].type && routes.entries[i].message_class == 4) have_default = true;
	if (!have_default) return false; /* Cannot restore an absent default route. */
	sms_worker.reporting = true; /* A lost reply can still have applied a SET. */
	error = sdx55_sms_command(host, SDX55_WMS_EVENT_REPORT, 1, NULL, NULL);
	if (!error) {
		sms_worker.primary = true;
		error = sdx55_sms_command(host, SDX55_WMS_PRIMARY, 1, NULL, NULL);
	}
	if (!error && memcmp(routes.entries, sms_worker.saved.entries, routes.count * sizeof(routes.entries[0]))) {
		sms_worker.routes_changed = true;
		error = sdx55_sms_command(host, SDX55_WMS_SET_ROUTES, 0, &routes, NULL);
	}
	if (error) return false;
	if (!sdx55_sms_command(host, SDX55_WMS_REGISTRATION, 0, NULL, NULL))
		hakios_tl("UMI SDX55: SMS transport registration=%u\n", sms_query.registration);
	return true;
}

static void
sdx55_sms_send_tick(struct umi_mhi *host, uint64_t now)
{
	if (sms_send_deadline) {
		if (now < sms_send_deadline) return;
		hakios_tl("UMI SDX55: SMS send response timed out\n");
		sdx55_sms_end_send(ETIMEDOUT, false);
		return;
	}
	lck_mtx_lock(&cell_status_lock);
	struct umi_sms_submission submit = sms_outbox;
	bzero(&sms_outbox, sizeof(sms_outbox));
	bool ims = voice_view.ims_registration == SDX55_SMS_IMS_REGISTERED &&
	    voice_worker.subscription == sms_worker.subscription;
	lck_mtx_unlock(&cell_status_lock);
	if (!submit.length) return;

	uint32_t packet[8 + (UMI_SMS_SUBMIT_MAX + 20) / 4] = {
	    1, 1, 1, SDX55_WMS_PORT, 0, 0, sms_endpoint.node, sms_endpoint.port,
	};
	size_t n = sdx55_sms_send_request(&submit, ++sms_send_txn, ims,
	    (uint8_t *)(packet + 8), sizeof(packet) - 32);
	bzero(&submit, sizeof(submit));
	if (!n) {
		sdx55_sms_end_send(EINVAL, false);
		return;
	}
	/* The regular modem poll dispatches the reply. Do not wait here while
	 * the network is processing the SMS, since this worker also runs calls. */
	clock_interval_to_deadline(45, NSEC_PER_SEC, &sms_send_deadline);
	packet[5] = n;
	hakios_tl("UMI SDX55: SMS send submit ims=%u\n", ims);
	int error = umi_mhi_send_qrtr_packet(host, packet, 32 + ((n + 3) & ~3u), 250, UINT16_MAX);
	bzero(packet, sizeof(packet));
	if (error && sms_send_deadline) {
		hakios_tl("UMI SDX55: SMS send transport errno=%d\n", error);
		sdx55_sms_end_send(error, false);
	}
}

static void
sdx55_sms_tick(struct umi_mhi *host)
{
	if (!sms_worker.enabled || !sms_worker.subscription || !sms_endpoint.port || host->poisoned) return;
	uint64_t now = mach_absolute_time();
	lck_mtx_lock(&cell_status_lock);
	bool receiver = now < sms_lease;
	lck_mtx_unlock(&cell_status_lock);
	if (!sms_worker.ready) {
		if (!receiver || voice_worker.target[0] || now < sms_worker.retry_at) return;
		clock_interval_to_deadline(15, NSEC_PER_SEC, &sms_worker.retry_at);
		uint32_t generation = sms_endpoint.generation;
		if (!sdx55_sms_setup(host) || generation != sms_endpoint.generation) {
			sdx55_sms_finish(host);
			return;
		}
		sms_worker.ready = true;
		lck_mtx_lock(&cell_status_lock);
		sms_view.ready = 1;
		sms_view.subscription = sms_worker.subscription;
		lck_mtx_unlock(&cell_status_lock);
		hakios_tl("UMI SDX55: SMS receive ready subscription=%u\n", sms_worker.subscription);
	}
	if (receiver) sdx55_sms_send_tick(host, now);
	if (sms_worker.count) {
		struct sdx55_sms_event entry = sms_worker.queue[sms_worker.head];
		uint32_t generation = sms_endpoint.generation;
		if (!entry.transfer && !entry.length) {
			int error = sdx55_sms_command(host, SDX55_WMS_RAW_READ, 0, NULL, &entry);
			if (generation != sms_endpoint.generation) return;
			if (!error) {
				entry.length = sms_query.entry.length;
				memcpy(entry.pdu, sms_query.entry.pdu, entry.length);
				sms_worker.queue[sms_worker.head] = entry;
				bzero(&sms_query.entry, sizeof(sms_query.entry));
			}
		}
		lck_mtx_lock(&cell_status_lock);
		unsigned disposition = sms_disposition;
		if (!sms_view.sequence && entry.length && receiver) {
			sms_view.sequence = ++sms_next;
			sms_view.length = entry.length;
			sms_view.subscription = sms_worker.subscription;
			memcpy(sms_view.pdu, entry.pdu, entry.length);
			clock_interval_to_deadline(20, NSEC_PER_SEC, &sms_deadline);
		}
		if (!disposition && (!receiver || (sms_view.sequence && now >= sms_deadline))) disposition = 2;
		bool waiting = sms_view.sequence && !disposition;
		if (!waiting) {
			sms_view = (struct umi_sms_snapshot){
				.version = UMI_SMS_VERSION, .ready = 1, .subscription = sms_worker.subscription,
			};
			sms_disposition = 0;
		}
		lck_mtx_unlock(&cell_status_lock);
		if (waiting) return;
		bzero(&sms_worker.queue[sms_worker.head], sizeof(entry));
		sms_worker.head = (sms_worker.head + 1) % SDX55_SMS_QUEUE_COUNT;
		sms_worker.count--;
		if (entry.ack)
			(void)sdx55_sms_command(host, SDX55_WMS_ACK, disposition, NULL, &entry);
		hakios_tl("UMI SDX55: SMS disposition=%u\n", disposition);
	}
	if (!receiver) sdx55_sms_finish(host);
}

static bool
sdx55_sms_client(proc_t process)
{
	const char *identity = cs_identity_get(process);
	return identity && !strcmp(identity, "com.apple.imagent") && csproc_get_platform_binary(process);
}

static int
sdx55_sms_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (!sdx55_sms_client(req->p)) return EPERM;
	if (!req->newptr) {
		lck_mtx_lock(&cell_status_lock);
		struct umi_sms_snapshot current = sms_view;
		lck_mtx_unlock(&cell_status_lock);
		return SYSCTL_OUT(req, &current, sizeof(current));
	}
	if (req->newlen != sizeof(struct umi_sms_receipt)) return EINVAL;
	struct umi_sms_receipt receipt;
	int error = SYSCTL_IN(req, &receipt, sizeof(receipt));
	if (error) return error;
	if (receipt.version != UMI_SMS_VERSION || receipt.accepted > 1) return EINVAL;
	lck_mtx_lock(&cell_status_lock);
	if (receipt.sequence) {
		if (receipt.sequence != sms_view.sequence || sms_disposition) error = ESTALE;
		else sms_disposition = receipt.accepted ? 1 : 3;
	} else {
		clock_interval_to_deadline(8, NSEC_PER_SEC, &sms_lease);
	}
	lck_mtx_unlock(&cell_status_lock);
	return error;
}

static int
sdx55_sms_send_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (!sdx55_sms_client(req->p)) return EPERM;
	if (!req->newptr) {
		lck_mtx_lock(&cell_status_lock);
		struct umi_sms_send_status current = sms_send_view;
		lck_mtx_unlock(&cell_status_lock);
		return SYSCTL_OUT(req, &current, sizeof(current));
	}
	if (req->newlen != sizeof(struct umi_sms_submission)) return EINVAL;
	struct umi_sms_submission submit;
	int error = SYSCTL_IN(req, &submit, sizeof(submit));
	if (error) return error;
	if (submit.version != UMI_SMS_VERSION || !submit.token || !sdx55_sms_submit_valid(&submit)) return EINVAL;
	lck_mtx_lock(&cell_status_lock);
	if (!sms_view.ready || mach_absolute_time() >= sms_lease) error = ENXIO;
	else if (sms_send_view.pending) error = EBUSY;
	else {
		sms_outbox = submit;
		sms_send_view = (struct umi_sms_send_status){
			.version = UMI_SMS_VERSION, .token = submit.token, .pending = 1,
		};
	}
	lck_mtx_unlock(&cell_status_lock);
	bzero(&submit, sizeof(submit));
	return error;
}
