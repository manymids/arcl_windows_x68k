#include "mcp_json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

static const char *skip_string(const char *p)
{
    if (*p != '"')
        return NULL;
    p++;
    while (*p)
    {
        if (*p == '\\')
        {
            if (!p[1])
                return NULL;
            p += 2;
        }
        else if (*p++ == '"')
            return p;
    }
    return NULL;
}

static const char *find_key_value(const char *json, const char *key)
{
    const char *p = skip_ws(json);
    size_t key_len = strlen(key);

    if (*p != '{')
        return NULL;
    p++;
    for (;;)
    {
        const char *key_start;
        const char *key_end;
        p = skip_ws(p);
        if (*p == '}')
            return NULL;
        key_start = p;
        key_end = skip_string(p);
        if (!key_end)
            return NULL;
        p = skip_ws(key_end);
        if (*p++ != ':')
            return NULL;
        p = skip_ws(p);
        if ((size_t)(key_end - key_start) == key_len + 2 &&
            memcmp(key_start + 1, key, key_len) == 0)
            return p;

        if (*p == '"')
            p = skip_string(p);
        else
        {
            int depth = 0;
            while (*p)
            {
                if (*p == '"')
                {
                    p = skip_string(p);
                    if (!p)
                        return NULL;
                    continue;
                }
                if (*p == '{' || *p == '[')
                    depth++;
                else if (*p == '}' || *p == ']')
                {
                    if (depth-- == 0)
                        break;
                }
                else if (*p == ',' && depth == 0)
                    break;
                p++;
            }
        }
        if (!p)
            return NULL;
        p = skip_ws(p);
        if (*p == ',')
        {
            p++;
            continue;
        }
        return NULL;
    }
}

static const char *find_key_value_any(const char *json, const char *key)
{
    const char *p = json;
    size_t key_len = strlen(key);

    while ((p = strchr(p, '"')) != NULL)
    {
        const char *key_start = p;
        const char *key_end = skip_string(p);
        const char *value;
        if (!key_end)
            return NULL;
        value = skip_ws(key_end);
        if (*value == ':' && (size_t)(key_end - key_start) == key_len + 2 &&
            memcmp(key_start + 1, key, key_len) == 0)
            return skip_ws(value + 1);
        p = key_end;
    }
    return NULL;
}

/* Parses exactly 4 hex digits at p into *out_value. Returns 0 (and leaves
 * *out_value untouched) on anything else, including a short/absent string -
 * this is deliberately strict per RFC 8259: \u must always be followed by
 * 4 hex digits. */
static int parse_hex4(const char *p, unsigned *out_value)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < 4; i++)
    {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out_value = v;
    return 1;
}

/* Appends the UTF-8 encoding of Unicode code point cp to out[*used],
 * bounds-checked against out_size. Returns 0 (without partially writing)
 * if it doesn't fit. */
static int append_utf8(char *out, size_t *used, size_t out_size, unsigned cp)
{
    unsigned char bytes[4];
    size_t n;

    if (cp <= 0x7F)
    {
        bytes[0] = (unsigned char)cp;
        n = 1;
    }
    else if (cp <= 0x7FF)
    {
        bytes[0] = (unsigned char)(0xC0 | (cp >> 6));
        bytes[1] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 2;
    }
    else if (cp <= 0xFFFF)
    {
        bytes[0] = (unsigned char)(0xE0 | (cp >> 12));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 3;
    }
    else
    {
        bytes[0] = (unsigned char)(0xF0 | (cp >> 18));
        bytes[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[3] = (unsigned char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*used + n >= out_size) /* leave room for the caller's trailing NUL */
        return 0;
    memcpy(out + *used, bytes, n);
    *used += n;
    return 1;
}

static int read_json_string(const char *p, char *out, size_t out_size)
{
    size_t used = 0;

    if (!p || *p++ != '"' || out_size == 0)
        return 0;
    while (*p && *p != '"')
    {
        char ch;
        if (*p == '\\')
        {
            p++;
            if (!*p)
                return 0;
            switch (*p)
            {
            case '"': case '\\': case '/': ch = *p; p++; break;
            case 'b': ch = '\b'; p++; break;
            case 'f': ch = '\f'; p++; break;
            case 'n': ch = '\n'; p++; break;
            case 'r': ch = '\r'; p++; break;
            case 't': ch = '\t'; p++; break;
            case 'u':
            {
                /* Standard JSON \uXXXX escape (RFC 8259 7): a lone BMP code
                 * point, or a surrogate pair (\uD800-\uDBFF followed by
                 * \uDC00-\uDFFF) for anything above U+FFFF - clients are
                 * always free to escape any character this way even though
                 * it's not required, so rejecting it (as this parser did
                 * before) breaks otherwise-conformant JSON producers. */
                unsigned cp, lo;
                p++; /* skip 'u' */
                if (!parse_hex4(p, &cp))
                    return 0;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    if (p[0] != '\\' || p[1] != 'u' || !parse_hex4(p + 2, &lo))
                        return 0;
                    if (lo < 0xDC00 || lo > 0xDFFF)
                        return 0;
                    cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    p += 6;
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                    return 0; /* unpaired low surrogate */
                if (!append_utf8(out, &used, out_size, cp))
                    return 0;
                continue; /* already wrote (and bounds-checked) the output bytes */
            }
            default: return 0;
            }
        }
        else
            ch = *p++;
        if (used + 1 >= out_size)
            return 0;
        out[used++] = ch;
    }
    if (*p != '"')
        return 0;
    out[used] = '\0';
    return 1;
}

int mcp_json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    return read_json_string(find_key_value(json, key), out, out_size);
}

int mcp_json_get_string_any(const char *json, const char *key, char *out, size_t out_size)
{
    return read_json_string(find_key_value_any(json, key), out, out_size);
}

int mcp_json_get_long_any(const char *json, const char *key, long *out)
{
    char *end;
    const char *value = find_key_value_any(json, key);
    long parsed;

    if (!value || !out)
        return 0;
    parsed = strtol(value, &end, 10);
    if (end == value || (*end && *end != ',' && *end != '}' && !isspace((unsigned char)*end)))
        return 0;
    *out = parsed;
    return 1;
}

int mcp_json_get_bool_any(const char *json, const char *key, int *out)
{
    const char *value = find_key_value_any(json, key);
    if (!value || !out)
        return 0;
    if (strncmp(value, "true", 4) == 0) { *out = 1; return 1; }
    if (strncmp(value, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

int mcp_json_get_id(const char *json, char *out, size_t out_size)
{
    const char *start = find_key_value(json, "id");
    const char *p;
    size_t len;

    if (!start || out_size == 0)
        return 0;
    if (*start == '"')
        p = skip_string(start);
    else
    {
        p = start;
        while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p))
            p++;
    }
    if (!p || p == start)
        return 0;
    len = (size_t)(p - start);
    if (len >= out_size)
        return 0;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}

void mcp_json_write_quoted(const char *text, char *out, size_t out_size)
{
    size_t used = 0;
    const unsigned char *p = (const unsigned char *)text;

    if (out_size == 0)
        return;
    out[used++] = '"';
    while (*p && used + 7 < out_size)
    {
        const char *escape = NULL;
        switch (*p)
        {
        case '"': escape = "\\\""; break;
        case '\\': escape = "\\\\"; break;
        case '\b': escape = "\\b"; break;
        case '\f': escape = "\\f"; break;
        case '\n': escape = "\\n"; break;
        case '\r': escape = "\\r"; break;
        case '\t': escape = "\\t"; break;
        default: break;
        }
        if (escape)
        {
            size_t len = strlen(escape);
            memcpy(out + used, escape, len);
            used += len;
        }
        else if (*p < 0x20)
            used += (size_t)snprintf(out + used, out_size - used, "\\u%04x", *p);
        else
            out[used++] = (char)*p;
        p++;
    }
    if (used + 2 <= out_size)
        out[used++] = '"';
    out[used < out_size ? used : out_size - 1] = '\0';
}

int mcp_json_get_hex_or_int_any(const char *json, const char *key, uint32_t *out)
{
    long lv;
    char buf[32];

    if (mcp_json_get_long_any(json, key, &lv))
    {
        *out = (uint32_t)lv;
        return 1;
    }
    if (mcp_json_get_string_any(json, key, buf, sizeof(buf)))
    {
        char *end;
        unsigned long v;
        int base = (buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) ? 16 : 10;
        v = strtoul(buf, &end, base);
        if (end != buf && *end == '\0')
        {
            *out = (uint32_t)v;
            return 1;
        }
    }
    return 0;
}

void mcp_json_set_error(char *out, size_t out_size, const char *message)
{
    if (out_size)
    {
        strncpy(out, message, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

void mcp_json_appendf(char *buf, size_t buf_size, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!buf || !used || *used >= buf_size)
        return;
    va_start(ap, fmt);
    n = vsnprintf(buf + *used, buf_size - *used, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= buf_size - *used)
        *used = buf_size; /* would have been truncated - clamp so no later
                            * call can compute an out-of-bounds pointer/size */
    else
        *used += (size_t)n;
}

/* Returns a pointer just past the JSON value starting at *p (which must not
 * be whitespace), or NULL on malformed input. Objects/arrays are matched by
 * bracket-agnostic depth counting (so `{"a":[1,{"b":2}]}` skips as one
 * unit); strings account for escapes via skip_string(); bare scalars
 * (numbers/true/false/null) stop at the next structural delimiter. */
static const char *value_end(const char *p)
{
    if (*p == '"')
        return skip_string(p);
    if (*p == '{' || *p == '[')
    {
        int depth = 0;
        while (*p)
        {
            if (*p == '"')
            {
                p = skip_string(p);
                if (!p)
                    return NULL;
                continue;
            }
            if (*p == '{' || *p == '[')
                depth++;
            else if (*p == '}' || *p == ']')
            {
                depth--;
                if (depth == 0)
                    return p + 1;
            }
            p++;
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p))
        p++;
    return p;
}

int mcp_json_extract_object(const char *json, const char *key, char *out, size_t out_size)
{
    const char *start = find_key_value_any(json, key);
    const char *end;
    size_t len;

    if (!start || *start != '{' || out_size == 0)
        return 0;
    end = value_end(start);
    if (!end)
        return 0;
    len = (size_t)(end - start);
    if (len >= out_size)
        return 0;
    memcpy(out, start, len);
    out[len] = '\0';
    return 1;
}
