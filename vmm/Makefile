# ==========================================================================
# vmm/Makefile  --  V86 Monitor (Open Watcom wmake)
#
# Build flags:
#   wcc386: -bt=none flat 32-bit, no runtime, -ms=stack overflow check off,
#           -mf flat mem model, -3 386 instr, -zl no auto lib refs.
#   wasm:   -3p enables privileged 386 instructions (lgdt, mov cr*, etc.)
#
# Link with OFFSET=0x8000 so absolute address fixups resolve to the load
# linear address (vmm.bin is loaded by bootsect at phys 0x08000).
# ==========================================================================

CC      = wcc386
CFLAGS  = -bt=none -mf -3 -zl -s -zq -fo=.obj

AS      = wasm
ASFLAGS = -bt=none -3p -zq

LD      = wlink

OBJS = entry.obj main.obj log.obj isr.obj

all: .symbolic vmm.bin guest_test.com

entry.obj: entry.asm
	$(AS) $(ASFLAGS) entry.asm -fo=entry.obj

isr.obj: isr.asm
	$(AS) $(ASFLAGS) isr.asm -fo=isr.obj

main.obj: main.c vmm.h
	$(CC) $(CFLAGS) main.c

log.obj: log.c vmm.h
	$(CC) $(CFLAGS) log.c

vmm.bin: $(OBJS)
	$(LD) @<<
FORMAT raw bin
NAME vmm.bin
OPTION nodefaultlibs
OPTION OFFSET=0x8000
OPTION MAP=vmm.map
ORDER
  CLNAME CODE
    SEGMENT _BOOT16
    SEGMENT _BOOT32
    SEGMENT _TEXT
  CLNAME DATA
    SEGMENT CONST
    SEGMENT CONST2
    SEGMENT _DATA
FILE entry.obj
FILE main.obj
FILE log.obj
FILE isr.obj
<<

# --------------------------------------------------------------------------
# guest_test.com -- 16-bit real-mode V86 guest used as a VMM self-test.
# Toolchain differs from vmm.bin: tiny model, 8086, DOS .COM format.
# --------------------------------------------------------------------------
guest_test.obj: guest_test.asm
	wasm -bt=DOS -mt -0 -zq guest_test.asm -fo=guest_test.obj

guest_test.com: guest_test.obj
	$(LD) SYSTEM dos com OPTION nodefaultlibs NAME guest_test.com FILE guest_test.obj

clean: .symbolic
	rm -f *.obj *.err *.map vmm.bin guest_test.com
