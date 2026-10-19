# XSurfNet - an open SANA-II driver for the Individual Computers X-Surf.
# Build under WSL with amiga-gcc:  PATH=/opt/amiga/bin:$PATH make
CC      = m68k-amigaos-gcc
CFLAGS  = -m68000 -Os -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer
OUT     = out

all: $(OUT)/xsprobe

$(OUT)/xsprobe: tools/xsprobe.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

clean:
	rm -rf $(OUT)
