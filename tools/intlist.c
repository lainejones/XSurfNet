/*
 * intlist - list the servers on the PORTS (INT2) interrupt chain.
 * Part of XSurfNet. GPL-3.0-only, see LICENSE.
 */
#include <stdio.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/interrupts.h>
#include <hardware/intbits.h>
#include <proto/exec.h>

int main(void)
{
    struct List *l;
    struct Node *n;
    Disable();
    l = (struct List *)SysBase->IntVects[INTB_PORTS].iv_Data;
    for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct Interrupt *i = (struct Interrupt *)n;
        Enable();
        printf("pri %4d  code $%08lx  data $%08lx  %s\n", n->ln_Pri, (unsigned long)i->is_Code,
               (unsigned long)i->is_Data, n->ln_Name ? n->ln_Name : "(no name)");
        Disable();
    }
    Enable();
    return 0;
}
