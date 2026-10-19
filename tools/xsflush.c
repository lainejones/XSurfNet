/*
 * xsflush - expunge xsurfnet.device from memory (only it), so the next open
 * loads the file from DEVS: again. Does nothing while the device is open.
 * Part of XSurfNet. GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/devices.h>
#include <proto/exec.h>

int main(void)
{
    struct Device *dev;
    int rc = 0;
    Forbid();
    dev = (struct Device *)FindName(&SysBase->DeviceList, "xsurfnet.device");
    if (!dev) {
        Permit();
        printf("xsurfnet.device is not in memory.\n");
        return 0;
    }
    if (dev->dd_Library.lib_OpenCnt) {
        Permit();
        printf("xsurfnet.device is open (%d), left alone.\n", dev->dd_Library.lib_OpenCnt);
        return 5;
    }
    RemDevice(dev);                 /* calls its Expunge */
    if (FindName(&SysBase->DeviceList, "xsurfnet.device")) rc = 5;
    Permit();
    printf(rc ? "xsurfnet.device did not go.\n" : "xsurfnet.device removed from memory.\n");
    return rc;
}
