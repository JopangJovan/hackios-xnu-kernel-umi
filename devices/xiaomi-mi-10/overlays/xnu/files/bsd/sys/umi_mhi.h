/* UMI QCA6390 MHI boot transport. The PCIe owner supplies MMIO and coherent DMA.
 * The caller must serialize calls and keep the endpoint powered throughout each call.
 */
#ifndef _SYS_UMI_MHI_H_
#define _SYS_UMI_MHI_H_
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct umi_mhi_dma {
	void *bytes;
	uint64_t address;                 /* SMMU IOVA, never a CPU physical address */
	size_t size;
};

struct umi_mhi_io {
	void *context;
	uint32_t bar_size;
	/* Zero selects the QCA6390 defaults.  External modems use a different
	 * protected IOVA window and a larger event-context array. */
	uint64_t dma_start, dma_end;
	uint8_t profile;
	uint32_t (*read32)(void *, uint32_t);
	void (*write32)(void *, uint32_t, uint32_t);
	/* Page-aligned coherent allocations, owned until endpoint shutdown/reset.
	 * Failure returns an errno; partial allocations remain owned by the platform.
	 */
	int (*allocate)(void *, size_t, struct umi_mhi_dma *);
	void (*sync_for_device)(void *, const struct umi_mhi_dma *);
	void (*sync_for_cpu)(void *, const struct umi_mhi_dma *);
	void (*delay_us)(void *, unsigned);
	void (*report)(void *, const char *, uint32_t);
	void (*qrtr_packet)(void *, const void *, size_t);
	/* Complete IP_HW0 transfer, valid only during the callback. Do not reenter MHI. */
	void (*cell_rx)(void *, const void *, size_t);
	void (*diag_rx)(void *, const void *, size_t);
};

#define UMI_MHI_PROFILE_QCA6390 0u
#define UMI_MHI_PROFILE_SDX55  1u
#define SDX55_EVENT_CONTEXTS 17u

struct umi_mhi {
	const struct umi_mhi_io *io;
	uint32_t bhi, bhie, channel_db, event_db, sequence;
	uint64_t event_consumer[SDX55_EVENT_CONTEXTS], event_wp[SDX55_EVENT_CONTEXTS];
	uint64_t command_wp, ipcr_rx_wp, ipcr_tx_wp;
	unsigned ipcr_tx_slot;
	uint16_t ipcr_wait_transaction;
	bool poisoned, sbl_sent, contexts_ready, amss_sent, ipcr_started;
	bool sahara_complete, mission_event_seen;
	bool sahara_started, sahara_received;
	bool sahara_tx_done;
	uint64_t sahara_tx_wp, sahara_tx_pending, sahara_rx_wp;
	uint32_t sahara_rx_words[12], sahara_rx_bytes;
	uint32_t sahara_queued[4][12], sahara_queued_bytes[4];
	unsigned sahara_queue_head, sahara_queue_count;
	bool ipcr_tx_pending, ipcr_tx_done, ipcr_waiting_response, ipcr_response_done;
	/* Retain these even on timeout: the endpoint may still be using them. */
	struct umi_mhi_dma sbl, image, vectors;
	struct umi_mhi_dma channels, events, commands, command_ring;
	struct umi_mhi_dma event_ring[SDX55_EVENT_CONTEXTS];
	struct umi_mhi_dma sahara_tx_ring, sahara_rx_ring, sahara_rx_buffers, sahara_tx_buffer;
	struct umi_mhi_dma efs_tx_ring, efs_rx_ring, efs_rx_buffer, efs_tx_buffers;
	uint8_t *efs_shadow;
	uint32_t efs_base, efs_offset;
	unsigned efs_state, efs_rx_slot, efs_tx_slot, efs_tx_head, efs_tx_count;
	bool cell_started, cell_tx_db_armed, cell_rx_drop;
	unsigned cell_rx_slot, cell_rx_count, cell_tx_slot;
	uint32_t cell_tx_bytes;
	size_t cell_rx_partial;
	struct umi_mhi_dma cell_tx_ring, cell_rx_ring, cell_tx_buffer, cell_rx_buffer, cell_rx_assembly;
	struct umi_mhi_dma ipcr_tx_ring, ipcr_tx_buffers, ipcr_rx_ring, ipcr_rx_buffers;
	struct umi_mhi_dma diag_tx_ring, diag_rx_ring, diag_tx_buffer, diag_rx_buffers;
	uint64_t diag_rx_rp, diag_rx_wp;
	bool diag_started, diag_tx_pending;
};

/* A zero-initialized host, an enumerated endpoint, and a mapped BAR are required.
 * Probe reads registers only. All functions return zero or a positive errno.
 */
int umi_mhi_probe(struct umi_mhi *, const struct umi_mhi_io *);
int umi_mhi_execution_environment(const struct umi_mhi *, uint32_t *);
/* BHI sends the first 512 KiB while in PBL. Before BHIE, the PCIe owner must
 * service READY, install MHI contexts/event rings and request M0.
 * BHIE sends the complete firmware image, including the first segment.
 */
int umi_mhi_download_sbl(struct umi_mhi *, const void *, size_t);
/* Install the three event rings used by UMI's stock MHI configuration and
 * enter M0. Call only once, after SBL has put MHI into READY.
 */
int umi_mhi_prepare_ready(struct umi_mhi *);
/* Start the SBL-only SAHARA 2/3 pair, post four bounded inbound buffers, and
 * report the first request without transmitting a protocol response. */
int umi_mhi_probe_sahara(struct umi_mhi *, unsigned timeout_ms);
int umi_mhi_sahara_handshake(struct umi_mhi *, unsigned timeout_ms);
int umi_mhi_sahara_next_image(struct umi_mhi *, unsigned timeout_ms);
int umi_mhi_sahara_serve_image(struct umi_mhi *, uint32_t image_id,
    const void *image, size_t size, unsigned timeout_ms);
int umi_mhi_download_amss(struct umi_mhi *, const void *, size_t);
int umi_mhi_wait_mission(struct umi_mhi *);
/* Start the stock UMI QRTR/IPCR channels (20/21) and post bounded receive
 * buffers. Polling is for bring-up only; production needs MSI event handling.
 */
/* CPU-only snapshot for the single EFS sync in the bounded diagnostic. */
#define UMI_EFS_SHADOW_BYTES (2u * 1024 * 1024)
int umi_mhi_probe_efs(struct umi_mhi *host, uint8_t *shadow);
int umi_mhi_start_ipcr(struct umi_mhi *);
int umi_mhi_sdx55_diag_start(struct umi_mhi *);
int umi_mhi_sdx55_diag_send(struct umi_mhi *, const void *, size_t);
/* Only the SDX55 ADSP satellite owns these offloaded contexts. */
int umi_mhi_sdx55_sat_context(struct umi_mhi *, unsigned id, bool event, uint64_t base, uint32_t bytes);
int umi_mhi_sdx55_sat_channel(struct umi_mhi *, unsigned channel, bool start);
/* Probe non-offload IP_HW0 in AMSS. RX stays owned until the owner resets PCIe. */
int umi_mhi_probe_cell_data(struct umi_mhi *);
#define UMI_MHI_CELL_MRU 32768u
/* Serialized PCIe worker only. Submit one complete negotiated MAP transfer.
 * Returns after the matching EOT. Poll IPCR while idle to service DL. */
int umi_mhi_send_cell_data(struct umi_mhi *, const void *frame, size_t length, unsigned timeout_ms);
int umi_mhi_poll_ipcr(struct umi_mhi *, unsigned timeout_ms, unsigned *received);
int umi_mhi_send_qrtr_hello(struct umi_mhi *);
int umi_mhi_send_qrtr_packet(struct umi_mhi *, const void *, size_t,
	unsigned timeout_ms, uint16_t expected_transaction);

#endif
