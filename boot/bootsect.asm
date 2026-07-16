        name    bootloader
        assume  nothing

_TEXT   segment word public 'CODE'

        assume  nothing
        public  start

        assume  cs:_TEXT

        org     0100h

start proc near

        xor ax, ax              ; ss = 0: do not inherit the BIOS stack segment
        mov ss, ax
        mov sp, 800h            ; scratch stack while loading (phys 0x0800)

        ; --- Phase 1: load vmm.bin (sectors 1..32) at 0x0800:0x0000 -----------
        mov ax, 800h
        mov es, ax
        xor bx, bx
        mov si, 1

read_vmm:
        call read_block
        jc read_failed
        add bx, 512
        inc si
        cmp si, 97              ; sectors 1..96 (vmm.bin 1..32 + unix.com 33..96)
        jb read_vmm

        ; Far-jump to VMM entry at 0x0800:0x0000.
        ; The retf frame must land OUTSIDE the unix.com blob (phys 0xC000..0x13FFF):
        ; the old ss=0x800:sp=0x7FFE put it at phys 0xFFFA (= kernel file offset
        ; 0x3FFA, inside open1), and the VMM's memcpy then carried that corruption
        ; into the running kernel.  Keep ss=0 -- with ss=0x800 the segment base
        ; 0x8000 alone lands the frame back in the blob regardless of sp.
        xor ax, ax
        mov ss, ax              ; ss = 0  (mov ss / mov sp kept adjacent: the 8086
        mov sp, 7000h           ; inhibits interrupts for one insn after an ss load)
        mov ax, 800h
        push ax                 ; CS = 0x0800
        xor ax, ax
        push ax                 ; IP = 0
        retf

read_failed:
        mov ah, 0               ; BIOS reset disk
        int 13h
        jmp read_vmm

read_block:
        push ax                 ; Save all registers
        push bx
        push cx
        push dx
        push si
        push di
        push bp

        mov ax, si              ; Block number in AX
        call LBA_to_CHS         ; Convert LBA block number to CHS
        mov ah, 2               ; BIOS function to read sector
        mov al, 1               ; Read 1 sector
        int 13h                 ; Call BIOS

        pop bp                  ; Restore registers
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
        ret

; Inputs:
; AX: LBA value (Logical Block Address)
; Outputs:
; CH: Cylinder high byte
; CL: Cylinder low byte (lower 6 bits for sector) and upper 2 bits of the cylinder
; DH: Head number

SECTORS_PER_TRACK equ 9    ; Number of sectors per track (typically 9 or 18 for floppy disks)
HEADS             equ 2    ; Number of heads (0 or 1, as head numbers start from 0)

LBA_to_CHS:
    ; Input: AX = LBA (Logical Block Address)
    push bx                        ; Save BX because it will be modified during the calculations

    ; Calculate Cylinder (Track)
    ; CX = LBA / (SECTORS_PER_TRACK * HEADS)
    mov cx, SECTORS_PER_TRACK * HEADS  ; Set CX to sectors per track * heads (18 for 1.2MB floppy)
    xor dx, dx                     ; Clear DX for division to avoid any overflow issues
    div cx                         ; Divide LBA by (sectors per track * heads), AX now has cylinder, DX has remainder (within track)

    ; AX contains the cylinder number
    ; Store the cylinder in CH (for high byte) and clear CL for now
    mov ch, al                     ; Move lower 8 bits of the cylinder into CH (floppy disks typically don’t exceed 256 cylinders)
    mov cl, 0                      ; Clear CL for now (CL will later store the sector number)

    ; Now calculate Head and Sector
    ; DX now contains the LBA remainder, which represents the position within the track,
    ; we use DX to calculate the head and sector

    ; Calculate Head
    ; DX / SECTORS_PER_TRACK -> AX = head number, DX = sector number
    mov ax, dx                     ; DX as dividend
    xor dx, dx
    mov bx, SECTORS_PER_TRACK      ; BX = sectors per track (9 in this case)
    div bx                         ; Divide LBA within the track by sectors per track, AX = head, DX = remainder (sector)

    ; DX contains the sector number (0-based, so we need to adjust it to 1-based)
    add dl, 1                      ; Convert sector number to 1-based as sectors start from 1 in INT 13h
    and dl, 3Fh                    ; Ensure the sector number fits in the lower 6 bits (1-63 range)
    or cl, dl                      ; Store the sector number in the lower 6 bits of CL

    ; AX now contains the head number
    mov dh, al                     ; Move the head number to DH (0 or 1)
    and dh, 1                      ; Only 2 heads are possible (head 0 or 1)
    xor dl, dl                     ; Clear DL (optional, depending on the calling conventions)

    ; Restore BX and return with CH, CL, and DH set up for INT 13h
    pop bx                         ; Restore BX from the stack
    ret

start endp

        org     02FEh
        dw      0AA55h

_TEXT   ends

        end     start
