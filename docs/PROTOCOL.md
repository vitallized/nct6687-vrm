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

## Decode (this VR)

| Reg | Format |
|-----|--------|
| VOUT `0x8B` | `VOUT_MODE` Direct (`0x40`) → mV = raw; else LINEAR16 |
| VIN `0x88` | Direct → `raw * 10` mV |
| POUT `0x96`, TEMP `0x8D` | LINEAR11 |
| IOUT | Prefer `POUT / VOUT` when VOUT > 0.2 V |

## Safety

- No address scans, block reads, or blind PAGE sweeps.
- In-kernel path holds `EC_io_lock` (same as stock fans/temps).
- Do not run the userspace reader while module `vrm=1`.
- Aggressive probing can wedge the EC until a power cycle.

## Dead ends on MS-7D89

- Host Intel I801 I2C — SPD only; VR is not there.
- MSI WMI ApService mailbox — BIOS settings/password, not sensors.
- EC “auto” SMBus sensor slots — empty; HWiNFO uses the manual eSIO master above.
