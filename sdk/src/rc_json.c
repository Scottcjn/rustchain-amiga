/*
 * rc_json.c - JSON escape, minimal field extractor, buffer builder.
 * Escape and extractor extracted unchanged from the working miner
 * (rustchain_amiga_miner.c). The builder is new SDK surface on top of
 * the same rules: no malloc, no floats, no %lld.
 */

#include <stdio.h>
#include <string.h>

#include "rustchain/rc_json.h"
#include "rustchain/rc_u64.h"

/* escape quote, backslash and control chars; truncates to fit */
void rc_json_escape(char *dst, int dstlen, const char *src)
{
    int o = 0;
    while (*src && o < dstlen - 8) {
        unsigned char ch = (unsigned char)*src++;
        if (ch == '"' || ch == '\\') {
            dst[o++] = '\\';
            dst[o++] = (char)ch;
        } else if (ch < 0x20) {
            sprintf(dst + o, "\\u%04x", ch);
            o += 6;
        } else {
            dst[o++] = (char)ch;
        }
    }
    dst[o] = '\0';
}

/* pull a string field value out of a JSON blob, dumb scan, good enough here */
int rc_json_find_string(const char *json, const char *key,
                        char *out, int outlen)
{
    char pat[64];
    const char *p;
    int o = 0;

    sprintf(pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == ':') p++;
    if (*p != '"') return 0;
    p++;
    while (*p && *p != '"' && o < outlen - 1)
        out[o++] = *p++;
    out[o] = '\0';
    return o > 0;
}

/* ------------------------------------------------------------------ */
/* buffer builder                                                     */
/* ------------------------------------------------------------------ */

void rc_jb_init(struct rc_jb *b, char *buf, int cap)
{
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->overflow = 0;
    if (cap > 0)
        buf[0] = '\0';
    else
        b->overflow = 1;
}

int rc_jb_ok(const struct rc_jb *b)
{
    return !b->overflow;
}

/* raw append with bounds check; sets overflow instead of writing past end */
static void jb_put(struct rc_jb *b, const char *s)
{
    int n = (int)strlen(s);
    if (b->overflow)
        return;
    if (b->len + n >= b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->buf + b->len, s, (size_t)n);
    b->len += n;
    b->buf[b->len] = '\0';
}

/* comma before a new item unless we are at the start of a container */
static void jb_comma(struct rc_jb *b)
{
    char last;
    if (b->len == 0)
        return;
    last = b->buf[b->len - 1];
    if (last != '{' && last != '[' && last != ':' && last != ',')
        jb_put(b, ",");
}

/* comma logic plus optional "key": prefix, shared by every item */
static void jb_item(struct rc_jb *b, const char *key)
{
    jb_comma(b);
    if (key) {
        char esc[96];
        rc_json_escape(esc, sizeof(esc), key);
        jb_put(b, "\"");
        jb_put(b, esc);
        jb_put(b, "\":");
    }
}

void rc_jb_obj_open(struct rc_jb *b, const char *key)
{
    jb_item(b, key);
    jb_put(b, "{");
}

void rc_jb_obj_close(struct rc_jb *b)
{
    jb_put(b, "}");
}

void rc_jb_arr_open(struct rc_jb *b, const char *key)
{
    jb_item(b, key);
    jb_put(b, "[");
}

void rc_jb_arr_close(struct rc_jb *b)
{
    jb_put(b, "]");
}

void rc_jb_str(struct rc_jb *b, const char *key, const char *val)
{
    jb_item(b, key);
    jb_put(b, "\"");
    if (!b->overflow) {
        /* escape straight into the buffer, then fix len */
        rc_json_escape(b->buf + b->len, b->cap - b->len, val);
        b->len += (int)strlen(b->buf + b->len);
        /* rc_json_escape truncates silently; treat a tight fit as overflow */
        if (b->cap - b->len < 8)
            b->overflow = 1;
    }
    jb_put(b, "\"");
}

void rc_jb_int(struct rc_jb *b, const char *key, int v)
{
    char num[24];
    sprintf(num, "%d", v);
    jb_item(b, key);
    jb_put(b, num);
}

/* note: unsigned long is 8 bytes on a 64-bit host build, size for 20 digits */
void rc_jb_ulong(struct rc_jb *b, const char *key, unsigned long v)
{
    char num[24];
    sprintf(num, "%lu", v);
    jb_item(b, key);
    jb_put(b, num);
}

void rc_jb_u64(struct rc_jb *b, const char *key, unsigned long long v)
{
    char num[24];
    jb_item(b, key);
    jb_put(b, rc_u64s(v, num));
}

void rc_jb_bool(struct rc_jb *b, const char *key, int v)
{
    jb_item(b, key);
    jb_put(b, v ? "true" : "false");
}

void rc_jb_raw(struct rc_jb *b, const char *key, const char *json)
{
    jb_item(b, key);
    jb_put(b, json);
}
