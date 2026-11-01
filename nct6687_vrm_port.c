/* One PAGE sample through /dev/port, using the kernel mailbox and millisi
 * decode. The eSIO mux window is nct_vrm_mux_cycle. No DKMS rebuild.
 *
 *   cc -Wall -Werror -Wno-unused-function -DNCT_VRM_MAILBOX_HOST -o nct6687_vrm_port nct6687_vrm_port.c
 *
 *   nct6687_vrm_port --base 0xa20 --addr 0xc0 --port 0 --page 0 --vout-exp -10
 */
#define _DEFAULT_SOURCE
#define NCT_VRM_MAILBOX_HOST 1

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dkms/nct6687_vrm_mux.h"

static int port_fd = -1;

void nct_vrm_host_outb(u16 port, u8 val)
{
	unsigned char b = val;

	if (port_fd < 0 || lseek(port_fd, port, SEEK_SET) < 0 || write(port_fd, &b, 1) != 1)
		perror("outb");
}

u8 nct_vrm_host_inb(u16 port)
{
	unsigned char b = 0;

	if (port_fd < 0 || lseek(port_fd, port, SEEK_SET) < 0 || read(port_fd, &b, 1) != 1)
		return 0;
	return b;
}

void nct_vrm_host_udelay(unsigned us)
{
	if (us)
		usleep(us);
}

void nct_vrm_host_usleep_range(unsigned min_us, unsigned max_us)
{
	(void)max_us;
	usleep(min_us ? min_us : 1);
}

struct port_job {
	u8 addr;
	u8 page;
	int vout_exp;
	int debug;
	long* sample;
	u16* raw;
	u8* vout_mode;
	u8* cap;
	u8* status;
	int temps;
	int temp2_ok;
	int temp3_ok;
	u16 temp2_raw;
	u16 temp3_raw;
	int rc;
};

/* 0x8E is the controller die, 0x8F is the TEMP pin. A status fail is a
 * no-answer. A stuck START is fatal so the mux fail path can recover.
 */
static int port_extra_temp(struct nct6687_data* data, u8 addr, u8 cmd, int* ok, u16* raw)
{
	u16 w;
	int rc = nct_vrm_read_word(data, addr, cmd, &w);

	*ok = 0;
	*raw = 0;
	if (rc == -ETIMEDOUT)
		return rc;
	if (rc)
		return 0;
	*ok = 1;
	*raw = w;
	return 0;
}

/* PAGE sample. raw receives that call's words. */
static int port_body(struct nct6687_data* data, void* vctx)
{
	struct port_job* job = vctx;

	job->rc = nct_vrm_sample_page(data, job->addr, job->page, job->vout_exp,
		job->sample, job->raw);
	if (job->rc)
		return job->rc;
	if (job->temps) {
		job->rc = port_extra_temp(data, job->addr, 0x8e, &job->temp2_ok, &job->temp2_raw);
		if (job->rc)
			return job->rc;
		job->rc = port_extra_temp(data, job->addr, 0x8f, &job->temp3_ok, &job->temp3_raw);
		if (job->rc)
			return job->rc;
	}
	*job->vout_mode = data->vrm_vout_mode_valid[job->page]
		? data->vrm_vout_mode_cache[job->page] : 0;
	return 0;
}

/* CAP 0x19 / STATUS 0x78. After SMB disable, before mux restore. */
static int port_debug(struct nct6687_data* data, void* vctx)
{
	struct port_job* job = vctx;

	if (!job->debug)
		return 0;
	if (nct_vrm_read_byte(data, job->addr, 0x19, job->cap) ||
		nct_vrm_read_byte(data, job->addr, 0x78, job->status)) {
		job->rc = -EIO;
		return -EIO;
	}
	return 0;
}

static int mux_sample(struct nct6687_data* data, u8 addr, unsigned port_mux, u8 page,
	int vout_exp, int debug, int temps, long sample[NCT_VRM_NMETRIC],
	u16 raw[NCT_VRM_NMETRIC], u8* vout_mode, u8* cap, u8* status,
	int* temp2_ok, u16* temp2_raw, int* temp3_ok, u16* temp3_raw)
{
	struct port_job job = {
		.addr = addr,
		.page = page,
		.vout_exp = vout_exp,
		.debug = debug,
		.sample = sample,
		.raw = raw,
		.vout_mode = vout_mode,
		.cap = cap,
		.status = status,
		.temps = temps,
	};
	enum nct_vrm_mux_phase phase;

	phase = nct_vrm_mux_cycle(data, port_mux, port_body, port_debug,
		nct_vrm_recover, &job, NCT_VRM_MUX_FAIL_DISABLES);
	*temp2_ok = job.temp2_ok;
	*temp2_raw = job.temp2_raw;
	*temp3_ok = job.temp3_ok;
	*temp3_raw = job.temp3_raw;
	if (phase == NCT_VRM_MUX_OK || phase == NCT_VRM_MUX_RESTORE)
		return 0;
	if (phase == NCT_VRM_MUX_BODY)
		return job.rc ? job.rc : -EIO;
	return -EIO;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: nct6687_vrm_port --base HEX --addr HEX --port 0|1 --page 0|1 "
		"--vout-exp N [--raw] [--temps]\n");
}

int main(int argc, char** argv)
{
	struct nct6687_data data;
	long sample[NCT_VRM_NMETRIC];
	u16 raw[NCT_VRM_NMETRIC];
	unsigned base = 0xa20, addr = 0xc0, port_mux = 0, page = 0;
	int vout_exp = -10, debug = 0, temps = 0, i, rc;
	int temp2_ok = 0, temp3_ok = 0;
	u16 temp2_raw = 0, temp3_raw = 0;
	u8 cap = 0, status = 0, vout_mode = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--raw")) {
			debug = 1;
		} else if (!strcmp(argv[i], "--temps")) {
			temps = 1;
		} else if (i + 1 >= argc) {
			usage();
			return 2;
		} else if (!strcmp(argv[i], "--base")) {
			base = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--addr")) {
			addr = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--port")) {
			port_mux = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--page")) {
			page = (unsigned)strtoul(argv[++i], NULL, 0);
		} else if (!strcmp(argv[i], "--vout-exp")) {
			vout_exp = (int)strtol(argv[++i], NULL, 0);
		} else {
			usage();
			return 2;
		}
	}
	if (page > 1 || port_mux > 1) {
		usage();
		return 2;
	}

	memset(&data, 0, sizeof(data));
	data.addr = base;
	data.vrm_smbus_page = -1;
	port_fd = open("/dev/port", O_RDWR | O_SYNC);
	if (port_fd < 0) {
		perror("/dev/port");
		return 1;
	}
	memset(raw, 0, sizeof(raw));
	rc = mux_sample(&data, (u8)addr, port_mux, (u8)page, vout_exp, debug, temps,
		sample, raw, &vout_mode, &cap, &status, &temp2_ok, &temp2_raw, &temp3_ok,
		&temp3_raw);
	nct_vrm_recover(&data);
	close(port_fd);
	port_fd = -1;
	if (rc) {
		fprintf(stderr, "PAGE sample failed (%d)\n", rc);
		return 2;
	}
	printf("page=%u\n", page);
	printf("addr=%u\n", addr);
	printf("base=%u\n", base);
	printf("vout_mv=%ld\n", sample[NCT_VRM_VOUT]);
	printf("vin_mv=%ld\n", sample[NCT_VRM_VIN]);
	printf("iout_ma=%ld\n", sample[NCT_VRM_IOUT]);
	printf("pout_uw=%ld\n", sample[NCT_VRM_POUT]);
	printf("temp_mc=%ld\n", sample[NCT_VRM_TEMP]);
	printf("iin_ma=%ld\n", sample[NCT_VRM_IIN]);
	printf("pin_uw=%ld\n", sample[NCT_VRM_PIN]);
	printf("eff_pcm=%ld\n", nct_vrm_efficiency_pcm(sample[NCT_VRM_POUT], sample[NCT_VRM_PIN]));
	printf("vout_mode=%u\n", vout_mode);
	printf("vout_raw=%u\n", raw[NCT_VRM_VOUT]);
	printf("vin_raw=%u\n", raw[NCT_VRM_VIN]);
	printf("iout_raw=%u\n", raw[NCT_VRM_IOUT]);
	printf("pout_raw=%u\n", raw[NCT_VRM_POUT]);
	printf("temp_raw=%u\n", raw[NCT_VRM_TEMP]);
	printf("iin_raw=%u\n", raw[NCT_VRM_IIN]);
	printf("pin_raw=%u\n", raw[NCT_VRM_PIN]);
	if (debug) {
		printf("capability=%u\n", cap);
		printf("status=%u\n", status);
	}
	if (temps) {
		printf("temp2_ok=%d\n", temp2_ok);
		printf("temp2_raw=%u\n", temp2_raw);
		printf("temp3_ok=%d\n", temp3_ok);
		printf("temp3_raw=%u\n", temp3_raw);
	}
	return 0;
}
