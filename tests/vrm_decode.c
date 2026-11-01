/*
 * Host compile of dkms/nct6687_vrm_decode.h — no linux headers, no .ko.
 * pytest drives this against tests/golden_vrm_decode.json, and drives the
 * rail slot (nct_vrm_rail_apply) through rail_seq. attr_names prints the
 * published hwmon filenames from tests/vrm_attr_names.h.
 *
 *   cc -Wall -Werror -DNCT_VRM_DECODE_HOST -o vrm_decode tests/vrm_decode.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vrm_attr_names.h"

#define NCT_VRM_DECODE_HOST 1
#include "../dkms/nct6687_vrm_hist.h"

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
		"       vrm_decode rail_seq VOUT,VIN,IOUT,POUT,TEMP,IIN,PIN...\n"
		"       vrm_decode mos_seq MILLI_C...\n"
		"       vrm_decode mos_milli WHOLE FRAC\n"
		"       vrm_decode publish_hist LIVE READY\n"
		"       vrm_decode ctrl_seq MILLI_C...\n"
		"       vrm_decode attr_names\n");
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

	if (argc == 2 && !strcmp(argv[1], "attr_names")) {
		const char* const* name;

		for (name = nct_vrm_published_attr_names; *name; name++)
			printf("%s\n", *name);
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "publish_hist")) {
		printf("%d\n", nct_vrm_publish_hist(atoi(argv[2]), atoi(argv[3])) ? 1 : 0);
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "mos_milli")) {
		printf("%ld\n", nct_vrm_mos_milli((unsigned)strtoul(argv[2], NULL, 0),
			(unsigned)strtoul(argv[3], NULL, 0)));
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "ctrl_seq")) {
		bool valid = false, init = false;
		long cur = 0, min = 0, max = 0;
		int i;

		for (i = 2; i < argc; i++)
			nct_vrm_ctrl_note(&valid, &init, &cur, &min, &max,
				strtol(argv[i], NULL, 0));
		printf("valid %d\ncur %ld\nmin %ld\nmax %ld\n",
			valid ? 1 : 0, cur, min, max);
		return 0;
	}
	if (argc >= 2 && !strcmp(argv[1], "mos_seq")) {
		bool init = false;
		long min = 0, max = 0;
		int i;

		for (i = 2; i < argc; i++)
			nct_vrm_mos_note(&init, &min, &max, strtol(argv[i], NULL, 0));
		printf("init %d\nmin %ld\nmax %ld\n",
			nct_vrm_mos_ready(init) ? 1 : 0,
			nct_vrm_mos_value(min, max, false),
			nct_vrm_mos_value(min, max, true));
		return 0;
	}
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
	usage();
	return 2;
}
