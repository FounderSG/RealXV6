PUBLIC  _clock_isr, _trap_isr, _getps, _setps, _save, _use_resume_stack, _do_resume, _memcpy, _memset
PUBLIC  _bios_getc, _bios_putc, _move_to_user_mode, _ide_isr, _kbd_isr, _uart_isr, _common_isr
EXTRN   _main: near, _u: near
EXTRN   _isr_savuar: near, _isr_router: near, _clock: near, _check_runrun: near
EXTRN   _trap0: near, _trap: near, _rkintr: near

DGROUP  GROUP _TEXT,_DATA,_BSS,_BSSEND

    .MODEL  TINY
    .CODE
    ORG   100h
STARTX          PROC    NEAR
    mov     ax, cs
    mov     ds, ax
    mov     es, ax
    mov     ax, offset DGROUP:_u
    add     ax, 1020
    mov     sp, ax

; Reset uninitialized data area
    xor     ax, ax
    mov     di, offset DGROUP: bdata@
    mov     cx, offset DGROUP: edata@
    sub     cx, di
    cld
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

SwitchToKernelStack MACRO
    mov cx, sp
    mov dx, ss
    mov ax, cs
    mov ss, ax
    mov ax, offset DGROUP:_u
    add ax, 1024
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
    push si
    push di
    call near ptr _isr_router
    pop di
    pop si
    or si, si
    jz @f
    call near ptr _check_runrun
    SwitchToUserStack
@@:
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
    ExitISR

_getps  proc    near
    pushf
    pop  ax
    ret
_getps  endp

_setps  proc    near
    push bp
    mov bp, sp
    mov ax, [bp+4]
    pop bp
    and ax, 0200h
    jz @f
    sti
    ret
@@:
    cli
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

_move_to_user_mode proc    near
    mov bp, sp
    mov dx, [bp+2]
    cli
    mov ss, dx
    mov sp, 0f000h
    sti
    mov ds, dx
    push dx
    mov ax, 0100h
    push ax
    retf
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

_BSS    SEGMENT word public 'BSS'
bdata@          label   byte
    db  128 dup (?)
resume_stack    label   word
_BSS   ENDS

_BSSEND         SEGMENT BYTE PUBLIC 'BSSEND'
edata@          label   byte
_BSSEND         ENDS

    END STARTX
