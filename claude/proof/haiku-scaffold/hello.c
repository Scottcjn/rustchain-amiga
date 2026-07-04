#include <stdio.h>
#include <dos/dos.h>
#include <clib/dos_protos.h>

int main(void)
{
    struct MsgPort *port;
    BPTR fh;

    port = (struct MsgPort *)DeviceProc("*");
    fh = DeviceProc("*")->mp_SigTask;

    printf("Hello, AmigaOS!\n");

    return 0;
}
