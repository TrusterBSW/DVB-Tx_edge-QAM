#!/usr/bin/env python3
"""Extracts multiplex i from an edgeQAM capture with N multiplexes (cs16 at N x 9.142857 MS/s):
shift to 0 Hz, low-pass, decimate by N -> cs16 at 9.142857 MS/s (for gr_dvbt_rx.py).
usage: mux_extract.py in.cs16 N i out.cs16 [F0_HZ]   (F0: offset of the wanted multiplex, overrides N/i)"""
import sys
import numpy as np
from scipy import signal

src, n, i, dst = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
raw = np.fromfile(src, dtype=np.int16).astype(np.float32)
x = raw[0::2] + 1j * raw[1::2]
fs = n * 64e6 / 7
# DC blocker (one pole, ~300 Hz, far narrower than the 4.46 kHz carrier spacing): HackRF DC spike
alpha = 2 * np.pi * 300 / fs
x = signal.lfilter([1, -1], [1, -(1 - alpha)], x)
f0 = float(sys.argv[5]) if len(sys.argv) > 5 else (2 * i - (n - 1)) * 4e6
x = x * np.exp(-2j * np.pi * f0 / fs * np.arange(len(x)))
h = signal.firwin(255, 3.9e6, fs=fs, window=("kaiser", 8.0))
y = signal.lfilter(h, 1, x)[::n] * n ** 0.5
out = np.empty(2 * len(y), np.int16)
out[0::2] = np.clip(np.round(y.real), -32768, 32767)
out[1::2] = np.clip(np.round(y.imag), -32768, 32767)
out.tofile(dst)
print(f"mux {i}/{n}: {len(y)} samples at {fs / n / 1e6:.6f} MS/s, rms {np.sqrt(np.mean(abs(y) ** 2)):.1f}")
