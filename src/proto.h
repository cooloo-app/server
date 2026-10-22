/*
 * proto.h - native line protocol engine (handoff §4.2), on top of noise_xx.
 *
 * daemon.c owns sockets/poll/handshake; proto.c owns everything after
 * Split(): the greeting, command parsing on the decrypted stream, the
 * users.conf registry, MSG streaming (READ/FOLLOW) with suspend/resume.
 *
 * The connection struct is shared: daemon.c keeps a fixed table of pconn.
 */
#ifndef COOLOO_PROTO_H
#define COOLOO_PROTO_H

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "cooloo.h"
#include "noise_xx.h"
#include "model.h"

#define CONN_OUT_CAP     (256 * 1024)  /* encrypted bytes pending write */
#define CONN_OUT_LOWAT   (64 * 1024)   /* resume suspended xfer below this */
#define CONN_IN_CAP      (64 * 1024)   /* decrypted stream not yet parsed */
#define CONN_MAX_FOLLOWS 8             /* handoff §4.2 limit */
#define SEND_RAW_MAX     (COOLOO_MAX_MSG * 2)  /* v0 raw buffer convention */

struct pconn {
    int      fd;
    int      established;              /* Split() done, line protocol live */
    int      close_pending;            /* fatal proto error: daemon closes */
    noise_session sess;

    /* outbound bytes (raw handshake bytes pre-establishment, then frames) */
    uint8_t  out[CONN_OUT_CAP];
    size_t   out_len;

    /* inbound decrypted stream awaiting parse */
    uint8_t  in[CONN_IN_CAP];
    size_t   in_len;

    char     nick[COOLOO_MAX_NICK + 1]; /* "" = unregistered (D25 gate) */
    char     fp[65];                    /* authenticated public key */
    char     effective[65];             /* AS identity, immutable during streams */
    int      identity_confirmed;        /* mirror clients must acknowledge context */

    /* live follows; offset semantics = store byte offsets */
    struct {
        char   room[COOLOO_MAX_ROOM + 1];
        off_t  offset;
        int    live;                   /* 0 while catch-up xfer runs */
    } follows[CONN_MAX_FOLLOWS];
    int      nfollows;

    /* at most one MSG transfer at a time (READ or FOLLOW catch-up);
     * MSG lines carry no room field, so sources are serialized */
    struct {
        int    active;
        char   room[COOLOO_MAX_ROOM + 1];
        off_t  offset;
        int    is_read;                /* 1: send END at EOF; 0: catch-up */
        int    is_sent;                /* filtered sender view, END includes scan cursor */
        int    follow_idx;             /* catch-up target (-1 for READ) */
        off_t  skip_until;             /* tail-read starting boundary */
    } xfer;

    /* SEND payload reception: header parsed, collecting len raw bytes */
    struct {
        int    active;
        int    discard;                /* oversize: skip len bytes, already ERR'd */
        char   room[COOLOO_MAX_ROOM + 1];
        size_t len;
        size_t got;
        char uuid[33];
        uint8_t buf[SEND_RAW_MAX];
    } sendst;

    /* Dedicated file transfer snapshots/staging, freed on disconnect. */
    void *file_upload;
    void *file_output;
    size_t file_size, file_got;
    unsigned long long file_base;
    char file_name[121];
    time_t   last_active;
};

/* Load <home>/users.conf into memory (daemon start). Missing file = empty. */
void proto_users_init(void);
void proto_cleanup(struct pconn *c);

/* nick bound to fingerprint, or "" (daemon calls after Split for greeting) */
void proto_lookup_nick(const char *fp_hex, char out_nick[COOLOO_MAX_NICK + 1]);

/* Queue the greeting line; call right after Split(). */
void proto_on_established(struct pconn *c);

/* Feed decrypted bytes; parses and executes complete commands. */
void proto_on_plain(struct pconn *c, const uint8_t *pt, size_t len);

/* 100ms tick: push increments to live follows (stat-based, D17). */
void proto_tick(struct pconn *c);

/* Outbound drained below low-water: resume a suspended xfer. */
void proto_on_writable(struct pconn *c);

/* Queue one plaintext chunk as an encrypted frame (daemon greeting path
 * uses proto_on_established; this is exported for completeness). */
int  proto_out(struct pconn *c, const char *line, size_t len);

#endif /* COOLOO_PROTO_H */
