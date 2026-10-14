/*
 * daemon.c - coolood: native Noise_TCP daemon (handoff §5, D16/D17/D24).
 *
 * Single-threaded poll() loop over a fixed connection table; storage and
 * framing are shared with the v0 gate (store.o) and the CLI (noise_xx.c).
 * Foreground process (systemd Type=simple), logs to stdout -> journald.
 *
 * Connection lifecycle:
 *   accept -> HANDSHAKE (fixed-size XX messages, 10s deadline)
 *          -> ESTABLISHED (framed encrypted stream, line protocol via proto.c)
 *   close on: proto request, idle 10min, handshake failure/timeout, IO error.
 */
#define _DEFAULT_SOURCE         /* flock etc. with -std=c99 on glibc */

#include "proto.h"
#include "noise_xx.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define LISTEN_BACKLOG   128
#define MAX_CONNS        1024
#define HS_TIMEOUT_SEC   10
#define IDLE_TIMEOUT_SEC 600
#define TICK_MS          100

struct conn {
    struct pconn p;                     /* proto-owned state */
    noise_hs hs;                        /* handshake state (pre-establish) */
    uint8_t  rbuf[NOISE_FRAME_OVERHEAD + NOISE_FRAME_MAX_PT];
    size_t   rlen;                      /* encrypted bytes buffered */
    size_t   hs_expect;                 /* bytes expected for current hs msg */
    time_t   hs_start;
    int      used;
};

static struct conn g_conns[MAX_CONNS];
static volatile sig_atomic_t g_stop;

static uint8_t g_server_sk[NOISE_KEY_LEN];
static uint8_t g_server_pk[NOISE_KEY_LEN];
static char  g_server_fp[65];

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---------------------------------------------------------------- */
/* server key + fingerprint (D26)                                   */
/* ---------------------------------------------------------------- */

static int read_random(uint8_t *out, size_t len)
{
#ifdef __APPLE__
    arc4random_buf(out, len);
    return 0;
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f)
        return -1;
    if (fread(out, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
#endif
}

/* Load <home>/server_key, generating it (0600) on first start.
 * Always (re)writes <home>/server_fingerprint for the SSH gate (D26). */
static int server_key_init(void)
{
    char path[1024];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "%s/server_key", cooloo_home());
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        if (errno != ENOENT)
            return -1;
        if (read_random(g_server_sk, sizeof g_server_sk) != 0)
            return -1;
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0)
            return -1;
        n = write(fd, g_server_sk, sizeof g_server_sk);
        close(fd);
        if (n != (ssize_t)sizeof g_server_sk)
            return -1;
        fprintf(stdout, "coolood: generated %s\n", path);
    } else {
        n = read(fd, g_server_sk, sizeof g_server_sk);
        close(fd);
        if (n != (ssize_t)sizeof g_server_sk)
            return -1;
    }

    noise_public_key(g_server_pk, g_server_sk);
    noise_fingerprint_hex(g_server_fp, g_server_pk);

    snprintf(path, sizeof(path), "%s/server_fingerprint", cooloo_home());
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    {
        char line[80];
        int len = snprintf(line, sizeof(line), "%s\n", g_server_fp);
        n = write(fd, line, (size_t)len);
        close(fd);
        if (n != len)
            return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- */
/* connection management                                            */
/* ---------------------------------------------------------------- */

static void conn_close(struct conn *c)
{
    if (c->used) {
        close(c->p.fd);
        memset(c, 0, sizeof *c);    /* also wipes key material */
    }
}

static void conn_abort(struct conn *c, const char *why)
{
    fprintf(stdout, "coolood: fd=%d closed: %s\n", c->p.fd, why);
    fflush(stdout);
    conn_close(c);
}

static int set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static void conn_accept(int listen_fd)
{
    for (;;) {
        struct sockaddr_in sa;
        socklen_t sl = sizeof sa;
        int fd = accept(listen_fd, (struct sockaddr *)&sa, &sl);
        int i;

        if (fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                perror("coolood: accept");
            return;
        }
        for (i = 0; i < MAX_CONNS; i++)
            if (!g_conns[i].used)
                break;
        if (i == MAX_CONNS || set_nonblock(fd) != 0) {
            close(fd);
            continue;
        }
        memset(&g_conns[i], 0, sizeof g_conns[i]);
        g_conns[i].used = 1;
        g_conns[i].p.fd = fd;
        g_conns[i].p.last_active = time(NULL);
        g_conns[i].hs_start = time(NULL);
        g_conns[i].hs_expect = NOISE_MSG1_LEN;
        noise_hs_init_responder(&g_conns[i].hs, g_server_sk,
                                (const uint8_t *)"cooloo-v2", 9);
    }
}

/* Handshake progress; raw bytes accumulate in c->rbuf (rlen). */
static void hs_step(struct conn *c)
{
    if (c->hs_expect == NOISE_MSG1_LEN && c->rlen >= NOISE_MSG1_LEN) {
        uint8_t msg2[NOISE_MSG2_LEN];
        size_t n2;
        uint8_t dummy[1];
        size_t dummy_len;

        if (noise_hs_read_msg1(&c->hs, c->rbuf, NOISE_MSG1_LEN,
                               dummy, sizeof dummy, &dummy_len) != 0) {
            conn_abort(c, "bad handshake msg1");
            return;
        }
        n2 = noise_hs_write_msg2(&c->hs, msg2, sizeof msg2, NULL, 0);
        if (!n2) {
            conn_abort(c, "handshake msg2 failed");
            return;
        }
        if (n2 > CONN_OUT_CAP - c->p.out_len) {
            conn_abort(c, "output overflow");
            return;
        }
        memcpy(c->p.out + c->p.out_len, msg2, n2);  /* raw, pre-encryption */
        c->p.out_len += n2;
        memmove(c->rbuf, c->rbuf + NOISE_MSG1_LEN, c->rlen - NOISE_MSG1_LEN);
        c->rlen -= NOISE_MSG1_LEN;
        c->hs_expect = NOISE_MSG3_LEN;
    }
    if (c->hs_expect == NOISE_MSG3_LEN && c->rlen >= NOISE_MSG3_LEN) {
        uint8_t dummy[1];
        size_t dummy_len;
        uint8_t client_pk[NOISE_KEY_LEN];

        if (noise_hs_read_msg3(&c->hs, c->rbuf, NOISE_MSG3_LEN,
                               dummy, sizeof dummy, &dummy_len) != 0) {
            conn_abort(c, "bad handshake msg3");
            return;
        }
        memcpy(client_pk, c->hs.rs, NOISE_KEY_LEN);  /* before split/wipe */
        memmove(c->rbuf, c->rbuf + NOISE_MSG3_LEN, c->rlen - NOISE_MSG3_LEN);
        c->rlen -= NOISE_MSG3_LEN;
        noise_hs_split(&c->hs, &c->p.sess);
        memset(&c->hs, 0, sizeof c->hs);
        c->p.established = 1;
        /* identity = client static key fingerprint (D20) */
        {
            static const char hex[] = "0123456789abcdef";
            size_t k;
            for (k = 0; k < 32; k++) {
                c->p.fp[k * 2] = hex[client_pk[k] >> 4];
                c->p.fp[k * 2 + 1] = hex[client_pk[k] & 15];
            }
            c->p.fp[64] = 0;
        }
        proto_lookup_nick(c->p.fp, c->p.nick);
        proto_on_established(&c->p);
    }
}

/* Decrypt complete transport frames from rbuf and feed proto. */
static void stream_step(struct conn *c)
{
    for (;;) {
        size_t flen, pt_len;
        long n;
        uint8_t pt[NOISE_FRAME_MAX_PT];

        if (c->rlen < NOISE_FRAME_HDR_LEN)
            return;
        pt_len = (size_t)c->rbuf[0] | ((size_t)c->rbuf[1] << 8);
        if (pt_len > NOISE_FRAME_MAX_PT) {
            conn_abort(c, "bad frame length");
            return;
        }
        flen = pt_len + NOISE_FRAME_OVERHEAD;
        if (c->rlen < flen)
            return;
        n = noise_session_recv(&c->p.sess, c->rbuf, flen, pt, sizeof pt);
        if (n < 0) {
            conn_abort(c, "frame decrypt failed");
            return;
        }
        memmove(c->rbuf, c->rbuf + flen, c->rlen - flen);
        c->rlen -= flen;
        c->p.last_active = time(NULL);
        proto_on_plain(&c->p, pt, (size_t)n);
        if (c->p.close_pending) {
            conn_abort(c, "protocol error");
            return;
        }
    }
}

static void conn_read(struct conn *c)
{
    ssize_t n = read(c->p.fd, c->rbuf + c->rlen, sizeof c->rbuf - c->rlen);
    if (n == 0) {
        conn_close(c);
        return;
    }
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            conn_abort(c, "read error");
        return;
    }
    c->rlen += (size_t)n;
    if (!c->p.established) {
        hs_step(c);
        if (c->used && c->rlen == sizeof c->rbuf)
            conn_abort(c, "handshake overflow");
        return;
    }
    stream_step(c);
    if (c->used && c->rlen == sizeof c->rbuf)
        conn_abort(c, "frame overflow");
}

static void conn_write(struct conn *c)
{
    ssize_t n = write(c->p.fd, c->p.out, c->p.out_len);
    if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            conn_abort(c, "write error");
        return;
    }
    memmove(c->p.out, c->p.out + n, c->p.out_len - (size_t)n);
    c->p.out_len -= (size_t)n;
    if (c->p.established)
        proto_on_writable(&c->p);
}

/* ---------------------------------------------------------------- */
/* main loop                                                        */
/* ---------------------------------------------------------------- */

static int listen_socket(const char *bind_addr, int port)
{
    int fd, one = 1;
    struct sockaddr_in sa;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
        return -1;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_addr, &sa.sin_addr) != 1)
        return -1;
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0)
        return -1;
    if (listen(fd, LISTEN_BACKLOG) < 0)
        return -1;
    if (set_nonblock(fd) != 0)
        return -1;
    return fd;
}

int main(int argc, char **argv)
{
    const char *bind_addr = "0.0.0.0";
    int port = 4222;
    int listen_fd, i;
    long long last_tick;
    struct pollfd pfds[MAX_CONNS + 1];
    struct conn *pc[MAX_CONNS + 1];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) {
            bind_addr = argv[++i];
        } else if (!strcmp(argv[i], "-p") && i + 1 < argc) {
            port = atoi(argv[++i]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "coolood: bad port\n");
                return 2;
            }
        } else {
            fprintf(stderr, "usage: coolood [-b addr] [-p port]\n");
            return 2;
        }
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);      /* portable: macOS lacks MSG_NOSIGNAL */

    proto_users_init();
    if (server_key_init() != 0) {
        fprintf(stderr, "coolood: server key init failed\n");
        return 1;
    }

    listen_fd = listen_socket(bind_addr, port);
    if (listen_fd < 0) {
        perror("coolood: listen");
        return 1;
    }
    fprintf(stdout, "coolood: listening on %s:%d fingerprint %s\n",
            bind_addr, port, g_server_fp);
    fflush(stdout);

    last_tick = now_ms();
    while (!g_stop) {
        int nfds = 0, nready, j;

        pfds[nfds].fd = listen_fd;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        pc[nfds] = NULL;
        nfds++;
        for (i = 0; i < MAX_CONNS; i++) {
            if (!g_conns[i].used)
                continue;
            pfds[nfds].fd = g_conns[i].p.fd;
            pfds[nfds].events = POLLIN;
            if (g_conns[i].p.out_len > 0)
                pfds[nfds].events |= POLLOUT;
            pfds[nfds].revents = 0;
            pc[nfds] = &g_conns[i];
            nfds++;
        }

        nready = poll(pfds, (nfds_t)nfds, TICK_MS);
        if (nready < 0) {
            if (errno == EINTR)
                continue;
            perror("coolood: poll");
            break;
        }

        if (nready > 0) {
            if (pfds[0].revents & POLLIN)
                conn_accept(listen_fd);
            for (j = 1; j < nfds; j++) {
                struct conn *c = pc[j];
                if (!c->used)
                    continue;
                if (pfds[j].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    conn_close(c);
                    continue;
                }
                if (pfds[j].revents & POLLOUT)
                    conn_write(c);
                if (c->used && (pfds[j].revents & POLLIN))
                    conn_read(c);
            }
        }

        /* 100ms follower tick + coarse timeout scan */
        if (now_ms() - last_tick >= TICK_MS) {
            time_t now = time(NULL);
            last_tick = now_ms();
            for (i = 0; i < MAX_CONNS; i++) {
                struct conn *c = &g_conns[i];
                if (!c->used)
                    continue;
                if (!c->p.established) {
                    if (now - c->hs_start > HS_TIMEOUT_SEC) {
                        conn_abort(c, "handshake timeout");
                        continue;
                    }
                } else {
                    if (now - c->p.last_active > IDLE_TIMEOUT_SEC) {
                        conn_abort(c, "idle timeout");
                        continue;
                    }
                    proto_tick(&c->p);
                    if (c->p.close_pending)
                        conn_abort(c, "protocol error");
                }
            }
        }
    }

    fprintf(stdout, "coolood: shutting down\n");
    close(listen_fd);
    for (i = 0; i < MAX_CONNS; i++)
        conn_close(&g_conns[i]);
    return 0;
}
