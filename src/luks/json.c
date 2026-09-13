/* falcon-boot: read-only JSON scanner for LUKS2 metadata. See json.h.
 *
 * The one structural routine is value_at(): it recognises the value that
 * starts at a cursor, advances the cursor past the whole of it, and reports
 * its type and span. Containers are walked with an explicit stack rather
 * than by recursion, so the frame is a fixed JSON_MAX_DEPTH bytes and no
 * input can drive the loader off the end of its EFI stack. Every read of
 * the buffer is guarded against the length; there is no "we know a closing
 * quote is coming" anywhere in here.
 */

#include <luks/json.h>
#include <lib/string.h>

static void skip_ws(const char *b, size_t n, size_t *p)
{
    while (*p < n) {
        char c = b[*p];

        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            break;
        }
        (*p)++;
    }
}

static int is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* On entry b[*p] is the opening quote. On success *p is past the closing
 * quote and, when body is non-NULL, it receives the span between them. */
static int string_span(const char *b, size_t n, size_t *p, json_value *body)
{
    size_t start;

    if (*p >= n || b[*p] != '"') {
        return JSON_ERR_SYNTAX;
    }
    (*p)++;
    start = *p;
    while (*p < n) {
        unsigned char c = (unsigned char)b[*p];

        if (c == '"') {
            if (body != NULL) {
                body->type = JSON_STRING;
                body->ptr = b + start;
                body->len = *p - start;
            }
            (*p)++;
            return JSON_OK;
        }
        if (c == '\\') {
            char e;

            (*p)++;
            if (*p >= n) {
                return JSON_ERR_SYNTAX;
            }
            e = b[*p];
            if (e == 'u') {
                unsigned int i;

                if (*p + 4u >= n) {
                    return JSON_ERR_SYNTAX;
                }
                for (i = 1u; i <= 4u; i++) {
                    if (hex_val(b[*p + i]) < 0) {
                        return JSON_ERR_SYNTAX;
                    }
                }
                *p += 4u;
            } else if (e != '"' && e != '\\' && e != '/' && e != 'b' &&
                       e != 'f' && e != 'n' && e != 'r' && e != 't') {
                return JSON_ERR_SYNTAX;
            }
            (*p)++;
            continue;
        }
        if (c < 0x20u) {
            return JSON_ERR_SYNTAX;  /* unescaped control character */
        }
        (*p)++;
    }
    return JSON_ERR_SYNTAX;          /* ran out of buffer before the quote */
}

static int number_span(const char *b, size_t n, size_t *p)
{
    size_t start = *p;

    if (*p < n && b[*p] == '-') {
        (*p)++;
    }
    if (*p >= n || !is_digit(b[*p])) {
        return JSON_ERR_SYNTAX;
    }
    /* RFC 8259: an integer part of "0" may not be followed by more digits.
     * Rejecting "01" costs one compare and keeps the scanner from quietly
     * accepting a document no other reader would. */
    if (b[*p] == '0') {
        (*p)++;
        if (*p < n && is_digit(b[*p])) {
            return JSON_ERR_SYNTAX;
        }
    } else {
        while (*p < n && is_digit(b[*p])) {
            (*p)++;
        }
    }
    if (*p < n && b[*p] == '.') {
        (*p)++;
        if (*p >= n || !is_digit(b[*p])) {
            return JSON_ERR_SYNTAX;
        }
        while (*p < n && is_digit(b[*p])) {
            (*p)++;
        }
    }
    if (*p < n && (b[*p] == 'e' || b[*p] == 'E')) {
        (*p)++;
        if (*p < n && (b[*p] == '+' || b[*p] == '-')) {
            (*p)++;
        }
        if (*p >= n || !is_digit(b[*p])) {
            return JSON_ERR_SYNTAX;
        }
        while (*p < n && is_digit(b[*p])) {
            (*p)++;
        }
    }
    return (*p > start) ? JSON_OK : JSON_ERR_SYNTAX;
}

static int literal(const char *b, size_t n, size_t *p, const char *lit)
{
    size_t i = 0;

    while (lit[i] != '\0') {
        if (*p + i >= n || b[*p + i] != lit[i]) {
            return JSON_ERR_SYNTAX;
        }
        i++;
    }
    *p += i;
    return JSON_OK;
}

/* ws, a string, ws, ':' - the part of an object member that precedes its
 * value. Leaves the cursor on the value's first byte. */
static int member_key(const char *b, size_t n, size_t *p, json_value *key)
{
    int rc;

    skip_ws(b, n, p);
    rc = string_span(b, n, p, key);
    if (rc != JSON_OK) {
        return rc;
    }
    skip_ws(b, n, p);
    if (*p >= n || b[*p] != ':') {
        return JSON_ERR_SYNTAX;
    }
    (*p)++;
    return JSON_OK;
}

/* Recognises and skips one complete value starting at *p (leading
 * whitespace allowed). Iterative: the stack holds the closing bracket each
 * open container is waiting for. */
static int value_at(const char *b, size_t n, size_t *p, json_value *out)
{
    char stack[JSON_MAX_DEPTH];
    size_t depth = 0;
    size_t start;
    json_type type = JSON_NULL;
    int rc;

    skip_ws(b, n, p);
    start = *p;
    if (*p >= n) {
        return JSON_ERR_SYNTAX;
    }

    for (;;) {
        char c;

        skip_ws(b, n, p);
        if (*p >= n) {
            return JSON_ERR_SYNTAX;
        }
        c = b[*p];

        if (c == '{' || c == '[') {
            if (depth == 0) {
                type = (c == '{') ? JSON_OBJECT : JSON_ARRAY;
            }
            if (depth >= JSON_MAX_DEPTH) {
                return JSON_ERR_DEPTH;
            }
            stack[depth] = (c == '{') ? '}' : ']';
            depth++;
            (*p)++;
            skip_ws(b, n, p);
            if (*p < n && b[*p] == stack[depth - 1u]) {
                (*p)++;            /* empty container */
                depth--;
            } else if (stack[depth - 1u] == '}') {
                rc = member_key(b, n, p, NULL);
                if (rc != JSON_OK) {
                    return rc;
                }
                continue;          /* go parse this member's value */
            } else {
                continue;          /* go parse the first element */
            }
        } else if (c == '"') {
            json_value s;

            rc = string_span(b, n, p, &s);
            if (rc != JSON_OK) {
                return rc;
            }
            if (depth == 0) {
                /* A bare string is the one value whose reported span is not
                 * its whole text: json.h defines it as the bytes between
                 * the quotes, which string_span already produced. */
                if (out != NULL) {
                    *out = s;
                }
                return JSON_OK;
            }
        } else if (c == '-' || is_digit(c)) {
            rc = number_span(b, n, p);
            if (rc != JSON_OK) {
                return rc;
            }
            if (depth == 0) {
                type = JSON_NUMBER;
            }
        } else if (c == 't' || c == 'f' || c == 'n') {
            const char *lit = (c == 't') ? "true" :
                              ((c == 'f') ? "false" : "null");

            rc = literal(b, n, p, lit);
            if (rc != JSON_OK) {
                return rc;
            }
            if (depth == 0) {
                type = (c == 'n') ? JSON_NULL : JSON_BOOL;
            }
        } else {
            return JSON_ERR_SYNTAX;
        }

        /* A value has just been consumed. Close out as many containers as
         * end here, then either finish or move to the next sibling. */
        for (;;) {
            if (depth == 0) {
                if (out != NULL) {
                    out->type = type;
                    out->ptr = b + start;
                    out->len = *p - start;
                }
                return JSON_OK;
            }
            skip_ws(b, n, p);
            if (*p >= n) {
                return JSON_ERR_SYNTAX;
            }
            if (b[*p] == stack[depth - 1u]) {
                (*p)++;
                depth--;
                continue;
            }
            if (b[*p] != ',') {
                return JSON_ERR_SYNTAX;
            }
            (*p)++;
            if (stack[depth - 1u] == '}') {
                rc = member_key(b, n, p, NULL);
                if (rc != JSON_OK) {
                    return rc;
                }
            }
            break;
        }
    }
}

int json_parse(const char *buf, size_t len, json_value *root)
{
    size_t p = 0;
    int rc;

    if (buf == NULL || root == NULL) {
        return JSON_ERR_BAD_ARG;
    }
    rc = value_at(buf, len, &p, root);
    if (rc != JSON_OK) {
        return rc;
    }
    skip_ws(buf, len, &p);
    return (p == len) ? JSON_OK : JSON_ERR_SYNTAX;
}

int json_iter_init(const json_value *container, json_iter *it)
{
    if (container == NULL || it == NULL || container->len < 2u) {
        return JSON_ERR_BAD_ARG;
    }
    if (container->type == JSON_OBJECT) {
        it->close = '}';
    } else if (container->type == JSON_ARRAY) {
        it->close = ']';
    } else {
        return JSON_ERR_TYPE;
    }
    it->p = container->ptr + 1;                    /* past the bracket */
    it->end = container->ptr + container->len - 1; /* before the closer */
    it->first = 1u;
    return JSON_OK;
}

int json_iter_next(json_iter *it, json_value *key, json_value *val)
{
    const char *b = it->p;
    size_t n = (size_t)(it->end - it->p);
    size_t p = 0;
    int rc;

    skip_ws(b, n, &p);
    if (p >= n) {
        return 0;                                  /* end of container */
    }
    if (!it->first) {
        if (b[p] != ',') {
            return JSON_ERR_SYNTAX;
        }
        p++;
        skip_ws(b, n, &p);
        if (p >= n) {
            return JSON_ERR_SYNTAX;
        }
    }
    it->first = 0u;

    if (key != NULL) {
        key->type = JSON_NULL;
        key->ptr = b + p;
        key->len = 0;
    }
    if (it->close == '}') {
        rc = member_key(b, n, &p, key);
        if (rc != JSON_OK) {
            return rc;
        }
    }
    rc = value_at(b, n, &p, val);
    if (rc != JSON_OK) {
        return rc;
    }
    it->p = b + p;
    return 1;
}

int json_obj_get(const json_value *obj, const char *name, size_t name_len,
                 json_value *out)
{
    json_iter it;
    json_value k, v;
    int rc;

    if (obj == NULL || name == NULL || out == NULL) {
        return JSON_ERR_BAD_ARG;
    }
    if (obj->type != JSON_OBJECT) {
        return JSON_ERR_TYPE;
    }
    rc = json_iter_init(obj, &it);
    if (rc != JSON_OK) {
        return rc;
    }
    for (;;) {
        rc = json_iter_next(&it, &k, &v);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            return JSON_ERR_NOTFOUND;
        }
        if (k.len == name_len && memcmp(k.ptr, name, name_len) == 0) {
            *out = v;
            return JSON_OK;
        }
    }
}

int json_arr_get(const json_value *arr, size_t index, json_value *out)
{
    json_iter it;
    json_value v;
    size_t i = 0;
    int rc;

    if (arr == NULL || out == NULL) {
        return JSON_ERR_BAD_ARG;
    }
    if (arr->type != JSON_ARRAY) {
        return JSON_ERR_TYPE;
    }
    rc = json_iter_init(arr, &it);
    if (rc != JSON_OK) {
        return rc;
    }
    for (;;) {
        rc = json_iter_next(&it, NULL, &v);
        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            return JSON_ERR_NOTFOUND;
        }
        if (i == index) {
            *out = v;
            return JSON_OK;
        }
        i++;
    }
}

int json_get(const json_value *root, const char *path, json_value *out)
{
    json_value cur;
    size_t i = 0;

    if (root == NULL || path == NULL || out == NULL) {
        return JSON_ERR_BAD_ARG;
    }
    cur = *root;
    while (path[i] != '\0') {
        size_t start = i;
        size_t seg;
        json_value next;
        int rc;

        while (path[i] != '\0' && path[i] != '.') {
            i++;
        }
        seg = i - start;
        if (seg == 0) {
            return JSON_ERR_BAD_ARG;
        }

        if (cur.type == JSON_OBJECT) {
            rc = json_obj_get(&cur, path + start, seg, &next);
        } else if (cur.type == JSON_ARRAY) {
            size_t idx = 0;
            size_t j;

            for (j = 0; j < seg; j++) {
                if (!is_digit(path[start + j])) {
                    return JSON_ERR_BAD_ARG;
                }
                idx = idx * 10u + (size_t)(path[start + j] - '0');
            }
            rc = json_arr_get(&cur, idx, &next);
        } else {
            return JSON_ERR_TYPE;
        }
        if (rc != JSON_OK) {
            return rc;
        }
        cur = next;
        if (path[i] == '.') {
            i++;
            if (path[i] == '\0') {
                return JSON_ERR_BAD_ARG;
            }
        }
    }
    *out = cur;
    return JSON_OK;
}

/* Appends a code point as UTF-8. Lone surrogates are passed through as the
 * replacement character rather than rejected: a malformed escape in a field
 * this loader does not interpret must not abort the unlock. */
static int utf8_put(char *out, size_t cap, size_t *at, uint32_t cp)
{
    uint8_t seq[4];
    size_t n;
    size_t i;

    if (cp < 0x80u) {
        seq[0] = (uint8_t)cp;
        n = 1;
    } else if (cp < 0x800u) {
        seq[0] = (uint8_t)(0xc0u | (cp >> 6));
        seq[1] = (uint8_t)(0x80u | (cp & 0x3fu));
        n = 2;
    } else if (cp < 0x10000u) {
        seq[0] = (uint8_t)(0xe0u | (cp >> 12));
        seq[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        seq[2] = (uint8_t)(0x80u | (cp & 0x3fu));
        n = 3;
    } else {
        seq[0] = (uint8_t)(0xf0u | (cp >> 18));
        seq[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3fu));
        seq[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3fu));
        seq[3] = (uint8_t)(0x80u | (cp & 0x3fu));
        n = 4;
    }
    if (*at + n + 1u > cap) {
        return JSON_ERR_NOSPACE;
    }
    for (i = 0; i < n; i++) {
        out[*at + i] = (char)seq[i];
    }
    *at += n;
    return JSON_OK;
}

static uint32_t hex4(const char *p)
{
    return (uint32_t)((hex_val(p[0]) << 12) | (hex_val(p[1]) << 8) |
                      (hex_val(p[2]) << 4) | hex_val(p[3]));
}

int json_str(const json_value *v, char *out, size_t cap, size_t *out_len)
{
    size_t i = 0;
    size_t at = 0;

    if (v == NULL || out == NULL || cap == 0) {
        return JSON_ERR_BAD_ARG;
    }
    if (v->type != JSON_STRING) {
        return JSON_ERR_TYPE;
    }
    while (i < v->len) {
        char c = v->ptr[i];

        if (c != '\\') {
            if (at + 2u > cap) {
                return JSON_ERR_NOSPACE;
            }
            out[at++] = c;
            i++;
            continue;
        }
        i++;
        if (i >= v->len) {
            return JSON_ERR_SYNTAX;
        }
        switch (v->ptr[i]) {
        case '"':  c = '"';  break;
        case '\\': c = '\\'; break;
        case '/':  c = '/';  break;
        case 'b':  c = '\b'; break;
        case 'f':  c = '\f'; break;
        case 'n':  c = '\n'; break;
        case 'r':  c = '\r'; break;
        case 't':  c = '\t'; break;
        case 'u': {
            uint32_t cp;
            int rc;

            if (i + 4u >= v->len) {
                return JSON_ERR_SYNTAX;
            }
            cp = hex4(v->ptr + i + 1u);
            i += 5u;
            if (cp >= 0xd800u && cp <= 0xdbffu &&
                i + 5u < v->len && v->ptr[i] == '\\' &&
                v->ptr[i + 1u] == 'u') {
                uint32_t lo = hex4(v->ptr + i + 2u);

                if (lo >= 0xdc00u && lo <= 0xdfffu) {
                    cp = 0x10000u + ((cp - 0xd800u) << 10) + (lo - 0xdc00u);
                    i += 6u;
                }
            }
            if (cp >= 0xd800u && cp <= 0xdfffu) {
                cp = 0xfffdu;               /* unpaired surrogate */
            }
            rc = utf8_put(out, cap, &at, cp);
            if (rc != JSON_OK) {
                return rc;
            }
            continue;
        }
        default:
            return JSON_ERR_SYNTAX;
        }
        if (at + 2u > cap) {
            return JSON_ERR_NOSPACE;
        }
        out[at++] = c;
        i++;
    }
    out[at] = '\0';
    if (out_len != NULL) {
        *out_len = at;
    }
    return JSON_OK;
}

int json_str_eq(const json_value *v, const char *s)
{
    size_t n = 0;

    if (v == NULL || s == NULL || v->type != JSON_STRING) {
        return 0;
    }
    while (s[n] != '\0') {
        n++;
    }
    return (n == v->len) && (memcmp(v->ptr, s, n) == 0);
}

int json_u64(const json_value *v, uint64_t *out)
{
    const char *p;
    size_t n;
    size_t i;
    uint64_t acc = 0;

    if (v == NULL || out == NULL) {
        return JSON_ERR_BAD_ARG;
    }
    if (v->type != JSON_NUMBER && v->type != JSON_STRING) {
        return JSON_ERR_TYPE;
    }
    p = v->ptr;
    n = v->len;
    if (n == 0) {
        return JSON_ERR_SYNTAX;
    }
    for (i = 0; i < n; i++) {
        if (!is_digit(p[i])) {
            return JSON_ERR_SYNTAX;   /* signs, fractions, exponents */
        }
        {
            uint64_t digit = (uint64_t)(p[i] - '0');
            uint64_t limit = (uint64_t)UINT64_MAX;

            if (acc > (limit - digit) / (uint64_t)10) {
                return JSON_ERR_SYNTAX;   /* would overflow */
            }
            acc = acc * (uint64_t)10 + digit;
        }
    }
    *out = acc;
    return JSON_OK;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

int json_b64(const json_value *v, uint8_t *out, size_t cap, size_t *out_len)
{
    size_t i;
    size_t at = 0;
    uint32_t acc = 0;
    unsigned int bits = 0;
    size_t pad = 0;

    if (v == NULL || out == NULL) {
        return JSON_ERR_BAD_ARG;
    }
    if (v->type != JSON_STRING) {
        return JSON_ERR_TYPE;
    }
    if ((v->len % 4u) != 0u) {
        return JSON_ERR_SYNTAX;
    }
    for (i = 0; i < v->len; i++) {
        char c = v->ptr[i];
        int d;

        if (c == '=') {
            /* Padding only in the last two positions, and nothing after. */
            if (i + 2u < v->len) {
                return JSON_ERR_SYNTAX;
            }
            pad++;
            continue;
        }
        if (pad != 0u) {
            return JSON_ERR_SYNTAX;
        }
        d = b64_val(c);
        if (d < 0) {
            return JSON_ERR_SYNTAX;
        }
        acc = (acc << 6) | (uint32_t)d;
        bits += 6u;
        if (bits >= 8u) {
            bits -= 8u;
            if (at >= cap) {
                return JSON_ERR_NOSPACE;
            }
            out[at++] = (uint8_t)(acc >> bits);
        }
    }
    if (pad > 2u) {
        return JSON_ERR_SYNTAX;
    }
    if (out_len != NULL) {
        *out_len = at;
    }
    return JSON_OK;
}
