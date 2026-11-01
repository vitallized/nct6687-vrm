/* Members only — spliced inside struct nct6687_data (see inject_text).
 * PAGE0=CPU, PAGE1=GT. Values are millisi. Do not include this file
 * except as a struct-body fragment.
 *
 * struct nct_vrm_rail is defined here, nested, because this fragment is the
 * first VRM code the compiler sees (before any VRM header). C gives a nested
 * struct tag file scope, so nct6687_vrm_decode.h can use it afterwards.
 */
#define NCT_VRM_NMETRIC 7 /* enum nct_vrm_metric in nct6687_vrm_decode.h */
	bool vrm_enabled;
	bool vrm_demand;
	unsigned long vrm_last_updated;
	unsigned long vrm_last_read;
	unsigned long vrm_read_gap;
	unsigned vrm_fail_count;
	/* PAGE 1 only. A GT miss backs off without slowing the CPU rail. */
	unsigned vrm_gt_fails;
	unsigned long vrm_gt_fail_at;
	/* Pre-arm 0x61/0x62 kept when a checked restore does not land. */
	bool vrm_mux_hold;
	u8 vrm_mux_cfg;
	u8 vrm_mux_baud;
	int vrm_smbus_page;
	u8 vrm_vout_mode_cache[2];
	bool vrm_vout_mode_valid[2];
	/* One rail slot per PAGE. Written only by nct_vrm_rail_apply. */
	struct nct_vrm_rail {
		bool valid;
		bool hist_init; /* VOUT, VIN, TEMP: seeded together */
		bool iout_hist_init;
		bool pout_hist_init;
		bool iin_hist_init;
		bool pin_hist_init;
		long val[NCT_VRM_NMETRIC];
		long min[NCT_VRM_NMETRIC];
		long max[NCT_VRM_NMETRIC];
	} vrm_rail[2];
	/* Stock EC thermistor "VRM MOS" (temp3). Not a PAGE sample. */
	bool vrm_mos_hist_init;
	unsigned long vrm_mos_read_at;
	long vrm_mos_min;
	long vrm_mos_max;
	/* READ_TEMPERATURE_2 (0x8E). Global controller die, not a PAGE slot. */
	bool vrm_ctrl_valid;
	bool vrm_ctrl_hist_init;
	long vrm_ctrl_mc;
	long vrm_ctrl_min;
	long vrm_ctrl_max;
