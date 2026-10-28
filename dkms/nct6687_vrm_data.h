/* Members only — spliced inside struct nct6687_data (see inject_text).
 * PAGE0=CPU, PAGE1=GT. Values are millisi. Do not include this file
 * except as a struct-body fragment.
 *
 * struct nct_vrm_rail is defined here, nested, because this fragment is the
 * first VRM code the compiler sees (before any VRM header). C gives a nested
 * struct tag file scope, so nct6687_vrm_decode.h can use it afterwards.
 */
#define NCT_VRM_NMETRIC 5 /* enum nct_vrm_metric in nct6687_vrm_decode.h */
	bool vrm_enabled;
	bool vrm_demand;
	unsigned long vrm_last_updated;
	unsigned long vrm_last_read;
	unsigned long vrm_read_gap;
	unsigned vrm_fail_count;
	int vrm_smbus_page;
	u8 vrm_vout_mode_cache[2];
	bool vrm_vout_mode_valid[2];
	/* One rail slot per PAGE. Written only by nct_vrm_rail_apply. */
	struct nct_vrm_rail {
		bool valid;
		bool hist_init; /* VOUT, VIN, TEMP: seeded together */
		bool iout_hist_init;
		bool pout_hist_init;
		long val[NCT_VRM_NMETRIC];
		long min[NCT_VRM_NMETRIC];
		long max[NCT_VRM_NMETRIC];
	} vrm_rail[2];
