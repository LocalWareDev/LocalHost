"""Resolve guest-kernel symbol names <-> RVAs from ntkrnlmp.pdb via DbgHelp.

Lives in the repo on purpose. Equivalent throwaway scripts were written into an
agent scratchpad twice before (U25 tooling, then again for U27) and both times
the scratchpad was purged, costing a re-derivation each time. Committed here so
it survives.

Usage:
    python tools/pdbsym.py name RtlpHpLfhOwnerMoveSubsegment ExInitializePoolHeapManagement
    python tools/pdbsym.py rva  0x34412C 0x3C3B64
    python tools/pdbsym.py find "*Init*Pool*"
    python tools/pdbsym.py selftest

The PDB is expected at C:\\LocalHost-evidence\\ntkrnlmp.pdb (see the
KDUMP_DIR define in Hypervisor.c). Re-fetch it with:
  https://msdl.microsoft.com/download/symbols/ntkrnlmp.pdb/D9424FC4861E47C10FAD1B35DEC6DCC81/ntkrnlmp.pdb
That GUID+age (D9424FC4-861E-47C1-0FAD1B35DEC6DCC8, age 1) is the Win10 22H2
installer kernel used throughout the 0x139 investigation; the hypervisor prints
it at module discovery, so re-check it there if symbols ever look wrong.
"""
import ctypes
import ctypes.wintypes as wt
import fnmatch
import os
import sys

# Default is the guest kernel. Override with the PDBSYM_PDB environment variable
# to resolve a different module (e.g. acpi.pdb for driver RVAs from a bugcheck
# stack, which is how U49 identified the ACPI failure site).
PDB_PATH = os.environ.get("PDBSYM_PDB", r"C:\LocalHost-evidence\ntkrnlmp.pdb")
BASE = 0x10000000  # arbitrary synthetic load base; RVA = addr - BASE

# Known-good anchors, established independently by earlier units of the
# investigation. selftest checks these so a wrong PDB or a broken DbgHelp load
# is caught loudly instead of silently returning plausible-looking garbage.
ANCHORS = {
    "RtlpHpLfhOwnerMoveSubsegment": 0x34412C,  # U22/U23 fault site owner
    "RtlpHpLfhBucketGetSubsegment": 0x343B04,  # U25
    "KeBugCheckEx": 0x3FD6F0,                  # U21
    "ExAllocateHeapPool": 0x2369F0,            # U29
    "RtlpHpLfhSlotAllocate": 0x237620,         # U28/U29
}

MAX_SYM_NAME = 2000


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG),
        ("TypeIndex", wt.ULONG),
        ("Reserved", ctypes.c_ulonglong * 2),
        ("Index", wt.ULONG),
        ("Size", wt.ULONG),
        ("ModBase", ctypes.c_ulonglong),
        ("Flags", wt.ULONG),
        ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong),
        ("Register", wt.ULONG),
        ("Scope", wt.ULONG),
        ("Tag", wt.ULONG),
        ("NameLen", wt.ULONG),
        ("MaxNameLen", wt.ULONG),
        ("Name", ctypes.c_char * (MAX_SYM_NAME + 1)),
    ]


dbghelp = ctypes.WinDLL("dbghelp.dll")
PSYM_ENUMERATESYMBOLS_CALLBACK = ctypes.WINFUNCTYPE(
    wt.BOOL, ctypes.POINTER(SYMBOL_INFO), wt.ULONG, ctypes.c_void_p
)

# argtypes/restype are mandatory here, not optional hygiene: DbgHelp returns and
# takes DWORD64 values, and ctypes' default int return silently truncates them,
# which makes every symbol resolve to address 0 while still reporting success.
dbghelp.SymInitialize.argtypes = [ctypes.c_void_p, ctypes.c_char_p, wt.BOOL]
dbghelp.SymInitialize.restype = wt.BOOL
dbghelp.SymSetOptions.argtypes = [wt.DWORD]
dbghelp.SymSetOptions.restype = wt.DWORD
dbghelp.SymLoadModuleEx.argtypes = [
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
    ctypes.c_ulonglong, wt.DWORD, ctypes.c_void_p, wt.DWORD,
]
dbghelp.SymLoadModuleEx.restype = ctypes.c_ulonglong
dbghelp.SymFromName.argtypes = [ctypes.c_void_p, ctypes.c_char_p,
                                ctypes.POINTER(SYMBOL_INFO)]
dbghelp.SymFromName.restype = wt.BOOL
dbghelp.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong,
                                ctypes.POINTER(ctypes.c_ulonglong),
                                ctypes.POINTER(SYMBOL_INFO)]
dbghelp.SymFromAddr.restype = wt.BOOL
dbghelp.SymEnumSymbols.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong,
                                   ctypes.c_char_p,
                                   PSYM_ENUMERATESYMBOLS_CALLBACK,
                                   ctypes.c_void_p]
dbghelp.SymEnumSymbols.restype = wt.BOOL


def _alloc_syminfo():
    si = SYMBOL_INFO()
    # SizeOfStruct must describe the struct as if Name were char[1] -- the
    # trailing buffer is over-allocation DbgHelp writes into, not part of the
    # declared size. Hence +1 for that first Name byte.
    si.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - (MAX_SYM_NAME + 1) + 1
    si.MaxNameLen = MAX_SYM_NAME
    return si


class Syms:
    def __init__(self, pdb=PDB_PATH):
        if not os.path.exists(pdb):
            sys.exit(f"PDB not found: {pdb}\nSee the module docstring for the download URL.")
        self.proc = ctypes.c_void_p(0x1234)  # any unique pseudo-handle
        # SYMOPT_UNDNAME | SYMOPT_LOAD_ANYTHING. Deliberately NOT
        # SYMOPT_DEFERRED_LOADS: with a directly-loaded .pdb that defers the
        # symbol read and makes enumeration come back empty.
        dbghelp.SymSetOptions(0x00000002 | 0x00000040)
        if not dbghelp.SymInitialize(self.proc, None, False):
            sys.exit(f"SymInitialize failed: {ctypes.GetLastError()}")
        # Loading the .pdb directly as the "image" avoids needing a matching
        # ntoskrnl.exe on disk. Pass a real size -- 0 can make the load fail.
        self.base = dbghelp.SymLoadModuleEx(
            self.proc, None, pdb.encode(), None,
            ctypes.c_ulonglong(BASE), os.path.getsize(pdb), None, 0
        )
        if not self.base:
            sys.exit(f"SymLoadModuleEx failed: {ctypes.GetLastError()}")

    def rva_of(self, name):
        si = _alloc_syminfo()
        if not dbghelp.SymFromName(self.proc, name.encode(), ctypes.byref(si)):
            return None
        return si.Address - BASE

    def name_at(self, rva):
        si = _alloc_syminfo()
        disp = ctypes.c_ulonglong(0)
        if not dbghelp.SymFromAddr(self.proc, ctypes.c_ulonglong(BASE + rva),
                                   ctypes.byref(disp), ctypes.byref(si)):
            return None, 0
        return si.Name.decode(errors="replace"), disp.value

    def all_syms(self):
        """Every symbol as (rva, name), ascending. Used by `near`."""
        out = []

        def cb(psi, size, ctx):
            si = psi.contents
            out.append((si.Address - BASE, si.Name.decode(errors="replace")))
            return True

        dbghelp.SymEnumSymbols(self.proc, ctypes.c_ulonglong(self.base),
                               b"*", PSYM_ENUMERATESYMBOLS_CALLBACK(cb), None)
        return sorted(out)

    def near(self, rva, syms=None):
        """Closest symbol at or below rva, plus the next one above.

        SymFromAddr returns nothing for addresses in sparsely-symbolized
        regions (notably the discardable INIT section, where Phase 0/1 boot
        code lives), which is exactly where this investigation's callers keep
        landing. Bracketing the address between the nearest public symbols
        still localizes it.
        """
        syms = syms if syms is not None else self.all_syms()
        below = above = None
        for r, nm in syms:
            if r <= rva:
                below = (r, nm)
            else:
                above = (r, nm)
                break
        return below, above

    def match(self, pattern):
        out = []

        def cb(psi, size, ctx):
            si = psi.contents
            nm = si.Name.decode(errors="replace")
            if fnmatch.fnmatch(nm.lower(), pattern.lower()):
                out.append((nm, si.Address - BASE))
            return True

        dbghelp.SymEnumSymbols(self.proc, ctypes.c_ulonglong(self.base),
                               b"*", PSYM_ENUMERATESYMBOLS_CALLBACK(cb), None)
        return sorted(out, key=lambda t: t[1])


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    mode = sys.argv[1].lower()
    s = Syms()

    if mode == "selftest":
        bad = 0
        for nm, want in ANCHORS.items():
            got = s.rva_of(nm)
            ok = got == want
            bad += 0 if ok else 1
            print(f"  {'ok  ' if ok else 'FAIL'} {nm}: got "
                  f"{'None' if got is None else hex(got)}, want {hex(want)}")
        print("\nSELFTEST PASSED" if not bad else f"\n{bad} ANCHOR(S) MISMATCHED -- do not trust this PDB")
        sys.exit(1 if bad else 0)

    if mode == "name":
        for nm in sys.argv[2:]:
            r = s.rva_of(nm)
            print(f"{nm} = {'NOT FOUND' if r is None else hex(r)}")
    elif mode == "rva":
        for a in sys.argv[2:]:
            rva = int(a, 16)
            nm, disp = s.name_at(rva)
            print(f"{hex(rva)} = {nm or 'NOT FOUND'}" + (f"+0x{disp:X}" if nm and disp else ""))
    elif mode == "near":
        syms = s.all_syms()
        print(f"({len(syms)} symbols loaded)")
        for a in sys.argv[2:]:
            rva = int(a, 16)
            below, above = s.near(rva, syms)
            print(f"\n{hex(rva)}:")
            if below:
                print(f"  <= {below[1]}+0x{rva - below[0]:X}  (sym at {hex(below[0])})")
            else:
                print("  <= (nothing below)")
            if above:
                print(f"  next above: {above[1]} at {hex(above[0])} (+0x{above[0] - rva:X} past)")
    elif mode == "find":
        for nm, rva in s.match(sys.argv[2]):
            print(f"{hex(rva)}  {nm}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
