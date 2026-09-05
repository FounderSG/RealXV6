# ==========================================================================
# NOTE: This Makefile requires Open Watcom 'wmake'.
# It uses specific syntax (like .symbolic, %make, & line continuation)
# that is not compatible with GNU make or Microsoft nmake.
# ==========================================================================

CC = wcc
# Extra defines for a variant build, e.g. the serial-console test kernel:
#   wmake XCFLAGS=-dKL_BACKEND_UART
XCFLAGS =
CFLAGS = -i=h -ms -0 -s -zls -ecc -bt=dos -ohs -zq -j -zl -fo=.obj $(XCFLAGS)
LD = wlink
LDFLAGS = SYSTEM dos com OPTION map,nodefaultlibs

all: .symbolic
    %make clean
    %make build

.EXTENSIONS:
.EXTENSIONS: .obj .c
.c:dmr;ken

m86.obj: dmr/m86.asm
	wasm -bt=DOS -mt -0 $< -fo=$@

.c.obj :
	$(CC) $(CFLAGS) $<

OBJS = bio.obj ide.obj kbd.obj kl.obj mem.obj pc.obj rk.obj tty.obj uart.obj &
	alloc.obj clock.obj fio.obj iget.obj main.obj malloc.obj nami.obj &
	pipe.obj prf.obj rdwri.obj sig.obj slp.obj subr.obj sys1.obj &
	sys2.obj sys3.obj sys4.obj sysent.obj text.obj trap.obj 

build: .symbolic m86.obj $(OBJS)
    $(LD) @<<
$(LDFLAGS)
NAME unix.com
FILE m86.obj
FILE $(OBJS: =, )
<<

clean : .symbolic
	rm -f *.obj *.err *.com *.map
