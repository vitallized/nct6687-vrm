/* VRM demand session: one PAGE attempt, or a skip.
 *
 * Caller does not hold EC_io_lock. The session takes it only when a sample
 * is due. A demand stamps vrm_last_read here, after the gap is captured, so
 * the gap is the previous demand, never a synthetic HZ.
 *
 * The eSIO save/arm/enable/body/disable/restore window is nct_vrm_mux_cycle.
 * Due-check, the EC lock, fail_count, the GT page, rail_apply, and recover
 * warnings stay here.
 *
 * The translation unit provides:
 *   unsigned long nct_vrm_now(void);
 *   void nct_vrm_ec_lock(struct nct6687_data *data);
 *   void nct_vrm_ec_unlock(struct nct6687_data *data);
 *   void nct_vrm_warn_recover(void);
 *
 * Host tests define those and compile with -DNCT_VRM_MAILBOX_HOST.
 */
#ifndef NCT6687_VRM_DEMAND_H
#define NCT6687_VRM_DEMAND_H

#include "nct6687_vrm_mux.h"
#include "nct6687_vrm_hist.h"

static unsigned long nct_vrm_now(void);
static unsigned long nct_vrm_eff_now_ms(void);
static void nct_vrm_ec_lock(struct nct6687_data* data);
static void nct_vrm_ec_unlock(struct nct6687_data* data);
static void nct_vrm_warn_recover(void);
static bool nct_vrm_claim_demand(struct nct6687_data* data);

/* First fail stays ~HZ/4; then HZ, 2*HZ, cap 8*HZ. fails==0 uses the
 * first-fail slot so a cold invalid cache still retries at HZ/4.
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

/* Background is 1 Hz. Demand after idle or first-ever uses the 20 ms floor,
 * not 1 Hz just because last_read is 0 or the gap is already >= 1 s.
 * Invalid uses the fail interval even on demand.
 */
static bool nct_vrm_should_sample(bool valid, bool demanded, bool last_read_set,
	unsigned long gap, unsigned long last_updated_age, unsigned long hz,
	unsigned long floor, unsigned fails, bool force_floor)
{
	unsigned long interval;

	if (!valid)
		interval = nct_vrm_fail_interval(fails, hz);
	else if (!demanded)
		interval = hz;
	/* force_floor is the prefetch timer. It is aimed just under 1 s, and
	 * the cache is one sample (~70 ms) younger than the gap, so the gap
	 * rule below would skip it every time. The 20 ms floor still applies.
	 * Fail backoff above is unchanged.
	 */
	else if (force_floor || !last_read_set || gap >= hz)
		interval = floor;
	else if (gap > floor)
		interval = gap;
	else
		interval = floor;

	/* Equal counts. last_updated and last_read are both jiffies, a few port
	 * writes apart, so a steady poll sees age == gap. Strict > skips that
	 * read and the next one samples, which is half the poll rate.
	 */
	return last_updated_age >= interval;
}

static int nct_vrm_bus_recover(struct nct6687_data* data)
{
	nct_vrm_invalidate_smbus(data);
	return nct_vrm_recover(data);
}

struct nct_vrm_demand_job {
	u8 addr;
	int vout_exp;
	bool sample_gt;
	unsigned long hz;
	long sample[NCT_VRM_NMETRIC];
};

/* GT retry uses the same fail ladder as the CPU rail. fails==0 is due. */
static bool nct_vrm_gt_due(const struct nct6687_data* data, unsigned long hz)
{
	if (!data->vrm_gt_fails)
		return true;
	return nct_vrm_now() - data->vrm_gt_fail_at >
		nct_vrm_fail_interval(data->vrm_gt_fails, hz);
}

/* Failed window: invalidate, stamp, count, recover. Not a bad restore. */
static int nct_vrm_demand_fail(struct nct6687_data* data)
{
	data->vrm_rail[0].valid = false;
	data->vrm_rail[1].valid = false;
	data->vrm_ctrl_valid = false;
	nct_vrm_eff_reset(&data->vrm_eff_init);
	data->vrm_last_updated = nct_vrm_now();
	data->vrm_fail_count++;
	if (nct_vrm_bus_recover(data))
		nct_vrm_warn_recover();
	return -EIO;
}

static int nct_vrm_demand_body(struct nct6687_data* data, void* vctx)
{
	struct nct_vrm_demand_job* job = vctx;
	struct nct_vrm_rail* cpu = &data->vrm_rail[0];
	struct nct_vrm_rail* gt = &data->vrm_rail[1];

	if (nct_vrm_sample_page(data, job->addr, 0, job->vout_exp, job->sample, NULL))
		return -EIO;

	/* 0x8E is the controller die. Global, so one read per session, not per PAGE.
	 * A miss drops this attempt the same way a CPU PAGE miss does. */
	{
		u16 ctrl;

		if (nct_vrm_read_word(data, job->addr, 0x8e, &ctrl))
			return -EIO;
		nct_vrm_ctrl_note(&data->vrm_ctrl_valid, &data->vrm_ctrl_hist_init,
			&data->vrm_ctrl_mc, &data->vrm_ctrl_min, &data->vrm_ctrl_max,
			nct_vrm_linear11_milli(ctrl));
	}

	nct_vrm_rail_apply(cpu, job->sample);
	nct_vrm_eff_update(&data->vrm_eff_init, &data->vrm_eff_pout_uw,
		&data->vrm_eff_pin_uw, &data->vrm_eff_at_ms,
		job->sample[NCT_VRM_POUT], job->sample[NCT_VRM_PIN],
		nct_vrm_eff_now_ms());
	data->vrm_last_updated = nct_vrm_now();

	/* GT failure after a good CPU sample keeps CPU valid and does not
	 * bump vrm_fail_count. The GT retry has its own backoff so a dead
	 * PAGE 1 is not sampled on every fast CPU demand. A skip leaves the
	 * previous GT slot alone.
	 */
	if (job->sample_gt && nct_vrm_gt_due(data, job->hz)) {
		if (nct_vrm_sample_page(data, job->addr, 1, job->vout_exp, job->sample,
				NULL)) {
			gt->valid = false;
			data->vrm_gt_fails++;
			data->vrm_gt_fail_at = nct_vrm_now();
			if (nct_vrm_bus_recover(data))
				nct_vrm_warn_recover();
		} else {
			nct_vrm_rail_apply(gt, job->sample);
			data->vrm_gt_fails = 0;
		}
	} else if (!job->sample_gt) {
		gt->valid = false;
	}
	return 0;
}

/* now/hz/floor are jiffies. floor is the 20 ms demand floor from the caller. */
static void nct_vrm_demand_session(struct nct6687_data* data, unsigned long hz,
	unsigned long floor, u8 addr, int vout_exp, bool sample_gt, bool force_floor)
{
	struct nct_vrm_rail* cpu = &data->vrm_rail[0];
	struct nct_vrm_rail* gt = &data->vrm_rail[1];
	struct nct_vrm_demand_job job;
	unsigned long now, gap, age;
	bool demanded, last_read_set;
	enum nct_vrm_mux_phase phase;

	if (!data->vrm_enabled)
		return;

	demanded = nct_vrm_claim_demand(data);

	now = nct_vrm_now();
	last_read_set = data->vrm_last_read != 0;
	gap = last_read_set ? now - data->vrm_last_read : 0;
	age = data->vrm_last_updated ? now - data->vrm_last_updated : ~0UL;
	if (demanded) {
		data->vrm_read_gap = gap;
		data->vrm_last_read = now;
	}

	if (!nct_vrm_should_sample(cpu->valid, demanded, last_read_set, gap, age,
			hz, floor, data->vrm_fail_count, force_floor))
		return;

	job.addr = addr;
	job.vout_exp = vout_exp;
	job.sample_gt = sample_gt;
	job.hz = hz;
	nct_vrm_ec_lock(data);
	phase = nct_vrm_mux_cycle(data, 0, nct_vrm_demand_body, NULL,
		nct_vrm_demand_fail, &job, NCT_VRM_MUX_RESTORE_CHECKED);
	/* Reset the fail count only after the PAGE sample and the mux restore
	 * both succeed. A failed restore drops the cache so backoff applies.
	 * ARM/BODY already ran demand_fail inside the cycle, before restore.
	 */
	switch (phase) {
	case NCT_VRM_MUX_OK:
		data->vrm_fail_count = 0;
		break;
	case NCT_VRM_MUX_RESTORE:
		cpu->valid = false;
		gt->valid = false;
		data->vrm_ctrl_valid = false;
		nct_vrm_eff_reset(&data->vrm_eff_init);
		data->vrm_fail_count++;
		break;
	case NCT_VRM_MUX_SAVE:
		nct_vrm_demand_fail(data);
		break;
	case NCT_VRM_MUX_ARM:
	case NCT_VRM_MUX_BODY:
		break;
	}
	nct_vrm_ec_unlock(data);
}

#endif
