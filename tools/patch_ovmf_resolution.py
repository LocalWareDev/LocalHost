"""Raise the guest display resolution in an OVMF firmware image.

    python tools/patch_ovmf_resolution.py --fd RELEASEX64_OVMF_LocalHost.fd
                                          --width 1280 --height 960
                                          --out RELEASEX64_OVMF_LocalHost.fd

WHY THIS IS THE ONLY LEVER, since several likelier-looking ones are dead ends:

  The guest allocates its own framebuffer and reports the size to us through
  fw_cfg "etc/ramfb" -- the hypervisor never chooses the resolution, it only
  learns it. So the number has to be changed on the firmware side.

  It is NOT settable via EDID. QEMU's ramfb takes a monitor description through
  an "etc/edid" fw_cfg file, and adding one looked like the obvious fix, but
  instrumenting the read showed the firmware never asks for it -- and the string
  "etc/edid" does not appear anywhere in the 15.3MB decompressed DXE volume.
  This OVMF build simply has no EDID support for ramfb.

  It is not the PCD defaults either, at least not reachably: the 800x600 pairs
  that show up in the volume are GOP/console MODE TABLES, and rewriting a list
  of available modes does not change which one gets selected.

WHAT ACTUALLY WORKS:

  QemuRamfbDxe carries a three-entry mode table -- 640x480, 800x600, 1024x768 at
  a 36-byte stride -- and selects INDEX 1. Not the mode matching some PCD, not
  the largest: index 1, which is why the guest always came up 800x600 with
  1024x768 sitting right there unused.

  So the patch is to rewrite entry 1's dimensions. Verified by A/B on the same
  harness: unpatched reports "ramfb configured: 800x600 stride=3200", patched
  reports the requested size with a stride to match (1024 -> 4096, 1280 -> 5120),
  which shows the driver really recomputed the framebuffer rather than being left
  half-patched.

  ramfb is a plain linear framebuffer with no timing constraints, so the size is
  not restricted to standard VESA modes -- any sane WxH works. Bigger costs guest
  RAM (w*h*4) and host blit time on every repaint, so this is a real trade rather
  than free.

WHY IT CAN PATCH IN PLACE:

  Two UINT32s are overwritten with two UINT32s, so every enclosing length is
  unchanged and no checksum needs recomputing. Only the recompressed volume
  changes size, and it is padded back up to the original section length exactly
  as patch_ovmf_logo.py does.

The mode table is located by signature -- the "etc/ramfb" string, then the
three known entries at their stride -- never by hardcoded offset, so this should
survive a different OVMF build. It refuses rather than guesses if the layout is
not what it expects.
"""
import argparse
import lzma
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from patch_ovmf_logo import find_lzma_section, LayoutError  # noqa: E402

MODE_STRIDE = 0x24
# The stock table. Matching all three is what makes locating it by signature
# safe: a lone 800x600 appears over a hundred times in the volume.
STOCK_MODES = [(640, 480), (800, 600), (1024, 768)]
# QemuRamfbDxe selects this entry. Established empirically (see the docstring),
# not from the edk2 source, so it is stated as a measurement rather than a fact.
SELECTED_INDEX = 1
SEARCH_WINDOW = 0x8000


def decode_volume(fd):
    """Decompress the DXE volume, accepting both shapes we produce.

    A stock OVMF carries a normal .lzma (FORMAT_ALONE) stream. Once
    patch_ovmf_logo.py has rewritten it, it is raw LZMA1 behind a hand-built
    13-byte header with no end-of-stream marker, which FORMAT_ALONE rejects --
    so a firmware that has already been logo-patched needs the raw path, and
    chaining the two tools would otherwise fail on the second one.
    """
    sec, data_off, size = find_lzma_section(fd)
    payload = bytes(fd[sec + data_off: sec + size])
    props = payload[0]
    dict_size, = struct.unpack_from("<I", payload, 1)
    usize, = struct.unpack_from("<Q", payload, 5)
    try:
        volume = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(payload)
        how = "FORMAT_ALONE"
    except lzma.LZMAError:
        filters = [{"id": lzma.FILTER_LZMA1, "dict_size": dict_size,
                    "lc": props % 9, "lp": (props // 9) % 5,
                    "pb": (props // 9) // 5}]
        volume = lzma.LZMADecompressor(
            format=lzma.FORMAT_RAW, filters=filters
        ).decompress(payload[13:], max_length=usize)
        how = "raw LZMA1"
    return sec, data_off, size, payload, bytearray(volume), how


def find_mode_table(volume):
    """Offset of QemuRamfbDxe's mode table, by signature."""
    anchor = volume.find(b"etc/ramfb")
    if anchor < 0:
        raise LayoutError("no 'etc/ramfb' string -- this firmware has no ramfb driver")
    limit = min(anchor + SEARCH_WINDOW, len(volume) - MODE_STRIDE * len(STOCK_MODES))
    for base in range(anchor, limit):
        modes = [struct.unpack_from("<II", volume, base + i * MODE_STRIDE)
                 for i in range(len(STOCK_MODES))]
        if modes == STOCK_MODES:
            return base
    raise LayoutError(
        "found the ramfb driver but not its 640x480/800x600/1024x768 mode table "
        "-- refusing to patch a layout this does not understand")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fd", required=True, help="input OVMF .fd")
    ap.add_argument("--out", required=True, help="output .fd")
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=960)
    args = ap.parse_args()

    if args.width % 8 or args.width <= 0 or args.height <= 0:
        sys.exit("width must be a positive multiple of 8, height positive")

    fd = bytearray(Path(args.fd).read_bytes())
    sec, data_off, sec_size, payload, volume, how = decode_volume(fd)
    print(f"DXE volume: {len(volume)} bytes decompressed ({how})")

    table = find_mode_table(volume)
    at = table + SELECTED_INDEX * MODE_STRIDE
    before = struct.unpack_from("<II", volume, at)
    struct.pack_into("<II", volume, at, args.width, args.height)
    print(f"mode table at 0x{table:X}; entry {SELECTED_INDEX} "
          f"{before[0]}x{before[1]} -> {args.width}x{args.height}")
    print(f"guest framebuffer becomes {args.width * args.height * 4 / 1048576:.1f} MB")

    # Re-encode exactly as patch_ovmf_logo.py does; see its notes on why raw
    # LZMA1 with a hand-built header is the only shape EDK2 accepts.
    props = payload[0]
    dict_size, = struct.unpack_from("<I", payload, 1)
    filters = [{"id": lzma.FILTER_LZMA1, "preset": 9 | lzma.PRESET_EXTREME,
                "dict_size": dict_size, "lc": props % 9,
                "lp": (props // 9) % 5, "pb": (props // 9) // 5}]
    stream = lzma.compress(bytes(volume), format=lzma.FORMAT_RAW, filters=filters)
    header = bytes([props]) + struct.pack("<I", dict_size) + struct.pack("<Q", len(volume))
    packed = header + stream
    if packed[:13] != payload[:13]:
        raise LayoutError("rebuilt LZMA header differs from the original -- refusing")

    room = sec_size - data_off
    print(f"recompressed to {len(packed)} bytes (room: {room})")
    if len(packed) > room:
        sys.exit(f"TOO BIG by {len(packed) - room} bytes -- refusing to write")

    fd[sec + data_off: sec + sec_size] = packed + b"\x00" * (room - len(packed))
    Path(args.out).write_bytes(bytes(fd))
    print(f"wrote {args.out} ({len(fd)} bytes)")


if __name__ == "__main__":
    main()
