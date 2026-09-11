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

#endif
