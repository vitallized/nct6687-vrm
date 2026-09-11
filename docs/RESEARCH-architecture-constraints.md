# Architecture constraints: NCT6687 eSIO SMBus VRM on Linux

Primary-source survey for how an out-of-tree DKMS hook should sit on `nct6687d` / Linux hwmon. Local notes already cover the MS-7D89 wire protocol (`docs/PROTOCOL.md`, `README.md`, `msi-wmi-inventory/HWiNFO_NCT_VRM_notes.md`). This file is **what first-party docs and sources allow**, not another reverse-engineering log.

Legend: **spec** = owning standard/datasheet; **src** = first-party source; **inf** = inference from those; **local** = this repo / HWiNFO notes (not a vendor doc).

Survey date: 2026-09-11.

---

## 1. Upstream nct6687d (`Fred78290/nct6687d`, `main`)

Sources: [nct6687.c](https://raw.githubusercontent.com/Fred78290/nct6687d/main/nct6687.c), [Makefile](https://raw.githubusercontent.com/Fred78290/nct6687d/main/Makefile), [Kbuild](https://raw.githubusercontent.com/Fred78290/nct6687d/main/Kbuild), [dkms.conf](https://raw.githubusercontent.com/Fred78290/nct6687d/main/dkms.conf), [README](https://github.com/Fred78290/nct6687d/blob/main/README.md), [commits](https://github.com/Fred78290/nct6687d/commits/main).

### Structure (src)

| Piece | What `main` does |
|-------|------------------|
| Super-I/O | LDN `0x0B` (`NCT6687_LD_HWM`), DEVID `0xD590` mask `0xFFF0`, BAR at CR60/61. `request_muxed_region` on `0x2e`/`0x4e`. |
| EC window | `data->addr` = BAR + `IOREGION_OFFSET` (**0**). Access via `addr+4` page, `addr+5` index, `addr+6` data. Unlock write `0xFF` then page. |
| Locking | `update_lock` (sensor cache, 1 Hz). **`EC_io_lock` around every page/index/data sequence** — comment says a concurrent `nct6687_read`/`write` can reprogram page/index mid-`inb_p`. Separate `fan_watchdog_lock`. |
| I/O claim | `devm_request_region(res->start, IOREGION_LENGTH, "nct6687")` with **`IOREGION_LENGTH 4`**. Resource is BAR+0..+3. Access is BAR+4..+6. **The claimed range does not cover the ports used.** |
| hwmon | `devm_hwmon_device_register_with_info(..., &nct6687_chip_info, data->extra_groups)`. `extra_groups[2]`. Info API: 14× `in`, 9× `fan`, 7× `temp`, 8× `pwm`. **No `curr` / `power`.** Extra group used only for MSI fan-watchdog when `msi_alt1 && msi_fan_brute_force`. |
| Params | `force`, `manual`, `msi_fan_brute_force`, `fan_config` (load-time only, 0444), `fan_mask`, `temp_mask`. **No SMBus / eSIO / VRM params.** |

Derived from nct6683 (header comment). README: blacklist in-tree `nct6683` so this module owns the chip.

### SMBus / eSIO / VRM hooks

**None.** Grep of `nct6687.c` on `main`: no SMBus host, no PMBus, no proto `0x82`/`0x83`, no PAGE/VOUT. The only `0x0B` use is Super-I/O LDN select. EC access is HWM page/index only.

### Makefile / Kbuild a DKMS inject must survive

| File | Current contract |
|------|------------------|
| `Kbuild` | `obj-m += nct6687.o` only. An `#include "nct6687_vrm.inc.c"` does **not** need a Kbuild line if it is pulled in from `nct6687.c`. The `.inc.c` must sit **next to** `nct6687.c` in the `M=` directory. |
| `Makefile` | Default/`modules`: `$(MAKE) -C $(KDIR) M=$(CURDIR)`. **No** `cp … ${kver}` on this path. Packaging targets (`akmod/build`, `dkms/install`) copy `Kbuild Makefile nct6687.c`. |
| `dkms.conf` | `PACKAGE_NAME=nct6687d`, `PACKAGE_VERSION=1`, `MAKE[0]="make -C ${kernel_source_dir} M=${dkms_tree}/nct6687d/1/build"`, `BUILT_MODULE_NAME[0]=nct6687`, `DEST_MODULE_LOCATION[0]=/updates`. DKMS builds the **source tree as `M=`**. It does **not** run a project-local `cp` into `${kver}`. |

**inf:** Patching an old `cp ${curpwd}/nct6687.c ${curpwd}/${kver}` line is dead on current `main`. The include must be present in `/usr/src/nct6687d-1/` (or the DKMS build dir) before `MAKE[0]`.

### Commits that break an include-injection patch

Exact-string inject (`nct6687_vrm_dkms_inject.py`) anchors: `IOREGION_LENGTH 4`, `struct mutex update_lock;`, `extra_groups[2]`, `nct6687_update_device` tail (`last_updated` / `valid` / `mutex_unlock` / `return data`), `nct6687_setup_voltages(data);`, the `extra_groups[0] = &nct6687_fan_watchdog_group` if-block.

| When | Commit | Why it bites |
|------|--------|----------------|
| 2026-08-14 | [3b45f7e](https://github.com/Fred78290/nct6687d/commit/3b45f7e) `hwmon: use the hwmon info API` | Dropped `SENSOR_TEMPLATE` / `groups[]` / `register_with_groups`. Extra attrs now go through `extra_groups` + `register_with_info`. |
| 2026-08-15 / 08-29 | [c99a8be](https://github.com/Fred78290/nct6687d/commit/c99a8be), [50bb199](https://github.com/Fred78290/nct6687d/commit/50bb199), [e2e0c78](https://github.com/Fred78290/nct6687d/commit/e2e0c78) | Split `Kbuild`, in-tree `M=` flow, `dkms.conf` no longer `make kver=… dkms/build`. **This is the Kbuild trap the pacman hook already documents.** |
| 2026-09-04 | [f86128e](https://github.com/Fred78290/nct6687d/commit/f86128e) `Normalize kernel coding style` | Whitespace / brace / comment churn vs exact anchors. |
| 2026-09-08 | [139e75c](https://github.com/Fred78290/nct6687d/commit/139e75c) `Use DMI data for board capabilities` | Probe no longer `dmi_check_system(...)` inline; `nct6687_board` helper. Nearby lines around `setup_voltages` / extra_groups if-block. |
| 2026-09-08 | [127ea90](https://github.com/Fred78290/nct6687d/commit/127ea90) | `devm_add_action_or_reset` fan restore; remove/suspend paths moved. |

As of this survey, the inject’s `extra_groups[2]` / `setup_voltages` / `update_device` tail **still exist** on `main`. Style-normalize and DMI commits are the next likely exact-string failures.

---

## 2. Linux hwmon / in-tree nct6683

### nct6683 driver (src + docs)

| Claim | Source |
|-------|--------|
| Supports NCT6683D / NCT6686D / NCT6687D. Prefix `nct6683`. ISA address from Super-I/O. **Datasheet: available from Nuvoton upon request.** | [Documentation/hwmon/nct6683.rst](https://www.kernel.org/doc/Documentation/hwmon/nct6683.rst), [docs.kernel.org/hwmon/nct6683.html](https://docs.kernel.org/hwmon/nct6683.html) |
| Intel firmware register map ≠ Nuvoton datasheet. Intel/Nuvoton spec is **NDA, not public**. Default: instantiate on Intel boards only; `force=1` for others. | same |
| Lists MSI B550 / X670-P / X870E as tested NCT6687D boards. | same |
| Super-I/O: same LDN `0x0B`, IDs `0xc730` / `0xd440` / `0xd590`. | [drivers/hwmon/nct6683.c](https://github.com/torvalds/linux/blob/master/drivers/hwmon/nct6683.c) |
| I/O: `IOREGION_OFFSET 4`, `IOREGION_LENGTH 4` — claims **BAR+4..+7** (“EC port 1”). Relative `EC_PAGE_REG=0`, `EC_INDEX_REG=1`, `EC_DATA_REG=2`. | same |
| **No `EC_io_lock`.** Only `update_lock` around the cached scan. `nct6683_read`/`write` are unlocked page/index sequences. | same |
| **No SMBus host.** Strings `"SMBus 0"`…`"SMBus 5"` are **temperature source labels**, not a master. | same |

**inf:** In-tree nct6683 and out-of-tree nct6687d both bind NCT6687D (`0xD590`). They cannot both `request_region` the overlapping EC window. nct6687d README: blacklist `nct6683`.

**src mismatch:** nct6683 claims port 1 (the window it uses). nct6687d claims port 0 (BAR+0..+3) but **uses** port 1 (BAR+4..+6). A second driver that `request_region`s BAR+4..+7 can get the ports nct6687d actually talks on.

### hwmon sysfs ABI (spec)

[Documentation/hwmon/sysfs-interface](https://kernel.org/doc/Documentation/hwmon/sysfs-interface) / [docs.kernel.org](https://docs.kernel.org/hwmon/sysfs-interface.html):

| Type | Sysfs | Unit | Index origin |
|------|-------|------|----------------|
| Voltage | `in[0-*]_input` | millivolt | 0 |
| Current | `curr[1-*]_input` | milliampere | 1 |
| Power | `power[1-*]_input` | microWatt | 1 |
| Temp | `temp[1-*]_input` | millidegree Celsius | 1 |

One value per file. Number always present if the type can repeat. **Do not invent attributes** unless discussed on the hwmon list ([submitting-patches](https://docs.kernel.org/hwmon/submitting-patches.html)).

This repo’s `in20` / `curr1` / `power1` / `temp20` units match the ABI (**local** channel numbers are a collision-avoidance choice, not an ABI requirement). `vrm_cpu` (three values in one file) is **non-standard**. Upstream: “Do not create non-standard attributes unless really needed… discuss it on the mailing list first.”

### Extending vs forking (spec)

[How to Get Your Patch Accepted Into the Hwmon Subsystem](https://docs.kernel.org/hwmon/submitting-patches.html):

- **Check if a driver for the chip already exists** before writing a new one. Variants → extend the existing driver.
- Adding functionality: update `Documentation/hwmon/<driver>.rst` + Kconfig; split cleanup vs feature; never mix fix/cleanup/feature.
- New drivers: `devm_hwmon_device_register_with_info()` (deprecated registration APIs forbidden for new code). nct6687d `main` already uses this.
- I2C probe allow-list does **not** include `0x60`. A chip at 0x60 “will have to be instantiated explicitly.” Irrelevant here: the VR is **not** on a Linux I2C adapter.

**inf:** Official path for NCT6687D fans/temps is **extend `nct6683`**, not a permanent fork. VRM-via-eSIO-master is new functionality on that chip’s EC space. A standalone `nct6687-vrm.ko` is a new driver that shares the same Super-I/O / EC ports — see §5. Shipping VRM as extra `attribute_group`s on the module that already holds `EC_io_lock` matches how nct6687d already attaches the fan-watchdog group.

Linux PMBus core ([pmbus-core](https://docs.kernel.org/hwmon/pmbus-core.html)) wants an `i2c_client`. This VR is behind the NCT SMBus **master**, not `i2c-i801`. Instantiating `isl68137` on the host SMBus does not reach it (**local** dead end in `PROTOCOL.md`; consistent with pmbus-core’s I2C assumption).

---

## 3. PMBus / Renesas

### PMBus numeric formats (spec)

[PMBus Part II Rev 1.3.1](https://pmbus.org/wp-content/uploads/2022/01/PMBus-Specification-Rev-1-3-1-Part-II-20150313.pdf):

| Format | Rule | Typical use |
|--------|------|-------------|
| LINEAR11 §7.3 | `X = Y × 2^N`; Y signed 11-bit, N signed 5-bit in one word | IOUT, VIN, temps, etc. (not VOUT) |
| DIRECT §7.4 | `Y = (mX + b) × 10^R` send; receive `X = (Y × 10^{-R} − b) / m`. `m,b,R` from COEFFICIENTS or **product literature** | any numeric |
| ULINEAR16 §8.4.1 | `Voltage = V × 2^N`; V unsigned 16-bit; N from VOUT_MODE[4:0] | VOUT-related only |

VOUT_MODE §8.3: bits [6:5] mode, [4:0] parameter. `00` = ULINEAR16 (param = N), `01` = VID, `10` = DIRECT (`0x40` when param = 0), `11` = IEEE half.

Command codes (Part II command list): `PAGE 0x00`, `VOUT_MODE 0x20`, `READ_VIN 0x88`, `READ_VOUT 0x8B`, `READ_IOUT 0x8C`, `READ_TEMPERATURE_1 0x8D`, `READ_POUT 0x96`. PAGE `00h`–`1Fh` = outputs; `FFh` = all outputs.

Linux pmbus-core: Direct coefficients “usually provided by chip manufacturers in device datasheets.” `PMBUS_USE_COEFFICIENTS_CMD` reads COEFFICIENTS from the device.

### Renesas — what is public vs not

| Part | Public first-party? |
|------|---------------------|
| **RAA229131** | **No.** Searched `renesas.com` product + datasheet index. Closest public: [RAA229139 short-form](https://www.renesas.com/en/document/sds/raa229139-short-form-datasheet) (AMD SVI3, ≤8 phases — not this board). No RAA229131 page/PDF. |
| **RAA229130** | **No** public datasheet found. |
| **ISL69269** | No public full datasheet found on renesas.com. In-tree Linux: `isl69269` → `raa_dmpvr2_3rail` in [isl68137.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/hwmon/pmbus/isl68137.c) (copyright includes Renesas Electronics America). |
| **ISL68229 / ISL68239** | **Yes.** [ISL68229/ISL68239 datasheet](https://www.renesas.com/en/document/dst/isl68229-isl68239-datasheet) — same DMPVR2 command set Linux uses for the 3-rail class. |

### ISL68229 datasheet facts (spec) — family, not “this board’s marking”

| Item | Datasheet |
|------|-----------|
| PAGE `00h` | `00h` Rail 0, `01h` Rail 1, `02h` Rail 2, `80h` phase, `FFh` all. Default `00h`. |
| VOUT_MODE `20h` | Read-only. Default **`40h`**. “Direct mode, 1 mV per LSB.” |
| READ_VOUT `8Bh` | Direct, 1 mV/LSB. `VOUT = raw`. |
| READ_VIN `88h` | Direct, **10 mV/LSB**. `VIN = raw × 10`. |
| READ_IOUT `8Ch` | Direct, **0.1 A/LSB**. `IOUT = raw / 10`. |
| READ_TEMPERATURE_1 `8Dh` | Direct, **1 °C/LSB**. Hottest power stage. |
| READ_POUT `96h` | Direct, **1 W/LSB**. |
| Address | Pin-strap resistor table. **7-bit `0x60` is the 0 Ω entry.** Other straps → `0x61`…`0x68`, `0x45`…`0x5F`. **Not a fixed 0x60.** |

Linux `isl68137.c` DMPVR2 coefficients (src, matches that datasheet):

| Sensor | `m` | `b` | `R` | Meaning |
|--------|-----|-----|-----|---------|
| VIN | 1 | 0 | 2 | raw × 10 mV |
| VOUT | 1 | 0 | 3 | raw × 1 mV |
| IOUT | 1 | 0 | 1 | raw × 0.1 A |
| POWER | 1 | 0 | 0 | raw × 1 W |
| TEMP | 1 | 0 | 0 | raw × 1 °C |

`isl69269` is **3-rail**. `raa229001` / `raa229004` are 2-rail. **No `raa229130` / `raa229131` ID** in that table.

### Datasheet vs this repo (contradictions)

| Topic | PROTOCOL.md / HWiNFO notes (**local**) | First-party |
|-------|----------------------------------------|-------------|
| Part name | “RAA229131-class”; HWiNFO string `ISL69269/RAA229130` (no literal RAA229131 in that HWiNFO build) | No public RAA229131 doc. ISL69269 is 3-rail DMPVR2 in Linux. |
| Addr `0xC0` / 7-bit `0x60` | Proven on MS-7D89 | `0x60` is **one** ISL68229 strap. 8-bit write `0xC0` = `0x60 << 1` (**inf**, SMBus convention). |
| PAGE 0 / 1 | CPU / GT | Datasheet: Rail 0 / Rail 1. Mapping CPU vs GT is **board firmware**, not in the public PDF. |
| VOUT `0x40` → mV | Used on MS-7D89 | ISL68229: VOUT_MODE default `40h` = Direct 1 mV/LSB. **Family datasheet agrees.** |
| VIN raw × 10 | “Renesas DMPVR2 Direct m=1,R=2” in HWiNFO notes | ISL68229 + isl68137 agree. |
| POUT / TEMP LINEAR11 | PROTOCOL | **Family datasheet + isl68137: Direct, not LINEAR11.** When N=0, LINEAR11 `0x0029` and Direct `0x0029` both read as 41 — **local sample cannot distinguish.** |
| IOUT | Prefer `POUT/VOUT`; “coarse LINEAR11-ish fallback” | Datasheet: Direct 0.1 A/LSB (`0x00E5` → 22.9 A). LINEAR11 of that word is nonsense (~229 A if N=0). |

**inf:** Treat MS-7D89 scalings as **DMPVR2-family** unless a RAA229131 PDF appears. PROTOCOL already warns Direct `m/R` are board-specific; POUT/TEMP-as-LINEAR11 is the weaker claim against the public family datasheet.

---

## 4. Nuvoton NCT6687 eSIO SMBus host

### Official NCT6687 datasheet

**None public.** Searched:

- `nuvoton.com` product index / eSIO series — [NCT6686D](https://www.nuvoton.com/products/cloud-computing/i-o/esio-series/nct6686d/) exists; **no NCT6687D product page**.
- Nuvoton news: [NCT6683D announcement](https://www.nuvoton.com/news/news/products-technology/TSNuvotonNews-000028/) (SMBus master mentioned as a feature, no register map); NCT6681D announcement. No NCT6687D.
- Kernel: “Datasheet: Available from Nuvoton upon request.” Intel firmware spec **NDA**.

### Closest official programming guide: NCT6686D HW datasheet

[NCT6686D_HW_Datasheet_V0_5.pdf](https://www.nuvoton.com/resource-files/NCT6686D_HW_Datasheet_V0_5.pdf) (Nuvoton, public):

| Item | What the PDF actually says |
|------|----------------------------|
| LDN `0x0B` | Logical Device B = **EC Space**. Includes HWM, PECI, TSI, **SMBus Master**, … |
| BAR | CR60/61: base `<100h:FF8h>`, **8-byte aligned**. Enable CR30. |
| Host windows | **8 I/O ports.** Port0 (BIOS/ACPI): page `+0`, index `+1`, data `+2`. Port1 (application): page `+4`, index `+5`, data `+6`. Event regs `+3` / `+7`. |
| Page lock | If MCU enables protection: PAGE writable only if current or write data is `0xFF`; INDEX writable only if PAGE ≠ `0xFF`. |
| EC operations / HWM / PECI / SMBus master **register map** | **Not in this PDF.** “Please refer to NCT6686D EC Space Specification” / “NCT6686D Family EC Space Datasheet.” Those are **not** on the public product page. |

**inf:** nct6683/nct6687d `0xFF` then page, then index, then data on **port 1** matches this datasheet. The eSIO SMBus host mailbox (ctrl `0x60`, cfg `0x61`, baud `0x62`, proto `0x63`, addr `0x65`, cmd `0x66`, payload `0x70` / page4 `0xB0`) is **not** specified in any Nuvoton document we could fetch.

### Non-Nuvoton public maps (not first-party)

[coreboot `nct6687d_hwm.h`](https://review.coreboot.org/c/coreboot/+/94655) names page-4 SMBus master regs (`SMBUS_MASTER_CFG1_REG` = page4 `0x60`, proto `0x63`, `WRITE_BYTE 0x02`, `READ_BYTE 0x82`, `READ_WORD 0x83`, …). Useful, **not** a Nuvoton datasheet.

HWiNFO notes (**local**): same mailbox; LDN `0x0B` BAR; chip filter `{0xC7, 0xD441, 0xD5}`.

---

## 5. DKMS / second `.ko` / port ownership

### Official DKMS (spec)

[dkms(8)](https://github.com/dkms-project/dkms/blob/main/dkms.8.in) / [README](https://github.com/dkms-project/dkms/blob/main/README.md):

| Model | What DKMS documents |
|-------|---------------------|
| Full module | Source + `dkms.conf` in `/usr/src/<name>-<ver>/`. `BUILT_MODULE_NAME`, `MAKE[0]`, `AUTOINSTALL`. This is what **nct6687d** ships. |
| Patch-then-build | `PATCH[#]=` **`-p1` files** in `/usr/src/<mod>-<ver>/patches/` or `/etc/dkms/<mod>/patches/`. Failed patch **halts** the build. Optional `PATCH_MATCH[#]` vs kernel version. |
| Binary tarball | Prebuilt `.ko` without a compiler. Not relevant here. |

**No** official “mutate another package’s `/usr/src` tree after `dkms install`” API. In-place `#include` inject is an **out-of-band overlay** on nct6687d’s sources. The supported in-tree equivalent is `PATCH[#]` **owned by a DKMS package that contains those patches** (either a forked `nct6687d` or a wrapper that vendors the patched tree).

`BUILD_DEPENDS[#]` is **other DKMS package names**, not “patch that other package.”

### Same EC I/O ports, second `.ko`?

| Mechanism | First-party rule |
|-----------|------------------|
| `request_region` / `devm_request_region` | Exclusive on `ioport_resource` ([`ioport.h`](https://raw.githubusercontent.com/torvalds/linux/master/include/linux/ioport.h)). Second claim of the **same** range → `NULL` / `-EBUSY`. `IORESOURCE_MUXED` is only used for Super-I/O config ports, not the EC BAR. |
| nct6687d claim vs use | Claims BAR+0..+3, uses BAR+4..+6. A second module can legally `request_region(BAR+4, 4)` **and still race** the first module’s unlocked (wrt ioport) accesses. |
| `EC_io_lock` | Process-private mutex inside `nct6687.ko`. Another `.ko` **cannot** take it. |
| nct6687d comment (src) | Page/index/data **must** stay inside one lock hold. |

**inf:** A separate `.ko` that talks to the same eSIO window is **not** architecturally viable: even if `request_region` succeeds on the unclaimed BAR+4..+7 slice, there is no shared lock with `nct6687_read`/`write`. Userspace `/dev/port` has the same race (`README`: `--force`). The only first-party-consistent in-kernel design is **same module, same `EC_io_lock`**.

Nuvoton: allocate **8** ports. Inject’s `IOREGION_LENGTH 4 → 8` (BAR+0..+7) is the claim that matches the datasheet and covers the ports actually used. That is a hardening of nct6687d’s undersized region, not a VRM-only quirk.

---

## 6. How the DKMS patch should relate to upstream

| Constraint | Consequence |
|------------|-------------|
| nct6687d has no SMBus/VRM | Feature cannot be a module-param-only enable; it is new code. |
| hwmon: extend existing chip driver | Prefer extra `attribute_group` on `nct6687` (already the `extra_groups[]` hook) over a second hwmon device fighting the same BAR. Long-term upstream = nct6683 + docs, not a forever fork. |
| Info API owns `in0`–`in13`, `temp1`–`temp7` | Extra-group `in20` / `temp20` / `curr1` / `power1` avoids colliding with `nct6687_info[]`. Units must stay mV / mA / µW / m°C. |
| `vrm_cpu` | Non-standard; keep out of any upstream series unless the list agrees. |
| Kbuild `obj-m += nct6687.o` + DKMS `M=` | Drop `nct6687_vrm.inc.c` **in the same directory** as `nct6687.c`. Do not depend on a `Makefile` `cp` into `${kver}`. |
| Exact-string inject | Fragile vs style/DMI/hwmon-API commits (table in §1). Official DKMS alternative: `PATCH[#]` inside a **vendored** nct6687d source package. |
| Second `.ko` | Rejected by locking + (partial) `request_region`. |
| `isl68137` / pmbus-core | Right decode tables for DMPVR2; **wrong bus**. Cannot bind without an I2C adapter to 0x60. |

---

## 7. Local notes — agreements and contradictions

| Local claim | vs primary sources |
|-------------|-------------------|
| eSIO window BAR+4/+5/+6, LDN `0x0B` | **Agrees** with NCT6686D HW datasheet + both Linux drivers. |
| Hold `EC_io_lock` for VRM | **Agrees** with nct6687d’s own EC-window comment. In-tree nct6683 has no such lock. |
| Do not run userspace reader with `vrm=1` | **Agrees** (no shared lock). |
| `IOREGION_LENGTH 8` | **Agrees** with Nuvoton “8 IO ports”; **fixes** nct6687d claiming only 4 at the wrong offset. |
| VOUT Direct mV when VOUT_MODE=`0x40`; VIN ×10 | **Agrees** with ISL68229 + isl68137 DMPVR2. |
| POUT/TEMP LINEAR11 | **Conflicts** with ISL68229 / isl68137 (Direct). May still numerically match when N=0. |
| IOUT = POUT/VOUT | **Inference.** Family datasheet has READ_IOUT Direct 0.1 A/LSB. |
| Address 0xC0 universal | **Conflicts** with ISL68229 strap table. Board-specific (**local** already warns). |
| “RAA229131” | **No public Renesas doc.** HWiNFO name is ISL69269/RAA229130. |
| nct6687d “Kbuild broke the hook” | **Agrees** with 2026-08 Kbuild/`dkms.conf` commits. |

---

## Sources (owning)

| Owner | URL |
|-------|-----|
| nct6687d | https://github.com/Fred78290/nct6687d |
| Linux nct6683 | https://www.kernel.org/doc/Documentation/hwmon/nct6683.rst · https://github.com/torvalds/linux/blob/master/drivers/hwmon/nct6683.c |
| hwmon ABI / submit | https://kernel.org/doc/Documentation/hwmon/sysfs-interface · https://docs.kernel.org/hwmon/submitting-patches.html |
| pmbus-core / isl68137 | https://docs.kernel.org/hwmon/pmbus-core.html · https://github.com/torvalds/linux/blob/master/drivers/hwmon/pmbus/isl68137.c |
| ioport | https://github.com/torvalds/linux/blob/master/include/linux/ioport.h |
| PMBus Part II 1.3.1 | https://pmbus.org/wp-content/uploads/2022/01/PMBus-Specification-Rev-1-3-1-Part-II-20150313.pdf |
| ISL68229/39 | https://www.renesas.com/en/document/dst/isl68229-isl68239-datasheet |
| NCT6686D HW | https://www.nuvoton.com/resource-files/NCT6686D_HW_Datasheet_V0_5.pdf |
| DKMS | https://github.com/dkms-project/dkms/blob/main/dkms.8.in |

Not used as owners: blogs, HWiNFO marketing pages, coreboot (cited only as a public register *name* dump, not a Nuvoton spec).
