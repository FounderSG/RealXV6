#include "os.h"

/*
 * The u-area is a VMM paging window (PT0[0x1D] @ near 0xD000): while a process
 * runs, the window maps its own core page 15, so every write to u lands
 * directly in that page.  savu therefore has nothing to copy; retu just remaps
 * the window to the incoming process's u-area page (core page 15 = p_addr+15).
 */
void savu(struct proc *p)
{
}

/*
 * Segment base the kernel uses to reach the *current* process's D-space (see the
 * user_space_io_pointer macro / copyout below).  For an EXE process this is the
 * WIN_DATA window selector, so kernel access follows the same (possibly sparse)
 * mapping the process itself uses; for a single-seg process it is the flat
 * physical segment.  Set by sureg (called from retu and exec).
 */
int user_dseg = 0;

/*
 * V6 `nofault` (conf/m40.s): a kernel fault on a bad USER address, taken while a
 * copy primitive is active, is recoverable -- the primitive returns an error and
 * the syscall sets EFAULT, rather than bringing the machine down.  `nofault` is
 * the armed flag the VMM reads on a kernel #PF (vmm/main.c); `nofault_env` is the
 * recovery context the primitive captured with save().  A single global env is
 * safe because the primitives are straight-line (no sleep) and never nest, so
 * `nofault` is never armed across a context switch (V6 treats it as a global too).
 */
int     nofault = 0;
label_t nofault_env;

/* HVC_SEGFLT_SETUP (0x07): BX = near IP of _segflt_isr in kernel code segment.
 * The VMM records it and vectors a user #PF there, on the kernel stack. */
extern void segflt_isr(void);   /* PUBLIC in dmr/m86.asm */
static void hvc_segflt_setup(int ip);
#pragma aux hvc_segflt_setup =  \
    "mov ah, 07h"               \
    "int 80h"                   \
    parm [bx]                   \
    modify [ax];

void segflt_setup(void)
{
    hvc_segflt_setup((int)segflt_isr);
}

/* HVC_PRIVFLT_SETUP (0x08): BX = near IP of _privflt_isr in kernel code segment.
 * The VMM records it and vectors a user-mode privileged-op fault there, on the
 * kernel stack -- the SIGINS analog of segflt's SIGSEG channel. */
extern void privflt_isr(void);  /* PUBLIC in dmr/m86.asm */
static void hvc_privflt_setup(int ip);
#pragma aux hvc_privflt_setup = \
    "mov ah, 08h"               \
    "int 80h"                   \
    parm [bx]                   \
    modify [ax];

void privflt_setup(void)
{
    hvc_privflt_setup((int)privflt_isr);
}

/* HVC_SUREG: BX = near ptr to sureg_desc (passed via parm [bx] pragma). */
static void hvc_sureg(struct sureg_desc *desc);
#pragma aux hvc_sureg =     \
    "mov ah, 06h"           \
    "int 80h"               \
    parm [bx]               \
    modify [ax bx];

void sureg(struct proc *p)
{
    static struct sureg_desc desc;  /* static: fixed near addr, no frame-ptr needed */

    if(p->p_tsize) {
        /* V6 sureg takes the text base from the text entry: after a swap-in
         * reload x_caddr moves and per-proc copies go stale.  p_taddr stays
         * as exec's I-space load cursor. */
        desc.taddr = p->p_textp != NULL ? p->p_textp->x_caddr : p->p_taddr;
        desc.tsize = p->p_tsize;
        desc.daddr = p->p_addr + 1;             /* data starts at slot 1 (slot 0 = u) */
        desc.dsize = p->p_dsize;
        desc.ssize = p->p_ssize;                /* real stack pages, NOT p_size-1-p_dsize:
                                                 * swgrow transiently inflates p_size as a
                                                 * swap reservation, which must not reach
                                                 * the 15-slot WIN_DATA window */
        desc.uaddr = p->p_addr;                 /* u = block slot 0 */
        desc.mode  = 1;
        user_dseg  = WDSEG;
    } else {
        desc.taddr = 0;
        desc.tsize = 0;
        desc.daddr = p->p_addr;                 /* single-seg block base: the VMM
                                                 * identity-maps it into the user
                                                 * page-table view (PT0_u) ... */
        desc.dsize = USIZE-1;                    /* ... all pages EXCEPT the u-page
                                                 * (slot USIZE-1), which stays
                                                 * invisible to user */
        desc.ssize = 0;
        desc.uaddr = p->p_addr + (USIZE-1);
        desc.mode  = 0;
        user_dseg  = p->p_addr * (PAGESIZ/16);
    }
    hvc_sureg(&desc);
}

void retu(struct proc *p)
{
    sureg(p);
}

/* external helper function defined in asm code */
extern void use_resume_stack(void);
extern void do_resume(label_t ctx);
struct proc *resume_proc;
int *resume_ctx;
void resume(struct proc *p, label_t ctx)
{
    resume_proc = p;
    resume_ctx = ctx;
    use_resume_stack();

    if(resume_proc != u.u_procp)
    {
        if(u.u_procp->p_stat != SZOMB)
            savu(u.u_procp);
        retu(resume_proc);
    }
    do_resume(resume_ctx);
}

/*
 * Switch to p's (possibly relocated) u-page and continue at ctx.  The x86
 * counterpart of V6 retu(): run the window remap on the dedicated resume
 * stack (so no live stack sits in the u-page during the remap), then reload
 * SP/BP from ctx.  Never returns.  ctx must have been captured with save()
 * BEFORE the u-page was copied, so the copy carries it; ctx is a near address
 * inside the u window, so do_resume dereferences it only after the remap and
 * reads the new page (as swtch's "interpreted in the new address space").
 *
 * Unlike resume(), there is no "same proc" fast-path skip: expand()/exec()
 * keep the same proc but move its u to a new physical page, which must always
 * be remapped.
 */
void uswitch(struct proc *p, label_t ctx)
{
    resume_proc = p;
    resume_ctx = ctx;
    use_resume_stack();
    retu(resume_proc);
    do_resume(resume_ctx);
}

/*
 * Graded SPL: the PDP-11 processor priority lives in FLAGS bits
 * 12-14 -- the VMM's g_spl shadow -- reached through getps/setps.  setps is
 * push;popf, so the priority bits reach the monitor, which delivers a device
 * IRQ iff its bus-request level exceeds the current priority.  splN sets
 * priority N and preserves IF; spl0 also forces IF=1 (enable all).  The ken/ C
 * `s=getps(); splN(); ...; setps(s)` pattern transparently gains graded
 * semantics with no change.
 */
void spl0(void)
{
    setps((getps() | 0x200) & ~0x7000);      /* priority 0, IF=1 */
}

void spl1(void)
{
    setps((getps() & ~0x7000) | (1 << 12));
}

void spl5(void)
{
    setps((getps() & ~0x7000) | (5 << 12));
}

void spl6(void)
{
    setps((getps() & ~0x7000) | (6 << 12));
}

void spl7(void)
{
    setps((getps() & ~0x7000) | (7 << 12));
}

#define user_space_io_pointer MK_FP(user_dseg, addr)

/*
 * User-space access primitives.  Each arms `nofault` around the access: on a
 * fault the VMM redirects to kfault(), whose resume() makes this save() "return"
 * nonzero, so the primitive returns the V6 fault value (-1) instead of halting.
 * setjmp caveat: on the fault path we return immediately without using the local
 * (c/w), so no `volatile` is needed; keep no needed local live across the fault.
 */
int fubyte(int addr)
{
    int c;
    if (save(nofault_env)) { nofault = 0; return -1; }
    nofault = 1;
    c = *(char far *)user_space_io_pointer & 0377;
    nofault = 0;
    return c;
}

int fuword(int addr)
{
    int w;
    if (save(nofault_env)) { nofault = 0; return -1; }
    nofault = 1;
    w = *(int far *)user_space_io_pointer;
    nofault = 0;
    return w;
}

int subyte(int addr, char ch)
{
    if (save(nofault_env)) { nofault = 0; return -1; }
    nofault = 1;
    *(char far *)user_space_io_pointer = ch;
    nofault = 0;
    return 0;
}

int suword(int addr, int value)
{
    if (save(nofault_env)) { nofault = 0; return -1; }
    nofault = 1;
    *(int far *)user_space_io_pointer = value;
    nofault = 0;
    return 0;
}

/*
 * I-space (code segment) byte access, used by xalloc to load the code segment
 * of a separated I&D program.  The shared text block is at physical p_taddr
 * (identity mapped); the running process reaches the same pages through
 * WIN_TEXT.
 */
#define user_ispace_io_pointer MK_FP(u.u_procp->p_taddr*(PAGESIZ/16), addr)

int fuibyte(int addr)
{
    return *(char far *)user_ispace_io_pointer & 0377;
}

int suibyte(int addr, char ch)
{
    *(char far *)user_ispace_io_pointer = ch;
    return 0;
}

#define PAGE_ADDR(page) MK_FP(page*(PAGESIZ/16), 0)

void copyseg(uint src, uint dst)
{
    memcpy(PAGE_ADDR(dst), PAGE_ADDR(src), PAGESIZ);
}

void clearseg(uint dst)
{
    memset(PAGE_ADDR(dst), 0, PAGESIZ);
}

int copyout(uint srcAddr, uint dstAddr, int iSize)
{
    if (save(nofault_env)) { nofault = 0; return -1; }
    nofault = 1;
    memcpy(MK_FP(user_dseg, dstAddr), MK_FP(core_cs, srcAddr), iSize);
    nofault = 0;
    return 0;
}

int copyin(uint srcAddr, uint dstAddr, int iSize)
{
    if (save(nofault_env)) { nofault = 0; return -1; }
    nofault = 1;
    memcpy(MK_FP(core_cs, dstAddr), MK_FP(user_dseg, srcAddr), iSize);
    nofault = 0;
    return 0;
}

/*
 * Reached ONLY by the VMM's kernel-#PF redirect when a copy primitive faulted
 * with `nofault` armed.  Longjmp back into that primitive so its save() returns
 * nonzero -> it returns -1.  Mirrors psig's resume(...,u_qsav) abnormal return
 * (ken/slp.c).  Entered by an eip-redirect (not a call), so no return address is
 * on the stack -- fine, resume() never returns and relocates to resume_stack
 * before restoring, so kfault's entry SP does not matter.
 */
void kfault(void)
{
    resume(u.u_procp, nofault_env);
}

/* HVC_NOFAULT_SETUP (0x09): BX = near IP of kfault, CX = near addr of nofault.
 * Registers the recovery vector and the flag address with the VMM (once, at boot). */
static void hvc_nofault_setup(int ip, int flagaddr);
#pragma aux hvc_nofault_setup = \
    "mov ah, 09h"               \
    "int 80h"                   \
    parm [bx] [cx]              \
    modify [ax];

void nofault_setup(void)
{
    hvc_nofault_setup((int)kfault, (int)&nofault);
}

typedef union {
    long i32;
    struct { int lo; int hi; } i16;
} unix_int32;

void dpadd(int x[2], int y)
{
    unix_int32 a;
    a.i16.lo = x[1];
    a.i16.hi = x[0];
    a.i32 += (uint)y;
    x[1] = a.i16.lo;
    x[0] = a.i16.hi;
}

int dpcmp(int xh, int xl, int yh, int yl)
{
    long diff;
    unix_int32 x, y;
    x.i16.hi = xh;
    x.i16.lo = xl;
    y.i16.hi = yh;
    y.i16.lo = yl;
    diff = x.i32 - y.i32;
    if(diff>512) return 512;
    else if(diff<-512) return -512;
    else return (int)diff;
}

int ldiv(int x, int y)
{ 
    return (x/y);
}

int lrem(int x, int y)
{
    return x%y; 
}

int lshift (int num[2], int bits)
{
    unix_int32 a;
    a.i16.lo = num[1];
    a.i16.hi = num[0];
    if(bits>=0) a.i32 <<= bits;
    else a.i32 >>= (-bits);
    return (int)a.i32;
}

void outport(unsigned port, unsigned val)
{
    _asm mov dx, port
    _asm mov ax, val
    _asm out dx, ax
}

void outportb(unsigned port, unsigned char val)
{
    _asm mov dx, port
    _asm mov al, val
    _asm out dx, al
}

unsigned inport(unsigned port)
{
    _asm mov dx,port
    _asm in  ax,dx
}

unsigned char inportb(unsigned port)
{
    _asm mov dx,port
    _asm in  al,dx
    _asm xor ah,ah
}

void idle(void)
{
    _asm sti
    _asm hlt
    _asm cli
}

void putchar(char c)
{ 
#if defined(KL_BACKEND_UART)
    uart_putc(c);
#else    
    bios_putc(c);
#endif
}

void setvect(int vectnumber, uint vectfunc)
{
    uint far *pvect;
    
    pvect = (uint far *)MK_FP(0x0000, vectnumber * 4);
    pvect[0] = vectfunc;
    pvect[1] = core_cs;
}

#define  TICK_T0_8254_CWR             0x43       /* 8254 PIT Control Word Register address.            */
#define  TICK_T0_8254_CTR0            0x40       /* 8254 PIT Timer 0 Register address.                 */
#define  TICK_T0_8254_CTR1            0x41       /* 8254 PIT Timer 1 Register address.                 */
#define  TICK_T0_8254_CTR2            0x42       /* 8254 PIT Timer 2 Register address.                 */

#define  TICK_T0_8254_CTR0_MODE3      0x36       /* 8254 PIT Binary Mode 3 for Counter 0 control word. */
#define  TICK_T0_8254_CTR2_MODE0      0xB0       /* 8254 PIT Binary Mode 0 for Counter 2 control word. */
#define  TICK_T0_8254_CTR2_LATCH      0x80       /* 8254 PIT Latch command control word                */

void PC_SetTickRate(void)
{
    uint count;                                           /* count = (2386360L / freq + 1) >> 1            */
    count = 19886;                                        /* 60Hz                                          */
    outportb(TICK_T0_8254_CWR,  TICK_T0_8254_CTR0_MODE3); /* Load the 8254 with desired frequency          */
    outportb(TICK_T0_8254_CTR0, count & 0xFF);            /* Low  byte                                     */
    outportb(TICK_T0_8254_CTR0, (count >> 8) & 0xFF);     /* High byte                                     */
}

void isr_savuar(int ds, int es, int dx, int cx, int bx, int ax,
    int di, int si, int bp, int ip, int cs, int flags)
{
    (void)es; (void)ds; (void)si; (void)di; (void)bp;
    (void)ip; (void)cs; (void)flags;

    u.u_ar0[R0] = ax;
    u.u_ar0[R1] = bx;
    u.u_ar0[R2] = cx;
    u.u_ar0[R3] = dx;
}

void isr_router(int irq, int mode)
{
    switch(irq)
    {
        case 0: clock(mode); break;
        case 1: ideintr(); break;
        case 2: kbdintr(); break;
        case 3: uartintr(); break;
    }
}

void check_runrun(void)
{
loop:
    spl7();
    if(runrun == 0)
    {
        return;
    }
    spl0();
    swtch();
    goto loop;
}

void trap0(int ds, int es, int dx, int cx, int bx, int ax,
    int di, int si, int bp, int ip, int cs, int flags,
    int arg0, int arg1, int arg2)
{
    (void)es; (void)ds; (void)si; (void)di; (void)bp;
    (void)ip; (void)cs; (void)flags;

    u.u_ar0[R0] = ax;
    u.u_ar0[R1] = bx;
    u.u_ar0[R2] = cx;
    u.u_ar0[R3] = dx;
    u.u_arg[0] = arg0;
    u.u_arg[1] = arg1;
    u.u_arg[2] = arg2;
    u.u_dirp = u.u_arg[0];
    u.u_error = 0;
}

void trap_epilogue(void)
{
    struct ctx far *ctx;
    ctx = (struct ctx far *)MK_FP(uret_ss, uret_sp);
    ctx->ax = u.u_ar0[R0];
    ctx->bx = u.u_ar0[R1];
    ctx->cx = u.u_ar0[R2];
    ctx->dx = u.u_ar0[R3];
}

/*
 * Copy the faulting 12-word interrupt frame (struct ctx) that _segflt_isr
 * built on the kernel stack down onto the user stack at usp-24, and point the
 * return SS:SP (uret_ss/uret_sp) at it.  On return _segflt_isr reloads
 * that SS:SP and IRETs through the frame: for a restart this re-runs the
 * faulting instruction; for a caught signal psig() first duplicates the frame
 * once more and vectors the trampoline.  Both need the frame on the user
 * stack because a V86 IRET does not reload SS:SP.
 */
void dupframe(unsigned kctx, unsigned uss, unsigned usp)
{
    int far *dst = (int far *)MK_FP(uss, usp - 24);
    int *src = (int *)kctx;            /* near: the ctx in the u page (DS-relative) */
    int i;

    for(i = 0; i < 12; i++)
        dst[i] = src[i];
    uret_ss = uss;
    uret_sp = usp - 24;
}

void sendsig(int p)
{
    int far *ustack;
    struct ctx far *ctx;

    uret_sp -= 24;                /* duplicate interrupt stack frame */
    ustack = (int far *)MK_FP(uret_ss, uret_sp);
    memcpy(ustack, &ustack[12], 24);
    ctx = (struct ctx far *)ustack;
    ctx->ip = SIGTRAMP_EXE;
    ctx->si = p;
    ctx = (struct ctx far *)&ustack[12];
    /* ip, cs, flag = ax, flag, return address */
    ctx->cs = ctx->flag;
    ctx->flag = ctx->ip;
    ctx->ip = ctx->ax;
}

#define PC_CLOCK_INTR   8
#define PC_KBD_INTR     9
#define PC_UART_INTR    12
#define PC_IDE_INTR     0x76
#define PC_UNIX_INTR    0x81

void pc_init(void)
{
    core_cs = FP_SEG(&core_cs);

#ifdef KL_BACKEND_UART
    uart_init();
    setvect(PC_UART_INTR, (uint)uart_isr);
#endif
#ifdef KL_BACKEND_KBD
    kbd_init();
    setvect(PC_KBD_INTR, (uint)kbd_isr);
#endif
    setvect(PC_IDE_INTR, (uint)ide_isr);
    outportb(0x1f6, 0xe0 | (0<<4));  /* select disk 0 */

    setvect(PC_CLOCK_INTR, (uint)clock_isr);
    PC_SetTickRate();

    setvect(PC_UNIX_INTR, (uint)trap_isr);
}
