/* Host checks for the prefetch decision. No kernel, no eSIO. */
#define NCT_VRM_PREFETCH_HOST 1

#include <stdio.h>

#include "../dkms/nct6687_vrm_prefetch.h"

static const char* act_name(enum nct_vrm_prefetch_act act)
{
	if (act == NCT_VRM_PREFETCH_NONE)
		return "none";
	if (act == NCT_VRM_PREFETCH_LATER)
		return "later";
	return "now";
}

static void show(const char* key, struct nct_vrm_prefetch_plan plan)
{
	printf("%s=%s %lu\n", key, act_name(plan.act), plan.delay_ms);
}

int main(void)
{
	struct nct_vrm_prefetch st = { 0 };
	struct nct_vrm_prefetch_plan plan;
	unsigned long delay;

	plan = nct_vrm_prefetch_note(&st, 0, ~0UL, 20);
	show("first", plan);

	plan = nct_vrm_prefetch_note(&st, 40, ~0UL, 20);
	show("burst", plan);

	plan = nct_vrm_prefetch_note(&st, 1000, 900, 20);
	show("seed", plan);
	printf("seed_age=%lu\n", st.age_at_read_ms);
	printf("seed_act=%s\n", act_name(st.last_act));
	nct_vrm_prefetch_note(&st, 1020, 1, 20);
	printf("held_age=%lu\n", st.age_at_read_ms);
	printf("held_act=%s\n", act_name(st.last_act));
	{
		struct nct_vrm_prefetch pass = { 0 };

		nct_vrm_prefetch_note(&pass, 0, 10, 20);
		plan = nct_vrm_prefetch_note(&pass, 60, 999, 20);
		show("pass_60", plan);
		printf("pass_60_age=%lu\n", pass.age_at_read_ms);
	}

	plan = nct_vrm_prefetch_note(&st, 2000, 900, 20);
	show("confirm", plan);
	nct_vrm_prefetch_note_duration(&st, 70);
	delay = nct_vrm_prefetch_delay_ms(&st, 2070, 20);
	printf("followup=%lu\n", delay);

	plan = nct_vrm_prefetch_note(&st, 3000, 40, 20);
	show("fresh_1s", plan);

	st = (struct nct_vrm_prefetch){ 0 };
	nct_vrm_prefetch_note(&st, 0, ~0UL, 20);
	nct_vrm_prefetch_note(&st, 250, 900, 20);
	nct_vrm_prefetch_note(&st, 500, 900, 20);
	plan = nct_vrm_prefetch_note(&st, 750, 40, 20);
	show("poll_250", plan);

	st = (struct nct_vrm_prefetch){ 0 };
	nct_vrm_prefetch_note(&st, 0, ~0UL, 20);
	nct_vrm_prefetch_note(&st, 2000, 900, 20);
	nct_vrm_prefetch_note(&st, 4000, 900, 20);
	plan = nct_vrm_prefetch_note(&st, 6000, 40, 20);
	show("poll_2000", plan);

	st = (struct nct_vrm_prefetch){ 0 };
	nct_vrm_prefetch_note(&st, 0, ~0UL, 20);
	nct_vrm_prefetch_note(&st, 50, 10, 20);
	nct_vrm_prefetch_note(&st, 100, 10, 20);
	plan = nct_vrm_prefetch_note(&st, 150, 10, 20);
	show("fast", plan);

	st = (struct nct_vrm_prefetch){ 0 };
	nct_vrm_prefetch_note(&st, 0, ~0UL, 20);
	nct_vrm_prefetch_note(&st, 1000, 900, 20);
	nct_vrm_prefetch_note(&st, 2000, 40, 20);
	plan = nct_vrm_prefetch_note(&st, 3500, 40, 20);
	show("jitter", plan);
	printf("stable_after_jitter=%u\n", st.stable);
	printf("jitter_act=%s\n", act_name(st.last_act));

	printf("idle=%d\n", nct_vrm_prefetch_idle(&st, 3500 + 1000 * 3) ? 1 : 0);
	printf("not_idle=%d\n", nct_vrm_prefetch_idle(&st, 3500 + 1000) ? 1 : 0);

	/* Armed prefetch came back 300 ms old. Duration was 327. Pull it in
	 * by 300 - 40 so the next start is that much later.
	 */
	st = (struct nct_vrm_prefetch){
		.interval_ms = 1000,
		.duration_ms = 327,
		.last_burst_ms = 5000,
		.stable = 2,
		.have_burst = true,
		.have_gap = true,
		.expect = true,
	};
	plan = nct_vrm_prefetch_note(&st, 6000, 300, 20);
	show("early", plan);
	printf("corrected_dur=%lu\n", st.duration_ms);
	plan = nct_vrm_prefetch_note(&st, 7000, 40, 20);
	show("after_correct", plan);

	/* 300 ms early against a 50 ms guess would have pinned duration at 1.
	 * That schedules the next start after the sample can finish.
	 */
	st = (struct nct_vrm_prefetch){
		.interval_ms = 1000,
		.duration_ms = 50,
		.last_burst_ms = 8000,
		.stable = 2,
		.have_burst = true,
		.have_gap = true,
		.expect = true,
	};
	nct_vrm_prefetch_note(&st, 9000, 300, 20);
	printf("floor_dur=%lu\n", st.duration_ms);
	st.duration_ms = 70;
	nct_vrm_prefetch_note_duration(&st, 1);
	printf("dur_ign=%lu\n", st.duration_ms);

	/* Steady 1.13 s poll. A hit re-arms, and a finished prefetch chains
	 * the next period, so ages stay under 100 ms after lock-in.
	 */
	{
		struct nct_vrm_prefetch poll = { 0 };
		unsigned long t = 0, done = 0;
		unsigned long arm_at = 0;
		int armed = 0;
		int i, late = 0;

		for (i = 0; i < 20; i++) {
			unsigned long age;

			t += 1130;
			if (armed && arm_at <= t) {
				unsigned long chain;

				done = arm_at + 70;
				nct_vrm_prefetch_note_duration(&poll, 70);
				chain = nct_vrm_prefetch_chain_ms(&poll, 70);
				if (chain) {
					arm_at = done + chain;
					armed = 1;
				} else {
					armed = 0;
				}
			}
			age = done ? t - done : ~0UL;
			plan = nct_vrm_prefetch_note(&poll, t, age, 20);
			if (plan.act == NCT_VRM_PREFETCH_LATER) {
				nct_vrm_prefetch_expect(&poll);
				arm_at = t + plan.delay_ms;
				armed = 1;
			} else if (plan.act == NCT_VRM_PREFETCH_NOW) {
				done = t + 70;
				nct_vrm_prefetch_note_duration(&poll, 70);
				{
					unsigned long d = nct_vrm_prefetch_delay_ms(&poll, done, 20);

					if (d) {
						nct_vrm_prefetch_expect(&poll);
						arm_at = done + d;
						armed = 1;
					} else {
						armed = 0;
					}
				}
			}
			if (i >= 4 && (plan.act != NCT_VRM_PREFETCH_LATER || age >= 100))
				late++;
			printf("poll_%d=%s %lu\n", i, act_name(plan.act), age == ~0UL ? 0UL : age);
		}
		printf("poll_late=%d\n", late);
	}
	return 0;
}
