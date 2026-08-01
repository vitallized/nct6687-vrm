# nct6687-vrm

Linux VRM telemetry (voltage / current / power / temp) for MSI boards that expose a Renesas PMBus controller through the **NCT6687 eSIO SMBus**, the same path HWiNFO uses on Windows.

**Proven on:** MSI MPG Z790 CARBON WIFI (**MS-7D89**), BIOS with NCT6687 EC, CachyOS/Arch + [`nct6687d`](https://github.com/FredrIQ/nct6687d) DKMS.

Other MSI Z790 (and similar) boards with the same EC + VR wiring may work; confirm PMBus at `0xC0` before enabling. See [docs/PROTOCOL.md](docs/PROTOCOL.md).

## AI / safety disclaimer

This project was developed with **AI assistance**. Treat it as experimental hardware tooling:

- **No warranty.** Misuse can hang the embedded controller; recovery may need a full power cycle (not just reboot).
- Review the scripts before running them as root.
- This **patches out-of-tree DKMS sources** in place. A future `nct6687d` release can break the inject anchors — use the pacman hook or re-run inject after upgrades.
- Start with `vrm=0`, confirm fans/temps, then enable VRM.

## Requirements

- Linux with root
- `nct6687d-dkms` / `nct6687d-dkms-git` already providing `nct6687.ko`
- Kernel headers for your running kernel (DKMS build)

**CoolerControl is not required.** If you use it (or any other hwmon client), stop it before `modprobe -r nct6687` so the module can unload.

## Quick install (DKMS — recommended)

From a clone of this repo:

```fish
# Optional: only if CoolerControl (or similar) is running
sudo systemctl stop coolercontrold

sudo python3 ./nct6687_vrm_dkms_inject.py --verify-compile
sudo python3 ./nct6687_vrm_dkms_inject.py --install
# Module reloads with vrm=0 — check fans/temps first

sudo modprobe -r nct6687; and sudo modprobe nct6687 vrm=1

sudo systemctl start coolercontrold   # if you stopped it
```

Check:

```fish
cat /sys/module/nct6687/parameters/vrm   # expect Y
sensors
# or: ls /sys/class/hwmon/hwmon*/curr1_input
```

| hwmon | Meaning |
|-------|---------|
| `in20_input` | CPU VOUT (mV) |
| `in21_input` | CPU VIN (mV) |
| `curr1_input` | CPU IOUT (mA) |
| `power1_input` | CPU POUT (µW) |
| `temp20_input` | VR temp (m°C) |

GT/iGPU (PAGE 1): `modprobe nct6687 vrm=1 vrm_gt=1` → `in22` / `curr2` / `power2` / `temp21`.

### Persist across reboot and package updates (Arch)

```fish
sudo bash ./pacman-hook/install.sh
```

This installs:

- `/etc/modprobe.d/nct6687-vrm.conf` → `options nct6687 vrm=1`
- A pacman hook that re-injects + rebuilds when `nct6687d-dkms-git` is upgraded (does **not** unload the live module mid-transaction; reboot or reload later)

After you change the inject script and pull updates, re-run `pacman-hook/install.sh` so `/usr/local` stays in sync.

## Rollback

```fish
sudo modprobe -r nct6687; and sudo modprobe nct6687 vrm=0
# full stock sources + rebuild:
sudo python3 ./nct6687_vrm_dkms_inject.py --restore --rebuild
# remove hook / modprobe.d if installed:
sudo rm -f /etc/pacman.d/hooks/nct6687-vrm-reinject.hook \
  /etc/modprobe.d/nct6687-vrm.conf \
  /usr/local/sbin/nct6687-vrm-reinject
sudo rm -rf /usr/local/lib/nct6687-vrm
```

## Userspace reader (optional)

For a one-shot check **without** patching the kernel module. Prefers `nct6687` unloaded; `--force` races the driver’s EC window.

```fish
sudo python3 ./nct6687_vrm.py --page 0
# if nct6687.ko is loaded (not recommended long-term):
sudo python3 ./nct6687_vrm.py --page 0 --force
```

Do **not** use this while the module is loaded with `vrm=1`.

## License

MIT — see [LICENSE](LICENSE).
