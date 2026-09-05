/*
 * noise_xx.h - Noise_XX_25519_ChaChaPoly_BLAKE2b handshake + transport framing.
 *
 * Spec: progress/summary/03-v1-execution-handoff.md §4.1 (canonical: wiki
 * reference/native-api.md once written). Single-file implementation on top of
 * Monocypher primitives + self-written HMAC-BLAKE2b (Monocypher has no HMAC).
 *
 * Both the server (coolood) and the client (cooloo) use byte-identical copies
 * of this file pair; keep them in sync (tests/vendor_sync.sh enforces it).
 */
#ifndef NOISE_XX_H
#define NOISE_XX_H

#include <stddef.h>
#include <stdint.h>

#define NOISE_KEY_LEN         32
#define NOISE_TAG_LEN         16
#define NOISE_HASH_LEN        64   /* BLAKE2b-512, HASHLEN=64 suite */

/* Handshake message sizes with empty payload (production cooloo always uses
 * empty payloads; the API still accepts payloads for cacophony KAT fidelity). */
#define NOISE_MSG1_LEN        32   /* -> e */
#define NOISE_MSG2_LEN        96   /* <- e, ee, s, es : 32 + 48 + 16 */
#define NOISE_MSG3_LEN        64   /* -> s, se        : 48 + 16 */
#define NOISE_HS_MAX_PAYLOAD  1024 /* sanity cap for handshake payloads */

/* Transport: 2-byte LE plaintext length || AEAD ciphertext || 16B tag. */
#define NOISE_FRAME_MAX_PT    16384
#define NOISE_FRAME_HDR_LEN   2
#define NOISE_FRAME_OVERHEAD  (NOISE_FRAME_HDR_LEN + NOISE_TAG_LEN)

typedef struct {
    uint8_t  h[NOISE_HASH_LEN];   /* handshake hash */
    uint8_t  ck[NOISE_HASH_LEN];  /* chaining key */
    uint8_t  k[NOISE_KEY_LEN];    /* cipher key, valid iff has_k */
    uint64_t n;                   /* AEAD nonce for k */
    int      has_k;
    int      role;                /* 0 = initiator, 1 = responder */
    int      step;                /* initiator: 0 fresh,1 sent m1,2 read m2,3 done;
                                     responder: 0 fresh,1 read m1,2 sent m2,3 done */
    uint8_t  e[NOISE_KEY_LEN];    /* our ephemeral secret */
    uint8_t  e_pub[NOISE_KEY_LEN];
    uint8_t  s[NOISE_KEY_LEN];    /* our static secret (copied in) */
    uint8_t  s_pub[NOISE_KEY_LEN];
    uint8_t  rs[NOISE_KEY_LEN];   /* remote static public */
    int      has_rs;              /* set once remote static is known */
    uint8_t  re[NOISE_KEY_LEN];   /* initiator: responder ephemeral (for se) */
} noise_hs;

typedef struct {
    uint8_t  k[NOISE_KEY_LEN];
    uint64_t n;
} noise_cs;

typedef struct {
    noise_cs send;
    noise_cs recv;
} noise_session;

/* prologue is mixed into h before any message (production: "cooloo-v1"). */
void noise_hs_init_initiator(noise_hs *hs, const uint8_t s[NOISE_KEY_LEN],
                             const uint8_t *prologue, size_t prologue_len);
void noise_hs_init_responder(noise_hs *hs, const uint8_t s[NOISE_KEY_LEN],
                             const uint8_t *prologue, size_t prologue_len);

/* Handshake steps. Writers return the message length (fixed part + payload),
 * 0 on error (bad state / small buffer). Readers return 0 on success (and set
 * *pt_len to the extracted payload length), -1 on failure (bad state, bad
 * AEAD tag, truncated message, payload larger than pt_cap).
 * Production passes pt_len=0 / pt_cap=0 and uses the NOISE_MSGx_LEN sizes.
 * After read_msg2 succeeds the initiator knows the server static (hs->rs):
 * do TOFU verification *before* calling write_msg3 (msg3 leaks identity). */
size_t noise_hs_write_msg1(noise_hs *hs, uint8_t *out, size_t out_cap,
                           const uint8_t *pt, size_t pt_len);
int    noise_hs_read_msg1(noise_hs *hs, const uint8_t *in, size_t in_len,
                          uint8_t *pt, size_t pt_cap, size_t *pt_len);
size_t noise_hs_write_msg2(noise_hs *hs, uint8_t *out, size_t out_cap,
                           const uint8_t *pt, size_t pt_len);
int    noise_hs_read_msg2(noise_hs *hs, const uint8_t *in, size_t in_len,
                          uint8_t *pt, size_t pt_cap, size_t *pt_len);
size_t noise_hs_write_msg3(noise_hs *hs, uint8_t *out, size_t out_cap,
                           const uint8_t *pt, size_t pt_len);
int    noise_hs_read_msg3(noise_hs *hs, const uint8_t *in, size_t in_len,
                          uint8_t *pt, size_t pt_cap, size_t *pt_len);

/* Split into the two directional cipher states (call when step==3).
 * Initiator: send=ck1 recv=ck2; responder: send=ck2 recv=ck1. */
void noise_hs_split(const noise_hs *hs, noise_session *sess);

/* Encrypt one transport frame: out = LE16(len) || ct || tag.
 * Returns frame length (len + NOISE_FRAME_OVERHEAD) or 0 on bad args. */
size_t noise_session_send(noise_session *s, const uint8_t *pt, size_t len,
                          uint8_t *out, size_t out_cap);

/* Decrypt one complete frame (in_len = frame length incl. header+tag).
 * Returns plaintext length, or -1 on truncation/tag mismatch. */
long noise_session_recv(noise_session *s, const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_cap);

/* Helpers shared by daemon and CLI. */
void noise_public_key(uint8_t pub[NOISE_KEY_LEN],
                      const uint8_t priv[NOISE_KEY_LEN]);
/* fingerprint = hex(BLAKE2b-256(pubkey)), 64 chars + NUL (D19). */
void noise_fingerprint_hex(char out[65], const uint8_t pub[NOISE_KEY_LEN]);

/* KAT / test hooks (not used in production paths). */
void noise_hs_set_ephemeral(noise_hs *hs, const uint8_t e[NOISE_KEY_LEN]);
void noise_hs_hash(const noise_hs *hs, uint8_t out[NOISE_HASH_LEN]);

#endif /* NOISE_XX_H */
