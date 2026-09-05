# Vendored: Monocypher 4.0.2

- Upstream: https://github.com/LoupVaillant/Monocypher
- Release tarball: `monocypher-4.0.2.tar.gz`
  - sha256(tarball)   = `38d07179738c0c90677dba3ceb7a7b8496bcfea758ba1a53e803fed30ae0879c`
- Files taken verbatim from `src/` of the tarball:
  - sha256(monocypher.c) = `afe2b098c8569577a84488e0b98d276d1fba6506adea68bb9241a52111734c59`
  - sha256(monocypher.h) = `f78bb31255cfb7beba66afd2137f5194c8a025cf40488b6cc1e295234d43f374`

Do not edit in place; upgrade by replacing the files and updating the hashes
above. The same two files exist in the client repo (`client/src/vendor/`);
`tests/vendor_sync.sh` verifies the copies are byte-identical.

Why Monocypher: public-domain/CC0, single-file C, provides X25519, BLAKE2b,
ChaCha20-IETF and Poly1305 — everything Noise_XX needs except HMAC, which
`../noise_xx.c` implements.
