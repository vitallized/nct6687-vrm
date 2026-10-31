/* eSIO mailbox: idle window, proto/START/status/payload. Then the PAGE
 * sample, the PMBus sequence built on those mailbox ops.
 *
 * Kernel C and the userspace reader (nct6687_vrm.py) keep the same named
 * ops. Host compile: tests/vrm_mailbox.c with -DNCT_VRM_MAILBOX_HOST
 * (fake inb/outb/delay inside that TU — no linux headers, no /dev/port).
 * PAGE sample traffic must match tests/golden_vrm_sample.json.
 *
 * Caller holds EC_io_lock. PAGE idle is 0xFF. Ports 2/3 hang the EC;
 * this header never touches the port mux (0x61) or baud (0x62).
 */
#ifndef NCT6687_VRM_MAILBOX_H
#define NCT6687_VRM_MAILBOX_H

#ifdef NCT_VRM_MAILBOX_HOST
#ifndef NCT_VRM_DECODE_HOST
#define NCT_VRM_DECODE_HOST 1
#endif
#endif
#include "nct6687_vrm_decode.h"

#define NCT_VRM_SMB_EN 0x80
#define NCT_VRM_SMB_START 0x40
#define NCT_VRM_SMB_CLEAR 0x08
#define NCT_VRM_PROTO_WBR 0x02
#define NCT_VRM_PROTO_RB 0x82
#define NCT_VRM_PROTO_RW 0x83

#ifdef NCT_VRM_MAILBOX_HOST
#include <errno.h>
#ifndef EC_SPACE_PAGE_REGISTER_OFFSET
#define EC_SPACE_PAGE_REGISTER_OFFSET 4
#define EC_SPACE_INDEX_REGISTER_OFFSET 5
#define EC_SPACE_DATA_REGISTER_OFFSET 6
#endif
void nct_vrm_host_outb(u16 port, u8 val);
u8 nct_vrm_host_inb(u16 port);
void nct_vrm_host_udelay(unsigned us);
void nct_vrm_host_usleep_range(unsigned min_us, unsigned max_us);

static inline void nct_vrm_outb(u16 port, u8 val)
{
	nct_vrm_host_outb(port, val);
}
static inline u8 nct_vrm_inb(u16 port)
{
	return nct_vrm_host_inb(port);
}
static inline void nct_vrm_udelay(unsigned us)
{
	nct_vrm_host_udelay(us);
}
static inline void nct_vrm_usleep_range(unsigned min_us, unsigned max_us)
{
	nct_vrm_host_usleep_range(min_us, max_us);
}
#else
static inline void nct_vrm_outb(u16 port, u8 val)
{
	outb_p(val, port);
}
static inline u8 nct_vrm_inb(u16 port)
{
	return inb_p(port);
}
static inline void nct_vrm_udelay(unsigned us)
{
	udelay(us);
}
static inline void nct_vrm_usleep_range(unsigned min_us, unsigned max_us)
{
	usleep_range(min_us, max_us);
}
#endif

/*
 * Caller must hold data->EC_io_lock.
 * Stock nct6687_read/write leave PAGE != 0xff. Under EC_io_lock nothing else
 * can be mid-eSIO — force idle select; fail if PAGE never settles.
 */
static int nct_vrm_idle(struct nct6687_data* data)
{
	int i;

	if (nct_vrm_inb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET) == 0xff)
		return 0;
	nct_vrm_outb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET, 0xff);
	for (i = 0; i < 10; i++) {
		if (nct_vrm_inb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET) == 0xff)
			return 0;
		nct_vrm_udelay(100);
	}
	return -EBUSY;
}

static int nct_vrm_esio_write(struct nct6687_data* data, u8 index, u8 value)
{
	if (nct_vrm_idle(data))
		return -EBUSY;
	nct_vrm_outb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET, 0x04);
	nct_vrm_outb(data->addr + EC_SPACE_INDEX_REGISTER_OFFSET, index);
	nct_vrm_outb(data->addr + EC_SPACE_DATA_REGISTER_OFFSET, value);
	nct_vrm_outb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET, 0xff);
	return 0;
}

static int nct_vrm_esio_read(struct nct6687_data* data, u8 page, u8 index, u8* out)
{
	if (nct_vrm_idle(data))
		return -EBUSY;
	nct_vrm_outb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET, page);
	nct_vrm_outb(data->addr + EC_SPACE_INDEX_REGISTER_OFFSET, index);
	*out = nct_vrm_inb(data->addr + EC_SPACE_DATA_REGISTER_OFFSET);
	nct_vrm_outb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET, 0xff);
	return 0;
}

static int nct_vrm_prep_clear(struct nct6687_data* data)
{
	u8 ctrl;
	int rc;

	if ((rc = nct_vrm_esio_write(data, 0x03, 0xff)))
		return rc;
	if ((rc = nct_vrm_esio_write(data, 0x04, 0xff)))
		return rc;
	if ((rc = nct_vrm_esio_read(data, 4, 0x60, &ctrl)))
		return rc;
	if ((rc = nct_vrm_esio_write(data, 0x60, (ctrl | NCT_VRM_SMB_CLEAR) & ~NCT_VRM_SMB_START)))
		return rc;
	return nct_vrm_esio_write(data, 0x60, ctrl & ~(NCT_VRM_SMB_START | NCT_VRM_SMB_CLEAR));
}

static int nct_vrm_wait_start_clear(struct nct6687_data* data)
{
	int i;
	u8 ctrl;

	for (i = 0; i < 100; i++) {
		if (nct_vrm_esio_read(data, 4, 0x60, &ctrl))
			return -EIO;
		if (!(ctrl & NCT_VRM_SMB_START))
			return 0;
		nct_vrm_usleep_range(500, 1000);
	}
	return -ETIMEDOUT;
}

/* 0 or -EBUSY (idle). Callers must not assume success. */
static int nct_vrm_recover(struct nct6687_data* data)
{
	int rc = nct_vrm_prep_clear(data);

	if (rc)
		return rc;
	return nct_vrm_esio_write(data, 0x60, 0x00);
}

static int nct_vrm_write_byte(struct nct6687_data* data, u8 addr, u8 cmd, u8 value)
{
	u8 sts;

	if (nct_vrm_prep_clear(data) || nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_WBR)
		|| nct_vrm_esio_write(data, 0x65, addr) || nct_vrm_esio_write(data, 0x66, cmd)
		|| nct_vrm_esio_write(data, 0x70, value) || nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
		return -EIO;
	nct_vrm_usleep_range(500, 1000);
	if (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))
		return -EIO;
	if (nct_vrm_wait_start_clear(data))
		return -ETIMEDOUT;
	if (nct_vrm_esio_read(data, 4, 0x03, &sts))
		return -EIO;
	return sts ? -EIO : 0;
}

static int nct_vrm_read_byte(struct nct6687_data* data, u8 addr, u8 cmd, u8* out)
{
	u8 sts, lo;

	if (nct_vrm_prep_clear(data) || nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_RB)
		|| nct_vrm_esio_write(data, 0x65, addr) || nct_vrm_esio_write(data, 0x66, cmd)
		|| nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
		return -EIO;
	nct_vrm_usleep_range(500, 1000);
	if (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))
		return -EIO;
	if (nct_vrm_wait_start_clear(data))
		return -ETIMEDOUT;
	if (nct_vrm_esio_read(data, 4, 0x03, &sts) || sts)
		return -EIO;
	if (nct_vrm_esio_read(data, 4, 0xb0, &lo))
		return -EIO;
	*out = lo;
	return 0;
}

static int nct_vrm_read_word(struct nct6687_data* data, u8 addr, u8 cmd, u16* out)
{
	u8 lo, hi, sts;

	if (nct_vrm_prep_clear(data) || nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_RW)
		|| nct_vrm_esio_write(data, 0x65, addr) || nct_vrm_esio_write(data, 0x66, cmd)
		|| nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
		return -EIO;
	nct_vrm_usleep_range(500, 1000);
	if (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))
		return -EIO;
	if (nct_vrm_wait_start_clear(data))
		return -ETIMEDOUT;
	if (nct_vrm_esio_read(data, 4, 0x03, &sts) || sts)
		return -EIO;
	if (nct_vrm_esio_read(data, 4, 0xb0, &lo) || nct_vrm_esio_read(data, 4, 0xb1, &hi))
		return -EIO;
	*out = lo | (hi << 8);
	return 0;
}

static void nct_vrm_invalidate_smbus(struct nct6687_data* data)
{
	data->vrm_smbus_page = -1;
	data->vrm_vout_mode_valid[0] = false;
	data->vrm_vout_mode_valid[1] = false;
}

/*
 * Production PAGE sample — source of truth for userspace _sample_page:
 *   PAGE write+readback (cache miss), VOUT_MODE 0x20 (cached),
 *   then TEMP 0x8D, VOUT 0x8B, VIN 0x88, IOUT 0x8C, IIN 0x89, POUT 0x96, PIN 0x97.
 *   Same relative order as HWiNFO 8.54. TEMP2/TEMP3 are not on this path.
 *   IOUT is always READ_IOUT (Direct, 0.1 A/LSB), not POUT/VOUT.
 *   CAP 0x19 / STATUS 0x78 are not on this path (debug-only in userspace).
 * Any failure invalidates the PAGE/VOUT_MODE cache. out[] is indexed by
 * enum nct_vrm_metric and is only written on success.
 */
static int nct_vrm_sample_page(struct nct6687_data* data, u8 addr, u8 page,
	int vout_exp, long out[NCT_VRM_NMETRIC])
{
	u16 vout, iout = 0, iin = 0, pout, pin = 0, vin, temp;
	u8 vout_mode, page_r;
	long v_mv, p_mw, t_mc, i_ma, iin_ma, pin_uw;

	if (page > 1)
		return -EINVAL;

	if (data->vrm_smbus_page != page) {
		if (nct_vrm_write_byte(data, addr, 0x00, page) || nct_vrm_read_byte(data, addr, 0x00, &page_r)) {
			nct_vrm_invalidate_smbus(data);
			return -EIO;
		}
		if (page_r != page) {
			nct_vrm_invalidate_smbus(data);
			return -EIO;
		}
		data->vrm_smbus_page = page;
	}

	if (!data->vrm_vout_mode_valid[page]) {
		if (nct_vrm_read_byte(data, addr, 0x20, &vout_mode)) {
			nct_vrm_invalidate_smbus(data);
			return -EIO;
		}
		data->vrm_vout_mode_cache[page] = vout_mode;
		data->vrm_vout_mode_valid[page] = true;
	} else {
		vout_mode = data->vrm_vout_mode_cache[page];
	}

	/* HWiNFO 8.54 order for these commands. Each word is its own command. */
	if (nct_vrm_read_word(data, addr, 0x8d, &temp) || nct_vrm_read_word(data, addr, 0x8b, &vout) || nct_vrm_read_word(data, addr, 0x88, &vin) || nct_vrm_read_word(data, addr, 0x8c, &iout) || nct_vrm_read_word(data, addr, 0x89, &iin) || nct_vrm_read_word(data, addr, 0x96, &pout) || nct_vrm_read_word(data, addr, 0x97, &pin)) {
		nct_vrm_invalidate_smbus(data);
		return -EIO;
	}

	v_mv = nct_vrm_decode_vout_mv(vout, vout_mode, vout_exp);
	p_mw = nct_vrm_linear11_milli(pout);
	t_mc = nct_vrm_linear11_milli(temp);
	i_ma = nct_vrm_decode_iout_ma(iout);
	iin_ma = nct_vrm_decode_iin_ma(iin);
	pin_uw = nct_vrm_decode_pin_uw(pin);

	out[NCT_VRM_VOUT] = v_mv;
	out[NCT_VRM_VIN] = nct_vrm_decode_vin_mv(vin);
	out[NCT_VRM_IOUT] = i_ma;
	out[NCT_VRM_POUT] = p_mw * 1000L;
	out[NCT_VRM_TEMP] = t_mc;
	out[NCT_VRM_IIN] = iin_ma;
	out[NCT_VRM_PIN] = pin_uw;
	return 0;
}

#endif
