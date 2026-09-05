/*
 * net_client.c - loopback smoke-test driver for coolood (NOT the v1 CLI;
 * the real client ships from the client repo in T3). Deterministic static
 * key from argv so tests can script identities; no TOFU (test-only).
 *
 * usage: net_client <port> <keyhex64> <cmd> [args]
 *   PING | WHOAMI | HELLO <nick> | SEND <room> <text> |
 *   READ <room> <off> | ROOMS | FOLLOW <room> <seconds> |
 *   RAWLINE <line> | SENDBIG <room> <len>
 * Prints the greeting line and all response lines to stdout.
 */
#include "noise_xx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>

static int g_fd = -1;
static noise_session g_sess;
static uint8_t g_plain[NOISE_FRAME_MAX_PT + 1];
static size_t g_plain_len;

static void die(const char *what)
{
    fprintf(stderr, "net_client: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void read_full(uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(g_fd, buf + got, len - got);
        if (n == 0) {
            fprintf(stderr, "net_client: server closed\n");
            exit(1);
        }
        if (n < 0)
            die("read");
        got += (size_t)n;
    }
}

static void write_full(const uint8_t *buf, size_t len)
{
    size_t put = 0;
    while (put < len) {
        ssize_t n = write(g_fd, buf + put, len - put);
        if (n < 0)
            die("write");
        put += (size_t)n;
    }
}

/* send one frame built from a plaintext chunk */
static void send_pt(const uint8_t *pt, size_t len)
{
    uint8_t frame[NOISE_FRAME_MAX_PT + NOISE_FRAME_OVERHEAD];
    size_t n;
    if (len > NOISE_FRAME_MAX_PT) {
        fprintf(stderr, "net_client: chunk too large\n");
        exit(1);
    }
    n = noise_session_send(&g_sess, pt, len, frame, sizeof frame);
    if (!n) {
        fprintf(stderr, "net_client: session_send failed\n");
        exit(1);
    }
    write_full(frame, n);
}

static void send_line(const char *line)
{
    send_pt((const uint8_t *)line, strlen(line));
}

/* receive one frame, append plaintext to the line buffer */
static void recv_pt(void)
{
    uint8_t hdr[2];
    size_t len;
    long n;

    read_full(hdr, 2);
    len = (size_t)hdr[0] | ((size_t)hdr[1] << 8);
    {
        uint8_t frame[NOISE_FRAME_MAX_PT + NOISE_FRAME_OVERHEAD];
        memcpy(frame, hdr, 2);
        read_full(frame + 2, len + NOISE_TAG_LEN);
        n = noise_session_recv(&g_sess, frame, len + NOISE_FRAME_OVERHEAD,
                               g_plain + g_plain_len,
                               sizeof g_plain - 1 - g_plain_len);
    }
    if (n < 0) {
        fprintf(stderr, "net_client: bad frame\n");
        exit(1);
    }
    g_plain_len += (size_t)n;
    g_plain[g_plain_len] = '\0';
}

/* read one response line (blocking); returns line without '\n' */
static void next_line(char *out, size_t cap)
{
    for (;;) {
        uint8_t *nl = memchr(g_plain, '\n', g_plain_len);
        size_t ll;
        if (nl) {
            ll = (size_t)(nl - g_plain);
            if (ll >= cap)
                ll = cap - 1;
            memcpy(out, g_plain, ll);
            out[ll] = '\0';
            ll = (size_t)(nl - g_plain) + 1;
            memmove(g_plain, nl + 1, g_plain_len - ll);
            g_plain_len -= ll;
            return;
        }
        recv_pt();
    }
}

static void print_until(const char *terminator)
{
    char line[8192];
    for (;;) {
        next_line(line, sizeof line);
        printf("%s\n", line);
        if (!strcmp(line, terminator))
            return;
    }
}

static void print_one(void)
{
    char line[8192];
    next_line(line, sizeof line);
    printf("%s\n", line);
}

static int hexval(int ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

int main(int argc, char **argv)
{
    struct sockaddr_in sa;
    uint8_t sk[NOISE_KEY_LEN];
    noise_hs hs;
    uint8_t msg[NOISE_MSG2_LEN];
    size_t n;
    int port, i;
    const char *cmd;
    struct timeval tv;

    if (argc < 4) {
        fprintf(stderr, "usage: %s <port> <keyhex64> <cmd> [args]\n", argv[0]);
        return 2;
    }
    port = atoi(argv[1]);
    if (strlen(argv[2]) != 64)
        return 2;
    for (i = 0; i < 32; i++) {
        int hi = hexval(argv[2][i * 2]), lo = hexval(argv[2][i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return 2;
        sk[i] = (uint8_t)(hi << 4 | lo);
    }
    cmd = argv[3];

    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0)
        die("socket");
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(g_fd, (struct sockaddr *)&sa, sizeof sa) != 0)
        die("connect");
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(g_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    /* Noise_XX initiator, empty payloads, no TOFU (test driver) */
    noise_hs_init_initiator(&hs, sk, (const uint8_t *)"cooloo-v1", 9);
    n = noise_hs_write_msg1(&hs, msg, sizeof msg, NULL, 0);
    if (!n)
        die("msg1");
    write_full(msg, n);
    read_full(msg, NOISE_MSG2_LEN);
    {
        uint8_t dummy[1];
        size_t dummy_len;
        if (noise_hs_read_msg2(&hs, msg, NOISE_MSG2_LEN,
                               dummy, sizeof dummy, &dummy_len) != 0)
            die("read msg2");
        n = noise_hs_write_msg3(&hs, msg, sizeof msg, NULL, 0);
        if (!n)
            die("msg3");
    }
    write_full(msg, n);
    noise_hs_split(&hs, &g_sess);
    memset(&hs, 0, sizeof hs);

    print_one();                /* greeting: COOLOO 1 <nick|-> */

    if (!strcmp(cmd, "PING")) {
        send_line("PING\n");
        print_one();
    } else if (!strcmp(cmd, "WHOAMI")) {
        send_line("WHOAMI\n");
        print_one();
    } else if (!strcmp(cmd, "HELLO") && argc == 5) {
        char line[128];
        snprintf(line, sizeof line, "HELLO %s\n", argv[4]);
        send_line(line);
        print_one();
    } else if (!strcmp(cmd, "SEND") && argc == 6) {
        char hdr[128];
        size_t hl = (size_t)snprintf(hdr, sizeof hdr, "SEND %s %zu\n",
                                     argv[4], strlen(argv[5]));
        send_pt((const uint8_t *)hdr, hl);
        send_pt((const uint8_t *)argv[5], strlen(argv[5]));
        print_one();
    } else if (!strcmp(cmd, "READ") && argc == 6) {
        char line[128];
        snprintf(line, sizeof line, "READ %s %s\n", argv[4], argv[5]);
        send_line(line);
        print_until("END");
    } else if (!strcmp(cmd, "ROOMS")) {
        send_line("ROOMS\n");
        print_until("END");
    } else if (!strcmp(cmd, "FOLLOW") && argc == 6) {
        char line[128];
        time_t deadline;
        /* FOLLOW <room> <seconds>: live mode (from tail), print until timeout */
        snprintf(line, sizeof line, "FOLLOW %s\n", argv[4]);
        send_line(line);
        deadline = time(NULL) + atoi(argv[5]);
        while (time(NULL) < deadline) {
            char l[8192];
            fd_set rf;
            struct timeval ptv;
            FD_ZERO(&rf);
            FD_SET(g_fd, &rf);
            ptv.tv_sec = 1;
            ptv.tv_usec = 0;
            if (select(g_fd + 1, &rf, NULL, NULL, &ptv) <= 0)
                continue;
            recv_pt();
            while (memchr(g_plain, '\n', g_plain_len)) {
                next_line(l, sizeof l);
                printf("%s\n", l);
                fflush(stdout);
            }
        }
    } else if (!strcmp(cmd, "RAWLINE") && argc == 5) {
        char line[8192];
        snprintf(line, sizeof line, "%s\n", argv[4]);
        send_line(line);
        print_one();
    } else if (!strcmp(cmd, "SENDBIG") && argc == 6) {
        char hdr[128];
        long len = atol(argv[5]), sent = 0;
        static const uint8_t zeros[4096];
        snprintf(hdr, sizeof hdr, "SEND %s %ld\n", argv[4], len);
        send_line(hdr);
        while (sent < len) {
            long chunk = len - sent > (long)sizeof zeros
                         ? (long)sizeof zeros : len - sent;
            send_pt(zeros, (size_t)chunk);
            sent += chunk;
        }
        print_one();
    } else {
        fprintf(stderr, "net_client: bad command\n");
        return 2;
    }
    close(g_fd);
    return 0;
}
