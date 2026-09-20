/*
 * Host compile of dkms/nct6687_vrm_mailbox.h — no linux headers, no .ko,
 * no /dev/port. pytest drives this against the same sequences as
 * tests/test_vrm_esio.py.
 *
 *   cc -Wall -Werror -DNCT_VRM_MAILBOX_HOST -o vrm_mailbox tests/vrm_mailbox.c
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

static void usage(void)
{
	fprintf(stderr,
		"usage: vrm_mailbox [--stuck-page] [--status HEX] [--b0 HEX] [--b1 HEX]\n"
		"                   esio_write INDEX VALUE\n"
		"                   esio_read PAGE INDEX\n"
		"                   write_byte ADDR CMD VALUE\n"
		"                   read_byte ADDR CMD\n"
		"                   read_word ADDR CMD\n"
		"                   recover\n");
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
				regs[0x03] = after_start_status;
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

int main(int argc, char** argv)
{
	struct nct6687_data data = { .addr = base };
	const char* fn;
	int i = 1;
	int rc;
	u8 a, b, c, out8;
	u16 out16;

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
	if (!strcmp(fn, "recover") && argc - i == 0) {
		rc = nct_vrm_recover(&data);
		printf("rc=%d\n", rc);
		dump_log();
		return rc ? 1 : 0;
	}
	usage();
	return 2;
}
