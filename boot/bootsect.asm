        name    bootloader
        assume  nothing

_TEXT   segment word public 'CODE'

        assume  nothing
        public  start

        assume  cs:_TEXT

        org     0100h

start proc near

        mov ax, 800h            ; Set SP below 0x10000
        mov sp, ax

        mov ax, 1000h           ; Load segment for 64K at 0x10000
        mov es, ax              ; ES = 0x1000
        mov bx, 100h            ; Set BX (offset) to 0x100

        ; Read 128 blocks (sectors) starting from block 1
        mov si, 1               ; Block number 1 (we'll loop from 1 to 128)

read_loop:
        call read_block         ; Call the function to read block in SI
        jc read_failed          ; Jump if carry flag is set (error occurred)

        add bx, 512             ; Advance offset by 512 (size of 1 sector)
        inc si                  ; Next block number
        cmp si, 120             ; Have we read 128 sectors?
        jb read_loop            ; If not, keep reading

        ; Jump to the loaded code at 0x1000:0x0100
        mov sp, 0FFFEh          ; Set kernel stack SP
        mov ax, es              ; Load kernel segment
        mov ss, ax              ; set kernel stack SS
        push ax                 ; push CS
        mov ax, 0100h
        push ax                 ; push IP
        retf                    ; far jump to kernel entry

read_failed:
        mov ah, 0               ; BIOS function to reset the disk system
        int 13h
        jmp read_loop           ; Retry after resetting

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
