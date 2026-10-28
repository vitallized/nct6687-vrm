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
#include "../dkms/nct6687_vrm_mailbox.h"

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
		"                   sample_page ADDR PAGE VOUT_EXP\n");
}

static void pm_log(const char* fmt, unsigned a, unsigned b)
{
	if (pm_nops < 256)
		snprintf(pm_ops[pm_nops++], sizeof(pm_ops[0]), fmt, a, b);
}

/* One PMBus transaction at START; returns the status byte for reg 0x03. */
static u8 pm_transact(void)
{
	u8 cmd = regs[0x66];
	u8 sts;

	switch (regs[0x63]) {
	case NCT_VRM_PROTO_WBR:
		pm_log("write_byte 0x%02x 0x%02x", cmd, regs[0x70]);
		sts = pm_fail[PM_WRITE_BYTE][cmd];
		if (!sts && cmd == 0x00)
			pm_page = regs[0x70];
		return sts;
	case NCT_VRM_PROTO_RB:
		pm_log("read_byte 0x%02x", cmd, 0);
		sts = pm_fail[PM_READ_BYTE][cmd];
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
		return pm_fail[PM_READ_WORD][cmd];
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
			regs[0x60] = val & ~SMB_START;
		} else if ((win_index == 0x03 || win_index == 0x04) && val == 0xff)
			regs[win_index] = 0;
		else
			regs[win_index] = val;
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

void nct_vrm_host_usleep_range(unsigned min_us, unsigned max_us)
{
	(void)min_us;
	(void)max_us;
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
			sample);
		printf("rc=%d\n", rc);
		for (op = 0; op < pm_nops; op++)
			printf("pmbus %s\n", pm_ops[op]);
		if (!rc)
			printf("out=%ld %ld %ld %ld %ld\n", sample[NCT_VRM_VOUT],
				sample[NCT_VRM_VIN], sample[NCT_VRM_IOUT],
				sample[NCT_VRM_POUT], sample[NCT_VRM_TEMP]);
		dump_vrm_cache(&data);
		return rc ? 1 : 0;
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
