/*
 * rtc_common.h - shared helpers for the RustChain Amiga tools
 * (rtcwallet, rtcfetch, rtctop)
 *
 * Code copied and adapted from miner/rustchain_amiga_miner.c so the tools
 * build without waiting on the sdk/ library. A later pass can swap this
 * directory for librustchain.
 *
 * m68k rules carried over from the miner (hard-won, do not "clean up"):
 *   - NO 64-bit shifts anywhere. bebbo gcc 6.5 at -m68000 miscompiles them.
 *     These tools avoid 64-bit math entirely; JSON numbers are handled as
 *     raw text and never converted unless they fit unsigned long.
 *   - No %lld. libnix printf cannot be trusted with it.
 *   - Plain HTTP over bsdsocket.library, GETs use HTTP/1.0 so servers
 *     never send chunked encoding.
 *   - C89-friendly: declarations first, no // comments in the .c.
 */

#ifndef RTC_COMMON_H
#define RTC_COMMON_H

#define RTC_TOOLS_VERSION "1.0"
#define RTC_DEFAULT_HOST "50.28.86.131"
#define RTC_DEFAULT_PORT 8088

/* production chain genesis, slot length. rtctop derives "now" from the
   server's slot number so an Amiga with an unset clock still shows sane
   attestation ages (resolution ~10 minutes). */
#define RTC_GENESIS_TS 1764706927UL
#define RTC_SLOT_SECONDS 600UL

/* ---- JSON (dumb scans, matches the miner's approach; good enough for
        the node's small flat responses) ---- */

/* Copy the raw value of "key": ... into out. Strings come back without
   quotes, numbers/true/false as raw text. Returns 1 on success.
   Finds the first occurrence of the key anywhere in the blob, so run it
   on a single extracted object when keys repeat. */
int rtc_json_raw(const char *json, const char *key, char *out, int outlen);

/* Return a pointer just past the '[' of "key": [ ... ], or NULL. */
const char *rtc_json_array(const char *json, const char *key);

/* From a position inside an array, copy the next balanced {...} object
   into out and return a pointer just past it. NULL when the array ends.
   Objects longer than outlen are truncated (node miner objects are ~240
   bytes, pass a 512 byte buffer). */
const char *rtc_json_next_obj(const char *p, char *out, int outlen);

/* ---- HTTP ---- */

/* Format an HTTP/1.0 GET (no chunked replies possible). Returns length
   or -1 if it does not fit. */
int rtc_format_get(char *out, int outlen, const char *host, int port,
                   const char *path);

/* Status code from a response, -1 if it does not look like HTTP. */
int rtc_http_status(const char *resp);

/* Pointer to the response body (past the blank line), NULL if missing. */
const char *rtc_http_body(const char *resp);

/* Parse http://host[:port][/path]. Returns 1 ok, 0 bad (https and
   anything else rejected; caller prints the reason). path gets "/" when
   absent. */
int rtc_parse_url(const char *url, char *host, int hostlen, int *port,
                  char *path, int pathlen);

/* ---- formatting ---- */

/* "37s" / "12m" / "3h" / "5d" into out (needs >= 16 bytes).
   then==0 gives "?", then>=now gives "0s". */
void rtc_age_str(unsigned long now, unsigned long then, char *out);

/* SHA-1 of a buffer as lowercase hex (copied from the miner, needed for
   the machine id and exposed so the host tests can hit NIST vectors). */
void rtc_sha1_hex(const unsigned char *data, unsigned long n, char out[41]);

/* This machine's suggested miner id: "amiga-<arch>-<rom sha1 first 8>".
   Detects CPU from ExecBase AttnFlags and hashes the 512KB ROM at
   0xF80000, same as the miner. outlen >= 32. */
void rtc_machine_id(char *out, int outlen);

/* ---- platform: network and break handling ---- */

/* Open bsdsocket.library (no-op on host). Returns 1 ok, 0 fail (prints
   the Roadshow/FS-UAE hint itself). Safe to call more than once. */
int rtc_net_open(void);
void rtc_net_close(void);

/* One-shot HTTP GET: connect, send, read to close. Returns total bytes
   received (headers + body) or -1 (reason printed to stderr).
   resp is always NUL terminated. */
long rtc_http_get(const char *host, int port, const char *path,
                  char *resp, long resplen);

/* Ctrl-C check (SIGBREAKF_CTRL_C, cleared on read). Host stub: 0. */
int rtc_check_break(void);

/* Sleep in 1s steps, returns 1 early if Ctrl-C arrived. */
int rtc_sleep_break(int seconds);

#endif /* RTC_COMMON_H */
