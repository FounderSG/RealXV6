#ifndef VMM_H
#define VMM_H

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;

/* --------------------------------------------------------------------------
 * V86 state passed by C to v86_enter. Layout must match isr.asm.
 * ------------------------------------------------------------------------ */
#pragma pack(push, 1)
struct v86_state {
    u16 cs;      /* +0  */
    u16 ip;      /* +2  */
    u16 ss;      /* +4  */
    u16 sp;      /* +6  */
    u32 eflags;  /* +8  */
    u16 ds;      /* +12 */
    u16 es;      /* +14 */
    u16 fs;      /* +16 */
    u16 gs;      /* +18 */
};                  /* size = 20 */

/* --------------------------------------------------------------------------
 * Trap frame: what isr_common in isr.asm produces on the PM stack.
 * Layout (lowest address first):
 *   pushad order (EDI..EAX), then vector + errcode, then CPU-pushed iret
 *   frame from V86 (EIP, CS, EFLAGS, ESP_v86, SS_v86, ES, DS, FS, GS).
 * ------------------------------------------------------------------------ */
struct trap_frame {
    u32 edi, esi, ebp, esp_pushad, ebx, edx, ecx, eax;
    u32 vector;
    u32 errcode;
    u32 eip;
    u32 cs;
    u32 eflags;
    u32 esp_v86;
    u32 ss_v86;
    u32 es;
    u32 ds;
    u32 fs;
    u32 gs;
};

/* IDT 32-bit interrupt/trap gate */
struct idt_entry {
    u16 off_low;
    u16 selector;
    u8  reserved;
    u8  type_attr;
    u16 off_high;
};                  /* size = 8 */

/* LIDT/LGDT pseudo-descriptor */
struct dtr {
    u16 limit;
    u32 base;
};                  /* size = 6 */

/* 32-bit TSS core */
struct tss {
    u16 prev_tss, res0;
    u32 esp0;
    u16 ss0, res1;
    u32 esp1;
    u16 ss1, res2;
    u32 esp2;
    u16 ss2, res3;
    u32 cr3;
    u32 eip;
    u32 eflags;
    u32 eax, ecx, edx, ebx;
    u32 esp, ebp, esi, edi;
    u16 es, res4;
    u16 cs, res5;
    u16 ss, res6;
    u16 ds, res7;
    u16 fs, res8;
    u16 gs, res9;
    u16 ldt, res10;
    u16 trap;
    u16 iomap_base;
};                  /* size = 0x68 = 104 */

/* TSS extended with the VME interrupt redirection bitmap (32 B, immediately
 * preceding the I/O bitmap as required by CR4.VME) and an I/O permission
 * bitmap covering ports 0..0x3FF (128 B), plus a trailing 0xFF sentinel.
 * All zeros in P2a => every INT n traps to PM, every port is allowed. */
#define VMM_IOMAP_BYTES   128       /* covers I/O ports 0..0x3FF */

struct tss_full {
    struct tss base;                /* offset 0..103 */
    u8 int_redir[32];               /* offset 104..135 (VME redirection map) */
    u8 iomap[VMM_IOMAP_BYTES];      /* offset 136..263 (I/O permission map) */
    u8 sentinel;                    /* offset 264 (must be 0xFF) */
};
#pragma pack(pop)

/* Hypercall numbers (guest invokes via int 0x80, number in AH) */
#define HVC_NOP     0x00
#define HVC_PUTC    0x01
#define HVC_GETC    0x02
#define HVC_HALT    0x03
#define HVC_LOG     0x04
#define HVC_REPORT  0x05    /* guest test verdict: AL=id BX=expect CX=actual DX=fail */
#define HVC_SUREG         0x06  /* load user segment registers: BX=near ptr to sureg_desc */
#define HVC_SEGFLT_SETUP  0x07  /* register _segflt_isr: BX = near IP in kernel CS */

/* Linear base of the WIN_U window (pt0[0x1D]).  The guest reaches its u-area
 * (and the kernel stack at its top, 0xE000) via GUEST_CS:0xD000..0xDFFF, which
 * maps to linear 0x1D000; the VMM reaches the same physical page here. */
#define WIN_U_LINEAR   0x1D000u

/* Descriptor passed by the guest kernel to HVC_SUREG (7 x u16, packed).
 * mode=0: single-seg process, program WIN_U only.
 * mode=1: EXE process, program WIN_TEXT + sparse WIN_DATA + WIN_U. */
#pragma pack(push, 1)
struct sureg_desc {
    u16 taddr;   /* WIN_TEXT: first physical page */
    u16 tsize;   /* WIN_TEXT: page count */
    u16 daddr;   /* WIN_DATA: first physical page of data block */
    u16 dsize;   /* WIN_DATA: data page count (low slots) */
    u16 ssize;   /* WIN_DATA: stack page count (high slots) */
    u16 uaddr;   /* WIN_U: physical page */
    u16 mode;    /* 0=single-seg (WIN_U only), 1=EXE (all three windows) */
};
#pragma pack(pop)

/* --------------------------------------------------------------------------
 * Cross-language symbols. Watcom decorates C symbols with a trailing '_';
 * asm publics use the same name with '_' suffix to match.
 * ------------------------------------------------------------------------ */

/* C entry called by entry.asm after PG=1 + VME=1 */
void vmm_main(void);

/* From isr.asm */
void v86_enter(struct v86_state *s);
void load_idt(struct dtr *idtr);
void load_tss(u16 selector);
void flush_tlb(void);               /* reload CR3 to invalidate TLB after a remap */
u32  read_cr2(void);                /* read CR2 (faulting linear address) after #PF */
extern void gp_stub(void);          /* address used to populate IDT[13] */

/* Exception stubs 0..31 from isr.asm */
extern void isr0(void);  extern void isr1(void);  extern void isr2(void);
extern void isr3(void);  extern void isr4(void);  extern void isr5(void);
extern void isr6(void);  extern void isr7(void);  extern void isr8(void);
extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void);
extern void isr15(void); extern void isr16(void); extern void isr17(void);
extern void isr18(void); extern void isr19(void); extern void isr20(void);
extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void);
extern void isr27(void); extern void isr28(void); extern void isr29(void);
extern void isr30(void); extern void isr31(void);

/* Hardware IRQ stubs (after PIC remap: master->0x20, slave->0x28). */
extern void isr32(void);                 /* PIT  (master IRQ0) -> vector 0x20 */
extern void isr33(void);                 /* KBD  (master IRQ1) -> vector 0x21 */
extern void isr46(void);                 /* IDE  (slave  IRQ6) -> vector 0x2E */

/* From entry.asm: GDT base (8 entries reserved; entry 3 is TSS slot) */
extern u8 gdt[];

/* From log.c */
void log_putc(char c);
void log_puts(const char *s);
void log_int(u32 n);
void log_hex(u32 n);

/* I/O primitives -- expanded inline at each call site by Watcom */
extern u8   inb(unsigned port);
#pragma aux inb =       \
    "in  al, dx"        \
    parm  [dx]          \
    value [al];

extern void outb(unsigned port, u8 val);
#pragma aux outb =      \
    "out dx, al"        \
    parm  [dx] [al];

extern u16  inw(unsigned port);
#pragma aux inw =       \
    "in  ax, dx"        \
    parm  [dx]          \
    value [ax];

extern void outw(unsigned port, u16 val);
#pragma aux outw =      \
    "out dx, ax"        \
    parm  [dx] [ax];

/* From main.c */
void trap_dispatch(struct trap_frame *tf);

#endif
