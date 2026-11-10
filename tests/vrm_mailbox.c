/*
 * Host compile of dkms/nct6687_vrm_mailbox.h — no linux headers, no .ko,
 * no /dev/port. pytest drives this against the same sequences as
 * tests/test_vrm_esio.py, and runs the real nct_vrm_sample_page against
 * a fake PMBus device behind the fake eSIO window
 * (tests/golden_vrm_sample.json).
 *
 *   cc -Wall -Werror -Wno-unused-function -DNCT_VRM_MAILBOX_HOST \
 *      -o vrm_mailbox tests/vrm_mailbox.c
 *
 * -Wno-unused-function: the decode header's hist/rate helpers are driven
 * by tests/vrm_decode.c, not here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NCT_VRM_MAILBOX_HOST 1
#include "../dkms/nct6687_vrm_demand.h"

static int host_fail_notes;
static struct nct_vrm_fail_note host_fail;

static void nct_vrm_warn_fail(const struct nct_vrm_fail_note* n)
{
	host_fail_notes++;
	host_fail = *n;
}

static unsigned long demand_now;

static unsigned long nct_vrm_now(void)
{
	return demand_now;
}

static unsigned long nct_vrm_eff_now_ms(void)
{
	return demand_now;
}

static void nct_vrm_ec_lock(struct nct6687_data* data)
{
	(void)data;
}

static void nct_vrm_ec_unlock(struct nct6687_data* data)
{
	(void)data;
}

static void nct_vrm_warn_recover(void)
{
}

static bool nct_vrm_claim_demand(struct nct6687_data* data)
{
	bool demanded = data->vrm_demand;

	data->vrm_demand = false;
	return demanded;
}

static bool nct_vrm_claim_full(struct nct6687_data* data)
{
	bool full = data->vrm_demand_full;

	data->vrm_demand_full = false;
	return full;
}

static unsigned host_fast;

static unsigned nct_vrm_fast_mode(void)
{
	return host_fast;
}

static bool host_mux_keep;
static bool host_stuck_start;
/* Writes to 0x61 and 0x62 since start. */
static int host_mux_writes;

static bool nct_vrm_mux_keep_fw(void)
{
	return host_mux_keep;
}

static unsigned long nct_vrm_eff_tau_ms(void)
{
	return 10000;
}

static unsigned host_shortcuts_warned;

static void nct_vrm_warn_shortcuts(unsigned bits)
{
	host_shortcuts_warned = bits;
}

static void nct_vrm_note_unpublished(int metric, long val)
{
	(void)metric;
	(void)val;
}

/* Direct nct_vrm_mux_cycle. Rail 0 valid is what the checked restore's host
 * reject keys off, same as demand's rail_apply after a good PAGE.
 */
struct nct_vrm_mux_probe {
	u8 addr;
	long sample[NCT_VRM_NMETRIC];
};

static int nct_vrm_mux_probe_body(struct nct6687_data* data, void* vctx)
{
	struct nct_vrm_mux_probe* job = vctx;
	int rc;

	rc = nct_vrm_sample_page(data, job->addr, 0, -10, job->sample, NULL);
	if (!rc)
		data->vrm_rail[0].valid = true;
	return rc;
}

#define SMB_START 0x40
#define IDLE_PAGE 0xff
#define WRITE_OP 0x04
#define LOG_MAX 4096

static unsigned int base = 0xa20;
static int idle_ok = 1;
static u8 after_start_status;
static u8 page = IDLE_PAGE;
static u8 win_index;
static u8 regs[256];

struct log_entry {
	char op;
	u16 port;
	u8 val;
};

static struct log_entry log_buf[LOG_MAX];
static int nlog;

/* Fake PMBus device: answers each mailbox START from proto/cmd/payload. */
enum { PM_WRITE_BYTE, PM_READ_BYTE, PM_READ_WORD, PM_NKIND };
static const char* const pm_kind_name[PM_NKIND] = {
	"write_byte", "read_byte", "read_word",
};
static int pmbus_dev;
static u8 pm_page;
static int pm_page_readback = -1;
static u8 pm_bytes[256];
static u16 pm_words[256];
static u8 pm_fail[PM_NKIND][256];
static unsigned pm_nth_fail[PM_NKIND][256];
static unsigned pm_nth_seen[PM_NKIND][256];
static char pm_ops[256][32];
static int pm_nops;

static void usage(void)
{
	fprintf(stderr,
		"usage: vrm_mailbox [--stuck-page] [--status HEX] [--b0 HEX] [--b1 HEX]\n"
		"                   esio_write INDEX VALUE\n"
		"                   esio_read PAGE INDEX\n"
		"                   write_byte ADDR CMD VALUE\n"
		"                   read_byte ADDR CMD\n"
		"                   read_word ADDR CMD\n"
		"                   recover\n"
		"       vrm_mailbox [--cache-page N] [--cache-mode PAGE=MODE]...\n"
		"                   [--byte CMD=VAL]... [--word CMD=VAL]...\n"
		"                   [--fail KIND:CMD=STS]... [--page-readback VAL]\n"
		"                   sample_page ADDR PAGE VOUT_EXP\n"
		"       vrm_mailbox [--now J] [--hz J] [--floor J] [--last-read J]\n"
		"                   [--last-updated J] [--fails N] [--read-gap J]\n"
		"                   [--demand] [--cpu-valid] [--gt] [--reject-mux-restore]\n"
		"                   [--fail-nth KIND:CMD=N] [--then-now J] [--then-last-read J]\n"
		"                   demand ADDR\n"
		"       vrm_mailbox [--cfg61 HEX] [--cfg62 HEX] [--byte CMD=VAL]...\n"
		"                   [--word CMD=VAL]... [--reject-mux-restore] [--mux-recover]\n"
		"                   mux ADDR SEL\n");
}

static void pm_log(const char* fmt, unsigned a, unsigned b)
{
	if (pm_nops < 256)
		snprintf(pm_ops[pm_nops++], sizeof(pm_ops[0]), fmt, a, b);
}

static u8 pm_status(int kind, u8 cmd)
{
	pm_nth_seen[kind][cmd]++;
	if (pm_nth_fail[kind][cmd] && pm_nth_seen[kind][cmd] == pm_nth_fail[kind][cmd])
		return 1;
	return pm_fail[kind][cmd];
}

/* One PMBus transaction at START; returns the status byte for reg 0x03. */
static u8 pm_transact(void)
{
	u8 cmd = regs[0x66];
	u8 sts;

	switch (regs[0x63]) {
	case NCT_VRM_PROTO_WBR:
		pm_log("write_byte 0x%02x 0x%02x", cmd, regs[0x70]);
		sts = pm_status(PM_WRITE_BYTE, cmd);
		if (!sts && cmd == 0x00)
			pm_page = regs[0x70];
		return sts;
	case NCT_VRM_PROTO_RB:
		pm_log("read_byte 0x%02x", cmd, 0);
		sts = pm_status(PM_READ_BYTE, cmd);
		if (cmd != 0x00)
			regs[0xb0] = pm_bytes[cmd];
		else if (pm_page_readback >= 0)
			regs[0xb0] = (u8)pm_page_readback;
		else
			regs[0xb0] = pm_page;
		return sts;
	case NCT_VRM_PROTO_RW:
		pm_log("read_word 0x%02x", cmd, 0);
		regs[0xb0] = pm_words[cmd] & 0xff;
		regs[0xb1] = pm_words[cmd] >> 8;
		return pm_status(PM_READ_WORD, cmd);
	}
	return 0xff;
}

void nct_vrm_host_outb(u16 port, u8 val)
{
	if (nlog < LOG_MAX) {
		log_buf[nlog].op = 'o';
		log_buf[nlog].port = port;
		log_buf[nlog].val = val;
		nlog++;
	}
	if (port == base + EC_SPACE_PAGE_REGISTER_OFFSET)
		page = val;
	else if (port == base + EC_SPACE_INDEX_REGISTER_OFFSET)
		win_index = val;
	else if (port == base + EC_SPACE_DATA_REGISTER_OFFSET) {
		if (win_index == 0x60) {
			if (val & SMB_START)
				regs[0x03] = pmbus_dev ? pm_transact() : after_start_status;
			/* --stuck-start: START never clears, so the wait times out. */
			regs[0x60] = (host_stuck_start && (val & SMB_START)) ? val : (val & ~SMB_START);
		} else if ((win_index == 0x03 || win_index == 0x04) && val == 0xff)
			regs[win_index] = 0;
		else
			regs[win_index] = val;
		if (win_index == 0x61 || win_index == 0x62)
			host_mux_writes++;
	}
}

u8 nct_vrm_host_inb(u16 port)
{
	u8 val = 0;

	if (port == base + EC_SPACE_PAGE_REGISTER_OFFSET)
		val = idle_ok ? page : 0x00;
	else if (port == base + EC_SPACE_INDEX_REGISTER_OFFSET)
		val = win_index;
	else if (port == base + EC_SPACE_DATA_REGISTER_OFFSET)
		val = regs[win_index];
	if (nlog < LOG_MAX) {
		log_buf[nlog].op = 'i';
		log_buf[nlog].port = port;
		log_buf[nlog].val = val;
		nlog++;
	}
	return val;
}

void nct_vrm_host_udelay(unsigned us)
{
	(void)us;
}

static unsigned nct_vrm_host_sleep_busy;

void nct_vrm_host_usleep_range(unsigned min_us, unsigned max_us)
{
	(void)min_us;
	(void)max_us;
	/* The START poll must be asleep here. A non-zero depth counts the sleep
	 * toward vrm_busy_ns.
	 */
	if (nct_vrm_busy_depth)
		nct_vrm_host_sleep_busy++;
}

static void dump_log(void)
{
	int i;

	for (i = 0; i < nlog; i++)
		printf("%s 0x%04x 0x%02x\n",
			log_buf[i].op == 'o' ? "outb" : "inb",
			log_buf[i].port, log_buf[i].val);
}

static int parse_u8(const char* s, u8* out)
{
	char* end;
	unsigned long v = strtoul(s, &end, 0);

	if (*s == '\0' || *end != '\0' || v > 0xff)
		return -1;
	*out = (u8)v;
	return 0;
}

/* "KEY=VAL" with KEY <= 0xff and VAL <= max. */
static int parse_pair(const char* s, u8* key, unsigned long max, unsigned long* val)
{
	char* end;
	unsigned long k = strtoul(s, &end, 0);

	if (end == s || *end != '=' || k > 0xff)
		return -1;
	s = end + 1;
	*val = strtoul(s, &end, 0);
	if (*s == '\0' || *end != '\0' || *val > max)
		return -1;
	*key = (u8)k;
	return 0;
}

/* "KIND:CMD=N" → pm_nth_fail. N is the 1-based occurrence that returns status 1. */
static int parse_fail_nth(const char* s)
{
	const char* colon = strchr(s, ':');
	unsigned long nth;
	u8 cmd;
	int k;

	if (!colon)
		return -1;
	for (k = 0; k < PM_NKIND; k++) {
		if (strlen(pm_kind_name[k]) == (size_t)(colon - s)
			&& !strncmp(s, pm_kind_name[k], colon - s))
			break;
	}
	if (k == PM_NKIND || parse_pair(colon + 1, &cmd, 0xffff, &nth) || !nth)
		return -1;
	pm_nth_fail[k][cmd] = (unsigned)nth;
	return 0;
}

/* "KIND:CMD=STS" → pm_fail. */
static int parse_fail(const char* s)
{
	const char* colon = strchr(s, ':');
	unsigned long sts;
	u8 cmd;
	int k;

	if (!colon)
		return -1;
	for (k = 0; k < PM_NKIND; k++) {
		if (strlen(pm_kind_name[k]) == (size_t)(colon - s)
			&& !strncmp(s, pm_kind_name[k], colon - s))
			break;
	}
	if (k == PM_NKIND || parse_pair(colon + 1, &cmd, 0xff, &sts) || !sts)
		return -1;
	pm_fail[k][cmd] = (u8)sts;
	return 0;
}

static void dump_vrm_cache(const struct nct6687_data* data)
{
	int p;

	printf("cache_page=%d\n", data->vrm_smbus_page);
	for (p = 0; p < 2; p++) {
		if (data->vrm_vout_mode_valid[p])
			printf("vout_mode%d=0x%02x\n", p, data->vrm_vout_mode_cache[p]);
		else
			printf("vout_mode%d=-\n", p);
	}
}

int main(int argc, char** argv)
{
	struct nct6687_data data = { .addr = base, .vrm_smbus_page = -1 };
	const char* fn;
	int i = 1;
	int rc;
	u8 a, b, c, out8;
	u16 out16;
	unsigned long pv;
	long sample[NCT_VRM_NMETRIC];
	int op;
	unsigned long demand_hz = 1000;
	unsigned long demand_floor = 20;
	unsigned long demand_last_read = 0;
	unsigned long demand_last_updated = 0;
	unsigned long demand_gap = 0;
	unsigned demand_fails = 0;
	int demand_on = 0;
	int force_floor = 0;
	int cpu_valid = 0;
	int sample_gt = 0;
	int mux_recover = 0;
	unsigned long then_now = 0;
	unsigned long then_last_read = 0;
	int have_then = 0;
	int then_full = 0;

	regs[0x03] = 0;
	regs[0x60] = 0;
	regs[0x61] = 0;
	regs[0x62] = 0;

	while (i < argc && argv[i][0] == '-') {
		if (!strcmp(argv[i], "--stuck-page"))
			idle_ok = 0;
		else if (!strcmp(argv[i], "--status") && i + 1 < argc) {
			if (parse_u8(argv[++i], &after_start_status)) {
				usage();
				return 2;
			}
		} else if (!strcmp(argv[i], "--b0") && i + 1 < argc) {
			if (parse_u8(argv[++i], &out8)) {
				usage();
				return 2;
			}
			regs[0xb0] = out8;
		} else if (!strcmp(argv[i], "--b1") && i + 1 < argc) {
			if (parse_u8(argv[++i], &out8)) {
				usage();
				return 2;
			}
			regs[0xb1] = out8;
		} else if (!strcmp(argv[i], "--cache-page") && i + 1 < argc) {
			data.vrm_smbus_page = (int)strtol(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--cache-mode") && i + 1 < argc) {
			if (parse_pair(argv[++i], &a, 0xff, &pv) || a > 1) {
				usage();
				return 2;
			}
			data.vrm_vout_mode_cache[a] = (u8)pv;
			data.vrm_vout_mode_valid[a] = true;
		} else if (!strcmp(argv[i], "--byte") && i + 1 < argc) {
			if (parse_pair(argv[++i], &a, 0xff, &pv)) {
				usage();
				return 2;
			}
			pm_bytes[a] = (u8)pv;
		} else if (!strcmp(argv[i], "--word") && i + 1 < argc) {
			if (parse_pair(argv[++i], &a, 0xffff, &pv)) {
				usage();
				return 2;
			}
			pm_words[a] = (u16)pv;
		} else if (!strcmp(argv[i], "--fail") && i + 1 < argc) {
			if (parse_fail(argv[++i])) {
				usage();
				return 2;
			}
		} else if (!strcmp(argv[i], "--fail-nth") && i + 1 < argc) {
			if (parse_fail_nth(argv[++i])) {
				usage();
				return 2;
			}
		} else if (!strcmp(argv[i], "--then-now") && i + 1 < argc) {
			then_now = strtoul(argv[++i], NULL, 0);
			have_then = 1;
		} else if (!strcmp(argv[i], "--then-last-read") && i + 1 < argc) {
			then_last_read = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--now") && i + 1 < argc) {
			demand_now = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--hz") && i + 1 < argc) {
			demand_hz = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--floor") && i + 1 < argc) {
			demand_floor = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--last-read") && i + 1 < argc) {
			demand_last_read = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--last-updated") && i + 1 < argc) {
			demand_last_updated = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--fails") && i + 1 < argc) {
			demand_fails = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--read-gap") && i + 1 < argc) {
			demand_gap = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--demand")) {
			demand_on = 1;
		} else if (!strcmp(argv[i], "--fast") && i + 1 < argc) {
			host_fast = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--mux-keep")) {
			host_mux_keep = true;
		} else if (!strcmp(argv[i], "--stuck-start")) {
			host_stuck_start = true;
		} else if (!strcmp(argv[i], "--full-at") && i + 1 < argc) {
			data.vrm_full_at = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--then-full")) {
			then_full = 1;
		} else if (!strcmp(argv[i], "--full-asked-at") && i + 1 < argc) {
			data.vrm_full_asked_at = strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--want-full")) {
			data.vrm_demand_full = true;
		} else if (!strcmp(argv[i], "--rail") && i + 1 < argc) {
			/* Previous published CPU rail, enum nct_vrm_metric order: VOUT,VIN,IOUT,POUT,TEMP,IIN,PIN. */
			char* q = argv[++i];
			int m;

			for (m = 0; m < NCT_VRM_NMETRIC; m++) {
				data.vrm_rail[0].val[m] = strtol(q, &q, 0);
				if (*q == ',')
					q++;
			}
		} else if (!strcmp(argv[i], "--force-floor")) {
			force_floor = 1;
		} else if (!strcmp(argv[i], "--cpu-valid")) {
			cpu_valid = 1;
		} else if (!strcmp(argv[i], "--gt")) {
			sample_gt = 1;
		} else if (!strcmp(argv[i], "--cfg61") && i + 1 < argc) {
			if (parse_u8(argv[++i], &out8)) {
				usage();
				return 2;
			}
			regs[0x61] = out8;
		} else if (!strcmp(argv[i], "--cfg62") && i + 1 < argc) {
			if (parse_u8(argv[++i], &out8)) {
				usage();
				return 2;
			}
			regs[0x62] = out8;
		} else if (!strcmp(argv[i], "--reject-mux-restore")) {
			nct_vrm_host_reject_mux_restore = 1;
		} else if (!strcmp(argv[i], "--mux-recover")) {
			mux_recover = 1;
		} else if (!strcmp(argv[i], "--page-readback") && i + 1 < argc) {
			if (parse_u8(argv[++i], &out8)) {
				usage();
				return 2;
			}
			pm_page_readback = out8;
		} else {
			usage();
			return 2;
		}
		i++;
	}
	data.addr = base;
	if (i >= argc) {
		usage();
		return 2;
	}
	fn = argv[i++];

	if (!strcmp(fn, "esio_write") && argc - i == 2) {
		if (parse_u8(argv[i], &a) || parse_u8(argv[i + 1], &b)) {
			usage();
			return 2;
		}
		rc = nct_vrm_esio_write(&data, a, b);
		printf("rc=%d\n", rc);
		dump_log();
		return rc ? 1 : 0;
	}
	if (!strcmp(fn, "esio_read") && argc - i == 2) {
		if (parse_u8(argv[i], &a) || parse_u8(argv[i + 1], &b)) {
			usage();
			return 2;
		}
		rc = nct_vrm_esio_read(&data, a, b, &out8);
		printf("rc=%d\n", rc);
		if (!rc)
			printf("value=0x%02x\n", out8);
		dump_log();
		return rc ? 1 : 0;
	}
	if (!strcmp(fn, "write_byte") && argc - i == 3) {
		if (parse_u8(argv[i], &a) || parse_u8(argv[i + 1], &b)
			|| parse_u8(argv[i + 2], &c)) {
			usage();
			return 2;
		}
		rc = nct_vrm_write_byte(&data, a, b, c);
		printf("rc=%d\n", rc);
		dump_log();
		return rc ? 1 : 0;
	}
	if (!strcmp(fn, "read_byte") && argc - i == 2) {
		if (parse_u8(argv[i], &a) || parse_u8(argv[i + 1], &b)) {
			usage();
			return 2;
		}
		rc = nct_vrm_read_byte(&data, a, b, &out8);
		printf("rc=%d\n", rc);
		if (!rc)
			printf("value=0x%02x\n", out8);
		dump_log();
		return rc ? 1 : 0;
	}
	if (!strcmp(fn, "read_word") && argc - i == 2) {
		if (parse_u8(argv[i], &a) || parse_u8(argv[i + 1], &b)) {
			usage();
			return 2;
		}
		rc = nct_vrm_read_word(&data, a, b, &out16);
		printf("rc=%d\n", rc);
		if (!rc)
			printf("value=0x%04x\n", out16);
		dump_log();
		return rc ? 1 : 0;
	}
	if (!strcmp(fn, "sample_page") && argc - i == 3) {
		if (parse_u8(argv[i], &a) || parse_u8(argv[i + 1], &b)) {
			usage();
			return 2;
		}
		pmbus_dev = 1;
		rc = nct_vrm_sample_page(&data, a, b, (int)strtol(argv[i + 2], NULL, 0),
			sample, NULL);
		printf("rc=%d\n", rc);
		for (op = 0; op < pm_nops; op++)
			printf("pmbus %s\n", pm_ops[op]);
		if (!rc)
			printf("out=%ld %ld %ld %ld %ld %ld %ld\n", sample[NCT_VRM_VOUT],
				sample[NCT_VRM_VIN], sample[NCT_VRM_IOUT],
				sample[NCT_VRM_POUT], sample[NCT_VRM_TEMP],
				sample[NCT_VRM_IIN], sample[NCT_VRM_PIN]);
		dump_vrm_cache(&data);
		return rc ? 1 : 0;
	}
	if (!strcmp(fn, "demand") && argc - i == 1) {
		int before;

		if (parse_u8(argv[i], &a)) {
			usage();
			return 2;
		}
		data.vrm_enabled = true;
		data.vrm_demand = demand_on;
		data.vrm_last_read = demand_last_read;
		data.vrm_last_updated = demand_last_updated;
		data.vrm_fail_count = demand_fails;
		data.vrm_read_gap = demand_gap;
		data.vrm_rail[0].valid = cpu_valid;
		pmbus_dev = 1;
		before = nlog;
		nct_vrm_demand_session(&data, demand_hz, demand_floor, a, -10, sample_gt,
			force_floor);
		if (have_then) {
			demand_now = then_now;
			data.vrm_last_read = then_last_read;
			data.vrm_demand = true;
			data.vrm_demand_full = then_full;
			nct_vrm_demand_session(&data, demand_hz, demand_floor, a, -10, sample_gt,
				force_floor);
		}
		printf("io=%d\n", nlog > before);
		printf("fail=%u\n", data.vrm_fail_count);
		printf("gt_fails=%u\n", data.vrm_gt_fails);
		printf("cpu_valid=%d\n", data.vrm_rail[0].valid ? 1 : 0);
		printf("gt_valid=%d\n", data.vrm_rail[1].valid ? 1 : 0);
		printf("demand=%d\n", data.vrm_demand ? 1 : 0);
		printf("read_gap=%lu\n", data.vrm_read_gap);
		printf("last_updated=%lu\n", data.vrm_last_updated);
		printf("vout=%ld\n", data.vrm_rail[0].val[NCT_VRM_VOUT]);
		printf("ctrl_valid=%d\n", data.vrm_ctrl_valid ? 1 : 0);
		printf("ctrl_mc=%ld\n", data.vrm_ctrl_mc);
		printf("sleep_busy=%u\n", nct_vrm_host_sleep_busy);
		printf("ports=%d\n", nlog - before);
		printf("shortcuts_warned=%u\n", host_shortcuts_warned);
		printf("shortcuts_dead=%d\n", nct_vrm_win.shortcuts_dead ? 1 : 0);
		printf("mux_writes=%d\n", host_mux_writes);
		printf("fail_notes=%d\n", host_fail_notes);
		printf("fail_cmd=%u\n", host_fail.cmd);
		printf("fail_rc=%d\n", host_fail.rc);
		printf("fail_sts=%d\n", host_fail.sts_read ? host_fail.sts : -1);
		printf("fail_regs_ok=%d\n", host_fail.regs_ok ? 1 : 0);
		printf("fail_cfg=%u\n", host_fail.cfg);
		printf("fail_speed=%u\n", host_fail.speed);
		printf("mux_kept=%d\n", data.vrm_mux_kept ? 1 : 0);
		printf("cfg61=%u\n", regs[0x61]);
		printf("cfg62=%u\n", regs[0x62]);
		{
			int k, idle_checks = 0;

			for (k = before; k < nlog && k < LOG_MAX; k++)
				if (log_buf[k].op == 'i' && log_buf[k].port == base + EC_SPACE_PAGE_REGISTER_OFFSET)
					idle_checks++;
			printf("idle_checks=%d\n", idle_checks);
		}
		printf("pm_ops=%d\n", pm_nops);
		printf("full_at=%lu\n", data.vrm_full_at);
		printf("want_full=%d\n", data.vrm_demand_full ? 1 : 0);
		{
			int m;

			for (m = 0; m < NCT_VRM_NMETRIC; m++)
				printf("rail%d=%ld\n", m, data.vrm_rail[0].val[m]);
		}
		return 0;
	}
	if (!strcmp(fn, "mux") && argc - i == 2) {
		struct nct_vrm_mux_probe job = { 0 };
		enum nct_vrm_mux_phase phase;
		unsigned sel;

		if (parse_u8(argv[i], &job.addr)) {
			usage();
			return 2;
		}
		sel = (unsigned)strtoul(argv[i + 1], NULL, 0);
		if (sel > 1) {
			usage();
			return 2;
		}
		pmbus_dev = 1;
		phase = nct_vrm_mux_cycle(&data, sel, nct_vrm_mux_probe_body, NULL,
			nct_vrm_recover, &job, NCT_VRM_MUX_RESTORE_CHECKED);
		if (mux_recover) {
			nct_vrm_host_reject_mux_restore = 0;
			phase = nct_vrm_mux_cycle(&data, sel, nct_vrm_mux_probe_body, NULL,
				nct_vrm_recover, &job, NCT_VRM_MUX_RESTORE_CHECKED);
		}
		printf("phase=%d\n", (int)phase);
		printf("restore_failed=%d\n", phase == NCT_VRM_MUX_RESTORE);
		printf("cfg=%u\n", regs[0x61]);
		printf("baud=%u\n", regs[0x62]);
		return 0;
	}
	if (!strcmp(fn, "recover") && argc - i == 0) {
		rc = nct_vrm_recover(&data);
		printf("rc=%d\n", rc);
		dump_log();
		return rc ? 1 : 0;
	}
	usage();
	return 2;
}
