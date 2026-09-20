/*
 * Host compile of dkms/nct6687_vrm_decode.h — no linux headers, no .ko.
 * pytest drives this against tests/golden_vrm_decode.json.
 *
 *   cc -Wall -Werror -DNCT_VRM_DECODE_HOST -o vrm_decode tests/vrm_decode.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCT_VRM_DECODE_HOST 1
#include "../dkms/nct6687_vrm_decode.h"

static void usage(void)
{
	fprintf(stderr,
		"usage: vrm_decode clamp_exp EXP\n"
		"       vrm_decode linear11_milli RAW\n"
		"       vrm_decode decode_vout_mv RAW VOUT_MODE FALLBACK_EXP\n"
		"       vrm_decode decode_vin_mv RAW\n"
		"       vrm_decode decode_iout_fallback_ma RAW\n"
		"       vrm_decode iout_from_pv_ma P_MW V_MV\n"
		"       vrm_decode hist_seq nonneg|always VAL...\n"
		"       vrm_decode should_sample VALID DEMANDED LAST_READ_SET GAP AGE HZ FLOOR FAILS\n"
		"       vrm_decode fail_interval FAILS HZ\n");
}

int main(int argc, char** argv)
{
	const char* fn;

	if (argc < 3) {
		usage();
		return 2;
	}
	fn = argv[1];
	if (!strcmp(fn, "clamp_exp") && argc == 3) {
		printf("%d\n", nct_vrm_clamp_exp((int)strtol(argv[2], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "linear11_milli") && argc == 3) {
		printf("%ld\n", nct_vrm_linear11_milli((u16)strtoul(argv[2], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "decode_vout_mv") && argc == 5) {
		printf("%ld\n",
			nct_vrm_decode_vout_mv((u16)strtoul(argv[2], NULL, 0),
				(u8)strtoul(argv[3], NULL, 0),
				(int)strtol(argv[4], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "decode_vin_mv") && argc == 3) {
		printf("%ld\n", nct_vrm_decode_vin_mv((u16)strtoul(argv[2], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "decode_iout_fallback_ma") && argc == 3) {
		printf("%ld\n",
			nct_vrm_decode_iout_fallback_ma((u16)strtoul(argv[2], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "iout_from_pv_ma") && argc == 4) {
		printf("%ld\n",
			nct_vrm_iout_from_pv_ma(strtol(argv[2], NULL, 0),
				strtol(argv[3], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "hist_seq") && argc >= 4) {
		bool reject_neg;
		bool init = false;
		long mn = 0;
		long mx = 0;
		int i;

		if (!strcmp(argv[2], "nonneg"))
			reject_neg = true;
		else if (!strcmp(argv[2], "always"))
			reject_neg = false;
		else {
			usage();
			return 2;
		}
		for (i = 3; i < argc; i++)
			nct_vrm_hist_add(&mn, &mx, &init, strtol(argv[i], NULL, 0),
				reject_neg);
		printf("%d %ld %ld\n", init ? 1 : 0, mn, mx);
		return 0;
	}
	if (!strcmp(fn, "fail_interval") && argc == 4) {
		printf("%lu\n",
			nct_vrm_fail_interval((unsigned)strtoul(argv[2], NULL, 0),
				strtoul(argv[3], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "should_sample") && argc == 10) {
		bool sample = nct_vrm_should_sample(
			!!strtol(argv[2], NULL, 0),
			!!strtol(argv[3], NULL, 0),
			!!strtol(argv[4], NULL, 0),
			strtoul(argv[5], NULL, 0),
			strtoul(argv[6], NULL, 0),
			strtoul(argv[7], NULL, 0),
			strtoul(argv[8], NULL, 0),
			(unsigned)strtoul(argv[9], NULL, 0));

		printf("%d\n", sample ? 1 : 0);
		return 0;
	}
	usage();
	return 2;
}
