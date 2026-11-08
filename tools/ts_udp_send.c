/* ts_udp_send: sends TS files over UDP at a constant rate (7 packets per datagram), in a loop.
 * Test source for the edgeQAM modulator (one stream per multiplex).
 * usage: ts_udp_send HOST RATE_BPS SECONDS file1.ts:port1 [file2.ts:port2 ...]
 * gcc -O2 -o ts_udp_send ts_udp_send.c */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define DG (7 * 188)
#define MAXS 8

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr, "usage: %s HOST RATE_BPS SECONDS file.ts:port ...\n", argv[0]);
        return 1;
    }
    double rate = atof(argv[2]), dur = atof(argv[3]);
    int ns = argc - 4, fd = socket(AF_INET, SOCK_DGRAM, 0), sz = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
    struct { unsigned char *d; long len, pos; struct sockaddr_in a; long sent; } s[MAXS];
    for (int i = 0; i < ns && i < MAXS; i++) {
        char *c = strrchr(argv[4 + i], ':');
        *c = 0;
        FILE *f = fopen(argv[4 + i], "rb");
        if (!f) { perror(argv[4 + i]); return 1; }
        fseek(f, 0, SEEK_END);
        s[i].len = ftell(f) / DG * DG;
        rewind(f);
        s[i].d = malloc(s[i].len);
        if (fread(s[i].d, 1, s[i].len, f) != (size_t)s[i].len) { perror("read"); return 1; }
        fclose(f);
        s[i].pos = 0;
        s[i].sent = 0;
        memset(&s[i].a, 0, sizeof s[i].a);
        s[i].a.sin_family = AF_INET;
        s[i].a.sin_port = htons(atoi(c + 1));
        inet_pton(AF_INET, argv[1], &s[i].a.sin_addr);
    }
    double t0 = now(), per = DG * 8.0 / rate;   /* seconds per datagram per stream */
    long errs = 0;
    while (now() - t0 < dur) {
        long due = (long)((now() - t0) / per) + 1;
        for (int i = 0; i < ns; i++)
            while (s[i].sent < due) {
                if (sendto(fd, s[i].d + s[i].pos, DG, 0, (struct sockaddr *)&s[i].a, sizeof s[i].a) != DG)
                    errs++;
                s[i].pos = (s[i].pos + DG) % s[i].len;
                s[i].sent++;
            }
        usleep(500);
    }
    double el = now() - t0;
    for (int i = 0; i < ns; i++)
        printf("stream %d: %ld datagrams, %.3f Mbit/s\n", i, s[i].sent, s[i].sent * DG * 8.0 / el / 1e6);
    printf("send errors: %ld\n", errs);
    return 0;
}
