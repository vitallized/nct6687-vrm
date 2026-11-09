/* Included into nct6687.c by nct6687_vrm_dkms_inject.py — not a standalone TU.
 * Relies on stock types/macros (struct nct6687_data, EC_SPACE_*, SENSOR_DEVICE_ATTR).
 */
#include <linux/hwmon-sysfs.h>
#include <linux/workqueue.h>

/* NCT6687_VRM_PMBUS_INJECT — eSIO PMBus VRM (MSI MS-7D89 / addr 0xC0) */
/*
 * Default OFF. A VRM sysfs read copies the cached PAGE sample and kicks a
 * refresh. The read does not wait on the eSIO mailbox. The refresh follows
 * the inter-read gap down to ~20 ms (1 Hz when only the fan/temp hook asked)
 * and holds only EC_io_lock — same lock as nct6687_read/write.
 * r257 dropped that mutex; the splice puts it back around those helpers.
 */
static bool vrm;
module_param(vrm, bool, 0444);
MODULE_PARM_DESC(vrm, "Enable eSIO PMBus VRM sensors (default 0=off)");

static bool vrm_gt;
module_param(vrm_gt, bool, 0444);
MODULE_PARM_DESC(vrm_gt, "Also sample PMBus PAGE 1 (GT/iGPU); default 0=CPU only");

static int vrm_addr = 0xC0;
module_param(vrm_addr, int, 0444);
MODULE_PARM_DESC(vrm_addr, "PMBus 8-bit write address (default 0xC0)");

/* Bits (see nct6687_vrm_mailbox.h): 1 = port I/O without the 0x80 pause;
 * 2, 4, 8 = mailbox shortcuts (keep ctrl, keep proto/addr, no CLEAR pulse),
 * which turn themselves off after one failed sample. 0 is the access
 * pattern of every earlier release.
 */
static int vrm_fast;
module_param(vrm_fast, int, 0444);
MODULE_PARM_DESC(vrm_fast, "eSIO access bits: 1 no port pause, 2 keep ctrl, 4 keep proto/addr, 8 no CLEAR pulse (default 0)");

static int vrm_vout_exp = -10;
module_param(vrm_vout_exp, int, 0444);
MODULE_PARM_DESC(vrm_vout_exp, "Fallback LINEAR16 exp if VOUT_MODE unknown (-16..15)");

#include "nct6687_vrm_decode.h"

static unsigned long nct_vrm_now(void)
{
    return jiffies;
}

static unsigned long nct_vrm_eff_now_ms(void)
{
    return jiffies_to_msecs(jiffies);
}

static void nct_vrm_ec_lock(struct nct6687_data* data)
{
    mutex_lock(&data->EC_io_lock);
}

static void nct_vrm_ec_unlock(struct nct6687_data* data)
{
    mutex_unlock(&data->EC_io_lock);
}

static void nct_vrm_warn_recover(void)
{
    pr_warn_ratelimited("VRM recover failed\n");
}

static bool nct_vrm_claim_demand(struct nct6687_data* data)
{
    return xchg(&data->vrm_demand, false);
}

static bool nct_vrm_claim_full(struct nct6687_data* data)
{
    return xchg(&data->vrm_demand_full, false);
}

static unsigned nct_vrm_fast_mode(void)
{
    /* NOPAUSE | KEEP_CTRL | KEEP_SETUP | NO_CLEAR; the mailbox header comes later. */
    return (unsigned)vrm_fast & 0x0fu;
}

static void nct_vrm_warn_shortcuts(unsigned bits)
{
    pr_warn("VRM sample failed with vrm_fast shortcuts 0x%x; they stay off until reload\n", bits);
}

static void nct_vrm_note_negative(int metric, long val)
{
    const char* what = "reading";
    const char* unit = "";

    switch (metric) {
    case NCT_VRM_IOUT:
        what = "IOUT";
        unit = "mA";
        break;
    case NCT_VRM_IIN:
        what = "IIN";
        unit = "mA";
        break;
    case NCT_VRM_POUT:
        what = "POUT";
        unit = "uW";
        break;
    case NCT_VRM_PIN:
        what = "PIN";
        unit = "uW";
        break;
    }
    pr_warn_ratelimited("VRM %s %ld %s not published\n", what, val, unit);
}

#include "nct6687_vrm_demand.h"
#include "nct6687_vrm_prefetch.h"

/* One refresh at a time. Sysfs does not take this lock: it copies the snap.
 * The demand flag itself is claimed with xchg, so a kick during the sample
 * is not lost.
 */
static DEFINE_MUTEX(nct_vrm_session_lock);

/* What a sysfs read is allowed to see. Published once, at the end of a
 * refresh, so a reader never waits on the mailbox and never tears a rail.
 */
struct nct_vrm_snap {
    struct nct_vrm_rail rail[2];
    bool ctrl_valid;
    bool ctrl_hist_init;
    long ctrl_mc;
    long ctrl_min;
    long ctrl_max;
    bool eff_init;
    long eff_pout_uw;
    long eff_pin_uw;
    bool mos_hist_init;
    long mos_min;
    long mos_max;
    u64 sample_ns;
};

static struct nct_vrm_snap nct_vrm_snap;
static DEFINE_SPINLOCK(nct_vrm_snap_lock);
static DEFINE_SPINLOCK(nct_vrm_prefetch_lock);
static struct nct_vrm_prefetch nct_vrm_prefetch_state;
static struct nct6687_data* nct_vrm_work_data;
static void nct_vrm_now_work_fn(struct work_struct* work);
static void nct_vrm_prefetch_work_fn(struct work_struct* work);
static void nct_vrm_sensor_work_fn(struct work_struct* work);
/* Immediate sample and the timed prefetch are different work items so a
 * fan tick cannot consume the prefetch or cancel its timer.
 */
static DECLARE_WORK(nct_vrm_now_work, nct_vrm_now_work_fn);
static DECLARE_DELAYED_WORK(nct_vrm_prefetch_work, nct_vrm_prefetch_work_fn);
static DECLARE_WORK(nct_vrm_sensor_work, nct_vrm_sensor_work_fn);

static unsigned long nct_vrm_mono_ms(void)
{
    return (unsigned long)ktime_to_ms(ktime_get());
}

static void nct_vrm_publish_snap(struct nct6687_data* data, bool stamped)
{
    unsigned long flags;

    spin_lock_irqsave(&nct_vrm_snap_lock, flags);
    memcpy(nct_vrm_snap.rail, data->vrm_rail, sizeof(nct_vrm_snap.rail));
    nct_vrm_snap.ctrl_valid = data->vrm_ctrl_valid;
    nct_vrm_snap.ctrl_hist_init = data->vrm_ctrl_hist_init;
    nct_vrm_snap.ctrl_mc = data->vrm_ctrl_mc;
    nct_vrm_snap.ctrl_min = data->vrm_ctrl_min;
    nct_vrm_snap.ctrl_max = data->vrm_ctrl_max;
    nct_vrm_snap.eff_init = data->vrm_eff_init;
    nct_vrm_snap.eff_pout_uw = data->vrm_eff_pout_uw;
    nct_vrm_snap.eff_pin_uw = data->vrm_eff_pin_uw;
    nct_vrm_snap.mos_hist_init = data->vrm_mos_hist_init;
    nct_vrm_snap.mos_min = data->vrm_mos_min;
    nct_vrm_snap.mos_max = data->vrm_mos_max;
    if (stamped)
        nct_vrm_snap.sample_ns = ktime_get_ns();
    else if (!data->vrm_rail[0].valid)
        nct_vrm_snap.sample_ns = 0;
    spin_unlock_irqrestore(&nct_vrm_snap_lock, flags);
}

static unsigned long nct_vrm_sample_age_ms(void)
{
    unsigned long flags;
    u64 ns;
    s64 age;

    spin_lock_irqsave(&nct_vrm_snap_lock, flags);
    ns = nct_vrm_snap.sample_ns;
    spin_unlock_irqrestore(&nct_vrm_snap_lock, flags);
    if (!ns)
        return ULONG_MAX;
    age = ktime_ms_delta(ktime_get(), ns_to_ktime(ns));
    if (age < 0)
        return 0;
    return (unsigned long)age;
}

static void nct_vrm_snap_read(struct nct_vrm_snap* out)
{
    unsigned long flags;

    spin_lock_irqsave(&nct_vrm_snap_lock, flags);
    *out = nct_vrm_snap;
    spin_unlock_irqrestore(&nct_vrm_snap_lock, flags);
}

static void nct_vrm_disarm(void* ignored)
{
    (void)ignored;
    cancel_work_sync(&nct_vrm_now_work);
    cancel_delayed_work_sync(&nct_vrm_prefetch_work);
    cancel_work_sync(&nct_vrm_sensor_work);
    WRITE_ONCE(nct_vrm_work_data, NULL);
}

static void nct_vrm_arm(struct nct6687_data* data, struct device* dev)
{
    int rc;

    WRITE_ONCE(nct_vrm_work_data, data);
    rc = devm_add_action_or_reset(dev, nct_vrm_disarm, NULL);
    if (rc)
        pr_warn("VRM refresh hook failed (%d)\n", rc);
}

/* Timer plus a highpri queue still lands late. Start this much sooner than
 * the 40 ms margin so the sample is done before the next poll, then the
 * early-arrival correction pulls a prompt timer back in.
 */
#define NCT_VRM_PREFETCH_SLACK_MS 80UL

static bool nct_vrm_prefetch_busy(void)
{
    return delayed_work_pending(&nct_vrm_prefetch_work) ||
        work_busy(&nct_vrm_prefetch_work.work);
}

/* chain: the prefetch that just ran arms the next one interval - duration
 * after it finished, which already keeps its phase. Taking the slack off
 * that too ran the chain every interval - 80 ms: at a 200 ms reader, a
 * sample every ~120 ms, two per read (measured 2026-11-08).
 */
static void nct_vrm_arm_prefetch(unsigned long delay_ms, bool chain)
{
    unsigned long flags, lead = delay_ms;

    if (!chain && lead > NCT_VRM_PREFETCH_SLACK_MS)
        lead -= NCT_VRM_PREFETCH_SLACK_MS;
    spin_lock_irqsave(&nct_vrm_prefetch_lock, flags);
    nct_vrm_prefetch_state.arm_n++;
    nct_vrm_prefetch_state.last_delay_ms = lead;
    spin_unlock_irqrestore(&nct_vrm_prefetch_lock, flags);
    mod_delayed_work(system_highpri_wq, &nct_vrm_prefetch_work, msecs_to_jiffies(lead));
}

static void nct_vrm_arm_now(void)
{
    /* Do not cancel the prefetch timer. A fallback read used to kill the
     * sample that was about to make the next read fresh.
     */
    schedule_work(&nct_vrm_now_work);
}

/* demand: this read wants the 20 ms floor, not the 1 Hz background rate.
 * A steady reader prefetches. A fan hook must not cancel that timer.
 * full: the reader shows more than vrm_cpu (VOUT IOUT POUT), so the next
 * sample reads every word.
 */
static void nct_vrm_kick(struct nct6687_data* data, bool demand, bool full)
{
    struct nct_vrm_prefetch_plan plan;
    unsigned long flags, age_ms;

    if (!READ_ONCE(nct_vrm_work_data))
        return;
    if (!demand) {
        /* 1 Hz background only when no prefetch is already waiting.
         * Never the delayed work: that timer belongs to the hit path.
         */
        if (!nct_vrm_prefetch_busy())
            schedule_work(&nct_vrm_now_work);
        return;
    }
    if (full)
        WRITE_ONCE(data->vrm_demand_full, true);
    WRITE_ONCE(data->vrm_demand, true);
    age_ms = nct_vrm_sample_age_ms();
    spin_lock_irqsave(&nct_vrm_prefetch_lock, flags);
    plan = nct_vrm_prefetch_note(&nct_vrm_prefetch_state, nct_vrm_mono_ms(), age_ms, 20);
    if (plan.delay_ms)
        nct_vrm_prefetch_expect(&nct_vrm_prefetch_state);
    spin_unlock_irqrestore(&nct_vrm_prefetch_lock, flags);
    if (plan.act == NCT_VRM_PREFETCH_NONE)
        return;
    /* A fallback read used to mod_delayed_work the timer that was about
     * to publish, which pushed it past this poll forever. Leave it.
     */
    if (plan.act != NCT_VRM_PREFETCH_LATER && nct_vrm_prefetch_busy())
        return;
    if (plan.delay_ms)
        nct_vrm_arm_prefetch(plan.delay_ms, false);
    if (plan.act != NCT_VRM_PREFETCH_LATER)
        nct_vrm_arm_now();
}

static void nct_vrm_data_reset(struct nct6687_data* data, bool enabled)
{
    data->vrm_enabled = enabled;
    data->vrm_demand = false;
    data->vrm_demand_full = false;
    data->vrm_full_at = 0;
    data->vrm_full_asked_at = 0;
    data->vrm_last_updated = 0;
    data->vrm_last_read = 0;
    nct_vrm_eff_reset(&data->vrm_eff_init);
    data->vrm_eff_pout_uw = 0;
    data->vrm_eff_pin_uw = 0;
    data->vrm_eff_at_ms = 0;
    data->vrm_read_gap = 0;
    data->vrm_fail_count = 0;
    data->vrm_gt_fails = 0;
    data->vrm_gt_fail_at = 0;
    data->vrm_mux_hold = false;
    data->vrm_mux_cfg = 0;
    data->vrm_mux_baud = 0;
    data->vrm_smbus_page = -1;
    data->vrm_vout_mode_valid[0] = false;
    data->vrm_vout_mode_valid[1] = false;
    memset(data->vrm_rail, 0, sizeof(data->vrm_rail));
    data->vrm_mos_hist_init = false;
    data->vrm_mos_read_at = 0;
    data->vrm_mos_min = 0;
    data->vrm_mos_max = 0;
    data->vrm_ctrl_valid = false;
    data->vrm_ctrl_hist_init = false;
    data->vrm_ctrl_mc = 0;
    data->vrm_ctrl_min = 0;
    data->vrm_ctrl_max = 0;
    nct_vrm_publish_snap(data, false);
    nct_vrm_prefetch_state = (struct nct_vrm_prefetch){ 0 };
    nct_vrm_busy_ns = 0;
    nct_vrm_busy_samples = 0;
    nct_vrm_busy_depth = 0;
    nct_vrm_start_min_us = 0;
}

/* nct6687_temp_label[2] is "VRM MOS" → hwmon temp3. r257 stores only the
 * current reading; min/max here are software hist so sensors prints low/high.
 */
#define NCT_VRM_MOS_CH 2
#define NCT_VRM_MOS_NR 100
#define NCT_VRM_CTRL_NR 101
#define NCT_VRM_FULL_NR 102
#define NCT_VRM_AGE_NR 103
#define NCT_VRM_AGE_AT_NR 104
#define NCT_VRM_PREFETCH_STATE_NR 105
#define NCT_VRM_BUSY_NS_NR 106

static bool nct_vrm_mos_present(void)
{
    return NCT6687_TEMP_ENABLED(NCT_VRM_MOS_CH) &&
        nct6687_temp_label[NCT_VRM_MOS_CH] &&
        !strcmp(nct6687_temp_label[NCT_VRM_MOS_CH], "VRM MOS");
}

static void nct_vrm_note_mos(struct nct6687_data* data)
{
    if (!nct_vrm_mos_present())
        return;
    nct_vrm_mos_note(&data->vrm_mos_hist_init, &data->vrm_mos_min,
        &data->vrm_mos_max, data->temperature[NCT_VRM_MOS_CH]);
}

/* Two registers, same decode as nct6687_update_temperatures. Not the full scan. */
static void nct_vrm_sample_mos(struct nct6687_data* data)
{
    unsigned long now, hold;
    u16 whole, frac;

    if (!nct_vrm_mos_present())
        return;
    now = jiffies;
    hold = msecs_to_jiffies(20);
    if (!hold)
        hold = 1;
    if (data->vrm_mos_hist_init && time_before(now, data->vrm_mos_read_at + hold))
        return;
    /* One EC_io_lock for both bytes. Two nct6687_read calls drop it between them. */
    nct6687_read_pair(data, NCT6687_REG_TEMP(NCT_VRM_MOS_CH), &whole, &frac);
    data->temperature[NCT_VRM_MOS_CH] = nct_vrm_mos_milli(whole, frac);
    data->vrm_mos_read_at = now;
    nct_vrm_note_mos(data);
}

/* Caller holds nct_vrm_session_lock until nct_vrm_session_leave.
 * A kick may already have set vrm_demand. That still samples the thermistor.
 */
static void nct_vrm_session_enter(struct nct6687_data* data, bool demand, bool force_floor)
{
    unsigned long floor;
    bool want_demand;

    mutex_lock(&nct_vrm_session_lock);
    want_demand = demand || READ_ONCE(data->vrm_demand);
    if (demand)
        data->vrm_demand = true;
    else if (!want_demand && !time_after(jiffies, data->last_updated)) {
        /* Cache hits also call update_vrm. Only a scan that just stored
         * temperature[] should hold off the VRM thermistor read.
         */
        nct_vrm_note_mos(data);
        data->vrm_mos_read_at = jiffies;
    }
    if (want_demand)
        nct_vrm_sample_mos(data);
    floor = msecs_to_jiffies(20);
    if (!floor)
        floor = 1;
    nct_vrm_demand_session(data, HZ, floor, (u8)(vrm_addr & 0xff), vrm_vout_exp,
        vrm_gt, force_floor);
}

static void nct_vrm_session_leave(struct nct6687_data* data, bool sampled)
{
    nct_vrm_publish_snap(data, sampled);
    if (sampled)
        nct_vrm_busy_note_sample();
    mutex_unlock(&nct_vrm_session_lock);
}

/* page0, when non-NULL, receives the CPU rail under this lock. -ENODATA if invalid.
 * sampled is true only when this call's EC sample changed vrm_last_updated.
 */
static int nct_vrm_locked(struct nct6687_data* data, bool demand, long* page0, bool* sampled,
    bool force_floor)
{
    unsigned long before;
    int rc = 0;

    before = data->vrm_last_updated;
    nct_vrm_session_enter(data, demand, force_floor);
    if (page0) {
        if (!data->vrm_rail[0].valid)
            rc = -ENODATA;
        else
            memcpy(page0, data->vrm_rail[0].val, sizeof(data->vrm_rail[0].val));
    }
    if (sampled)
        *sampled = data->vrm_last_updated != before;
    nct_vrm_session_leave(data, data->vrm_last_updated != before);
    return rc;
}

/* One sample before the hwmon device is registered, so the first read is
 * not ENODATA. Same session as a demand refresh, including the stuck-START
 * budget. A miss drops the fail stamp so the next read retries at once.
 */
static void nct_vrm_prime(struct nct6687_data* data)
{
    if (!data->vrm_enabled)
        return;
    nct_vrm_locked(data, true, NULL, NULL, false);
    if (data->vrm_rail[0].valid)
        return;
    data->vrm_fail_count = 0;
    data->vrm_gt_fails = 0;
    data->vrm_gt_fail_at = 0;
    data->vrm_last_updated = 0;
    data->vrm_last_read = 0;
    data->vrm_demand = false;
    nct_vrm_publish_snap(data, false);
}

/* prefetch: this run was the timed one, so chain the next period even if
 * the hit read never re-arms. now-work passes false and arms from the
 * read's burst instead.
 */
static void nct_vrm_run_sample(bool prefetch)
{
    struct nct6687_data* data = READ_ONCE(nct_vrm_work_data);
    unsigned long flags, seq, delay, dur;
    bool demanded, sampled;
    ktime_t t0;

    if (!data || !READ_ONCE(data->vrm_enabled))
        return;
    spin_lock_irqsave(&nct_vrm_prefetch_lock, flags);
    seq = nct_vrm_prefetch_state.burst_seq;
    if (prefetch)
        nct_vrm_prefetch_state.run_n++;
    if (prefetch && nct_vrm_prefetch_idle(&nct_vrm_prefetch_state, nct_vrm_mono_ms())) {
        nct_vrm_prefetch_state.skip_n++;
        spin_unlock_irqrestore(&nct_vrm_prefetch_lock, flags);
        return;
    }
    spin_unlock_irqrestore(&nct_vrm_prefetch_lock, flags);

    /* The read that armed this timer already consumed vrm_demand.
     * The 1 Hz background gate, and the demand gap rule, both drop a
     * prefetch: it starts just under 1 s after the kick, and the cache
     * is one sample younger than that gap. force_floor keeps the 20 ms
     * floor and the fail backoff, and lets this run publish.
     */
    if (prefetch)
        WRITE_ONCE(data->vrm_demand, true);
    demanded = READ_ONCE(data->vrm_demand);
    t0 = ktime_get();
    nct_vrm_locked(data, prefetch, NULL, &sampled, prefetch);
    dur = (unsigned long)ktime_ms_delta(ktime_get(), t0);
    if (!dur)
        dur = 1;

    spin_lock_irqsave(&nct_vrm_prefetch_lock, flags);
    if (sampled)
        nct_vrm_prefetch_note_duration(&nct_vrm_prefetch_state, dur);
    else if (prefetch)
        nct_vrm_prefetch_state.skip_n++;
    delay = 0;
    if (sampled && prefetch)
        delay = nct_vrm_prefetch_chain_ms(&nct_vrm_prefetch_state, dur);
    else if (sampled && demanded && nct_vrm_prefetch_state.burst_seq == seq)
        delay = nct_vrm_prefetch_delay_ms(&nct_vrm_prefetch_state, nct_vrm_mono_ms(), 20);
    if (delay)
        nct_vrm_prefetch_expect(&nct_vrm_prefetch_state);
    spin_unlock_irqrestore(&nct_vrm_prefetch_lock, flags);
    /* The prefetch work is running, so it is "busy"; it still has to arm
     * the next period. A now-work must not move a timer the read already set.
     */
    if (delay && (prefetch || !nct_vrm_prefetch_busy()))
        nct_vrm_arm_prefetch(delay, sampled && prefetch);
}

static void nct_vrm_now_work_fn(struct work_struct* work)
{
    (void)work;
    nct_vrm_run_sample(false);
}

static void nct_vrm_prefetch_work_fn(struct work_struct* work)
{
    (void)work;
    nct_vrm_run_sample(true);
}

/* 1 Hz fan/temp/volt scan. Runs on a worker so a reader never takes
 * EC_io_lock while a PAGE sample holds it. update_lock stays inside this
 * function; the VRM sample does not take it.
 */
static void nct_vrm_refresh_sensors(struct nct6687_data* data)
{
    mutex_lock(&data->update_lock);
    if (time_after(jiffies, data->last_updated + HZ) || !data->valid) {
        nct6687_update_voltage(data);
        nct6687_update_temperatures(data);
        nct6687_update_fans(data);
        data->last_updated = jiffies;
        data->valid = true;
    }
    mutex_unlock(&data->update_lock);
}

static void nct_vrm_sensor_work_fn(struct work_struct* work)
{
    struct nct6687_data* data = READ_ONCE(nct_vrm_work_data);

    (void)work;
    if (!data)
        return;
    nct_vrm_refresh_sensors(data);
}

static void nct_vrm_schedule_sensors(void)
{
    if (!READ_ONCE(nct_vrm_work_data))
        return;
    schedule_work(&nct_vrm_sensor_work);
}

static void nct6687_update_vrm(struct nct6687_data* data)
{
    /* Always, including vrm=0. The reader must not scan the EC itself. */
    nct_vrm_schedule_sensors();
    if (!data->vrm_enabled)
        return;

    /* Background hook. Demand flag stays clear, so the session keeps 1 Hz.
     * Do not sample here: a fan read must not block on the mailbox.
     */
    nct_vrm_kick(data, false, true);
}

static ssize_t vrm_cpu_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct nct6687_data* data = dev_get_drvdata(dev);
    long vout, iout, pout;
    int ok = 0;

    struct nct_vrm_snap snap;

    (void)attr;
    /* Cached PAGE sample. The refresh runs in a workqueue. Do not call
     * nct6687_update_device: that walks every fan, temp, and volt.
     */
    if (!data->vrm_enabled)
        return -ENODATA;
    nct_vrm_snap_read(&snap);
    nct_vrm_kick(data, true, false);
    if (snap.rail[0].valid) {
        vout = snap.rail[0].val[NCT_VRM_VOUT];
        iout = snap.rail[0].val[NCT_VRM_IOUT];
        pout = snap.rail[0].val[NCT_VRM_POUT];
        ok = 1;
    }
    if (!ok)
        return -ENODATA;
    return sprintf(buf, "%ld %ld %ld\n", vout, iout, pout);
}

/* VOUT VIN IOUT IIN POUT PIN from the cached CPU rail. */
static ssize_t vrm_cpu_full_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct nct6687_data* data = dev_get_drvdata(dev);
    struct nct_vrm_snap snap;

    (void)attr;
    if (!data->vrm_enabled)
        return -ENODATA;
    nct_vrm_snap_read(&snap);
    nct_vrm_kick(data, true, true);
    if (!snap.rail[0].valid)
        return -ENODATA;
    return nct_vrm_format_cpu_full(buf, snap.rail[0].val);
}

/*
 * One row per hwmon channel: (PAGE, metric) into data->vrm_rail.
 * nct_vrm_show / is_visible key off SENSOR_DEVICE_ATTR_2 nr=channel,
 * index=stat. PAGE 1 rows are hidden unless vrm_gt.
 */
enum nct_vrm_stat {
    NCT_VRM_INPUT,
    NCT_VRM_MIN,
    NCT_VRM_MAX,
    NCT_VRM_LABEL,
};

enum nct_vrm_ch {
    NCT_VRM_CPU_VOUT,
    NCT_VRM_CPU_VIN,
    NCT_VRM_CPU_IOUT,
    NCT_VRM_CPU_IIN,
    NCT_VRM_CPU_POUT,
    NCT_VRM_CPU_PIN,
    NCT_VRM_CPU_EFF,
    NCT_VRM_CPU_TEMP,
    NCT_VRM_GT_VOUT,
    NCT_VRM_GT_VIN,
    NCT_VRM_GT_IOUT,
    NCT_VRM_GT_IIN,
    NCT_VRM_GT_POUT,
    NCT_VRM_GT_PIN,
    NCT_VRM_GT_TEMP,
};

struct nct_vrm_chan {
    const char* label;
    u8 page;
    u8 metric; /* enum nct_vrm_metric */
};

static const struct nct_vrm_chan nct_vrm_chans[] = {
    [NCT_VRM_CPU_VOUT] = { "VRM VOUT", 0, NCT_VRM_VOUT },
    [NCT_VRM_CPU_VIN] = { "VRM VIN", 0, NCT_VRM_VIN },
    [NCT_VRM_CPU_IOUT] = { "VRM IOUT", 0, NCT_VRM_IOUT },
    [NCT_VRM_CPU_IIN] = { "VRM IIN", 0, NCT_VRM_IIN },
    [NCT_VRM_CPU_POUT] = { "VRM POUT", 0, NCT_VRM_POUT },
    [NCT_VRM_CPU_PIN] = { "VRM PIN", 0, NCT_VRM_PIN },
    [NCT_VRM_CPU_EFF] = { "VRM EFFICIENCY", 0, NCT_VRM_EFF },
    [NCT_VRM_CPU_TEMP] = { "VRM TEMP", 0, NCT_VRM_TEMP },
    [NCT_VRM_GT_VOUT] = { "VRM GT VOUT", 1, NCT_VRM_VOUT },
    [NCT_VRM_GT_VIN] = { "VRM GT VIN", 1, NCT_VRM_VIN },
    [NCT_VRM_GT_IOUT] = { "VRM GT IOUT", 1, NCT_VRM_IOUT },
    [NCT_VRM_GT_IIN] = { "VRM GT IIN", 1, NCT_VRM_IIN },
    [NCT_VRM_GT_POUT] = { "VRM GT POUT", 1, NCT_VRM_POUT },
    [NCT_VRM_GT_PIN] = { "VRM GT PIN", 1, NCT_VRM_PIN },
    [NCT_VRM_GT_TEMP] = { "VRM GT TEMP", 1, NCT_VRM_TEMP },
};

static ssize_t nct_vrm_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct sensor_device_attribute_2* sattr = to_sensor_dev_attr_2(attr);
    const struct nct_vrm_chan* ch;
    const struct nct_vrm_rail* r;
    struct nct6687_data* data;
    struct nct_vrm_snap snap;
    long val = 0;
    int rc = 0;

    if (sattr->nr >= ARRAY_SIZE(nct_vrm_chans))
        return -EINVAL;
    ch = &nct_vrm_chans[sattr->nr];

    if (sattr->index == NCT_VRM_LABEL)
        return sprintf(buf, "%s\n", ch->label);

    data = dev_get_drvdata(dev);
    if (!data->vrm_enabled)
        return -ENODATA;

    /* pwm9, read-only, no _enable. This sensors divides the count by 2
     * and prints "%", so 100% is 200. pwm1–pwm8 are the fan duties.
     * libsensors 3.6.2 keeps 4 chars of a pwm name, so pwm10 would alias
     * pwm1. No GT efficiency channel.
     * The snap is one publish. eff_publish reads two smoothed powers.
     */
    nct_vrm_snap_read(&snap);
    nct_vrm_kick(data, true, true);
    if (ch->metric == NCT_VRM_EFF) {
        if (sattr->index != NCT_VRM_INPUT)
            rc = -EINVAL;
        else if (!nct_vrm_eff_publish(snap.eff_init, snap.eff_pout_uw,
                snap.eff_pin_uw, &val))
            rc = -ENODATA;
    } else {
        r = &snap.rail[ch->page];
        switch (sattr->index) {
        case NCT_VRM_INPUT:
            if (!r->valid)
                rc = -ENODATA;
            else
                val = r->val[ch->metric];
            break;
        case NCT_VRM_MIN:
            if (!nct_vrm_publish_hist(r->valid, nct_vrm_rail_hist_ready(r, ch->metric)))
                rc = -ENODATA;
            else
                val = r->min[ch->metric];
            break;
        case NCT_VRM_MAX:
            if (!nct_vrm_publish_hist(r->valid, nct_vrm_rail_hist_ready(r, ch->metric)))
                rc = -ENODATA;
            else
                val = r->max[ch->metric];
            break;
        default:
            rc = -EINVAL;
            break;
        }
    }
    if (rc)
        return rc;
    return sprintf(buf, "%ld\n", val);
}

/* Live age of the published sample. Not a demand read: no kick, no cadence. */
static ssize_t vrm_sample_age_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct nct6687_data* data = dev_get_drvdata(dev);
    unsigned long age;

    (void)attr;
    if (!data->vrm_enabled)
        return -ENODATA;
    age = nct_vrm_sample_age_ms();
    if (age == ULONG_MAX)
        return -ENODATA;
    return sprintf(buf, "%lu\n", age);
}

/* Age the latest demand burst saw, frozen then. A debug read does not move it. */
static ssize_t vrm_sample_age_at_read_show(struct device* dev, struct device_attribute* attr,
    char* buf)
{
    unsigned long flags, age;
    bool valid;
    struct nct6687_data* data = dev_get_drvdata(dev);

    (void)attr;
    if (!data->vrm_enabled)
        return -ENODATA;
    spin_lock_irqsave(&nct_vrm_prefetch_lock, flags);
    valid = nct_vrm_prefetch_state.age_at_read_valid;
    age = nct_vrm_prefetch_state.age_at_read_ms;
    spin_unlock_irqrestore(&nct_vrm_prefetch_lock, flags);
    if (!valid)
        return -ENODATA;
    return sprintf(buf, "%lu\n", age);
}

/* prefetch|fallback|learning and the learned interval in ms. Not a demand read. */
static ssize_t vrm_prefetch_state_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    unsigned long flags, interval, dur, dly;
    unsigned stable, arm, run, skip;
    enum nct_vrm_prefetch_act act;
    struct nct6687_data* data = dev_get_drvdata(dev);
    const char* mode = "learning";

    (void)attr;
    if (!data->vrm_enabled)
        return -ENODATA;
    spin_lock_irqsave(&nct_vrm_prefetch_lock, flags);
    act = nct_vrm_prefetch_state.last_act;
    interval = nct_vrm_prefetch_state.interval_ms;
    dur = nct_vrm_prefetch_state.duration_ms;
    stable = nct_vrm_prefetch_state.stable;
    arm = nct_vrm_prefetch_state.arm_n;
    run = nct_vrm_prefetch_state.run_n;
    skip = nct_vrm_prefetch_state.skip_n;
    dly = nct_vrm_prefetch_state.last_delay_ms;
    spin_unlock_irqrestore(&nct_vrm_prefetch_lock, flags);
    if (act == NCT_VRM_PREFETCH_LATER)
        mode = "prefetch";
    else if (act == NCT_VRM_PREFETCH_NOW)
        mode = "fallback";
    return sprintf(buf, "%s %lu dur=%lu st=%u arm=%u run=%u skip=%u dly=%lu\n",
        mode, interval, dur, stable, arm, run, skip, dly);
}

/* Cumulative busy ns, then how many refreshes that covers. Not a demand read. */
static ssize_t vrm_busy_ns_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct nct6687_data* data = dev_get_drvdata(dev);

    (void)attr;
    if (!data->vrm_enabled)
        return -ENODATA;
    return sprintf(buf, "%llu %llu\n",
        (unsigned long long)READ_ONCE(nct_vrm_busy_ns),
        (unsigned long long)READ_ONCE(nct_vrm_busy_samples));
}

#define NCT_VRM_ATTR(_name, _ch, _stat) \
    static SENSOR_DEVICE_ATTR_2(_name, 0444, nct_vrm_show, NULL, _ch, _stat)

static SENSOR_DEVICE_ATTR_2(vrm_cpu, 0444, vrm_cpu_show, NULL,
    NCT_VRM_CPU_VOUT, NCT_VRM_INPUT);
static SENSOR_DEVICE_ATTR_2(vrm_cpu_full, 0444, vrm_cpu_full_show, NULL,
    NCT_VRM_FULL_NR, NCT_VRM_INPUT);
static SENSOR_DEVICE_ATTR_2(vrm_sample_age, 0444, vrm_sample_age_show, NULL,
    NCT_VRM_AGE_NR, NCT_VRM_INPUT);
static SENSOR_DEVICE_ATTR_2(vrm_sample_age_at_read, 0444, vrm_sample_age_at_read_show, NULL,
    NCT_VRM_AGE_AT_NR, NCT_VRM_INPUT);
static SENSOR_DEVICE_ATTR_2(vrm_prefetch_state, 0444, vrm_prefetch_state_show, NULL,
    NCT_VRM_PREFETCH_STATE_NR, NCT_VRM_INPUT);
static SENSOR_DEVICE_ATTR_2(vrm_busy_ns, 0444, vrm_busy_ns_show, NULL,
    NCT_VRM_BUSY_NS_NR, NCT_VRM_INPUT);
NCT_VRM_ATTR(in20_input, NCT_VRM_CPU_VOUT, NCT_VRM_INPUT);
NCT_VRM_ATTR(in20_label, NCT_VRM_CPU_VOUT, NCT_VRM_LABEL);
NCT_VRM_ATTR(in20_min, NCT_VRM_CPU_VOUT, NCT_VRM_MIN);
NCT_VRM_ATTR(in20_max, NCT_VRM_CPU_VOUT, NCT_VRM_MAX);
NCT_VRM_ATTR(in21_input, NCT_VRM_CPU_VIN, NCT_VRM_INPUT);
NCT_VRM_ATTR(in21_label, NCT_VRM_CPU_VIN, NCT_VRM_LABEL);
NCT_VRM_ATTR(in21_min, NCT_VRM_CPU_VIN, NCT_VRM_MIN);
NCT_VRM_ATTR(in21_max, NCT_VRM_CPU_VIN, NCT_VRM_MAX);
NCT_VRM_ATTR(curr1_input, NCT_VRM_CPU_IOUT, NCT_VRM_INPUT);
NCT_VRM_ATTR(curr1_label, NCT_VRM_CPU_IOUT, NCT_VRM_LABEL);
NCT_VRM_ATTR(curr1_min, NCT_VRM_CPU_IOUT, NCT_VRM_MIN);
NCT_VRM_ATTR(curr1_max, NCT_VRM_CPU_IOUT, NCT_VRM_MAX);
NCT_VRM_ATTR(curr3_input, NCT_VRM_CPU_IIN, NCT_VRM_INPUT);
NCT_VRM_ATTR(curr3_label, NCT_VRM_CPU_IIN, NCT_VRM_LABEL);
NCT_VRM_ATTR(curr3_min, NCT_VRM_CPU_IIN, NCT_VRM_MIN);
NCT_VRM_ATTR(curr3_max, NCT_VRM_CPU_IIN, NCT_VRM_MAX);
NCT_VRM_ATTR(power1_input, NCT_VRM_CPU_POUT, NCT_VRM_INPUT);
NCT_VRM_ATTR(power1_label, NCT_VRM_CPU_POUT, NCT_VRM_LABEL);
/* input_lowest/highest: sensors prints those before power min/max, and
 * power min/max print max-then-min. There is no power "low" label.
 */
NCT_VRM_ATTR(power1_input_lowest, NCT_VRM_CPU_POUT, NCT_VRM_MIN);
NCT_VRM_ATTR(power1_input_highest, NCT_VRM_CPU_POUT, NCT_VRM_MAX);
NCT_VRM_ATTR(power3_input, NCT_VRM_CPU_PIN, NCT_VRM_INPUT);
NCT_VRM_ATTR(power3_label, NCT_VRM_CPU_PIN, NCT_VRM_LABEL);
NCT_VRM_ATTR(power3_input_lowest, NCT_VRM_CPU_PIN, NCT_VRM_MIN);
NCT_VRM_ATTR(power3_input_highest, NCT_VRM_CPU_PIN, NCT_VRM_MAX);
NCT_VRM_ATTR(pwm9, NCT_VRM_CPU_EFF, NCT_VRM_INPUT);
NCT_VRM_ATTR(pwm9_label, NCT_VRM_CPU_EFF, NCT_VRM_LABEL);
NCT_VRM_ATTR(temp20_input, NCT_VRM_CPU_TEMP, NCT_VRM_INPUT);
NCT_VRM_ATTR(temp20_label, NCT_VRM_CPU_TEMP, NCT_VRM_LABEL);
NCT_VRM_ATTR(temp20_min, NCT_VRM_CPU_TEMP, NCT_VRM_MIN);
NCT_VRM_ATTR(temp20_max, NCT_VRM_CPU_TEMP, NCT_VRM_MAX);

static ssize_t nct_vrm_ctrl_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct sensor_device_attribute_2* sattr = to_sensor_dev_attr_2(attr);
    struct nct6687_data* data;
    struct nct_vrm_snap snap;
    long val = 0;
    int rc = 0;

    if (sattr->index == NCT_VRM_LABEL)
        return sprintf(buf, "VRM CONTROLLER\n");

    data = dev_get_drvdata(dev);
    if (!data->vrm_enabled)
        return -ENODATA;
    nct_vrm_snap_read(&snap);
    nct_vrm_kick(data, true, true);
    switch (sattr->index) {
    case NCT_VRM_INPUT:
        if (!snap.ctrl_valid)
            rc = -ENODATA;
        else
            val = snap.ctrl_mc;
        break;
    case NCT_VRM_MIN:
    case NCT_VRM_MAX:
        if (!nct_vrm_publish_hist(snap.ctrl_valid, snap.ctrl_hist_init))
            rc = -ENODATA;
        else
            val = sattr->index == NCT_VRM_MAX ? snap.ctrl_max : snap.ctrl_min;
        break;
    default:
        rc = -EINVAL;
        break;
    }
    if (rc)
        return rc;
    return sprintf(buf, "%ld\n", val);
}

static SENSOR_DEVICE_ATTR_2(temp22_input, 0444, nct_vrm_ctrl_show, NULL,
    NCT_VRM_CTRL_NR, NCT_VRM_INPUT);
static SENSOR_DEVICE_ATTR_2(temp22_label, 0444, nct_vrm_ctrl_show, NULL,
    NCT_VRM_CTRL_NR, NCT_VRM_LABEL);
static SENSOR_DEVICE_ATTR_2(temp22_min, 0444, nct_vrm_ctrl_show, NULL,
    NCT_VRM_CTRL_NR, NCT_VRM_MIN);
static SENSOR_DEVICE_ATTR_2(temp22_max, 0444, nct_vrm_ctrl_show, NULL,
    NCT_VRM_CTRL_NR, NCT_VRM_MAX);
NCT_VRM_ATTR(in22_input, NCT_VRM_GT_VOUT, NCT_VRM_INPUT);
NCT_VRM_ATTR(in22_label, NCT_VRM_GT_VOUT, NCT_VRM_LABEL);
NCT_VRM_ATTR(in22_min, NCT_VRM_GT_VOUT, NCT_VRM_MIN);
NCT_VRM_ATTR(in22_max, NCT_VRM_GT_VOUT, NCT_VRM_MAX);
NCT_VRM_ATTR(in23_input, NCT_VRM_GT_VIN, NCT_VRM_INPUT);
NCT_VRM_ATTR(in23_label, NCT_VRM_GT_VIN, NCT_VRM_LABEL);
NCT_VRM_ATTR(in23_min, NCT_VRM_GT_VIN, NCT_VRM_MIN);
NCT_VRM_ATTR(in23_max, NCT_VRM_GT_VIN, NCT_VRM_MAX);
NCT_VRM_ATTR(curr2_input, NCT_VRM_GT_IOUT, NCT_VRM_INPUT);
NCT_VRM_ATTR(curr2_label, NCT_VRM_GT_IOUT, NCT_VRM_LABEL);
NCT_VRM_ATTR(curr2_min, NCT_VRM_GT_IOUT, NCT_VRM_MIN);
NCT_VRM_ATTR(curr2_max, NCT_VRM_GT_IOUT, NCT_VRM_MAX);
NCT_VRM_ATTR(curr4_input, NCT_VRM_GT_IIN, NCT_VRM_INPUT);
NCT_VRM_ATTR(curr4_label, NCT_VRM_GT_IIN, NCT_VRM_LABEL);
NCT_VRM_ATTR(curr4_min, NCT_VRM_GT_IIN, NCT_VRM_MIN);
NCT_VRM_ATTR(curr4_max, NCT_VRM_GT_IIN, NCT_VRM_MAX);
NCT_VRM_ATTR(power2_input, NCT_VRM_GT_POUT, NCT_VRM_INPUT);
NCT_VRM_ATTR(power2_label, NCT_VRM_GT_POUT, NCT_VRM_LABEL);
NCT_VRM_ATTR(power2_input_lowest, NCT_VRM_GT_POUT, NCT_VRM_MIN);
NCT_VRM_ATTR(power2_input_highest, NCT_VRM_GT_POUT, NCT_VRM_MAX);
NCT_VRM_ATTR(power4_input, NCT_VRM_GT_PIN, NCT_VRM_INPUT);
NCT_VRM_ATTR(power4_label, NCT_VRM_GT_PIN, NCT_VRM_LABEL);
NCT_VRM_ATTR(power4_input_lowest, NCT_VRM_GT_PIN, NCT_VRM_MIN);
NCT_VRM_ATTR(power4_input_highest, NCT_VRM_GT_PIN, NCT_VRM_MAX);
NCT_VRM_ATTR(temp21_input, NCT_VRM_GT_TEMP, NCT_VRM_INPUT);
NCT_VRM_ATTR(temp21_label, NCT_VRM_GT_TEMP, NCT_VRM_LABEL);
NCT_VRM_ATTR(temp21_min, NCT_VRM_GT_TEMP, NCT_VRM_MIN);
NCT_VRM_ATTR(temp21_max, NCT_VRM_GT_TEMP, NCT_VRM_MAX);

static struct nct6687_data* nct6687_update_device(struct device* dev);

static ssize_t nct_vrm_mos_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct sensor_device_attribute_2* sattr = to_sensor_dev_attr_2(attr);
    struct nct_vrm_snap snap;
    long val = 0;
    int ok;

    /* Stock scan, then the cached thermistor hist. Not a VRM demand. */
    nct6687_update_device(dev);
    nct_vrm_snap_read(&snap);
    ok = nct_vrm_mos_ready(snap.mos_hist_init);
    if (ok)
        val = nct_vrm_mos_value(snap.mos_min, snap.mos_max,
            sattr->index == NCT_VRM_MAX);
    if (!ok)
        return -ENODATA;
    return sprintf(buf, "%ld\n", val);
}

static SENSOR_DEVICE_ATTR_2(temp3_min, 0444, nct_vrm_mos_show, NULL,
    NCT_VRM_MOS_NR, NCT_VRM_MIN);
static SENSOR_DEVICE_ATTR_2(temp3_max, 0444, nct_vrm_mos_show, NULL,
    NCT_VRM_MOS_NR, NCT_VRM_MAX);

static umode_t nct6687_vrm_attr_is_visible(struct kobject* kobj,
    struct attribute* attr, int idx)
{
    struct sensor_device_attribute_2* sattr =
        container_of(attr, struct sensor_device_attribute_2, dev_attr.attr);

    (void)kobj;
    (void)idx;
    if (sattr->nr == NCT_VRM_MOS_NR)
        return nct_vrm_mos_present() ? 0444 : 0;
    if (sattr->nr >= ARRAY_SIZE(nct_vrm_chans))
        return 0444;
    if (nct_vrm_chans[sattr->nr].page == 1 && !vrm_gt)
        return 0;
    return 0444;
}

static struct attribute* nct6687_vrm_attrs[] = {
    &sensor_dev_attr_vrm_cpu.dev_attr.attr,
    &sensor_dev_attr_vrm_cpu_full.dev_attr.attr,
    &sensor_dev_attr_vrm_sample_age.dev_attr.attr,
    &sensor_dev_attr_vrm_sample_age_at_read.dev_attr.attr,
    &sensor_dev_attr_vrm_prefetch_state.dev_attr.attr,
    &sensor_dev_attr_vrm_busy_ns.dev_attr.attr,
    &sensor_dev_attr_in20_input.dev_attr.attr,
    &sensor_dev_attr_in20_label.dev_attr.attr,
    &sensor_dev_attr_in20_min.dev_attr.attr,
    &sensor_dev_attr_in20_max.dev_attr.attr,
    &sensor_dev_attr_in21_input.dev_attr.attr,
    &sensor_dev_attr_in21_label.dev_attr.attr,
    &sensor_dev_attr_in21_min.dev_attr.attr,
    &sensor_dev_attr_in21_max.dev_attr.attr,
    &sensor_dev_attr_curr1_input.dev_attr.attr,
    &sensor_dev_attr_curr1_label.dev_attr.attr,
    &sensor_dev_attr_curr1_min.dev_attr.attr,
    &sensor_dev_attr_curr1_max.dev_attr.attr,
    &sensor_dev_attr_curr3_input.dev_attr.attr,
    &sensor_dev_attr_curr3_label.dev_attr.attr,
    &sensor_dev_attr_curr3_min.dev_attr.attr,
    &sensor_dev_attr_curr3_max.dev_attr.attr,
    &sensor_dev_attr_power1_input.dev_attr.attr,
    &sensor_dev_attr_power1_label.dev_attr.attr,
    &sensor_dev_attr_power1_input_lowest.dev_attr.attr,
    &sensor_dev_attr_power1_input_highest.dev_attr.attr,
    &sensor_dev_attr_power3_input.dev_attr.attr,
    &sensor_dev_attr_power3_label.dev_attr.attr,
    &sensor_dev_attr_power3_input_lowest.dev_attr.attr,
    &sensor_dev_attr_power3_input_highest.dev_attr.attr,
    &sensor_dev_attr_pwm9.dev_attr.attr,
    &sensor_dev_attr_pwm9_label.dev_attr.attr,
    &sensor_dev_attr_temp20_input.dev_attr.attr,
    &sensor_dev_attr_temp20_label.dev_attr.attr,
    &sensor_dev_attr_temp20_min.dev_attr.attr,
    &sensor_dev_attr_temp20_max.dev_attr.attr,
    &sensor_dev_attr_temp22_input.dev_attr.attr,
    &sensor_dev_attr_temp22_label.dev_attr.attr,
    &sensor_dev_attr_temp22_min.dev_attr.attr,
    &sensor_dev_attr_temp22_max.dev_attr.attr,
    &sensor_dev_attr_temp3_min.dev_attr.attr,
    &sensor_dev_attr_temp3_max.dev_attr.attr,
    &sensor_dev_attr_in22_input.dev_attr.attr,
    &sensor_dev_attr_in22_label.dev_attr.attr,
    &sensor_dev_attr_in22_min.dev_attr.attr,
    &sensor_dev_attr_in22_max.dev_attr.attr,
    &sensor_dev_attr_in23_input.dev_attr.attr,
    &sensor_dev_attr_in23_label.dev_attr.attr,
    &sensor_dev_attr_in23_min.dev_attr.attr,
    &sensor_dev_attr_in23_max.dev_attr.attr,
    &sensor_dev_attr_curr2_input.dev_attr.attr,
    &sensor_dev_attr_curr2_label.dev_attr.attr,
    &sensor_dev_attr_curr2_min.dev_attr.attr,
    &sensor_dev_attr_curr2_max.dev_attr.attr,
    &sensor_dev_attr_curr4_input.dev_attr.attr,
    &sensor_dev_attr_curr4_label.dev_attr.attr,
    &sensor_dev_attr_curr4_min.dev_attr.attr,
    &sensor_dev_attr_curr4_max.dev_attr.attr,
    &sensor_dev_attr_power2_input.dev_attr.attr,
    &sensor_dev_attr_power2_label.dev_attr.attr,
    &sensor_dev_attr_power2_input_lowest.dev_attr.attr,
    &sensor_dev_attr_power2_input_highest.dev_attr.attr,
    &sensor_dev_attr_power4_input.dev_attr.attr,
    &sensor_dev_attr_power4_label.dev_attr.attr,
    &sensor_dev_attr_power4_input_lowest.dev_attr.attr,
    &sensor_dev_attr_power4_input_highest.dev_attr.attr,
    &sensor_dev_attr_temp21_input.dev_attr.attr,
    &sensor_dev_attr_temp21_label.dev_attr.attr,
    &sensor_dev_attr_temp21_min.dev_attr.attr,
    &sensor_dev_attr_temp21_max.dev_attr.attr,
    NULL,
};

static const struct attribute_group nct6687_vrm_group = {
    .attrs = nct6687_vrm_attrs,
    .is_visible = nct6687_vrm_attr_is_visible,
};
