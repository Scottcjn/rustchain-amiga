/*
 * rc_http.h - plain HTTP/1.1 over bsdsocket.library.
 *
 * Talks to the legacy node endpoint (http://50.28.86.131:8088 by
 * default in the attest module). Connection: close on every request,
 * whole response read into a caller buffer. The node's responses are
 * small Content-Length JSON bodies, no chunked handling needed.
 *
 * Needs a running TCP/IP stack: Roadshow or AmiTCP on real hardware,
 * bsdsocket_library = 1 in FS-UAE.
 *
 * The formatting and parsing helpers are portable and covered by the
 * host test; the socket calls are Amiga-only and become failing stubs
 * under -DHOST_TEST.
 *
 * Part of librustchain, the RustChain SDK for AmigaOS.
 */

#ifndef RC_HTTP_H
#define RC_HTTP_H

/* request buffer size, must hold headers plus the largest payload */
#define RC_HTTP_MAX 5120
/* response buffer size used by the attest module */
#define RC_RESP_MAX 8192

/*
 * User-Agent. Same string the shipped standalone miner sends; the
 * server does not gate on it, kept for wire familiarity.
 */
#define RC_HTTP_UA "rustchain-amiga/1.0"

/*
 * Open bsdsocket.library (version 3+). Returns 1 on success.
 * Safe to call more than once. Host build always returns 0.
 */
int rc_http_open(void);

/* close bsdsocket.library if rc_http_open opened it */
void rc_http_close(void);

/* format a POST request into out; returns length or -1 if too big */
int rc_http_format_post(char *out, int outlen,
                        const char *host, int port,
                        const char *path, const char *body);

/* format a GET request into out; returns length or -1 if too big */
int rc_http_format_get(char *out, int outlen,
                       const char *host, int port, const char *path);

/* parse the status code out of a raw response, -1 if not HTTP */
int rc_http_status(const char *resp);

/* pointer to the body (after the blank line), NULL if not found */
const char *rc_http_body(const char *resp);

/*
 * POST body to http://host:port/path, read the whole response
 * (headers plus body) into resp. Returns bytes read, or -1 on any
 * failure (resolve, connect, send). Requires rc_http_open() first.
 */
int rc_http_post(const char *host, int port,
                 const char *path, const char *body,
                 char *resp, int resplen);

/* same for GET */
int rc_http_get(const char *host, int port, const char *path,
                char *resp, int resplen);

#endif /* RC_HTTP_H */
