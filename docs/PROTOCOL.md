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

START: write `EN` (`0x80`) to ctrl `0x60`, then `EN|START` (`0xC0`) 20 µs later (with `vrm_fast` bit 16, only the `EN|START` write; `EN` is already set), and wait until bit `0x40` reads back `0`. Status at page 4 index `0x03` must then be `0`.

Clear: before each transaction the driver sets bit `0x08` (CLEAR) in ctrl `0x60` and writes it back off straight away, without waiting. `vrm_fast` bit 8 skips the pulse after a transaction that ended with status 0.

A read word is proto `0x83`, the 7-bit address shifted left one into `0x65`, the command in `0x66`, then the low byte at `0xB0` and the high byte at `0xB1`. The page port is idle only at `0xFF`. The driver holds `EC_io_lock`, so when the port is not `0xFF` it writes `0xFF` at once and gives up after about 1 ms; it writes `0xFF` again after each access.

**What HWiNFO does (8.55 build 6110; read from the unpacked Windows binary with Ghidra on 2026-11-09, not run here).** It drives this same mailbox the same way: one PMBus command per transaction, a PAGE write per rail, then one read word per value (`0x8B`, `0x88`, `0x8C`, `0x89`, `0x96`, `0x97`, `0x8D`, `0x8E`, `0x8F`). It has no bulk or latched read of the telemetry; its block read (`0x84`) is used when identifying the controller, not for readings. Where it differs from the driver:

- Clear is one write of `EN|CLEAR` (`0x88`); it then reads `0x60` until bit `0x08` drops, for up to 10 ms.
- A read word writes `0x63`, `0x65`, `0x66`, then `EN|START` (`0xC0`) to `0x60` in one write. It does not write `EN` first (the driver does the same with `vrm_fast` bit 16), and its SMBus routines never write the port (`0x61`) or speed (`0x62`) registers (the driver does the same with `vrm_mux_keep=1` when port 0 is selected).
- It then re-reads `0x60` until bit `0x40` drops, with no sleep between reads, and gives up after 15 ms.
- When the page port is not `0xFF` it waits up to 60 ms, 1 ms at a time, logs `eSIO t/o`, and writes `0xFF`.
- The status byte is page 4 index `0x03` for id byte `0xD5` (NCT6687D, DEVID `0xD590`, the same as here), and index `0x02` for id byte `0xC7`, and for `0xD4` when the next byte is `0x41`. The driver only handles the first.

Save/restore cfg `0x61` and baud `0x62`; force port bits to `0` for the transfer. On MS-7D89 the firmware leaves `0x61=0x04` (port 0) and `0x62=0x03`, the same speed value the driver writes (logged by the driver once after load, 2026-11-09). `vrm_mux_keep=1` skips the forcing when port 0 is already selected.

**Speed.** One transaction takes about 5 ms: a refresh of 8 words measured 40–44 ms with `0x62` written as `0x01`, `0x03` and `0x05` (MS-7D89, EC firmware 1.0 build 11/01/22, 2026-11-09). A full sample therefore spans about 35 ms from its first word to its last, and neighbouring words are 5 ms apart. Faster START polling did not shorten it. **Whether the bus speed matters is not settled.** coreboot's header gives the master's speed field the mask `7 << 4` (bits 4–6) but defines the codes in bits 0–2, so those three runs may all have run at one speed. If the field is bits 4–6, every run had it at 0, which is 12.5 kHz by those names: a read word is about 47 bit times, 3.8 ms at 12.5 kHz, close to the 5 ms measured. The firmware itself leaves `0x62=0x03`, so HWiNFO, which never writes it, runs at the same setting as the driver. The mailbox has block protocols (`0x84` block read, `0x06` block write-read, 64-byte buffers in coreboot's header), but standard PMBus has no command that returns several telemetry values at once, and block reads stay off-limits here (see Safety).

## Decode (this VR) — and uncertainty

What we use on MS-7D89:

| Reg | Format used here |
|-----|------------------|
| VOUT `0x8B` | If `VOUT_MODE` mode bits = Direct (`0x40` → mode 2): treat raw as **mV** (R=3 style). Else LINEAR16 using mode N or `vrm_vout_exp`. Helpers: `nct_vrm_decode_vout_mv` / `decode_vout_mv`. |
| VIN `0x88` | Direct **10 mV/LSB** via `nct_vrm_decode_vin_mv` / `decode_vin_mv` (`raw * 10` → mV), i.e. m=1, b=0, R=2 style. |
| POUT `0x96`, TEMP `0x8D` | LINEAR11 via `nct_vrm_linear11_milli` / `linear11_milli` (integer millisi on this board). |
| IOUT `0x8C` | Always `READ_IOUT`, Direct **0.1 A/LSB**, via `nct_vrm_decode_iout_ma` / `decode_iout_ma` (`raw * 100` → mA). |
| IIN `0x89` | `READ_IIN`, Direct **0.01 A/LSB**, via `nct_vrm_decode_iin_ma` / `decode_iin_ma` (`raw * 10` → mA). |
| PIN `0x97` | `READ_PIN`, Direct **1 W/LSB**, two's complement, via `nct_vrm_decode_pin_uw` / `decode_pin_uw` (`nct_vrm_i16(raw) * 1000000` → µW). `0xFFFF` is −1 W. A negative IOUT, IIN, POUT, or PIN, and a TEMP (`0x8D`) or controller temperature (`0x8E`) above 200 °C, is not published (last published value kept) and is noted with `pr_warn_ratelimited`. Efficiency skips that sample. |

Sample order is VOUT, IOUT, POUT, PIN, IIN, VIN, TEMP, so the words that are compared with each other are read back to back and the slow ones come last. PAGE temperature is `0x8D`. `READ_TEMPERATURE_2` (`0x8E`, controller die) is one extra word per demand session, global, not per PAGE. `READ_TEMPERATURE_3` (`0x8F`) is not sampled. The words are separate commands and are not one snapshot. Efficiency is `nct_vrm_efficiency_pcm` / `efficiency_pcm`: `POUT/PIN` as milli-percent, 100% when output power is at least input power, and 0 when either power is not positive. That cap is 100% when output is at least input, not a second read. `nct_vrm_efficiency_pwm` / `efficiency_pwm` maps that to a count for read-only `pwm9`. libsensors 3.6.2 `get_type_scaling` returns `int`, so its 2.55 pwm factor is 2 and `sensors` prints count/2. 100% is 200. `pwm10` is not used: that build copies only four characters of a pwm filename, so `pwm10` would alias fan `pwm1`.

HWiNFO 8.55 decodes this family (its ids for RAA229130, RAA229131, RAA229132 and ISL69269 share one path) the way the driver does: VOUT is the raw word in mV, VIN raw × 10 mV, IOUT raw × 0.1 A, IIN raw × 0.01 A, and the temperatures raw °C. It takes POUT and PIN alike as the raw word in watts. The driver decodes POUT and the temperatures as LINEAR11, which gives the same number for a raw word of 0 to 1023; from 1024 to 2047 LINEAR11 reads negative, which the driver does not publish. HWiNFO also drops words the driver would show: a POUT or PIN above 500, a temperature above 200, an IOUT or IIN of `0xFD00` or more, and a VOUT or VIN below 11 or above `0xFFEF`. Of those the driver keeps only the temperature limit. It leaves out the 500 W cap on purpose, since an unlocked desktop part can draw more than that through one VR.

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
