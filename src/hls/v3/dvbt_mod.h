/* DVB-T multi-multiplex modulator (EN 300 744) for the Fishball 7010 - edgeQAM.
 * Each multiplex: 2K, 64-QAM, 7/8, GI 1/32, non-hierarchical, 8 MHz -> 31.668 Mbit/s of TS.
 * DVBT_NMUX adjacent multiplexes (2 or 4; one alone: hls/v1) share one IFFT of 2048 * DVBT_NMUX points at
 * DVBT_NMUX * 9.142857 Msample/s; multiplex i is centred at (i - (N-1)/2) * 8 MHz.
 *
 * ts_in : TS packets, 32-bit words, little endian, 47 words per packet, packet slots taken
 *         in turn by multiplex 0, 1, ..., N-1, 0, ... A multiplex that has no packet waiting
 *         at a packet boundary sends a null packet.
 * ctl   : optional, bit i = 0 mutes multiplex i (all its carriers zero). Default all on.
 * iq_out: one sample per word, I bits 15:0, Q bits 31:16 (signed 16 bits).
 * st_ts : one word per OFDM symbol: bits 7:0 null packets inserted (all multiplexes),
 *         15:8 sync errors.  st_fft: one word per symbol, bit 0 IFFT overflow.
 *         Both must always be read. */
#pragma once

#include <ap_fixed.h>
#include <ap_int.h>
#include <hls_fft.h>
#include <hls_stream.h>
#include <complex>

#ifndef DVBT_NMUX
#define DVBT_NMUX 2
#endif
#if DVBT_NMUX == 2
#define DVBT_LOG2 12
#elif DVBT_NMUX == 4
#define DVBT_LOG2 13
#else
#error "DVBT_NMUX must be 2 or 4 (one multiplex alone: hls/v1)"
#endif
#define DVBT_NFFT (1 << DVBT_LOG2)
#define DVBT_NCP (DVBT_NFFT / 32)
#define DVBT_NSYM_OUT (DVBT_NFFT + DVBT_NCP)
#define DVBT_NCAR 1705
#define DVBT_MUX_BINS 1792          /* 8 MHz / (1/224 us) */

struct fft_cfg : hls::ip_fft::params_t {
    static const unsigned ordering_opt = hls::ip_fft::natural_order;
    static const unsigned max_nfft = DVBT_LOG2;
    static const unsigned arch_opt = hls::ip_fft::pipelined_streaming_io;
    static const unsigned stages_block_ram = DVBT_LOG2 - 9;   /* 4096: 3, 8192: 4 (IP range) */
    static const unsigned input_width = 16;
    static const unsigned output_width = 16;
    static const unsigned config_width = 16;   /* fwd_inv (1) + scale schedule (<= 14), padded */
    static const unsigned scaling_opt = hls::ip_fft::scaled;
    static const unsigned rounding_opt = hls::ip_fft::convergent_rounding;
    static const unsigned phase_factor_width = 16;
};
typedef hls::ip_fft::config_t<fft_cfg> fftcfg_t;
typedef hls::ip_fft::status_t<fft_cfg> fftst_t;
typedef ap_fixed<16, 1> d_t;
typedef std::complex<d_t> cd_t;

void dvbt_mod(hls::stream<ap_uint<32> > &ts_in, hls::stream<ap_uint<8> > &ctl,
              hls::stream<ap_uint<32> > &iq_out, hls::stream<ap_uint<16> > &st_ts,
              hls::stream<ap_uint<16> > &st_fft);
