#!/usr/bin/env python3
"""Inject eSIO PMBus VRM hwmon attrs into nct6687 DKMS sources, then rebuild.

Safety: --verify-compile; --install loads vrm=0; update_vrm outside update_lock;
GT hidden unless vrm_gt=1; modprobe -r must succeed.
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Substring match — the injected comment is longer than a bare /* ... */ line.
MARKER = "NCT6687_VRM_PMBUS_INJECT"

VRM_CODE = '\n/* NCT6687_VRM_PMBUS_INJECT — eSIO PMBus VRM (MSI MS-7D89 / addr 0xC0) */\n/*\n * Default OFF. update_vrm runs AFTER update_lock is released (own 1Hz cache)\n * and holds only EC_io_lock — same lock as nct6687_read/write.\n */\nstatic bool vrm;\nmodule_param(vrm, bool, 0444);\nMODULE_PARM_DESC(vrm, "Enable eSIO PMBus VRM sensors (default 0=off)");\n\nstatic bool vrm_gt;\nmodule_param(vrm_gt, bool, 0444);\nMODULE_PARM_DESC(vrm_gt, "Also sample PMBus PAGE 1 (GT/iGPU); default 0=CPU only");\n\nstatic int vrm_addr = 0xC0;\nmodule_param(vrm_addr, int, 0444);\nMODULE_PARM_DESC(vrm_addr, "PMBus 8-bit write address (default 0xC0)");\n\nstatic int vrm_vout_exp = -10;\nmodule_param(vrm_vout_exp, int, 0444);\nMODULE_PARM_DESC(vrm_vout_exp, "Fallback LINEAR16 exp if VOUT_MODE unknown (-16..15)");\n\n#define NCT_VRM_SMB_EN    0x80\n#define NCT_VRM_SMB_START 0x40\n#define NCT_VRM_SMB_CLEAR 0x08\n#define NCT_VRM_PROTO_WBR 0x02\n#define NCT_VRM_PROTO_RB  0x82\n#define NCT_VRM_PROTO_RW  0x83\n\nstatic int nct_vrm_clamp_exp(int exp)\n{\n\tif (exp < -16)\n\t\treturn -16;\n\tif (exp > 15)\n\t\treturn 15;\n\treturn exp;\n}\n\n/*\n * Caller must hold data->EC_io_lock.\n * Stock nct6687_read/write leave PAGE != 0xff. Under EC_io_lock nothing else\n * can be mid-eSIO — force idle select; fail if PAGE never settles.\n */\nstatic int nct_vrm_idle(struct nct6687_data *data)\n{\n\tint i;\n\n\tif (inb_p(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET) == 0xff)\n\t\treturn 0;\n\toutb_p(0xff, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);\n\tfor (i = 0; i < 10; i++) {\n\t\tif (inb_p(data->addr + EC_SPACE_PAGE_REGISTER_OFFSET) == 0xff)\n\t\t\treturn 0;\n\t\tudelay(100);\n\t}\n\treturn -EBUSY;\n}\n\nstatic int nct_vrm_esio_write(struct nct6687_data *data, u8 index, u8 value)\n{\n\tif (nct_vrm_idle(data))\n\t\treturn -EBUSY;\n\toutb_p(0x04, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);\n\toutb_p(index, data->addr + EC_SPACE_INDEX_REGISTER_OFFSET);\n\toutb_p(value, data->addr + EC_SPACE_DATA_REGISTER_OFFSET);\n\toutb_p(0xff, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);\n\treturn 0;\n}\n\nstatic int nct_vrm_esio_read(struct nct6687_data *data, u8 page, u8 index, u8 *out)\n{\n\tif (nct_vrm_idle(data))\n\t\treturn -EBUSY;\n\toutb_p(page, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);\n\toutb_p(index, data->addr + EC_SPACE_INDEX_REGISTER_OFFSET);\n\t*out = inb_p(data->addr + EC_SPACE_DATA_REGISTER_OFFSET);\n\toutb_p(0xff, data->addr + EC_SPACE_PAGE_REGISTER_OFFSET);\n\treturn 0;\n}\n\nstatic int nct_vrm_prep_clear(struct nct6687_data *data)\n{\n\tu8 ctrl;\n\n\tif (nct_vrm_esio_write(data, 0x03, 0xff) ||\n\t    nct_vrm_esio_write(data, 0x04, 0xff) ||\n\t    nct_vrm_esio_read(data, 4, 0x60, &ctrl) ||\n\t    nct_vrm_esio_write(data, 0x60, (ctrl | NCT_VRM_SMB_CLEAR) & ~NCT_VRM_SMB_START) ||\n\t    nct_vrm_esio_write(data, 0x60, ctrl & ~(NCT_VRM_SMB_START | NCT_VRM_SMB_CLEAR)))\n\t\treturn -EIO;\n\treturn 0;\n}\n\nstatic int nct_vrm_wait_start_clear(struct nct6687_data *data)\n{\n\tint i;\n\tu8 ctrl;\n\n\tfor (i = 0; i < 100; i++) {\n\t\tif (nct_vrm_esio_read(data, 4, 0x60, &ctrl))\n\t\t\treturn -EIO;\n\t\tif (!(ctrl & NCT_VRM_SMB_START))\n\t\t\treturn 0;\n\t\tusleep_range(500, 1000);\n\t}\n\treturn -ETIMEDOUT;\n}\n\nstatic void nct_vrm_bus_recover(struct nct6687_data *data)\n{\n\tnct_vrm_prep_clear(data);\n\tnct_vrm_esio_write(data, 0x60, 0x00);\n}\n\nstatic int nct_vrm_write_byte(struct nct6687_data *data, u8 addr, u8 cmd, u8 value)\n{\n\tu8 sts;\n\n\tif (nct_vrm_prep_clear(data) ||\n\t    nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_WBR) ||\n\t    nct_vrm_esio_write(data, 0x65, addr) ||\n\t    nct_vrm_esio_write(data, 0x66, cmd) ||\n\t    nct_vrm_esio_write(data, 0x70, value) ||\n\t    nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))\n\t\treturn -EIO;\n\tusleep_range(500, 1000);\n\tif (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))\n\t\treturn -EIO;\n\tif (nct_vrm_wait_start_clear(data))\n\t\treturn -ETIMEDOUT;\n\tif (nct_vrm_esio_read(data, 4, 0x03, &sts))\n\t\treturn -EIO;\n\treturn sts ? -EIO : 0;\n}\n\nstatic int nct_vrm_read_byte(struct nct6687_data *data, u8 addr, u8 cmd, u8 *out)\n{\n\tu8 sts, lo;\n\n\tif (nct_vrm_prep_clear(data) ||\n\t    nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_RB) ||\n\t    nct_vrm_esio_write(data, 0x65, addr) ||\n\t    nct_vrm_esio_write(data, 0x66, cmd) ||\n\t    nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))\n\t\treturn -EIO;\n\tusleep_range(500, 1000);\n\tif (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))\n\t\treturn -EIO;\n\tif (nct_vrm_wait_start_clear(data))\n\t\treturn -ETIMEDOUT;\n\tif (nct_vrm_esio_read(data, 4, 0x03, &sts) || sts)\n\t\treturn -EIO;\n\tif (nct_vrm_esio_read(data, 4, 0xb0, &lo))\n\t\treturn -EIO;\n\t*out = lo;\n\treturn 0;\n}\n\nstatic int nct_vrm_read_word(struct nct6687_data *data, u8 addr, u8 cmd, u16 *out)\n{\n\tu8 lo, hi, sts;\n\n\tif (nct_vrm_prep_clear(data) ||\n\t    nct_vrm_esio_write(data, 0x63, NCT_VRM_PROTO_RW) ||\n\t    nct_vrm_esio_write(data, 0x65, addr) ||\n\t    nct_vrm_esio_write(data, 0x66, cmd) ||\n\t    nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN))\n\t\treturn -EIO;\n\tusleep_range(500, 1000);\n\tif (nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN | NCT_VRM_SMB_START))\n\t\treturn -EIO;\n\tif (nct_vrm_wait_start_clear(data))\n\t\treturn -ETIMEDOUT;\n\tif (nct_vrm_esio_read(data, 4, 0x03, &sts) || sts)\n\t\treturn -EIO;\n\tif (nct_vrm_esio_read(data, 4, 0xb0, &lo) ||\n\t    nct_vrm_esio_read(data, 4, 0xb1, &hi))\n\t\treturn -EIO;\n\t*out = lo | (hi << 8);\n\treturn 0;\n}\n\nstatic long nct_vrm_linear11_milli(u16 raw)\n{\n\tint exp = (raw >> 11) & 0x1f;\n\tint mant = raw & 0x7ff;\n\tlong abs_m;\n\tbool neg;\n\n\tif (exp >= 16)\n\t\texp -= 32;\n\tif (mant >= 1024)\n\t\tmant -= 2048;\n\n\tneg = mant < 0;\n\tabs_m = (neg ? -(long)mant : (long)mant) * 1000L;\n\tif (exp >= 0) {\n\t\texp = nct_vrm_clamp_exp(exp);\n\t\tif (exp > 0)\n\t\t\tabs_m <<= exp;\n\t} else {\n\t\tabs_m >>= nct_vrm_clamp_exp(-exp);\n\t}\n\treturn neg ? -abs_m : abs_m;\n}\n\nstatic long nct_vrm_decode_vout_mv(u16 vout, u8 vout_mode)\n{\n\tint mode = (vout_mode >> 5) & 0x7;\n\tint exp;\n\n\tif (mode == 2)\n\t\treturn (long)vout; /* Direct R=3 → mV = raw */\n\n\tif (mode == 0) {\n\t\texp = vout_mode & 0x1f;\n\t\tif (exp >= 16)\n\t\t\texp -= 32;\n\t} else {\n\t\texp = vrm_vout_exp;\n\t}\n\texp = nct_vrm_clamp_exp(exp);\n\tif (exp >= 0)\n\t\treturn ((long)vout * 1000L) << exp;\n\treturn ((long)vout * 1000L) >> (-exp);\n}\n\nstatic int nct_vrm_sample_page(struct nct6687_data *data, u8 addr, u8 page,\n\t\t\t       long *vout_mv, long *vin_mv, long *iout_ma,\n\t\t\t       long *pout_uw, long *temp_mc)\n{\n\tu16 vout, iout, pout, vin, temp;\n\tu8 vout_mode, page_r;\n\tlong v_mv, p_mw, t_mc, i_ma;\n\n\tif (nct_vrm_write_byte(data, addr, 0x00, page) ||\n\t    nct_vrm_read_byte(data, addr, 0x00, &page_r))\n\t\treturn -EIO;\n\tif (page_r != page)\n\t\treturn -EIO;\n\tif (nct_vrm_read_byte(data, addr, 0x20, &vout_mode) ||\n\t    nct_vrm_read_word(data, addr, 0x8b, &vout) ||\n\t    nct_vrm_read_word(data, addr, 0x8c, &iout) ||\n\t    nct_vrm_read_word(data, addr, 0x96, &pout) ||\n\t    nct_vrm_read_word(data, addr, 0x88, &vin) ||\n\t    nct_vrm_read_word(data, addr, 0x8d, &temp))\n\t\treturn -EIO;\n\n\tv_mv = nct_vrm_decode_vout_mv(vout, vout_mode);\n\tp_mw = nct_vrm_linear11_milli(pout);\n\tt_mc = nct_vrm_linear11_milli(temp);\n\tif (v_mv > 200)\n\t\ti_ma = (p_mw * 1000L) / v_mv;\n\telse\n\t\ti_ma = ((long)iout * 1000L) >> 3;\n\n\t*vout_mv = v_mv;\n\t*vin_mv = (long)vin * 10L;\n\t*iout_ma = i_ma;\n\t*pout_uw = p_mw * 1000L;\n\t*temp_mc = t_mc;\n\treturn 0;\n}\n\nstatic void nct6687_update_vrm(struct nct6687_data *data)\n{\n\tu8 cfg_save, baud_save;\n\tu8 addr;\n\tlong vout_mv, vin_mv, iout_ma, pout_uw, temp_mc;\n\n\tif (!data->vrm_enabled)\n\t\treturn;\n\n\tif (data->vrm_valid &&\n\t    !time_after(jiffies, data->vrm_last_updated + HZ))\n\t\treturn;\n\n\taddr = (u8)(vrm_addr & 0xff);\n\tcfg_save = 0;\n\tbaud_save = 0;\n\n\tmutex_lock(&data->EC_io_lock);\n\n\tif (nct_vrm_esio_read(data, 4, 0x61, &cfg_save) ||\n\t    nct_vrm_esio_read(data, 4, 0x62, &baud_save)) {\n\t\tdata->vrm_valid = false;\n\t\tdata->vrm_gt_valid = false;\n\t\tnct_vrm_bus_recover(data);\n\t\tmutex_unlock(&data->EC_io_lock);\n\t\treturn;\n\t}\n\n\tif (nct_vrm_esio_write(data, 0x61, (cfg_save & ~0x03) | 0x00) ||\n\t    nct_vrm_esio_write(data, 0x62, 0x03) ||\n\t    nct_vrm_esio_write(data, 0x60, NCT_VRM_SMB_EN) ||\n\t    nct_vrm_sample_page(data, addr, 0, &vout_mv, &vin_mv, &iout_ma,\n\t\t\t\t&pout_uw, &temp_mc)) {\n\t\tdata->vrm_valid = false;\n\t\tdata->vrm_gt_valid = false;\n\t\tnct_vrm_bus_recover(data);\n\t\tnct_vrm_esio_write(data, 0x61, cfg_save);\n\t\tnct_vrm_esio_write(data, 0x62, baud_save);\n\t\tmutex_unlock(&data->EC_io_lock);\n\t\treturn;\n\t}\n\n\tdata->vrm_vout = vout_mv;\n\tdata->vrm_vin = vin_mv;\n\tdata->vrm_iout = iout_ma;\n\tdata->vrm_pout = pout_uw;\n\tdata->vrm_temp = temp_mc;\n\tdata->vrm_valid = true;\n\tdata->vrm_last_updated = jiffies;\n\n\tif (vrm_gt) {\n\t\tif (nct_vrm_sample_page(data, addr, 1, &vout_mv, &vin_mv, &iout_ma,\n\t\t\t\t\t&pout_uw, &temp_mc)) {\n\t\t\tdata->vrm_gt_valid = false;\n\t\t\tnct_vrm_bus_recover(data);\n\t\t} else {\n\t\t\tdata->vrm_gt_vout = vout_mv;\n\t\t\tdata->vrm_gt_vin = vin_mv;\n\t\t\tdata->vrm_gt_iout = iout_ma;\n\t\t\tdata->vrm_gt_pout = pout_uw;\n\t\t\tdata->vrm_gt_temp = temp_mc;\n\t\t\tdata->vrm_gt_valid = true;\n\t\t\tnct_vrm_esio_write(data, 0x60, 0x00);\n\t\t}\n\t} else {\n\t\tdata->vrm_gt_valid = false;\n\t\tnct_vrm_esio_write(data, 0x60, 0x00);\n\t}\n\n\tnct_vrm_esio_write(data, 0x61, cfg_save);\n\tnct_vrm_esio_write(data, 0x62, baud_save);\n\tmutex_unlock(&data->EC_io_lock);\n}\n\nstatic ssize_t show_vrm_vout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_vout);\n}\n\nstatic ssize_t show_vrm_vin(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_vin);\n}\n\nstatic ssize_t show_vrm_iout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_iout);\n}\n\nstatic ssize_t show_vrm_pout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_pout);\n}\n\nstatic ssize_t show_vrm_temp(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_temp);\n}\n\nstatic ssize_t show_vrm_gt_vout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_gt_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_gt_vout);\n}\n\nstatic ssize_t show_vrm_gt_vin(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_gt_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_gt_vin);\n}\n\nstatic ssize_t show_vrm_gt_iout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_gt_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_gt_iout);\n}\n\nstatic ssize_t show_vrm_gt_pout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_gt_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_gt_pout);\n}\n\nstatic ssize_t show_vrm_gt_temp(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\tstruct nct6687_data *data = nct6687_update_device(dev);\n\n\tif (!data->vrm_gt_valid)\n\t\treturn -ENODATA;\n\treturn sprintf(buf, "%ld\\n", data->vrm_gt_temp);\n}\n\nstatic ssize_t show_vrm_label_vout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM CPU VOUT\\n");\n}\n\nstatic ssize_t show_vrm_label_vin(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM CPU VIN\\n");\n}\n\nstatic ssize_t show_vrm_label_iout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM CPU IOUT\\n");\n}\n\nstatic ssize_t show_vrm_label_pout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM CPU POUT\\n");\n}\n\nstatic ssize_t show_vrm_label_temp(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM CPU TEMP\\n");\n}\n\nstatic ssize_t show_vrm_label_gt_vout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM GT VOUT\\n");\n}\n\nstatic ssize_t show_vrm_label_gt_vin(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM GT VIN\\n");\n}\n\nstatic ssize_t show_vrm_label_gt_iout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM GT IOUT\\n");\n}\n\nstatic ssize_t show_vrm_label_gt_pout(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM GT POUT\\n");\n}\n\nstatic ssize_t show_vrm_label_gt_temp(struct device *dev, struct device_attribute *attr, char *buf)\n{\n\treturn sprintf(buf, "VRM GT TEMP\\n");\n}\n\nstatic SENSOR_DEVICE_ATTR(in20_input, 0444, show_vrm_vout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(in20_label, 0444, show_vrm_label_vout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(in21_input, 0444, show_vrm_vin, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(in21_label, 0444, show_vrm_label_vin, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(curr1_input, 0444, show_vrm_iout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(curr1_label, 0444, show_vrm_label_iout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(power1_input, 0444, show_vrm_pout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(power1_label, 0444, show_vrm_label_pout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(temp20_input, 0444, show_vrm_temp, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(temp20_label, 0444, show_vrm_label_temp, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(in22_input, 0444, show_vrm_gt_vout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(in22_label, 0444, show_vrm_label_gt_vout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(in23_input, 0444, show_vrm_gt_vin, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(in23_label, 0444, show_vrm_label_gt_vin, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(curr2_input, 0444, show_vrm_gt_iout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(curr2_label, 0444, show_vrm_label_gt_iout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(power2_input, 0444, show_vrm_gt_pout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(power2_label, 0444, show_vrm_label_gt_pout, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(temp21_input, 0444, show_vrm_gt_temp, NULL, 0);\nstatic SENSOR_DEVICE_ATTR(temp21_label, 0444, show_vrm_label_gt_temp, NULL, 0);\n\nstatic umode_t nct6687_vrm_attr_is_visible(struct kobject *kobj,\n\t\t\t\t\t   struct attribute *attr, int idx)\n{\n\tif (!vrm_gt &&\n\t    (attr == &sensor_dev_attr_in22_input.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_in22_label.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_in23_input.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_in23_label.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_curr2_input.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_curr2_label.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_power2_input.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_power2_label.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_temp21_input.dev_attr.attr ||\n\t     attr == &sensor_dev_attr_temp21_label.dev_attr.attr))\n\t\treturn 0;\n\treturn 0444;\n}\n\nstatic struct attribute *nct6687_vrm_attrs[] = {\n\t&sensor_dev_attr_in20_input.dev_attr.attr,\n\t&sensor_dev_attr_in20_label.dev_attr.attr,\n\t&sensor_dev_attr_in21_input.dev_attr.attr,\n\t&sensor_dev_attr_in21_label.dev_attr.attr,\n\t&sensor_dev_attr_curr1_input.dev_attr.attr,\n\t&sensor_dev_attr_curr1_label.dev_attr.attr,\n\t&sensor_dev_attr_power1_input.dev_attr.attr,\n\t&sensor_dev_attr_power1_label.dev_attr.attr,\n\t&sensor_dev_attr_temp20_input.dev_attr.attr,\n\t&sensor_dev_attr_temp20_label.dev_attr.attr,\n\t&sensor_dev_attr_in22_input.dev_attr.attr,\n\t&sensor_dev_attr_in22_label.dev_attr.attr,\n\t&sensor_dev_attr_in23_input.dev_attr.attr,\n\t&sensor_dev_attr_in23_label.dev_attr.attr,\n\t&sensor_dev_attr_curr2_input.dev_attr.attr,\n\t&sensor_dev_attr_curr2_label.dev_attr.attr,\n\t&sensor_dev_attr_power2_input.dev_attr.attr,\n\t&sensor_dev_attr_power2_label.dev_attr.attr,\n\t&sensor_dev_attr_temp21_input.dev_attr.attr,\n\t&sensor_dev_attr_temp21_label.dev_attr.attr,\n\tNULL,\n};\n\nstatic const struct attribute_group nct6687_vrm_group = {\n\t.attrs = nct6687_vrm_attrs,\n\t.is_visible = nct6687_vrm_attr_is_visible,\n};\n'

STRUCT_FIELDS = '\n\t/* VRM PMBus (eSIO): PAGE0=CPU, PAGE1=GT */\n\tbool vrm_enabled;\n\tbool vrm_valid;\n\tbool vrm_gt_valid;\n\tunsigned long vrm_last_updated;\n\tlong vrm_vout; /* mV */\n\tlong vrm_vin;  /* mV */\n\tlong vrm_iout; /* mA */\n\tlong vrm_pout; /* uW */\n\tlong vrm_temp; /* mC */\n\tlong vrm_gt_vout;\n\tlong vrm_gt_vin;\n\tlong vrm_gt_iout;\n\tlong vrm_gt_pout;\n\tlong vrm_gt_temp;\n'

FORWARD_DECL = '\nstatic void nct6687_update_vrm(struct nct6687_data *data);\n'

PROBE_ENABLE = '\n\tdata->vrm_enabled = vrm;\n\tdata->vrm_last_updated = 0;\n\tif (data->vrm_enabled)\n\t\tdev_info(dev, "VRM PMBus eSIO sensors enabled (addr=0x%02x vout_exp=%d gt=%d)\\n",\n\t\t\t vrm_addr & 0xff, vrm_vout_exp, vrm_gt ? 1 : 0);\n\telse\n\t\tdev_info(dev, "VRM PMBus eSIO sensors built-in but disabled (modprobe nct6687 vrm=1)\\n");\n\n'

GROUP_INSERT = '\tif (data->vrm_enabled)\n\t\tdata->groups[groups++] = &nct6687_vrm_group;\n\n'


def find_src() -> Path:
    matches = sorted(glob.glob("/usr/src/nct6687d*/nct6687.c"))
    if not matches:
        raise SystemExit("No /usr/src/nct6687d*/nct6687.c found")
    return Path(matches[-1])


def parse_dkms(pkg_dir: Path) -> tuple[str, str]:
    conf = (pkg_dir / "dkms.conf").read_text()
    pname = pver = None
    for line in conf.splitlines():
        if line.startswith("PACKAGE_NAME="):
            pname = line.split("=", 1)[1].strip().strip('"')
        if line.startswith("PACKAGE_VERSION="):
            pver = line.split("=", 1)[1].strip().strip('"')
    if not pname or not pver:
        raise SystemExit("Could not parse dkms.conf")
    return pname, pver


def installed_kernels(pname: str, pver: str) -> list[str]:
    try:
        out = subprocess.check_output(["dkms", "status", "-m", pname, "-v", pver], text=True)
    except subprocess.CalledProcessError:
        return [os.uname().release]
    kvers = []
    for line in out.splitlines():
        m = re.search(r",\s*([^,]+),\s*\w+:\s*installed", line)
        if m:
            kvers.append(m.group(1).strip())
    return kvers or [os.uname().release]


def inject_text(text: str) -> str:
    if MARKER in text:
        raise SystemExit("Already injected (marker present)")

    if "#define IOREGION_LENGTH 4" not in text:
        raise SystemExit("IOREGION_LENGTH 4 not found — driver layout changed")
    text = text.replace("#define IOREGION_LENGTH 4", "#define IOREGION_LENGTH 8", 1)

    needle = "\tstruct mutex update_lock;"
    if needle not in text:
        raise SystemExit("struct field anchor not found")
    if "const struct attribute_group *groups[6]" not in text:
        raise SystemExit("groups[6] not found — cannot safely add VRM group")
    text = text.replace(needle, STRUCT_FIELDS + "\n" + needle, 1)

    upd_sig = "static struct nct6687_data *nct6687_update_device(struct device *dev)"
    if upd_sig not in text:
        raise SystemExit("nct6687_update_device signature not found")
    text = text.replace(upd_sig, FORWARD_DECL + "\n" + upd_sig, 1)

    anchor = "/*\n * Sysfs callback functions\n */"
    if anchor not in text:
        raise SystemExit("sysfs anchor not found")
    text = text.replace(anchor, VRM_CODE + "\n" + anchor, 1)

    upd_end = (
        "\t\tdata->last_updated = jiffies;\n"
        "\t\tdata->valid = true;\n"
        "\t}\n\n"
        "\tmutex_unlock(&data->update_lock);\n\n"
        "\treturn data;\n"
        "}"
    )
    upd_end_new = (
        "\t\tdata->last_updated = jiffies;\n"
        "\t\tdata->valid = true;\n"
        "\t}\n\n"
        "\tmutex_unlock(&data->update_lock);\n\n"
        "\t/* VRM: EC_io_lock only — do not hold update_lock across SMBus */\n"
        "\tnct6687_update_vrm(data);\n\n"
        "\treturn data;\n"
        "}"
    )
    if upd_end not in text:
        raise SystemExit("update_device end anchor not found")
    text = text.replace(upd_end, upd_end_new, 1)

    probe_anchor = "\tnct6687_setup_voltages(data);\n"
    if probe_anchor not in text:
        raise SystemExit("probe setup anchor not found")
    text = text.replace(probe_anchor, probe_anchor + PROBE_ENABLE, 1)

    idx = text.find("scnprintf(build, sizeof(build)")
    if idx < 0:
        raise SystemExit("probe group anchor (scnprintf build) not found")
    line_start = text.rfind("\n", 0, idx) + 1
    text = text[:line_start] + GROUP_INSERT + text[line_start:]
    return text


def inject(src: Path) -> None:
    text = src.read_text()
    if MARKER in text:
        print("Already injected:", src)
        return
    bak = Path(str(src) + ".pre-vrm")
    if not bak.exists():
        shutil.copy2(src, bak)
        print("Backup:", bak)
    src.write_text(inject_text(text))
    print("Patched", src)


def restore(src: Path) -> None:
    bak = Path(str(src) + ".pre-vrm")
    if not bak.exists():
        raise SystemExit(f"No backup {bak}")
    shutil.copy2(bak, src)
    print("Restored", src, "from", bak)


def verify_compile(src: Path) -> Path:
    pkg_dir = src.parent
    makefile = pkg_dir / "Makefile"
    if not makefile.exists():
        raise SystemExit(f"No Makefile in {pkg_dir}")
    kver = os.uname().release
    build_root = Path(__file__).resolve().parent / ".vrm-verify-build"
    if build_root.exists():
        shutil.rmtree(build_root)
    build_root.mkdir(parents=True)
    shutil.copy2(makefile, build_root / "Makefile")
    raw = src.read_text()
    (build_root / "nct6687.c").write_text(raw if MARKER in raw else inject_text(raw))
    print(f"Verify-compile in {build_root} for {kver}")
    subprocess.check_call(["make", f"TARGET={kver}", "build"], cwd=build_root)
    kos = list(build_root.rglob("nct6687.ko"))
    if not kos:
        raise SystemExit("nct6687.ko not found")
    print("OK: built", kos[0])
    return kos[0]


def rebuild(src: Path, reload: bool, load_vrm: bool = False) -> None:
    pkg_dir = src.parent
    pname, pver = parse_dkms(pkg_dir)
    kvers = installed_kernels(pname, pver)
    current = os.uname().release
    if current not in kvers:
        kvers.append(current)
    print(f"Rebuilding {pname}/{pver} for kernels: {', '.join(kvers)}")
    for kver in kvers:
        # install --force alone reuses stale builds; source was patched in-place
        print(f"--- dkms build -k {kver} --force ---")
        subprocess.check_call(["dkms", "build", "-m", pname, "-v", pver, "-k", kver, "--force"])
        print(f"--- dkms install -k {kver} --force ---")
        subprocess.check_call(["dkms", "install", "-m", pname, "-v", pver, "-k", kver, "--force"])
    if not reload:
        print("Skipped modprobe reload (--no-reload).")
        return
    vrm_arg = "vrm=1" if load_vrm else "vrm=0"
    print(f"Reloading nct6687 {vrm_arg}...")
    rc = subprocess.call(["modprobe", "-r", "nct6687"])
    if rc != 0:
        raise SystemExit(
            f"modprobe -r nct6687 failed (rc={rc}). "
            "Something still holds the module — close hwmon clients and retry. "
            "DKMS is built but the LIVE module was NOT replaced."
        )
    subprocess.check_call(["modprobe", "nct6687", vrm_arg])
    vrm_sys = Path("/sys/module/nct6687/parameters/vrm")
    if not vrm_sys.exists():
        raise SystemExit(
            "Reload finished but /sys/module/nct6687/parameters/vrm is missing — "
            "the live module is still the unpatched build. "
            "Run: sudo python3 nct6687_vrm_dkms_inject.py --rebuild"
        )
    print("Live module param vrm=" + vrm_sys.read_text().strip())
    if load_vrm:
        print("Loaded WITH VRM. Rollback: modprobe nct6687 vrm=0")
    else:
        print("Loaded with vrm=0. Enable later: modprobe -r nct6687 && modprobe nct6687 vrm=1")


def want_vrm_enabled(cli_enable: bool) -> bool:
    """CLI --enable-vrm wins; else honor /etc/modprobe.d/*nct6687* options."""
    if cli_enable:
        return True
    for path in sorted(Path("/etc/modprobe.d").glob("*.conf")):
        try:
            text = path.read_text()
        except OSError:
            continue
        for line in text.splitlines():
            s = line.split("#", 1)[0].strip()
            if not s.startswith("options"):
                continue
            parts = s.split()
            if len(parts) < 3 or parts[1] != "nct6687":
                continue
            for tok in parts[2:]:
                if tok in ("vrm=1", "vrm=Y", "vrm=y", "vrm=true"):
                    return True
                if tok in ("vrm=0", "vrm=N", "vrm=n", "vrm=false"):
                    return False
    return False


def reinject(src: Path, reload: bool, load_vrm: bool) -> None:
    """Pacman-hook path: patch wiped stock sources, force-rebuild DKMS."""
    if MARKER not in src.read_text():
        print("Re-injecting VRM patch into", src)
        inject(src)
    else:
        print("VRM patch already present in", src)
    rebuild(src, reload=reload, load_vrm=load_vrm)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--verify-compile", action="store_true")
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--restore", action="store_true")
    ap.add_argument("--rebuild", action="store_true")
    ap.add_argument(
        "--reinject",
        action="store_true",
        help="Re-apply patch after package upgrade (pacman hook); skips verify-compile",
    )
    ap.add_argument("--no-reload", action="store_true")
    ap.add_argument("--enable-vrm", action="store_true")
    ap.add_argument("--src", type=Path, default=None)
    args = ap.parse_args()
    if not any([args.verify_compile, args.install, args.restore, args.rebuild, args.reinject]):
        ap.print_help()
        print("\nRefusing bare run. Use --verify-compile first, then --install.", file=sys.stderr)
        return 2
    if args.verify_compile:
        verify_compile(args.src or find_src())
        print("\nCompile OK. Install with: sudo python3", Path(__file__).name, "--install")
        return 0
    if os.geteuid() != 0 and (args.install or args.restore or args.rebuild or args.reinject):
        print("Need root", file=sys.stderr)
        return 1
    src = args.src or find_src()
    load_vrm = want_vrm_enabled(args.enable_vrm)
    if args.restore:
        restore(src)
        if args.rebuild:
            rebuild(src, reload=not args.no_reload, load_vrm=False)
        return 0
    if args.reinject:
        # Prefer no live reload during pacman; modprobe.d applies on next load/boot.
        # Caller can omit --no-reload to force replace the running module.
        reinject(src, reload=not args.no_reload, load_vrm=load_vrm)
        return 0
    if args.install:
        if MARKER not in src.read_text():
            print("Step 1/3: verify-compile...")
            verify_compile(src)
            print("Step 2/3: inject...")
            inject(src)
        else:
            print("Already injected; rebuilding...")
        print("Step 3/3: dkms install...")
        rebuild(src, reload=not args.no_reload, load_vrm=load_vrm)
        return 0
    if args.rebuild:
        rebuild(src, reload=not args.no_reload, load_vrm=load_vrm)
        return 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
