/*
 * Host compile of dkms/nct6687_vrm_decode.h — no linux headers, no .ko.
 * pytest drives this against tests/golden_vrm_decode.json, and drives the
 * rail slot (nct_vrm_rail_apply) through rail_seq.
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
		"       vrm_decode decode_iout_ma RAW\n"
		"       vrm_decode decode_iin_ma RAW\n"
		"       vrm_decode decode_pin_uw RAW\n"
		"       vrm_decode efficiency_pcm POUT_UW PIN_UW\n"
		"       vrm_decode efficiency_pwm POUT_UW PIN_UW\n"
		"       vrm_decode iout_from_pv_ma P_MW V_MV\n"
		"       vrm_decode should_sample VALID DEMANDED LAST_READ_SET GAP AGE HZ FLOOR FAILS\n"
		"       vrm_decode fail_interval FAILS HZ\n"
		"       vrm_decode rail_seq VOUT,VIN,IOUT,POUT,TEMP,IIN,PIN...\n");
}

/* Apply each PAGE sample to one fresh rail slot; print the slot. */
static int rail_seq(int n, char** samples)
{
	static const char* const names[NCT_VRM_NMETRIC] = {
		"vout", "vin", "iout", "pout", "temp", "iin", "pin",
	};
	struct nct_vrm_rail r;
	long s[NCT_VRM_NMETRIC];
	char* p;
	int i, m;

	memset(&r, 0, sizeof(r));
	for (i = 0; i < n; i++) {
		p = samples[i];
		for (m = 0; m < NCT_VRM_NMETRIC; m++) {
			s[m] = strtol(p, &p, 0);
			if (*p != (m + 1 < NCT_VRM_NMETRIC ? ',' : '\0'))
				return -1;
			p++;
		}
		nct_vrm_rail_apply(&r, s);
	}
	printf("valid %d\n", r.valid ? 1 : 0);
	for (m = 0; m < NCT_VRM_NMETRIC; m++)
		printf("%s %ld %d %ld %ld\n", names[m], r.val[m],
			nct_vrm_rail_hist_ready(&r, m) ? 1 : 0, r.min[m], r.max[m]);
	return 0;
}

int main(int argc, char** argv)
{
	const char* fn;

	if (argc >= 2 && !strcmp(argv[1], "rail_seq")) {
		if (rail_seq(argc - 2, argv + 2)) {
			usage();
			return 2;
		}
		return 0;
	}
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
	if (!strcmp(fn, "decode_iout_ma") && argc == 3) {
		printf("%ld\n",
			nct_vrm_decode_iout_ma((u16)strtoul(argv[2], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "decode_iin_ma") && argc == 3) {
		printf("%ld\n",
			nct_vrm_decode_iin_ma((u16)strtoul(argv[2], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "decode_pin_uw") && argc == 3) {
		printf("%ld\n",
			nct_vrm_decode_pin_uw((u16)strtoul(argv[2], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "efficiency_pcm") && argc == 4) {
		printf("%ld\n",
			nct_vrm_efficiency_pcm(strtol(argv[2], NULL, 0),
				strtol(argv[3], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "efficiency_pwm") && argc == 4) {
		printf("%ld\n",
			nct_vrm_efficiency_pwm(strtol(argv[2], NULL, 0),
				strtol(argv[3], NULL, 0)));
		return 0;
	}
	if (!strcmp(fn, "iout_from_pv_ma") && argc == 4) {
		printf("%ld\n",
			nct_vrm_iout_from_pv_ma(strtol(argv[2], NULL, 0),
				strtol(argv[3], NULL, 0)));
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
