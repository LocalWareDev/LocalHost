import sys
import ctypes
from ctypes import wintypes
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

SCRATCH = r"C:\Users\DELL\AppData\Local\Temp\claude\c--Users-DELL-OneDrive-Desktop-LocalHost-py\5bed7398-339c-497a-9383-821cf9d2769e\scratchpad"
DBGHELP_PATH = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll"
PDB_PATH = SCRATCH + r"\ntkrnlmp.pdb"

dbghelp = ctypes.WinDLL(DBGHELP_PATH)
SymSetOptions = dbghelp.SymSetOptions
SymSetOptions.argtypes = [wintypes.DWORD]
SymInitializeW = dbghelp.SymInitializeW
SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
SymInitializeW.restype = wintypes.BOOL
SymLoadModuleExW = dbghelp.SymLoadModuleExW
SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                              ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
SymLoadModuleExW.restype = ctypes.c_uint64

class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.DWORD), ("TypeIndex", wintypes.DWORD),
        ("Reserved", ctypes.c_uint64 * 2), ("Index", wintypes.DWORD), ("Size", wintypes.DWORD),
        ("ModBase", ctypes.c_uint64), ("Flags", wintypes.DWORD), ("Value", ctypes.c_uint64),
        ("Address", ctypes.c_uint64), ("Register", wintypes.DWORD), ("Scope", wintypes.DWORD),
        ("Tag", wintypes.DWORD), ("NameLen", wintypes.DWORD), ("MaxNameLen", wintypes.DWORD),
        ("Name", ctypes.c_wchar * 2000),
    ]

SymFromAddrW = dbghelp.SymFromAddrW
SymFromAddrW.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
SymFromAddrW.restype = wintypes.BOOL

SymSetOptions(0x80000000 | 0x10 | 0x2)  # DEBUG | LOAD_LINES | UNDNAME
hProcess = ctypes.c_void_p(1)
SymInitializeW(hProcess, SCRATCH, False)
FAKE_BASE = 0x10000000
SymLoadModuleExW(hProcess, None, PDB_PATH, None, FAKE_BASE, 0x1000000, None, 0)

def sym_at(rva):
    info = SYMBOL_INFOW()
    info.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW) - 2000 * ctypes.sizeof(ctypes.c_wchar)
    info.MaxNameLen = 2000
    disp = ctypes.c_uint64(0)
    ok = SymFromAddrW(hProcess, FAKE_BASE + rva, ctypes.byref(disp), ctypes.byref(info))
    if ok:
        if disp.value == 0:
            return info.Name
        return f"{info.Name}+0x{disp.value:X}"
    return None

name = sys.argv[1]
start_rva = int(sys.argv[2], 16)
with open(SCRATCH + f"\\{name}.bin", "rb") as f:
    code = f.read()

md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = True
for insn in md.disasm(code, FAKE_BASE + start_rva):
    rva = insn.address - FAKE_BASE
    annotation = ""
    if insn.mnemonic in ("call", "jmp") and insn.op_str.startswith("0x"):
        target = int(insn.op_str, 16)
        target_rva = target - FAKE_BASE
        sym = sym_at(target_rva)
        annotation = f"    ; RVA 0x{target_rva:X} {sym or ''}"
    print(f"0x{rva:06X}:\t{insn.mnemonic}\t{insn.op_str}{annotation}")
