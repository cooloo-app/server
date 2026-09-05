/*
 * noise_xx.c - Noise_XX_25519_ChaChaPoly_BLAKE2b (HASHLEN=64).
 *
 * Why hand-rolled: we need a single-file, zero-dependency Noise for both
 * coolood and the CLI; Monocypher gives us X25519/BLAKE2b/ChaCha20/Poly1305
 * but no HMAC and no 12-byte-nonce AEAD (its AEAD is XChaCha, 24-byte nonce),
 * so HMAC-BLAKE2b (RFC 2104, block 128) and the RFC 8439 ChaChaPoly AEAD are
 * composed here. Correctness is enforced by the cacophony known-answer
 * vectors (tests/noise_vectors.py) — they must pass before any merge.
 *
 * Token order per Noise spec §7.5 (first letter = initiator key):
 *   XX: -> e | <- e, ee, s, es | -> s, se
 *   ee = DH(e_i, e_r)   es = DH(e_i, s_r)   se = DH(s_i, e_r)
 *
 * Handshake payloads: the Noise payload slot is fully implemented (cacophony
 * vectors exercise it), but production cooloo always sends empty payloads.
 */
#include "noise_xx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/monocypher.h"

/* ---------------------------------------------------------------- */
/* HMAC / HKDF (Noise §4, on top of Monocypher BLAKE2b)             */
/* ---------------------------------------------------------------- */

/* HMAC-BLAKE2b, RFC 2104, block size = 128 (BLAKE2b block bytes).
 * Two message parts so HKDF can do HMAC(temp, o1||0x02) without a copy. */
static void hmac_b2b(uint8_t out[64], const uint8_t *key, size_t key_len,
                     const uint8_t *m1, size_t m1_len,
                     const uint8_t *m2, size_t m2_len)
{
    uint8_t k[128];
    uint8_t inner[64];
    crypto_blake2b_ctx ctx;
    size_t i;

    memset(k, 0, sizeof k);
    if (key_len > sizeof k)          /* never happens here (keys are 64B) */
        crypto_blake2b(k, 64, key, key_len);
    else
        memcpy(k, key, key_len);

    for (i = 0; i < sizeof k; i++)
        k[i] ^= 0x36;
    crypto_blake2b_init(&ctx, 64);
    crypto_blake2b_update(&ctx, k, sizeof k);
    crypto_blake2b_update(&ctx, m1, m1_len);
    if (m2)
        crypto_blake2b_update(&ctx, m2, m2_len);
    crypto_blake2b_final(&ctx, inner);

    for (i = 0; i < sizeof k; i++)
        k[i] ^= 0x36 ^ 0x5c;         /* ipad -> opad */
    crypto_blake2b_init(&ctx, 64);
    crypto_blake2b_update(&ctx, k, sizeof k);
    crypto_blake2b_update(&ctx, inner, sizeof inner);
    crypto_blake2b_final(&ctx, out);
}

/* HKDF with 2 outputs (MixKey, Split): temp=HMAC(ck,ikm);
 * o1=HMAC(temp,0x01); o2=HMAC(temp,o1||0x02). */
static void hkdf2(uint8_t o1[64], uint8_t o2[64],
                  const uint8_t ck[64], const uint8_t *ikm, size_t ikm_len)
{
    static const uint8_t b1 = 0x01, b2 = 0x02;
    uint8_t temp[64];
    hmac_b2b(temp, ck, 64, ikm, ikm_len, 0, 0);
    hmac_b2b(o1, temp, 64, &b1, 1, 0, 0);
    hmac_b2b(o2, temp, 64, o1, 64, &b2, 1);
}

static void mix_hash(noise_hs *hs, const uint8_t *data, size_t len)
{
    crypto_blake2b_ctx ctx;
    crypto_blake2b_init(&ctx, 64);
    crypto_blake2b_update(&ctx, hs->h, 64);
    crypto_blake2b_update(&ctx, data, len);
    crypto_blake2b_final(&ctx, hs->h);
}

static void mix_key(noise_hs *hs, const uint8_t *ikm, size_t ikm_len)
{
    uint8_t o1[64], o2[64];
    hkdf2(o1, o2, hs->ck, ikm, ikm_len);
    memcpy(hs->ck, o1, 64);
    memcpy(hs->k, o2, 32);           /* HASHLEN=64: truncate to 32 (spec §4) */
    hs->n = 0;
    hs->has_k = 1;
}

/* ---------------------------------------------------------------- */
/* RFC 8439 ChaCha20-Poly1305 AEAD, nonce = 4 zero bytes || LE64(n) */
/* (Noise "ChaChaPoly"; Monocypher's own AEAD is XChaCha, unusable) */
/* ---------------------------------------------------------------- */

/* mac = Poly1305(ad || pad16 || ct || pad16 || le64(ad_len) || le64(ct_len))
 * under the one-time key = first 32 keystream bytes (counter 0). */
static void aead_mac(uint8_t tag[16],
                     const uint8_t key[32], uint64_t n,
                     const uint8_t *ad, size_t ad_len,
                     const uint8_t *ct, size_t ct_len)
{
    uint8_t nonce[12] = { 0 };
    uint8_t zeros[32] = { 0 };
    uint8_t otk[32];
    uint8_t lens[16];
    crypto_poly1305_ctx poly;
    size_t i;

    for (i = 0; i < 8; i++)
        nonce[4 + i] = (uint8_t)(n >> (8 * i));
    crypto_chacha20_ietf(otk, zeros, 32, key, nonce, 0);

    crypto_poly1305_init(&poly, otk);
    crypto_poly1305_update(&poly, ad, ad_len);
    if (ad_len % 16)
        crypto_poly1305_update(&poly, zeros, 16 - ad_len % 16);
    crypto_poly1305_update(&poly, ct, ct_len);
    if (ct_len % 16)
        crypto_poly1305_update(&poly, zeros, 16 - ct_len % 16);
    for (i = 0; i < 8; i++) {
        lens[i]     = (uint8_t)(ad_len >> (8 * i));
        lens[8 + i] = (uint8_t)(ct_len >> (8 * i));
    }
    crypto_poly1305_update(&poly, lens, 16);
    crypto_poly1305_final(&poly, tag);
}

static void aead_seal(uint8_t *ct, uint8_t tag[16],
                      const uint8_t key[32], uint64_t n,
                      const uint8_t *ad, size_t ad_len,
                      const uint8_t *pt, size_t pt_len)
{
    uint8_t nonce[12] = { 0 };
    size_t i;
    for (i = 0; i < 8; i++)
        nonce[4 + i] = (uint8_t)(n >> (8 * i));
    if (pt_len)
        crypto_chacha20_ietf(ct, pt, pt_len, key, nonce, 1);
    aead_mac(tag, key, n, ad, ad_len, ct, pt_len);
}

static int aead_open(uint8_t *pt,
                     const uint8_t key[32], uint64_t n,
                     const uint8_t *ad, size_t ad_len,
                     const uint8_t *ct, size_t ct_len, const uint8_t tag[16])
{
    uint8_t expected[16];
    uint8_t nonce[12] = { 0 };
    size_t i;
    aead_mac(expected, key, n, ad, ad_len, ct, ct_len);
    if (crypto_verify16(expected, tag))
        return -1;
    for (i = 0; i < 8; i++)
        nonce[4 + i] = (uint8_t)(n >> (8 * i));
    crypto_chacha20_ietf(pt, ct, ct_len, key, nonce, 1);
    return 0;
}

/* ---------------------------------------------------------------- */
/* EncryptAndHash / DecryptAndHash (Noise §5.2)                     */
/* ---------------------------------------------------------------- */

/* ct must have room for pt_len (+16 if has_k). Returns ciphertext length. */
static size_t encrypt_and_hash(noise_hs *hs, uint8_t *ct,
                               const uint8_t *pt, size_t pt_len)
{
    size_t ct_len = pt_len;
    if (hs->has_k) {
        aead_seal(ct, ct + pt_len, hs->k, hs->n++, hs->h, 64, pt, pt_len);
        ct_len += NOISE_TAG_LEN;
    } else if (pt_len) {
        memcpy(ct, pt, pt_len);
    }
    mix_hash(hs, ct, ct_len);
    return ct_len;
}

/* Returns plaintext length, or -1 on bad tag. State advances only on
 * success is NOT guaranteed for tag failure (h is not mixed on failure,
 * nonce is not consumed — caller aborts the connection anyway). */
static long decrypt_and_hash(noise_hs *hs, uint8_t *pt,
                             const uint8_t *ct, size_t ct_len)
{
    size_t pt_len = ct_len;
    if (hs->has_k) {
        if (ct_len < NOISE_TAG_LEN)
            return -1;
        pt_len -= NOISE_TAG_LEN;
        if (aead_open(pt, hs->k, hs->n, hs->h, 64, ct, pt_len, ct + pt_len))
            return -1;
        hs->n++;
    } else if (pt_len) {
        memcpy(pt, ct, pt_len);
    }
    mix_hash(hs, ct, ct_len);
    return (long)pt_len;
}

/* ---------------------------------------------------------------- */
/* Handshake                                                        */
/* ---------------------------------------------------------------- */

static const char kProtocolName[] = "Noise_XX_25519_ChaChaPoly_BLAKE2b"; /* 33B */

static void hs_init(noise_hs *hs, int role, const uint8_t s[NOISE_KEY_LEN],
                    const uint8_t *prologue, size_t prologue_len)
{
    memset(hs, 0, sizeof *hs);
    /* name (33B) <= HASHLEN: h = name || zeros to 64 (spec §5.2) */
    memcpy(hs->h, kProtocolName, sizeof kProtocolName - 1);
    memcpy(hs->ck, hs->h, 64);
    hs->role = role;
    memcpy(hs->s, s, NOISE_KEY_LEN);
    crypto_x25519_public_key(hs->s_pub, s);
    if (prologue_len)
        mix_hash(hs, prologue, prologue_len);
}

void noise_hs_init_initiator(noise_hs *hs, const uint8_t s[NOISE_KEY_LEN],
                             const uint8_t *prologue, size_t prologue_len)
{
    hs_init(hs, 0, s, prologue, prologue_len);
}

void noise_hs_init_responder(noise_hs *hs, const uint8_t s[NOISE_KEY_LEN],
                             const uint8_t *prologue, size_t prologue_len)
{
    hs_init(hs, 1, s, prologue, prologue_len);
}

void noise_hs_set_ephemeral(noise_hs *hs, const uint8_t e[NOISE_KEY_LEN])
{
    memcpy(hs->e, e, NOISE_KEY_LEN);
    crypto_x25519_public_key(hs->e_pub, e);
}

void noise_hs_hash(const noise_hs *hs, uint8_t out[NOISE_HASH_LEN])
{
    memcpy(out, hs->h, NOISE_HASH_LEN);
}

/* Fill ephemeral from OS RNG unless already injected (KAT hook). */
static int ensure_ephemeral(noise_hs *hs)
{
    static const uint8_t zero[NOISE_KEY_LEN] = { 0 };
    if (memcmp(hs->e, zero, NOISE_KEY_LEN))
        return 0;                    /* injected, pub already derived */
#ifdef __APPLE__
    arc4random_buf(hs->e, NOISE_KEY_LEN);
#else
    {
        /* /dev/urandom is fine here: no fork-after-read in either binary,
         * and Linux getrandom guarantees quality after boot. */
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f)
            return -1;
        if (fread(hs->e, 1, NOISE_KEY_LEN, f) != NOISE_KEY_LEN) {
            fclose(f);
            return -1;
        }
        fclose(f);
    }
#endif
    crypto_x25519_public_key(hs->e_pub, hs->e);
    return 0;
}

static void dh(uint8_t out[32], const uint8_t priv[32], const uint8_t pub[32])
{
    /* Monocypher 4.x returns void; low-order inputs yield an all-zero
     * shared secret which then fails the next AEAD tag check anyway. */
    crypto_x25519(out, priv, pub);
}

/* shared payload-append helper for writers: checks cap before mutating */
static int payload_fits(size_t cap, size_t fixed, size_t pt_len, int has_k)
{
    return cap >= fixed + pt_len + (has_k ? NOISE_TAG_LEN : 0);
}

size_t noise_hs_write_msg1(noise_hs *hs, uint8_t *out, size_t out_cap,
                           const uint8_t *pt, size_t pt_len)
{
    size_t off = 0;
    if (hs->role != 0 || hs->step != 0 || pt_len > NOISE_HS_MAX_PAYLOAD)
        return 0;
    if (!payload_fits(out_cap, NOISE_KEY_LEN, pt_len, hs->has_k))
        return 0;
    if (ensure_ephemeral(hs))
        return 0;
    memcpy(out, hs->e_pub, NOISE_KEY_LEN);
    mix_hash(hs, out, NOISE_KEY_LEN);
    off += NOISE_KEY_LEN;
    off += encrypt_and_hash(hs, out + off, pt, pt_len);
    hs->step = 1;
    return off;
}

int noise_hs_read_msg1(noise_hs *hs, const uint8_t *in, size_t in_len,
                       uint8_t *pt, size_t pt_cap, size_t *pt_len)
{
    long plen;
    if (hs->role != 1 || hs->step != 0 || in_len < NOISE_MSG1_LEN)
        return -1;
    if (in_len - NOISE_MSG1_LEN > pt_cap ||
        in_len - NOISE_MSG1_LEN > NOISE_HS_MAX_PAYLOAD)
        return -1;
    memcpy(hs->re, in, NOISE_KEY_LEN);      /* e_i, needed for ee and es */
    mix_hash(hs, in, NOISE_MSG1_LEN);
    plen = decrypt_and_hash(hs, pt, in + NOISE_MSG1_LEN,
                            in_len - NOISE_MSG1_LEN);
    if (plen < 0)
        return -1;
    *pt_len = (size_t)plen;
    hs->step = 1;
    return 0;
}

size_t noise_hs_write_msg2(noise_hs *hs, uint8_t *out, size_t out_cap,
                           const uint8_t *pt, size_t pt_len)
{
    uint8_t secret[32];
    size_t off = 0;

    if (hs->role != 1 || hs->step != 1 || pt_len > NOISE_HS_MAX_PAYLOAD)
        return 0;
    if (ensure_ephemeral(hs))
        return 0;
    /* fixed part: e(32) || AEAD(s)(48); payload AEAD follows */
    if (!payload_fits(out_cap, 80, pt_len, 1))
        return 0;

    memcpy(out + off, hs->e_pub, NOISE_KEY_LEN);        /* e */
    mix_hash(hs, hs->e_pub, NOISE_KEY_LEN);
    off += NOISE_KEY_LEN;

    dh(secret, hs->e, hs->re);                          /* ee = DH(e_r, e_i) */
    mix_key(hs, secret, 32);

    off += encrypt_and_hash(hs, out + off, hs->s_pub, NOISE_KEY_LEN); /* s */

    dh(secret, hs->s, hs->re);                          /* es = DH(s_r, e_i) */
    mix_key(hs, secret, 32);
    memset(hs->re, 0, sizeof hs->re);                   /* e_i done */

    off += encrypt_and_hash(hs, out + off, pt, pt_len); /* payload */
    hs->step = 2;
    return off;
}

int noise_hs_read_msg2(noise_hs *hs, const uint8_t *in, size_t in_len,
                       uint8_t *pt, size_t pt_cap, size_t *pt_len)
{
    uint8_t secret[32];
    long plen;

    if (hs->role != 0 || hs->step != 1 || in_len < NOISE_MSG2_LEN)
        return -1;
    if (in_len - 80 - NOISE_TAG_LEN > pt_cap ||
        in_len - 80 - NOISE_TAG_LEN > NOISE_HS_MAX_PAYLOAD)
        return -1;

    mix_hash(hs, in, NOISE_KEY_LEN);                    /* e_r */
    memcpy(hs->re, in, NOISE_KEY_LEN);                  /* keep for msg3 se */
    dh(secret, hs->e, hs->re);                          /* ee = DH(e_i, e_r) */
    mix_key(hs, secret, 32);

    if (decrypt_and_hash(hs, hs->rs, in + 32, 48) != NOISE_KEY_LEN)
        return -1;
    hs->has_rs = 1;    /* TOFU check must happen here, before msg3 */

    dh(secret, hs->e, hs->rs);                          /* es = DH(e_i, s_r) */
    mix_key(hs, secret, 32);

    plen = decrypt_and_hash(hs, pt, in + 80, in_len - 80);
    if (plen < 0)
        return -1;
    *pt_len = (size_t)plen;
    hs->step = 2;
    return 0;
}

size_t noise_hs_write_msg3(noise_hs *hs, uint8_t *out, size_t out_cap,
                           const uint8_t *pt, size_t pt_len)
{
    uint8_t secret[32];
    size_t off = 0;

    if (hs->role != 0 || hs->step != 2 || !hs->has_rs ||
        pt_len > NOISE_HS_MAX_PAYLOAD)
        return 0;
    if (!payload_fits(out_cap, 48, pt_len, 1))
        return 0;

    off += encrypt_and_hash(hs, out + off, hs->s_pub, NOISE_KEY_LEN); /* s */
    dh(secret, hs->s, hs->re);                          /* se = DH(s_i, e_r) */
    mix_key(hs, secret, 32);
    memset(hs->re, 0, sizeof hs->re);                   /* e_r done */

    off += encrypt_and_hash(hs, out + off, pt, pt_len); /* payload */
    hs->step = 3;
    return off;
}

int noise_hs_read_msg3(noise_hs *hs, const uint8_t *in, size_t in_len,
                       uint8_t *pt, size_t pt_cap, size_t *pt_len)
{
    uint8_t secret[32];
    long plen;

    if (hs->role != 1 || hs->step != 2 || in_len < NOISE_MSG3_LEN)
        return -1;
    if (in_len - 48 - NOISE_TAG_LEN > pt_cap ||
        in_len - 48 - NOISE_TAG_LEN > NOISE_HS_MAX_PAYLOAD)
        return -1;

    if (decrypt_and_hash(hs, hs->rs, in, 48) != NOISE_KEY_LEN)
        return -1;
    hs->has_rs = 1;
    dh(secret, hs->e, hs->rs);                          /* se = DH(e_r, s_i) */
    mix_key(hs, secret, 32);

    plen = decrypt_and_hash(hs, pt, in + 48, in_len - 48);
    if (plen < 0)
        return -1;
    *pt_len = (size_t)plen;
    hs->step = 3;
    return 0;
}

/* ---------------------------------------------------------------- */
/* Split + transport                                                */
/* ---------------------------------------------------------------- */

void noise_hs_split(const noise_hs *hs, noise_session *sess)
{
    uint8_t o1[64], o2[64];
    hkdf2(o1, o2, hs->ck, 0, 0);
    /* HASHLEN=64: truncate both outputs to 32 for cipher keys (spec §4) */
    if (hs->role == 0) {
        memcpy(sess->send.k, o1, 32);
        memcpy(sess->recv.k, o2, 32);
    } else {
        memcpy(sess->send.k, o2, 32);
        memcpy(sess->recv.k, o1, 32);
    }
    sess->send.n = 0;
    sess->recv.n = 0;
}

size_t noise_session_send(noise_session *s, const uint8_t *pt, size_t len,
                          uint8_t *out, size_t out_cap)
{
    if (len > NOISE_FRAME_MAX_PT || out_cap < len + NOISE_FRAME_OVERHEAD)
        return 0;
    out[0] = (uint8_t)(len & 0xff);
    out[1] = (uint8_t)(len >> 8);
    aead_seal(out + 2, out + 2 + len, s->send.k, s->send.n++, 0, 0, pt, len);
    return len + NOISE_FRAME_OVERHEAD;
}

long noise_session_recv(noise_session *s, const uint8_t *in, size_t in_len,
                        uint8_t *out, size_t out_cap)
{
    size_t len;
    if (in_len < NOISE_FRAME_OVERHEAD)
        return -1;
    len = (size_t)in[0] | ((size_t)in[1] << 8);
    if (len > NOISE_FRAME_MAX_PT || in_len != len + NOISE_FRAME_OVERHEAD)
        return -1;
    if (out_cap < len)
        return -1;
    if (aead_open(out, s->recv.k, s->recv.n, 0, 0, in + 2, len, in + 2 + len))
        return -1;
    s->recv.n++;
    return (long)len;
}

void noise_public_key(uint8_t pub[NOISE_KEY_LEN],
                      const uint8_t priv[NOISE_KEY_LEN])
{
    crypto_x25519_public_key(pub, priv);
}

void noise_fingerprint_hex(char out[65], const uint8_t pub[NOISE_KEY_LEN])
{
    static const char hexd[] = "0123456789abcdef";
    uint8_t digest[32];
    size_t i;
    crypto_blake2b(digest, 32, pub, NOISE_KEY_LEN);
    for (i = 0; i < 32; i++) {
        out[i * 2]     = hexd[digest[i] >> 4];
        out[i * 2 + 1] = hexd[digest[i] & 0xf];
    }
    out[64] = '\0';
}
