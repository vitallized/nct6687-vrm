/* Included into nct6687.c by nct6687_vrm_dkms_inject.py — not a standalone TU.
 * Relies on stock types/macros (struct nct6687_data, EC_SPACE_*, SENSOR_DEVICE_ATTR).
 */
#include <linux/hwmon-sysfs.h>

/* NCT6687_VRM_PMBUS_INJECT — eSIO PMBus VRM (MSI MS-7D89 / addr 0xC0) */
/*
 * Default OFF. update_vrm runs AFTER update_lock is released (adaptive cache:
 * 1 Hz background; match VRM sysfs poll rate down to ~20 ms when demanded)
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

static int vrm_vout_exp = -10;
module_param(vrm_vout_exp, int, 0444);
MODULE_PARM_DESC(vrm_vout_exp, "Fallback LINEAR16 exp if VOUT_MODE unknown (-16..15)");

#include "nct6687_vrm_decode.h"
#include "nct6687_vrm_mailbox.h"

static void nct_vrm_data_reset(struct nct6687_data* data, bool enabled)
{
    data->vrm_enabled = enabled;
    data->vrm_demand = false;
    data->vrm_last_updated = 0;
    data->vrm_last_read = 0;
    data->vrm_read_gap = 0;
    data->vrm_fail_count = 0;
    data->vrm_smbus_page = -1;
    data->vrm_vout_mode_valid[0] = false;
    data->vrm_vout_mode_valid[1] = false;
    memset(data->vrm_rail, 0, sizeof(data->vrm_rail));
    data->vrm_mos_hist_init = false;
    data->vrm_mos_min = 0;
    data->vrm_mos_max = 0;
}

/* nct6687_temp_label[2] is "VRM MOS" → hwmon temp3. r257 stores only the
 * current reading; min/max here are software hist so sensors prints low/high.
 */
#define NCT_VRM_MOS_CH 2
#define NCT_VRM_MOS_NR 100

static bool nct_vrm_mos_present(void)
{
    return NCT6687_TEMP_ENABLED(NCT_VRM_MOS_CH) &&
        nct6687_temp_label[NCT_VRM_MOS_CH] &&
        !strcmp(nct6687_temp_label[NCT_VRM_MOS_CH], "VRM MOS");
}

static void nct_vrm_note_mos(struct nct6687_data* data)
{
    long t;

    if (!nct_vrm_mos_present())
        return;
    t = data->temperature[NCT_VRM_MOS_CH];
    nct_vrm_hist_point(&data->vrm_mos_min, &data->vrm_mos_max, t,
        !data->vrm_mos_hist_init);
    data->vrm_mos_hist_init = true;
}

static int nct_vrm_bus_recover(struct nct6687_data* data)
{
    nct_vrm_invalidate_smbus(data);
    return nct_vrm_recover(data);
}

static void nct6687_update_vrm(struct nct6687_data* data)
{
    struct nct_vrm_rail* cpu = &data->vrm_rail[0];
    struct nct_vrm_rail* gt = &data->vrm_rail[1];
    u8 cfg_save, baud_save;
    u8 addr;
    long sample[NCT_VRM_NMETRIC];
    unsigned long now, floor, gap, age;
    bool demanded, last_read_set;

    if (!data->vrm_enabled)
        return;

    /* EC temp was refreshed by update_device, or is the probe sample.
     * Demand reads re-apply that cached value; they do not invent a new one.
     */
    nct_vrm_note_mos(data);

    /*
     * Rate-limit even when invalid: a wedged VR/mux must not be hammered at
     * hwmon poll rate. Background / non-VRM paths: 1 Hz. VRM sysfs demand:
     * match inter-read gap down to ~20 ms. After failure: nct_vrm_fail_interval
     * (HZ/4, then HZ, 2*HZ, cap 8*HZ).
     * First demand after idle (or first-ever) uses the 20 ms floor — not 1 Hz
     * just because last_read is 0 or aged >= 1 s.
     */
    demanded = data->vrm_demand;
    data->vrm_demand = false;

    now = jiffies;
    floor = msecs_to_jiffies(20);
    if (!floor)
        floor = 1;

    last_read_set = data->vrm_last_read != 0;
    gap = last_read_set ? now - data->vrm_last_read : 0;
    age = data->vrm_last_updated ? now - data->vrm_last_updated : ~0UL;
    if (demanded)
        data->vrm_read_gap = gap;

    if (!nct_vrm_should_sample(cpu->valid, demanded, last_read_set, gap,
            age, HZ, floor, data->vrm_fail_count))
        return;

    addr = (u8)(vrm_addr & 0xff);
    cfg_save = 0;
    baud_save = 0;

    mutex_lock(&data->EC_io_lock);

    if (nct_vrm_esio_read(data, 4, 0x61, &cfg_save) || nct_vrm_esio_read(data, 4, 0x62, &baud_save)) {
        cpu->valid = false;
        gt->valid = false;
        data->vrm_last_updated = jiffies;
        data->vrm_fail_count++;
        if (nct_vrm_bus_recover(data))
            pr_warn_ratelimited("nct6687: VRM recover failed\n");
        mutex_unlock(&data->EC_io_lock);
        return;
    }

    if (nct_vrm_esio_write(data, 0x61, (cfg_save & ~0x03) | 0x00) || nct_vrm_esio_write(data, 0x62, 0x03) || nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN) || nct_vrm_sample_page(data, addr, 0, vrm_vout_exp, sample)) {
        cpu->valid = false;
        gt->valid = false;
        data->vrm_last_updated = jiffies;
        data->vrm_fail_count++;
        if (nct_vrm_bus_recover(data))
            pr_warn_ratelimited("nct6687: VRM recover failed\n");
        /* One mux restore. Ignore write errors — no retry on a wedged window. */
        nct_vrm_esio_write(data, 0x61, cfg_save);
        nct_vrm_esio_write(data, 0x62, baud_save);
        mutex_unlock(&data->EC_io_lock);
        return;
    }

    nct_vrm_rail_apply(cpu, sample);
    data->vrm_last_updated = jiffies;

    /* GT failure after a good CPU sample keeps CPU valid. */
    if (vrm_gt) {
        if (nct_vrm_sample_page(data, addr, 1, vrm_vout_exp, sample)) {
            gt->valid = false;
            if (nct_vrm_bus_recover(data))
                pr_warn_ratelimited("nct6687: VRM recover failed\n");
        } else {
            nct_vrm_rail_apply(gt, sample);
            nct_vrm_esio_write(data, 0x60, 0x00);
        }
    } else {
        gt->valid = false;
        nct_vrm_esio_write(data, 0x60, 0x00);
    }

    /* One mux restore. If it fails, drop the cache so fail-interval backoff
     * applies — do not retry the write on a wedged window. Reset the fail
     * count only after the PAGE sample and the mux restore both succeed.
     */
    if (nct_vrm_esio_write(data, 0x61, cfg_save) || nct_vrm_esio_write(data, 0x62, baud_save)) {
        cpu->valid = false;
        gt->valid = false;
        data->vrm_fail_count++;
    } else {
        data->vrm_fail_count = 0;
    }
    mutex_unlock(&data->EC_io_lock);
}

static struct nct6687_data* nct_vrm_touch_and_update(struct device* dev)
{
    struct nct6687_data* data = dev_get_drvdata(dev);

    data->vrm_demand = true;
    /* VRM sysfs must not go through nct6687_update_device: that refreshes
     * every fan/temp/volt channel under update_lock. HUD polling would
     * otherwise force a full EC scan ~1 Hz on top of the SMBus sample.
     * Background 1 Hz VRM still runs from the update_device hook when
     * other nct6687 attrs are read.
     * Stamp last_read after should_sample so first-ever / idle demand sees
     * last_read==0 or the real gap — not a synthetic HZ.
     */
    nct6687_update_vrm(data);
    data->vrm_last_read = jiffies;
    return data;
}

static ssize_t vrm_cpu_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct nct6687_data* data = nct_vrm_touch_and_update(dev);
    const struct nct_vrm_rail* cpu = &data->vrm_rail[0];

    if (!cpu->valid)
        return -ENODATA;
    return sprintf(buf, "%ld %ld %ld\n", cpu->val[NCT_VRM_VOUT],
        cpu->val[NCT_VRM_IOUT], cpu->val[NCT_VRM_POUT]);
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
    NCT_VRM_GT_EFF,
    NCT_VRM_GT_TEMP,
};

struct nct_vrm_chan {
    const char* label;
    u8 page;
    u8 metric; /* enum nct_vrm_metric */
};

static const struct nct_vrm_chan nct_vrm_chans[] = {
    [NCT_VRM_CPU_VOUT] = { "VRM CPU VOUT", 0, NCT_VRM_VOUT },
    [NCT_VRM_CPU_VIN] = { "VRM CPU VIN", 0, NCT_VRM_VIN },
    [NCT_VRM_CPU_IOUT] = { "VRM CPU IOUT", 0, NCT_VRM_IOUT },
    [NCT_VRM_CPU_IIN] = { "VRM CPU IIN", 0, NCT_VRM_IIN },
    [NCT_VRM_CPU_POUT] = { "VRM CPU POUT", 0, NCT_VRM_POUT },
    [NCT_VRM_CPU_PIN] = { "VRM CPU PIN", 0, NCT_VRM_PIN },
    [NCT_VRM_CPU_EFF] = { "VRM CPU Efficiency", 0, NCT_VRM_EFF },
    [NCT_VRM_CPU_TEMP] = { "VRM CPU TEMP", 0, NCT_VRM_TEMP },
    [NCT_VRM_GT_VOUT] = { "VRM GT VOUT", 1, NCT_VRM_VOUT },
    [NCT_VRM_GT_VIN] = { "VRM GT VIN", 1, NCT_VRM_VIN },
    [NCT_VRM_GT_IOUT] = { "VRM GT IOUT", 1, NCT_VRM_IOUT },
    [NCT_VRM_GT_IIN] = { "VRM GT IIN", 1, NCT_VRM_IIN },
    [NCT_VRM_GT_POUT] = { "VRM GT POUT", 1, NCT_VRM_POUT },
    [NCT_VRM_GT_PIN] = { "VRM GT PIN", 1, NCT_VRM_PIN },
    [NCT_VRM_GT_EFF] = { "VRM GT Efficiency", 1, NCT_VRM_EFF },
    [NCT_VRM_GT_TEMP] = { "VRM GT TEMP", 1, NCT_VRM_TEMP },
};

static ssize_t nct_vrm_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct sensor_device_attribute_2* sattr = to_sensor_dev_attr_2(attr);
    const struct nct_vrm_chan* ch;
    const struct nct_vrm_rail* r;
    struct nct6687_data* data;
    long val;

    if (sattr->nr >= ARRAY_SIZE(nct_vrm_chans))
        return -EINVAL;
    ch = &nct_vrm_chans[sattr->nr];

    if (sattr->index == NCT_VRM_LABEL)
        return sprintf(buf, "%s\n", ch->label);

    data = nct_vrm_touch_and_update(dev);
    r = &data->vrm_rail[ch->page];

    /* humidity*_input. sensors prints this as percent and appends %RH. */
    if (ch->metric == NCT_VRM_EFF) {
        if (sattr->index != NCT_VRM_INPUT)
            return -EINVAL;
        if (!r->valid)
            return -ENODATA;
        val = nct_vrm_efficiency_pcm(r->val[NCT_VRM_POUT], r->val[NCT_VRM_PIN]);
        if (val <= 0)
            return -ENODATA;
        return sprintf(buf, "%ld\n", val);
    }

    switch (sattr->index) {
    case NCT_VRM_INPUT:
        if (!r->valid)
            return -ENODATA;
        val = r->val[ch->metric];
        break;
    case NCT_VRM_MIN:
        if (!nct_vrm_rail_hist_ready(r, ch->metric))
            return -ENODATA;
        val = r->min[ch->metric];
        break;
    case NCT_VRM_MAX:
        if (!nct_vrm_rail_hist_ready(r, ch->metric))
            return -ENODATA;
        val = r->max[ch->metric];
        break;
    default:
        return -EINVAL;
    }
    return sprintf(buf, "%ld\n", val);
}

#define NCT_VRM_ATTR(_name, _ch, _stat) \
    static SENSOR_DEVICE_ATTR_2(_name, 0444, nct_vrm_show, NULL, _ch, _stat)

static SENSOR_DEVICE_ATTR_2(vrm_cpu, 0444, vrm_cpu_show, NULL,
    NCT_VRM_CPU_VOUT, NCT_VRM_INPUT);
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
NCT_VRM_ATTR(humidity1_input, NCT_VRM_CPU_EFF, NCT_VRM_INPUT);
NCT_VRM_ATTR(humidity1_label, NCT_VRM_CPU_EFF, NCT_VRM_LABEL);
NCT_VRM_ATTR(temp20_input, NCT_VRM_CPU_TEMP, NCT_VRM_INPUT);
NCT_VRM_ATTR(temp20_label, NCT_VRM_CPU_TEMP, NCT_VRM_LABEL);
NCT_VRM_ATTR(temp20_min, NCT_VRM_CPU_TEMP, NCT_VRM_MIN);
NCT_VRM_ATTR(temp20_max, NCT_VRM_CPU_TEMP, NCT_VRM_MAX);
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
NCT_VRM_ATTR(humidity2_input, NCT_VRM_GT_EFF, NCT_VRM_INPUT);
NCT_VRM_ATTR(humidity2_label, NCT_VRM_GT_EFF, NCT_VRM_LABEL);
NCT_VRM_ATTR(temp21_input, NCT_VRM_GT_TEMP, NCT_VRM_INPUT);
NCT_VRM_ATTR(temp21_label, NCT_VRM_GT_TEMP, NCT_VRM_LABEL);
NCT_VRM_ATTR(temp21_min, NCT_VRM_GT_TEMP, NCT_VRM_MIN);
NCT_VRM_ATTR(temp21_max, NCT_VRM_GT_TEMP, NCT_VRM_MAX);

static struct nct6687_data* nct6687_update_device(struct device* dev);

static ssize_t nct_vrm_mos_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct sensor_device_attribute_2* sattr = to_sensor_dev_attr_2(attr);
    struct nct6687_data* data = nct6687_update_device(dev);

    if (!data->vrm_mos_hist_init)
        return -ENODATA;
    return sprintf(buf, "%ld\n",
        sattr->index == NCT_VRM_MAX ? data->vrm_mos_max : data->vrm_mos_min);
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
    &sensor_dev_attr_humidity1_input.dev_attr.attr,
    &sensor_dev_attr_humidity1_label.dev_attr.attr,
    &sensor_dev_attr_temp20_input.dev_attr.attr,
    &sensor_dev_attr_temp20_label.dev_attr.attr,
    &sensor_dev_attr_temp20_min.dev_attr.attr,
    &sensor_dev_attr_temp20_max.dev_attr.attr,
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
    &sensor_dev_attr_humidity2_input.dev_attr.attr,
    &sensor_dev_attr_humidity2_label.dev_attr.attr,
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
