"""Replace the TianoCore boot logo in an OVMF firmware image.

    python tools/patch_ovmf_logo.py --fd RELEASEX64_OVMF.fd --image LocalHost.png
                                    --out RELEASEX64_OVMF_LocalHost.fd

WHERE THE LOGO ACTUALLY LIVES, since none of this is guessable:

  The .fd holds three firmware volumes. The DXE one is a single LZMA-compressed
  GUIDed section, which is why grepping the .fd for a BMP -- or even for the
  string "TianoCore" -- finds nothing at all.

  Inside, LogoDxe (FILE_GUID F74D20EE-37E7-48FC-97F7-9B1047749C69) carries the
  image as an HII package list embedded in its PE32 resource data. It is NOT a
  BMP: it is an EFI_HII_IIBT_IMAGE_8BIT block -- palette-indexed pixels -- inside
  an EFI_HII_PACKAGE_IMAGES (type 0x06) package.

WHY THIS CAN PATCH IN PLACE:

  The replacement is written at the SAME dimensions with the SAME 256-colour
  palette size, so every enclosing length (image block, HII package, package
  list, PE32 section, FFS file, firmware volume) is unchanged. Nothing needs its
  size fixed up and no checksum needs recomputing.

  The one thing that does change size is the recompressed volume. LZMA output is
  padded back up to the original section length; a decoder stops at the
  uncompressed size recorded in the stream header and ignores trailing bytes, so
  padding is safe where changing the section length would not be -- shrinking the
  section would move every FFS file after it.

Everything is located by signature rather than by hardcoded offset, so this
should survive a different OVMF build. It refuses rather than guesses if the
layout is not what it expects.
"""
import argparse
import lzma
import struct
import sys
from pathlib import Path

LZMA_SECTION_GUID = "EE4E5898-3914-4259-9D6E-DC7BD79403CF"
LOGO_DXE_GUID = "F74D20EE-37E7-48FC-97F7-9B1047749C69"

EFI_SECTION_GUID_DEFINED = 0x02
EFI_SECTION_PE32 = 0x10
EFI_HII_PACKAGE_IMAGES = 0x06
EFI_HII_PACKAGE_END = 0xDF
EFI_HII_IIBT_IMAGE_8BIT = 0x14
EFI_HII_IIBT_IMAGE_8BIT_TRANS = 0x15


def guid_bytes(text):
    p = text.split("-")
    return (struct.pack("<IHH", int(p[0], 16), int(p[1], 16), int(p[2], 16))
            + bytes.fromhex(p[3]) + bytes.fromhex(p[4]))


class LayoutError(RuntimeError):
    """The firmware is not laid out the way this tool understands."""


def find_lzma_section(fd):
    """Locate the compressed DXE volume. Returns (section_start, data_offset, size)."""
    at = fd.find(guid_bytes(LZMA_SECTION_GUID))
    if at < 0:
        raise LayoutError("no LZMA-compressed section found")
    sec = at - 4                       # the GUID sits at +4 in a GUIDed section
    size = fd[sec] | (fd[sec + 1] << 8) | (fd[sec + 2] << 16)
    stype = fd[sec + 3]
    if stype != EFI_SECTION_GUID_DEFINED:
        raise LayoutError(f"expected a GUID-defined section, got type 0x{stype:02X}")
    data_off, _attrs = struct.unpack_from("<HH", fd, sec + 20)
    return sec, data_off, size


def find_logo_image(volume):
    """Walk the decompressed volume down to the logo's pixels.

    Returns a dict describing where the pixels and palette live, in volume
    coordinates, so the caller can overwrite them directly.
    """
    ffs = volume.find(guid_bytes(LOGO_DXE_GUID))
    if ffs < 0:
        raise LayoutError("LogoDxe not present in this firmware")

    attrs = volume[ffs + 19]
    fsize = volume[ffs + 20] | (volume[ffs + 21] << 8) | (volume[ffs + 22] << 16)
    hdr_len = 24
    if attrs & 0x01:                   # FFS_ATTRIB_LARGE_FILE
        fsize, = struct.unpack_from("<Q", volume, ffs + 24)
        hdr_len = 32
    body_at = ffs + hdr_len
    body_end = ffs + fsize

    # Find the PE32 section holding the HII resource.
    pe_at = pe_end = None
    off = body_at
    while off + 4 <= body_end:
        ssize = volume[off] | (volume[off + 1] << 8) | (volume[off + 2] << 16)
        stype = volume[off + 3]
        if ssize == 0 or off + ssize > body_end:
            break
        if stype == EFI_SECTION_PE32:
            pe_at, pe_end = off + 4, off + ssize
            break
        off += (ssize + 3) & ~3        # sections are 4-byte aligned
    if pe_at is None:
        raise LayoutError("LogoDxe has no PE32 section")

    # The HII package list begins with LogoDxe's own GUID, followed by its length.
    list_at = volume.find(guid_bytes(LOGO_DXE_GUID), pe_at, pe_end)
    if list_at < 0:
        raise LayoutError("no HII package list inside LogoDxe")
    list_len, = struct.unpack_from("<I", volume, list_at + 16)

    # Walk packages for the IMAGES one.
    p = list_at + 20
    end = list_at + list_len
    while p + 4 <= end:
        header, = struct.unpack_from("<I", volume, p)
        plen, ptype = header & 0xFFFFFF, (header >> 24) & 0xFF
        if plen == 0:
            break
        if ptype == EFI_HII_PACKAGE_IMAGES:
            img_off, pal_off = struct.unpack_from("<II", volume, p + 4)
            blk = p + img_off
            btype = volume[blk]
            if btype not in (EFI_HII_IIBT_IMAGE_8BIT, EFI_HII_IIBT_IMAGE_8BIT_TRANS):
                raise LayoutError(
                    f"logo is image block type 0x{btype:02X}; this tool only "
                    "handles the 8-bit palette form (0x14/0x15)")
            palette_index = volume[blk + 1]
            width, height = struct.unpack_from("<HH", volume, blk + 2)

            pal_info = p + pal_off
            pal_count, = struct.unpack_from("<H", volume, pal_info)
            if palette_index < 1 or palette_index > pal_count:
                raise LayoutError("logo references a palette that is not there")
            q = pal_info + 2
            for _ in range(palette_index - 1):
                psize, = struct.unpack_from("<H", volume, q)
                q += 2 + psize
            palette_size, = struct.unpack_from("<H", volume, q)
            return {
                "width": width, "height": height,
                "pixels_at": blk + 6, "pixels_len": width * height,
                "palette_at": q + 2, "palette_colors": palette_size // 3,
            }
        p += plen
        if ptype == EFI_HII_PACKAGE_END:
            break
    raise LayoutError("no HII IMAGES package in LogoDxe")


def render_replacement(image_path, width, height):
    """Scale the source image into width x height, letterboxed on black.

    Cropped to its non-black content first: artwork tends to carry a wide margin
    (the LocalHost logo is 2000x2000 with the wordmark in a narrow band), and
    scaling that whole canvas into a 193x58 strip would leave the wordmark
    unreadably small.
    """
    from PySide6.QtGui import QImage, QColor
    from PySide6.QtCore import Qt

    src = QImage(str(image_path))
    if src.isNull():
        raise RuntimeError(f"could not load image: {image_path}")
    src = src.convertToFormat(QImage.Format_RGB32)

    # Bounding box of everything that is not near-black.
    left, top, right, bottom = src.width(), src.height(), -1, -1
    for y in range(src.height()):
        for x in range(src.width()):
            c = QColor(src.pixel(x, y))
            if c.red() + c.green() + c.blue() > 30:
                left = min(left, x); right = max(right, x)
                top = min(top, y); bottom = max(bottom, y)
    if right >= left and bottom >= top:
        src = src.copy(left, top, right - left + 1, bottom - top + 1)

    scaled = src.scaled(width, height, Qt.KeepAspectRatio, Qt.SmoothTransformation)
    canvas = QImage(width, height, QImage.Format_RGB32)
    canvas.fill(QColor(0, 0, 0))
    ox = (width - scaled.width()) // 2
    oy = (height - scaled.height()) // 2
    for y in range(scaled.height()):
        for x in range(scaled.width()):
            canvas.setPixel(ox + x, oy + y, scaled.pixel(x, y))
    return canvas


def quantise_grayscale(image, colors):
    """Map to a grayscale ramp filling the existing palette.

    A ramp rather than a computed palette because it needs no clustering pass and
    is exact for the monochrome wordmark this is built for; the palette keeps its
    original entry count so nothing downstream changes size.
    """
    from PySide6.QtGui import QColor
    levels = min(colors, 256)
    pixels = bytearray(image.width() * image.height())
    for y in range(image.height()):
        for x in range(image.width()):
            c = QColor(image.pixel(x, y))
            lum = (c.red() * 299 + c.green() * 587 + c.blue() * 114) // 1000
            pixels[y * image.width() + x] = lum * (levels - 1) // 255
    palette = bytearray()
    for i in range(colors):
        v = (i * 255 // (levels - 1)) if i < levels else 0
        palette += bytes((v, v, v))    # EFI_HII_RGB_PIXEL is B, G, R
    return bytes(pixels), bytes(palette)


def main():
    ap = argparse.ArgumentParser(description="Replace the OVMF boot logo.")
    ap.add_argument("--fd", required=True, help="input OVMF .fd")
    ap.add_argument("--image", required=True, help="replacement image")
    ap.add_argument("--out", required=True, help="output .fd (never written in place)")
    ap.add_argument("--preview", help="also save the scaled logo as a PNG")
    args = ap.parse_args()

    fd_path, out_path = Path(args.fd), Path(args.out)
    if out_path.resolve() == fd_path.resolve():
        print("refusing to overwrite the input firmware -- pass a different --out")
        return 2

    fd = bytearray(fd_path.read_bytes())
    print(f"firmware: {fd_path.name} ({len(fd)} bytes)")

    sec, data_off, sec_size = find_lzma_section(fd)
    payload = bytes(fd[sec + data_off: sec + sec_size])
    volume = bytearray(lzma.decompress(payload, format=lzma.FORMAT_ALONE))
    print(f"DXE volume: {len(volume)} bytes decompressed "
          f"(from {len(payload)} at 0x{sec:06X})")

    info = find_logo_image(volume)
    print(f"logo: {info['width']}x{info['height']}, "
          f"{info['palette_colors']}-colour palette")

    image = render_replacement(args.image, info["width"], info["height"])
    if args.preview:
        image.save(args.preview)
        print(f"preview written to {args.preview}")
    pixels, palette = quantise_grayscale(image, info["palette_colors"])

    if len(pixels) != info["pixels_len"]:
        raise LayoutError("internal: pixel count mismatch")
    volume[info["pixels_at"]: info["pixels_at"] + len(pixels)] = pixels
    volume[info["palette_at"]: info["palette_at"] + len(palette)] = palette
    print(f"replaced {len(pixels)} pixel bytes and {len(palette)} palette bytes")

    # Recompress and pad back to the original section length. Padding is what
    # keeps every enclosing structure the size it already is.
    #
    # The LZMA parameters MATCH THE ORIGINAL STREAM rather than using Python's
    # defaults, and that is not cosmetic. EDK2's in-firmware decompressor sizes
    # its scratch buffer from the dictionary size in the header; Python's preset
    # 9 asks for a 64MB dictionary where OVMF's own stream declares 16MB, which
    # would demand a far bigger scratch buffer than the platform set aside. The
    # properties byte 0x5D that OVMF ships decodes to lc=3, lp=0, pb=2.
    # ENCODED AS RAW LZMA1 WITH A HAND-BUILT HEADER, which is what EDK2 ships and
    # is the only shape that works here. The two obvious alternatives both fail:
    #
    #   - lzma.compress(FORMAT_ALONE) writes "size unknown" and an end-of-stream
    #     marker. EDK2 READS the size out of the header to size its output
    #     buffer, so "unknown" gives it garbage.
    #   - Patching the real size into that stream makes liblzma reject it
    #     outright ("Corrupt input data"), because a known size together with an
    #     EOS marker is not a combination it accepts.
    #
    # Raw LZMA1 has no EOS marker, so prepending a 13-byte header carrying the
    # real props, dictionary size and uncompressed length reproduces the original
    # byte-for-byte in shape -- verified below by decoding it the way the
    # firmware does.
    props = payload[0]
    dict_size, = struct.unpack_from("<I", payload, 1)
    filters = [{"id": lzma.FILTER_LZMA1,
                "preset": 9 | lzma.PRESET_EXTREME,
                "dict_size": dict_size,
                "lc": props % 9,
                "lp": (props // 9) % 5,
                "pb": (props // 9) // 5}]
    stream = lzma.compress(bytes(volume), format=lzma.FORMAT_RAW, filters=filters)
    header = bytes([props]) + struct.pack("<I", dict_size) + struct.pack("<Q", len(volume))
    packed = header + stream
    if packed[:13] != payload[:13]:
        raise LayoutError("rebuilt LZMA header differs from the original -- "
                          "refusing to ship it")
    room = sec_size - data_off
    print(f"recompressed to {len(packed)} bytes (room: {room})")
    if len(packed) > room:
        print(f"\nTOO BIG by {len(packed) - room} bytes -- the replacement image "
              "compresses worse than the original.\nTry a simpler image (fewer "
              "distinct shades / flatter areas).")
        return 1

    fd[sec + data_off: sec + sec_size] = packed + b"\x00" * (room - len(packed))
    out_path.write_bytes(bytes(fd))
    print(f"\nwrote {out_path} ({len(fd)} bytes, unchanged size)")

    # VERIFY THE WAY THE FIRMWARE WILL READ IT: re-read the file from disk, take
    # props and the uncompressed size out of the 13-byte header, and decode the
    # remainder as raw LZMA1 capped at that size. That is precisely what EDK2's
    # decompressor does, so a pass here means the firmware can unpack it -- which
    # a liblzma ALONE-format check would NOT have told us.
    check = out_path.read_bytes()
    vsec, vdoff, vsize = find_lzma_section(check)
    blob = check[vsec + vdoff: vsec + vsize]
    vprops = blob[0]
    vdict, = struct.unpack_from("<I", blob, 1)
    vsize_out, = struct.unpack_from("<Q", blob, 5)
    dec = lzma.LZMADecompressor(
        format=lzma.FORMAT_RAW,
        filters=[{"id": lzma.FILTER_LZMA1, "dict_size": vdict,
                  "lc": vprops % 9, "lp": (vprops // 9) % 5,
                  "pb": (vprops // 9) // 5}])
    vvol = bytearray(dec.decompress(blob[13:], max_length=vsize_out))
    if len(vvol) != len(volume):
        print(f"verify FAILED: round-tripped {len(vvol)} bytes, expected {len(volume)}")
        return 1
    vinfo = find_logo_image(vvol)
    ok = bytes(vvol[vinfo["pixels_at"]: vinfo["pixels_at"] + len(pixels)]) == pixels
    print(f"verify: re-read firmware, decoded {len(vvol)} bytes as the firmware "
          f"would, logo pixels match -> {ok}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
