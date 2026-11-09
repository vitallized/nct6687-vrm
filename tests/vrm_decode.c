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

static int nct_vrm_neg_notes;

static void nct_vrm_note_negative(int metric, long val)
{
	(void)metric;
	(void)val;
	nct_vrm_neg_notes++;
}

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
		"       vrm_decode cpu_full VOUT VIN IOUT IIN POUT PIN\n"
		"       vrm_decode eff_converge\n"
		"       vrm_decode eff_publish POUT_UW PIN_UW\n"
		"       vrm_decode eff_reset\n"
		"       vrm_decode eff_hold\n"
		"       vrm_decode sweep_rigor\n"
		"       vrm_decode ctrl_seq MILLI_C...\n"
		"       vrm_decode attr_names\n");
}

/* Apply each PAGE sample to one fresh rail slot; print the slot. */
static int sweep_fail(const char* what, unsigned raw, long got, long expect)
{
	printf("fail %s raw %u got %ld expect %ld\n", what, raw, got, expect);
	return 1;
}

/* Every u16. Direct words must be two's complement, and a negative millisi
 * sample must not move the live value or the high.
 */
static int sweep_rigor(void)
{
	static const long scale[NCT_VRM_NMETRIC] = {
		[NCT_VRM_IOUT] = 100L,
		[NCT_VRM_IIN] = 10L,
		[NCT_VRM_PIN] = 1000000L,
	};
	unsigned raw;
	long i16, got, dec;
	long sample[NCT_VRM_NMETRIC];
	struct nct_vrm_rail rail;
	int metric;
	int publish_checks = 0;

	for (raw = 0; raw < 65536; raw++) {
		i16 = (raw & 0x8000) ? (long)raw - 65536L : (long)raw;
		got = nct_vrm_decode_iout_ma((u16)raw);
		if (got != i16 * 100L)
			return sweep_fail("iout", raw, got, i16 * 100L);
		got = nct_vrm_decode_iin_ma((u16)raw);
		if (got != i16 * 10L)
			return sweep_fail("iin", raw, got, i16 * 10L);
		got = nct_vrm_decode_pin_uw((u16)raw);
		if (got != i16 * 1000000L)
			return sweep_fail("pin", raw, got, i16 * 1000000L);
		if ((raw & 0x8000) && (nct_vrm_decode_iout_ma((u16)raw) >= 0 ||
				nct_vrm_decode_iin_ma((u16)raw) >= 0 ||
				nct_vrm_decode_pin_uw((u16)raw) >= 0))
			return sweep_fail("sign", raw, 0, -1);
	}
	printf("decode_ok 196608\n");
	printf("witness iout_fff6 %ld\n", nct_vrm_decode_iout_ma(0xFFF6));
	printf("witness iin_ffde %ld\n", nct_vrm_decode_iin_ma(0xFFDE));
	printf("witness pin_ffff %ld\n", nct_vrm_decode_pin_uw(0xFFFF));
	printf("pin_bucket");
	for (raw = 65525; raw <= 65535; raw++)
		printf(" %ld", nct_vrm_decode_pin_uw((u16)raw));
	printf("\n");
	printf("witness pout_ffff %ld\n", nct_vrm_linear11_milli(0xFFFF) * 1000L);

	memset(&rail, 0, sizeof(rail));
	nct_vrm_neg_notes = 0;
	sample[NCT_VRM_VOUT] = 1260;
	sample[NCT_VRM_VIN] = 12130;
	sample[NCT_VRM_IOUT] = 22900;
	sample[NCT_VRM_POUT] = 32000000L;
	sample[NCT_VRM_TEMP] = 43000;
	sample[NCT_VRM_IIN] = 1830;
	sample[NCT_VRM_PIN] = 33000000L;
	nct_vrm_rail_apply(&rail, sample);
	sample[NCT_VRM_IOUT] = nct_vrm_decode_iout_ma(0xFFF6);
	sample[NCT_VRM_IIN] = nct_vrm_decode_iin_ma(0xFFDE);
	sample[NCT_VRM_POUT] = nct_vrm_linear11_milli(0xFFFF) * 1000L;
	sample[NCT_VRM_PIN] = nct_vrm_decode_pin_uw(0xFFFF);
	nct_vrm_rail_apply(&rail, sample);
	if (rail.val[NCT_VRM_IOUT] != 22900 || rail.max[NCT_VRM_IOUT] != 22900 ||
		rail.val[NCT_VRM_IIN] != 1830 || rail.max[NCT_VRM_IIN] != 1830 ||
		rail.val[NCT_VRM_POUT] != 32000000L || rail.max[NCT_VRM_POUT] != 32000000L ||
		rail.val[NCT_VRM_PIN] != 33000000L || rail.max[NCT_VRM_PIN] != 33000000L ||
		nct_vrm_neg_notes != 4)
		return sweep_fail("held", 0xFFFF, rail.val[NCT_VRM_PIN], 33000000L);
	printf("held %ld %ld %ld %ld %d\n",
		rail.val[NCT_VRM_IOUT], rail.val[NCT_VRM_IIN],
		rail.val[NCT_VRM_POUT], rail.val[NCT_VRM_PIN], nct_vrm_neg_notes);

	for (metric = 0; metric < NCT_VRM_NMETRIC; metric++) {
		long (*decode)(u16);
		long expect_min = 0, expect_max = 0;
		int expect_notes = 0;
		bool have = false;
		const char* name = "?";

		if (metric == NCT_VRM_IOUT) {
			decode = nct_vrm_decode_iout_ma;
			name = "iout";
		} else if (metric == NCT_VRM_IIN) {
			decode = nct_vrm_decode_iin_ma;
			name = "iin";
		} else if (metric == NCT_VRM_PIN) {
			decode = nct_vrm_decode_pin_uw;
			name = "pin";
		} else if (metric == NCT_VRM_POUT) {
			decode = NULL;
			name = "pout";
		} else {
			continue;
		}

		memset(&rail, 0, sizeof(rail));
		nct_vrm_neg_notes = 0;
		for (raw = 0; raw < 65536; raw++) {
			long prev, prev_min, prev_max;
			int prev_notes, was_ready;

			if (metric == NCT_VRM_POUT)
				dec = nct_vrm_linear11_milli((u16)raw) * 1000L;
			else
				dec = decode((u16)raw);
			sample[NCT_VRM_VOUT] = 1260;
			sample[NCT_VRM_VIN] = 12130;
			sample[NCT_VRM_IOUT] = 22900;
			sample[NCT_VRM_POUT] = 32000000L;
			sample[NCT_VRM_TEMP] = 43000;
			sample[NCT_VRM_IIN] = 1830;
			sample[NCT_VRM_PIN] = 33000000L;
			sample[metric] = dec;
			prev = rail.val[metric];
			prev_min = rail.min[metric];
			prev_max = rail.max[metric];
			prev_notes = nct_vrm_neg_notes;
			was_ready = nct_vrm_rail_hist_ready(&rail, metric);
			nct_vrm_rail_apply(&rail, sample);
			publish_checks++;
			if (rail.val[metric] < 0)
				return sweep_fail(name, raw, rail.val[metric], prev);
			if (dec < 0) {
				expect_notes++;
				if (rail.val[metric] != prev || rail.min[metric] != prev_min ||
					rail.max[metric] != prev_max ||
					nct_vrm_neg_notes != prev_notes + 1 ||
					nct_vrm_rail_hist_ready(&rail, metric) != was_ready)
					return sweep_fail(name, raw, rail.val[metric], prev);
			} else {
				if (rail.val[metric] != dec || nct_vrm_neg_notes != prev_notes)
					return sweep_fail(name, raw, rail.val[metric], dec);
				if (!have) {
					expect_min = expect_max = dec;
					have = true;
				} else {
					if (dec < expect_min)
						expect_min = dec;
					if (dec > expect_max)
						expect_max = dec;
				}
				if (!nct_vrm_rail_hist_ready(&rail, metric) ||
					rail.min[metric] != expect_min ||
					rail.max[metric] != expect_max)
					return sweep_fail(name, raw, rail.max[metric], expect_max);
			}
			if (metric != NCT_VRM_POUT && dec < 0 &&
				rail.val[metric] == (long)raw * scale[metric])
				return sweep_fail(name, raw, rail.val[metric], prev);
		}
		printf("%s_neg %d\n%s_high %ld\n%s_live %ld\n",
			name, expect_notes, name, rail.max[metric], name, rail.val[metric]);
	}
	printf("publish_ok %d\n", publish_checks);

	{
		bool init = false;
		long pout = 0, pin = 0, pwm = 0;
		unsigned long at = 0;
		long resume_p, resume_i;

		nct_vrm_eff_update(&init, &pout, &pin, &at, 30000000L, 36000000L, 0);
		nct_vrm_eff_update(&init, &pout, &pin, &at, 30000000L, -1000000L, 1000);
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm) || pwm / 2 != 83 ||
			pout != 30000000L || pin != 36000000L)
			return sweep_fail("eff_neg_pin", 0, pout, 30000000L);
		printf("eff_neg_pin %ld %ld %ld\n", pwm / 2, pout, pin);

		nct_vrm_eff_update(&init, &pout, &pin, &at, -1000000L, 36000000L, 2000);
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm) || pwm / 2 != 83 ||
			pout != 30000000L || pin != 36000000L)
			return sweep_fail("eff_neg_pout", 0, pout, 30000000L);
		printf("eff_neg_pout %ld %ld %ld\n", pwm / 2, pout, pin);

		nct_vrm_eff_update(&init, &pout, &pin, &at, 30000000L, 0, 3000);
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm) || pwm / 2 != 83 ||
			pout != 30000000L || pin != 36000000L)
			return sweep_fail("eff_zero", 0, pin, 36000000L);
		printf("eff_zero %ld %ld %ld\n", pwm / 2, pout, pin);

		nct_vrm_eff_update(&init, &pout, &pin, &at, 40000000L, 50000000L, 4000);
		resume_p = pout;
		resume_i = pin;
		if (resume_p <= 30000000L || resume_p >= 40000000L ||
			resume_i <= 36000000L || resume_i >= 50000000L || resume_p < 0 ||
			resume_i < 0 || !nct_vrm_eff_publish(init, pout, pin, &pwm))
			return sweep_fail("eff_resume", 0, resume_p, 31666666L);
		printf("eff_resume %ld %ld %ld\n", pwm / 2, resume_p, resume_i);

		nct_vrm_eff_update(&init, &pout, &pin, &at, -1000000L, 36000000L, 4000 + 6000);
		if (pout < 0 || pin < 0 || nct_vrm_eff_publish(init, pout, pin, &pwm))
			return sweep_fail("eff_idle", 0, pout, 0);
		printf("eff_idle_clears nodata\n");
	}

	printf("ok\n");
	return 0;
}

static int rail_seq(int n, char** samples)
{
	static const char* const names[NCT_VRM_NMETRIC] = {
		"vout", "vin", "iout", "pout", "temp", "iin", "pin",
	};
	struct nct_vrm_rail r;
	long s[NCT_VRM_NMETRIC];
	char* p;
	int i, m;

	nct_vrm_neg_notes = 0;
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
	printf("noted %d\n", nct_vrm_neg_notes);
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
	if (argc == 2 && !strcmp(argv[1], "eff_converge")) {
		bool init = false;
		long pout = 0, pin = 0, mid_p = 0, mid_i = 0;
		unsigned long at = 0, t;

		nct_vrm_eff_update(&init, &pout, &pin, &at, 0, 0, 0);
		for (t = 100; t <= 40000; t += 100) {
			nct_vrm_eff_update(&init, &pout, &pin, &at, 1000000L, 2000000L, t);
			if (t == 10000) {
				mid_p = pout;
				mid_i = pin;
			}
		}
		printf("%ld %ld %ld %ld\n", mid_p, mid_i, pout, pin);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "eff_spike")) {
		bool init = false;
		long pout = 0, pin = 0, pwm = 0;
		unsigned long at = 0, t;

		/* Default threshold lets a 190 W PIN beside 30 W POUT through.
		 * 2.5x (250) would reject it.
		 */
		printf("%d\n", nct_vrm_eff_agree(30000000L, 190000000L));
		printf("%d\n", nct_vrm_eff_agree_x100(30000000L, 190000000L, 250));

		/* 30 W / 36 W, then that PIN spike, using a 2.5x gate via direct blend skip. */
		nct_vrm_eff_update(&init, &pout, &pin, &at, 30000000L, 36000000L, 0);
		if (!nct_vrm_eff_agree_x100(30000000L, 190000000L, 250)) {
			/* skipped: smoothed values stay at the baseline */
		} else {
			nct_vrm_eff_update(&init, &pout, &pin, &at, 30000000L, 190000000L, 1000);
		}
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm))
			printf("nodata\n");
		else
			printf("%ld\n", pwm / 2);

		/* Both registers step 30 W → 130 W together. */
		init = false;
		for (t = 0; t <= 40000; t += 1000)
			nct_vrm_eff_update(&init, &pout, &pin, &at,
				t == 0 ? 30000000L : 130000000L,
				t == 0 ? 30000000L : 130000000L, t);
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm))
			printf("nodata\n");
		else
			printf("%ld %ld %ld\n", pwm / 2, pout, pin);
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "eff_publish")) {
		bool init = false;
		long pout = 0, pin = 0, pwm = 0;
		unsigned long at = 0;

		nct_vrm_eff_update(&init, &pout, &pin, &at,
			strtol(argv[2], NULL, 0), strtol(argv[3], NULL, 0), 0);
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm))
			printf("nodata\n");
		else
			printf("%ld\n", pwm);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "eff_reset")) {
		bool init = false;
		long pout = 0, pin = 0, pwm = 0;
		unsigned long at = 0;

		nct_vrm_eff_update(&init, &pout, &pin, &at, 10, 20, 0);
		nct_vrm_eff_update(&init, &pout, &pin, &at, 10, 20, 100);
		nct_vrm_eff_update(&init, &pout, &pin, &at, 1000, 2000, 20000);
		printf("%ld %ld\n", pout, pin);
		nct_vrm_eff_reset(&init);
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm))
			printf("nodata\n");
		else
			printf("%ld\n", pwm);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "eff_warmup")) {
		/* A first pair with POUT > PIN (76 W / 53 W, seen live), then
		 * 4 pairs at 90 W / 100 W, 200 ms apart, 10 s window. The plain
		 * mean during warm-up gives (76 + 360) / (53 + 400) = 96%; the
		 * old seed-weighted blend stays at 100% (POUT >= PIN).
		 */
		bool init = false;
		long pout = 0, pin = 0, pwm = 0;
		unsigned long at = 0;
		unsigned n = 0;
		int i;

		nct_vrm_eff_update_n(&init, &pout, &pin, &at, &n, 10000, 76000000L, 53000000L, 0);
		for (i = 1; i <= 4; i++)
			nct_vrm_eff_update_n(&init, &pout, &pin, &at, &n, 10000, 90000000L,
				100000000L, i * 200);
		nct_vrm_eff_publish(init, pout, pin, &pwm);
		printf("%u %ld\n", n, pwm / 2);
		/* Old blend from the same samples. */
		init = false;
		nct_vrm_eff_update(&init, &pout, &pin, &at, 76000000L, 53000000L, 0);
		for (i = 1; i <= 4; i++)
			nct_vrm_eff_update(&init, &pout, &pin, &at, 90000000L, 100000000L, i * 200);
		nct_vrm_eff_publish(init, pout, pin, &pwm);
		printf("%ld\n", pwm / 2);
		return 0;
	}
	if (argc == 2 && !strcmp(argv[1], "sweep_rigor"))
		return sweep_rigor();
	if (argc == 2 && !strcmp(argv[1], "eff_hold")) {
		bool init = false;
		long pout = 0, pin = 0, pwm = 0;
		unsigned long at = 0;

		/* 30 W / 36 W, then a negative PIN. The average stays. */
		nct_vrm_eff_update(&init, &pout, &pin, &at, 30000000L, 36000000L, 0);
		nct_vrm_eff_update(&init, &pout, &pin, &at, 30000000L, -1000000L, 1000);
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm))
			printf("nodata\n");
		else
			printf("%ld %ld %ld\n", pwm / 2, pout, pin);
		init = false;
		nct_vrm_eff_update(&init, &pout, &pin, &at, -1000000L, 36000000L, 0);
		if (!nct_vrm_eff_publish(init, pout, pin, &pwm))
			printf("nodata\n");
		else
			printf("%ld\n", pwm);
		return 0;
	}
	if (argc == 8 && !strcmp(argv[1], "cpu_full")) {
		long v[NCT_VRM_NMETRIC] = { 0 };
		char buf[128];

		v[NCT_VRM_VOUT] = strtol(argv[2], NULL, 0);
		v[NCT_VRM_VIN] = strtol(argv[3], NULL, 0);
		v[NCT_VRM_IOUT] = strtol(argv[4], NULL, 0);
		v[NCT_VRM_IIN] = strtol(argv[5], NULL, 0);
		v[NCT_VRM_POUT] = strtol(argv[6], NULL, 0);
		v[NCT_VRM_PIN] = strtol(argv[7], NULL, 0);
		nct_vrm_format_cpu_full(buf, v);
		fputs(buf, stdout);
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
