/*
 * xsurfnet.device - SANA-II driver for the Individual Computers X-Surf.
 * Part of XSurfNet. GPL-3.0-only, see LICENSE.
 *
 * One unit (0). The RTL8019AS is an NE2000 at ISA 0x300 behind the X-Surf's
 * ISA window (board + 0x8000, every ISA address doubled). A driver process
 * does all the chip work; the INT2 server only masks the chip and signals it.
 *
 * Rules that keep the chip safe without a lock:
 *  - outside Disable() the chip is always on register page 0, so the
 *    interrupt server can read ISR and write IMR at any time (neither
 *    touches CR, so a remote DMA in progress is not disturbed);
 *  - only the driver process touches anything else.
 */
#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/memory.h>
#include <exec/interrupts.h>
#include <exec/execbase.h>
#include <hardware/intbits.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <libraries/configvars.h>
#include <utility/tagitem.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/expansion.h>
#include <proto/utility.h>
#include "sana2.h"

#define REG(r, decl) register decl __asm(#r)

struct ExecBase *SysBase;
struct DosLibrary *DOSBase;
struct ExpansionBase *ExpansionBase;
struct Library *UtilityBase;

extern const char xs_devname[], xs_idstring[];
extern void xs_intstub(void);
extern ULONG xs_callcopy(APTR func, APTR to, APTR from, ULONG len);

/* ---- hardware ---------------------------------------------------------- */

#define XS_MANUF     4626
#define XS_PROD      23
#define XS_DENEB     0xC0FFEE01     /* E3B Deneb's fake X-Surf entry */
#define XS_ISA       0x8000
#define XS_ISAHI     0x007E
#define NIC_BASE     0x300
#define ASIC_DATA    0x10
#define ASIC_RESET   0x1F

/* 8390 registers, page 0 */
#define CR      0x00
#define PSTART  0x01
#define PSTOP   0x02
#define BNRY    0x03
#define TPSR    0x04
#define TBCR0   0x05
#define TBCR1   0x06
#define ISR     0x07
#define RSAR0   0x08
#define RSAR1   0x09
#define RBCR0   0x0A
#define RBCR1   0x0B
#define RCR     0x0C
#define TCR     0x0D
#define DCR     0x0E
#define IMR     0x0F
/* page 1 */
#define PAR0    0x01
#define CURR    0x07
#define MAR0    0x08

#define CR_STP   0x01
#define CR_STA   0x02
#define CR_TXP   0x04
#define CR_RREAD 0x08
#define CR_RWRITE 0x10
#define CR_NODMA 0x20
#define CR_PAGE1 0x40

#define ISR_PRX  0x01
#define ISR_PTX  0x02
#define ISR_RXE  0x04
#define ISR_TXE  0x08
#define ISR_OVW  0x10
#define ISR_CNT  0x20
#define ISR_RDC  0x40
#define ISR_RST  0x80
#define IMR_ALL  (ISR_PRX | ISR_PTX | ISR_RXE | ISR_TXE | ISR_OVW | ISR_CNT)

#define RCR_AB   0x04
#define RCR_AM   0x08
#define RCR_PRO  0x10

/* 16 KB of buffer RAM at 0x4000: one transmit buffer, then the ring */
#define TX_PAGE  0x40             /* two transmit buffers, 6 pages each */
#define TX_PAGE2 0x46
#define RX_START 0x4C
#define RX_STOP  0x80

#define ETH_HDR  14
#define ETH_MTU  1500
#define ETH_MAX  (ETH_HDR + ETH_MTU)
#define ETH_MIN  60

static volatile UBYTE *board;       /* the X-Surf's Zorro base */
static volatile UBYTE *nic;         /* board + XS_ISA + NIC_BASE * 2 */
static volatile UWORD *dataw;       /* the ASIC data port, word access */
static volatile UBYTE *datab;       /* ... and byte access */
static UBYTE wordmode;
static UBYTE swapmode;              /* word mode, but the bytes of a word arrive swapped */

#define NR(r)       nic[(r) * 2]
#define NW(r, v)    (nic[(r) * 2] = (v))

/* ---- device state ------------------------------------------------------ */

struct Opener {
    struct MinNode node;
    struct MinList readq;
    APTR copyto, copyfrom;
    struct Hook *filter;
};

struct McEntry { UBYTE addr[6]; UWORD refs; };
#define MAX_MC 32

static struct Device *xsdev;
static BPTR seglist;
static struct Unit unit;
static struct MinList openers, orphanq, eventq, cmdq;
static struct Task *proc;           /* the driver process */
static struct Task *parent;         /* who waits for it to start or stop */
static BYTE intsig = -1, cmdsig = -1;
static ULONG intmask, cmdmask;
static volatile UBYTE imr_shadow;
static struct Interrupt intr;
static UBYTE mac[6], hwmac[6];
static UBYTE online, configured, exclusive, promisc, hw_ok;
static struct IOSana2Req *txbusy;   /* frame on the wire */
static UBYTE txslot;                /* its buffer (TX_PAGE or TX_PAGE2) */
static struct IOSana2Req *txnext;   /* next frame, already in the other buffer */
static UWORD txnext_len;
static struct McEntry mc[MAX_MC];
static UBYTE mar[8];
static struct Sana2DeviceStats stats;
static ULONG n_ints, n_wakes, n_rxdrain, n_ovw, n_txmiss, n_corrupt;   /* S2_GETSPECIALSTATS */
static UBYTE txbuf[ETH_MAX + 2] __attribute__((aligned(4)));
static UBYTE rxbuf[ETH_MAX + 6] __attribute__((aligned(4)));

/* gcc may turn loops into these */
void *memcpy(void *d, const void *s, unsigned long n)
{
    UBYTE *dd = d; const UBYTE *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, unsigned long n)
{
    UBYTE *dd = d;
    while (n--) *dd++ = c;
    return d;
}

static void wait_us(ULONG us)       /* a CIA read is about 1.4 us */
{
    volatile UBYTE *cia = (volatile UBYTE *)0xBFE001;
    ULONG n = us + 1;
    while (n--) (void)*cia;
}

/* ---- ISA Plug and Play (only needed when no EEPROM set the chip up) ----- */

static int hi = -1;
static volatile UBYTE *isa(UWORD port)
{
    ULONG off = (ULONG)port * 2;
    int want = off >= 0x1000;
    if (want != hi) { board[XS_ISAHI] = want ? 0x80 : 0x00; hi = want; }
    return board + XS_ISA + (off & 0x0FFF);
}
#define PNP_ADDR   0x279
#define PNP_WDATA  0xA79
#define PNP_RDATA  0x203

static const UBYTE pnp_key[32] = {
    0xDA, 0x6D, 0x36, 0x1B, 0x8D, 0x46, 0x23, 0x91, 0x48, 0xA4, 0xD2, 0x69, 0x34, 0x9A, 0x4D, 0x26,
    0x13, 0x89, 0x44, 0xA2, 0x51, 0x28, 0x94, 0xCA, 0x65, 0x32, 0x19, 0x0C, 0x86, 0x43, 0xA1, 0x50
};

static void pnp_reg(UBYTE reg, UBYTE v) { *isa(PNP_ADDR) = reg; *isa(PNP_WDATA) = v; }

static int pnp_configure(void)
{
    int b, i, any = 0;
    *isa(PNP_ADDR) = 0; *isa(PNP_ADDR) = 0;
    for (i = 0; i < 32; i++) *isa(PNP_ADDR) = pnp_key[i];
    pnp_reg(0x02, 0x04);
    pnp_reg(0x03, 0x00);
    pnp_reg(0x00, PNP_RDATA >> 2);
    *isa(PNP_ADDR) = 0x01;
    wait_us(1000);
    for (b = 0; b < 72; b++) {
        UBYTE r1 = *isa(PNP_RDATA), r2;
        wait_us(250);
        r2 = *isa(PNP_RDATA);
        wait_us(250);
        if (r1 == 0x55 && r2 == 0xAA) any = 1;
    }
    if (!any) { isa(0); return 0; }
    pnp_reg(0x06, 0x01);
    pnp_reg(0x07, 0x00);
    pnp_reg(0x60, NIC_BASE >> 8);
    pnp_reg(0x61, NIC_BASE & 0xFF);
    pnp_reg(0x30, 0x01);
    pnp_reg(0x02, 0x02);
    isa(0);                         /* latch back to the low 4 KB */
    return 1;
}

/* ---- 8390 basics -------------------------------------------------------- */

static int nic_present(void)
{
    NW(CR, CR_NODMA | CR_STP);
    return NR(RBCR0) == 0x50 && NR(RBCR1) == 0x70;   /* RTL8019AS "Pp" */
}

static int nic_reset(void)
{
    int n;
    NW(ASIC_RESET, NR(ASIC_RESET));
    for (n = 0; n < 5000; n++) {
        if (NR(ISR) & ISR_RST) break;
        wait_us(10);
    }
    NW(ISR, 0xFF);
    return n < 5000;
}

static int rdma_wait(void)
{
    int n;
    for (n = 0; n < 20000; n++)
        if (NR(ISR) & ISR_RDC) { NW(ISR, ISR_RDC); return 1; }
    NW(ISR, ISR_RDC);
    return 0;
}

static UBYTE dcr_value = 0x48;     /* DCR for normal work: 0x48 8-bit, 0x49 16-bit, 0x4B 16-bit + BOS */

/* program a remote DMA: like the X-Surf's own driver, complete/abort any
 * DMA first (CR 0x22 keeps the chip's run state otherwise unchanged) */
static void rdma_setup(UWORD addr, UWORD len, UBYTE cmd)
{
    NW(CR, CR_NODMA | CR_STA);
    NW(ISR, ISR_RDC);
    NW(RBCR0, len); NW(RBCR1, len >> 8);
    NW(RSAR0, addr); NW(RSAR1, addr >> 8);
    NW(CR, cmd | CR_STA);
}

/* 16-bit transfers move longs (two bus cycles each), so counts are rounded
 * up to 4; every caller's buffer has the slack */
static void rdma_read(UWORD addr, UBYTE *buf, UWORD len)
{
    if (wordmode) len = (len + 3) & ~3;
    rdma_setup(addr, len, CR_RREAD);
    if (wordmode && !swapmode) {
        volatile ULONG *d = (volatile ULONG *)dataw;
        ULONG *l = (ULONG *)buf;
        UWORD n = len >> 2;
        while (n >= 8) {
            l[0] = *d; l[1] = *d; l[2] = *d; l[3] = *d;
            l[4] = *d; l[5] = *d; l[6] = *d; l[7] = *d;
            l += 8; n -= 8;
        }
        while (n--) *l++ = *d;
    } else if (wordmode) {
        UWORD *w = (UWORD *)buf;
        UWORD n = len >> 1;
        while (n--) { UWORD v = *dataw; *w++ = (v << 8) | (v >> 8); }
    } else {
        while (len--) *buf++ = *datab;
    }
    rdma_wait();
}

static int rdma_write(UWORD addr, const UBYTE *buf, UWORD len)
{
    if (wordmode) len = (len + 3) & ~3;
    rdma_setup(addr, len, CR_RWRITE);
    if (wordmode && !swapmode) {
        volatile ULONG *d = (volatile ULONG *)dataw;
        const ULONG *l = (const ULONG *)buf;
        UWORD n = len >> 2;
        while (n >= 8) {
            *d = l[0]; *d = l[1]; *d = l[2]; *d = l[3];
            *d = l[4]; *d = l[5]; *d = l[6]; *d = l[7];
            l += 8; n -= 8;
        }
        while (n--) *d = *l++;
    } else if (wordmode) {
        const UWORD *w = (const UWORD *)buf;
        UWORD n = len >> 1;
        while (n--) { UWORD v = *w++; *dataw = (v << 8) | (v >> 8); }
    } else {
        while (len--) *datab = *buf++;
    }
    return rdma_wait();
}

/* Write a pattern with 16-bit moves (DCR dcr), read it back with 8-bit
 * moves: 1 = arrives straight, 2 = bytes swapped in each word, 0 = neither.
 * Only valid straight after a chip reset: on a real X-Surf, once an 8-bit
 * transfer (the PROM read) has run, mixing 8- and 16-bit transfers returns
 * every second byte wrong. */
static char busdiag[80];
static int busdiag_len;
static void diag_add(const char *t)
{
    while (*t && busdiag_len < (int)sizeof(busdiag) - 1) busdiag[busdiag_len++] = *t++;
    busdiag[busdiag_len] = 0;
}
static void diag_hex(UBYTE v)
{
    static const char hx[] = "0123456789abcdef";
    char t[4];
    t[0] = ' '; t[1] = hx[v >> 4]; t[2] = hx[v & 15]; t[3] = 0;
    diag_add(t);
}

static int bus_try(UBYTE dcr)
{
    static UBYTE pat[32] __attribute__((aligned(4)));
    UBYTE back[32] __attribute__((aligned(4)));
    int i, straight = 1, swapped = 1;
    for (i = 0; i < 32; i++) pat[i] = 0x5A ^ (i * 7);
    NW(CR, CR_NODMA | CR_STP);
    NW(RCR, 0x20);                  /* monitor */
    NW(TCR, 0x02);                  /* loopback */
    NW(PSTART, RX_START); NW(PSTOP, RX_STOP);
    /* An 8-bit write and read first: on a real X-Surf, straight after a
     * reset, 8-bit reads return every second byte wrong until an 8-bit write
     * has been done; after that they read back correctly. */
    NW(DCR, 0x48);
    wordmode = 0; swapmode = 0;
    rdma_write(TX_PAGE << 8, pat, 32);
    rdma_read(TX_PAGE << 8, back, 32);
    NW(DCR, dcr);
    wordmode = 1;
    i = rdma_write(TX_PAGE << 8, pat, 32);
    diag_add(" [");
    diag_hex(dcr);
    diag_add(i ? " rdc" : " nordc");
    NW(DCR, 0x48);
    wordmode = 0;
    rdma_read(TX_PAGE << 8, back, 32);
    for (i = 0; i < 6; i++) diag_hex(back[i]);
    diag_add("]");
    for (i = 0; i < 32; i++) {
        if (back[i] != pat[i]) straight = 0;
        if (back[i] != pat[i ^ 1]) swapped = 0;
    }
    return straight ? 1 : swapped ? 2 : 0;
}

/* pick the bus mode, then read the station address in that mode */
static void setup_bus_and_mac(void)
{
    UBYTE prom[32] __attribute__((aligned(4)));
    int r, i;

    nic_reset();
    r = bus_try(0x49);
    if (r == 2) {                   /* swapped: does the chip's byte-order bit fix it? */
        nic_reset();
        if (bus_try(0x4B) == 1) r = 3;
    }
    nic_reset();
    switch (r) {
    case 1:  dcr_value = 0x49; wordmode = 1; swapmode = 0; break;
    case 2:  dcr_value = 0x49; wordmode = 1; swapmode = 1; break;
    case 3:  dcr_value = 0x4B; wordmode = 1; swapmode = 0; break;
    default: dcr_value = 0x48; wordmode = 0; swapmode = 0; break;
    }
    NW(CR, CR_NODMA | CR_STP);
    NW(RCR, 0x20);                  /* monitor, loopback while we look */
    NW(TCR, 0x02);
    NW(DCR, dcr_value);
    /* the PROM holds each address byte twice (one 16-bit word per byte), so
     * byte i is prom[2i] in either mode and whatever the byte order */
    rdma_read(0, prom, 32);
    for (i = 0; i < 6; i++) hwmac[i] = prom[i * 2];
    NW(CR, CR_NODMA | CR_STP);
}

static ULONG crc32_be(const UBYTE *a)
{
    ULONG crc = 0xFFFFFFFF;
    int i, b;
    for (i = 0; i < 6; i++) {
        UBYTE c = a[i];
        for (b = 0; b < 8; b++) {
            ULONG carry = ((crc & 0x80000000) ? 1 : 0) ^ (c & 1);
            c >>= 1;
            crc <<= 1;
            if (carry) crc = (crc ^ 0x04C11DB6) | carry;
        }
    }
    return crc;
}

static void build_mar(void)
{
    int i;
    memset(mar, 0, 8);
    for (i = 0; i < MAX_MC; i++)
        if (mc[i].refs) {
            UBYTE h = crc32_be(mc[i].addr) >> 26;
            mar[h >> 3] |= 1 << (h & 7);
        }
}

static UBYTE rcr_value(void)
{
    return RCR_AB | RCR_AM | (promisc ? RCR_PRO : 0);
}

static void load_page1(UBYTE curr)
{
    int i;
    Disable();
    NW(CR, CR_PAGE1 | CR_NODMA | (online ? CR_STA : CR_STP));
    for (i = 0; i < 6; i++) NW(PAR0 + i, mac[i]);
    for (i = 0; i < 8; i++) NW(MAR0 + i, mar[i]);
    if (curr) NW(CURR, curr);
    NW(CR, CR_NODMA | (online ? CR_STA : CR_STP));
    Enable();
}

static void enable_ints(void)
{
    Disable();
    imr_shadow = IMR_ALL;
    NW(IMR, IMR_ALL);
    Enable();
}

static void disable_ints(void)
{
    Disable();
    imr_shadow = 0;
    NW(IMR, 0);
    Enable();
}

/* (re)start the chip with the current address, filter and an empty ring */
static void nic_start(void)
{
    disable_ints();
    NW(CR, CR_NODMA | CR_STP);
    NW(DCR, dcr_value);
    NW(RBCR0, 0); NW(RBCR1, 0);
    NW(RCR, 0x20);
    NW(TCR, 0x02);
    NW(PSTART, RX_START); NW(PSTOP, RX_STOP);
    NW(BNRY, RX_START);
    NW(ISR, 0xFF);
    online = 0;
    load_page1(RX_START + 1);
    NW(CR, CR_NODMA | CR_STA);
    NW(TCR, 0x00);
    NW(RCR, rcr_value());
    online = 1;
    enable_ints();
}

static void nic_stop(void)
{
    disable_ints();
    NW(CR, CR_NODMA | CR_STP);
    NW(ISR, 0xFF);
}

/* ---- request plumbing --------------------------------------------------- */

static void reply(struct IOSana2Req *ior)
{
    if (!(ior->ios2_Req.io_Flags & IOF_QUICK))
        ReplyMsg((struct Message *)ior);
}

static void fail(struct IOSana2Req *ior, BYTE err, ULONG wire)
{
    ior->ios2_Req.io_Error = err;
    ior->ios2_WireError = wire;
    reply(ior);
}

static void fire_events(ULONG what)
{
    struct IOSana2Req *ior, *next;
    Forbid();
    for (ior = (struct IOSana2Req *)eventq.mlh_Head;
         (next = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ); ior = next)
        if (ior->ios2_WireError & what) {
            Remove((struct Node *)ior);
            ior->ios2_WireError &= what;
            ReplyMsg((struct Message *)ior);
        }
    Permit();
}

static void flush_list(struct MinList *l)
{
    struct IOSana2Req *ior;
    while ((ior = (struct IOSana2Req *)RemHead((struct List *)l))) {
        ior->ios2_Req.io_Error = IOERR_ABORTED;
        ReplyMsg((struct Message *)ior);
    }
}

static void init_list(struct MinList *l)
{
    l->mlh_Head = (struct MinNode *)&l->mlh_Tail;
    l->mlh_Tail = NULL;
    l->mlh_TailPred = (struct MinNode *)&l->mlh_Head;
}

static int is_mc_listed(const UBYTE *a)
{
    int i, j;
    for (i = 0; i < MAX_MC; i++)
        if (mc[i].refs) {
            for (j = 0; j < 6 && mc[i].addr[j] == a[j]; j++) ;
            if (j == 6) return 1;
        }
    return 0;
}

/* ---- receive ------------------------------------------------------------ */

static void deliver(struct IOSana2Req *ior, struct Opener *o, UWORD len, UWORD type, int bc, int mcast)
{
    int raw = ior->ios2_Req.io_Flags & SANA2IOF_RAW;
    UBYTE *from = raw ? rxbuf : rxbuf + ETH_HDR;
    ULONG n = raw ? len : len - ETH_HDR;
    int i;
    for (i = 0; i < 6; i++) {
        ior->ios2_DstAddr[i] = rxbuf[i];
        ior->ios2_SrcAddr[i] = rxbuf[6 + i];
    }
    ior->ios2_PacketType = type;
    ior->ios2_DataLength = n;
    ior->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);
    if (bc) ior->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    else if (mcast) ior->ios2_Req.io_Flags |= SANA2IOF_MCAST;
    ior->ios2_Req.io_Error = 0;
    ior->ios2_WireError = 0;
    if (!xs_callcopy(o->copyto, ior->ios2_Data, from, n)) {
        ior->ios2_Req.io_Error = S2ERR_NO_RESOURCES;
        ior->ios2_WireError = S2WERR_BUFF_ERROR;
    }
    ReplyMsg((struct Message *)ior);
}

static void dispatch(UWORD len)
{
    struct Opener *o;
    struct IOSana2Req *ior;
    UWORD type = (rxbuf[12] << 8) | rxbuf[13];
    int bc = (rxbuf[0] & rxbuf[1] & rxbuf[2] & rxbuf[3] & rxbuf[4] & rxbuf[5]) == 0xFF;
    int mcast = !bc && (rxbuf[0] & 1);
    int taken = 0;

    if (mcast && !promisc && !is_mc_listed(rxbuf)) return;   /* a hash neighbour */
    stats.PacketsReceived++;

    Forbid();
    for (o = (struct Opener *)openers.mlh_Head; o->node.mln_Succ; o = (struct Opener *)o->node.mln_Succ) {
        for (ior = (struct IOSana2Req *)o->readq.mlh_Head; ior->ios2_Req.io_Message.mn_Node.ln_Succ;
             ior = (struct IOSana2Req *)ior->ios2_Req.io_Message.mn_Node.ln_Succ) {
            if (ior->ios2_PacketType != type) continue;
            if (o->filter && !CallHookPkt(o->filter, ior, rxbuf + ETH_HDR)) break;
            Remove((struct Node *)ior);
            deliver(ior, o, len, type, bc, mcast);
            taken = 1;
            break;
        }
    }
    if (!taken) {
        stats.UnknownTypesReceived++;
        if ((ior = (struct IOSana2Req *)RemHead((struct List *)&orphanq)))
            deliver(ior, (struct Opener *)ior->ios2_BufferManagement, len, type, bc, mcast);
    }
    Permit();
}

static UBYTE read_curr(void)
{
    UBYTE c;
    Disable();
    NW(CR, CR_PAGE1 | CR_NODMA | CR_STA);
    c = NR(CURR);
    NW(CR, CR_NODMA | CR_STA);
    Enable();
    return c;
}

/* returns 0 if the ring looked corrupt (the caller restarts the chip) */
static int rx_drain(void)
{
    UBYTE hdr[4] __attribute__((aligned(2)));
    UBYTE curr = read_curr();
    for (;;) {
        UBYTE bnry, page, next;
        UWORD count, len, first;
        bnry = NR(BNRY);
        page = bnry + 1;
        if (page >= RX_STOP) page = RX_START;
        if (page == curr && (curr = read_curr()) == page) return 1;

        rdma_read(page << 8, hdr, 4);
        next = hdr[1];
        count = hdr[2] | (hdr[3] << 8);
        if (next < RX_START || next >= RX_STOP || count < ETH_MIN + 4 || count > ETH_MAX + 4) {
            stats.BadData++;
            return 0;
        }
        len = count - 4;            /* drop the CRC */
        if (!(hdr[0] & 0x01)) {     /* not "received intact" */
            stats.BadData++;
        } else {
            UWORD addr = (page << 8) + 4;
            first = (RX_STOP << 8) - addr;
            if (len > first) {      /* wraps round the ring */
                rdma_read(addr, rxbuf, first);
                rdma_read(RX_START << 8, rxbuf + first, len - first);
            } else {
                rdma_read(addr, rxbuf, len);
            }
            dispatch(len);
        }
        bnry = next - 1;
        if (bnry < RX_START) bnry = RX_STOP - 1;
        NW(BNRY, bnry);
    }
}

/* ---- transmit ----------------------------------------------------------- */

/* copy a frame into the transmit buffer at page slot: its length, or 0 if
 * the request failed (and was replied) */
static UWORD tx_prepare(struct IOSana2Req *ior, UBYTE slot)
{
    struct Opener *o = (struct Opener *)ior->ios2_BufferManagement;
    ULONG n = ior->ios2_DataLength;
    UWORD total;
    int i;

    if (ior->ios2_Req.io_Flags & SANA2IOF_RAW) {
        if (n > ETH_MAX || n < ETH_HDR) { fail(ior, S2ERR_MTU_EXCEEDED, 0); return 0; }
        if (!xs_callcopy(o->copyfrom, txbuf, ior->ios2_Data, n)) {
            fail(ior, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR); return 0;
        }
        total = n;
    } else {
        if (n > ETH_MTU) { fail(ior, S2ERR_MTU_EXCEEDED, 0); return 0; }
        for (i = 0; i < 6; i++) {
            txbuf[i] = (ior->ios2_Req.io_Command == S2_BROADCAST) ? 0xFF : ior->ios2_DstAddr[i];
            txbuf[6 + i] = mac[i];
        }
        txbuf[12] = ior->ios2_PacketType >> 8;
        txbuf[13] = ior->ios2_PacketType;
        if (!xs_callcopy(o->copyfrom, txbuf + ETH_HDR, ior->ios2_Data, n)) {
            fail(ior, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR); return 0;
        }
        total = n + ETH_HDR;
    }
    while (total < ETH_MIN) txbuf[total++] = 0;
    if (!rdma_write(slot << 8, txbuf, total)) {
        fail(ior, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR); return 0;
    }
    return total;
}

static void tx_fire(struct IOSana2Req *ior, UBYTE slot, UWORD total)
{
    NW(ISR, ISR_PTX | ISR_TXE);     /* no stale completion for this frame */
    NW(TPSR, slot);
    NW(TBCR0, total); NW(TBCR1, total >> 8);
    NW(CR, CR_NODMA | CR_TXP | CR_STA);
    txbusy = ior;
    txslot = slot;
}

/* a write from the queue: onto the wire if it is free, else into the other
 * buffer so it can go the moment the current frame is out. 1 = must wait */
static int tx_start(struct IOSana2Req *ior)
{
    UWORD total;
    if (!txbusy) {
        UBYTE slot = (txslot == TX_PAGE) ? TX_PAGE2 : TX_PAGE;
        if ((total = tx_prepare(ior, slot))) tx_fire(ior, slot, total);
        return 0;
    }
    if (txnext) return 1;
    if ((total = tx_prepare(ior, txslot == TX_PAGE ? TX_PAGE2 : TX_PAGE))) {
        txnext = ior;
        txnext_len = total;
    }
    return 0;
}

static void tx_done(int ok)
{
    struct IOSana2Req *ior = txbusy;
    txbusy = NULL;
    if (!ior) return;
    if (txnext) {                   /* the next frame is ready: send it first */
        tx_fire(txnext, txslot == TX_PAGE ? TX_PAGE2 : TX_PAGE, txnext_len);
        txnext = NULL;
    }
    if (ok) {
        stats.PacketsSent++;
        ior->ios2_Req.io_Error = 0;
        ReplyMsg((struct Message *)ior);
    } else {
        ior->ios2_Req.io_Error = S2ERR_TX_FAILURE;
        ior->ios2_WireError = S2WERR_TOO_MANY_RETRIES;
        ReplyMsg((struct Message *)ior);
        fire_events(S2EVENT_ERROR | S2EVENT_TX);
    }
}

/* ---- commands run by the driver process --------------------------------- */

static void go_offline(void)
{
    struct IOSana2Req *ior;
    if (!online) return;
    nic_stop();
    online = 0;
    if (txbusy) { ior = txbusy; txbusy = NULL; fail(ior, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE); }
    if (txnext) { ior = txnext; txnext = NULL; fail(ior, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE); }
    fire_events(S2EVENT_OFFLINE);
}

static void go_online(void)
{
    if (online) return;
    nic_start();
    stats.Reconfigurations++;
    fire_events(S2EVENT_ONLINE);
}

/* returns 1 if the request was a write the caller must hold until tx is free */
static int run_cmd(struct IOSana2Req *ior)
{
    int i, free_slot = -1;
    switch (ior->ios2_Req.io_Command) {
    case CMD_WRITE:
    case S2_BROADCAST:
    case S2_MULTICAST:
        if (!online) { fail(ior, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE); return 0; }
        return tx_start(ior);
    case S2_CONFIGINTERFACE:
        if (configured) { fail(ior, S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED); return 0; }
        for (i = 0; i < 6; i++) mac[i] = ior->ios2_SrcAddr[i];
        if (mac[0] & 1) for (i = 0; i < 6; i++) mac[i] = hwmac[i];   /* not usable */
        configured = 1;
        go_online();
        fail(ior, 0, 0);
        return 0;
    case S2_ONLINE:
        if (!configured) { fail(ior, S2ERR_BAD_STATE, S2WERR_NOT_CONFIGURED); return 0; }
        go_online();
        fail(ior, 0, 0);
        return 0;
    case S2_OFFLINE:
        go_offline();
        fail(ior, 0, 0);
        return 0;
    case S2_ADDMULTICASTADDRESS:
        if (!(ior->ios2_SrcAddr[0] & 1)) { fail(ior, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST); return 0; }
        for (i = 0; i < MAX_MC; i++) {
            int j;
            if (!mc[i].refs) { if (free_slot < 0) free_slot = i; continue; }
            for (j = 0; j < 6 && mc[i].addr[j] == ior->ios2_SrcAddr[j]; j++) ;
            if (j == 6) { mc[i].refs++; fail(ior, 0, 0); return 0; }
        }
        if (free_slot < 0) { fail(ior, S2ERR_NO_RESOURCES, S2WERR_MULTICAST_FULL); return 0; }
        for (i = 0; i < 6; i++) mc[free_slot].addr[i] = ior->ios2_SrcAddr[i];
        mc[free_slot].refs = 1;
        build_mar();
        load_page1(0);
        fail(ior, 0, 0);
        return 0;
    case S2_DELMULTICASTADDRESS:
        for (i = 0; i < MAX_MC; i++) {
            int j;
            if (!mc[i].refs) continue;
            for (j = 0; j < 6 && mc[i].addr[j] == ior->ios2_SrcAddr[j]; j++) ;
            if (j == 6) {
                if (!--mc[i].refs) { build_mar(); load_page1(0); }
                fail(ior, 0, 0);
                return 0;
            }
        }
        fail(ior, S2ERR_BAD_STATE, S2WERR_BAD_MULTICAST);
        return 0;
    }
    fail(ior, IOERR_NOCMD, 0);
    return 0;
}

static void service(void)
{
    struct IOSana2Req *ior;
    UBYTE isr;
    int loops;

    for (loops = 0; loops < 64; loops++) {
        isr = NR(ISR);
        if (isr & ISR_OVW) {
            /* ring overrun, handled as the DP8390 data sheet says: the
             * frames already in the ring are kept, only the one that did
             * not fit is lost */
            UBYTE txp = NR(CR) & CR_TXP, resend = 0;
            stats.Overruns++;
            n_ovw++;
            NW(CR, CR_NODMA | CR_STP);
            wait_us(2000);
            NW(RBCR0, 0); NW(RBCR1, 0);
            if (txp && !(NR(ISR) & (ISR_PTX | ISR_TXE))) resend = 1;
            NW(TCR, 0x02);
            NW(CR, CR_NODMA | CR_STA);
            if (!rx_drain()) { nic_start(); continue; }
            NW(ISR, ISR_OVW | ISR_PRX | ISR_RXE);
            NW(TCR, 0x00);
            if (resend) NW(CR, CR_NODMA | CR_TXP | CR_STA);
            continue;
        }
        if (isr & (ISR_PRX | ISR_RXE)) {
            NW(ISR, ISR_PRX | ISR_RXE);
            if (isr & ISR_RXE) stats.BadData++;
            n_rxdrain++;
            if (online && !rx_drain()) { n_corrupt++; nic_start(); continue; }
        }
        if (isr & (ISR_PTX | ISR_TXE)) {
            NW(ISR, ISR_PTX | ISR_TXE);
            tx_done(!(isr & ISR_TXE) || (isr & ISR_PTX));
        }
        else if (txbusy && !(NR(CR) & CR_TXP)) {
            /* the chip is done but no PTX/TXE was seen: don't hold every write */
            n_txmiss++;
            tx_done(1);
        }
        if (isr & ISR_CNT) {
            (void)NR(0x0D); (void)NR(0x0E); (void)NR(0x0F);   /* tally counters */
            NW(ISR, ISR_CNT);
        }

        /* commands: run in order, but a write waits while one is on the wire */
        for (;;) {
            Forbid();
            ior = (struct IOSana2Req *)cmdq.mlh_Head;
            if (!ior->ios2_Req.io_Message.mn_Node.ln_Succ) ior = NULL;
            else if (txbusy && txnext && (ior->ios2_Req.io_Command == CMD_WRITE ||
                                ior->ios2_Req.io_Command == S2_BROADCAST ||
                                ior->ios2_Req.io_Command == S2_MULTICAST)) ior = NULL;
            else Remove((struct Node *)ior);
            Permit();
            if (!ior) break;
            if (run_cmd(ior)) {     /* both buffers full: back to the front */
                Forbid(); AddHead((struct List *)&cmdq, (struct Node *)ior); Permit();
                break;
            }
        }
        if (!(NR(ISR) & IMR_ALL)) break;
    }
}

static void xs_proc(void)
{
    struct Task *me = FindTask(NULL);
    ULONG sigs;

    intsig = AllocSignal(-1);
    cmdsig = AllocSignal(-1);
    intmask = 1UL << intsig;
    cmdmask = 1UL << cmdsig;
    intr.is_Node.ln_Type = NT_INTERRUPT;
    intr.is_Node.ln_Pri = 0;
    intr.is_Node.ln_Name = (char *)xs_devname;
    intr.is_Data = NULL;
    intr.is_Code = xs_intstub;
    proc = me;
    AddIntServer(INTB_PORTS, &intr);
    Signal(parent, SIGF_SINGLE);

    for (;;) {
        sigs = Wait(intmask | cmdmask | SIGBREAKF_CTRL_C);
        if (sigs & SIGBREAKF_CTRL_C) break;
        n_wakes++;
        service();
        if (online) enable_ints();
    }

    go_offline();
    nic_stop();
    RemIntServer(INTB_PORTS, &intr);
    FreeSignal(intsig); FreeSignal(cmdsig);
    intsig = cmdsig = -1;
    Forbid();
    proc = NULL;
    Signal(parent, SIGF_SINGLE);
}

/* INT2 server, called from start.S; nonzero = it was us */
ULONG xs_interrupt(void)
{
    UBYTE isr;
    if (!imr_shadow) return 0;
    isr = NR(ISR) & imr_shadow;
    if (!isr) return 0;
    imr_shadow = 0;
    NW(IMR, 0);
    n_ints++;
    Signal(proc, intmask);
    return 1;
}

/* ---- hardware bring-up (first open) ------------------------------------ */

static int hw_setup(void)
{
    struct ConfigDev *cd = NULL;
    int i;
    if (!(ExpansionBase = (struct ExpansionBase *)OpenLibrary("expansion.library", 37))) return 0;
    while ((cd = FindConfigDev(cd, XS_MANUF, XS_PROD)))
        if (cd->cd_Rom.er_SerialNumber != XS_DENEB) break;
    CloseLibrary((struct Library *)ExpansionBase);
    if (!cd) return 0;
    board = (volatile UBYTE *)cd->cd_BoardAddr;
    nic = board + XS_ISA + NIC_BASE * 2;
    dataw = (volatile UWORD *)(nic + ASIC_DATA * 2);
    datab = nic + ASIC_DATA * 2;
    if (!nic_present() && !(pnp_configure() && nic_present())) return 0;
    setup_bus_and_mac();
    nic_stop();
    for (i = 0; i < 6; i++) mac[i] = hwmac[i];
    /* tell the user how the chip is driven (ENV:XSurfNet-Bus) */
    {
        char msg[120];
        const char *m = wordmode ? (swapmode ? "16-bit, swapped" : dcr_value == 0x4B ? "16-bit, BOS" : "16-bit") : "8-bit";
        int k = 0;
        while (*m) msg[k++] = *m++;
        for (m = busdiag; *m && k < (int)sizeof(msg) - 1; ) msg[k++] = *m++;
        msg[k] = 0;
        SetVar("XSurfNet-Bus", msg, -1, GVF_GLOBAL_ONLY);
    }
    return 1;
}

/* ---- device vectors ---------------------------------------------------- */

static void start_proc_wait(void)
{
    parent = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    if (CreateNewProcTags(NP_Entry, (ULONG)xs_proc, NP_Name, (ULONG)xs_devname,
                          NP_Priority, 5, NP_StackSize, 4096, TAG_DONE))
        Wait(SIGF_SINGLE);
}

static void stop_proc_wait(void)
{
    if (!proc) return;
    parent = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    Signal(proc, SIGBREAKF_CTRL_C);
    Wait(SIGF_SINGLE);
}

static void close_libs(void)
{
    if (UtilityBase) { CloseLibrary(UtilityBase); UtilityBase = NULL; }
    if (DOSBase) { CloseLibrary((struct Library *)DOSBase); DOSBase = NULL; }
}

void xs_open(REG(a1, struct IOSana2Req *ior), REG(d0, ULONG unitnum), REG(d1, ULONG flags),
             REG(a6, struct Device *dev))
{
    struct Opener *o;
    dev->dd_Library.lib_OpenCnt++;  /* no expunge while we may Wait() */
    ior->ios2_Req.io_Error = IOERR_OPENFAIL;
    if (unitnum != 0) goto out;
    if (ior->ios2_Req.io_Message.mn_Length < sizeof(struct IOSana2Req)) { ior->ios2_Req.io_Error = IOERR_BADLENGTH; goto out; }
    if (exclusive || ((flags & SANA2OPF_MINE) && unit.unit_OpenCnt)) { ior->ios2_Req.io_Error = IOERR_UNITBUSY; goto out; }

    if (!unit.unit_OpenCnt) {
        if (!DOSBase && !(DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 37))) goto out;
        if (!UtilityBase && !(UtilityBase = OpenLibrary("utility.library", 37))) goto out;
        if (!hw_ok) hw_ok = hw_setup();
        if (!hw_ok) goto out;
        start_proc_wait();
        if (!proc) goto out;
    }
    if (!(o = AllocMem(sizeof(*o), MEMF_PUBLIC | MEMF_CLEAR))) goto out;
    init_list(&o->readq);
    o->copyto = (APTR)GetTagData(S2_CopyToBuff, 0, (struct TagItem *)ior->ios2_BufferManagement);
    o->copyfrom = (APTR)GetTagData(S2_CopyFromBuff, 0, (struct TagItem *)ior->ios2_BufferManagement);
    o->filter = (struct Hook *)GetTagData(S2_PacketFilter, 0, (struct TagItem *)ior->ios2_BufferManagement);
    Forbid();
    AddTail((struct List *)&openers, (struct Node *)o);
    Permit();
    if (flags & SANA2OPF_MINE) exclusive = 1;
    if (flags & SANA2OPF_PROM) promisc = 1;
    ior->ios2_BufferManagement = o;
    ior->ios2_Req.io_Unit = &unit;
    ior->ios2_Req.io_Error = 0;
    unit.unit_OpenCnt++;
    dev->dd_Library.lib_OpenCnt++;
    dev->dd_Library.lib_Flags &= ~LIBF_DELEXP;
out:
    if (!unit.unit_OpenCnt) { stop_proc_wait(); close_libs(); }
    dev->dd_Library.lib_OpenCnt--;
}

static BPTR do_expunge(struct Device *dev)
{
    BPTR seg;
    if (dev->dd_Library.lib_OpenCnt) { dev->dd_Library.lib_Flags |= LIBF_DELEXP; return 0; }
    seg = seglist;
    Remove(&dev->dd_Library.lib_Node);
    FreeMem((UBYTE *)dev - dev->dd_Library.lib_NegSize,
            dev->dd_Library.lib_NegSize + dev->dd_Library.lib_PosSize);
    return seg;
}

BPTR xs_close(REG(a1, struct IOSana2Req *ior), REG(a6, struct Device *dev))
{
    struct Opener *o = (struct Opener *)ior->ios2_BufferManagement;
    struct IOSana2Req *r, *next;
    Forbid();
    if (o) {
        Remove((struct Node *)o);
        flush_list(&o->readq);
        /* orphan reads and event requests of this opener */
        for (r = (struct IOSana2Req *)orphanq.mlh_Head; (next = (struct IOSana2Req *)r->ios2_Req.io_Message.mn_Node.ln_Succ); r = next)
            if (r->ios2_BufferManagement == o) { Remove((struct Node *)r); r->ios2_Req.io_Error = IOERR_ABORTED; ReplyMsg((struct Message *)r); }
        for (r = (struct IOSana2Req *)eventq.mlh_Head; (next = (struct IOSana2Req *)r->ios2_Req.io_Message.mn_Node.ln_Succ); r = next)
            if (r->ios2_BufferManagement == o) { Remove((struct Node *)r); r->ios2_Req.io_Error = IOERR_ABORTED; ReplyMsg((struct Message *)r); }
    }
    Permit();
    if (o) FreeMem(o, sizeof(*o));
    ior->ios2_Req.io_Device = (struct Device *)-1;
    ior->ios2_Req.io_Unit = (struct Unit *)-1;
    if (!--unit.unit_OpenCnt) {
        exclusive = promisc = 0;
        configured = 0;
        stop_proc_wait();           /* takes the unit offline */
        close_libs();
    }
    if (!--dev->dd_Library.lib_OpenCnt && (dev->dd_Library.lib_Flags & LIBF_DELEXP))
        return do_expunge(dev);
    return 0;
}

BPTR xs_expunge(REG(a6, struct Device *dev))
{
    return do_expunge(dev);
}

ULONG xs_null(void) { return 0; }

void xs_beginio(REG(a1, struct IOSana2Req *ior), REG(a6, struct Device *dev))
{
    struct Sana2DeviceQuery *q;
    struct MinList *l = NULL;
    int i;

    ior->ios2_Req.io_Message.mn_Node.ln_Type = NT_MESSAGE;
    ior->ios2_Req.io_Error = 0;

    switch (ior->ios2_Req.io_Command) {
    case CMD_READ:
        if (!((struct Opener *)ior->ios2_BufferManagement)->copyto) {
            ior->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT; ior->ios2_WireError = S2WERR_NULL_POINTER; break;
        }
        l = &((struct Opener *)ior->ios2_BufferManagement)->readq;
        break;
    case S2_READORPHAN:
        if (!((struct Opener *)ior->ios2_BufferManagement)->copyto) {
            ior->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT; ior->ios2_WireError = S2WERR_NULL_POINTER; break;
        }
        l = &orphanq;
        break;
    case S2_ONEVENT:
        if ((ior->ios2_WireError & S2EVENT_ONLINE) && online) { ior->ios2_WireError = S2EVENT_ONLINE; break; }
        if ((ior->ios2_WireError & S2EVENT_OFFLINE) && !online) { ior->ios2_WireError = S2EVENT_OFFLINE; break; }
        l = &eventq;
        break;
    case CMD_WRITE:
    case S2_BROADCAST:
    case S2_MULTICAST:
        ior->ios2_WireError = 0;
        if (!((struct Opener *)ior->ios2_BufferManagement)->copyfrom) {
            ior->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT; ior->ios2_WireError = S2WERR_NULL_POINTER; break;
        }
        if (!online) { ior->ios2_Req.io_Error = S2ERR_OUTOFSERVICE; ior->ios2_WireError = S2WERR_UNIT_OFFLINE; break; }
        if (ior->ios2_Req.io_Command == S2_MULTICAST && !(ior->ios2_DstAddr[0] & 1)) {
            ior->ios2_Req.io_Error = S2ERR_BAD_ADDRESS; ior->ios2_WireError = S2WERR_BAD_MULTICAST; break;
        }
        /* fall through */
    case S2_CONFIGINTERFACE:
    case S2_ONLINE:
    case S2_OFFLINE:
    case S2_ADDMULTICASTADDRESS:
    case S2_DELMULTICASTADDRESS:
        ior->ios2_Req.io_Flags &= ~IOF_QUICK;
        Forbid();
        AddTail((struct List *)&cmdq, (struct Node *)ior);
        Permit();
        Signal(proc, cmdmask);
        return;
    case S2_DEVICEQUERY:
        q = (struct Sana2DeviceQuery *)ior->ios2_StatData;
        if (!q) { ior->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT; ior->ios2_WireError = S2WERR_NULL_POINTER; break; }
        q->DevQueryFormat = 0;
        q->DeviceLevel = 0;
        if (q->SizeAvailable >= 18) q->AddrFieldSize = 48;
        if (q->SizeAvailable >= 22) q->MTU = ETH_MTU;
        if (q->SizeAvailable >= 26) q->BPS = 10000000;
        if (q->SizeAvailable >= 30) q->HardwareType = S2WireType_Ethernet;
        if (q->SizeAvailable >= 34) q->RawMTU = ETH_MAX;
        q->SizeSupplied = q->SizeAvailable >= 34 ? 34 : q->SizeAvailable;
        break;
    case S2_GETSTATIONADDRESS:
        for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++) ior->ios2_SrcAddr[i] = ior->ios2_DstAddr[i] = 0;
        for (i = 0; i < 6; i++) { ior->ios2_SrcAddr[i] = mac[i]; ior->ios2_DstAddr[i] = hwmac[i]; }
        break;
    case S2_GETGLOBALSTATS:
        if (!ior->ios2_StatData) { ior->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT; ior->ios2_WireError = S2WERR_NULL_POINTER; break; }
        memcpy(ior->ios2_StatData, &stats, sizeof(stats));
        break;
    case S2_GETSPECIALSTATS:
        if (!ior->ios2_StatData) { ior->ios2_Req.io_Error = S2ERR_BAD_ARGUMENT; ior->ios2_WireError = S2WERR_NULL_POINTER; break; }
        {
            /* driver internals, for xsstat: type 0x58530000 + n, Count = value */
            struct Sana2SpecialStatHeader *h = (struct Sana2SpecialStatHeader *)ior->ios2_StatData;
            ULONG *rec = (ULONG *)(h + 1);
            ULONG v[12];
            int k, nq = 0;
            struct Node *nd;
            struct Opener *o;
            Forbid();
            for (o = (struct Opener *)openers.mlh_Head; o->node.mln_Succ; o = (struct Opener *)o->node.mln_Succ)
                for (nd = (struct Node *)o->readq.mlh_Head; nd->ln_Succ; nd = nd->ln_Succ) nq++;
            v[0] = n_ints; v[1] = n_wakes; v[2] = n_rxdrain; v[3] = n_ovw; v[4] = n_txmiss; v[5] = n_corrupt;
            v[6] = imr_shadow; v[7] = online; v[8] = txbusy != NULL; v[9] = txnext != NULL;
            v[10] = nq;
            for (k = 0, nd = (struct Node *)cmdq.mlh_Head; nd->ln_Succ; nd = nd->ln_Succ) k++;
            v[11] = k;
            Permit();
            for (k = 0; k < 12 && k < (int)h->RecordCountMax; k++) {
                rec[k * 3] = 0x58530000 + k;
                rec[k * 3 + 1] = v[k];
                rec[k * 3 + 2] = 0;
            }
            h->RecordCountSupplied = k;
        }
        break;
    case CMD_FLUSH: {
        struct Opener *o;
        Forbid();
        for (o = (struct Opener *)openers.mlh_Head; o->node.mln_Succ; o = (struct Opener *)o->node.mln_Succ)
            flush_list(&o->readq);
        flush_list(&orphanq);
        flush_list(&eventq);
        Permit();
        break;
    }
    default:
        ior->ios2_Req.io_Error = (ior->ios2_Req.io_Command == S2_TRACKTYPE ||
                                  ior->ios2_Req.io_Command == S2_UNTRACKTYPE ||
                                  ior->ios2_Req.io_Command == S2_GETTYPESTATS) ? S2ERR_NOT_SUPPORTED : IOERR_NOCMD;
        break;
    }
    if (l) {
        ior->ios2_Req.io_Flags &= ~IOF_QUICK;
        Forbid();
        AddTail((struct List *)l, (struct Node *)ior);
        Permit();
        return;
    }
    reply(ior);
}

LONG xs_abortio(REG(a1, struct IOSana2Req *ior), REG(a6, struct Device *dev))
{
    struct Node *n;
    struct Opener *o = (struct Opener *)ior->ios2_BufferManagement;
    struct MinList *lists[4];
    int i;
    lists[0] = &o->readq; lists[1] = &orphanq; lists[2] = &eventq; lists[3] = &cmdq;
    Forbid();
    for (i = 0; i < 4; i++)
        for (n = (struct Node *)lists[i]->mlh_Head; n->ln_Succ; n = n->ln_Succ)
            if (n == (struct Node *)ior) {
                Remove(n);
                ior->ios2_Req.io_Error = IOERR_ABORTED;
                ReplyMsg((struct Message *)ior);
                Permit();
                return 0;
            }
    Permit();
    return 0;
}

static const APTR vectors[] = {
    (APTR)xs_open, (APTR)xs_close, (APTR)xs_expunge, (APTR)xs_null,
    (APTR)xs_beginio, (APTR)xs_abortio, (APTR)-1
};

/* called from start.S with the segment list */
struct Device *xs_init(REG(d0, BPTR seg))
{
    struct Device *dev;
    SysBase = *(struct ExecBase **)4;
    dev = (struct Device *)MakeLibrary((APTR)vectors, NULL, NULL, sizeof(struct Device), 0);
    if (!dev) return NULL;
    seglist = seg;
    init_list(&openers); init_list(&orphanq); init_list(&eventq); init_list(&cmdq);
    unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
    unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    dev->dd_Library.lib_Node.ln_Type = NT_DEVICE;
    dev->dd_Library.lib_Node.ln_Name = (char *)xs_devname;
    dev->dd_Library.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    dev->dd_Library.lib_Version = 1;
    dev->dd_Library.lib_Revision = 0;
    dev->dd_Library.lib_IdString = (APTR)xs_idstring;
    xsdev = dev;
    AddDevice(dev);
    return dev;
}
