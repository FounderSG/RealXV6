# ==========================================================================
# NOTE: This Makefile requires Open Watcom 'wmake'.
# It uses specific syntax (like .symbolic, %make, & line continuation)
# that is not compatible with GNU make or Microsoft nmake.
# ==========================================================================

CC = wcc
CFLAGS = -wcd=107 -ms -0 -s -zls -ecc -zp1 -bt=dos -ohs -zq -j -zl -fo=.obj
LD = wlink
LDFLAGS = SYSTEM dos com FILE startup.obj LIBRARY unix OPTION map,nodefaultlibs

.EXTENSIONS:
.EXTENSIONS: .com .obj .asm .c

.obj.com:
	$(LD) @<<
$(LDFLAGS) FILE $< NAME $@
<<
.c.obj:
	$(CC) @<<
$(CFLAGS) $<
<<
.asm.obj:
	wasm -bt=DOS -mt -0 $< -fo=$@

BINS = init.com sh.com ls.com cat.com wc.com echo.com pwd.com kill.com date.com
BINS += rm.com login.com cp.com chmod.com mkdir.com sync.com id.com zombie.com
BINS += mount.com umount.com forktest.com sigtest.com stty.com more.com fib.com
BINS += ln.com nice.com mknod.com sleep.com grep.com
BINS += mkfs.com

all : .symbolic clean unix.lib $(BINS)

OBJS = syscall.obj ulib.obj ctime.obj printf.obj

unix.lib : $(OBJS) startup.obj
	wlib @<<
-q -fo unix.lib $(OBJS)
<<

clean : .symbolic
	rm -f *.obj *.err *.com *.lib *.map
