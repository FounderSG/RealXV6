        name    cstart_exe
        assume  nothing

; Emit the __DOSseg__ marker so wlink uses DOS segment ordering and places
; DGROUP in its own frame after the code -- giving a true separated I&D
; layout (CS != DS, data offsets relative to the data segment).
        DOSSEG

; Startup stub for a separated I&D ("EXE") program: code segment (_TEXT) and
; data segment (DGROUP) are distinct.  Unlike the .COM stub, _TEXT is NOT part
; of DGROUP, there is no "org 100h", and the stub never loads a segment
; register from a baked-in segment value -- the kernel sets CS, DS, ES and SS
; in the initial frame.  Keeping the code free of segment values is what makes
; it position independent (so exe2aout.py can assert zero relocations).
;
; CLASSIC (stack-high) layout, for the VMM where the MMU window bounds the
; segment: DGROUP is laid out [ data ][ bss ][ STACK ] with the stack as the
; LAST segment, so the kernel loads a_data at DS:0, zeroes bss+stack, and sets
; SP = a_data+a_bss+a_stack (top).  (The no-MMU realmode-exe branch instead
; put the stack FIRST at DGROUP:0; here the window catches overflow, so the
; conventional layout is fine.)

; STKSIZE bytes of user stack, reserved as the LAST DGROUP segment.  It is
; uninitialized and trailing, so wlink keeps it out of the load image (BSS-like
; minalloc); the kernel allocates and zeroes it.
ifndef STKSIZE
STKSIZE equ 1000h
endif

DGROUP group _NULL,CONST,STRINGS,_DATA,DATA,CONST2,_BSS,STACK

; An empty leading CODE segment anchors the code in the linker's AUTO group
; (DOSSEG orders 'CODE' first), so _TEXT is not folded into DGROUP and DGROUP
; gets its own frame after the code -- a true separated I&D layout.
BEGTEXT segment word public 'CODE'
BEGTEXT ends

_TEXT   segment word public 'CODE'

        extrn   _main                 : near

        assume  cs:_TEXT
        assume  ds:DGROUP
        assume  es:DGROUP

        public  sigtramp, startx, _syscall, _callsig

; The signal trampoline MUST be the first instruction of the code segment:
; psig() vectors a caught signal to code offset SIGTRAMP_EXE (= 0).
sigtramp:
        jmp     _callsig

; Program entry (the a.out header records this offset).  DS/ES/SS are already
; the data segment on entry; just clear BSS and call main.
startx  proc near
        mov     cx, offset DGROUP:_end   ; end of _BSS (start of stack area)
        mov     di, offset DGROUP:_edata ; start of _BSS
        sub     cx, di                   ; bytes of BSS to clear
        xor     al, al
        cld
        rep     stosb

        call    _main
        mov     dx, 1                    ; SYS_exit
        int     81h
startx  endp

_callsig proc near
        call    si                  ; call signal handler (si = handler offset)
        pop     ax                  ; don't touch ds
        pop     ax                  ; don't touch es
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        pop     di
        pop     si
        pop     bp
        pop     ax                  ; ip, cs, flag = ax, flag, return address
        popf
        ret
_callsig endp

_syscall proc near
        pop cx     ; return address
        pop dx     ; syscall number
        pop ax     ; r0
        int 0x81
        or dx, dx
        jz @f      ; no error
        mov _errno, dx
        mov dx, -1
@@:
        mov _r0, ax
        mov _r1, bx
        mov _r3, dx
        sub sp, 4
        push cx
        ret
_syscall endp

_TEXT   ends

; A FAR_DATA-class segment puts the code in the linker's AUTO group (with
; DOSSEG), so it is NOT folded into DGROUP.
FAR_DATA segment word public 'FAR_DATA'
FAR_DATA ends

; First DGROUP segment (class BEGDATA): null-pointer guard at DGROUP:0.  With
; DOSSEG this anchors DGROUP to its own frame after the code, so data offsets
; are data-segment relative and CS != DS -- a true separated I&D layout.
_NULL   segment para public 'BEGDATA'
        dw      0,0,0,0,0,0,0,0
_NULL   ends

CONST   segment word public 'DATA'
CONST   ends

STRINGS segment word public 'DATA'
STRINGS ends

_DATA   segment word public 'DATA'
_DATA   ends

DATA    segment word public 'DATA'
        extrn _errno: word, _r0: word, _r1: word, _r3: word
DATA    ends

CONST2  segment word public 'DATA'
CONST2  ends

_BSS    segment word public 'BSS'
        extrn   _edata                  : byte  ; end of DATA (start of BSS)
        extrn   _end                    : byte  ; end of BSS (start of stack)
_BSS    ends

; The user stack: LAST DGROUP segment.  Trailing + uninitialized, so wlink
; emits no image bytes for it (minalloc); the kernel allocates and zeroes it
; and sets SP to its top.  A STACK-class segment must exist or the linker
; collapses code and data into one segment.
STACK   segment para stack 'STACK'
        db      STKSIZE dup(?)
STACK   ends

        end     startx
