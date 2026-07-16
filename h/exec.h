/*
 * Header of a separated I&D ("EXE") executable.
 *
 * Kept close to the V6 a.out 8-word header.  An EXE has a distinct code
 * segment and data segment: the kernel runs it with CS = code base and
 * DS = SS = ES = data base.  On the VMM the code lives in the WIN_TEXT
 * window (CS = 0xA000) and the data in WIN_DATA (DS = 0xD000); the code
 * bakes in no segment value, so it is position independent and the windows
 * supply a uniform base regardless of physical placement.
 *
 * On disk the file is laid out as:
 *      [ 16-byte struct exec ][ a_text bytes of code ][ a_data bytes of data ]
 * Neither BSS nor the stack is stored.  The data segment is laid out, low to
 * high, as:
 *      [ initialized data (a_data) ][ bss (a_bss) ][ stack (a_stack) ]
 * The kernel loads a_data at DS:0, zeroes the bss+stack that follow, and sets
 * SP = a_data + a_bss + a_stack -- so the stack is at the HIGH end and grows
 * down (classic layout; the MMU window bounds it, see WIN_DATA in vmm/).
 */
struct exec
{
    int a_magic;    /* magic number, A_MAGIC for separated I&D */
    int a_text;     /* size of code segment in bytes */
    int a_data;     /* size of initialized data in bytes */
    int a_bss;      /* size of bss (uninitialized data) in bytes */
    int a_syms;     /* size of symbol table (unused, 0) */
    int a_entry;    /* entry point: offset into the code segment */
    int a_stack;    /* user stack size in bytes (the high end of the data seg) */
    int a_flag;     /* unused */
};

#define A_MAGIC 0411    /* separated instruction & data (V6 0411) */

/*
 * Offset, within a process's code segment, of the "jmp _callsig" signal
 * trampoline that psig() vectors a caught signal through.  The EXE startup
 * makes it the first instruction of the code segment.
 */
#define SIGTRAMP_EXE 0x0000
