/*
 * rc_json.h - JSON helpers: string escape, a minimal field extractor,
 * and a small append-only buffer builder.
 *
 * This is not a JSON library. The node speaks small flat JSON bodies
 * over plain HTTP and this is exactly enough to build and read them on
 * a 68000 without malloc or floats.
 *
 * The builder never allocates: you give it a buffer, it appends into
 * it and sets an overflow flag instead of writing past the end. Commas
 * are inserted automatically. 64-bit values are printed via rc_u64s
 * (libnix %lld is not safe, see rc_u64.h).
 *
 * Part of librustchain, the RustChain SDK for AmigaOS.
 */

#ifndef RC_JSON_H
#define RC_JSON_H

/*
 * Escape quote, backslash and control chars into dst; truncates to
 * fit dstlen. Always NUL-terminates.
 */
void rc_json_escape(char *dst, int dstlen, const char *src);

/*
 * Pull a string field value out of a JSON blob. Dumb scan for
 * "key" then the quoted value; good enough for the node's small flat
 * responses (nonce, ticket_id). Key must be shorter than 60 chars.
 * Returns 1 and fills out on success, 0 if not found.
 */
int rc_json_find_string(const char *json, const char *key,
                        char *out, int outlen);

/* ------------------------------------------------------------------ */
/* buffer builder                                                     */
/* ------------------------------------------------------------------ */

struct rc_jb {
    char *buf;
    int cap;
    int len;
    int overflow;
};

/* bind the builder to a caller-owned buffer; writes buf[0] = NUL */
void rc_jb_init(struct rc_jb *b, char *buf, int cap);

/* 1 if everything fit so far, 0 after any overflow */
int rc_jb_ok(const struct rc_jb *b);

/*
 * All value functions take a key. Pass NULL for a bare value (array
 * element). A comma is inserted automatically where JSON needs one.
 */
void rc_jb_obj_open(struct rc_jb *b, const char *key);
void rc_jb_obj_close(struct rc_jb *b);
void rc_jb_arr_open(struct rc_jb *b, const char *key);
void rc_jb_arr_close(struct rc_jb *b);

void rc_jb_str(struct rc_jb *b, const char *key, const char *val);
void rc_jb_int(struct rc_jb *b, const char *key, int v);
void rc_jb_ulong(struct rc_jb *b, const char *key, unsigned long v);
void rc_jb_u64(struct rc_jb *b, const char *key, unsigned long long v);
void rc_jb_bool(struct rc_jb *b, const char *key, int v);

/* raw preformatted JSON fragment, caller guarantees validity */
void rc_jb_raw(struct rc_jb *b, const char *key, const char *json);

#endif /* RC_JSON_H */
