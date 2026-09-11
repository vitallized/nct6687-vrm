"""PMBus raw → millisi. Same interface as dkms/nct6687_vrm_decode.h.

Integer results must match tests/golden_vrm_decode.json (and the host C
binary built from that header). Callers keep their own register sequences.
"""

from __future__ import annotations


def clamp_exp(exp: int) -> int:
    if exp < -16:
        return -16
    if exp > 15:
        return 15
    return exp


def linear11_milli(raw: int) -> int:
    raw &= 0xFFFF
    exp = (raw >> 11) & 0x1F
    if exp >= 16:
        exp -= 32
    mant = raw & 0x7FF
    if mant >= 1024:
        mant -= 2048
    neg = mant < 0
    abs_m = (-mant if neg else mant) * 1000
    if exp >= 0:
        exp = clamp_exp(exp)
        if exp > 0:
            abs_m <<= exp
    else:
        abs_m >>= clamp_exp(-exp)
    return -abs_m if neg else abs_m


def decode_vout_mv(raw: int, vout_mode: int, fallback_exp: int) -> int:
    raw &= 0xFFFF
    mode = (vout_mode >> 5) & 0x7
    if mode == 2:
        return raw
    if mode == 0:
        exp = vout_mode & 0x1F
        if exp >= 16:
            exp -= 32
    else:
        exp = fallback_exp
    exp = clamp_exp(exp)
    milli = raw * 1000
    if exp >= 0:
        return milli << exp
    return milli >> (-exp)


def decode_vin_mv(raw: int) -> int:
    return (raw & 0xFFFF) * 10


def decode_iout_fallback_ma(raw: int) -> int:
    return ((raw & 0xFFFF) * 1000) >> 3


def linear11(raw: int) -> float:
    """SI units (W, °C, …) from LINEAR11, truncated the same way as millisi."""
    return linear11_milli(raw) / 1000.0


def decode_vout(raw: int, vout_mode: int, fallback_exp: int) -> tuple[float, str]:
    """Return (volts, method). Respects PMBus VOUT_MODE when possible."""
    mode = (vout_mode >> 5) & 0x7
    mv = decode_vout_mv(raw, vout_mode, fallback_exp)
    if mode == 2:
        return mv / 1000.0, "direct-R3"
    if mode == 0:
        exp = vout_mode & 0x1F
        if exp >= 16:
            exp -= 32
        exp = clamp_exp(exp)
        return mv / 1000.0, f"linear16-mode(exp={exp})"
    exp = clamp_exp(fallback_exp)
    return mv / 1000.0, f"linear16-fallback(exp={exp})"
