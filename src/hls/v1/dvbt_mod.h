/* DVB-T modulator (EN 300 744) for the Fishball 7010 - edgeQAM PoC.
 * Fixed mode: 2K, 64-QAM, code rate 7/8, guard interval 1/32, non-hierarchical, 8 MHz
 * (31.668 Mbit/s of TS, 9.142857 Msample/s out).
 *
 * In : TS stream, 32-bit words, little endian (first byte in bits 7:0), 47 words per packet.
 *      If no packet is waiting at a packet boundary a null packet is inserted.
 * Out: complex baseband, one sample per word: I bits 15:0, Q bits 31:16 (signed 16 bits).
 * Status: st_ts one word per OFDM symbol (bits 7:0 null packets inserted, 15:8 sync errors),
 *         st_fft one word per OFDM symbol (bit 0 IFFT overflow). Must always be read. */
#pragma once

#include <ap_fixed.h>
#include <ap_int.h>
#include <hls_fft.h>
#include <hls_stream.h>
#include <complex>

#define DVBT_NFFT 2048
#define DVBT_NCP 64
#define DVBT_NSYM_OUT (DVBT_NFFT + DVBT_NCP)

struct fft_cfg : hls::ip_fft::params_t {
    static const unsigned ordering_opt = hls::ip_fft::natural_order;
    static const unsigned max_nfft = 11;
    static const unsigned arch_opt = hls::ip_fft::radix_4_burst_io;
    static const unsigned input_width = 16;
    static const unsigned output_width = 16;
    static const unsigned config_width = 16;   /* fwd_inv (1) + scale schedule (12), padded */
    static const unsigned scaling_opt = hls::ip_fft::scaled;
    static const unsigned rounding_opt = hls::ip_fft::convergent_rounding;
    static const unsigned phase_factor_width = 16;
};
typedef hls::ip_fft::config_t<fft_cfg> fftcfg_t;
typedef hls::ip_fft::status_t<fft_cfg> fftst_t;
typedef ap_fixed<16, 1> d_t;
typedef std::complex<d_t> cd_t;

void dvbt_mod(hls::stream<ap_uint<32> > &ts_in, hls::stream<ap_uint<32> > &iq_out,
              hls::stream<ap_uint<16> > &st_ts, hls::stream<ap_uint<16> > &st_fft);
