/* MHI/QRTR calls run on the existing modem worker. The Phone app only queues
 * commands and reads the snapshot. Phone numbers never enter the log. */
static struct sdx55_cell_endpoint voice_endpoint = {.service=SDX55_VOICE_SERVICE};
static struct sdx55_cell_endpoint imsa_endpoint = {.service=SDX55_IMSA_SERVICE};
static struct sdx55_cell_endpoint imss_endpoint = {.service=SDX55_IMSS_SERVICE};
static struct {
	uint16_t txn, message;
	uint32_t service, node, port;
	bool pending, done;
	int error;
	struct sdx55_voice_result reply;
} voice_query;
static struct {
	bool armed, owned, uncertain, seen, has_indication;
	uint8_t id, subscription;
	uint32_t voice_generation, imsa_generation, imss_generation;
	uint16_t ims_reason;
	bool ims_bound, ims_queries_rejected;
	uint64_t poll_after, ims_after, originate_until;
	char target[UMI_VOICE_NUMBER_SIZE];
	struct sdx55_voice_result indication;
} voice_worker;
static struct umi_voice_snapshot voice_view = {
	.size=sizeof(voice_view), .version=UMI_VOICE_VERSION, .ims_registration=UMI_VOICE_IMS_UNKNOWN,
	.ims_voice=UMI_VOICE_IMS_UNKNOWN, .ims_technology=UMI_VOICE_IMS_UNKNOWN
};
static struct umi_voice_request voice_request;
static struct umi_voice_route_status voice_route_view;
static struct umi_voice_mute_status voice_mute_view;
static bool voice_accepting;

static void
sdx55_voice_publish_route(unsigned pending, unsigned error)
{
	unsigned route = sdx55_voice_audio_route();
	lck_mtx_lock(&cell_status_lock);
	voice_route_view = (struct umi_voice_route_status){route, pending, error};
	if (!route) bzero(&voice_mute_view, sizeof(voice_mute_view));
	lck_mtx_unlock(&cell_status_lock);
	hakios_tl("UMI SDX55: voice output route=%u pending=%u errno=%u\n", route, pending, error);
}

static void
sdx55_voice_publish(unsigned state, int error, unsigned qmi_error, const struct sdx55_voice_call *call)
{
	lck_mtx_lock(&cell_status_lock);
	bool changed = voice_view.state != state || voice_view.error != (unsigned)error ||
	    voice_view.qmi_error != qmi_error || (call && voice_view.end_reason != call->reason);
	voice_view.state = state;
	voice_view.error = error;
	voice_view.qmi_error = qmi_error;
	if (call) {
		voice_view.end_reason = call->reason;
		voice_view.call_type = call->type;
		voice_view.call_mode = call->mode;
	}
	lck_mtx_unlock(&cell_status_lock);
	if (changed) hakios_tl("UMI SDX55: voice state=%u errno=%d qmi=%u end=%u type=%u mode=%u\n",
	    state, error, qmi_error, call ? call->reason : 0, call ? call->type : 0, call ? call->mode : 0);
}

static void
sdx55_voice_announce(uint32_t type, uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	struct sdx55_cell_endpoint *ep;
	if (service == SDX55_VOICE_SERVICE && instance == 2) ep = &voice_endpoint;
	else if (service == SDX55_IMSA_SERVICE && instance == 1) ep = &imsa_endpoint;
	else if (service == SDX55_IMSS_SERVICE && instance == 1) ep = &imss_endpoint;
	else return;
	if (!node || node == UINT32_MAX || !port || (umi_sdx55_dms_node && node != umi_sdx55_dms_node)) return;
	if (type == 4 && (ep->node != node || ep->port != port)) {
		ep->node = node;
		ep->port = port;
	} else if (type == 5 && ep->node == node && ep->port == port) ep->port = 0;
	else return;
	ep->generation++;
	if (voice_query.pending && voice_query.service == service) {
		voice_query.done = true;
		voice_query.error = ENXIO;
	}
}

static void
sdx55_voice_receive(uint32_t local, uint32_t node, uint32_t port, const uint8_t *p, size_t size)
{
	const struct sdx55_cell_endpoint *ep = local == SDX55_VOICE_PORT ? &voice_endpoint :
	    local == SDX55_IMSA_PORT ? &imsa_endpoint : &imss_endpoint;
	if (!voice_worker.armed || size < 7 || ep->node != node || ep->port != port) return;
	uint16_t message = sdx55_cell_le16(p+3), txn = sdx55_cell_le16(p+1);
	if (ep == &voice_endpoint && p[0] == QMI_INDICATION && message == SDX55_VOICE_CODEC) {
		if (!voice_worker.target[0] || sdx55_cell_le16(p+5) != size - 7) return;
		unsigned mode = 0, codec = 0, rate = 0;
		for (size_t offset = 7; offset < size;) {
			if (size - offset < 3) return;
			uint8_t tag = p[offset];
			unsigned count = sdx55_cell_le16(p+offset+1);
			offset += 3;
			if (count > size - offset) return;
			if (count == 4) {
				unsigned value = sdx55_cell_le16(p+offset) | (uint32_t)sdx55_cell_le16(p+offset+2) << 16;
				if (tag == 0x10) mode = value;
				if (tag == 0x11) codec = value;
				if (tag == 0x12) rate = value;
			}
			offset += count;
		}
		hakios_tl("UMI SDX55: voice codec mode=%u codec=%u rate=%u\n", mode, codec, rate);
		return;
	}
	if (ep == &voice_endpoint && p[0] == QMI_INDICATION && message == SDX55_VOICE_ALL_STATUS) {
		if (!voice_worker.target[0] || voice_worker.voice_generation != ep->generation) return;
		struct sdx55_voice_result report;
		if (!sdx55_voice_decode(ep->service, message, txn, p, size, voice_worker.target, &report)) {
			voice_worker.indication = report;
			voice_worker.has_indication = true;
		}
		return;
	}
	if (p[0] != QMI_RESPONSE || !voice_query.pending || voice_query.done || voice_query.service != ep->service ||
	    voice_query.node != node || voice_query.port != port || voice_query.message != message || voice_query.txn != txn) return;
	voice_query.error = sdx55_voice_decode(ep->service, message, txn, p, size, voice_worker.target, &voice_query.reply);
	voice_query.done = true;
}

static int
sdx55_voice_command(struct umi_mhi *host, const struct sdx55_cell_endpoint *ep, uint16_t message,
    unsigned argument, struct sdx55_voice_result *reply)
{
	*reply = (struct sdx55_voice_result){0};
	if (!ep->port || ep->node != umi_sdx55_dms_node || host->poisoned) return ENXIO;
	unsigned local = ep == &voice_endpoint ? SDX55_VOICE_PORT : ep == &imsa_endpoint ? SDX55_IMSA_PORT : SDX55_IMSS_PORT;
	uint32_t packet[28] = {1, 1, 1, local, 0, 0, ep->node, ep->port};
	uint16_t txn = voice_query.txn + 1;
	size_t length = sdx55_voice_request(ep->service, message, txn, argument, voice_worker.target,
	    (uint8_t *)(packet+8), sizeof(packet)-32);
	if (!length) return EINVAL;
	voice_query = (typeof(voice_query)){.txn=txn, .message=message, .service=ep->service,
	    .node=ep->node, .port=ep->port, .pending=true};
	packet[5] = (uint32_t)length;
	int error = umi_mhi_send_qrtr_packet(host, packet, 32 + ((length+3)&~3u), 1000, UINT16_MAX);
	bzero(packet, sizeof(packet));
	unsigned timeout = message == SDX55_VOICE_DIAL && ep->service == SDX55_VOICE_SERVICE ? 6000 : 2000;
	for (unsigned ms = 0; !error && !voice_query.done && ms < timeout; ms += 10) {
		error = umi_sdx55_poll_loc(host, 10);
		if (!error) error = sdx55_flush_flow_acks(host);
	}
	voice_query.pending = false;
	*reply = voice_query.reply;
	if (!error) error = voice_query.done ? voice_query.error : ETIMEDOUT;
	hakios_tl("UMI SDX55: voice QMI service=%u message=%#x errno=%d qmi=%u\n",
	    ep->service, message, error, reply->qmi_error);
	return error;
}

static void
sdx55_voice_ims(struct umi_mhi *host)
{
	struct sdx55_voice_result reply;
	uint32_t registration = UMI_VOICE_IMS_UNKNOWN, voice = UMI_VOICE_IMS_UNKNOWN, technology = UMI_VOICE_IMS_UNKNOWN;
	uint16_t reason = 0;
	bool registration_rejected = false, voice_rejected = false;
	if (imss_endpoint.port && voice_worker.imss_generation != imss_endpoint.generation) {
		uint32_t generation = imss_endpoint.generation;
		voice_worker.imss_generation = generation;
		if (!sdx55_voice_command(host, &imss_endpoint, SDX55_IMSS_BIND, voice_worker.subscription, &reply) &&
		    generation == imss_endpoint.generation &&
		    !sdx55_voice_command(host, &imss_endpoint, SDX55_IMSS_GET_ENABLED, 0, &reply))
			hakios_tl("UMI SDX55: IMS settings registration_enabled=%u volte_enabled=%u\n", reply.ims_enabled, reply.volte_enabled);
	}
	if (voice_worker.imsa_generation != imsa_endpoint.generation) voice_worker.ims_bound = false;
	if (imsa_endpoint.port && !voice_worker.ims_bound) {
		uint32_t generation = imsa_endpoint.generation;
		voice_worker.ims_bound = !sdx55_voice_command(host, &imsa_endpoint, SDX55_IMSA_BIND, voice_worker.subscription, &reply);
		voice_worker.imsa_generation = generation;
		if (voice_worker.ims_bound && generation == imsa_endpoint.generation &&
		    !sdx55_voice_command(host, &imsa_endpoint, SDX55_IMSA_GET_BIND, 0, &reply))
			hakios_tl("UMI SDX55: IMS bind subscription=%u index=%u\n", voice_worker.subscription, reply.value);
	}
	if (voice_worker.ims_bound && voice_worker.imsa_generation == imsa_endpoint.generation) {
		int error = sdx55_voice_command(host, &imsa_endpoint, SDX55_IMSA_REGISTRATION, 0, &reply);
		if (!error) { registration = reply.value; reason = reply.reason; }
		registration_rejected = error == EIO && reply.qmi_error == 70; /* QMI InvalidOperation */
	}
	if (voice_worker.ims_bound && voice_worker.imsa_generation == imsa_endpoint.generation) {
		int error = sdx55_voice_command(host, &imsa_endpoint, SDX55_IMSA_SERVICES, 0, &reply);
		if (!error) {
			voice = reply.value;
			technology = reply.technology;
		}
		voice_rejected = error == EIO && reply.qmi_error == 70;
	}
	voice_worker.ims_queries_rejected = registration_rejected && voice_rejected;
	lck_mtx_lock(&cell_status_lock);
	bool changed = voice_view.ims_registration != registration || voice_view.ims_voice != voice ||
	    voice_view.ims_technology != technology || voice_worker.ims_reason != reason;
	voice_worker.ims_reason = reason;
	voice_view.ims_registration = registration;
	voice_view.ims_voice = voice;
	voice_view.ims_technology = technology;
	lck_mtx_unlock(&cell_status_lock);
	if (changed) hakios_tl("UMI SDX55: IMS subscription=%u registration=%u voice=%u technology=%u registration_error=%u\n",
	    voice_worker.subscription, registration, voice, technology, reason);
	clock_interval_to_deadline(12, NSEC_PER_SEC, &voice_worker.ims_after);
}

static void
sdx55_voice_clear_call(void)
{
	sdx55_voice_audio_stop();
	sdx55_voice_publish_route(UMI_VOICE_ROUTE_NONE, 0);
	voice_worker.owned = voice_worker.uncertain = voice_worker.seen = voice_worker.has_indication = false;
	bzero(voice_worker.target, sizeof(voice_worker.target));
}

static bool
sdx55_voice_call_live(const struct sdx55_voice_call *call)
{
	return call->state != 9; /* END */
}

static struct sdx55_voice_call *
sdx55_voice_owned_call(struct sdx55_voice_result *report)
{
	if (voice_worker.owned) {
		struct sdx55_voice_call *call = sdx55_voice_find(report, voice_worker.id);
		return call && call->direction == 1 ? call : NULL;
	}
	/* If DIAL's response was lost, recover only an unambiguous outgoing
	 * call that reports the exact number the Phone app just submitted. */
	struct sdx55_voice_call *match = NULL;
	for (unsigned i = 0; i < report->count; i++) {
		struct sdx55_voice_call *call = &report->calls[i];
		if (call->direction != 1 || !call->matches_number) continue;
		if (match) return NULL;
		match = call;
	}
	if (match) {
		voice_worker.id = match->id;
		voice_worker.owned = true;
		voice_worker.uncertain = false;
	}
	return match;
}

static void
sdx55_voice_apply(struct sdx55_voice_result *report, bool polled)
{
	struct sdx55_voice_call *call = sdx55_voice_owned_call(report);
	if (!call) {
		if (polled && voice_worker.owned && (voice_worker.seen || mach_absolute_time() >= voice_worker.originate_until)) {
			sdx55_voice_clear_call();
			sdx55_voice_publish(UMI_VOICE_ENDED, 0, 0, NULL);
		}
		return;
	}
	voice_worker.seen = true;
	if (call->type == 2 && (call->state == 1 || call->state == 3 || call->state == 5))
		sdx55_voice_audio_start(voice_worker.subscription);
	unsigned state = UMI_VOICE_DIALING;
	switch (call->state) {
	case 3: case 6: state = UMI_VOICE_ACTIVE; break;
	case 5: state = UMI_VOICE_RINGING; break;
	case 8: state = UMI_VOICE_ENDING; break;
	case 9: state = UMI_VOICE_ENDED; sdx55_voice_clear_call(); break;
	}
	bool connected = state == UMI_VOICE_ACTIVE && voice_view.state != UMI_VOICE_ACTIVE;
	if (connected)
		sdx55_voice_audio_observe_mailbox("active");
	sdx55_voice_publish(state, 0, 0, call);
	if (connected) {
		sdx55_voice_publish_route(UMI_VOICE_ROUTE_NONE, 0);
		sdx55_voice_audio_connected();
	}
}

static void
sdx55_voice_dial(struct umi_mhi *host)
{
	struct sdx55_voice_result reply;
	int error = sdx55_voice_command(host, &voice_endpoint, SDX55_VOICE_GET_ALL, 0, &reply);
	for (unsigned i = 0; !error && i < reply.count; i++)
		if (sdx55_voice_call_live(&reply.calls[i])) error = EBUSY;
	if (!error && mach_absolute_time() >= voice_worker.ims_after) sdx55_voice_ims(host);
	unsigned type = 0;
	if (!error && voice_worker.imsa_generation == imsa_endpoint.generation && voice_view.ims_registration == 2 &&
	    voice_view.ims_voice == 2 && voice_view.ims_technology != 0 &&
	    voice_view.ims_technology != 2) type = SDX55_DIAL_AUTOMATIC;
	if (!error && !type) {
		const struct sdx55_cell_endpoint nas = sdx55_services[CELL_NAS];
		error = sdx55_cell_command(host, &nas, SDX55_NAS_GET_SERVING_SYSTEM, 0, NULL);
		if (!error && (sdx55_query.cell.registration != 1 || sdx55_query.cell.cs_attach != 1)) {
			/* Let VOICE report the result of an explicit test on a serving
			 * LTE/NR network even if the modem reports no IMS registration. */
			if ((voice_view.ims_registration == 0 || voice_worker.ims_queries_rejected) &&
			    voice_worker.imsa_generation == imsa_endpoint.generation &&
			    (sdx55_radio.rat == 4 || sdx55_radio.rat == 5)) type = SDX55_DIAL_AUTOMATIC;
			else error = ENETUNREACH;
		}
	}
	if (!error && voice_worker.voice_generation != voice_endpoint.generation) error = ENXIO;
	if (!error && type == SDX55_DIAL_AUTOMATIC) {
		error = sdx55_voice_audio_prepare(host);
		if (error) hakios_tl("UMI SDX55: voice mailbox setup errno=%d\n", error);
	}
	if (error) {
		sdx55_voice_clear_call();
		sdx55_voice_publish(UMI_VOICE_FAILED, error, reply.qmi_error, NULL);
		return;
	}
	clock_interval_to_deadline(15, NSEC_PER_SEC, &voice_worker.originate_until);
	hakios_tl("UMI SDX55: voice dial automatic=%u IMS registration=%u availability=%u\n",
	    type == SDX55_DIAL_AUTOMATIC, voice_view.ims_registration, voice_view.ims_voice);
	error = sdx55_voice_command(host, &voice_endpoint, SDX55_VOICE_DIAL, type, &reply);
	if (!error && reply.has_id) {
		voice_worker.owned = true;
		voice_worker.id = reply.id;
		if (type == SDX55_DIAL_AUTOMATIC) sdx55_voice_audio_start(voice_worker.subscription);
		return;
	}
	if (error == EIO && reply.qmi_error && !reply.has_id) {
		struct sdx55_voice_call rejected = {.type=type, .reason=reply.reason};
		sdx55_voice_clear_call();
		sdx55_voice_publish(UMI_VOICE_FAILED, error, reply.qmi_error, &rejected);
		return;
	}
	voice_worker.uncertain = true;
	sdx55_voice_publish(UMI_VOICE_UNCERTAIN, error ? error : EPROTO, reply.qmi_error, NULL);
}

static void
sdx55_voice_end(struct umi_mhi *host)
{
	if (!voice_worker.owned && !voice_worker.uncertain) return;
	struct sdx55_voice_result reply;
	int error = sdx55_voice_command(host, &voice_endpoint, SDX55_VOICE_GET_ALL, 0, &reply);
	struct sdx55_voice_call *call = error ? NULL : sdx55_voice_owned_call(&reply);
	if (!call || !sdx55_voice_call_live(call)) {
		if (!error) sdx55_voice_apply(&reply, true);
		if (voice_worker.target[0]) sdx55_voice_publish(UMI_VOICE_UNCERTAIN, error ? error : EAGAIN, reply.qmi_error, NULL);
		return;
	}
	error = sdx55_voice_command(host, &voice_endpoint, SDX55_VOICE_END, voice_worker.id, &reply);
	if (!error && reply.has_id && reply.id != voice_worker.id) error = EPROTO;
	sdx55_voice_publish(error ? UMI_VOICE_UNCERTAIN : UMI_VOICE_ENDING, error, reply.qmi_error, NULL);
}

static void
sdx55_voice_begin(void)
{
	uint32_t option = 0;
	if (!PE_parse_boot_argn("sdx55_voice", &option, sizeof(option)) || option != 1) return;
	voice_worker.armed = true;
	lck_mtx_lock(&cell_status_lock);
	voice_worker.subscription = cell_view.nas_subscription;
	voice_view.subscription = voice_worker.subscription;
	lck_mtx_unlock(&cell_status_lock);
}

static void
sdx55_voice_tick(struct umi_mhi *host)
{
	if (!voice_worker.armed || host->poisoned) return;
	sdx55_ims_dcm_tick(host, voice_worker.subscription);
	uint64_t now = mach_absolute_time();
	if (voice_worker.voice_generation != voice_endpoint.generation && (voice_worker.owned || voice_worker.uncertain)) {
		/* A modem restart invalidates all previous call IDs. */
		sdx55_voice_clear_call();
		sdx55_voice_publish(UMI_VOICE_UNAVAILABLE, ENXIO, 0, NULL);
	}
	if (!voice_accepting || voice_worker.voice_generation != voice_endpoint.generation) {
		if (now < voice_worker.poll_after || !voice_worker.subscription) return;
		clock_interval_to_deadline(5, NSEC_PER_SEC, &voice_worker.poll_after);
		lck_mtx_lock(&cell_status_lock);
		voice_accepting = false;
		bzero(&voice_request, sizeof(voice_request));
		lck_mtx_unlock(&cell_status_lock);
		struct sdx55_voice_result reply;
		uint32_t generation = voice_endpoint.generation;
		int error = sdx55_voice_command(host, &voice_endpoint, SDX55_VOICE_BIND, voice_worker.subscription, &reply);
		if (error) { sdx55_voice_publish(UMI_VOICE_UNAVAILABLE, error, reply.qmi_error, NULL); return; }
		voice_worker.voice_generation = generation;
		(void)sdx55_voice_command(host, &voice_endpoint, SDX55_VOICE_REGISTER, 0, &reply);
		sdx55_voice_ims(host);
		if (voice_endpoint.generation != generation) return;
		lck_mtx_lock(&cell_status_lock);
		voice_accepting = true;
		lck_mtx_unlock(&cell_status_lock);
		sdx55_voice_publish(UMI_VOICE_READY, 0, 0, NULL);
		return;
	}
	if (voice_worker.has_indication && voice_worker.target[0]) {
		voice_worker.has_indication = false;
		struct sdx55_voice_result indication = voice_worker.indication;
		sdx55_voice_apply(&indication, false);
	}
	lck_mtx_lock(&cell_status_lock);
	unsigned action = voice_request.action;
	if (action == UMI_VOICE_DIAL) {
		memcpy(voice_worker.target, voice_request.number, sizeof(voice_worker.target));
		voice_view.state = UMI_VOICE_DIALING;
		voice_view.error = voice_view.qmi_error = voice_view.end_reason = voice_view.call_type = voice_view.call_mode = 0;
	}
	bzero(&voice_request, sizeof(voice_request));
	lck_mtx_unlock(&cell_status_lock);
	if (action == UMI_VOICE_DIAL) { sdx55_voice_dial(host); return; }
	if (action == UMI_VOICE_END) { sdx55_voice_end(host); return; }
	if (action == UMI_VOICE_USE_RECEIVER || action == UMI_VOICE_USE_SPEAKER) {
		unsigned route = action == UMI_VOICE_USE_SPEAKER ? UMI_VOICE_ROUTE_SPEAKER : UMI_VOICE_ROUTE_RECEIVER;
		int error = voice_view.state == UMI_VOICE_ACTIVE && voice_worker.owned ?
		    sdx55_voice_audio_switch(route) : ENOENT;
		sdx55_voice_publish_route(UMI_VOICE_ROUTE_NONE, error);
		return;
	}
	if (action == UMI_VOICE_MUTE || action == UMI_VOICE_UNMUTE) {
		bool mute = action == UMI_VOICE_MUTE;
		int error = voice_view.state == UMI_VOICE_ACTIVE && voice_worker.owned ?
		    sdx55_voice_audio_mute(mute) : ENOENT;
		lck_mtx_lock(&cell_status_lock);
		voice_mute_view.pending = 0;
		voice_mute_view.error = error;
		if (!error) voice_mute_view.muted = mute;
		lck_mtx_unlock(&cell_status_lock);
		hakios_tl("UMI SDX55: voice microphone muted=%u errno=%d\n", mute, error);
		return;
	}
	if (voice_worker.target[0] && now >= voice_worker.poll_after) {
		struct sdx55_voice_result reply;
		if (!sdx55_voice_command(host, &voice_endpoint, SDX55_VOICE_GET_ALL, 0, &reply)) sdx55_voice_apply(&reply, true);
		clock_interval_to_deadline(1, NSEC_PER_SEC, &voice_worker.poll_after);
	}
	if (!voice_worker.target[0] && now >= voice_worker.ims_after) sdx55_voice_ims(host);
}

static void
sdx55_voice_finish(struct umi_mhi *host)
{
	if (!voice_worker.armed) return;
	lck_mtx_lock(&cell_status_lock);
	voice_accepting = false;
	bzero(&voice_request, sizeof(voice_request));
	lck_mtx_unlock(&cell_status_lock);
	if (!host->poisoned && voice_worker.voice_generation == voice_endpoint.generation) sdx55_voice_end(host);
	sdx55_ims_dcm_close(host);
	sdx55_voice_clear_call();
	voice_worker.armed = false;
	sdx55_voice_publish(UMI_VOICE_UNAVAILABLE, 0, 0, NULL);
}

static int
sdx55_voice_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (kauth_cred_getuid(kauth_cred_get()) != 0) {
		const char *identity = cs_identity_get(req->p);
		if (!identity || !csproc_get_platform_binary(req->p) ||
		    (strcmp(identity, "com.apple.mobilephone") &&
		    (req->newptr || strcmp(identity, "com.apple.Preferences")))) return EPERM;
	}
	lck_mtx_lock(&cell_status_lock);
	struct umi_voice_snapshot view = voice_view;
	lck_mtx_unlock(&cell_status_lock);
	int error = SYSCTL_OUT(req, &view, sizeof(view));
	if (error || !req->newptr) return error;
	struct umi_voice_request request = {0};
	if (req->newlen != sizeof(request)) return EINVAL;
	error = SYSCTL_IN(req, &request, sizeof(request));
	if (error) goto out;
	if (request.size != sizeof(request) || request.version != UMI_VOICE_VERSION ||
	    (request.action != UMI_VOICE_DIAL && request.action != UMI_VOICE_END) ||
	    (request.action == UMI_VOICE_DIAL && !umi_voice_number_length(request.number))) { error = EINVAL; goto out; }
	lck_mtx_lock(&cell_status_lock);
	unsigned state = voice_view.state;
	if (!voice_accepting) error = ENXIO;
	else if (request.action == UMI_VOICE_DIAL) {
		if (state != UMI_VOICE_READY && state != UMI_VOICE_ENDED && state != UMI_VOICE_FAILED) error = EBUSY;
		else { voice_request = request; voice_view.state = UMI_VOICE_QUEUED; }
	} else if (state == UMI_VOICE_QUEUED && voice_request.action == UMI_VOICE_DIAL) {
		bzero(&voice_request, sizeof(voice_request));
		voice_view.state = UMI_VOICE_ENDED;
	} else if (state >= UMI_VOICE_DIALING && (state <= UMI_VOICE_ENDING || state == UMI_VOICE_UNCERTAIN)) {
		voice_request.action = UMI_VOICE_END;
	} else error = ENOENT;
	lck_mtx_unlock(&cell_status_lock);
out:
	bzero(&request, sizeof(request));
	return error;
}

static int
sdx55_voice_route_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	if (kauth_cred_getuid(kauth_cred_get()) != 0) {
		const char *identity = cs_identity_get(req->p);
		if (!identity || (strcmp(identity, "com.apple.mobilephone") && strcmp(identity, "com.apple.InCallService")) ||
		    !csproc_get_platform_binary(req->p)) return EPERM;
	}
	lck_mtx_lock(&cell_status_lock);
	struct umi_voice_route_status view = voice_route_view;
	lck_mtx_unlock(&cell_status_lock);
	int error = SYSCTL_OUT(req, &view, sizeof(view));
	if (error || !req->newptr) return error;
	uint32_t route;
	if (req->newlen != sizeof(route)) return EINVAL;
	error = SYSCTL_IN(req, &route, sizeof(route));
	if (error) return error;
	if (route != UMI_VOICE_ROUTE_RECEIVER && route != UMI_VOICE_ROUTE_SPEAKER) return EINVAL;

	lck_mtx_lock(&cell_status_lock);
	if (!voice_accepting || voice_view.state != UMI_VOICE_ACTIVE || !voice_route_view.route) error = ENXIO;
	else if (voice_request.action || voice_route_view.pending) error = EBUSY;
	else if (route != voice_route_view.route) {
		voice_request.action = route == UMI_VOICE_ROUTE_SPEAKER ? UMI_VOICE_USE_SPEAKER : UMI_VOICE_USE_RECEIVER;
		voice_route_view.pending = route;
		voice_route_view.error = 0;
	}
	lck_mtx_unlock(&cell_status_lock);
	return error;
}

static int
sdx55_voice_mute_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	const char *identity = cs_identity_get(req->p);
	if (!identity || strcmp(identity, "com.apple.mobilephone") || !csproc_get_platform_binary(req->p)) return EPERM;
	lck_mtx_lock(&cell_status_lock);
	struct umi_voice_mute_status view = voice_mute_view;
	lck_mtx_unlock(&cell_status_lock);
	int error = SYSCTL_OUT(req, &view, sizeof(view));
	if (error || !req->newptr) return error;
	uint32_t value;
	if (req->newlen != sizeof(value)) return EINVAL;
	error = SYSCTL_IN(req, &value, sizeof(value));
	if (error || value > 1) return error ? error : EINVAL;
	lck_mtx_lock(&cell_status_lock);
	if (!voice_accepting || voice_view.state != UMI_VOICE_ACTIVE || !voice_route_view.route) error = ENXIO;
	else if (voice_request.action || voice_mute_view.pending) error = EBUSY;
	else if (voice_mute_view.muted != value) {
		voice_request.action = value ? UMI_VOICE_MUTE : UMI_VOICE_UNMUTE;
		voice_mute_view.pending = 1;
		voice_mute_view.error = 0;
	}
	lck_mtx_unlock(&cell_status_lock);
	return error;
}
