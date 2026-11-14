/* The MHI worker owns updates. Userland can take a coherent read at any time. */
static LCK_GRP_DECLARE(cell_status_group, "umi-cell-status");
static LCK_MTX_DECLARE(cell_status_lock, &cell_status_group);
static struct umi_sdx55_cell_snapshot cell_view = {
	.size = sizeof(cell_view), .version = UMI_SDX55_CELL_VERSION
};

static void
sdx55_status_store(struct umi_sdx55_cell_snapshot next)
{
	bool changed = memcmp(&cell_view, &next, sizeof(next)) != 0;
	cell_view = next;
	lck_mtx_unlock(&cell_status_lock);
	if (changed) hakios_tl("UMI SDX55: cell snapshot gen=%u active=%u valid=%#x present=%#x ready=%#x NAS=%u PS=%u WDS=%u\n",
	    next.generation, next.active, next.valid, next.present_mask, next.ready_mask,
	    next.registration, next.ps_attach, next.wds_status);
}

static void
sdx55_status_session(bool active)
{
	lck_mtx_lock(&cell_status_lock);
	uint32_t generation = cell_view.generation + active;
	if (active && !generation) generation = 1;
	sdx55_status_store((struct umi_sdx55_cell_snapshot){
	    .size = sizeof(cell_view), .version = UMI_SDX55_CELL_VERSION,
	    .active = active, .generation = generation});
}

static void
sdx55_status_clear(struct umi_sdx55_cell_snapshot *v, unsigned mask)
{
	v->valid &= ~mask;
	if (mask & UMI_CELL_UIM) v->slots = v->present_mask = v->ready_mask = v->error_mask = 0;
	if (mask & UMI_CELL_NAS) v->registration = v->ps_attach = v->radio_mask = v->data_mask = 0;
	if (mask & UMI_CELL_NR) v->nr_service = v->nr_domain = 0;
	if (mask & UMI_CELL_WDS) v->wds_status = 0;
}

static void
sdx55_status_lost(unsigned service)
{
	lck_mtx_lock(&cell_status_lock);
	struct umi_sdx55_cell_snapshot v = cell_view;
	if (service == 11) sdx55_status_clear(&v, UMI_CELL_UIM);
	if (service == 3) {
		sdx55_status_clear(&v, UMI_CELL_NAS | UMI_CELL_NR);
		v.nas_subscription = 0;
	}
	if (service == 1) {
		sdx55_status_clear(&v, UMI_CELL_WDS);
		v.wds_subscription = 0;
	}
	sdx55_status_store(v);
}

static void
sdx55_status_result(unsigned service, uint16_t message, uint32_t arg, int error,
    const struct sdx55_cell_status *c, const struct sdx55_wan_status *w)
{
	lck_mtx_lock(&cell_status_lock);
	struct umi_sdx55_cell_snapshot v = cell_view;
	if (!v.active) {
		lck_mtx_unlock(&cell_status_lock);
		return;
	}
	if (service == 11 && message == SDX55_UIM_GET_CARD_STATUS) {
		sdx55_status_clear(&v, UMI_CELL_UIM);
		if (!error) {
			v.valid |= UMI_CELL_UIM;
			v.slots = c->cards;
			for (unsigned i = 0; i < 2 && i < c->cards; i++) {
				if (c->slots[i].card_state == 1) {
					v.present_mask |= 1u << i;
					if (c->slots[i].ready_gw) v.ready_mask |= 1u << i;
				}
				if (c->slots[i].card_state == 2) v.error_mask |= 1u << i;
			}
		}
	} else if (service == 3 && message == SDX55_NAS_GET_SERVING_SYSTEM) {
		sdx55_status_clear(&v, UMI_CELL_NAS);
		if (!error) {
			v.valid |= UMI_CELL_NAS;
			v.registration = c->registration;
			v.ps_attach = c->ps_attach;
			v.radio_mask = c->radio_mask;
			v.data_mask = c->data_mask;
		}
	} else if (service == 3 && message == SDX55_NAS_GET_SYSTEM_INFO) {
		sdx55_status_clear(&v, UMI_CELL_NR);
		if (!error && c->nr_service != UINT8_MAX) {
			v.valid |= UMI_CELL_NR;
			v.nr_service = c->nr_service;
			v.nr_domain = c->nr_domain;
		}
	} else if (service == 1 && message == SDX55_WDS_GET_STATUS) {
		sdx55_status_clear(&v, UMI_CELL_WDS);
		if (!error) {
			v.valid |= UMI_CELL_WDS;
			v.wds_status = w->connection_status;
		}
	} else if (service == 3 && message == SDX55_NAS_BIND_SUBSCRIPTION) {
		sdx55_status_clear(&v, UMI_CELL_NAS | UMI_CELL_NR);
		v.nas_subscription = error ? 0 : arg;
	} else if (service == 1 && message == SDX55_WDS_BIND_SUBSCRIPTION) {
		sdx55_status_clear(&v, UMI_CELL_WDS);
		v.wds_subscription = error ? 0 : arg;
	} else if (service == 1 && (message == SDX55_WDS_START || message == SDX55_WDS_STOP)) {
		sdx55_status_clear(&v, UMI_CELL_WDS);
	}
	sdx55_status_store(v);
}
