PUBLIC  _clock_isr, _trap_isr, _getps, _setps, _save, _use_resume_stack, _do_resume, _memcpy, _memset
PUBLIC  _bios_getc, _bios_putc, _move_to_user_mode, _ide_isr, _kbd_isr, _uart_isr, _common_isr
PUBLIC  _segflt_isr, _privflt_isr
EXTRN   _main: near
EXTRN   _isr_savuar: near, _isr_router: near, _clock: near, _check_runrun: near
EXTRN   _trap0: near, _trap: near, _rkintr: near
EXTRN   _segflt: near, _privflt: near
EXTRN   _intr_ps: word

DGROUP  GROUP _TEXT,_DATA,_BSS,_BSSEND

; Fixed near aperture for the current process's u-area (see h/user.h).
U_AREA  EQU   0D000h

    .MODEL  TINY
    .CODE
    ORG   100h
STARTX          PROC    NEAR
    mov     ax, cs
    mov     ds, ax
    mov     es, ax

; Map WIN_U (PT0[0x1D] @ linear 0xD000) to proc[0]'s u-area page BEFORE SP
; moves into the window.  proc[0].p_addr = core_cs/256 = 0x10, so its u-area
; (core page 15) = 0x10+15 = 0x1F.  Build a sureg_desc on the current stack
; (SS=CS=0x1000, SP=0xFFFE) and call HVC_SUREG; int 80h does not push to the
; guest stack, so this is safe.
    sub     sp, 14          ; allocate sureg_desc (7 words) on the entry stack
    mov     bp, sp
    xor     ax, ax
    mov     [bp+0], ax      ; taddr = 0
    mov     [bp+2], ax      ; tsize = 0
    mov     [bp+4], ax      ; daddr = 0
    mov     [bp+6], ax      ; dsize = 0
    mov     [bp+8], ax      ; ssize = 0
    mov     ax, 001Fh
    mov     [bp+10], ax     ; uaddr = 0x1F (proc[0] u-area physical page)
    xor     ax, ax
    mov     [bp+12], ax     ; mode = 0 (single-seg: WIN_U only)
    mov     bx, bp          ; near ptr to descriptor (DS=0x1000)
    mov     ah, 06h         ; HVC_SUREG
    int     80h
    add     sp, 14          ; restore entry stack

    mov     ax, U_AREA
    add     ax, 4092
    mov     sp, ax

; Reset uninitialized data area
    xor     ax, ax
    mov     di, offset DGROUP: bdata@
    mov     cx, offset DGROUP: edata@
    sub     cx, di
    cld
    rep     stosb

; Zero the relocated u-area: it left BSS, so the clear above misses it.
; Nothing is pushed yet and rep stosb does not touch the stack.
    xor     ax, ax
    mov     di, U_AREA
    mov     cx, 1000h
    rep     stosb

; Call main function
    call    _main
    jmp     $
STARTX          ENDP

EnterISR MACRO
    push bp
    push si
    push di
    push ax
    push bx
    push cx
    push dx
    push es
    push ds
    mov ax, cs
    mov ds, ax
    ENDM

ExitISR MACRO
    pop ds
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    pop di
    pop si
    pop bp
    iret
    ENDM

; Return to USER mode.  Identical register pops to ExitISR, but ends in the URET
; hypercall (int 82h) instead of iret: the VMM pops the {ip,cs,flags} frame
; exactly as it would for iret AND flips out of kernel mode (g_kmode=0), so
; user-mode protection takes effect on the returned-to process.  A plain iret
; here would leave the VMM in kernel mode with every protection silently off.
UExitISR MACRO
    pop ds
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    pop di
    pop si
    pop bp
    int 82h
    ENDM

SwitchToKernelStack MACRO
    mov cx, sp
    mov dx, ss
    mov ax, cs
    mov ss, ax
    mov ax, U_AREA
    add ax, 4096
    mov sp, ax
    push dx
    push cx
    ENDM

SwitchToUserStack MACRO
    cli
    pop cx
    pop dx
    mov sp, cx
    mov ss, dx
    ENDM

_clock_isr:
    EnterISR
    ; Capture the interrupted PS image for clock()'s callout/lbolt priority
    ; gate.  The iret frame the VMM pushed sits just above the 9-word EnterISR
    ; block: ip@[sp+18], cs@[sp+20], flags@[sp+22]; the VMM stashed the
    ; interrupted virtual priority in flags bits 12-14.
    mov bp, sp
    mov ax, [bp+22]
    mov word ptr _intr_ps, ax
    xor di, di
    jmp _common_isr

_ide_isr:
    EnterISR
    mov al, 20h        ; send EOI to slave PIC
    out 0A0h, al
    mov di, 1
    jmp _common_isr

_kbd_isr:
    EnterISR
    mov di, 2
    jmp _common_isr

_uart_isr:
    EnterISR
    mov di, 3
    jmp _common_isr

_common_isr   proc    near
    mov al, 20h        ; send EOI
    out 20h, al
    mov ax, cs
    mov si, ss
    sub si, ax
    jz @f              ; ss == cs intr in kernel mode
    call near ptr _isr_savuar
    SwitchToKernelStack
@@:
    sti                ; on the kernel stack now: run the handler interruptible so
                       ; a higher-BR IRQ can nest (graded rule dev_pri>g_spl gates)
    push si
    push di
    call near ptr _isr_router
    pop di
    pop si
    or si, si
    jz common_kret          ; ss == cs: interrupted the kernel -> plain iret,
                            ; VMM stays in kernel mode (nested return)
    call near ptr _check_runrun
    SwitchToUserStack
    UExitISR                ; interrupted user mode -> URET flips VMM to user
common_kret:
    ExitISR
_common_isr   endp

_trap_isr:
    EnterISR
    call near ptr _trap0
    SwitchToKernelStack
    sti
    call near ptr _trap
    call near ptr _check_runrun
    SwitchToUserStack
    UExitISR                ; syscall always returns to user mode

_getps  proc    near
    pushf
    pop  ax
    ret
_getps  endp

; setps(int) -- restore the full PS via popf, so the VMM's popf emulation both
; sets IF and extracts the virtual priority (FLAGS bits 12-14) into g_spl.
; getps stays pushf (the VMM's pushf emulation stashes g_spl into the image).
_setps  proc    near
    push bp
    mov bp, sp
    mov ax, [bp+4]
    pop bp
    push ax
    popf
    ret
_setps  endp

_save   proc    near
    push    bp
    mov bp,sp
    push    si
    push    di
    mov ax, 1
    push    ax
    push    bx
    push    cx
    push    dx
    push    es
    push    ds
    mov ax, ds
    mov es, ax
    mov di,word ptr [bp+4]
    mov si, sp
    mov cx, 10
    cld
    rep movsw
    mov bx,word ptr [bp+4]
    mov word ptr [bx+20], cs
    pushf
    pop ax
    mov word ptr [bx+22], ax
    mov word ptr [bx+24], ss
    mov ax,bp
    add ax,4
    mov word ptr [bx+26], ax
    pop ds
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    pop di
    pop si
    pop bp
    xor ax,ax
    ret
_save   endp

_use_resume_stack   proc    near
    cli
    pop ax
    mov sp, offset DGROUP:resume_stack
    push ax
    ret
_use_resume_stack   endp

_do_resume   proc    near
    pop     bx              ; discard return address
    pop     bx              ; bx = ctx
    mov es, [bx+24]         ; es = ctx->ss
    mov di, [bx+26]         ; di = ctx->sp
    sub di, 24
    mov si, bx
    mov cx, 12
    cld
    rep movsw               ; copy saved registers onto target stack
    mov ax, es
    mov ss, ax
    sub di, 24
    mov sp, di
    pop ds                  ; restore registers in reverse save() order
    pop es
    pop dx
    pop cx
    pop bx
    pop ax
    pop di
    pop si
    pop bp
    iret
_do_resume   endp


; Drop proc[1] into user mode to run icode.  V6 does this by manipulating the
; PS previous-mode bits and returning through the trap; here the return-to-user
; must go through URET (int 82h) so the VMM leaves kernel mode -- a plain retf
; does not trap, so it would enter user with every protection silently off.
; Push an iret-style frame {ip=0x100, cs=user seg, flags=IF} and URET it.
_move_to_user_mode proc    near
    mov bp, sp
    mov dx, [bp+2]
    cli
    mov ss, dx
    mov sp, 0f000h
    sti
    mov ds, dx
    mov ax, 0200h           ; user flags: IF=1 (user always interruptible)
    push ax
    push dx                 ; user cs
    mov ax, 0100h
    push ax                 ; user ip (icode entry)
    int 82h                 ; URET: VMM pops {ip,cs,flags}, g_kmode=0, -> user
_move_to_user_mode endp

; void memcpy(void far *dst, const void far *src, int n)
_memcpy proc    near
    push bp
    mov bp,sp
    push si
    push di
    push ds
    les di, [bp+4]
    lds si, [bp+8]
    mov cx,[bp+12]
    shr cx,1
    cld
    rep movsw
    jnc @f
    movsb
@@:
    pop ds
    pop di
    pop si
    pop bp
    ret
_memcpy endp

; void memset(void far *addr, int c, unsigned len)
_memset proc    near
    push bp
    mov bp,sp
    push di
    les di, [bp+4]
    mov al, [bp+8]
    mov ah, al
    mov cx, [bp+10]
    shr cx,1
    cld
    rep stosw
    jnc @f
    stosb
@@:
    pop di
    pop bp
    ret
_memset endp

; int bios_getc(void)
_bios_getc  proc    near
    mov ah, 1
    int 16h
    jnz @f
    mov ax, -1
    ret
@@:
    mov ah, 0
    int 16h
    mov ah, 0
    ret
_bios_getc  endp

; void bios_putc(char c)
_bios_putc  proc    near
    push bp
    mov bp, sp
    mov al, byte ptr [bp+4]
    mov ah, 0eh
    mov bh, 0
    int 10h
    pop bp
    ret
_bios_putc  endp

; VMM redirect target for a user #PF (stack growth or segmentation violation).
; Entered via VMM iretd on the KERNEL stack (the user SP may be inside the
; not-present stack gap, so it cannot be used).  SS = GUEST_CS, DS/ES and the
; GP registers hold the faulting user state, IF=0, VM=1.  The VMM pushed, at
; the top of the u-area kernel stack (growing down from 0xE000):
;   0xDFF4 ip  0xDFF6 cs  0xDFF8 flags   (user iret frame; SP enters here)
;   0xDFFA fault_off  0xDFFC user_sp  0xDFFE user_ss
; EnterISR completes a struct ctx below the iret frame; segflt() grows the
; stack (restart) or posts SIGSEG, leaving the return SS:SP in
; uret_ss/uret_sp and a ctx to IRET through on the user stack.
_segflt_isr     proc    near
    EnterISR                        ; push bp,si,di,ax,bx,cx,dx,es,ds; ds=cs
    mov     bp, sp                  ; bp -> ctx (ds@0..bp@16, ip@18,cs@20,flag@22,
                                    ;            fault_off@24, user_sp@26, user_ss@28)
    push    bp                      ; kctx (near ptr to the ctx)
    push    word ptr [bp+28]        ; user_ss
    push    word ptr [bp+26]        ; user_sp
    push    word ptr [bp+24]        ; fault_off
    call    near ptr _segflt        ; segflt(fault_off, user_sp, user_ss, kctx)
    ; segflt set uret_ss/uret_sp = return ss:sp; return to user through it.
    mov     sp, U_AREA + 4096 - 4   ; -> uret_sp slot (0xDFFC)
    SwitchToUserStack               ; ss:sp = uret_ss:uret_sp
    UExitISR                        ; pop ctx regs; URET to user
_segflt_isr     endp

; VMM redirect target for a user-mode privileged/illegal operation (the x86
; stand-in for the PDP-11 illegal-instruction / BPT / EMT / IOT traps): the
; SIGINS channel.  Entered via VMM iretd on the KERNEL stack exactly like
; _segflt_isr, but the word at 0xDFFA is a trap TYPE code (see privflt_trap)
; instead of a fault offset, and the pushed user ip is the faulting instruction
; itself (re-executed on a caught return, per PDP-11 semantics).
_privflt_isr    proc    near
    EnterISR                        ; push bp,si,di,ax,bx,cx,dx,es,ds; ds=cs
    mov     bp, sp                  ; bp -> ctx (ds@0..bp@16, ip@18,cs@20,flag@22,
                                    ;            type@24, user_sp@26, user_ss@28)
    push    bp                      ; kctx (near ptr to the ctx)
    push    word ptr [bp+28]        ; user_ss
    push    word ptr [bp+26]        ; user_sp
    push    word ptr [bp+24]        ; trap type code
    call    near ptr _privflt       ; privflt(type, user_sp, user_ss, kctx)
    ; privflt set uret_ss/uret_sp = return ss:sp; return to user through it.
    mov     sp, U_AREA + 4096 - 4   ; -> uret_sp slot (0xDFFC)
    SwitchToUserStack               ; ss:sp = uret_ss:uret_sp
    UExitISR                        ; pop ctx regs; URET to user
_privflt_isr    endp

_BSS    SEGMENT word public 'BSS'
bdata@          label   byte
    db  128 dup (?)
resume_stack    label   word
_BSS   ENDS

_BSSEND         SEGMENT BYTE PUBLIC 'BSSEND'
edata@          label   byte
_BSSEND         ENDS

    END STARTX
