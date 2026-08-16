#include "vmm.h"

/* --------------------------------------------------------------------------
 * Static placements live in the binary (_DATA so they're zero-init by the
 * loader; we don't have a BSS-zeroing startup yet).
 * ------------------------------------------------------------------------ */
static struct idt_entry idt[256]   = { 0 };
static struct dtr       idtr       = { 0, 0 };
static struct tss_full  tss0       = { 0 };
static u16              g_segflt_isr_ip  = 0;  /* _segflt_isr offset, set by HVC_SEGFLT_SETUP */
static u16              g_privflt_isr_ip = 0;  /* _privflt_isr offset, set by HVC_PRIVFLT_SETUP */
static u16              g_kfault_ip      = 0;  /* kfault offset, set by HVC_NOFAULT_SETUP */
static u32              g_nofault_addr   = 0;  /* linear addr of the guest `nofault` flag */

/* Post-mortem dump; defined below but called earlier (hyper_dispatch, #PF). */
static void dump_state(struct trap_frame *tf, const char *why);

/* --------------------------------------------------------------------------
 * Mode bit (the PDP-11 PSW current-mode analog).  g_kmode == 1 iff the guest
 * kernel -- or BIOS code invoked by the kernel -- is executing; == 0 iff a
 * guest *user* process is executing.  The CPU treats kernel and user V86 code
 * identically (both CPL 3), so this software bit is what lets the VMM apply
 * different policy to the two: user-mode privileged ops become signals, the
 * kernel's are honored.
 *
 * Boot starts in the guest kernel (STARTX), so g_kmode = 1.  guest_test.com
 * never leaves kmode, so q-test is unaffected by any of this.
 *
 * enter_kmode() is invoked on every path that vectors the guest onto a kernel
 * handler (hardware IRQ inject, INT reflect, user #PF, privflt); URET (int 82h)
 * is the sole path back to user (sets g_kmode = 0).
 * ------------------------------------------------------------------------ */
static u8 g_kmode = 1;

static void enter_kmode(void)
{
    g_kmode = 1;
    /* The hardware consequences of the mode (I/O bitmap and page directory) are
     * applied once at the VMM exit sync, keyed off the final g_kmode -- see
     * trap_dispatch. */
}

/* --------------------------------------------------------------------------
 * Virtual PDP-11 processor priority (graded SPL).  g_spl (0-7) is the
 * VMM's shadow of the guest's PS priority; the live EFLAGS never carries it
 * (V86_FLAGS_MASK forces bits 12-14 to 0), so the priority lives ONLY in
 * g_spl and in the bits-12-14 field of stack flag images.  A device IRQ is
 * delivered iff the guest IF is set AND the device's bus-request level exceeds
 * the current priority (dev_pri > g_spl) -- the PDP-11 grant condition.  Device
 * priorities copy the V6 hardware: KW11-L clock BR6, RK11 disk BR5, KL11
 * console BR4.  User mode always runs at priority 0 (set by URET).
 * ------------------------------------------------------------------------ */
static u8 g_spl = 0;

#define PIT_PRI  6      /* KW11-L line clock  (master IRQ0) */
#define IDE_PRI  5      /* RK11 disk          (slave  IRQ6) */
#define KBD_PRI  4      /* KL11 console       (master IRQ1) */

/* I/O permission bitmap base values written to tss0.base.iomap_base at the exit
 * sync.  ALLOW points the base at tss0.iomap (an all-zero 128-byte map -> guest
 * ports 0..0x3FF permitted, so the kernel's disk PIO runs natively at full
 * speed).  DENY sets the base beyond the TSS limit, so EVERY guest IN/OUT #GPs
 * and the umode whitelist turns it into SIGINS.  The CPU re-reads the base on
 * each I/O permission check, so a plain store takes effect on the next V86
 * instruction -- no LTR reload. */
#define IOMAP_ALLOW  ((u16)((u8 *)&tss0.iomap - (u8 *)&tss0))
#define IOMAP_DENY   ((u16)sizeof(tss0))

/* Physical page-table roots (the KISA/UISA analog).  PD_k/PT0_k are built by
 * entry.asm: identity map 0-4MB plus the kernel's WIN_TEXT/WIN_DATA/WIN_U
 * windows -- the kernel keeps full reach.  PD_u/PT0_u are built here in
 * vmm_main and rebuilt by every HVC_SUREG so the USER view maps ONLY the
 * running process's own text/data/stack (plus the VMM's supervisor pages, U=0,
 * so ring-0 traps can still be delivered while CR3=PD_u).  A V86 user process
 * is thereby confined to its own address space. */
#define PD_K_PHYS   0x2000u
#define PT0_K_PHYS  0x3000u
#define PD_U_PHYS   0x4000u
#define PT0_U_PHYS  0x5000u

/* mode field (0 = single-seg icode, 1 = EXE) of the most recent HVC_SUREG.  The
 * #PF stack-growth branch is gated on ==1, so a mode=0 process forging
 * DS=0xD000 cannot trip a bogus grow(). */
static u16 g_sureg_mode = 0;

/* Rebuild PT0_u (the user page-table view) from scratch: clear all 1024 PTEs,
 * lay in the VMM's own supervisor pages (U=0) so ring-0 trap delivery keeps
 * working while CR3=PD_u, then map the current process's windows (U=1).  Called
 * from vmm_main (initial) and from every HVC_SUREG (a mode=0/1 process image).
 * Rebuilding wholesale avoids stale-entry bugs when a range moves. */
static void build_pt0u(u16 taddr, u16 tsize, u16 daddr, u16 dsize, u16 ssize, u16 mode)
{
    u32 *pt0u = (u32 *)PT0_U_PHYS;
    u32  k;

    for (k = 0; k < 1024; k++)
        pt0u[k] = 0;

    /* VMM infrastructure, identity-mapped SUPERVISOR (U=0): the CPU reads the
     * IDT/GDT/TSS/handlers (vmm.bin, pages 0x08-0x0B) and pushes the ring-0
     * trap frame (page 0x0F, esp0=0xFFFE) while CR3=PD_u.  U=0 makes them
     * present for trap delivery yet untouchable from V86 CPL 3 -- the one place
     * the U/S bit separates the VMM from the guest (never guest kernel from
     * guest user; both are CPL 3). */
    pt0u[0x08] = 0x08000u | 0x3u;
    pt0u[0x09] = 0x09000u | 0x3u;
    pt0u[0x0A] = 0x0A000u | 0x3u;
    pt0u[0x0B] = 0x0B000u | 0x3u;
    pt0u[0x0F] = 0x0F000u | 0x3u;

    if (mode == 1) {
        /* EXE: WIN_TEXT (0xA0..) RO user (0x5), sparse WIN_DATA (0xD0..) RW
         * user (0x7) -- mirroring the kernel view, but with NO 0x1D entry: the
         * u-area / kernel stack is invisible to user, as in V6. */
        for (k = 0; k < tsize; k++)
            pt0u[0xA0 + k] = ((u32)(taddr + k) << 12) | 0x5u;
        for (k = 0; k < dsize; k++)
            pt0u[0xD0 + k] = ((u32)(daddr + k) << 12) | 0x7u;
        for (k = 0; k < ssize; k++)
            pt0u[0xD0 + (16 - ssize) + k] = ((u32)(daddr + dsize + k) << 12) | 0x7u;
    } else {
        /* mode=0 (icode): identity-map the process block RW user, EXCLUDING the
         * u-page (sureg passes dsize = USIZE-1).  dsize == 0 (proc[0]'s STARTX
         * descriptor, which never enters user mode) leaves only the VMM pages. */
        for (k = 0; k < dsize; k++)
            pt0u[daddr + k] = ((u32)(daddr + k) << 12) | 0x7u;
    }
}

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
/* new_spl: the priority to adopt (PDP-11 "new PS from the interrupt vector")
 * for a hardware IRQ, or -1 for a software INT reflection (leave g_spl). */
static void inject_v86(struct trap_frame *tf, u8 guest_vec, u16 return_ip, int new_spl)
{
    u16 sp, ss;
    u16 ret_cs, ret_flags;
    u16 *ivt;
    u32 stack_lin;

    /* Vectoring onto an IVT handler always enters the guest kernel (this covers
     * every hardware-IRQ delivery, deliver_pending, and reflect_int -- both the
     * kernel's BIOS calls and a user process's int 81h syscall). */
    enter_kmode();

    sp        = (u16)(tf->esp_v86 & 0xFFFF);
    ss        = (u16)(tf->ss_v86 & 0xFFFF);
    stack_lin = (u32)(ss << 4);

    ret_cs    = (u16)(tf->cs & 0xFFFF);
    ret_flags = (u16)(tf->eflags & 0xFFFF);
    /* Stash the current virtual priority into the pushed flags image (bits
     * 12-14), so the handler's IRET restores it; the live eflags never carries
     * it (V86_FLAGS_MASK forces those bits to 0). */
    ret_flags = (ret_flags & ~0x7000u) | ((u16)g_spl << 12);

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
    tf->eflags &= ~(u32)(0x100 | 0x200);    /* clear TF, IF (handler at IF=0) */

    /* Hardware IRQ: adopt the device's bus-request level as the new priority. */
    if (new_spl >= 0)
        g_spl = (u8)new_spl;
}

static void reflect_int(struct trap_frame *tf, u8 vec)
{
    inject_v86(tf, vec, (u16)((tf->eip + 2) & 0xFFFF), -1);
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
#define P_PIT  0x04
static u8 g_pending_irq = 0;

static void deliver_pending(struct trap_frame *tf)
{
    if (!(tf->eflags & 0x200))           /* guest IF still 0: keep holding all */
        return;
    /* Deliver at most one held IRQ, highest bus-request level first, each gated
     * on the graded-SPL rule (dev_pri > g_spl).  The injected handler's IRET
     * lowers the priority and re-runs deliver_pending, draining the rest. */
    if ((g_pending_irq & P_PIT) && PIT_PRI > g_spl) {
        g_pending_irq &= ~P_PIT;
        inject_v86(tf, 8, (u16)(tf->eip & 0xFFFF), PIT_PRI);
        return;
    }
    if ((g_pending_irq & P_IDE) && IDE_PRI > g_spl) {
        g_pending_irq &= ~P_IDE;
        inject_v86(tf, 0x76, (u16)(tf->eip & 0xFFFF), IDE_PRI);
        return;
    }
    if ((g_pending_irq & P_KBD) && KBD_PRI > g_spl) {
        g_pending_irq &= ~P_KBD;
        inject_v86(tf, 9, (u16)(tf->eip & 0xFFFF), KBD_PRI);
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

    /* Defense in depth: hypercalls are kernel-only.  trap_dispatch already
     * routes a user int 80h to SIGINS (never reaching here), so a !g_kmode
     * hypercall means the mode bookkeeping is broken -- fail loud. */
    if (!g_kmode) {
        dump_state(tf, "hypercall from user mode");
        for (;;)
            ;
    }

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

            /* WIN_DATA sparse (linear 0xD0000, pt0[0xD0..0xDF]): data pages at
             * low slots, stack pages at high slots, heap gap in between left
             * not-present.  All 16 slots are usable (UDPAGES=16); USTACK's top
             * 2 bytes (0xFFFE-0xFFFF) are reserved, so slot 0xDF is real stack. */
            if ((u32)dsize + ssize > 16) {
                log_puts("HVC_SUREG d+s>16\r\n");
                for (;;) ;
            }
            for (k = 0; k < dsize; k++)
                pt0[0xD0 + k] = ((u32)(daddr + k) << 12) | 0x7u;
            for (k = dsize; k < (u32)(16 - ssize); k++)
                pt0[0xD0 + k] = 0;
            for (k = 0; k < ssize; k++)
                pt0[0xD0 + (16 - ssize) + k] = ((u32)(daddr + dsize + k) << 12) | 0x7u;
        }

        /* Rebuild the user page-table view to match, and record the mode for
         * the #PF stack-growth gate.  flush_tlb below reloads PD_k (HVC_SUREG
         * runs in kmode); the PD_u changes take effect when URET next loads it. */
        g_sureg_mode = mode;
        build_pt0u(taddr, tsize, daddr, dsize, ssize, mode);

        flush_tlb();
        tf->eax = (tf->eax & 0xFFFF0000);
        return;
    }

    case HVC_SEGFLT_SETUP:
        g_segflt_isr_ip = (u16)(tf->ebx & 0xFFFF);
        tf->eax = (tf->eax & 0xFFFF0000u);
        return;

    case HVC_PRIVFLT_SETUP:
        g_privflt_isr_ip = (u16)(tf->ebx & 0xFFFF);
        tf->eax = (tf->eax & 0xFFFF0000u);
        return;

    case HVC_NOFAULT_SETUP:
        g_kfault_ip = (u16)(tf->ebx & 0xFFFF);
        /* kernel is tiny-model: DGROUP base = CS = GUEST_CS (STARTX does mov ds,cs) */
        g_nofault_addr = ((u32)GUEST_CS << 4) + (u32)(tf->ecx & 0xFFFF);
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
 * the top of the u-area kernel stack (WIN_U, top = 0xE000), the user iret
 * frame {ip,cs,flags} plus {fault_off, user_sp, user_ss}, then vector CS:IP to
 * _segflt_isr with SS:SP = kernel stack.  The faulting user's GP registers and
 * DS/ES are left live in tf so _segflt_isr's EnterISR captures them into a
 * struct ctx.  fault_off is the WIN_DATA-relative offset (< USTACK = 0xFFFE)
 * for a stack-growth candidate, or USTACK for a read-only WIN_TEXT write,
 * which segflt() turns straight into SIGSEG.
 * ------------------------------------------------------------------------ */
static void pfault_trap(struct trap_frame *tf, u16 fault_off)
{
    u16 *ktop = (u16 *)(WIN_U_LINEAR + 0x1000u);  /* just past kernel stack top (0xE000) */
    enter_kmode();                   /* a user #PF vectors onto the kernel stack */
    ktop[-1] = (u16)tf->ss_v86;      /* 0xDFFE  user_ss */
    ktop[-2] = (u16)tf->esp_v86;     /* 0xDFFC  user_sp */
    ktop[-3] = fault_off;            /* 0xDFFA */
    ktop[-4] = (u16)tf->eflags;      /* 0xDFF8  user flags */
    ktop[-5] = (u16)tf->cs;          /* 0xDFF6  user cs */
    ktop[-6] = (u16)tf->eip;         /* 0xDFF4  user ip */

    tf->cs      = GUEST_CS;
    tf->eip     = (u32)g_segflt_isr_ip;
    tf->ss_v86  = GUEST_CS;
    tf->esp_v86 = 0xE000u - 12u;     /* 0xDFF4: SP at the user ip word */
    tf->eflags  = (tf->eflags & ~(u32)0x200u) | (u32)(1u << 17); /* VM=1, IF=0 */
    /* leave tf->ds, tf->es and tf->eax..edi = faulting user state for EnterISR */
}

/* --------------------------------------------------------------------------
 * Reflect a user-mode privileged/illegal operation to the guest kernel as a
 * trap on the kernel stack -- the SIGINS channel, the x86 stand-in for the
 * PDP-11 illegal-instruction/BPT/EMT/IOT traps.  Frame construction is
 * identical to pfault_trap (same kernel-stack layout at the top of WIN_U),
 * except the word at 0xD3FA carries a trap TYPE code instead of a fault offset,
 * and it vectors to _privflt_isr.
 *
 * The pushed ip is the FAULTING instruction (tf->eip is not advanced): a
 * caught-and-returned SIGINS re-executes and re-faults, matching PDP-11
 * re-execution semantics; the default action (core+exit) never returns anyway.
 *
 * Trap type codes (word at 0xD3FA):  1 = privileged operation (everything ->
 * SIGINS), 2 = breakpoint (SIGTRC), 3 = arithmetic (SIGFPT),
 * 4 = illegal instruction (SIGINS, #UD).
 * ------------------------------------------------------------------------ */
static void privflt_trap(struct trap_frame *tf, u16 type)
{
    u16 *ktop = (u16 *)(WIN_U_LINEAR + 0x1000u);  /* just past kernel stack top (0xE000) */
    enter_kmode();                   /* the fault vectors onto the kernel stack */
    ktop[-1] = (u16)tf->ss_v86;      /* 0xDFFE  user_ss */
    ktop[-2] = (u16)tf->esp_v86;     /* 0xDFFC  user_sp */
    ktop[-3] = type;                 /* 0xDFFA  trap type code */
    ktop[-4] = (u16)tf->eflags;      /* 0xDFF8  user flags */
    ktop[-5] = (u16)tf->cs;          /* 0xDFF6  user cs */
    ktop[-6] = (u16)tf->eip;         /* 0xDFF4  user ip (faulting instr, not advanced) */

    tf->cs      = GUEST_CS;
    tf->eip     = (u32)g_privflt_isr_ip;
    tf->ss_v86  = GUEST_CS;
    tf->esp_v86 = 0xE000u - 12u;     /* 0xDFF4: SP at the user ip word */
    tf->eflags  = (tf->eflags & ~(u32)0x200u) | (u32)(1u << 17); /* VM=1, IF=0 */
    /* leave tf->ds, tf->es and tf->eax..edi = faulting user state for EnterISR */
}

/* --------------------------------------------------------------------------
 * Common trap dispatcher.  All V86 #GP traps land here; we decode the
 * faulting instruction and emulate it.  CR4.VME is left clear (see entry.asm),
 * so every sensitive op #GP-traps here and we emulate it ourselves.
 *
 * This is the inner body; trap_dispatch() wraps it with the CR3/iomap exit
 * sync (§4.8).  g_kmode may flip inside here (enter_kmode / URET), and the
 * wrapper applies the hardware consequences once, keyed off the final value.
 * ------------------------------------------------------------------------ */
static void trap_dispatch_inner(struct trap_frame *tf)
{
    u32  lin;
    u8  *p;
    u8   op;
    int  prefix_len;
    u32  instr_len;
    u16  flags16;

    trace_record(tf);

    /* Hardware IRQ: PIT (vec 0x20, priority 6) -> guest IVT[8].  Deliver iff the
     * graded rule allows (IF && 6 > g_spl); the guest's clock_isr does its own
     * EOI.  Otherwise EOI the periodic timer here and remember one owed tick
     * (coalesced), delivered when the priority next drops below 6. */
    if (tf->vector == 32) {
        if ((tf->eflags & 0x200) && PIT_PRI > g_spl)
            inject_v86(tf, 8, (u16)(tf->eip & 0xFFFF), PIT_PRI);
        else {
            outb(0x20, 0x20);            /* EOI so the periodic timer keeps running */
            g_pending_irq |= P_PIT;      /* hold at most one owed tick */
        }
        return;
    }

    /* Hardware IRQ: KBD (vec 0x21, priority 4) -> guest IVT[9].  When held we do
     * NOT EOI -- the PIC in-service bit blocks a re-trap until the guest's own
     * kbd_isr runs and EOIs; re-injected once IF && 4 > g_spl. */
    if (tf->vector == 33) {
        if ((tf->eflags & 0x200) && KBD_PRI > g_spl)
            inject_v86(tf, 9, (u16)(tf->eip & 0xFFFF), KBD_PRI);
        else
            g_pending_irq |= P_KBD;
        return;
    }

    /* Hardware IRQ: IDE primary (vec 0x2E, priority 5) -> guest IVT[0x76].
     * Guest's ide_isr handles EOI to both slave and master PICs; held with no
     * EOI otherwise. */
    if (tf->vector == 46) {
        if ((tf->eflags & 0x200) && IDE_PRI > g_spl)
            inject_v86(tf, 0x76, (u16)(tf->eip & 0xFFFF), IDE_PRI);
        else
            g_pending_irq |= P_IDE;
        return;
    }

    /* #PF (vector 14).  NOTE: do NOT call dump_state for a USER #PF -- it reads
     * ss:sp which may be the not-present page that caused the fault, triggering
     * a triple fault. */
    if (tf->vector == 14) {
        u32 cr2 = read_cr2();
        u8  ec  = (u8)(tf->errcode & 0x7);   /* P | W | U bits */

        /* Kernel #PF.  V6 nofault: a copy primitive touching a bad user address
         * arms `nofault`; redirect to kfault, which longjmps out of the primitive
         * (-> it returns -1 -> the syscall sets EFAULT) and the machine stays up.
         * With nofault disarmed it is a genuine kernel bug -- dump and halt (the
         * V6 panic analog), which keeps the post-mortem for real bugs. */
        if (g_kmode) {
            if (g_nofault_addr && *(u16 *)g_nofault_addr) {
                tf->cs  = GUEST_CS;
                tf->eip = (u32)g_kfault_ip;
                tf->ds  = GUEST_CS;   /* copyin's memcpy left DS = user window;
                                       * kfault needs kernel DS to reach u/env/resume */
                /* leave SS:SP = the faulting KERNEL stack; resume() unwinds it via
                 * nofault_env.  No WIN_U frame like pfault_trap, no enter_kmode --
                 * we are already in kmode on the kernel stack. */
                return;
            }
            dump_state(tf, "kernel #PF");
            for (;;) ;
        }

        /* User #PF.  A not-present fault inside the sparse WIN_DATA window of an
         * EXE process is a stack-growth candidate: reflect to segflt(), which
         * does the precise ndata/nstack check and either grows the stack (and
         * restarts) or kills the process.  The g_sureg_mode==1 gate stops a
         * mode=0 process that forged DS=0xD000 from tripping a bogus grow(). */
        if (cr2 >= 0xD0000u && cr2 < 0xDF000u && !(ec & 1) && g_sureg_mode == 1) {
            pfault_trap(tf, (u16)(cr2 - 0xD0000u));
            return;
        }

        /* Everything else is a segmentation violation: a not-present access to
         * kernel / VMM / IVT / page-table / another process's memory, a
         * present-page protection fault (incl. a write to the read-only shared
         * text, or a V86 CPL-3 touch of a U=0 VMM page), or an out-of-window
         * access.  segflt() sees fa >= USTACK and posts SIGSEG (default
         * core+exit, catchable on the user stack). */
        pfault_trap(tf, 0xFFFEu);   /* >= USTACK (0xFFFE): segflt -> SIGSEG */
        return;
    }

    /* User-mode CPU exceptions: deliver as V6 signals instead of halting
     * the machine.  A divide error (#DE) or #UD is a genuine fault on its own
     * IDT vector; int3/INTO usually arrive as #GP and are typed in the umode
     * whitelist below, but we map their vectors here too in case the CPU
     * delivers them directly.  In kernel mode any such exception is a bug and
     * falls through to the post-mortem halt (the V6 panic analog). */
    if (!g_kmode && tf->vector != 13) {
        u16 type;
        switch (tf->vector) {
        case 0:  type = 3; break;   /* #DE divide     -> SIGFPT */
        case 4:  type = 3; break;   /* #OF overflow   -> SIGFPT */
        case 3:  type = 2; break;   /* #BP breakpoint -> SIGTRC */
        default: type = 4; break;   /* #UD (6) and any other -> SIGINS */
        }
        privflt_trap(tf, type);
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

    /* User-mode default-deny whitelist.  A user process only ever #GP-traps on
     * a sensitive instruction, and the only ones it is allowed to execute are
     * the syscall INT (0xCD, sub-gated to int 81h below), PUSHF (0x9C) and POPF
     * (0x9D, IF pinned below).  Everything else -- int 80h/82h, other INT n
     * (BIOS), CLI/STI/IRET/HLT, all IN/OUT/INS/OUTS forms (now #GP-trapping
     * because the umode iomap denies every port), and any unknown opcode -- is
     * a privileged operation and becomes SIGINS. */
    if (!g_kmode && op != 0xCD && op != 0x9C && op != 0x9D) {
        u16 type = 1;                       /* default: privileged op -> SIGINS */
        if (op == 0xCC)      type = 2;      /* int3 -> SIGTRC (breakpoint) */
        else if (op == 0xCE) type = 3;      /* into -> SIGFPT (overflow) */
        privflt_trap(tf, type);
        return;
    }

    switch (op) {
    case 0xCD: {                            /* INT imm8 */
        u8 vec = p[1];
        instr_len = 2 + prefix_len;

        if (vec == 0x80) {                  /* hypercall */
            if (!g_kmode) {                 /* user int 80h: a privileged op */
                privflt_trap(tf, 1);
                return;
            }
            /* Advance past the int 80h before dispatch so the guest resumes at
             * the next instruction.  No hypercall reads or rewrites tf->eip. */
            tf->eip = (tf->eip + instr_len) & 0xFFFF;
            hyper_dispatch(tf);
            return;
        }

        if (vec == URET_VECTOR) {           /* int 82h: URET, kernel -> user */
            u16 ip, cs, fl;
            if (!g_kmode) {                 /* user int 82h: URET forgery */
                privflt_trap(tf, 1);
                return;
            }
            /* Pop the iret-style frame the kernel's UExitISR left on SS:SP,
             * exactly like the IRET emulation below. */
            ip = v86_pop16(tf);
            cs = v86_pop16(tf);
            fl = v86_pop16(tf) & ~(u16)V86_FLAGS_MASK;
            tf->eip    = ip;
            tf->cs     = cs;
            /* User always runs interruptible (V6 user PS priority 0): force
             * IF=1 in addition to the reserved bit. */
            tf->eflags = (tf->eflags & 0xFFFF0000) | (u32)fl | 0x2u | 0x200u;
            g_kmode = 0;
            g_spl   = 0;                    /* user runs at PDP-11 priority 0 */
            /* The exit path loads the user CR3 and denies I/O here. */
            deliver_pending(tf);            /* a held device IRQ re-enters kmode */
            return;
        }

        /* Any other software INT.  From user mode only the syscall vector
         * (int 81h) is allowed through -- it reflects to IVT[0x81], and
         * enter_kmode (inside inject_v86) flips into kernel mode.  Every other
         * user INT n (BIOS int 10h/13h, etc.) is a privileged op -> SIGINS. */
        if (!g_kmode && vec != 0x81) {
            privflt_trap(tf, 1);
            return;
        }

        /* Reflect the INT to the guest IVT so the real handler runs in V86. In
         * particular BIOS int 10h renders to VGA text memory -- that's what
         * -curses displays.  For AH=0Eh (teletype) we also tee the byte to the
         * serial log so the guest console is still capturable headless
         * (-nographic) for automated testing; interactive -curses runs leave
         * COM1 unwired, so this copy is simply discarded and the user never
         * sees it.  reflect_int sets tf->eip = IVT[vec], no further advance. */
        if (vec == 0x10 && ((tf->eax >> 8) & 0xFF) == 0x0E)
            log_putc((char)(tf->eax & 0xFF));
        reflect_int(tf, vec);
        return;
    }

    case 0x9C: {                           /* PUSHF / PUSHFD */
        u16 img = (u16)(tf->eflags & 0xFFFF);
        if (g_kmode)                       /* stash the virtual priority */
            img = (img & ~0x7000u) | ((u16)g_spl << 12);
        v86_push16(tf, img);
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        return;
    }

    case 0x9D: {                           /* POPF / POPFD */
        u16 raw = v86_pop16(tf);
        if (g_kmode)                       /* extract the virtual priority,
                                            * BEFORE stripping V86_FLAGS_MASK */
            g_spl = (u8)((raw >> 12) & 7);
        flags16 = raw & ~(u16)V86_FLAGS_MASK;
        tf->eflags = (tf->eflags & 0xFFFF0000) | (u32)flags16 | 0x2u;
        if (!g_kmode)
            tf->eflags |= 0x200u;          /* user always runs interruptible:
                                            * a user popf can never clear IF
                                            * (required by the _callsig
                                            * trampoline, which restores IF=1) */
        tf->eip = (tf->eip + 1 + prefix_len) & 0xFFFF;
        deliver_pending(tf);               /* IF/priority may have just dropped */
        return;
    }

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
        u16 ip, cs, raw;
        ip       = v86_pop16(tf);
        cs       = v86_pop16(tf);
        raw      = v86_pop16(tf);
        if (g_kmode)                        /* restore the virtual priority,
                                             * incl. do_resume's context-switch
                                             * iret -- BEFORE stripping the mask */
            g_spl = (u8)((raw >> 12) & 7);
        flags16  = raw & ~(u16)V86_FLAGS_MASK;
        tf->eip  = ip;
        tf->cs   = cs;
        tf->eflags = (tf->eflags & 0xFFFF0000) | (u32)flags16 | 0x2u;
        deliver_pending(tf);               /* IF/priority may have just dropped */
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
 * Trap-dispatch wrapper: apply the CR3 entry rule, run the emulator, then apply
 * the CR3/iomap exit sync (§4.8) once, keyed off the final g_kmode.  g_kmode may
 * flip several times inside trap_dispatch_inner (e.g. URET sets 0, then a held
 * IRQ's inject_v86 sets 1 again); only the value the guest actually resumes
 * under matters, so we sync here, at the single exit, not at each transition.
 *
 *   kmode  -> iomap ALLOW, CR3 = PD_k (ports run native; kernel has full reach)
 *   umode  -> iomap DENY,  CR3 = PD_u (every IN/OUT #GPs; process is confined)
 *
 * Net cost: a kernel trap (the common case -- every spl/pushf/cli) does ZERO
 * CR3 loads; only a real user<->kernel transition pays one.  The iomap store is
 * cheap (no TLB flush) and takes effect on the next V86 instruction.
 * ------------------------------------------------------------------------ */
void trap_dispatch(struct trap_frame *tf)
{
    /* Entry rule: a trap from user leaves CR3 = PD_u, but the emulator must
     * reach the kernel view -- pfault_trap/privflt_trap write the kernel-stack
     * frame through WIN_U (0x1D000), absent from PD_u -- so load PD_k before
     * touching any of it (i.e. before trap_dispatch_inner's trace_record). */
    if (!g_kmode)
        load_cr3(PD_K_PHYS);

    trap_dispatch_inner(tf);

    /* Exit rule: flip the I/O bitmap and page-table view to the mode the guest
     * resumes under.  Only a real transition loads CR3; a kernel trap leaves
     * PD_k in place. */
    tss0.base.iomap_base = g_kmode ? IOMAP_ALLOW : IOMAP_DENY;
    if (!g_kmode)
        load_cr3(PD_U_PHYS);
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

    /* Build the user page-table view (PD_u @ 0x4000 -> PT0_u @ 0x5000).  Only
     * directory entry 0 (the first 4 MB, where all guest memory lives) is
     * present.  PT0_u starts with just the VMM's supervisor pages; the first
     * HVC_SUREG fills in the running process's windows.  PD_u is loaded only on
     * a return to user mode, which happens well after the first HVC_SUREG, so
     * PT0_u is always populated by then -- this initial fill just keeps PD_u
     * well-formed. */
    {
        u32 *pd_u = (u32 *)PD_U_PHYS;
        u32  k;
        for (k = 0; k < 1024; k++)
            pd_u[k] = 0;
        pd_u[0] = PT0_U_PHYS | 0x7u;    /* P|RW|US; per-page US in PT0_u decides */
        build_pt0u(0, 0, 0, 0, 0, 0);
    }

    /* Hardening: mark the VMM's own low pages SUPERVISOR (U=0) in the KERNEL
     * view too (they are already U=0 in the user view via build_pt0u).  The
     * guest kernel is also V86 CPL 3, so this stops a buggy guest kernel from
     * corrupting the page tables (0x02-0x05), the VMM binary (0x08-0x0B), or the
     * ring-0 stack (0x0F) with a wild pointer.  The VMM itself runs CPL 0 and
     * still reaches them.  Page 0 (IVT) stays U=1 -- the kernel writes vectors
     * there at boot; page 1 (low memory) is left as-is. */
    {
        u32 *pt0 = (u32 *)PT0_K_PHYS;
        u32  k;
        for (k = 0x02; k <= 0x0F; k++)
            pt0[k] = (k << 12) | 0x3u;   /* identity, P|RW, U=0 (clear US) */
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
