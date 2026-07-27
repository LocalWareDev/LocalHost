"""Find direct callers of a function in the captured guest-kernel image.

Scans ntoskrnl_dump.bin for E8 rel32 CALL instructions whose computed target is
the requested RVA, then names each call site via tools/pdbsym.py. Also reports
the offset of the call within its containing function, which is what lets you
ORDER two calls made by the same caller.

The dump is a memory image read page-by-page starting at the module base, so a
file offset in it IS the RVA -- no PE section translation needed.

Usage:
    python tools/findcalls.py 0x99E7A8            # who calls this RVA
    python tools/findcalls.py HalpInitSystemHelper # by name
    python tools/findcalls.py --in InitBootProcessor 0xA56F60 0x99E7A8
        ^ restrict to calls made from inside one function, to compare their
          offsets and establish which happens first

Caveats this cannot see:
  - INDIRECT calls (call [reg], jump tables, guard_dispatch_icall thunks).
    A negative result means "no direct E8 caller", never "never called".
  - E8 bytes that are actually data or mid-instruction operands. Sites whose
    containing function cannot be resolved are flagged as low confidence.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pdbsym

DUMP = r"C:\LocalHost-evidence\ntoskrnl_dump.bin"


def resolve(sym, tok):
    """Accept either 0xHEX or a symbol name."""
    tok = tok.strip()
    if tok.lower().startswith("0x"):
        return int(tok, 16)
    rva = sym.rva_of(tok)
    if rva is None:
        sys.exit(f"symbol not found: {tok}")
    return rva


def find_direct_callers(data, target_rva):
    hits = []
    n = len(data)
    for i in range(n - 5):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        if i + 5 + rel == target_rva:
            hits.append(i)
    return hits


def list_calls_from(data, sym, syms, func):
    """Every direct E8 call made from inside `func`, in address order.

    This is the inverse of the caller search and the one that establishes
    ORDER: the offsets are where each call sits in the function body, so a
    callee at a smaller offset runs before one at a larger offset (barring
    branching, which is why offsets are printed rather than hidden).
    """
    lo = resolve(sym, func)
    below, above = sym.near(lo, syms)
    hi = above[0] if above else lo + 0x20000
    print(f"=== direct calls made from {func} ({hex(lo)}..{hig(hi)}) ===")
    out = []
    for i in range(lo, min(hi, len(data) - 5)):
        if data[i] != 0xE8:
            continue
        rel = struct.unpack_from("<i", data, i + 1)[0]
        tgt = i + 5 + rel
        if not (0 < tgt < len(data)):
            continue
        tb, _ = sym.near(tgt, syms)
        nm = f"{tb[1]}+0x{tgt - tb[0]:X}" if tb else "?"
        if tb and tgt == tb[0]:
            nm = tb[1]
        out.append((i - lo, tgt, nm))
    for off, tgt, nm in out:
        print(f"  +0x{off:<5X} -> {hex(tgt)}  {nm}")
    return out


def hig(v):
    return hex(v)


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit(__doc__)

    if args[0] == "--from":
        sym = pdbsym.Syms()
        syms = sym.all_syms()
        data = open(DUMP, "rb").read()
        for f in args[1:]:
            list_calls_from(data, sym, syms, f)
            print()
        return

    restrict = None
    if args[0] == "--in":
        restrict = args[1]
        args = args[2:]
    if not args:
        sys.exit(__doc__)

    if not os.path.exists(DUMP):
        sys.exit(f"kernel dump not found: {DUMP}\n"
                 "It is re-captured automatically on the next hypervisor run "
                 "(see KDUMP_PATH in Hypervisor.c).")
    data = open(DUMP, "rb").read()
    print(f"dump: {len(data):,} bytes")

    sym = pdbsym.Syms()
    syms = sym.all_syms()
    print(f"symbols: {len(syms):,}")

    lo = hi = None
    if restrict:
        lo = resolve(sym, restrict)
        below, above = sym.near(lo, syms)
        hi = above[0] if above else lo + 0x10000
        print(f"restricting to {restrict} = {hex(lo)}..{hex(hi)} "
              f"(next symbol {above[1] if above else '?'})")

    for tok in args:
        target = resolve(sym, tok)
        nm_below, _ = sym.near(target, syms)
        label = f"{nm_below[1]}+0x{target - nm_below[0]:X}" if nm_below else "?"
        print(f"\n=== direct E8 callers of {tok} ({hex(target)}, {label}) ===")
        hits = find_direct_callers(data, target)
        if not hits:
            print("  none (could still be called indirectly -- see docstring)")
        shown = 0
        for site in hits:
            if lo is not None and not (lo <= site < hi):
                continue
            b, _a = sym.near(site, syms)
            if b:
                print(f"  call at {hex(site)} = {b[1]}+0x{site - b[0]:X}")
            else:
                print(f"  call at {hex(site)} = (no containing symbol; low confidence)")
            shown += 1
        if lo is not None:
            print(f"  ({shown} of {len(hits)} total call sites are inside {restrict})")
        elif len(hits) > shown:
            print(f"  ({len(hits)} total)")


if __name__ == "__main__":
    main()
