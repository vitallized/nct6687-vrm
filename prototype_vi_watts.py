#!/usr/bin/env python3
"""PROTOTYPE, throwaway. Loop VOUT×IOUT against READ_POUT, with session min/max.

IOUT is the chip READ_IOUT once this driver is loaded. V×I will not match
POUT: the three registers update apart. Ctrl+C stops. Min/max are this run only.

Run: python3 prototype_vi_watts.py
"""

import time
from pathlib import Path

HWMON = Path("/sys/class/hwmon")
INTERVAL = 0.5


def bind() -> Path:
    for hw in HWMON.glob("hwmon*"):
        if (hw / "name").read_text().strip() != "nct6687":
            continue
        one = hw / "vrm_cpu"
        if one.is_file():
            return one
    raise SystemExit("nct6687 vrm_cpu not found")


def sample(path: Path) -> dict[str, float]:
    # One read: VOUT mV, IOUT mA, POUT µW from the same PAGE sample.
    v_mv, i_ma, p_uw = (int(x) for x in path.read_text().split())
    vi_w = (v_mv * i_ma) / 1_000_000
    pout_w = p_uw / 1_000_000
    return {
        "vout": v_mv / 1000,
        "iout": i_ma / 1000,
        "pout": pout_w,
        "vi": vi_w,
        "delta": vi_w - pout_w,
    }


def main() -> None:
    path = bind()
    keys = ("vout", "iout", "pout", "vi", "delta")
    lo = {k: None for k in keys}
    hi = {k: None for k in keys}
    n = 0
    print("PROTOTYPE  Ctrl+C to stop. Min/max are this run. IOUT is READ_IOUT.")
    try:
        while True:
            cur = sample(path)
            n += 1
            for k in keys:
                v = cur[k]
                lo[k] = v if lo[k] is None else min(lo[k], v)
                hi[k] = v if hi[k] is None else max(hi[k], v)
            print(
                f"\033[H\033[J"
                f"PROTOTYPE  n={n}  every {INTERVAL:.1f}s  Ctrl+C stops\n"
                f"IOUT is READ_IOUT. Delta is V×I minus chip watts.\n"
                f"{'':16} {'now':>10} {'min':>10} {'max':>10}\n"
                f"{'VOUT V':16} {cur['vout']:10.3f} {lo['vout']:10.3f} {hi['vout']:10.3f}\n"
                f"{'IOUT A':16} {cur['iout']:10.3f} {lo['iout']:10.3f} {hi['iout']:10.3f}\n"
                f"{'POUT W':16} {cur['pout']:10.3f} {lo['pout']:10.3f} {hi['pout']:10.3f}\n"
                f"{'VOUT×IOUT W':16} {cur['vi']:10.3f} {lo['vi']:10.3f} {hi['vi']:10.3f}\n"
                f"{'delta W':16} {cur['delta']:+10.3f} {lo['delta']:+10.3f} {hi['delta']:+10.3f}",
                flush=True,
            )
            time.sleep(INTERVAL)
    except KeyboardInterrupt:
        print("\nstopped")


if __name__ == "__main__":
    main()
