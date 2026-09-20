/* PMBus raw → millisi. Integer arithmetic only.
 *
 * Kernel C and userspace Python (nct6687_vrm_decode.py) must return the same
 * values as tests/golden_vrm_decode.json. Host compile: tests/vrm_decode.c
 * with -DNCT_VRM_DECODE_HOST (no linux headers).
 */
#ifndef NCT6687_VRM_DECODE_H
#define NCT6687_VRM_DECODE_H

#ifdef NCT_VRM_DECODE_HOST
#include <stdint.h>
#include <stdbool.h>
typedef uint8_t u8;
typedef uint16_t u16;
#endif

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

/* Coarse LINEAR16 N=-3 when P/V is unusable: (raw * 1000) >> 3 milliamps. */
static long nct_vrm_decode_iout_fallback_ma(u16 iout)
{
	return ((long)iout * 1000L) >> 3;
}

/* IOUT mA from already-decoded POUT mW and VOUT mV. Toward-zero like C `/`.
 * Caller owns the 200 mV / READ_IOUT policy. v_mv == 0 → 0.
 */
static long nct_vrm_iout_from_pv_ma(long p_mw, long v_mv)
{
	if (!v_mv)
		return 0;
	return (p_mw * 1000L) / v_mv;
}

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
 * IOUT (mA) and POUT (uW) use reject_neg: LINEAR11 can decode a signed spike
 * (live: -13900 mA / -17000000 uW) and hist would latch it until reload.
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
