| XSurfNet - ROMTag, strings and the interrupt server stub.
| GPL-3.0-only, see LICENSE.
|
| No RTF_AUTOINIT: rt_Init points at Init, which calls the C initialiser
| (xs_init: MakeLibrary + AddDevice). Exec passes the segment list in a0 for a
| non-autoinit resident (some loaders also in d0).

    .text

| Executing the device as a program just returns -1.
Start:
    moveq   #-1,d0
    rts
    .even

RomTag:
    .word   0x4AFC              | RTC_MATCHWORD
    .long   RomTag              | rt_MatchTag
    .long   EndSkip             | rt_EndSkip
    .byte   0                   | rt_Flags (no RTF_AUTOINIT)
    .byte   1                   | rt_Version
    .byte   3                   | rt_Type = NT_DEVICE
    .byte   0                   | rt_Pri
    .long   _xs_devname         | rt_Name
    .long   _xs_idstring        | rt_IdString
    .long   Init                | rt_Init
EndSkip:
    .even

    .globl  _xs_devname
_xs_devname:
    .ascii  "xsurfnet.device"
    .byte   0
    .even

    .globl  _xs_idstring
_xs_idstring:
    .ascii  "xsurfnet.device 1.0b7 (19.10.2026)"
    .byte   13,10,0
    .ascii  "$VER: xsurfnet.device 1.0b7 (19.10.2026) X-Surf SANA-II driver"
    .byte   0
    .even

| d0/a0 = segment list, a6 = ExecBase
Init:
    tst.l   d0
    bne.s   1f
    move.l  a0,d0
1:
    jmp     _xs_init

| INT2 (PORTS) server: a1 = is_Data. Exec checks the Z flag on return:
| set = not ours, so the next server runs.
    .globl  _xs_intstub
_xs_intstub:
    jsr     _xs_interrupt
    tst.l   d0
    rts

| ULONG xs_callcopy(func, to, from, len): call a SANA-II buffer function
| (a0 = to, a1 = from, d0 = length), keeping every register the C caller expects.
    .globl  _xs_callcopy
_xs_callcopy:
    movem.l d2-d7/a2-a6,-(sp)
    move.l  48(sp),a2
    move.l  52(sp),a0
    move.l  56(sp),a1
    move.l  60(sp),d0
    jsr     (a2)
    movem.l (sp)+,d2-d7/a2-a6
    rts
