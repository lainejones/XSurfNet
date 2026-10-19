# XSurfNet - an open SANA-II driver for the Individual Computers X-Surf.
# Build under WSL with amiga-gcc:  PATH=/opt/amiga/bin:$PATH make
CC      = m68k-amigaos-gcc
CFLAGS  = -m68000 -Os -noixemul -Wall -Wno-pointer-sign -fomit-frame-pointer
OUT     = out

all: $(OUT)/xsprobe $(OUT)/xsurfnet.device $(OUT)/intlist $(OUT)/xsword $(OUT)/xsflush $(OUT)/xsstat $(OUT)/xsbus

$(OUT)/xsbus: tools/xsbus.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/xsstat: tools/xsstat.c include/sana2.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -Iinclude -s -o $@ $<

$(OUT)/xsflush: tools/xsflush.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/xsword: tools/xsword.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/intlist: tools/intlist.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

$(OUT)/xsurfnet.device: dev/start.S dev/xsurfnet.c include/sana2.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -ffreestanding -fno-builtin -Iinclude -nostartfiles -nostdlib -s -o $@ dev/start.S dev/xsurfnet.c -lgcc

$(OUT)/xsprobe: tools/xsprobe.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -s -o $@ $<

clean:
	rm -rf $(OUT)
