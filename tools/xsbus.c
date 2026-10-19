/*
 * xsbus - how fast can the CPU move data through the X-Surf? (diagnostic)
 * Part of XSurfNet. GPL-3.0-only, see LICENSE.
 *
 * Run only while no driver uses the card: it resets the chip. Times remote
 * DMA reads and writes of the chip's 16 KB buffer with long, word and byte
 * moves (E-clock), plus a plain memory copy for comparison.
 */
#include <stdio.h>
#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>
#include <proto/timer.h>

struct ExpansionBase *ExpansionBase;
struct Device *TimerBase;

static volatile UBYTE *nic;
#define NR(r)    nic[(r) * 2]
#define NW(r, v) (nic[(r) * 2] = (v))
#define DATAW    ((volatile UWORD *)(nic + 0x20))
#define DATAL    ((volatile ULONG *)(nic + 0x20))
#define DATAB    (nic + 0x20)

#define LEN   8192          /* bytes per remote DMA */
#define LOOPS 128           /* 1 MB per test */

static ULONG efreq;

static ULONG now_us(void)
{
    struct EClockVal e;
    ReadEClock(&e);
    return (ULONG)((((unsigned long long)e.ev_hi << 32) | e.ev_lo) * 1000000ULL / efreq);
}

static void waitrdc(void)
{
    int n;
    for (n = 0; n < 100000; n++) if (NR(7) & 0x40) break;
    NW(7, 0x40);
}

static void setup(UWORD addr, UWORD len, UBYTE cmd)
{
    NW(0, 0x22);
    NW(7, 0x40);
    NW(0x0A, len); NW(0x0B, len >> 8);
    NW(0x08, addr); NW(0x09, addr >> 8);
    NW(0, cmd);
}

static void report(const char *what, ULONG us, ULONG bytes)
{
    printf("%-26s %6lu KB/s  (%lu us for %lu KB)\n", what,
           us ? (unsigned long)((bytes / 1024) * 1000000ULL / us) : 0UL,
           (unsigned long)us, (unsigned long)(bytes / 1024));
}

int main(void)
{
    struct ConfigDev *cd;
    struct timerequest tr;
    ULONG *buf, *buf2, t0, t1, i, k;

    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37))) return 20;
    cd = FindConfigDev(NULL, 4626, 23);
    CloseLibrary((struct Library *)ExpansionBase);
    if (!cd) { printf("no X-Surf\n"); return 20; }
    nic = (volatile UBYTE *)cd->cd_BoardAddr + 0x8000 + 0x300 * 2;
    if (OpenDevice("timer.device", UNIT_ECLOCK, (struct IORequest *)&tr, 0)) return 20;
    TimerBase = tr.tr_node.io_Device;
    { struct EClockVal e; efreq = ReadEClock(&e); }
    buf = AllocMem(LEN, MEMF_FAST | MEMF_CLEAR);
    buf2 = AllocMem(LEN, MEMF_FAST | MEMF_CLEAR);
    if (!buf || !buf2) { printf("no fast memory, using any\n");
        if (buf) FreeMem(buf, LEN); if (buf2) FreeMem(buf2, LEN);
        buf = AllocMem(LEN, MEMF_CLEAR); buf2 = AllocMem(LEN, MEMF_CLEAR); }
    printf("X-Surf at $%08lx, E-clock %lu Hz, buffers at $%08lx\n",
           (unsigned long)cd->cd_BoardAddr, (unsigned long)efreq, (unsigned long)buf);

    /* reset, 8-bit write + read first (real X-Surf quirk), then 16-bit */
    NW(0, 0x21);
    NW(0x1F, NR(0x1F));
    for (i = 0; i < 5000 && !(NR(7) & 0x80); i++) ;
    NW(7, 0xFF); NW(0x0F, 0);
    NW(0x0C, 0x20); NW(0x0D, 0x02); NW(0x01, 0x46); NW(0x02, 0x80);
    NW(0x0E, 0x48);
    setup(0x4000, 32, 0x12); for (i = 0; i < 32; i++) *DATAB = i; waitrdc();
    setup(0x4000, 32, 0x0A); for (i = 0; i < 32; i++) (void)*DATAB; waitrdc();
    NW(0x0E, 0x49);

    Forbid();
    t0 = now_us();
    for (k = 0; k < LOOPS; k++) {
        ULONG *l = buf; UWORD n = LEN / 4;
        setup(0x4000, LEN, 0x0A);
        while (n >= 8) { l[0] = *DATAL; l[1] = *DATAL; l[2] = *DATAL; l[3] = *DATAL;
                         l[4] = *DATAL; l[5] = *DATAL; l[6] = *DATAL; l[7] = *DATAL; l += 8; n -= 8; }
        waitrdc();
    }
    t1 = now_us();
    Permit();
    report("16-bit read, long moves", t1 - t0, LEN * LOOPS);

    Forbid();
    t0 = now_us();
    for (k = 0; k < LOOPS; k++) {
        ULONG *l = buf; UWORD n = LEN / 4;
        setup(0x4000, LEN, 0x12);
        while (n >= 8) { *DATAL = l[0]; *DATAL = l[1]; *DATAL = l[2]; *DATAL = l[3];
                         *DATAL = l[4]; *DATAL = l[5]; *DATAL = l[6]; *DATAL = l[7]; l += 8; n -= 8; }
        waitrdc();
    }
    t1 = now_us();
    Permit();
    report("16-bit write, long moves", t1 - t0, LEN * LOOPS);

    Forbid();
    t0 = now_us();
    for (k = 0; k < LOOPS; k++) {
        UWORD *w = (UWORD *)buf; UWORD n = LEN / 2;
        setup(0x4000, LEN, 0x0A);
        while (n--) *w++ = *DATAW;
        waitrdc();
    }
    t1 = now_us();
    Permit();
    report("16-bit read, word moves", t1 - t0, LEN * LOOPS);

    NW(0x0E, 0x48);
    Forbid();
    t0 = now_us();
    for (k = 0; k < LOOPS / 4; k++) {
        UBYTE *b = (UBYTE *)buf; UWORD n = LEN;
        setup(0x4000, LEN, 0x0A);
        while (n--) *b++ = *DATAB;
        waitrdc();
    }
    t1 = now_us();
    Permit();
    report("8-bit read, byte moves", t1 - t0, LEN * LOOPS / 4);

    /* register access cost: one ISA read */
    Forbid();
    t0 = now_us();
    for (k = 0; k < 100000; k++) (void)NR(7);
    t1 = now_us();
    Permit();
    printf("%-26s %6lu ns each\n", "ISA register read", (unsigned long)((t1 - t0) * 10 / 1000));

    Forbid();
    t0 = now_us();
    for (k = 0; k < LOOPS; k++) CopyMemQuick(buf, buf2, LEN);
    t1 = now_us();
    Permit();
    report("memory copy (CopyMemQuick)", t1 - t0, LEN * LOOPS);

    NW(0, 0x21);
    NW(0x0E, 0x48);
    FreeMem(buf, LEN); FreeMem(buf2, LEN);
    CloseDevice((struct IORequest *)&tr);
    return 0;
}
