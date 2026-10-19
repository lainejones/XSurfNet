# XSurfNet

An open-source SANA-II network driver for the Individual Computers **X-Surf**
(the first X-Surf: an RTL8019AS ISA network chip on a Zorro II carrier, 10 Mbit).

Status: early. `xsprobe` finds the board, sets the chip up through ISA Plug and
Play when its EEPROM hasn't, and reads the MAC address. The driver is next.

## The hardware, as the driver sees it

- Zorro II board, manufacturer 4626 (Individual Computers), product 23, 64 KB.
- Offset `0x8000` is an ISA I/O window: ISA port `p` is the byte at board
  offset `0x8000 + 2*p`. Bit 7 of the board register at `0x7E` selects the upper
  4 KB of ISA space (ISA ports `0x800` and up, for Plug and Play's `0xA79`).
- The RTL8019AS (NE2000 compatible) is either set up by its EEPROM or waits for
  ISA Plug and Play; XSurfNet puts it at ISA `0x300` (data port `0x310`, reset
  `0x31F`). Its interrupt reaches the Amiga as INT2.
- Also on the board: an IDE port and clockports, which the driver leaves alone.

References: NetBSD's `xsurf.c` / `if_ne_xsurf.c` (Bernd Ernesti) and WinUAE's
X-Surf emulation (Toni Wilen), from which the register layout above was learnt;
the RTL8019AS data sheet; the ISA Plug and Play specification.

## Building

```
wsl -e bash -lc 'cd /mnt/c/projects/XSurfNet && PATH=/opt/amiga/bin:$PATH make'
```

## Licence

GPL-3.0-only, see [LICENSE](LICENSE).
