/*
 * amiport - MacPorts-style package client for classic AmigaOS (m68k).
 *
 * Talks plain HTTP/1.0 to an amiports repo (see ports/FORMAT.md):
 *   <repo>/index.txt                the package index
 *   <repo>/packages/<archive>       .apak package archives
 *
 * Commands:
 *   amiport list                       fetch and print the index
 *   amiport info <pkg>                 print one package's index entry
 *   amiport install <pkg>              download, verify SHA-1, extract
 *   amiport installed                  print the local package db
 *
 * Options (after the command):
 *   --repo http://host:port           repo base URL
 *   --prefix DIR                      install prefix (default SYS:amiports)
 *
 * Install layout: <prefix>/<pkg>/<files...>, registration appended to
 * <prefix>/installed.db as "name|version|sha1|filecount".
 *
 * Archives are .apak (uncompressed, big-endian u32 fields, see
 * ports/FORMAT.md). The download lands in a temp file, is then read into
 * one RAM buffer, SHA-1'd, and extracted from that same buffer, so the
 * verified bytes are exactly the extracted bytes (no verify/extract race).
 * Archives are capped at MAX_ARCHIVE_BYTES to bound that allocation.
 *
 * m68k rules: C89, no 64-bit math, no %lld (see vendor/rtc_common.h).
 *
 * Exit codes: 0 ok, 5 usage, 10 network, 12 package not in index,
 * 13 sha1 mismatch, 14 extract/io error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vendor/rtc_common.h"

#ifndef HOST_TEST
#include <dos/dos.h>
#include <proto/dos.h>
#define DEFAULT_PREFIX "SYS:amiports"
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#define DEFAULT_PREFIX "./amiports"
#endif

#define AMIPORT_VERSION "1.0"
#define DEFAULT_REPO "http://127.0.0.1:8873"

#define INDEX_CAP 65536L
#define MAX_ARCHIVE_FILES 1000UL
#define MAX_MEMBER_SIZE (8UL * 1024UL * 1024UL)
/* the whole archive is read into one RAM buffer before it is verified and
   extracted (see cmd_install), so cap it to keep a hostile server from
   asking for a wild allocation. Spec packages are ~1MB; this leaves head
   room without being unbounded. */
#define MAX_ARCHIVE_BYTES (4UL * 1024UL * 1024UL)

/* one parsed line of index.txt:
   name|version|archive|sha1|size|license|description */
struct pkg_entry {
    char name[64];
    char version[32];
    char archive[96];
    char sha1[48];
    char license[32];
    char descr[160];
    unsigned long size;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                      */
/* ------------------------------------------------------------------ */

/* join prefix + component into out; Amiga "VOL:" needs no separator */
static void path_join(char *out, int outlen, const char *prefix,
                      const char *comp)
{
    int plen = (int)strlen(prefix);
    const char *sep = "/";

    if (plen > 0 && (prefix[plen - 1] == ':' || prefix[plen - 1] == '/'))
        sep = "";
    if (plen + (int)strlen(sep) + (int)strlen(comp) >= outlen) {
        out[0] = '\0';
        return;
    }
    sprintf(out, "%s%s%s", prefix, sep, comp);
}

/* mkdir that succeeds when the directory already exists */
static int make_dir(const char *path)
{
#ifndef HOST_TEST
    BPTR lock;

    lock = CreateDir((STRPTR)path);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    lock = Lock((STRPTR)path, SHARED_LOCK);
    if (lock) {
        UnLock(lock);
        return 1;
    }
    return 0;
#else
    if (mkdir(path, 0755) == 0)
        return 1;
    return errno == EEXIST;
#endif
}

static void delete_file(const char *path)
{
#ifndef HOST_TEST
    DeleteFile((STRPTR)path);
#else
    remove(path);
#endif
}

/* archive member names: relative, '/' separated, no tricks (FORMAT.md:
   ASCII, no leading '/', no ':', no '\', no '..' segments) */
static int name_is_safe(const char *name)
{
    const char *p;

    if (name[0] == '\0' || name[0] == '/')
        return 0;
    for (p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c >= 0x7f)   /* control or non-ASCII */
            return 0;
        if (*p == ':' || *p == '\\')
            return 0;
    }
    for (p = name; *p; ) {
        if (p[0] == '.' && p[1] == '.' &&
            (p[2] == '/' || p[2] == '\0'))
            return 0;
        while (*p && *p != '/')
            p++;
        if (*p == '/')
            p++;
    }
    return 1;
}

/* package names are a single path component (FORMAT.md: [a-z0-9-]); much
   stricter than member names, no '/' at all. The name comes from the repo
   index and is used as a directory component, so guard it before any path
   is built from it. */
static int pkg_name_is_safe(const char *name)
{
    const char *p;

    if (name[0] == '\0')
        return 0;
    if (strstr(name, "..") != NULL)
        return 0;
    for (p = name; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f)   /* control chars */
            return 0;
        if (c == '/' || c == ':' || c == '\\')
            return 0;
    }
    return 1;
}

/* read a big-endian u32 from buf[*pos], advancing *pos; 0 if short.
   Bounds are checked as "bytes remaining < 4" so a near-max *pos cannot
   wrap the addition on ILP32 m68k. */
static int buf_u32(const unsigned char *buf, unsigned long len,
                   unsigned long *pos, unsigned long *v)
{
    unsigned long p = *pos;

    if (p > len || len - p < 4)
        return 0;
    *v = ((unsigned long)buf[p] << 24) | ((unsigned long)buf[p + 1] << 16) |
         ((unsigned long)buf[p + 2] << 8) | (unsigned long)buf[p + 3];
    *pos = p + 4;
    return 1;
}

/* ------------------------------------------------------------------ */
/* index handling                                                     */
/* ------------------------------------------------------------------ */

/* split one index line (pipe separated) into e; returns 1 ok */
static int parse_index_line(const char *line, struct pkg_entry *e)
{
    char *fields[7];
    static char work[512];
    char *p;
    int i;

    if ((int)strlen(line) >= (int)sizeof(work))
        return 0;
    strcpy(work, line);

    p = work;
    for (i = 0; i < 7; i++) {
        fields[i] = p;
        if (i < 6) {
            p = strchr(p, '|');
            if (!p)
                return 0;
            *p++ = '\0';
        }
    }

    if (strlen(fields[0]) == 0 || strlen(fields[0]) >= sizeof(e->name))
        return 0;
    if (strlen(fields[1]) >= sizeof(e->version)) return 0;
    if (strlen(fields[2]) >= sizeof(e->archive)) return 0;
    if (strlen(fields[3]) >= sizeof(e->sha1)) return 0;
    if (strlen(fields[5]) >= sizeof(e->license)) return 0;

    strcpy(e->name, fields[0]);
    strcpy(e->version, fields[1]);
    strcpy(e->archive, fields[2]);
    strcpy(e->sha1, fields[3]);
    e->size = strtoul(fields[4], NULL, 10);
    strcpy(e->license, fields[5]);
    strncpy(e->descr, fields[6], sizeof(e->descr) - 1);
    e->descr[sizeof(e->descr) - 1] = '\0';
    return 1;
}

/* fetch index.txt into buf (NUL terminated body only); 0 on failure */
static int fetch_index(const char *repo_host, int repo_port,
                       const char *repo_path, char *buf, long buflen)
{
    static char resp[INDEX_CAP];
    char path[560];
    const char *body;
    long got;
    int status;

    path_join(path, (int)sizeof(path), repo_path, "index.txt");
    got = rtc_http_get(repo_host, repo_port, path, resp, (long)sizeof(resp));
    if (got <= 0)
        return 0;
    status = rtc_http_status(resp);
    body = rtc_http_body(resp);
    if (status != 200 || !body) {
        fprintf(stderr, "amiport: index fetch gave HTTP %d\n", status);
        return 0;
    }
    if ((long)strlen(body) >= buflen) {
        fprintf(stderr, "amiport: index too large\n");
        return 0;
    }
    strcpy(buf, body);
    return 1;
}

/* walk index lines; returns 1 and fills e when pkg found */
static int index_find(const char *index_body, const char *pkg,
                      struct pkg_entry *e)
{
    const char *p = index_body;
    static char line[512];

    while (*p) {
        const char *nl = strchr(p, '\n');
        long n = nl ? (long)(nl - p) : (long)strlen(p);

        if (n > 0 && n < (long)sizeof(line)) {
            memcpy(line, p, (size_t)n);
            line[n] = '\0';
            if (line[n - 1] == '\r')
                line[n - 1] = '\0';
            if (line[0] != '#' && parse_index_line(line, e) &&
                strcmp(e->name, pkg) == 0)
                return 1;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* commands                                                           */
/* ------------------------------------------------------------------ */

static int cmd_list(const char *host, int port, const char *rpath)
{
    static char body[INDEX_CAP];
    static char line[512];
    struct pkg_entry e;
    const char *p;
    int count = 0;

    if (!fetch_index(host, port, rpath, body, INDEX_CAP))
        return 10;

    printf("%-20s %-10s %9s  %s\n", "NAME", "VERSION", "SIZE", "DESCRIPTION");
    p = body;
    while (*p) {
        const char *nl = strchr(p, '\n');
        long n = nl ? (long)(nl - p) : (long)strlen(p);

        if (n > 0 && n < (long)sizeof(line)) {
            memcpy(line, p, (size_t)n);
            line[n] = '\0';
            if (line[n - 1] == '\r')
                line[n - 1] = '\0';
            if (line[0] != '#' && parse_index_line(line, &e)) {
                printf("%-20s %-10s %9lu  %s\n",
                       e.name, e.version, e.size, e.descr);
                count++;
            }
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    printf("%d package%s in the repo\n", count, count == 1 ? "" : "s");
    return 0;
}

static int cmd_info(const char *host, int port, const char *rpath,
                    const char *pkg)
{
    static char body[INDEX_CAP];
    struct pkg_entry e;

    if (!fetch_index(host, port, rpath, body, INDEX_CAP))
        return 10;
    if (!index_find(body, pkg, &e)) {
        fprintf(stderr, "amiport: package '%s' not in the index\n", pkg);
        return 12;
    }
    printf("name:        %s\n", e.name);
    printf("version:     %s\n", e.version);
    printf("description: %s\n", e.descr);
    printf("license:     %s\n", e.license);
    printf("archive:     %s\n", e.archive);
    printf("size:        %lu bytes\n", e.size);
    printf("sha1:        %s\n", e.sha1);
    return 0;
}

/* extract an .apak archive held entirely in memory into destdir; returns
   file count or -1. buf/buf_len are the exact bytes that were SHA-verified
   by the caller, so parsing and extraction operate on the verified image
   and there is no reopen-by-path window between verify and extract. */
static long extract_apak(const unsigned char *buf, unsigned long buf_len,
                         const char *destdir)
{
    static char name[256];
    static char dest[512];
    static char sub[512];
    unsigned long pos = 0;
    unsigned long count, namelen, prot, size, i;
    long done = 0;

    if (buf_len < 8 || memcmp(buf, "APAK0001", 8) != 0) {
        fprintf(stderr, "amiport: not an APAK0001 archive\n");
        return -1;
    }
    pos = 8;
    if (!buf_u32(buf, buf_len, &pos, &count) ||
        count == 0 || count > MAX_ARCHIVE_FILES) {
        fprintf(stderr, "amiport: bad archive file count\n");
        return -1;
    }

    for (i = 0; i < count; i++) {
        char *p;
        FILE *o;

        if (!buf_u32(buf, buf_len, &pos, &namelen) || namelen == 0 ||
            namelen >= sizeof(name)) {
            fprintf(stderr, "amiport: bad member name length\n");
            return -1;
        }
        if (buf_len - pos < namelen) {
            fprintf(stderr, "amiport: truncated archive\n");
            return -1;
        }
        memcpy(name, buf + pos, (size_t)namelen);
        pos += namelen;
        name[namelen] = '\0';
        if (!name_is_safe(name)) {
            fprintf(stderr, "amiport: unsafe member name '%s'\n", name);
            return -1;
        }
        if (!buf_u32(buf, buf_len, &pos, &prot) ||
            !buf_u32(buf, buf_len, &pos, &size) ||
            size > MAX_MEMBER_SIZE) {
            fprintf(stderr, "amiport: bad member header\n");
            return -1;
        }
        if (buf_len - pos < size) {
            fprintf(stderr, "amiport: truncated member data\n");
            return -1;
        }

        /* create intermediate directories */
        for (p = name; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                path_join(sub, (int)sizeof(sub), destdir, name);
                *p = '/';
                if (sub[0] == '\0' || !make_dir(sub)) {
                    fprintf(stderr, "amiport: cannot create %s\n", sub);
                    return -1;
                }
            }
        }

        path_join(dest, (int)sizeof(dest), destdir, name);
        if (dest[0] == '\0') {
            fprintf(stderr, "amiport: path too long for '%s'\n", name);
            return -1;
        }
        o = fopen(dest, "wb");
        if (!o) {
            fprintf(stderr, "amiport: cannot write %s\n", dest);
            return -1;
        }
        if (size > 0 && fwrite(buf + pos, 1, (size_t)size, o) != size) {
            fprintf(stderr, "amiport: short write on %s\n", dest);
            fclose(o);
            return -1;
        }
        pos += size;
        fclose(o);
#ifndef HOST_TEST
        if (prot != 0)
            SetProtection((STRPTR)dest, (LONG)prot);
#else
        (void)prot;
#endif
        printf("  extracted %s (%lu bytes)\n", name, size);
        done++;
    }

    return done;
}

static int cmd_install(const char *host, int port, const char *rpath,
                       const char *pkg, const char *prefix)
{
    static char body[INDEX_CAP];
    struct pkg_entry e;
    char pkgdir[512];
    char tmpfile[512];
    char dbfile[512];
    char url_path[640];
    char got_sha1[41];
    FILE *f;
    unsigned char *abuf = NULL;
    long bodylen = 0, nfiles;
    int status;

    if (!fetch_index(host, port, rpath, body, INDEX_CAP))
        return 10;
    if (!index_find(body, pkg, &e)) {
        fprintf(stderr, "amiport: package '%s' not in the index\n", pkg);
        return 12;
    }
    /* the name becomes a directory component below; never trust it */
    if (!pkg_name_is_safe(e.name)) {
        fprintf(stderr, "amiport: refusing package with unsafe name '%s'\n",
                e.name);
        return 12;
    }

    printf("installing %s %s (%lu bytes, %s)\n",
           e.name, e.version, e.size, e.license);

    if (!make_dir(prefix)) {
        fprintf(stderr, "amiport: cannot create prefix %s\n", prefix);
        return 14;
    }
    path_join(pkgdir, (int)sizeof(pkgdir), prefix, e.name);
    if (pkgdir[0] == '\0' || !make_dir(pkgdir)) {
        fprintf(stderr, "amiport: cannot create %s\n", pkgdir);
        return 14;
    }

    /* Download the archive to a temp file (memory-light transport), then
       read the whole image into one RAM buffer, hash THAT buffer, and
       extract from it. The bytes we verify are byte-for-byte the bytes we
       extract, so there is no verify-then-reopen TOCTOU: swapping the temp
       file on disk after the hash cannot smuggle unverified bytes into the
       install, and nothing is ever reopened by path for extraction. */
    path_join(tmpfile, (int)sizeof(tmpfile), prefix, "download.apak.tmp");
    f = fopen(tmpfile, "wb");
    if (!f) {
        fprintf(stderr, "amiport: cannot write %s\n", tmpfile);
        return 14;
    }
    sprintf(url_path, "%s%spackages/%s", rpath,
            rpath[strlen(rpath) - 1] == '/' ? "" : "/", e.archive);
    status = rtc_http_get_file(host, port, url_path, f, &bodylen, got_sha1);
    fclose(f);
    if (status != 200) {
        fprintf(stderr, "amiport: archive fetch failed (HTTP %d)\n", status);
        delete_file(tmpfile);
        return 10;
    }
    printf("  fetched %s: %ld bytes\n", e.archive, bodylen);

    if (bodylen <= 0 || (unsigned long)bodylen > MAX_ARCHIVE_BYTES) {
        fprintf(stderr, "amiport: archive size %ld out of range\n", bodylen);
        delete_file(tmpfile);
        return 14;
    }
    abuf = (unsigned char *)malloc((size_t)bodylen);
    if (!abuf) {
        fprintf(stderr, "amiport: out of memory for %ld-byte archive\n",
                bodylen);
        delete_file(tmpfile);
        return 14;
    }
    f = fopen(tmpfile, "rb");
    if (!f || fread(abuf, 1, (size_t)bodylen, f) != (size_t)bodylen) {
        fprintf(stderr, "amiport: cannot read back %s\n", tmpfile);
        if (f)
            fclose(f);
        free(abuf);
        delete_file(tmpfile);
        return 14;
    }
    fclose(f);
    delete_file(tmpfile);   /* disk copy no longer needed, and not trusted */

    /* verify the in-memory image: this hash covers exactly what we extract */
    rtc_sha1_hex(abuf, (unsigned long)bodylen, got_sha1);
    if (strcmp(got_sha1, e.sha1) != 0) {
        fprintf(stderr, "amiport: SHA-1 MISMATCH, refusing to install\n");
        fprintf(stderr, "  index: %s\n", e.sha1);
        fprintf(stderr, "  got:   %s\n", got_sha1);
        free(abuf);
        return 13;
    }
    printf("  sha1 verified: %s\n", got_sha1);

    nfiles = extract_apak(abuf, (unsigned long)bodylen, pkgdir);
    free(abuf);
    if (nfiles < 0)
        return 14;

    /* register; if this write fails the install is not recorded, so it
       must not be reported as success */
    path_join(dbfile, (int)sizeof(dbfile), prefix, "installed.db");
    f = fopen(dbfile, "ab");
    if (!f) {
        fprintf(stderr, "amiport: cannot update %s, install not registered\n",
                dbfile);
        return 14;
    }
    if (fprintf(f, "%s|%s|%s|%ld\n", e.name, e.version, e.sha1, nfiles) < 0) {
        fprintf(stderr, "amiport: failed writing %s\n", dbfile);
        fclose(f);
        return 14;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "amiport: failed closing %s\n", dbfile);
        return 14;
    }

    printf("installed %s %s to %s (%ld file%s)\n",
           e.name, e.version, pkgdir, nfiles, nfiles == 1 ? "" : "s");
    return 0;
}

static int cmd_installed(const char *prefix)
{
    char dbfile[512];
    static char line[512];
    FILE *f;
    int count = 0;

    path_join(dbfile, (int)sizeof(dbfile), prefix, "installed.db");
    f = fopen(dbfile, "rb");
    if (!f) {
        printf("no packages installed under %s\n", prefix);
        return 0;
    }
    printf("installed packages (%s):\n", dbfile);
    while (fgets(line, (int)sizeof(line), f)) {
        char *nl = strchr(line, '\n');

        if (nl)
            *nl = '\0';
        if (line[0]) {
            printf("  %s\n", line);
            count++;
        }
    }
    fclose(f);
    printf("%d record%s\n", count, count == 1 ? "" : "s");
    return 0;
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf("amiport " AMIPORT_VERSION " - AmigaOS ports client\n");
    printf("usage: amiport <command> [args] [--repo URL] [--prefix DIR]\n");
    printf("  list                 show the repo index\n");
    printf("  info <pkg>           show one package\n");
    printf("  install <pkg>        download, verify sha1, extract\n");
    printf("  installed            show the local package db\n");
    printf("defaults: --repo " DEFAULT_REPO " --prefix " DEFAULT_PREFIX "\n");
}

int main(int argc, char **argv)
{
    const char *cmd = NULL, *pkg = NULL;
    const char *repo = DEFAULT_REPO;
    const char *prefix = DEFAULT_PREFIX;
    char host[128];
    char rpath[512];
    int port = 0;
    int i, rc;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--repo") == 0 && i + 1 < argc) {
            repo = argv[++i];
        } else if (strcmp(argv[i], "--prefix") == 0 && i + 1 < argc) {
            prefix = argv[++i];
        } else if (!cmd) {
            cmd = argv[i];
        } else if (!pkg) {
            pkg = argv[i];
        } else {
            usage();
            return 5;
        }
    }

    if (!cmd) {
        usage();
        return 5;
    }

    if (strcmp(cmd, "installed") == 0)
        return cmd_installed(prefix);

    if (!rtc_parse_url(repo, host, (int)sizeof(host), &port,
                       rpath, (int)sizeof(rpath))) {
        fprintf(stderr, "amiport: bad repo url (http:// only): %s\n", repo);
        return 5;
    }
    /* strip one trailing slash so path_join stays predictable */
    if (strlen(rpath) > 1 && rpath[strlen(rpath) - 1] == '/')
        rpath[strlen(rpath) - 1] = '\0';

    if (!rtc_net_open())
        return 10;

    if (strcmp(cmd, "list") == 0) {
        rc = cmd_list(host, port, rpath);
    } else if (strcmp(cmd, "info") == 0 && pkg) {
        rc = cmd_info(host, port, rpath, pkg);
    } else if (strcmp(cmd, "install") == 0 && pkg) {
        rc = cmd_install(host, port, rpath, pkg, prefix);
    } else {
        usage();
        rc = 5;
    }

    rtc_net_close();
    return rc;
}
