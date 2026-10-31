# NCT6687 eSIO VRM

CPU (and optional GT) VRM telemetry read through the NCT6687 eSIO SMBus master, then published as hwmon.

## Language

**eSIO window**:
The NCT6687 EC page/index/data ports used to talk to the on-chip SMBus master.
_Avoid_: LPC mailbox, Super-I/O dump, classic NCT SMBus host

**eSIO mailbox**:
One SMBus-master transaction (proto, START, status, payload) over the eSIO window.
_Avoid_: ByteIO, C ByteIO, host SMBus

**PAGE sample**:
One PMBus PAGE’s VOUT, VIN, IOUT, IIN, POUT, PIN, and TEMP after decode to millisi. IOUT is `READ_IOUT` (Direct, 0.1 A per count). IIN is `READ_IIN` (Direct, 0.01 A per count). PIN is `READ_PIN` (Direct, 1 W per count). Read order is TEMP, VOUT, VIN, IOUT, IIN, POUT, PIN. Efficiency is `POUT/PIN` capped at 100%, not another register.
_Avoid_: rail decode, full scan, sensor poll

**millisi**:
Integer thousandths used by both kernel C and the userspace reader (mV, mA, mW, m°C).
_Avoid_: SI float, raw PMBus word

**DKMS splice**:
The small hooks pasted into distro `nct6687.c` so the VRM include builds inside `nct6687.ko`.
_Avoid_: second module, fork, vendor tree

**overlay**:
This project mutates the distro `nct6687d` DKMS tree after install and again after upgrade. It does not ship `nct6687d`.
_Avoid_: vendor, pin, owned nct6687d package

**VRM data header**:
The members that live on `nct6687_data` for VRM cache, hist, and PAGE/VOUT_MODE state. Spliced as an include inside that struct.
_Avoid_: STRUCT_FIELDS, Python field list

**software hist**:
Per-device min/max of PAGE samples, plus the stock VRM MOS thermistor (`temp3`). IOUT, IIN (mA) and POUT, PIN (µW) drop a negative millisi sample. POUT can still be a LINEAR11 spike. The current and PIN reads are unsigned Direct, so that drop is only a guard. VOUT/VIN/TEMP still take the signed value. POUT and PIN are published as `power*_input_lowest` / `power*_input_highest` so `sensors` prints lowest then highest.
_Avoid_: hwmon chip min, HWiNFO peak

**VRM demand**:
A VRM sysfs read (not a fan/temp attr). First demand after idle, or first-ever, uses the 20 ms floor. Background (fan/temp hook) stays 1 Hz. Consecutive PAGE failures back off (HZ/4 → 8 Hz).
_Avoid_: 1 Hz as the HUD rate

**upgrade persist**:
Re-apply the DKMS splice after a distro `nct6687d` upgrade without unloading the live module. `pre_transaction` is PreTransaction-only — alone it strips live overlay extras; `post_transaction` puts them back.
_Avoid_: install path, --install re-splice

**splice patch**:
The committed `-p1` that is the overlay apply language for the DKMS splice.
_Avoid_: inject_text apply, exact-string inject on the live tree
