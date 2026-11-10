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
	/* A reader of more than vrm_cpu asked since the last full sample. */
	bool vrm_demand_full;
	/* jiffies of the last full PAGE sample (all words + controller), 0 = none. */
	unsigned long vrm_full_at;
	/* jiffies a full reader last asked (claimed at a sample), 0 = never. */
	unsigned long vrm_full_asked_at;
	unsigned long vrm_last_updated;
	unsigned long vrm_last_read;
	/* Efficiency uses these, not the latest POUT/PIN words. */
	bool vrm_eff_init;
	long vrm_eff_pout_uw;
	long vrm_eff_pin_uw;
	unsigned long vrm_eff_at_ms;
	/* Samples in the average since its seed (warm-up, NCT_VRM_EFF_MIN_N). */
	unsigned vrm_eff_n;
	unsigned long vrm_read_gap;
	unsigned vrm_fail_count;
	/* PAGE 1 only. A GT miss backs off without slowing the CPU rail. */
	unsigned vrm_gt_fails;
	unsigned long vrm_gt_fail_at;
	/* Pre-arm 0x61/0x62 kept when a checked restore does not land. */
	bool vrm_mux_hold;
	u8 vrm_mux_cfg;
	u8 vrm_mux_baud;
	/* This cycle left 0x61/0x62 as found (NCT_VRM_MUX_KEEP_FW). */
	bool vrm_mux_kept;
	/* Last 0x61/0x62 pair a save read; vrm_mux_seen once there is one. */
	bool vrm_mux_seen;
	u8 vrm_mux_seen_cfg;
	u8 vrm_mux_seen_baud;
	int vrm_smbus_page;
	u8 vrm_vout_mode_cache[2];
	bool vrm_vout_mode_valid[2];
	/* One rail slot per PAGE. Written only by nct_vrm_rail_apply. */
	struct nct_vrm_rail {
		bool valid;
		bool hist_init; /* VOUT, VIN: seeded together */
		bool temp_hist_init;
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
