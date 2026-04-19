// kernel/cap/token.c
// Phase 15a: Capability Objects v2 — token slow-path helpers.
//
// The hot-path `cap_token_resolve` lives in object.c (needs full
// cap_object_t layout + g_cap_object_ptrs). Helpers that don't need
// a specific object (pretty-print, generic audience check given a
// pointer) live here.

#include "token.h"
#include "object.h"

bool cap_token_validate_audience(const struct cap_object *obj, int32_t pid) {
    if (!obj) return false;
    if (obj->flags & CAP_FLAG_PUBLIC) return true;
    for (uint8_t i = 0; i < obj->audience_count && i < 8; i++) {
        if (obj->audience_set[i] == pid) return true;
    }
    return false;
}

// ---- ASCII printer helpers (kernel has no libc in all contexts yet) ----

static int tok_put_str(char *buf, int pos, int buflen, const char *s) {
    while (*s && pos < buflen - 1) buf[pos++] = *s++;
    return pos;
}

static int tok_put_dec(char *buf, int pos, int buflen, uint32_t v) {
    char tmp[11];
    int tlen = 0;
    if (v == 0) {
        tmp[tlen++] = '0';
    } else {
        while (v) { tmp[tlen++] = (char)('0' + (v % 10u)); v /= 10u; }
    }
    while (tlen > 0 && pos < buflen - 1) buf[pos++] = tmp[--tlen];
    return pos;
}

static int tok_put_hex2(char *buf, int pos, int buflen, uint8_t v) {
    static const char hex[] = "0123456789abcdef";
    if (pos < buflen - 1) buf[pos++] = hex[(v >> 4) & 0xF];
    if (pos < buflen - 1) buf[pos++] = hex[v & 0xF];
    return pos;
}

int cap_token_describe(cap_token_t tok, char *buf, int buflen) {
    if (!buf || buflen <= 0) return 0;
    int pos = 0;
    pos = tok_put_str(buf, pos, buflen, "tok={gen=");
    pos = tok_put_dec(buf, pos, buflen, cap_token_gen(tok));
    pos = tok_put_str(buf, pos, buflen, ",idx=");
    pos = tok_put_dec(buf, pos, buflen, cap_token_idx(tok));
    pos = tok_put_str(buf, pos, buflen, ",flags=0x");
    pos = tok_put_hex2(buf, pos, buflen, cap_token_flags(tok));
    if (pos < buflen - 1) buf[pos++] = '}';
    buf[pos] = '\0';
    return pos;
}
