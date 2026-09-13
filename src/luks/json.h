/* falcon-boot: a read-only, allocation-free JSON scanner for LUKS2
 * metadata (docs/DESIGN.md Sections 3, 5, 9).
 *
 * Purpose. A LUKS2 header's second half is a JSON document: the keyslots,
 * their KDF parameters, the AF splitter settings, the digest objects and the
 * data segment all live there. Reading it is unavoidable if the loader is to
 * unlock p3 at all.
 *
 * Reasoning. This is NOT a JSON library and must never grow into one. It
 * parses by scanning the document in place: no allocator, no tree, no
 * unescaping until a caller asks for a specific string, and one fixed
 * container stack whose depth is a compile-time constant. Every value is a
 * (pointer, length) view into the caller's buffer, so the whole working set
 * for a lookup is a couple of dozen bytes of stack.
 *
 * The design target is not conformance, it is not reading out of bounds.
 * This code parses a 16 KB blob off a USB stick that anyone can have written
 * anything into, before a passphrase has been checked and before anything
 * has been authenticated. So every scan is bounded by the buffer length at
 * every step, the container stack is depth-limited, and a malformed document
 * is a clean JSON_ERR_SYNTAX rather than a best-effort interpretation. The
 * header checksum is verified before any of this runs (see luks.h), which
 * makes casual corruption a non-event; this bound is for the case where
 * someone rewrote the checksum too.
 *
 * Known and accepted limits, all fine for LUKS2 metadata:
 *   - Object member names are matched as raw bytes, so an escape sequence in
 *     a *name* will not match the plain spelling of that name. cryptsetup
 *     writes plain ASCII names.
 *   - Duplicate member names resolve to the first occurrence.
 *   - Numbers are returned as spans; json_u64 parses the integer forms only,
 *     and also accepts a decimal string, because LUKS2 writes 64-bit
 *     quantities (offsets, sizes) as JSON strings to dodge the double
 *     precision limit.
 *   - No UTF-8 validation of string contents.
 *
 * Interactions. Used only by src/luks/luks.c. base64 decoding rides along
 * here because every binary field in the format (salts, digests) is a
 * base64 JSON string, and the decoder is 30 lines that would otherwise need
 * a file of its own.
 */

#ifndef FALCON_LUKS_JSON_H
#define FALCON_LUKS_JSON_H

#include <stdint.h>
#include <stddef.h>

/* Status codes, repo convention: OK zero, errors negative. */
enum {
    JSON_OK           = 0,
    JSON_ERR_BAD_ARG  = -1,
    JSON_ERR_SYNTAX   = -2,  /* not well-formed within the bounds given */
    JSON_ERR_DEPTH    = -3,  /* nesting beyond JSON_MAX_DEPTH */
    JSON_ERR_NOTFOUND = -4,  /* no such member or index */
    JSON_ERR_TYPE     = -5,  /* value is not the kind the caller asked for */
    JSON_ERR_NOSPACE  = -6,  /* caller's output buffer is too small */
};

/* Bounded on purpose. Real LUKS2 metadata nests five deep
 * (keyslots -> "0" -> kdf -> ...); 32 leaves enormous headroom while
 * keeping the scanner's stack frame a fixed 32 bytes. */
#define JSON_MAX_DEPTH 32u

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_OBJECT,
    JSON_ARRAY
} json_type;

/* A view into the document. Nothing is copied.
 *
 * For JSON_STRING, ptr is the first byte after the opening quote and len
 * runs to the byte before the closing quote, so the span is still escaped;
 * json_str() is what turns it into bytes. For every other type the span is
 * the value's whole text, brackets included for containers. */
typedef struct {
    json_type   type;
    const char *ptr;
    size_t      len;
} json_value;

/* Structurally validates the whole document and yields the root value.
 * Everything below assumes it has been called and succeeded. */
int json_parse(const char *buf, size_t len, json_value *root);

/* Direct member and element access. name_len is the exact length of name,
 * so a path component can be passed without being copied out first. */
int json_obj_get(const json_value *obj, const char *name, size_t name_len,
                 json_value *out);
int json_arr_get(const json_value *arr, size_t index, json_value *out);

/* Dotted path from a container: "keyslots.0.kdf.type". A component is an
 * object member name or, inside an array, a decimal index. */
int json_get(const json_value *root, const char *path, json_value *out);

/* Iteration over a container's members or elements. For an array, key is
 * set to a JSON_NULL with a zero length. */
typedef struct {
    const char *p;      /* cursor inside the container's span */
    const char *end;    /* one past the last byte of the span */
    char        close;  /* '}' or ']' */
    uint8_t     first;
} json_iter;

int json_iter_init(const json_value *container, json_iter *it);
/* 1 = a pair was produced, 0 = end of container, negative = error. */
int json_iter_next(json_iter *it, json_value *key, json_value *val);

/* Unescapes a string value into out and NUL-terminates it. \uXXXX is
 * decoded to UTF-8, surrogate pairs included. *out_len may be NULL. */
int json_str(const json_value *v, char *out, size_t cap, size_t *out_len);
/* 1 when the string value equals the plain ASCII literal s. Compares the
 * raw span, so an escaped spelling does not match; see the header's limits. */
int json_str_eq(const json_value *v, const char *s);
/* Accepts a JSON integer or a decimal string; rejects anything that would
 * overflow 64 bits, and rejects a fractional or exponent form. */
int json_u64(const json_value *v, uint64_t *out);

/* Base64 (RFC 4648 standard alphabet, '=' padded, no embedded whitespace)
 * decode of a string value. Writes at most cap bytes. */
int json_b64(const json_value *v, uint8_t *out, size_t cap, size_t *out_len);

#endif /* FALCON_LUKS_JSON_H */
