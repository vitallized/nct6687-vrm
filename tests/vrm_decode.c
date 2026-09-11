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
		"       vrm_decode iout_from_pv_ma P_MW V_MV\n");
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
	usage();
	return 2;
}
