#ifndef _PC_H
#define _PC_H       1

#define NULL        0

#define FP_OFF(__p) ((unsigned)(__p))
#define FP_SEG(__p) ((unsigned)((unsigned long)(void __far*)(__p) >> 16))
#define MK_FP(__s,__o) (((unsigned short)(__s)):>((void __near *)(__o)))
#define disable()   { _asm cli }
#define enable()    { _asm sti }

/*
 * Console backend.  The default is the PC console: PS/2 keyboard in, VGA out.
 * Defining KL_BACKEND_UART on the compiler command line (-dKL_BACKEND_UART)
 * selects the COM1 serial console instead.
 */
#ifndef KL_BACKEND_UART
#define KL_BACKEND_KBD
#endif

typedef unsigned uint;

#pragma aux (cdecl) main;
#pragma disable_message (102, 106, 107, 113)
#pragma pack(1)

#endif
