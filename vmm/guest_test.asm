; ============================================================================
; vmm/guest_test.asm  --  VMM V86 guest self-test
;
; Standalone DOS .COM that runs as the guest in place of unix.com. Exercises
; the sensitive instructions and interrupt paths that the VMM must handle,
; reporting each verdict via HVC_REPORT (int 80h, AH=05). Final HVC_HALT
; carries the failure count in AL.
;
; Build: wasm -bt=DOS -mt -0 guest_test.asm -fo=guest_test.obj
;        wlink SYSTEM dos com NAME guest_test.com FILE guest_test.obj
; Run:   loaded by bootsect at sectors 33.., relocated by VMM to 0x10100,
;        entered in V86 at CS=DS_via_init=0x1000, SS=0x1000, SP=0xFFFE.
; ============================================================================

        name    guest_test
        assume  nothing

_TEXT   segment word public 'CODE'
        assume  cs:_TEXT, ds:_TEXT, es:_TEXT, ss:_TEXT
        org     0100h

        public  start

; ----------------------------------------------------------------------------
; Hypercall numbers (must match vmm/vmm.h)
; ----------------------------------------------------------------------------
HVC_NOP    equ 00h
HVC_PUTC   equ 01h
HVC_HALT   equ 03h
HVC_REPORT equ 05h
HVC_SUREG  equ 06h              ; struct-based: BX = near ptr to sureg_desc

; ============================================================================
; Entry
; ============================================================================
start   proc near
        ; VMM enters us with DS=0, ES=0, SS=CS=0x1000, SP=0xFFFE.
        ; First: install IVT[0x42] -> stub_42 (writes via DS=0).
        mov word ptr ds:[042h*4],     offset stub_42
        mov ax, cs
        mov word ptr ds:[042h*4 + 2], ax

        ; Now switch DS/ES to our segment for normal var access.
        mov ds, ax
        mov es, ax
        cli

        mov word ptr [fail_count], 0

        ; --- Run tests -----------------------------------------------------
        call t01_smoke
        call t02_pushf
        call t03_cli
        call t04_sti
        call t05_uart_lsr
        call t06_int_reflect
        call t07_bios_putc
        call t08_pushfd_prefix
        call t09_io_imm
        call t10_report_self
        call t11_timer_irq
        call t12_ide_irq
        call t13_map_window

        ; --- Halt with failure count in AL ---------------------------------
        mov al, byte ptr [fail_count]
        mov ah, HVC_HALT
        int 80h
@@:     jmp @b
start   endp

; ============================================================================
; report(id=al, expect=bx, actual=cx, fail=dx low bit)
;   Bumps fail_count if dx != 0, then fires HVC_REPORT. Preserves nothing
;   useful besides flags — caller must reload args.
; ============================================================================
report  proc near
        or dx, dx
        jz @f
        inc word ptr [fail_count]
@@:
        mov ah, HVC_REPORT
        int 80h
        ret
report  endp

; Helper: cmp_set_dx — set dx=0 if bx==cx, else dx=1. Trashes flags.
cmp_set_dx proc near
        xor dx, dx
        cmp bx, cx
        je @f
        mov dx, 1
@@:
        ret
cmp_set_dx endp

; ============================================================================
; T01 -- hypercall smoke (HVC_NOP + HVC_PUTC). Always PASS if we got here.
; ============================================================================
t01_smoke proc near
        mov ah, HVC_NOP
        int 80h
        ; emit "[T01]" via HVC_PUTC for visual progress
        mov ah, HVC_PUTC
        mov al, '['
        int 80h
        mov ah, HVC_PUTC
        mov al, 'T'
        int 80h
        mov ah, HVC_PUTC
        mov al, '0'
        int 80h
        mov ah, HVC_PUTC
        mov al, '1'
        int 80h
        mov ah, HVC_PUTC
        mov al, ']'
        int 80h

        mov al, 01h
        xor bx, bx
        xor cx, cx
        xor dx, dx
        call report
        ret
t01_smoke endp

; ============================================================================
; T02 -- PUSHF/POPF round-trip. Push 0EC5h, popf, pushf, expect 0EC7h
;        (VMM forces bit1 on in POPF emulation).
;        Bits chosen: CF|PF|AF|ZF|SF|IF|DF|OF (no TF).
; ============================================================================
t02_pushf proc near
        mov ax, 0EC5h
        push ax
        popf
        pushf
        pop ax
        mov cx, ax
        mov bx, 0EC7h
        call cmp_set_dx
        cli                     ; POPF set IF=1 -- restore safe state
        mov al, 02h
        call report
        ret
t02_pushf endp

; ============================================================================
; T03 -- CLI clears IF.
; ============================================================================
t03_cli proc near
        cli
        pushf
        pop ax
        and ax, 0200h
        mov cx, ax
        xor bx, bx
        call cmp_set_dx
        mov al, 03h
        call report
        ret
t03_cli endp

; ============================================================================
; T04 -- STI sets IF.
; ============================================================================
t04_sti proc near
        sti
        pushf
        pop ax
        and ax, 0200h
        mov cx, ax
        mov bx, 0200h
        call cmp_set_dx
        cli                     ; restore
        mov al, 04h
        call report
        ret
t04_sti endp

; ============================================================================
; T05 -- UART LSR (port 0x3FD) readable; THRE (bit 5) set on idle line.
;        Also emit 'V' to COM1 for visual.
; ============================================================================
t05_uart_lsr proc near
        mov dx, 3FDh
        in al, dx
        and al, 020h
        mov bl, al              ; save actual byte
        xor bh, bh

        ; emit 'V' for visual confirmation in serial log
        mov dx, 3F8h
        mov al, 'V'
        out dx, al

        mov cx, bx
        mov bx, 020h
        call cmp_set_dx
        mov al, 05h
        call report
        ret
t05_uart_lsr endp

; ============================================================================
; T06 -- INT n reflection via IVT[0x42]. Verify:
;        (a) stub got control and wrote magic
;        (b) SP-at-stub-entry = SP-before-INT - 6 (IP/CS/FLAGS pushed)
; ============================================================================
t06_int_reflect proc near
        mov word ptr [got_42], 0
        mov word ptr [stub_sp], 0
        mov word ptr [pre_sp], sp

        int 042h                ; reflect_int -> IVT[0x42] -> stub_42 -> IRET

        ; Check (a): magic written
        mov cx, [got_42]
        mov bx, 0BEEFh
        call cmp_set_dx
        push dx                 ; save partial fail

        ; Check (b): SP delta = 6
        mov ax, [pre_sp]
        sub ax, [stub_sp]
        mov cx, ax
        mov bx, 0006h
        call cmp_set_dx
        pop ax                  ; combine
        or dx, ax               ; either failure -> dx != 0

        mov al, 06h
        mov bx, 0BEEFh          ; cosmetic (expect)
        mov cx, [got_42]        ; cosmetic (actual)
        call report
        ret
t06_int_reflect endp

stub_42 proc near
        ; reflect_int leaves DS untouched, so DS=CS still holds here.
        mov [stub_sp], sp       ; SP right after CPU pushed flags/CS/IP
        push ax
        mov ax, 0BEEFh
        mov [got_42], ax
        pop ax
        iret
stub_42 endp

; ============================================================================
; T07 -- BIOS teletype (int 10h AH=0Eh) goes through VMM short-circuit
;        and appears as "[V]" in serial log. Visual; PASS unconditionally.
; ============================================================================
t07_bios_putc proc near
        mov ah, 0Eh
        mov al, 'V'
        mov bh, 0
        int 10h

        mov al, 07h
        xor bx, bx
        xor cx, cx
        xor dx, dx
        call report
        ret
t07_bios_putc endp

; ============================================================================
; T08 -- 0x66-prefixed PUSHFD. Current VMM emulates this as a 2-byte push
;        (treats 32-bit variant as 16-bit). Test asserts:
;        SP delta = 2 and pushed value = (eflags low 16) for cmp-with-PUSHF.
;        If VMM is later fixed to a true 4-byte push, this test must update.
; ============================================================================
t08_pushfd_prefix proc near
        cli
        ; reference: plain PUSHF value
        pushf
        pop ax
        mov si, ax              ; SI = reference flags from non-prefixed PUSHF

        mov bp, sp
        db 66h, 9Ch             ; 0x66 + 0x9C = PUSHFD (VMM treats as 16-bit)
        mov bx, bp
        sub bx, sp              ; bytes pushed
        pop ax                  ; reclaim pushed word
        mov di, ax              ; preserve pushed value for cosmetic report
        mov cx, bx
        mov bx, 2               ; expected push width (VMM today)
        call cmp_set_dx
        push dx                 ; save fail flag for width check

        mov cx, di              ; CX = value pushed by 66h-prefixed PUSHF
        mov bx, si              ; expected = reference plain PUSHF value
        call cmp_set_dx
        pop ax
        or dx, ax               ; combine: any failure -> dx != 0

        mov al, 08h
        mov bx, si              ; cosmetic expect = reference flags
        mov cx, di              ; cosmetic actual = pushed value
        call report
        ret
t08_pushfd_prefix endp

; ============================================================================
; T09 -- IN/OUT imm8 (opcodes E4/E5/E6/E7). Exercise all four; check that
;        IN AL, 21h reads back 0xFF (PIC mask set by VMM at boot).
; ============================================================================
t09_io_imm proc near
        ; E6: OUT imm8, AL (port 0x80 = POST, harmless)
        mov al, 055h
        out 80h, al

        ; E7: OUT imm8, AX
        mov ax, 0AAAAh
        out 80h, ax

        ; E5: IN AX, imm8
        in ax, 80h
        ; (no verify -- port 80 read value is platform-defined)

        ; E4: IN AL, imm8 -- read PIC mask. VMM unmasks IRQ0 (PIT),
        ; IRQ1 (KBD) and IRQ2 (cascade) at boot, leaving master mask = 0xF8.
        in al, 21h
        mov bl, al
        xor bh, bh
        mov cx, bx
        mov bx, 0F8h
        call cmp_set_dx

        mov al, 09h
        mov bx, 0F8h
        ; cx already = actual
        call report
        ret
t09_io_imm endp

; ============================================================================
; T10 -- HVC_REPORT self-test: send one explicit PASS.
;        (No FAIL injected -- we don't want to inflate fail_count.)
; ============================================================================
t10_report_self proc near
        mov al, 10h
        mov bx, 0CAFEh
        mov cx, 0CAFEh
        xor dx, dx
        call report
        ret
t10_report_self endp

; ============================================================================
; T11 -- PIT timer IRQ delivery.
;   Install IVT[8]=stub_pit, program PIT mode 3 @ 60 Hz (mirrors
;   dmr/pc.c PC_SetTickRate), STI, spin until tick_cnt >= 3, CLI, report.
;   VMM has already remapped PIC and unmasked IRQ0 at boot.
; ============================================================================
t11_timer_irq proc near
        ; Install IVT[8] = { offset stub_pit, CS } via ES override
        push es
        xor ax, ax
        mov es, ax
        mov word ptr es:[8*4],     offset stub_pit
        mov ax, cs
        mov word ptr es:[8*4 + 2], ax
        pop es

        ; Program PIT counter 0: mode 3 (square wave), binary, lo/hi byte
        mov al, 036h
        out 043h, al
        ; Divisor = 19886 (~60 Hz) -- same constant as PC_SetTickRate
        mov ax, 19886
        out 040h, al        ; low byte
        mov al, ah
        out 040h, al        ; high byte

        mov word ptr [tick_cnt], 0

        sti                 ; let IRQ0 in
        ; Spin until tick_cnt >= 2 or outer timeout. We size the loop to
        ; comfortably exceed 2 * 16.67ms = ~33ms of wall clock under QEMU.
        ; bp counts outer iterations, inner loop dec'd bx.
        mov bp, 8           ; 8 * 0xFFFF * 0x100 nops budget
@@wait_outer:
        mov cx, 0FFFFh
@@wait_mid:
        mov ax, [tick_cnt]
        cmp ax, 2
        jge @@done
        mov bx, 100h
@@wait_inner:
        nop
        dec bx
        jnz @@wait_inner
        dec cx
        jnz @@wait_mid
        dec bp
        jnz @@wait_outer
@@done:
        cli

        mov ax, [tick_cnt]
        mov cx, ax          ; actual = tick count seen
        xor dx, dx
        cmp ax, 2
        jge @@pass
        mov dx, 1
@@pass:
        mov al, 011h
        mov bx, 2           ; expect at least 2 ticks
        call report
        ret
t11_timer_irq endp

; IVT[8] handler: bump counter, EOI master PIC, return.
; Runs with DS = CS (guest segments untouched by inject_v86).
stub_pit proc near
        push ax
        inc word ptr [tick_cnt]
        mov al, 020h
        out 020h, al        ; non-specific EOI to master PIC
        pop ax
        iret
stub_pit endp

; ============================================================================
; T12 -- IDE primary IRQ delivery (slave IRQ6 -> PM vec 0x2E -> guest IVT[0x76]).
;   Install IVT[0x76]=stub_ide. Mirror dmr/ide.c idestart() to issue
;   READ sector 0 with nIEN cleared. STI; wait for ide_cnt >= 1; report.
;   Requires q-test to attach an IDE drive (use Unix360-test.img itself).
; ============================================================================
t12_ide_irq proc near
        ; Install IVT[0x76] = stub_ide via ES override
        push es
        xor ax, ax
        mov es, ax
        mov word ptr es:[076h*4],     offset stub_ide
        mov ax, cs
        mov word ptr es:[076h*4 + 2], ax
        pop es

        ; Select master drive before probing status (pc_init does this).
        mov dx, 01F6h
        mov al, 0E0h
        out dx, al

        ; idewait(0) with bounded retries -- otherwise a missing drive
        ; spins forever. Save last status seen for diagnostic report.
        mov dx, 01F7h
        mov cx, 0FFFFh
t12_wait_drdy:
        in al, dx
        mov [ide_status], al
        mov bl, al
        and bl, 0C0h            ; BSY | DRDY
        cmp bl, 040h            ; DRDY=1, BSY=0 -> ready
        je t12_drive_ready
        dec cx
        jnz t12_wait_drdy
        ; Drive never became ready -- report FAIL with the status byte.
        mov al, [ide_status]
        xor ah, ah
        mov cx, ax              ; actual = last status seen
        mov al, 012h
        mov bx, 0FE00h          ; sentinel "drive not ready" expect
        mov dx, 1
        call report
        ret
t12_drive_ready:

        mov word ptr [ide_cnt], 0

        ; --- idestart() sequence -------------------------------------------
        mov dx, 03F6h
        xor al, al              ; nIEN=0: enable IRQs
        out dx, al
        mov dx, 01F2h
        mov al, 1               ; sector count
        out dx, al
        mov dx, 01F3h
        xor al, al              ; LBA bits 0..7
        out dx, al
        mov dx, 01F4h
        out dx, al              ; LBA bits 8..15 = 0
        mov dx, 01F5h
        out dx, al              ; LBA bits 16..23 = 0
        mov dx, 01F6h
        mov al, 0E0h            ; master + LBA mode, bits 24..27 = 0
        out dx, al
        mov dx, 01F7h
        mov al, 020h            ; IDE_CMD_READ
        out dx, al

        sti
        ; Wait for IDE IRQ. Larger budget than T11 since drive may stall a bit.
        mov bp, 16
t12_outer:
        mov cx, 0FFFFh
t12_mid:
        mov ax, [ide_cnt]
        or ax, ax
        jnz t12_done
        mov bx, 100h
t12_inner:
        nop
        dec bx
        jnz t12_inner
        dec cx
        jnz t12_mid
        dec bp
        jnz t12_outer
t12_done:
        cli

        mov ax, [ide_cnt]
        mov cx, ax              ; actual = irq count
        xor dx, dx
        or ax, ax
        jnz t12_pass
        mov dx, 1
t12_pass:
        mov al, 012h
        mov bx, 1               ; expect at least 1 IDE IRQ
        call report
        ret
t12_ide_irq endp

; IVT[0x76] handler: bump counter, read status to ack drive, EOI slave+master.
; Same DS=CS invariant as stub_pit -- inject_v86 leaves segments untouched.
stub_ide proc near
        push ax
        push dx
        inc word ptr [ide_cnt]
        mov dx, 01F7h           ; read status to ack the drive-side IRQ
        in al, dx
        mov al, 020h            ; non-specific EOI
        out 0A0h, al            ; slave first
        out 020h, al            ; master second
        pop dx
        pop ax
        iret
stub_ide endp

; ============================================================================
; T13 -- VMM user-window remap (HVC_SUREG).
;   Seed two physical pages (0x40000, 0x50000) with distinct words, then point
;   WIN_TEXT (linear 0xA0000 = seg 0xA000) at each via a mode=1 sureg_desc and
;   read back through the window. The value read must follow the remap, proving
;   PTE rewrite + TLB flush work. WIN_U is pointed at its identity page (0x1D)
;   and WIN_DATA is left empty (dsize=ssize=0); neither is touched here.
;   Two verdicts: T13 (expect 0xAAAA), T14 (expect 0x5555).
; ============================================================================
t13_map_window proc near
        ; seed phys page 0x40 (seg 0x4000) and 0x50 (seg 0x5000)
        mov ax, 04000h
        mov es, ax
        mov word ptr es:[0], 0AAAAh
        mov ax, 05000h
        mov es, ax
        mov word ptr es:[0], 05555h

        ; map WIN_TEXT -> phys page 0x40, read back through seg 0xA000
        mov word ptr [sureg_taddr], 040h
        mov bx, offset sureg_buf
        mov ah, HVC_SUREG
        int 80h
        mov ax, 0A000h
        mov es, ax
        mov cx, word ptr es:[0]     ; actual
        mov bx, 0AAAAh              ; expect
        call cmp_set_dx
        mov al, 013h
        call report

        ; remap WIN_TEXT -> phys page 0x50, read again
        mov word ptr [sureg_taddr], 050h
        mov bx, offset sureg_buf
        mov ah, HVC_SUREG
        int 80h
        mov ax, 0A000h
        mov es, ax
        mov cx, word ptr es:[0]
        mov bx, 05555h
        call cmp_set_dx
        mov al, 014h
        call report

        mov ax, cs                  ; restore es = our segment
        mov es, ax
        ret
t13_map_window endp

; ============================================================================
; Data (in-segment, near labels)
; ============================================================================
fail_count      dw 0
got_42          dw 0
pre_sp          dw 0
stub_sp         dw 0
tick_cnt        dw 0
ide_cnt         dw 0
ide_status      db 0

; sureg_desc for T13/T14: {taddr, tsize, daddr, dsize, ssize, uaddr, mode}.
; mode=1 programs all three windows: WIN_TEXT gets taddr (the page under
; test), WIN_DATA is left empty (dsize=ssize=0), WIN_U is re-pointed at its
; identity page 0x1D so linear 0xD000 does not move.  daddr must be nonzero
; (any real page; unused since dsize=0) to pass the VMM's null-page tripwire.
sureg_buf:
sureg_taddr     dw 0            ; taddr: set per sub-test
                dw 1            ; tsize = 1 page
                dw 040h         ; daddr: nonzero for the tripwire, not mapped
                dw 0            ; dsize
                dw 0            ; ssize
                dw 01Dh         ; uaddr: identity page for linear 0xD000
                dw 1            ; mode = EXE (program WIN_TEXT)

_TEXT   ends

        end     start
