; ============================================================================
; vmm/isr.asm  --  Interrupt stubs, V86 entry, and CR-loading helpers.
;
; Symbols use Watcom default decoration (trailing '_') so that C externs
; match.  All code is 32-bit protected-mode (USE32).
; ============================================================================

        .386P
        name    vmm_isr

        extrn   trap_dispatch_ : near       ; C handler

_TEXT   segment dword public use32 'CODE'

; ----------------------------------------------------------------------------
; Exception stubs 0..31.  CPU pushes errcode only for vectors 8,10,11,12,13,
; 14,17; for all others we push a dummy 0 to keep the trap_frame uniform.
; Each stub pushes its vector number then jumps to isr_common.
; ----------------------------------------------------------------------------
; Watcom wasm doesn't have NASM-style macros, so each stub is spelled out.

        public  isr0_, isr1_, isr2_, isr3_, isr4_, isr5_, isr6_, isr7_
        public  isr8_, isr9_, isr10_, isr11_, isr12_, isr13_, isr14_, isr15_
        public  isr16_, isr17_, isr18_, isr19_, isr20_, isr21_, isr22_, isr23_
        public  isr24_, isr25_, isr26_, isr27_, isr28_, isr29_, isr30_, isr31_

isr0_:  push 0
        push 0
        jmp isr_common
isr1_:  push 0
        push 1
        jmp isr_common
isr2_:  push 0
        push 2
        jmp isr_common
isr3_:  push 0
        push 3
        jmp isr_common
isr4_:  push 0
        push 4
        jmp isr_common
isr5_:  push 0
        push 5
        jmp isr_common
isr6_:  push 0
        push 6
        jmp isr_common
isr7_:  push 0
        push 7
        jmp isr_common
isr8_:  push 8                  ; #DF (errcode pushed by CPU)
        jmp isr_common
isr9_:  push 0
        push 9
        jmp isr_common
isr10_: push 10                 ; #TS (errcode pushed by CPU)
        jmp isr_common
isr11_: push 11                 ; #NP (errcode pushed by CPU)
        jmp isr_common
isr12_: push 12                 ; #SS (errcode pushed by CPU)
        jmp isr_common
isr13_: push 13                 ; #GP (errcode pushed by CPU)
        jmp isr_common
isr14_: push 14                 ; #PF (errcode pushed by CPU)
        jmp isr_common
isr15_: push 0
        push 15
        jmp isr_common
isr16_: push 0
        push 16
        jmp isr_common
isr17_: push 17                 ; #AC (errcode pushed by CPU)
        jmp isr_common
isr18_: push 0
        push 18
        jmp isr_common
isr19_: push 0
        push 19
        jmp isr_common
isr20_: push 0
        push 20
        jmp isr_common
isr21_: push 0
        push 21
        jmp isr_common
isr22_: push 0
        push 22
        jmp isr_common
isr23_: push 0
        push 23
        jmp isr_common
isr24_: push 0
        push 24
        jmp isr_common
isr25_: push 0
        push 25
        jmp isr_common
isr26_: push 0
        push 26
        jmp isr_common
isr27_: push 0
        push 27
        jmp isr_common
isr28_: push 0
        push 28
        jmp isr_common
isr29_: push 0
        push 29
        jmp isr_common
isr30_: push 0
        push 30
        jmp isr_common
isr31_: push 0
        push 31
        jmp isr_common

; PIC-remapped hardware IRQ stubs.
; Master IRQ0 (PIT) -> vector 32; master IRQ1 (KBD) -> vector 33;
; slave IRQ6 (IDE primary) -> vector 46.
        public  isr32_, isr33_, isr46_
isr32_: push 0
        push 32
        jmp isr_common
isr33_: push 0
        push 33
        jmp isr_common
isr46_: push 0
        push 46
        jmp isr_common

; Note: ESP at this point already holds:
;   [esp+0]  = vector
;   [esp+4]  = errcode (real or dummy)
;   [esp+8]  = EIP   (V86)
;   ...etc.
isr_common:
        pushad                      ; EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI
        mov     ax, 10h             ; A V86->PM trap loads NULL into DS/ES/FS/GS;
        mov     ds, ax              ; reload PM_DS so the C handler can reach its
        mov     es, ax              ; globals + guest memory (flat) via DS/ES.
                                    ; QEMU-TCG tolerates a null DS; Bochs #GPs.
                                    ; iretd restores the guest segs from the frame.
        mov     eax, esp            ; watcall: 1st arg in EAX -> &trap_frame
        call    trap_dispatch_
        popad
        add     esp, 8              ; pop vector + errcode
        iretd

; Back-compat: gp_stub_ alias still used by main.c if not migrated yet.
        public  gp_stub_
gp_stub_:
        push    13                  ; vector
        pushad
        mov     ax, 10h             ; reload PM_DS/ES (V86->PM nulls them)
        mov     ds, ax
        mov     es, ax
        mov     eax, esp
        call    trap_dispatch_
        popad
        add     esp, 8
        iretd

; ----------------------------------------------------------------------------
; v86_enter(struct v86_state *s)   -- watcall: s in EAX
;
; Builds a V86 iret frame on the PM stack and iretd's into V86.  Never
; returns through the C ABI; control resumes in C only when a fault occurs
; in V86 and trap_dispatch returns.
; ----------------------------------------------------------------------------
        public  v86_enter_
v86_enter_:
        cli

        ; Order on stack (top first, popped last to first by iretd):
        ;   GS, FS, DS, ES, SS, ESP, EFLAGS, CS, EIP
        movzx   ecx, word ptr [eax+18]      ; gs
        push    ecx
        movzx   ecx, word ptr [eax+16]      ; fs
        push    ecx
        movzx   ecx, word ptr [eax+14]      ; es
        push    ecx
        movzx   ecx, word ptr [eax+12]      ; ds
        push    ecx
        movzx   ecx, word ptr [eax+4]       ; ss
        push    ecx
        movzx   ecx, word ptr [eax+6]       ; sp (V86 16-bit SP)
        push    ecx
        push    dword ptr [eax+8]           ; eflags (VM=1 set by caller)
        movzx   ecx, word ptr [eax]         ; cs
        push    ecx
        movzx   ecx, word ptr [eax+2]       ; ip
        push    ecx

        iretd

; ----------------------------------------------------------------------------
; load_idt(struct dtr *idtr)   -- watcall: idtr in EAX
; ----------------------------------------------------------------------------
        public  load_idt_
load_idt_:
        lidt    fword ptr [eax]
        ret

; ----------------------------------------------------------------------------
; load_tss(u16 selector)   -- watcall: selector in AX
; ----------------------------------------------------------------------------
        public  load_tss_
load_tss_:
        ltr     ax
        ret

; ----------------------------------------------------------------------------
; flush_tlb()  -- reload CR3 to invalidate the TLB after rewriting PTEs.
; ----------------------------------------------------------------------------
        public  flush_tlb_
flush_tlb_:
        mov     eax, cr3
        mov     cr3, eax
        ret

; ----------------------------------------------------------------------------
; read_cr2() -> u32  -- return the faulting linear address after a #PF.
; ----------------------------------------------------------------------------
        public  read_cr2_
read_cr2_:
        mov     eax, cr2
        ret

        align   4       ; bring combined _TEXT to DWORD boundary; Watcom 1.9
                        ; does not insert inter-segment gap bytes so we must
                        ; ensure the gap is 0. Works here because isr.obj is
                        ; linked last in _TEXT, and intra-segment alignment is
                        ; handled correctly by both Watcom versions.
_TEXT   ends

        end
