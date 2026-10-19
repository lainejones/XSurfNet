/*
 * xsword - how does the X-Surf's RTL8019AS take 16-bit data? (diagnostic)
 * Part of XSurfNet. GPL-3.0-only, see LICENSE.
 *
 * Run only while no driver uses the card: it resets the chip. Writes a
 * pattern to buffer RAM with word and long moves in 16-bit mode and reads it
 * back in 16-bit and 8-bit mode, printing every result.
 */
#include <stdio.h>
#include <exec/types.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/expansion.h>

struct ExpansionBase *ExpansionBase;

static volatile UBYTE *nic;
#define NR(r)    nic[(r) * 2]
#define NW(r, v) (nic[(r) * 2] = (v))
#define DATA     0x10

static UBYTE pat[64] __attribute__((aligned(4)));

static void waitrdc(const char *what)
{
    int n;
    for (n = 0; n < 20000; n++)
        if (NR(7) & 0x40) { NW(7, 0x40); return; }
    printf("  (%s: no RDC, ISR %02x)\n", what, NR(7));
    NW(7, 0x40);
}

static void setup(UBYTE dcr, UWORD addr, UWORD len, UBYTE cmd)
{
    NW(0, 0x22);
    NW(0x0E, dcr);
    NW(7, 0x40);
    NW(0x0A, len); NW(0x0B, len >> 8);
    NW(0x08, addr); NW(0x09, addr >> 8);
    NW(0, cmd);
}

static void show(const char *what, const UBYTE *b)
{
    int i, ok = 1, sw = 1;
    for (i = 0; i < 64; i++) { if (b[i] != pat[i]) ok = 0; if (b[i] != pat[i ^ 1]) sw = 0; }
    printf("%-34s %s:", what, ok ? "SAME" : sw ? "SWAP" : "DIFF");
    for (i = 0; i < 16; i++) printf(" %02x", b[i]);
    printf("\n");
}

static void wr_word(UBYTE dcr, UWORD addr) { int i; setup(dcr, addr, 64, 0x12); for (i = 0; i < 32; i++) *(volatile UWORD *)(nic + DATA * 2) = ((UWORD *)pat)[i]; waitrdc("write"); }
static void wr_long(UBYTE dcr, UWORD addr) { int i; setup(dcr, addr, 64, 0x12); for (i = 0; i < 16; i++) *(volatile ULONG *)(nic + DATA * 2) = ((ULONG *)pat)[i]; waitrdc("write"); }
static void wr_byte(UWORD addr) { int i; setup(0x48, addr, 64, 0x12); for (i = 0; i < 64; i++) nic[DATA * 2] = pat[i]; waitrdc("write"); }
static void rd_word(UBYTE dcr, UWORD addr, UBYTE *b) { int i; setup(dcr, addr, 64, 0x0A); for (i = 0; i < 32; i++) ((UWORD *)b)[i] = *(volatile UWORD *)(nic + DATA * 2); waitrdc("read"); }
static void rd_long(UBYTE dcr, UWORD addr, UBYTE *b) { int i; setup(dcr, addr, 64, 0x0A); for (i = 0; i < 16; i++) ((ULONG *)b)[i] = *(volatile ULONG *)(nic + DATA * 2); waitrdc("read"); }
static void rd_byte(UWORD addr, UBYTE *b) { int i; setup(0x48, addr, 64, 0x0A); for (i = 0; i < 64; i++) b[i] = nic[DATA * 2]; waitrdc("read"); }

int main(void)
{
    struct ConfigDev *cd;
    UBYTE b[64] __attribute__((aligned(4)));
    int i;
    UWORD a = 0x4000;

    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37))) return 20;
    cd = FindConfigDev(NULL, 4626, 23);
    CloseLibrary((struct Library *)ExpansionBase);
    if (!cd) { printf("no X-Surf\n"); return 20; }
    nic = (volatile UBYTE *)cd->cd_BoardAddr + 0x8000 + 0x300 * 2;
    for (i = 0; i < 64; i++) pat[i] = 0x10 + i * 3;

    NW(0, 0x21);
    printf("ID %02x %02x\n", NR(0x0A), NR(0x0B));
    NW(0x1F, NR(0x1F));                     /* reset */
    for (i = 0; i < 5000 && !(NR(7) & 0x80); i++) ;
    NW(7, 0xFF);

    /* replay of xsurfnet.device 1.0's start: PROM read in byte mode with the
     * chip stopped, then a word write with the chip stopped, byte read back */
    {
        UBYTE prom[32];
        NW(0, 0x21); NW(0x0E, 0x48);
        NW(0x0A, 32); NW(0x0B, 0); NW(0x08, 0); NW(0x09, 0);
        NW(0, 0x0A);
        for (i = 0; i < 32; i++) prom[i] = nic[DATA * 2];
        waitrdc("prom");
        printf("PROM %02x %02x %02x %02x %02x %02x %02x %02x\n", prom[0], prom[1], prom[2], prom[3], prom[4], prom[5], prom[6], prom[7]);
        NW(0, 0x21); NW(0x0E, 0x49);
        NW(0x0C, 0x20); NW(0x0D, 0x02); NW(0x01, 0x46); NW(0x02, 0x80);
        NW(7, 0x40);
        NW(0x0A, 64); NW(0x0B, 0); NW(0x08, 0); NW(0x09, 0x40);
        NW(0, 0x12);
        for (i = 0; i < 32; i++) *(volatile UWORD *)(nic + DATA * 2) = ((UWORD *)pat)[i];
        waitrdc("drv write");
        NW(0x0E, 0x48);
        NW(0x0A, 64); NW(0x0B, 0); NW(0x08, 0); NW(0x09, 0x40);
        NW(0, 0x0A);
        for (i = 0; i < 64; i++) b[i] = nic[DATA * 2];
        waitrdc("drv read");
        show("driver 1.0 sequence", b);
        rd_byte(a, b);                      show("  same data, byte read again", b);
        rd_word(0x49, a, b);                show("  same data, word read", b);
        NW(0x1F, NR(0x1F));
        for (i = 0; i < 5000 && !(NR(7) & 0x80); i++) ;
        NW(7, 0xFF);
    }

    NW(0x0C, 0x20); NW(0x0D, 0x02);         /* monitor, loopback */
    NW(0x01, 0x46); NW(0x02, 0x80);
    NW(0x0F, 0);

    wr_byte(a); rd_byte(a, b);         show("byte write, byte read", b);
    rd_word(0x49, a, b);               show("byte write, word read", b);
    rd_long(0x49, a, b);               show("byte write, long read", b);
    wr_word(0x49, a); rd_byte(a, b);   show("word write, byte read", b);
    rd_word(0x49, a, b);               show("word write, word read", b);
    wr_long(0x49, a); rd_byte(a, b);   show("long write, byte read", b);
    rd_long(0x49, a, b);               show("long write, long read", b);
    /* the same with byte-order bit BOS (DCR 0x4B) */
    wr_byte(a); rd_word(0x4B, a, b);   show("byte write, word read BOS", b);
    wr_word(0x4B, a); rd_byte(a, b);   show("word write BOS, byte read", b);
    NW(0, 0x21);
    NW(0x0E, 0x48);
    return 0;
}
