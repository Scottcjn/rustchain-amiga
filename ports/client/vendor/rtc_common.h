/*
 * rtc_common.h - vendored copy for the amiport client.
 *
 * ORIGIN: copied from tools/common/rtc_common.h (RustChain Amiga tools)
 * on 2026-07-02. Vendored so ports/ never links against another agent's
 * directory. Modifications from the original:
 *   - removed rtc_machine_id, rtc_age_str, rtc_json_array,
 *     rtc_json_next_obj (amiport does not use them)
 *   - added rtc_http_get_file(): streaming GET straight to a FILE*,
 *     computing the SHA-1 of the body on the fly (packages up to ~1MB
 *     must not be held in RAM on a small Amiga)
 *   - HOST_TEST builds now use real POSIX sockets instead of stubs, so
 *     the full install flow can be tested on Linux against the same
 *     HTTP repo before going in-guest
 *
 * m68k rules carried over from the miner (hard-won, do not "clean up"):
 *   - NO 64-bit shifts anywhere. bebbo gcc 6.5 at -m68000 miscompiles them.
 *   - No %lld. libnix printf cannot be trusted with it.
 *   - Plain HTTP over bsdsocket.library, GETs use HTTP/1.0 so servers
 *     never send chunked encoding.
 *   - C89: declarations first, no // comments in the .c.
 */

#ifndef RTC_COMMON_H
#define RTC_COMMON_H

#include <stdio.h>

#define RTC_TOOLS_VERSION "1.0"

/* ---- JSON (dumb scans; good enough for flat responses) ---- */

/* Copy the raw value of "key": ... into out. Strings come back without
   quotes, numbers/true/false as raw text. Returns 1 on success. */
int rtc_json_raw(const char *json, const char *key, char *out, int outlen);

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

/* SHA-1 of a buffer as lowercase hex. */
void rtc_sha1_hex(const unsigned char *data, unsigned long n, char out[41]);

/* ---- platform: network and break handling ---- */

/* Open bsdsocket.library (no-op on host). Returns 1 ok, 0 fail (prints
   the Roadshow/FS-UAE hint itself). Safe to call more than once. */
int rtc_net_open(void);
void rtc_net_close(void);

/* One-shot HTTP GET into a buffer: connect, send, read to close.
   Returns total bytes received (headers + body) or -1 (reason printed
   to stderr). resp is always NUL terminated. */
long rtc_http_get(const char *host, int port, const char *path,
                  char *resp, long resplen);

/* Streaming HTTP GET: headers are parsed in a small internal buffer,
   the body is written to out in chunks, and the SHA-1 of the body is
   computed on the fly into sha1hex (41 bytes). Returns the HTTP status
   code (>=100), or -1 on network/protocol error (reason on stderr).
   bodylen receives the number of body bytes written. */
int rtc_http_get_file(const char *host, int port, const char *path,
                      FILE *out, long *bodylen, char sha1hex[41]);

/* Ctrl-C check (SIGBREAKF_CTRL_C, cleared on read). Host: 0. */
int rtc_check_break(void);

#endif /* RTC_COMMON_H */
