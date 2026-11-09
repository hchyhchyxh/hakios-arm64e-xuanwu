/*
 * dada: the Skywalk side of iOS 26's Bluetooth UART, com.apple.uart.sk.bluetooth.
 *
 * bluetoothd's serial driver ("POSIX", transport H4) connects the kernel control of that name (hakios_dada_wifi.c
 * bt_kctl_*), sets the line through socket options, reads a 16-byte nexus UUID from option 0x40105412 and opens a
 * Skywalk channel on it (os_channel_create): the H4 stream then goes through that channel's rings. This is that nexus:
 * a kernel pipe (like bsd/skywalk/nexus/kpipe/nx_kpipe_loopback.c) whose TX ring empties into the chip's UART and whose
 * RX ring fills from what the chip said (the UART pump's ring, hakios_bt_take).
 */
#include <skywalk/os_skywalk_private.h>
#include <skywalk/nexus/kpipe/nx_kernel_pipe.h>

extern void hakios_bt_put(const uint8_t *p, uint32_t n);     /* to the chip (hakios_dada_wifi.c) */
extern uint32_t hakios_bt_take(uint8_t *p, uint32_t most);   /* from the chip, what is waiting */

void hakios_btsk_start(void);
bool hakios_btsk_uuid(uint8_t uuid[16]);
void hakios_btsk_kick(void);

static uuid_t btsk_dom_uuid, btsk_prov_uuid, btsk_nx_uuid;
static nexus_controller_t btsk_ncd;
static kern_pbufpool_t btsk_rx_pp;
static kern_channel_ring_t btsk_txring, btsk_rxring;
static uint64_t btsk_bufsz;
static volatile int btsk_up;
static uint32_t btsk_said;

#define SAY(...) do { if (btsk_said++ < 40) printf("btsk: " __VA_ARGS__); } while (0)

static errno_t
btsk_dom_init(kern_nexus_domain_provider_t domprov __unused)
{
	return 0;
}

static void
btsk_dom_fini(kern_nexus_domain_provider_t domprov __unused)
{
}

static errno_t
btsk_pre_connect(kern_nexus_provider_t nxprov __unused, proc_t p __unused, kern_nexus_t nexus,
    nexus_port_t port, kern_channel_t channel __unused, void **ch_ctx)
{
	kern_pbufpool_t tx_pp = NULL;
	errno_t error = kern_nexus_get_pbufpool(nexus, &tx_pp, &btsk_rx_pp);

	*ch_ctx = NULL;
	SAY("bluetoothd opens a channel (port %u): pools %d\n", port, error);
	return error;
}

static errno_t
btsk_connected(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_t channel __unused)
{
	btsk_up = 1;
	SAY("the channel is connected: the H4 stream runs through it\n");
	return 0;
}

static void
btsk_pre_disconnect(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_t channel __unused)
{
	btsk_up = 0;
}

static void
btsk_disconnected(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_t channel __unused)
{
	btsk_up = 0;
	btsk_rx_pp = NULL;
	SAY("the channel is closed\n");
}

static errno_t
btsk_ring_init(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_t channel __unused,
    kern_channel_ring_t ring, boolean_t is_tx_ring, void **ring_ctx)
{
	if (is_tx_ring) {
		btsk_txring = ring;
	} else {
		btsk_rxring = ring;
	}
	*ring_ctx = NULL;
	return 0;
}

static void
btsk_ring_fini(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring)
{
	if (ring == btsk_txring) {
		btsk_txring = NULL;
	} else if (ring == btsk_rxring) {
		btsk_rxring = NULL;
	}
}

static errno_t
btsk_slot_init(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring __unused,
    channel_slot_t slot __unused, uint32_t slot_id __unused, struct kern_slot_prop **slot_prop_addr, void **pslot_ctx)
{
	*slot_prop_addr = NULL;
	*pslot_ctx = NULL;
	return 0;
}

static void
btsk_slot_fini(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring __unused,
    channel_slot_t slot __unused, uint32_t slot_id __unused)
{
}

/* bluetoothd sent: each TX slot's bytes to the chip, the slots handed back */
static errno_t
btsk_sync_tx(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring,
    uint32_t flags __unused)
{
	kern_channel_slot_t ts, last = NULL;
	uint32_t slots = 0;

	for (ts = kern_channel_get_next_slot(ring, NULL, NULL); ts != NULL; ts = kern_channel_get_next_slot(ring, ts, NULL)) {
		kern_packet_t ph = kern_channel_slot_get_packet(ring, ts);
		kern_buflet_t buf = ph != 0 ? kern_packet_get_next_buflet(ph, NULL) : NULL;

		if (buf != NULL) {
			uint8_t *base = kern_buflet_get_data_address(buf);
			uint32_t len = kern_buflet_get_data_length(buf), off = kern_buflet_get_data_offset(buf);

			if (base != NULL && len != 0) {
				hakios_bt_put(base + off, len);
			}
		}
		last = ts;
		slots++;
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	return 0;
}

/* bluetoothd reads: what the chip said, in fresh packets on the RX slots */
static errno_t
btsk_sync_rx(kern_nexus_provider_t nxprov __unused, kern_nexus_t nexus __unused, kern_channel_ring_t ring,
    uint32_t flags __unused)
{
	kern_channel_slot_t rs, last = NULL;

	(void)kern_channel_reclaim(ring);
	if (btsk_rx_pp == NULL) {
		return 0;
	}
	for (rs = kern_channel_get_next_slot(ring, NULL, NULL); rs != NULL; rs = kern_channel_get_next_slot(ring, rs, NULL)) {
		kern_packet_t ph = 0;
		kern_buflet_t buf;
		uint8_t *base;
		uint32_t n, room;

		if (kern_pbufpool_alloc_nosleep(btsk_rx_pp, 1, &ph) != 0 || ph == 0) {
			break;
		}
		buf = kern_packet_get_next_buflet(ph, NULL);
		base = buf != NULL ? kern_buflet_get_data_address(buf) : NULL;
		room = (uint32_t)MIN(btsk_bufsz, 1024ull);
		n = base != NULL ? hakios_bt_take(base, room) : 0;
		if (n == 0) {
			kern_pbufpool_free(btsk_rx_pp, ph);
			break;
		}
		(void)kern_buflet_set_data_offset(buf, 0);
		(void)kern_buflet_set_data_length(buf, (uint16_t)n);
		(void)kern_packet_finalize(ph);
		if (kern_channel_slot_attach_packet(ring, rs, ph) != 0) {
			kern_pbufpool_free(btsk_rx_pp, ph);
			break;
		}
		last = rs;
	}
	if (last != NULL) {
		kern_channel_advance_slot(ring, last);
	}
	return 0;
}

/* the chip said something: wake bluetoothd's RX side (it syncs, btsk_sync_rx fills) */
void
hakios_btsk_kick(void)
{
	kern_channel_ring_t ring = btsk_rxring;

	if (btsk_up && ring != NULL) {
		kern_channel_notify(ring, 0);
	}
}

bool
hakios_btsk_uuid(uint8_t uuid[16])
{
	if (uuid_is_null(btsk_nx_uuid)) {
		return false;
	}
	memcpy(uuid, btsk_nx_uuid, 16);
	return true;
}

void
hakios_btsk_start(void)
{
	nexus_attr_t nxa = NULL;
	errno_t error;
	uuid_string_t text;

	struct kern_nexus_domain_provider_init dom_init = {
		.nxdpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxdpi_flags = 0,
		.nxdpi_init = btsk_dom_init,
		.nxdpi_fini = btsk_dom_fini,
	};
	struct kern_nexus_provider_init prov_init = {
		.nxpi_version = KERN_NEXUS_DOMAIN_PROVIDER_CURRENT_VERSION,
		.nxpi_flags = NXPIF_VIRTUAL_DEVICE,
		.nxpi_pre_connect = btsk_pre_connect,
		.nxpi_connected = btsk_connected,
		.nxpi_pre_disconnect = btsk_pre_disconnect,
		.nxpi_disconnected = btsk_disconnected,
		.nxpi_ring_init = btsk_ring_init,
		.nxpi_ring_fini = btsk_ring_fini,
		.nxpi_slot_init = btsk_slot_init,
		.nxpi_slot_fini = btsk_slot_fini,
		.nxpi_sync_tx = btsk_sync_tx,
		.nxpi_sync_rx = btsk_sync_rx,
		.nxpi_tx_doorbell = NULL,
	};
	struct kern_nexus_init nx_init = {
		.nxi_version = KERN_NEXUS_CURRENT_VERSION,
		.nxi_flags = 0,
		.nxi_tx_pbufpool = NULL,
		.nxi_rx_pbufpool = NULL,
	};

	if (!uuid_is_null(btsk_nx_uuid)) {
		return;
	}
	error = kern_nexus_register_domain_provider(NEXUS_TYPE_KERNEL_PIPE, (const uint8_t *)"com.apple.uart.sk.bluetooth",
	    &dom_init, sizeof(dom_init), &btsk_dom_uuid);
	if (error == 0) {
		error = kern_nexus_controller_create(&btsk_ncd);
	}
	if (error == 0) {
		error = kern_nexus_attr_create(&nxa);
	}
	if (error == 0) {
		error = kern_nexus_attr_set(nxa, NEXUS_ATTR_ANONYMOUS, 1);
	}
	if (error == 0) {
		error = kern_nexus_controller_register_provider(btsk_ncd, btsk_dom_uuid,
		    (const uint8_t *)"com.apple.nexus.uart.sk.bluetooth", &prov_init, sizeof(prov_init), nxa, &btsk_prov_uuid);
	}
	if (error == 0) {
		(void)kern_nexus_controller_read_provider_attr(btsk_ncd, btsk_prov_uuid, nxa);
		(void)kern_nexus_attr_get(nxa, NEXUS_ATTR_SLOT_BUF_SIZE, &btsk_bufsz);
		error = kern_nexus_controller_alloc_provider_instance(btsk_ncd, btsk_prov_uuid, NULL, NULL, &btsk_nx_uuid,
		    &nx_init);
	}
	if (nxa != NULL) {
		kern_nexus_attr_destroy(nxa);
	}
	uuid_unparse_upper(btsk_nx_uuid, text);
	printf("btsk: the Bluetooth UART's nexus: %s (error %d), buffers of %llu bytes\n", error == 0 ? text : "NONE", error,
	    btsk_bufsz);
}
