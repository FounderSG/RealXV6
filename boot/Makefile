# ==========================================================================
# NOTE: This Makefile requires Open Watcom 'wmake'.
# It uses specific syntax (like .symbolic, %make, & line continuation)
# that is not compatible with GNU make or Microsoft nmake.
# ==========================================================================

all : .symbolic Unix360.img unix0_rk.img unix.img

# Build boot sector and generate floppy image
# Layout (720 sectors total):
#   sector 0:         boot.com (bootsector)
#   sectors 1..32:    vmm.bin   (~16 KB budget; bootsect reads 72 sectors)
#   sectors 33..72:   unix.com  (40 sectors = 20 KB, embedded for VMM to memcpy)
#   sectors 73+:      unused
Unix360.img: ../unix.com ../vmm/vmm.bin bootsect.asm
	wasm -bt=DOS -mt -1 bootsect.asm -fo=bootsect.obj
	wlink $(LDFLAGS) SYSTEM dos com FILE bootsect.obj OPTION nodefaultlibs NAME boot.com
	# HEADS = 2 * TRACKS = 40 * SECTORS_PER_TRACK = 9 = 720
	dd if=/dev/zero of=Unix360.img bs=512 count=720
	dd if=boot.com of=Unix360.img conv=notrunc
	dd if=../vmm/vmm.bin of=Unix360.img conv=notrunc bs=512 seek=1
	dd if=../unix.com of=Unix360.img conv=notrunc bs=512 seek=33

../vmm/vmm.bin: .symbolic
	cd ../vmm && wmake

../vmm/guest_test.com: .symbolic
	cd ../vmm && wmake guest_test.com

# VMM self-test image: identical to Unix360.img but boots guest_test.com
# instead of unix.com. Exercises sensitive ops and reports via HVC_REPORT.
Unix360-test.img: ../vmm/vmm.bin ../vmm/guest_test.com bootsect.asm
	wasm -bt=DOS -mt -1 bootsect.asm -fo=bootsect.obj
	wlink $(LDFLAGS) SYSTEM dos com FILE bootsect.obj OPTION nodefaultlibs NAME boot.com
	dd if=/dev/zero of=Unix360-test.img bs=512 count=720
	dd if=boot.com of=Unix360-test.img conv=notrunc
	dd if=../vmm/vmm.bin of=Unix360-test.img conv=notrunc bs=512 seek=1
	dd if=../vmm/guest_test.com of=Unix360-test.img conv=notrunc bs=512 seek=33

../tools/mkfs: ../tools/mkfs.c
	gcc -Wno-implicit-function-declaration -Wno-implicit-int -Wno-parentheses -Wno-return-type -Wno-incompatible-pointer-types ../tools/mkfs.c -o ../tools/mkfs

unix.img: proto ../tools/mkfs
	../tools/mkfs unix.img proto
	dd if=/dev/zero of=unix.img bs=1 count=1 seek=10321919 conv=notrunc

b: .symbolic Unix360.img unix.img
	bochs -qf bochs.rc

q: .symbolic Unix360.img unix.img
	qemu-system-i386 -curses -monitor none -drive file=Unix360.img,format=raw,if=floppy -drive file=unix.img,format=raw,if=ide -boot a

unix0_rk.img:
	gunzip -c unix0_rk.img.gz > $@

bochs: .symbolic Unix360.img unix0_rk.img
	bochs -qf bochsrc.bxrc

# Interactive console-only unix under the VMM. -curses shows the guest's VGA
# text screen and sends keystrokes to the PS/2 keyboard (guest IRQ1). -monitor
# none disables qemu's monitor so a stray Esc/Alt sequence can't switch the
# console into the qemu monitor (where guest input would be lost).
# To quit: halt the guest, or kill qemu (Ctrl-C may reach the guest).
qemu: .symbolic Unix360.img unix0_rk.img
	qemu-system-i386 -curses -monitor none -drive file=Unix360.img,format=raw,if=floppy -drive file=unix0_rk.img,format=raw,if=ide -boot a

qemu-uart: .symbolic Unix360.img unix0_rk.img
	qemu-system-i386 -curses -serial stdio -monitor none -drive file=Unix360.img,format=raw,if=floppy -drive file=unix0_rk.img,format=raw,if=ide -boot a

q-test: .symbolic Unix360-test.img
	qemu-system-i386 -curses -serial stdio -drive file=Unix360-test.img,format=raw,if=floppy -drive file=Unix360-test.img,format=raw,if=ide -snapshot -boot a

clean: .symbolic
	rm -f *.obj *.err *.com *.img *.txt ../tools/mkfs
