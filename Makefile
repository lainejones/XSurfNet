# XSurfNet - an open SANA-II driver for the Individual Computers X-Surf.
# Build under WSL with amiga-gcc:  PATH=/opt/amiga/bin:$PATH make
CC      = m68k-amigaos-gcc
CFLAGS  = -m68000 -Os -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer
OUT     = out

all: $(OUT)/xsprobe $(OUT)/xsurfnet.device

$(OUT)/xsurfnet.device: dev/start.S dev/xsurfnet.c include/sana2.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -ffreestanding -fno-builtin -Iinclude -nostartfiles -nostdlib -s -o $@ dev/start.S dev/xsurfnet.c -lgcc

$(OUT)/xsprobe: tools/xsprobe.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

clean:
	rm -rf $(OUT)
