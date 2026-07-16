#include "vmm.h"

void log_putc(char c)
{
    while ((inb(0x3FD) & 0x20) == 0)
        ;
    outb(0x3F8, (u8)c);
}

void log_puts(const char *s)
{
    while (*s)
        log_putc(*s++);
}

void log_int(u32 n)
{
    char buf[12];
    int  i = 0;

    if (n == 0) {
        log_putc('0');
        return;
    }
    while (n) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0)
        log_putc(buf[--i]);
}

void log_hex(u32 n)
{
    int i;
    for (i = 7; i >= 0; i--) {
        u8 nib = (u8)((n >> (i * 4)) & 0xF);
        log_putc(nib < 10 ? (char)('0' + nib) : (char)('A' + nib - 10));
    }
}
