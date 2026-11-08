#!/usr/bin/env python3
"""DVB-T 2K quality of a real RF capture (one multiplex, cs16 at 9.142857 MS/s, e.g. from
mux_extract.py): symbol timing + fractional CFO from the guard interval, integer CFO from the
continual pilots, per-symbol channel estimate interpolated on the pilots, then MER per carrier
group across the 8 MHz channel (where does the degradation sit?).
usage: dvbt_rf_check.py capture.cs16 [--skip N] [--groups 24]"""
import argparse
import numpy as np

CONT = [0, 48, 54, 87, 141, 156, 192, 201, 255, 279, 282, 333, 432, 450, 483, 525, 531, 618, 636,
        714, 759, 765, 780, 804, 873, 888, 918, 939, 942, 969, 984, 1050, 1101, 1107, 1110, 1137,
        1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704]
TPS = [34, 50, 209, 346, 413, 569, 595, 688, 790, 901, 1073, 1219, 1262, 1286, 1469, 1594, 1687]
K, N, CP = 1705, 2048, 64
L = N + CP


def wk_seq():
    reg, w = 0x7FF, []
    for _ in range(K):
        w.append(reg & 1)
        nb = ((reg >> 2) ^ reg) & 1
        reg = (reg >> 1) | (nb << 10)
    return np.array(w)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--skip", type=int, default=50000)
    ap.add_argument("--groups", type=int, default=24)
    ap.add_argument("--nsym", type=int, default=400)
    a = ap.parse_args()
    raw = np.fromfile(a.file, dtype=np.int16).astype(np.float64)
    x = (raw[0::2] + 1j * raw[1::2])[a.skip:]
    nsym = min(a.nsym, len(x) // L - 3)
    x = x[: (nsym + 2) * L]

    # timing + fractional CFO from the guard interval (averaged over all symbols)
    prod = x[:-N] * np.conj(x[N:])
    cs = np.concatenate([[0], np.cumsum(prod)])
    best, bd = -1, 0
    for d in range(L):
        idx = d + L * np.arange(nsym)
        v = np.abs(np.sum(cs[idx + CP] - cs[idx]))
        if v > best:
            best, bd = v, d
    idx = bd + L * np.arange(nsym)
    eps = -np.angle(np.sum(cs[idx + CP] - cs[idx])) / (2 * np.pi)      # fraction of a carrier
    n = np.arange(len(x))
    x = x * np.exp(-2j * np.pi * eps * n / N)

    wk = wk_seq()
    pref = (4.0 / 3.0) * (1 - 2 * wk)
    cont = np.array(CONT)
    tpsk = np.array(TPS)
    qam = np.array([-7, -5, -3, -1, 1, 3, 5, 7])

    # integer CFO: continual pilot power vs bin shift, on the first symbols
    start = bd + CP
    X0 = np.array([np.fft.fft(x[start + s * L: start + s * L + N]) for s in range(20)])
    P0 = np.mean(np.abs(X0) ** 2, axis=0)
    shifts = range(-40, 41)
    sc = [np.sum(P0[(cont - 852 + m) % N]) for m in shifts]
    mi = list(shifts)[int(np.argmax(sc))]
    print(f"symbol start {bd}, CFO {mi + eps:+.3f} carriers ({(mi + eps) * 4464.29:+.0f} Hz)")
    bins = (np.arange(K) - 852 + mi) % N

    err = np.zeros(K)
    pw = np.zeros(K)
    cnt = np.zeros(K)
    mers = []
    lm_prev = None
    for s in range(nsym):
        X = np.fft.fft(x[start + s * L: start + s * L + N])[bins]
        lm = int(np.argmax([np.mean(np.abs(X[m * 3::12]) ** 2) for m in range(4)]))
        pk = np.unique(np.r_[cont, np.arange(3 * lm, K, 12)])
        h = X[pk] / pref[pk]
        # channel: magnitude and unwrapped phase interpolated across the band from the pilots
        mag = np.interp(np.arange(K), pk, np.abs(h))
        ph = np.interp(np.arange(K), pk, np.unwrap(np.angle(h)))
        Y = X / (mag * np.exp(1j * ph))
        isd = np.ones(K, bool)
        isd[pk] = False
        isd[tpsk] = False
        d = Y * np.sqrt(42)
        ideal = qam[np.clip(np.round((d.real + 7) / 2), 0, 7).astype(int)] + \
            1j * qam[np.clip(np.round((d.imag + 7) / 2), 0, 7).astype(int)]
        e = np.abs(d - ideal) ** 2
        err[isd] += e[isd]
        pw[isd] += np.abs(ideal[isd]) ** 2
        cnt[isd] += 1
        mers.append(10 * np.log10(np.sum(np.abs(ideal[isd]) ** 2) / np.sum(e[isd])))
        lm_prev = lm
    mers = np.array(mers)
    print(f"{nsym} symbols: MER mean {10 * np.log10(np.sum(pw) / np.sum(err)):.1f} dB, per symbol "
          f"min {mers.min():.1f} / median {np.median(mers):.1f} / max {mers.max():.1f} dB")
    g = a.groups
    edges = np.linspace(0, K, g + 1).astype(int)
    print("MER per carrier group (k, offset from channel centre):")
    for i in range(g):
        sl = slice(edges[i], edges[i + 1])
        m = 10 * np.log10(np.sum(pw[sl]) / np.sum(err[sl]))
        f0 = (edges[i] - 852) * 4464.29 / 1e6
        f1 = (edges[i + 1] - 852) * 4464.29 / 1e6
        print(f"  k {edges[i]:4d}-{edges[i + 1] - 1:4d} ({f0:+5.2f}..{f1:+5.2f} MHz): {m:5.1f} dB " + "#" * int(max(0, m)))


if __name__ == "__main__":
    main()
