/*
 * noise_kat.c - known-answer harness for noise_xx.c against cacophony vectors.
 *
 * Driven by tests/noise_vectors.py, which writes a line-based spec file
 * (JSON parsing in C is not worth it):
 *   init_static <hex64>      init_ephemeral <hex64>
 *   resp_static <hex64>      resp_ephemeral <hex64>
 *   prologue <hex|->
 *   msg <i|r> <payload_hex|-> <ciphertext_hex|->   (× N)
 *
 * The harness runs both parties in-process, asserting every emitted byte
 * equals the vector ciphertext (this is what validates against the official
 * vectors, not just self-consistency), and every decryption round-trips.
 * First 3 messages are the XX handshake (payloads must be empty); the rest
 * are transport frames (compared excluding our 2-byte LE frame header).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "noise_xx.h"

#define MAX_MSGS   64
#define MAX_HEX    65536

struct msg {
    int from_initiator;
    uint8_t payload[4096];
    size_t  payload_len;
    uint8_t ct[4096];
    size_t  ct_len;
};

static struct {
    uint8_t init_s[32], init_e[32], resp_s[32], resp_e[32];
    uint8_t prologue[256];
    size_t  prologue_len;
    struct msg msgs[MAX_MSGS];
    int n_msgs;
} g;

static void die(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    exit(1);
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t unhex(uint8_t *out, size_t cap, const char *s)
{
    size_t n = 0;
    if (!strcmp(s, "-"))
        return 0;
    while (s[0] && s[1]) {
        int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0 || n >= cap)
            die("bad hex in spec");
        out[n++] = (uint8_t)(hi << 4 | lo);
        s += 2;
    }
    if (s[0])
        die("odd-length hex in spec");
    return n;
}

static void parse_spec(const char *path)
{
    FILE *f = fopen(path, "r");
    static char line[MAX_HEX];
    char key[64], a[MAX_HEX / 2], b[MAX_HEX / 2], c[MAX_HEX / 2];

    if (!f)
        die("cannot open spec");
    while (fgets(line, sizeof line, f)) {
        if (!strcmp(line, "\n") || line[0] == '#')
            continue;
        if (sscanf(line, "%63s", key) != 1)
            continue;
        if (!strcmp(key, "msg")) {
            struct msg *m;
            if (g.n_msgs >= MAX_MSGS)
                die("too many messages");
            m = &g.msgs[g.n_msgs++];
            if (sscanf(line, "%*s %1s %s %s", a, b, c) != 3)
                die("bad msg line");
            m->from_initiator = a[0] == 'i';
            m->payload_len = unhex(m->payload, sizeof m->payload, b);
            m->ct_len      = unhex(m->ct, sizeof m->ct, c);
        } else {
            if (sscanf(line, "%*s %s", a) != 1)
                die("bad key line");
            if      (!strcmp(key, "init_static"))    unhex(g.init_s, 32, a);
            else if (!strcmp(key, "init_ephemeral")) unhex(g.init_e, 32, a);
            else if (!strcmp(key, "resp_static"))    unhex(g.resp_s, 32, a);
            else if (!strcmp(key, "resp_ephemeral")) unhex(g.resp_e, 32, a);
            else if (!strcmp(key, "prologue"))
                g.prologue_len = unhex(g.prologue, sizeof g.prologue, a);
            else
                die("unknown key in spec");
        }
    }
    fclose(f);
    if (g.n_msgs < 3)
        die("need at least 3 messages (handshake)");
}

static void expect_ct(const char *what, const uint8_t *got, size_t got_len,
                      const uint8_t *want, size_t want_len)
{
    if (got_len != want_len || memcmp(got, want, got_len)) {
        fprintf(stderr, "FAIL: %s mismatch (got %zu bytes, want %zu)\n",
                what, got_len, want_len);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    noise_hs hi, hr;
    noise_session si, sr;
    uint8_t buf[NOISE_MSG2_LEN + NOISE_HS_MAX_PAYLOAD + NOISE_TAG_LEN];
    uint8_t plain[NOISE_HS_MAX_PAYLOAD];
    size_t plen;
    int i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <spec>\n", argv[0]);
        return 2;
    }
    parse_spec(argv[1]);

    noise_hs_init_initiator(&hi, g.init_s, g.prologue, g.prologue_len);
    noise_hs_init_responder(&hr, g.resp_s, g.prologue, g.prologue_len);
    noise_hs_set_ephemeral(&hi, g.init_e);
    noise_hs_set_ephemeral(&hr, g.resp_e);

    /* handshake: msg0 i->r, msg1 r->i, msg2 i->r (payloads allowed) */
    for (i = 0; i < 3; i++) {
        struct msg *m = &g.msgs[i];
        size_t n = 0;

        if (i == 0) {
            if (!m->from_initiator) die("msg0 not from initiator");
            n = noise_hs_write_msg1(&hi, buf, sizeof buf,
                                    m->payload, m->payload_len);
            if (!n) die("write_msg1");
            expect_ct("msg1", buf, n, m->ct, m->ct_len);
            if (noise_hs_read_msg1(&hr, m->ct, m->ct_len,
                                   plain, sizeof plain, &plen)) die("read_msg1");
        } else if (i == 1) {
            if (m->from_initiator) die("msg1 not from responder");
            n = noise_hs_write_msg2(&hr, buf, sizeof buf,
                                    m->payload, m->payload_len);
            if (!n) die("write_msg2");
            expect_ct("msg2", buf, n, m->ct, m->ct_len);
            if (noise_hs_read_msg2(&hi, m->ct, m->ct_len,
                                   plain, sizeof plain, &plen)) die("read_msg2");
            if (!hi.has_rs) die("initiator missing rs after msg2");
        } else {
            if (!m->from_initiator) die("msg2 not from initiator");
            n = noise_hs_write_msg3(&hi, buf, sizeof buf,
                                    m->payload, m->payload_len);
            if (!n) die("write_msg3");
            expect_ct("msg3", buf, n, m->ct, m->ct_len);
            if (noise_hs_read_msg3(&hr, m->ct, m->ct_len,
                                   plain, sizeof plain, &plen)) die("read_msg3");
        }
        if (plen != m->payload_len || memcmp(plain, m->payload, plen))
            die("handshake payload roundtrip");
    }

    noise_hs_split(&hi, &si);
    noise_hs_split(&hr, &sr);

    /* transport messages */
    for (; i < g.n_msgs; i++) {
        struct msg *m = &g.msgs[i];
        noise_session *snd = m->from_initiator ? &si : &sr;
        noise_session *rcv = m->from_initiator ? &sr : &si;
        size_t flen;

        if (m->payload_len > NOISE_FRAME_MAX_PT)
            die("transport payload too large");
        flen = noise_session_send(snd, m->payload, m->payload_len,
                                  buf, sizeof buf);
        if (!flen)
            die("session_send");
        /* vector ct has no 2-byte header; compare frame body */
        if (buf[0] != (uint8_t)(m->payload_len & 0xff) ||
            buf[1] != (uint8_t)(m->payload_len >> 8))
            die("frame header mismatch");
        expect_ct("transport", buf + 2, flen - 2, m->ct, m->ct_len);

        {
            long rl = noise_session_recv(rcv, buf, flen, plain, sizeof plain);
            if (rl < 0 || (size_t)rl != m->payload_len ||
                memcmp(plain, m->payload, m->payload_len))
                die("session_recv roundtrip");
        }
    }

    printf("PASS (%d messages)\n", g.n_msgs);
    return 0;
}
