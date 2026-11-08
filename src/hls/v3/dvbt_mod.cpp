/* DVB-T multi-multiplex modulator, v3: one coding chain time-shared by the DVBT_NMUX
 * multiplexes, pipelined IFFT, guard interval without a symbol buffer.
 * Port of DVB-Tx_DUAL engine/src/{outer,dvbt,ofdm}.c; same interface as v2 (dvbt_mod.h).
 *
 * Guard interval: the IFFT input bin n is multiplied by exp(-j 2 pi n NCP / NFFT), which
 * rotates the IFFT output by NCP samples: x'[i] = x[(i - NCP) mod NFFT]. The symbol with its
 * guard interval, x[N-NCP..N-1] x[0..N-1], is then x'[0..N-1] followed by x'[0..NCP-1]:
 * only NCP samples are kept, and the output streams as the IFFT produces it. */
#include "dvbt_mod.h"
#include "dvbt_tables.h"

#if DVBT_NMUX == 2
#define SCALE_SCH 0x555    /* 12 stages (6 pairs), shift 1 per pair: rms 0.12 FS */
#else
#define SCALE_SCH 0x1555   /* 13 stages (6 pairs + 1), shift 1 each: rms 0.084 FS */
#endif
static const d_t QAM_LV[4] = { 1 * 0.019287, 3 * 0.019287, 5 * 0.019287, 7 * 0.019287 };
static const d_t PILOT_A = 0.125 * 4.0 / 3.0;
static const d_t TPS_A = 0.125;

/* order of the multiplexes in the natural IFFT bin order: positive frequencies first */
#if DVBT_NMUX == 2
static const int MUX_ORDER[2] = { 1, 0 };
#else
static const int MUX_ORDER[4] = { 2, 3, 0, 1 };
#endif

/* ---- input: packet slots in turn to each multiplex ------------------------------ */
static void split_proc(hls::stream<ap_uint<32> > &ts_in, hls::stream<ap_uint<32> > ts_m[DVBT_NMUX])
{
    static ap_uint<6> w = 0;
    static ap_uint<2> m = 0;
SPLIT:
    for (int it = 0; it < 4 * 5 * 47 * DVBT_NMUX; it++) {
#pragma HLS pipeline II = 1
        if (!ts_in.empty() && !ts_m[m].full()) {
            ts_m[m].write(ts_in.read());
            if (w == 46) {
                w = 0;
                m = m == DVBT_NMUX - 1 ? ap_uint<2>(0) : ap_uint<2>(m + 1);
            } else {
                w++;
            }
        }
    }
}

/* ---- outer coding, shared: PRBS, RS(204,188), Forney interleaver ----------------- */
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

static void outer_proc(hls::stream<ap_uint<32> > ts_m[DVBT_NMUX], hls::stream<ap_uint<8> > &ob,
                       hls::stream<ap_uint<16> > &st_ts)
{
    static ap_uint<2> symc = 0;
    /* per multiplex state */
    static ap_uint<8> s_pos[DVBT_NMUX] = { 0 };
    static ap_uint<4> s_br[DVBT_NMUX] = { 0 };
    static ap_uint<3> s_grp[DVBT_NMUX] = { 0 };
    static ap_uint<15> s_prbs[DVBT_NMUX] = { 0 };
    static ap_uint<8> s_par[DVBT_NMUX][16] = { { 0 } };
#pragma HLS array_partition variable = s_par dim = 2 complete
    static ap_uint<8> s_ilptr[DVBT_NMUX][12] = { { 0 } };
#pragma HLS array_partition variable = s_ilptr dim = 2 complete
    static bool s_null[DVBT_NMUX] = { 0 };
    static ap_uint<32> s_word[DVBT_NMUX] = { 0 };
    static ap_uint<8> ilbuf[DVBT_NMUX * 1122] = { 0 };
#pragma HLS reset variable = ilbuf off   /* no per-byte reset logic: stale content after a reset only affects the first ~11 packets */
#pragma HLS dependence variable = ilbuf type = inter false
    static const ap_uint<11> OFF[12] = { 0, 0, 17, 51, 102, 170, 255, 357, 476, 612, 765, 935 };
    static const ap_uint<8> LEN[12] = { 0, 17, 34, 51, 68, 85, 102, 119, 136, 153, 170, 187 };

    int nb = symc == 0 ? 993 : 992;
    ap_uint<8> nulls = 0, syncerr = 0;
MUXES:
    for (int m = 0; m < DVBT_NMUX; m++) {
        ap_uint<8> pos = s_pos[m];
        ap_uint<4> br = s_br[m];
        ap_uint<3> grp = s_grp[m];
        ap_uint<15> prbs = s_prbs[m];
        bool null_pkt = s_null[m];
        ap_uint<32> word = s_word[m];
        ap_uint<8> par[16], ilptr[12];
#pragma HLS array_partition variable = par complete
#pragma HLS array_partition variable = ilptr complete
        for (int j = 0; j < 16; j++) {
#pragma HLS unroll
            par[j] = s_par[m][j];
        }
        for (int j = 0; j < 12; j++) {
#pragma HLS unroll
            ilptr[j] = s_ilptr[m][j];
        }
        ap_uint<13> base = m * 1122;
BYTES:
        for (int i = 0; i < nb; i++) {
#pragma HLS pipeline II = 2
            ap_uint<8> mb;
            if (pos < 188) {
                if (pos == 0)
                    null_pkt = ts_m[m].empty();
                ap_uint<8> in;
                if (null_pkt) {
                    in = pos == 1 ? 0x1F : pos == 3 ? 0x10 : pos == 0 ? 0x47 : 0xFF;
                } else {
                    if (pos(1, 0) == 0)
                        word = ts_m[m].read();
                    ap_uint<2> sh = pos(1, 0);
                    in = word(8 * sh + 7, 8 * sh);
                }
                if (pos == 0) {
                    if (null_pkt)
                        nulls++;
                    else if (in != 0x47)
                        syncerr++;
                }
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
                mb = pos == 0 ? ap_uint<8>(grp == 0 ? 0xB8 : 0x47) : ap_uint<8>(in ^ pb);
                ap_uint<8> fb = mb ^ par[15];
                for (int j = 15; j > 0; j--) {
#pragma HLS unroll
                    par[j] = par[j - 1] ^ gmul_const(fb, RS_G[j]);
                }
                par[0] = gmul_const(fb, RS_G[0]);
            } else {
                mb = par[15 - (pos - 188)];
            }
            ap_uint<8> o;
            if (br == 0) {
                o = mb;
            } else {
                ap_uint<13> a = base + OFF[br] + ilptr[br];
                o = ilbuf[a];
                ilbuf[a] = mb;
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
        s_pos[m] = pos;
        s_br[m] = br;
        s_grp[m] = grp;
        s_prbs[m] = prbs;
        s_null[m] = null_pkt;
        s_word[m] = word;
        for (int j = 0; j < 16; j++) {
#pragma HLS unroll
            s_par[m][j] = par[j];
        }
        for (int j = 0; j < 12; j++) {
#pragma HLS unroll
            s_ilptr[m][j] = ilptr[j];
        }
    }
    symc++;
    st_ts.write((ap_uint<16>)((syncerr, nulls)));
}

/* ---- inner coding, shared: code 7/8, 6 coded bits (one bit-interleaver row) per cycle */
static void inner_proc(hls::stream<ap_uint<8> > &ib, ap_uint<6> cells[DVBT_NMUX * 1512])
{
    static ap_uint<1> sympar = 0;
    static ap_uint<7> s_sr[DVBT_NMUX] = { 0 };
    static ap_uint<16> s_res[DVBT_NMUX] = { 0 };   /* info bits reservoir, oldest bit first */
    static ap_uint<5> s_cnt[DVBT_NMUX] = { 0 };
    static ap_uint<3> s_pj[DVBT_NMUX] = { 0 };
    ap_uint<1> B[6][126];
#pragma HLS array_partition variable = B dim = 1 complete
    static const ap_uint<7> HSH[6] = { 0, 63, 105, 42, 21, 84 };
    static const ap_uint<3> INV[6] = { 0, 3, 1, 4, 2, 5 };

MUXES:
    for (int m = 0; m < DVBT_NMUX; m++) {
        ap_uint<7> sr = s_sr[m];
        ap_uint<16> res = s_res[m];
        ap_uint<5> cnt = s_cnt[m];
        ap_uint<3> pj = s_pj[m];
        ap_uint<13> cbase = m * 1512;
        ap_uint<11> q = 0;
BLOCKS:
        for (int blk = 0; blk < 12; blk++) {
FILL:
            for (int w = 0; w < 126; w++) {
#pragma HLS pipeline II = 1
#pragma HLS dependence variable = B type = inter false
                /* info bits used by this row: 5 if the period position 1 (Y only) is in it */
                ap_uint<3> d1 = 1 - pj;
                ap_uint<3> need = d1 < 6 ? 5 : 6;
                if (cnt < need) {       /* one byte refill is enough; read only when needed */
                    ap_uint<8> by = ib.read();
                    res = res | ((ap_uint<16>)by << (8 - cnt));
                    cnt += 8;
                }
                /* h = 6 previous bits then the next 6 info bits; encoder state after t new bits =
                 * h[t+6 .. t] (bit 0 newest). X/Y for t = 1..6 computed in parallel, then picked
                 * according to the puncturing phase: pure wiring, no carried chain */
                ap_uint<12> h;
                for (int b = 0; b < 6; b++) {
#pragma HLS unroll
                    h[11 - b] = sr[5 - b];        /* oldest first at the top */
                    h[5 - b] = res[15 - b];
                }
                ap_uint<1> X[7], Y[7];
                for (int t = 0; t <= 6; t++) {
#pragma HLS unroll
                    /* state register: bit j = h bit (6 - t) + j ... newest = h[6 - t] */
                    ap_uint<1> s0 = h[6 - t + 0], s1 = h[6 - t + 1], s2 = h[6 - t + 2], s3 = h[6 - t + 3];
                    ap_uint<1> s5 = h[6 - t + 5], s6 = (6 - t + 6) <= 11 ? h[6 - t + 6] : sr[6];
                    X[t] = s0 ^ s1 ^ s2 ^ s3 ^ s6;
                    Y[t] = s0 ^ s2 ^ s3 ^ s5 ^ s6;
                }
                ap_uint<6> row = 0;
                ap_uint<3> c = 0;
                ap_uint<3> pp = pj;
                for (int i = 0; i < 6; i++) {
#pragma HLS unroll
                    if (pp != 1)
                        c++;
                    row[i] = (pp == 0 || pp == 5 || pp == 7) ? X[c] : Y[c];
                    pp++;
                }
                /* new state: the last 7 bits after consuming c (= need) info bits */
                ap_uint<13> hs = (sr[6], h);
                sr = hs(12 - c, 6 - c);
                res <<= c;
                cnt -= c;
                pj += 6;
                for (int i = 0; i < 6; i++) {
#pragma HLS unroll
                    B[i][w] = row[i];
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
                cells[cbase + addr] = cell;
                q++;
            }
        }
        s_sr[m] = sr;
        s_res[m] = res;
        s_cnt[m] = cnt;
        s_pj[m] = pj;
    }
    sympar++;
}

/* ---- frame, shared: carriers of each multiplex in IFFT bin order ----------------- */
static void frame_proc(ap_uint<6> cells[DVBT_NMUX * 1512], hls::stream<cd_t> &car)
{
    static ap_uint<7> l = 0;
    static ap_uint<2> f = 0;
    static bool tsign = false;

    if (l == 0)
        tsign = false;
    else if (TPS_ROM[f][l])
        tsign = !tsign;
    ap_uint<1> par = l[0];
    ap_uint<4> sp = 3 * l(1, 0);
MUXES:
    for (int o = 0; o < DVBT_NMUX; o++) {
        ap_uint<13> cbase = MUX_ORDER[o] * 1512;
        ap_uint<11> dn = 0;
        ap_uint<4> k12 = 0;
CARS:
        for (int k = 0; k < DVBT_NCAR; k++) {
#pragma HLS pipeline II = 1
            cd_t v;
            ap_uint<3> cr = CAR_ROM[k];
            bool wk = cr[2];
            if (cr[0] || k12 == sp) {
                v = cd_t(wk ? d_t(-PILOT_A) : PILOT_A, 0);
            } else if (cr[1]) {
                v = cd_t((wk != tsign) ? d_t(-TPS_A) : TPS_A, 0);
            } else {
                ap_uint<11> a = par == 0 ? dn : H_ROM[dn];
                ap_uint<6> y = cells[cbase + a];
                ap_uint<2> ri = y[3] ? ap_uint<2>(y[1] ? 1 : 0) : ap_uint<2>(y[1] ? 2 : 3);
                ap_uint<2> ii = y[2] ? ap_uint<2>(y[0] ? 1 : 0) : ap_uint<2>(y[0] ? 2 : 3);
                d_t re = QAM_LV[ri], im = QAM_LV[ii];
                v = cd_t(y[5] ? d_t(-re) : re, y[4] ? d_t(-im) : im);
                dn++;
            }
            k12 = k12 == 11 ? ap_uint<4>(0) : ap_uint<4>(k12 + 1);
            car.write(v);
        }
    }
    if (l == 67) {
        l = 0;
        f++;
    } else {
        l++;
    }
}

/* ---- bins: multiplexes placed, muted ones zeroed, rotation for the guard interval -- */
static void place_proc(hls::stream<cd_t> &car, hls::stream<ap_uint<8> > &ctl, hls::stream<cd_t> &bins,
                       hls::stream<fftcfg_t> &fcfg)
{
    static ap_uint<8> mask = 0xFF;
    if (!ctl.empty())
        mask = ctl.read();
    fftcfg_t c;
    c.setDir(0);
    c.setSch(SCALE_SCH);
    fcfg.write(c);
    /* exp(-j 2 pi n / 32) */
    static const d_t ROT_C[32] = { 0.99997, 0.98079, 0.92388, 0.83147, 0.70711, 0.55557, 0.38268, 0.19509,
                                   0, -0.19509, -0.38268, -0.55557, -0.70711, -0.83147, -0.92388, -0.98079,
                                   -0.99997, -0.98079, -0.92388, -0.83147, -0.70711, -0.55557, -0.38268, -0.19509,
                                   0, 0.19509, 0.38268, 0.55557, 0.70711, 0.83147, 0.92388, 0.98079 };
BINS:
    for (int n = 0; n < DVBT_NFFT; n++) {
#pragma HLS pipeline II = 1
        cd_t v(0, 0);
        bool act = false, on = false;
        for (int i = 0; i < DVBT_NMUX; i++) {
#pragma HLS unroll
            const int start = ((2 * i - (DVBT_NMUX - 1)) * DVBT_MUX_BINS / 2 - 852 + DVBT_NFFT) % DVBT_NFFT;
            ap_uint<DVBT_LOG2> rel = n - start;
            if (rel < DVBT_NCAR) {
                act = true;
                on = mask[i];
            }
        }
        if (act) {          /* a single read per bin */
            cd_t x = car.read();
            if (on)
                v = x;
        }
        ap_uint<5> ph = n & 31;
        d_t cr = ROT_C[ph], ci = ROT_C[(ph + 8) & 31];   /* cos, -sin = cos(a + pi/2) */
        /* (a + jb)(c + jd) with d = -sin(2 pi n / 32) */
        ap_fixed<32, 2> re = v.real() * cr - v.imag() * ci;
        ap_fixed<32, 2> im = v.real() * ci + v.imag() * cr;
        bins.write(cd_t((d_t)re, (d_t)im));
    }
}

static void stat_proc(hls::stream<ap_uint<16> > &st_ts_in, hls::stream<ap_uint<16> > &st_ts)
{
    st_ts.write(st_ts_in.read());
}

/* ---- output: the rotated IFFT output, then its first NCP samples again ------------ */
static void cp_proc(hls::stream<cd_t> &xk, hls::stream<fftst_t> &fst, hls::stream<ap_uint<32> > &iq,
                    hls::stream<ap_uint<16> > &st_fft)
{
    ap_uint<32> head[DVBT_NCP];
OUT:
    for (int i = 0; i < DVBT_NFFT; i++) {
#pragma HLS pipeline II = 1
        cd_t v = xk.read();
        ap_uint<32> w;
        w(15, 0) = v.real().range(15, 0);
        w(31, 16) = v.imag().range(15, 0);
        if (i < DVBT_NCP)
            head[i] = w;
        iq.write(w);
    }
    fftst_t s = fst.read();
    st_fft.write((ap_uint<16>)s.getOvflo());
TAIL:
    for (int i = 0; i < DVBT_NCP; i++) {
#pragma HLS pipeline II = 1
        iq.write(head[i]);
    }
}

void dvbt_mod(hls::stream<ap_uint<32> > &ts_in, hls::stream<ap_uint<8> > &ctl,
              hls::stream<ap_uint<32> > &iq_out, hls::stream<ap_uint<16> > &st_ts,
              hls::stream<ap_uint<16> > &st_fft)
{
#pragma HLS interface mode = axis port = ts_in
#pragma HLS interface mode = axis port = ctl
#pragma HLS interface mode = axis port = iq_out
#pragma HLS interface mode = axis port = st_ts
#pragma HLS interface mode = axis port = st_fft
#pragma HLS interface mode = ap_ctrl_none port = return
#pragma HLS dataflow
#ifndef __SYNTHESIS__
    static   /* C simulation: keep the packets left over between calls, as the FIFOs do */
#endif
    hls::stream<ap_uint<32> > ts_m[DVBT_NMUX];
#pragma HLS stream variable = ts_m depth = 512
    hls::stream<ap_uint<8>, 64> ob("ob");
    hls::stream<ap_uint<16>, 2> st_o("st_o");
    ap_uint<6> cells[DVBT_NMUX * 1512];
    hls::stream<cd_t, 4> car("car");
    hls::stream<cd_t, 4> bins("bins");
    hls::stream<cd_t, 4> xk("xk");
    hls::stream<fftcfg_t, 2> fcfg("fcfg");
    hls::stream<fftst_t, 2> fst("fst");

    split_proc(ts_in, ts_m);
    outer_proc(ts_m, ob, st_o);
    stat_proc(st_o, st_ts);
    inner_proc(ob, cells);
    frame_proc(cells, car);
    place_proc(car, ctl, bins, fcfg);
    hls::fft<fft_cfg>(bins, xk, fst, fcfg);
    cp_proc(xk, fst, iq_out, st_fft);
}
