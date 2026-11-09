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
typedef uint64_t u64;
#endif

/* vrm_fast bits (module parameter; default 0, the traffic every release
 * before it used). Only a session started with nct_vrm_win_session(true)
 * uses them: the /dev/port helper and plain host tests never do.
 *   NOPAUSE: inb/outb without the port-0x80 pause write of inb_p/outb_p.
 * Tried and dropped (2026-11-08, MS-7D89): keeping page 4 selected across
 * a transaction's ops (INDEX + DATA only). Every word read back 0; this EC
 * needs 0xFF then the page before each access, as the stock driver does.
 */
#define NCT_VRM_FAST_NOPAUSE 0x01
/* Mailbox shortcuts (2026-11-08 trial). Each only after the session's own
 * first transaction, from state the session itself wrote:
 *   KEEP_CTRL: prep_clear uses the 0x60 value the session last wrote or read
 *     instead of reading it back.
 *   KEEP_SETUP: proto (0x63) and address (0x65) are not rewritten when they
 *     equal what the session last wrote.
 *   NO_CLEAR: after a transaction whose status was 0, prep_clear writes the
 *     status bytes only, without the 0x60 CLEAR pulse.
 * A failed sample, or a VOUT word of 0, turns all three off until reload.
 */
#define NCT_VRM_FAST_KEEP_CTRL 0x02
#define NCT_VRM_FAST_KEEP_SETUP 0x04
#define NCT_VRM_FAST_NO_CLEAR 0x08
#define NCT_VRM_FAST_SHORTCUTS (NCT_VRM_FAST_KEEP_CTRL | NCT_VRM_FAST_KEEP_SETUP | NCT_VRM_FAST_NO_CLEAR)

/* Window state, one writer (the caller holds EC_io_lock). */
static struct {
	/* A session is on: trust idle_known, apply fast. */
	bool session;
	unsigned fast;
	/* This session last left PAGE at 0xFF itself, so the idle check is
	 * known to pass. Stock reads leave PAGE elsewhere, but they take
	 * EC_io_lock, which the session holds throughout.
	 */
	bool idle_known;
	/* Last 0x60 value written or read (START dropped: the master clears it). */
	bool ctrl_known;
	u8 ctrl;
	/* Last proto and address written; bit 0 proto known, bit 1 address. */
	u8 setup_known;
	u8 proto;
	u8 addr;
	/* The last transaction finished with status 0. */
	bool clean;
	/* Shortcuts failed once: off until the module reloads. */
	bool shortcuts_dead;
} nct_vrm_win;

static inline bool nct_vrm_short(unsigned bit)
{
	return nct_vrm_win.session && (nct_vrm_win.fast & bit);
}

#ifdef NCT_VRM_MAILBOX_HOST

static inline void nct_vrm_outb(u16 port, u8 val)
{
	nct_vrm_host_outb(port, val);
}
static inline u8 nct_vrm_inb(u16 port)
{
	return nct_vrm_host_inb(port);
}
static inline void nct_vrm_usleep_range(unsigned min_us, unsigned max_us)
{
	nct_vrm_host_usleep_range(min_us, max_us);
}
#else
static inline void nct_vrm_outb(u16 port, u8 val)
{
	if (nct_vrm_win.session && (nct_vrm_win.fast & NCT_VRM_FAST_NOPAUSE))
		outb(val, port);
	else
		outb_p(val, port);
}
static inline u8 nct_vrm_inb(u16 port)
{
	if (nct_vrm_win.session && (nct_vrm_win.fast & NCT_VRM_FAST_NOPAUSE))
		return inb(port);
	return inb_p(port);
}
static inline void nct_vrm_usleep_range(unsigned min_us, unsigned max_us)
{
	usleep_range(min_us, max_us);
}
#endif

/* Busy time of the refresh: port I/O, the START poll's register checks,
 * udelay, and decode. usleep_range is a sleep and is not entered.
 * One writer, the VRM session. vrm_busy_ns is the cumulative total;
 * vrm_busy_samples is how many refreshes published.
 */
#ifndef NCT_VRM_MAILBOX_HOST
static u64 nct_vrm_busy_ns;
static u64 nct_vrm_busy_t0;
#endif
static u64 nct_vrm_busy_samples;
static unsigned nct_vrm_busy_depth;

static inline void nct_vrm_busy_enter(void)
{
	if (nct_vrm_busy_depth++ == 0) {
#ifndef NCT_VRM_MAILBOX_HOST
		nct_vrm_busy_t0 = ktime_get_ns();
#endif
	}
}

static inline void nct_vrm_busy_leave(void)
{
	if (nct_vrm_busy_depth == 0)
		return;
	if (--nct_vrm_busy_depth == 0) {
#ifndef NCT_VRM_MAILBOX_HOST
		u64 now = ktime_get_ns();

		if (now > nct_vrm_busy_t0)
			WRITE_ONCE(nct_vrm_busy_ns,
				READ_ONCE(nct_vrm_busy_ns) + (now - nct_vrm_busy_t0));
#endif
	}
}

static inline void nct_vrm_busy_note_sample(void)
{
#ifdef NCT_VRM_MAILBOX_HOST
	nct_vrm_busy_samples++;
#else
	WRITE_ONCE(nct_vrm_busy_samples, READ_ONCE(nct_vrm_busy_samples) + 1);
#endif
}

static inline void nct_vrm_udelay(unsigned us)
{
	nct_vrm_busy_enter();
#ifdef NCT_VRM_MAILBOX_HOST
	nct_vrm_host_udelay(us);
#else
	udelay(us);
#endif
	nct_vrm_busy_leave();
}

/*
 * Caller must hold data->EC_io_lock.
 * Stock nct6687_read/write leave PAGE != 0xff. Under EC_io_lock nothing else
 * can be mid-eSIO — force idle select; fail if PAGE never settles.
 */
static int nct_vrm_idle(struct nct6687_data* data)
{
	int i;

	if (nct_vrm_win.session && nct_vrm_win.idle_known)
		return 0;
	if (nct_vrm_inb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET) == 0xff)
		goto idle;
	nct_vrm_outb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET, 0xff);
	for (i = 0; i < 10; i++) {
		if (nct_vrm_inb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET) == 0xff)
			goto idle;
		nct_vrm_udelay(100);
	}
	return -EBUSY;
idle:
	nct_vrm_win.idle_known = true;
	return 0;
}

/* Start or end a session (the demand session, under EC_io_lock). Off
 * forgets what the window looked like: the lock is about to drop.
 */
static inline void nct_vrm_win_session(bool on, unsigned fast)
{
	if (nct_vrm_win.shortcuts_dead)
		fast &= ~NCT_VRM_FAST_SHORTCUTS;
	nct_vrm_win.session = on;
	nct_vrm_win.fast = on ? fast : 0;
	nct_vrm_win.idle_known = false;
	nct_vrm_win.ctrl_known = false;
	nct_vrm_win.setup_known = 0;
	nct_vrm_win.clean = false;
}

/* Idle check (skipped when this session left the window idle), then select. */
static int nct_vrm_win_open(struct nct6687_data* data, u8 page)
{
	if (nct_vrm_idle(data))
		return -EBUSY;
	nct_vrm_outb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET, page);
	return 0;
}

/* Back to the idle page. */
static void nct_vrm_win_close(struct nct6687_data* data)
{
	nct_vrm_outb(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET, 0xff);
	nct_vrm_win.idle_known = true;
}

static int nct_vrm_esio_write(struct nct6687_data* data, u8 index, u8 value)
{
	int rc = 0;

	nct_vrm_busy_enter();
	if (nct_vrm_win_open(data, 0x04))
		rc = -EBUSY;
	else {
		nct_vrm_outb(data->addr + EC_SPACE_INDEX_REGISTER_OFFSET, index);
		nct_vrm_outb(data->addr + EC_SPACE_DATA_REGISTER_OFFSET, value);
		nct_vrm_win_close(data);
		if (index == 0x60) {
			nct_vrm_win.ctrl = value & ~NCT_VRM_SMB_START;
			nct_vrm_win.ctrl_known = true;
		}
	}
	if (rc) {
		nct_vrm_win.ctrl_known = false;
		nct_vrm_win.setup_known = 0;
	}
	nct_vrm_busy_leave();
	return rc;
}

/* proto (0x63) or address (0x65): skipped under KEEP_SETUP when unchanged. */
static int nct_vrm_esio_setup(struct nct6687_data* data, u8 index, u8 value)
{
	u8* last = index == 0x63 ? &nct_vrm_win.proto : &nct_vrm_win.addr;
	u8 bit = index == 0x63 ? 1 : 2;
	int rc;

	if (nct_vrm_short(NCT_VRM_FAST_KEEP_SETUP) && (nct_vrm_win.setup_known & bit) && *last == value)
		return 0;
	rc = nct_vrm_esio_write(data, index, value);
	if (!rc) {
		*last = value;
		nct_vrm_win.setup_known |= bit;
	}
	return rc;
}

static int nct_vrm_esio_read(struct nct6687_data* data, u8 page, u8 index, u8* out)
{
	int rc = 0;

	nct_vrm_busy_enter();
	if (nct_vrm_win_open(data, page))
		rc = -EBUSY;
	else {
		nct_vrm_outb(data->addr + EC_SPACE_INDEX_REGISTER_OFFSET, index);
		*out = nct_vrm_inb(data->addr + EC_SPACE_DATA_REGISTER_OFFSET);
		nct_vrm_win_close(data);
		if (page == 4 && index == 0x60) {
			nct_vrm_win.ctrl = *out & ~NCT_VRM_SMB_START;
			nct_vrm_win.ctrl_known = true;
		}
	}
	nct_vrm_busy_leave();
	return rc;
}

static int nct_vrm_prep_clear(struct nct6687_data* data)
{
	u8 ctrl;
	int rc;

	if ((rc = nct_vrm_esio_write(data, 0x03, 0xff)))
		return rc;
	if ((rc = nct_vrm_esio_write(data, 0x04, 0xff)))
		return rc;
	if (nct_vrm_short(NCT_VRM_FAST_NO_CLEAR) && nct_vrm_win.clean)
		return 0;
	if (nct_vrm_short(NCT_VRM_FAST_KEEP_CTRL) && nct_vrm_win.ctrl_known)
		ctrl = nct_vrm_win.ctrl;
	else if ((rc = nct_vrm_esio_read(data, 4, 0x60, &ctrl)))
		return rc;
	if ((rc = nct_vrm_esio_write(data, 0x60, (ctrl | NCT_VRM_SMB_CLEAR) & ~NCT_VRM_SMB_START)))
		return rc;
	return nct_vrm_esio_write(data, 0x60, ctrl & ~(NCT_VRM_SMB_START | NCT_VRM_SMB_CLEAR));
}

/* EC turnaround is 5–13 ms per word (trace 2026-11-02). A check is several
 * inb_p/outb_p, ~30–40 µs, so polling from t=0 is the CPU cost: 41.4 ms/sample
 * at 20 µs, 16.7 ms at 400 µs, against 11.8 ms when the poll was ~1 ms.
 * The sysfs read does not wait. Sleep ~4 ms before the first check, then
 * every ~1 ms. This kernel sleeps to the top of usleep_range. Once a word
 * has cleared, the next first-sleep is that minimum minus 1 ms. Stuck START
 * still gives up at 100 ms. Overshoot is at most one poll per word.
 */
#define NCT_VRM_START_FIRST_US 4000
#define NCT_VRM_START_GUARD_US 1000
#define NCT_VRM_START_MIN_SAMPLE_US 2000
#define NCT_VRM_START_BUDGET_US 100000
#define NCT_VRM_POLL_MIN_US 800
#define NCT_VRM_POLL_MAX_US 1000
#define NCT_VRM_START_TRIES 110

/* 0 until a word clears. Then the fastest START-clear time, in µs. */
static unsigned nct_vrm_start_min_us;

static u64 nct_vrm_wait_now_ns(void)
{
#ifdef NCT_VRM_MAILBOX_HOST
	return 0;
#else
	return ktime_get_ns();
#endif
}

/* Gap after EN, before START. The old 500–1000 µs sleep ran with the bus
 * idle (~8 ms across one PAGE sample). outb_p already posts the write.
 */
#define NCT_VRM_ARM_UDELAY 20

static int nct_vrm_wait_start_clear(struct nct6687_data* data)
{
	unsigned i, initial, elapsed_us;
	u8 ctrl;
	u64 t0, now;

	initial = NCT_VRM_START_FIRST_US;
	if (nct_vrm_start_min_us > NCT_VRM_START_GUARD_US)
		initial = nct_vrm_start_min_us - NCT_VRM_START_GUARD_US;
	if (initial < NCT_VRM_START_GUARD_US)
		initial = NCT_VRM_START_GUARD_US;
	if (initial > 8000)
		initial = 8000;

	t0 = nct_vrm_wait_now_ns();
	nct_vrm_usleep_range(initial - 200, initial);
	for (i = 0; i < NCT_VRM_START_TRIES; i++) {
		if (nct_vrm_esio_read(data, 4, 0x60, &ctrl))
			return -EIO;
		now = nct_vrm_wait_now_ns();
		elapsed_us = (t0 && now > t0) ? (unsigned)((now - t0) / 1000ULL) : 0;
		if (!(ctrl & NCT_VRM_SMB_START)) {
			if (elapsed_us >= NCT_VRM_START_MIN_SAMPLE_US &&
				(!nct_vrm_start_min_us || elapsed_us < nct_vrm_start_min_us))
				nct_vrm_start_min_us = elapsed_us;
			return 0;
		}
		if (elapsed_us >= NCT_VRM_START_BUDGET_US)
			return -ETIMEDOUT;
		nct_vrm_usleep_range(NCT_VRM_POLL_MIN_US, NCT_VRM_POLL_MAX_US);
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

/* Clear, program proto/addr/cmd (and the payload byte when has_value), EN,
 * then START. 0 or -EIO.
 */
static int nct_vrm_arm_start(struct nct6687_data* data, u8 proto, u8 addr, u8 cmd,
	bool has_value, u8 value)
{
	int rc;

	/* prep_clear reads clean (NO_CLEAR); this transaction is not clean yet. */
	rc = nct_vrm_prep_clear(data);
	nct_vrm_win.clean = false;
	rc = rc || nct_vrm_esio_setup(data, 0x63, proto)
		|| nct_vrm_esio_setup(data, 0x65, addr) || nct_vrm_esio_write(data, 0x66, cmd)
		|| (has_value && nct_vrm_esio_write(data, 0x70, value))
		|| nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN);
	if (!rc) {
		nct_vrm_udelay(NCT_VRM_ARM_UDELAY);
		rc = nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START);
	}
	return rc ? -EIO : 0;
}

/* After START cleared: status, then n payload bytes from 0xB0 on (n <= 2).
 * 0 or -EIO (a nonzero status included).
 */
static int nct_vrm_fetch(struct nct6687_data* data, u8* payload, unsigned n)
{
	u8 sts;
	unsigned i;
	int rc;

	rc = nct_vrm_esio_read(data, 4, 0x03, &sts) || sts;
	for (i = 0; !rc && i < n; i++)
		rc = nct_vrm_esio_read(data, 4, 0xb0 + i, &payload[i]);
	nct_vrm_win.clean = !rc;
	return rc ? -EIO : 0;
}

static int nct_vrm_write_byte(struct nct6687_data* data, u8 addr, u8 cmd, u8 value)
{
	if (nct_vrm_arm_start(data, NCT_VRM_PROTO_WBR, addr, cmd, true, value))
		return -EIO;
	if (nct_vrm_wait_start_clear(data))
		return -ETIMEDOUT;
	return nct_vrm_fetch(data, NULL, 0);
}

static int nct_vrm_read_byte(struct nct6687_data* data, u8 addr, u8 cmd, u8* out)
{
	u8 b[1];

	if (nct_vrm_arm_start(data, NCT_VRM_PROTO_RB, addr, cmd, false, 0))
		return -EIO;
	if (nct_vrm_wait_start_clear(data))
		return -ETIMEDOUT;
	if (nct_vrm_fetch(data, b, 1))
		return -EIO;
	*out = b[0];
	return 0;
}

static int nct_vrm_read_word(struct nct6687_data* data, u8 addr, u8 cmd, u16* out)
{
	u8 b[2];

	if (nct_vrm_arm_start(data, NCT_VRM_PROTO_RW, addr, cmd, false, 0))
		return -EIO;
	if (nct_vrm_wait_start_clear(data))
		return -ETIMEDOUT;
	if (nct_vrm_fetch(data, b, 2))
		return -EIO;
	*out = b[0] | (b[1] << 8);
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
 *   Order is TEMP, VOUT, VIN, IOUT, IIN, POUT, PIN. TEMP2 (0x8E, controller die) is one
 *   read in the demand session, not here. TEMP3 (0x8F) is not sampled.
 *   IOUT is always READ_IOUT (Direct, 0.1 A/LSB), not POUT/VOUT.
 *   CAP 0x19 / STATUS 0x78 are not on this path (debug-only in userspace).
 * Any failure invalidates the PAGE/VOUT_MODE cache. out[] is indexed by
 * enum nct_vrm_metric and is only written on success. raw may be NULL; when
 * set, it receives those same words.
 */
/* With shortcuts on, a VOUT word of 0 means they broke the read (a running
 * rail is never 0 V): fail the sample so the session turns them off.
 */
static bool nct_vrm_short_suspect(u16 vout)
{
	return vout == 0 && nct_vrm_short(NCT_VRM_FAST_SHORTCUTS);
}

static int nct_vrm_sample_page_words(struct nct6687_data* data, u8 addr, u8 page,
	int vout_exp, long out[NCT_VRM_NMETRIC], u16* raw, bool lite);

static int nct_vrm_sample_page(struct nct6687_data* data, u8 addr, u8 page,
	int vout_exp, long out[NCT_VRM_NMETRIC], u16* raw)
{
	return nct_vrm_sample_page_words(data, addr, page, vout_exp, out, raw, false);
}

/* lite: VOUT, IOUT, POUT only (what vrm_cpu shows), in that order. out[]
 * and raw[] keep their other slots as the caller left them.
 */
static int nct_vrm_sample_page_words(struct nct6687_data* data, u8 addr, u8 page,
	int vout_exp, long out[NCT_VRM_NMETRIC], u16* raw, bool lite)
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

	if (lite) {
		if (nct_vrm_read_word(data, addr, 0x8b, &vout) || nct_vrm_read_word(data, addr, 0x8c, &iout)
			|| nct_vrm_read_word(data, addr, 0x96, &pout) || nct_vrm_short_suspect(vout)) {
			nct_vrm_invalidate_smbus(data);
			return -EIO;
		}
		nct_vrm_busy_enter();
		out[NCT_VRM_VOUT] = nct_vrm_decode_vout_mv(vout, vout_mode, vout_exp);
		out[NCT_VRM_IOUT] = nct_vrm_decode_iout_ma(iout);
		out[NCT_VRM_POUT] = nct_vrm_linear11_milli(pout) * 1000L;
		if (raw) {
			raw[NCT_VRM_VOUT] = vout;
			raw[NCT_VRM_IOUT] = iout;
			raw[NCT_VRM_POUT] = pout;
		}
		nct_vrm_busy_leave();
		return 0;
	}

	/* TEMP, VOUT, VIN, IOUT, IIN, POUT, PIN. Each word is its own command. */
	if (nct_vrm_read_word(data, addr, 0x8d, &temp) || nct_vrm_read_word(data, addr, 0x8b, &vout) || nct_vrm_read_word(data, addr, 0x88, &vin) || nct_vrm_read_word(data, addr, 0x8c, &iout) || nct_vrm_read_word(data, addr, 0x89, &iin) || nct_vrm_read_word(data, addr, 0x96, &pout) || nct_vrm_read_word(data, addr, 0x97, &pin) || nct_vrm_short_suspect(vout)) {
		nct_vrm_invalidate_smbus(data);
		return -EIO;
	}

	nct_vrm_busy_enter();
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
	if (raw) {
		raw[NCT_VRM_VOUT] = vout;
		raw[NCT_VRM_VIN] = vin;
		raw[NCT_VRM_IOUT] = iout;
		raw[NCT_VRM_POUT] = pout;
		raw[NCT_VRM_TEMP] = temp;
		raw[NCT_VRM_IIN] = iin;
		raw[NCT_VRM_PIN] = pin;
	}
	nct_vrm_busy_leave();
	return 0;
}

#endif
