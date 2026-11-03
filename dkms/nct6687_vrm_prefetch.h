/* When to start the next VRM refresh.
 *
 * A demand read returns the sample already published. Once the gap between
 * read bursts is steady, the next refresh is started
 * interval - sample_duration - margin before the following burst, so the
 * sample is a few tens of milliseconds old instead of a full interval.
 * The first bursts, and any gap more than 30% off the smoothed interval,
 * do not prefetch: the caller refreshes on the read, as before.
 *
 * Times are milliseconds on one monotonic clock. One burst is every demand
 * read inside the burst window of the burst's first read. The window is
 * at least 150 ms, and longer when a sample itself takes longer, so a
 * poll pass that stalled behind the EC mailbox stays one burst.
 * Host: tests/vrm_prefetch.c with -DNCT_VRM_PREFETCH_HOST.
 */
#ifndef NCT6687_VRM_PREFETCH_H
#define NCT6687_VRM_PREFETCH_H

#ifdef NCT_VRM_PREFETCH_HOST
#include <stdbool.h>
#endif

#define NCT_VRM_PREFETCH_BURST_MS 150UL
#define NCT_VRM_PREFETCH_MARGIN_MS 40UL
#define NCT_VRM_PREFETCH_FRESH_MS 100UL
#define NCT_VRM_PREFETCH_JITTER_PCT 30UL
#define NCT_VRM_PREFETCH_IDLE_MULT 3UL
#define NCT_VRM_PREFETCH_DEFAULT_DUR_MS 80UL
/* A PAGE sample is tens of milliseconds. Shorter than this is not one,
 * and the scheduler must not be told the EC finishes in 1 ms.
 */
#define NCT_VRM_PREFETCH_MIN_DUR_MS 40UL

enum nct_vrm_prefetch_act {
	NCT_VRM_PREFETCH_NONE = 0,
	NCT_VRM_PREFETCH_NOW,
	NCT_VRM_PREFETCH_LATER,
};

struct nct_vrm_prefetch {
	unsigned long interval_ms;
	unsigned long duration_ms;
	unsigned long last_burst_ms;
	unsigned long burst_seen_ms;
	unsigned long burst_seq;
	unsigned stable;
	bool have_burst;
	bool have_gap;
	bool burst_open;
	bool expect; /* a prefetch was armed for the next burst */
	bool age_at_read_valid;
	unsigned long age_at_read_ms; /* age the last new demand burst observed */
	enum nct_vrm_prefetch_act last_act;
	unsigned arm_n; /* times a prefetch timer was queued */
	unsigned run_n; /* times that timer's work started */
	unsigned skip_n; /* of those, ones that did not publish */
	unsigned long last_delay_ms; /* delay actually queued */
};

struct nct_vrm_prefetch_plan {
	enum nct_vrm_prefetch_act act;
	unsigned long delay_ms;
};

static unsigned long nct_vrm_prefetch_dur(const struct nct_vrm_prefetch* st)
{
	return st->duration_ms ? st->duration_ms : NCT_VRM_PREFETCH_DEFAULT_DUR_MS;
}

/* One poll pass. At least 150 ms, or the sample plus the margin when that
 * is longer, so reads stalled behind the EC mailbox stay in the burst.
 */
static unsigned long nct_vrm_prefetch_window_ms(const struct nct_vrm_prefetch* st)
{
	unsigned long span = nct_vrm_prefetch_dur(st) + NCT_VRM_PREFETCH_MARGIN_MS;

	return span > NCT_VRM_PREFETCH_BURST_MS ? span : NCT_VRM_PREFETCH_BURST_MS;
}

/* 0 if a prefetch cannot finish before the next burst. Else milliseconds
 * from now_ms until that prefetch should start.
 */
static unsigned long nct_vrm_prefetch_delay_ms(const struct nct_vrm_prefetch* st,
	unsigned long now_ms, unsigned long floor_ms)
{
	unsigned long dur, start;

	if (st->stable < 1 || !st->have_gap || !st->have_burst)
		return 0;
	dur = nct_vrm_prefetch_dur(st);
	if (st->interval_ms <= dur + NCT_VRM_PREFETCH_MARGIN_MS + floor_ms)
		return 0;
	start = st->last_burst_ms + st->interval_ms - dur - NCT_VRM_PREFETCH_MARGIN_MS;
	if (start <= now_ms)
		return 0;
	return start - now_ms;
}

/* True when a prefetch timer fired long after the reader went away. */
static bool nct_vrm_prefetch_idle(const struct nct_vrm_prefetch* st, unsigned long now_ms)
{
	unsigned long idle;

	if (!st->have_gap || !st->interval_ms || !st->have_burst || now_ms < st->last_burst_ms)
		return false;
	idle = st->interval_ms * NCT_VRM_PREFETCH_IDLE_MULT;
	return now_ms - st->last_burst_ms >= idle;
}

static void nct_vrm_prefetch_note_duration(struct nct_vrm_prefetch* st, unsigned long dur_ms)
{
	/* Not a PAGE sample. Do not pin the scheduler to a 1 ms guess. */
	if (dur_ms < NCT_VRM_PREFETCH_MIN_DUR_MS)
		return;
	if (dur_ms > 1000)
		dur_ms = 1000;
	/* A sample that waited on the lock is slower than the EC. Keep the
	 * fast ones. Rise slowly if the EC itself gets slower.
	 */
	if (!st->duration_ms || dur_ms < st->duration_ms)
		st->duration_ms = dur_ms;
	else
		st->duration_ms = (st->duration_ms * 7 + dur_ms) / 8;
}

static void nct_vrm_prefetch_expect(struct nct_vrm_prefetch* st)
{
	st->expect = true;
}

/* Delay from the moment a prefetch sample finishes until the next prefetch
 * should start, preserving phase. 0 if the cadence is not locked.
 */
static unsigned long nct_vrm_prefetch_chain_ms(const struct nct_vrm_prefetch* st,
	unsigned long dur_ms)
{
	if (st->stable < 1 || !st->have_gap || !st->interval_ms || !dur_ms)
		return 0;
	if (st->interval_ms <= dur_ms + NCT_VRM_PREFETCH_MARGIN_MS)
		return 0;
	return st->interval_ms - dur_ms;
}

/* sample_age_ms is the age of the published sample, or a huge value when
 * nothing has been published. floor_ms is the 20 ms demand floor.
 */
static struct nct_vrm_prefetch_plan nct_vrm_prefetch_note(struct nct_vrm_prefetch* st,
	unsigned long now_ms, unsigned long sample_age_ms, unsigned long floor_ms)
{
	struct nct_vrm_prefetch_plan plan = { NCT_VRM_PREFETCH_NOW, 0 };
	unsigned long gap, diff, delay;

	if (st->burst_open && now_ms >= st->burst_seen_ms &&
		now_ms - st->burst_seen_ms < nct_vrm_prefetch_window_ms(st))
		return (struct nct_vrm_prefetch_plan){ NCT_VRM_PREFETCH_NONE, 0 };

	/* The previous prefetch finished early when age is still well under
	 * the interval but past the margin. Shorten the duration we subtract
	 * so the next one starts later. A miss (age near the whole interval)
	 * is not this case.
	 */
	if (st->expect && st->interval_ms && sample_age_ms < st->interval_ms / 2 && sample_age_ms > NCT_VRM_PREFETCH_MARGIN_MS) {
		unsigned long early = sample_age_ms - NCT_VRM_PREFETCH_MARGIN_MS;
		unsigned long dur = nct_vrm_prefetch_dur(st);

		st->duration_ms = dur > early ? dur - early : NCT_VRM_PREFETCH_MIN_DUR_MS;
		if (st->duration_ms < NCT_VRM_PREFETCH_MIN_DUR_MS)
			st->duration_ms = NCT_VRM_PREFETCH_MIN_DUR_MS;
	}
	st->expect = false;

	st->burst_open = true;
	st->burst_seen_ms = now_ms;
	st->burst_seq++;

	if (st->have_burst && now_ms > st->last_burst_ms) {
		gap = now_ms - st->last_burst_ms;
		diff = 0;
		if (st->have_gap && st->interval_ms)
			diff = gap > st->interval_ms ? gap - st->interval_ms : st->interval_ms - gap;
		if (!st->have_gap) {
			st->interval_ms = gap;
			st->have_gap = true;
			st->stable = 0;
		} else if (!st->interval_ms || diff * 100 > st->interval_ms * NCT_VRM_PREFETCH_JITTER_PCT) {
			st->stable = 0;
		} else {
			st->interval_ms = (st->interval_ms * 3 + gap) / 4;
			if (st->stable < 8)
				st->stable++;
		}
	}
	st->have_burst = true;
	st->last_burst_ms = now_ms;

	/* delay is when to start the sample for the *next* burst, whether
	 * this read is a hit or a fallback. A hit must re-arm; a fallback
	 * must arm too, or the next read is one interval old again.
	 */
	delay = nct_vrm_prefetch_delay_ms(st, now_ms, floor_ms);
	if (delay)
		plan.delay_ms = delay;
	if (delay && st->stable >= 1 && st->interval_ms && sample_age_ms < st->interval_ms / 2)
		plan.act = NCT_VRM_PREFETCH_LATER;
	/* Frozen for a debug read. A same-burst note returned above and
	 * left these alone.
	 */
	st->age_at_read_ms = sample_age_ms;
	st->age_at_read_valid = sample_age_ms != ~0UL;
	st->last_act = plan.act;
	return plan;
}

#endif
