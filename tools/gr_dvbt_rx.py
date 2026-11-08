#!/usr/bin/env python3
"""Software DVB-T receiver (GNU Radio gr-dtv) for an edgeQAM capture: cs16 I/Q of one
multiplex at 9.142857 MS/s (2K, 64-QAM, 7/8, GI 1/32) -> TS file.
usage: gr_dvbt_rx.py capture.cs16 out.ts"""
import sys
import numpy as np
from gnuradio import blocks, dtv, fft, gr
from gnuradio.fft import window


def main():
    cap, out = sys.argv[1], sys.argv[2]
    tb = gr.top_block()
    # cs16 file -> complex float (no copy of the whole capture in memory)
    src16 = blocks.file_source(gr.sizeof_short, cap, False)
    s2f = blocks.short_to_float(1, 2048.0)
    deint = blocks.deinterleave(gr.sizeof_float)
    f2c = blocks.float_to_complex(1)
    tb.connect(src16, s2f, deint)
    tb.connect((deint, 0), (f2c, 0))
    tb.connect((deint, 1), (f2c, 1))
    src = f2c
    acq = dtv.dvbt_ofdm_sym_acquisition(1, 2048, 1705, 64, 30)
    f = fft.fft_vcc(2048, True, window.rectangular(2048), True, 1)
    ref = dtv.dvbt_demod_reference_signals(gr.sizeof_gr_complex, 2048, 1512, dtv.MOD_64QAM, dtv.NH,
                                           dtv.C7_8, dtv.C7_8, dtv.GI_1_32, dtv.T2k, 1, 0)
    demap = dtv.dvbt_demap(1512, dtv.MOD_64QAM, dtv.NH, dtv.T2k, 1)
    sdeint = dtv.dvbt_symbol_inner_interleaver(1512, dtv.T2k, 0)
    v2s = blocks.vector_to_stream(gr.sizeof_char, 1512)
    bdeint = dtv.dvbt_bit_inner_deinterleaver(1512, dtv.MOD_64QAM, dtv.NH, dtv.T2k)
    vit = dtv.dvbt_viterbi_decoder(dtv.MOD_64QAM, dtv.NH, dtv.C7_8, 768)
    cdeint = dtv.dvbt_convolutional_deinterleaver(136, 12, 17)
    rs = dtv.dvbt_reed_solomon_dec(2, 8, 0x11d, 255, 239, 8, 51, 8)
    desc = dtv.dvbt_energy_descramble(8)
    snk = blocks.file_sink(gr.sizeof_char, out)
    tb.connect(src, acq, f, ref, demap, sdeint, bdeint, v2s, vit, cdeint, rs, desc, snk)
    tb.run()


if __name__ == "__main__":
    main()
