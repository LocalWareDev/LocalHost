"""
LocalHost Workstation - Main GUI Skeleton
VMware Workstation-style layout:
- Left: Library tree (Home + My Computer > VM list), with a search box
- Home page: 3 big buttons (Get Started, Create a New Virtual Machine, Connect to VPS)
- VM page: header + Power on/Edit links + Devices list + Preview pane
- No Description box, no Virtual Machine Details box
- Menu bar: File, Edit, View, VM, Manage Snapshots, Help
- Right-click sidebar: context menu (VM row vs. empty space)
"""

import sys
import os
import re
import json
import time
import datetime
import platform
import ctypes
import ctypes.wintypes as wintypes
import threading
from pathlib import Path

# Optional: powers the App Utilisation panel's CPU/RAM/Disk figures. The panel
# degrades to "unavailable" rather than the app failing to start, since resource
# monitoring is a convenience and running VMs is not.
try:
    import psutil
except ImportError:
    psutil = None

import PySide6
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QSplitter, QVBoxLayout, QHBoxLayout,
    QTreeWidget, QTreeWidgetItem, QLabel, QFrame, QToolBar, QPushButton,
    QSizePolicy, QMenuBar, QStatusBar, QStackedWidget,
    QDialog, QFormLayout, QLineEdit, QSpinBox, QComboBox, QMessageBox,
    QFileDialog, QDialogButtonBox, QMenu, QInputDialog, QAbstractItemView,
    QListWidget, QListWidgetItem, QProgressBar
)
from PySide6.QtGui import QAction, QIcon, QDesktopServices
from PySide6.QtCore import Qt, QSize, QStandardPaths, QProcess, QTimer, Signal, QUrl


# There's no CI/tagging pipeline behind this project -- these are
# maintained by hand and should be bumped alongside any future release.
APP_VERSION = "1.0.0"
APP_RELEASE_TAG = "v1.0.0"
APP_RELEASE_DATE = "July 13, 2026"


def _create_sparse_disk_image(path, size_bytes):
    """Create a raw disk image of size_bytes, marked sparse on NTFS so the
    logical size (e.g. 60 GB) doesn't reserve real disk space -- only the
    bytes the guest actually writes ever hit the filesystem.

    Deliberately bypasses Python's own file.truncate(): on this codebase's
    target environment it was observed to hang indefinitely when growing a
    file to a large size (tens of GB), even though the equivalent raw
    SetFilePointerEx+SetEndOfFile call below completes instantly. Cause
    unconfirmed (suspected AV/CRT-layer interaction) -- the raw API sidesteps
    it entirely, which is also exactly what real disk-image tools use.
    """
    FSCTL_SET_SPARSE = 0x000900C4
    GENERIC_WRITE = 0x40000000
    CREATE_ALWAYS = 2
    FILE_ATTRIBUTE_NORMAL = 0x80
    FILE_BEGIN = 0

    kernel32 = ctypes.windll.kernel32
    kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel32.CreateFileW.restype = ctypes.c_void_p
    kernel32.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                                          ctypes.c_void_p]
    kernel32.DeviceIoControl.restype = ctypes.c_int
    kernel32.SetFilePointerEx.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p, ctypes.c_uint32]
    kernel32.SetEndOfFile.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    handle = kernel32.CreateFileW(str(path), GENERIC_WRITE, 0, None,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, None)
    if handle in (None, 0, -1):
        return False
    try:
        bytes_returned = ctypes.c_uint32(0)
        kernel32.DeviceIoControl(handle, FSCTL_SET_SPARSE, None, 0,
                                  None, 0, ctypes.byref(bytes_returned), None)
        if not kernel32.SetFilePointerEx(handle, ctypes.c_int64(size_bytes), None, FILE_BEGIN):
            return False
        return bool(kernel32.SetEndOfFile(handle))
    finally:
        kernel32.CloseHandle(handle)


def _resize_sparse_disk_image(path, new_size_bytes):
    """Resize an existing sparse disk image to new_size_bytes. Growing just
    moves the logical end-of-file forward -- the newly exposed region reads
    as zero until the guest writes to it, same as the initial allocation
    above. Shrinking truncates the file at new_size_bytes, permanently
    discarding anything stored past that point -- callers must confirm with
    the user before shrinking. Opens with no sharing, so this naturally
    fails while the VM that owns the disk is still running and holding it
    open."""
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    FILE_ATTRIBUTE_NORMAL = 0x80
    FILE_BEGIN = 0

    kernel32 = ctypes.windll.kernel32
    kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel32.CreateFileW.restype = ctypes.c_void_p
    kernel32.SetFilePointerEx.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p, ctypes.c_uint32]
    kernel32.SetEndOfFile.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    handle = kernel32.CreateFileW(str(path), GENERIC_WRITE, 0, None,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
    if handle in (None, 0, -1):
        return False
    try:
        if not kernel32.SetFilePointerEx(handle, ctypes.c_int64(new_size_bytes), None, FILE_BEGIN):
            return False
        return bool(kernel32.SetEndOfFile(handle))
    finally:
        kernel32.CloseHandle(handle)


class _FILE_ALLOCATED_RANGE_BUFFER(ctypes.Structure):
    _fields_ = [("FileOffset", ctypes.c_int64), ("Length", ctypes.c_int64)]


def _copy_sparse_disk_image(src_path, dst_path):
    """Copy a raw disk image for a snapshot, preserving sparseness: only
    the byte ranges the guest has actually written get copied, so
    snapshotting an 80 GB disk that's a few hundred MB of real data takes
    proportionally that long, not 80 GB worth of I/O -- same spirit as
    _create_sparse_disk_image's initial allocation. Falls back to a plain
    full copy if the allocated-ranges query isn't supported (e.g. the
    disks folder ends up on a non-NTFS volume) so a snapshot is never
    silently empty."""
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_SHARE_READ = 0x00000001
    OPEN_EXISTING = 3
    CREATE_ALWAYS = 2
    FILE_ATTRIBUTE_NORMAL = 0x80
    FILE_BEGIN = 0
    FSCTL_SET_SPARSE = 0x000900C4
    FSCTL_QUERY_ALLOCATED_RANGES = 0x000940CF
    CHUNK_SIZE = 4 * 1024 * 1024

    kernel32 = ctypes.windll.kernel32
    kernel32.CreateFileW.restype = ctypes.c_void_p
    kernel32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                                      ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
    kernel32.GetFileSizeEx.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int64)]
    kernel32.SetFilePointerEx.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_void_p, ctypes.c_uint32]
    kernel32.SetEndOfFile.argtypes = [ctypes.c_void_p]
    kernel32.ReadFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                   ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    kernel32.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
    kernel32.DeviceIoControl.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                                          ctypes.c_void_p, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32),
                                          ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]

    def copy_range(src, dst, offset, length, chunk):
        kernel32.SetFilePointerEx(src, ctypes.c_int64(offset), None, FILE_BEGIN)
        kernel32.SetFilePointerEx(dst, ctypes.c_int64(offset), None, FILE_BEGIN)
        remaining = length
        while remaining > 0:
            to_read = min(remaining, len(chunk))
            n_read = ctypes.c_uint32(0)
            if not kernel32.ReadFile(src, chunk, to_read, ctypes.byref(n_read), None) or n_read.value == 0:
                return False
            n_written = ctypes.c_uint32(0)
            if not kernel32.WriteFile(dst, chunk, n_read.value, ctypes.byref(n_written), None):
                return False
            remaining -= n_read.value
        return True

    src = kernel32.CreateFileW(str(src_path), GENERIC_READ, FILE_SHARE_READ, None,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
    if src in (None, 0, -1):
        return False
    try:
        total_size = ctypes.c_int64(0)
        if not kernel32.GetFileSizeEx(src, ctypes.byref(total_size)):
            return False
        total_size = total_size.value

        dst = kernel32.CreateFileW(str(dst_path), GENERIC_WRITE, 0, None,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, None)
        if dst in (None, 0, -1):
            return False
        try:
            bytes_returned = ctypes.c_uint32(0)
            kernel32.DeviceIoControl(dst, FSCTL_SET_SPARSE, None, 0, None, 0,
                                      ctypes.byref(bytes_returned), None)
            if not kernel32.SetFilePointerEx(dst, ctypes.c_int64(total_size), None, FILE_BEGIN):
                return False
            if not kernel32.SetEndOfFile(dst):
                return False
            if total_size == 0:
                return True

            chunk = ctypes.create_string_buffer(CHUNK_SIZE)
            out_buf = (_FILE_ALLOCATED_RANGE_BUFFER * 1024)()
            scan_offset = 0
            first_query = True
            while scan_offset < total_size:
                query = _FILE_ALLOCATED_RANGE_BUFFER(scan_offset, total_size - scan_offset)
                returned = ctypes.c_uint32(0)
                ok = kernel32.DeviceIoControl(
                    src, FSCTL_QUERY_ALLOCATED_RANGES,
                    ctypes.byref(query), ctypes.sizeof(query),
                    out_buf, ctypes.sizeof(out_buf),
                    ctypes.byref(returned), None
                )
                if not ok and first_query:
                    # Sparse-range querying isn't supported here at all --
                    # fall back to a plain full copy rather than leaving
                    # the snapshot silently empty.
                    return copy_range(src, dst, 0, total_size, chunk)
                first_query = False
                count = returned.value // ctypes.sizeof(_FILE_ALLOCATED_RANGE_BUFFER)
                if count == 0:
                    break
                for i in range(count):
                    rng = out_buf[i]
                    if not copy_range(src, dst, rng.FileOffset, rng.Length, chunk):
                        return False
                last = out_buf[count - 1]
                scan_offset = last.FileOffset + last.Length
                if count < len(out_buf):
                    break
            return True
        finally:
            kernel32.CloseHandle(dst)
    finally:
        kernel32.CloseHandle(src)


def _parse_memory_to_mb(text, default=4096):
    m = re.match(r"\s*([\d.]+)\s*(GB|MB)", text or "", re.IGNORECASE)
    if not m:
        return default
    value = float(m.group(1))
    return int(value * 1024) if m.group(2).upper() == "GB" else int(value)


def _parse_disk_gb(text, default=60):
    m = re.match(r"\s*(\d+)\s*GB", text or "", re.IGNORECASE)
    return int(m.group(1)) if m else default


# ---------------------------------------------------------------------------
# ISO inspection for the New VM Wizard -- validates that a selected file is
# actually an ISO 9660 image (not just judging by its .iso extension) and,
# if so, makes a best-effort guess at the guest OS from the image's own
# bytes so the wizard can pre-fill sensible defaults.
# ---------------------------------------------------------------------------
ISO_SECTOR_SIZE = 2048
ISO_PVD_SECTOR = 16  # the Primary Volume Descriptor always lives at sector 16

# (guest OS label, marker substrings to look for, recommended MB, recommended GB)
# Ordered most-specific-first so e.g. "Windows 11" is tried before the
# generic "Windows" fallback.
_OS_SIGNATURES = [
    ("Windows 11 (64-bit)", ("WIN11", "WINDOWS 11"), 4096, 64),
    ("Windows 10 (64-bit)", ("WIN10", "WINDOWS 10"), 4096, 64),
    ("Windows (64-bit)", ("MICROSOFT", "WINDOWS", "BOOTMGR", "CCCOMA"), 4096, 64),
    ("Ubuntu 64-bit", ("UBUNTU",), 2048, 25),
    ("Debian 64-bit", ("DEBIAN",), 2048, 20),
    ("Fedora 64-bit", ("FEDORA",), 2048, 20),
    ("Rocky Linux 64-bit", ("ROCKY",), 2048, 20),
    ("AlmaLinux 64-bit", ("ALMALINUX", "ALMA LINUX"), 2048, 20),
    ("CentOS 64-bit", ("CENTOS",), 2048, 20),
    ("Red Hat Enterprise Linux 64-bit", ("RHEL", "RED HAT ENTERPRISE"), 2048, 20),
    ("openSUSE 64-bit", ("OPENSUSE", "SUSE"), 2048, 20),
    ("Linux Mint 64-bit", ("LINUX MINT",), 2048, 20),
    ("Arch Linux 64-bit", ("ARCH LINUX", "ARCHLINUX"), 1024, 20),
]


def _is_valid_iso9660(path):
    """Real validation instead of trusting the .iso extension: every ISO
    9660 image -- which is what essentially all bootable OS installer
    discs are, Windows included via the El Torito extension -- has a
    Primary Volume Descriptor at sector 16 whose bytes 1-5 spell out the
    "CD001" identifier. That's the format's own self-identifying magic
    per ECMA-119, so checking it catches a renamed .txt file or a
    truncated/corrupt download regardless of what the filename claims."""
    try:
        with open(path, "rb") as f:
            f.seek(ISO_PVD_SECTOR * ISO_SECTOR_SIZE)
            header = f.read(6)
    except OSError:
        return False
    return len(header) == 6 and header[1:6] == b"CD001"


def _read_iso_volume_label(path):
    """Primary Volume Descriptor layout (ECMA-119 8.4): the system
    identifier is bytes 8-40 and the volume identifier is bytes 40-72 of
    the descriptor's sector -- both fixed-width, space-padded fields.
    Official install media usually sets these to something descriptive,
    e.g. "UBUNTU 22_04_3 LTS AMD64" or "CCCOMA_X64FRE_EN-US_DV9"."""
    try:
        with open(path, "rb") as f:
            f.seek(ISO_PVD_SECTOR * ISO_SECTOR_SIZE)
            pvd = f.read(ISO_SECTOR_SIZE)
    except OSError:
        return ""
    if len(pvd) < 72:
        return ""
    system_id = pvd[8:40].decode("ascii", errors="ignore")
    volume_id = pvd[40:72].decode("ascii", errors="ignore")
    return f"{system_id} {volume_id}".strip()


def detect_guest_os(path, scan_bytes=8 * 1024 * 1024):
    """Best-effort guest OS detection for an already-validated ISO 9660
    image: checks the volume label first (cheap and usually specific
    enough on its own), then falls back to scanning the first scan_bytes
    of the image for the same marker strings (catches images with a
    generic/blank volume label -- most of an ISO's own directory/file
    names and any boot-loader strings live well within the first few MB).
    Returns (label, memory_mb, disk_gb) for the first signature that
    matches, or None if nothing recognizable turned up. Callers must treat
    None as "let the user pick manually" rather than a failure -- this is
    a convenience, not a requirement for creating the VM."""
    label_text = _read_iso_volume_label(path).upper()
    for label, markers, memory_mb, disk_gb in _OS_SIGNATURES:
        if any(marker in label_text for marker in markers):
            return label, memory_mb, disk_gb

    try:
        with open(path, "rb") as f:
            content = f.read(scan_bytes).upper()
    except OSError:
        return None
    for label, markers, memory_mb, disk_gb in _OS_SIGNATURES:
        if any(marker.encode("ascii") in content for marker in markers):
            return label, memory_mb, disk_gb
    return None


# ---------------------------------------------------------------------------
# Win32 window embedding -- the hypervisor runs as its own process with its
# own top-level window (the guest display). Rather than leaving that as a
# separate floating window, we reparent it into our preview pane (making it
# a WS_CHILD of that QFrame) and can pop it back out to a borderless
# WS_POPUP window covering the whole screen for a real fullscreen mode.
# ---------------------------------------------------------------------------
_user32 = ctypes.windll.user32
_user32.FindWindowW.restype = ctypes.c_void_p
_user32.FindWindowW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p]
_user32.SetParent.restype = ctypes.c_void_p
_user32.SetParent.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_user32.GetWindowLongPtrW.restype = ctypes.c_longlong
_user32.GetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int]
_user32.SetWindowLongPtrW.restype = ctypes.c_longlong
_user32.SetWindowLongPtrW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_longlong]
_user32.MoveWindow.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int, ctypes.c_bool]
_user32.SetWindowPos.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_uint]
_user32.ShowWindow.argtypes = [ctypes.c_void_p, ctypes.c_int]
_user32.SetForegroundWindow.argtypes = [ctypes.c_void_p]
_user32.GetForegroundWindow.restype = ctypes.c_void_p
_user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
_user32.GetKeyState.restype = ctypes.c_short
_user32.GetKeyState.argtypes = [ctypes.c_int]

GWL_STYLE = -16
WS_CHILD = 0x40000000
WS_POPUP = 0x80000000
WS_VISIBLE = 0x10000000
WS_CAPTION = 0x00C00000
WS_THICKFRAME = 0x00040000
WS_MINIMIZEBOX = 0x00020000
WS_MAXIMIZEBOX = 0x00010000
WS_SYSMENU = 0x00080000
WS_DECORATIONS = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU
SWP_NOZORDER = 0x0004
SWP_NOMOVE = 0x0002
SWP_NOSIZE = 0x0001
SWP_FRAMECHANGED = 0x0020
SW_SHOW = 5
SW_HIDE = 0


def _win32_async(fn, *args):
    """Run a raw Win32 HWND call (SetParent, SetWindowLongPtr, MoveWindow, ...)
    on a background thread.

    Calling these directly from a Qt timer/event callback risks a Windows
    fatal exception (0x8001010D, RPC_E_CANTCALLOUT_ININPUTSYNCCALL): if the
    Qt main thread happens to be mid-dispatch of some other synchronous
    window message when we try to make our own outgoing cross-process
    window call, COM's reentrancy guard kills the process outright rather
    than raising a catchable Python exception. A plain background thread
    isn't inside that dispatch, so it isn't subject to the same guard.
    """
    threading.Thread(target=fn, args=args, daemon=True).start()


# ---------------------------------------------------------------------------
# Global low-level keyboard hook for Shift+F11 (VM fullscreen toggle).
#
# A plain QShortcut only fires for key events Qt's own event loop sees.
# Once a VM is embedded, its guest-display window is a *foreign HWND* --
# owned by Hypervisor.exe, a different process with its own window and
# thread. Click into that region and keyboard focus goes to that window,
# not any Qt widget; Qt never receives the keystroke at all, so the
# shortcut silently never fires. A WH_KEYBOARD_LL hook intercepts key
# events at the OS level before Windows routes them to whichever window
# has focus, sidestepping that entirely.
# ---------------------------------------------------------------------------
WH_KEYBOARD_LL = 13
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_SYSKEYDOWN = 0x0104
WM_SYSKEYUP = 0x0105
VK_F11 = 0x7A
VK_SHIFT = 0x10
HC_ACTION = 0


class KBDLLHOOKSTRUCT(ctypes.Structure):
    _fields_ = [
        ("vkCode", wintypes.DWORD),
        ("scanCode", wintypes.DWORD),
        ("flags", wintypes.DWORD),
        ("time", wintypes.DWORD),
        ("dwExtraInfo", ctypes.c_void_p),
    ]


LowLevelKeyboardProc = ctypes.WINFUNCTYPE(ctypes.c_long, ctypes.c_int, wintypes.WPARAM,
                                           ctypes.POINTER(KBDLLHOOKSTRUCT))

_user32.SetWindowsHookExW.restype = ctypes.c_void_p
_user32.SetWindowsHookExW.argtypes = [ctypes.c_int, LowLevelKeyboardProc, ctypes.c_void_p, wintypes.DWORD]
_user32.CallNextHookEx.restype = ctypes.c_long
_user32.CallNextHookEx.argtypes = [ctypes.c_void_p, ctypes.c_int, wintypes.WPARAM, ctypes.c_void_p]
_user32.UnhookWindowsHookEx.argtypes = [ctypes.c_void_p]


# ---------------------------------------------------------------------------
# Best-effort "suspend": the hypervisor has no snapshot/save-state format to
# suspend-to-disk into, so a real VMware-style suspend isn't on the table.
# What we CAN do is freeze the guest in place by suspending every thread in
# its process (a standard debugger-style process freeze via Toolhelp32) and
# resume it later with full state intact -- the guest just stops consuming
# CPU and stops responding until resumed, rather than actually persisting.
# ---------------------------------------------------------------------------
_kernel32 = ctypes.windll.kernel32
TH32CS_SNAPTHREAD = 0x00000004
THREAD_SUSPEND_RESUME = 0x0002
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ThreadID", wintypes.DWORD),
        ("th32OwnerProcessID", wintypes.DWORD),
        ("tpBasePri", ctypes.c_long),
        ("tpDeltaPri", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
    ]


_kernel32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
_kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
_kernel32.Thread32First.argtypes = [ctypes.c_void_p, ctypes.POINTER(THREADENTRY32)]
_kernel32.Thread32Next.argtypes = [ctypes.c_void_p, ctypes.POINTER(THREADENTRY32)]
_kernel32.OpenThread.restype = ctypes.c_void_p
_kernel32.OpenThread.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_kernel32.SuspendThread.argtypes = [ctypes.c_void_p]
_kernel32.ResumeThread.argtypes = [ctypes.c_void_p]
_kernel32.CloseHandle.argtypes = [ctypes.c_void_p]


def _process_thread_ids(pid):
    snap = _kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    if not snap or snap == INVALID_HANDLE_VALUE:
        return []
    tids = []
    try:
        entry = THREADENTRY32()
        entry.dwSize = ctypes.sizeof(THREADENTRY32)
        found = _kernel32.Thread32First(snap, ctypes.byref(entry))
        while found:
            if entry.th32OwnerProcessID == pid:
                tids.append(entry.th32ThreadID)
            found = _kernel32.Thread32Next(snap, ctypes.byref(entry))
    finally:
        _kernel32.CloseHandle(snap)
    return tids


def _suspend_process(pid):
    tids = _process_thread_ids(pid)
    ok = bool(tids)
    for tid in tids:
        handle = _kernel32.OpenThread(THREAD_SUSPEND_RESUME, False, tid)
        if not handle:
            ok = False
            continue
        if _kernel32.SuspendThread(handle) == 0xFFFFFFFF:
            ok = False
        _kernel32.CloseHandle(handle)
    return ok


def _resume_process(pid):
    for tid in _process_thread_ids(pid):
        handle = _kernel32.OpenThread(THREAD_SUSPEND_RESUME, False, tid)
        if handle:
            _kernel32.ResumeThread(handle)
            _kernel32.CloseHandle(handle)


# ---------------------------------------------------------------------------
# VMs are launched via QProcess.startDetached() rather than a normal owned
# QProcess -- a QProcess kills its child when the QProcess object itself is
# destroyed (confirmed: even just dropping the last Python reference and
# letting it get garbage-collected kills a still-running child), which would
# defeat "run in background"/"suspend on close" the moment the app actually
# exits. A detached process is a genuine, independent OS process with no
# such teardown hook, so it survives the manager closing outright. The
# tradeoff is losing QProcess's finished signal, waitForStarted(), etc. --
# tracked by PID instead, with a poll timer standing in for the signal and
# these two helpers standing in for QProcess.kill()/state checks.
# ---------------------------------------------------------------------------
PROCESS_TERMINATE = 0x0001
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
STILL_ACTIVE = 259

_kernel32.OpenProcess.restype = ctypes.c_void_p
_kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
_kernel32.TerminateProcess.argtypes = [ctypes.c_void_p, wintypes.UINT]
_kernel32.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]


def _process_is_alive(pid):
    handle = _kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        return False
    try:
        exit_code = wintypes.DWORD()
        if not _kernel32.GetExitCodeProcess(handle, ctypes.byref(exit_code)):
            return False
        return exit_code.value == STILL_ACTIVE
    finally:
        _kernel32.CloseHandle(handle)


def _terminate_process(pid):
    handle = _kernel32.OpenProcess(PROCESS_TERMINATE, False, pid)
    if not handle:
        return False
    try:
        return bool(_kernel32.TerminateProcess(handle, 0))
    finally:
        _kernel32.CloseHandle(handle)


# ---------------------------------------------------------------------------
# Dummy VM data model — swap this out for your real vmrun/Hyper-V backend
# ---------------------------------------------------------------------------
# ----------------------------------------------------------------------
# App Utilisation sampling
# ----------------------------------------------------------------------
# GPU usage per process, read the same way Task Manager reads it: the PDH
# "GPU Engine" counter set, whose instance names embed the owning PID.
#
# psutil has no GPU support at all, and the vendor tools are no use here --
# nvidia-smi is NVIDIA-only and generally cannot attribute usage to a process
# on consumer drivers. PDH is the only source that is both vendor-neutral and
# per-process, which is exactly what an "App Utilisation" panel needs.
#
# An instance name looks like:
#   pid_1234_luid_0x00000000_0x0000ABCD_phys_0_eng_0_engtype_3D
# so the PID is matched from the front and the engine type from the back.
PDH_FMT_DOUBLE = 0x00000200
PDH_MORE_DATA = 0x800007D2
_GPU_COUNTER_PATH = r"\GPU Engine(*)\Utilization Percentage"
_GPU_INSTANCE_RE = re.compile(r"^pid_(\d+)_.*_engtype_(.+)$", re.IGNORECASE)


class PDH_FMT_COUNTERVALUE(ctypes.Structure):
    _fields_ = [("CStatus", wintypes.DWORD), ("doubleValue", ctypes.c_double)]


class PDH_FMT_COUNTERVALUE_ITEM_W(ctypes.Structure):
    _fields_ = [("szName", wintypes.LPWSTR), ("FmtValue", PDH_FMT_COUNTERVALUE)]


class _GpuUsageCounter:
    """Per-process GPU utilisation via PDH. Unavailable is a normal outcome
    (no GPU counters on older Windows, or in some remote sessions) -- callers
    get None and show a dash rather than a wrong number."""

    def __init__(self):
        self.available = False
        self._query = None
        self._counter = None
        try:
            self._pdh = ctypes.WinDLL("pdh.dll")
        except OSError:
            return
        # PDH_STATUS must be read as UNSIGNED. The status codes are defined as
        # 0x8000xxxx, and under ctypes' default signed-int return type
        # PDH_MORE_DATA comes back as -2147481646 -- so the "buffer too small"
        # check silently never matches and the counter array is never fetched.
        for fn in ("PdhOpenQueryW", "PdhAddEnglishCounterW", "PdhCollectQueryData",
                   "PdhGetFormattedCounterArrayW", "PdhCloseQuery"):
            getattr(self._pdh, fn).restype = wintypes.DWORD
        self._pdh.PdhOpenQueryW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p,
                                            ctypes.POINTER(ctypes.c_void_p)]
        self._pdh.PdhAddEnglishCounterW.argtypes = [ctypes.c_void_p, wintypes.LPCWSTR,
                                                    ctypes.c_void_p,
                                                    ctypes.POINTER(ctypes.c_void_p)]
        self._pdh.PdhCollectQueryData.argtypes = [ctypes.c_void_p]
        self._pdh.PdhGetFormattedCounterArrayW.argtypes = [
            ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
            ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        self._pdh.PdhCloseQuery.argtypes = [ctypes.c_void_p]

        query = ctypes.c_void_p()
        if self._pdh.PdhOpenQueryW(None, 0, ctypes.byref(query)) != 0:
            return
        counter = ctypes.c_void_p()
        # The *English* variant so this keeps working on localised Windows,
        # where the display name of the counter is translated.
        if self._pdh.PdhAddEnglishCounterW(query, _GPU_COUNTER_PATH, 0,
                                           ctypes.byref(counter)) != 0:
            self._pdh.PdhCloseQuery(query)
            return
        self._query = query
        self._counter = counter
        # Utilisation is a rate, so the first collection only establishes a
        # baseline -- a single sample can never produce a value.
        self._pdh.PdhCollectQueryData(query)
        self.available = True

    def sample(self, pids):
        """GPU percent attributable to `pids`, or None if unavailable.

        Per engine type, usage is summed across the processes we care about;
        the reported figure is then the MAX across engine types rather than
        their sum. Summing 3D + Copy + VideoDecode routinely exceeds 100% while
        the GPU is nowhere near saturated, because the engines run in parallel.
        Max is what Task Manager shows and is the honest answer to "how busy is
        the GPU on our behalf".
        """
        if not self.available or not pids:
            return None if not self.available else 0.0
        if self._pdh.PdhCollectQueryData(self._query) != 0:
            return None

        size = wintypes.DWORD(0)
        count = wintypes.DWORD(0)
        status = self._pdh.PdhGetFormattedCounterArrayW(
            self._counter, PDH_FMT_DOUBLE, ctypes.byref(size),
            ctypes.byref(count), None)
        if status != PDH_MORE_DATA or size.value == 0:
            return 0.0        # no GPU engine instances at all right now

        buf = ctypes.create_string_buffer(size.value)
        status = self._pdh.PdhGetFormattedCounterArrayW(
            self._counter, PDH_FMT_DOUBLE, ctypes.byref(size),
            ctypes.byref(count), buf)
        if status != 0:
            return None

        items = ctypes.cast(
            buf, ctypes.POINTER(PDH_FMT_COUNTERVALUE_ITEM_W * count.value)).contents
        wanted = {str(p) for p in pids}
        per_engine = {}
        for item in items:
            if not item.szName:
                continue
            m = _GPU_INSTANCE_RE.match(item.szName)
            if not m or m.group(1) not in wanted:
                continue
            value = item.FmtValue.doubleValue
            if value <= 0:
                continue
            engine = m.group(2)
            per_engine[engine] = per_engine.get(engine, 0.0) + value
        if not per_engine:
            return 0.0
        return min(100.0, max(per_engine.values()))

    def close(self):
        if self._query is not None:
            self._pdh.PdhCloseQuery(self._query)
            self._query = None
            self.available = False


class _AppUsageSampler:
    """Aggregate CPU/RAM/Disk/GPU across this app and every VM it launched.

    Deliberately aggregate: the question the panel answers is "what is LocalHost
    costing my machine right now", so the GUI process is counted alongside the
    hypervisors rather than pretending the VMs are the whole cost.
    """

    def __init__(self):
        self._procs = {}            # pid -> psutil.Process, kept alive between
                                    # samples so cpu_percent() has a baseline
        self._last_io_bytes = None
        self._last_io_time = None
        self._disk_peak = 1.0       # bytes/sec, for auto-scaling the disk bar
        self.gpu = _GpuUsageCounter()
        self._cpu_count = (psutil.cpu_count(logical=True) or 1) if psutil else 1
        self._total_ram = psutil.virtual_memory().total if psutil else 0

    def _process_for(self, pid):
        proc = self._procs.get(pid)
        if proc is None:
            try:
                proc = psutil.Process(pid)
                proc.cpu_percent(None)   # prime the baseline; first read is 0.0
            except (psutil.NoSuchProcess, psutil.AccessDenied, OSError):
                return None
            self._procs[pid] = proc
        return proc

    def sample(self, vm_pids):
        """Returns a dict of current usage. `vm_pids` is the running VMs'
        Hypervisor.exe PIDs; this process is always included."""
        result = {"cpu": None, "ram_bytes": 0, "ram_percent": None,
                  "disk_bps": None, "gpu": None, "vm_count": len(vm_pids),
                  "available": psutil is not None}
        pids = list(dict.fromkeys([os.getpid()] + list(vm_pids)))

        # GPU is independent of psutil, so it is sampled even if psutil is gone.
        result["gpu"] = self.gpu.sample(pids)
        if psutil is None:
            return result

        # Drop processes that have exited, so their handles do not accumulate
        # across a session of powering VMs on and off.
        for dead in [p for p in self._procs if p not in pids]:
            self._procs.pop(dead, None)

        cpu_total = 0.0
        ram_total = 0
        io_total = 0
        have_io = False
        for pid in pids:
            proc = self._process_for(pid)
            if proc is None:
                continue
            try:
                cpu_total += proc.cpu_percent(None)
                ram_total += proc.memory_info().rss
                io = proc.io_counters()
                io_total += io.read_bytes + io.write_bytes
                have_io = True
            except (psutil.NoSuchProcess, psutil.AccessDenied, OSError):
                self._procs.pop(pid, None)

        # psutil reports process CPU against ONE core, so a busy 4-thread VM
        # reads 400%. Normalise to whole-machine percent, which is what the
        # meter and Task Manager both mean by "CPU".
        result["cpu"] = min(100.0, cpu_total / self._cpu_count)
        result["ram_bytes"] = ram_total
        if self._total_ram:
            result["ram_percent"] = min(100.0, ram_total * 100.0 / self._total_ram)

        if have_io:
            now = time.monotonic()
            if self._last_io_bytes is not None:
                elapsed = now - self._last_io_time
                # Counters are monotonic per process, but the SET of processes
                # changes as VMs start and stop, so a total can legitimately go
                # down. Clamp rather than report a negative rate.
                delta = max(0, io_total - self._last_io_bytes)
                if elapsed > 0:
                    result["disk_bps"] = delta / elapsed
                    self._disk_peak = max(self._disk_peak, result["disk_bps"])
            self._last_io_bytes = io_total
            self._last_io_time = now
        return result

    @property
    def disk_peak(self):
        return self._disk_peak

    def close(self):
        self.gpu.close()


class _UsageMeter(QWidget):
    """One labelled row of the App Utilisation panel: name, current value, bar."""

    def __init__(self, name, parent=None):
        super().__init__(parent)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)

        top = QHBoxLayout()
        top.setContentsMargins(0, 0, 0, 0)
        self.name_label = QLabel(name)
        self.name_label.setObjectName("UsageName")
        self.value_label = QLabel("--")
        self.value_label.setObjectName("UsageValue")
        self.value_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)
        top.addWidget(self.name_label)
        top.addStretch(1)
        top.addWidget(self.value_label)
        layout.addLayout(top)

        self.bar = QProgressBar()
        self.bar.setObjectName("UsageBar")
        self.bar.setRange(0, 100)
        self.bar.setValue(0)
        self.bar.setTextVisible(False)
        self.bar.setFixedHeight(6)
        layout.addWidget(self.bar)

    def set_value(self, percent, text):
        """percent may be None -- the bar empties and the value shows a dash,
        which is how 'not measurable' is distinguished from a genuine zero."""
        if percent is None:
            self.bar.setValue(0)
            self.value_label.setText(text or "--")
            return
        self.bar.setValue(int(max(0, min(100, round(percent)))))
        self.value_label.setText(text)


def _format_bytes(n):
    if n is None:
        return "--"
    for unit in ("B", "KB", "MB", "GB", "TB"):
        if n < 1024 or unit == "TB":
            return f"{n:.0f} {unit}" if unit in ("B", "KB") else f"{n:.1f} {unit}"
        n /= 1024.0
    return f"{n:.1f} TB"


class VM:
    def __init__(self, name, memory="4 GB", processors=2, hard_disk="60 GB",
                 cdrom="Auto detect", network="NAT", usb="Present",
                 sound="Auto detect", display="Auto detect", tpm="Present",
                 hypervisor="LocalHost Hypervisor", iso_path="", disk_path="",
                 snapshots=None, guest_os="Other / Unknown",
                 firmware="Legacy BIOS", uefi_firmware_path=""):
        self.name = name
        self.memory = memory
        self.processors = processors
        self.hard_disk = hard_disk
        self.cdrom = cdrom
        self.network = network
        self.usb = usb
        self.sound = sound
        self.display = display
        self.tpm = tpm
        self.hypervisor = hypervisor
        self.iso_path = iso_path
        self.disk_path = disk_path  # path to this VM's raw disk image, if any
        # Disk-only snapshots for now: [{"name", "created", "disk_path"}, ...],
        # oldest first. No memory/CPU state yet -- that's hypervisor-side work
        # (see the "no snapshot format" comments around suspend) still to come.
        self.snapshots = snapshots if snapshots is not None else []
        self.guest_os = guest_os  # auto-detected from the installer ISO, or manually chosen
        self.firmware = firmware  # "Legacy BIOS" or "UEFI" -- see power_on_vm
        self.uefi_firmware_path = uefi_firmware_path  # path to an OVMF .fd, only used when firmware == "UEFI"

    def to_dict(self):
        return {
            "memory": self.memory,
            "processors": self.processors,
            "hard_disk": self.hard_disk,
            "cdrom": self.cdrom,
            "network": self.network,
            "usb": self.usb,
            "sound": self.sound,
            "display": self.display,
            "tpm": self.tpm,
            "hypervisor": self.hypervisor,
            "iso_path": self.iso_path,
            "disk_path": self.disk_path,
            "snapshots": self.snapshots,
            "guest_os": self.guest_os,
            "firmware": self.firmware,
            "uefi_firmware_path": self.uefi_firmware_path,
        }

    @classmethod
    def from_dict(cls, name, data):
        return cls(
            name=name,
            memory=data.get("memory", "4 GB"),
            processors=data.get("processors", 2),
            hard_disk=data.get("hard_disk", "60 GB"),
            cdrom=data.get("cdrom", "Auto detect"),
            network=data.get("network", "NAT"),
            usb=data.get("usb", "Present"),
            sound=data.get("sound", "Auto detect"),
            display=data.get("display", "Auto detect"),
            tpm=data.get("tpm", "Present"),
            hypervisor=data.get("hypervisor", "LocalHost Hypervisor"),
            iso_path=data.get("iso_path", ""),
            disk_path=data.get("disk_path", ""),
            snapshots=data.get("snapshots", []),
            guest_os=data.get("guest_os", "Other / Unknown"),
            firmware=data.get("firmware", "Legacy BIOS"),
            uefi_firmware_path=data.get("uefi_firmware_path", ""),
        )


# No dummy VMs — the tree starts empty on first run. Real VMs are loaded
# from library.json (see LocalHostWindow.load_library) on every launch.
DUMMY_VMS = []



# ---------------------------------------------------------------------------
# Create New Virtual Machine Wizard
# ---------------------------------------------------------------------------
class NewVMWizard(QDialog):
    """Simple single-page 'new VM' dialog. Split into QWizard pages later
    if you want the full multi-step VMware feel."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Create a New Virtual Machine")
        self.setMinimumWidth(460)
        self.result_vm = None  # populated on accept

        layout = QVBoxLayout(self)

        form = QFormLayout()
        form.setSpacing(10)

        # VM name
        self.name_edit = QLineEdit()
        self.name_edit.setPlaceholderText("e.g. Ubuntu Dev Box")
        form.addRow("VM Name:", self.name_edit)

        # Memory
        self.memory_spin = QSpinBox()
        self.memory_spin.setRange(512, 131072)
        self.memory_spin.setValue(4096)
        self.memory_spin.setSuffix(" MB")
        self.memory_spin.setSingleStep(512)
        form.addRow("Memory:", self.memory_spin)

        # Processors
        self.cpu_spin = QSpinBox()
        self.cpu_spin.setRange(1, 32)
        self.cpu_spin.setValue(2)
        form.addRow("Processors:", self.cpu_spin)

        # Disk size -- capped at 128 GB since the hypervisor's ATA emulation
        # is LBA28 (2^28 sectors * 512 bytes = exactly 128 GB addressable).
        self.disk_spin = QSpinBox()
        self.disk_spin.setRange(1, 128)
        self.disk_spin.setValue(60)
        self.disk_spin.setSuffix(" GB")
        form.addRow("Hard Disk:", self.disk_spin)

        # ISO path -- required, since there's no other way to install a
        # guest OS onto a freshly created blank disk. Validated as a real
        # ISO 9660 image (not just by extension) and used to auto-detect
        # the guest OS as soon as one is selected -- see
        # _validate_and_detect_iso.
        iso_row = QHBoxLayout()
        self.iso_edit = QLineEdit()
        self.iso_edit.setPlaceholderText("Path to installer ISO (required)")
        self.iso_edit.editingFinished.connect(self.on_iso_path_edited)
        iso_browse_btn = QPushButton("Browse...")
        iso_browse_btn.clicked.connect(self.on_browse_iso)
        iso_row.addWidget(self.iso_edit)
        iso_row.addWidget(iso_browse_btn)

        # Stacked into the same form row as the ISO field, right under the
        # path/Browse line, rather than as its own separate row.
        iso_container = QVBoxLayout()
        iso_container.addLayout(iso_row)
        self.iso_detect_label = QLabel("")
        self.iso_detect_label.setStyleSheet("color: #888;")
        iso_container.addWidget(self.iso_detect_label)
        form.addRow("Installer ISO:", iso_container)

        # Guest OS -- auto-filled after a valid ISO is selected, but always
        # left editable: detection is best-effort, and an ISO we don't
        # recognize just falls back to "Other / Unknown" for the user to
        # correct themselves rather than blocking VM creation.
        self.guest_os_combo = QComboBox()
        self.guest_os_combo.addItem("Other / Unknown")
        for os_label, _markers, _mem, _disk in _OS_SIGNATURES:
            self.guest_os_combo.addItem(os_label)
        form.addRow("Guest OS:", self.guest_os_combo)

        # Firmware -- UEFI needs an OVMF.fd the user supplies (not bundled).
        # The path row only appears once UEFI is selected.
        self.firmware_combo = QComboBox()
        self.firmware_combo.addItems(["Legacy BIOS (SeaBIOS)", "UEFI (OVMF)"])
        self.firmware_combo.currentTextChanged.connect(self.on_firmware_changed)
        form.addRow("Firmware:", self.firmware_combo)

        uefi_row = QHBoxLayout()
        self.uefi_path_edit = QLineEdit()
        self.uefi_path_edit.setPlaceholderText("Path to OVMF.fd (required for UEFI)")
        uefi_browse_btn = QPushButton("Browse...")
        uefi_browse_btn.clicked.connect(self.on_browse_uefi_firmware)
        uefi_row.addWidget(self.uefi_path_edit)
        uefi_row.addWidget(uefi_browse_btn)

        uefi_container = QVBoxLayout()
        uefi_container.setContentsMargins(0, 0, 0, 0)
        uefi_container.addLayout(uefi_row)
        uefi_hint = QLabel(
            "Note: there's no installer/CD-ROM support yet -- the hard disk "
            "image must already be bootable (e.g. a GPT/FAT ESP with "
            "\\EFI\\BOOT\\BOOTX64.EFI)."
        )
        uefi_hint.setWordWrap(True)
        uefi_hint.setStyleSheet("color: #888;")
        uefi_container.addWidget(uefi_hint)

        # QFormLayout rows aren't directly hide-able -- wrap the row's
        # contents in a real QWidget so on_firmware_changed can toggle both
        # the label and the field as a unit.
        self.uefi_firmware_row_widget = QWidget()
        self.uefi_firmware_row_widget.setLayout(uefi_container)
        self.uefi_firmware_row_label = QLabel("UEFI Firmware:")
        form.addRow(self.uefi_firmware_row_label, self.uefi_firmware_row_widget)
        self.on_firmware_changed(self.firmware_combo.currentText())

        layout.addLayout(form)

        # OK / Cancel
        buttons = QDialogButtonBox(
            QDialogButtonBox.Ok | QDialogButtonBox.Cancel
        )
        buttons.accepted.connect(self.on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def on_browse_iso(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Select Installer ISO", "", "ISO Images (*.iso);;All Files (*)"
        )
        if path:
            self.iso_edit.setText(path)
            self._validate_and_detect_iso(path)

    def on_firmware_changed(self, text):
        is_uefi = text.startswith("UEFI")
        self.uefi_firmware_row_label.setVisible(is_uefi)
        self.uefi_firmware_row_widget.setVisible(is_uefi)

    def on_browse_uefi_firmware(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Select UEFI Firmware", "", "UEFI Firmware (*.fd);;All Files (*)"
        )
        if path:
            self.uefi_path_edit.setText(path)

    def on_iso_path_edited(self):
        # Fires on Enter / focus-loss, so a manually-typed path gets the
        # same automatic validation + detection Browse gets, without
        # re-running on every keystroke.
        path = self.iso_edit.text().strip()
        if path:
            self._validate_and_detect_iso(path)

    def _validate_and_detect_iso(self, path):
        """Confirms the selected file is really an ISO 9660 image -- not
        just judging by the .iso extension -- and rejects it immediately
        with an error dialog if not, rather than waiting for OK. For a
        valid image, tries to auto-detect the guest OS and pre-fills the
        guest OS field plus recommended memory/disk size; an unrecognized
        ISO just resets to "Other / Unknown" and leaves the user to pick."""
        if not Path(path).is_file():
            return  # let on_accept's "ISO not found" message handle this
        if not _is_valid_iso9660(path):
            self.iso_edit.clear()
            self.iso_detect_label.setText("")
            self.guest_os_combo.setCurrentIndex(0)
            QMessageBox.critical(
                self, "Invalid ISO",
                f"'{Path(path).name}' doesn't look like a valid ISO image "
                "(no ISO 9660 signature found). Choose a different file."
            )
            return

        detected = detect_guest_os(path)
        if detected:
            os_label, memory_mb, disk_gb = detected
            index = self.guest_os_combo.findText(os_label)
            if index >= 0:
                self.guest_os_combo.setCurrentIndex(index)
            self.memory_spin.setValue(memory_mb)
            self.disk_spin.setValue(min(disk_gb, self.disk_spin.maximum()))
            self.iso_detect_label.setText(f"Detected: {os_label}")
        else:
            self.guest_os_combo.setCurrentIndex(0)
            self.iso_detect_label.setText(
                "Couldn't identify the guest OS -- pick one below, or leave it as Other/Unknown."
            )

    def on_accept(self):
        name = self.name_edit.text().strip()
        if not name:
            self.name_edit.setStyleSheet("border: 1px solid #e05555;")
            self.name_edit.setPlaceholderText("Name is required!")
            return

        iso_path = self.iso_edit.text().strip()
        if not iso_path:
            self.iso_edit.setStyleSheet("border: 1px solid #e05555;")
            self.iso_edit.setPlaceholderText("An installer ISO is required!")
            return
        if not Path(iso_path).is_file():
            self.iso_edit.setStyleSheet("border: 1px solid #e05555;")
            QMessageBox.warning(
                self, "ISO not found",
                f"Can't find an ISO file at:\n{iso_path}"
            )
            return
        if not _is_valid_iso9660(iso_path):
            # Defense in depth -- Browse/editingFinished already catch this
            # as soon as the ISO is chosen, but re-check here too in case
            # the file changed since, or somehow slipped past that.
            self.iso_edit.setStyleSheet("border: 1px solid #e05555;")
            QMessageBox.critical(
                self, "Invalid ISO",
                f"'{Path(iso_path).name}' doesn't look like a valid ISO image "
                "(no ISO 9660 signature found). Choose a different file."
            )
            return

        firmware = "UEFI" if self.firmware_combo.currentText().startswith("UEFI") else "Legacy BIOS"
        uefi_firmware_path = self.uefi_path_edit.text().strip()
        if firmware == "UEFI":
            if not uefi_firmware_path:
                self.uefi_path_edit.setStyleSheet("border: 1px solid #e05555;")
                self.uefi_path_edit.setPlaceholderText("A UEFI firmware (.fd) file is required!")
                return
            if not Path(uefi_firmware_path).is_file():
                self.uefi_path_edit.setStyleSheet("border: 1px solid #e05555;")
                QMessageBox.warning(
                    self, "UEFI firmware not found",
                    f"Can't find a file at:\n{uefi_firmware_path}"
                )
                return

        self.result_vm = VM(
            name=name,
            memory=f"{self.memory_spin.value() / 1024:.1f} GB",
            processors=self.cpu_spin.value(),
            hard_disk=f"{self.disk_spin.value()} GB",
            cdrom=iso_path,
            hypervisor="LocalHost Hypervisor",
            iso_path=iso_path,
            guest_os=self.guest_os_combo.currentText(),
            firmware=firmware,
            uefi_firmware_path=uefi_firmware_path,
        )

        self.accept()


# ---------------------------------------------------------------------------
# Edit Virtual Machine Settings
# ---------------------------------------------------------------------------
class VMSettingsDialog(QDialog):
    """Edit an existing VM's specs in place. Memory/processors/network/USB/
    sound/display/TPM are cosmetic for now -- same as in NewVMWizard, the
    hypervisor doesn't yet take these as launch parameters -- but they're
    still real, persisted settings. The hard disk can genuinely grow (sparse
    -extends the backing .img with existing data untouched); shrinking isn't
    offered since that would require actually relocating guest data.
    Renaming isn't offered here to avoid juggling live process/tracking-dict
    keys out from under a VM that might be running."""

    def __init__(self, vm, is_running=False, parent=None):
        super().__init__(parent)
        self.vm = vm
        self.is_running = is_running
        self.setWindowTitle(f"{vm.name} - Settings")
        self.setMinimumWidth(460)

        layout = QVBoxLayout(self)
        form = QFormLayout()
        form.setSpacing(10)

        form.addRow("VM Name:", QLabel(vm.name))

        self.memory_spin = QSpinBox()
        self.memory_spin.setRange(512, 131072)
        self.memory_spin.setValue(_parse_memory_to_mb(vm.memory))
        self.memory_spin.setSuffix(" MB")
        self.memory_spin.setSingleStep(512)
        form.addRow("Memory:", self.memory_spin)

        self.cpu_spin = QSpinBox()
        self.cpu_spin.setRange(1, 32)
        self.cpu_spin.setValue(vm.processors)
        form.addRow("Processors:", self.cpu_spin)

        self._current_disk_gb = _parse_disk_gb(vm.hard_disk)
        self.disk_spin = QSpinBox()
        self.disk_spin.setRange(1, 128)
        self.disk_spin.setValue(self._current_disk_gb)
        self.disk_spin.setSuffix(" GB")
        self.disk_spin.setEnabled(not is_running)
        disk_row = QHBoxLayout()
        disk_row.addWidget(self.disk_spin)
        disk_note = QLabel(
            "(power off to resize)" if is_running
            else "(shrinking permanently deletes data past the new size)"
        )
        disk_note.setStyleSheet("color: #888;")
        disk_row.addWidget(disk_note)
        form.addRow("Hard Disk:", disk_row)

        iso_row = QHBoxLayout()
        self.iso_edit = QLineEdit(vm.iso_path)
        self.iso_edit.setPlaceholderText("Path to installer ISO (optional)")
        iso_browse_btn = QPushButton("Browse...")
        iso_browse_btn.clicked.connect(self.on_browse_iso)
        iso_row.addWidget(self.iso_edit)
        iso_row.addWidget(iso_browse_btn)
        form.addRow("CD/DVD (ISO):", iso_row)

        # Same detection the wizard runs at creation time -- lets a "wrong
        # ISO" or "Other/Unknown" guess get corrected without recreating
        # the VM, and manual picks are always allowed too.
        self.guest_os_combo = QComboBox()
        self.guest_os_combo.addItem("Other / Unknown")
        for os_label, _markers, _mem, _disk in _OS_SIGNATURES:
            self.guest_os_combo.addItem(os_label)
        self._set_combo(self.guest_os_combo, vm.guest_os, "Other / Unknown")
        form.addRow("Guest OS:", self.guest_os_combo)

        # Firmware -- UEFI needs an OVMF.fd the user supplies (not bundled).
        # The path row only appears once UEFI is selected. Locked while
        # running, same rationale as the hard disk size: the hypervisor
        # process already has its firmware path fixed for this run.
        self.firmware_combo = QComboBox()
        self.firmware_combo.addItems(["Legacy BIOS (SeaBIOS)", "UEFI (OVMF)"])
        self._set_combo(self.firmware_combo, "UEFI (OVMF)" if vm.firmware == "UEFI" else "Legacy BIOS (SeaBIOS)",
                         "Legacy BIOS (SeaBIOS)")
        self.firmware_combo.setEnabled(not is_running)
        self.firmware_combo.currentTextChanged.connect(self.on_firmware_changed)
        form.addRow("Firmware:", self.firmware_combo)

        uefi_row = QHBoxLayout()
        self.uefi_path_edit = QLineEdit(vm.uefi_firmware_path)
        self.uefi_path_edit.setPlaceholderText("Path to OVMF.fd (required for UEFI)")
        self.uefi_path_edit.setEnabled(not is_running)
        uefi_browse_btn = QPushButton("Browse...")
        uefi_browse_btn.setEnabled(not is_running)
        uefi_browse_btn.clicked.connect(self.on_browse_uefi_firmware)
        uefi_row.addWidget(self.uefi_path_edit)
        uefi_row.addWidget(uefi_browse_btn)

        uefi_container = QVBoxLayout()
        uefi_container.setContentsMargins(0, 0, 0, 0)
        uefi_container.addLayout(uefi_row)
        uefi_hint = QLabel(
            "Note: there's no installer/CD-ROM support yet -- the hard disk "
            "image must already be bootable (e.g. a GPT/FAT ESP with "
            "\\EFI\\BOOT\\BOOTX64.EFI)."
        )
        uefi_hint.setWordWrap(True)
        uefi_hint.setStyleSheet("color: #888;")
        uefi_container.addWidget(uefi_hint)

        self.uefi_firmware_row_widget = QWidget()
        self.uefi_firmware_row_widget.setLayout(uefi_container)
        self.uefi_firmware_row_label = QLabel("UEFI Firmware:")
        form.addRow(self.uefi_firmware_row_label, self.uefi_firmware_row_widget)
        self.on_firmware_changed(self.firmware_combo.currentText())

        self.network_combo = QComboBox()
        self.network_combo.addItems(["NAT", "Bridged", "Host-only"])
        self._set_combo(self.network_combo, vm.network, "NAT")
        form.addRow("Network Adapter:", self.network_combo)

        self.usb_combo = QComboBox()
        self.usb_combo.addItems(["Present", "Absent"])
        self._set_combo(self.usb_combo, vm.usb, "Present")
        form.addRow("USB Controller:", self.usb_combo)

        self.sound_combo = QComboBox()
        self.sound_combo.addItems(["Auto detect", "Present", "Absent"])
        self._set_combo(self.sound_combo, vm.sound, "Auto detect")
        form.addRow("Sound Card:", self.sound_combo)

        self.display_combo = QComboBox()
        self.display_combo.addItems(["Auto detect", "Present", "Absent"])
        self._set_combo(self.display_combo, vm.display, "Auto detect")
        form.addRow("Display:", self.display_combo)

        self.tpm_combo = QComboBox()
        self.tpm_combo.addItems(["Present", "Absent"])
        self._set_combo(self.tpm_combo, vm.tpm, "Present")
        form.addRow("Trusted Platform Module:", self.tpm_combo)

        layout.addLayout(form)

        buttons = QDialogButtonBox(
            QDialogButtonBox.Ok | QDialogButtonBox.Cancel
        )
        buttons.accepted.connect(self.on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    @staticmethod
    def _set_combo(combo, value, fallback):
        combo.setCurrentText(value if value in
                              [combo.itemText(i) for i in range(combo.count())] else fallback)

    def on_browse_iso(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Select Installer ISO", "", "ISO Images (*.iso);;All Files (*)"
        )
        if path:
            self.iso_edit.setText(path)

    def on_firmware_changed(self, text):
        is_uefi = text.startswith("UEFI")
        self.uefi_firmware_row_label.setVisible(is_uefi)
        self.uefi_firmware_row_widget.setVisible(is_uefi)

    def on_browse_uefi_firmware(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Select UEFI Firmware", "", "UEFI Firmware (*.fd);;All Files (*)"
        )
        if path:
            self.uefi_path_edit.setText(path)

    def on_accept(self):
        firmware = "UEFI" if self.firmware_combo.currentText().startswith("UEFI") else "Legacy BIOS"
        uefi_firmware_path = self.uefi_path_edit.text().strip()
        if firmware == "UEFI":
            if not uefi_firmware_path:
                self.uefi_path_edit.setStyleSheet("border: 1px solid #e05555;")
                self.uefi_path_edit.setPlaceholderText("A UEFI firmware (.fd) file is required!")
                return
            if not Path(uefi_firmware_path).is_file():
                self.uefi_path_edit.setStyleSheet("border: 1px solid #e05555;")
                QMessageBox.warning(
                    self, "UEFI firmware not found",
                    f"Can't find a file at:\n{uefi_firmware_path}"
                )
                return

        new_disk_gb = self.disk_spin.value()
        if new_disk_gb != self._current_disk_gb:
            if not self.vm.disk_path or not Path(self.vm.disk_path).exists():
                QMessageBox.warning(
                    self, "Can't resize disk",
                    "This VM has no backing disk image to resize."
                )
                return

            if new_disk_gb < self._current_disk_gb:
                choice = QMessageBox.warning(
                    self, "Shrink hard disk?",
                    f"Shrinking from {self._current_disk_gb} GB to {new_disk_gb} GB "
                    "permanently deletes anything stored past the new size. If the "
                    "guest's filesystem extends into that space, this can corrupt it.\n\n"
                    "This can't be undone. Continue?",
                    QMessageBox.Yes | QMessageBox.No, QMessageBox.No
                )
                if choice != QMessageBox.Yes:
                    return

            if not _resize_sparse_disk_image(self.vm.disk_path, new_disk_gb * 1024 * 1024 * 1024):
                QMessageBox.warning(
                    self, "Can't resize disk",
                    "Couldn't resize the hard disk. Make sure the VM is powered off "
                    "and try again."
                )
                return

        self.vm.memory = f"{self.memory_spin.value() / 1024:.1f} GB"
        self.vm.processors = self.cpu_spin.value()
        self.vm.hard_disk = f"{new_disk_gb} GB"
        self.vm.iso_path = self.iso_edit.text().strip()
        self.vm.cdrom = self.vm.iso_path or "Auto detect"
        self.vm.network = self.network_combo.currentText()
        self.vm.usb = self.usb_combo.currentText()
        self.vm.sound = self.sound_combo.currentText()
        self.vm.display = self.display_combo.currentText()
        self.vm.tpm = self.tpm_combo.currentText()
        self.vm.guest_os = self.guest_os_combo.currentText()
        self.vm.firmware = firmware
        self.vm.uefi_firmware_path = uefi_firmware_path

        self.accept()


# ---------------------------------------------------------------------------
# Snapshot Manager
# ---------------------------------------------------------------------------
class SnapshotManagerDialog(QDialog):
    """Lists a VM's disk snapshots with Take/Revert/Delete. The actual file
    work lives on the main window (take_snapshot/revert_to_snapshot/
    delete_snapshot) -- this is just the list + confirmations."""

    def __init__(self, window, vm, parent=None):
        super().__init__(parent)
        self.window = window
        self.vm = vm
        self.setWindowTitle(f"{vm.name} - Snapshot Manager")
        self.setMinimumSize(420, 320)

        layout = QVBoxLayout(self)
        layout.addWidget(QLabel(f"Snapshots of {vm.name}:"))

        self.list_widget = QListWidget()
        layout.addWidget(self.list_widget)

        btn_row = QHBoxLayout()
        self.take_btn = QPushButton("Take Snapshot...")
        self.take_btn.clicked.connect(self.on_take)
        self.revert_btn = QPushButton("Revert to Selected")
        self.revert_btn.clicked.connect(self.on_revert)
        self.delete_btn = QPushButton("Delete Selected")
        self.delete_btn.clicked.connect(self.on_delete)
        btn_row.addWidget(self.take_btn)
        btn_row.addWidget(self.revert_btn)
        btn_row.addWidget(self.delete_btn)
        layout.addLayout(btn_row)

        buttons = QDialogButtonBox(QDialogButtonBox.Close)
        buttons.rejected.connect(self.accept)
        layout.addWidget(buttons)

        self.list_widget.currentItemChanged.connect(self._update_button_states)
        self._refresh_list()

    def _refresh_list(self):
        self.list_widget.clear()
        for snap in self.vm.snapshots:
            item = QListWidgetItem(f"{snap['name']}    ({snap.get('created', '')})")
            item.setData(Qt.UserRole, snap)
            self.list_widget.addItem(item)
        self._update_button_states()

    def _update_button_states(self, *_args):
        running = self.vm.name in self.window.processes
        has_selection = self.list_widget.currentItem() is not None
        self.take_btn.setEnabled(not running)
        self.revert_btn.setEnabled(not running and has_selection)
        self.delete_btn.setEnabled(has_selection)

    def _selected_snapshot(self):
        item = self.list_widget.currentItem()
        return item.data(Qt.UserRole) if item else None

    def on_take(self):
        if self.window.take_snapshot(self.vm):
            self._refresh_list()

    def on_revert(self):
        snap = self._selected_snapshot()
        if not snap:
            return
        choice = QMessageBox.warning(
            self, "Revert to Snapshot?",
            f"Revert '{self.vm.name}' to snapshot '{snap['name']}'?\n\n"
            "Any changes made to the disk since this snapshot was taken "
            "will be permanently lost. This can't be undone.",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No
        )
        if choice != QMessageBox.Yes:
            return
        if self.window.revert_to_snapshot(self.vm, snap):
            self._refresh_list()

    def on_delete(self):
        snap = self._selected_snapshot()
        if not snap:
            return
        choice = QMessageBox.warning(
            self, "Delete Snapshot?",
            f"Delete snapshot '{snap['name']}'? This can't be undone.",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No
        )
        if choice != QMessageBox.Yes:
            return
        if self.window.delete_snapshot(self.vm, snap):
            self._refresh_list()


# ---------------------------------------------------------------------------
# Connect to VPS Dialog
# ---------------------------------------------------------------------------
class ConnectToVPSDialog(QDialog):
    """Prompts for the VPS ID and the VPS account password before
    establishing a connection. Both are required."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("Connect to VPS")
        self.setMinimumWidth(420)
        self.result_data = None  # populated on accept

        layout = QVBoxLayout(self)

        form = QFormLayout()
        form.setSpacing(10)

        self.vps_id_edit = QLineEdit()
        self.vps_id_edit.setPlaceholderText("e.g. vps-8f3a21c7")
        form.addRow("VPS ID:", self.vps_id_edit)

        self.password_edit = QLineEdit()
        self.password_edit.setEchoMode(QLineEdit.Password)
        self.password_edit.setPlaceholderText("VPS account password")
        form.addRow("VPS Account Password:", self.password_edit)

        layout.addLayout(form)

        buttons = QDialogButtonBox(
            QDialogButtonBox.Ok | QDialogButtonBox.Cancel
        )
        buttons.button(QDialogButtonBox.Ok).setText("Connect")
        buttons.accepted.connect(self.on_accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)

    def _flag_field(self, field, message):
        """Mark a required field as missing. Also used with clear=True to undo
        the marking, since the previous version left the red border and the
        'is required!' placeholder stuck on screen even after the user fixed
        the field and retried."""
        field.setStyleSheet("border: 1px solid #e05555;")
        field.setPlaceholderText(message)

    def _clear_flag(self, field, placeholder):
        field.setStyleSheet("")
        field.setPlaceholderText(placeholder)

    def on_accept(self):
        vps_id = self.vps_id_edit.text().strip()
        password = self.password_edit.text()

        # Reset any previous error styling first, so fields that have since
        # been filled in stop looking like errors.
        self._clear_flag(self.vps_id_edit, "e.g. vps-8f3a21c7")
        self._clear_flag(self.password_edit, "VPS account password")

        # Validate both fields before returning, so the user sees every
        # missing field at once rather than one per attempt.
        missing = []
        if not vps_id:
            self._flag_field(self.vps_id_edit, "VPS ID is required!")
            missing.append(self.vps_id_edit)
        if not password:
            self._flag_field(self.password_edit, "VPS account password is required!")
            missing.append(self.password_edit)

        if missing:
            missing[0].setFocus()
            return

        self.result_data = {
            "vps_id": vps_id,
            "password": password,
        }
        self.accept()


# ---------------------------------------------------------------------------
# About Dialog
# ---------------------------------------------------------------------------
class AboutDialog(QDialog):
    """Version/release info and a bit of the runtime environment -- see
    APP_VERSION/APP_RELEASE_TAG/APP_RELEASE_DATE near the top of this file."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("About LocalHost Workstation Pro")
        self.setMinimumWidth(440)

        layout = QVBoxLayout(self)
        layout.setSpacing(8)

        icon_path = Path(__file__).resolve().parent / "LocalHost software logo.ico"
        if icon_path.exists():
            icon_label = QLabel()
            icon_label.setPixmap(QIcon(str(icon_path)).pixmap(64, 64))
            icon_label.setAlignment(Qt.AlignCenter)
            layout.addWidget(icon_label)

        title_label = QLabel("LocalHost Workstation Pro")
        title_label.setWordWrap(True)
        title_label.setAlignment(Qt.AlignCenter)
        title_label.setStyleSheet("font-size: 18px; font-weight: 600;")
        layout.addWidget(title_label)

        version_label = QLabel(f"Version {APP_VERSION} ({APP_RELEASE_TAG})")
        version_label.setAlignment(Qt.AlignCenter)
        layout.addWidget(version_label)

        date_label = QLabel(f"Released {APP_RELEASE_DATE}")
        date_label.setAlignment(Qt.AlignCenter)
        date_label.setStyleSheet("color: #888;")
        layout.addWidget(date_label)

        desc_label = QLabel(
            "A VM manager built on the Windows Hypervisor Platform, with "
            "its own hypervisor engine underneath (SeaBIOS boot, ATA disk "
            "emulation) rather than wrapping an existing one."
        )
        desc_label.setWordWrap(True)
        desc_label.setAlignment(Qt.AlignCenter)
        layout.addWidget(desc_label)

        info_frame = QFrame()
        info_frame.setObjectName("PreviewFrame")
        info_frame_layout = QVBoxLayout(info_frame)
        info_frame_layout.setContentsMargins(10, 8, 10, 8)
        for label, value in (
            ("Python", platform.python_version()),
            ("PySide6 (Qt)", PySide6.__version__),
            ("Platform", platform.platform()),
        ):
            row = QHBoxLayout()
            name_label = QLabel(label)
            name_label.setObjectName("DeviceLabel")
            value_label = QLabel(value)
            value_label.setObjectName("DeviceValue")
            row.addWidget(name_label)
            row.addStretch(1)
            row.addWidget(value_label)
            info_frame_layout.addLayout(row)
        layout.addWidget(info_frame)

        buttons = QDialogButtonBox(QDialogButtonBox.Ok)
        buttons.accepted.connect(self.accept)
        layout.addWidget(buttons)


# ---------------------------------------------------------------------------
# Main Window
# ---------------------------------------------------------------------------
class LocalHostWindow(QMainWindow):
    # Cross-thread notifications from background Win32 workers. Qt signals
    # are the correct way to marshal back to the main thread from a plain
    # threading.Thread with no event loop of its own -- unlike
    # QTimer.singleShot, which silently never fires when scheduled from a
    # thread that isn't running a Qt event loop.
    _embed_ready = Signal(str, int)   # vm_name, hwnd
    _request_resize = Signal(str)     # vm_name

    def __init__(self):
        super().__init__()
        self.setWindowTitle("LocalHost Workstation Pro")
        icon_path = self.app_dir() / "LocalHost software logo.ico"
        if icon_path.exists():
            self.setWindowIcon(QIcon(str(icon_path)))
        self.resize(1400, 820)

        self.vms = {}
        self.current_vm = None
        self._loading = False  # guard so load_library() doesn't trigger saves
        self.processes = {}  # vm name -> running Hypervisor.exe PID (detached, not QProcess-owned)
        self.embedded_hwnds = {}  # vm name -> hypervisor window HWND, reparented into preview_frame
        self.fullscreen_vm = None  # vm name currently popped out to a fullscreen window, if any
        self._f11_down = False  # debounces key-repeat in the keyboard hook (see below)
        self.suspended_vms = set()  # vm names currently thread-frozen (best-effort suspend)
        self._pending_restart = set()  # vm names to power back on once their old process exits

        self._build_menu_bar()
        self._build_toolbar()
        self._build_central_widget()
        self._build_status_bar()

        self._apply_dark_theme()

        self.load_library()
        self._reconnect_running_vms()

        # select Home by default
        self.vm_tree.setCurrentItem(self.home_item)

        self._embed_ready.connect(self._on_embedded)
        self._request_resize.connect(self._resize_embedded_window)

        # Detached processes (see power_on_vm) have no finished signal --
        # notice they've exited by polling instead.
        self._process_poll_timer = QTimer(self)
        self._process_poll_timer.timeout.connect(self._poll_running_processes)
        self._process_poll_timer.start(1000)

        # Shift+F11 fullscreen toggle: a global low-level keyboard hook, NOT
        # a QShortcut. See the WH_KEYBOARD_LL comment above -- a QShortcut
        # would silently never fire once focus is on the embedded guest
        # window (a different process). The hook is scoped to our own
        # windows via _foreground_belongs_to_us(), so it doesn't steal the
        # combo from other applications.
        self._keyboard_hook_proc = LowLevelKeyboardProc(self._keyboard_hook_callback)
        self._keyboard_hook = _user32.SetWindowsHookExW(WH_KEYBOARD_LL, self._keyboard_hook_proc, None, 0)

    # ------------------------------------------------------------------
    # Menu Bar
    # ------------------------------------------------------------------
    def _build_menu_bar(self):
        menubar = self.menuBar()
        for name in ("File", "Edit", "View", "VM"):
            menubar.addMenu(name)

        # "Tabs" replaced with "Manage Snapshots"
        snapshots_menu = menubar.addMenu("Manage Snapshots")
        take_snapshot = QAction("Take Snapshot...", self)
        take_snapshot.triggered.connect(self.on_take_snapshot)
        revert_snapshot = QAction("Revert to Snapshot", self)
        revert_snapshot.triggered.connect(self.on_revert_snapshot)
        snapshot_manager = QAction("Snapshot Manager...", self)
        snapshot_manager.triggered.connect(self.on_manage_snapshots)
        snapshots_menu.addAction(take_snapshot)
        snapshots_menu.addAction(revert_snapshot)
        snapshots_menu.addSeparator()
        snapshots_menu.addAction(snapshot_manager)

        help_menu = menubar.addMenu("Help")
        report_bug_action = QAction("Report a Bug...", self)
        report_bug_action.triggered.connect(self.on_report_bug)
        help_menu.addAction(report_bug_action)
        help_menu.addSeparator()
        about_action = QAction("About LocalHost Workstation Pro...", self)
        about_action.triggered.connect(self.on_about)
        help_menu.addAction(about_action)

    # ------------------------------------------------------------------
    # Toolbar
    # ------------------------------------------------------------------
    def _build_toolbar(self):
        toolbar = QToolBar("Main Toolbar")
        toolbar.setMovable(False)
        toolbar.setIconSize(QSize(20, 20))
        self.addToolBar(toolbar)

        self.power_toolbar_action = QAction("Power On", self)
        self.power_toolbar_action.triggered.connect(self.on_toolbar_power_toggle)
        toolbar.addAction(self.power_toolbar_action)
        toolbar.addSeparator()

        self.suspend_toolbar_action = QAction("Suspend", self)
        self.suspend_toolbar_action.triggered.connect(self.on_suspend_vm)
        toolbar.addAction(self.suspend_toolbar_action)
        toolbar.addSeparator()

        self.restart_toolbar_action = QAction("Restart", self)
        self.restart_toolbar_action.triggered.connect(self.on_restart_vm)
        toolbar.addAction(self.restart_toolbar_action)
        toolbar.addSeparator()

        self.snapshot_toolbar_action = QAction("Snapshot", self)
        self.snapshot_toolbar_action.triggered.connect(self.on_take_snapshot)
        toolbar.addAction(self.snapshot_toolbar_action)
        toolbar.addSeparator()

    # ------------------------------------------------------------------
    # Central widget: Library (left) + VM detail panel (right)
    # ------------------------------------------------------------------
    def _build_central_widget(self):
        splitter = QSplitter(Qt.Horizontal)

        # ---- Left: Library panel ----
        library_panel = QWidget()
        library_layout = QVBoxLayout(library_panel)
        library_layout.setContentsMargins(6, 6, 6, 6)

        library_header = QLabel("Library")
        library_header.setObjectName("PanelHeader")
        library_layout.addWidget(library_header)

        self.library_search_edit = QLineEdit()
        self.library_search_edit.setObjectName("LibrarySearch")
        self.library_search_edit.setPlaceholderText("Search")
        self.library_search_edit.setClearButtonEnabled(True)
        self.library_search_edit.textChanged.connect(self.on_library_search_changed)
        library_layout.addWidget(self.library_search_edit)

        self.vm_tree = QTreeWidget()
        self.vm_tree.setHeaderHidden(True)

        # Multi-select with click-drag rubber-band (like selecting desktop
        # icons) + let the user drag items straight onto a folder to move them.
        self.vm_tree.setSelectionMode(QAbstractItemView.ExtendedSelection)
        self.vm_tree.setDragDropMode(QAbstractItemView.InternalMove)
        self.vm_tree.setDefaultDropAction(Qt.MoveAction)

        self.home_item = QTreeWidgetItem(["Home"])
        self.home_item.setData(0, Qt.UserRole, "home")
        self.vm_tree.addTopLevelItem(self.home_item)

        self.my_computer_item = QTreeWidgetItem(["My Computer"])
        self.my_computer_item.setData(0, Qt.UserRole, "root")
        self.vm_tree.addTopLevelItem(self.my_computer_item)
        self.my_computer_item.setExpanded(True)
        self.vm_tree.currentItemChanged.connect(self.on_tree_selection_changed)
        self.vm_tree.setContextMenuPolicy(Qt.CustomContextMenu)
        self.vm_tree.customContextMenuRequested.connect(self.on_tree_context_menu)
        # Persist whenever the user drags a VM onto a folder to move it
        self.vm_tree.model().rowsMoved.connect(lambda *args: self.save_library())

        tree_frame = QFrame()
        tree_frame.setObjectName("LibraryTreeFrame")
        tree_frame_layout = QVBoxLayout(tree_frame)
        tree_frame_layout.setContentsMargins(0, 0, 0, 0)
        tree_frame_layout.addWidget(self.vm_tree)

        # The tree and the usage panel share the sidebar through a splitter, so
        # the divider can be dragged instead of the panel permanently costing
        # the library a fixed slice of height.
        usage_panel = self._build_usage_panel()
        left_split = QSplitter(Qt.Vertical)
        left_split.addWidget(tree_frame)
        left_split.addWidget(usage_panel)
        left_split.setStretchFactor(0, 1)   # the tree absorbs new space
        left_split.setStretchFactor(1, 0)
        # NEITHER pane may collapse. Marking only the tree non-collapsible left
        # the panel collapsible, and on a display where DPI scaling makes the
        # logical sidebar short, the splitter duly collapsed it to zero height --
        # the panel was built, laid out and "visible", just squeezed out of
        # existence. The minimum height is what actually guarantees it a slice.
        left_split.setCollapsible(0, False)
        left_split.setCollapsible(1, False)
        tree_frame.setMinimumHeight(120)
        library_layout.addWidget(left_split)
        # Sized after the widget is parented, so the splitter honours it against
        # real geometry rather than the pre-layout default.
        left_split.setSizes([420, usage_panel.sizeHint().height()])

        library_panel.setMinimumWidth(220)
        library_panel.setMaximumWidth(320)

        # ---- Right: VM detail panel ----
        vm_page = QWidget()
        vm_page_layout = QHBoxLayout(vm_page)
        vm_page_layout.setContentsMargins(0, 0, 0, 0)
        vm_page_layout.setSpacing(0)

        detail_panel = QWidget()
        detail_layout = QVBoxLayout(detail_panel)
        detail_layout.setContentsMargins(16, 16, 16, 16)
        detail_layout.setSpacing(10)

        self.vm_title = QLabel("Select a virtual machine")
        self.vm_title.setObjectName("VMTitle")
        detail_layout.addWidget(self.vm_title)

        # Power on / Edit settings links
        links_layout = QVBoxLayout()
        links_layout.setSpacing(4)
        self.power_on_link = QPushButton("\u25B6  Power on this virtual machine")
        self.power_on_link.setObjectName("LinkButton")
        self.power_on_link.setCursor(Qt.PointingHandCursor)
        self.power_on_link.clicked.connect(self.on_power_on_link_clicked)

        self.edit_settings_link = QPushButton("\u270E  Edit virtual machine settings")
        self.edit_settings_link.setObjectName("LinkButton")
        self.edit_settings_link.setCursor(Qt.PointingHandCursor)
        self.edit_settings_link.clicked.connect(self.on_edit_settings)

        links_layout.addWidget(self.power_on_link)
        links_layout.addWidget(self.edit_settings_link)
        detail_layout.addLayout(links_layout)

        # Devices section
        devices_header = QLabel("Devices")
        devices_header.setObjectName("SectionHeader")
        detail_layout.addWidget(devices_header)

        devices_frame = QFrame()
        devices_frame.setObjectName("DevicesFrame")
        devices_frame_layout = QVBoxLayout(devices_frame)
        devices_frame_layout.setContentsMargins(10, 8, 10, 8)
        self.devices_layout = QVBoxLayout()
        self.devices_layout.setSpacing(4)
        devices_frame_layout.addLayout(self.devices_layout)
        detail_layout.addWidget(devices_frame)

        detail_layout.addStretch(0)

        # ---- Far right: Preview pane ----
        preview_panel = QWidget()
        preview_layout = QVBoxLayout(preview_panel)
        preview_layout.setContentsMargins(16, 16, 16, 16)

        self.preview_frame = QFrame()
        self.preview_frame.setObjectName("PreviewFrame")
        self.preview_frame.setMinimumSize(500, 400)
        self.preview_frame.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        preview_layout.addWidget(self.preview_frame)
        # Force the native HWND to be realized now, at startup, rather than
        # lazily on first winId() call from inside a QTimer callback later.
        self.preview_frame.winId()

        vm_page_layout.addWidget(detail_panel, 1)
        vm_page_layout.addWidget(preview_panel, 2)

        # ---- Home page ----
        home_page = self._build_home_page()

        self.content_stack = QStackedWidget()
        self.content_stack.addWidget(home_page)   # index 0
        self.content_stack.addWidget(vm_page)      # index 1

        splitter.addWidget(library_panel)
        splitter.addWidget(self.content_stack)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)

        self.setCentralWidget(splitter)

    def _build_usage_panel(self):
        """The App Utilisation panel: what LocalHost and its running VMs are
        costing the machine, so the answer doesn't require Task Manager."""
        panel = QWidget()
        layout = QVBoxLayout(panel)
        layout.setContentsMargins(0, 8, 0, 0)
        layout.setSpacing(4)

        header = QLabel("App Utilisation")
        header.setObjectName("PanelHeader")
        layout.addWidget(header)

        frame = QFrame()
        frame.setObjectName("UsageFrame")
        frame_layout = QVBoxLayout(frame)
        frame_layout.setContentsMargins(10, 8, 10, 8)
        frame_layout.setSpacing(7)

        self.usage_meters = {
            "cpu": _UsageMeter("CPU"),
            "ram": _UsageMeter("RAM"),
            "disk": _UsageMeter("Disk"),
            "gpu": _UsageMeter("GPU"),
        }
        for key in ("cpu", "ram", "disk", "gpu"):
            frame_layout.addWidget(self.usage_meters[key])

        self.usage_scope_label = QLabel("No VMs running")
        self.usage_scope_label.setObjectName("UsageScope")
        frame_layout.addWidget(self.usage_scope_label)

        layout.addWidget(frame)
        # Four meters plus the scope line have a real minimum size; without this
        # the splitter is free to squeeze the panel away to nothing.
        panel.setMinimumHeight(frame.sizeHint().height() + header.sizeHint().height() + 12)

        self.usage_sampler = _AppUsageSampler()
        if psutil is None:
            self.usage_scope_label.setText("psutil not installed")
        elif not self.usage_sampler.gpu.available:
            # Say so rather than showing a permanent 0% that looks like a bug.
            self.usage_meters["gpu"].set_value(None, "n/a")

        self._usage_timer = QTimer(self)
        self._usage_timer.timeout.connect(self._update_app_usage)
        self._usage_timer.start(1000)
        return panel

    def _update_app_usage(self):
        # Skipped while the window is minimised or hidden: sampling costs a
        # handful of syscalls per VM per second, and nobody is reading it.
        if self.isMinimized() or not self.isVisible():
            return
        try:
            usage = self.usage_sampler.sample(list(self.processes.values()))
        except Exception:
            # Monitoring must never be able to take the app down with it.
            return

        if not usage["available"]:
            return

        cpu = usage["cpu"]
        self.usage_meters["cpu"].set_value(cpu, "--" if cpu is None else f"{cpu:.0f}%")

        ram_pct = usage["ram_percent"]
        self.usage_meters["ram"].set_value(ram_pct, _format_bytes(usage["ram_bytes"]))

        disk = usage["disk_bps"]
        if disk is None:
            self.usage_meters["disk"].set_value(None, "--")
        else:
            # No fixed ceiling exists for disk throughput, so the bar is scaled
            # against the highest rate seen this session -- it shows relative
            # activity, while the figure beside it stays absolute.
            peak = max(self.usage_sampler.disk_peak, 1.0)
            self.usage_meters["disk"].set_value(
                disk * 100.0 / peak, f"{_format_bytes(disk)}/s")

        gpu = usage["gpu"]
        if gpu is None:
            self.usage_meters["gpu"].set_value(None, "n/a")
        else:
            self.usage_meters["gpu"].set_value(gpu, f"{gpu:.0f}%")

        n = usage["vm_count"]
        self.usage_scope_label.setText(
            "LocalHost only" if n == 0 else
            f"LocalHost + {n} VM" if n == 1 else f"LocalHost + {n} VMs")

    def _build_home_page(self):
        home_page = QWidget()
        outer = QVBoxLayout(home_page)
        outer.addStretch(1)

        title = QLabel("LocalHost Workstation Pro")
        title.setObjectName("HomeTitle")
        title.setAlignment(Qt.AlignCenter)
        outer.addWidget(title)

        subtitle = QLabel("Create, run, and manage your virtual machines")
        subtitle.setObjectName("HomeSubtitle")
        subtitle.setAlignment(Qt.AlignCenter)
        outer.addWidget(subtitle)

        outer.addSpacing(30)

        buttons_row = QHBoxLayout()
        buttons_row.addStretch(1)

        self.get_started_btn = self._make_home_button("Get Started")
        self.create_vm_btn = self._make_home_button("Create a New Virtual Machine")
        self.connect_server_btn = self._make_home_button("Connect to VPS")

        self.get_started_btn.clicked.connect(self.on_get_started)
        self.create_vm_btn.clicked.connect(self.on_create_vm)
        self.connect_server_btn.clicked.connect(self.on_connect_vps)

        buttons_row.addWidget(self.get_started_btn)
        buttons_row.addWidget(self.create_vm_btn)
        buttons_row.addWidget(self.connect_server_btn)
        buttons_row.addStretch(1)

        outer.addLayout(buttons_row)
        outer.addStretch(2)

        return home_page

    def _make_home_button(self, text):
        btn = QPushButton(text)
        btn.setObjectName("HomeCardButton")
        btn.setCursor(Qt.PointingHandCursor)
        btn.setMinimumSize(200, 120)
        btn.setSizePolicy(QSizePolicy.Preferred, QSizePolicy.Fixed)
        return btn

    def _build_status_bar(self):
        self.setStatusBar(QStatusBar())

    # ------------------------------------------------------------------
    # Persistence — library.json (folders + VMs), so nobody's VMs
    # vanish overnight if the app crashes or the machine loses power.
    # ------------------------------------------------------------------
    @staticmethod
    def library_file_path():
        app_data_dir = QStandardPaths.writableLocation(
            QStandardPaths.AppDataLocation
        )
        if not app_data_dir:
            # Fallback: next to the script, if AppData isn't resolvable
            app_data_dir = str(Path(__file__).resolve().parent)
        folder = Path(app_data_dir)
        folder.mkdir(parents=True, exist_ok=True)
        return folder / "library.json"

    def _vm_disks_dir(self):
        d = self.library_file_path().parent / "disks"
        d.mkdir(parents=True, exist_ok=True)
        return d

    def _create_vm_disk_image(self, vm_name, size_gb):
        """Allocate a per-VM raw disk image (sparse -- doesn't actually use
        size_gb worth of space until the guest writes to it). The hypervisor's
        ATA emulation is LBA28, so only the first ~128 GB is addressable."""
        safe_name = "".join(c if c.isalnum() or c in "-_ " else "_" for c in vm_name).strip() or "vm"
        path = self._vm_disks_dir() / f"{safe_name}.img"
        if not _create_sparse_disk_image(path, size_gb * 1024 * 1024 * 1024):
            self.statusBar().showMessage(f"Failed to create disk image at {path}", 5000)
            return None
        return path

    def _vm_snapshots_dir(self, vm_name):
        safe_name = "".join(c if c.isalnum() or c in "-_ " else "_" for c in vm_name).strip() or "vm"
        d = self.library_file_path().parent / "snapshots" / safe_name
        d.mkdir(parents=True, exist_ok=True)
        return d

    def _force_stop_vm(self, vm_name):
        """Kill a VM's process (if running) and immediately clear its
        tracking state, rather than waiting up to a second for the poll
        timer to notice on its own -- used right before removing/deleting
        a VM so it can't be left running with no tree row left to control
        it. Doesn't go through on_vm_process_finished since that also
        schedules a restart for a VM mid-Restart, which is exactly what we
        don't want while the VM is being deleted out from under it."""
        pid = self.processes.pop(vm_name, None)
        if pid is not None:
            _terminate_process(pid)
        self.embedded_hwnds.pop(vm_name, None)
        self.suspended_vms.discard(vm_name)
        self._pending_restart.discard(vm_name)
        if self.fullscreen_vm == vm_name:
            self.fullscreen_vm = None

    @staticmethod
    def _unlink_with_retry(path_str, attempts=10, delay=0.2):
        """TerminateProcess (see _force_stop_vm) returns before Windows has
        necessarily finished releasing that process's file handles -- if a
        VM was just running, deleting its disk image right afterward can
        race that teardown and fail with the file still "in use" for a
        brief moment. Retry for up to ~2s instead of silently leaving the
        disk image behind."""
        path = Path(path_str)
        for _ in range(attempts):
            try:
                path.unlink(missing_ok=True)
                return
            except OSError:
                time.sleep(delay)
        try:
            path.unlink(missing_ok=True)
        except OSError:
            pass

    def _delete_vm_files(self, vm):
        """Best-effort delete of everything "Delete from Disk" claims to
        delete: the VM's own disk image and all of its snapshot images.
        Failures are swallowed (e.g. a file still locked after retrying)
        rather than blocking the library removal that follows -- this is
        already a last-resort cleanup, not a transaction."""
        if vm.disk_path:
            self._unlink_with_retry(vm.disk_path)
        for snap in vm.snapshots:
            snap_path = snap.get("disk_path", "")
            if snap_path:
                self._unlink_with_retry(snap_path)
        try:
            self._vm_snapshots_dir(vm.name).rmdir()  # only if now empty
        except OSError:
            pass

    def _serialize_node(self, item):
        node_type = item.data(0, Qt.UserRole)
        if node_type == "vm":
            return {"type": "vm", "name": item.text(0)}
        if node_type == "folder":
            return {
                "type": "folder",
                "name": item.text(0),
                "children": [
                    self._serialize_node(item.child(i))
                    for i in range(item.childCount())
                ],
            }
        return None

    def save_library(self):
        """Write the current folder/VM tree + VM specs to library.json.
        Safe to call often — it's cheap and atomic (write temp, then replace)."""
        if self._loading:
            return  # don't save while we're still loading, we'd stomp the file

        data = {
            "tree": [
                self._serialize_node(self.my_computer_item.child(i))
                for i in range(self.my_computer_item.childCount())
            ],
            "vms": {name: vm.to_dict() for name, vm in self.vms.items()},
        }

        path = self.library_file_path()
        tmp_path = path.with_suffix(".json.tmp")
        try:
            with open(tmp_path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2)
            tmp_path.replace(path)  # atomic on both Windows and POSIX
        except OSError as e:
            self.statusBar().showMessage(f"Failed to save library: {e}", 5000)

    def _build_nodes(self, nodes, parent_item):
        for node in nodes:
            if node.get("type") == "vm":
                name = node.get("name", "")
                if name not in self.vms:
                    continue  # orphaned entry, e.g. vms dict was hand-edited
                vm_item = QTreeWidgetItem(parent_item, [name])
                vm_item.setData(0, Qt.UserRole, "vm")
            elif node.get("type") == "folder":
                folder_item = QTreeWidgetItem(parent_item, [node.get("name", "Folder")])
                folder_item.setData(0, Qt.UserRole, "folder")
                self._build_nodes(node.get("children", []), folder_item)

    def load_library(self):
        path = self.library_file_path()
        if not path.exists():
            return

        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError) as e:
            self.statusBar().showMessage(f"Could not read library.json: {e}", 5000)
            return

        self._loading = True
        try:
            vms_data = data.get("vms", {})
            self.vms = {
                name: VM.from_dict(name, spec) for name, spec in vms_data.items()
            }
            self._build_nodes(data.get("tree", []), self.my_computer_item)
            self.my_computer_item.setExpanded(True)
        finally:
            self._loading = False

    def closeEvent(self, event):
        # VMs are detached OS processes (see power_on_vm), not owned by
        # this app -- they survive a real close on their own. So closing
        # the manager always really closes it; the only question is what
        # happens to any VMs still running first.
        running_vms = list(self.processes.keys())
        if running_vms:
            choice = self._prompt_running_vms_on_close(running_vms)
            if choice == "cancel":
                event.ignore()
                return

            if choice in ("background", "suspend"):
                # Pop every embedded VM window out of the preview pane and
                # hide it -- otherwise it would either get destroyed along
                # with this window (it's a WS_CHILD of it) or, if detached
                # while visible, end up floating on the desktop with no
                # manager left to control it. blocking=True matters here:
                # this has to actually finish before the window underneath
                # it is torn down, not just get scheduled on a thread that
                # may not run in time.
                for vm_name in running_vms:
                    if vm_name == self.fullscreen_vm:
                        # Already a top-level WS_POPUP (see
                        # toggle_vm_fullscreen), so it survives the main
                        # window closing on its own without needing to be
                        # detached -- it just still needs to be hidden, or
                        # it's left covering the whole screen with no
                        # manager left to bring it back. Blocking, same
                        # reasoning as _detach_from_preview: if this VM is
                        # about to be suspended too, the hide has to land
                        # before its owning thread is frozen.
                        hwnd = self.embedded_hwnds.get(vm_name)
                        if hwnd:
                            t = threading.Thread(target=_user32.ShowWindow, args=(hwnd, SW_HIDE), daemon=True)
                            t.start()
                            t.join(timeout=2.0)
                        self.fullscreen_vm = None
                    elif vm_name in self.embedded_hwnds:
                        self._detach_from_preview(vm_name, blocking=True, show=False)
                if choice == "suspend":
                    for vm_name in running_vms:
                        if vm_name not in self.suspended_vms:
                            pid = self.processes.get(vm_name)
                            if pid and _suspend_process(pid):
                                self.suspended_vms.add(vm_name)
            else:
                # choice == "shutdown"
                for pid in list(self.processes.values()):
                    _terminate_process(pid)

        # Past the point of no return -- a cancelled close returns above, so
        # reaching here means the window really is going away. Stop sampling and
        # release the PDH query handle.
        if hasattr(self, "_usage_timer"):
            self._usage_timer.stop()
        if hasattr(self, "usage_sampler"):
            self.usage_sampler.close()

        self.save_library()
        if getattr(self, "_keyboard_hook", None):
            _user32.UnhookWindowsHookEx(self._keyboard_hook)
            self._keyboard_hook = None
        super().closeEvent(event)

    def _prompt_running_vms_on_close(self, running_vms):
        box = QMessageBox(self)
        box.setIcon(QMessageBox.Question)
        box.setWindowTitle("Virtual Machines Still Running")
        names = "\n".join(f"• {name}" for name in running_vms)
        noun = "virtual machine is" if len(running_vms) == 1 else "virtual machines are"
        box.setText(
            f"The following {noun} still powered on:\n\n{names}\n\n"
            "What would you like to do before closing?"
        )
        bg_btn = box.addButton("Run in Background", QMessageBox.AcceptRole)
        suspend_btn = box.addButton("Suspend", QMessageBox.AcceptRole)
        shutdown_btn = box.addButton("Shut Down", QMessageBox.DestructiveRole)
        cancel_btn = box.addButton(QMessageBox.Cancel)
        box.setDefaultButton(cancel_btn)
        box.exec()

        clicked = box.clickedButton()
        if clicked == bg_btn:
            return "background"
        if clicked == suspend_btn:
            return "suspend"
        if clicked == shutdown_btn:
            return "shutdown"
        return "cancel"

    def resizeEvent(self, event):
        super().resizeEvent(event)
        if self.current_vm and self.fullscreen_vm is None:
            self._resize_embedded_window(self.current_vm.name)

    # ------------------------------------------------------------------
    # Behavior
    # ------------------------------------------------------------------
    def on_library_search_changed(self, text):
        query = text.strip().lower()
        for i in range(self.my_computer_item.childCount()):
            self._filter_tree_item(self.my_computer_item.child(i), query)

    def _filter_tree_item(self, item, query):
        """Hides VMs (and folders with no matching descendants) that don't
        match query. Returns whether this item ended up visible, so a
        parent folder can tell if any of its children matched. A folder
        whose own name matches stays visible (with its full contents)
        even if none of the VMs inside it do."""
        node_type = item.data(0, Qt.UserRole)
        if node_type == "vm":
            match = query in item.text(0).lower()
            item.setHidden(not match)
            return match

        if node_type == "folder":
            name_match = query in item.text(0).lower()
            # If the folder itself matches, show its whole contents --
            # recurse with an empty query so every descendant counts as a
            # match too, instead of hiding VMs that don't individually
            # match the search text.
            child_query = "" if name_match else query
            any_child_visible = False
            for i in range(item.childCount()):
                if self._filter_tree_item(item.child(i), child_query):
                    any_child_visible = True
            visible = name_match or any_child_visible
            item.setHidden(not visible)
            if visible and query:
                item.setExpanded(True)
            return visible

        return True  # unrecognized node type -- leave it alone, stay visible

    def on_tree_selection_changed(self, current, previous):
        if current is None:
            return

        node_type = current.data(0, Qt.UserRole)

        # "Home", "My Computer", or a folder -> stay on / return to home page
        if node_type in ("home", "root", "folder"):
            self.content_stack.setCurrentIndex(0)
            return

        # A VM was selected -> show VM detail page
        vm_name = current.text(0)
        vm = self.vms.get(vm_name)
        if not vm:
            return

        self.current_vm = vm
        self.content_stack.setCurrentIndex(1)
        self.vm_title.setText(vm.name)
        self._update_power_ui()
        if (vm.name in self.processes and vm.name not in self.embedded_hwnds
                and self.fullscreen_vm != vm.name):
            # Running (e.g. reconnected from a prior session, or left
            # backgrounded with its window hidden) but not embedded in
            # *this* window yet -- pull it into the preview pane now.
            self._embed_hypervisor_window(vm.name)
        self._show_embedded_window_for_current_vm()
        self._refresh_device_rows()

    def _refresh_device_rows(self):
        vm = self.current_vm
        if not vm:
            return

        # Clear old device rows
        while self.devices_layout.count():
            item = self.devices_layout.takeAt(0)
            w = item.widget()
            if w:
                w.deleteLater()

        device_rows = [
            ("Guest OS", vm.guest_os),
            ("Firmware", vm.firmware),
            ("Memory", vm.memory),
            ("Processors", str(vm.processors)),
            ("Hard Disk (NVMe)", vm.hard_disk),
            ("CD/DVD", vm.cdrom),
            ("Network Adapter", vm.network),
            ("USB Controller", vm.usb),
            ("Sound Card", vm.sound),
            ("Display", vm.display),
            ("Trusted Platform Module", vm.tpm),
        ]
        for label, value in device_rows:
            row = QWidget()
            row_layout = QHBoxLayout(row)
            row_layout.setContentsMargins(0, 0, 0, 0)
            name_label = QLabel(label)
            name_label.setObjectName("DeviceLabel")
            value_label = QLabel(value)
            value_label.setObjectName("DeviceValue")
            row_layout.addWidget(name_label)
            row_layout.addStretch(1)
            row_layout.addWidget(value_label)
            self.devices_layout.addWidget(row)

    def on_tree_context_menu(self, pos):
        item = self.vm_tree.itemAt(pos)
        selected = self.vm_tree.selectedItems()
        vm_selected = [i for i in selected if i.data(0, Qt.UserRole) == "vm"]

        menu = QMenu(self)

        # --- Multiple VMs rubber-band-selected: batch actions ---
        if item is not None and item in vm_selected and len(vm_selected) > 1:
            power_on_action = menu.addAction(f"Power On {len(vm_selected)} VMs")
            menu.addSeparator()
            group_action = menu.addAction("Group into New Folder...")
            menu.addSeparator()
            remove_action = menu.addAction(f"Remove {len(vm_selected)} from Library")

            chosen = menu.exec(self.vm_tree.viewport().mapToGlobal(pos))
            if chosen == power_on_action:
                self.on_power_on_multiple(vm_selected)
            elif chosen == group_action:
                self.on_group_into_folder(vm_selected)
            elif chosen == remove_action:
                for vm_item in list(vm_selected):
                    self.on_remove_vm(vm_item)
            return

        node_type = item.data(0, Qt.UserRole) if item is not None else None

        # --- Right-clicked a single VM row ---
        if node_type == "vm":
            power_on_action = menu.addAction("Power On")
            edit_action = menu.addAction("Settings...")
            edit_action.setEnabled(item.text(0) not in self.processes)
            menu.addSeparator()
            remove_action = menu.addAction("Remove from Library")
            delete_action = menu.addAction("Delete from Disk")

            chosen = menu.exec(self.vm_tree.viewport().mapToGlobal(pos))
            if chosen is None:
                return

            self.vm_tree.setCurrentItem(item)  # select it so handlers act on the right VM
            if chosen == power_on_action:
                self.on_power_on()
            elif chosen == edit_action:
                self.on_edit_settings()
            elif chosen == remove_action:
                self.on_remove_vm(item)
            elif chosen == delete_action:
                self.on_delete_vm(item)

        # --- Right-clicked a folder ---
        elif node_type == "folder":
            new_vm_action = menu.addAction("New Virtual Machine...")
            menu.addSeparator()
            rename_action = menu.addAction("Rename Folder...")
            ungroup_action = menu.addAction("Ungroup Folder (keep VMs)")
            delete_action = menu.addAction("Delete Folder and VMs...")

            chosen = menu.exec(self.vm_tree.viewport().mapToGlobal(pos))
            if chosen == new_vm_action:
                self.on_create_vm(target_folder=item)
            elif chosen == rename_action:
                self.on_rename_folder(item)
            elif chosen == ungroup_action:
                self.on_ungroup_folder(item)
            elif chosen == delete_action:
                self.on_delete_folder(item)

        else:
            # Right-clicked empty space, "Home", or "My Computer"
            new_vm_action = menu.addAction("New Virtual Machine...")
            new_folder_action = menu.addAction("New Folder...")
            connect_action = menu.addAction("Connect to VPS...")

            chosen = menu.exec(self.vm_tree.viewport().mapToGlobal(pos))
            if chosen == new_vm_action:
                self.on_create_vm()
            elif chosen == new_folder_action:
                self.on_new_folder()
            elif chosen == connect_action:
                self.on_connect_vps()

    # ------------------------------------------------------------------
    # Folder / multi-select behavior
    # ------------------------------------------------------------------
    def on_group_into_folder(self, vm_items):
        """Take a rubber-band-selected batch of VMs and nest them under a
        new folder the user names, VMware/desktop-icon style."""
        folder_name, ok = QInputDialog.getText(self, "New Folder", "Folder name:")
        folder_name = folder_name.strip()
        if not ok or not folder_name:
            return

        folder_item = QTreeWidgetItem(self.my_computer_item, [folder_name])
        folder_item.setData(0, Qt.UserRole, "folder")

        for vm_item in vm_items:
            old_parent = vm_item.parent()
            if old_parent is None:
                continue
            old_parent.takeChild(old_parent.indexOfChild(vm_item))
            folder_item.addChild(vm_item)

        self.my_computer_item.setExpanded(True)
        folder_item.setExpanded(True)
        self.vm_tree.setCurrentItem(folder_item)
        self.statusBar().showMessage(
            f"Grouped {len(vm_items)} VM(s) into '{folder_name}'", 3000
        )
        self.save_library()

    def on_new_folder(self, parent_item=None):
        folder_name, ok = QInputDialog.getText(self, "New Folder", "Folder name:")
        folder_name = folder_name.strip()
        if not ok or not folder_name:
            return

        parent_item = parent_item or self.my_computer_item
        folder_item = QTreeWidgetItem(parent_item, [folder_name])
        folder_item.setData(0, Qt.UserRole, "folder")
        parent_item.setExpanded(True)
        self.vm_tree.setCurrentItem(folder_item)
        self.statusBar().showMessage(f"Created folder '{folder_name}'", 3000)
        self.save_library()

    def on_rename_folder(self, folder_item):
        old_name = folder_item.text(0)
        new_name, ok = QInputDialog.getText(
            self, "Rename Folder", "Folder name:", text=old_name
        )
        new_name = new_name.strip()
        if ok and new_name:
            folder_item.setText(0, new_name)
            self.statusBar().showMessage(f"Renamed folder to '{new_name}'", 3000)
            self.save_library()

    def on_ungroup_folder(self, folder_item):
        """Move the folder's VMs back to My Computer and remove the folder."""
        parent = folder_item.parent() or self.my_computer_item
        for vm_item in list(folder_item.takeChildren()):
            parent.addChild(vm_item)
        idx = self.my_computer_item.indexOfChild(folder_item)
        if idx != -1:
            self.my_computer_item.takeChild(idx)
        self.statusBar().showMessage("Folder removed, VMs kept in Library", 3000)
        self.save_library()

    def on_delete_folder(self, folder_item):
        """Remove the folder AND every VM nested inside it, deleting each
        VM's disk image and snapshots from disk too."""
        vm_names = [
            folder_item.child(i).text(0) for i in range(folder_item.childCount())
        ]
        choice = QMessageBox.warning(
            self, "Delete Folder and VMs?",
            f"Permanently delete this folder and {len(vm_names)} VM(s)? "
            "This deletes their hard disks and snapshots from disk too. "
            "This can't be undone.",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No
        )
        if choice != QMessageBox.Yes:
            return

        for name in vm_names:
            if name in self.processes:
                self._force_stop_vm(name)
            vm = self.vms.get(name)
            if vm:
                self._delete_vm_files(vm)
            self.vms.pop(name, None)
            if self.current_vm and self.current_vm.name == name:
                self.current_vm = None
                self.content_stack.setCurrentIndex(0)

        idx = self.my_computer_item.indexOfChild(folder_item)
        if idx != -1:
            self.my_computer_item.takeChild(idx)
        self.statusBar().showMessage(
            f"Deleted folder and {len(vm_names)} VM(s)", 3000
        )
        self.save_library()

    def on_power_on_multiple(self, vm_items):
        for vm_item in vm_items:
            vm = self.vms.get(vm_item.text(0))
            if vm:
                self.power_on_vm(vm)

    def on_remove_vm(self, item):
        vm_name = item.text(0)
        if vm_name in self.processes:
            choice = QMessageBox.warning(
                self, "VM is running",
                f"'{vm_name}' is currently running. Removing it from the "
                "library will power it off first. Continue?",
                QMessageBox.Yes | QMessageBox.No, QMessageBox.No
            )
            if choice != QMessageBox.Yes:
                return
            self._force_stop_vm(vm_name)

        parent = item.parent()
        if parent:
            parent.removeChild(item)
        self.vms.pop(vm_name, None)
        if self.current_vm and self.current_vm.name == vm_name:
            self.current_vm = None
            self.content_stack.setCurrentIndex(0)
        self.statusBar().showMessage(f"Removed '{vm_name}' from Library", 3000)
        # Note: this only removes it from the sidebar, not from disk
        self.save_library()

    def on_delete_vm(self, item):
        vm_name = item.text(0)
        vm = self.vms.get(vm_name)
        choice = QMessageBox.warning(
            self, "Delete from Disk?",
            f"Permanently delete '{vm_name}'? This deletes its hard disk "
            "and all of its snapshots from disk. This can't be undone.",
            QMessageBox.Yes | QMessageBox.No, QMessageBox.No
        )
        if choice != QMessageBox.Yes:
            return

        if vm_name in self.processes:
            self._force_stop_vm(vm_name)
        if vm:
            self._delete_vm_files(vm)
        self.on_remove_vm(item)
        self.statusBar().showMessage(f"Deleted '{vm_name}' from disk", 3000)

    def on_power_on(self):
        if self.current_vm:
            self.power_on_vm(self.current_vm)

    def on_toolbar_power_toggle(self):
        if not self.current_vm:
            self.statusBar().showMessage("Select a VM first.", 3000)
            return
        if self.current_vm.name in self.processes:
            self.power_off_vm(self.current_vm)
        else:
            self.power_on_vm(self.current_vm)

    def on_power_on_link_clicked(self):
        if not self.current_vm:
            return
        if self.current_vm.name in self.processes:
            self.power_off_vm(self.current_vm)
        else:
            self.power_on_vm(self.current_vm)

    def on_restart_vm(self):
        if not self.current_vm:
            self.statusBar().showMessage("Select a VM first.", 3000)
            return
        vm = self.current_vm
        if vm.name not in self.processes:
            self.power_on_vm(vm)
            return
        if vm.name in self.suspended_vms:
            _resume_process(self.processes[vm.name])
            self.suspended_vms.discard(vm.name)
        self._pending_restart.add(vm.name)
        self.power_off_vm(vm)
        self.statusBar().showMessage(f"Restarting {vm.name}...", 3000)

    def on_suspend_vm(self):
        if not self.current_vm:
            self.statusBar().showMessage("Select a VM first.", 3000)
            return
        vm = self.current_vm
        pid = self.processes.get(vm.name)
        if pid is None:
            self.statusBar().showMessage(f"'{vm.name}' isn't running.", 3000)
            return

        if vm.name in self.suspended_vms:
            _resume_process(pid)
            self.suspended_vms.discard(vm.name)
            self.statusBar().showMessage(f"Resumed {vm.name}.", 3000)
            if vm.name not in self.embedded_hwnds and self.fullscreen_vm != vm.name:
                QTimer.singleShot(100, lambda: self._embed_hypervisor_window(vm.name))
        else:
            # Best-effort suspend: no snapshot format to save state to disk,
            # so this freezes the guest's process in place (all threads
            # stopped, full memory/register state intact) rather than a
            # true suspend-to-disk. Pop the window out of the shared preview
            # pane first -- once its owning thread is suspended it can't
            # answer the parent's synchronous window messages anymore, and
            # leaving it embedded as a WS_CHILD risks freezing the whole
            # manager, not just the guest window.
            if vm.name in self.embedded_hwnds and self.fullscreen_vm != vm.name:
                self._detach_from_preview(vm.name)
            if _suspend_process(pid):
                self.suspended_vms.add(vm.name)
                self.statusBar().showMessage(f"Suspended {vm.name} (paused in memory, not saved to disk).", 4000)
            else:
                self.statusBar().showMessage(f"Couldn't suspend {vm.name}.", 4000)
        self._update_power_ui()

    # ------------------------------------------------------------------
    # Hypervisor process management
    # ------------------------------------------------------------------
    @staticmethod
    def app_dir():
        if getattr(sys, "frozen", False):
            return Path(sys.executable).resolve().parent
        return Path(__file__).resolve().parent

    def hypervisor_exe_path(self):
        return self.app_dir() / "Hypervisor.exe"

    def power_on_vm(self, vm):
        if vm.name in self.processes:
            self.statusBar().showMessage(f"'{vm.name}' is already running.", 3000)
            return

        exe_path = self.hypervisor_exe_path()
        if not exe_path.exists():
            self.statusBar().showMessage(
                f"Hypervisor.exe not found at {exe_path}", 5000
            )
            return

        if vm.firmware == "UEFI":
            if not vm.uefi_firmware_path or not Path(vm.uefi_firmware_path).is_file():
                self.statusBar().showMessage(
                    f"'{vm.name}' is set to UEFI firmware but no OVMF.fd path is "
                    "configured -- edit VM settings to set one.", 6000
                )
                return
            firmware_path = vm.uefi_firmware_path
        else:
            firmware_path = "bios.bin"

        args = [vm.name, firmware_path]
        if vm.disk_path and Path(vm.disk_path).exists():
            args.append(vm.disk_path)

        # Detached, not owned: see the comment above _process_is_alive. A
        # normal QProcess would get killed the instant the manager exits,
        # which is exactly what "run in background"/"suspend on close"
        # needs to *not* happen.
        ok, pid = QProcess.startDetached(str(exe_path), args, str(self.app_dir()))
        if not ok:
            self.statusBar().showMessage(f"Failed to start '{vm.name}'.", 5000)
            return

        self.processes[vm.name] = pid
        self.statusBar().showMessage(f"Powering on {vm.name}...", 3000)
        if self.current_vm is vm:
            self._update_power_ui()
        QTimer.singleShot(200, lambda: self._embed_hypervisor_window(vm.name))

    def power_off_vm(self, vm):
        pid = self.processes.get(vm.name)
        if pid is None:
            return
        if self.fullscreen_vm == vm.name:
            self._exit_vm_fullscreen()
        self.suspended_vms.discard(vm.name)
        _terminate_process(pid)
        self.statusBar().showMessage(f"Powering off {vm.name}...", 3000)

    def on_vm_process_finished(self, vm_name):
        self.processes.pop(vm_name, None)
        self.embedded_hwnds.pop(vm_name, None)  # the window died with the process
        self.suspended_vms.discard(vm_name)
        if self.fullscreen_vm == vm_name:
            self.fullscreen_vm = None
        self.statusBar().showMessage(f"'{vm_name}' has stopped.", 3000)
        if self.current_vm and self.current_vm.name == vm_name:
            self._update_power_ui()
        if vm_name in self._pending_restart:
            self._pending_restart.discard(vm_name)
            vm = self.vms.get(vm_name)
            if vm:
                # Small delay so the old process/window is fully torn down
                # before we launch a new one under the same VM name.
                QTimer.singleShot(300, lambda: self.power_on_vm(vm))

    def _poll_running_processes(self):
        """Stand-in for QProcess.finished -- detached processes (see
        power_on_vm) don't have one, so we notice they've exited by asking
        Windows every second instead."""
        for vm_name, pid in list(self.processes.items()):
            if not _process_is_alive(pid):
                self.on_vm_process_finished(vm_name)

    def _reconnect_running_vms(self):
        """Since VMs are launched detached (see power_on_vm), a VM left
        running in the background survives this app closing entirely --
        including a full relaunch. On startup, find any of our own VMs
        that are still out there (by their window title) and adopt them
        back into self.processes instead of leaving them as untracked
        orphans, which would otherwise let the user "power on" a VM that's
        already running a second time, pointed at the same disk image.
        Known gap: there's no way to tell from here whether a reconnected
        VM was left suspended -- it's always adopted as plain "running".
        """
        for vm_name in self.vms:
            hwnd = None
            # FindWindowW compares titles via a synchronous cross-process
            # GetWindowText call to the target window -- if the guest's
            # message loop doesn't answer promptly (e.g. mid-frame in its
            # own emulation loop) it can spuriously come back empty, same
            # fragility _embed_hypervisor_window already retries around.
            for _ in range(5):
                hwnd = _user32.FindWindowW(None, f"LocalHost Hypervisor -- {vm_name}")
                if hwnd:
                    break
                time.sleep(0.1)
            if not hwnd:
                continue
            pid = wintypes.DWORD()
            _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            if pid.value:
                self.processes[vm_name] = pid.value

    def _update_power_ui(self):
        if not self.current_vm:
            return
        running = self.current_vm.name in self.processes
        suspended = self.current_vm.name in self.suspended_vms

        if running:
            self.power_on_link.setText("⏹  Power off this virtual machine")
        else:
            self.power_on_link.setText("▶  Power on this virtual machine")

        self.power_toolbar_action.setText("Power Off" if running else "Power On")
        self.suspend_toolbar_action.setText("Resume" if suspended else "Suspend")
        self.suspend_toolbar_action.setEnabled(running)
        self.restart_toolbar_action.setEnabled(True)
        self.edit_settings_link.setEnabled(not running)
        self.edit_settings_link.setCursor(
            Qt.ArrowCursor if running else Qt.PointingHandCursor
        )
        # Taking a snapshot means reading the disk image outside the
        # running hypervisor's handle on it -- needs it powered off.
        self.snapshot_toolbar_action.setEnabled(not running)

    # ------------------------------------------------------------------
    # Embedding the hypervisor's guest-display window into the preview pane
    # ------------------------------------------------------------------
    def _embed_hypervisor_window(self, vm_name, attempt=0):
        """Poll briefly for the hypervisor's window to appear (it's created
        inside a separate process, so it isn't there the instant the
        process starts), then reparent it into the preview pane instead of
        leaving it as a floating window."""
        if vm_name not in self.processes:
            return  # powered off again before its window ever showed up

        hwnd = _user32.FindWindowW(None, f"LocalHost Hypervisor -- {vm_name}")
        if not hwnd:
            if attempt < 25:
                QTimer.singleShot(200, lambda: self._embed_hypervisor_window(vm_name, attempt + 1))
            return

        preview_hwnd = int(self.preview_frame.winId())

        def do_embed():
            style = _user32.GetWindowLongPtrW(hwnd, GWL_STYLE)
            style = (style & ~(WS_POPUP | WS_DECORATIONS)) | WS_CHILD
            _user32.SetWindowLongPtrW(hwnd, GWL_STYLE, style)
            _user32.SetParent(hwnd, preview_hwnd)
            self._embed_ready.emit(vm_name, hwnd)

        _win32_async(do_embed)

    def _on_embedded(self, vm_name, hwnd):
        # Back on the Qt thread: safe to touch our own state and QWidgets.
        if vm_name not in self.processes:
            return  # powered off again while the reparent was in flight
        self.embedded_hwnds[vm_name] = hwnd
        if self.current_vm and self.current_vm.name == vm_name:
            self._resize_embedded_window(vm_name)
            _win32_async(_user32.ShowWindow, hwnd, SW_SHOW)
        else:
            _win32_async(_user32.ShowWindow, hwnd, SW_HIDE)

    def _resize_embedded_window(self, vm_name):
        hwnd = self.embedded_hwnds.get(vm_name)
        if not hwnd:
            return
        rect = self.preview_frame.rect()
        _win32_async(_user32.MoveWindow, hwnd, 0, 0, max(rect.width(), 1), max(rect.height(), 1), True)

    def _detach_from_preview(self, vm_name, blocking=False, show=True):
        """Pop a VM's window out of the shared preview pane back to a
        normal top-level window, without killing the process. Used before
        suspending a VM (see on_suspend_vm) so its embedded WS_CHILD window
        isn't left hanging off the main window once its owning thread is
        frozen -- and before closing the app to run in the background/
        suspend on close (see closeEvent), for two reasons: a WS_CHILD
        window is destroyed along with its parent, and even for a plain
        WS_POPUP, cross-process operations on a window whose owning thread
        may be about to get suspended risk becoming a blocking round trip
        instead of the instant local call they normally are. blocking=True
        waits for the detach to actually finish (up to 2s) before
        returning, for callers -- like closeEvent -- that need it done
        before the window underneath is torn down. show=False leaves it
        hidden instead of visible, for backgrounding it headlessly rather
        than popping it out onto the desktop."""
        hwnd = self.embedded_hwnds.pop(vm_name, None)
        if not hwnd:
            return

        def do_detach():
            style = _user32.GetWindowLongPtrW(hwnd, GWL_STYLE)
            style = (style & ~WS_CHILD) | WS_POPUP | WS_DECORATIONS
            _user32.SetWindowLongPtrW(hwnd, GWL_STYLE, style)
            _user32.SetParent(hwnd, None)
            _user32.SetWindowPos(hwnd, None, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED)
            _user32.ShowWindow(hwnd, SW_SHOW if show else SW_HIDE)

        if blocking:
            t = threading.Thread(target=do_detach, daemon=True)
            t.start()
            t.join(timeout=2.0)
        else:
            _win32_async(do_detach)

    def _show_embedded_window_for_current_vm(self):
        """Only one VM's console can occupy the shared preview pane at a
        time -- hide every other embedded window, show/resize this one."""
        vm_name = self.current_vm.name if self.current_vm else None
        for name, hwnd in self.embedded_hwnds.items():
            if name == vm_name or name == self.fullscreen_vm:
                continue
            _win32_async(_user32.ShowWindow, hwnd, SW_HIDE)
        if vm_name and vm_name in self.embedded_hwnds and vm_name != self.fullscreen_vm:
            self._resize_embedded_window(vm_name)
            _win32_async(_user32.ShowWindow, self.embedded_hwnds[vm_name], SW_SHOW)

    # ------------------------------------------------------------------
    # Fullscreen (Shift+F11)
    # ------------------------------------------------------------------
    def _foreground_belongs_to_us(self):
        """True if the current OS foreground window is either our own main
        window or one of our VM subprocesses' windows (embedded or already
        fullscreened) -- scopes the global keyboard hook so it doesn't
        steal Shift+F11 from unrelated applications."""
        hwnd = _user32.GetForegroundWindow()
        if not hwnd:
            return False
        pid = wintypes.DWORD()
        _user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        fg_pid = pid.value
        if fg_pid == os.getpid():
            return True
        return fg_pid in self.processes.values()

    def _keyboard_hook_callback(self, nCode, wParam, lParam):
        if nCode == HC_ACTION:
            kbd = lParam.contents
            if kbd.vkCode == VK_F11 and (_user32.GetKeyState(VK_SHIFT) & 0x8000):
                if wParam in (WM_KEYDOWN, WM_SYSKEYDOWN):
                    if not self._f11_down:
                        # Only the physical press triggers the toggle --
                        # holding the key generates repeated WM_KEYDOWN
                        # events (auto-repeat), which without this guard
                        # would toggle fullscreen on and off rapidly.
                        self._f11_down = True
                        if self._foreground_belongs_to_us():
                            self.toggle_vm_fullscreen()
                    return 1  # swallow the press (and its repeats)
                elif wParam in (WM_KEYUP, WM_SYSKEYUP):
                    self._f11_down = False
                    return 1  # swallow the matching release too
        return _user32.CallNextHookEx(self._keyboard_hook, nCode, wParam, lParam)

    def toggle_vm_fullscreen(self):
        if self.fullscreen_vm is not None:
            self._exit_vm_fullscreen()
            return

        if not self.current_vm or self.current_vm.name not in self.embedded_hwnds:
            self.statusBar().showMessage("No running VM to show fullscreen.", 3000)
            return

        vm_name = self.current_vm.name
        hwnd = self.embedded_hwnds[vm_name]
        screen = self.screen() or QApplication.primaryScreen()
        geo = screen.geometry()
        # Qt reports geometry in logical pixels; raw Win32 SetWindowPos wants
        # physical pixels. At fractional DPI scaling (e.g. 225%) these
        # differ, and using the logical size directly leaves the "fullscreen"
        # window covering only a corner of the real screen.
        dpr = screen.devicePixelRatio()
        x, y, w, h = (int(geo.x() * dpr), int(geo.y() * dpr),
                      int(geo.width() * dpr), int(geo.height() * dpr))

        def do_fullscreen():
            style = _user32.GetWindowLongPtrW(hwnd, GWL_STYLE)
            style = (style & ~(WS_CHILD | WS_DECORATIONS)) | WS_POPUP
            _user32.SetWindowLongPtrW(hwnd, GWL_STYLE, style)
            _user32.SetParent(hwnd, None)
            # hWndInsertAfter=None (0) means HWND_TOP here since SWP_NOZORDER
            # isn't set -- brings it visually to front without the
            # focus-stealing semantics of SetForegroundWindow.
            _user32.SetWindowPos(hwnd, None, x, y, w, h, SWP_FRAMECHANGED)
            _user32.ShowWindow(hwnd, SW_SHOW)
            # Deliberately no SetForegroundWindow: it has cross-process
            # shell/COM hooks (focus-stealing prevention) that triggered a
            # fatal 0x8001010D (RPC_E_CANTCALLOUT_ININPUTSYNCCALL) even from
            # a background thread -- unlike SetParent/SetWindowPos, the
            # conflict isn't about which of our threads calls it, it's with
            # another process's (explorer/DWM) synchronous dispatch. The
            # reparent + resize above already produces the fullscreen
            # window; it just may not be topmost until clicked.

        _win32_async(do_fullscreen)
        self.fullscreen_vm = vm_name

    def _exit_vm_fullscreen(self):
        vm_name = self.fullscreen_vm
        self.fullscreen_vm = None
        hwnd = self.embedded_hwnds.get(vm_name)
        if not hwnd:
            return

        preview_hwnd = int(self.preview_frame.winId())
        show_after = bool(self.current_vm and self.current_vm.name == vm_name)

        def do_unfullscreen():
            style = _user32.GetWindowLongPtrW(hwnd, GWL_STYLE)
            style = (style & ~(WS_POPUP | WS_DECORATIONS)) | WS_CHILD
            _user32.SetWindowLongPtrW(hwnd, GWL_STYLE, style)
            _user32.SetParent(hwnd, preview_hwnd)
            _user32.SetWindowPos(hwnd, None, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED)
            if show_after:
                _user32.ShowWindow(hwnd, SW_SHOW)
                self._request_resize.emit(vm_name)
            else:
                _user32.ShowWindow(hwnd, SW_HIDE)

        _win32_async(do_unfullscreen)

    def on_edit_settings(self):
        if not self.current_vm:
            return
        is_running = self.current_vm.name in self.processes
        dialog = VMSettingsDialog(self.current_vm, is_running=is_running, parent=self)
        if dialog.exec() == QDialog.Accepted:
            self._refresh_device_rows()
            self.save_library()
            self.statusBar().showMessage(f"Updated settings for {self.current_vm.name}.", 3000)

    # ------------------------------------------------------------------
    # Snapshots (disk-only for now -- no memory/CPU state yet, see VM.snapshots)
    # ------------------------------------------------------------------
    def on_take_snapshot(self):
        if not self.current_vm:
            self.statusBar().showMessage("Select a VM first.", 3000)
            return
        self.take_snapshot(self.current_vm)

    def on_revert_snapshot(self):
        if not self.current_vm:
            self.statusBar().showMessage("Select a VM first.", 3000)
            return
        if not self.current_vm.snapshots:
            self.statusBar().showMessage(f"'{self.current_vm.name}' has no snapshots yet.", 3000)
            return
        # No "current snapshot" pointer to revert to implicitly -- let the
        # user pick which one from the manager.
        self.on_manage_snapshots()

    def on_manage_snapshots(self):
        if not self.current_vm:
            self.statusBar().showMessage("Select a VM first.", 3000)
            return
        dialog = SnapshotManagerDialog(self, self.current_vm, parent=self)
        dialog.exec()

    def take_snapshot(self, vm):
        if vm.name in self.processes:
            self.statusBar().showMessage(f"Power off '{vm.name}' before taking a snapshot.", 4000)
            return False
        if not vm.disk_path or not Path(vm.disk_path).exists():
            self.statusBar().showMessage(f"'{vm.name}' has no disk to snapshot.", 4000)
            return False

        default_name = f"Snapshot {len(vm.snapshots) + 1}"
        name, ok = QInputDialog.getText(self, "Take Snapshot", "Snapshot name:", text=default_name)
        name = name.strip()
        if not ok or not name:
            return False
        if any(s["name"] == name for s in vm.snapshots):
            QMessageBox.warning(self, "Snapshot exists", f"'{vm.name}' already has a snapshot named '{name}'.")
            return False

        snap_path = self._vm_snapshots_dir(vm.name) / f"{int(time.time() * 1000)}.img"
        self.statusBar().showMessage(f"Taking snapshot '{name}'...")
        QApplication.processEvents()
        if not _copy_sparse_disk_image(vm.disk_path, snap_path):
            self.statusBar().showMessage(f"Failed to take snapshot '{name}'.", 5000)
            return False

        vm.snapshots.append({
            "name": name,
            "created": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
            "disk_path": str(snap_path),
        })
        self.save_library()
        self.statusBar().showMessage(f"Snapshot '{name}' taken.", 3000)
        return True

    def revert_to_snapshot(self, vm, snap):
        if vm.name in self.processes:
            self.statusBar().showMessage(f"Power off '{vm.name}' before reverting.", 4000)
            return False
        if not vm.disk_path:
            self.statusBar().showMessage(f"'{vm.name}' has no disk to revert.", 4000)
            return False
        snap_path = snap.get("disk_path", "")
        if not snap_path or not Path(snap_path).exists():
            QMessageBox.warning(self, "Snapshot missing", "That snapshot's disk image can't be found.")
            return False

        self.statusBar().showMessage(f"Reverting to '{snap['name']}'...")
        QApplication.processEvents()
        if not _copy_sparse_disk_image(snap_path, vm.disk_path):
            self.statusBar().showMessage(f"Failed to revert to '{snap['name']}'.", 5000)
            return False
        self.statusBar().showMessage(f"Reverted to '{snap['name']}'.", 3000)
        if self.current_vm is vm:
            self._refresh_device_rows()
        return True

    def delete_snapshot(self, vm, snap):
        snap_path = snap.get("disk_path", "")
        if snap_path:
            try:
                Path(snap_path).unlink(missing_ok=True)
            except OSError:
                pass
        # Match by disk_path (unique per snapshot), not object identity --
        # `snap` came back out of a QListWidgetItem via Qt's data storage,
        # which doesn't guarantee the same Python object identity as what
        # was stored in vm.snapshots.
        vm.snapshots = [s for s in vm.snapshots if s.get("disk_path") != snap_path]
        self.save_library()
        self.statusBar().showMessage(f"Deleted snapshot '{snap['name']}'.", 3000)
        return True

    def on_about(self):
        AboutDialog(self).exec()

    def on_report_bug(self):
        QDesktopServices.openUrl(QUrl("https://github.com/issues/recent"))

    def on_get_started(self):
        self.statusBar().showMessage("Get Started clicked", 3000)
        # TODO: open a "getting started" guide / walkthrough

    def on_create_vm(self, target_folder=None):
        wizard = NewVMWizard(self)
        if wizard.exec() == QDialog.Accepted and wizard.result_vm:
            new_vm = wizard.result_vm

            if new_vm.name in self.vms:
                self.statusBar().showMessage(f"A VM named '{new_vm.name}' already exists.", 4000)
                return

            disk_path = self._create_vm_disk_image(new_vm.name, wizard.disk_spin.value())
            if disk_path:
                new_vm.disk_path = str(disk_path)

            self.vms[new_vm.name] = new_vm

            parent_item = target_folder or self.my_computer_item
            new_item = QTreeWidgetItem(parent_item, [new_vm.name])
            new_item.setData(0, Qt.UserRole, "vm")
            parent_item.setExpanded(True)
            self.vm_tree.setCurrentItem(new_item)  # jumps straight to the new VM's detail page

            self.statusBar().showMessage(f"Created VM: {new_vm.name}", 3000)
            self.save_library()

    def on_connect_vps(self):
        dialog = ConnectToVPSDialog(self)
        if dialog.exec() == QDialog.Accepted and dialog.result_data:
            data = dialog.result_data
            self.statusBar().showMessage(
                f"Connecting to VPS {data['vps_id']}...", 3000
            )
            # TODO: authenticate against the VPS here, e.g.
            # self.backend.connect_vps(data['vps_id'], data['password'])
            # Note: data['password'] is deliberately not logged or shown.

    # ------------------------------------------------------------------
    # Styling
    # ------------------------------------------------------------------
    def _apply_dark_theme(self):
        self.setStyleSheet("""
            QMainWindow, QWidget { background-color: #1e1e1e; color: #e0e0e0; }
            QMenuBar { background-color: #252526; color: #e0e0e0; }
            QMenuBar::item:selected { background-color: #094771; }
            QMenu { background-color: #252526; color: #e0e0e0; }
            QMenu::item:selected { background-color: #094771; }
            QToolBar { background-color: #252526; border: none; spacing: 4px; padding: 4px; }
            QStatusBar { background-color: #252526; color: #a0a0a0; }

            QTreeWidget { background-color: #1e1e1e; border: none; color: #d0d0d0; }
            QTreeWidget::item { padding: 4px; }
            QTreeWidget::item:selected { background-color: #094771; color: white; }

            QLabel#PanelHeader { font-size: 13px; font-weight: bold; color: #a0a0a0; padding: 4px 0; }
            QLabel#VMTitle { font-size: 22px; font-weight: 600; padding-bottom: 6px; }
            QLabel#SectionHeader { font-size: 13px; font-weight: bold; color: #a0a0a0; padding-top: 8px; }
            QLabel#DeviceLabel { color: #d0d0d0; }
            QLabel#DeviceValue { color: #8ab4f8; }

            QPushButton#LinkButton {
                background: transparent;
                border: none;
                color: #4fa3ff;
                text-align: left;
                padding: 2px 0;
                font-size: 13px;
            }
            QPushButton#LinkButton:hover { color: #7cc0ff; text-decoration: underline; }

            QFrame#PreviewFrame {
                border: 1px solid #3c3c3c;
                border-radius: 2px;
                background-color: #0a0a0a;
            }

            QFrame#LibraryTreeFrame {
                border: 1px solid #3c3c3c;
                border-radius: 2px;
            }
            QFrame#DevicesFrame {
                border: 1px solid #3c3c3c;
                border-radius: 2px;
            }

            QFrame#UsageFrame {
                border: 1px solid #3c3c3c;
                border-radius: 2px;
            }
            QLabel#UsageName { color: #d0d0d0; font-size: 12px; }
            QLabel#UsageValue { color: #8ab4f8; font-size: 12px; font-weight: 600; }
            QLabel#UsageScope { color: #808080; font-size: 11px; padding-top: 2px; }
            QProgressBar#UsageBar {
                background-color: #2a2a2a;
                border: none;
                border-radius: 3px;
            }
            QProgressBar#UsageBar::chunk {
                background-color: #4fa3ff;
                border-radius: 3px;
            }

            QLabel#HomeTitle { font-size: 28px; font-weight: 600; color: #f0f0f0; }
            QLabel#HomeSubtitle { font-size: 14px; color: #a0a0a0; padding-top: 4px; }

            QPushButton#HomeCardButton {
                background-color: #2a2a2a;
                border: 1px solid #3c3c3c;
                border-radius: 6px;
                color: #e0e0e0;
                font-size: 13px;
                font-weight: 500;
                padding: 12px;
                margin: 0 10px;
            }
            QPushButton#HomeCardButton:hover {
                background-color: #333333;
                border: 1px solid #4fa3ff;
            }
            QPushButton#HomeCardButton:pressed {
                background-color: #094771;
            }
        """)


def main():
    app = QApplication(sys.argv)
    window = LocalHostWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()