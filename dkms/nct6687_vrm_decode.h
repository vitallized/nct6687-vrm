/* PMBus raw → millisi. Integer arithmetic only.
 *
 * Kernel C and userspace Python (nct6687_vrm_decode.py) must return the same
 * values as tests/golden_vrm_decode.json. Host compile: tests/vrm_decode.c
 * with -DNCT_VRM_DECODE_HOST (no linux headers).
 *
 * Also owns the rail slot policy (nct_vrm_rail_apply): store one PAGE sample
 * and fold it into the software hist.
 */
#ifndef NCT6687_VRM_DECODE_H
#define NCT6687_VRM_DECODE_H

#ifdef NCT_VRM_DECODE_HOST
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
/* Host stand-in for stock struct nct6687_data: eSIO base + the real VRM
 * data header, so host tests see the same rail slots as the kernel.
 */
struct nct6687_data {
	unsigned int addr;
#include "nct6687_vrm_data.h"
};
#endif

/* Rail slot index. Order is the PAGE sample order of nct_vrm_rail.val[]. */
enum nct_vrm_metric {
	NCT_VRM_VOUT, /* mV */
	NCT_VRM_VIN,  /* mV */
	NCT_VRM_IOUT, /* mA */
	NCT_VRM_POUT, /* uW */
	NCT_VRM_TEMP, /* m°C */
	NCT_VRM_IIN,  /* mA */
	NCT_VRM_PIN,  /* uW */
};
_Static_assert(NCT_VRM_PIN + 1 == NCT_VRM_NMETRIC, "NCT_VRM_NMETRIC");

/* Not a val[] slot. POUT/PIN ratio, shown through pwm9 (100% = 200). */
#define NCT_VRM_EFF 255

static int nct_vrm_clamp_exp(int exp)
{
	if (exp < -16)
		return -16;
	if (exp > 15)
		return 15;
	return exp;
}

static long nct_vrm_linear11_milli(u16 raw)
{
	int exp = (raw >> 11) & 0x1f;
	int mant = raw & 0x7ff;
	long abs_m;
	bool neg;

	if (exp >= 16)
		exp -= 32;
	if (mant >= 1024)
		mant -= 2048;

	neg = mant < 0;
	abs_m = (neg ? -(long)mant : (long)mant) * 1000L;
	if (exp >= 0) {
		exp = nct_vrm_clamp_exp(exp);
		if (exp > 0)
			abs_m <<= exp;
	} else {
		abs_m >>= nct_vrm_clamp_exp(-exp);
	}
	return neg ? -abs_m : abs_m;
}

static long nct_vrm_decode_vout_mv(u16 vout, u8 vout_mode, int fallback_exp)
{
	int mode = (vout_mode >> 5) & 0x7;
	int exp;

	if (mode == 2)
		return (long)vout; /* Direct R=3 → mV = raw */

	if (mode == 0) {
		exp = vout_mode & 0x1f;
		if (exp >= 16)
			exp -= 32;
	} else {
		exp = fallback_exp;
	}
	exp = nct_vrm_clamp_exp(exp);
	if (exp >= 0)
		return ((long)vout * 1000L) << exp;
	return ((long)vout * 1000L) >> (-exp);
}

/* Direct VIN, 10 mV/LSB (m=1, b=0, R=2). */
static long nct_vrm_decode_vin_mv(u16 vin)
{
	return (long)vin * 10L;
}

/* READ_IOUT Direct, 0.1 A/LSB → milliamps. */
static long nct_vrm_decode_iout_ma(u16 iout)
{
	return (long)iout * 100L;
}

/* READ_IIN Direct, 0.01 A/LSB → milliamps. */
static long nct_vrm_decode_iin_ma(u16 iin)
{
	return (long)iin * 10L;
}

/* READ_PIN Direct, 1 W/LSB → microwatts. */
static long nct_vrm_decode_pin_uw(u16 pin)
{
	return (long)pin * 1000000L;
}

/*
 * HWiNFO 8.54 "VRM Efficiency": POUT/PIN*100, then min(100).
 * Milli-percent. Either power <= 0 → 0.
 * pout >= pin is the cap, and it also keeps pout*100000 inside signed long.
 */
static long nct_vrm_efficiency_pcm(long pout_uw, long pin_uw)
{
	if (pout_uw <= 0 || pin_uw <= 0)
		return 0;
	if (pout_uw >= pin_uw)
		return 100000L;
	return (pout_uw * 100000L) / pin_uw;
}

/*
 * pwm count for sensors 3.6.2. get_type_scaling() returns int, so the
 * 2.55 pwm factor is truncated to 2 and sensors prints count/2 as "%".
 * 100% is 200. 255 would print as 128%.
 */
static long nct_vrm_efficiency_pwm(long pout_uw, long pin_uw)
{
	long pcm = nct_vrm_efficiency_pcm(pout_uw, pin_uw);
	long pct;

	if (pcm <= 0)
		return 0;
	pct = (pcm + 500L) / 1000L;
	if (pct <= 0)
		return 0;
	if (pct > 100L)
		pct = 100L;
	return pct * 2L;
}

#ifdef NCT_VRM_DECODE_HOST
/* Not on the PAGE sample path. Toward-zero like C `/`. v_mv == 0 → 0. */
static long nct_vrm_iout_from_pv_ma(long p_mw, long v_mv)
{
	if (!v_mv)
		return 0;
	return (p_mw * 1000L) / v_mv;
}
#endif

/* Software min/max. first=true seeds both ends. */
static void nct_vrm_hist_point(long* min, long* max, long val, bool first)
{
	if (first) {
		*min = *max = val;
		return;
	}
	if (val < *min)
		*min = val;
	if (val > *max)
		*max = val;
}

/*
 * IOUT, IIN (mA) and POUT, PIN (uW) use reject_neg. Live 2026-09-20 latched
 * -13900 mA because IOUT was P/V of a LINEAR11 POUT spike (-17000000 uW).
 * READ_IOUT Direct does not produce that spike; the guard stays.
 * VOUT/VIN/TEMP keep reject_neg=false.
 */
static void nct_vrm_hist_add(long* min, long* max, bool* init, long val,
	bool reject_neg)
{
	if (reject_neg && val < 0)
		return;
	nct_vrm_hist_point(min, max, val, !*init);
	*init = true;
}

/*
 * Store one good PAGE sample in its rail slot and update the software hist.
 * VOUT/VIN/TEMP share hist_init and always take the signed value; IOUT/POUT
 * and IIN/PIN each seed on their own first non-negative sample.
 */
static void nct_vrm_rail_apply(struct nct_vrm_rail* r,
	const long sample[NCT_VRM_NMETRIC])
{
	bool first = !r->hist_init;

	memcpy(r->val, sample, sizeof(r->val));
	r->valid = true;
	nct_vrm_hist_point(&r->min[NCT_VRM_VOUT], &r->max[NCT_VRM_VOUT],
		sample[NCT_VRM_VOUT], first);
	nct_vrm_hist_point(&r->min[NCT_VRM_VIN], &r->max[NCT_VRM_VIN],
		sample[NCT_VRM_VIN], first);
	nct_vrm_hist_add(&r->min[NCT_VRM_IOUT], &r->max[NCT_VRM_IOUT],
		&r->iout_hist_init, sample[NCT_VRM_IOUT], true);
	nct_vrm_hist_add(&r->min[NCT_VRM_POUT], &r->max[NCT_VRM_POUT],
		&r->pout_hist_init, sample[NCT_VRM_POUT], true);
	nct_vrm_hist_point(&r->min[NCT_VRM_TEMP], &r->max[NCT_VRM_TEMP],
		sample[NCT_VRM_TEMP], first);
	nct_vrm_hist_add(&r->min[NCT_VRM_IIN], &r->max[NCT_VRM_IIN],
		&r->iin_hist_init, sample[NCT_VRM_IIN], true);
	nct_vrm_hist_add(&r->min[NCT_VRM_PIN], &r->max[NCT_VRM_PIN],
		&r->pin_hist_init, sample[NCT_VRM_PIN], true);
	r->hist_init = true;
}

/* min/max for this metric has at least one sample. */
static bool nct_vrm_rail_hist_ready(const struct nct_vrm_rail* r,
	enum nct_vrm_metric m)
{
	if (m == NCT_VRM_IOUT)
		return r->iout_hist_init;
	if (m == NCT_VRM_POUT)
		return r->pout_hist_init;
	if (m == NCT_VRM_IIN)
		return r->iin_hist_init;
	if (m == NCT_VRM_PIN)
		return r->pin_hist_init;
	return r->hist_init;
}

/*
 * Retry interval after consecutive PAGE-sample failures. First fail stays
 * ~HZ/4; then HZ, 2*HZ, cap 8*HZ. fails==0 (never failed / just reset) uses
 * the first-fail slot so a cold !valid cache still retries at HZ/4.
 */
static unsigned long nct_vrm_fail_interval(unsigned fails, unsigned long hz)
{
	if (fails <= 1)
		return hz / 4;
	if (fails == 2)
		return hz;
	if (fails == 3)
		return 2 * hz;
	return 8 * hz;
}

/*
 * PAGE sample due? valid / demanded / last_read / gap / cache age in jiffies.
 *
 * Live (2026-09-20): 1.2 s idle then vrm_cpu in 0.11 ms (stale). First demand
 * had gap>=HZ (aged last_read, or last_read==0 stored as HZ) so interval=HZ
 * and a recent fan update_vrm won the rate-limit. Demand after idle / first
 * read uses the 20 ms floor, not 1 Hz. Background stays 1 Hz; invalid uses
 * nct_vrm_fail_interval even on demand.
 */
static bool nct_vrm_should_sample(bool valid, bool demanded, bool last_read_set,
	unsigned long gap, unsigned long last_updated_age, unsigned long hz,
	unsigned long floor, unsigned fails)
{
	unsigned long interval;

	if (!valid)
		interval = nct_vrm_fail_interval(fails, hz);
	else if (!demanded)
		interval = hz;
	else if (!last_read_set || gap >= hz)
		interval = floor;
	else if (gap > floor)
		interval = gap;
	else
		interval = floor;

	return last_updated_age > interval;
}

#endif
