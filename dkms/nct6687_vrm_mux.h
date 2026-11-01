/* eSIO mux cycle: save 0x61/0x62, select the port, SMB enable, body,
 * SMB disable, restore 0x61/0x62.
 *
 * One window for the demand session and the /dev/port reader. The mailbox
 * header does not own these registers. Caller serializes the window
 * (EC_io_lock in the kernel).
 *
 * Port select is the low two bits of 0x61. Baud for the transfer is 0x03.
 * after may be NULL. It runs only after a good body and the SMB disable,
 * before restore; non-zero bails like a failed body.
 *
 * NCT_VRM_MUX_FAIL_DISABLES — body failure writes 0x60=0 before on_fail
 * (port). Demand omits it so bus_recover still sees SMB enabled.
 * NCT_VRM_MUX_RESTORE_CHECKED — a good body's restore errors return
 * NCT_VRM_MUX_RESTORE. Without the flag both regs are written and errors
 * are ignored.
 *
 * SAVE does not call on_fail and does not restore (the read never landed).
 * ARM and BODY call on_fail, then restore with errors ignored. RESTORE is
 * a good body whose checked restore failed; on_fail is not called.
 *
 * Host tests set nct_vrm_host_reject_mux_restore. The checked restore then
 * fails once rail 0 is valid, after a good PAGE sample. Absent in the kernel.
 */
#ifndef NCT6687_VRM_MUX_H
#define NCT6687_VRM_MUX_H

#include "nct6687_vrm_mailbox.h"

#ifdef NCT_VRM_MAILBOX_HOST
int nct_vrm_host_reject_mux_restore;
#endif

enum nct_vrm_mux_phase {
	NCT_VRM_MUX_OK = 0,
	NCT_VRM_MUX_SAVE,
	NCT_VRM_MUX_ARM,
	NCT_VRM_MUX_BODY,
	NCT_VRM_MUX_RESTORE,
};

#define NCT_VRM_MUX_FAIL_DISABLES 0x01
#define NCT_VRM_MUX_RESTORE_CHECKED 0x02

static void nct_vrm_mux_put(struct nct6687_data* data, u8 cfg, u8 baud)
{
	nct_vrm_esio_write(data, 0x61, cfg);
	nct_vrm_esio_write(data, 0x62, baud);
}

/* One shot. Errors on the first reg skip the second — no retry on a wedged
 * window. The host reject stands in for that first write failing.
 */
static int nct_vrm_mux_restore(struct nct6687_data* data, u8 cfg, u8 baud)
{
#ifdef NCT_VRM_MAILBOX_HOST
	if (nct_vrm_host_reject_mux_restore && data->vrm_rail[0].valid)
		return -EIO;
#endif
	if (nct_vrm_esio_write(data, 0x61, cfg) || nct_vrm_esio_write(data, 0x62, baud))
		return -EIO;
	return 0;
}

static enum nct_vrm_mux_phase nct_vrm_mux_bail(struct nct6687_data* data,
	int (*on_fail)(struct nct6687_data* data), u8 cfg, u8 baud,
	enum nct_vrm_mux_phase phase)
{
	if (on_fail)
		on_fail(data);
	nct_vrm_mux_put(data, cfg, baud);
	return phase;
}

static void nct_vrm_mux_remember(struct nct6687_data* data, u8 cfg, u8 baud)
{
	data->vrm_mux_hold = true;
	data->vrm_mux_cfg = cfg;
	data->vrm_mux_baud = baud;
}

static enum nct_vrm_mux_phase nct_vrm_mux_cycle(struct nct6687_data* data,
	unsigned mux_sel, int (*body)(struct nct6687_data* data, void* ctx),
	int (*after)(struct nct6687_data* data, void* ctx),
	int (*on_fail)(struct nct6687_data* data), void* ctx, unsigned flags)
{
	u8 cfg = 0, baud = 0;
	int rc;

	/* A failed restore leaves port 0 / baud 0x03 in the registers. Reading
	 * those back would make the next success restore the armed values.
	 * Keep the pre-arm pair until a restore of that pair lands.
	 */
	if (data->vrm_mux_hold) {
		cfg = data->vrm_mux_cfg;
		baud = data->vrm_mux_baud;
	} else if (nct_vrm_esio_read(data, 4, 0x61, &cfg) ||
		nct_vrm_esio_read(data, 4, 0x62, &baud))
		return NCT_VRM_MUX_SAVE;

	if (nct_vrm_esio_write(data, 0x61, (cfg & ~0x03) | (mux_sel & 3)) ||
		nct_vrm_esio_write(data, 0x62, 0x03) ||
		nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
		return nct_vrm_mux_bail(data, on_fail, cfg, baud, NCT_VRM_MUX_ARM);

	rc = body(data, ctx);
	if (rc) {
		if (flags & NCT_VRM_MUX_FAIL_DISABLES)
			nct_vrm_esio_write(data, 0x60, 0x00);
		return nct_vrm_mux_bail(data, on_fail, cfg, baud, NCT_VRM_MUX_BODY);
	}

	nct_vrm_esio_write(data, 0x60, 0x00);
	if (after && after(data, ctx))
		return nct_vrm_mux_bail(data, on_fail, cfg, baud, NCT_VRM_MUX_BODY);
	if (flags & NCT_VRM_MUX_RESTORE_CHECKED) {
		if (nct_vrm_mux_restore(data, cfg, baud)) {
			nct_vrm_mux_remember(data, cfg, baud);
			return NCT_VRM_MUX_RESTORE;
		}
	} else {
		nct_vrm_mux_put(data, cfg, baud);
	}
	data->vrm_mux_hold = false;
	return NCT_VRM_MUX_OK;
}

#endif
