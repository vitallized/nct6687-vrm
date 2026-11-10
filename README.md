# nct6687-vrm

## Disclaimer

Developed with AI assistance. This is experimental hardware work. No warranty.

Misuse can hang the embedded controller. A reboot will not get you out of that. You need a full power cycle.

Review scripts before running them as root. The inject patches out-of-tree DKMS sources in place. Upstream `nct6687d` changes can break the anchors, so re-run after upgrades or install the pacman hook.

Load with `vrm=0` first. Check fans and board temps. Then enable VRM.

## What this is

CPU VRM voltage, current, power, and temperature on Linux, read through the NCT6687 eSIO SMBus.

The DKMS splice is a small hook in `nct6687.c`. The VRM implementation is `#include`'d from [`dkms/nct6687_vrm.inc.c`](dkms/nct6687_vrm.inc.c); decode, the eSIO mailbox, and the `nct6687_data` members live in sibling headers. Upstream driver updates are more likely to break a few anchors than a giant inlined blob. That's the point.

Proven on MSI MPG Z790 CARBON WIFI, MS-7D89, Renesas multiphase at PMBus `0xC0`.
Needs [`nct6687d`](https://github.com/Fred78290/nct6687d) already providing fans and temps via `nct6687.ko`.
Adds hwmon channels for the CPU VRM, and optionally the GT/iGPU page.

Other MSI boards with the same EC and VR wiring may work. Confirm `0xC0` with the [userspace reader](#userspace-reader-optional) before leaving `vrm=1` on. Protocol details are in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Background

These rails are read through the NCT6687 eSIO SMBus host. Super-I/O LDN `0x0B` BAR, data window at EC `base+4/+5/+6`, PMBus address `0xC0`. PAGE `0` is CPU Vcore. PAGE `1` is GT/iGPU.

That path works on Linux. MSI's WMI/BIOS mailbox does not. This repo is the DKMS patch plus an optional userspace reader. More on what not to poke: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Requirements

- Root on Linux
- `nct6687d-dkms` or `nct6687d-dkms-git`, or anything else that already ships a working `nct6687.ko`
- Matching kernel headers for DKMS rebuilds

`vrm` is a load-time module parameter. Changing it, or picking up a newly built `.ko`, means unload and reload `nct6687`. `modprobe -r` fails if something still holds the module. Quit whatever is using the NCT6687 hwmon and retry.

## Install

```sh
git clone https://github.com/vitallized/nct6687-vrm.git
cd nct6687-vrm

# 1) Compile-check (does not change the live module)
sudo python3 ./nct6687_vrm_dkms_inject.py --verify-compile

# 2) Patch DKMS sources, rebuild, reload with vrm=0
sudo python3 ./nct6687_vrm_dkms_inject.py --install

# 3) Confirm fans / board temps still look normal (`sensors`, etc.)

# 4) Enable VRM
sudo modprobe -r nct6687
sudo modprobe nct6687 vrm=1
```

Verify:

```sh
cat /sys/module/nct6687/parameters/vrm    # Y
sensors
```

### Sensors

| Sysfs | Meaning | Unit |
|-------|---------|------|
| `in20_input` | CPU VOUT | mV |
| `in21_input` | CPU VIN | mV |
| `curr1_input` | CPU IOUT (`READ_IOUT`, 0.1 A/count) | mA |
| `curr3_input` | CPU IIN (`READ_IIN`, 0.01 A/count) | mA |
| `power1_input` | CPU POUT | µW |
| `power1_input_lowest` / `power1_input_highest` | CPU POUT software hist | µW |
| `power3_input` | CPU PIN (`READ_PIN`, 1 W/count) | µW |
| `power3_input_lowest` / `power3_input_highest` | CPU PIN software hist | µW |
| `pwm9` | VRM EFFICIENCY, `POUT/PIN` capped at 100% | count (`sensors` prints count/2 as `%`; 100% is 200) |
| `temp20_input` | VRM TEMP, hottest power stage (`0x8D`) | m°C |
| `temp22_input` | VRM CONTROLLER, controller die (`0x8E`) | m°C |
| `temp3_min` / `temp3_max` | VRM MOS software hist | m°C |
| `vrm_cpu` | CPU VOUT + IOUT + POUT (one read) | mV mA µW |
| `vrm_cpu_full` | CPU VOUT VIN IOUT IIN POUT PIN (one read) | mV mV mA mA µW µW |
| `vrm_sample_age` | Age of the published PAGE sample right now. Reading it does not start a refresh | ms |
| `vrm_sample_age_at_read` | Age the last demand burst saw, frozen then. Reading it does not start a refresh | ms |
| `vrm_prefetch_state` | `prefetch` or `fallback`, then the learned interval | ms |
| `vrm_busy_ns` | Cumulative ns the refresh spent running (port I/O, START-poll checks, decode), then how many refreshes that covers. Sleep between polls is not included. Reading it does not start a refresh. Divide the two for CPU ns per sample | ns count |

A VRM sysfs read returns the cached PAGE sample and does not wait on the eSIO mailbox. Once the gap between read bursts is steady, the refresh starts early so the sample is a few tens of milliseconds old at the next read. `vrm_sample_age` is that age in milliseconds. The first reads after load, and a gap more than about 30% off the recent interval, still refresh when the read arrives. Reads that stop are not followed by more EC traffic. Background (fan/temp hook) stays at 1 Hz and uses that same refresh, but only while a VRM attribute was read in the last 10 s: fan/temp readers alone cause no PMBus traffic. The refresh follows the inter-read gap down to about 20 ms. Consecutive sample failures back off (about 4 Hz, then 1 Hz, 0.5 Hz, cap 8 s) so a wedged VR does not hold `EC_io_lock` at 4 Hz forever. Each failure also logs, ratelimited, which PMBus command failed and how (START did not clear, or an error status), and what the SMBus master's control (`0x60`), port (`0x61`) and speed (`0x62`) registers held at that moment: `nct6687: VRM sample failed: last PMBus cmd 0x8c (proto 0x83) error status, status 0x02; master ctrl 0x60=… port/cfg 0x61=… speed 0x62=…`. The refresh still takes that lock, but a fan, temp, or voltage read does not wait on it: the 1 Hz scan runs on the worker. VRM sysfs does not run the full fan/temp scan. `vrm_cpu` returns VOUT, IOUT, and POUT in one read. A negative IOUT, IIN, POUT, or PIN, and a VRM TEMP or VRM CONTROLLER reading above 200 °C, stay off `sensors`: the live value keeps the last published reading, hist ignores it, and `dmesg` gets a ratelimited `nct6687: VRM … not published` line. POUT hist is `power1_input_lowest` / `power1_input_highest` and PIN hist is `power3_input_lowest` / `power3_input_highest` so `sensors` prints `lowest` then `highest`. `power*_min` / `power*_max` would print max-then-min, and power has no `low` label. VRM MOS hist is `temp3_min` / `temp3_max` (`sensors` prints `low` / `high`). VRM TEMP stays `temp20_min` / `temp20_max`. VRM CONTROLLER is `temp22_input` plus `temp22_min` / `temp22_max`. Efficiency is not a register. It is smoothed POUT and PIN (about 10 s; `vrm_eff_tau=N` seconds at load, 1–60), capped at 100%, published as read-only `pwm9` so `sensors` prints a plain `%`. It reads N/A until 5 samples are in, and until the window has filled it is a plain mean of the samples so far, so one odd first sample cannot pin it at 100%. A single sample is not a usable efficiency: the controller's words come from different instants of a load that moves faster than one sample (under a game load, one sample's POUT/PIN is off by about 19 points and four in ten read POUT ≥ PIN). Measured spread of the readout at 5 samples a second under that load: 10 s ±3, 5 s ±5, 3 s ±6.5, 1 s ±10 points. A sample whose POUT and PIN differ by more than `NCT_VRM_EFF_REJECT_X100` / 100 is left out of that average. That constant is 10000, so the check is off for real spikes. A gap of more than 5 s, or a failed sample, clears the smoothing. `pwm1`–`pwm8` stay the fan duties. There is no `pwm9_enable`. Either power at or below zero hides the value. Whole percents only. libsensors 3.6.2 keeps four characters of a pwm name, so there is no `pwm10` and no GT efficiency line.

**Lite samples.** `vrm_cpu` shows VOUT, IOUT and POUT only, so while it is the only VRM attribute being read, a sample reads just those three words. The other words, the controller temperature, efficiency and GT keep the last full sample, and a full sample still runs at least every 2 s. Any other VRM read (`in*`, `curr*`, `power*`, `temp2x`, `pwm9`, `vrm_cpu_full`) makes every sample full for the next 5 s, so the values a full reader shows come from one moment. A HUD that polls only `vrm_cpu` costs 3 words a sample instead of 8.

**`vrm_fast=1`** (load time, default 0) uses `inb`/`outb` instead of `inb_p`/`outb_p`, which add a write to port 0x80 after each of the ~600 port accesses a sample makes. The stock fan/temp/voltage register reads (about 270 accesses per 1 Hz scan) follow the same bit. On MS-7D89 this, with lite samples and the idle check below, took a HUD sample from 10.4 ms to 1.9 ms of CPU. Compare yours with `vrm_busy_ns` (ns ÷ samples). Inside a refresh the idle check runs once: after its own first access the session knows it left the window idle.

**Mailbox shortcuts (`vrm_fast` bits 2, 4, 8, 16; trial).** Each skips a step of the per-word mailbox sequence, using state the refresh itself wrote earlier in the same refresh: `2` keeps the control register value instead of reading it back, `4` does not rewrite the protocol and address when they are unchanged, `8` drops the CLEAR pulse after a transaction that ended with status 0, and `16` starts each transaction with one write of `EN|START` instead of `EN`, a 20 µs wait, then `EN|START` (HWiNFO 8.55 does the same on this EC). All three (`vrm_fast=15`) cut port accesses per sample by about a fifth (HUD) to a third (full); on MS-7D89 a HUD sample went from 2.1 to 1.7 ms of CPU. Bit 16 on top of that (`vrm_fast=31`) took a full sample from 3.56 to 3.09 ms of CPU on MS-7D89 at idle (2026-11-09, two 100-sample runs each), and from 3.73 to 3.39 ms with a game, the VRM HUD at 5 Hz and a sensor app running (five 1-minute windows each, interleaved, about 1,200 samples per setting, lower in every window). How long a refresh takes did not change. The shipped modprobe conf sets `vrm_fast=31`. A failed sample, or a VOUT of 0 V, turns them off until the module reloads and logs `VRM sample failed with vrm_fast shortcuts …` once.

**`vrm_mux_keep=1`** (load time, default 0; trial) leaves the SMBus master's port (`0x61`) and speed (`0x62`) as the firmware set them, as HWiNFO does, whenever port 0 is already selected; otherwise a refresh still forces port 0 and speed `0x03` and puts the firmware's values back. On MS-7D89 the firmware leaves `0x61=0x04` and `0x62=0x03`, which is port 0 and the speed the driver forces anyway, so this only skips four register writes per refresh. Under the game load above that was 0.13 ms of CPU per sample (3.39 to 3.26 ms on top of `vrm_fast=31`, lower in all five windows), with no failed samples in 2,500. The values the firmware left are logged once after load (`VRM eSIO SMBus master as the firmware left it`). The shipped conf leaves it off; set `vrm_mux_keep=1` on its line in `/etc/modprobe.d/nct6687-vrm.conf` to turn it on. The persist install never overwrites that file once it exists.

Keeping page 4 selected across a transaction (INDEX + DATA per access) was tried and does not work on this EC: every word reads back 0. It needs 0xFF and then the page before each access, as the stock driver does.

GT / iGPU is PMBus PAGE 1. Usually idle if you have a discrete GPU:

```sh
sudo modprobe -r nct6687
sudo modprobe nct6687 vrm=1 vrm_gt=1
```

That adds `in22`, `in23`, `curr2`, `curr4`, `power2`, `power4`, `temp21`.

### Persist (Arch)

Keeps `vrm=1` across reboot and re-applies the splice when `nct6687d` sources or kernel headers land (linux-only topgrade included):

```sh
sudo bash ./pacman-hook/install.sh
# same thing:
sudo python3 ./nct6687_vrm_persist.py install
```

That copies **this checkout** into `/usr/local` (`SOURCE_REPO=` here). A dirty or behind tree is what the next upgrade will re-apply. Audit before running. It installs:

| Path | Source in this repo |
|------|---------------------|
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_dkms_inject.py` | `nct6687_vrm_dkms_inject.py` |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_persist.py` | `nct6687_vrm_persist.py` (pacman pre/post) |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm.inc.c` | `dkms/nct6687_vrm.inc.c` (VRM implementation `#include`'d into the driver) |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_decode.h` | `dkms/nct6687_vrm_decode.h` (raw→millisi math, also used by host tests) |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_data.h` | `dkms/nct6687_vrm_data.h` (VRM members of `nct6687_data`) |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_mailbox.h` | `dkms/nct6687_vrm_mailbox.h` (eSIO mailbox; host-tested) |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_demand.h` | `dkms/nct6687_vrm_demand.h` (VRM demand session; host-tested) |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_prefetch.h` | `dkms/nct6687_vrm_prefetch.h` (when the next refresh starts; host-tested) |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_hist.h` | `dkms/nct6687_vrm_hist.h` (software hist for PAGE rails and VRM MOS) |
| `/usr/local/lib/nct6687-vrm/vrm-splice.patch` | `patches/vrm-splice.patch` (overlay `-p1`) |
| `/usr/local/lib/nct6687-vrm/source.env` | written at install (`SOURCE_REPO=` this checkout) |
| `/etc/pacman.d/hooks/nct6687-vrm-preupgrade.hook` | `pacman-hook/nct6687-vrm-preupgrade.hook` |
| `/etc/pacman.d/hooks/nct6687-vrm-reinject.hook` | `pacman-hook/nct6687-vrm-reinject.hook` |
| `/etc/modprobe.d/nct6687-vrm.conf` | `pacman-hook/nct6687-vrm.conf` (every VRM option on its own line, with what it does and its range; written once, then yours to edit) |

The pre-upgrade hook deletes unowned overlay leftovers in every `/usr/src/nct6687d*` directory, including leftover trees that no longer have `nct6687.c`. That means the VRM include, sibling headers, `*.pre-vrm` backups, leftover `Kbuild`. Pacman can then extract newly packaged files. This is what blocked `nct6687d-dkms-git` when upstream started shipping `Kbuild`. Run `pre_transaction` only as that PreTransaction hook — alone it also strips the **live** overlay extras. `post_transaction` (or `--install`) puts them back.

The post-upgrade hook rebuilds on disk. It does not unload the running module mid-transaction. Reboot or reload later to pick up the new build. If the splice patch no longer applies, the hook fails and leaves stock `nct6687`. If the splice applied and DKMS rebuild then exits, the hook returns 1 and **leaves the tree spliced**.

`refresh` updates `/usr/local` and any `/etc` hook files that `install` already wrote. It does not invent `/etc` hooks. `--check` from `/usr/local` compares the installed payload to `SOURCE_REPO`, not to itself.

Re-run `install.sh` after moving the repo, or to refresh immediately.

Status: `python3 ./nct6687_vrm_dkms_inject.py --check`

## Rollback

```sh
# Disable VRM only
sudo modprobe -r nct6687
sudo modprobe nct6687 vrm=0

# Restore stock nct6687.c and rebuild
sudo python3 ./nct6687_vrm_dkms_inject.py --restore --rebuild
# also removes the VRM include and headers from the DKMS tree

# Remove persist bits (if you installed the hook)
sudo rm -f /etc/pacman.d/hooks/nct6687-vrm-reinject.hook \
           /etc/pacman.d/hooks/nct6687-vrm-preupgrade.hook \
           /etc/modprobe.d/nct6687-vrm.conf
sudo rm -rf /usr/local/lib/nct6687-vrm
```

## Userspace reader (optional)

One-shot debug without patching the kernel. Prefer `nct6687` unloaded. `--force` races the driver's EC window for a single sample. `--loop` is refused while the module is loaded, even with `--force`.

```sh
sudo python3 ./nct6687_vrm.py --page 0
sudo python3 ./nct6687_vrm.py --page 0 --force   # if the module is loaded
```

Do not run this while the module has `vrm=1`.

## License

MIT. See [LICENSE](LICENSE).
