// Host stub: io_owner only uses HMAC-SHA256 for token fingerprints; tests do
// not depend on the fingerprint value, so a deterministic fake is enough.
#pragma once
#include <stddef.h>
#include <string.h>
typedef int mbedtls_md_type_t;
typedef struct { int unused; } mbedtls_md_info_t;
#define MBEDTLS_MD_SHA256 6
static inline const mbedtls_md_info_t *mbedtls_md_info_from_type(mbedtls_md_type_t t)
{
    static const mbedtls_md_info_t info = {0};
    (void)t;
    return &info;
}
static inline int mbedtls_md_hmac(const mbedtls_md_info_t *md, const unsigned char *key, size_t keylen,
                                  const unsigned char *in, size_t ilen, unsigned char *out)
{
    (void)md; (void)in; (void)ilen;
    memset(out, 0, 32);
    for (size_t i = 0; i < keylen; i++) out[i % 32] ^= key[i];
    return 0;
}
