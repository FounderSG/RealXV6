#include "os.h"

void savu(struct proc *p)
{
    memcpy(MK_FP(p->p_addr*(PAGESIZ/16), USTACK), &u, sizeof(u));
}

void retu(struct proc *p)
{
    memcpy(&u, MK_FP(p->p_addr*(PAGESIZ/16), USTACK), sizeof(u));
}

void spl0(void)
{
    core_spl = 0;
    enable();
}

void spl1(void)
{
    core_spl = 1;
    disable();
}

void spl5(void)
{
    core_spl = 5;
    disable();
}

void spl6(void)
{
    core_spl = 6;
    disable();
}

void spl7(void)
{
    core_spl = 7;
    disable();
}

#define user_space_io_pointer MK_FP(u.u_procp->p_addr*(PAGESIZ/16), addr)

int fubyte(int addr)
{
    return *(char far *)user_space_io_pointer & 0377;
}

int fuword(int addr)
{
    return *(int far *)user_space_io_pointer;
}

int subyte(int addr, char ch)
{
    *(char far *)user_space_io_pointer = ch;
    return 0;
}

int suword(int addr, int value)
{
    *(int far *)user_space_io_pointer = value;
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

void copyout(uint srcAddr, uint dstAddr, int iSize)
{
    memcpy(MK_FP(u.u_procp->p_addr*(PAGESIZ/16), dstAddr), MK_FP(core_cs, srcAddr), iSize);
}

/*
 * User to kernel, the mirror of copyout, same (src, dst, size) order.
 */
void copyin(uint srcAddr, uint dstAddr, int iSize)
{
    memcpy(MK_FP(core_cs, dstAddr), MK_FP(u.u_procp->p_addr*(PAGESIZ/16), srcAddr), iSize);
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
