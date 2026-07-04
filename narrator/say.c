/*
 * say.c - speak text through the Amiga narrator.device
 *
 * Classic AmigaOS SAY from C:
 *   1. translator.library Translate() turns English into phonemes
 *   2. narrator.device CMD_WRITE speaks the phoneme string
 *
 * Usage:
 *   say "hello world"
 *   echo "hello from a pipe" | say
 *   say --dry-run "hello"      (print phonemes only, no audio)
 *
 * Cross-compile (bebbo m68k-amigaos-gcc, see Makefile):
 *   m68k-amigaos-gcc -noixemul -m68020 -O2 -fomit-frame-pointer -o say say.c
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <devices/narrator.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/translator.h>

#include <stdio.h>
#include <string.h>

#define INPUT_MAX 4096
#define PHON_MAX  16384

struct Library *TranslatorBase = NULL;

/* Keep the big buffers static, not on the stack. */
static char input[INPUT_MAX];
static char phonetic[PHON_MAX];

/* Audio channel allocation maps. Each byte names a left/right channel
 * pair; narrator.device grabs the first combination it can allocate.
 * 3, 5, 10, 12 are the classic stereo (one left + one right) pairs. */
static UBYTE chan_maps[] = { 3, 5, 10, 12 };

static struct MsgPort *port = NULL;
static struct narrator_rb *nrb = NULL;
static int dev_open = 0;
static int io_pending = 0;

static void cleanup(void)
{
    /* Order matters: finish/abort I/O, close device, delete request,
     * delete port, close library. */
    if (nrb) {
        if (io_pending) {
            AbortIO((struct IORequest *)nrb);
            WaitIO((struct IORequest *)nrb);
            io_pending = 0;
        }
        if (dev_open) {
            CloseDevice((struct IORequest *)nrb);
            dev_open = 0;
        }
        DeleteIORequest((struct IORequest *)nrb);
        nrb = NULL;
    }
    if (port) {
        DeleteMsgPort(port);
        port = NULL;
    }
    if (TranslatorBase) {
        CloseLibrary(TranslatorBase);
        TranslatorBase = NULL;
    }
}

int main(int argc, char **argv)
{
    int dry_run = 0;
    int i;
    size_t len = 0;
    LONG terr;
    BYTE derr;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dry-run") == 0) {
            dry_run = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0 ||
                   strcmp(argv[i], "?") == 0) {
            printf("Usage: say [--dry-run] [text...]\n");
            printf("Reads stdin when no text is given.\n");
            printf("--dry-run prints the phoneme translation instead of speaking.\n");
            fflush(stdout);
            return 0;
        } else {
            size_t alen = strlen(argv[i]);
            if (len > 0 && len < INPUT_MAX - 1)
                input[len++] = ' ';
            if (alen > INPUT_MAX - 1 - len)
                alen = INPUT_MAX - 1 - len;
            memcpy(input + len, argv[i], alen);
            len += alen;
        }
    }
    input[len] = '\0';

    if (len == 0) {
        len = fread(input, 1, INPUT_MAX - 1, stdin);
        input[len] = '\0';
    }

    /* Strip trailing whitespace/newlines. */
    while (len > 0 && (input[len - 1] == '\n' || input[len - 1] == '\r' ||
                       input[len - 1] == ' '  || input[len - 1] == '\t'))
        input[--len] = '\0';

    if (len == 0) {
        printf("say: nothing to speak\n");
        fflush(stdout);
        return 5;
    }

    TranslatorBase = OpenLibrary((CONST_STRPTR)"translator.library", 0);
    if (!TranslatorBase) {
        printf("say: cannot open translator.library\n");
        fflush(stdout);
        return 20;
    }

    terr = Translate((CONST_STRPTR)input, (LONG)len, (STRPTR)phonetic, PHON_MAX);
    if (terr != 0) {
        /* Negative return means the output buffer filled up at that
         * input offset. Speak what we got. */
        printf("say: Translate() returned %ld, speaking partial text\n", (long)terr);
        fflush(stdout);
    }

    if (phonetic[0] == '\0') {
        printf("say: translation came back empty\n");
        fflush(stdout);
        cleanup();
        return 5;
    }

    if (dry_run) {
        printf("%s\n", phonetic);
        fflush(stdout);
        cleanup();
        return 0;
    }

    port = CreateMsgPort();
    if (!port) {
        printf("say: cannot create message port\n");
        fflush(stdout);
        cleanup();
        return 20;
    }

    nrb = (struct narrator_rb *)CreateIORequest(port, sizeof(struct narrator_rb));
    if (!nrb) {
        printf("say: cannot create IO request\n");
        fflush(stdout);
        cleanup();
        return 20;
    }

    derr = OpenDevice((CONST_STRPTR)"narrator.device", 0,
                      (struct IORequest *)nrb, 0);
    if (derr != 0) {
        printf("say: cannot open narrator.device (error %ld)\n", (long)derr);
        fflush(stdout);
        cleanup();
        return 20;
    }
    dev_open = 1;

    /* Fill in the speech request. Defaults from devices/narrator.h:
     * rate 150 wpm, pitch 110 Hz, natural contours, male voice,
     * volume 64 (max), 22200 Hz sampling. */
    nrb->message.io_Command = CMD_WRITE;
    nrb->message.io_Data    = (APTR)phonetic;
    nrb->message.io_Length  = (LONG)strlen(phonetic);
    nrb->rate     = DEFRATE;
    nrb->pitch    = DEFPITCH;
    nrb->mode     = NATURALF0;
    nrb->sex      = MALE;
    nrb->ch_masks = chan_maps;
    nrb->nm_masks = sizeof(chan_maps);
    nrb->volume   = DEFVOL;
    nrb->sampfreq = DEFFREQ;
    nrb->mouths   = 0;   /* no mouth shapes needed */
    nrb->chanmask = 0;   /* filled in by the device */
    nrb->numchan  = 0;   /* filled in by the device */

    SendIO((struct IORequest *)nrb);
    io_pending = 1;
    WaitIO((struct IORequest *)nrb);
    io_pending = 0;

    if (nrb->message.io_Error != 0) {
        printf("say: narrator write failed (error %ld)\n",
               (long)nrb->message.io_Error);
        fflush(stdout);
        cleanup();
        return 10;
    }

    cleanup();
    return 0;
}
