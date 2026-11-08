/* DVB-T modulator, see dvbt_mod.h. Port of DVB-Tx_DUAL engine/src/{outer,dvbt,ofdm}.c.
 * Each call of the dataflow region produces one OFDM symbol (2112 samples). */
#include "dvbt_mod.h"
#include "dvbt_tables.h"

/* ---- scaling ------------------------------------------------------------------
 * cells: mean power A^2 with A = 0.125 (64-QAM unit = A/sqrt(42)), pilots 4/3 A, TPS A.
 * IFFT 2048 with 5 right shifts: rms out ~ A*sqrt(1842)/32 = 0.17 of full scale. */
#define SCALE_SCH 0x155   /* 1 bit per radix-4 stage on stages 0..4, last radix-2 stage 0 */
static const d_t QAM_LV[4] = { 1 * 0.019287, 3 * 0.019287, 5 * 0.019287, 7 * 0.019287 };
static const d_t PILOT_A = 0.125 * 4.0 / 3.0;
static const d_t TPS_A = 0.125;

/* ---- outer coding: energy dispersal, RS(204,188), Forney interleaver I=12 M=17 ---- */
static ap_uint<8> xtime(ap_uint<8> a) { return (ap_uint<8>)(a << 1) ^ (a[7] ? ap_uint<8>(0x1D) : ap_uint<8>(0)); }

static ap_uint<8> gmul_const(ap_uint<8> a, const unsigned char b)
{
#pragma HLS inline
    ap_uint<8> r = 0;
    for (int i = 0; i < 8; i++) {
#pragma HLS unroll
        if ((b >> i) & 1)
            r ^= a;
        a = xtime(a);
    }
    return r;
}

static void outer_proc(hls::stream<ap_uint<32> > &ts_in, hls::stream<ap_uint<8> > &ob,
                       hls::stream<ap_uint<16> > &st_ts)
{
    static ap_uint<2> symc = 0;      /* symbol index mod 4: 993, 992, 992, 992 bytes */
    static ap_uint<8> pos = 0;       /* byte in the 204-byte packet */
    static ap_uint<4> br = 0;        /* interleaver branch = pos mod 12 */
    static ap_uint<3> grp = 0;       /* packet in the 8-packet PRBS period */
    static ap_uint<15> prbs = 0;
    static ap_uint<8> par[16] = { 0 };
#pragma HLS array_partition variable = par complete
    static ap_uint<8> ilbuf[1122] = { 0 };
#pragma HLS dependence variable = ilbuf type = inter false
    static ap_uint<8> ilptr[12] = { 0 };
#pragma HLS array_partition variable = ilptr complete
    static bool null_pkt = true;
    static ap_uint<32> word = 0;
    static const ap_uint<11> OFF[12] = { 0, 0, 17, 51, 102, 170, 255, 357, 476, 612, 765, 935 };
    static const ap_uint<8> LEN[12] = { 0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187 };

    int nb = symc == 0 ? 993 : 992;
    ap_uint<8> nulls = 0, syncerr = 0;

BYTES:
    for (int i = 0; i < nb; i++) {
#pragma HLS pipeline II = 2   /* 993 bytes per 23100-cycle symbol: plenty */
        ap_uint<8> m;
        if (pos < 188) {
            if (pos == 0)
                null_pkt = ts_in.empty();
            ap_uint<8> in;
            if (null_pkt) {
                in = pos == 1 ? 0x1F : pos == 3 ? 0x10 : pos == 0 ? 0x47 : 0xFF;
            } else {
                if (pos(1, 0) == 0)
                    word = ts_in.read();
                ap_uint<2> sh = pos(1, 0);
                in = word(8 * sh + 7, 8 * sh);
            }
            if (pos == 0) {
                if (null_pkt)
                    nulls++;
                else if (in != 0x47)
                    syncerr++;
            }
            /* PRBS 1 + X^14 + X^15: reset at each 8-packet period, clocked on every byte
             * position but the first sync byte, applied to all but the sync bytes */
            ap_uint<8> pb = 0;
            if (pos == 0 && grp == 0) {
                prbs = 0x00A9;
            } else {
                for (int b = 0; b < 8; b++) {
#pragma HLS unroll
                    ap_uint<1> o = prbs[13] ^ prbs[14];
                    prbs = (prbs << 1) | o;
                    pb = (pb << 1) | o;
                }
            }
            if (pos == 0)
                m = grp == 0 ? 0xB8 : 0x47;
            else
                m = in ^ pb;
            /* RS systematic encoder */
            ap_uint<8> fb = m ^ par[15];
            for (int j = 15; j > 0; j--) {
#pragma HLS unroll
                par[j] = par[j - 1] ^ gmul_const(fb, RS_G[j]);
            }
            par[0] = gmul_const(fb, RS_G[0]);
        } else {
            m = par[15 - (pos - 188)];
        }

        /* Forney convolutional interleaver */
        ap_uint<8> o;
        if (br == 0) {
            o = m;
        } else {
            ap_uint<11> a = OFF[br] + ilptr[br];
            o = ilbuf[a];
            ilbuf[a] = m;
            ilptr[br] = ilptr[br] + 1 == LEN[br] ? ap_uint<8>(0) : ap_uint<8>(ilptr[br] + 1);
        }
        ob.write(o);

        br = br == 11 ? ap_uint<4>(0) : ap_uint<4>(br + 1);
        if (pos == 203) {
            pos = 0;
            grp++;
            for (int j = 0; j < 16; j++) {
#pragma HLS unroll
                par[j] = 0;
            }
        } else {
            pos++;
        }
    }
    symc++;
    st_ts.write((ap_uint<16>)((syncerr, nulls)));
}

/* ---- inner coding: convolutional code 7/8, bit interleaver, symbol interleaver write --- */
static void inner_proc(hls::stream<ap_uint<8> > &ib, ap_uint<6> cells[1512])
{
    static ap_uint<1> sympar = 0;
    static ap_uint<7> sr = 0;
    static ap_uint<8> byte = 0;
    static ap_uint<4> nbits = 0;
    static ap_uint<3> pj = 0;       /* position in the puncturing period XYYYYXYX */
    ap_uint<1> B[6][126];
#pragma HLS array_partition variable = B dim = 1 complete
    static const ap_uint<7> HSH[6] = { 0, 63, 105, 42, 21, 84 };   /* h(e) */
    static const ap_uint<3> INV[6] = { 0, 3, 1, 4, 2, 5 };          /* bit e <- demux input INV[e] */

    ap_uint<11> q = 0;
BLOCKS:
    for (int blk = 0; blk < 12; blk++) {
        ap_uint<3> i6 = 0;
        ap_uint<7> w = 0;
FILL:
        for (int c = 0; c < 756; c++) {
#pragma HLS pipeline II = 1
#pragma HLS dependence variable = B type = inter false
            if (pj != 1) {
                if (nbits == 0) {
                    byte = ib.read();
                    nbits = 8;
                }
                sr = (sr(5, 0), byte[7]);
                byte <<= 1;
                nbits--;
            }
            /* X = 171o (mask 0x4F), Y = 133o (mask 0x6D), bit 0 = newest */
            ap_uint<1> x = sr[0] ^ sr[1] ^ sr[2] ^ sr[3] ^ sr[6];
            ap_uint<1> y = sr[0] ^ sr[2] ^ sr[3] ^ sr[5] ^ sr[6];
            ap_uint<1> bit = (pj == 0 || pj == 5 || pj == 7) ? x : y;
            pj++;
            B[i6][w] = bit;
            if (i6 == 5) {
                i6 = 0;
                w++;
            } else {
                i6++;
            }
        }
READ:
        for (int r = 0; r < 126; r++) {
#pragma HLS pipeline II = 1
            ap_uint<6> cell = 0;
            for (int e = 0; e < 6; e++) {
#pragma HLS unroll
                ap_uint<8> a = r + HSH[e];
                if (a >= 126)
                    a -= 126;
                cell[5 - e] = B[INV[e]][a];
            }
            ap_uint<11> addr = sympar == 0 ? H_ROM[q] : q;
            cells[addr] = cell;
            q++;
        }
    }
    sympar++;
}

/* ---- frame: pilots, TPS, carrier map, in IFFT input order ---------------------- */
static void frame_proc(ap_uint<6> cells[1512], hls::stream<cd_t> &bins, hls::stream<fftcfg_t> &fcfg)
{
    static ap_uint<7> l = 0;        /* symbol in frame */
    static ap_uint<2> f = 0;        /* frame in superframe */
    static bool tsign = false;

    fftcfg_t c;
    c.setDir(0);                    /* inverse */
    c.setSch(SCALE_SCH);
    fcfg.write(c);

    if (l == 0)
        tsign = false;
    else if (TPS_ROM[f][l])
        tsign = !tsign;
    ap_uint<2> lm = l(1, 0);
    ap_uint<1> par = l[0];
    ap_uint<4> sp = 3 * lm;         /* scattered pilots on k mod 12 == 3 (l mod 4) */
    ap_uint<11> dn = D0[lm];
    ap_uint<11> k = 852;
    ap_uint<4> k12 = 0;

BINS:
    for (int n = 0; n < DVBT_NFFT; n++) {
#pragma HLS pipeline II = 1
        if (n == 1196) {
            k = 0;
            k12 = 0;
            dn = 0;
        }
        bool act = n <= 852 || n >= 1196;
        cd_t v(0, 0);
        if (act) {
            ap_uint<3> cr = CAR_ROM[k];
            bool wk = cr[2];
            if (cr[0] || k12 == sp) {
                v = cd_t(wk ? d_t(-PILOT_A) : PILOT_A, 0);
            } else if (cr[1]) {
                v = cd_t((wk != tsign) ? d_t(-TPS_A) : TPS_A, 0);
            } else {
                ap_uint<11> a = par == 0 ? dn : H_ROM[dn];
                ap_uint<6> y = cells[a];          /* y0 = bit 5 */
                /* |re| = 4 + (1-2 y2)(2 + (1-2 y4)): (y2,y4) 00->7 01->5 10->1 11->3 */
                ap_uint<2> ri = y[3] ? ap_uint<2>(y[1] ? 1 : 0) : ap_uint<2>(y[1] ? 2 : 3);
                ap_uint<2> ii = y[2] ? ap_uint<2>(y[0] ? 1 : 0) : ap_uint<2>(y[0] ? 2 : 3);
                d_t re = QAM_LV[ri], im = QAM_LV[ii];
                v = cd_t(y[5] ? d_t(-re) : re, y[4] ? d_t(-im) : im);
                dn++;
            }
            k++;
            k12 = k12 == 11 ? ap_uint<4>(0) : ap_uint<4>(k12 + 1);
        }
        bins.write(v);
    }
    if (l == 67) {
        l = 0;
        f++;
    } else {
        l++;
    }
}

/* ---- cyclic prefix and output ------------------------------------------------- */
static void cp_proc(hls::stream<cd_t> &xk, hls::stream<fftst_t> &fst, hls::stream<ap_uint<32> > &iq,
                    hls::stream<ap_uint<16> > &st_fft)
{
    ap_uint<32> buf[DVBT_NFFT];
IN:
    for (int i = 0; i < DVBT_NFFT; i++) {
#pragma HLS pipeline II = 1
        cd_t v = xk.read();
        ap_uint<32> w;
        w(15, 0) = v.real().range(15, 0);
        w(31, 16) = v.imag().range(15, 0);
        buf[i] = w;
    }
    fftst_t s = fst.read();
    st_fft.write((ap_uint<16>)s.getOvflo());
OUT:
    for (int i = 0; i < DVBT_NSYM_OUT; i++) {
#pragma HLS pipeline II = 1
        int a = i < DVBT_NCP ? i + DVBT_NFFT - DVBT_NCP : i - DVBT_NCP;
        iq.write(buf[a]);
    }
}

void dvbt_mod(hls::stream<ap_uint<32> > &ts_in, hls::stream<ap_uint<32> > &iq_out,
              hls::stream<ap_uint<16> > &st_ts, hls::stream<ap_uint<16> > &st_fft)
{
#pragma HLS interface mode = axis port = ts_in
#pragma HLS interface mode = axis port = iq_out
#pragma HLS interface mode = axis port = st_ts
#pragma HLS interface mode = axis port = st_fft
#pragma HLS interface mode = ap_ctrl_none port = return
#pragma HLS dataflow
    hls::stream<ap_uint<8>, 64> ob("ob");
    ap_uint<6> cells[1512];
    hls::stream<cd_t, 4> bins("bins");
    hls::stream<cd_t, 4> xk("xk");
    hls::stream<fftcfg_t, 2> fcfg("fcfg");
    hls::stream<fftst_t, 2> fst("fst");

    outer_proc(ts_in, ob, st_ts);
    inner_proc(ob, cells);
    frame_proc(cells, bins, fcfg);
    hls::fft<fft_cfg>(bins, xk, fst, fcfg);
    cp_proc(xk, fst, iq_out, st_fft);
}
