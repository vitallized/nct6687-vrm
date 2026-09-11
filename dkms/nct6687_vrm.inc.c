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

#define NCT_VRM_SMB_EN 0x80
#define NCT_VRM_SMB_START 0x40
#define NCT_VRM_SMB_CLEAR 0x08
#define NCT_VRM_PROTO_WBR 0x02
#define NCT_VRM_PROTO_RB 0x82
#define NCT_VRM_PROTO_RW 0x83

static int nct_vrm_clamp_exp(int exp)
{
    if (exp < -16)
        return -16;
    if (exp > 15)
        return 15;
    return exp;
}

/*
 * Caller must hold data->EC_io_lock.
 * Stock nct6687_read/write leave PAGE != 0xff. Under EC_io_lock nothing else
 * can be mid-eSIO — force idle select; fail if PAGE never settles.
 */
static int nct_vrm_idle(struct nct6687_data* data)
{
    int i;

    if (inb_p(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET) == 0xff)
        return 0;
    outb_p(0xff, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
    for (i = 0; i < 10; i++) {
        if (inb_p(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET) == 0xff)
            return 0;
        udelay(100);
    }
    return -EBUSY;
}

static int nct_vrm_esio_write(struct nct6687_data* data, u8 index, u8 value)
{
    if (nct_vrm_idle(data))
        return -EBUSY;
    outb_p(0x04, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
    outb_p(index, data->addr + EC_SPACE_INDEX_REGISTER_OFFSET);
    outb_p(value, data->addr + EC_SPACE_DATA_REGISTER_OFFSET);
    outb_p(0xff, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
    return 0;
}

static int nct_vrm_esio_read(struct nct6687_data* data, u8 page, u8 index, u8* out)
{
    if (nct_vrm_idle(data))
        return -EBUSY;
    outb_p(page, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
    outb_p(index, data->addr + EC_SPACE_INDEX_REGISTER_OFFSET);
    *out = inb_p(data->addr + EC_SPACE_DATA_REGISTER_OFFSET);
    outb_p(0xff, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
    return 0;
}

static int nct_vrm_prep_clear(struct nct6687_data* data)
{
    u8 ctrl;

    if (nct_vrm_esio_write(data, 0x03, 0xff) || nct_vrm_esio_write(data, 0x04, 0xff) || nct_vrm_esio_read(data, 4, 0x60, &ctrl) || nct_vrm_esio_write(data, 0x60, (ctrl | NCT_VRM_SMB_CLEAR) & ~NCT_VRM_SMB_START) || nct_vrm_esio_write(data, 0x60, ctrl & ~(NCT_VRM_SMB_START | NCT_VRM_SMB_CLEAR)))
        return -EIO;
    return 0;
}

static int nct_vrm_wait_start_clear(struct nct6687_data* data)
{
    int i;
    u8 ctrl;

    for (i = 0; i < 100; i++) {
        if (nct_vrm_esio_read(data, 4, 0x60, &ctrl))
            return -EIO;
        if (!(ctrl & NCT_VRM_SMB_START))
            return 0;
        usleep_range(500, 1000);
    }
    return -ETIMEDOUT;
}

static void nct_vrm_invalidate_smbus(void);

static void nct_vrm_bus_recover(struct nct6687_data* data)
{
    nct_vrm_invalidate_smbus();
    nct_vrm_prep_clear(data);
    nct_vrm_esio_write(data, 0x60, 0x00);
}

static int nct_vrm_write_byte(struct nct6687_data* data, u8 addr, u8 cmd, u8 value)
{
    u8 sts;

    if (nct_vrm_prep_clear(data) || nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_WBR) || nct_vrm_esio_write(data, 0x65, addr) || nct_vrm_esio_write(data, 0x66, cmd) || nct_vrm_esio_write(data, 0x70, value) || nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
        return -EIO;
    usleep_range(500, 1000);
    if (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))
        return -EIO;
    if (nct_vrm_wait_start_clear(data))
        return -ETIMEDOUT;
    if (nct_vrm_esio_read(data, 4, 0x03, &sts))
        return -EIO;
    return sts ? -EIO : 0;
}

static int nct_vrm_read_byte(struct nct6687_data* data, u8 addr, u8 cmd, u8* out)
{
    u8 sts, lo;

    if (nct_vrm_prep_clear(data) || nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_RB) || nct_vrm_esio_write(data, 0x65, addr) || nct_vrm_esio_write(data, 0x66, cmd) || nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
        return -EIO;
    usleep_range(500, 1000);
    if (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))
        return -EIO;
    if (nct_vrm_wait_start_clear(data))
        return -ETIMEDOUT;
    if (nct_vrm_esio_read(data, 4, 0x03, &sts) || sts)
        return -EIO;
    if (nct_vrm_esio_read(data, 4, 0xb0, &lo))
        return -EIO;
    *out = lo;
    return 0;
}

static int nct_vrm_read_word(struct nct6687_data* data, u8 addr, u8 cmd, u16* out)
{
    u8 lo, hi, sts;

    if (nct_vrm_prep_clear(data) || nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_RW) || nct_vrm_esio_write(data, 0x65, addr) || nct_vrm_esio_write(data, 0x66, cmd) || nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
        return -EIO;
    usleep_range(500, 1000);
    if (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))
        return -EIO;
    if (nct_vrm_wait_start_clear(data))
        return -ETIMEDOUT;
    if (nct_vrm_esio_read(data, 4, 0x03, &sts) || sts)
        return -EIO;
    if (nct_vrm_esio_read(data, 4, 0xb0, &lo) || nct_vrm_esio_read(data, 4, 0xb1, &hi))
        return -EIO;
    *out = lo | (hi << 8);
    return 0;
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

static long nct_vrm_decode_vout_mv(u16 vout, u8 vout_mode)
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
        exp = vrm_vout_exp;
    }
    exp = nct_vrm_clamp_exp(exp);
    if (exp >= 0)
        return ((long)vout * 1000L) << exp;
    return ((long)vout * 1000L) >> (-exp);
}

/* Single-device caches (same assumption as the min/max hist below). */
static int vrm_smbus_page = -1;
static u8 vrm_vout_mode_cache[2];
static bool vrm_vout_mode_valid[2];

static void nct_vrm_invalidate_smbus(void)
{
    vrm_smbus_page = -1;
    vrm_vout_mode_valid[0] = false;
    vrm_vout_mode_valid[1] = false;
}

static int nct_vrm_sample_page(struct nct6687_data* data, u8 addr, u8 page,
    long* vout_mv, long* vin_mv, long* iout_ma,
    long* pout_uw, long* temp_mc)
{
    u16 vout, iout = 0, pout, vin, temp;
    u8 vout_mode, page_r;
    long v_mv, p_mw, t_mc, i_ma;

    if (page > 1)
        return -EINVAL;

    if (vrm_smbus_page != page) {
        if (nct_vrm_write_byte(data, addr, 0x00, page) || nct_vrm_read_byte(data, addr, 0x00, &page_r)) {
            nct_vrm_invalidate_smbus();
            return -EIO;
        }
        if (page_r != page) {
            nct_vrm_invalidate_smbus();
            return -EIO;
        }
        vrm_smbus_page = page;
    }

    if (!vrm_vout_mode_valid[page]) {
        if (nct_vrm_read_byte(data, addr, 0x20, &vout_mode)) {
            nct_vrm_invalidate_smbus();
            return -EIO;
        }
        vrm_vout_mode_cache[page] = vout_mode;
        vrm_vout_mode_valid[page] = true;
    } else {
        vout_mode = vrm_vout_mode_cache[page];
    }

    /* VOUT, POUT, VIN, TEMP — skip IOUT unless P/V is unusable. */
    if (nct_vrm_read_word(data, addr, 0x8b, &vout) || nct_vrm_read_word(data, addr, 0x96, &pout) || nct_vrm_read_word(data, addr, 0x88, &vin) || nct_vrm_read_word(data, addr, 0x8d, &temp)) {
        nct_vrm_invalidate_smbus();
        return -EIO;
    }

    v_mv = nct_vrm_decode_vout_mv(vout, vout_mode);
    p_mw = nct_vrm_linear11_milli(pout);
    t_mc = nct_vrm_linear11_milli(temp);
    if (v_mv > 200) {
        i_ma = (p_mw * 1000L) / v_mv;
    } else {
        if (nct_vrm_read_word(data, addr, 0x8c, &iout)) {
            nct_vrm_invalidate_smbus();
            return -EIO;
        }
        i_ma = ((long)iout * 1000L) >> 3;
    }

    *vout_mv = v_mv;
    *vin_mv = (long)vin * 10L;
    *iout_ma = i_ma;
    *pout_uw = p_mw * 1000L;
    *temp_mc = t_mc;
    return 0;
}

/* Software min/max since module load (same idea as stock nct6687 voltage[1]/[2]). */
static bool vrm_hist_init;
static long vrm_vout_min, vrm_vout_max;
static long vrm_vin_min, vrm_vin_max;
static long vrm_iout_min, vrm_iout_max;
static long vrm_pout_min, vrm_pout_max;
static long vrm_temp_min, vrm_temp_max;
static bool vrm_gt_hist_init;
static long vrm_gt_vout_min, vrm_gt_vout_max;
static long vrm_gt_vin_min, vrm_gt_vin_max;
static long vrm_gt_iout_min, vrm_gt_iout_max;
static long vrm_gt_pout_min, vrm_gt_pout_max;
static long vrm_gt_temp_min, vrm_gt_temp_max;

static void nct_vrm_hist_point(long* min, long* max, long val, bool first)
{
    if (first)
        *min = *max = val;
    else {
        if (val < *min)
            *min = val;
        if (val > *max)
            *max = val;
    }
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
        bool first = !vrm_hist_init;

        nct_vrm_hist_point(&vrm_vout_min, &vrm_vout_max, vout_mv, first);
        nct_vrm_hist_point(&vrm_vin_min, &vrm_vin_max, vin_mv, first);
        nct_vrm_hist_point(&vrm_iout_min, &vrm_iout_max, iout_ma, first);
        nct_vrm_hist_point(&vrm_pout_min, &vrm_pout_max, pout_uw, first);
        nct_vrm_hist_point(&vrm_temp_min, &vrm_temp_max, temp_mc, first);
        vrm_hist_init = true;
    }

    if (vrm_gt) {
        if (nct_vrm_sample_page(data, addr, 1, &vout_mv, &vin_mv, &iout_ma,
                &pout_uw, &temp_mc)) {
            data->vrm_gt_valid = false;
            nct_vrm_bus_recover(data);
        } else {
            bool first = !vrm_gt_hist_init;

            data->vrm_gt_vout = vout_mv;
            data->vrm_gt_vin = vin_mv;
            data->vrm_gt_iout = iout_ma;
            data->vrm_gt_pout = pout_uw;
            data->vrm_gt_temp = temp_mc;
            data->vrm_gt_valid = true;
            nct_vrm_hist_point(&vrm_gt_vout_min, &vrm_gt_vout_max, vout_mv, first);
            nct_vrm_hist_point(&vrm_gt_vin_min, &vrm_gt_vin_max, vin_mv, first);
            nct_vrm_hist_point(&vrm_gt_iout_min, &vrm_gt_iout_max, iout_ma, first);
            nct_vrm_hist_point(&vrm_gt_pout_min, &vrm_gt_pout_max, pout_uw, first);
            nct_vrm_hist_point(&vrm_gt_temp_min, &vrm_gt_temp_max, temp_mc, first);
            vrm_gt_hist_init = true;
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
    long* min;
    long* max;
    bool* hist_init;
};

#define NCT_VRM_CHAN(_label, _flags, _field, _min, _max, _init) \
    { \
        .label = (_label), \
        .flags = (_flags), \
        .value_off = offsetof(struct nct6687_data, _field), \
        .min = (_min), \
        .max = (_max), \
        .hist_init = (_init), \
    }

static const struct nct_vrm_chan nct_vrm_chans[] = {
    [NCT_VRM_CPU_VOUT] = NCT_VRM_CHAN("VRM CPU VOUT", 0,
        vrm_vout, &vrm_vout_min, &vrm_vout_max, &vrm_hist_init),
    [NCT_VRM_CPU_VIN] = NCT_VRM_CHAN("VRM CPU VIN", 0,
        vrm_vin, &vrm_vin_min, &vrm_vin_max, &vrm_hist_init),
    [NCT_VRM_CPU_IOUT] = NCT_VRM_CHAN("VRM CPU IOUT", 0,
        vrm_iout, &vrm_iout_min, &vrm_iout_max, &vrm_hist_init),
    [NCT_VRM_CPU_POUT] = NCT_VRM_CHAN("VRM CPU POUT", 0,
        vrm_pout, &vrm_pout_min, &vrm_pout_max, &vrm_hist_init),
    [NCT_VRM_CPU_TEMP] = NCT_VRM_CHAN("VRM CPU TEMP", 0,
        vrm_temp, &vrm_temp_min, &vrm_temp_max, &vrm_hist_init),
    [NCT_VRM_GT_VOUT] = NCT_VRM_CHAN("VRM GT VOUT", NCT_VRM_F_GT,
        vrm_gt_vout, &vrm_gt_vout_min, &vrm_gt_vout_max, &vrm_gt_hist_init),
    [NCT_VRM_GT_VIN] = NCT_VRM_CHAN("VRM GT VIN", NCT_VRM_F_GT,
        vrm_gt_vin, &vrm_gt_vin_min, &vrm_gt_vin_max, &vrm_gt_hist_init),
    [NCT_VRM_GT_IOUT] = NCT_VRM_CHAN("VRM GT IOUT", NCT_VRM_F_GT,
        vrm_gt_iout, &vrm_gt_iout_min, &vrm_gt_iout_max, &vrm_gt_hist_init),
    [NCT_VRM_GT_POUT] = NCT_VRM_CHAN("VRM GT POUT", NCT_VRM_F_GT,
        vrm_gt_pout, &vrm_gt_pout_min, &vrm_gt_pout_max, &vrm_gt_hist_init),
    [NCT_VRM_GT_TEMP] = NCT_VRM_CHAN("VRM GT TEMP", NCT_VRM_F_GT,
        vrm_gt_temp, &vrm_gt_temp_min, &vrm_gt_temp_max, &vrm_gt_hist_init),
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
        if (!*ch->hist_init)
            return -ENODATA;
        val = *ch->min;
        break;
    case NCT_VRM_MAX:
        if (!*ch->hist_init)
            return -ENODATA;
        val = *ch->max;
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
