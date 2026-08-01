# NCT6687 eSIO → Renesas PMBus (VRM)

Verified on **MSI MPG Z790 CARBON WIFI (MS-7D89)**, NCT6687 EC, Renesas multiphase (RAA229131-class) at SMBus write address **0xC0**.

## Access path

| Item | Value |
|------|--------|
| EC BAR | From `nct6687.<addr>` platform device (often `0xA20`) |
| eSIO window | `base+4` page/op, `base+5` index, `base+6` data |
| SMBus port | **0 only** |
| PMBus address | `0xC0` (7-bit `0x60`) |
| PAGE 0 | CPU Vcore |
| PAGE 1 | GT / iGPU (often idle with dGPU-only) |

Do **not** use SMBus ports 2/3 on this EC — they have hung the chip in testing.

## Transactions

Protocols (eSIO index `0x63`): write-byte `0x02` (payload at `0x70`), read-byte `0x82`, read-word `0x83`.

START: ctrl `0x60` ← `EN|START` (`0x80|0x40`). Wait until START clears. Status at page 4 index `0x03` must be `0`.

Before traffic: select port 0 in cfg `0x61`, set baud `0x62` (e.g. `0x03`), restore both afterward.

## Decode (this VR)

| Register | Format |
|----------|--------|
| VOUT (`0x8B`) | If `VOUT_MODE` Direct (`0x40`) → mV = raw; else LINEAR16 |
| VIN (`0x88`) | Direct 10 mV/LSB → `raw * 10` mV |
| POUT (`0x96`), TEMP (`0x8D`) | LINEAR11 |
| IOUT | Prefer `P/V` when VOUT > 0.2 V |

## Safety

- No address scans, no block reads, no blind PAGE sweeps.
- Stock `nct6687.ko` fans/temps share the EC; in-kernel VRM uses `EC_io_lock`.
- Do **not** run the userspace reader while module `vrm=1` (same ports, no userspace lock).
- Aggressive probing can wedge the EC until a power cycle.

## Ruled out (this board)

- Host Intel I801 I2C — SPD only; VR is not on that bus.
- MSI WMI “ApService” mailbox — BIOS settings/password, not sensors.
- EC auto-sensor SMBus slots — empty; HWiNFO uses the manual eSIO master above.
