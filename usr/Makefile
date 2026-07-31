# ==========================================================================
# NOTE: This Makefile requires Open Watcom 'wmake'.
# It uses specific syntax (like .symbolic, %make, & line continuation)
# that is not compatible with GNU make or Microsoft nmake.
# ==========================================================================

CC = wcc
CFLAGS = -wcd=107 -wcd=131 -ms -0 -s -ze -zls -ecc -zp1 -bt=dos -ohs -zq -j -zl -fo=.obj
LD = wlink
LDFLAGS = SYSTEM dos FILE startup.obj LIBRARY unix OPTION map,nodefaultlibs
PYTHON = python

.EXTENSIONS:
.EXTENSIONS: .aout .obj .asm .c

# Link as an MZ .exe, then convert to a.out.
.obj.aout:
	$(LD) @<<
$(LDFLAGS) FILE $< NAME $*.exe
<<
	$(PYTHON) ../tools/exe2aout.py $*.exe $*.map $@
.c.obj:
	$(CC) @<<
$(CFLAGS) $<
<<
# Small model (-ms), not tiny (-mt): keep _TEXT out of DGROUP.
.asm.obj:
	wasm -bt=DOS -ms -0 $< -fo=$@

BINS = init.aout sh.aout ls.aout cat.aout wc.aout echo.aout pwd.aout kill.aout date.aout
BINS += rm.aout login.aout cp.aout chmod.aout mkdir.aout sync.aout id.aout zombie.aout
BINS += mount.aout umount.aout forktest.aout sigtest.aout stty.aout more.aout fib.aout
BINS += ln.aout nice.aout mknod.aout sleep.aout grep.aout ps.aout ed.aout
BINS += mkfs.aout df.aout

all : .symbolic clean unix.lib startup.obj $(BINS)

OBJS = syscall.obj ulib.obj ctime.obj printf.obj setexit.obj

unix.lib : $(OBJS)
	wlib @<<
-q -fo unix.lib $(OBJS)
<<

clean : .symbolic
	rm -f *.obj *.err *.com *.lib *.map *.exe *.aout
