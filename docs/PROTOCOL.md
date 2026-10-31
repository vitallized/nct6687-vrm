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
| VOUT `0x8B` | If `VOUT_MODE` mode bits = Direct (`0x40` → mode 2): treat raw as **mV** (R=3 style). Else LINEAR16 using mode N or `vrm_vout_exp`. Helpers: `nct_vrm_decode_vout_mv` / `decode_vout_mv`. |
| VIN `0x88` | Direct **10 mV/LSB** via `nct_vrm_decode_vin_mv` / `decode_vin_mv` (`raw * 10` → mV), i.e. m=1, b=0, R=2 style. |
| POUT `0x96`, TEMP `0x8D` | LINEAR11 via `nct_vrm_linear11_milli` / `linear11_milli` (integer millisi, matching HWiNFO on this board). |
| IOUT `0x8C` | Always `READ_IOUT`, Direct **0.1 A/LSB**, via `nct_vrm_decode_iout_ma` / `decode_iout_ma` (`raw * 100` → mA). |
| IIN `0x89` | `READ_IIN`, Direct **0.01 A/LSB**, via `nct_vrm_decode_iin_ma` / `decode_iin_ma` (`raw * 10` → mA). |
| PIN `0x97` | `READ_PIN`, Direct **1 W/LSB**, via `nct_vrm_decode_pin_uw` / `decode_pin_uw` (`raw * 1000000` → µW). |

Sample order is TEMP, VOUT, VIN, IOUT, IIN, POUT, PIN. One temperature (`0x8D`). TEMP2/TEMP3 are not sampled. The words are separate commands and are not one snapshot. `iout_from_pv_ma` is a host/Python helper and is not on the sample path. Efficiency is `nct_vrm_efficiency_pcm` / `efficiency_pcm`: `POUT/PIN` as milli-percent, 100% when output power is at least input power, and 0 when either power is not positive. That is the HWiNFO 8.54 cap (`minsd` with 100.0), not a second read.

**VOUT and VIN Direct scales were chosen because they matched HWiNFO on this board.** IOUT 0.1 A/LSB, IIN 0.01 A/LSB, and PIN 1 W/LSB are the DMPVR2 family Direct scales. A PAGE 0 probe on 2026-10-30 returned IIN `0x0231` (5.61 A) and PIN `0x0065` (101 W). Those two do not multiply back to each other. LINEAR11 for POUT/TEMP matches this board’s words when N=0; the family datasheet calls those Direct. The Direct VOUT/VIN scaling is **board- and part-specific inference**. Kernel C and the userspace reader share one millisi decode (`dkms/nct6687_vrm_decode.h`, `nct6687_vrm_decode.py`); `tests/golden_vrm_decode.json` is the raw→SI table both must match.

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
