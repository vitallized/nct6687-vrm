/* Included into nct6687.c by nct6687_vrm_dkms_inject.py — not a standalone TU.
 * Relies on stock types/macros (struct nct6687_data, EC_SPACE_*, SENSOR_DEVICE_ATTR).
 */

/* NCT6687_VRM_PMBUS_INJECT — eSIO PMBus VRM (MSI MS-7D89 / addr 0xC0) */
/*
 * Default OFF. update_vrm runs AFTER update_lock is released (own 1Hz cache)
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

#define NCT_VRM_SMB_EN    0x80
#define NCT_VRM_SMB_START 0x40
#define NCT_VRM_SMB_CLEAR 0x08
#define NCT_VRM_PROTO_WBR 0x02
#define NCT_VRM_PROTO_RB  0x82
#define NCT_VRM_PROTO_RW  0x83

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
static int nct_vrm_idle(struct nct6687_data *data)
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

static int nct_vrm_esio_write(struct nct6687_data *data, u8 index, u8 value)
{
	if (nct_vrm_idle(data))
		return -EBUSY;
	outb_p(0x04, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
	outb_p(index, data->addr + EC_SPACE_INDEX_REGISTER_OFFSET);
	outb_p(value, data->addr + EC_SPACE_DATA_REGISTER_OFFSET);
	outb_p(0xff, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
	return 0;
}

static int nct_vrm_esio_read(struct nct6687_data *data, u8 page, u8 index, u8 *out)
{
	if (nct_vrm_idle(data))
		return -EBUSY;
	outb_p(page, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
	outb_p(index, data->addr + EC_SPACE_INDEX_REGISTER_OFFSET);
	*out = inb_p(data->addr + EC_SPACE_DATA_REGISTER_OFFSET);
	outb_p(0xff, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);
	return 0;
}

static int nct_vrm_prep_clear(struct nct6687_data *data)
{
	u8 ctrl;

	if (nct_vrm_esio_write(data, 0x03, 0xff) ||
	    nct_vrm_esio_write(data, 0x04, 0xff) ||
	    nct_vrm_esio_read(data, 4, 0x60, &ctrl) ||
	    nct_vrm_esio_write(data, 0x60, (ctrl | NCT_VRM_SMB_CLEAR) & ~NCT_VRM_SMB_START) ||
	    nct_vrm_esio_write(data, 0x60, ctrl & ~(NCT_VRM_SMB_START | NCT_VRM_SMB_CLEAR)))
		return -EIO;
	return 0;
}

static int nct_vrm_wait_start_clear(struct nct6687_data *data)
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

static void nct_vrm_bus_recover(struct nct6687_data *data)
{
	nct_vrm_prep_clear(data);
	nct_vrm_esio_write(data, 0x60, 0x00);
}

static int nct_vrm_write_byte(struct nct6687_data *data, u8 addr, u8 cmd, u8 value)
{
	u8 sts;

	if (nct_vrm_prep_clear(data) ||
	    nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_WBR) ||
	    nct_vrm_esio_write(data, 0x65, addr) ||
	    nct_vrm_esio_write(data, 0x66, cmd) ||
	    nct_vrm_esio_write(data, 0x70, value) ||
	    nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
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

static int nct_vrm_read_byte(struct nct6687_data *data, u8 addr, u8 cmd, u8 *out)
{
	u8 sts, lo;

	if (nct_vrm_prep_clear(data) ||
	    nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_RB) ||
	    nct_vrm_esio_write(data, 0x65, addr) ||
	    nct_vrm_esio_write(data, 0x66, cmd) ||
	    nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
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

static int nct_vrm_read_word(struct nct6687_data *data, u8 addr, u8 cmd, u16 *out)
{
	u8 lo, hi, sts;

	if (nct_vrm_prep_clear(data) ||
	    nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_RW) ||
	    nct_vrm_esio_write(data, 0x65, addr) ||
	    nct_vrm_esio_write(data, 0x66, cmd) ||
	    nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))
		return -EIO;
	usleep_range(500, 1000);
	if (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))
		return -EIO;
	if (nct_vrm_wait_start_clear(data))
		return -ETIMEDOUT;
	if (nct_vrm_esio_read(data, 4, 0x03, &sts) || sts)
		return -EIO;
	if (nct_vrm_esio_read(data, 4, 0xb0, &lo) ||
	    nct_vrm_esio_read(data, 4, 0xb1, &hi))
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

static int nct_vrm_sample_page(struct nct6687_data *data, u8 addr, u8 page,
			       long *vout_mv, long *vin_mv, long *iout_ma,
			       long *pout_uw, long *temp_mc)
{
	u16 vout, iout, pout, vin, temp;
	u8 vout_mode, page_r;
	long v_mv, p_mw, t_mc, i_ma;

	if (nct_vrm_write_byte(data, addr, 0x00, page) ||
	    nct_vrm_read_byte(data, addr, 0x00, &page_r))
		return -EIO;
	if (page_r != page)
		return -EIO;
	if (nct_vrm_read_byte(data, addr, 0x20, &vout_mode) ||
	    nct_vrm_read_word(data, addr, 0x8b, &vout) ||
	    nct_vrm_read_word(data, addr, 0x8c, &iout) ||
	    nct_vrm_read_word(data, addr, 0x96, &pout) ||
	    nct_vrm_read_word(data, addr, 0x88, &vin) ||
	    nct_vrm_read_word(data, addr, 0x8d, &temp))
		return -EIO;

	v_mv = nct_vrm_decode_vout_mv(vout, vout_mode);
	p_mw = nct_vrm_linear11_milli(pout);
	t_mc = nct_vrm_linear11_milli(temp);
	if (v_mv > 200)
		i_ma = (p_mw * 1000L) / v_mv;
	else
		i_ma = ((long)iout * 1000L) >> 3;

	*vout_mv = v_mv;
	*vin_mv = (long)vin * 10L;
	*iout_ma = i_ma;
	*pout_uw = p_mw * 1000L;
	*temp_mc = t_mc;
	return 0;
}

static void nct6687_update_vrm(struct nct6687_data *data)
{
	u8 cfg_save, baud_save;
	u8 addr;
	long vout_mv, vin_mv, iout_ma, pout_uw, temp_mc;

	if (!data->vrm_enabled)
		return;

	if (data->vrm_valid &&
	    !time_after(jiffies, data->vrm_last_updated + HZ))
		return;

	addr = (u8)(vrm_addr & 0xff);
	cfg_save = 0;
	baud_save = 0;

	mutex_lock(&data->EC_io_lock);

	if (nct_vrm_esio_read(data, 4, 0x61, &cfg_save) ||
	    nct_vrm_esio_read(data, 4, 0x62, &baud_save)) {
		data->vrm_valid = false;
		data->vrm_gt_valid = false;
		nct_vrm_bus_recover(data);
		mutex_unlock(&data->EC_io_lock);
		return;
	}

	if (nct_vrm_esio_write(data, 0x61, (cfg_save & ~0x03) | 0x00) ||
	    nct_vrm_esio_write(data, 0x62, 0x03) ||
	    nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN) ||
	    nct_vrm_sample_page(data, addr, 0, &vout_mv, &vin_mv, &iout_ma,
				&pout_uw, &temp_mc)) {
		data->vrm_valid = false;
		data->vrm_gt_valid = false;
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

	if (vrm_gt) {
		if (nct_vrm_sample_page(data, addr, 1, &vout_mv, &vin_mv, &iout_ma,
					&pout_uw, &temp_mc)) {
			data->vrm_gt_valid = false;
			nct_vrm_bus_recover(data);
		} else {
			data->vrm_gt_vout = vout_mv;
			data->vrm_gt_vin = vin_mv;
			data->vrm_gt_iout = iout_ma;
			data->vrm_gt_pout = pout_uw;
			data->vrm_gt_temp = temp_mc;
			data->vrm_gt_valid = true;
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

static ssize_t show_vrm_vout(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_vout);
}

static ssize_t show_vrm_vin(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_vin);
}

static ssize_t show_vrm_iout(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_iout);
}

static ssize_t show_vrm_pout(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_pout);
}

static ssize_t show_vrm_temp(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_temp);
}

static ssize_t show_vrm_gt_vout(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_gt_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_gt_vout);
}

static ssize_t show_vrm_gt_vin(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_gt_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_gt_vin);
}

static ssize_t show_vrm_gt_iout(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_gt_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_gt_iout);
}

static ssize_t show_vrm_gt_pout(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_gt_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_gt_pout);
}

static ssize_t show_vrm_gt_temp(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct nct6687_data *data = nct6687_update_device(dev);

	if (!data->vrm_gt_valid)
		return -ENODATA;
	return sprintf(buf, "%ld\n", data->vrm_gt_temp);
}

static ssize_t show_vrm_label_vout(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM CPU VOUT\n");
}

static ssize_t show_vrm_label_vin(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM CPU VIN\n");
}

static ssize_t show_vrm_label_iout(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM CPU IOUT\n");
}

static ssize_t show_vrm_label_pout(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM CPU POUT\n");
}

static ssize_t show_vrm_label_temp(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM CPU TEMP\n");
}

static ssize_t show_vrm_label_gt_vout(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM GT VOUT\n");
}

static ssize_t show_vrm_label_gt_vin(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM GT VIN\n");
}

static ssize_t show_vrm_label_gt_iout(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM GT IOUT\n");
}

static ssize_t show_vrm_label_gt_pout(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM GT POUT\n");
}

static ssize_t show_vrm_label_gt_temp(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "VRM GT TEMP\n");
}

static SENSOR_DEVICE_ATTR(in20_input, 0444, show_vrm_vout, NULL, 0);
static SENSOR_DEVICE_ATTR(in20_label, 0444, show_vrm_label_vout, NULL, 0);
static SENSOR_DEVICE_ATTR(in21_input, 0444, show_vrm_vin, NULL, 0);
static SENSOR_DEVICE_ATTR(in21_label, 0444, show_vrm_label_vin, NULL, 0);
static SENSOR_DEVICE_ATTR(curr1_input, 0444, show_vrm_iout, NULL, 0);
static SENSOR_DEVICE_ATTR(curr1_label, 0444, show_vrm_label_iout, NULL, 0);
static SENSOR_DEVICE_ATTR(power1_input, 0444, show_vrm_pout, NULL, 0);
static SENSOR_DEVICE_ATTR(power1_label, 0444, show_vrm_label_pout, NULL, 0);
static SENSOR_DEVICE_ATTR(temp20_input, 0444, show_vrm_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(temp20_label, 0444, show_vrm_label_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(in22_input, 0444, show_vrm_gt_vout, NULL, 0);
static SENSOR_DEVICE_ATTR(in22_label, 0444, show_vrm_label_gt_vout, NULL, 0);
static SENSOR_DEVICE_ATTR(in23_input, 0444, show_vrm_gt_vin, NULL, 0);
static SENSOR_DEVICE_ATTR(in23_label, 0444, show_vrm_label_gt_vin, NULL, 0);
static SENSOR_DEVICE_ATTR(curr2_input, 0444, show_vrm_gt_iout, NULL, 0);
static SENSOR_DEVICE_ATTR(curr2_label, 0444, show_vrm_label_gt_iout, NULL, 0);
static SENSOR_DEVICE_ATTR(power2_input, 0444, show_vrm_gt_pout, NULL, 0);
static SENSOR_DEVICE_ATTR(power2_label, 0444, show_vrm_label_gt_pout, NULL, 0);
static SENSOR_DEVICE_ATTR(temp21_input, 0444, show_vrm_gt_temp, NULL, 0);
static SENSOR_DEVICE_ATTR(temp21_label, 0444, show_vrm_label_gt_temp, NULL, 0);

static umode_t nct6687_vrm_attr_is_visible(struct kobject *kobj,
					   struct attribute *attr, int idx)
{
	if (!vrm_gt &&
	    (attr == &sensor_dev_attr_in22_input.dev_attr.attr ||
	     attr == &sensor_dev_attr_in22_label.dev_attr.attr ||
	     attr == &sensor_dev_attr_in23_input.dev_attr.attr ||
	     attr == &sensor_dev_attr_in23_label.dev_attr.attr ||
	     attr == &sensor_dev_attr_curr2_input.dev_attr.attr ||
	     attr == &sensor_dev_attr_curr2_label.dev_attr.attr ||
	     attr == &sensor_dev_attr_power2_input.dev_attr.attr ||
	     attr == &sensor_dev_attr_power2_label.dev_attr.attr ||
	     attr == &sensor_dev_attr_temp21_input.dev_attr.attr ||
	     attr == &sensor_dev_attr_temp21_label.dev_attr.attr))
		return 0;
	return 0444;
}

static struct attribute *nct6687_vrm_attrs[] = {
	&sensor_dev_attr_in20_input.dev_attr.attr,
	&sensor_dev_attr_in20_label.dev_attr.attr,
	&sensor_dev_attr_in21_input.dev_attr.attr,
	&sensor_dev_attr_in21_label.dev_attr.attr,
	&sensor_dev_attr_curr1_input.dev_attr.attr,
	&sensor_dev_attr_curr1_label.dev_attr.attr,
	&sensor_dev_attr_power1_input.dev_attr.attr,
	&sensor_dev_attr_power1_label.dev_attr.attr,
	&sensor_dev_attr_temp20_input.dev_attr.attr,
	&sensor_dev_attr_temp20_label.dev_attr.attr,
	&sensor_dev_attr_in22_input.dev_attr.attr,
	&sensor_dev_attr_in22_label.dev_attr.attr,
	&sensor_dev_attr_in23_input.dev_attr.attr,
	&sensor_dev_attr_in23_label.dev_attr.attr,
	&sensor_dev_attr_curr2_input.dev_attr.attr,
	&sensor_dev_attr_curr2_label.dev_attr.attr,
	&sensor_dev_attr_power2_input.dev_attr.attr,
	&sensor_dev_attr_power2_label.dev_attr.attr,
	&sensor_dev_attr_temp21_input.dev_attr.attr,
	&sensor_dev_attr_temp21_label.dev_attr.attr,
	NULL,
};

static const struct attribute_group nct6687_vrm_group = {
	.attrs = nct6687_vrm_attrs,
	.is_visible = nct6687_vrm_attr_is_visible,
};
