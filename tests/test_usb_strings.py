"""USB string descriptors as get_descriptor() hands them out (src/usb/usbcfg.c), from the ELF.

ChibiOS sends min(wLength, ud_size) bytes of a descriptor (USBDescriptor in vcom_strings[]). Each
string's ud_size must equal its bLength (USB 2.0 9.3, 9.4.3): a host asking for 255 bytes must
get the descriptor, not the bytes after it.

D6: the product string's array had room for the board number that init_usb_descriptor() adds at
boot, and its wrapper's size was sizeof the array: 38 bytes for a bLength of 26 (28-36 with the
board number). Now the wrapper starts at bLength and init_usb_descriptor() sets both together;
the array is exactly as large as the longest string (a board number of 4 characters).

  test_usb_strings.py <striso_control.elf>
"""
import struct
import sys

# the product string's layout (init_usb_descriptor): "Striso board " then up to 4 characters
PRODUCT_ID_OFFSET, PRODUCT_ID_CHARS = 28, 4


def elf_symbols(path):
    """{name: (address, bytes)} of the ELF's initialized data symbols (LTO suffixes dropped)."""
    data = open(path, "rb").read()
    assert data[:4] == b"\x7fELF" and data[4] == 1 and data[5] == 1, "not an ELF32 LE file"
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    syms = {}
    for s in secs:
        if s[1] != 2:  # SHT_SYMTAB
            continue
        strtab = secs[s[6]]
        for off in range(s[4], s[4] + s[5], 16):
            name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", data, off)
            n = data[strtab[4] + name:data.index(b"\0", strtab[4] + name)].decode()
            if size and 0 < shndx < len(secs) and secs[shndx][1] != 8:  # not NOBITS
                sec = secs[shndx]
                syms[n.split(".")[0]] = (value, data[sec[4] + value - sec[3]:sec[4] + value - sec[3] + size])
    return syms


def main(elf):
    syms = elf_symbols(elf)
    by_addr = {a: (n, b) for n, (a, b) in syms.items()}
    assert "vcom_strings" in syms, "vcom_strings not in the ELF"
    table = syms["vcom_strings"][1]
    bad = 0
    for idx in range(len(table) // 8):
        size, ptr = struct.unpack_from("<II", table, 8 * idx)
        name, b = by_addr.get(ptr, (None, None))
        if b is None:
            print(f"FAIL string {idx}: points to 0x{ptr:08x}, no symbol")
            bad += 1
            continue
        ok = size == b[0] and size <= len(b)
        bad += not ok
        print(f"{'ok  ' if ok else 'FAIL'} string {idx} {name:32s} bLength {b[0]:3d}  bytes sent {size:3d}"
              f"  (array {len(b)})")
        if name == "descriptor_product_string":
            room = len(b) - PRODUCT_ID_OFFSET
            ok = room == 2 * PRODUCT_ID_CHARS
            bad += not ok
            print(f"{'ok  ' if ok else 'FAIL'}   room for the board number: {room // 2} characters"
                  f" (expected {PRODUCT_ID_CHARS})")
    print("USB string sizes: " + ("FAILED" if bad else "all passed"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
