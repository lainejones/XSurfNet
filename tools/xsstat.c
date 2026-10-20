/*
 * xsstat - print xsurfnet.device's internal counters (S2_GETSPECIALSTATS)
 * and the live chip registers. Part of XSurfNet. GPL-3.0-only, see LICENSE.
 *
 *   xsstat [DEVICE=DEVS:Networks/xsurfnet.device]
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <exec/io.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "sana2.h"

static const char *names[15] = {
    "interrupts", "wakeups", "rx drains", "overruns", "tx missed", "ring corrupt",
    "IMR shadow", "online", "tx busy", "tx next", "reads queued", "cmds queued",
    "rx direct", "tx direct", "rx unwanted"
};

int main(void)
{
    LONG args[1] = { 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"DEVICE", args, NULL);
    const char *dev = args[0] ? (const char *)args[0] : "DEVS:Networks/xsurfnet.device";
    struct MsgPort *port = CreateMsgPort();
    struct IOSana2Req *io = (struct IOSana2Req *)CreateIORequest(port, sizeof(*io));
    ULONG buf[2 + 15 * 3];
    int i, rc = RETURN_FAIL;

    if (!rda || !io) goto out;
    io->ios2_BufferManagement = NULL;
    if (OpenDevice((CONST_STRPTR)dev, 0, (struct IORequest *)io, 0)) { printf("cannot open %s\n", dev); goto out; }
    memset(buf, 0, sizeof(buf));
    buf[0] = 15;
    io->ios2_Req.io_Command = S2_GETSPECIALSTATS;
    io->ios2_StatData = buf;
    DoIO((struct IORequest *)io);
    for (i = 0; i < (int)buf[1]; i++)
        printf("%-14s %lu\n", names[(buf[2 + i * 3] & 0xFF) < 15 ? (buf[2 + i * 3] & 0xFF) : 0],
               (unsigned long)buf[2 + i * 3 + 1]);
    CloseDevice((struct IORequest *)io);
    rc = RETURN_OK;
out:
    if (io) DeleteIORequest((struct IORequest *)io);
    if (port) DeleteMsgPort(port);
    if (rda) FreeArgs(rda);
    return rc;
}
