#include "os.h"

extern void putchar(char c);

/*
 * In case console is off,
 * panicstr contains argument to last
 * call to panic.
 */
char *panicstr;

/*
 * Print an unsigned integer in base b.
 */
void printn(unsigned n, unsigned b)
{
    static char digits[] = "0123456789ABCDEF";
    unsigned a;
    if((a = n/b)!=0)
        printn(a, b);
    putchar(digits[n%b]);
}

/*
 * Scaled down version of C Library printf.
 * Only %s %l %d (==%l) %o are recognized.
 * Used to print diagnostic information
 * directly on console tty.
 * Since it is not interrupt driven,
 * all system activities are pretty much
 * suspended.
 * Printf should not be used for chit-chat.
 */
void printf(char *fmt, ...)
{
    char *s;
    unsigned *adx, c;

    adx = (unsigned *)&fmt; adx++;
loop:
    while((c = *fmt++) != '%') {
        if(c == '\0')
            return;
        putchar(c);
    }
    c = *fmt++;
    if(c == 'd' || c == 'l')
        printn(*adx, 10);
    if(c == 'o')
        printn(*adx, 8);
    if(c == 'x' || c == 'p')
        printn(*adx, 16);
    if(c == 's') {
        s = (char *)*adx;
        while((c = *s++)!=0)
            putchar(c);
    }
    adx++;
    goto loop;
}

/*
 * Panic is called on unresolvable
 * fatal errors.
 * It syncs, prints "panic: mesg" and
 * then loops.
 */
void panic(char *s)
{
    panicstr = s;
    update();
    printf("panic: %s\n", s);
    for(;;)
        idle();
}

/*
 * prdev prints a warning message of the
 * form "mesg on dev x/y".
 * x and y are the major and minor parts of
 * the device argument.
 */
void prdev(char *str, int dev)
{
    printf("%s on dev %l/%l\n", 
            str, major(dev), minor(dev));
}

/*
 * deverr prints a diagnostic from
 * a device driver.
 * It prints the device, block number,
 * and an octal word (usually some error
 * status register) passed as argument.
 */
void deverror(struct buf *bp, int o1, int o2)
{
    register struct buf *rbp;

    rbp = bp;
    prdev("err", rbp->b_dev);
    printf("bn%l er%o %o\n", rbp->b_blkno, o1, o2);
}
