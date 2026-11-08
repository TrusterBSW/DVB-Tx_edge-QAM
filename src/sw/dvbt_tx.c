/* dvbt_tx - feeds the FPGA DVB-T modulator of the Fishball (edgeQAM) with MPEG-TS.
 *
 * The TS packets go through the standard TX DMA (IIO buffer of cf-ad9361-dds-core-lpc); the
 * bitstream fishball_dvbt_iio routes that stream to the modulator when bit 1 of the DAC GPIO
 * register is set. With N multiplexes, packet slots are taken in turn by multiplex 0..N-1.
 * One input per multiplex: a UDP port (unicast or multicast group) or a file (looped, '-' =
 * stdin). A multiplex without a packet ready gets a null packet; a multiplex without input
 * is disabled (no carriers).
 *
 * dvbt_tx -n N -f FREQ_MUX1_HZ [-a ATT_DB] [-p HTTP_PORT] [-c CONF] [-d DISABLE_MASK] [-v] IN0 [IN1 ...]
 *   IN = udp:PORT | udp:GROUP:PORT | file:PATH | -        (same count as N, missing = nulls)
 * Each multiplex: 2K 64-QAM 7/8 GI 1/32 8 MHz, 31.668 Mbit/s. -f / freq= is the centre frequency of
 * multiplex 1 (as tuned on a TV); multiplex k (1..N) is at FREQ + (k-1) * 8 MHz, in adjacent channels.
 * The AD9363 LO is in the middle: FREQ + (N-1) * 4 MHz. N = 1 needs bitstream v1, N = 2 or 4 the
 * dedicated edgeQAM bitstreams.
 * HTTP API (default port 8080):
 *   GET /status                              JSON: rates, FPGA counters, inputs, temperatures
 *   GET /set?freq=HZ&att=DB&disable=MASK&save=1  live changes (save: rewrite FREQ/ATT/DISABLE in CONF)
 *       disable: bit i = 1 -> multiplex i+1 not transmitted (no carriers in its 8 MHz channel)
 * Status register (gpio_in): sel 0 symbols, 1 nulls inserted by the FPGA, 2 DAC underflows,
 * 3 {sync errors, IFFT overflows}. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <iio.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define TS 188
#define MAXMUX 4
#define RING (1 << 13)             /* packets per multiplex input */
#define PKTS_PER_BUF 344           /* multiple of 8: whole rounds, 8-byte aligned */
#define DAC_GPIO_BASE 0x79024000u
#define GPIO_IN 0xB8
#define GPIO_OUT 0xBC
#define MUX_RATE (64e6 / 7.0)      /* sample rate of one multiplex */
#define TS_RATE 31668449.2

static volatile int stop;
static volatile uint32_t *dacregs;

typedef struct {
    char spec[128];
    uint8_t *ring;
    _Atomic unsigned wr, rd;
    _Atomic unsigned long rx, drop, null_ins, resync;
    int file;                       /* file input: producer waits when the ring is full */
    pthread_t th;
} input;

static input in[MAXMUX];
static int nmux = 1;

/* live settings (HTTP API) and last statistics */
static pthread_mutex_t api_lock = PTHREAD_MUTEX_INITIALIZER;
static long long g_lo, g_lo_req, g_lo_cfg;   /* read back, requested, configured */
static double g_att, g_att_req;
static uint32_t g_mute, g_mute_req;
static int g_change;                 /* bit 0 freq, 1 att, 2 mute */
static int g_set_err;                /* last /set refused by the AD9363 (asleep, out of range) */
static const char *g_conf = "/boot/edgeqam.conf";
static struct timespec g_t_start;    /* uptime_s in /status: lets a client detect a restart */
static struct {
    double sym_rate, ts_rate;
    uint32_t nulls, unf, started, ovf, syncerr;
    long long fs;
} g_st;

static void on_sig(int s) { (void)s; stop = 1; }

static unsigned ring_level(input *p) { return atomic_load(&p->wr) - atomic_load(&p->rd); }

/* append TS data, resynchronising on 0x47 if needed */
static void put_packets(input *p, const uint8_t *b, size_t n)
{
    while (n >= TS) {
        if (b[0] != 0x47) {
            atomic_fetch_add(&p->resync, 1);
            b++, n--;
            continue;
        }
        while (p->file && ring_level(p) >= RING && !stop)
            usleep(1000);
        if (ring_level(p) >= RING) {
            atomic_fetch_add(&p->drop, 1);
        } else {
            unsigned w = atomic_load(&p->wr);
            memcpy(p->ring + (size_t)(w & (RING - 1)) * TS, b, TS);
            atomic_store(&p->wr, w + 1);
            atomic_fetch_add(&p->rx, 1);
        }
        b += TS, n -= TS;
    }
}

static void *udp_thread(void *arg)
{
    input *p = arg;
    char grp[64] = "";
    int port;
    const char *s = p->spec + 4;
    if (strchr(s, ':')) {
        sscanf(s, "%63[^:]:%d", grp, &port);
    } else {
        port = atoi(s);
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0), one = 1, rcv = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind");
        stop = 1;
        return NULL;
    }
    if (grp[0]) {
        struct ip_mreq m = { 0 };
        m.imr_multiaddr.s_addr = inet_addr(grp);
        m.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof m) < 0)
            perror("IP_ADD_MEMBERSHIP");
    }
    struct timeval tv = { 0, 200000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    /* up to 64 datagrams per system call (4 x 31.7 Mbit/s = ~12 000 datagrams/s of 7 packets) */
    enum { NMSG = 64, DGRAM = 2048 };
    static __thread uint8_t bufs[NMSG][DGRAM];
    struct mmsghdr msgs[NMSG];
    struct iovec iov[NMSG];
    while (!stop) {
        for (int k = 0; k < NMSG; k++) {
            iov[k].iov_base = bufs[k];
            iov[k].iov_len = DGRAM;
            memset(&msgs[k].msg_hdr, 0, sizeof msgs[k].msg_hdr);
            msgs[k].msg_hdr.msg_iov = &iov[k];
            msgs[k].msg_hdr.msg_iovlen = 1;
        }
        int got = recvmmsg(fd, msgs, NMSG, MSG_WAITFORONE, NULL);
        for (int k = 0; k < got; k++) {
            uint8_t *b = bufs[k];
            size_t n = msgs[k].msg_len, off = 0;
            if (n % TS == 12 && b[0] == 0x80)     /* RTP header */
                off = 12;
            put_packets(p, b + off, n - off);
        }
    }
    close(fd);
    return NULL;
}

static void *file_thread(void *arg)
{
    input *p = arg;
    const char *path = strcmp(p->spec, "-") ? p->spec + 5 : NULL;
    uint8_t buf[TS * 256];
    while (!stop) {
        FILE *f = path ? fopen(path, "rb") : stdin;
        if (!f) {
            perror(path);
            stop = 1;
            break;
        }
        size_t n;
        while (!stop && (n = fread(buf, 1, sizeof buf, f)) > 0)
            put_packets(p, buf, n - n % TS);
        if (!path)
            break;
        fclose(f);
    }
    return NULL;
}

static uint32_t status(int sel)
{
    uint32_t v = dacregs[GPIO_OUT / 4];
    dacregs[GPIO_OUT / 4] = (v & ~0xCu) | (uint32_t)sel << 2;
    usleep(20);
    return dacregs[GPIO_IN / 4];
}

static int wr_attr_ll(struct iio_channel *ch, const char *a, long long v)
{
    int r = iio_channel_attr_write_longlong(ch, a, v);
    if (r < 0)
        fprintf(stderr, "cannot set %s = %lld: %s\n", a, v, strerror(-r));
    return r;
}

static time_t time_now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec;
}

static double read_num(const char *path, double scale)
{
    FILE *f = fopen(path, "r");
    double v = 0;
    if (f) {
        if (fscanf(f, "%lf", &v) != 1)
            v = 0;
        fclose(f);
    }
    return v * scale;
}

/* rewrite KEY=value lines of the configuration file (others kept) */
static void conf_save(void)
{
    char tmp[256], line[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", g_conf);
    FILE *in_f = fopen(g_conf, "r"), *out = fopen(tmp, "w");
    if (!out) {
        if (in_f)
            fclose(in_f);
        return;
    }
    int done_f = 0, done_a = 0, done_m = 0;
    while (in_f && fgets(line, sizeof line, in_f)) {
        if (!strncmp(line, "FREQ=", 5)) {
            fprintf(out, "FREQ=%lld\n", g_lo_cfg);
            done_f = 1;
        } else if (!strncmp(line, "ATT=", 4)) {
            fprintf(out, "ATT=%.2f\n", g_att);
            done_a = 1;
        } else if (!strncmp(line, "DISABLE=", 8) || !strncmp(line, "MUTE=", 5)) {
            fprintf(out, "DISABLE=%u\n", g_mute);
            done_m = 1;
        } else {
            fputs(line, out);
        }
    }
    if (!done_f)
        fprintf(out, "FREQ=%lld\n", g_lo_cfg);
    if (!done_a)
        fprintf(out, "ATT=%.2f\n", g_att);
    if (!done_m)
        fprintf(out, "DISABLE=%u\n", g_mute);
    if (in_f)
        fclose(in_f);
    fclose(out);
    rename(tmp, g_conf);
    if (system("sync") != 0) { }
}

static int qparam(const char *q, const char *key, char *val, size_t n)
{
    size_t kl = strlen(key);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            size_t i = 0;
            p += kl + 1;
            while (*p && *p != '&' && *p != ' ' && i + 1 < n)
                val[i++] = *p++;
            val[i] = 0;
            return 1;
        }
    }
    return 0;
}

static void *http_thread(void *arg)
{
    int port = (int)(intptr_t)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 4) < 0) {
        perror("http bind");
        return NULL;
    }
    struct timeval tv = { 0, 300000 };
    setsockopt(ls, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    while (!stop) {
        int c = accept(ls, NULL, NULL);
        if (c < 0)
            continue;
        struct timeval ctv = { 0, 500000 };   /* a silent client blocks the (single-threaded) server <= 0.5 s */
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof ctv);
        char req[2048] = "", body[4096];
        ssize_t n = recv(c, req, sizeof req - 1, 0);
        if (n <= 0) {
            close(c);
            continue;
        }
        req[n] = 0;
        char path[512] = "";
        sscanf(req, "%*s %511s", path);
        char *q = strchr(path, '?');
        if (q)
            *q++ = 0;
        int code = 200, len = 0;
        if (!strcmp(path, "/status")) {
            pthread_mutex_lock(&api_lock);
            len = snprintf(body, sizeof body,
                "{\"uptime_s\":%ld,\"multiplexes\":%d,\"freq_mux1_hz\":%lld,\"lo_hz\":%lld,\"attenuation_db\":%.2f,\"sample_rate\":%lld,"
                "\"disable_mask\":%u,\"symbols_per_s\":%.0f,\"ts_mbit_per_mux\":%.3f,"
                "\"fpga\":{\"started\":%u,\"null_packets\":%u,\"dac_underflows\":%u,\"ifft_overflows\":%u,"
                "\"sync_errors\":%u},\"temp_zynq_c\":%.1f,\"temp_ad9363_c\":%.1f,\"inputs\":[",
                (long)(time_now_s() - g_t_start.tv_sec), nmux, g_lo_cfg, g_lo, g_att, g_st.fs, g_mute, g_st.sym_rate, g_st.ts_rate, g_st.started, g_st.nulls,
                g_st.unf, g_st.ovf, g_st.syncerr,
                (read_num("/sys/bus/iio/devices/iio:device1/in_temp0_raw", 1) * 503.975 / 4096 - 273.15),
                read_num("/sys/bus/iio/devices/iio:device0/in_temp0_input", 0.001));
            for (int i = 0; i < nmux && len < (int)sizeof body - 300; i++)
                len += snprintf(body + len, sizeof body - len,
                    "%s{\"mux\":%d,\"centre_hz\":%lld,\"input\":\"%s\",\"rx_packets\":%lu,"
                    "\"null_inserted\":%lu,\"dropped\":%lu,\"resync\":%lu,\"queued\":%u,\"disabled\":%u}",
                    i ? "," : "", i + 1, g_lo + (long long)((2 * i - (nmux - 1)) * 4000000LL), in[i].spec, in[i].rx,
                    in[i].null_ins, in[i].drop, in[i].resync, ring_level(&in[i]), (g_mute >> i) & 1);
            pthread_mutex_unlock(&api_lock);
            len += snprintf(body + len, sizeof body - len, "]}\n");
        } else if (!strcmp(path, "/set")) {
            char v[64];
            int any = 0, save = 0;
            pthread_mutex_lock(&api_lock);
            if (q && qparam(q, "freq", v, sizeof v)) {
                long long f = (long long)atof(v);
                if (f >= 70000000LL && f <= 6000000000LL) {
                    g_lo_req = f + (long long)(nmux - 1) * 4000000LL;   /* f = multiplex 1 */
                    g_change |= 1;
                    any = 1;
                }
            }
            if (q && qparam(q, "att", v, sizeof v)) {
                double at = atof(v);
                if (at >= 0 && at <= 89.75) {
                    g_att_req = at;
                    g_change |= 2;
                    any = 1;
                }
            }
            if (q && (qparam(q, "disable", v, sizeof v) || qparam(q, "mute", v, sizeof v))) {
                g_mute_req = (uint32_t)strtoul(v, NULL, 0) & ((1u << nmux) - 1);
                g_change |= 4;
                any = 1;
            }
            if (q && qparam(q, "save", v, sizeof v) && atoi(v))
                save = 1;
            pthread_mutex_unlock(&api_lock);
            for (int k = 0; k < 30 && g_change; k++)     /* applied by the main loop */
                usleep(20000);
            if (save)
                conf_save();
            if (g_set_err)
                any = 0;            /* the AD9363 refused (e.g. asleep): report an error */
            len = snprintf(body, sizeof body, "{\"ok\":%s,\"freq_mux1_hz\":%lld,\"lo_hz\":%lld,\"attenuation_db\":%.2f,"
                           "\"disable_mask\":%u,\"saved\":%s}\n", any ? "true" : "false", g_lo_cfg, g_lo, g_att, g_mute,
                           save ? "true" : "false");
            if (!any)
                code = 400;
        } else {
            code = 404;
            len = snprintf(body, sizeof body, "{\"error\":\"GET /status or /set?freq=HZ&att=DB&disable=MASK&save=1\"}\n");
        }
        char hdr[256];
        int hl = snprintf(hdr, sizeof hdr, "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\n"
                          "Content-Length: %d\r\nConnection: close\r\n\r\n",
                          code, code == 200 ? "OK" : code == 400 ? "Bad Request" : "Not Found", len);
        if (send(c, hdr, hl, MSG_NOSIGNAL) > 0)
            send(c, body, len, MSG_NOSIGNAL);
        close(c);
    }
    close(ls);
    return NULL;
}

int main(int argc, char **argv)
{
    long long lo = 0;
    double att = 30;
    int verbose = 0, opt, http_port = 8080;
    long mute_opt = -1;
    while ((opt = getopt(argc, argv, "n:f:a:p:c:d:m:v")) != -1) {
        switch (opt) {
        case 'd':
        case 'm': mute_opt = strtol(optarg, NULL, 0); break;
        case 'p': http_port = atoi(optarg); break;
        case 'c': g_conf = optarg; break;
        case 'n': nmux = atoi(optarg); break;
        case 'f': lo = (long long)atof(optarg); break;    /* multiplex 1, LO computed below */
        case 'a': att = atof(optarg); break;
        case 'v': verbose = 1; break;
        default: goto usage;
        }
    }
    if ((nmux != 1 && nmux != 2 && nmux != 4) || lo <= 0 || optind >= argc) {
usage:
        fprintf(stderr, "usage: dvbt_tx -n 1|2|4 -f FREQ_MUX1_HZ [-a ATT_DB] [-p HTTP_PORT] [-c CONF] [-d DISABLE_MASK] [-v] IN0 [IN1 ...]\n"
                        "  IN = udp:PORT | udp:GROUP:PORT | file:PATH | -\n");
        return 1;
    }
    lo += (long long)(nmux - 1) * 4000000LL;          /* LO in the middle of the N channels */
    int nin = argc - optind;
    if (nin > nmux)
        nin = nmux;
    for (int i = 0; i < nmux; i++) {
        in[i].ring = malloc((size_t)RING * TS);
        if (i < nin) {
            snprintf(in[i].spec, sizeof in[i].spec, "%s", argv[optind + i]);
            int isudp = !strncmp(in[i].spec, "udp:", 4);
            in[i].file = !isudp;
            if (!isudp && strcmp(in[i].spec, "-") && strncmp(in[i].spec, "file:", 5)) {
                fprintf(stderr, "bad input %s\n", in[i].spec);
                return 1;
            }
            pthread_create(&in[i].th, NULL, isudp ? udp_thread : file_thread, &in[i]);
        } else {
            strcpy(in[i].spec, "(no input: disabled)");
        }
    }
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGPIPE, SIG_IGN);

    /* wake the radio up and keep the eco daemon from putting it to sleep */
    if (system("eco pause >/dev/null 2>&1") != 0)
        fprintf(stderr, "(eco pause failed, ignored)\n");

    int mfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mfd < 0) {
        perror("/dev/mem");
        return 1;
    }
    dacregs = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, DAC_GPIO_BASE);
    if (dacregs == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    struct iio_context *ctx = iio_create_local_context();
    struct iio_device *phy = ctx ? iio_context_find_device(ctx, "ad9361-phy") : NULL;
    struct iio_device *dds = ctx ? iio_context_find_device(ctx, "cf-ad9361-dds-core-lpc") : NULL;
    if (!phy || !dds) {
        fprintf(stderr, "IIO devices not found\n");
        return 1;
    }
    long long fs = (long long)(MUX_RATE * nmux + 0.5);
    struct iio_channel *txv = iio_device_find_channel(phy, "voltage0", true);
    struct iio_channel *txlo = iio_device_find_channel(phy, "altvoltage1", true);
    iio_channel_attr_write_double(txv, "hardwaregain", -att);
    wr_attr_ll(txv, "sampling_frequency", fs);
    wr_attr_ll(txv, "rf_bandwidth", (long long)(8.5e6 * nmux));
    wr_attr_ll(txlo, "frequency", lo);
    long long fs_rd = 0, lo_rd = 0;
    iio_channel_attr_read_longlong(txv, "sampling_frequency", &fs_rd);
    iio_channel_attr_read_longlong(txlo, "frequency", &lo_rd);
    fprintf(stderr, "dvbt_tx: %d multiplex(es), LO %.6f MHz, %lld S/s (asked %lld), attenuation %.2f dB\n",
            nmux, lo_rd / 1e6, fs_rd, fs, att);
    for (int i = 0; i < nmux; i++)
        fprintf(stderr, "  mux %d at %.3f MHz <- %s\n", i + 1, (lo_rd + (2 * i - (nmux - 1)) * 4e6) / 1e6,
                in[i].spec);

    /* modulator on (bit 1), TX FIR of Tezuka off (bit 0), multiplexes without input muted
     * (bits 11:8, v2 bitstreams) */
    uint32_t mute = 0;
    for (int i = nin; i < nmux; i++)
        mute |= 1u << i;
    if (mute_opt >= 0)
        mute |= (uint32_t)mute_opt & ((1u << nmux) - 1);
    uint32_t g = dacregs[GPIO_OUT / 4];
    dacregs[GPIO_OUT / 4] = (g & ~0xF0Fu) | 0x2u | mute << 8;
    g_lo = lo_rd;
    g_lo_cfg = lo - (long long)(nmux - 1) * 4000000LL;
    g_att = att;
    g_mute = mute;
    g_st.fs = fs_rd;
    clock_gettime(CLOCK_MONOTONIC, &g_t_start);
    pthread_t hth;
    if (http_port > 0)
        pthread_create(&hth, NULL, http_thread, (void *)(intptr_t)http_port);

    struct iio_channel *c0 = iio_device_find_channel(dds, "voltage0", true);
    struct iio_channel *c1 = iio_device_find_channel(dds, "voltage1", true);
    iio_channel_enable(c0);
    iio_channel_enable(c1);
    iio_device_set_kernel_buffers_count(dds, 4);
    size_t nsamp = (size_t)PKTS_PER_BUF * TS / 4;
    struct iio_buffer *buf = iio_device_create_buffer(dds, nsamp, false);
    if (!buf) {
        fprintf(stderr, "cannot create TX buffer: %s\n", strerror(errno));
        stop = 1;
    }
    static const uint8_t nullpkt_hdr[4] = { 0x47, 0x1F, 0xFF, 0x10 };
    struct timespec t0, t;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    unsigned long bufs = 0;
    uint32_t sym0 = status(0);
    while (!stop) {
        uint8_t *d = iio_buffer_start(buf);
        for (int k = 0; k < PKTS_PER_BUF; k++, d += TS) {
            input *p = &in[k % nmux];
            if (ring_level(p) > 0) {
                unsigned r = atomic_load(&p->rd);
                memcpy(d, p->ring + (size_t)(r & (RING - 1)) * TS, TS);
                atomic_store(&p->rd, r + 1);
            } else {
                memcpy(d, nullpkt_hdr, 4);
                memset(d + 4, 0xFF, TS - 4);
                atomic_fetch_add(&p->null_ins, 1);
            }
        }
        ssize_t r = iio_buffer_push(buf);
        if (r < 0) {
            fprintf(stderr, "push: %s\n", strerror((int)-r));
            break;
        }
        bufs++;
        clock_gettime(CLOCK_MONOTONIC, &t);
        double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) * 1e-9;
        /* another program may rewrite the DAC GPIO register or the sample rate: restore them */
        uint32_t want = 0x2u | mute << 8, cur = dacregs[GPIO_OUT / 4];
        if ((cur & 0xF03u) != want) {
            fprintf(stderr, "warning: DAC GPIO overwritten (0x%08x), modulator re-enabled\n", cur);
            dacregs[GPIO_OUT / 4] = (cur & ~0xF0Fu) | want;
        }
        if (bufs % 64 == 0) {
            /* the eco sleep daemon (or anyone) may have put the AD9363 to sleep or cut the TX LO:
             * the edgeQAM stream is not seen as TX activity by it */
            char ensm[32] = "";
            if (iio_device_attr_read(phy, "ensm_mode", ensm, sizeof ensm) > 0 && strncmp(ensm, "fdd", 3)) {
                fprintf(stderr, "warning: AD9363 ENSM was '%s', set back to fdd\n", ensm);
                iio_device_attr_write(phy, "ensm_mode", "fdd");
                if (system("eco pause >/dev/null 2>&1") != 0) { }
            }
            long long pd = 0;
            if (iio_channel_attr_read_longlong(txlo, "powerdown", &pd) == 0 && pd) {
                fprintf(stderr, "warning: TX LO was powered down, powered up again\n");
                wr_attr_ll(txlo, "powerdown", 0);
                if (system("eco pause >/dev/null 2>&1") != 0) { }
            }
            long long f = 0;
            iio_channel_attr_read_longlong(txv, "sampling_frequency", &f);
            if (f != fs_rd) {
                fprintf(stderr, "warning: sample rate changed by someone else (%lld), restored\n", f);
                wr_attr_ll(txv, "sampling_frequency", fs);
                wr_attr_ll(txlo, "frequency", lo);
                iio_channel_attr_write_double(txv, "hardwaregain", -att);
            }
        }
        if (g_change) {                     /* HTTP API requests */
            pthread_mutex_lock(&api_lock);
            g_set_err = 0;
            if (g_change & 1) {
                if (wr_attr_ll(txlo, "frequency", g_lo_req) < 0)
                    g_set_err = 1;
                iio_channel_attr_read_longlong(txlo, "frequency", &g_lo);
                lo = g_lo_req;
                g_lo_cfg = lo - (long long)(nmux - 1) * 4000000LL;
                fprintf(stderr, "api: LO %.6f MHz\n", g_lo / 1e6);
            }
            if (g_change & 2) {
                att = g_att_req;
                if (iio_channel_attr_write_double(txv, "hardwaregain", -att) < 0)
                    g_set_err = 1;
                g_att = att;
                fprintf(stderr, "api: attenuation %.2f dB\n", att);
            }
            if (g_change & 4) {
                mute = g_mute_req;
                g_mute = mute;
                uint32_t gg = dacregs[GPIO_OUT / 4];
                dacregs[GPIO_OUT / 4] = (gg & ~0xF0Fu) | 0x2u | mute << 8;
                fprintf(stderr, "api: disabled multiplexes mask 0x%x\n", mute);
            }
            g_change = 0;
            pthread_mutex_unlock(&api_lock);
        }
        static double since_print;
        if (el >= 1.0) {
            uint32_t sym = status(0), nul = status(1), unf = status(2), so = status(3);
            pthread_mutex_lock(&api_lock);
            g_st.sym_rate = (sym - sym0) / el;
            g_st.ts_rate = bufs * (double)PKTS_PER_BUF / nmux * TS * 8 / el / 1e6;
            g_st.nulls = nul;
            g_st.unf = unf & 0xFFFF;
            g_st.started = unf >> 31;
            g_st.ovf = so & 0xFFFF;
            g_st.syncerr = so >> 16;
            pthread_mutex_unlock(&api_lock);
            since_print += el;
        }
        if (el >= 1.0 && (verbose || since_print >= 60.0)) {
            since_print = 0;
            uint32_t sym = status(0), nul = g_st.nulls, unf = g_st.unf | g_st.started << 31,
                     so = g_st.ovf | g_st.syncerr << 16;
            sym = sym0 + (uint32_t)(g_st.sym_rate * el);
            fprintf(stderr, "symbols %u/s (expect %.0f) | FPGA: nulls %u, DAC underflows %u%s, "
                            "IFFT ovf %u, sync err %u | TS out %.2f Mbit/s/mux",
                    (unsigned)((sym - sym0) / el), MUX_RATE / 2112, nul, unf & 0xFFFF,
                    (unf >> 31) ? "" : " (not started)", so & 0xFFFF, so >> 16,
                    bufs * (double)PKTS_PER_BUF / nmux * TS * 8 / el / 1e6);
            for (int i = 0; i < nmux; i++)
                fprintf(stderr, " | mux%d rx %lu null %lu drop %lu resync %lu lvl %u", i, in[i].rx,
                        in[i].null_ins, in[i].drop, in[i].resync, ring_level(&in[i]));
            fprintf(stderr, "\n");
        }
        if (el >= 1.0) {
            sym0 = status(0);
            t0 = t;
            bufs = 0;
        }
    }
    stop = 1;
    if (buf)
        iio_buffer_destroy(buf);
    iio_channel_attr_write_double(txv, "hardwaregain", -89.75);
    g = dacregs[GPIO_OUT / 4];
    dacregs[GPIO_OUT / 4] = g & ~0xF0Eu;
    iio_context_destroy(ctx);
    if (system("eco reprise >/dev/null 2>&1") != 0)
        fprintf(stderr, "(eco reprise failed, ignored)\n");
    fprintf(stderr, "dvbt_tx: stopped, modulator off, TX attenuation 89.75 dB\n");
    return 0;
}
