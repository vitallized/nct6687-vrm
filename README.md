# nct6687-vrm

## Disclaimer

Developed with AI assistance. This is experimental hardware work. No warranty.

Misuse can hang the embedded controller. A reboot will not get you out of that. You need a full power cycle.

Review scripts before running them as root. The inject patches out-of-tree DKMS sources in place. Upstream `nct6687d` changes can break the anchors, so re-run after upgrades or install the pacman hook.

Load with `vrm=0` first. Check fans and board temps. Then enable VRM.

## What this is

CPU VRM voltage, current, power, and temperature on Linux, read through the NCT6687 eSIO SMBus. Same path HWiNFO uses on Windows.

The DKMS patch is a small hook in `nct6687.c`. The VRM code lives in [`dkms/nct6687_vrm.inc.c`](dkms/nct6687_vrm.inc.c) and gets `#include`'d at build time. Upstream driver updates are more likely to break a few anchors than a giant inlined blob. That's the point.

Proven on MSI MPG Z790 CARBON WIFI, MS-7D89, Renesas multiphase at PMBus `0xC0`.
Needs [`nct6687d`](https://github.com/Fred78290/nct6687d) already providing fans and temps via `nct6687.ko`.
Adds hwmon channels for the CPU VRM, and optionally the GT/iGPU page.

Other MSI boards with the same EC and VR wiring may work. Confirm `0xC0` with the [userspace reader](#userspace-reader-optional) before leaving `vrm=1` on. Protocol details are in [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Background

[HWiNFO](https://www.hwinfo.com/) already reads these rails on Windows through the NCT6687 eSIO SMBus host. Super-I/O LDN `0x0B` BAR, data window at EC `base+4/+5/+6`, PMBus address `0xC0`. PAGE `0` is CPU Vcore. PAGE `1` is GT/iGPU.

That path works on Linux. MSI's WMI/BIOS mailbox does not. This repo is the DKMS patch plus an optional userspace reader. Not affiliated with HWiNFO. More on what not to poke: [docs/PROTOCOL.md](docs/PROTOCOL.md).

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
| `curr1_input` | CPU IOUT | mA |
| `power1_input` | CPU POUT | µW |
| `temp20_input` | VR temperature | m°C |
| `vrm_cpu` | CPU VOUT + IOUT + POUT (one read) | mV mA µW |

Cache defaults to 1 Hz. If something polls VRM hwmon faster, the sample rate follows that poll down to about 20 ms. Fast polling shares `EC_io_lock` with fans and temps, so those reads can stall. VRM sysfs only samples the VR, not the full fan/temp scan. `vrm_cpu` returns VOUT, IOUT, and POUT in one read.

GT / iGPU is PMBus PAGE 1. Usually idle if you have a discrete GPU:

```sh
sudo modprobe -r nct6687
sudo modprobe nct6687 vrm=1 vrm_gt=1
```

That adds `in22`, `in23`, `curr2`, `power2`, `temp21`.

### Persist (Arch)

Keeps `vrm=1` across reboot and re-applies the patch when `nct6687d-dkms-git` is upgraded:

```sh
sudo bash ./pacman-hook/install.sh
```

Audit before running. That script installs:

| Path | Source in this repo |
|------|---------------------|
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_dkms_inject.py` | `nct6687_vrm_dkms_inject.py` |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm.inc.c` | `dkms/nct6687_vrm.inc.c` (VRM implementation `#include`'d into the driver) |
| `/usr/local/lib/nct6687-vrm/refresh-from-source.sh` | `pacman-hook/refresh-from-source.sh` |
| `/usr/local/lib/nct6687-vrm/source.env` | written at install (`SOURCE_REPO=` this checkout) |
| `/usr/local/sbin/nct6687-vrm-preupgrade` | `pacman-hook/nct6687-vrm-preupgrade` |
| `/usr/local/sbin/nct6687-vrm-reinject` | `pacman-hook/nct6687-vrm-reinject` |
| `/etc/pacman.d/hooks/nct6687-vrm-preupgrade.hook` | `pacman-hook/nct6687-vrm-preupgrade.hook` |
| `/etc/pacman.d/hooks/nct6687-vrm-reinject.hook` | `pacman-hook/nct6687-vrm-reinject.hook` |
| `/etc/modprobe.d/nct6687-vrm.conf` | `pacman-hook/nct6687-vrm.conf` (`options nct6687 vrm=1`) |

The pre-upgrade hook deletes unowned files in `/usr/src/nct6687d*`. That means the VRM include, `*.pre-vrm` backups, leftover `Kbuild`. Pacman can then extract newly packaged files. This is what blocked `nct6687d-dkms-git` when upstream started shipping `Kbuild`.

The post-upgrade hook rebuilds on disk. It does not unload the running module mid-transaction. Reboot or reload later to pick up the new build.

If this checkout still exists at `SOURCE_REPO`, both helpers copy a newer inject/include into `/usr/local` first, so a `git pull` is enough for the next upgrade. Re-run `install.sh` after moving the repo, or to refresh immediately.

Status: `python3 ./nct6687_vrm_dkms_inject.py --check`

## Rollback

```sh
# Disable VRM only
sudo modprobe -r nct6687
sudo modprobe nct6687 vrm=0

# Restore stock DKMS sources (nct6687.c + Makefile) and rebuild
sudo python3 ./nct6687_vrm_dkms_inject.py --restore --rebuild
# also removes nct6687_vrm.inc.c from the DKMS tree

# Remove persist bits (if you installed the hook)
sudo rm -f /etc/pacman.d/hooks/nct6687-vrm-reinject.hook \
           /etc/pacman.d/hooks/nct6687-vrm-preupgrade.hook \
           /etc/modprobe.d/nct6687-vrm.conf \
           /usr/local/sbin/nct6687-vrm-reinject \
           /usr/local/sbin/nct6687-vrm-preupgrade
sudo rm -rf /usr/local/lib/nct6687-vrm
```

## Userspace reader (optional)

One-shot debug without patching the kernel. Prefer `nct6687` unloaded. `--force` races the driver's EC window. Fine for a quick check. Do not loop it.

```sh
sudo python3 ./nct6687_vrm.py --page 0
sudo python3 ./nct6687_vrm.py --page 0 --force   # if the module is loaded
```

Do not run this while the module has `vrm=1`.

## License

MIT. See [LICENSE](LICENSE).
