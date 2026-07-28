"""Disassemble a range of the captured guest-kernel image, with symbols.

Committed alongside pdbsym.py/findcalls.py for the same reason: equivalent
one-off disassembly scripts were written into agent scratchpads twice and lost
to purges, each time costing a re-derivation.

A file offset in ntoskrnl_dump.bin IS an RVA (the dump is a memory image read
page-by-page from the module base), so no PE section translation is needed.

Usage:
    python tools/disasm.py HalpApicInitializeIoUnit
    python tools/disasm.py HalpApicInitializeIoUnit 0x148
    python tools/disasm.py 0x3A5780 0x120
    python tools/disasm.py --mark 0x101,0x103 HalpApicInitializeIoUnit

--mark takes function-relative offsets and flags those instructions with '>>>',
for pointing at RIPs observed by the U40 sampler.
"""
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pdbsym

DUMP = r"C:\LocalHost-evidence\ntoskrnl_dump.bin"


def main():
    args = sys.argv[1:]
    marks = set()
    if args and args[0] == "--mark":
        marks = {int(x, 16) for x in args[1].split(",")}
        args = args[2:]
    if not args:
        sys.exit(__doc__)

    sym = pdbsym.Syms()
    syms = sym.all_syms()

    tok = args[0]
    if tok.lower().startswith("0x"):
        start = int(tok, 16)
    else:
        start = sym.rva_of(tok)
        if start is None:
            sys.exit(f"symbol not found: {tok}")

    if len(args) > 1:
        length = int(args[1], 16)
    else:
        _b, above = sym.near(start, syms)
        length = (above[0] - start) if above else 0x100

    data = open(DUMP, "rb").read()
    code = data[start:start + length]

    below, _ = sym.near(start, syms)
    fname = below[1] if below else "?"
    print(f"=== {fname} @ {hex(start)}, {length:#x} bytes ===")

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    for insn in md.disasm(code, start):
        off = insn.address - start
        flag = ">>>" if off in marks else "   "
        line = f"{flag} +0x{off:<4X} {insn.address:08X}  {insn.mnemonic:<9} {insn.op_str}"
        # Annotate direct call/jmp targets with symbol names.
        if insn.mnemonic in ("call", "jmp") and insn.op_str.startswith("0x"):
            try:
                tgt = int(insn.op_str, 16)
                tb, _ = sym.near(tgt, syms)
                if tb:
                    d = tgt - tb[0]
                    line += f"   ; {tb[1]}" + (f"+0x{d:X}" if d else "")
            except ValueError:
                pass
        print(line)


if __name__ == "__main__":
    main()
