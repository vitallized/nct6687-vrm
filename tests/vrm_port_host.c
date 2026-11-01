/* Host fake for nct6687_vrm_port --raw. No /dev/port.
 *
 *   cc -Wall -Werror -Wno-unused-function -DNCT_VRM_MAILBOX_HOST -DNCT_VRM_PORT_TEST \
 *      -o vrm_port_host tests/vrm_port_host.c nct6687_vrm_port.c
 */
#include <stdio.h>
#include <string.h>

#define BASE 0xa20u
#define SMB_START 0x40u
#define SMB_EN 0x80u

static unsigned char page = 0xff;
static unsigned char win_index;
static unsigned char regs[256];
static unsigned char pm_page;
static int en_during_mux;
static int saw_cap;
static int saw_status;

int nct_vrm_port_trace(int debug, unsigned char* cap, unsigned char* status);

void nct_vrm_host_outb(unsigned short port, unsigned char val)
{
	if (port == BASE + 4)
		page = val;
	else if (port == BASE + 5)
		win_index = val;
	else if (port == BASE + 6) {
		if ((win_index == 0x61 || win_index == 0x62) && (regs[0x60] & SMB_EN))
			en_during_mux++;
		if (win_index == 0x60) {
			if ((val & SMB_START) && regs[0x63] == 0x02 && regs[0x66] == 0x00)
				pm_page = regs[0x70];
			if ((val & SMB_START) && regs[0x63] == 0x82) {
				if (regs[0x66] == 0x19)
					saw_cap = 1;
				if (regs[0x66] == 0x78)
					saw_status = 1;
				regs[0xb0] = (regs[0x66] == 0x00) ? pm_page : regs[regs[0x66]];
			}
			regs[0x03] = 0;
			regs[0x60] = val & ~SMB_START;
		} else if ((win_index == 0x03 || win_index == 0x04) && val == 0xff)
			regs[win_index] = 0;
		else
			regs[win_index] = val;
	}
}

unsigned char nct_vrm_host_inb(unsigned short port)
{
	if (port == BASE + 4)
		return page;
	if (port == BASE + 5)
		return win_index;
	if (port == BASE + 6)
		return regs[win_index];
	return 0;
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

int nct_vrm_port_refuse(int loaded, int force);

int main(int argc, char** argv)
{
	unsigned char cap = 0, status = 0;
	int debug = argc > 1 && !strcmp(argv[1], "--raw");
	int rc;

	if (argc > 1 && !strcmp(argv[1], "--refuse")) {
		printf("loaded=%d\n", nct_vrm_port_refuse(1, 0));
		printf("force=%d\n", nct_vrm_port_refuse(1, 1));
		printf("clear=%d\n", nct_vrm_port_refuse(0, 0));
		return 0;
	}

	regs[0x20] = 0x40;
	regs[0x19] = 0x19;
	regs[0x78] = 0x78;
	regs[0x61] = 0x05;
	regs[0x62] = 0x11;
	page = 0xff;
	rc = nct_vrm_port_trace(debug, &cap, &status);
	printf("rc=%d\n", rc);
	printf("en_during_mux=%d\n", en_during_mux);
	printf("saw_cap=%d\n", saw_cap);
	printf("saw_status=%d\n", saw_status);
	printf("cap=%u\n", cap);
	printf("status=%u\n", status);
	printf("cfg=%u\n", regs[0x61]);
	printf("baud=%u\n", regs[0x62]);
	return 0;
}
