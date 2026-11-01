/* Published hwmon filenames for the VRM group.
 *
 * sensors picks the label from the filename. power*_input_lowest /
 * power*_input_highest print lowest then highest. power*_min / power*_max
 * would print max then min, and power has no low label. temp*_min /
 * temp*_max print low / high: temp3 is VRM MOS, temp20 is VRM TEMP,
 * temp22 is VRM CONTROLLER.
 * Efficiency is read-only pwm9 plus pwm9_label. pwm10 is absent: libsensors
 * 3.6.2 keeps four characters of a pwm name, so pwm10 aliases fan pwm1.
 * No pwm9_enable. No humidity channel.
 *
 * Host-only, included by tests/vrm_decode.c. Same name contract as the
 * SENSOR_DEVICE_ATTR identifiers in dkms/nct6687_vrm.inc.c (order matches
 * nct6687_vrm_attrs). The kernel TU does not include this file.
 * `vrm_decode attr_names` prints one name per line. NULL terminates.
 */
#ifndef VRM_ATTR_NAMES_H
#define VRM_ATTR_NAMES_H

static const char* const nct_vrm_published_attr_names[] = {
	"vrm_cpu",
	"in20_input",
	"in20_label",
	"in20_min",
	"in20_max",
	"in21_input",
	"in21_label",
	"in21_min",
	"in21_max",
	"curr1_input",
	"curr1_label",
	"curr1_min",
	"curr1_max",
	"curr3_input",
	"curr3_label",
	"curr3_min",
	"curr3_max",
	"power1_input",
	"power1_label",
	"power1_input_lowest",
	"power1_input_highest",
	"power3_input",
	"power3_label",
	"power3_input_lowest",
	"power3_input_highest",
	"pwm9",
	"pwm9_label",
	"temp20_input",
	"temp20_label",
	"temp20_min",
	"temp20_max",
	"temp22_input",
	"temp22_label",
	"temp22_min",
	"temp22_max",
	"temp3_min",
	"temp3_max",
	"in22_input",
	"in22_label",
	"in22_min",
	"in22_max",
	"in23_input",
	"in23_label",
	"in23_min",
	"in23_max",
	"curr2_input",
	"curr2_label",
	"curr2_min",
	"curr2_max",
	"curr4_input",
	"curr4_label",
	"curr4_min",
	"curr4_max",
	"power2_input",
	"power2_label",
	"power2_input_lowest",
	"power2_input_highest",
	"power4_input",
	"power4_label",
	"power4_input_lowest",
	"power4_input_highest",
	"temp21_input",
	"temp21_label",
	"temp21_min",
	"temp21_max",
	NULL,
};

#endif
