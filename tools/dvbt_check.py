#!/usr/bin/env python3
"""Checks a DVB-T capture (cs16 interleaved I/Q, 1 multiplex at the elementary rate or N
multiplexes at N x 9.142857 MS/s): symbol timing from the guard interval, FFT, continual and
scattered pilots, channel equalisation, 64-QAM MER of the data cells, TPS decoding.
usage: dvbt_check.py capture.cs16 [--mux N --index i] [--fft 2048 --gi 32]"""
import argparse
import numpy as np

CONT = [0, 48, 54, 87, 141, 156, 192, 201, 255, 279, 282, 333, 432, 450, 483, 525, 531, 618, 636,
        714, 759, 765, 780, 804, 873, 888, 918, 939, 942, 969, 984, 1050, 1101, 1107, 1110, 1137,
        1140, 1146, 1206, 1269, 1323, 1377, 1491, 1683, 1704]
TPS = [34, 50, 209, 346, 413, 569, 595, 688, 790, 901, 1073, 1219, 1262, 1286, 1469, 1594, 1687]
K = 1705


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
    ap.add_argument("--mux", type=int, default=1, help="multiplexes in the capture")
    ap.add_argument("--index", type=int, default=0, help="multiplex to analyse")
    ap.add_argument("--skip", type=int, default=20000, help="samples skipped at the start")
    a = ap.parse_args()

    raw = np.fromfile(a.file, dtype=np.int16).astype(np.float64)
    x = (raw[0::2] + 1j * raw[1::2])[a.skip:]
    nfft = 2048 * a.mux
    ncp = nfft // 32
    L = nfft + ncp
    nsym = len(x) // L - 2
    print(f"{len(x)} samples, {nsym} symbols of {L}; rms {np.sqrt(np.mean(abs(x) ** 2)):.1f} LSB, "
          f"peak {np.max(np.abs(np.r_[x.real, x.imag])):.0f}")

    # timing: guard interval correlation accumulated over the symbols
    nuse = min(nsym, 200)
    best, bd = -1, 0
    seg = x[: (nuse + 1) * L + nfft]
    prod = seg[: len(seg) - nfft] * np.conj(seg[nfft:])
    cs = np.concatenate([[0], np.cumsum(prod)])
    for d in range(L):
        idx = d + L * np.arange(nuse)
        v = np.sum(np.abs(cs[idx + ncp] - cs[idx]))
        if v > best:
            best, bd = v, d
    start = bd + ncp   # residual timing error -> linear phase across carriers, equalised below
    print(f"symbol start at {bd} (guard interval correlation)")

    centre = (2 * a.index - (a.mux - 1)) * 1792 // 2
    bins = (np.arange(K) - 852 + centre) % nfft
    wk = wk_seq()
    pref = (4.0 / 3.0) * (1 - 2 * wk)
    cont = np.array(CONT)
    tpsk = np.array(TPS)
    qam = np.array([-7, -5, -3, -1, 1, 3, 5, 7])

    cars, lmod = [], []
    for s in range(nsym):
        b = start + s * L
        if b + nfft > len(x):
            break
        X = np.fft.fft(x[b: b + nfft])[bins]
        # scattered pilot phase: the set with the most power (pilots boosted 4/3, insensitive
        # to a residual linear phase)
        sc = [np.mean(np.abs(X[m * 3::12]) ** 2) for m in range(4)]
        lmod.append(int(np.argmax(sc)))
        cars.append(X)
    cars = np.array(cars)
    lmod = np.array(lmod)
    steps = (np.diff(lmod) % 4)
    print(f"scattered pilot sets: {''.join(str(v) for v in lmod[:24])}...  (step 1 every symbol: "
          f"{np.mean(steps == 1) * 100:.1f} %)")

    mers, pil_mer, dat_all = [], [], []
    tps_val = []
    for s, X in enumerate(cars):
        m = lmod[s]
        pk = np.unique(np.r_[cont, np.arange(3 * m, K, 12)])
        h = X[pk] / pref[pk]
        # channel: amplitude/phase + linear phase (timing) fitted on the pilots
        ph = np.unwrap(np.angle(h))
        p = np.polyfit(pk, ph, 1)
        amp = np.mean(np.abs(h))
        H = amp * np.exp(1j * np.polyval(p, np.arange(K)))
        Y = X / H
        pil_err = Y[pk] - pref[pk]
        pil_mer.append(10 * np.log10(np.sum(abs(pref[pk]) ** 2) / np.sum(abs(pil_err) ** 2)))
        isdata = np.ones(K, bool)
        isdata[pk] = False
        isdata[tpsk] = False
        d = Y[isdata] * np.sqrt(42)
        if np.sum(isdata) != 1512:
            print(f"symbol {s}: {np.sum(isdata)} data cells (expected 1512)")
        ideal = qam[np.clip(np.round((d.real + 7) / 2), 0, 7).astype(int)] + \
            1j * qam[np.clip(np.round((d.imag + 7) / 2), 0, 7).astype(int)]
        mers.append(10 * np.log10(np.sum(abs(ideal) ** 2) / np.sum(abs(d - ideal) ** 2)))
        dat_all.append(d)
        tps_val.append(Y[tpsk] * (1 - 2 * wk[tpsk]))
    print(f"MER data 64-QAM: mean {np.mean(mers):.1f} dB, min {np.min(mers):.1f} dB | "
          f"pilots {np.mean(pil_mer):.1f} dB")
    d = np.concatenate(dat_all)
    hist = np.histogram2d(np.clip(np.round((d.real + 7) / 2), 0, 7), np.clip(np.round((d.imag + 7) / 2), 0, 7),
                          bins=8)[0]
    print(f"64-QAM points used: {np.sum(hist > 0)}/64, occupancy min/max {hist.min():.0f}/{hist.max():.0f}")

    # TPS: DBPSK between consecutive symbols, majority over the 17 carriers
    t = np.array(tps_val)
    bits = (np.real(np.sum(t[1:] * np.conj(t[:-1]), axis=1)) < 0).astype(int)
    sync0 = [int(c) for c in f"{0x35EE:016b}"]
    sync1 = [int(c) for c in f"{0xCA11:016b}"]
    found = []
    for i in range(len(bits) - 60):
        w = list(bits[i: i + 16])
        if w == sync0 or w == sync1:
            found.append(i)
    if not found:
        print("TPS: sync word not found")
        return
    i = found[0]
    # bits[i] is s1 (bits[j] = s_{j+1} of the symbol following the frame start)
    s = [None] + list(bits[i: i + 67])
    def f(a, b):
        return int("".join(str(v) for v in s[a: b + 1]), 2)
    const = {0: "QPSK", 1: "16-QAM", 2: "64-QAM"}.get(f(25, 26), "?")
    cr = {0: "1/2", 1: "2/3", 2: "3/4", 3: "5/6", 4: "7/8"}
    gi = {0: "1/32", 1: "1/16", 2: "1/8", 3: "1/4"}[f(36, 37)]
    mode = {0: "2K", 1: "8K"}.get(f(38, 39), "?")
    print(f"TPS: sync words at symbols {found[:6]} (every 68: "
          f"{all((b - a) % 68 == 0 for a, b in zip(found, found[1:]))}), length {f(17, 22)}, "
          f"frame {f(23, 24)}, {const}, hierarchy {f(27, 29)}, HP {cr.get(f(30, 32))}, LP {cr.get(f(33, 35))}, "
          f"GI {gi}, {mode}, cell id byte {f(40, 47)}")


if __name__ == "__main__":
    main()
