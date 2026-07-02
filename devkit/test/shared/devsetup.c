/*
 * devsetup -- minimal boot-time assigns for the bare AROS ROM shell.
 *
 * The AROS m68k ROM boot shell has no Assign or Makedir commands, and
 * vbcc's vc frontend touches ENV:, T: and vbcc: while starting up. On a
 * volume-less boot each touch pops a "please insert volume" requester and
 * a headless run hangs forever. This program creates the directories and
 * assigns with plain dos.library calls instead.
 *
 * Built with the devkit's own vbcc (see devkit/test/README-test.md).
 * Elyan Labs 2026, MIT.
 */
#include <proto/dos.h>
#include <stdio.h>

static void mkassign(const char *dir, const char *name)
{
    BPTR l;

    l = Lock((STRPTR)dir, ACCESS_READ);
    if (!l) {
        l = CreateDir((STRPTR)dir);
        if (l) {
            UnLock(l);
            l = Lock((STRPTR)dir, ACCESS_READ);
        }
    }
    if (l) {
        if (AssignLock((STRPTR)name, l)) {
            printf("assigned %s: -> %s\n", name, dir);
        } else {
            UnLock(l);
            printf("AssignLock FAILED for %s:\n", name);
        }
    } else {
        printf("no lock on %s\n", dir);
    }
}

int main(int argc, char **argv)
{
    /* optional argument: path of the vbcc drawer (default DH1:vbcc,
     * which is where the devkit test rig mounts it; on the distro HDF
     * pass SYS:Development/vbcc instead) */
    const char *vbccdir = (argc > 1) ? argv[1] : "DH1:vbcc";

    mkassign("RAM:Env", "ENV");
    mkassign("RAM:T",   "T");
    mkassign(vbccdir,   "vbcc");
    return 0;
}
