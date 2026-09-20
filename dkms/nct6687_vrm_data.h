/* Members only — spliced inside struct nct6687_data (see inject_text).
 * PAGE0=CPU, PAGE1=GT. Values are millisi. Do not include this file
 * except as a struct-body fragment.
 */
	bool vrm_enabled;
	bool vrm_valid;
	bool vrm_gt_valid;
	bool vrm_demand;
	unsigned long vrm_last_updated;
	unsigned long vrm_last_read;
	unsigned long vrm_read_gap;
	long vrm_vout; /* mV */
	long vrm_vin;  /* mV */
	long vrm_iout; /* mA */
	long vrm_pout; /* uW */
	long vrm_temp; /* mC */
	long vrm_gt_vout;
	long vrm_gt_vin;
	long vrm_gt_iout;
	long vrm_gt_pout;
	long vrm_gt_temp;
	int vrm_smbus_page;
	u8 vrm_vout_mode_cache[2];
	bool vrm_vout_mode_valid[2];
	bool vrm_hist_init;
	bool vrm_iout_hist_init;
	bool vrm_pout_hist_init;
	long vrm_vout_min;
	long vrm_vout_max;
	long vrm_vin_min;
	long vrm_vin_max;
	long vrm_iout_min;
	long vrm_iout_max;
	long vrm_pout_min;
	long vrm_pout_max;
	long vrm_temp_min;
	long vrm_temp_max;
	bool vrm_gt_hist_init;
	bool vrm_gt_iout_hist_init;
	bool vrm_gt_pout_hist_init;
	long vrm_gt_vout_min;
	long vrm_gt_vout_max;
	long vrm_gt_vin_min;
	long vrm_gt_vin_max;
	long vrm_gt_iout_min;
	long vrm_gt_iout_max;
	long vrm_gt_pout_min;
	long vrm_gt_pout_max;
	long vrm_gt_temp_min;
	long vrm_gt_temp_max;
