/* Included into nct6687.c by nct6687_vrm_dkms_inject.py — not a standalone TU.
 * Relies on stock types/macros (struct nct6687_data, EC_SPACE_*, SENSOR_DEVICE_ATTR).
 */
#include <linux/hwmon-sysfs.h>

/* NCT6687_VRM_PMBUS_INJECT — eSIO PMBus VRM (MSI MS-7D89 / addr 0xC0) */
/*
 * Default OFF. update_vrm runs AFTER update_lock is released (adaptive cache:
 * 1 Hz background; match VRM sysfs poll rate down to ~20 ms when demanded)
 * and holds only EC_io_lock — same lock as nct6687_read/write.
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
    data->vrm_valid = false;
    data->vrm_gt_valid = false;
    data->vrm_demand = false;
    data->vrm_last_updated = 0;
    data->vrm_last_read = 0;
    data->vrm_read_gap = 0;
    data->vrm_smbus_page = -1;
    data->vrm_vout_mode_valid[0] = false;
    data->vrm_vout_mode_valid[1] = false;
    data->vrm_hist_init = false;
    data->vrm_iout_hist_init = false;
    data->vrm_pout_hist_init = false;
    data->vrm_gt_hist_init = false;
    data->vrm_gt_iout_hist_init = false;
    data->vrm_gt_pout_hist_init = false;
}

static void nct_vrm_invalidate_smbus(struct nct6687_data* data)
{
    data->vrm_smbus_page = -1;
    data->vrm_vout_mode_valid[0] = false;
    data->vrm_vout_mode_valid[1] = false;
}

static void nct_vrm_bus_recover(struct nct6687_data* data)
{
    nct_vrm_invalidate_smbus(data);
    nct_vrm_recover(data);
}

/*
 * Production PAGE sample — source of truth for userspace read_vrm:
 *   PAGE write+readback (cache miss), VOUT_MODE 0x20 (cached),
 *   then VOUT 0x8B, POUT 0x96, VIN 0x88, TEMP 0x8D.
 *   IOUT 0x8C only when decoded VOUT <= 200 mV; otherwise IOUT = P/V.
 *   CAP 0x19 / STATUS 0x78 are not on this path (debug-only in userspace).
 */
static int nct_vrm_sample_page(struct nct6687_data* data, u8 addr, u8 page,
    long* vout_mv, long* vin_mv, long* iout_ma,
    long* pout_uw, long* temp_mc)
{
    u16 vout, iout = 0, pout, vin, temp;
    u8 vout_mode, page_r;
    long v_mv, p_mw, t_mc, i_ma;

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

    /* VOUT, POUT, VIN, TEMP — skip IOUT unless P/V is unusable. */
    if (nct_vrm_read_word(data, addr, 0x8b, &vout) || nct_vrm_read_word(data, addr, 0x96, &pout) || nct_vrm_read_word(data, addr, 0x88, &vin) || nct_vrm_read_word(data, addr, 0x8d, &temp)) {
        nct_vrm_invalidate_smbus(data);
        return -EIO;
    }

    v_mv = nct_vrm_decode_vout_mv(vout, vout_mode, vrm_vout_exp);
    p_mw = nct_vrm_linear11_milli(pout);
    t_mc = nct_vrm_linear11_milli(temp);
    if (v_mv > 200) {
        i_ma = nct_vrm_iout_from_pv_ma(p_mw, v_mv);
    } else {
        if (nct_vrm_read_word(data, addr, 0x8c, &iout)) {
            nct_vrm_invalidate_smbus(data);
            return -EIO;
        }
        i_ma = nct_vrm_decode_iout_fallback_ma(iout);
    }

    *vout_mv = v_mv;
    *vin_mv = nct_vrm_decode_vin_mv(vin);
    *iout_ma = i_ma;
    *pout_uw = p_mw * 1000L;
    *temp_mc = t_mc;
    return 0;
}

static void nct6687_update_vrm(struct nct6687_data* data)
{
    u8 cfg_save, baud_save;
    u8 addr;
    long vout_mv, vin_mv, iout_ma, pout_uw, temp_mc;
    unsigned long now, floor, interval;
    bool demanded;

    if (!data->vrm_enabled)
        return;

    /*
     * Rate-limit even when invalid: a wedged VR/mux must not be hammered at
     * hwmon poll rate. Background / non-VRM paths: 1 Hz. VRM sysfs demand:
     * match inter-read gap down to ~20 ms. After failure: retry at ~4 Hz.
     */
    demanded = data->vrm_demand;
    data->vrm_demand = false;

    now = jiffies;
    floor = msecs_to_jiffies(20);
    if (!floor)
        floor = 1;

    if (!data->vrm_valid)
        interval = HZ / 4;
    else if (demanded && data->vrm_read_gap < HZ)
        interval = max_t(unsigned long, data->vrm_read_gap, floor);
    else
        interval = HZ;

    if (data->vrm_last_updated && !time_after(now, data->vrm_last_updated + interval))
        return;

    addr = (u8)(vrm_addr & 0xff);
    cfg_save = 0;
    baud_save = 0;

    mutex_lock(&data->EC_io_lock);

    if (nct_vrm_esio_read(data, 4, 0x61, &cfg_save) || nct_vrm_esio_read(data, 4, 0x62, &baud_save)) {
        data->vrm_valid = false;
        data->vrm_gt_valid = false;
        data->vrm_last_updated = jiffies;
        nct_vrm_bus_recover(data);
        mutex_unlock(&data->EC_io_lock);
        return;
    }

    if (nct_vrm_esio_write(data, 0x61, (cfg_save & ~0x03) | 0x00) || nct_vrm_esio_write(data, 0x62, 0x03) || nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN) || nct_vrm_sample_page(data, addr, 0, &vout_mv, &vin_mv, &iout_ma, &pout_uw, &temp_mc)) {
        data->vrm_valid = false;
        data->vrm_gt_valid = false;
        data->vrm_last_updated = jiffies;
        nct_vrm_bus_recover(data);
        nct_vrm_esio_write(data, 0x61, cfg_save);
        nct_vrm_esio_write(data, 0x62, baud_save);
        mutex_unlock(&data->EC_io_lock);
        return;
    }

    data->vrm_vout = vout_mv;
    data->vrm_vin = vin_mv;
    data->vrm_iout = iout_ma;
    data->vrm_pout = pout_uw;
    data->vrm_temp = temp_mc;
    data->vrm_valid = true;
    data->vrm_last_updated = jiffies;
    {
        bool first = !data->vrm_hist_init;

        nct_vrm_hist_point(&data->vrm_vout_min, &data->vrm_vout_max, vout_mv, first);
        nct_vrm_hist_point(&data->vrm_vin_min, &data->vrm_vin_max, vin_mv, first);
        nct_vrm_hist_add(&data->vrm_iout_min, &data->vrm_iout_max,
            &data->vrm_iout_hist_init, iout_ma, true);
        nct_vrm_hist_add(&data->vrm_pout_min, &data->vrm_pout_max,
            &data->vrm_pout_hist_init, pout_uw, true);
        nct_vrm_hist_point(&data->vrm_temp_min, &data->vrm_temp_max, temp_mc, first);
        data->vrm_hist_init = true;
    }

    if (vrm_gt) {
        if (nct_vrm_sample_page(data, addr, 1, &vout_mv, &vin_mv, &iout_ma,
                &pout_uw, &temp_mc)) {
            data->vrm_gt_valid = false;
            nct_vrm_bus_recover(data);
        } else {
            bool first = !data->vrm_gt_hist_init;

            data->vrm_gt_vout = vout_mv;
            data->vrm_gt_vin = vin_mv;
            data->vrm_gt_iout = iout_ma;
            data->vrm_gt_pout = pout_uw;
            data->vrm_gt_temp = temp_mc;
            data->vrm_gt_valid = true;
            nct_vrm_hist_point(&data->vrm_gt_vout_min, &data->vrm_gt_vout_max, vout_mv, first);
            nct_vrm_hist_point(&data->vrm_gt_vin_min, &data->vrm_gt_vin_max, vin_mv, first);
            nct_vrm_hist_add(&data->vrm_gt_iout_min, &data->vrm_gt_iout_max,
                &data->vrm_gt_iout_hist_init, iout_ma, true);
            nct_vrm_hist_add(&data->vrm_gt_pout_min, &data->vrm_gt_pout_max,
                &data->vrm_gt_pout_hist_init, pout_uw, true);
            nct_vrm_hist_point(&data->vrm_gt_temp_min, &data->vrm_gt_temp_max, temp_mc, first);
            data->vrm_gt_hist_init = true;
            nct_vrm_esio_write(data, 0x60, 0x00);
        }
    } else {
        data->vrm_gt_valid = false;
        nct_vrm_esio_write(data, 0x60, 0x00);
    }

    nct_vrm_esio_write(data, 0x61, cfg_save);
    nct_vrm_esio_write(data, 0x62, baud_save);
    mutex_unlock(&data->EC_io_lock);
}

static struct nct6687_data* nct_vrm_touch_and_update(struct device* dev)
{
    struct nct6687_data* data = dev_get_drvdata(dev);
    unsigned long now = jiffies;

    data->vrm_read_gap = data->vrm_last_read ? now - data->vrm_last_read : HZ;
    data->vrm_last_read = now;
    data->vrm_demand = true;
    /* VRM sysfs must not go through nct6687_update_device: that refreshes
     * every fan/temp/volt channel under update_lock. HUD polling would
     * otherwise force a full EC scan ~1 Hz on top of the SMBus sample.
     * Background 1 Hz VRM still runs from the update_device hook when
     * other nct6687 attrs are read.
     */
    nct6687_update_vrm(data);
    return data;
}

static ssize_t vrm_cpu_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct nct6687_data* data = nct_vrm_touch_and_update(dev);

    if (!data->vrm_valid)
        return -ENODATA;
    return sprintf(buf, "%ld %ld %ld\n", data->vrm_vout, data->vrm_iout, data->vrm_pout);
}

/*
 * One row per hwmon channel. nct_vrm_show / is_visible key off
 * SENSOR_DEVICE_ATTR_2 nr=channel, index=stat. Labels live here so adding a
 * rail is a table row plus the attr declarations — not a new show function.
 */
#define NCT_VRM_F_GT 1

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
    NCT_VRM_CPU_POUT,
    NCT_VRM_CPU_TEMP,
    NCT_VRM_GT_VOUT,
    NCT_VRM_GT_VIN,
    NCT_VRM_GT_IOUT,
    NCT_VRM_GT_POUT,
    NCT_VRM_GT_TEMP,
};

struct nct_vrm_chan {
    const char* label;
    u8 flags;
    u16 value_off;
    u16 min_off;
    u16 max_off;
    u16 hist_init_off;
};

#define NCT_VRM_CHAN(_label, _flags, _field, _min, _max, _init) \
    { \
        .label = (_label), \
        .flags = (_flags), \
        .value_off = offsetof(struct nct6687_data, _field), \
        .min_off = offsetof(struct nct6687_data, _min), \
        .max_off = offsetof(struct nct6687_data, _max), \
        .hist_init_off = offsetof(struct nct6687_data, _init), \
    }

static const struct nct_vrm_chan nct_vrm_chans[] = {
    [NCT_VRM_CPU_VOUT] = NCT_VRM_CHAN("VRM CPU VOUT", 0,
        vrm_vout, vrm_vout_min, vrm_vout_max, vrm_hist_init),
    [NCT_VRM_CPU_VIN] = NCT_VRM_CHAN("VRM CPU VIN", 0,
        vrm_vin, vrm_vin_min, vrm_vin_max, vrm_hist_init),
    [NCT_VRM_CPU_IOUT] = NCT_VRM_CHAN("VRM CPU IOUT", 0,
        vrm_iout, vrm_iout_min, vrm_iout_max, vrm_iout_hist_init),
    [NCT_VRM_CPU_POUT] = NCT_VRM_CHAN("VRM CPU POUT", 0,
        vrm_pout, vrm_pout_min, vrm_pout_max, vrm_pout_hist_init),
    [NCT_VRM_CPU_TEMP] = NCT_VRM_CHAN("VRM CPU TEMP", 0,
        vrm_temp, vrm_temp_min, vrm_temp_max, vrm_hist_init),
    [NCT_VRM_GT_VOUT] = NCT_VRM_CHAN("VRM GT VOUT", NCT_VRM_F_GT,
        vrm_gt_vout, vrm_gt_vout_min, vrm_gt_vout_max, vrm_gt_hist_init),
    [NCT_VRM_GT_VIN] = NCT_VRM_CHAN("VRM GT VIN", NCT_VRM_F_GT,
        vrm_gt_vin, vrm_gt_vin_min, vrm_gt_vin_max, vrm_gt_hist_init),
    [NCT_VRM_GT_IOUT] = NCT_VRM_CHAN("VRM GT IOUT", NCT_VRM_F_GT,
        vrm_gt_iout, vrm_gt_iout_min, vrm_gt_iout_max, vrm_gt_iout_hist_init),
    [NCT_VRM_GT_POUT] = NCT_VRM_CHAN("VRM GT POUT", NCT_VRM_F_GT,
        vrm_gt_pout, vrm_gt_pout_min, vrm_gt_pout_max, vrm_gt_pout_hist_init),
    [NCT_VRM_GT_TEMP] = NCT_VRM_CHAN("VRM GT TEMP", NCT_VRM_F_GT,
        vrm_gt_temp, vrm_gt_temp_min, vrm_gt_temp_max, vrm_gt_hist_init),
};

static ssize_t nct_vrm_show(struct device* dev, struct device_attribute* attr, char* buf)
{
    struct sensor_device_attribute_2* sattr = to_sensor_dev_attr_2(attr);
    const struct nct_vrm_chan* ch;
    struct nct6687_data* data;
    long val;
    bool valid;

    if (sattr->nr >= ARRAY_SIZE(nct_vrm_chans))
        return -EINVAL;
    ch = &nct_vrm_chans[sattr->nr];

    if (sattr->index == NCT_VRM_LABEL)
        return sprintf(buf, "%s\n", ch->label);

    data = nct_vrm_touch_and_update(dev);

    switch (sattr->index) {
    case NCT_VRM_INPUT:
        valid = (ch->flags & NCT_VRM_F_GT) ? data->vrm_gt_valid : data->vrm_valid;
        if (!valid)
            return -ENODATA;
        val = *(long*)((char*)data + ch->value_off);
        break;
    case NCT_VRM_MIN:
        if (!*(bool*)((char*)data + ch->hist_init_off))
            return -ENODATA;
        val = *(long*)((char*)data + ch->min_off);
        break;
    case NCT_VRM_MAX:
        if (!*(bool*)((char*)data + ch->hist_init_off))
            return -ENODATA;
        val = *(long*)((char*)data + ch->max_off);
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
NCT_VRM_ATTR(power1_input, NCT_VRM_CPU_POUT, NCT_VRM_INPUT);
NCT_VRM_ATTR(power1_label, NCT_VRM_CPU_POUT, NCT_VRM_LABEL);
NCT_VRM_ATTR(power1_min, NCT_VRM_CPU_POUT, NCT_VRM_MIN);
NCT_VRM_ATTR(power1_max, NCT_VRM_CPU_POUT, NCT_VRM_MAX);
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
NCT_VRM_ATTR(power2_input, NCT_VRM_GT_POUT, NCT_VRM_INPUT);
NCT_VRM_ATTR(power2_label, NCT_VRM_GT_POUT, NCT_VRM_LABEL);
NCT_VRM_ATTR(power2_min, NCT_VRM_GT_POUT, NCT_VRM_MIN);
NCT_VRM_ATTR(power2_max, NCT_VRM_GT_POUT, NCT_VRM_MAX);
NCT_VRM_ATTR(temp21_input, NCT_VRM_GT_TEMP, NCT_VRM_INPUT);
NCT_VRM_ATTR(temp21_label, NCT_VRM_GT_TEMP, NCT_VRM_LABEL);
NCT_VRM_ATTR(temp21_min, NCT_VRM_GT_TEMP, NCT_VRM_MIN);
NCT_VRM_ATTR(temp21_max, NCT_VRM_GT_TEMP, NCT_VRM_MAX);


static umode_t nct6687_vrm_attr_is_visible(struct kobject* kobj,
    struct attribute* attr, int idx)
{
    struct sensor_device_attribute_2* sattr =
        container_of(attr, struct sensor_device_attribute_2, dev_attr.attr);

    (void)kobj;
    (void)idx;
    if (sattr->nr >= ARRAY_SIZE(nct_vrm_chans))
        return 0444;
    if ((nct_vrm_chans[sattr->nr].flags & NCT_VRM_F_GT) && !vrm_gt)
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
    &sensor_dev_attr_power1_input.dev_attr.attr,
    &sensor_dev_attr_power1_label.dev_attr.attr,
    &sensor_dev_attr_power1_min.dev_attr.attr,
    &sensor_dev_attr_power1_max.dev_attr.attr,
    &sensor_dev_attr_temp20_input.dev_attr.attr,
    &sensor_dev_attr_temp20_label.dev_attr.attr,
    &sensor_dev_attr_temp20_min.dev_attr.attr,
    &sensor_dev_attr_temp20_max.dev_attr.attr,
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
    &sensor_dev_attr_power2_input.dev_attr.attr,
    &sensor_dev_attr_power2_label.dev_attr.attr,
    &sensor_dev_attr_power2_min.dev_attr.attr,
    &sensor_dev_attr_power2_max.dev_attr.attr,
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
