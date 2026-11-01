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
| POUT `0x96`, TEMP `0x8D` | LINEAR11 via `nct_vrm_linear11_milli` / `linear11_milli` (integer millisi on this board). |
| IOUT `0x8C` | Always `READ_IOUT`, Direct **0.1 A/LSB**, via `nct_vrm_decode_iout_ma` / `decode_iout_ma` (`raw * 100` → mA). |
| IIN `0x89` | `READ_IIN`, Direct **0.01 A/LSB**, via `nct_vrm_decode_iin_ma` / `decode_iin_ma` (`raw * 10` → mA). |
| PIN `0x97` | `READ_PIN`, Direct **1 W/LSB**, two's complement, via `nct_vrm_decode_pin_uw` / `decode_pin_uw` (`nct_vrm_i16(raw) * 1000000` → µW). `0xFFFF` is −1 W. A negative IOUT, IIN, POUT, or PIN is not published (last non-negative value kept) and is noted with `pr_warn_ratelimited`. Efficiency skips that sample. |

Sample order is TEMP, VOUT, VIN, IOUT, IIN, POUT, PIN. PAGE temperature is `0x8D`. `READ_TEMPERATURE_2` (`0x8E`, controller die) is one extra word per demand session, global, not per PAGE. `READ_TEMPERATURE_3` (`0x8F`) is not sampled. The words are separate commands and are not one snapshot. Efficiency is `nct_vrm_efficiency_pcm` / `efficiency_pcm`: `POUT/PIN` as milli-percent, 100% when output power is at least input power, and 0 when either power is not positive. That cap is 100% when output is at least input, not a second read. `nct_vrm_efficiency_pwm` / `efficiency_pwm` maps that to a count for read-only `pwm9`. libsensors 3.6.2 `get_type_scaling` returns `int`, so its 2.55 pwm factor is 2 and `sensors` prints count/2. 100% is 200. `pwm10` is not used: that build copies only four characters of a pwm filename, so `pwm10` would alias fan `pwm1`.

**VOUT and VIN Direct scales were chosen because they matched measured rails on this board.** IOUT 0.1 A/LSB, IIN 0.01 A/LSB, and PIN 1 W/LSB are the DMPVR2 family Direct scales. A PAGE 0 probe on 2026-10-30 returned IIN `0x0231` (5.61 A) and PIN `0x0065` (101 W). Those two do not multiply back to each other. LINEAR11 for POUT/TEMP matches this board’s words when N=0; the family datasheet calls those Direct. The Direct VOUT/VIN scaling is **board- and part-specific inference**. Millisi decode lives in `dkms/nct6687_vrm_decode.h`. `tests/golden_vrm_decode.json` is the raw→SI table the host-compiled C must match.

Implications if you try this on another MSI board or a different VR part number:

- SMBus may NACK or status-fail — obvious failure.
- Or it may return raw words that look “fine” but decode to **plausible wrong** volts/amps (especially VIN and VOUT) because Direct `m/R` differ.
- Cross-check against a known load before trusting numbers. Do not assume `0xC0` + these scalings are universal.

## Safety

- No address scans, block reads, or blind PAGE sweeps.
- In-kernel path holds `EC_io_lock` (same as stock fans/temps).
- Do not run the userspace reader while module `vrm=1`.
- Aggressive probing can wedge the EC until a power cycle.

## Dead ends on MS-7D89

- Host Intel I801 I2C — SPD only; VR is not there.
- MSI WMI ApService mailbox — BIOS settings/password, not sensors.
- EC “auto” SMBus sensor slots — empty; this path uses the manual eSIO master above.
