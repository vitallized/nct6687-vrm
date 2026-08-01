# nct6687-vrm

## Disclaimer

Developed with **AI assistance**. Experimental hardware tooling — **no warranty**.

- Misuse can hang the embedded controller; recovery may need a **full power cycle** (not only a reboot).
- Review scripts before running as root.
- Patches **out-of-tree DKMS sources** in place. Upstream `nct6687d` changes can break the inject; re-run after upgrades (or install the pacman hook).
- Always bring the module up with **`vrm=0` first**, confirm fans/temps, then enable VRM.

## What this is

CPU VRM voltage, current, power, and temperature on Linux — via the NCT6687 **eSIO SMBus**, the same path HWiNFO uses on Windows.

The DKMS patch keeps a **small hook splice** in `nct6687.c` and puts the VRM implementation in [`dkms/nct6687_vrm.inc.c`](dkms/nct6687_vrm.inc.c) (`#include`'d at build time). Upstream driver updates are more likely to break a few anchors than a giant inlined blob.

| | |
|--|--|
| **Proven** | MSI MPG Z790 CARBON WIFI (**MS-7D89**), Renesas multiphase @ PMBus `0xC0` |
| **Needs** | [`nct6687d`](https://github.com/Fred78290/nct6687d) already providing fans/temps (`nct6687.ko`) |
| **Adds** | hwmon channels for VRM (optional GT/iGPU page) |

Other MSI boards with the same EC + VR wiring may work. Confirm `0xC0` with the [userspace reader](#userspace-reader-optional) before leaving `vrm=1` enabled. Protocol details: [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Background

[HWiNFO](https://www.hwinfo.com/) already reads these VRM rails on Windows through the NCT6687 **eSIO SMBus** host — Super-I/O LDN `0x0B` BAR, data window at EC `base+4/+5/+6`, PMBus address `0xC0`, PAGE `0` = CPU Vcore / PAGE `1` = GT/iGPU. The same path works on Linux; MSI’s WMI/BIOS mailbox does not expose this telemetry.

This repo turns that into a DKMS patch and an optional userspace reader. Not affiliated with HWiNFO. More detail (and what not to poke): [docs/PROTOCOL.md](docs/PROTOCOL.md).

## Requirements

- Root on Linux
- `nct6687d-dkms` / `nct6687d-dkms-git` (or equivalent) installed and working
- Matching kernel headers for DKMS rebuilds

`vrm` is a load-time module parameter. Changing it (or picking up a newly built `.ko`) means **unload + reload** `nct6687`. That only fails if something still holds the module (`modprobe -r` errors) — then quit whatever is using the NCT6687 hwmon and retry.

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

Cache defaults to **1 Hz**; polling VRM hwmon faster raises the sample rate down to ~**20 ms**. Fast polling shares `EC_io_lock` with fans/temps (can delay those reads).

GT / iGPU (PMBus PAGE 1), usually idle with a discrete GPU:

```sh
sudo modprobe -r nct6687
sudo modprobe nct6687 vrm=1 vrm_gt=1
```

→ `in22` / `in23` / `curr2` / `power2` / `temp21`

### Persist (Arch)

Keeps `vrm=1` across reboot and re-applies the patch when `nct6687d-dkms-git` is upgraded:

```sh
sudo bash ./pacman-hook/install.sh
```

Audit before running — that script installs:

| Path | Source in this repo |
|------|---------------------|
| `/usr/local/lib/nct6687-vrm/nct6687_vrm_dkms_inject.py` | `nct6687_vrm_dkms_inject.py` |
| `/usr/local/lib/nct6687-vrm/nct6687_vrm.inc.c` | `dkms/nct6687_vrm.inc.c` (VRM implementation `#include`'d into the driver) |
| `/usr/local/sbin/nct6687-vrm-reinject` | `pacman-hook/nct6687-vrm-reinject` |
| `/etc/pacman.d/hooks/nct6687-vrm-reinject.hook` | `pacman-hook/nct6687-vrm-reinject.hook` |
| `/etc/modprobe.d/nct6687-vrm.conf` | `pacman-hook/nct6687-vrm.conf` (`options nct6687 vrm=1`) |

The hook rebuilds on disk during pacman; it does **not** unload the running module mid-transaction. Reboot or reload later to pick up a post-upgrade build.

After `git pull` changes to the inject/include, re-run `pacman-hook/install.sh` so `/usr/local` stays in sync.
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
           /etc/modprobe.d/nct6687-vrm.conf \
           /usr/local/sbin/nct6687-vrm-reinject
sudo rm -rf /usr/local/lib/nct6687-vrm
```

## Userspace reader (optional)

One-shot / debug without patching the kernel. Prefer with `nct6687` **unloaded**. `--force` races the driver’s EC window — fine for a quick check, not for continuous use.

```sh
sudo python3 ./nct6687_vrm.py --page 0
sudo python3 ./nct6687_vrm.py --page 0 --force   # if the module is loaded
```

Do **not** run this while the module has `vrm=1`.

## License

MIT — see [LICENSE](LICENSE).
