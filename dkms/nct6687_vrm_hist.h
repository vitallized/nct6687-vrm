/* Software hist: PAGE rail slots and the VRM MOS thermistor.
 *
 * One place remembers min/max. The demand session decides when a PAGE sample
 * is stored. The MOS note is the stock temp3 reading, not a PMBus register.
 * IOUT/IIN/POUT/PIN: a negative is logged and not published. The live value
 * stays at the last non-negative reading (0 if there has not been one).
 * VOUT/VIN/TEMP and the MOS reading keep the signed value.
 */
#ifndef NCT6687_VRM_HIST_H
#define NCT6687_VRM_HIST_H

#include "nct6687_vrm_decode.h"

/* Kernel logs it. Host tests count it. Defined by the translation unit. */
static void nct_vrm_note_negative(int metric, long val);

static bool nct_vrm_drops_negative(int metric)
{
	return metric == NCT_VRM_IOUT || metric == NCT_VRM_POUT ||
		metric == NCT_VRM_IIN || metric == NCT_VRM_PIN;
}

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
	long pub[NCT_VRM_NMETRIC];
	bool first = !r->hist_init;
	int m;

	for (m = 0; m < NCT_VRM_NMETRIC; m++) {
		if (nct_vrm_drops_negative(m) && sample[m] < 0) {
			nct_vrm_note_negative(m, sample[m]);
			pub[m] = r->val[m];
		} else {
			pub[m] = sample[m];
		}
	}
	memcpy(r->val, pub, sizeof(r->val));
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

/* PAGE 0 snapshot: VOUT VIN IOUT IIN POUT PIN. One line, millisi units. */
static int nct_vrm_format_cpu_full(char* buf, const long* v)
{
	return sprintf(buf, "%ld %ld %ld %ld %ld %ld\n",
		v[NCT_VRM_VOUT], v[NCT_VRM_VIN], v[NCT_VRM_IOUT], v[NCT_VRM_IIN],
		v[NCT_VRM_POUT], v[NCT_VRM_PIN]);
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
