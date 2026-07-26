import ctypes
from ctypes import wintypes

DBGHELP_PATH = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\dbghelp.dll"
SYMBOL_DIR = r"C:\Users\DELL\AppData\Local\Temp\claude\c--Users-DELL-OneDrive-Desktop-LocalHost-py\5bed7398-339c-497a-9383-821cf9d2769e\scratchpad"
PDB_PATH = SYMBOL_DIR + r"\ntkrnlmp.pdb"

dbghelp = ctypes.WinDLL(DBGHELP_PATH)

SymSetOptions = dbghelp.SymSetOptions
SymSetOptions.argtypes = [wintypes.DWORD]
SymSetOptions.restype = wintypes.DWORD

SymInitializeW = dbghelp.SymInitializeW
SymInitializeW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, wintypes.BOOL]
SymInitializeW.restype = wintypes.BOOL

SymLoadModuleExW = dbghelp.SymLoadModuleExW
SymLoadModuleExW.argtypes = [wintypes.HANDLE, wintypes.HANDLE, wintypes.LPCWSTR, wintypes.LPCWSTR,
                              ctypes.c_uint64, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD]
SymLoadModuleExW.restype = ctypes.c_uint64

class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wintypes.DWORD),
        ("TypeIndex", wintypes.DWORD),
        ("Reserved", ctypes.c_uint64 * 2),
        ("Index", wintypes.DWORD),
        ("Size", wintypes.DWORD),
        ("ModBase", ctypes.c_uint64),
        ("Flags", wintypes.DWORD),
        ("Value", ctypes.c_uint64),
        ("Address", ctypes.c_uint64),
        ("Register", wintypes.DWORD),
        ("Scope", wintypes.DWORD),
        ("Tag", wintypes.DWORD),
        ("NameLen", wintypes.DWORD),
        ("MaxNameLen", wintypes.DWORD),
        ("Name", ctypes.c_wchar * 2000),
    ]

SymFromAddrW = dbghelp.SymFromAddrW
SymFromAddrW.argtypes = [wintypes.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
SymFromAddrW.restype = wintypes.BOOL

SymFromNameW = dbghelp.SymFromNameW
SymFromNameW.argtypes = [wintypes.HANDLE, wintypes.LPCWSTR, ctypes.POINTER(SYMBOL_INFOW)]
SymFromNameW.restype = wintypes.BOOL

GetLastError = ctypes.windll.kernel32.GetLastError

SYMOPT_DEBUG = 0x80000000
SYMOPT_LOAD_LINES = 0x00000010
SYMOPT_UNDNAME = 0x00000002
SymSetOptions(SYMOPT_DEBUG | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME)

hProcess = ctypes.c_void_p(1)  # fake handle, valid for offline symbol resolution
ok = SymInitializeW(hProcess, SYMBOL_DIR, False)
print(f"SymInitializeW: {ok} (err={GetLastError()})")

FAKE_BASE = 0x10000000
base = SymLoadModuleExW(hProcess, None, PDB_PATH, None, FAKE_BASE, 0x1000000, None, 0)
print(f"SymLoadModuleExW: base=0x{base:X} (err={GetLastError()})")

# RVAs observed via the stall watchdog: RIP cycled among these addresses
# (offsets into ntkrnlmp.pdb, RAX consistently held the lowest one, 0x4BE7D0,
# suggesting it's the loop's own start/retry address).
targets = {
    "case 25 check: global qword tested for nonzero": 0xC4BE68,
    "case 25 failure branch target (cold path)": 0x4A9092,
}

info = SYMBOL_INFOW()
info.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW) - 2000 * ctypes.sizeof(ctypes.c_wchar)
info.MaxNameLen = 2000

for label, rva in targets.items():
    addr = FAKE_BASE + rva
    disp = ctypes.c_uint64(0)
    ok = SymFromAddrW(hProcess, addr, ctypes.byref(disp), ctypes.byref(info))
    if ok:
        print(f"{label} (RVA 0x{rva:X}): {info.Name}+0x{disp.value:X}")
    else:
        print(f"{label} (RVA 0x{rva:X}): FAILED err={GetLastError()}")

for name in ("KiBugCheckData", "KeBugCheckEx"):
    info2 = SYMBOL_INFOW()
    info2.SizeOfStruct = ctypes.sizeof(SYMBOL_INFOW) - 2000 * ctypes.sizeof(ctypes.c_wchar)
    info2.MaxNameLen = 2000
    ok = SymFromNameW(hProcess, name, ctypes.byref(info2))
    if ok:
        print(f"{name}: address=0x{info2.Address:X} -> RVA=0x{info2.Address - FAKE_BASE:X}")
    else:
        print(f"{name}: FAILED err={GetLastError()}")
