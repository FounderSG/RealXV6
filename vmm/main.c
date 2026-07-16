#include "vmm.h"

/* --------------------------------------------------------------------------
 * Static placements live in the binary (_DATA so they're zero-init by the
 * loader; we don't have a BSS-zeroing startup yet).
 * ------------------------------------------------------------------------ */
static struct idt_entry idt[256]   = { 0 };
static struct dtr       idtr       = { 0, 0 };
static struct tss_full  tss0       = { 0 };
static u16              g_segflt_isr_ip = 0;   /* _segflt_isr offset, set by HVC_SEGFLT_SETUP */

/* Bootsect loads vmm.bin + unix.com together at phys 0x08000:
 *   0x08000..0x0BFFF  vmm.bin  (sectors 1..32, ~16 KB budget)
 *   0x0C000..0x13FFF  unix.com (sectors 33..96, 32 KB embedded for VMM memcpy)
 * We memcpy unix.com to its expected guest address 0x10100 before V86 entry.
 * The blob size must exceed unix.com's actual size: bytes past it copy as zeros,
 * and truncating the kernel's tail (CONST strings, bdevsw/cdevsw) wipes the
 * device-switch tables and panics in iinit.  Keep the sector layout in sync
 * with bootsect.asm. */
#define GUEST_CS         0x1000
#define GUEST_IP         0x0100
#define UNIX_SRC_LINEAR  0x0C000
#define UNIX_DST_LINEAR  0x10100
#define UNIX_BLOB_SIZE   32768       /* 32 KB; must be >= sizeof(unix.com) */

/* --------------------------------------------------------------------------
 * Install IDT entries 0..31 (CPU exceptions).  Hardware-IRQ vectors and
 * software-INT vectors stay zero -- those should never reach PM in P2a
 * because we haven't unmasked the PIC yet and V86 INT n is reflected by
 * trap_dispatch.
 * ------------------------------------------------------------------------ */
static void install_gate(int vec, u32 handler)
{
    idt[vec].off_low   = (u16)(handler & 0xFFFF);
    idt[vec].selector  = 0x08;          /* PM_CS */
    idt[vec].reserved  = 0;
    idt[vec].type_attr = 0x8E;          /* P=1, DPL=0, 32-bit interrupt gate */
    idt[vec].off_high  = (u16)(handler >> 16);
}

static void idt_init(void)
{
    install_gate(0,  (u32)isr0);   install_gate(1,  (u32)isr1);
    install_gate(2,  (u32)isr2);   install_gate(3,  (u32)isr3);
    install_gate(4,  (u32)isr4);   install_gate(5,  (u32)isr5);
    install_gate(6,  (u32)isr6);   install_gate(7,  (u32)isr7);
    install_gate(8,  (u32)isr8);   install_gate(9,  (u32)isr9);
    install_gate(10, (u32)isr10);  install_gate(11, (u32)isr11);
    install_gate(12, (u32)isr12);  install_gate(13, (u32)isr13);
    install_gate(14, (u32)isr14);  install_gate(15, (u32)isr15);
    install_gate(16, (u32)isr16);  install_gate(17, (u32)isr17);
    install_gate(18, (u32)isr18);  install_gate(19, (u32)isr19);
    install_gate(20, (u32)isr20);  install_gate(21, (u32)isr21);
    install_gate(22, (u32)isr22);  install_gate(23, (u32)isr23);
    install_gate(24, (u32)isr24);  install_gate(25, (u32)isr25);
    install_gate(26, (u32)isr26);  install_gate(27, (u32)isr27);
    install_gate(28, (u32)isr28);  install_gate(29, (u32)isr29);
    install_gate(30, (u32)isr30);  install_gate(31, (u32)isr31);

    /* Hardware IRQ vectors (post-PIC-remap). */
    install_gate(32, (u32)isr32);    /* PIT  (master IRQ0) */
    install_gate(33, (u32)isr33);    /* KBD  (master IRQ1) */
    install_gate(46, (u32)isr46);    /* IDE  (slave  IRQ6) */

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (u32)idt;
    load_idt(&idtr);
}

/* --------------------------------------------------------------------------
 * Remap the i8259 PIC pair: master IRQ0..7 -> vectors 0x20..0x27,
 * slave IRQ8..15 -> vectors 0x28..0x2F.  Necessary because BIOS leaves
 * master IRQs at 0x08..0x0F, which collide with CPU exception vectors
 * (#DF=8, #PF=14, etc.).  After remap, IRQs land in our IDT[0x20+] slots.
 *
 * Master mask: 0xF8 -- unmask IRQ0 (PIT), IRQ1 (KBD), IRQ2 (cascade).
 * Slave  mask: 0xBF -- unmask IRQ6 (IDE primary, == IRQ14 from BIOS pov).
 * ------------------------------------------------------------------------ */
static void pic_init(void)
{
    /* ICW1: cascade, ICW4 required */
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    /* ICW2: vector offset */
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    /* ICW3: cascade wiring -- slave on master IRQ2 */
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    /* ICW4: 8086 mode */
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    /* OCW1: masks */
    outb(0x21, 0xF8);
    outb(0xA1, 0xBF);
}

/* --------------------------------------------------------------------------
 * Patch GDT entry 3 (selector 0x18) with a 32-bit TSS descriptor pointing
 * at tss0, set up I/O bitmap (allow all 0..0x3FF), VME redirection (all
 * trap), then load TR.
 * ------------------------------------------------------------------------ */
static void tss_init(void)
{
    u32 base  = (u32)&tss0;
    u32 limit = sizeof(tss0) - 1;       /* covers the sentinel byte */
    u8 *e     = gdt + 0x18;

    e[0] = (u8)(limit & 0xFF);
    e[1] = (u8)((limit >> 8) & 0xFF);
    e[2] = (u8)(base & 0xFF);
    e[3] = (u8)((base >> 8) & 0xFF);
    e[4] = (u8)((base >> 16) & 0xFF);
    e[5] = 0x89;                        /* P=1, DPL=0, type=9 (avail 32-bit TSS) */
    e[6] = (u8)((limit >> 16) & 0x0F);
    e[7] = (u8)((base >> 24) & 0xFF);

    tss0.base.ss0        = 0x10;        /* PM_DS */
    tss0.base.esp0       = 0xFFFE;      /* PM ring0 stack top */
    tss0.base.iomap_base = (u16)((u32)&tss0.iomap - (u32)&tss0);
    tss0.sentinel        = 0xFF;
    /* int_redir + iomap stay zero -> int n traps, all I/O allowed */

    load_tss(0x18);
}

/* --------------------------------------------------------------------------
 * Inject an interrupt frame into V86: push flags/CS/return_ip onto the V86
 * stack, then redirect tf->cs:eip at the guest IVT[guest_vec] handler.
 * After iretd, V86 resumes inside the handler with IF/TF=0.
 *
 *   return_ip = address V86 should resume at when the handler IRETs.
 *               For 'int n' software interrupts: tf->eip + 2 (past CD vec).
 *               For hardware IRQs:               tf->eip (the next instr).
 * ------------------------------------------------------------------------ */
static void inject_v86(struct trap_frame *tf, u8 guest_vec, u16 return_ip)
{
    u16 sp, ss;
    u16 ret_cs, ret_flags;
    u16 *ivt;
    u32 stack_lin;

    sp        = (u16)(tf->esp_v86 & 0xFFFF);
    ss        = (u16)(tf->ss_v86 & 0xFFFF);
    stack_lin = (u32)(ss << 4);

    ret_cs    = (u16)(tf->cs & 0xFFFF);
    ret_flags = (u16)(tf->eflags & 0xFFFF);

    /* Push onto V86 stack (low->high address): flags, CS, IP */
    sp -= 2;
    *(u16 *)(stack_lin + sp) = ret_flags;
    sp -= 2;
    *(u16 *)(stack_lin + sp) = ret_cs;
    sp -= 2;
    *(u16 *)(stack_lin + sp) = return_ip;

    /* IVT at linear 0; each entry is { IP, CS } (4 bytes) */
    ivt = (u16 *)((u32)guest_vec * 4);

    tf->cs      = ivt[1];
    tf->eip     = ivt[0];
    tf->esp_v86 = sp;
    tf->eflags &= ~(u32)(0x100 | 0x200);    /* clear TF, IF */
}

static void reflect_int(struct trap_frame *tf, u8 vec)
{
    inject_v86(tf, vec, (u16)((tf->eip + 2) & 0xFFFF));
}

/* --------------------------------------------------------------------------
 * Deferred hardware-IRQ injection.
 *
 * A hardware IRQ must never be delivered into the guest while the guest has
 * interrupts disabled (virtual IF=0) -- doing so re-enters the kernel inside
 * its cli'd context-switch critical section (resume/do_resume), which runs on
 * a tiny private stack with the u-window mid-remap, corrupting the switch.
 * Unlike a PIT tick (which we may simply drop), a device IRQ carries state
 * (a disk completion that a process is sleeping on), so it must be held
 * pending and injected once the guest re-enables interrupts.
 *
 * We leave the held IRQ un-EOI'd: the PIC in-service bit then blocks a re-trap
 * until the guest's own ISR finally runs and sends EOI.  Re-injection happens
 * on the next IF 0->1 transition (STI / POPF / IRET).
 * ------------------------------------------------------------------------ */
#define P_KBD  0x01
#define P_IDE  0x02
static u8 g_pending_irq = 0;

static void deliver_pending(struct trap_frame *tf)
{
    if (!(tf->eflags & 0x200))           /* guest IF still 0: keep holding */
        return;
    if (g_pending_irq & P_IDE) {
        g_pending_irq &= ~P_IDE;
        inject_v86(tf, 0x76, (u16)(tf->eip & 0xFFFF));
        return;                          /* one at a time; ISR iret re-enables IF */
    }
    if (g_pending_irq & P_KBD) {
        g_pending_irq &= ~P_KBD;
        inject_v86(tf, 9, (u16)(tf->eip & 0xFFFF));
    }
}

/* Flags a V86 guest must not change via POPF/IRET: IOPL (bits 12-13) and NT
 * (bit 14).  If the guest raised IOPL to 3, CLI/STI/INT/IRET would stop trapping
 * to #GP and run natively, defeating the monitor's interrupt virtualization.
 * The guest entered with IOPL=0/NT=0 and the monitor never needs them set, so we
 * just force them back to 0 whenever the guest loads flags. */
#define V86_FLAGS_MASK 0x7000u          /* IOPL | NT */

/* --------------------------------------------------------------------------
 * Hypercall dispatch (called from trap_dispatch on V86 int 0x80).
 *  AH = hypercall number, args in AL/BX/CX/DX/etc.  Return in AX.
 * ------------------------------------------------------------------------ */
static void hyper_dispatch(struct trap_frame *tf)
{
    u8 ah = (u8)((tf->eax >> 8) & 0xFF);
    u8 al = (u8)(tf->eax & 0xFF);

    switch (ah) {
    case HVC_NOP:
        tf->eax = (tf->eax & 0xFFFF0000) | 0x0000;
        return;

    case HVC_PUTC:
        log_putc((char)al);
        tf->eax = (tf->eax & 0xFFFF0000) | 0x0000;
        return;

    case HVC_HALT:
        log_puts("guest halted, code=");
        log_int((u32)al);
        log_puts("\r\n");
        for (;;)
            ;

    case HVC_REPORT: {
        u16 expect = (u16)(tf->ebx & 0xFFFF);
        u16 actual = (u16)(tf->ecx & 0xFFFF);
        u16 flags  = (u16)(tf->edx & 0xFFFF);
        log_puts("[T");
        log_hex((u32)al);
        log_puts("] ");
        log_puts((flags & 1) ? "FAIL" : "PASS");
        log_puts(" expect=");
        log_hex((u32)expect);
        log_puts(" actual=");
        log_hex((u32)actual);
        log_puts("\r\n");
        tf->eax = (tf->eax & 0xFFFF0000) | 0x0000;
        return;
    }

    case HVC_SUREG: {
        /* Load user segment registers from a sureg_desc struct in guest memory.
         * BX = near offset of sureg_desc within the guest's DS segment.
         * PT0 is the identity page table built by entry.asm at phys 0x3000;
         * pt0[i] maps linear page i. */
        u32 lin  = ((u32)(tf->ds & 0xFFFF) << 4) + (u32)(tf->ebx & 0xFFFF);
        u16 *d   = (u16 *)lin;
        /* d[0]=taddr, d[1]=tsize, d[2]=daddr, d[3]=dsize,
         * d[4]=ssize, d[5]=uaddr, d[6]=mode */
        u16 taddr = d[0], tsize = d[1];
        u16 daddr = d[2], dsize = d[3], ssize = d[4];
        u16 uaddr = d[5], mode  = d[6];
        u32 *pt0  = (u32 *)0x3000;
        u32  k;
        /* Always remap WIN_U (1 page @ linear 0xD000, pt0[0x1D]). */
        pt0[0x1D] = ((u32)uaddr << 12) | 0x7u;

        if (mode == 1) {
            /* Tripwire: an EXE sureg with a NULL data block or NULL u-page
             * means the kernel is installing windows for a half-built
             * process image (would map page 0 / the IVT as the u-area). */
            if (uaddr == 0 || daddr == 0) {
                log_puts("HVC_SUREG null page: taddr=");
                log_hex(taddr); log_puts(" daddr="); log_hex(daddr);
                log_puts(" dsize="); log_hex(dsize);
                log_puts(" ssize="); log_hex(ssize);
                log_puts(" uaddr="); log_hex(uaddr);
                log_puts("\r\n");
                dump_state(tf, "SUREG null page");
                for (;;) ;
            }
            /* EXE: program WIN_TEXT (linear 0xA0000, pt0[0xA0..0xAF]) */
            if (tsize > 16) {
                log_puts("HVC_SUREG tsize>16\r\n");
                for (;;) ;
            }
            for (k = 0; k < tsize; k++)
                pt0[0xA0 + k] = ((u32)(taddr + k) << 12) | 0x5u;  /* user, RO: pure text */
            for (; k < 16; k++)
                pt0[0xA0 + k] = 0;

            /* WIN_DATA sparse (linear 0xD0000, pt0[0xD0..0xDF]):
             * data pages at low slots, stack pages at high slots (just under
             * NS=15), heap gap in between left not-present. */
            if ((u32)dsize + ssize > 15) {
                log_puts("HVC_SUREG d+s>15\r\n");
                for (;;) ;
            }
            for (k = 0; k < dsize; k++)
                pt0[0xD0 + k] = ((u32)(daddr + k) << 12) | 0x7u;
            for (k = dsize; k < (u32)(15 - ssize); k++)
                pt0[0xD0 + k] = 0;
            for (k = 0; k < ssize; k++)
                pt0[0xD0 + (15 - ssize) + k] = ((u32)(daddr + dsize + k) << 12) | 0x7u;
            pt0[0xDF] = 0;                         /* slot 15: above USTACK, not-present */
        }

        flush_tlb();
        tf->eax = (tf->eax & 0xFFFF0000);
        return;
    }

    case HVC_SEGFLT_SETUP:
        g_segflt_isr_ip = (u16)(tf->ebx & 0xFFFF);
        tf->eax = (tf->eax & 0xFFFF0000u);
        return;

    default:
        log_puts("unknown hypercall AH=0x");
        log_hex((u32)ah);
        log_puts("\r\n");
        for (;;)
            ;
    }
}

/* Push a 16-bit word onto the V86 stack; updates sp_out. */
static void v86_push16(struct trap_frame *tf, u16 val)
{
    u16 sp = (u16)((tf->esp_v86 & 0xFFFF) - 2);
    u32 stack_lin = (u32)((tf->ss_v86 & 0xFFFF) << 4);
    *(u16 *)(stack_lin + sp) = val;
    tf->esp_v86 = sp;
}

static u16 v86_pop16(struct trap_frame *tf)
{
    u16 sp = (u16)(tf->esp_v86 & 0xFFFF);
    u32 stack_lin = (u32)((tf->ss_v86 & 0xFFFF) << 4);
    u16 val = *(u16 *)(stack_lin + sp);
    tf->esp_v86 = (sp + 2) & 0xFFFF;
    return val;
}

/* --------------------------------------------------------------------------
 * Post-mortem crash dump.  A small ring records the last few sensitive events
 * (vec, cs:ip, ss:sp, opcode); on any unexpected trap (unhandled vector,
 * unemulated #GP op, or wild OUT) we dump full guest state + the stack + the
 * faulting bytes + the ring, instead of a bare halt.  Observation is VMM-side
 * only (we never perturb guest memory).  Explicit = { 0 } init keeps these in
 * _DATA (there is no BSS clear yet).
 * ------------------------------------------------------------------------ */
#define TRACE_N 24
struct trace_ent { u16 vec, cs, ip, ss, sp, op; };
static struct trace_ent trace_ring[TRACE_N] = { 0 };
static u32 trace_pos = 0;

static void trace_record(struct trap_frame *tf)
{
    struct trace_ent *e = &trace_ring[trace_pos % TRACE_N];
    u32 lin = ((tf->cs & 0xFFFF) << 4) + (tf->eip & 0xFFFF);
    e->vec = (u16)tf->vector;
    e->cs  = (u16)tf->cs;
    e->ip  = (u16)tf->eip;
    e->ss  = (u16)tf->ss_v86;
    e->sp  = (u16)tf->esp_v86;
    e->op  = *(u8 *)lin;
    trace_pos++;
}

static void dump_state(struct trap_frame *tf, const char *why)
{
    u32 lin = ((tf->cs & 0xFFFF) << 4) + (tf->eip & 0xFFFF);
    u32 stk = ((tf->ss_v86 & 0xFFFF) << 4) + (tf->esp_v86 & 0xFFFF);
    u32 i;

    log_puts("\r\n=== VMM POST-MORTEM: ");
    log_puts(why);
    log_puts(" ===\r\nvec="); log_hex(tf->vector);
    log_puts(" err="); log_hex(tf->errcode);
    log_puts(" cs:ip="); log_hex(tf->cs & 0xFFFF);
    log_putc(':'); log_hex(tf->eip & 0xFFFF);
    log_puts(" fl="); log_hex(tf->eflags & 0xFFFF);
    log_puts("\r\nax="); log_hex(tf->eax & 0xFFFF);
    log_puts(" bx="); log_hex(tf->ebx & 0xFFFF);
    log_puts(" cx="); log_hex(tf->ecx & 0xFFFF);
    log_puts(" dx="); log_hex(tf->edx & 0xFFFF);
    log_puts("\r\nsi="); log_hex(tf->esi & 0xFFFF);
    log_puts(" di="); log_hex(tf->edi & 0xFFFF);
    log_puts(" bp="); log_hex(tf->ebp & 0xFFFF);
    log_puts(" ss:sp="); log_hex(tf->ss_v86 & 0xFFFF);
    log_putc(':'); log_hex(tf->esp_v86 & 0xFFFF);
    log_puts(" ds="); log_hex(tf->ds & 0xFFFF);
    log_puts(" es="); log_hex(tf->es & 0xFFFF);

    log_puts("\r\ncode:");
    for (i = 0; i < 8; i++) { log_putc(' '); log_hex(*(u8 *)(lin + i)); }
    log_puts("\r\nstack:");
    for (i = 0; i < 8; i++) { log_putc(' '); log_hex(*(u16 *)(stk + i * 2)); }

    log_puts("\r\ntrace (old->new):\r\n");
    for (i = 0; i < TRACE_N; i++) {
        struct trace_ent *e = &trace_ring[(trace_pos + i) % TRACE_N];
        if (e->vec == 0 && e->cs == 0 && e->ip == 0)
            continue;                       /* never-written ring slot */
        log_puts("  v="); log_hex(e->vec);
        log_puts(" "); log_hex(e->cs); log_putc(':'); log_hex(e->ip);
        log_puts(" op="); log_hex(e->op);
        log_puts(" ss:sp="); log_hex(e->ss); log_putc(':'); log_hex(e->sp);
        log_puts("\r\n");
    }
}

/* The V6 port drives no DMA, so any OUT to a DMA page register (0x81..0x8F) is
 * wild code executing -- the exact moment control went off the rails (this is
 * the `dma: invalid channel 0x84` qemu prints).  Dump there, before the
 * post-panic sti/hlt idle loop overwrites the trace ring. */
static void check_wild_out(struct trap_frame *tf, unsigned port)
{
    if (port >= 0x81 && port <= 0x8F) {
        dump_state(tf, "wild OUT to DMA page reg");
        for (;;)
            ;
    }
}

/* --------------------------------------------------------------------------
 * Reflect a user #PF to the guest as a trap on the KERNEL stack (the user SP
 * may be inside the not-present stack gap, so it cannot be used).  Push, at
 * the top of the u-area kernel stack (WIN_U, top = 0xD400), the user iret
 * frame {ip,cs,flags} plus {fault_off, user_sp, user_ss}, then vector CS:IP to
 * _segflt_isr with SS:SP = kernel stack.  The faulting user's GP registers and
 * DS/ES are left live in tf so _segflt_isr's EnterISR captures them into a
 * struct ctx.  fault_off is the WIN_DATA-relative offset (< USTACK = 0xF000)
 * for a stack-growth candidate, or USTACK for a read-only WIN_TEXT write,
 * which segflt() turns straight into SIGSEG.
 * ------------------------------------------------------------------------ */
static void pfault_trap(struct trap_frame *tf, u16 fault_off)
{
    u16 *ktop = (u16 *)(WIN_U_LINEAR + 0x400u);   /* just past u_stack top (0xD400) */
    ktop[-1] = (u16)tf->ss_v86;      /* 0xD3FE  user_ss */
    ktop[-2] = (u16)tf->esp_v86;     /* 0xD3FC  user_sp */
    ktop[-3] = fault_off;            /* 0xD3FA */
    ktop[-4] = (u16)tf->eflags;      /* 0xD3F8  user flags */
    ktop[-5] = (u16)tf->cs;          /* 0xD3F6  user cs */
    ktop[-6] = (u16)tf->eip;         /* 0xD3F4  user ip */

    tf->cs      = GUEST_CS;
    tf->eip     = (u32)g_segflt_isr_ip;
    tf->ss_v86  = GUEST_CS;
    tf->esp_v86 = 0xD400u - 12u;     /* 0xD3F4: SP at the user ip word */
    tf->eflags  = (tf->eflags & ~(u32)0x200u) | (u32)(1u << 17); /* VM=1, IF=0 */
    /* leave tf->ds, tf->es and tf->eax..edi = faulting user state for EnterISR */
}

/* --------------------------------------------------------------------------
 * Common trap dispatcher.  All V86 #GP traps land here; we decode the
 * faulting instruction and emulate it.  CR4.VME is left clear (see entry.asm),
 * so every sensitive op #GP-traps here and we emulate it ourselves.
 * ------------------------------------------------------------------------ */
void trap_dispatch(struct trap_frame *tf)
{
    u32  lin;
    u8  *p;
    u8   op;
    int  prefix_len;
    u32  instr_len;
    u16  flags16;

    trace_record(tf);

    /* Hardware IRQ: PIT (vec 0x20) -> guest IVT[8].  No EOI in VMM --
     * guest's clock_isr already does `out 20h, 20h`.  Honor the guest's virtual
     * IF: a clock tick may simply be dropped when interrupts are disabled. */
    if (tf->vector == 32) {
        if (tf->eflags & 0x200)          /* guest IF=1: deliver the tick */
            inject_v86(tf, 8, (u16)(tf->eip & 0xFFFF));
        else
            outb(0x20, 0x20);            /* guest IF=0 (e.g. in a reflected
                                          * BIOS handler): EOI here, drop tick */
        return;
    }

    /* Hardware IRQ: KBD (vec 0x21, master IRQ1) -> guest IVT[9].  No EOI in
     * VMM -- guest's kbd_isr (_common_isr) already does `out 20h, 20h`.  Honor
     * the guest's virtual IF: hold the IRQ pending if interrupts are disabled
     * (see deliver_pending). */
    if (tf->vector == 33) {
        if (tf->eflags & 0x200)
            inject_v86(tf, 9, (u16)(tf->eip & 0xFFFF));
        else
            g_pending_irq |= P_KBD;
        return;
    }

    /* Hardware IRQ: IDE primary (vec 0x2E, slave IRQ6) -> guest IVT[0x76].
     * Guest's ide_isr handles EOI to both slave and master PICs.  Honor the
     * guest's virtual IF: hold pending if interrupts are disabled. */
    if (tf->vector == 46) {
        if (tf->eflags & 0x200)
            inject_v86(tf, 0x76, (u16)(tf->eip & 0xFFFF));
        else
            g_pending_irq |= P_IDE;
        return;
    }

    /* #PF (vector 14): stack growth handler.
     * NOTE: do NOT call dump_state for user #PF -- it reads ss:sp which may be
     * the not-present page that caused the fault, triggering a triple fault. */
    if (tf->vector == 14) {
        u32 cr2 = read_cr2();
        u8  ec  = (u8)(tf->errcode & 0x7);   /* P | W | U bits */
        u16 cs  = (u16)(tf->cs & 0xFFFF);

        /* Kernel #PF: always a bug -- dump and halt. */
        if (cs == GUEST_CS) {
            dump_state(tf, "kernel #PF");
            for (;;) ;
        }

        /* User write to the read-only shared text (WIN_TEXT PTE 0x5):
         * reflect to the guest kernel, which kills the process (V6: text
         * segments are pure).  ec: P=1 and W=1. */
        if ((ec & 3) == 3 && cr2 >= 0xA0000u && cr2 < 0xB0000u) {
            pfault_trap(tf, 0xF000u);   /* >= USTACK (0xF000): segflt -> SIGSEG */
            return;
        }

        /* Protection violation (P=1): not stack growth.
         * Read or write to a not-present page (P=0) in the heap gap is handled
         * below; segflt() validates the CR2 range.  A genuine protection
         * violation on a present page is a monitor bug -- halt. */
        if (ec & 1) {
            log_puts("\r\n=== VMM #PF (protection violation) cs:ip=");
            log_hex(cs); log_putc(':'); log_hex(tf->eip & 0xFFFF);
            log_puts(" cr2="); log_hex(cr2);
            log_puts(" err="); log_hex(tf->errcode);
            log_puts("\r\n");
            for (;;) ;
        }

        /* CR2 outside WIN_DATA (linear 0xD0000..0xDEFFF): unexpected.
         * Log frame values only (no dereference: ss:sp may be unmapped, and
         * for a ring-0 fault the tail of the frame is not even pushed). */
        if (cr2 < 0xD0000u || cr2 >= 0xDF000u) {
            log_puts("\r\n=== VMM #PF outside WIN_DATA cr2=");
            log_hex(cr2);
            log_puts(" err="); log_hex(tf->errcode);
            log_puts(" cs:eip="); log_hex(tf->cs); log_putc(':'); log_hex(tf->eip);
            log_puts(" efl="); log_hex(tf->eflags);
            log_puts("\r\nss:sp="); log_hex(tf->ss_v86); log_putc(':'); log_hex(tf->esp_v86);
            log_puts(" ds="); log_hex(tf->ds);
            log_puts(" es="); log_hex(tf->es);
            log_puts(" ax="); log_hex(tf->eax);
            log_puts(" bx="); log_hex(tf->ebx);
            log_puts(" cx="); log_hex(tf->ecx);
            log_puts(" dx="); log_hex(tf->edx);
            log_puts(" si="); log_hex(tf->esi);
            log_puts(" di="); log_hex(tf->edi);
            log_puts("\r\n");
            for (;;) ;
        }

        /* Stack growth candidate: reflect as a trap on the kernel stack,
         * redirected to _segflt_isr.  Kernel segflt() does the precise
         * ndata/nstack check and either grows or kills the process. */
        pfault_trap(tf, (u16)(cr2 - 0xD0000u));
        return;
    }

    if (tf->vector != 13) {
        dump_state(tf, "unhandled vector");
        for (;;)
            ;
    }

    lin        = ((tf->cs & 0xFFFF) << 4) + (tf->eip & 0xFFFF);
    p          = (u8 *)lin;
    prefix_len = 0;

    /* Skip 0x66 (operand-size) prefix; we treat the 32-bit variants the
     * same as 16-bit for the simple sensitive ops, which is wrong in
     * detail but good enough to make forward progress. */
    if (p[0] == 0x66) {
        prefix_len = 1;
        p++;
    }
    op = p[0];

    switch (op) {
    case 0xCD: {                            /* INT imm8 */
        u8 vec = p[1];
        instr_len = 2 + prefix_len;
        if (vec == 0x80) {
            /* Advance past the int 80h before dispatch so the guest resumes at
             * the next instruction.  No hypercall reads or rewrites tf->eip. */
            tf->eip = (tf->eip + instr_len) & 0xFFFF;
            hyper_dispatch(tf);
        } else {
            /* Reflect all other software INTs to the guest IVT so the real
             * handler runs in V86. In particular BIOS int 10h renders to
             * VGA text memory -- that's what -curses displays.
             * For AH=0Eh (teletype) we also tee the byte to the serial log so
             * the guest console is still capturable headless (-nographic) for
             * automated testing; interactive -curses runs leave COM1 unwired,
             * so this copy is simply discarded and the user never sees it.
             * reflect_int sets tf->eip = IVT[vec], no further advance. */
            if (vec == 0x10 && ((tf->eax >> 8) & 0xFF) == 0x0E)
                log_putc((char)(tf->eax & 0xFF));
            reflect_int(tf, vec);
        }
        return;
    }

    case 0x9C:                              /* PUSHF / PUSHFD */
        v86_push16(tf, (u16)(tf->eflags & 0xFFFF));
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        return;

    case 0x9D:                              /* POPF / POPFD */
        flags16 = v86_pop16(tf) & ~(u16)V86_FLAGS_MASK;
        tf->eflags = (tf->eflags & 0xFFFF0000) | (u32)flags16 | 0x2u;
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        deliver_pending(tf);               /* IF may have just gone 0->1 */
        return;

    case 0xFA:                              /* CLI */
        tf->eflags &= ~(u32)0x200;
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        return;

    case 0xFB:                              /* STI */
        tf->eflags |= 0x200;
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        deliver_pending(tf);               /* IF 0->1: flush a held device IRQ */
        return;

    case 0xCF: {                            /* IRET / IRETD */
        u16 ip, cs;
        ip       = v86_pop16(tf);
        cs       = v86_pop16(tf);
        flags16  = v86_pop16(tf) & ~(u16)V86_FLAGS_MASK;
        tf->eip  = ip;
        tf->cs   = cs;
        tf->eflags = (tf->eflags & 0xFFFF0000) | (u32)flags16 | 0x2u;
        deliver_pending(tf);               /* IF may have just gone 0->1 */
        return;
    }

    case 0xEC:                              /* IN AL, DX */
        tf->eax = (tf->eax & 0xFFFFFF00) | (u32)inb((unsigned)(tf->edx & 0xFFFF));
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        return;

    case 0xED:                              /* IN AX, DX (operand-size 16 default in V86) */
        tf->eax = (tf->eax & 0xFFFF0000) | (u32)inw((unsigned)(tf->edx & 0xFFFF));
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        return;

    case 0xEE:                              /* OUT DX, AL */
        check_wild_out(tf, (unsigned)(tf->edx & 0xFFFF));
        outb((unsigned)(tf->edx & 0xFFFF), (u8)(tf->eax & 0xFF));
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        return;

    case 0xEF:                              /* OUT DX, AX */
        check_wild_out(tf, (unsigned)(tf->edx & 0xFFFF));
        outw((unsigned)(tf->edx & 0xFFFF), (u16)(tf->eax & 0xFFFF));
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        return;

    case 0xE4:                              /* IN AL, imm8 */
        tf->eax = (tf->eax & 0xFFFFFF00) | (u32)inb((unsigned)p[1]);
        tf->eip = (tf->eip + 2 + prefix_len) & 0xFFFF;
        return;

    case 0xE5:                              /* IN AX, imm8 */
        tf->eax = (tf->eax & 0xFFFF0000) | (u32)inw((unsigned)p[1]);
        tf->eip = (tf->eip + 2 + prefix_len) & 0xFFFF;
        return;

    case 0xE6:                              /* OUT imm8, AL */
        check_wild_out(tf, (unsigned)p[1]);
        outb((unsigned)p[1], (u8)(tf->eax & 0xFF));
        tf->eip = (tf->eip + 2 + prefix_len) & 0xFFFF;
        return;

    case 0xE7:                              /* OUT imm8, AX */
        check_wild_out(tf, (unsigned)p[1]);
        outw((unsigned)p[1], (u16)(tf->eax & 0xFFFF));
        tf->eip = (tf->eip + 2 + prefix_len) & 0xFFFF;
        return;

    case 0xF4:                              /* HLT (V86 _idle) */
        /* Proper semantic is "wait for next IRQ". We just skip the byte
         * and let V86 continue; the next iteration of idle()'s sti/hlt/cli
         * loop will retry until PIT fires and the ISR runs the scheduler. */
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        return;

    default:
        dump_state(tf, "unemulated #GP op");
        for (;;)
            ;
    }
}

/* --------------------------------------------------------------------------
 * P2a entry: set up IDT/TSS, then hand off to unix.com in V86.
 * ------------------------------------------------------------------------ */
void vmm_main(void)
{
    struct v86_state s;

    log_puts("monitor alive\r\n");

    pic_init();
    idt_init();
    tss_init();

    /* The user windows live in the adapter holes: WIN_TEXT @ linear 0xA0000
     * (VGA graphics, untouched in text mode) and WIN_DATA @ linear 0xD0000
     * (option-ROM area) -- so all DRAM below 640K keeps its identity mapping
     * (docs/RECLAIM_128K.md).  Drop the holes' identity PTEs now: the slots stay
     * not-present until the first HVC_SUREG (their only writer), so any stray
     * access through these linear ranges faults loudly instead of silently
     * reaching adapter space.  Verified per docs/RECLAIM_128K.md par.7: a full
     * regression ran green with both ranges not-present and the windows
     * still at their old 0x20000/0x30000 homes. */
    {
        u32 *pt0 = (u32 *)0x3000;
        u32  k;
        for (k = 0; k < 16; k++) {
            pt0[0xA0 + k] = 0;
            pt0[0xD0 + k] = 0;
        }
        flush_tlb();
    }

    /* Relocate the embedded unix.com to its expected guest address.
     * SRC=0xC000 and DST=0x10100 overlap (SRC ends at 0x10FFF, inside DST
     * range 0x10100..0x150FF), so memcpy backwards to avoid corrupting
     * unread source bytes. */
    {
        u8       *dst = (u8 *)(UNIX_DST_LINEAR + UNIX_BLOB_SIZE);
        const u8 *src = (const u8 *)(UNIX_SRC_LINEAR + UNIX_BLOB_SIZE);
        u32       n   = UNIX_BLOB_SIZE;
        while (n--)
            *--dst = *--src;
    }

    log_puts("entering V86 unix\r\n");

    s.cs     = GUEST_CS;
    s.ip     = GUEST_IP;
    s.ss     = GUEST_CS;                    /* unix.com .COM convention: SS = CS */
    s.sp     = 0xFFFE;
    s.eflags = 0x00020002;                  /* VM=1 (bit 17), reserved bit 1, IF=0 */
    s.ds     = 0;                           /* kernel STARTX sets DS = CS itself */
    s.es     = 0;
    s.fs     = 0;
    s.gs     = 0;

    v86_enter(&s);

    /* unreachable */
    for (;;)
        ;
}
