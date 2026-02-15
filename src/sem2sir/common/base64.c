#include "base64.h"

#include <stdlib.h>

static const char B64_ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t grit_base64_encode_bound(size_t n) {
    // 4 chars per 3 bytes, rounded up.
    return ((n + 2) / 3) * 4;
}

bool grit_base64_encode_alloc(const uint8_t *in, size_t in_len, char **out_str) {
    if (!out_str) return false;
    *out_str = NULL;
    if (in_len > 0 && !in) return false;

    size_t out_len = grit_base64_encode_bound(in_len);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return false;

    size_t oi = 0;
    size_t i = 0;
    while (i + 3 <= in_len) {
        uint32_t x = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | (uint32_t)in[i + 2];
        out[oi++] = B64_ALPHABET[(x >> 18) & 63];
        out[oi++] = B64_ALPHABET[(x >> 12) & 63];
        out[oi++] = B64_ALPHABET[(x >> 6) & 63];
        out[oi++] = B64_ALPHABET[x & 63];
        i += 3;
    }

    size_t rem = in_len - i;
    if (rem == 1) {
        uint32_t x = ((uint32_t)in[i] << 16);
        out[oi++] = B64_ALPHABET[(x >> 18) & 63];
        out[oi++] = B64_ALPHABET[(x >> 12) & 63];
        out[oi++] = '=';
        out[oi++] = '=';
    } else if (rem == 2) {
        uint32_t x = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8);
        out[oi++] = B64_ALPHABET[(x >> 18) & 63];
        out[oi++] = B64_ALPHABET[(x >> 12) & 63];
        out[oi++] = B64_ALPHABET[(x >> 6) & 63];
        out[oi++] = '=';
    }

    out[oi] = '\0';
    *out_str = out;
    return true;
}

static int b64_rev(uint8_t ch) {
    if (ch >= 'A' && ch <= 'Z') return (int)(ch - 'A');
    if (ch >= 'a' && ch <= 'z') return 26 + (int)(ch - 'a');
    if (ch >= '0' && ch <= '9') return 52 + (int)(ch - '0');
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

bool grit_base64_decode_alloc(const char *in, size_t in_len, uint8_t **out_bytes, size_t *out_len) {
    if (!out_bytes) return false;
    *out_bytes = NULL;
    if (out_len) *out_len = 0;

    if (in_len == 0) {
        uint8_t *out = (uint8_t *)malloc(1);
        if (!out) return false;
        *out_bytes = out;
        if (out_len) *out_len = 0;
        return true;
    }

    if (!in) return false;
    if (in_len % 4 != 0) return false;

    size_t pad = 0;
    if (in_len >= 1 && in[in_len - 1] == '=') pad++;
    if (in_len >= 2 && in[in_len - 2] == '=') pad++;

    size_t decoded_len = (in_len / 4) * 3;
    if (pad > decoded_len) return false;
    decoded_len -= pad;

    uint8_t *out = (uint8_t *)malloc(decoded_len ? decoded_len : 1);
    if (!out) return false;

    size_t oi = 0;
    for (size_t i = 0; i < in_len; i += 4) {
        int v0 = b64_rev((uint8_t)in[i]);
        int v1 = b64_rev((uint8_t)in[i + 1]);
        int v2 = (in[i + 2] == '=') ? -2 : b64_rev((uint8_t)in[i + 2]);
        int v3 = (in[i + 3] == '=') ? -2 : b64_rev((uint8_t)in[i + 3]);

        if (v0 < 0 || v1 < 0) {
            free(out);
            return false;
        }

        if (v2 == -1 || v3 == -1) {
            free(out);
            return false;
        }

        uint32_t x = ((uint32_t)v0 << 18) | ((uint32_t)v1 << 12);

        if (v2 >= 0) {
            x |= ((uint32_t)v2 << 6);
        }
        if (v3 >= 0) {
            x |= (uint32_t)v3;
        }

        if (oi < decoded_len) out[oi++] = (uint8_t)((x >> 16) & 0xFF);
        if (v2 == -2) {
            // '==' padding: no more bytes in this quantum.
            continue;
        }
        if (oi < decoded_len) out[oi++] = (uint8_t)((x >> 8) & 0xFF);
        if (v3 == -2) {
            // '=' padding: one more byte only.
            continue;
        }
        if (oi < decoded_len) out[oi++] = (uint8_t)(x & 0xFF);
    }

    if (oi != decoded_len) {
        free(out);
        return false;
    }

    *out_bytes = out;
    if (out_len) *out_len = decoded_len;
    return true;
}
