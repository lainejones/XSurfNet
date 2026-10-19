# XSurfNet

An open-source SANA-II network driver for the Individual Computers **X-Surf**
(the original X-Surf: an RTL8019AS ISA network chip on a Zorro II board, 10 Mbit).

XSurfNet is `xsurfnet.device`. It works with any SANA-II TCP/IP stack and was
written and tested with [PrismNet](https://github.com/lainejones/PrismNet).

## Features

- **Multicast.** The chip's multicast hash filter is programmed from
  `S2_ADDMULTICASTADDRESS` / `S2_DELMULTICASTADDRESS`, so multicast protocols
  work: with PrismNet, the Amiga answers to its `name.local` over mDNS.
- **16-bit transfers.** The driver tests the bus at start-up and moves data
  16 bits at a time (with 32-bit CPU moves), falling back to 8-bit only if the
  test fails. `ENV:XSurfNet-Bus` shows the mode it chose.
- **ISA Plug and Play.** If the chip's EEPROM hasn't set it up, the driver does
  the Plug and Play setup itself (I/O port 0x300).
- **Overrun recovery as the DP8390 data sheet describes it:** when the 16 KB
  receive buffer fills, the frames already in it are kept.
- **Two transmit buffers:** the next frame is copied to the card while the
  current one is being sent.
- **Leaves the card clean:** on the last close the chip is reset to its
  power-on state, so another driver can take over without a reboot.
- Promiscuous mode, raw frames, broadcast and multicast writes, `S2_ONEVENT`,
  orphan reads, packet-filter hooks, global statistics.
- Runs on a 68000; about 10 KB.

## Requirements

- An X-Surf (Zorro II, manufacturer 4626, product 23). The X-Surf 100 is a
  different card and is not supported.
- AmigaOS with V37 (2.04) libraries or newer. Tested on AmigaOS 3.2.
- A SANA-II TCP/IP stack.

## Installing

1. Copy `Devs/Networks/xsurfnet.device` to `DEVS:Networks/`.
2. Tell your stack to use `DEVS:Networks/xsurfnet.device`, unit 0, in place of
   the X-Surf driver it uses now. With PrismNet, that is the `DEVICE=` line of
   the interface file in `ENVARC:PrismNet/interfaces` (or PrismNet Prefs).
3. Reboot (or restart the stack).

Only one driver may use the card at a time: stop the stack before switching
drivers.

## Tools

In the `Tools` drawer of the package:

- `xsstat` prints the driver's internal counters (interrupts, overruns,
  missed transmit completions, queue lengths) while it runs.
- `xsflush` removes `xsurfnet.device` from memory when nothing has it open, so
  the next open loads the file again (for updating without a reboot).

The source also has diagnostics that reset the chip (`xsprobe`, `xsword`,
`xsbus`); run those only while no driver uses the card.

## Performance

Measured on an A2000 with a TF536 (68030) and an X-Surf, with PrismNet, copying
1 MB over TCP between the Amiga and a PC on the same LAN:

| Direction | Speed |
|---|---|
| To the Amiga | 474-487 KB/s |
| From the Amiga | 382-384 KB/s |

The card itself moves about 2.7 MB/s through 16-bit transfers on that machine
(`xsbus`), well above the 10 Mbit wire (about 1.2 MB/s), so the network rather
than the bus is the limit.

## Notes

- Straight after a reset, a real X-Surf returns every second byte wrong on 8-bit
  reads until an 8-bit write has been done. The driver's bus test does an 8-bit
  write and read first for that reason, then uses 16-bit transfers only.
- WinUAE's X-Surf emulation passes 16-bit data byte-swapped; the driver detects
  this and uses the chip's byte-order bit, so it also runs in WinUAE.
- One stall (the driver stopped receiving) was seen once during testing in
  WinUAE and could not be reproduced since, including a 40-minute soak test on
  real hardware. If you see the network stop, `xsstat` output would help.

## The hardware, as the driver sees it

- Zorro II board, manufacturer 4626 (Individual Computers), product 23, 64 KB.
- Offset `0x8000` is an ISA I/O window: ISA port `p` is the byte at board
  offset `0x8000 + 2*p`. Bit 7 of the board register at `0x7E` selects the upper
  4 KB of ISA space (ISA ports `0x800` and up, for Plug and Play's `0xA79`).
- The RTL8019AS (NE2000 compatible) sits at ISA `0x300` (data port `0x310`,
  reset `0x31F`). Its interrupt reaches the Amiga as INT2.
- Also on the board: an IDE port and clockports, which the driver leaves alone.
- The E3B Deneb's emulated X-Surf entry (serial `0xC0FFEE01`) is skipped.

## Building

With [amiga-gcc](https://github.com/bebbo/amiga-gcc):

```
make
./dist.sh
```

`make` builds the driver and the tools into `out/`; `dist.sh` stages the release
package in `dist/`.

## Credits

The register layout was learnt from NetBSD's `xsurf.c` and `if_ne_xsurf.c`
(Bernd Ernesti) and from WinUAE's X-Surf emulation (Toni Wilen), with the
RTL8019AS data sheet and the ISA Plug and Play specification. Thanks to them.

## Licence

GPL-3.0-only, see [LICENSE](LICENSE).
