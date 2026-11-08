/* Reference for dvbt_mod with N multiplexes: dvbtx DVB-T coder + ofdm_group (IFFT 2048*N).
 * Multiplex i reads in.ts from packet i*500 (wrapping), is centred on bin (i-(N-1)/2)*1792.
 * build: gcc -O2 -I$DVBTX/engine/src gen/dvbt_multi_ref.c $DVBTX/engine/src/{dvbt,outer,ofdm,util}.c -lfftw3f -lm -lpthread
 * usage: dvbt_multi_ref in.ts out.cf32 N nsym */
#include <stdio.h>
#include <stdlib.h>
#include "dvbt.h"
#include "ofdm.h"

typedef struct { FILE *f; } src;
static void pull(void *ctx, uint8_t *pkt)
{
    src *s = ctx;
    if (fread(pkt, 1, 188, s->f) != 188) { rewind(s->f); if (fread(pkt, 1, 188, s->f) != 188) exit(1); }
}

int main(int argc, char **argv)
{
    if (argc < 5) { fprintf(stderr, "usage: in.ts out.cf32 N nsym\n"); return 1; }
    int n = atoi(argv[3]), nsym = atoi(argv[4]), nfft = 2048 * n;
    FILE *o = fopen(argv[2], "wb");
    dvbt_cfg c = { 2048, 6, 7, 8, 32, 8e6, 0 };
    dvbt *t[4];
    src s[4];
    ofdm_group *g = ofdm_group_new(nfft, nfft / 32, 1);
    for (int i = 0; i < n; i++) {
        s[i].f = fopen(argv[1], "rb");
        fseek(s[i].f, (long)i * 500 * 188, SEEK_SET);
        t[i] = dvbt_new(&c, pull, &s[i]);
        if (ofdm_group_add(g, 1705, (2 * i - (n - 1)) * 1792 / 2, 1.0f) < 0) { fprintf(stderr, "add\n"); return 1; }
    }
    float complex *car[4], *out = malloc(sizeof(float complex) * ofdm_group_len(g));
    for (int i = 0; i < n; i++) car[i] = malloc(sizeof(float complex) * 1705);
    for (int k = 0; k < nsym; k++) {
        for (int i = 0; i < n; i++) dvbt_symbol(t[i], car[i]);
        ofdm_group_synth(g, (const float complex *const *)car, out);
        fwrite(out, sizeof(float complex), ofdm_group_len(g), o);
    }
    fclose(o);
    return 0;
}
