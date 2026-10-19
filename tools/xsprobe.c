/*
 * xsprobe - find an X-Surf, configure its RTL8019AS and read its MAC address.
 * Part of XSurfNet. GPL-3.0-only, see LICENSE.
 *
 *   xsprobe [NOPNP] [DUMP] [PEEK]
 *
 * PEEK only reads the registers of a running chip (no reset, no PnP), so it
 * can look at what a driver has set up.
 *
 * The X-Surf (Individual Computers, Zorro II 4626/23) is an ISA card on a Zorro
 * carrier: board offset 0x8000 is an ISA I/O window with every ISA address
 * doubled, and bit 7 of the board register at 0x7E selects the upper 4 KB of
 * ISA space. The RTL8019AS is either already at ISA 0x300 (set up by its
 * EEPROM) or waits for ISA Plug and Play, which this tool then performs.
 */
#include <stdio.h>
#include <string.h>
#include <exec/types.h>
#include <dos/dos.h>
#include <libraries/configvars.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>

struct ExpansionBase *ExpansionBase;

#define XS_MANUF   4626
#define XS_PROD    23
#define XS_ISA     0x8000           /* ISA I/O window */
#define XS_ISAHI   0x007E           /* bit 7: upper 4 KB of ISA space */
#define NIC_BASE   0x300            /* NE2000 registers (ISA I/O) */
#define ASIC_DATA  0x10             /* remote DMA data port, from NIC_BASE */
#define ASIC_RESET 0x1F

#define PNP_ADDR   0x279
#define PNP_WDATA  0xA79
#define PNP_RDATA  0x203            /* read-data port we choose ((0x80 << 2) | 3) */

static volatile UBYTE *board;
static int hi = -1;                 /* current state of the 0x7E latch */

/* the RTL8019AS's Plug and Play initiation key (Realtek data sheet) */
static const UBYTE pnp_key[32] = {
    0xDA, 0x6D, 0x36, 0x1B, 0x8D, 0x46, 0x23, 0x91, 0x48, 0xA4, 0xD2, 0x69, 0x34, 0x9A, 0x4D, 0x26,
    0x13, 0x89, 0x44, 0xA2, 0x51, 0x28, 0x94, 0xCA, 0x65, 0x32, 0x19, 0x0C, 0x86, 0x43, 0xA1, 0x50
};

static volatile UBYTE *isa(UWORD port)
{
    ULONG off = (ULONG)port * 2;
    int want = off >= 0x1000;
    if (want != hi) {
        board[XS_ISAHI] = want ? 0x80 : 0x00;
        hi = want;
    }
    return board + XS_ISA + (off & 0x0FFF);
}
static void  outb(UWORD port, UBYTE v) { *isa(port) = v; }
static UBYTE inb(UWORD port)           { return *isa(port); }

static void pnp_reg(UBYTE reg, UBYTE v) { outb(PNP_ADDR, reg); outb(PNP_WDATA, v); }

static void wait_us(ULONG us)       /* a few CIA reads ~ 1.4 us each */
{
    volatile UBYTE *cia = (volatile UBYTE *)0xBFE001;
    ULONG n = us + 1;
    while (n--) (void)*cia;
}

/* Plug and Play: key, isolate the one card, CSN 1, I/O 0x300, activate */
static int pnp_configure(UBYTE id[9])
{
    int b, i;
    outb(PNP_ADDR, 0); outb(PNP_ADDR, 0);            /* restart the key sequence */
    for (i = 0; i < 32; i++) outb(PNP_ADDR, pnp_key[i]);
    pnp_reg(0x02, 0x04);                             /* reset CSNs */
    pnp_reg(0x03, 0x00);                             /* wake[0]: isolation */
    pnp_reg(0x00, PNP_RDATA >> 2);                   /* set the read-data port */
    outb(PNP_ADDR, 0x01);                            /* serial isolation */
    wait_us(1000);
    memset(id, 0, 9);
    for (b = 0; b < 72; b++) {
        UBYTE r1 = inb(PNP_RDATA), r2;
        wait_us(250);
        r2 = inb(PNP_RDATA);
        wait_us(250);
        if (r1 == 0x55 && r2 == 0xAA) id[b >> 3] |= 1 << (b & 7);
    }
    if (!id[0] && !id[1] && !id[2] && !id[3]) return 0;  /* nobody answered */
    pnp_reg(0x06, 0x01);                             /* CSN 1 */
    pnp_reg(0x07, 0x00);                             /* logical device 0 */
    pnp_reg(0x60, NIC_BASE >> 8);
    pnp_reg(0x61, NIC_BASE & 0xFF);
    pnp_reg(0x30, 0x01);                             /* activate */
    pnp_reg(0x02, 0x02);                             /* back to wait-for-key */
    return 1;
}

/* NE2000 at NIC_BASE? page-0 register 0x0A/0x0B read "Pp" on an RTL8019AS */
static int nic_present(void)
{
    outb(NIC_BASE + 0x00, 0x21);                     /* page 0, stop, abort DMA */
    return inb(NIC_BASE + 0x0A) == 0x50 && inb(NIC_BASE + 0x0B) == 0x70;
}

static int nic_reset(void)
{
    int n;
    outb(NIC_BASE + ASIC_RESET, inb(NIC_BASE + ASIC_RESET));
    for (n = 0; n < 5000; n++) {
        if (inb(NIC_BASE + 0x07) & 0x80) break;      /* ISR: RST */
        wait_us(10);
    }
    outb(NIC_BASE + 0x07, 0xFF);                     /* clear ISR */
    return n < 5000;
}

/* read 32 bytes of the station PROM with remote DMA, byte mode */
static void read_prom(UBYTE *mac)
{
    UBYTE prom[32];
    int i;
    outb(NIC_BASE + 0x00, 0x21);
    outb(NIC_BASE + 0x0E, 0x48);                     /* DCR: byte, FIFO 8 */
    outb(NIC_BASE + 0x0A, 32); outb(NIC_BASE + 0x0B, 0);     /* RBCR */
    outb(NIC_BASE + 0x08, 0);  outb(NIC_BASE + 0x09, 0);     /* RSAR 0 */
    outb(NIC_BASE + 0x00, 0x0A);                     /* remote read */
    for (i = 0; i < 32; i++) prom[i] = inb(NIC_BASE + ASIC_DATA);
    outb(NIC_BASE + 0x07, 0x40);                     /* clear RDC */
    for (i = 0; i < 6; i++) mac[i] = prom[i * 2];    /* every byte doubled */
}

int main(void)
{
    LONG args[3] = { 0, 0, 0 };
    struct RDArgs *rda = ReadArgs((CONST_STRPTR)"NOPNP/S,DUMP/S,PEEK/S", args, NULL);
    struct ConfigDev *cd;
    UBYTE id[9], mac[6];
    int i, rc = RETURN_FAIL;

    if (!rda) { PrintFault(IoErr(), (CONST_STRPTR)"xsprobe"); return RETURN_FAIL; }
    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary((CONST_STRPTR)"expansion.library", 37))) return RETURN_FAIL;
    cd = FindConfigDev(NULL, XS_MANUF, XS_PROD);
    if (!cd) { printf("No X-Surf (Zorro 4626/23) found.\n"); goto out; }
    board = (volatile UBYTE *)cd->cd_BoardAddr;
    printf("X-Surf at $%08lx, %lu KB, serial %08lx\n", (unsigned long)board,
           (unsigned long)(cd->cd_BoardSize >> 10), (unsigned long)cd->cd_Rom.er_SerialNumber);

    if (args[2]) {
        UBYTE cr = inb(NIC_BASE);
        int pg;
        printf("CR %02x\n", cr);
        for (pg = 0; pg < 4; pg++) {
            Disable();
            outb(NIC_BASE, (cr & 0x3F) | 0x20 | (pg << 6));
            {
                UBYTE r[16];
                for (i = 1; i < 16; i++) r[i] = inb(NIC_BASE + i);
                outb(NIC_BASE, (cr & 0x3F) | 0x20);
                Enable();
                printf("page %d:", pg);
                for (i = 1; i < 16; i++) printf(" %02x", r[i]);
                printf("\n");
            }
        }
        rc = RETURN_OK;
        goto out;
    }
    if (nic_present()) {
        printf("RTL8019AS already at ISA $%03x (EEPROM / jumpers)\n", NIC_BASE);
    } else if (args[0]) {
        printf("No chip at ISA $%03x and NOPNP given.\n", NIC_BASE);
        goto out;
    } else {
        if (!pnp_configure(id)) { printf("Plug and Play: no card answered isolation.\n"); goto out; }
        printf("Plug and Play: card id");
        for (i = 0; i < 9; i++) printf(" %02x", id[i]);
        printf(" -> CSN 1, I/O $%03x, active\n", NIC_BASE);
        if (!nic_present()) { printf("...but no NE2000 answers at ISA $%03x.\n", NIC_BASE); goto out; }
    }
    if (!nic_reset()) printf("warning: no reset acknowledge (ISR.RST)\n");
    read_prom(mac);
    printf("MAC address %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (args[1]) {
        int pg;
        for (pg = 0; pg < 4; pg++) {
            outb(NIC_BASE, 0x21 | (pg << 6));
            printf("page %d:", pg);
            for (i = 1; i < 16; i++) printf(" %02x", inb(NIC_BASE + i));
            printf("\n");
        }
        outb(NIC_BASE, 0x21);
    }
    rc = RETURN_OK;
out:
    CloseLibrary((struct Library *)ExpansionBase);
    FreeArgs(rda);
    return rc;
}
