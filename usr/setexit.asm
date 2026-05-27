        name    setexit
        assume  nothing

; V6-style non-local goto: setexit()/reset(), the pre-setjmp mechanism
; used by ed.  setexit() records the current call frame and returns
; normally; a later reset() unwinds the stack back to that point, as if
; setexit() had returned a second time.  Tiny model (.COM): CS=DS=SS, so
; saving SP/BP and the return offset is enough.

DGROUP  group   _TEXT,_DATA

_DATA   segment word public 'DATA'
sav_ip  dw      0
sav_sp  dw      0
sav_bp  dw      0
_DATA   ends

_TEXT   segment word public 'CODE'
        assume  cs:_TEXT, ds:DGROUP
        public  _setexit, _reset

; void setexit(void)
_setexit proc near
        pop     ax                      ; ax = near return address
        mov     word ptr sav_ip, ax
        mov     word ptr sav_sp, sp     ; SP as it is after the return addr is popped
        mov     word ptr sav_bp, bp
        jmp     ax                      ; normal return (first time through)
_setexit endp

; void reset(void)
_reset  proc near
        mov     sp, word ptr sav_sp
        mov     bp, word ptr sav_bp
        mov     ax, word ptr sav_ip
        jmp     ax                      ; resume right after the setexit() call
_reset  endp

_TEXT   ends
        end
