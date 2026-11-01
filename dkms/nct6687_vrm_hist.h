/* Software hist: PAGE rail slots and the VRM MOS thermistor.
 *
 * One place remembers min/max. The demand session decides when a PAGE sample
 * is stored. The MOS note is the stock temp3 reading, not a PMBus register.
 * IOUT/IIN/POUT/PIN drop a negative millisi sample. VOUT/VIN/TEMP and the MOS
 * reading keep the signed value.
 */
#ifndef NCT6687_VRM_HIST_H
#define NCT6687_VRM_HIST_H

#include "nct6687_vrm_decode.h"

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

static void nct_vrm_hist_add(long* min, long* max, bool* init, long val,
	bool reject_neg)
{
	if (reject_neg && val < 0)
		return;
	nct_vrm_hist_point(min, max, val, !*init);
	*init = true;
}

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

/* Low/high stay stored, but sensors only sees them while the live value is valid. */
static bool nct_vrm_publish_hist(bool live, bool ready)
{
	return live && ready;
}

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

/* Controller die. Signed, same rule as PAGE TEMP. */
static void nct_vrm_ctrl_note(bool* valid, bool* init, long* cur, long* min,
	long* max, long milli_c)
{
	nct_vrm_hist_point(min, max, milli_c, !*init);
	*init = true;
	*cur = milli_c;
	*valid = true;
}

/* Stock temp register: signed °C in the low byte, 0.5 °C in bit 7 of the next. */
static long nct_vrm_mos_milli(unsigned whole, unsigned frac)
{
	int value = (int)(whole & 0xff);

	if (value >= 128)
		value -= 256;
	return (long)value * 1000L + (500L * ((frac >> 7) & 1));
}

/* Stock VRM MOS temp3. Signed, same rule as PAGE TEMP. */
static void nct_vrm_mos_note(bool* init, long* min, long* max, long milli_c)
{
	nct_vrm_hist_point(min, max, milli_c, !*init);
	*init = true;
}

static bool nct_vrm_mos_ready(bool init)
{
	return init;
}

static long nct_vrm_mos_value(long min, long max, bool want_max)
{
	return want_max ? max : min;
}

#endif
