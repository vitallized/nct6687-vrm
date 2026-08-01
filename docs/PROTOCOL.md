# NCT6687 eSIO → Renesas PMBus

Verified on **MSI MPG Z790 CARBON WIFI (MS-7D89)** — NCT6687 EC, Renesas multiphase (RAA229131-class) at SMBus write address **0xC0**.

## Access

| Item | Value |
|------|--------|
| EC BAR | Platform `nct6687.<addr>` (often `0xA20`) |
| eSIO window | `base+4` page/op, `base+5` index, `base+6` data |
| SMBus port | **0 only** |
| Address | `0xC0` (7-bit `0x60`) |
| PAGE 0 | CPU Vcore |
| PAGE 1 | GT / iGPU (often zero with dGPU-only) |

Ports **2/3** have hung this EC in testing — do not use them.

## Transactions

| Proto (`0x63`) | Role |
|----------------|------|
| `0x02` | Write byte (payload register `0x70`) |
| `0x82` | Read byte |
| `0x83` | Read word |

START: write `EN|START` (`0xC0`) to ctrl `0x60`, wait until START clears. Status at page 4 index `0x03` must be `0`.

Save/restore cfg `0x61` and baud `0x62`; force port bits to `0` for the transfer.

## Decode (this VR) — and uncertainty

What we use on MS-7D89:

| Reg | Format used here |
|-----|------------------|
| VOUT `0x8B` | If `VOUT_MODE` mode bits = Direct (`0x40` → mode 2): treat raw as **mV** (R=3 style). Else LINEAR16 using mode N or `vrm_vout_exp`. |
| VIN `0x88` | Treat as Direct with **10 mV/LSB** (`raw * 10` → mV), i.e. m=1, b=0, R=2 style. |
| POUT `0x96`, TEMP `0x8D` | LINEAR11 (standard PMBus) |
| IOUT | Prefer `POUT / VOUT` when VOUT > 0.2 V; else a coarse LINEAR11-ish fallback |

**These Direct coefficients were not taken from a Renesas datasheet citation in this repo.** They were chosen because they matched HWiNFO / known-good rail values on this board after the PMBus path worked. LINEAR11 for power/temp is standard; the Direct VOUT/VIN scaling is **board- and part-specific inference**.

Implications if you try this on another MSI board or a different VR part number:

- SMBus may NACK or status-fail — obvious failure.
- Or it may return raw words that look “fine” but decode to **plausible wrong** volts/amps (especially VIN and VOUT) because Direct `m/R` differ.
- Cross-check against HWiNFO (or a known load) before trusting numbers. Do not assume `0xC0` + these scalings are universal.

## Safety

- No address scans, block reads, or blind PAGE sweeps.
- In-kernel path holds `EC_io_lock` (same as stock fans/temps).
- Do not run the userspace reader while module `vrm=1`.
- Aggressive probing can wedge the EC until a power cycle.

## Dead ends on MS-7D89

- Host Intel I801 I2C — SPD only; VR is not there.
- MSI WMI ApService mailbox — BIOS settings/password, not sensors.
- EC “auto” SMBus sensor slots — empty; HWiNFO uses the manual eSIO master above.
