/* PMBus raw → millisi. Integer arithmetic only.
 *
 * Kernel C and userspace Python (nct6687_vrm_decode.py) must return the same
 * values as tests/golden_vrm_decode.json. Host compile: tests/vrm_decode.c
 * with -DNCT_VRM_DECODE_HOST (no linux headers).
 *
 * Software hist lives in nct6687_vrm_hist.h.
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

/* Family datasheet: two's complement. 0xFFF6 is -10 counts, not 6552.6 A. */
static long nct_vrm_i16(u16 raw)
{
	return (raw & 0x8000) ? (long)raw - 65536L : (long)raw;
}

/* READ_IOUT Direct, 0.1 A/LSB → milliamps. */
static long nct_vrm_decode_iout_ma(u16 iout)
{
	return nct_vrm_i16(iout) * 100L;
}

/* READ_IIN Direct, 0.01 A/LSB → milliamps. */
static long nct_vrm_decode_iin_ma(u16 iin)
{
	return nct_vrm_i16(iin) * 10L;
}

/* READ_PIN Direct, 1 W/LSB, two's complement. 0xFFFF is -1 W, not 65535 W. */
static long nct_vrm_decode_pin_uw(u16 pin)
{
	return nct_vrm_i16(pin) * 1000000L;
}

/*
 * Efficiency: POUT/PIN*100, then min(100).
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

/* About 10 s, so a 1 Hz poll still averages many samples.
 * A gap longer than IDLE means sampling stopped.
 * REJECT is times 100 (250 = 2.5x). 10000 disables it for real VR spikes.
 */
#define NCT_VRM_EFF_TAU_MS 10000UL
#define NCT_VRM_EFF_IDLE_MS 5000UL
#define NCT_VRM_EFF_REJECT_X100 10000UL

static void nct_vrm_eff_reset(bool* init)
{
	*init = false;
}

static long nct_vrm_eff_blend(long prev, long sample, unsigned long dt_ms,
	unsigned long tau_ms)
{
	if (dt_ms == 0)
		return prev;
	return prev + (long)(((long long)(sample - prev) * (long long)dt_ms) /
		(long long)(tau_ms + dt_ms));
}

/* 0 when the two powers differ by more than x100/100, or either is not positive. */
static int nct_vrm_eff_agree_x100(long pout_uw, long pin_uw, unsigned long x100)
{
	if (pout_uw <= 0 || pin_uw <= 0 || x100 == 0)
		return 0;
	if ((long long)pout_uw * 100 > (long long)pin_uw * (long long)x100)
		return 0;
	if ((long long)pin_uw * 100 > (long long)pout_uw * (long long)x100)
		return 0;
	return 1;
}

static int nct_vrm_eff_agree(long pout_uw, long pin_uw)
{
	return nct_vrm_eff_agree_x100(pout_uw, pin_uw, NCT_VRM_EFF_REJECT_X100);
}

/* Seed on the first positive pair and after a long gap. Otherwise blend.
 * A negative power is not a sample: the average stays, unless the gap
 * since the last accepted pair is already past IDLE, in which case it
 * clears. A sample past NCT_VRM_EFF_REJECT_X100 is ignored and does not
 * refresh the idle timer. The default constant is wide enough that real
 * spikes pass. Either power <= 0 still fails nct_vrm_eff_agree.
 */
static void nct_vrm_eff_update(bool* init, long* pout, long* pin,
	unsigned long* at_ms, long sample_pout, long sample_pin,
	unsigned long now_ms)
{
	unsigned long dt;

	if (sample_pout < 0 || sample_pin < 0) {
		if (*init && now_ms - *at_ms > NCT_VRM_EFF_IDLE_MS)
			nct_vrm_eff_reset(init);
		return;
	}
	if (!*init || now_ms - *at_ms > NCT_VRM_EFF_IDLE_MS) {
		*pout = sample_pout;
		*pin = sample_pin;
		*at_ms = now_ms;
		*init = true;
		return;
	}
	if (!nct_vrm_eff_agree(sample_pout, sample_pin))
		return;
	dt = now_ms - *at_ms;
	*pout = nct_vrm_eff_blend(*pout, sample_pout, dt, NCT_VRM_EFF_TAU_MS);
	*pin = nct_vrm_eff_blend(*pin, sample_pin, dt, NCT_VRM_EFF_TAU_MS);
	*at_ms = now_ms;
}

/* 1 and *pwm set. 0 when there is no reading. POUT above PIN stays at the 100% cap. */
static int nct_vrm_eff_publish(bool init, long pout_uw, long pin_uw, long* pwm)
{
	long v;

	if (!init)
		return 0;
	v = nct_vrm_efficiency_pwm(pout_uw, pin_uw);
	if (v <= 0)
		return 0;
	*pwm = v;
	return 1;
}

#endif
