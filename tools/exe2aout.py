#!/usr/bin/env python3
"""
Convert a Watcom small-model DOS MZ .exe into the RealXV6 separated-I&D
(a.out) executable format -- VMM (stack-high) variant.

    exe2aout.py <input.exe> <input.map> <output.aout>

The MZ load module is laid out as [_TEXT][DGROUP-initialized-data], each
segment paragraph aligned.  DGROUP is [ data ][ bss ][ STACK ] with bss and the
stack TRAILING and uninitialized, so the load image holds only the initialized
data; the kernel zeroes bss+stack and sets SP to the top of the stack.  The
text/data split, the bss size and the stack size come from the linker .map.

Because the EXE startup never loads a segment register from a baked-in segment
value (the kernel/VMM windows supply CS/DS/ES/SS), a correctly built image has
NO relocations.  We assert that: zero relocations is what makes the code
position independent -- relocatable without a fixup table and shareable.

Output file: [16-byte struct exec][a_text bytes code][a_data bytes data].
The struct exec header is 8 little-endian words:
    a_magic a_text a_data a_bss a_syms a_entry a_stack a_flag
"""
import struct
import sys

A_MAGIC = 0o411


def die(msg):
    sys.stderr.write("exe2aout: " + msg + "\n")
    sys.exit(1)


def roundup(x, n):
    return (x + n - 1) // n * n


def parse_map(path):
    """Return (text_size, dgroup_size, bss_size, stack_size) bytes from a map."""
    text_size = dgroup_size = bss_size = stack_size = None
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) < 2:
                continue
            # Segment table rows:  NAME  CLASS  [GROUP]  SEG:OFF  SIZE
            if p[0] == "_TEXT" and p[1] == "CODE":
                text_size = int(p[-1], 16)
            elif p[0] == "STACK":
                stack_size = int(p[-1], 16)
            elif p[0] == "_BSS":
                bss_size = int(p[-1], 16)
            # Group table row:  DGROUP  SEG:OFF  SIZE
            elif p[0] == "DGROUP" and len(p) == 3:
                dgroup_size = int(p[-1], 16)
    if text_size is None:
        die("%s: no _TEXT segment found" % path)
    if dgroup_size is None:
        die("%s: no DGROUP group found" % path)
    if stack_size is None:
        die("%s: no STACK segment found (startup must reserve the user stack)" % path)
    if bss_size is None:
        bss_size = 0
    return text_size, dgroup_size, bss_size, stack_size


def main():
    if len(sys.argv) != 4:
        die("usage: exe2aout.py <input.exe> <input.map> <output.aout>")
    exe, mapf, out = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(exe, "rb") as f:
        data = f.read()
    if data[0:2] not in (b"MZ", b"ZM"):
        die("%s: not an MZ executable" % exe)
    (e_cblp, e_cp, e_crlc, e_cparhdr, e_minalloc, e_maxalloc,
     e_ss, e_sp, e_csum, e_ip, e_cs, e_lfarlc, e_ovno) = \
        struct.unpack_from("<13H", data, 2)

    if e_crlc != 0:
        relocs = []
        for i in range(e_crlc):
            roff, rseg = struct.unpack_from("<HH", data, e_lfarlc + i * 4)
            relocs.append("%04x:%04x" % (rseg, roff))
        die("%s has %d relocation(s) -- code is not position independent: %s"
            % (exe, e_crlc, " ".join(relocs)))

    # Load module = file minus MZ header; it holds [_TEXT][pad][init data] and
    # excludes BSS/STACK (those are trailing minalloc).  Trailing zero padding
    # to the 512-byte page is harmless and ignored (we slice exactly a_data).
    if e_cblp == 0:
        total = e_cp * 512
    else:
        total = (e_cp - 1) * 512 + e_cblp
    image = data[e_cparhdr * 16:total]

    text_size, dgroup_size, bss_size, stack_size = parse_map(mapf)
    a_text = roundup(text_size, 16)            # DGROUP starts on a paragraph
    a_data = len(image) - a_text                # everything after code = init data
    a_bss = bss_size                            # bss after a_data; kernel clears it
    a_stack = stack_size                        # STACK at the high end; kernel sets SP
    a_entry = e_ip

    if e_cs != 0:
        die("%s: initial CS=%#x, expected 0 (code segment must be first)" % (exe, e_cs))
    if a_data < 0:
        die("%s: code size %d exceeds image %d" % (exe, a_text, len(image)))
    if a_entry >= a_text:
        die("%s: entry %#x outside code segment (text %d)" % (exe, a_entry, a_text))

    text = image[:a_text]
    dat = image[a_text:]

    hdr = struct.pack("<8H", A_MAGIC, a_text, a_data, a_bss & 0xFFFF,
                      0, a_entry, a_stack & 0xFFFF, 0)
    with open(out, "wb") as f:
        f.write(hdr)
        f.write(text)
        f.write(dat)

    sys.stderr.write(
        "exe2aout: %s -> %s  text=%d data=%d bss=%d stack=%d entry=%#x\n"
        % (exe, out, a_text, a_data, a_bss, a_stack, a_entry))


if __name__ == "__main__":
    main()
