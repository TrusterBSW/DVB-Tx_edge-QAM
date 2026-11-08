/* C simulation: dvbt_mod against the dvbtx reference (tests/dvbt_ref.c, cf32 at 9.142857 MS/s).
 * usage: csim argv: in.ts ref.cf32 nsym [starve_from starve_len]
 * Per symbol, least-squares gain fit then SNR = |g ref|^2 / |out - g ref|^2. */
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "dvbt_mod.h"

int main(int argc, char **argv)
{
    const char *tsn = argc > 1 ? argv[1] : "in.ts";
    const char *refn = argc > 2 ? argv[2] : "ref.cf32";
    int nsym = argc > 3 ? atoi(argv[3]) : 200;
    FILE *tf[DVBT_NMUX], *rf = fopen(refn, "rb");
    for (int m = 0; m < DVBT_NMUX; m++) {   /* multiplex m reads from packet m*500, as dvbt_multi_ref */
        tf[m] = fopen(tsn, "rb");
        if (tf[m])
            fseek(tf[m], (long)m * 500 * 188, SEEK_SET);
    }
    if (!tf[0] || !rf) {
        printf("cannot open %s or %s\n", tsn, refn);
        return 1;
    }
    hls::stream<ap_uint<32> > ts_in, iq_out;
    hls::stream<ap_uint<8> > ctl;
    hls::stream<ap_uint<16> > st_ts, st_fft;
    FILE *of = fopen("hls_out.cs16", "wb");

    long fed = 0;
    double worst = 1e9, sum = 0, peak = 0, pw = 0;
    int ovf = 0, nulls = 0, serr = 0, bad = 0;
    std::vector<float> ref(2 * DVBT_NSYM_OUT);
    for (int s = 0; s < nsym; s++) {
        /* keep at least 8 packets waiting so that no null packet is inserted */
        while (ts_in.size() < 8 * 47 * DVBT_NMUX) {
            for (int m = 0; m < DVBT_NMUX; m++) {   /* one packet slot per multiplex in turn */
                unsigned char p[188];
                if (fread(p, 1, 188, tf[m]) != 188) {
                    rewind(tf[m]);
                    if (fread(p, 1, 188, tf[m]) != 188)
                        return 1;
                }
                for (int i = 0; i < 47; i++)
                    ts_in.write((ap_uint<32>)(p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 |
                                              (unsigned)p[4 * i + 3] << 24));
                fed++;
            }
        }
        dvbt_mod(ts_in, ctl, iq_out, st_ts, st_fft);
        ap_uint<16> a = st_ts.read(), b = st_fft.read();
        nulls += a(7, 0);
        serr += a(15, 8);
        if (s >= 2)   /* symbols 0-1: zeroed Forney interleaver -> identical cells -> IFFT overflow */
            ovf += b[0];

        if (fread(ref.data(), sizeof(float), 2 * DVBT_NSYM_OUT, rf) != 2 * DVBT_NSYM_OUT) {
            printf("reference too short\n");
            return 1;
        }
        std::vector<double> oi(DVBT_NSYM_OUT), oq(DVBT_NSYM_OUT);
        short buf[2 * DVBT_NSYM_OUT];
        for (int i = 0; i < DVBT_NSYM_OUT; i++) {
            ap_uint<32> w = iq_out.read();
            short si = (short)(unsigned)w(15, 0), sq = (short)(unsigned)w(31, 16);
            buf[2 * i] = si;
            buf[2 * i + 1] = sq;
            oi[i] = si / 32768.0;
            oq[i] = sq / 32768.0;
            double m = std::fabs(oi[i]) > std::fabs(oq[i]) ? std::fabs(oi[i]) : std::fabs(oq[i]);
            if (m > peak)
                peak = m;
            pw += oi[i] * oi[i] + oq[i] * oq[i];
        }
        fwrite(buf, sizeof buf, 1, of);
        /* g = <out, ref> / <ref, ref> (complex) */
        double nr = 0, ni = 0, d = 0;
        for (int i = 0; i < DVBT_NSYM_OUT; i++) {
            double ri = ref[2 * i], rq = ref[2 * i + 1];
            nr += oi[i] * ri + oq[i] * rq;
            ni += oq[i] * ri - oi[i] * rq;
            d += ri * ri + rq * rq;
        }
        double gr = nr / d, gi = ni / d, es = 0, ee = 0;
        for (int i = 0; i < DVBT_NSYM_OUT; i++) {
            double ri = ref[2 * i], rq = ref[2 * i + 1];
            double pr = gr * ri - gi * rq, pq = gr * rq + gi * ri;
            es += pr * pr + pq * pq;
            ee += (oi[i] - pr) * (oi[i] - pr) + (oq[i] - pq) * (oq[i] - pq);
        }
        double snr = 10 * log10(es / (ee + 1e-30));
        if (s >= 2 && snr < worst)
            worst = snr;
        if (s >= 2 && snr < 40)
            bad++;
        sum += snr;
        if (s < 3 || snr < 40)
            printf("sym %3d  SNR %.1f dB  gain %.6f%+.6fj\n", s, snr, gr, gi);
    }
    double rms = sqrt(pw / (nsym * (double)DVBT_NSYM_OUT));
    printf("RESULT N=%d %d symbols (from #2): mean SNR %.1f dB, worst %.1f dB, %d below 40 dB, rms %.4f FS, peak %.4f FS"
           " (%.1f dB PAPR margin), IFFT overflows %d, null packets %d, sync errors %d, %ld packets fed\n",
           DVBT_NMUX, nsym, sum / nsym, worst, bad, rms, peak, 20 * log10(1 / rms), ovf, nulls, serr, fed);
    fclose(of);
    return (bad == 0 && ovf == 0 && nulls == 0) ? 0 : 1;
}
