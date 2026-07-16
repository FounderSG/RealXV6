; ============================================================================
; vmm/entry.asm  --  V86 Monitor (VMM) entry stub
;
; Loaded by bootsect at physical 0x08000 (CS = 0x0800 on entry).
; Built with wlink OFFSET=0x8000 so OFFSET label resolves to linear address
; directly (no runtime +0x8000 patching needed).
;
; Path:
;   real-mode 16-bit -> GDT -> CR0.PE -> 32-bit PM -> identity paging ->
;   CR4.VME -> CR0.PG -> jmp to vmm_main (C code in _TEXT)
; ============================================================================

        .386P
        name    vmm_entry

CGROUP  group   _BOOT16, _BOOT32

        extrn   vmm_main_       : near          ; in _TEXT (compiled C)

; ----------------------------------------------------------------------------
; 16-bit real-mode entry
; ----------------------------------------------------------------------------
_BOOT16 segment dword public use16 'CODE'
        assume  cs:CGROUP, ds:CGROUP, es:CGROUP, ss:CGROUP
        org     0

        public  real_entry
real_entry:
        cli

        mov     ax, 0800h
        mov     ds, ax
        mov     es, ax
        ; Stack must NOT sit in the unix.com source blob (linear 0xC000..0x13FFF):
        ; the old ss=0x800:sp=0x7FFE put it at linear 0xFFFE, so vmm_main's pushes
        ; corrupted the kernel image (open1 @ 0xFFC6) before the memcpy relocated
        ; it.  Put it at linear 0x7000 (below vmm.bin @ 0x8000), clear of the blob.
        xor     ax, ax
        mov     ss, ax
        mov     sp, 7000h

        ; --- Initialize COM1 (115200 8N1) for boot diagnostics ----------------
        mov     dx, 3FBh
        mov     al, 80h
        out     dx, al
        mov     dx, 3F8h
        mov     al, 1
        out     dx, al
        mov     dx, 3F9h
        xor     al, al
        out     dx, al
        mov     dx, 3FBh
        mov     al, 3
        out     dx, al
        mov     dx, 3FCh
        mov     al, 3
        out     dx, al

        mov     bl, 'R'
        call    putc16

        ; A20 fast gate
        in      al, 92h
        or      al, 2
        and     al, 0FEh
        out     92h, al

        ; LGDT. With linker OFFSET=0x8000, OFFSET label resolves to the
        ; linear address; subtract 0x8000 to get the in-segment displacement
        ; that cs:[disp16] needs.
        mov     bx, OFFSET CGROUP:gdt_ptr - 8000h
        db      66h
        lgdt    fword ptr cs:[bx]

        mov     eax, cr0
        or      eax, 1
        mov     cr0, eax

        ; Far jump to 32-bit code (PM CS = 0x08). 66 EA off32 sel16
        db      66h, 0EAh
        dd      OFFSET CGROUP:pm32_start
        dw      08h

putc16:
        push    ax
        push    dx
        mov     dx, 3FDh
putc16_wait:
        in      al, dx
        test    al, 20h
        jz      putc16_wait
        mov     dx, 3F8h
        mov     al, bl
        out     dx, al
        pop     dx
        pop     ax
        ret

        align   4
        public  _gdt
_gdt:
gdt:
        dq      0
        ; 0x08  PM_CS: base=0, limit=4 GB, 32-bit
        dw      0FFFFh
        dw      0
        db      0
        db      9Ah
        db      0CFh
        db      0
        ; 0x10  PM_DS: base=0, limit=4 GB
        dw      0FFFFh
        dw      0
        db      0
        db      92h
        db      0CFh
        db      0
        ; 0x18  TSS placeholder (patched by tss_init)
        dq      0
gdt_end:

        align   2
gdt_ptr:
        dw      gdt_end - gdt - 1
        dd      OFFSET CGROUP:gdt                ; linker resolves to 0x8040

_BOOT16 ends


; ----------------------------------------------------------------------------
; 32-bit protected-mode startup (before C runtime is reachable)
; ----------------------------------------------------------------------------
_BOOT32 segment dword public use32 'CODE'
        assume  cs:CGROUP, ds:CGROUP, es:CGROUP, ss:CGROUP, fs:CGROUP, gs:CGROUP

        public  pm32_start
pm32_start:
        cld
        mov     ax, 10h
        mov     ds, ax
        mov     es, ax
        mov     fs, ax
        mov     gs, ax
        mov     ss, ax
        mov     esp, 7000h              ; clear of the unix.com blob (0xC000..0x13FFF)

        mov     bl, 'P'
        call    putc32

        ; --- Build page directory at phys 0x2000 ---------------------------
        ; Placed within the first 64 KB so it is accessible with any megs setting.
        ; 0x2000-0x2FFF: page directory; 0x3000-0x3FFF: PT0.
        mov     edi, 2000h
        mov     ecx, 1024
        xor     eax, eax
        rep     stosd

        ; --- Fill page table 0 at phys 0x3000 with identity map 0-4 MB -----
        ; PTE flags: P | RW | US  (US=1 required so V86 ring-3 can fetch)
        mov     edi, 3000h
        mov     eax, 007h
        mov     ecx, 1024
fill_pt:
        mov     [edi], eax
        add     eax, 1000h
        add     edi, 4
        loop    fill_pt

        mov     edi, 2000h
        mov     eax, 3007h              ; PD[0] = PT0 | P | RW | US
        mov     [edi], eax

        mov     eax, 2000h
        mov     cr3, eax

        ; CR4.VME LEFT CLEAR.  The monitor is built for trap-and-emulate: it
        ; enters V86 with real IF=0 and sets IF=1 only by emulating the guest's
        ; sti.  Under VME, sti sets only VIF (eflags bit 19), not the real IF, so
        ; real IF stays 0 and NO hardware IRQ ever fires -> the guest hangs in its
        ; first disk read (before "Unix Ready").  Using VME would require an
        ; interrupt-model rework (enter IF=1, gate injection on VIF, handle VIP).
        ; QEMU-2.3 TCG ignores VME anyway; Bochs needs it off to boot.
        ; mov     eax, cr4
        ; or      eax, 1
        ; mov     cr4, eax

        mov     eax, cr0
        or      eax, 80000000h
        mov     cr0, eax

        jmp     short pg_on
pg_on:
        mov     bl, 'G'
        call    putc32

        ; --- Hand off to C entry point --------------------------------------
        call    vmm_main_

halt:
        cli
        hlt
        jmp     halt

putc32:
        push    eax
        push    edx
        mov     dx, 3FDh
putc32_wait:
        in      al, dx
        test    al, 20h
        jz      putc32_wait
        mov     dx, 3F8h
        mov     al, bl
        out     dx, al
        pop     edx
        pop     eax
        ret

        align   4       ; ensures _TEXT (isr.asm) lands at DWORD boundary on both
                        ; Watcom 1.9 and 2.0 (1.9 does not insert inter-segment gaps)
_BOOT32 ends

        end     real_entry
