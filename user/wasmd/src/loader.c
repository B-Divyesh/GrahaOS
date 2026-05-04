// user/wasmd/src/loader.c
// Phase 26 Stage E.1 — WebAssembly 1.0 binary parser (header + sections,
// import section detail). Pure userspace; no syscalls. No execution.

#include "loader.h"

#include <stdint.h>
#include <stddef.h>

// LEB128 helpers (unsigned + signed). Returns bytes consumed (>= 0) or
// negative on failure. *out is populated on success.
static int leb128_u32(const uint8_t *buf, size_t len, uint32_t *out) {
    uint32_t result = 0;
    int shift = 0;
    int consumed = 0;
    while ((size_t)consumed < len) {
        uint8_t b = buf[consumed++];
        result |= ((uint32_t)(b & 0x7F)) << shift;
        if ((b & 0x80) == 0) {
            *out = result;
            return consumed;
        }
        shift += 7;
        if (shift >= 32) return WASM_E_BAD_LEB128;
    }
    return WASM_E_TRUNCATED;
}

// Read 4 bytes little-endian.
static int read_u32_le(const uint8_t *buf, size_t len, uint32_t *out) {
    if (len < 4) return WASM_E_TRUNCATED;
    *out = (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
    return 4;
}

// Read a UTF-8-ish name with length prefix (LEB128 u32 then bytes). Truncates
// to dst_max-1 chars + NUL. Returns total bytes consumed or negative.
static int read_name(const uint8_t *buf, size_t len, char *dst, size_t dst_max) {
    uint32_t name_len = 0;
    int n = leb128_u32(buf, len, &name_len);
    if (n < 0) return n;
    if ((size_t)(n) + name_len > len) return WASM_E_TRUNCATED;
    size_t copy = (name_len < dst_max - 1) ? name_len : (dst_max - 1);
    for (size_t i = 0; i < copy; i++) dst[i] = (char)buf[n + i];
    dst[copy] = '\0';
    return n + (int)name_len;
}

static int parse_import_section(const uint8_t *buf, size_t len,
                                wasm_module_t *m) {
    uint32_t n_imports = 0;
    int n = leb128_u32(buf, len, &n_imports);
    if (n < 0) return n;
    if (n_imports > WASM_MAX_IMPORTS) return WASM_E_TOO_MANY_IMPORTS;

    size_t off = (size_t)n;
    for (uint32_t i = 0; i < n_imports; i++) {
        wasm_import_t *imp = &m->imports[i];

        int r = read_name(buf + off, len - off, imp->module_name,
                          sizeof(imp->module_name));
        if (r < 0) return r;
        off += (size_t)r;

        r = read_name(buf + off, len - off, imp->field_name,
                      sizeof(imp->field_name));
        if (r < 0) return r;
        off += (size_t)r;

        if (off >= len) return WASM_E_TRUNCATED;
        imp->kind = buf[off++];

        // Per kind: parse the type ref. We don't validate the actual type
        // signature for substrate v1 — only the import descriptor scope.
        switch (imp->kind) {
        case WASM_IMPORT_FUNC: {
            uint32_t t = 0;
            r = leb128_u32(buf + off, len - off, &t);
            if (r < 0) return r;
            imp->type_idx = t;
            off += (size_t)r;
            break;
        }
        case WASM_IMPORT_TABLE:
        case WASM_IMPORT_MEMORY:
        case WASM_IMPORT_GLOBAL:
            // skip — we don't parse limits/global types in v1
            // accept and stop there; rest of section is skipped at top
            return WASM_E_NOT_IMPLEMENTED;
        default:
            return WASM_E_BAD_LEB128;
        }
    }
    m->n_imports = n_imports;
    return WASM_OK;
}

int wasm_load(const uint8_t *bytes, size_t len, wasm_module_t *out) {
    if (!bytes || !out || len < 8) return WASM_E_TRUNCATED;
    uint32_t magic = 0, version = 0;
    int r = read_u32_le(bytes, len, &magic);
    if (r < 0) return r;
    if (magic != WASM_MAGIC) return WASM_E_BAD_MAGIC;
    r = read_u32_le(bytes + 4, len - 4, &version);
    if (r < 0) return r;
    if (version != WASM_VERSION_1) return WASM_E_BAD_VERSION;

    out->bytes     = bytes;
    out->len       = len;
    out->version   = version;
    out->n_imports = 0;

    // Walk sections. Each section is: id (1 byte) + payload_len (LEB) + payload.
    size_t off = 8;
    while (off < len) {
        if (off >= len) break;
        uint8_t section_id = bytes[off++];
        uint32_t payload_len = 0;
        r = leb128_u32(bytes + off, len - off, &payload_len);
        if (r < 0) return r;
        off += (size_t)r;
        if (off + payload_len > len) return WASM_E_TRUNCATED;

        if (section_id == WASM_SEC_IMPORT) {
            int rc = parse_import_section(bytes + off, payload_len, out);
            if (rc < 0) return rc;
        }
        // We deliberately ignore other sections in v1. Type, function, code,
        // export, etc. are recognised by the spec but not modelled by the
        // substrate. A future runtime fills these in without breaking the
        // public loader API.

        off += payload_len;
    }
    return WASM_OK;
}

// Tiny strcmp.
static int wasm_streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

int wasm_validate_imports(const wasm_module_t *m,
                          const char *const *allowed_imports,
                          size_t n_allowed,
                          const char **out_missing) {
    if (out_missing) *out_missing = NULL;
    for (uint32_t i = 0; i < m->n_imports; i++) {
        const wasm_import_t *imp = &m->imports[i];
        // Only enforce check on func imports; tables/memories/globals from a
        // host module are accepted in v1 (substrate scope).
        if (imp->kind != WASM_IMPORT_FUNC) continue;
        int found = 0;
        for (size_t k = 0; k < n_allowed; k++) {
            if (allowed_imports[k] && wasm_streq(imp->field_name, allowed_imports[k])) {
                found = 1;
                break;
            }
        }
        if (!found) {
            if (out_missing) *out_missing = imp->field_name;
            return WASM_E_INSTANTIATE_MISSING_CAP;
        }
    }
    return WASM_OK;
}
