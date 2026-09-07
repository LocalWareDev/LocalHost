// Known limitations and the backend roadmap are tracked in docs/
// (docs/known-limitations.md, docs/roadmap.md). In particular, real Windows
// guests currently bugcheck 0x5C during HAL timer init -- see
// docs/investigations/vppt-synic-blocker.md for the full investigation
// before re-attempting a fix here.

// USB host controller selection. xHCI replaces EHCI on the same PCI function
// (0:4.0) and the same GSI, so no DSDT/_PRT change is needed -- only the class
// prog-IF changes, 0x20 (EHCI) -> 0x30 (xHCI).
//
// Kept as a switch rather than a straight deletion until xHCI is carrying the
// tablet: this is the transport the pointer depends on, and "the mouse is gone"
// is not a bisectable signal on its own. EHCI goes once xHCI reports.
//
// MUST BE DEFINED HERE, at the top. It first lived beside the xhci_dev.c
// include far down the file, which is AFTER pciInitConfigSpaces -- so the #if
// there chose the EHCI branch, the device advertised prog-IF 0x20 and Intel
// ICH9 EHCI IDs, and Windows dutifully loaded usbehci.sys and spoke EHCI at our
// xHCI register model. That cost a full debug cycle and looked exactly like a
// wrong register map: the trace showed writes to "reserved" offsets that are
// EHCI's USBINTR, FRINDEX, PERIODICLISTBASE, ASYNCLISTADDR and CONFIGFLAG, and
// a USBCMD bit 16 that is EHCI's Interrupt Threshold Control.
// 0 while the xHCI model is incomplete: Windows binds USBXHCI.sys but loops
// through init/teardown without programming a ring, so the tablet does not
// enumerate and the guest has NO pointer. EHCI carries it today. Set to 1 to
// resume the xHCI work (next step there: MSI/MSI-X, see xhci_dev.c).
#define LH_USE_XHCI 0

#define _WIN32_WINNT 0x0A00
// winsock2.h must come before windows.h (it defines _WINSOCKAPI_, which
// stops windows.h from pulling in the legacy winsock.h and conflicting).
// This is the ONE place the networking backend (see the "net" section
// below, around rtl8139TransmitFrame/rtl8139ReceiveFrame) touches anything
// Windows-specific -- everything past initialization uses plain BSD
// sockets calls (socket/bind/connect/send/recv/select), the same subset
// Winsock and POSIX sockets share, so the backend logic itself stays
// portable to a hypothetical future Linux/macOS build.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// timeBeginPeriod. Without it Windows' default timer resolution is ~15.6ms, so
// every Sleep(1) in this file actually sleeps ~15.6ms -- see the call in main().
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include <WinHvPlatformDefs.h>
#include <WinHvPlatform.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <intrin.h>
#include <stdarg.h>
#include <errno.h>

unsigned char cmosRegisters[256] = { 0 };
unsigned char cmosSelectedReg = 0;
int refreshToggle = 0;

// --- CMOS RTC time/date fields (registers 0x00-0x09) + status registers
// A/B/D (0x0A/0x0B/0x0D) ---
// Previously cmosRegisters[] was a flat, statically zero-initialized array:
// reads of the RTC time/date fields always returned 0, and Register A/B/D
// never reflected real hardware semantics at all. Cross-referenced against
// the real EDK2 PcRtc.c source (the driver OVMF uses to implement
// EFI_RUNTIME_SERVICES->GetTime(), which Windows Boot Manager's
// BlpTimeInitialize calls into): PcRtcGetTime()'s RtcWaitToUpdate() helper
// returns EFI_DEVICE_ERROR outright if Register D's VRT (Valid RAM/Time)
// bit is 0, and separately, if Register B's Dm bit selects BCD mode (which
// bit 0 means it does, since Dm=0 -> BCD), a raw 0x00 byte IS valid BCD
// zero -- but the resulting Month=0/Day=0 then fails RtcTimeFieldsValid()'s
// range check (Month must be 1-12, Day must be a valid day for that
// month), which PcRtcGetTime() remaps to EFI_DEVICE_ERROR same as the VRT
// case. This exact chain was confirmed empirically via runtime
// instrumentation (BlpTimeInitialize is the function that returns
// 0xC0000185/STATUS_IO_DEVICE_ERROR, the NTSTATUS PcRtcGetTime's
// EFI_DEVICE_ERROR maps to) before this fix.
//
// Fix: report live wall-clock time on every read of 0x00-0x09, encoded to
// match whatever BCD/binary (Register B bit 2) and 12/24-hour (bit 1) mode
// is *currently* configured -- PcRtcGetTime() re-reads Register B fresh on
// every call rather than caching a fixed mode, so the encoding must track
// it live rather than being fixed at startup. Register A always reports
// UIP=0 (our "update" is instantaneous, never caught mid-cycle) and
// Register D always reports VRT=1 (battery/CMOS always valid) regardless
// of whatever raw byte a guest write may have stored there.
unsigned char cmosEncodeRtcField(unsigned char decimalValue, int useBcd) {
    if (!useBcd) return decimalValue;
    return (unsigned char)(((decimalValue / 10) << 4) | (decimalValue % 10));
}

extern LARGE_INTEGER perfFrequency;  // defined just below; needed by the latch
// Max age of the latched RTC snapshot, in ms. 0 disables the latch (re-sample on
// every register read, the old behaviour). Override with LOCALHOST_RTC_LATCH_MS.
double g_rtcLatchMs = 20.0;

unsigned char cmosReadRtcField(unsigned char reg) {
    // LATCHED, not sampled per register.
    //
    // This used to call GetLocalTime() fresh on every single register read, so a
    // guest reading the time as a sequence -- seconds, minutes, hours, day,
    // month, year, which is how every RTC driver does it -- could have the clock
    // tick between two of those reads and get an inconsistent time (23:59:59
    // followed by an hour of 00). Windows validates the sequence and RETRIES when
    // it disagrees, so it can spin re-reading the CMOS. Measured: 95% of all
    // sampled guest RIPs (11503 of 12055) sat in one page hammering ports
    // 0x70/0x71, with the index values showing repeated time-register sweeps.
    //
    // Real hardware holds the time registers stable between update cycles and
    // signals updates via UIP; since we always report UIP=0, we must present a
    // consistent snapshot instead. Latch one and reuse it for the whole burst: a
    // read sequence takes microseconds, so a gap of more than a few milliseconds
    // means a NEW sequence, which is when it is safe to re-sample. Between bursts
    // the clock still tracks real time exactly.
    //
    // Also removes a GetLocalTime syscall per register read.
    // The staleness bound is NOT optional: latching purely on "same burst" would
    // freeze the clock for any guest that polls the seconds register in a tight
    // loop waiting for it to tick -- a calibration loop would then spin forever,
    // which is worse than the bug being fixed. So re-sample either when a new
    // burst starts (a gap, meaning the previous sequence finished) or whenever the
    // snapshot is older than 250ms, whichever comes first. A read sequence lasts
    // microseconds, so the residual chance of a refresh landing mid-sequence is
    // negligible, and the clock can never drift more than a quarter second.
    static SYSTEMTIME cachedSt;
    static LARGE_INTEGER lastReadTick, lastRefreshTick;
    static int haveCache = 0;
    SYSTEMTIME st;
    {
        LARGE_INTEGER now;
        double gapMs, ageMs;
        QueryPerformanceCounter(&now);
        gapMs = (haveCache && perfFrequency.QuadPart)
            ? (double)(now.QuadPart - lastReadTick.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart
            : 1e9;
        ageMs = (haveCache && perfFrequency.QuadPart)
            ? (double)(now.QuadPart - lastRefreshTick.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart
            : 1e9;
        // 250ms was too coarse: it kept the exit count down but Windows loaded its
        // kernel and then stalled, never reaching the Setup GUI (screen frozen on
        // the firmware logo for 150s, guest idle at 160k exits). A quarter-second
        // granular clock is evidently not something the kernel's timekeeping
        // tolerates. 20ms still gives a read SEQUENCE (microseconds) a consistent
        // snapshot -- which is the entire point -- while advancing the clock 50
        // times a second. g_rtcLatchMs = 0 disables the latch entirely.
        if (!haveCache || g_rtcLatchMs <= 0.0 || gapMs > 5.0 || ageMs > g_rtcLatchMs) {
            GetLocalTime(&cachedSt);
            lastRefreshTick = now;
            haveCache = 1;
        }
        lastReadTick = now;
        st = cachedSt;
    }
    unsigned char regB = cmosRegisters[0x0B];
    int useBcd = (regB & 0x04) == 0;   // Dm bit: 0 = BCD, 1 = binary
    int is24Hour = (regB & 0x02) != 0; // Mil bit: 1 = 24-hour, 0 = 12-hour + PM bit

    switch (reg) {
        case 0x00: return cmosEncodeRtcField((unsigned char)st.wSecond, useBcd);
        case 0x02: return cmosEncodeRtcField((unsigned char)st.wMinute, useBcd);
        case 0x04: {
            unsigned char hour24 = (unsigned char)st.wHour;
            if (is24Hour) return cmosEncodeRtcField(hour24, useBcd);
            int isPM = hour24 >= 12;
            unsigned char hour12 = (unsigned char)(hour24 % 12);
            if (hour12 == 0) hour12 = 12;
            unsigned char encoded = cmosEncodeRtcField(hour12, useBcd);
            return isPM ? (unsigned char)(encoded | 0x80) : encoded;
        }
        case 0x06: return cmosEncodeRtcField((unsigned char)(st.wDayOfWeek + 1), useBcd); // RTC convention: 1-7
        case 0x07: return cmosEncodeRtcField((unsigned char)st.wDay, useBcd);
        case 0x08: return cmosEncodeRtcField((unsigned char)st.wMonth, useBcd);
        case 0x09: return cmosEncodeRtcField((unsigned char)(st.wYear % 100), useBcd);
        default: return 0;
    }
}

LARGE_INTEGER perfFrequency;
LARGE_INTEGER lastToggleTime;

// ---------------------------------------------------------------------
// Live INT3 breakpoint infrastructure for the bugcheck-0x139 investigation
// (see docs/investigations/post-vppt-boot-stall.md, "live breakpointing"
// update). Deliberately isolated to this block plus its two call sites
// (the reset handler and the main exit-handling switch) so it can be
// deleted cleanly once the root cause is identified.
//
// Strategy (round 2): the fail-site breakpoint (RVA 0x34420F, the
// `mov ecx,3` immediately preceding the `int 0x29` trap at 0x344214)
// already captured R10 (subsegment=NULL) reliably across two boots. But
// RDX (the "owner" parameter) read 0 there too -- and static disassembly
// shows RDX legitimately gets reassigned mid-function (`xor edx,edx` at
// RVA 0x344195, on an intermediate success path) before reaching the fail
// site, so that 0 is NOT trustworthy as the genuine entry parameter.
// Retargeted to the function's true entry instead (RVA 0x34412C, the
// first prologue instruction, confirmed live via FailSite.disasm.txt) --
// RCX/RDX/R8 are the genuine, unclobbered subsegment/owner/flags
// parameters there. Unlike the fail-site breakpoint, entry is hit on
// EVERY call (including ordinary successful ones), so the handler re-arms
// after each non-matching hit (RCX != 0) and only produces a full report
// (+ stops re-arming) when RCX == 0, the signature of the known failing
// call.
// RVA 0x34412C (RtlpHpLfhOwnerMoveSubsegment's true entry) confirmed both
// subsegment(rcx) and owner(rdx) NULL, then RVA 0x2382C1
// (RtlpHpLfhSlotAllocate+0xCA1, confirmed via a downloaded matching
// ntkrnlmp.pdb + capstone) confirmed the NULL value is `*(rbp+0x58)` --
// which a full-function disassembly then proved is RtlpHpLfhSlotAllocate's
// OWN second incoming parameter (spilled to its home slot at entry,
// offset math: rbp = entry_rsp-0x48, so rbp+0x58 = entry_rsp+0x10, exactly
// where the prologue's `mov [rsp+0x10],rdx` puts it), never written
// anywhere else in the function (see
// docs/investigations/post-vppt-boot-stall.md, 2026-07-17 continuation).
// [ACTIVE: retargeted back to 0x237620 -- see below] Then RVA 0x237620
// (RtlpHpLfhSlotAllocate's own entry) traced the chain up
// to ExAllocateHeapPool+0x2B1, which reads R13 (NULL) from a global array
// at ExPoolState+0x3900 (stride 0x20C0). A follow-up survey found ALL 16
// sampled array indices NULL, not just one -- systemic, not a single bad
// slot. Per explicit user direction: investigate (without changing reset
// behavior) whether the array is never initialized post-reset or
// explicitly cleared without rebuild. RVA 0x3C3B64 is
// `ExInitializePoolHeapManagement`, found via PDB symbol enumeration
// (SymEnumSymbols against patterns like "*Init*Pool*") as the strongest
// name-matched candidate for what populates this array -- no matching
// "*teardown*"/"*destroy*" pool function exists in the public symbol
// table at all, itself a data point. Breaking at its entry (unconditional,
// first-hit-only, like the very first breakpoint site in this
// investigation) checks whether/when it actually runs.
// RETARGETED AGAIN (2026-07-17): full-image static scan (16MB kernel dump
// + capstone, offline) found exactly ONE writer to the ExPoolState array
// anywhere in ntoskrnl.exe -- ExInitializePoolHeapManagement, writing
// +8 -- and NO writer anywhere to +0x10 (the field found NULL). This
// raises a sharper alternative hypothesis: maybe +0x10 isn't a
// reset-broken re-init path at all, but a slot that's simply never
// populated on ANY boot pass until first requested with these specific
// flags -- i.e. not a regression, just the first-ever use of this
// particular flag-selected pool variant happening to land on pass 2.
// Back to RVA 0x237620 (RtlpHpLfhSlotAllocate's entry, already has
// working re-arm emulation from earlier this session) to survey the
// array's +0x10 field repeatedly throughout PASS 1's own extensive,
// successful heap activity -- if it's null there too, this isn't a
// reset-specific gap at all.
// RETARGETED (2026-07-17): RVA 0xA3DC14 is `Phase1InitializationDiscard`'s
// true entry (confirmed via SymFromName), the real system-thread routine
// that runs Phase 1 kernel init on genuine Windows. Testing whether it's
// reached at all on pass 2, one level above both pool-heap initializers.
// 2026-07-19, U18: repurposed to HalpInitSystemHelper+0x5A (the `inc edi`
// immediately after the dispatch call returns and the `js` non-negative
// check). Multi-shot counter: if this fires ~as often as HalpIommuInitSystem
// on pass 2, the inner loop advances past the IOMMU dispatch (cycling);
// if it ~never fires, execution never reaches the increment (frozen at the
// dispatch return -- e.g. an interrupt diverts before it).
// 2026-07-19, U23b: the per-call check at RtlpHpLfhOwnerMoveSubsegment's
// entry was abandoned -- it is a HOT heap function and breakpointing every
// call added so much overhead (exitCount ballooned to ~20M) that the guest
// never reached pass 2. DR2 is parked at an inert address (base+0, the PE
// header, never executed) so it never fires. U25b: crash-state inspection is
// dead (trap-frame GPRs all zero at the int 0x29 fastfail; the subsegment is
// not on the shallow stack). So instead instrument the WRITER path. DR2 ->
// RtlpHpLfhBucketAddSubsegment (0x343DB8), which links a new subsegment into a
// bucket and is called only when a bucket grows (far rarer than the alloc-path
// walk). Pass-2 gated (g_sawReset). Args: rcx=owner, rdx=subsegment, r8=bucket,
// r9=flag. AddSubsegment & SubsegmentFree both fired 0x on pass 2 pre-crash.
// U26 breakthrough: breakpoint RtlpHpLfhOwnerMoveSubsegment+0xE3 (0x34420F),
// the `mov ecx,3` right before `int 0x29`. This offset is reached ONLY via the
// failed LIST_ENTRY-check jne's (+0x4D/+0xAD/+0xB6), so it fires ONLY on the
// corrupt call -- once, zero overhead -- and unlike the trap frame the LIVE
// registers are intact here: entry=rcx (insert check) or rdx (remove check),
// bad neighbor in rax/r11. This finally recovers the corrupted subsegment VA.
// U28: DR2 targets RtlpHpLfhBucketGetSubsegment+0x45 (GETSUB_RVA), where
// rdx=[rbx]=the subsegment being handed to MoveSubsegment. Registers read
// normally at this mid-function point. This site is HOT, so DR2 is NOT armed
// at discovery; it is armed dynamically only in a narrow exitCount window
// before the pass-2 crash (see the windowed-arm block in the main loop) so the
// fatal (already-corrupt) subsegment is captured with bounded overhead.
#define BP_FAIL_SITE_RVA 0x343B49
#define LOOPADV_MAX_HITS 3000

// 2026-07-18 (superseded, kept for history): the previous version of this
// breakpoint set bisected InitBootProcessor's own sequence and found that
// CmInitSystem0 (+0x348) and KeInitSystem (+0x370) are BOTH never reached
// on pass 2, while HalInitSystem's own subtree (+0x312, confirmed via
// HalpApicInitializeIoUnit) IS reached -- bracketing the divergence to a
// 54-byte stretch of InitBootProcessor's own code. Disassembling that
// stretch (disasm_bracket.py, scratchpad) found the answer immediately:
//   +0x312: call HalInitSystem
//   +0x317: test al, al
//   +0x319: je <bailout>      <- skips everything else if HalInitSystem
//                                 returns FALSE, including KeInitializeClock,
//                                 CmInitSystem0, KeInitSystem, MmInitSystem,
//                                 and eventually PsInitSystem/PspInitPhase0.
// The bailout target (RVA 0xA3DA8F, InitBootProcessor+0xA3B) loads
// `ecx = 0x5C` before its first call -- 0x5C is the real NT bugcheck code
// HAL_INITIALIZATION_FAILED, strongly suggesting this path calls
// KeBugCheckEx. That's inconsistent with what's actually observed (pass 2
// runs for tens of thousands of VM exits before eventually bugchecking
// with 0x139, not immediately with 0x5C) -- so whether this branch is
// actually taken on pass 2, and if so why it doesn't immediately
// bugcheck, needs direct confirmation, not just static inference.
//   WP_PHASE1INIT_ENTRY_RVA (repurposed): KeInitializeClock entry
//     (InitBootProcessor's other call in this bracket, +0x338) -- was
//     "status unknown" before; resolved now for completeness.
//   BP_FAIL_SITE_RVA (unchanged): MmInitSystem entry -- kept as the
//     known-negative anchor/consistency check.
//
// 2026-07-18, U12: U10 found HalpIommuInitSystem never succeeds on pass 2
// (see the block below). I7 (docs/investigations/phase0-divergence-summary.md)
// speculates this same retry loop might be the *direct* cause of the
// eventual 0x139 bugcheck -- since HalpIommuInitSystem's own callees
// plausibly allocate pool memory before MmInitSystem has initialized the
// Segment Heap descriptors. First attempt: a one-shot breakpoint at the
// exact faulting instruction (RtlpHpLfhOwnerMoveSubsegment+0xE8, RVA
// 0x344214, confirmed via the KiBugCheckData/EXCEPTION_RECORD scan much
// earlier this investigation) with a deep heuristic stack scan -- this
// DID extend the known allocator chain by one level
// (ExAllocatePoolWithTag, not previously identified), but the *caller* of
// ExAllocatePoolWithTag itself couldn't be reliably distinguished from
// stale stack data using that heuristic method (a plain "any
// canonical-looking qword" scan isn't real unwind-based stack walking).
// U12 traced this to KiSwInterruptDispatch, unrelated to HalpIommuInitSystem
// -- see docs/investigations/phase0-divergence-summary.md (I7 refined) and
// post-vppt-boot-stall.md ("U12 resolved") for the full result.
//
// 2026-07-18, U13: why does HalpIommuInitSystem itself never make
// progress on pass 2? Static disassembly of HalpInitSystemHelper's FULL
// body (not just its one visible `call guard_dispatch_icall` -- earlier
// scans only listed distinct call *instructions*, missing that this one
// sits inside a genuine nested loop) revealed the real structure:
//   outer loop: ebx = ecx_arg .. edx_arg (a range)
//   inner loop: edi = 0..0x15 (21), table = &HalSubComponents, stride 0x10
//     rax = table[outer? no -- fixed at r12][inner].funcptr
//     ecx=outer_index, edx=r15d (current processor number, gs:[0x1a4]),
//     r8=original third arg
//     call guard_dispatch_icall(rax) -- dispatches to the specific
//       component initializer for this (outer, inner) slot
//     if (result < 0) bail out of BOTH loops entirely (single shared
//       failure path, not a per-slot retry)
// HalpIommuInitSystem's own entry immediately tests its first parameter
// (ecx, i.e. the outer loop index) for zero and branches differently if
// so. Given the earlier live trace (docs, U10) showed the SAME dispatch
// target (HalpIommuInitSystem) firing thousands of times with an
// IDENTICAL RSP, but HalpInitSystemHelper's own bailout-on-failure would
// exit the whole function (and thus return control to InitBootProcessor,
// contradicting the already-confirmed fact that it never returns) --
// something doesn't add up between the static structure and the live
// result. Rather than keep guessing from statics, WP_RETURN_RVA and
// HALI_DISPATCH_CALL_RVA are retargeted to directly observe
// HalpIommuInitSystem's own inputs and outputs on every call:
//   WP_RETURN_RVA (repurposed): HalpIommuInitSystem's own entry -- reads
//     RCX/RDX/R8/R9 (its real arguments) on every hit.
//   HALI_DISPATCH_CALL_RVA (repurposed): HalpIommuInitSystem's own return
//     point (RVA 0x9A188F = entry+0x1DF, the `ret` instruction, EAX
//     already holds its real return value at this exact point) -- reads
//     EAX on every hit.
// U34 (2026-07-27): DR0 retargeted from HalpIommuInitSystem entry (that whole
// loop was proven to be normal boot activity, identical on both passes, by U20)
// to ExInitializePoolHeapManagement's entry. U33 proved the ExPoolState pool
// descriptors are never initialized on pass 2; the open question is whether this
// function -- the only writer to that array in the image -- is even CALLED.
//
// RVA verified against ntkrnlmp.pdb via tools/pdbsym.py (whose selftest checks
// five independently-established anchors first), not taken on faith from a
// comment.
//
// This site is NOT subject to the U15 re-arm-lag trap that invalidated earlier
// "0 hits" claims: U33 timed the pass-1 init at ~12000 exits AFTER module
// discovery, and discovery is exactly when breakpoints are armed. So pass 1
// firing is a positive control proving the instrument works, which is what makes
// silence on pass 2 evidence rather than an artifact. One-shot per pass (the
// function runs once), and it walks the stack to name its own callers, giving
// the next rung to instrument if pass 2 turns out never to call it.
// U35 (2026-07-27): moved one rung up the chain. U34 established that
// ExInitializePoolHeapManagement is never called on pass 2, and that its pass-1
// caller is MiInitNucleus+0x44B -- so pool-heap init is reached from the memory
// manager's Phase-0 nucleus init. The question becomes whether MiInitNucleus
// itself runs on pass 2 (making the divergence a truncation INSIDE it, before
// +0x44B) or never runs either (pushing the divergence further up into MM init).
//
// Positive-control caveat, to be checked in the log rather than assumed: on
// pass 1 the pool init fired ~11800 exits after module discovery, and
// MiInitNucleus entry precedes that call site by only a short distance. If
// MiInitNucleus entry turns out to land BEFORE discovery on pass 1, this
// breakpoint loses its control and a pass-2 miss would be uninterpretable --
// the U15 trap. Verify pass 1 fires before drawing any pass-2 conclusion.
// U36 (2026-07-27): chain resolved so far, bottom-up --
//   InitBootProcessor+0x57A -> MmInitSystem+0xD5 -> MiInitNucleus+0x44B
//   -> ExInitializePoolHeapManagement -> ExPoolState descriptors
// U34 and U35 showed neither ExInitializePoolHeapManagement nor MiInitNucleus
// runs on pass 2, both with a working pass-1 positive control.
//
// Skipping MmInitSystem deliberately: it calls MiInitNucleus only 0xD5 bytes in,
// so if MmInitSystem ran at all, MiInitNucleus would almost certainly have been
// reached. InitBootProcessor is the more informative target -- it has 0x57A
// bytes of body before it calls MmInitSystem, i.e. real room to start and then
// truncate. If it fires on pass 2, the divergence is localized INSIDE
// InitBootProcessor; if it does not, the divergence is higher still
// (KiSystemStartup / KiInitializeKernel level).
//
// U36 RESULT (2026-07-27): InitBootProcessor entry is NOT OBSERVABLE this way.
// Armed at 0xA3D054 it fired on NEITHER pass -- including pass 1, which was the
// positive control -- because its entry precedes module discovery (discovery at
// 107795, pool-init edge at 119795). No control means a pass-2 miss carries no
// information, so that run was discarded rather than interpreted. Do not retry
// this site at its entry; anything at or above InitBootProcessor's entry is
// unreachable by discovery-armed breakpoints.
//
// U37: back to MmInitSystem, the rung U36 skipped on the (sound but unverified)
// argument that MiInitNucleus is called only 0xD5 into it. That shortcut was a
// mistake: MmInitSystem is the ONLY rung in this region that is both above
// MiInitNucleus and still testable, because its entry (~113700 on pass 1, per the
// U35 run where MiInitNucleus hit at 113752) lands AFTER discovery. Outcomes:
//   fires on pass 2  -> divergence is inside MmInitSystem's first 0xD5 bytes
//   silent on pass 2 -> divergence is above it, i.e. InitBootProcessor never
//                       reaches its +0x57A call, pushing the question into the
//                       pre-discovery region that needs a different technique
//                       (e.g. a data poll like U33, not a breakpoint).
// U38 (2026-07-28): bisecting inside InitBootProcessor. tools/findcalls.py
// recovered its ordered direct-call map from the kernel dump (file offset == RVA,
// since the dump is a memory image read from the module base):
//     +0x312 -> HalInitSystem
//     +0x348 -> CmInitSystem0
//     +0x370 -> KeInitSystem
//     +0x543 -> ExInitSystem
//     +0x575 -> MmInitSystem        <-- U37 proved this never runs on pass 2
//     +0x5C8 -> ExAllocatePoolWithTag  (first pool use, AFTER MM init)
// The +0x575 call site independently confirms U37's captured return address
// 0xA3D5CE (= +0x575 plus the 5-byte call), so the map is trustworthy.
//
// U18-U20 saw HAL-init activity on pass 2, which suggests InitBootProcessor runs
// there and truncates somewhere in (+0x312, +0x575). That is still only an
// inference, so probe it directly.
//
// CmInitSystem0 chosen because findcalls.py shows it has EXACTLY ONE caller in
// the whole image (InitBootProcessor+0x348) -- unlike HalInitSystem/KeInitSystem/
// ExInitSystem, which are also called from Phase1InitializationDiscard and would
// need the stack walk to disambiguate. It also sits immediately after
// HalInitSystem returns, so it cleanly answers "did InitBootProcessor get past
// HAL init on pass 2?":
//   fires on pass 2  -> yes; truncation is in (+0x348, +0x575)
//   silent on pass 2 -> truncation is in (+0x312, +0x348), i.e. inside or right
//                       after HAL init -- which would put the divergence in the
//                       very HAL code U20 had dismissed as normal.
// As always the pass-1 hit is the control; no pass-1 hit means no conclusion.
// U38 RESULT: CmInitSystem0 fired on pass 1 at 133803 (caller 0xA3D3A1 =
// InitBootProcessor+0x34D, exactly the +0x348 call's return address) and NEVER on
// pass 2. So the truncation window collapses to (+0x312, +0x348): pass 2 does not
// get past HalInitSystem, i.e. HalInitSystem never returns there.
//
// U39: confirm that InitBootProcessor actually REACHES HalInitSystem on pass 2,
// which until now was only inferred from U15-U20 seeing HAL helper activity. That
// inference is weak because findcalls.py shows HalpInitSystemHelper has seven
// callers, so helper traffic does not imply HalInitSystem specifically.
//
// HalpInitSystemPhase0 is the right probe: exactly ONE caller in the image
// (HalInitSystem+0x2D). HalInitSystem is just a phase dispatcher --
//     +0x19 -> HalpInitSystemPhase1  (also called from KiInitializeKernel+0x5C9)
//     +0x25 -> _security_init_cookie
//     +0x2D -> HalpInitSystemPhase0  (unique)
// so a hit here proves HalInitSystem ran, and combined with U38 proves it never
// returned -- localizing the divergence INSIDE HAL phase-0 init. That would
// vindicate U18/U19's "advancing but non-terminating loop" and overturn U20's
// dismissal of that loop as normal activity.
//
// Control risk: this is called at InitBootProcessor+0x312, earlier than any site
// probed so far, and may land before module discovery on pass 1. If pass 1 stays
// silent this run is discarded like U36, not interpreted.
// U44 (2026-07-28): the question U28-U31 never asked. They traced the fatal
// allocation DOWNWARD (ExAllocateHeapPool -> RtlpHpLfhSlotAllocate ->
// GetSubsegment(owner=0) -> MoveSubsegment -> fastfail) but never identified the
// ORIGINATING caller -- the subsystem that asked for memory before any pool
// existed. U42 and U43 exonerated IOAPIC state and interrupt routing, so "who
// allocates" is now the live question.
//
// Breakpoint ExAllocateHeapPool's entry. At the entry, before the prologue,
// [rsp+0x00] is still the true return address and the qwords above it are caller
// frames, so the stack walk names the allocating subsystem.
//
// Timing is favourable, unlike the U36/U39 sites: on pass 2 the allocation happens
// after module discovery and immediately before the bugcheck, so the site is
// armable. Pass 1 hits it too (it is a hot function there), which is a free
// positive control. One-shot per pass -- an exec breakpoint left armed on an
// executed entry re-faults on resume (U21), and on pass 1 this function is far too
// hot to trace repeatedly (U23 ballooned exitCount to ~24M trying).
// U45 (2026-07-28): U44's deep stack walk was unreliable. Static analysis
// refuted its chain: HalpInterruptRegisterLine's only allocation path is
// HalpMmAllocateMemoryInternal -> HalpAllocPhysicalMemory, which serves from the
// loader memory-descriptor list and never touches pool (HalpAllocPhysicalMemoryInternal
// is a leaf that only carves loader descriptors). The HalpAllocPhysicalMemory+0x4B
// and HalpInterruptRegisterLine+0xFB frames U44 reported were stale -- +0xFB is a
// memset return, not an allocation. Only U44's guaranteed [rsp+0x00] survives:
// ExAllocateHeapPool was called by ExAllocatePoolWithTag (its +0x64 is verified to
// sit right after `call ExAllocateHeapPool`).
//
// So walk ONE guaranteed frame higher: break ExAllocatePoolWithTag's ENTRY, where
// [rsp+0x00] is a guaranteed return address naming its true caller. This is the
// reliable "one guaranteed frame per breakpoint" method (as U17/U29 used), not a
// heuristic scan of a deep frame. The deeper walk is still printed but must be
// treated as context only.
// U46b (2026-07-28): decisive timing test. One-shot KiSwInterruptDispatch on BOTH
// passes, logging exitCount vs discovery and whether pool exists yet (ExPoolState
// pooldesc+0x10 populated). Separates the two live explanations:
//   - the software interrupt fires pass-2-specifically early (pass 1 doesn't take
//     it at that point), vs
//   - it's normal early-boot code that runs before pool init in both passes, and
//     pass 1 only survives because its pool init is reached while pass 2's isn't.
// If pass 1 hits this with pooldesc+0x10 already populated, it is the latter; if
// pass 1 never hits it (or hits it with pool absent yet survives), it is the former.
#define WP_RETURN_RVA 0x3DC890                 /* KiSwInterruptDispatch entry (U46b) */
#define U34_SITE_NAME "KiSwInterruptDispatch"  /* label for the DR0 one-shot logging */
// 2026-07-18, U15: full disassembly of HalpInitSystemHelper (disasm_helper_full.py)
// confirmed the loop structure precisely: ebx=ecx_arg (outer index), ebp=edx_arg
// (outer limit), edi=inner index 0..0x15 over HalSubComponents. Every dispatch call
// in a given outer iteration shares the SAME ecx=ebx across all 21 inner slots --
// so observing HalpIommuInitSystem called thousands of times with an IDENTICAL
// rcx=0x9 does NOT distinguish "stuck in one outer iteration, inner loop
// legitimately re-reaching this slot" from "the entire HalpInitSystemHelper
// function is being freshly re-invoked from outside, over and over, each time
// reaching the same slot first" (H6 vs H7 in the docs). Direct discriminator:
// breakpoint HalpInitSystemHelper's OWN entry, multi-shot. If it fires roughly
// once per HalpIommuInitSystem call (1:1), that's H7 (repeated external
// re-invocation). If it fires once (or a small bounded number of times) while
// HalpIommuInitSystem keeps firing for thousands more, that's H6 (single call,
// internal loop genuinely stuck without ever returning to re-enter this function).
// 2026-07-19, U21: U20 established the HAL init loop is normal (identical
// on both passes) and the watchdog "stall" is just HaliHaltSystem (the
// post-bugcheck CPU halt). So DR1 is retargeted from the (useless-on-pass-2)
// HalpInitSystemHelper entry to KeBugCheckEx's entry -- to catch the 0x139
// bugcheck itself at the moment it fires: rcx=bugcheck code, rdx/r8/r9=
// params 1-3, [rsp+0x28]=param 4, [rsp+0x00]=the caller that detected the
// corruption (the real lead).
#define WP_PHASE1INIT_ENTRY_RVA 0x3FD6F0 /* KeBugCheckEx entry (U21) */
#define CRASH_STACK_SCAN_BYTES 0x400     /* deep stack walk, kept for the (now unused) crash-site slot's code path */
#define POOL_CALLER_MAX_HITS 1  /* U34: one-shot -- pool init runs once per boot */
#define HELPER_ENTRY_MAX_HITS 3000

// 2026-07-18, U10: the live check above (WP_RETURN_RVA) falsified the
// "HalInitSystem returns FALSE" hypothesis -- it never returns to its
// caller AT ALL on pass 2. Static disassembly traced why HalpApicInitializeIoUnit
// (the last confirmed-reached point) isn't a simple direct call:
// HalInitSystem -> HalpInitSystemPhase0/Phase1 (a phase dispatcher, like
// PsInitSystem/ExInitSystem) -> both call a shared HalpInitSystemHelper ->
// which makes exactly ONE call, to `guard_dispatch_icall` (Control Flow
// Guard's indirect-call trampoline) -- meaning this is a table-driven
// dispatch loop (standard real-HAL architecture: one function pointer per
// platform sub-init routine), not a direct call chain. This explains why
// the much earlier whole-image static scan (U6's first attempt) found no
// direct references to anything -- CFG-guarded indirect calls don't leave
// a literal `call rel32` or matching RIP-relative load at the call site.
// Per the standard MSVC/CFG x64 ABI, the actual target is loaded into RAX
// immediately before `call guard_dispatch_icall`. A DEDICATED FOURTH
// breakpoint (DR3) traces this: unlike DR0-DR2 (one-shot, disabled after
// firing), DR3 stays armed across repeated hits within the same pass,
// logging each dispatched target's RAX value and resolved symbol -- this
// directly reveals which table entry runs on pass 1 that pass 2 never
// reaches (or hangs inside).
#define HALI_DISPATCH_CALL_RVA 0x9A188F /* HalpIommuInitSystem+0x1DF, its own `ret` (U13, EAX = real return value) */
// First run at a cap of 40 showed the dispatch target repeating
// IDENTICALLY (same RAX, same RSP) many times in a row before the cap
// was hit -- resolved to real HAL subsystem initializers
// (HalpIommuInitSystem, then HalpAcpiInitSystem), confirming this is a
// genuine table-driven loop with a per-subsystem retry/poll pattern, not
// a single stuck instruction. 40 wasn't enough to see either subsystem's
// retry loop actually terminate on pass 1. Raised substantially to
// capture full retry counts on pass 1 (establishing a normal baseline)
// and to give pass 2 maximum room to either match that pattern or hang
// indefinitely on one target.
#define HALI_DISPATCH_MAX_HITS 3000

// Small ring buffer of recent hypervisor-side activity (VM exits,
// interrupt injections, device events), timestamped in milliseconds
// since first use. Appending is just a struct write -- no I/O -- so the
// steady-state cost is negligible; contents are only printed when the
// breakpoint fires.
#define BP_EVENT_RING_SIZE 128
typedef struct {
    double timestampMs;
    char desc[96];
} BpEventEntry;
BpEventEntry g_bpEventRing[BP_EVENT_RING_SIZE];
int g_bpEventRingPos = 0;
int g_bpEventRingCount = 0;
LARGE_INTEGER g_bpEventRingStart = { 0 };

void logBpEvent(const char *fmt, ...) {
    if (g_bpEventRingStart.QuadPart == 0) {
        if (perfFrequency.QuadPart == 0) return; // not yet initialized this early
        QueryPerformanceCounter(&g_bpEventRingStart);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    BpEventEntry *e = &g_bpEventRing[g_bpEventRingPos];
    e->timestampMs = (double)(now.QuadPart - g_bpEventRingStart.QuadPart) * 1000.0 / perfFrequency.QuadPart;
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(e->desc, sizeof(e->desc), _TRUNCATE, fmt, args);
    va_end(args);
    g_bpEventRingPos = (g_bpEventRingPos + 1) % BP_EVENT_RING_SIZE;
    if (g_bpEventRingCount < BP_EVENT_RING_SIZE) g_bpEventRingCount++;
}

void printBpEventRing(void) {
    printf("[bp] last %d hypervisor events before the breakpoint (ms since first logged event):\n", g_bpEventRingCount);
    int startIdx = (g_bpEventRingCount < BP_EVENT_RING_SIZE) ? 0 : g_bpEventRingPos;
    int i;
    for (i = 0; i < g_bpEventRingCount; i++) {
        int idx = (startIdx + i) % BP_EVENT_RING_SIZE;
        printf("  [%9.3f ms] %s\n", g_bpEventRing[idx].timestampMs, g_bpEventRing[idx].desc);
    }
    fflush(stdout);
}

// Breakpoint state. g_bpModuleBase==0 means "not yet located for the
// current boot instance" -- cleared by the port-0x64/0xFE reset handler
// so it's rediscovered fresh after each reset (KASLR gives ntoskrnl.exe a
// new base every boot). Discovery is opportunistic: the main loop checks
// on ordinary I/O port exits (cheap, frequent, already touching live
// RIP/CR3) whether the current RIP looks like a plausible canonical
// kernel address and, if so, tries the same backward MZ/PE scan used
// throughout this investigation.
UINT64 g_bpModuleBase = 0;
UINT64 g_bpTargetVA = 0;
UINT64 g_bpCr3 = 0; // the CR3 active when the patch was written, for correct unpatching later
unsigned char g_bpOriginalByte = 0;
int g_bpPatched = 0;
int g_bpHitCount = 0;
int g_nonBpHitCount = 0; // throttling counter for genuine unrelated INT3s hit while our interception is active

// Hardware data watchpoint state (DR0/DR7) -- see the discovery block for
// why/how this is armed. Re-armed fresh at every module rediscovery
// (including post-reset, since the reset handler zeroes DR7).
// g_wpTargetVA/DR2 = MmInitSystem entry (BP_FAIL_SITE_RVA), known-negative
//   anchor from U8.
// g_wpTargetVA0/DR0 = InitBootProcessor+0x317 (WP_RETURN_RVA), the
//   `test al,al` right after `call HalInitSystem` -- AL here IS
//   HalInitSystem's raw return value.
// g_wpTargetVA1/DR1 = KeInitializeClock entry (WP_PHASE1INIT_ENTRY_RVA).
// All three are armed together and disabled independently (one-shot each)
// as they fire, so a single armed pass can observe all three events.
UINT64 g_wpTargetVA = 0;
UINT64 g_wpTargetVA0 = 0;
UINT64 g_wpTargetVA1 = 0;
int g_wpArmed = 0;
int g_wpHitCount = 0;
int g_wp0Fired = 0;
int g_wp1Fired = 0;
int g_wp2Fired = 0;

// DR3 -- the multi-shot CFG-dispatch tracer (U10, HALI_DISPATCH_CALL_RVA).
// Unlike DR0-DR2 above, this one is NOT one-shot: it stays armed across
// repeated hits within the same pass (a table-driven dispatch loop calls
// through the same site once per table entry), capped at
// HALI_DISPATCH_MAX_HITS to bound worst-case log volume.
UINT64 g_haliDispatchTargetVA = 0;
int g_haliDispatchArmed = 0;
int g_haliDispatchHitCount = 0;

// DR0 (repurposed for U12) -- also multi-shot, same pattern as DR3: traces
// every call to ExAllocatePoolWithTag (WP_RETURN_RVA) by reading the
// guaranteed-real return address at [rsp+0x00] on each hit, capped at
// POOL_CALLER_MAX_HITS. Uses g_wpTargetVA0 (already computed by the
// discovery/arm block) as its target VA; this flag pair just tracks its
// own multi-shot state separately from the one-shot g_wp0Fired semantics
// the generic wpSlot logic used to use for this same DR slot.
int g_poolCallerArmed = 0;
int g_poolCallerHitCount = 0;

// DR1 (repurposed for U15) -- also multi-shot, same pattern as DR0/DR3:
// traces every entry to HalpInitSystemHelper (WP_PHASE1INIT_ENTRY_RVA) by
// reading ecx/edx/r8 (its real arguments: outer index, outer limit, third
// arg) on each hit, capped at HELPER_ENTRY_MAX_HITS. Discriminates H6
// (single call, internal loop stuck) from H7 (repeated external
// re-invocation) -- see the WP_PHASE1INIT_ENTRY_RVA comment block above.
int g_helperEntryArmed = 0;
int g_helperEntryHitCount = 0;

// DR2 (repurposed for U18) -- multi-shot counter at HalpInitSystemHelper+0x5A
// (the inc edi after the dispatch call). See BP_FAIL_SITE_RVA comment.
int g_loopAdvArmed = 0;
int g_loopAdvHitCount = 0;
// U23c: set once the guest's mid-install reset has happened, so the hot
// RtlpHpLfhOwnerMoveSubsegment entry check (DR2) is armed ONLY on pass 2 --
// pass 1 then runs at full speed and only pass 2 bears the per-call overhead
// (which balloons exitCount) up to the crash.
int g_sawReset = 0;
// Set once the guest programs any IOAPIC redirection entry, i.e. it has moved
// from PIC to APIC interrupt routing. Gates the legacy-vector fallback, which is
// correct during firmware but illegal afterwards -- see ioapicResolveVector.
int g_guestApicMode = 0;
long g_legacyVectorDropped = 0;
// U28: exitCount at the pass-2 reset, and whether the windowed GetSubsegment
// capture (DR2 at RtlpHpLfhBucketGetSubsegment+0x45) has been armed yet. That
// site is HOT, so we only arm it in a narrow window before the ~reset+120k
// crash to bound overhead while still containing the fatal call.
long g_resetExitCount = 0;
int g_getsubWindowArmed = 0;
#define GETSUB_WINDOW_START 40000L   /* arm DR2 this many exits after reset */
// U30: chain traced up to ExAllocateHeapPool, where r10 = *(*(rsp+0x58)) is a
// null LFH heap-context. Breakpoint just after that load (0x236C58); when r10
// (the context) is null, capture ptrB=[rsp+0x58] (what structure holds the null
// context pointer) + dump around it -- identifies the true root datum.
#define GETSUB_RVA 0x236C58          /* ExAllocateHeapPool, after r10=*(*(rsp+0x58)) */

// U33 (2026-07-27): U32 proved the init gap (pass 1 populates the ExPoolState
// pool descriptors, pass 2 leaves them zero) but sampled only every 200k exits,
// which is far too coarse to answer the remaining question: is the descriptor
// init NEVER RUN on pass 2, or is it run and then EXPLICITLY CLEARED without a
// rebuild? Those are different bugs with different fixes, and the 2026-07-17
// note ("investigate whether the array is never initialized post-reset or
// explicitly cleared without rebuild") left it open.
//
// Deliberately a DATA instrument, not a breakpoint. A breakpoint on
// ExInitializePoolHeapManagement (RVA 0x3C3B64, the only writer to this array
// in the whole image) would very likely miss on pass 2 by construction:
// breakpoints can only be armed after opportunistic module discovery (200ms
// throttle, requires a canonical kernel RIP), while pool-heap init runs
// extremely early in Phase 0. That is exactly the re-arm-lag trap that produced
// U15's false "0 hits -> different caller" headline, later walked back by U17.
// Polling guest memory needs no arming and cannot be outrun, so it is valid
// from the first exit of each pass.
//
// EDGE-TRIGGERED: poll often but log only on CHANGE, so granularity can be
// tight without flooding the log. A null->nonnull edge dates the init; a
// nonnull->null edge would prove the clear-without-rebuild variant instead.
#define U33_POLL_INTERVAL 2000L   /* exits between descriptor polls */
#define U33_POOLS 4               /* pool types 0..3 */
#define U33_MAX_EDGES 400         /* bound the log if something oscillates */
#define EXPOOLSTATE_DESC_RVA 0xC57EC0ULL /* ExPoolState+0x3900, descriptor[0] */
#define EXPOOLSTATE_DESC_STRIDE 0x20C0ULL

// U27/U33: the kernel-image dump used to go to a session-scoped agent scratchpad
// directory, which has now been purged TWICE (noted in U27, again on 2026-07-27),
// silently breaking re-capture because fopen() on the dead path just fails. Use a
// stable location outside the project instead: *.bin is gitignored so it would
// never reach Git anyway, and keeping it out of the project also keeps the 16MB
// blob out of every SD-card robocopy sync.
#define KDUMP_DIR  "C:\\LocalHost-evidence"
#define KDUMP_PATH KDUMP_DIR "\\ntoskrnl_dump.bin"

// U40 (2026-07-28): arming-free RIP sampler.
//
// U36 and U39 established a hard floor on the breakpoint technique: on pass 1 the
// sites at and below HalInitSystem's entry execute BEFORE module discovery, so
// they can never be armed in time, and a pass-2 miss there carries no information.
// U38 narrowed the pass-2 truncation to InitBootProcessor (+0x312, +0x348) -- i.e.
// inside HalInitSystem -- but confirming pass 2 even REACHES HalInitSystem needs an
// instrument that requires no arming at all.
//
// VpContext.Rip is already read every iteration of the run loop, so sampling it is
// free and valid from the first exit of a pass, exactly like U33's memory poll was.
// Bucket samples by 4KB page into a small open-addressed histogram (one per pass)
// and keep a ring of recent samples. Dumped on reset, on the bugcheck, and
// periodically. Raw pages are printed alongside pass-relative RVAs so the
// pre-discovery samples (taken before any base is known) stay usable: resolve them
// later with tools/pdbsym.py.
// Pass 2's kernel turned out to execute only ~3200 exits before the bugcheck, so
// the pass-1-calibrated 100-exit interval yields barely 30 kernel samples there.
// Sample 10x denser once past the reset, and keep a longer ring, to resolve the
// actual pass-2 kernel trace rather than just its endpoint.
#define U40_SAMPLE_INTERVAL 100L   /* exits between RIP samples (pass 1) */
#define U40_SAMPLE_INTERVAL_P2 10L /* denser once past the reset */
#define U40_BUCKETS 1024           /* power of two, open addressing */
#define U40_RING 256               /* recent samples kept verbatim */
#define U40_TOPN 18                /* hottest pages reported per dump */
#define U40_DUMP_INTERVAL 250000L  /* periodic dump cadence */

typedef struct { UINT64 page; long count; } U40Bucket;
static U40Bucket g_ripHist[2][U40_BUCKETS];
static UINT64 g_ripRing[2][U40_RING];
static long g_ripRingEC[2][U40_RING];
static int g_ripRingPos[2];
static long g_ripSamples[2];
static UINT64 g_passModuleBase[2];   /* base discovered during each pass, for RVAs */

static void u40Sample(UINT64 rip, int pass, long ec) {
    UINT64 page = rip >> 12;
    unsigned int h = (unsigned int)((page * 0x9E3779B1ULL) & (U40_BUCKETS - 1));
    unsigned int i;
    g_ripSamples[pass]++;
    g_ripRing[pass][g_ripRingPos[pass]] = rip;
    g_ripRingEC[pass][g_ripRingPos[pass]] = ec;
    g_ripRingPos[pass] = (g_ripRingPos[pass] + 1) % U40_RING;
    for (i = 0; i < U40_BUCKETS; i++) {
        unsigned int slot = (h + i) & (U40_BUCKETS - 1);
        if (g_ripHist[pass][slot].count == 0) {       /* empty -> claim it */
            g_ripHist[pass][slot].page = page;
            g_ripHist[pass][slot].count = 1;
            return;
        }
        if (g_ripHist[pass][slot].page == page) {
            g_ripHist[pass][slot].count++;
            return;
        }
    }
    /* Table full: drop the sample rather than evict, so counts stay truthful. */
}

// Clears a pass's histogram so the next dump covers only what happened since.
// Used to bracket the boot image's execution: reset when BDS says it is starting
// one, dump when BDS says it failed, and the profile in between is that image
// alone rather than the whole firmware phase averaged together.
static void u40Reset(int pass) {
    memset(g_ripHist[pass], 0, sizeof(g_ripHist[pass]));
    memset(g_ripRing[pass], 0, sizeof(g_ripRing[pass]));
    memset(g_ripRingEC[pass], 0, sizeof(g_ripRingEC[pass]));
    g_ripRingPos[pass] = 0;
    g_ripSamples[pass] = 0;
}

// Names the PE image a guest address falls inside, by walking back to its
// MZ/PE header and reading the PDB path out of the CodeView debug record.
//
// This is what makes the RIP histogram usable during the firmware phase. The
// addresses there are all in the same range -- OVMF's own DXE code and every
// image it loads live in the top of low RAM -- so "0x4F1A7000 is hot" cannot be
// read as firmware or as the booted image without this. UEFI identity-maps, so
// a guest virtual address indexes guest RAM directly.
//
// Returns 1 and fills name[] if an image was identified.
extern void *guestMemory;      // defined with the rest of the VM state further down
extern SIZE_T guestMemSize;
static int u40IdentifyImage(UINT64 addr, UINT64 *outBase, UINT32 *outSize,
                            char *name, size_t nameLen) {
    UINT64 page;
    if (!guestMemory || addr >= guestMemSize) return 0;
    name[0] = '\0';
    // Images are page-aligned; walk back a bounded distance rather than the
    // whole of RAM so a miss costs nothing.
    for (page = addr & ~0xFFFULL; page + 0x1000 <= guestMemSize; page -= 0x1000) {
        const unsigned char *p = (const unsigned char *)guestMemory + page;
        UINT32 lfanew, sig;
        if (p[0] != 'M' || p[1] != 'Z') { if (page < 0x1000 || (addr - page) > 0x4000000ULL) break; continue; }
        lfanew = *(const UINT32 *)(p + 0x3C);
        if (lfanew < 0x40 || page + lfanew + 0x108 > guestMemSize) { if (page == 0) break; continue; }
        sig = *(const UINT32 *)(p + lfanew);
        if (sig != 0x00004550) { if (page == 0) break; continue; }   // "PE\0\0"
        {
            const unsigned char *opt = p + lfanew + 0x18;
            UINT16 magic = *(const UINT16 *)opt;
            UINT32 sizeOfImage = *(const UINT32 *)(opt + 0x38);
            UINT32 ddOff = (magic == 0x20B) ? 0x70 : 0x60;           // PE32+ vs PE32
            UINT32 dbgRva, dbgSize;
            if (addr - page >= sizeOfImage) { if (page == 0) break; continue; }
            *outBase = page;
            // SizeOfImage separates a small OVMF DXE driver (tens of KB) from a
            // loaded boot application like bootmgfw.efi (megabytes), which is the
            // whole question this instrument exists to answer.
            *outSize = sizeOfImage;
            dbgRva  = *(const UINT32 *)(opt + ddOff + 6 * 8);        // debug dir = index 6
            dbgSize = *(const UINT32 *)(opt + ddOff + 6 * 8 + 4);
            if (dbgRva && dbgSize >= 28 && page + dbgRva + 28 <= guestMemSize) {
                const unsigned char *dd = (const unsigned char *)guestMemory + page + dbgRva;
                UINT32 type = *(const UINT32 *)(dd + 12);
                UINT32 raw  = *(const UINT32 *)(dd + 20);            // AddressOfRawData (RVA)
                if (type == 2 && raw && page + raw + 24 < guestMemSize) {
                    const unsigned char *cv = (const unsigned char *)guestMemory + page + raw;
                    if (*(const UINT32 *)cv == 0x53445352) {         // "RSDS"
                        const char *pdb = (const char *)(cv + 24);
                        size_t i = 0;
                        while (i < nameLen - 1 && pdb[i] && (UINT64)(page + raw + 24 + i) < guestMemSize) {
                            name[i] = pdb[i]; i++;
                        }
                        name[i] = '\0';
                    }
                }
            }
            return 1;
        }
    }
    return 0;
}

// Sweeps guest RAM for LARGE PE images -- the ones a boot application would be,
// as opposed to the 12-28KB DXE drivers the firmware is made of.
//
// This answers a question the RIP profile cannot: that profile samples on VM
// EXITS, so an image that runs without touching I/O is invisible to it, and
// "no samples in a large image" does not mean no large image ran. Whether
// LoadImage succeeded is visible in memory regardless of what executed --
// bootmgfw.efi is ~1.6MB, so if it was loaded it is sitting there to be found.
// Set by u40ScanLargeImages when it finds a boot application (>=1MB): where it
// was loaded and where its entry point is, so execution can be trapped there.
UINT64 g_bootImgBase = 0, g_bootImgEntry = 0;
UINT32 g_bootImgSize = 0;
int g_bootImgBpArmed = 0, g_bootImgBpHits = 0;
UINT64 g_bootImgRetAddr = 0;   // where the image returns to; DR3 moves here
long g_bootImgEntryExit = 0;   // exit count at entry, to measure how long it ran
// Set while the boot application is executing -- between its entry point and its
// return. Used to log ONLY what it touches: 2960 exits is small enough to read in
// full, where logging the whole boot is not.
int g_bootImgRunning = 0;
long g_bootImgIoLogged = 0;
long g_bootImgReadLogged = 0;
long g_ahciShortReads = 0;
// Transfers that moved the wrong number of bytes -- the signature of silent data
// corruption (Windows Setup 0x80070570). See the checks in ahciServicePort.
long g_ahciBadWrites = 0, g_ahciBadReads = 0;
long g_bootImgCmosLogged = 0;
long g_bootImgMmioLogged = 0;
long g_bootImgWriteLogged = 0;
// UNCAPPED per-port tally for the boot application's window. The logged trace
// above stops at 400 entries, which only shows the START of a ~3000-exit window
// -- and reading the beginning of a truncated log has already produced one wrong
// conclusion in this investigation. Counting every access costs an increment.
#define BOOTIMG_PORTS_MAX 24
UINT16 g_bootImgPortNum[BOOTIMG_PORTS_MAX];
long   g_bootImgPortCount[BOOTIMG_PORTS_MAX];
int    g_bootImgPortsSeen = 0;
long   g_bootImgIoTotal = 0, g_bootImgMmioTotal = 0;

UINT64 g_bootImgMmioPage[BOOTIMG_PORTS_MAX];
long   g_bootImgMmioCount[BOOTIMG_PORTS_MAX];
int    g_bootImgMmioSeen = 0;

// A RING of the boot application's most recent exits, dumped when it returns.
//
// Every other instrument aimed at this window is capped from its START -- the
// I/O trace at 400 entries, [ahci-svc] at 40, the AHCI command log at its own
// max -- so all of them show bootmgfw.efi's first moments, over and over, and
// not one has ever shown its last. But the status it returns is decided at the
// END of the window. That is the only part that matters and the only part never
// captured, and raising a cap does not fix it: the head is not where the answer
// is. A ring keeps the tail instead, at a fixed cost, however long the window
// turns out to be.
//
// Recorded for EVERY exit, not just the interesting-looking ones, because
// "which exits are interesting" is precisely what is not yet known here.
// Sized to hold the WHOLE window (~2500 exits) rather than a tail of it. The
// window turned out to be small enough to read end to end, and every previous
// cap in this investigation has hidden the part that mattered.
#define BOOTIMG_RING_MAX 4096
typedef struct {
    long   exitIndex;
    UINT32 reason;
    UINT64 rip;
    UINT64 addr;    // I/O port number, or MMIO guest-physical address
    UINT64 value;   // RAX for port I/O
    UINT8  isWrite;
    UINT8  size;
    UINT8  kind;    // 0 = other, 1 = port I/O, 2 = MMIO
} BootImgRingEntry;
BootImgRingEntry g_bootImgRing[BOOTIMG_RING_MAX];
long g_bootImgRingCount = 0;  // total recorded; slot = count % BOOTIMG_RING_MAX

static const char *exitReasonName(UINT32 r) {
    switch (r) {
        case WHvRunVpExitReasonMemoryAccess:     return "MemoryAccess";
        case WHvRunVpExitReasonX64IoPortAccess:  return "IoPort";
        case WHvRunVpExitReasonX64Halt:          return "Halt";
        case WHvRunVpExitReasonX64Cpuid:         return "Cpuid";
        case WHvRunVpExitReasonException:        return "Exception";
        case WHvRunVpExitReasonCanceled:         return "Canceled";
        default:                                 return "other";
    }
}

static void bootImgRingRecord(WHV_RUN_VP_EXIT_CONTEXT *ec, long exitIndex) {
    BootImgRingEntry *e = &g_bootImgRing[g_bootImgRingCount % BOOTIMG_RING_MAX];
    memset(e, 0, sizeof(*e));
    e->exitIndex = exitIndex;
    e->reason = (UINT32)ec->ExitReason;
    e->rip = ec->VpContext.Rip;
    if (ec->ExitReason == WHvRunVpExitReasonX64IoPortAccess) {
        e->kind = 1;
        e->addr = ec->IoPortAccess.PortNumber;
        e->value = ec->IoPortAccess.Rax;
        e->isWrite = (UINT8)ec->IoPortAccess.AccessInfo.IsWrite;
        e->size = (UINT8)ec->IoPortAccess.AccessInfo.AccessSize;
    } else if (ec->ExitReason == WHvRunVpExitReasonMemoryAccess) {
        e->kind = 2;
        e->addr = ec->MemoryAccess.Gpa;
        e->isWrite = (ec->MemoryAccess.AccessInfo.AccessType == WHvMemoryAccessWrite) ? 1 : 0;
    }
    g_bootImgRingCount++;
}

static void bootImgRingDump(long lines) {
    long total = g_bootImgRingCount;
    long have  = total < BOOTIMG_RING_MAX ? total : BOOTIMG_RING_MAX;
    long show  = have < lines ? have : lines;
    long start = total - show;
    long i;
    printf("[bootimg-ring] the LAST %ld of %ld exits, ending at the return:\n", show, total);
    for (i = start; i < total; i++) {
        BootImgRingEntry *e = &g_bootImgRing[i % BOOTIMG_RING_MAX];
        if (e->kind == 1) {
            printf("[bootimg-ring] %6ld rip=0x%08llX  %s port=0x%03llX size=%u rax=0x%llX\n",
                   e->exitIndex, (unsigned long long)e->rip,
                   e->isWrite ? "OUT" : "IN ", (unsigned long long)e->addr,
                   e->size, (unsigned long long)e->value);
        } else if (e->kind == 2) {
            printf("[bootimg-ring] %6ld rip=0x%08llX  %s mmio gpa=0x%llX\n",
                   e->exitIndex, (unsigned long long)e->rip,
                   e->isWrite ? "W  " : "R  ", (unsigned long long)e->addr);
        } else {
            printf("[bootimg-ring] %6ld rip=0x%08llX  %s (reason %u)\n",
                   e->exitIndex, (unsigned long long)e->rip,
                   exitReasonName(e->reason), e->reason);
        }
    }
    fflush(stdout);
}

// MMIO is counted by 4KB page, which is the granularity that distinguishes one
// device's register block from another's.
static void bootImgCountMmio(UINT64 gpa) {
    UINT64 page = gpa & ~0xFFFULL;
    int i;
    g_bootImgMmioTotal++;
    for (i = 0; i < g_bootImgMmioSeen; i++) {
        if (g_bootImgMmioPage[i] == page) { g_bootImgMmioCount[i]++; return; }
    }
    if (g_bootImgMmioSeen < BOOTIMG_PORTS_MAX) {
        g_bootImgMmioPage[g_bootImgMmioSeen] = page;
        g_bootImgMmioCount[g_bootImgMmioSeen] = 1;
        g_bootImgMmioSeen++;
    }
}

static void bootImgCountPort(UINT16 port) {
    int i;
    g_bootImgIoTotal++;
    for (i = 0; i < g_bootImgPortsSeen; i++) {
        if (g_bootImgPortNum[i] == port) { g_bootImgPortCount[i]++; return; }
    }
    if (g_bootImgPortsSeen < BOOTIMG_PORTS_MAX) {
        g_bootImgPortNum[g_bootImgPortsSeen] = port;
        g_bootImgPortCount[g_bootImgPortsSeen] = 1;
        g_bootImgPortsSeen++;
    }
}
#define BOOTIMG_IO_LOG_MAX 400

// EFI_STATUS names, so a returned code reads as itself instead of as a constant
// nobody remembers. The high bit marks an error; the low bits are the code.
static const char *efiStatusName(UINT64 s) {
    static const char *errs[] = {
        "SUCCESS", "LOAD_ERROR", "INVALID_PARAMETER", "UNSUPPORTED",
        "BAD_BUFFER_SIZE", "BUFFER_TOO_SMALL", "NOT_READY", "DEVICE_ERROR",
        "WRITE_PROTECTED", "OUT_OF_RESOURCES", "VOLUME_CORRUPTED", "VOLUME_FULL",
        "NO_MEDIA", "MEDIA_CHANGED", "NOT_FOUND", "ACCESS_DENIED",
        "NO_RESPONSE", "NO_MAPPING", "TIMEOUT", "NOT_STARTED",
        "ALREADY_STARTED", "ABORTED", "ICMP_ERROR", "TFTP_ERROR",
        "PROTOCOL_ERROR", "INCOMPATIBLE_VERSION", "SECURITY_VIOLATION", "CRC_ERROR",
        "END_OF_MEDIA", "", "", "END_OF_FILE", "INVALID_LANGUAGE",
        "COMPROMISED_DATA", "IP_ADDRESS_CONFLICT", "HTTP_ERROR"
    };
    UINT64 code = s & ~(1ULL << 63);
    if (s == 0) return "EFI_SUCCESS";
    if (!(s >> 63)) return "(warning or non-error value)";
    if (code < sizeof(errs) / sizeof(errs[0]) && errs[code][0]) return errs[code];
    return "(unknown EFI_STATUS)";
}

static void u40ScanLargeImages(const char *why) {
    UINT64 page;
    int found = 0;
    if (!guestMemory || guestMemSize < 0x1000) return;
    printf("[u40-img] large PE images in guest RAM (%s):\n", why);
    for (page = 0; page + 0x1000 <= guestMemSize; page += 0x1000) {
        const unsigned char *p = (const unsigned char *)guestMemory + page;
        UINT32 lfanew, sizeOfImage, entry;
        const unsigned char *opt;
        UINT16 magic;
        if (p[0] != 'M' || p[1] != 'Z') continue;
        lfanew = *(const UINT32 *)(p + 0x3C);
        if (lfanew < 0x40 || lfanew > 0x1000 || page + lfanew + 0x108 > guestMemSize) continue;
        if (*(const UINT32 *)(p + lfanew) != 0x00004550) continue;   // "PE\0\0"
        opt = p + lfanew + 0x18;
        magic = *(const UINT16 *)opt;
        if (magic != 0x10B && magic != 0x20B) continue;
        sizeOfImage = *(const UINT32 *)(opt + 0x38);
        entry       = *(const UINT32 *)(opt + 0x10);
        // 256KB filter: every firmware module here is under 64KB, so anything
        // this large is a loaded application rather than part of OVMF.
        if (sizeOfImage < 256 * 1024 || sizeOfImage > 64 * 1024 * 1024) continue;
        if (page + sizeOfImage > guestMemSize) continue;
        found++;
        printf("[u40-img]   base=0x%llX size=%uKB entryPoint=0x%llX (rva 0x%X) machine=%s\n",
               (unsigned long long)page, sizeOfImage / 1024,
               (unsigned long long)(page + entry), entry,
               magic == 0x20B ? "x64" : "x86");
        // The boot application, as opposed to the firmware's own big modules:
        // everything OVMF loads for itself here is under 1MB, and bootmgfw.efi
        // is 1404KB. Remembered so its entry point can be trapped.
        if (sizeOfImage >= 1024 * 1024 && sizeOfImage > g_bootImgSize) {
            g_bootImgBase = page;
            g_bootImgEntry = page + entry;
            g_bootImgSize = sizeOfImage;
        }
        if (found >= 12) break;
    }
    if (!found) printf("[u40-img]   none found -- no boot application is resident\n");
    fflush(stdout);
}

static void u40Dump(int pass, const char *why) {
    UINT64 base = g_passModuleBase[pass];
    int shown, k;
    printf("[u40] === RIP histogram pass %d (%s): %ld samples, base=0x%llX ===\n",
           pass + 1, why, g_ripSamples[pass], (unsigned long long)base);
    if (g_ripSamples[pass] == 0) { fflush(stdout); return; }
    for (shown = 0; shown < U40_TOPN; shown++) {
        int best = -1; long bestCount = 0;
        for (k = 0; k < U40_BUCKETS; k++) {
            if (g_ripHist[pass][k].count > bestCount) { bestCount = g_ripHist[pass][k].count; best = k; }
        }
        if (best < 0) break;
        UINT64 pg = g_ripHist[pass][best].page;
        UINT64 va = pg << 12;
        printf("[u40]   %6ld  page 0x%llX", bestCount, (unsigned long long)va);
        if (base && va >= base && va - base < 0x2000000ULL)
            printf("  = base+0x%llX", (unsigned long long)(va - base));
        // Which PE image is this inside? Without it the firmware's own code and
        // the image it booted are indistinguishable -- they share an address range.
        {
            UINT64 imgBase = 0;
            UINT32 imgSize = 0;
            char imgName[128];
            if (u40IdentifyImage(va, &imgBase, &imgSize, imgName, sizeof(imgName))) {
                printf("  [img 0x%llX+0x%llX size=%uKB", (unsigned long long)imgBase,
                       (unsigned long long)(va - imgBase), imgSize / 1024);
                if (imgName[0]) {
                    const char *leaf = imgName, *s;
                    for (s = imgName; *s; s++) if (*s == '\\' || *s == '/') leaf = s + 1;
                    printf(" %s", leaf);
                }
                printf("]");
                // Sixteen bytes of the image at this page. A RELEASE OVMF strips
                // the CodeView record, so there is no name to read -- but the FV
                // still holds every module's PE32, and these bytes identify which
                // one this is by matching at the same RVA. Twenty different 16KB
                // drivers ship in this firmware, so size alone settles nothing.
                if (guestMemory && va + 16 <= guestMemSize) {
                    const unsigned char *b = (const unsigned char *)guestMemory + va;
                    int bi;
                    printf(" rva=0x%llX bytes=", (unsigned long long)(va - imgBase));
                    for (bi = 0; bi < 16; bi++) printf("%02X", b[bi]);
                }
            }
        }
        printf("\n");
        g_ripHist[pass][best].count = -bestCount;   /* mark as reported */
    }
    for (k = 0; k < U40_BUCKETS; k++)               /* restore counts */
        if (g_ripHist[pass][k].count < 0) g_ripHist[pass][k].count = -g_ripHist[pass][k].count;
    printf("[u40]   most recent %d samples (newest last):\n[u40]    ", U40_RING);
    for (k = 0; k < U40_RING; k++) {
        int idx = (g_ripRingPos[pass] + k) % U40_RING;
        if (g_ripRing[pass][idx])
            printf(" %llX@%ld", (unsigned long long)g_ripRing[pass][idx], g_ripRingEC[pass][idx]);
    }
    printf("\n");
    fflush(stdout);
}

// U46: dump the WHP-emulated local APIC state. The LAPIC lives in WHP (we set
// WHvX64LocalApicEmulationModeXApic), and our port-0x64/0xFE reset touches only
// CPU registers via WHvSetVirtualProcessorRegisters -- it never clears the LAPIC.
// So whatever pass 1 leaves pending (an IRR bit, an in-service vector, an armed
// periodic timer) survives into pass 2. U45 showed pass 2 takes an early software
// interrupt into KiSwInterrupt that allocates pool before pool exists; a surviving
// pending interrupt or armed APIC timer is the prime suspect. This reads the raw
// xAPIC register page and reports the vectors that are set plus the timer state.
//
// WHvGetVirtualProcessorInterruptControllerState returns the xAPIC register file
// (register at MMIO offset X is at byte offset X): TPR@0x80, PPR@0xA0, SVR@0xF0,
// ISR@0x100..0x170, IRR@0x200..0x270, LVT Timer@0x320, InitialCount@0x380,
// CurrentCount@0x390, DivideConfig@0x3E0.
static void u46DumpLapic(WHV_PARTITION_HANDLE partition, const char *why) {
    unsigned char apic[4096] = { 0 };
    UINT32 written = 0;
    HRESULT hr = WHvGetVirtualProcessorInterruptControllerState(partition, 0, apic, sizeof(apic), &written);
    if (FAILED(hr)) {
        printf("[u46] LAPIC read failed (%s): HRESULT=0x%lx\n", why, hr);
        fflush(stdout);
        return;
    }
    UINT32 tpr = *(UINT32 *)&apic[0x80], ppr = *(UINT32 *)&apic[0xA0];
    UINT32 svr = *(UINT32 *)&apic[0xF0];
    UINT32 lvtTimer = *(UINT32 *)&apic[0x320];
    UINT32 initCnt = *(UINT32 *)&apic[0x380], curCnt = *(UINT32 *)&apic[0x390];
    UINT32 divCfg = *(UINT32 *)&apic[0x3E0];
    printf("[u46] LAPIC state (%s, %u bytes): TPR=0x%X PPR=0x%X SVR=0x%X\n",
           why, written, tpr, ppr, svr);
    printf("[u46]   LVT Timer=0x%X (vector=0x%02X %s masked=%d) InitCnt=0x%X CurCnt=0x%X Div=0x%X\n",
           lvtTimer, lvtTimer & 0xFF,
           (lvtTimer & 0x20000) ? "PERIODIC" : "one-shot",
           (lvtTimer & 0x10000) ? 1 : 0, initCnt, curCnt, divCfg);
    // ISR/IRR: 8 dwords each, one bit per vector. Report set vectors.
    int reg, bit, any;
    for (reg = 0, any = 0; reg < 8; reg++) {
        UINT32 w = *(UINT32 *)&apic[0x100 + reg * 0x10];
        for (bit = 0; bit < 32; bit++) if (w & (1u << bit)) { printf("%s0x%02X", any++ ? "," : "[u46]   ISR set vectors: ", reg * 32 + bit); }
    }
    if (any) printf("\n");
    for (reg = 0, any = 0; reg < 8; reg++) {
        UINT32 w = *(UINT32 *)&apic[0x200 + reg * 0x10];
        for (bit = 0; bit < 32; bit++) if (w & (1u << bit)) { printf("%s0x%02X", any++ ? "," : "[u46]   IRR set (pending) vectors: ", reg * 32 + bit); }
    }
    if (any) printf("\n");
    if (!any) printf("[u46]   IRR: no pending vectors\n");
    fflush(stdout);
}

// U47 (2026-07-28): FIX TEST. U46/U46b established that our port-0x64/0xFE reset
// leaves the WHP LAPIC warm (TPR=0xF0, APIC enabled, LVT timer armed at vector
// 0xFD periodic), and pass 2 then takes an early software interrupt into
// KiSwInterrupt whose handler allocates NonPagedPool before pool init has run ->
// 0x139. A real hardware reset quiesces the local APIC. Do the same here:
// read-modify-write the xAPIC page to mask every LVT, stop and zero the timer,
// clear all pending/in-service/trigger state, and drop TPR to 0. This removes the
// inherited armed timer -- the identified trigger -- without fully disabling the
// APIC (SVR left as-is), which is the conservative first cut. Gated so it is easy
// to toggle for A/B comparison.
#define U47_RESET_LAPIC 1
static void u47ResetLapic(WHV_PARTITION_HANDLE partition) {
    unsigned char apic[4096] = { 0 };
    UINT32 written = 0;
    HRESULT hr = WHvGetVirtualProcessorInterruptControllerState(partition, 0, apic, sizeof(apic), &written);
    if (FAILED(hr)) { printf("[u47] LAPIC read failed: 0x%lx -- not resetting\n", hr); fflush(stdout); return; }
    *(UINT32 *)&apic[0x80] = 0;          // TPR = 0
    // Mask every LVT (bit 16 set, vector 0) = cold-boot 0x10000.
    UINT32 lvtOffs[] = { 0x2F0, 0x320, 0x330, 0x340, 0x350, 0x360, 0x370 };
    unsigned int li;
    for (li = 0; li < sizeof(lvtOffs) / sizeof(lvtOffs[0]); li++)
        *(UINT32 *)&apic[lvtOffs[li]] = 0x10000;
    *(UINT32 *)&apic[0x380] = 0;          // Timer Initial Count = 0 (stops it)
    *(UINT32 *)&apic[0x390] = 0;          // Timer Current Count = 0
    *(UINT32 *)&apic[0x300] = 0;          // ICR low
    *(UINT32 *)&apic[0x310] = 0;          // ICR high
    // Clear ISR (0x100), TMR (0x180), IRR (0x200): 8 dwords each, 0x10 stride.
    int r;
    for (r = 0; r < 8; r++) {
        *(UINT32 *)&apic[0x100 + r * 0x10] = 0;
        *(UINT32 *)&apic[0x180 + r * 0x10] = 0;
        *(UINT32 *)&apic[0x200 + r * 0x10] = 0;
    }
    hr = WHvSetVirtualProcessorInterruptControllerState(partition, 0, apic, sizeof(apic));
    if (FAILED(hr)) { printf("[u47] LAPIC write failed: 0x%lx\n", hr); fflush(stdout); return; }
    printf("[u47] LAPIC quiesced at reset (LVTs masked, timer stopped, pending/ISR/IRR cleared, TPR=0)\n");
    fflush(stdout);
}

// U49: verify the guest actually received our DSDT. Embedding a real 522-byte
// AML table did NOT change the 0xA5 (identical p1=0x11 p2=0x3, 19/19 boots, and
// byte-identical AHCI activity to the previous build) -- which is exactly what
// you would also see if the table never reached the guest at all. Rather than
// assume, scan guest physical RAM for ACPI table signatures and report what is
// really there: our DSDT carries OEM ID "LCLHST", so it is unambiguous.
// One-shot, bounded, and only a host-side memory scan (ACPI tables live in guest
// physical RAM, so no page-table walk is needed).
// U53: OFF by default. This scan walks all 3GB of guest RAM once per signature
// plus another full pass for the XSDT walk -- billions of iterations -- and it
// runs on the main loop, so the vCPU is not executing while it works. Measured
// cost: a ~26 second freeze at module discovery, which the watchdog then reported
// as a guest STALL (RIP and exitCount both frozen at 126854) and which I very
// nearly mistook for a guest hang. Same lesson as the DR3 breakpoint in U40: an
// instrument that perturbs the thing it measures. Set to 1 only when the ACPI
// table layout in guest memory is actually in question.
#define U53_SCAN_ACPI_TABLES 0
static void u49VerifyAcpiTables(unsigned char *mem, SIZE_T memSize) {
    static int done = 0;
    if (done || !mem || !U53_SCAN_ACPI_TABLES) return;
    done = 1;
    const char *sigs[] = { "DSDT", "FACP", "APIC", "XSDT", "DBG2" };
    int si;
    printf("[u49] scanning %llu MB of guest RAM for ACPI tables...\n",
           (unsigned long long)(memSize / (1024 * 1024)));
    for (si = 0; si < 5; si++) {
        SIZE_T off; int found = 0;
        for (off = 0; off + 36 < memSize && found < 3; off += 4) {  /* tables are 4-byte aligned */
            if (memcmp(mem + off, sigs[si], 4) != 0) continue;
            UINT32 len = *(UINT32 *)(mem + off + 4);
            if (len < 36 || len > 0x10000) continue;               /* implausible -- not a real header */
            char oem[7] = { 0 };
            memcpy(oem, mem + off + 10, 6);
            unsigned char sum = 0; UINT32 k;
            for (k = 0; k < len && off + k < memSize; k++) sum = (unsigned char)(sum + mem[off + k]);
            printf("[u49]   %s at GPA 0x%llX len=%u rev=%u OEM='%s' checksum=%s\n",
                   sigs[si], (unsigned long long)off, len, mem[off + 8], oem,
                   (sum == 0) ? "OK" : "BAD");
            found++;
        }
        if (!found) printf("[u49]   %s: NOT FOUND in guest RAM\n", sigs[si]);
    }
    // U51: acpi.sys returns STATUS_ACPI_INVALID_TABLE (0xC0140019) from
    // ACPILoadProcessRSDT specifically because it enumerated the root table and
    // never found a FADT ('FACP'). So print what each XSDT actually POINTS AT --
    // finding a valid FADT lying in RAM proves nothing if the XSDT the OS follows
    // does not reference it.
    {
        SIZE_T off;
        for (off = 0; off + 36 < memSize; off += 4) {
            if (memcmp(mem + off, "XSDT", 4) != 0) continue;
            UINT32 len = *(UINT32 *)(mem + off + 4);
            if (len < 36 || len > 0x1000) continue;
            UINT32 n = (len - 36) / 8, e;
            printf("[u51] XSDT at GPA 0x%llX: %u entries\n", (unsigned long long)off, n);
            for (e = 0; e < n; e++) {
                UINT64 ptr = *(UINT64 *)(mem + off + 36 + e * 8);
                if (ptr < memSize && ptr + 36 < memSize) {
                    char s[5] = { 0 };
                    memcpy(s, mem + ptr, 4);
                    UINT32 tlen = *(UINT32 *)(mem + ptr + 4);
                    unsigned char trev = mem[ptr + 8], tsum = 0; UINT32 q;
                    for (q = 0; q < tlen && ptr + q < memSize; q++) tsum = (unsigned char)(tsum + mem[ptr + q]);
                    printf("[u51]     [%u] -> GPA 0x%llX  '%s' len=%u rev=%u checksum=%s%s\n",
                           e, (unsigned long long)ptr, s, tlen, trev, (tsum == 0) ? "OK" : "BAD",
                           (memcmp(s, "FACP", 4) == 0) ? "   <== the FADT acpi.sys needs" : "");
                    // ACPI 2.0+ (XSDT-based) systems expect FADT revision >= 3 and
                    // length >= 244, which is where the X_* 64-bit address fields
                    // live. A revision-1 / 116-byte FADT is ACPI 1.0 shaped and has
                    // none of them -- flag that explicitly, since it is the current
                    // suspect for STATUS_ACPI_INVALID_TABLE.
                    if (memcmp(s, "FACP", 4) == 0 && (trev < 3 || tlen < 244))
                        printf("[u51]         ^ ACPI 1.0 shaped (rev<3 / len<244): no X_* extended address fields\n");
                } else {
                    printf("[u51]     [%u] -> GPA 0x%llX  (outside guest RAM -- UNREADABLE)\n",
                           e, (unsigned long long)ptr);
                }
            }
        }
    }
    fflush(stdout);
}

void injectInterrupt(WHV_PARTITION_HANDLE partition, unsigned char vector);
int guestInterruptsEnabled(WHV_PARTITION_HANDLE partition);
int ioapicResolveVector(int gsi, unsigned char legacyVector, unsigned char *outVector);
// The redirection tables live much further down this file; the input heartbeat
// needs to report a GSI's raw entry, so reach it through an accessor.
UINT64 ioapicRteFor(int gsi);

// U53: route a device interrupt through the guest's own I/O APIC programming.
//
// Every device IRQ in this file used to be injected as a HARDCODED legacy PIC
// vector -- AHCI/ATA 0x76, NIC 0x73, keyboard 0x09, mouse 0x74 -- straight into
// the vCPU, ignoring the redirection tables entirely. Only the RTC ever consulted
// ioapicResolveVector. Measured consequence: once Windows switches to APIC mode it
// programs its own vectors into the I/O APIC (observed live: entry 8 -> 0xD1,
// entry 9 -> 0xB0, both unmasked) and registers its ISRs for those vectors, so an
// AHCI completion injected as 0x76 lands on a vector nothing is listening to. The
// storage driver never sees its completion, the guest goes idle waiting, and no
// disk write ever happens -- matching the observed ~64 exits/sec idle that tracks
// the RTC tick rate exactly.
//
// ioapicResolveVector keeps this backward compatible: while an entry is still at
// its power-on default it returns the legacy vector (so firmware/bootloader keeps
// working exactly as before), and once the guest programs the entry we deliver the
// vector the guest actually asked for. A masked entry means the guest does not
// want the interrupt, so it is dropped rather than forced through.
// --- Interrupt queue -------------------------------------------------------
//
// WHvRegisterPendingInterruption is a SINGLE SLOT holding one undelivered
// interrupt. Writing it while the guest has not yet taken the previous one simply
// destroys that one, and there is no queue anywhere to catch it.
//
// That is fatal here because the RTC fires at ~1kHz. Measured: 14 keystrokes
// queued, 14 keyboard IRQs fired on the guest's own unmasked vector 0xA0, and the
// guest read NONE of them, while the RTC delivered 36083 interrupts in the same
// run. The keyboard was never ignored -- it was overwritten, every time.
//
// Simply refusing to overwrite is not the answer either: the slot is occupied
// essentially always, so the new interrupt gets dropped instead and input still
// never lands (measured skippedBusy=5025 with kbRead still frozen). Whichever one
// is discarded, a 1kHz source starves everything else.
//
// So queue them and drain in PRIORITY order, oldest first within a priority. Input
// outranks storage, which outranks the timer: a late timer tick is invisible, a
// lost keystroke is not.
// GSIs for the devices we emulate. The PCI ones match the _PRT in acpi/dsdt.asl
// (device 2 = AHCI -> GSI 16, device 3 = RTL8139 -> GSI 17, device 5 = e1000 backup -> GSI 21); the ISA ones are their
// classic IRQ numbers. Declared here because the interrupt queue below needs them
// to decide priority.
#define GSI_KEYBOARD 1
#define GSI_MOUSE    12
#define GSI_AHCI     16
#define GSI_NIC      17   // PCI 0:3.0 RTL8139 (primary NIC)
#define GSI_E1000    21   // PCI 0:5.0 Intel 82540EM backup NIC

#define IRQ_PRIO_INPUT   0
#define IRQ_PRIO_DEVICE  1
#define IRQ_PRIO_TIMER   2
#define IRQ_QUEUE_MAX    64

typedef struct { unsigned char vector; int prio; unsigned long seq; } QueuedIrq;
static QueuedIrq g_irqQueue[IRQ_QUEUE_MAX];
static int g_irqQueueCount = 0;
static unsigned long g_irqSeq = 0;
long g_irqQueued = 0, g_irqDelivered = 0, g_irqQueueFull = 0;
long g_irqCoalesced = 0;    // already pending for that vector
long g_irqDeferredTpr = 0;  // held back because the guest has that level masked
long g_irqTprReadFail = 0;  // CR8 unreadable -- we deliver anyway, see drainInterruptQueue
// WHY drainInterruptQueue declined to deliver. Delivery freezing solid while the
// queue backs up is a state the counters could not previously distinguish: a
// stuck injection slot, a guest running with IF=0, and a failed register read all
// looked identical from outside (delivered simply stopped moving). Separated so
// the next stall names its own cause instead of needing another run to guess.
long g_irqSlotBusy = 0;     // previous injection still pending -- guest has not taken it
long g_irqIfClear = 0;      // guest running with interrupts masked (EFLAGS.IF=0)
long g_irqGetFail = 0;      // could not read the pending-interruption register
UINT64 g_irqLastSlot = 0;   // raw slot value last time it was found occupied
// Stuck-slot recovery. See the unwedge block in drainInterruptQueue: how many
// consecutive passes the identical pending value has to persist before it is
// treated as wedged rather than in flight. drainInterruptQueue runs every
// run-loop pass, so this is a small fraction of a second of real time -- long
// enough that an event WHP is about to inject is never touched.
#define IRQ_STUCK_CLEAR_AFTER 200
static UINT64 g_irqStuckLastValue = 0;
static int g_irqStuckRepeats = 0;
long g_irqSlotCleared = 0;  // wedged events we dropped to unblock the queue
// WHvRequestInterrupt outcome tallies. Defined further down with injectInterrupt,
// which is the other caller; declared here because drainInterruptQueue uses them
// when IRQ_VIA_APIC is on.
extern long g_reqIrqOk, g_reqIrqFail;
extern HRESULT g_reqIrqLastHr;
extern unsigned char g_reqIrqLastVector;

// TPR gating, scoped to INPUT vectors only. Set to 0 to disable it entirely.
//
// Applying it to EVERY vector was measured and is catastrophic: the guest triple
// faults ~15s in, reproducibly, before it has even programmed the IOAPIC. Early
// Windows boot sits at a high IRQL for long stretches, so a blanket gate starves
// the timer and storage exactly when the HAL is waiting on them -- those runs
// never started AHCI at all (PxCMD=0, zero IRQ injections), while the unmodified
// baseline booted fine on the same host minutes earlier. Bisected against that
// baseline; the queue coalescing below was cleared by the same bisection.
//
// The deadlock the gate exists to prevent is specific to the i8042: i8042prt
// raises to IRQL 10 and takes its interrupt spinlock, and a keyboard/mouse ISR
// entered there re-enters that same lock on a one-vCPU guest and spins forever.
// Only the input vectors need it, and confining it to them leaves the timer and
// storage paths -- which demonstrably work -- exactly as they were.
#define IRQ_RESPECT_TPR   1
#define IRQ_COALESCE_IRR  1

// Extend the TPR gate above to TIMER vectors as well as input. DEFAULT ON, and
// unlike the last two attempts this one is measured. Paired A/B, arms alternated
// within one batch, Windows Setup on screen in all four runs:
//
//     timer gate OFF   4 stalls -> 3/15 keys      5 stalls -> 2/15
//     timer gate ON    0 stalls -> 15/15          0 stalls -> 15/15
//
// Then four consecutive confirmation runs with it on: 60/60 keys, 0 stalls,
// 0 bugchecks, Setup up every time.
//
// It works because the raw injection path forces a vector in regardless of CR8,
// which breaks the guarantee the HAL depends on -- it raises to HIGH_LEVEL
// exactly so nothing can preempt while it holds its time lock. Forcing the RTC
// in anyway lands its handler in RtlGetInterruptTimePrecise ->
// HalpAcquireHighLevelLock on a lock this same CPU already holds, and with one
// vCPU nothing can ever release it.
//
// LOCALHOST_TPR_GATE_TIMER=0 restores the old behaviour.
int g_tprGateTimer = 1;

// Route DEVICE (storage/NIC) interrupts through the APIC instead of the raw slot.
// DEFAULT ON. With the timer also on the APIC this is what finally lets storahci
// complete commands: AHCI IRQ injections 15 -> 105, guest sectors 2817 -> 11547,
// and Windows Setup reaches its edition-selection screen (which requires reading
// the install image off the ISO). LOCALHOST_APIC_DEVICE=0 reverts.
int g_apicDeviceVectors = 1;
// Route TIMER (RTC) interrupts through the APIC instead of the raw slot.
//
// DEFAULT ON, measured. The raw slot jams on vector 0xD1 and leaves a HALTED
// guest unwakeable, which is the VM "going mad" after being left alone: the guest
// sits in HalProcessorIdle and nothing ever wakes it.
//
//   raw slot:  RTC fired 37000, only 1800 queued, 35414 coalesced onto ONE
//              entry stuck at depth=1, 20 unwedge events, exitCount frozen 112s
//   via APIC:  depth=0 throughout a 4-minute soak, 0 stalls, 0 unwedges,
//              delivered climbing 2376 -> 7181 -> 15815, guest never froze
//
// The APIC has an IRR bit per vector, applies priority itself and tracks EOI, so
// it cannot jam the way one register does. The reason this was previously scoped
// away -- it "died immediately after the first RTC tick was delivered through the
// APIC as vector 0xD1" -- was the HAL time-lock deadlock fixed in 31d7bab.
// LOCALHOST_APIC_TIMER=0 reverts.
int g_apicTimerVectors = 1;

// Follow the guest when it re-programs BAR5. DEFAULT ON now that AHCI completion
// interrupts actually arrive (device+timer vectors on the APIC above). This was
// off while the raw slot could not carry them and Setup never reached its GUI;
// with the APIC carrying them, Setup reads the install image off the ISO and gets
// as far as its edition-selection screen. LOCALHOST_AHCI_REBASE=0 reverts.
int g_ahciAllowRebase = 1;

// Deliver queued interrupts through the virtual APIC (WHvRequestInterrupt)
// instead of forcing them into the raw pending-interruption slot.
//
// The raw slot is ONE register with no priority, no queuing and no EOI
// awareness, and it wedges: measured with vector 0xD1 (the RTC) pending in it
// across 20560 consecutive samples, after which nothing was ever delivered to
// the guest again and AHCI piled up behind it. The guest's ISR EOIs the
// WHP-emulated LAPIC, but the LAPIC never had an in-service bit for a vector we
// injected behind its back, so the two views of interrupt state drift apart
// until delivery stops for good.
//
// The APIC has an IRR bit per vector, applies TPR/PPR priority itself and tracks
// EOI -- all the bookkeeping the slot has no concept of. In this mode our queue
// stops being a substitute for the APIC and becomes a short buffer in front of
// it, so the slot-busy check and the TPR gate are both skipped: the APIC does
// both, correctly, and holding a vector back here as well would only duplicate
// what IRR already does.
//
// Vectors below 0x10 still take the raw path -- the APIC rejects them (0..15 are
// CPU exception vectors) and firmware legitimately uses low PIC-remapped vectors
// before the guest switches to APIC mode.
//
// SCOPED TO INPUT VECTORS, for the same reason the TPR gate is. Routing every
// vector through the APIC triple faults the guest ~15s in -- reproduced here, and
// matching an earlier attempt that died "immediately after the first RTC tick was
// delivered through the APIC as vector 0xD1". The timer and storage paths work on
// the raw slot and are left on it.
//
// Pairing the two is what makes this worth doing: the slot wedges on the RTC's
// vector, but input delivered through the APIC does not touch the slot at all, so
// the mouse and keyboard keep flowing across a jam that stops everything else.
// Set to 0 to put input back on the raw slot.
//
// WHY THE RTC CANNOT JOIN THEM, established by measurement rather than caution:
//
//   1. The raw slot STOPS BEING HONOURED once the guest's APIC is fully active.
//      Caught in the act: vector 0xD1 pending while the guest was provably able
//      to take it -- IF=1, CR8=0, no interrupt shadow, no NMI mask -- and WHP
//      never delivered it, for the rest of the run. Everything queued behind it
//      stops too, storage included. See the [irq-stuck-why] diagnostic.
//   2. But routing the RTC through the APIC instead kills the guest instantly.
//      Reproduced three times, and the log pins it precisely: the guest triple
//      faults on the very first tick that goes out that way, immediately after
//      "[rtc-diag] tick #1 FIRED vector=0xD1".
//   3. Gating on the kernel being up does NOT separate these. g_bpModuleBase is
//      set during early kernel DISCOVERY -- "kernelFound=1" is printed before
//      that first tick -- so the gate opens too early to help.
//
// So input goes through the APIC (which works, and which the i8042 needs for its
// TPR handling), the timer stays on the raw slot (which eventually wedges), and
// the wedge is still open. What it needs is a reason WHY 0xD1 specifically is
// fatal through the APIC when 0x90/0xA0 are not -- double delivery from the
// halted-loop RTC path and EOI/in-service bookkeeping are the two candidates
// worth checking first.
#define IRQ_VIA_APIC      1

static void queueInterrupt(unsigned char vector, int prio) {
    int i;
#if !IRQ_COALESCE_IRR
    (void)i;
#endif
    // ONE PENDING ENTRY PER VECTOR, which is what a real APIC's IRR is: a single
    // bit per vector, not a count. Without this a source that keeps firing while
    // the guest has that level masked -- the RTC, at ~1kHz -- fills the queue in
    // 64ms and pushes every other device's interrupt out of it.
#if IRQ_COALESCE_IRR
    for (i = 0; i < g_irqQueueCount; i++) {
        if (g_irqQueue[i].vector == vector) { g_irqCoalesced++; return; }
    }
#endif
    if (g_irqQueueCount >= IRQ_QUEUE_MAX) { g_irqQueueFull++; return; }
    g_irqQueue[g_irqQueueCount].vector = vector;
    g_irqQueue[g_irqQueueCount].prio = prio;
    g_irqQueue[g_irqQueueCount].seq = g_irqSeq++;
    g_irqQueueCount++;
    g_irqQueued++;
}

// Dumps the guest's IDT gate for a few vectors, to answer -- without a debugger --
// whether Windows has actually CONNECTED an interrupt service routine to them.
//
// This is the question left after the interrupt queue: delivery is provably
// correct (keystrokes queued, IRQs fired on the guest's own unmasked vector 0xA0,
// none masked, none dropped) and the guest still never reads the byte. Either it
// has no handler for 0xA0, or it has one and is ignoring us.
//
// Vector 0xD1 is the control: the RTC uses it and is serviced tens of thousands of
// times per run, so whatever a WORKING gate looks like, 0xD1 looks like it. kd
// could answer this too, but its ~75MB/s memory drain kills a session on this host
// before a command completes -- twice now.
extern void *guestMemory;   // defined further down with the rest of the VM state
extern SIZE_T guestMemSize;
static void dumpIdtGate(WHV_PARTITION_HANDLE partition, unsigned char vector, const char *what) {
    WHV_REGISTER_NAME idtrName = WHvX64RegisterIdtr;
    WHV_REGISTER_VALUE idtr = { 0 };
    if (FAILED(WHvGetVirtualProcessorRegisters(partition, 0, &idtrName, 1, &idtr))) return;
    UINT64 base = idtr.Table.Base;
    UINT16 limit = idtr.Table.Limit;
    UINT32 off = (UINT32)vector * 16;
    if (off + 16 > (UINT32)limit + 1) { printf("[idt] vector 0x%02X beyond IDT limit\n", vector); return; }

    // IDTR.Base is a VIRTUAL address -- a kernel VA like 0xFFFFF802`47068000, not
    // a GPA. Indexing guest RAM with it directly (as this first did) is simply
    // wrong and reports "not in mapped guest RAM" for a perfectly valid IDT.
    // WHvTranslateGva walks the guest's own page tables for us.
    WHV_GUEST_PHYSICAL_ADDRESS gpa = 0;
    WHV_TRANSLATE_GVA_RESULT tr = { 0 };
    HRESULT thr = WHvTranslateGva(partition, 0, base + off,
                                  WHvTranslateGvaFlagValidateRead, &tr, &gpa);
    if (FAILED(thr) || tr.ResultCode != WHvTranslateGvaResultSuccess) {
        printf("[idt] %-8s vector=0x%02X -- GVA 0x%llX untranslatable (hr=0x%lX result=%d)\n",
               what, vector, (unsigned long long)(base + off),
               (unsigned long)thr, (int)tr.ResultCode);
        fflush(stdout);
        return;
    }
    if (!guestMemory || (UINT64)gpa + 16 > (UINT64)guestMemSize) {
        printf("[idt] %-8s vector=0x%02X -- GPA 0x%llX outside mapped RAM\n",
               what, vector, (unsigned long long)gpa);
        fflush(stdout);
        return;
    }
    const unsigned char *g = (const unsigned char *)guestMemory + gpa;
    UINT16 offLow  = (UINT16)(g[0] | (g[1] << 8));
    UINT16 sel     = (UINT16)(g[2] | (g[3] << 8));
    unsigned char ist = g[4];
    unsigned char type = g[5];
    UINT16 offMid  = (UINT16)(g[6] | (g[7] << 8));
    UINT32 offHigh = (UINT32)(g[8] | (g[9] << 8) | (g[10] << 16) | ((UINT32)g[11] << 24));
    UINT64 handler = (UINT64)offLow | ((UINT64)offMid << 16) | ((UINT64)offHigh << 32);
    printf("[idt] %-8s vector=0x%02X handler=0x%016llX sel=0x%04X ist=%u type=0x%02X present=%d",
           what, vector, (unsigned long long)handler, sel, ist & 7, type, (type & 0x80) ? 1 : 0);
    if (g_bpModuleBase && handler > g_bpModuleBase && handler - g_bpModuleBase < 0x2000000ULL)
        printf(" (ntoskrnl+0x%llX)", (unsigned long long)(handler - g_bpModuleBase));
    printf("\n");
    fflush(stdout);
}

// Hands the guest one queued interrupt if its pending slot is free. Called every
// run-loop pass, so a backlog drains as fast as the guest will accept.
// RESPECTS THE GUEST'S TASK PRIORITY. This is not a refinement, it is the
// difference between a working guest and a deadlocked one.
//
// WHvRegisterPendingInterruption shoves a vector straight into the vCPU's event
// injection field. It does NOT go through the virtual APIC, so it does not
// consult the TPR -- meaning we were delivering interrupts the guest had
// explicitly masked, at moments the guest's own code treats as impossible.
//
// On x64 Windows, IRQL *is* CR8. When i8042prt calls KeAcquireInterruptSpinLock
// it raises to the i8042 interrupt object's synchronise IRQL (10, the higher of
// its keyboard vector 0xA0 and mouse vector 0x90) and takes the lock -- and at
// that IRQL, hardware would mask BOTH those vectors. Injecting the mouse vector
// anyway ran its ISR, which tried to acquire the lock the interrupted code was
// already holding, on a guest with one vCPU: an unbreakable spin at DIRQL.
// Measured exactly that -- rip parked in ntoskrnl's spin path, cr8=0xA, exit
// count frozen for ten minutes, immediately after the mouse init handshake.
//
// A vector is deliverable only when its priority class (vector >> 4) strictly
// outranks CR8. Anything else stays QUEUED, not dropped, and goes out as soon as
// the guest lowers IRQL -- which is exactly what the queue is for.
void drainInterruptQueue(WHV_PARTITION_HANDLE partition) {
    if (g_irqQueueCount == 0) return;
    WHV_REGISTER_NAME pendName = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE existing = { 0 };
    unsigned tpr = 0;

#if IRQ_VIA_APIC
    // Hand the INPUT interrupts to the APIC, oldest first. No slot check and no
    // TPR gate for these -- the APIC's IRR is a better place to hold a masked
    // vector than our queue is, and it releases it at the right moment by itself.
    {
        int drained = 0;
        while (g_irqQueueCount > 0 && drained < IRQ_QUEUE_MAX) {
            int b = -1, k;
            WHV_INTERRUPT_CONTROL ic;
            HRESULT hr;
            for (k = 0; k < g_irqQueueCount; k++) {
                // The APIC rejects vectors below 0x10; those are firmware-era
                // legacy PIC-remapped ones and keep the raw path they always had.
                if (g_irqQueue[k].vector < 0x10) continue;
                // INPUT VECTORS ONLY. Widening this has been tried twice and is
                // fatal both times -- see the note below on the RTC.
                //
                // DEVICE vectors too when g_apicDeviceVectors is set. The raw slot
                // demonstrably jams -- 20 unwedge events in one run on vectors
                // 0x50 and 0xD1 -- and that did not matter while storahci never
                // bound to our controller. Now that it does (e3b482b), it issues a
                // command, we complete it, and the completion interrupt cannot get
                // through: PxIS=0x1 pending, 15 injections total, the guest reads
                // 1MB and then waits forever at IRQL 0. That is the slow boot.
                //
                // Worth retrying precisely because the measurement that scoped
                // this to input predates today's work: it died "immediately after
                // the first RTC tick was delivered through the APIC as vector
                // 0xD1", which is the HAL time-lock deadlock fixed in 31d7bab.
                // Gated and default-off until measured.
                // TIMER too, when g_apicTimerVectors is set. The raw slot is fatal
                // for this vector specifically: measured over one idle run, the
                // RTC fired 37000 times, only 1800 were ever queued, 35414 were
                // coalesced onto ONE entry that sat at depth=1, and the unwedge
                // path fired 20 times -- every one of them vector 0xD1. The guest
                // halts in HalProcessorIdle and is then never woken: exitCount
                // frozen for 112s, which presents as the VM "going mad" because
                // input does nothing while the guest is not running at all.
                //
                // The APIC has an IRR bit per vector, applies priority itself and
                // tracks EOI, so it cannot jam the way one register does. Retrying
                // it for the timer is justified now because the reason it was
                // scoped away -- it "died immediately after the first RTC tick was
                // delivered through the APIC as vector 0xD1" -- is the HAL
                // time-lock deadlock fixed in 31d7bab.
                if (g_irqQueue[k].prio != IRQ_PRIO_INPUT &&
                    !(g_apicDeviceVectors && g_irqQueue[k].prio == IRQ_PRIO_DEVICE) &&
                    !(g_apicTimerVectors  && g_irqQueue[k].prio == IRQ_PRIO_TIMER)) continue;
                if (b < 0 || g_irqQueue[k].seq < g_irqQueue[b].seq) b = k;
            }
            if (b < 0) break;                       // nothing left for the APIC

            memset(&ic, 0, sizeof(ic));
            ic.Type = WHvX64InterruptTypeFixed;
            ic.DestinationMode = WHvX64InterruptDestinationModePhysical;
            ic.TriggerMode = WHvX64InterruptTriggerModeEdge;
            ic.Destination = 0;                     // APIC ID of the single vCPU
            ic.Vector = g_irqQueue[b].vector;
            hr = WHvRequestInterrupt(partition, &ic, sizeof(ic));
            if (FAILED(hr)) {
                // Leave it queued and retry next pass rather than dropping it.
                g_reqIrqFail++;
                g_reqIrqLastHr = hr;
                g_reqIrqLastVector = g_irqQueue[b].vector;
                break;
            }
            g_reqIrqOk++;
            for (k = b; k < g_irqQueueCount - 1; k++) g_irqQueue[k] = g_irqQueue[k + 1];
            g_irqQueueCount--;
            g_irqDelivered++;
            drained++;
        }
        if (g_irqQueueCount == 0) return;
        // Anything left is timer/storage, or a sub-0x10 legacy vector: raw slot.
    }
#endif

    if (FAILED(WHvGetVirtualProcessorRegisters(partition, 0, &pendName, 1, &existing))) {
        g_irqGetFail++;
        return;
    }
    if (existing.Reg64 & 1ULL) {                    // guest has not taken the last one
        g_irqSlotBusy++;
        g_irqLastSlot = existing.Reg64;
        // UNWEDGE IT. The guest is demonstrably able to take an interrupt at this
        // point -- IF=1, CR8=0, no shadow, no NMI mask, measured -- and WHP still
        // never consumes what is sitting in the slot. Nothing arrives again for
        // the rest of the run, storage included, so the pending event is worth
        // nothing where it is: dropping it costs an interrupt that was never
        // going to be delivered, and buys back every one behind it.
        //
        // Deliberately patient. It only fires after the same vector has been
        // stuck across many consecutive passes, so a genuinely in-flight event
        // -- one WHP is about to inject on the next entry -- is never disturbed.
        if (existing.Reg64 == g_irqStuckLastValue) {
            g_irqStuckRepeats++;
        } else {
            g_irqStuckLastValue = existing.Reg64;
            g_irqStuckRepeats = 1;
        }
        if (g_irqStuckRepeats >= IRQ_STUCK_CLEAR_AFTER) {
            WHV_REGISTER_VALUE clear = { 0 };
            g_irqStuckRepeats = 0;
            g_irqStuckLastValue = 0;
            if (SUCCEEDED(WHvSetVirtualProcessorRegisters(partition, 0, &pendName, 1, &clear))) {
                g_irqSlotCleared++;
                if (g_irqSlotCleared <= 20) {
                    printf("[irq-unwedge] cleared a stuck pending interruption "
                           "(vector=0x%02X, stuck %d passes) -- cleared %ld so far\n",
                           (unsigned)((existing.Reg64 >> 16) & 0xFF),
                           IRQ_STUCK_CLEAR_AFTER, g_irqSlotCleared);
                    fflush(stdout);
                }
            }
            return;
        }

        // WHY is it stuck? The counters say a vector sits here forever, but not
        // what is preventing WHP from injecting it. The candidates each imply a
        // different fix, so read the vCPU's actual interruptibility rather than
        // reasoning about it: RFLAGS.IF (guest ran into a long cli region), the
        // interrupt shadow (a STI/MOV-SS window WHP will not inject into), and
        // CR8/RIP for context. Sampled sparsely -- this fires tens of thousands
        // of times and console output would distort the guest.
        if ((g_irqSlotBusy % 20000) == 1) {
            WHV_REGISTER_NAME dn[4] = { WHvX64RegisterRflags, WHvX64RegisterCr8,
                                        WHvX64RegisterRip, WHvRegisterInterruptState };
            WHV_REGISTER_VALUE dv[4] = { 0 };
            if (SUCCEEDED(WHvGetVirtualProcessorRegisters(partition, 0, dn, 4, dv))) {
                printf("[irq-stuck-why] slotBusy=%ld vector=0x%02X | IF=%d CR8=%u "
                       "rip=0x%llX | shadow=%u nmiMasked=%u\n",
                       g_irqSlotBusy, (unsigned)((existing.Reg64 >> 16) & 0xFF),
                       (dv[0].Reg64 & (1ULL << 9)) ? 1 : 0,
                       (unsigned)(dv[1].Reg64 & 0xF),
                       (unsigned long long)dv[2].Reg64,
                       (unsigned)dv[3].InterruptState.InterruptShadow,
                       (unsigned)dv[3].InterruptState.NmiMasked);
                fflush(stdout);
            }
        }
        return;
    }
    if (!guestInterruptsEnabled(partition)) { g_irqIfClear++; return; }

    // Read CR8 in its OWN call, and treat a failure as "deliver anyway".
    //
    // Both of those are load-bearing. Batching it with WHvRegisterPendingInterruption
    // above -- they are different register classes -- made the whole call fail, and
    // because the old code then returned, NOTHING was ever delivered: the guest ran
    // with every device interrupt silently withheld and triple-faulted 15s in, twice.
    // The tell was delivered=0 deferredTpr=0 while the RTC reported 25 ticks fired.
    // An unreadable TPR must never be able to silence every device in the machine.
    {
        WHV_REGISTER_NAME cr8Name = WHvX64RegisterCr8;
        WHV_REGISTER_VALUE cr8 = { 0 };
        if (SUCCEEDED(WHvGetVirtualProcessorRegisters(partition, 0, &cr8Name, 1, &cr8)))
            tpr = (unsigned)(cr8.Reg64 & 0xF);
        else
            g_irqTprReadFail++;                     // tpr stays 0: nothing is masked
    }

    int best = -1, i;
    for (i = 0; i < g_irqQueueCount; i++) {
#if IRQ_RESPECT_TPR
        // Input only -- see IRQ_RESPECT_TPR's comment for why this is not applied
        // to the timer and storage vectors.
        //
        // TIMER too, when g_tprGateTimer is set. This raw path forces a vector in
        // regardless of CR8, and that breaks the guarantee the HAL relies on: it
        // raises to HIGH_LEVEL precisely so nothing can preempt while it holds its
        // time lock. Forcing the RTC in there anyway lands its handler in
        // RtlGetInterruptTimePrecise -> HalpAcquireHighLevelLock on a lock the
        // same CPU already holds, and on ONE vCPU nothing can release it -- the
        // deadlock resolved in 9d7ec5f, which is what actually eats keystrokes
        // (0 stalls measured 15/15 delivered, 5 stalls measured 2/15).
        //
        // Narrower than the two attempts already reverted here: gating EVERY
        // vector triple-faulted at ~15s by starving storage, and routing
        // everything through the APIC died on the first RTC tick. Storage is
        // untouched by this; a deferred timer tick is queued, not dropped, and
        // goes out as soon as the guest lowers IRQL.
        if (g_irqQueue[i].prio == IRQ_PRIO_INPUT &&
            (unsigned)(g_irqQueue[i].vector >> 4) <= tpr) { g_irqDeferredTpr++; continue; }
        // TIMER: defer ONLY at HIGH_LEVEL, not at every raised IRQL.
        //
        // The first cut of this gated the timer on the same rule as input --
        // defer whenever the vector's priority class <= CR8 -- and that starved
        // the guest's clock. The RTC is vector 0xD1, class 13, so it was held
        // back at any IRQL >= 13, and because the queue keeps ONE entry per
        // vector, every tick arriving behind a deferred one was coalesced away.
        // Measured: 9000 ticks fired, 573 delivered -- a ~7Hz clock instead of
        // ~1kHz, so Windows ran with time crawling and the boot took minutes.
        // That is the "boot is so slow" regression, and it was mine.
        //
        // The deadlock this exists to prevent is specific: it happens when we
        // force a timer interrupt into a guest holding the HAL's time lock, which
        // it takes at HIGH_LEVEL precisely so nothing can preempt. So gate on
        // HIGH_LEVEL alone. Below that the guest is interruptible by design and
        // the clock flows at full rate.
        if (g_tprGateTimer && g_irqQueue[i].prio == IRQ_PRIO_TIMER &&
            tpr >= 0xF) { g_irqDeferredTpr++; continue; }
#endif
        if (best < 0 ||
            g_irqQueue[i].prio < g_irqQueue[best].prio ||
            (g_irqQueue[i].prio == g_irqQueue[best].prio && g_irqQueue[i].seq < g_irqQueue[best].seq))
            best = i;
    }
    if (best < 0) return;                           // nothing deliverable right now
    unsigned char vec = g_irqQueue[best].vector;
    for (i = best; i < g_irqQueueCount - 1; i++) g_irqQueue[i] = g_irqQueue[i + 1];
    g_irqQueueCount--;

    WHV_REGISTER_VALUE regValue = { 0 };
    regValue.Reg64 = 1ULL | ((UINT64)vec << 16);    // pending, type 0 (external), vector
    WHvSetVirtualProcessorRegisters(partition, 0, &pendName, 1, &regValue);
    g_irqDelivered++;
}

static int injectDeviceIrqPrio(WHV_PARTITION_HANDLE partition, int gsi,
                               unsigned char legacyVector, int prio) {
    unsigned char vec;
    if (!ioapicResolveVector(gsi, legacyVector, &vec)) return 0; // masked by the guest
    queueInterrupt(vec, prio);
    drainInterruptQueue(partition);                 // deliver immediately if possible
    return 1;
}

static int injectDeviceIrq(WHV_PARTITION_HANDLE partition, int gsi, unsigned char legacyVector) {
    // Input devices outrank everything else; see the queue comment above.
    int prio = (gsi == GSI_KEYBOARD || gsi == GSI_MOUSE) ? IRQ_PRIO_INPUT : IRQ_PRIO_DEVICE;
    return injectDeviceIrqPrio(partition, gsi, legacyVector, prio);
}


// WHV rejects injecting a pending interruption while the guest has
// interrupts masked (EFLAGS.IF=0) -- that produces an
// InvalidVpRegisterValue exit on the next run. A real PIC would latch
// the IRQ request until the CPU unmasks interrupts; we do the same
// with a single-slot flag that the main loop drains once IF=1 (see
// deliverPendingAtaIrq, called every iteration).
int pendingAtaIrq = 0;

// U56b: AHCI interrupts are LEVEL-triggered. Per the AHCI spec the controller
// asserts its interrupt while GHC.IE is set and (PxIS & PxIE) is non-zero, and
// keeps asserting until the driver clears the status bits. We only ever injected
// edge-style from the command-completion path, so any condition that was already
// pending when the driver armed PxIE produced no injection at all.
//
// Measured: storahci does start the controller (GHC=0x80000002 AE=1 IE=1,
// PxCMD=0x0000C013 ST=1 FRE=1, PxIE=0x7D40004F) and then sits with
// PxIS=0x00000001 -- a D2H Register FIS interrupt pending, never cleared, because
// the guest was never actually interrupted. That is why it idles in
// HalProcessorIdle with the port started and no commands in flight.
//
// Re-assert while the condition holds, throttled so a guest that is slow to run
// its ISR is not flooded. Also maintains the global IS port bit, which the driver
// reads to find which port raised the interrupt.
extern void *ahciAbarMemory; // defined with the rest of the AHCI BAR5 state below

// AHCI port table. Declared up here because ahciServiceLevelInterrupt below has
// to scan every port to find which one is asserting; the definition and the full
// explanation live with the rest of the disk state further down.
#define AHCI_PORT_COUNT 2
typedef struct {
    FILE   *file;
    UINT64  sectors;
    UINT32  sectorSize;
    int     present;
    char    path[512];
} AhciPortDevice;
extern AhciPortDevice ahciPorts[AHCI_PORT_COUNT];
static int ahciPortsImplemented(void);
// U61: interrupt-handshake tracing -- injections we make vs acknowledgements the
// driver writes back. Declared here because ahciServiceLevelInterrupt below is the
// first user; ahciHandleAbarMmio (further down) logs the acknowledgement side.
// U65: one switch for the heavy per-event diagnostics accumulated across U59-U62.
// They were each written to answer a specific question and have all now answered it,
// but together they made the VM slow enough to bog the host down while WinDbg was
// also attached (a force shutdown was needed during U64). Turning them off leaves
// the CHEAP uncapped counters in place -- those cost a couple of increments per
// event and are precisely what has repeatedly saved this investigation from
// drawing false conclusions out of capped logs.
//
// Set to 1 to get the ABAR register conversation, per-command ATA tracing, the
// interrupt-handshake trace and the framebuffer checksum back.
#define LOCALHOST_VERBOSE_DIAG 0

#define U61_IRQ_LOG_MAX 40
int g_ahciIrqLogged = 0, g_ahciAckLogged = 0;
long g_ahciIrqCount = 0; // U61: uncapped injection count, reported in the heartbeat
long g_ahciRejected = 0; // commands we answered with ERR|ABRT -- see the reject log
long g_ahciDropped = 0;  // commands discarded for an unusable PxCLB/CTBA
long g_bootImgSvcLogged = 0;
long g_pitTicksDelivered = 0; // firmware-era timer ticks (see deliverPitTimerIrq)
static long g_ahciLastIrqExit = 0;
// Which ports were asserting at the last injection, so a NEW completion goes out
// immediately while a repeat of the same unacknowledged one stays throttled.
static UINT32 g_ahciLastAsserting = 0;
// Wall-clock timestamp of the last AHCI interrupt injection. Replaces counting in
// VM exits, which made disk latency depend on how busy the CPU was.
static LARGE_INTEGER g_ahciLastIrqTick;
static void ahciServiceLevelInterrupt(WHV_PARTITION_HANDLE partition, long exitCount) {
    if (!ahciAbarMemory) return;
    unsigned char *ab = (unsigned char *)ahciAbarMemory;
    // U56c REVERTED: mirroring PxCMD.CR/FR onto ST/FRE from here was a regression.
    // Measured: PCI config accesses collapsed 20901 -> 4472, AHCI commands 3991 ->
    // 1, PxCI stuck at 1, and the kernel never loaded at all -- the firmware could
    // no longer read the disk. Writing PxCMD from the main loop races the existing
    // AHCI command path, which also owns that register. The underlying observation
    // still stands (nothing ever sets CR/FR, so storahci's start attempts time out
    // and it resets in a loop), but the fix has to live inside the AHCI engine that
    // already owns these registers, not in a concurrent poller. See U57.
    UINT32 ghc = *(UINT32 *)(ab + 0x04);
    if (!(ghc & 0x2)) return;                       // GHC.IE clear -- interrupts globally off
    // Any implemented port can be the one asserting, and the global IS bit the
    // driver reads to find out has to name the right one -- so scan them all and
    // set a bit per asserting port rather than assuming port 0.
    UINT32 asserting = 0;
    {
        int p;
        for (p = 0; p < AHCI_PORT_COUNT; p++) {
            unsigned char *pp;
            if (!ahciPorts[p].present) continue;
            pp = ab + 0x100 + (UINT32)p * 0x80;
            if ((*(UINT32 *)(pp + 0x10) & *(UINT32 *)(pp + 0x14)) != 0)
                asserting |= (1u << p);
        }
        if (asserting == 0) { g_ahciLastAsserting = 0; return; } // nothing to report
        *(UINT32 *)(ab + 0x08) |= asserting;        // global IS
    }
    // EDGE-AWARE. This used to throttle EVERY assertion to one per 500 VM exits,
    // which with ~1000 exits/sec capped disk completions at ~2/sec. Every command
    // waited on that timer, so the guest managed ~3 AHCI commands a second and
    // Windows Setup copied files at ~144 KB/s -- an install would have taken
    // something like eight hours, and it looked stuck at 0%.
    //
    // The throttle exists to stop a storm of RE-assertions for a condition the
    // driver has not acknowledged yet, and for that it is still right. But a NEW
    // completion is not a re-assertion: it is exactly the edge the driver is
    // waiting for, and delaying it serves nothing. So deliver immediately when the
    // set of asserting ports CHANGES, and keep the throttle only for repeating an
    // unchanged one.
    //
    // Safe now in a way it was not before: these go through the APIC (6666e4c),
    // which has an IRR bit per vector and tracks EOI, so a fast edge cannot jam a
    // single slot the way the raw path did.
    // THROTTLE BY TIME, NOT EXIT COUNT.
    //
    // The 500-EXIT gate was the whole bottleneck, and the edge check above does
    // not rescue it: `asserting` is a bitmap of PORTS, so port 0 asserting for a
    // brand-new completion looks identical to the previous one and still lands in
    // the throttle. Measured mid-install: 1250 exits/sec / 500 = 2.5 injections
    // per second, and the observed rate was 2.5 -- the guest was managing 1.7 AHCI
    // commands/sec and 19 KB/s, which is ~60 hours for a Windows install.
    //
    // Counting in exits is wrong in principle too: it ties device latency to how
    // busy the CPU happens to be, so the quieter the guest gets the slower its
    // disk becomes -- the same mistake as driving the USB schedules and the window
    // repaint off exit count, both already fixed here.
    //
    // 1ms re-assert. Still a real throttle against re-asserting an unacknowledged
    // condition, but ~400x above the old ceiling and far above the ~100/sec the
    // driver actually needs. Safe because these go through the APIC, whose IRR
    // holds one bit per vector -- a duplicate assertion coalesces there instead of
    // jamming, which is exactly what the raw slot could not do.
    {
        LARGE_INTEGER nowIrq;
        QueryPerformanceCounter(&nowIrq);
        if (asserting == g_ahciLastAsserting && perfFrequency.QuadPart) {
            double sinceMs = (double)(nowIrq.QuadPart - g_ahciLastIrqTick.QuadPart)
                             * 1000.0 / (double)perfFrequency.QuadPart;
            if (sinceMs < 1.0) return;                  // same condition, just re-asserted
        }
        g_ahciLastIrqTick = nowIrq;
    }
    g_ahciLastAsserting = asserting;
    if (!guestInterruptsEnabled(partition)) return;
    g_ahciLastIrqExit = exitCount;
    // U61: Windows issues exactly 12 commands and stops, leaving PxIS=0x1 set with
    // PxCI=0 -- a completion interrupt that looks unacknowledged. Log the handshake
    // so we can tell whether we are failing to deliver, or delivering and the
    // driver is declining to acknowledge. Bounded; only after the kernel loads.
    g_ahciIrqCount++; // uncapped -- the log below is capped and has misled me before
    if (LOCALHOST_VERBOSE_DIAG && g_bpModuleBase && g_ahciIrqLogged < U61_IRQ_LOG_MAX) {
        g_ahciIrqLogged++;
        unsigned char *p0 = ab + 0x100;
        printf("[u61] inject #%d: port0 PxIS=0x%08X PxIE=0x%08X IS=0x%08X GHC=0x%08X exit=%ld\n",
               g_ahciIrqLogged, *(UINT32 *)(p0 + 0x10), *(UINT32 *)(p0 + 0x14),
               *(UINT32 *)(ab + 0x08), ghc, exitCount);
        fflush(stdout);
    }
    injectDeviceIrq(partition, GSI_AHCI, 0x76);
}

void ataMaybeInjectIrq(WHV_PARTITION_HANDLE partition) {
    if (guestInterruptsEnabled(partition)) {
        injectDeviceIrq(partition, GSI_AHCI, 0x76); // U53: was hardcoded 0x76
    } else {
        pendingAtaIrq = 1;
    }
}

void deliverPendingAtaIrq(WHV_PARTITION_HANDLE partition) {
    if (pendingAtaIrq && guestInterruptsEnabled(partition)) {
        injectDeviceIrq(partition, GSI_AHCI, 0x76); // U53: routed, was hardcoded 0x76
        pendingAtaIrq = 0;
    }
}

// --- PIT (8253/8254) channel 2 one-shot emulation ---
// SeaBIOS calibrates the TSC by programming channel 2 with a count via
// ports 0x43/0x42, enabling the gate via port 0x61 bit0, then polling
// port 0x61 bit5 (the channel-2 output) until it goes high -- which real
// hardware does once the programmed count of 1.193182MHz ticks has
// elapsed. We fake that countdown using wall-clock time instead of an
// actual clock divider.
#define PIT_HZ 1193182.0
unsigned char port61Gate = 0;      // last-written bit0 (gate2)
unsigned char port61SpeakerData = 0; // last-written bit1 (speaker data)

// Port 0x92 (PS/2 System Control Port A): bit1 is the "fast" A20 gate,
// bit0 is "Alternate Hot Reset" -- writing 1 there triggers a real CPU
// reset on actual hardware. BIOS code uses this (and the equivalent
// keyboard-controller reset command) to bounce from protected mode back
// to real mode. We only track the non-reset bits for readback.
unsigned char port92Value = 0x02;

// --- A20 gate emulation ---
// BIOS POST tests the A20 gate by writing to the classic 0x100000-0x10FFFF
// "HMA" window and checking whether it aliases back to 0x0-0xFFFF (gate
// disabled, real 8086-style 20-bit wraparound) or is genuinely distinct
// memory (gate enabled). We don't have a real address-line to toggle, so we
// fake the same observable behavior by remapping that GPA window to either
// the same host memory as low RAM (aliased/wrapped) or its own dedicated
// buffer (not aliased), whichever the gate state currently implies.
#define A20_WINDOW_BASE 0x100000
#define A20_WINDOW_SIZE 0x10000
void *guestMemory = NULL;  // low 1MB, also backs the A20-disabled alias
SIZE_T guestMemSize = 0;   // set once in main(); global so device emulation
                           // code (e.g. AHCI command processing) outside
                           // main() can bounds-check guest-memory pointers
void *hmaMemory = NULL;    // dedicated backing for the A20-enabled window
int a20Enabled = 0;        // matches real hardware's power-on default (off)

int a20RemapCount = 0;
int memAccessFaultCount = 0;
int ahciCmdLogCount = 0;
// Raised from 200. Firmware spends every one of the first couple of hundred on
// ISO9660 volume-descriptor and directory reads, so the END of the conversation
// -- what the boot manager asked for immediately before giving up -- was never
// visible, and reading only the tail of a capped log produced a wrong conclusion
// about which sectors were being fetched. A firmware-only boot issues ~530
// commands, so this covers it whole.
#define AHCI_CMD_LOG_MAX 2000
int g_ahciReads = 0, g_ahciWrites = 0, g_ahciOther = 0; // U55: uncapped per-opcode tallies
long g_ahciFwSectors = 0, g_ahciGuestSectors = 0; // U62: sectors moved, firmware vs guest
// U60: separate budget for commands issued after the kernel loaded, i.e. Windows'
// own, which the firmware-dominated log above never had room for.
#define U60_GUEST_CMD_LOG_MAX 120
int g_ahciGuestCmdLogged = 0;
static const char *ataCmdName(unsigned char c) {
    switch (c) {
        case 0xEC: return "IDENTIFY DEVICE";
        case 0xEF: return "SET FEATURES";
        case 0x20: return "READ SECTORS";
        case 0x24: return "READ SECTORS EXT";
        case 0xC8: return "READ DMA";
        case 0x25: return "READ DMA EXT";
        case 0x30: return "WRITE SECTORS";
        case 0x34: return "WRITE SECTORS EXT";
        case 0xCA: return "WRITE DMA";
        case 0x35: return "WRITE DMA EXT";
        case 0xE7: return "FLUSH CACHE";
        case 0xEA: return "FLUSH CACHE EXT";
        case 0x00: return "NOP/none";
        case 0xA0: return "PACKET (ATAPI)";
        case 0xA1: return "IDENTIFY PACKET DEVICE";
        case 0x40: return "READ VERIFY SECTORS";
        case 0x06: return "DATA SET MANAGEMENT (TRIM)";
        case 0xB0: return "SMART";
        case 0xE5: return "CHECK POWER MODE";
        case 0x2F: return "READ LOG EXT";
        default:   return "?";
    }
}

// "etc/ramfb" state (see the fw_cfg section below for how these get
// populated) -- declared up here, ahead of WndProc, so the paint handler
// can use them directly.
int ramfbConfigWritten = 0; // becomes true once all 28 bytes have been written at least once
UINT64 ramfbAddress = 0;
UINT32 ramfbWidth = 0, ramfbHeight = 0, ramfbStride = 0;

// Aspect-preserving fit of the guest framebuffer inside a client area.
//
// The painter used to stretch the framebuffer across the WHOLE client rect,
// which looks right only while the two happen to share an aspect ratio. They
// did while the guest was 800x600 and 1280x960 in a 4:3 preview pane. Once the
// guest went 16:9 the picture came out visibly stretched -- circles as
// ellipses. Letterboxing keeps the proportions; the leftover strips are painted
// black so they read as a deliberate border rather than a rendering fault.
//
// The SAME rectangle must drive the tablet's absolute coordinates. The guest is
// told "the pointer is at this fraction of the display", so measuring that
// fraction against the whole window while the image occupies only part of it
// puts the guest cursor at a steadily growing offset from the host one -- the
// exact class of mismatch the tablet exists to eliminate.
static void guestDisplayRect(int clientW, int clientH, RECT *out) {
    out->left = 0; out->top = 0; out->right = clientW; out->bottom = clientH;
    if (clientW <= 0 || clientH <= 0 || ramfbWidth == 0 || ramfbHeight == 0) return;

    // Integer throughout: compare the two aspect ratios by cross-multiplying
    // rather than dividing, so nothing depends on floating point here.
    long long fbW = (long long)ramfbWidth, fbH = (long long)ramfbHeight;
    long long wantW, wantH;
    if (fbW * (long long)clientH > fbH * (long long)clientW) {
        wantW = clientW;                      // framebuffer is the wider shape:
        wantH = (fbH * (long long)clientW) / fbW;  // full width, bars top+bottom
    } else {
        wantH = clientH;                      // taller shape: full height,
        wantW = (fbW * (long long)clientH) / fbH;  // bars left+right
    }
    if (wantW < 1) wantW = 1;
    if (wantH < 1) wantH = 1;
    out->left = (int)((clientW - wantW) / 2);
    out->top = (int)((clientH - wantH) / 2);
    out->right = out->left + (int)wantW;
    out->bottom = out->top + (int)wantH;
}

// ACPI power button / soft-off handoff between the WINDOW thread and the VM
// thread -- declared up here for the same reason as the ramfb state above:
// WndProc sets g_powerButtonRequest on WM_CLOSE and is defined further up the
// file than the rest of the PM1a emulation. volatile + Interlocked because the
// two threads genuinely run concurrently. See the PM1a_EVT_BLK block for what
// consumes these.
volatile LONG g_powerButtonRequest = 0;
volatile LONG g_guestPoweredOff = 0;

// --- UEFI (OVMF) boot support ---
// UEFI firmware wants to sit at the very top of a real 4GB address space
// (see the reset-vector setup in main()), not squeezed into the legacy
// sub-1MB BIOS area, so it needs a much larger flat guest RAM region than
// the legacy 1MB path uses.
//
// Capped at 3GB (0xC0000000), not 4GB: firmware is mapped at the literal
// top of the 32-bit space (see fwBase in main()), and real PC platforms
// leave a "PCI hole" below 4GB for MMIO (PCI BARs, LAPIC, IOAPIC, HPET,
// firmware) rather than backing that whole range with RAM -- our AHCI
// ABAR and the firmware image itself both live in that gap. Going toward
// Windows boot support, which wants 4GB+ total, needs a *second*,
// separate high-memory region mapped above 4GB rather than just growing
// this one past the hole.
#define UEFI_GUEST_RAM_SIZE (3072ULL * 1024 * 1024)
void *uefiFirmwareMemory = NULL;   // backing for the mapped-at-top-of-4GB firmware image

// --- Minimal PCI host-bridge stub ---
// OVMF's PlatformPei reads PCI config space (bus 0/device 0/function 0)
// very early to tell an i440fx platform apart from Q35 and pick the right
// register layout. We don't emulate a real PCI bus -- just enough of the
// CONFIG_ADDRESS/CONFIG_DATA mechanism (ports 0xCF8/0xCFC) to answer that
// one probe as an i440fx host bridge (vendor 0x8086, device 0x1237) and
// report every other bus/device/function as absent (0xFFFFFFFF), matching
// real unpopulated PCI slots. See pciHandleConfigAccess.
UINT32 pciConfigAddress = 0;

// --- Minimal 16550 UART emulation (COM1, ports 0x3F8-0x3FF) ---
// Real UEFI console/terminal drivers (unlike the raw QEMU debugcon sink at
// port 0x402, which just accepts bytes unconditionally) speak the actual
// 16550 protocol: before writing to the transmit register (0x3F8), they
// poll the Line Status Register (0x3FD) for the "transmitter holding
// register empty" bit. Previously 0x3F8 was handled identically to 0x402
// (a bare byte sink) with no LSR/other-register support at all -- with no
// real UART behind it, always reporting "ready" is the simplest correct
// answer, so writes never block. Received data is always "none available"
// since nothing feeds real serial input in; this is output-only. See
// uartHandleAccess.
unsigned char uartIer = 0, uartLcr = 0, uartMcr = 0, uartScr = 0;
unsigned char uartDivisorLow = 0, uartDivisorHigh = 0;

// --- Second 16550 UART (COM2, ports 0x2F8-0x2FF), bridged to a Windows
// named pipe instead of our text log -- deliberately a separate port range
// from COM1 (which carries OVMF's own DEBUG() log output) so a real kernel
// debugger's binary KD protocol never shares a wire with human-readable
// ASCII log text. WinDbg attaches via `-k com:pipe,port=\\.\pipe\LocalHostKD,
// resets=0,reconnect`, and the guest is pointed at it with
// `bcdedit /set {bootmgr} bootdebug on` + `bcdedit /dbgsettings serial
// debugport:2 baudrate:115200` against the offline BCD store.
unsigned char uart2Ier = 0, uart2Lcr = 0, uart2Mcr = 0, uart2Scr = 0;
unsigned char uart2FifoEnabled = 0;
unsigned char uart2DivisorLow = 0, uart2DivisorHigh = 0;
HANDLE kdPipe = INVALID_HANDLE_VALUE;
long g_uart2TxTotal = 0, g_uart2Tx30 = 0, g_uart2Tx69 = 0, g_uart2Tx62 = 0; // U63: uncapped COM2 TX tallies

// U68: uncapped tallies for the pipe bridge itself. The guest transmits KD
// packets and kd reports [no_debuggee], but "guest wrote N bytes to COM2" says
// nothing about whether those bytes ever reached the debugger -- the ring
// buffer, the writer thread and WriteFile all sit in between, and any of them
// can silently swallow them (WriteFile on a pipe with no client fails with
// ERROR_PIPE_LISTENING, and its return value was being ignored). Every existing
// KD log in this file is capped, which is exactly how five earlier "finding"s in
// this investigation turned out to be nothing but a log that had stopped.
// These are never capped and are printed from the heartbeat.
long g_kdTxToPipe = 0;     // bytes WriteFile actually accepted
long g_kdTxWriteFail = 0;  // WriteFile calls that failed
long g_kdTxDropped = 0;    // guest THR writes dropped because kdTxBuf was full
DWORD g_kdTxLastErr = 0;   // GetLastError() from the most recent failed WriteFile
long g_kdRxFromPipe = 0;   // bytes read off the pipe (debugger -> us)
long g_kdRxToGuest = 0;    // bytes the guest actually consumed via RBR reads
long g_kdRxDropped = 0;    // pipe bytes dropped because kdRxBuf was full

// U69: deferred break-in. A debugger asks a running target to stop by sending a
// single 0x62 ('b') byte, which the kernel notices in KdPollBreakIn -- that is
// all Ctrl+Break in WinDbg actually does. We cannot press Ctrl+Break on a kd
// launched with redirected stdio, but we own the wire, so we can put the byte on
// it ourselves once the guest has booted far enough to be worth interrupting.
// That turns "break in at an arbitrary later moment and run !process 0 0" into
// something scriptable. Seconds since VM start; 0 disables.
long g_kdBreakinAtSec = 0;
int  g_kdBreakinSent = 0;

// U91: break in on GUEST PROGRESS rather than wall-clock seconds.
//
// Wall time turned out to be meaningless as a trigger. Two runs measured at the
// same elapsedSec=61s were at exitCount 1335000 and 305000 -- a 4.4x difference
// in how far the guest had actually got -- because host memory pressure slows
// the guest down enormously. A 75s break caught 12 processes; a *later* 95s
// break on a slower run caught only 2, which reads like the guest regressed when
// really the clock is just not measuring the guest.
//
// exitCount is a direct measure of guest work done, so a threshold on it is
// reproducible across runs regardless of how loaded the host is. Checked in the
// heartbeat, which fires every 5000 exits -- ample granularity. 0 disables.
long g_kdBreakinAtExits = 0;
CRITICAL_SECTION kdRxLock;
unsigned char kdRxBuf[4096];
volatile int kdRxHead = 0, kdRxTail = 0; // ring buffer: pipe reader thread -> guest RBR reads
CRITICAL_SECTION kdTxLock;
HANDLE kdTxEvent = NULL; // signaled whenever the writer thread should wake up and drain kdTxBuf
unsigned char kdTxBuf[4096];
volatile int kdTxHead = 0, kdTxTail = 0; // ring buffer: guest THR writes -> pipe writer thread
volatile int kdClientConnected = 0; // set by the reader thread once WinDbg attaches

// --- COM3 (0x3E8-0x3EF): the LocalHost Guest Tools channel -----------------
// A third UART, bridged to the host named pipe \.\pipe\LocalHostGT, carrying
// the Guest Tools protocol between the manager on the host and an agent
// running inside the guest.
//
// WHY A UART, RATHER THAN A BACKDOOR PORT OR A SOCKET
//   * A VMware-style magic I/O port has to be driven with IN/OUT, which on x64
//     Windows means ring 0, which means a SIGNED kernel driver in the guest and
//     a permanently test-signed install. A serial port is reachable from an
//     ordinary usermode program via CreateFile("\\.\COM3") through Windows'
//     in-box serial.sys, so the agent needs no driver at all.
//   * It does not depend on the NIC, DHCP, or the NAT backend, so it works in
//     early boot, during Setup, and while the network path is being changed
//     underneath it -- and a Guest Tools bug can never masquerade as a
//     networking one.
//
// HOW THIS DIFFERS FROM COM2, on which it is otherwise closely modelled:
// COM2 is POLLED-ONLY. Its IIR reports "no interrupt pending" unconditionally,
// which is harmless there because the kernel debugger drives the UART registers
// itself and polls them. serial.sys does NOT poll -- it attaches an ISR and
// waits on it. A COM3 that never asserted IRQ4 would enumerate correctly, open
// correctly, and then hang forever on the agent's first ReadFile. So this one
// implements real 16550 interrupt semantics; see uart3UpdateIrq.
unsigned char uart3Ier = 0, uart3Lcr = 0, uart3Mcr = 0, uart3Scr = 0;
unsigned char uart3FifoEnabled = 0;
unsigned char uart3DivisorLow = 0, uart3DivisorHigh = 0;
// THR-empty interrupt pending. Our transmit completes synchronously, so the
// underlying condition is permanently true; a real 16550 reports it as an EDGE
// and clears it when the CPU reads IIR or writes THR. Modelling it as a level
// would leave IRQ4 asserted forever and livelock the guest's ISR.
volatile int uart3ThreInt = 0;
HANDLE gtPipe = INVALID_HANDLE_VALUE;
CRITICAL_SECTION gtRxLock;
unsigned char gtRxBuf[8192];
volatile int gtRxHead = 0, gtRxTail = 0;  // pipe reader thread -> guest RBR reads
CRITICAL_SECTION gtTxLock;
HANDLE gtTxEvent = NULL;                  // wakes the writer thread to drain gtTxBuf
unsigned char gtTxBuf[8192];
volatile int gtTxHead = 0, gtTxTail = 0;  // guest THR writes -> pipe writer thread
volatile int gtClientConnected = 0;       // a host-side tools client is attached
long g_gtTxToPipe = 0, g_gtTxDropped = 0, g_gtTxWriteFail = 0;
DWORD g_gtTxLastErr = 0;
long g_gtRxFromPipe = 0, g_gtRxToGuest = 0, g_gtRxDropped = 0;
long g_gtIrqRaised = 0;

// --- EFI runtime instrumentation ---
// Inline hooks on bootmgfw.efi's own EfiOpenProtocol/EfiLocateHandleBuffer
// wrapper functions (RVAs found via real PDB symbols, see the scratchpad
// disassembly work), installed by patching their machine code directly in
// guest memory once bootmgfw's runtime load base is located (same
// memory-scan-for-a-known-string technique used for the earlier, abandoned
// KD attempt -- minus the hardware-breakpoint piece, which WHV silently
// failed to intercept). Traps through port 0xE2 -- the same "OUT triggers a
// VM exit, host reads full register state" mechanism already proven
// reliable everywhere else in this hypervisor -- rather than #DB/#BP
// exceptions, which are NOT intercepted by WHV in this environment
// (confirmed empirically: the guest's own exception handler caught them
// instead of us, crashing OVMF).
int efiHooksInstalled = 0;
int efiHookScanAttempted = 0;
UINT64 efiHookLoadBase = 0;
UINT64 efiHookScratchBase = 0; // guest GPA where trampoline code lives
UINT64 efiHookOpenProtoOrigAddr = 0;
UINT64 efiHookLocateHandleBufOrigAddr = 0;
UINT64 efiHookOpenProtoPreAddr = 0, efiHookOpenProtoPostAddr = 0;
UINT64 efiHookLHBPreAddr = 0, efiHookLHBPostAddr = 0;

// State captured at each hook's pre-call VM exit, read back at the
// corresponding post-call VM exit to correlate request and result (safe as
// a single global scratch slot since bootmgfw runs single-threaded/
// cooperatively on our one vCPU -- calls are properly nested, not
// concurrent).
UINT64 efiHookLastInterfaceOutAddr = 0;
int efiHookLastIsBlockIo = 0;
UINT64 efiHookLastNoHandlesOutAddr = 0;

// Real, standard UEFI spec GUIDs (bytes_le), confirmed against bootmgfw.efi's
// own embedded copies during static analysis (RVA 0xD528/0xD348) rather than
// assumed from memory.
const unsigned char efiBlockIoProtocolGuid[16] = {
    0x21,0x5B,0x4E,0x96, 0x59,0x64, 0x11,0xD2, 0x8E,0x39, 0x00,0xA0,0xC9,0x69,0x72,0x3B
};
const unsigned char efiDevicePathProtocolGuid[16] = {
    0x91,0x6E,0x57,0x09, 0x3F,0x6D, 0x11,0xD2, 0x8E,0x39, 0x00,0xA0,0xC9,0x69,0x72,0x3B
};

#define EFI_HOOK_NEEDLE_RVA 0x70B0
#define EFI_HOOK_OPENPROTO_RVA 0x58324
#define EFI_HOOK_LHB_RVA 0x58194

const unsigned char efiHookNeedle[] = {
    'B',0,'l',0,'I',0,'n',0,'i',0,'t',0,'i',0,'a',0,'l',0,'i',0,'z',0,
    'e',0,'L',0,'i',0,'b',0,'r',0,'a',0,'r',0,'y',0,' ',0,'f',0,'a',0,
    'i',0,'l',0,'e',0,'d',0
};

void efiHookWriteJmp(unsigned char *at, UINT64 fromAddr, UINT64 toAddr) {
    at[0] = 0xE9;
    INT32 rel = (INT32)(toAddr - (fromAddr + 5));
    memcpy(at + 1, &rel, 4);
}

// --- TEMP DIAGNOSTIC: identify which routine is hammering the CMOS RTC
// ports after BlpTimeInitialize succeeds. The RTC-validity fix (see
// cmosReadRtcField) resolved the original hard failure, but boot now
// crawls through extremely heavy, slow CMOS polling afterward -- a
// separate issue. This captures RIP (resolved to an RVA within
// bootmgfw.efi once we know its runtime load base, via the same PDB
// symbol table used to find BlpTimeInitialize) and the guest's current TSC
// value on a bounded number of CMOS accesses, cheaply: one one-shot memory
// scan for the load base (not repeated -- retries shouldn't recur now that
// the original failure is fixed), then only register reads already free
// at the existing CMOS port trap. No inline code patching this time.
int rtcDiagLoadBaseKnown = 0;
UINT64 rtcDiagLoadBase = 0;
int rtcDiagLogCount = 0;

void rtcDiagTryFindLoadBase(void) {
    if (rtcDiagLoadBaseKnown || !guestMemory) return;
    unsigned char *mem = (unsigned char *)guestMemory;
    UINT64 needleLen = sizeof(efiHookNeedle);
    UINT64 i, limit = guestMemSize > needleLen ? guestMemSize - needleLen : 0;
    for (i = 0; i < limit; i++) {
        if (mem[i] == efiHookNeedle[0] && memcmp(mem + i, efiHookNeedle, needleLen) == 0) {
            rtcDiagLoadBase = i - EFI_HOOK_NEEDLE_RVA;
            rtcDiagLoadBaseKnown = 1;
            printf("[rtcdiag] bootmgfw.efi load base=0x%llX\n", (unsigned long long)rtcDiagLoadBase);
            fflush(stdout);
            return;
        }
    }
}

// TEMP DIAGNOSTIC: RIP at the CMOS trap turned out to be outside
// bootmgfw.efi's range entirely (a huge, nonsensical RVA), meaning some
// OTHER module -- almost certainly the next-stage loader, given AHCI disk
// activity resumed right around the same point -- owns this code. Rather
// than guessing which file to extract and symbolicate, find the owning
// module directly in guest memory: scan backward from RIP for its PE
// header (MZ + "PE\0\0"), then read that module's own embedded RSDS debug
// directory entry (PDB filename) the same way pefile did for bootmgfw.efi,
// just done manually here since this is live guest memory, not a file on
// disk.
int rtcDiagModuleIdentified = 0;

void rtcDiagIdentifyModule(unsigned char *guestMem, UINT64 rip) {
    if (rtcDiagModuleIdentified) return;
    UINT64 scanStart = (rip > 0x2000000) ? rip - 0x2000000 : 0; // search up to 32MB back
    UINT64 addr;
    for (addr = (rip & ~0xFFFULL); addr + 0x400 < guestMemSize && addr >= scanStart; addr -= 0x1000) {
        if (guestMem[addr] == 'M' && guestMem[addr + 1] == 'Z') {
            UINT32 peOff = *(UINT32 *)(guestMem + addr + 0x3C);
            if (addr + peOff + 4 < guestMemSize &&
                guestMem[addr + peOff] == 'P' && guestMem[addr + peOff + 1] == 'E' &&
                guestMem[addr + peOff + 2] == 0 && guestMem[addr + peOff + 3] == 0) {
                printf("[rtcdiag] found PE header for owning module at guest addr 0x%llX (rip was 0x%llX, +0x%llX into it)\n",
                       (unsigned long long)addr, (unsigned long long)rip, (unsigned long long)(rip - addr));
                fflush(stdout);
                // Properly parse the PE32+ Optional Header's Data Directory
                // (a brute-force scan for the "RSDS" signature anywhere in
                // the image hit a false positive in code bytes first try).
                // DataDirectory starts at (PE sig)+4 + (FileHeader)20 + 112
                // = peOff+136; entry 6 (IMAGE_DIRECTORY_ENTRY_DEBUG) is 8
                // bytes at peOff+136+6*8 = peOff+184: {RVA(4), Size(4)}.
                UINT32 debugDirRva = *(UINT32 *)(guestMem + addr + peOff + 184);
                if (debugDirRva == 0 || addr + debugDirRva + 28 >= guestMemSize) {
                    printf("[rtcdiag] no debug directory RVA in this module's PE header\n");
                    fflush(stdout);
                    rtcDiagModuleIdentified = 1;
                    return;
                }
                // IMAGE_DEBUG_DIRECTORY: ... Type(4)@0xC, SizeOfData(4)@0x10, AddressOfRawData(4)@0x14
                unsigned char *dbgDir = guestMem + addr + debugDirRva;
                UINT32 dbgType = *(UINT32 *)(dbgDir + 0xC);
                UINT32 cvRva = *(UINT32 *)(dbgDir + 0x14);
                printf("[rtcdiag] debug directory: type=%u codeViewRva=0x%X\n", dbgType, cvRva);
                if (dbgType == 2 /* IMAGE_DEBUG_TYPE_CODEVIEW */ && cvRva != 0 && addr + cvRva + 24 < guestMemSize) {
                    unsigned char *cv = guestMem + addr + cvRva;
                    if (cv[0] == 'R' && cv[1] == 'S' && cv[2] == 'D' && cv[3] == 'S') {
                        char pdbName[128] = { 0 };
                        UINT64 nameOff = addr + cvRva + 4 + 16 + 4; // past signature, GUID, age
                        size_t k;
                        for (k = 0; k < sizeof(pdbName) - 1 && nameOff + k < guestMemSize; k++) {
                            char c = (char)guestMem[nameOff + k];
                            if (c == 0) break;
                            pdbName[k] = c;
                        }
                        printf("[rtcdiag] module PDB name: %s\n", pdbName);
                    } else {
                        printf("[rtcdiag] CodeView record doesn't start with RSDS (got %02X %02X %02X %02X)\n",
                               cv[0], cv[1], cv[2], cv[3]);
                    }
                }
                fflush(stdout);
                rtcDiagModuleIdentified = 1;
                return;
            }
        }
        if (addr < 0x1000) break;
    }
    printf("[rtcdiag] no PE header found scanning back from rip=0x%llX\n", (unsigned long long)rip);
    fflush(stdout);
    rtcDiagModuleIdentified = 1;
}

void rtcDiagLogAccess(WHV_PARTITION_HANDLE partition, UINT64 rip, unsigned char reg, int isWrite) {
    if (rtcDiagLogCount >= 80) return;
    rtcDiagLogCount++;
    if (rtcDiagLogCount == 1 && guestMemory) {
        rtcDiagIdentifyModule((unsigned char *)guestMemory, rip);
    }
    // TEMP DIAGNOSTIC: RIP alone only shows OVMF's own PcRtc driver code
    // (confirmed by its register-access sequence matching PcRtcGetTime()'s
    // real read order exactly), never the caller repeatedly invoking it --
    // dump the stack once to find a plausible return address instead.
    if (rtcDiagLogCount == 3 && guestMemory) {
        WHV_REGISTER_NAME rspName = WHvX64RegisterRsp;
        WHV_REGISTER_VALUE rspVal = { 0 };
        WHvGetVirtualProcessorRegisters(partition, 0, &rspName, 1, &rspVal);
        UINT64 rsp = rspVal.Reg64;
        unsigned char *mem = (unsigned char *)guestMemory;
        printf("[rtcdiag] stack dump at rsp=0x%llX (showing PcRtc-range hits + last one before each):\n", (unsigned long long)rsp);
        int i;
        int prevWasPcRtc = 0;
        for (i = 0; i < 800; i++) {
            UINT64 addr = rsp + (UINT64)i * 8;
            if (addr + 8 >= guestMemSize) break;
            UINT64 val = *(UINT64 *)(mem + addr);
            int looksLikePcRtcAddr = (val >= 0xBFD67000ULL && val < 0xBFD70000ULL);
            if (looksLikePcRtcAddr || (prevWasPcRtc && !looksLikePcRtcAddr)) {
                printf("[rtcdiag]   [rsp+0x%03X] = 0x%016llX%s\n", i * 8, (unsigned long long)val,
                       looksLikePcRtcAddr ? " (in PcRtc module)" : "  <-- first non-PcRtc after a PcRtc hit");
            }
            prevWasPcRtc = looksLikePcRtcAddr;
        }
        fflush(stdout);
    }
    WHV_REGISTER_NAME tscName = WHvX64RegisterTsc;
    WHV_REGISTER_VALUE tscVal = { 0 };
    WHvGetVirtualProcessorRegisters(partition, 0, &tscName, 1, &tscVal);
    UINT64 rva = rtcDiagLoadBaseKnown ? (rip - rtcDiagLoadBase) : 0;
    printf("[rtcdiag] #%d rip=0x%llX rva=0x%llX reg=0x%02X %s tsc=%llu\n",
           rtcDiagLogCount, (unsigned long long)rip, (unsigned long long)rva, reg,
           isWrite ? "write" : "read", (unsigned long long)tscVal.Reg64);
    fflush(stdout);
}

// TEMP DIAGNOSTIC: the OpenProtocol/LocateHandleBuffer hooks below never
// fired in testing -- both installed successfully but the guest reached its
// "give up, show boot menu" fallback without ever hitting them, meaning the
// real failure happens *before* BlpIoInitialize (which is what calls down
// into the OpenProtocol/LocateHandleBuffer chain) even runs. These
// checkpoints watch every `test eax,eax` that immediately follows each of
// InitializeLibrary's other early-init calls (BlpFwInitialize,
// BlpArchInitialize, BlpMmInitialize, BlpTimeInitialize), all confirmed via
// the same PDB-symbol-verified disassembly as the rest of this call graph,
// to find exactly which one first returns a negative (error) EAX -- none of
// them need fresh disk I/O either, consistent with the "zero new AHCI
// commands before the failure" constraint established earlier.
typedef struct { UINT32 rva; const char *label; } EfiCheckpoint;
EfiCheckpoint efiCheckpoints[] = {
    { 0x19F046, "after BlpFwInitialize(1st)" },
    { 0x19F151, "after BlpArchInitialize(1st)" },
    { 0x19F169, "after BlpMmInitialize" },
    { 0x19F1FA, "after BlpFwInitialize(2nd)" },
    { 0x19F209, "after BlpTimeInitialize" },
    { 0x19F21B, "after BlpArchInitialize(2nd)" },
    { 0x19F325, "after BlpIoInitialize" },
};
#define EFI_CHECKPOINT_COUNT (sizeof(efiCheckpoints) / sizeof(efiCheckpoints[0]))
// Multiple copies of bootmgfw.efi can be resident in guest memory at once
// (confirmed empirically: 3 simultaneous matches found for the same needle
// string, roughly one image-size apart -- almost certainly BDS loading
// several candidate boot files, e.g. root \bootmgfw.efi, \EFI\BOOT\
// BOOTX64.EFI, and \EFI\Microsoft\Boot\bootmgfw.efi, to validate them
// during boot-option discovery, even though only one is ever actually
// executed). Patching only the first-found copy meant our hooks sat on a
// copy that was never the one actually running, and never fired.
#define EFI_HOOK_MAX_COPIES 8
int efiHookCopyCount = 0;
UINT64 efiCheckpointHookAddr[EFI_HOOK_MAX_COPIES][EFI_CHECKPOINT_COUNT];   // origAddr -- what the port handler matches RIP against
UINT64 efiCheckpointResumeAddr[EFI_HOOK_MAX_COPIES][EFI_CHECKPOINT_COUNT]; // trampoline address to resume at instead of origAddr+2

void efiHookWriteCall(unsigned char *at, UINT64 fromAddr, UINT64 toAddr) {
    at[0] = 0xE8;
    INT32 rel = (INT32)(toAddr - (fromAddr + 5));
    memcpy(at + 1, &rel, 4);
}

// Scans guest RAM once for a string known (from static analysis of the
// extracted bootmgfw.efi) to live at a fixed offset from that image's own
// load base -- string bytes are never relocated, only pointers to them are
// -- then patches EfiOpenProtocol/EfiLocateHandleBuffer's entry points to
// jump into trampolines that log via port 0xE2 before/after calling the
// real, relocated original code.
void tryInstallEfiHooks(void) {
    if (!guestMemory) return;
    unsigned char *mem = (unsigned char *)guestMemory;
    UINT64 needleLen = sizeof(efiHookNeedle);
    UINT64 i, limit = guestMemSize > needleLen ? guestMemSize - needleLen : 0;
    UINT64 loadBases[EFI_HOOK_MAX_COPIES];
    int matchCount = 0;
    for (i = 0; i < limit && matchCount < EFI_HOOK_MAX_COPIES; i++) {
        if (mem[i] == efiHookNeedle[0] && memcmp(mem + i, efiHookNeedle, needleLen) == 0) {
            loadBases[matchCount] = i - EFI_HOOK_NEEDLE_RVA;
            printf("[efihook] copy #%d: needle at 0x%llX -> loadBase=0x%llX\n",
                   matchCount + 1, (unsigned long long)i, (unsigned long long)loadBases[matchCount]);
            matchCount++;
        }
    }
    fflush(stdout);
    if (matchCount == 0) {
        printf("[efihook] bootmgfw.efi load base not found in guest memory\n");
        fflush(stdout);
        return;
    }
    efiHookCopyCount = matchCount;
    efiHookLoadBase = loadBases[0]; // kept for the (currently disabled) single-copy OpenProtocol/LHB hooks below

    efiHookOpenProtoOrigAddr = efiHookLoadBase + EFI_HOOK_OPENPROTO_RVA;
    efiHookLocateHandleBufOrigAddr = efiHookLoadBase + EFI_HOOK_LHB_RVA;
    // Scratch region for trampoline code. NOTE: guestMemSize-0x2000 (right at
    // the very top of guest RAM) was tried first and crashed OVMF -- a crash
    // dump showed the GDT at 0xBFD50000 and a live stack around 0xBBE1xxxx,
    // both within a few hundred KB of that address, meaning firmware/boot
    // manager actively uses the top of RAM for these structures and our
    // write smashed one of them. 0x08000000 (128MB) is far from both that
    // high-memory working set and the low-1MB legacy area.
    efiHookScratchBase = 0x08000000;

#if 0 // TEMP DIAGNOSTIC: disabled to isolate whether these hooks interfere
      // with the checkpoint hooks below -- neither ever fired with both
      // installed, so testing the checkpoints alone first.
    UINT64 openProtoStub = efiHookScratchBase + 0x000;
    efiHookOpenProtoPreAddr = efiHookScratchBase + 0x020;
    UINT64 openProtoCallSite = efiHookOpenProtoPreAddr + 2; // right after the pre "out"
    efiHookOpenProtoPostAddr = openProtoCallSite + 5;        // right after "call stub"

    UINT64 lhbStub = efiHookScratchBase + 0x040;
    efiHookLHBPreAddr = efiHookScratchBase + 0x060;
    UINT64 lhbCallSite = efiHookLHBPreAddr + 2;
    efiHookLHBPostAddr = lhbCallSite + 5;

    // --- OpenProtocol prologue stub: relocated original 5 bytes (a single
    // "mov [rsp+8], rbx" instruction, confirmed via disassembly) + jmp back
    // into the real function body right after that instruction. ---
    unsigned char stubBuf[16] = { 0 };
    memcpy(stubBuf, mem + efiHookOpenProtoOrigAddr, 5);
    efiHookWriteJmp(stubBuf + 5, openProtoStub + 5, efiHookOpenProtoOrigAddr + 5);
    memcpy(mem + openProtoStub, stubBuf, 10);

    // --- OpenProtocol trampoline: out (pre-log); call stub; out (post-log); ret ---
    unsigned char tramp[16] = { 0 };
    tramp[0] = 0xE6; tramp[1] = 0xE2; // out 0xE2, al
    efiHookWriteCall(tramp + 2, efiHookOpenProtoPreAddr + 2, openProtoStub);
    tramp[7] = 0xE6; tramp[8] = 0xE2;
    tramp[9] = 0xC3; // ret
    memcpy(mem + efiHookOpenProtoPreAddr, tramp, 10);

    // --- patch the real OpenProtocol entry to jump into our trampoline ---
    unsigned char patch[8] = { 0 };
    efiHookWriteJmp(patch, efiHookOpenProtoOrigAddr, efiHookOpenProtoPreAddr);
    memcpy(mem + efiHookOpenProtoOrigAddr, patch, 5);

    // --- LocateHandleBuffer prologue stub: relocated original 7 bytes
    // ("mov rax,rsp" + "mov [rax+8],rbx", confirmed via disassembly) + jmp back. ---
    unsigned char stubBuf2[20] = { 0 };
    memcpy(stubBuf2, mem + efiHookLocateHandleBufOrigAddr, 7);
    efiHookWriteJmp(stubBuf2 + 7, lhbStub + 7, efiHookLocateHandleBufOrigAddr + 7);
    memcpy(mem + lhbStub, stubBuf2, 12);

    // --- LocateHandleBuffer trampoline ---
    unsigned char tramp2[16] = { 0 };
    tramp2[0] = 0xE6; tramp2[1] = 0xE2;
    efiHookWriteCall(tramp2 + 2, efiHookLHBPreAddr + 2, lhbStub);
    tramp2[7] = 0xE6; tramp2[8] = 0xE2;
    tramp2[9] = 0xC3;
    memcpy(mem + efiHookLHBPreAddr, tramp2, 10);

    // --- patch the real LocateHandleBuffer entry (7 bytes: 5-byte jmp + 2 NOPs) ---
    unsigned char patch2[8] = { 0 };
    efiHookWriteJmp(patch2, efiHookLocateHandleBufOrigAddr, efiHookLHBPreAddr);
    patch2[5] = 0x90; patch2[6] = 0x90;
    memcpy(mem + efiHookLocateHandleBufOrigAddr, patch2, 7);
#endif

    // --- checkpoint hooks: in-place 2-byte "test eax,eax" -> "out 0xE2,al"
    // swap (same size, no relocation needed for the 2-byte patch site
    // itself -- there isn't room there for a 5-byte jmp rel32). The host
    // port handler recognizes origAddr directly (that's the only guest
    // code that actually executes and traps), logs EAX, then resumes
    // execution *at the trampoline* (not origAddr+2) so the relocated
    // test+jmp-back runs and the real conditional branch that follows
    // still sees correct flags. Installed identically across every
    // discovered copy of bootmgfw.efi, since only one is ever actually
    // executed but we don't know which in advance. ---
    for (int copyIdx = 0; copyIdx < efiHookCopyCount; copyIdx++) {
        for (size_t ci = 0; ci < EFI_CHECKPOINT_COUNT; ci++) {
            UINT64 origAddr = loadBases[copyIdx] + efiCheckpoints[ci].rva;
            UINT64 tramp3Addr = efiHookScratchBase + 0x100 + (copyIdx * EFI_CHECKPOINT_COUNT + ci) * 0x20;
            efiCheckpointHookAddr[copyIdx][ci] = origAddr;
            efiCheckpointResumeAddr[copyIdx][ci] = tramp3Addr;

            unsigned char tramp3[16] = { 0 };
            memcpy(tramp3, mem + origAddr, 2);                          // relocated "test eax,eax"
            efiHookWriteJmp(tramp3 + 2, tramp3Addr + 2, origAddr + 2);  // jmp back
            memcpy(mem + tramp3Addr, tramp3, 7);

            unsigned char cpPatch[2] = { 0xE6, 0xE2 }; // out 0xE2, al (in place, same size)
            memcpy(mem + origAddr, cpPatch, 2);
        }
    }

    efiHooksInstalled = 1;
    printf("[efihook] installed checkpoint hooks across %d copies, scratch=0x%llX\n",
           efiHookCopyCount, (unsigned long long)efiHookScratchBase);
    fflush(stdout);
}

void efiHookFormatGuid(unsigned char *guestMem, UINT64 addr, char *outBuf, size_t outBufSize) {
    if (addr == 0 || addr + 16 > guestMemSize) { _snprintf_s(outBuf, outBufSize, _TRUNCATE, "(null)"); return; }
    unsigned char *g = guestMem + addr;
    _snprintf_s(outBuf, outBufSize, _TRUNCATE,
                "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6],
                g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

void efiHookLogBlockIoMedia(unsigned char *guestMem, UINT64 interfacePtr) {
    if (interfacePtr == 0 || interfacePtr + 0x20 > guestMemSize) {
        printf("[efihook]   (interface ptr null/out-of-range: 0x%llX)\n", (unsigned long long)interfacePtr);
        return;
    }
    UINT64 mediaPtr = *(UINT64 *)(guestMem + interfacePtr + 8); // EFI_BLOCK_IO_PROTOCOL->Media
    if (mediaPtr == 0 || mediaPtr + 0x20 > guestMemSize) {
        printf("[efihook]   Media ptr null/out-of-range: 0x%llX\n", (unsigned long long)mediaPtr);
        return;
    }
    unsigned char *m = guestMem + mediaPtr;
    UINT32 mediaId = *(UINT32 *)(m + 0);
    unsigned char removable = m[4], present = m[5], logicalPartition = m[6], readOnly = m[7];
    UINT32 blockSize = *(UINT32 *)(m + 0xC);
    UINT64 lastBlock = *(UINT64 *)(m + 0x18);
    printf("[efihook]   Media@0x%llX: MediaId=0x%X Removable=%d Present=%d LogicalPartition=%d ReadOnly=%d BlockSize=%u LastBlock=%llu\n",
           (unsigned long long)mediaPtr, mediaId, removable, present, logicalPartition, readOnly,
           blockSize, (unsigned long long)lastBlock);
    fflush(stdout);
}

void efiHookHandleAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    UINT64 rip = exitContext->VpContext.Rip;
    unsigned char *mem = (unsigned char *)guestMemory;

    WHV_REGISTER_NAME regNames[6] = { WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterR8,
                                       WHvX64RegisterR9, WHvX64RegisterRax, WHvX64RegisterRsp };
    WHV_REGISTER_VALUE regVals[6] = { 0 };
    WHvGetVirtualProcessorRegisters(partition, 0, regNames, 6, regVals);
    UINT64 rcx = regVals[0].Reg64, rdx = regVals[1].Reg64, r8 = regVals[2].Reg64,
           r9 = regVals[3].Reg64, rax = regVals[4].Reg64, rsp = regVals[5].Reg64;

    static int logCount = 0;
    int shouldLog = (logCount < 500);

    if (rip == efiHookOpenProtoPreAddr) {
        efiHookLastInterfaceOutAddr = r8;
        char guidStr[40];
        efiHookFormatGuid(mem, rdx, guidStr, sizeof(guidStr));
        efiHookLastIsBlockIo = (rdx + 16 <= guestMemSize && memcmp(mem + rdx, efiBlockIoProtocolGuid, 16) == 0);
        int isDevicePath = (rdx + 16 <= guestMemSize && memcmp(mem + rdx, efiDevicePathProtocolGuid, 16) == 0);
        if (shouldLog) {
            logCount++;
            printf("[efihook] #%d OpenProtocol(Handle=0x%llX, Protocol=%s%s%s, InterfaceOut=0x%llX, Agent=0x%llX)\n",
                   logCount, (unsigned long long)rcx, guidStr,
                   efiHookLastIsBlockIo ? " [BlockIo]" : "", isDevicePath ? " [DevicePath]" : "",
                   (unsigned long long)r8, (unsigned long long)r9);
            fflush(stdout);
        }
    } else if (rip == efiHookOpenProtoPostAddr) {
        if (shouldLog) {
            printf("[efihook]   -> status=0x%llX%s\n", (unsigned long long)rax,
                   rax == 0 ? " (SUCCESS)" : (rax == 0x8000000000000007ULL ? " (EFI_DEVICE_ERROR !!!)" : ""));
            if (rax == 0 && efiHookLastIsBlockIo && efiHookLastInterfaceOutAddr + 8 <= guestMemSize) {
                UINT64 ifacePtr = *(UINT64 *)(mem + efiHookLastInterfaceOutAddr);
                printf("[efihook]   Interface=0x%llX\n", (unsigned long long)ifacePtr);
                efiHookLogBlockIoMedia(mem, ifacePtr);
            }
            fflush(stdout);
        }
    } else if (rip == efiHookLHBPreAddr) {
        efiHookLastNoHandlesOutAddr = r9;
        char guidStr[40];
        efiHookFormatGuid(mem, rdx, guidStr, sizeof(guidStr));
        int isBlockIo = (rdx + 16 <= guestMemSize && memcmp(mem + rdx, efiBlockIoProtocolGuid, 16) == 0);
        if (shouldLog) {
            logCount++;
            printf("[efihook] #%d LocateHandleBuffer(SearchType=%llu, Protocol=%s%s, SearchKey=0x%llX)\n",
                   logCount, (unsigned long long)rcx, guidStr, isBlockIo ? " [BlockIo]" : "",
                   (unsigned long long)r8);
            fflush(stdout);
        }
    } else if (rip == efiHookLHBPostAddr) {
        if (shouldLog) {
            UINT64 noHandles = (efiHookLastNoHandlesOutAddr + 8 <= guestMemSize)
                                    ? *(UINT64 *)(mem + efiHookLastNoHandlesOutAddr) : 0;
            printf("[efihook]   -> status=0x%llX%s NoHandles=%llu\n", (unsigned long long)rax,
                   rax == 0 ? " (SUCCESS)" : (rax == 0x8000000000000007ULL ? " (EFI_DEVICE_ERROR !!!)" : ""),
                   (unsigned long long)noHandles);
            fflush(stdout);
        }
    }

    UINT64 resumeAt = rip + exitContext->VpContext.InstructionLength; // default: advance past the 2-byte "out"
    for (int copyIdx = 0; copyIdx < efiHookCopyCount; copyIdx++) {
        for (size_t ci = 0; ci < EFI_CHECKPOINT_COUNT; ci++) {
            if (rip == efiCheckpointHookAddr[copyIdx][ci]) {
                printf("[efihook-cp] copy#%d %s: eax=0x%llX%s\n", copyIdx + 1, efiCheckpoints[ci].label,
                       (unsigned long long)rax, (rax & 0x80000000ULL) ? " (NEGATIVE/ERROR)" : " (ok)");
                fflush(stdout);
                resumeAt = efiCheckpointResumeAddr[copyIdx][ci]; // run the relocated test+jmp-back, not origAddr+2
                goto efiHookCheckpointFound;
            }
        }
    }
efiHookCheckpointFound:;

    WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
    WHV_REGISTER_VALUE newRip = { 0 };
    newRip.Reg64 = resumeAt;
    WHvSetVirtualProcessorRegisters(partition, 0, &ripName, 1, &newRip);
}

void updateA20Mapping(WHV_PARTITION_HANDLE partition) {
    a20RemapCount++;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    WHvUnmapGpaRange(partition, A20_WINDOW_BASE, A20_WINDOW_SIZE);
    void *target = a20Enabled ? hmaMemory : guestMemory;
    WHvMapGpaRange(partition, target, A20_WINDOW_BASE, A20_WINDOW_SIZE,
                   WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);

    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
    if (a20RemapCount <= 20 || a20RemapCount % 500 == 0) {
        printf("[a20 remap #%d took %.3f ms, now %s]\n", a20RemapCount, ms, a20Enabled ? "enabled" : "disabled");
        fflush(stdout);
    }
}
UINT16 pitChannel2Reload = 0;
unsigned char pitChannel2AccessMode = 0; // bits 5-4 of the port 0x43 command byte
unsigned char pitChannel2LowByte = 0;
int pitChannel2WritePhase = 0;    // 0 = expect low/only byte, 1 = expect high byte
int pitChannel2Loaded = 0;
LARGE_INTEGER pitChannel2LoadTime;

// --- HLT / interrupt handling state ---
// cpuHalted: set when the guest executes HLT. We don't stop the hypervisor,
// we just stop calling WHvRunVirtualProcessor until we have something to
// inject (keyboard data or a timer tick), then resume normally.
int cpuHalted = 0;
// HOW MUCH WALL TIME IS THE GUEST ACTUALLY RUNNING?
//
// Setup copies at ~19 KB/s while its disk is idle, every interrupt is delivered,
// and the exit rate (~667/sec, ~1% of a core) is far too low to be the cost. So
// either the guest is computing slowly, or it is HALTED most of the time and only
// does work when something wakes it. Those need opposite fixes and no counter so
// far distinguishes them. Accumulate the two directly.
double g_haltedMs = 0.0, g_runningMs = 0.0;
long g_haltEntries = 0;
LARGE_INTEGER lastTimerTick;
#define TIMER_TICK_INTERVAL_MS 54.925 // ~18.2 Hz, classic PC/PIT default rate

// --- Enough 8259 and PIT channel 0 state to know when the guest wants ticks ---
//
// NOT a PIC emulation: no priority, no IRR/ISR, no EOI. The only question being
// answered is whether the firmware has said it is ready for timer interrupts,
// because injecting IRQ0 before that wedges it -- measured, exit count frozen at
// 8571 for 280s when the tick was delivered unconditionally from the main loop.
//
// Two conditions say "ready": PIT channel 0 has been programmed with a count,
// and the master PIC has IRQ0 unmasked. Neither was visible before -- ports
// 0x20/0x21 were not decoded at all, and port 0x43 only handled channel 2 -- so
// there was no way to tell readiness from a guest that had not touched either.
unsigned char pic1Mask = 0xFF;        // OCW1; everything masked at power-on
unsigned char pic1VectorBase = 0x08;  // ICW2; OVMF remaps IRQ0-7 here
unsigned char pic1InitStage = 0;      // 0=operational, 1=ICW2, 2=ICW3, 3=ICW4
unsigned char pic1Icw1 = 0;
int pitChannel0Programmed = 0;        // channel 0 has been given a reload count
unsigned char pitChannel0AccessMode = 0;
int pitChannel0WritePhase = 0;

// --- RTC (MC146818) periodic interrupt emulation ---
// We already emulate CMOS/RTC time/date register reads (see
// cmosReadRtcField), but never generated the periodic interrupt on IRQ8
// that real hardware fires when Register B's PIE bit is set -- confirmed
// live (see docs/investigations/vppt-synic-blocker.md part 10) to be
// exactly what Windows HAL's HalpTimerWaitForPhase0Interrupt is waiting
// on: it configures the IOAPIC redirection entry for GSI 8 with vector
// 0xD1, unmasked, then busy-waits up to 3 seconds for that interrupt to
// actually fire. Since nothing ever raised it, HAL gave up and bugchecked
// (0x5C, STATUS_UNSUCCESSFUL) every single time. Rate-select-to-period
// table per the standard MC146818 divider formula (period = 2^(rate-1) /
// 32768 seconds, valid for rate 3-15; rates 0-2 mean "no periodic output").
LARGE_INTEGER lastRtcPeriodicTick;
int rtcPeriodicTickArmed = 0;
double rtcPeriodicIntervalMs(unsigned char regA) {
    int rate = regA & 0x0F;
    if (rate < 3) return 0.0; // periodic interrupt disabled by this rate value
    return (double)(1 << (rate - 1)) / 32768.0 * 1000.0;
}

// --- Primary ATA/IDE controller emulation (LBA28 PIO, master drive only) ---
// Backs the guest's virtual hard disk with a per-VM raw image file. Commands
// complete synchronously (no simulated seek latency): a command written to
// 0x1F7 performs the whole file I/O immediately and leaves the result sitting
// in ataDataBuffer for the guest to stream out/in via 0x1F0, then fires IRQ14
// (vector 0x76, the real-mode default for the primary ATA controller).
FILE *ataDiskFile = NULL;
UINT64 ataDiskSectors = 0;
// 512 for a plain raw/VHD disk image; 2048 for an ISO -- EDK2's PartitionDxe
// El Torito parser (MdeModulePkg/Universal/Disk/PartitionDxe/ElTorito.c)
// hard-requires BlockIo->Media->BlockSize == 2048 before it will even
// attempt to recognize a CD-ROM boot catalog, which is how Windows
// installer media conventionally gets consumed -- without this, boot still
// partially works via UdfDxe's direct UDF/ISO9660 traversal (proven: files
// get found and read correctly, verified byte-for-byte against the source
// ISO), but something downstream (likely bootmgfw.efi itself, once
// started) fails with a generic Device Error, plausibly because it expects
// the conventional El-Torito-mediated boot path.
UINT32 ataSectorSize = 512;

// --- AHCI: one entry per implemented port ---------------------------------
//
// The controller used to implement a single port, which meant an ISO and a hard
// disk could not both be attached: booting installer media left the installer
// with nothing to install ONTO. Two ports is the minimum that makes an OS
// install possible -- boot the ISO on one, write to the disk on the other.
//
// Port 0 is the BOOT device (the ISO when one is configured, otherwise the hard
// disk); port 1 carries the other one. Firmware enumerates in port order, so
// putting the boot medium first is what makes an attached ISO take precedence
// without needing a boot-order setting.
//
// ataDiskFile/ataDiskSectors/ataSectorSize above stay as they were and track
// PORT 0. The legacy IDE path (ports 0x1F0-0x1F7) is a separate device model
// that only ever exposes one drive, and rewiring it is not needed to install an
// OS -- firmware and Windows both drive the AHCI controller.
// (AHCI_PORT_COUNT and AhciPortDevice are declared near ahciServiceLevelInterrupt,
// which needs them earlier in the file.)
AhciPortDevice ahciPorts[AHCI_PORT_COUNT];

static int ahciPortsImplemented(void) {
    int i, n = 0;
    for (i = 0; i < AHCI_PORT_COUNT; i++) if (ahciPorts[i].present) n++;
    return n;
}

unsigned char ataFeatures = 0;
unsigned char ataSectorCount = 1;
unsigned char ataLbaLow = 0, ataLbaMid = 0, ataLbaHigh = 0;
unsigned char ataDriveHead = 0xA0;
unsigned char ataStatus = 0x50;   // DRDY | DSC
unsigned char ataError = 0;

// The legacy PIO path's sector-count register is 8-bit (max 256 sectors =
// 128KB per command, hardware-enforced there regardless of this constant),
// but AHCI has no such limit -- a single command's FIS sector count is
// 16-bit and, more importantly, PRDT entries can be chained for
// multi-megabyte transfers in one command. EDK2's disk I/O layer can and
// does coalesce a file's cluster chain into large single reads (observed:
// a ~1.1MB EFI binary load failing to find/load at all once its read size
// exceeded the old 128KB cap here -- silently rejected as ok=0, which
// bubbled up as a generic "Not Found" rather than an obvious I/O error).
#define ATA_MAX_TRANSFER (8 * 1024 * 1024)
unsigned char ataDataBuffer[ATA_MAX_TRANSFER];
UINT32 ataDataLen = 0;
UINT32 ataDataPos = 0;
int ataDataIsWrite = 0;
UINT32 ataPendingLba = 0;

#define ATA_ST_BSY  0x80
#define ATA_ST_DRDY 0x40
#define ATA_ST_DSC  0x10  // Drive Seek Complete -- some drivers wait for
                          // this alongside DRDY before proceeding; our
                          // synchronous emulation never has a seek "in
                          // progress" so it's always set once ready.
#define ATA_ST_DRQ  0x08
#define ATA_ST_ERR  0x01
#define ATA_ERR_ABRT 0x04
#define ATA_IRQ14_VECTOR 0x76

UINT32 ataCurrentLba(void) {
    return (UINT32)ataLbaLow | ((UINT32)ataLbaMid << 8) | ((UINT32)ataLbaHigh << 16)
           | ((UINT32)(ataDriveHead & 0x0F) << 24);
}

// Bit 4 of the drive/head register (0x1F6) selects master (0) vs slave (1).
// We only emulate a single drive on the primary channel's master position --
// the slave must consistently look absent (floating bus, all-1s reads) or
// the guest sees two identical phantom drives instead of one real one.
int ataSlaveSelected(void) {
    return (ataDriveHead & 0x10) != 0;
}

void ataPutString(unsigned char *dst, const char *src, int len) {
    int srcLen = (int)strlen(src);
    int i;
    for (i = 0; i < len; i += 2) {
        unsigned char c0 = (i < srcLen) ? (unsigned char)src[i] : ' ';
        unsigned char c1 = (i + 1 < srcLen) ? (unsigned char)src[i + 1] : ' ';
        dst[i] = c1;
        dst[i + 1] = c0;
    }
}

void ataFillIdentify(unsigned char *buf, UINT64 sectors) {
    UINT16 id[256];
    UINT32 lba28;
    UINT32 heads, sectorsPerTrack, cylinders;
    memset(id, 0, sizeof(id));

    lba28 = (UINT32)((sectors > 0x0FFFFFFF) ? 0x0FFFFFFF : sectors);

    // Legacy CHS geometry, derived from the actual LBA capacity so it's at
    // least self-consistent -- some BIOSes sanity-check reported CHS against
    // the LBA sector count and distrust/skip a drive where they wildly
    // disagree (e.g. a fixed 16383/16/63 placeholder next to a tiny image).
    heads = 16;
    sectorsPerTrack = 63;
    cylinders = lba28 / (heads * sectorsPerTrack);
    if (cylinders > 16383) cylinders = 16383;
    if (cylinders == 0) cylinders = 1;

    id[0] = 0x0040; // fixed, non-removable ATA device
    id[1] = (UINT16)cylinders;
    id[3] = (UINT16)heads;
    id[6] = (UINT16)sectorsPerTrack;
    ataPutString((unsigned char *)&id[10], "LH0001", 20);                  // serial number
    ataPutString((unsigned char *)&id[23], "1.0", 8);                      // firmware revision
    ataPutString((unsigned char *)&id[27], "LocalHost Virtual Disk", 40);  // model number
    id[49] = 0x0200; // bit9: LBA supported

    // "Current" CHS translation + capacity (words 53-58). Some BIOS ATA
    // drivers read *these* for capacity instead of the total-user-sectors
    // field (60-61) when word 53 bit0 claims them valid -- leaving them
    // zeroed (with the validity bit unset) can make a drive look like it
    // has 0 usable sectors and get silently skipped as a boot candidate.
    id[53] = 0x0003; // bit0: words 54-58 valid, bit1: words 64-70 valid
    id[54] = (UINT16)cylinders;
    id[55] = (UINT16)heads;
    id[56] = (UINT16)sectorsPerTrack;
    id[57] = (UINT16)(lba28 & 0xFFFF);
    id[58] = (UINT16)(lba28 >> 16);

    id[60] = (UINT16)(lba28 & 0xFFFF);
    id[61] = (UINT16)(lba28 >> 16);

    memcpy(buf, id, 512);
}

void ataCompleteWithError(WHV_PARTITION_HANDLE partition) {
    ataStatus = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_ERR;
    ataError = ATA_ERR_ABRT;
    ataDataLen = 0;
    ataDataPos = 0;
    ataMaybeInjectIrq(partition);
}

void ataHandleCommand(WHV_PARTITION_HANDLE partition, unsigned char cmd) {
    // A real absent slave drive doesn't respond to commands at all -- no
    // status change, no IRQ. Silently drop it here so probing the slave
    // never looks like a second copy of our one real (master) disk.
    if (ataSlaveSelected()) return;

    UINT32 lba = ataCurrentLba();
    UINT32 count = ataSectorCount == 0 ? 256 : ataSectorCount;
    printf("[ata] cmd=0x%02X drivehead=0x%02X lba=%u count=%u diskFile=%p diskSectors=%llu\n",
           cmd, ataDriveHead, lba, count, (void *)ataDiskFile, (unsigned long long)ataDiskSectors);
    fflush(stdout);

    switch (cmd) {
        case 0xEC: // IDENTIFY DEVICE
            if (!ataDiskFile) { ataCompleteWithError(partition); break; }
            ataFillIdentify(ataDataBuffer, ataDiskSectors);
            ataDataLen = 512;
            ataDataPos = 0;
            ataDataIsWrite = 0;
            ataStatus = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_DRQ;
            ataMaybeInjectIrq(partition);
            break;

        case 0x20: case 0x21: // READ SECTORS (with/without retry)
            if (!ataDiskFile || (UINT64)(lba + count) > ataDiskSectors || count * 512 > ATA_MAX_TRANSFER) {
                ataCompleteWithError(partition);
                break;
            }
            _fseeki64(ataDiskFile, (long long)lba * 512, SEEK_SET);
            fread(ataDataBuffer, 1, (size_t)count * 512, ataDiskFile);
            ataDataLen = count * 512;
            ataDataPos = 0;
            ataDataIsWrite = 0;
            ataStatus = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_DRQ;
            ataMaybeInjectIrq(partition);
            printf("[ata] READ SECTORS ok: dataLen=%u status=0x%02X\n", ataDataLen, ataStatus);
            fflush(stdout);
            break;

        case 0x30: case 0x31: // WRITE SECTORS (with/without retry)
            if (!ataDiskFile || (UINT64)(lba + count) > ataDiskSectors || count * 512 > ATA_MAX_TRANSFER) {
                ataCompleteWithError(partition);
                break;
            }
            ataPendingLba = lba;
            ataDataLen = count * 512;
            ataDataPos = 0;
            ataDataIsWrite = 1;
            ataStatus = ATA_ST_DRDY | ATA_ST_DSC | ATA_ST_DRQ;
            // No IRQ yet -- fired once the guest finishes pushing the data (below).
            break;

        case 0xE7: case 0xEA: // FLUSH CACHE / FLUSH CACHE EXT
            if (ataDiskFile) fflush(ataDiskFile);
            ataStatus = ATA_ST_DRDY | ATA_ST_DSC;
            ataMaybeInjectIrq(partition);
            break;

        case 0x91: // INITIALIZE DEVICE PARAMETERS
            ataStatus = ATA_ST_DRDY | ATA_ST_DSC;
            ataMaybeInjectIrq(partition);
            break;

        default:
            ataCompleteWithError(partition);
            break;
    }
}

// Port 0x1F0 (ATA data register) carries the actual sector/IDENTIFY payload.
// Real ATA drivers move that data with `rep insw`/`rep outsw`, which WHV
// exits *once* for the whole repeated operation -- Rcx holds the repeat
// count and Rsi/Rdi/Ds/Es locate the guest-memory buffer directly, rather
// than routing anything through Rax. Handling this as a plain register-value
// IN/OUT (like every other port here) silently transfers nothing.
void ataHandlePioDataPort(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext, void *guestMemory) {
    WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitContext->IoPortAccess;

    if (!io->AccessInfo.StringOp) {
        // Plain `in ax, dx` / `out dx, ax` -- transfer through Rax like every
        // other emulated port, no guest-memory buffer involved.
        UINT16 word = 0xFFFF;
        if (io->AccessInfo.IsWrite) {
            word = (UINT16)io->Rax;
            if (ataDataIsWrite && ataDataPos < ataDataLen) {
                ataDataBuffer[ataDataPos++] = (unsigned char)(word & 0xFF);
                if (io->AccessInfo.AccessSize >= 2 && ataDataPos < ataDataLen) {
                    ataDataBuffer[ataDataPos++] = (unsigned char)((word >> 8) & 0xFF);
                }
                if (ataDataPos >= ataDataLen) {
                    if (ataDiskFile) {
                        _fseeki64(ataDiskFile, (long long)ataPendingLba * 512, SEEK_SET);
                        fwrite(ataDataBuffer, 1, ataDataLen, ataDiskFile);
                        fflush(ataDiskFile);
                    }
                    ataStatus = ATA_ST_DRDY | ATA_ST_DSC;
                    ataMaybeInjectIrq(partition);
                }
            }
        } else if (!ataDataIsWrite && ataDataPos < ataDataLen) {
            word = ataDataBuffer[ataDataPos++];
            if (io->AccessInfo.AccessSize >= 2 && ataDataPos < ataDataLen) {
                word |= (UINT16)ataDataBuffer[ataDataPos++] << 8;
            }
            if (ataDataPos >= ataDataLen) ataStatus = ATA_ST_DRDY | ATA_ST_DSC;
        }
        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
        WHV_REGISTER_VALUE values[2] = { 0 };
        values[0].Reg64 = word;
        values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
        WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
        return;
    }

    UINT32 accessSize = io->AccessInfo.AccessSize;
    if (accessSize < 1) accessSize = 2;
    UINT32 count = io->AccessInfo.RepPrefix ? (UINT32)io->Rcx : 1;
    if (count == 0) count = 1;

    UINT64 rsiOut = io->Rsi;
    UINT64 rdiOut = io->Rdi;
    UINT32 i;

    if (io->AccessInfo.IsWrite) {
        unsigned char *guestPtr = (unsigned char *)guestMemory + (((UINT64)io->Ds.Base + io->Rsi) & 0xFFFFF);
        for (i = 0; i < count && ataDataIsWrite && ataDataPos < ataDataLen; i++) {
            UINT16 word = *(UINT16 *)(guestPtr + (UINT64)i * accessSize);
            ataDataBuffer[ataDataPos++] = (unsigned char)(word & 0xFF);
            if (accessSize >= 2 && ataDataPos < ataDataLen) {
                ataDataBuffer[ataDataPos++] = (unsigned char)((word >> 8) & 0xFF);
            }
        }
        rsiOut += (UINT64)i * accessSize;
        if (ataDataIsWrite && ataDataPos >= ataDataLen) {
            if (ataDiskFile) {
                _fseeki64(ataDiskFile, (long long)ataPendingLba * 512, SEEK_SET);
                fwrite(ataDataBuffer, 1, ataDataLen, ataDiskFile);
                fflush(ataDiskFile);
            }
            ataStatus = ATA_ST_DRDY | ATA_ST_DSC;
            ataMaybeInjectIrq(partition);
        }
    } else {
        unsigned char *guestPtr = (unsigned char *)guestMemory + (((UINT64)io->Es.Base + io->Rdi) & 0xFFFFF);
        for (i = 0; i < count; i++) {
            UINT16 word = 0xFFFF;
            if (!ataDataIsWrite && ataDataPos < ataDataLen) {
                word = ataDataBuffer[ataDataPos++];
                if (accessSize >= 2 && ataDataPos < ataDataLen) {
                    word |= (UINT16)ataDataBuffer[ataDataPos++] << 8;
                }
            }
            *(UINT16 *)(guestPtr + (UINT64)i * accessSize) = word;
        }
        rdiOut += (UINT64)count * accessSize;
        if (!ataDataIsWrite && ataDataLen > 0 && ataDataPos >= ataDataLen) {
            ataStatus = ATA_ST_DRDY | ATA_ST_DSC; // transfer complete, DRQ clears
        }
    }

    WHV_REGISTER_NAME names[4] = { WHvX64RegisterRcx, WHvX64RegisterRsi, WHvX64RegisterRdi, WHvX64RegisterRip };
    WHV_REGISTER_VALUE values[4] = { 0 };
    values[0].Reg64 = (io->AccessInfo.StringOp && io->AccessInfo.RepPrefix) ? 0 : io->Rcx;
    values[1].Reg64 = rsiOut;
    values[2].Reg64 = rdiOut;
    values[3].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
    WHvSetVirtualProcessorRegisters(partition, 0, names, 4, values);
}

// Both PS/2 queues are written by TWO threads and nothing serialised them: the
// window thread produces keystrokes and mouse packets from WndProc, the VM
// thread produces device command replies and performs every dequeue. The ring
// indices, the shared arrival-sequence counter and the mouse's 3-byte packet
// framing are all exposed to that race, and a torn mouse packet is not a subtle
// corruption -- the guest reads the fields out of phase and the pointer flies
// off the screen. Initialised in main() before the window thread is created.
CRITICAL_SECTION ps2Lock;
#define PS2_LOCK()   EnterCriticalSection(&ps2Lock)
#define PS2_UNLOCK() LeaveCriticalSection(&ps2Lock)

// One slot is always left empty so head==tail unambiguously means "empty".
#define PS2_QUEUE_SIZE 64

// Floor on the RTC periodic interval, in ms. 0 = honour whatever rate the guest
// programmed. See the rate-cap comment in deliverRtcPeriodicIrq.
double g_rtcMinIntervalMs = 0.0;

// ABAR traffic split by driver: firmware versus the Windows kernel. See the
// comment in ahciHandleAbarMmio -- "Setup finds no drives" needs to distinguish
// storahci never binding from storahci binding and then failing.
long g_ahciAbarGuestAccesses = 0, g_ahciAbarFwAccesses = 0;

// How long to wait before re-asserting the i8042 interrupt for a byte the guest
// has not read yet. See the re-assert comment in ps2ServiceOutputIrq.
//
// Runtime-tunable (LOCALHOST_PS2_REASSERT_MS) so it can be A/B'd against the
// input wedge: if the guest is already inside its keyboard ISR, re-asserting is
// a re-entrant interrupt at DIRQL, which is precisely the deadlock i8042prt
// cannot survive on a one-vCPU guest. A very large value effectively disables
// re-assertion, leaving one interrupt per byte presented.
double g_ps2ReassertMs = 50.0;
#define PS2_REASSERT_MS g_ps2ReassertMs

// PS/2 hot-path tracing. DEFAULT OFF, and that is a latency fix, not tidying.
//
// [ps2-key] (make AND break), [ps2-irq] and [ps2-out] each did printf + fflush
// PER EVENT and were deliberately written unbounded, on the reasoning that
// keystrokes are rare. They are -- but a single keystroke is 2 bytes, each byte
// costs an interrupt attempt and a guest read, so one keypress became ~6
// SYNCHRONOUS flushes to a multi-megabyte log on the same physical disk that
// backs the guest's virtual drive. [ps2-irq] sits directly in the interrupt
// delivery path, and [ps2-out] on the guest's port-0x60 read. Blocking there is
// exactly what makes typing feel sluggish while the counters all read clean.
// (Last run's log: 2.5 MB, and the run before hit 11 MB.)
//
// Set LOCALHOST_PS2_TRACE=1 to get the full byte-by-byte trace back when
// diagnosing input; the rare, edge-triggered [ps2-stall] and [ps2-masked] logs
// are always on, since they fire on trouble rather than per event.
int g_ps2Trace = -1;
static int ps2Trace(void) {
    if (g_ps2Trace < 0) {
        const char *e = getenv("LOCALHOST_PS2_TRACE");
        g_ps2Trace = (e && e[0] == '1') ? 1 : 0;
    }
    return g_ps2Trace;
}

// Heartbeat pacing, for the same reason and with more impact.
//
// The heartbeat triggers on `exitCount % 5000`, and its own comment justifies a
// register read inside it because "the guest now idles at ~64 exits/sec" -- at
// that rate it fires once every ~78 seconds. Measured today: roughly TWICE A
// SECOND, i.e. ~9,500 exits/sec, producing 12,917 lines and 1.79 MB of log in
// 5.7 minutes. Every one of those does a WHvGetVirtualProcessorRegisters
// hypercall plus ~20 printf+fflush pairs, synchronously, in the run loop.
//
// An exit-count trigger silently changes frequency whenever the guest's exit
// rate changes, so the assumption it was tuned against expires without warning.
// Gate on WALL TIME too: the exit counter still decides where to sample, the
// clock decides how often, and a faster guest can no longer turn the diagnostic
// into the bottleneck. LOCALHOST_HEARTBEAT_SEC overrides (0 = every trigger).
double g_heartbeatMinSec = -1.0;
static int heartbeatDue(void) {
    static LARGE_INTEGER last = { 0 };
    LARGE_INTEGER now;
    if (g_heartbeatMinSec < 0.0) {
        const char *e = getenv("LOCALHOST_HEARTBEAT_SEC");
        g_heartbeatMinSec = (e && e[0]) ? atof(e) : 2.0;
    }
    if (g_heartbeatMinSec <= 0.0 || !perfFrequency.QuadPart) return 1;
    QueryPerformanceCounter(&now);
    if (last.QuadPart == 0) { last = now; return 1; }
    if ((double)(now.QuadPart - last.QuadPart) / (double)perfFrequency.QuadPart < g_heartbeatMinSec)
        return 0;
    last = now;
    return 1;
}

unsigned char kbQueue[PS2_QUEUE_SIZE];
int kbHead = 0, kbTail = 0;

// Monotonic totals of bytes placed in each output buffer. Real i8042 hardware
// raises its IRQ every time a byte lands in the output buffer -- once per byte,
// not once per "the queue became non-empty" and not on a timer. Counting arrivals
// lets the run loop reproduce exactly that: see the PS/2 block in
// usbServiceSchedules.
long g_kbEnqueuedTotal = 0, g_auxEnqueuedTotal = 0;

// Bytes that came from REAL USER INPUT, as opposed to command responses the
// device generates (ACKs, reset/identify replies). The distinction matters for
// interrupt delivery: the guest POLLS for command responses -- it has just issued
// the command and is waiting on the status register -- but it can only learn about
// a keystroke from an interrupt. Injecting for command responses therefore buys
// nothing and fires during the i8042 init handshake, which is exactly when an
// unexpected interrupt destabilised boot.
volatile LONG g_kbUserBytes = 0, g_auxUserBytes = 0;

// Arrival order across BOTH queues. A real i8042 has a SINGLE output buffer: one
// byte at a time, from whichever device produced it, read first-come-first-served.
// Modelling it as two independent queues with the keyboard always winning the
// status register means one unread keyboard byte hides the mouse FOREVER -- and
// that is exactly what happened. See the status-register read for the full story.
unsigned long g_ps2Seq = 0;
unsigned long kbSeqQ[PS2_QUEUE_SIZE], auxSeqQ[PS2_QUEUE_SIZE];

// Bytes discarded because a queue was already full. This used to be impossible
// to see because it was also impossible to detect: both enqueues simply advanced
// the tail past the head, which does not lose one byte -- it silently reorders
// the entire queue and desynchronises the mouse's packet framing. A nonzero
// count here means the guest is not draining; for the mouse it should stay at
// zero now that motion is coalesced rather than queued unbounded.
long g_kbDropped = 0, g_auxDropped = 0;
long g_kbVkLogged = 0;   // total WM_KEYDOWNs seen; slot = count % KB_LASTREAD_MAX
extern unsigned char g_kbVkRing[];
// Declared here rather than beside the other keyboard counters because kbEnqueue
// (just below) needs it, and that sits earlier in the file than they do.
#define KB_LASTREAD_MAX 48
extern unsigned char g_kbLastEnq[];
extern long g_kbLastEnqCount;

static int ps2Used(int head, int tail) { return (tail - head + PS2_QUEUE_SIZE) % PS2_QUEUE_SIZE; }
static int ps2Free(int head, int tail) { return PS2_QUEUE_SIZE - 1 - ps2Used(head, tail); }

void kbEnqueue(unsigned char b) {
    PS2_LOCK();
    // A FULL queue used to discard the byte being ADDED, which is the worst
    // possible choice for a keyboard: the guest drains slowly (it cannot be
    // interrupted at all until it programs GSI 1's redirection entry, and only
    // once per re-assert after that), so the queue saturates with keystrokes the
    // user typed seconds ago and every FRESH keypress is thrown away. Measured
    // against Windows Setup: queue pinned at 63, 21 bytes dropped, and 12 TAB
    // presses produced exactly one TAB in the guest -- indistinguishable from
    // "the keyboard does nothing".
    //
    // Drop from the HEAD instead, oldest first, so recent keys always get in. Two
    // bytes, because scancodes travel as make/break pairs and evicting a lone make
    // leaves the guest holding a key down forever.
    if (ps2Free(kbHead, kbTail) < 1) {
        int evict = 2, k;
        for (k = 0; k < evict && kbHead != kbTail; k++)
            kbHead = (kbHead + 1) % PS2_QUEUE_SIZE;
        g_kbDropped += evict;
    }
    kbSeqQ[kbTail] = g_ps2Seq++;
    kbQueue[kbTail] = b;
    kbTail = (kbTail + 1) % PS2_QUEUE_SIZE;
    g_kbEnqueuedTotal++;
    // Ring of what went IN, to sit beside the ring of what came OUT. The consumed
    // stream shows only SPACE (39/B9) even though TAB/DOWN/A are posted and
    // kbUser rises, and enqueued-vs-consumed is the only way to tell whether
    // those bytes are lost before the queue or after it.
    g_kbLastEnq[g_kbLastEnqCount % KB_LASTREAD_MAX] = b;
    g_kbLastEnqCount++;
    PS2_UNLOCK();
}
int kbHasData() { return kbHead != kbTail; }
unsigned char kbDequeue() {
    unsigned char b;
    PS2_LOCK();
    b = kbQueue[kbHead];
    kbHead = (kbHead + 1) % PS2_QUEUE_SIZE;
    PS2_UNLOCK();
    return b;
}

// --- PS/2 AUX (mouse) port -- Phase 1: controller/device command handling
// only, no host input capture yet (see docs/roadmap.md). Mirrors the
// keyboard queue above exactly; real i8042 hardware keeps keyboard and AUX
// as independent queues with a fixed keyboard-first read priority rather
// than one interleaved FIFO, so this is modeled the same way rather than
// merged with kbQueue.
unsigned char auxQueue[PS2_QUEUE_SIZE];
int auxHead = 0, auxTail = 0;

void auxEnqueue(unsigned char b) {
    PS2_LOCK();
    // A FULL queue used to discard the byte being ADDED -- the same mistake
    // kbEnqueue had, and worse here. Mouse packets are THREE bytes (status, dx,
    // dy), so losing one byte does not lose one movement: it shifts every
    // following byte into the wrong field, and the driver reads dx as status and
    // dy as dx from then on. That is why the pointer "works until it doesn't" and
    // never recovers on its own -- the stream is desynchronised, not merely
    // behind.
    //
    // Evict a WHOLE PACKET from the head instead. Three bytes, because dropping
    // any other number is what breaks framing in the first place; the head is
    // packet-aligned in the steady state since motion is generated a packet at a
    // time. Costs one stale movement and keeps every later packet aligned.
    if (ps2Free(auxHead, auxTail) < 1) {
        int evict = 3, k;
        for (k = 0; k < evict && auxHead != auxTail; k++)
            auxHead = (auxHead + 1) % PS2_QUEUE_SIZE;
        g_auxDropped += evict;
    }
    auxSeqQ[auxTail] = g_ps2Seq++;
    auxQueue[auxTail] = b;
    auxTail = (auxTail + 1) % PS2_QUEUE_SIZE;
    g_auxEnqueuedTotal++;
    PS2_UNLOCK();
}

// Which queue holds the OLDEST undelivered byte -- i.e. which one a real single
// output buffer would be presenting right now. Returns 1 for aux, 0 for keyboard.
int ps2AuxIsNext(void) {
    int r;
    PS2_LOCK();
    if (!kbHasData())      r = auxHasData();
    else if (!auxHasData()) r = 0;
    else                    r = (auxSeqQ[auxHead] < kbSeqQ[kbHead]) ? 1 : 0;
    PS2_UNLOCK();
    return r;
}
int auxHasData() { return auxHead != auxTail; }
unsigned char auxDequeue() {
    unsigned char b;
    PS2_LOCK();
    b = auxQueue[auxHead];
    auxHead = (auxHead + 1) % PS2_QUEUE_SIZE;
    PS2_UNLOCK();
    return b;
}

// Controller-level AUX port state (set via 0x64 writes 0xA7/0xA8, consumed
// by 0xD4 below).
int auxPortEnabled = 0;
// One-shot flag set by a 0x64 write of 0xD4: the *next* 0x60 write is routed
// to the mouse device's own command handler (auxHandleCommand) instead of
// the keyboard's.
int nextByteTargetsAux = 0;
// i8042 controller configuration byte, readable via 0x20 and writable via 0x60.
// Default: port1 IRQ on, system flag set, translation on -- and port2 (aux) IRQ
// left for the guest to enable, which is precisely what it wants to do.
unsigned char i8042ConfigByte = 0x45;
int nextByteIsConfig = 0;

// Mouse device state, PS/2 spec defaults (set on 0xFF reset and 0xF6 set-
// defaults): resolution 4 counts/mm, sample rate 100/s, 1:1 scaling,
// streaming reports disabled until 0xF4.
unsigned char auxResolution = 2;   // 2 = 4 counts/mm (the spec default code, not counts/mm itself)
unsigned char auxSampleRate = 100;
int auxScaling2to1 = 0;
int auxReportingEnabled = 0;
// Lives here with the rest of the device state rather than down with the host
// input plumbing, because the 0xE9 status reply reports it too.
unsigned char auxButtonMask = 0; // bit0=left, bit1=right, bit2=middle

// Set by 0xE8 (set resolution) / 0xF3 (set sample rate): the next
// AUX-directed byte (still individually 0xD4-prefixed by the driver) is a
// parameter for that command rather than a new command.
int auxAwaitingParam = 0;
unsigned char auxAwaitingParamFor = 0;

// Discards anything the mouse has queued but the guest has not read. Real
// hardware does this on reset -- the device stops streaming and its buffer is
// emptied -- and it matters here: a reset issued mid-motion would otherwise put
// the reply (0xFA 0xAA 0x00) BEHIND leftover packet bytes, so i8042prt reads a
// movement byte where it expects the ACK and fails the handshake it just
// started. Which is exactly the handshake that has to work. Defined with the
// motion state it clears, further down.
void auxFlushQueue(void);

void auxResetState(void) {
    auxResolution = 2;
    auxSampleRate = 100;
    auxScaling2to1 = 0;
    auxReportingEnabled = 0;
    auxAwaitingParam = 0;
}

// Handles a byte written to port 0x60 while nextByteTargetsAux is set --
// i.e. a byte the driver directed at the mouse device itself (as opposed to
// the 0xA7/0xA8/0xA9/0xD4 controller-level commands on port 0x64, handled
// where those are dispatched). Mirrors the keyboard command handling's
// style: an explicit reset handshake, a handful of specifically-modeled
// commands, and a generic-ACK fallback for everything else.
void auxHandleCommand(unsigned char val) {
    // Bounded. This was unbounded, and once the mouse actually started working it
    // became one of the hottest log lines in the build -- every command, forever.
    // That matters more than it sounds: when stdout is a CONSOLE rather than a
    // file, each printf blocks the VM thread on console rendering, and a boot
    // emits ~70000 lines. Enough of them to watch the init handshake, then quiet.
    static int auxCmdLogged = 0;
    if (auxCmdLogged < 80) {
        auxCmdLogged++;
        printf("[aux] command 0x%02X\n", val);
        fflush(stdout);
    }
    if (auxAwaitingParam) {
        // Parameter byte for a preceding 0xE8/0xF3 -- just acknowledge and
        // store it; the actual value doesn't affect our packet generation.
        if (auxAwaitingParamFor == 0xE8) auxResolution = val;
        else if (auxAwaitingParamFor == 0xF3) auxSampleRate = val;
        auxAwaitingParam = 0;
        auxEnqueue(0xFA);
        return;
    }

    switch (val) {
        case 0xFF: // Reset
            auxResetState();
            auxFlushQueue();  // the reply must be the FIRST thing the guest reads back
            auxEnqueue(0xFA); // command acknowledged
            auxEnqueue(0xAA); // self-test passed
            auxEnqueue(0x00); // device ID: 0x00 = standard PS/2 mouse
            break;
        case 0xF6: // Set defaults
            auxResetState();
            auxFlushQueue();
            auxEnqueue(0xFA);
            break;
        case 0xF4: // Enable data reporting
            auxReportingEnabled = 1;
            auxEnqueue(0xFA);
            break;
        case 0xF5: // Disable data reporting -- streaming stops and the buffer is dropped
            auxReportingEnabled = 0;
            auxFlushQueue();
            auxEnqueue(0xFA);
            break;
        case 0xE8: // Set resolution -- next byte is the parameter
            auxAwaitingParam = 1;
            auxAwaitingParamFor = 0xE8;
            auxEnqueue(0xFA);
            break;
        case 0xF3: // Set sample rate -- next byte is the parameter
            auxAwaitingParam = 1;
            auxAwaitingParamFor = 0xF3;
            auxEnqueue(0xFA);
            break;
        case 0xE6: // Set scaling 1:1
            auxScaling2to1 = 0;
            auxEnqueue(0xFA);
            break;
        case 0xE7: // Set scaling 2:1
            auxScaling2to1 = 1;
            auxEnqueue(0xFA);
            break;
        case 0xF2: // Get device ID
            auxEnqueue(0xFA);
            auxEnqueue(0x00);
            break;
        case 0xE9: // Status request -- ACK then THREE status bytes
            // i8042prt really does send this (observed mid-handshake, between the
            // scaling and resolution commands). It fell through to the generic ACK
            // below, which answers with one byte where the device owes four: the
            // driver is then three bytes out of phase with the stream for the rest
            // of the conversation, reading the next command's ACK as its status.
            auxEnqueue(0xFA);
            auxEnqueue((unsigned char)((auxScaling2to1 ? 0x10 : 0x00) |
                                       (auxReportingEnabled ? 0x20 : 0x00) |
                                       (auxButtonMask & 0x07)));
            auxEnqueue(auxResolution);
            auxEnqueue(auxSampleRate);
            break;
        default: // Generic ACK, matching the keyboard's own catch-all.
            auxEnqueue(0xFA);
            break;
    }
}

// Phase 2: host mouse input -> PS/2 relative-motion packets. Tracked
// separately from auxHandleCommand's protocol/command-response state above.
int auxLastCursorX = 0, auxLastCursorY = 0;
int auxCursorPosKnown = 0;
// Whether TrackMouseEvent is currently armed for WM_MOUSELEAVE. Windows disarms
// tracking automatically once it fires, so this is re-armed on the next move.
int auxMouseTracking = 0;
// Set once RegisterRawInputDevices succeeds. When it does, WM_INPUT owns movement
// (raw, unaccelerated deltas); if registration ever fails we fall back to deriving
// deltas from WM_MOUSEMOVE's cooked coordinates rather than losing the mouse.
int g_rawMouseAvailable = 0;
// Uncapped PS/2 mouse tallies. The mouse has to be verifiable at a glance now
// that it is the working pointer again.
long g_auxPackets = 0;        // packets actually queued to the guest
long g_auxBytesToGuest = 0;   // bytes the guest actually read back out of port 0x60
long g_kbBytesToGuest = 0;    // keystroke bytes the guest actually read
long g_kbIrqSent = 0, g_kbIrqMasked = 0;  // keyboard IRQs we fired / that were masked
long g_kbMaskedLogged = 0;                // counts RTE transitions seen at refusals
UINT64 g_kbLastRefusedRte = 0xFFFFFFFFFFFFFFFFULL;  // sentinel: no refusal seen yet
// exitCount is a local in main(); mirror it so diagnostics elsewhere can date an
// event without threading the value through every call.
long g_exitCountForDiag = 0;
unsigned char g_kbLastRead[KB_LASTREAD_MAX];
long g_kbLastReadCount = 0;               // total; slot = count % KB_LASTREAD_MAX
unsigned char g_kbLastEnq[KB_LASTREAD_MAX];
long g_kbLastEnqCount = 0;
unsigned char g_kbVkRing[KB_LASTREAD_MAX];
long g_injectSkippedBusy = 0;             // injections skipped because one was still pending
long g_auxPacketsGated = 0;   // suppressed because the guest has not enabled reporting
long g_auxIrqSent = 0, g_auxIrqMasked = 0;  // mouse IRQs we fired / that were masked
long g_auxCoalesced = 0;      // host motion events folded into a later packet

// --- The output buffer's interrupt ---
//
// A real i8042 raises an IRQ when a byte MOVES INTO its single output buffer,
// and the guest's ISR reads exactly ONE byte per interrupt. So the rule is one
// interrupt per byte PRESENTED, which re-raises by itself after every read that
// leaves more data behind.
//
// Neither rule tried before is that rule, and both fail the same way. "One
// interrupt per byte enqueued" keys on a monotonic arrival counter, so a reply
// that lands as one batch -- 0xFA 0xAA 0x00 for a mouse reset, 0xFA 0x00 for an
// identify -- got exactly ONE interrupt, the guest read one byte, and the rest
// were stranded with nothing left to announce them. That is what the mouse init
// handshake was dying on: reset completed, 0xF2 (identify) was acknowledged, the
// ID byte never arrived, i8042prt timed out and started the whole sequence over.
// The other rule, "re-assert while any data is pending", delivers every byte but
// becomes a storm aimed at a driver that is not ready -- the ~50/sec version of
// it is what wedged the guest at IRQL 15 in earlier attempts.
//
// Keying on the ARRIVAL SEQUENCE of the byte at the head of the output buffer is
// the rule itself: one interrupt per byte, in order, and silence while idle.
// Why the main-loop PS/2 delivery does or does not run. kbIrq stayed at 1 across
// a run holding 20 queued bytes, so the question is whether this path is reached
// at all and, if so, which condition turns it away.
long g_ps2GateReached = 0, g_ps2GatePassed = 0, g_ps2GateNoKernel = 0, g_ps2GateIfClear = 0;
static unsigned long g_ps2AnnouncedSeq = 0;
static int g_ps2Announced = 0;
static LARGE_INTEGER g_ps2AnnouncedAt;

// Identifies the byte the output buffer is presenting. Returns 0 when empty.
static int ps2OutputHead(int *isAux, unsigned long *seq) {
    int found = 0;
    PS2_LOCK();
    if (kbHasData() || auxHasData()) {
        int aux = ps2AuxIsNext();
        *isAux = aux;
        *seq = aux ? auxSeqQ[auxHead] : kbSeqQ[kbHead];
        found = 1;
    }
    PS2_UNLOCK();
    return found;
}

// Raises the interrupt for whatever the output buffer holds right now, at most
// once per byte. Returns 1 if an interrupt was actually queued for the guest.
int ps2ServiceOutputIrq(WHV_PARTITION_HANDLE partition) {
    int isAux = 0, delivered;
    unsigned long seq = 0;
    LARGE_INTEGER now;

    if (!ps2OutputHead(&isAux, &seq)) { g_ps2Announced = 0; return 0; }

    QueryPerformanceCounter(&now);

    // STALL DETECTOR, keyed on the BYTE rather than on our announcement state.
    // The first version of this lived inside the "already announced" branch and
    // never fired: when delivery fails, g_ps2Announced stays 0 and that branch is
    // never entered -- so the case most worth inspecting was the one case it
    // could not see. Keyed on the head byte's arrival sequence instead, it fires
    // whenever the guest is not consuming, however the delivery attempt went.
    //
    // Whether vector 0xA0 is in the APIC's IRR (announced, never delivered) or
    // its ISR (delivered, the handler was entered and never completed) tells two
    // opposite stories: ours to fix, versus the guest stuck inside its keyboard
    // ISR -- the i8042prt DIRQL deadlock this codebase already documents.
    {
        static unsigned long stallSeq = 0;
        static LARGE_INTEGER stallSince, lastDump;
        if (seq != stallSeq) { stallSeq = seq; stallSince = now; }
        else if (perfFrequency.QuadPart && stallSince.QuadPart) {
            double stuckMs = (double)(now.QuadPart - stallSince.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart;
            double sinceDumpMs = lastDump.QuadPart
                ? (double)(now.QuadPart - lastDump.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart
                : 1e9;
            if (stuckMs > 1500.0 && sinceDumpMs > 4000.0) {
                lastDump = now;
                printf("[ps2-stall] head byte unread for %.0f ms (isAux=%d, announced=%d, "
                       "kbQueueDepth=%d) -- LAPIC follows\n",
                       stuckMs, isAux, g_ps2Announced,
                       (kbTail - kbHead + PS2_QUEUE_SIZE) % PS2_QUEUE_SIZE);
                fflush(stdout);
                u46DumpLapic(partition, "ps2 byte unread");
            }
        }
    }
    if (g_ps2Announced && seq == g_ps2AnnouncedSeq) {
        // Same byte still sitting unread. Re-announce it, but slowly. A real
        // controller holds its line asserted until the byte is read, so going
        // permanently silent after a single edge means one missed interrupt
        // wedges the ENTIRE controller -- there is only one output buffer, so an
        // unread byte blocks the other device too.
        //
        // This was 500ms, which silently capped the drain rate at TWO BYTES A
        // SECOND whenever a byte went unread -- one keystroke is two bytes, so
        // typing outran the controller and the queue backed up until it dropped
        // everything, which is what "the input is slow and unreliable" actually
        // was. 500ms was picked when a 20ms re-assert destabilised boot, but that
        // measurement predates the priority interrupt queue; 50ms is still 2.5x
        // more conservative than the value that caused trouble.
        double sinceMs = perfFrequency.QuadPart
            ? (double)(now.QuadPart - g_ps2AnnouncedAt.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart
            : 0.0;
        // The guest has been sitting on this byte for a while. Whether vector 0xA0
        // is in the APIC's IRR (announced, never delivered) or its ISR (delivered,
        // the ISR was entered and never completed) tells two opposite stories:
        // the first is a delivery problem on our side, the second means the guest
        // is stuck INSIDE its keyboard ISR -- the i8042prt DIRQL deadlock this
        // codebase already documents. Reading it is the only way to tell them
        // apart, and the counters cannot.
        if (sinceMs < PS2_REASSERT_MS) return 0;
    }

    if (isAux) {
        g_auxIrqSent++;
        delivered = injectDeviceIrq(partition, GSI_MOUSE, 0x74);
        if (!delivered) g_auxIrqMasked++;
    } else {
        long apicOkBefore = g_reqIrqOk, apicFailBefore = g_reqIrqFail;
        g_kbIrqSent++;
        delivered = injectDeviceIrq(partition, GSI_KEYBOARD, 0x09);
        // PER-ATTEMPT LEDGER, unbounded. Every aggregate counter so far has been a
        // stale snapshot from a change-gated heartbeat, so "the interrupt was
        // raised" and "the guest was actually asked" have never been distinguished
        // for an individual keystroke. This says, for each attempt: which byte was
        // being presented, whether the vector resolved, and whether the APIC
        // accepted the request. Paired with [ps2-out] it gives the whole path from
        // queued byte to guest read. PS/2 attempts are rare (keystrokes plus one
        // re-assert per 50ms), so this cannot flood the way an RTC-rate log would.
        if (ps2Trace()) {
            printf("[ps2-irq] head=0x%02X seq=%lu -> %s (apicOk +%ld, apicFail +%ld) queueDepth=%d\n",
                   kbQueue[kbHead], seq,
                   delivered ? "raised" : "REFUSED",
                   g_reqIrqOk - apicOkBefore, g_reqIrqFail - apicFailBefore,
                   (kbTail - kbHead + PS2_QUEUE_SIZE) % PS2_QUEUE_SIZE);
            fflush(stdout);
        }
        if (!delivered) {
            g_kbIrqMasked++;
            // WHY it was refused. The heartbeat's "masked" total conflates a
            // never-routed entry with one the guest programmed and then masked,
            // and the end-of-run snapshot shows the entry LIVE -- so the refusals
            // must be happening in states the snapshot never sees. Log the entry
            // as it was at the moment of the decision.
            // EDGE-TRIGGERED, not capped. A "first 24" filled up during boot and
            // showed only HAL's masked default, telling us nothing about the state
            // during the phase where keys are actually pressed. Logging on CHANGE
            // gives the entry's whole history for the cost of one comparison.
            {
                UINT64 rte = ioapicRteFor(GSI_KEYBOARD);
                if (rte != g_kbLastRefusedRte) {
                    g_kbLastRefusedRte = rte;
                    g_kbMaskedLogged++;
                    printf("[ps2-masked] kbd irq refused (total %ld): rte CHANGED to 0x%016llX (%s) apicMode=%d\n",
                           g_kbIrqMasked, (unsigned long long)rte,
                           rte == 0x10000ULL ? "pristine" : (rte & 0x10000ULL) ? "masked bit set" : "live?!",
                           g_guestApicMode);
                    fflush(stdout);
                }
            }
        }
    }

    // Only counted as announced if it actually went somewhere. A masked GSI
    // means the guest has not programmed that IOAPIC entry yet (normal during
    // early boot); retrying costs one vector resolve per pass and stops the byte
    // being lost to a window the guest was never listening in.
    if (delivered) {
        g_ps2Announced = 1;
        g_ps2AnnouncedSeq = seq;
        g_ps2AnnouncedAt = now;
    }
    return delivered;
}

// --- Host motion -> PS/2 packets ---
//
// Motion the guest has not been given yet. PS/2 is a RELATIVE protocol, so
// motion that cannot be sent right now is simply added to the next packet: the
// pointer still arrives exactly where the host pointer is, described in fewer,
// larger steps. That is what a real mouse does too -- it samples at a fixed rate
// and reports the movement accumulated since its last report.
//
// This is what keeps the byte stream intact. Host raw input arrives at the
// physical mouse's polling rate (125-1000 reports/sec) while the guest drains
// the output buffer one byte per interrupt. Queueing all of it overruns a
// 64-byte ring in a fraction of a second, and an overrun does not merely lose
// motion -- it breaks the 3-byte packet framing, after which the guest reads
// every field out of phase and the cursor flies off the screen.
static int auxPendingDx = 0, auxPendingDy = 0;
static int auxPendingMotion = 0;
static unsigned char auxLastReportedButtons = 0;

// At most this much un-read mouse data in flight (four packets). Enough that a
// burst still feels smooth; past it the guest is behind, and more packets would
// only add latency to a position it is about to be told about anyway.
#define AUX_MAX_INFLIGHT 12

void auxFlushQueue(void) {
    PS2_LOCK();
    auxHead = auxTail = 0;
    auxPendingDx = auxPendingDy = 0;
    auxPendingMotion = 0;
    PS2_UNLOCK();
}

// Builds and enqueues one standard 3-byte PS/2 packet (status, dx, dy) from the
// accumulated motion and the current button state. Caller holds ps2Lock.
static void auxFlushMotionLocked(void) {
    int buttonsChanged = (auxButtonMask != auxLastReportedButtons);
    int px, py, restX = 0, restY = 0;
    unsigned char status;

    if (!auxPendingMotion && !buttonsChanged) return;

    // Two gates the guest controls: the driver must have enabled reporting
    // (0xF4) and the aux port must be on. Counted separately from accepted
    // packets so "the mouse is dead" can be told apart from "the guest has not
    // enabled it yet" -- otherwise both look identical from outside.
    if (!auxReportingEnabled || !auxPortEnabled) {
        g_auxPacketsGated++;
        auxPendingDx = auxPendingDy = 0;
        auxPendingMotion = 0;
        auxLastReportedButtons = auxButtonMask;
        return;
    }

    // Hold motion back while the guest is behind -- but never a button change,
    // which cannot be folded into a later packet without losing the click.
    if (!buttonsChanged && ps2Used(auxHead, auxTail) >= AUX_MAX_INFLIGHT) { g_auxCoalesced++; return; }
    if (ps2Free(auxHead, auxTail) < 3) { g_auxCoalesced++; return; }

    px = auxPendingDx;
    py = -auxPendingDy;             // PS/2 Y+ is up; Windows client-area Y+ is down
    auxPendingDx = auxPendingDy = 0;
    auxPendingMotion = 0;
    auxLastReportedButtons = auxButtonMask;
    g_auxPackets++;

    // Each field is 9-bit signed (sign bit in the status byte + 8 data bits).
    // Anything past that stays pending and goes out in the next packet rather
    // than being clipped: a large accumulated movement should arrive slightly
    // later, not partly vanish. Carrying the remainder also means the overflow
    // bits (6 and 7) are never needed -- drivers treat those as "discard this
    // packet", which would throw the movement away.
    if (px > 255)  { restX = px - 255;  px = 255;  }
    if (px < -256) { restX = px + 256;  px = -256; }
    if (py > 255)  { restY = py - 255;  py = 255;  }
    if (py < -256) { restY = py + 256;  py = -256; }
    if (restX || restY) {
        auxPendingDx = restX;
        auxPendingDy = -restY;      // back into host convention
        auxPendingMotion = 1;
    }

    status = (unsigned char)(auxButtonMask & 0x07);
    status |= 0x08;                 // bit 3: always-1 marker, used by drivers to resync the stream
    if (px < 0) status |= 0x10;     // bit 4: X sign
    if (py < 0) status |= 0x20;     // bit 5: Y sign

    InterlockedIncrement(&g_auxUserBytes);
    auxEnqueue(status);
    auxEnqueue((unsigned char)(px & 0xFF));
    auxEnqueue((unsigned char)(py & 0xFF));
}

// Called from the window thread on every mouse-move or button-state change.
// dx/dy are relative motion in client pixels.
void auxSendPacket(int dx, int dy) {
    PS2_LOCK();
    if (dx || dy) {
        auxPendingDx += dx;
        auxPendingDy += dy;
        auxPendingMotion = 1;
    }
    auxFlushMotionLocked();
    PS2_UNLOCK();
}

// Called from the VM thread each time the guest reads a byte out of the output
// buffer: as it drains, held-back motion flows out at exactly the rate the guest
// is willing to consume, instead of piling up behind it.
void auxDrainPendingMotion(void) {
    PS2_LOCK();
    auxFlushMotionLocked();
    PS2_UNLOCK();
}

// Maps a Windows virtual key to a set-1 scancode, and reports whether it is an
// EXTENDED key -- one the guest expects to arrive prefixed with 0xE0.
//
// The prefix is not cosmetic. The arrow keys, Insert/Delete/Home/End/PageUp/
// PageDown, right Ctrl/Alt, the Windows keys and keypad Enter/slash all share
// their base scancode with a keypad key, and 0xE0 is the only thing telling them
// apart. Sending a bare 0x48 does not mean "Up" to the guest, it means keypad-8,
// which is why arrows behaved oddly. The Windows keys were not in this table at
// all, so they mapped to 0 and were dropped entirely -- and the HOST handled
// them, which is what opens the host Start menu.
unsigned char vkToScancode(int vk, int *extended) {
    if (extended) *extended = 0;
    switch (vk) {
        // --- extended (0xE0-prefixed) keys ---
        case VK_LWIN:    if (extended) *extended = 1; return 0x5B;
        case VK_RWIN:    if (extended) *extended = 1; return 0x5C;
        case VK_APPS:    if (extended) *extended = 1; return 0x5D;  // context-menu key
        case VK_INSERT:  if (extended) *extended = 1; return 0x52;
        case VK_DELETE:  if (extended) *extended = 1; return 0x53;
        case VK_HOME:    if (extended) *extended = 1; return 0x47;
        case VK_END:     if (extended) *extended = 1; return 0x4F;
        case VK_PRIOR:   if (extended) *extended = 1; return 0x49;  // PageUp
        case VK_NEXT:    if (extended) *extended = 1; return 0x51;  // PageDown
        case VK_RCONTROL:if (extended) *extended = 1; return 0x1D;
        case VK_RMENU:   if (extended) *extended = 1; return 0x38;  // right Alt
        case VK_DIVIDE:  if (extended) *extended = 1; return 0x35;  // keypad /
        case VK_UP:      if (extended) *extended = 1; return 0x48;
        case VK_LEFT:    if (extended) *extended = 1; return 0x4B;
        case VK_RIGHT:   if (extended) *extended = 1; return 0x4D;
        case VK_DOWN:    if (extended) *extended = 1; return 0x50;

        // --- plain keys ---
        case VK_LCONTROL: case VK_CONTROL: return 0x1D;
        case VK_LMENU:    case VK_MENU:    return 0x38;
        case VK_LSHIFT:   case VK_SHIFT:   return 0x2A;
        case VK_RSHIFT:   return 0x36;
        case VK_CAPITAL:  return 0x3A;
        case VK_F1:  return 0x3B; case VK_F2:  return 0x3C; case VK_F3:  return 0x3D;
        case VK_F4:  return 0x3E; case VK_F5:  return 0x3F; case VK_F6:  return 0x40;
        case VK_F7:  return 0x41; case VK_F8:  return 0x42; case VK_F9:  return 0x43;
        case VK_F10: return 0x44; case VK_F11: return 0x57; case VK_F12: return 0x58;
        case VK_OEM_MINUS:  return 0x0C; case VK_OEM_PLUS:   return 0x0D;
        case VK_OEM_4:      return 0x1A; case VK_OEM_6:      return 0x1B;
        case VK_OEM_1:      return 0x27; case VK_OEM_7:      return 0x28;
        case VK_OEM_3:      return 0x29; case VK_OEM_5:      return 0x2B;
        case VK_OEM_COMMA:  return 0x33; case VK_OEM_PERIOD: return 0x34;
        case VK_OEM_2:      return 0x35;
        case VK_ESCAPE: return 0x01;
        case '1': return 0x02; case '2': return 0x03; case '3': return 0x04;
        case '4': return 0x05; case '5': return 0x06; case '6': return 0x07;
        case '7': return 0x08; case '8': return 0x09; case '9': return 0x0A;
        case '0': return 0x0B;
        case VK_BACK: return 0x0E;
        case VK_TAB: return 0x0F;
        case 'Q': return 0x10; case 'W': return 0x11; case 'E': return 0x12;
        case 'R': return 0x13; case 'T': return 0x14; case 'Y': return 0x15;
        case 'U': return 0x16; case 'I': return 0x17; case 'O': return 0x18;
        case 'P': return 0x19;
        case VK_RETURN: return 0x1C;
        case 'A': return 0x1E; case 'S': return 0x1F; case 'D': return 0x20;
        case 'F': return 0x21; case 'G': return 0x22; case 'H': return 0x23;
        case 'J': return 0x24; case 'K': return 0x25; case 'L': return 0x26;
        case 'Z': return 0x2C; case 'X': return 0x2D; case 'C': return 0x2E;
        case 'V': return 0x2F; case 'B': return 0x30; case 'N': return 0x31;
        case 'M': return 0x32;
        case VK_SPACE: return 0x39;
        // NOTE: the arrow keys used to live here as bare 0x48/0x4B/0x4D/0x50,
        // which are the KEYPAD scancodes. They are handled as extended keys at
        // the top of this switch now; do not re-add them here.
        default: return 0x00;
    }
}

#define LOG_BUFFER_SIZE 8192
char logBuffer[LOG_BUFFER_SIZE];
int logLength = 0;
CRITICAL_SECTION logLock;

// TEMP DIAGNOSTIC: incremented every time "starting Boot0002" (BdsDxe's own
// debug text, printed right before it calls StartImage on the real, chosen
// boot file) appears in the accumulated log text -- NOT just once, since
// the whole boot attempt retries multiple times (each retry reloads a
// fresh, unpatched copy of bootmgfw.efi), so a one-shot trigger only ever
// catches the first attempt even when that attempt isn't the one that
// matters. The main loop re-installs hooks on every new occurrence.
int efiHookStartingBoot0002Count = 0;
// Incremented when BDS prints "failed to start Boot..." -- see appendToLog.
int g_bdsBootFailed = 0;
// Bumped by appendToLog when the boot application prints its CD/DVD prompt;
// the main loop presses a key once per bump. Set LOCALHOST_NO_AUTO_BOOT_KEY=1
// to leave the prompt to the user.
long g_autoBootKeyWanted = 0, g_autoBootKeySent = 0, g_autoBootKeyReported = 0;
int g_noAutoBootKey = 0;
// Set when the target disk already carries a GPT, i.e. something is installed on
// it. Suppresses the CD/DVD auto-answer so we boot the installed system instead
// of the installer. LOCALHOST_BOOT_ISO=1 overrides, for a deliberate reinstall.
int g_diskHasOs = 0, g_forceBootIso = 0;
// NIC register traffic split by driver, plus a ring of the most recent accesses.
// Windows reports the RTL8139 as Code 10; this separates "the driver never
// reached the card" from "it reached it and gave up", which need different fixes.
#define NIC_RING_MAX 40
long g_nicGuestAccesses = 0, g_nicFwAccesses = 0, g_nicRingCount = 0, g_nicReadLogged = 0, g_nicCfgLogged = 0;
unsigned char g_nicRingOff[NIC_RING_MAX], g_nicRingWrite[NIC_RING_MAX];
UINT32 g_nicRingVal[NIC_RING_MAX];

void appendToLog(char c) {
    EnterCriticalSection(&logLock);
    if (logLength < LOG_BUFFER_SIZE - 1) {
        logBuffer[logLength++] = c;
        logBuffer[logLength] = '\0';
    }
    LeaveCriticalSection(&logLock);
    putchar(c);
    fflush(stdout);

    {
        static const char needle[] = "starting Boot0002";
        static int matchPos = 0;
        if (c == needle[matchPos]) {
            matchPos++;
            if (needle[matchPos] == '\0') { efiHookStartingBoot0002Count++; matchPos = 0; }
        } else {
            matchPos = (c == needle[0]) ? 1 : 0;
        }
    }

    // Answer "Press any key to boot from CD or DVD" automatically.
    //
    // Windows install media prints this, polls the i8042 for about five seconds,
    // and on getting nothing returns EFI_TIMEOUT -- after which the firmware
    // reports "No bootable option or device was found" and the ISO looks broken.
    // It is not broken; nobody answered. Making the user win a five-second race
    // every boot is not a reasonable way to start a VM.
    //
    // Triggered on the guest's OWN prompt text rather than on a timer or a
    // breakpoint: the boot application writes it to COM1, which we already
    // capture here, so this fires exactly when the prompt is on screen and at no
    // other time. Re-arms per occurrence because BdsDxe retries the boot.
    {
        static const char needle[] = "Press any key to boot";
        static int matchPos = 0;
        static ULONGLONG lastMatchTick = 0;
        if (c == needle[matchPos]) {
            matchPos++;
            if (needle[matchPos] == '\0') {
                // DEBOUNCED. The boot application mirrors its console to BOTH
                // COM1 and COM2, so a single on-screen prompt reaches this
                // matcher twice and looked like the prompt appearing again --
                // which is the one thing that must not be misreported, since a
                // genuine repeat means a mid-install reboot. A real second prompt
                // is a reboot away, many seconds out, so anything within five
                // seconds is the same prompt echoed on the other port.
                ULONGLONG tick = GetTickCount64();
                if (tick - lastMatchTick > 5000) {
                    lastMatchTick = tick;
                    g_autoBootKeyWanted++;
                }
                matchPos = 0;
            }
        } else {
            matchPos = (c == needle[0]) ? 1 : 0;
        }
    }

    // BDS announcing a failed boot attempt is the ONE moment worth profiling.
    // StartImage returns the booted image's own status, so "failed to start"
    // carrying "Time out" means the Windows Boot Manager ran and gave up -- and
    // the RIP histogram collected up to this instant says where it was spending
    // that time. Sampled from the firmware's own serial output because there is
    // no other signal for it: no debugger is attached, and kd could not see a
    // UEFI application anyway.
    {
        static const char failNeedle[] = "failed to start Boot";
        static int failPos = 0;
        if (c == failNeedle[failPos]) {
            failPos++;
            if (failNeedle[failPos] == '\0') { g_bdsBootFailed++; failPos = 0; }
        } else {
            failPos = (c == failNeedle[0]) ? 1 : 0;
        }
    }
}

HWND g_hwnd = NULL;
HFONT g_font = NULL;
char g_windowTitle[256] = "Hypervisor.c -- Guest Display";

// Forward declarations: the USB tablet state is defined further down with the
// rest of the EHCI model, but the window procedure below is what feeds it.
extern int usbTabletConfigured;
extern int g_tabletEnabled;
// "The tablet owns the pointer" -- true only when it is both enabled and actually
// configured by the guest. PS/2 keeps the pointer otherwise.
#define LH_TABLET_OWNS_POINTER (g_tabletEnabled && usbTabletConfigured)
extern volatile LONG g_tabletX, g_tabletY, g_tabletButtons, g_tabletWheel, g_tabletDirty;
// Counts pointer updates arriving at the window. Separates "the motion never
// reached us" from "USB never delivered it" -- reports=0 alone cannot tell those
// apart, and that ambiguity has already cost one wrong guess in this file.
extern volatile LONG g_tabletMoves;

// Defined with the watchdog further down. Needed here so a keystroke can end the
// current WHvRunVirtualProcessor call -- see ps2WakeRunLoop.
extern WHV_PARTITION_HANDLE g_watchdogPartition;

// A keystroke has been queued; make sure the run loop gets a turn to deliver it.
//
// PS/2 servicing runs only from the run loop, and that loop only iterates when
// the guest EXITS. A guest sitting at a dialog waiting for input barely exits at
// all, so WHvRunVirtualProcessor does not return, ps2ServiceOutputIrq is never
// called, and the keystroke waits -- which is exactly the reported "input does
// nothing", and why disabling the 50ms re-assert (the only thing that recovered
// it on some later exit) made delivery collapse from 8/15 to 1/15.
//
// Same fault the USB schedules already had, and the same fix: stop depending on
// the guest to generate exits. Cancelling is documented-safe from another thread
// and the run loop already handles WHvRunVpExitReasonCanceled.
//
// STRICTLY EVENT-DRIVEN, once per key event. The existing rtcCancelThread carries
// a hard-won warning: an unconditional cancel every 10ms "perturbs WHV itself
// rather than helping" and made an unrelated early-boot stall reproduce 100% of
// the time. A human generates a few key events a second, which is orders of
// magnitude below that, and none at all while idle. Deliberately NOT applied to
// mouse motion, which can fire hundreds of times a second and would recreate
// precisely the continuous pattern that was reverted.
// DEFAULT OFF: measured, and it makes input WORSE. Paired A/B, alternating arms
// so host drift hits both equally, counting only runs with Setup on screen:
//
//     wake ON   3, 1, 2   =  6/45 keys delivered
//     wake OFF  8, 1, 7   = 16/45
//
// Nearly three times worse with it on. The reasoning behind it still looks sound
// -- servicing really is gated on the guest happening to exit -- but the fix does
// not follow from the diagnosis, and the measurement wins. This is the same
// warning rtcCancelThread already carries: cancelling perturbs WHV itself, and
// that holds even when the cancel is event-driven and bounded to key events
// rather than a continuous 10ms loop.
//
// Kept behind LOCALHOST_PS2_WAKE=1 rather than deleted, so the next person does
// not re-derive it from the same (still reasonable) argument and re-measure it.
int g_ps2WakeEnabled = 0;
static void ps2WakeRunLoop(void) {
    if (g_ps2WakeEnabled && g_watchdogPartition)
        WHvCancelRunVirtualProcessor(g_watchdogPartition, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_KEYDOWN: {
            int ext = 0;
            unsigned char sc = vkToScancode((int)wParam, &ext);
            // Which VKs actually reach this handler, and what they map to. The
            // enqueue ring shows only SPACE arriving while TAB/DOWN/A posted in the
            // same loop never appear -- this separates "the message never got here"
            // from "it got here and mapped to scancode 0".
            // A RING, not a capped log: a "first 40" filled up entirely with the
            // boot-phase keys and showed nothing from the phase that matters.
            g_kbVkRing[g_kbVkLogged % KB_LASTREAD_MAX] = (unsigned char)wParam;
            g_kbVkLogged++;
            // UNBOUNDED and unconditional. Keystrokes are rare (hundreds at most)
            // and stdout is a file here, so there is no cost -- and every bounded
            // or change-gated version of this has left it ambiguous whether a key
            // was missing or merely unprinted.
            if (ps2Trace()) {
                printf("[ps2-key] #%ld WM_KEYDOWN vk=0x%02X -> sc=0x%02X%s  queueDepth=%d dropped=%ld\n",
                       g_kbVkLogged, (unsigned)wParam, sc, ext ? " (E0)" : "",
                       (kbTail - kbHead + PS2_QUEUE_SIZE) % PS2_QUEUE_SIZE, g_kbDropped);
                fflush(stdout);
            }
            if (sc != 0) {
                // Extended keys go out as 0xE0 then the make code. Without the
                // prefix the guest reads a keypad key instead -- see vkToScancode.
                if (ext) { kbEnqueue(0xE0); InterlockedIncrement(&g_kbUserBytes); }
                kbEnqueue(sc);
                InterlockedIncrement(&g_kbUserBytes);
                ps2WakeRunLoop();
            }
            return 0;
        }
        case WM_KEYUP: {
            int ext = 0;
            unsigned char sc = vkToScancode((int)wParam, &ext);
            // Log the BREAK code too. Only makes were logged, so a log full of
            // WM_KEYDOWN and no WM_KEYUP reads exactly like "we never send break
            // codes and the guest is holding every key down" -- a wrong diagnosis
            // this very log led to. Make/break pairing is now visible directly.
            if (ps2Trace()) {
                printf("[ps2-key] #%ld WM_KEYUP   vk=0x%02X -> sc=0x%02X%s  queueDepth=%d dropped=%ld\n",
                       g_kbVkLogged, (unsigned)wParam, (unsigned char)(sc | 0x80),
                       ext ? " (E0)" : "",
                       (kbTail - kbHead + PS2_QUEUE_SIZE) % PS2_QUEUE_SIZE, g_kbDropped);
                fflush(stdout);
            }
            if (sc != 0) {
                // Break codes carry the same 0xE0 prefix as their make codes; a
                // release without it leaves the guest holding the key down.
                if (ext) { kbEnqueue(0xE0); InterlockedIncrement(&g_kbUserBytes); }
                kbEnqueue(sc | 0x80);
                InterlockedIncrement(&g_kbUserBytes);
                ps2WakeRunLoop();
            }
            return 0;
        }
        // Raw device motion, used INSTEAD of WM_MOUSEMOVE deltas for movement.
        //
        // WM_MOUSEMOVE reports cooked coordinates: Windows has already applied the
        // host's pointer acceleration ("Enhance pointer precision"), the pointer
        // speed slider and DPI scaling before we see them. Feeding those to the
        // guest means the guest's own acceleration is applied on top of the host's,
        // so the pointer inherits the host mouse's feel instead of behaving like a
        // real device attached to the guest. Raw Input gives the unfiltered
        // per-device deltas the hardware actually reported, which is what a real
        // PS/2 mouse would deliver.
        //
        // Still gated on the pointer being over the window (auxCursorPosKnown,
        // maintained by WM_MOUSEMOVE/WM_MOUSELEAVE below) so hover semantics are
        // preserved -- raw input is registered with RIDEV_INPUTSINK, which would
        // otherwise deliver motion even while the window is unfocused.
        case WM_INPUT: {
            UINT size = 0;
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &size, sizeof(RAWINPUTHEADER)) == 0 &&
                size > 0 && size <= sizeof(RAWINPUT)) {
                RAWINPUT ri;
                if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == size &&
                    ri.header.dwType == RIM_TYPEMOUSE) {
                    // MOUSE_MOVE_ABSOLUTE means a tablet/remote-desktop style
                    // device reporting screen coordinates; only relative motion is
                    // meaningful for a PS/2 mouse.
                    // Suppressed once the USB tablet is configured: WM_MOUSEMOVE
                    // already reports the absolute position, and feeding relative
                    // deltas in as well would move the guest cursor twice.
                    if (!LH_TABLET_OWNS_POINTER &&
                        !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && auxCursorPosKnown) {
                        int dx = ri.data.mouse.lLastX;
                        int dy = ri.data.mouse.lLastY;
                        if (dx != 0 || dy != 0) auxSendPacket(dx, dy);
                    }
                }
            }
            return DefWindowProc(hwnd, msg, wParam, lParam); // required for WM_INPUT cleanup
        }
        case WM_MOUSEMOVE: {
            int x = (int)(short)LOWORD(lParam);
            int y = (int)(short)HIWORD(lParam);
            // Ask for WM_MOUSELEAVE once per hover. Windows does not send it
            // unless tracking is armed, and it disarms itself after firing, so
            // this is re-armed on each re-entry.
            if (!auxMouseTracking) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                tme.dwHoverTime = 0;
                if (TrackMouseEvent(&tme)) auxMouseTracking = 1;
            }
            // Movement itself is NOT emitted here any more -- WM_INPUT above owns
            // that, using raw unaccelerated deltas. This handler now exists only to
            // know whether the pointer is over the window (which gates raw input)
            // and to arm leave-tracking. Emitting from both would double-count
            // every motion.
            //
            // The out-and-back jump this originally fixed is still handled: leaving
            // clears auxCursorPosKnown, so raw motion is ignored until the pointer
            // is back over the window. PS/2 is a relative device -- it can only say
            // "moved by this much", never "the pointer is now here".
            // TABLET MODE: report the pointer's ABSOLUTE position, scaled to the
            // HID report descriptor's 0..32767 logical range. This is the whole
            // reason the USB tablet exists -- a position, not a delta, so the
            // guest cursor lands exactly where the host cursor is instead of
            // drifting. Scaled against the client rect so it stays correct when
            // the window is resized and the framebuffer is stretched to fit.
            {
                RECT rc, dsp;
                GetClientRect(hwnd, &rc);
                // Against the IMAGE rect, not the window: with letterboxing the
                // two differ, and scaling against the window would offset the
                // guest cursor by the size of the bars.
                guestDisplayRect(rc.right - rc.left, rc.bottom - rc.top, &dsp);
                int w = dsp.right - dsp.left, h = dsp.bottom - dsp.top;
                if (w > 0 && h > 0) {
                    int ix = x - dsp.left, iy = y - dsp.top;
                    int cx = ix < 0 ? 0 : (ix >= w ? w - 1 : ix);
                    int cy = iy < 0 ? 0 : (iy >= h ? h - 1 : iy);
                    InterlockedExchange(&g_tabletX, (LONG)(((LONGLONG)cx * 32767) / (w - 1 > 0 ? w - 1 : 1)));
                    InterlockedExchange(&g_tabletY, (LONG)(((LONGLONG)cy * 32767) / (h - 1 > 0 ? h - 1 : 1)));
                    InterlockedExchange(&g_tabletDirty, 1);
                    InterlockedIncrement(&g_tabletMoves);
                }
            }
            // Once the tablet is configured it owns the pointer; emitting PS/2
            // deltas as well would move the guest cursor twice per motion.
            if (!LH_TABLET_OWNS_POINTER && !g_rawMouseAvailable && auxCursorPosKnown) {
                int dx = x - auxLastCursorX;
                int dy = y - auxLastCursorY;
                if (dx != 0 || dy != 0) auxSendPacket(dx, dy);
            }
            auxLastCursorX = x;
            auxLastCursorY = y;
            auxCursorPosKnown = 1;
            return 0;
        }
        case WM_MOUSELEAVE: {
            // Pointer left the window. Drop the reference point so the next entry
            // re-seeds instead of synthesising a jump, and re-arm tracking.
            auxCursorPosKnown = 0;
            auxMouseTracking = 0;
            return 0;
        }
        case WM_SETCURSOR: {
            // Hide the HOST cursor over the guest's display, so only the GUEST's
            // own cursor is visible. Otherwise two arrows sit on top of each
            // other -- and any difference between them, however small, reads as
            // the pointer being broken.
            //
            // Only over HTCLIENT: the frame, title bar and buttons keep a normal
            // cursor, or the window becomes awkward to move and close.
            //
            // Gated on the tablet owning the pointer, which is the case where the
            // guest cursor is genuinely at the host pointer's position. Under the
            // relative PS/2 mouse the two drift apart, and hiding the host cursor
            // there would remove the only pointer the user can actually trust.
            if (LOWORD(lParam) == HTCLIENT && LH_TABLET_OWNS_POINTER) {
                SetCursor(NULL);
                return TRUE;
            }
            return DefWindowProc(hwnd, msg, wParam, lParam);
        }
        // Buttons feed BOTH devices: the tablet's report byte when it is
        // configured, and the PS/2 aux device otherwise, so clicking still works
        // before USB enumeration completes (and on a guest with no USB stack).
#define LH_TABLET_BUTTON(maskBit, down)                                        \
        do {                                                                   \
            if (down) InterlockedOr(&g_tabletButtons, (maskBit));              \
            else      InterlockedAnd(&g_tabletButtons, ~(LONG)(maskBit));      \
            InterlockedExchange(&g_tabletDirty, 1);                            \
            if (LH_TABLET_OWNS_POINTER) return 0;                              \
        } while (0)

        case WM_LBUTTONDOWN: { LH_TABLET_BUTTON(0x01, 1); auxButtonMask |= 0x01; auxSendPacket(0, 0); return 0; }
        case WM_LBUTTONUP:   { LH_TABLET_BUTTON(0x01, 0); auxButtonMask &= ~0x01; auxSendPacket(0, 0); return 0; }
        case WM_RBUTTONDOWN: { LH_TABLET_BUTTON(0x02, 1); auxButtonMask |= 0x02; auxSendPacket(0, 0); return 0; }
        case WM_RBUTTONUP:   { LH_TABLET_BUTTON(0x02, 0); auxButtonMask &= ~0x02; auxSendPacket(0, 0); return 0; }
        case WM_MBUTTONDOWN: { LH_TABLET_BUTTON(0x04, 1); auxButtonMask |= 0x04; auxSendPacket(0, 0); return 0; }
        case WM_MBUTTONUP:   { LH_TABLET_BUTTON(0x04, 0); auxButtonMask &= ~0x04; auxSendPacket(0, 0); return 0; }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rect;
            GetClientRect(hwnd, &rect);

            // Once the guest has configured a ramfb framebuffer, prefer
            // showing that (it's what OVMF/Windows Setup actually draws
            // to) over the text-mode log rendering below.
            if (ramfbConfigWritten && ramfbWidth > 0 && ramfbHeight > 0 &&
                guestMemory && ramfbAddress + (UINT64)ramfbStride * ramfbHeight <= guestMemSize) {
                BITMAPINFO bmi = { 0 };
                bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bmi.bmiHeader.biWidth = (LONG)ramfbWidth;
                bmi.bmiHeader.biHeight = -(LONG)ramfbHeight; // negative: top-down, matches GOP's row order
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = BI_RGB;
                RECT dst;
                guestDisplayRect(rect.right - rect.left, rect.bottom - rect.top, &dst);
                // Black out only the letterbox strips, not the whole client
                // area: filling everything first would blit the full rect on
                // every repaint and make the guest image flicker.
                if (dst.left > rect.left || dst.top > rect.top ||
                    dst.right < rect.right || dst.bottom < rect.bottom) {
                    HRGN bars = CreateRectRgnIndirect(&rect);
                    HRGN image = CreateRectRgnIndirect(&dst);
                    CombineRgn(bars, bars, image, RGN_DIFF);
                    FillRgn(hdc, bars, (HBRUSH)GetStockObject(BLACK_BRUSH));
                    DeleteObject(image);
                    DeleteObject(bars);
                }
                // DELIBERATELY the cheap stretch mode, despite it dropping the
                // rows and columns that do not survive a scale rather than
                // blending them.
                //
                // HALFTONE was tried here for quality and is unusable: measured
                // on this machine at 1280x720 -> 1050x590 it costs 57.99ms per
                // blit against COLORONCOLOR's 3.48ms -- 16.7x slower, and three
                // and a half times the entire 16.7ms frame budget at 60fps.
                // This runs on the window thread, which also carries input, so
                // it did not merely drop frames: it made the whole VM feel
                // broken. Quality is not worth that, and the fix for
                // readability was the resolution (see the ramfb mode table),
                // not the filter.
                //
                // If blending is ever wanted, it has to be a hand-rolled box
                // filter over the framebuffer, not GDI's.
                SetStretchBltMode(hdc, COLORONCOLOR);
                StretchDIBits(
                    hdc, dst.left, dst.top, dst.right - dst.left, dst.bottom - dst.top,
                    0, 0, ramfbWidth, ramfbHeight,
                    (unsigned char *)guestMemory + ramfbAddress, &bmi, DIB_RGB_COLORS, SRCCOPY);
            } else {
                HFONT oldFont = (HFONT)SelectObject(hdc, g_font);
                SetBkColor(hdc, RGB(0, 0, 0));
                SetTextColor(hdc, RGB(0, 255, 0));

                EnterCriticalSection(&logLock);
                FillRect(hdc, &rect, (HBRUSH)GetStockObject(BLACK_BRUSH));
                DrawTextA(hdc, logBuffer, logLength, &rect, DT_LEFT | DT_TOP | DT_WORDBREAK);
                LeaveCriticalSection(&logLock);

                SelectObject(hdc, oldFont);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        // Closing the console asks the GUEST to shut down; it does not kill it.
        // Returning 0 without calling DefWindowProc deliberately suppresses the
        // default destroy -- the window has to stay alive to keep painting while
        // Windows runs its shutdown sequence, and the process exits only once
        // the guest writes S5 (see the PM1a_CNT handler). The manager's "power
        // off" button drives exactly this path by posting WM_CLOSE.
        case WM_CLOSE:
            InterlockedExchange(&g_powerButtonRequest, 1);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// U56 (user-reported bug): the window intermittently went "not responding" and
// then recovered.
//
// Cause: despite its name this function never created a thread -- it built the
// window on the CALLING (main) thread. A window's messages are delivered to the
// queue of the thread that created it, so the pump only ran when the main loop
// reached its PeekMessage calls. Whenever the guest executed a long stretch
// without exiting -- exactly what an idle or halted guest does, and this guest now
// idles in HalProcessorIdle -- WHvRunVirtualProcessor did not return, no messages
// were pumped, and Windows flagged the window unresponsive. It recovered as soon
// as the guest exited again, which is why the symptom came and went.
//
// Fix: give the window its own thread that both creates it and runs a blocking
// GetMessage loop, so responsiveness no longer depends on guest exit frequency.
// The main loop's InvalidateRect calls still work cross-thread (they post
// WM_PAINT to this thread's queue) and now repaint promptly.
// KEYBOARD CAPTURE.
//
// Without this the host eats the keys the guest most needs. The Windows key opens
// the HOST Start menu, Alt+Tab switches HOST windows, and the guest never sees
// either -- they are never delivered to our window at all, so no amount of work
// in WndProc can recover them. A low-level hook is the only place they can be
// intercepted before the shell claims them.
//
// While our window is in the foreground and capture is on, every keystroke is
// posted to our window and then SWALLOWED (return 1) so the host never processes
// it. Because it is swallowed, the normal focus path delivers nothing, so each
// key still arrives exactly once -- no double input.
//
// RIGHT CTRL IS THE RELEASE KEY, and it is not optional: capture that swallows
// Alt+Tab with no way out would trap the user in the window. It toggles capture,
// is never forwarded to the guest, and the state is shown in the title bar.
// Deliberately right Ctrl -- the same convention VirtualBox uses, and a key
// almost nothing needs.
//
// Scoped to the foreground check: when our window is not focused this hook does
// nothing at all, so it can never interfere with the rest of the desktop.
int g_kbCaptureEnabled = 1;
static HHOOK g_kbHook = NULL;

static void updateCaptureTitle(void) {
    char buf[256];
    if (!g_hwnd) return;
    _snprintf(buf, sizeof(buf) - 1, "%s%s", g_windowTitle,
              g_kbCaptureEnabled ? "  [keys captured -- Right Ctrl to release]"
                                 : "  [keys released -- Right Ctrl to capture]");
    buf[sizeof(buf) - 1] = '\0';
    SetWindowTextA(g_hwnd, buf);
}

static LRESULT CALLBACK lowLevelKbProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION && g_hwnd && GetForegroundWindow() == g_hwnd) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;
        int isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        int isUp   = (wParam == WM_KEYUP   || wParam == WM_SYSKEYUP);
        if (k->vkCode == VK_RCONTROL) {          // the release key
            if (isDown) {
                g_kbCaptureEnabled = !g_kbCaptureEnabled;
                updateCaptureTitle();
                printf("[kbd-capture] %s\n", g_kbCaptureEnabled ? "ON" : "OFF");
                fflush(stdout);
            }
            return 1;                            // never reaches host or guest
        }
        if (g_kbCaptureEnabled && (isDown || isUp)) {
            PostMessage(g_hwnd, isDown ? WM_KEYDOWN : WM_KEYUP, k->vkCode, 0);
            return 1;                            // host never sees it
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wParam, lParam);
}

static DWORD WINAPI windowThreadProc(LPVOID param) {
    (void)param;
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "HypervisorWindowClass";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowA("HypervisorWindowClass", g_windowTitle,
                            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            740, 480, NULL, NULL, GetModuleHandle(NULL), NULL);
    ShowWindow(g_hwnd, SW_SHOW);

    // Installed on THIS thread because a low-level hook is dispatched on the
    // thread that set it, and that thread must pump messages -- which this one
    // does, below. Installing it from the VM thread would silently never fire.
    g_kbHook = SetWindowsHookExA(WH_KEYBOARD_LL, lowLevelKbProc, GetModuleHandle(NULL), 0);
    if (!g_kbHook) {
        printf("[kbd-capture] SetWindowsHookEx failed (%lu) -- the Windows key will "
               "keep opening the HOST Start menu\n", (unsigned long)GetLastError());
        fflush(stdout);
    }
    updateCaptureTitle();

    // Raw Input for the mouse, so the guest receives unaccelerated device deltas
    // rather than host-cooked coordinates (see the WM_INPUT handler for why).
    // Usage page 0x01 / usage 0x02 is the generic-desktop mouse. RIDEV_INPUTSINK
    // delivers even when unfocused, which is what makes plain hover work; the
    // handler itself gates on the pointer actually being over the window.
    {
        RAWINPUTDEVICE rid;
        rid.usUsagePage = 0x01;
        rid.usUsage = 0x02;
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = g_hwnd;
        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            printf("[window] RegisterRawInputDevices failed (%lu) -- mouse will fall back to cooked coordinates\n",
                   (unsigned long)GetLastError());
            fflush(stdout);
            g_rawMouseAvailable = 0;
        } else {
            g_rawMouseAvailable = 1;
        }
    }

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

void createWindowThread() {
    HANDLE h = CreateThread(NULL, 0, windowThreadProc, NULL, 0, NULL);
    if (!h) {
        printf("[window] CreateThread failed (%lu) -- falling back to main-thread window\n",
               (unsigned long)GetLastError());
        fflush(stdout);
        windowThreadProc(NULL); // degraded, but better than no window at all
        return;
    }
    CloseHandle(h);
    // Callers (and the main loop's InvalidateRect) expect g_hwnd to be valid on
    // return, so wait for the thread to publish it rather than racing it.
    int spins = 0;
    while (!g_hwnd && spins++ < 5000) Sleep(1);
}

// Returns non-zero if the guest currently has interrupts enabled (EFLAGS.IF).
int guestInterruptsEnabled(WHV_PARTITION_HANDLE partition) {
    WHV_REGISTER_NAME regName = WHvX64RegisterRflags;
    WHV_REGISTER_VALUE regValue = { 0 };
    HRESULT hr = WHvGetVirtualProcessorRegisters(partition, 0, &regName, 1, &regValue);
    if (FAILED(hr)) return 0;
    return (regValue.Reg64 & (1ULL << 9)) != 0; // bit 9 = IF
}

// Injects a hardware interrupt vector directly into the vCPU via the
// "pending interruption" register. This bypasses any PIC/APIC model --
// it's a minimal stand-in until real 8259 PIC emulation exists.
// DISABLED for now, but the earlier explanation of WHY was WRONG and is corrected
// here.
//
// WHvRequestInterrupt is the right API for device interrupts: it goes through the
// virtual APIC, respecting TPR/ISR priority and queuing in IRR rather than
// overwriting a single slot. It failed with WHV_E_INVALID_PARTITION_CONFIG
// (0xC0350005), and I previously recorded that as "the partition lacks LAPIC
// emulation". That is false -- WHvX64LocalApicEmulationModeXApic is set at
// partition creation whenever uefiMode is on, which is always.
//
// Counting outcomes separately showed what is really happening: ok=1 fail=1, with
// the failure on VECTOR 0x08. The local APIC cannot deliver vectors below 0x10 --
// 0..15 are reserved for CPU exceptions -- so the API correctly rejects it. The
// call works fine for legal vectors.
//
// Vector 0x08 is our LEGACY PIC-remapped fallback, used by ioapicResolveVector
// while an IOAPIC entry is still pristine. The keyboard's legacy fallback is 0x09,
// also below 0x10 and therefore also illegal to deliver through the APIC -- and
// meaningless to a Windows kernel in APIC mode even via raw injection. That is a
// real bug in its own right and a strong candidate for why main-loop PS/2 IRQ
// delivery destabilises the guest.
//
// Switching wholesale still needs care: with the API enabled, boot did not survive
// (guest exited ~20s in). Legacy vectors must stop being injected once the guest
// is in APIC mode before this can be turned on.
// STILL 0. With step 1 in place (no illegal low vectors reach here) the API no
// longer fails -- zero WHvRequestInterrupt failures across a whole boot -- but the
// GUEST does: it triple-faulted 14s in, "[Unhandled exit reason: 4]"
// (UnrecoverableException), immediately after the first RTC tick was delivered
// through the APIC as vector 0xD1. The identical tick delivered by raw injection
// is fine. So routing real device interrupts through WHvRequestInterrupt needs
// more than a legal vector -- most likely EOI/level semantics we do not model yet.
// Left wired up behind this switch since the vector split it needed is now done.
#define U99_REQUEST_INTERRUPT 0
long g_reqIrqOk = 0, g_reqIrqFail = 0;
HRESULT g_reqIrqLastHr = 0;
unsigned char g_reqIrqLastVector = 0;

void injectInterrupt(WHV_PARTITION_HANDLE partition, unsigned char vector) {
    logBpEvent("injectInterrupt vector=0x%02X", vector);

#if U99_REQUEST_INTERRUPT
    // Deliver through the emulated LOCAL APIC rather than forcing an event in.
    //
    // WHvRegisterPendingInterruption (below) is raw event injection: a SINGLE-SLOT
    // register with no priority, no queuing and no EOI awareness. Setting it while
    // an interruption is already pending silently loses the previous one, and
    // forcing a vector in while the guest is still inside that vector's ISR
    // desynchronises the LAPIC's in-service bookkeeping. Once that happens the
    // vector can stop being delivered permanently.
    //
    // That is exactly the observed USB stall: acknowledgements and report delivery
    // freeze together after ~2000 successful interrupts, while we keep injecting to
    // an unmasked GSI on the vector Windows itself programmed (dropped=0). AHCI has
    // survived this all along only because storahci polls as well as taking
    // interrupts; USBPORT is purely interrupt-driven and is the first subsystem
    // here that cannot tolerate a lost or blocked one.
    //
    // WHvRequestInterrupt is the API meant for device interrupts: it goes through
    // the virtual APIC, which respects TPR/ISR priority and queues in IRR instead
    // of overwriting. Edge trigger is correct here because the level behaviour is
    // already modelled above by re-asserting while the condition holds.
    // Only vectors 0x10 and above can go through the APIC -- 0..15 are reserved
    // for CPU exceptions and WHvRequestInterrupt rejects them
    // (WHV_E_INVALID_PARTITION_CONFIG). Firmware legitimately uses the low
    // PIC-remapped vectors while the guest is still in PIC mode, so those keep
    // using raw injection, which is what they have always used and what boot
    // depends on. Once the guest is in APIC mode ioapicResolveVector no longer
    // hands out low vectors at all (see g_guestApicMode), so this split is
    // temporary by construction rather than a permanent special case.
    if (vector < 0x10) goto legacyInjection;

    WHV_INTERRUPT_CONTROL interrupt;
    memset(&interrupt, 0, sizeof(interrupt));
    interrupt.Type = WHvX64InterruptTypeFixed;
    interrupt.DestinationMode = WHvX64InterruptDestinationModePhysical;
    interrupt.TriggerMode = WHvX64InterruptTriggerModeEdge;
    interrupt.Destination = 0;             // APIC ID of the single vCPU
    interrupt.Vector = vector;
    HRESULT irqHr = WHvRequestInterrupt(partition, &interrupt, sizeof(interrupt));
    // Count outcomes separately. Last time this was tried, ONE failure was logged
    // (vector 0x08) and I concluded from it that the partition lacked LAPIC
    // emulation -- which is false: WHvX64LocalApicEmulationModeXApic is set at
    // creation whenever uefiMode is on, which is always. Whether the failure is
    // systematic or a one-off at a particular moment changes the diagnosis
    // completely, so measure it instead of inferring from a single line.
    if (SUCCEEDED(irqHr)) { g_reqIrqOk++; return; }
    g_reqIrqFail++;
    g_reqIrqLastHr = irqHr;
    g_reqIrqLastVector = vector;
    // Fall through to the legacy path if the platform refuses the request, so a
    // failure here degrades to the old behaviour rather than dropping the IRQ.
    {
        static int reqFailLogged = 0;
        if (reqFailLogged < 5) {
            reqFailLogged++;
            printf("[irq] WHvRequestInterrupt failed hr=0x%lX vector=0x%02X (ok=%ld fail=%ld) -- using legacy injection\n",
                   (unsigned long)irqHr, vector, g_reqIrqOk, g_reqIrqFail);
            fflush(stdout);
        }
    }
legacyInjection:
#endif

    WHV_REGISTER_NAME regName = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE regValue = { 0 };

    UINT64 pending = 0;
    pending |= 1ULL;                       // InterruptionPending = 1
    pending |= ((UINT64)0ULL << 1);        // InterruptionType = WHvX64PendingInterrupt (0)
    pending |= ((UINT64)vector << 16);     // InterruptionVector (bits 16-31)
    regValue.Reg64 = pending;

    WHvSetVirtualProcessorRegisters(partition, 0, &regName, 1, &regValue);
}

// Real, stateful 256-byte config spaces for the handful of functions we
// model. Earlier this stub answered every register with a fixed canned
// constant and silently discarded all writes ("nothing we model is
// writable"). That broke down once PCI bus enumeration started doing real
// write-then-read-back verification (the standard way to probe which
// registers/bits are actually implemented, and how BAR sizing works): a
// write that never sticks makes that verification -- and any driver logic
// built on top of it -- retry forever. Backing each function with real
// read/write storage lets writes actually stick, while a handful of fields
// stay read-only (identity/class registers) or hardwired to 0 (BARs,
// expansion ROM, capabilities pointer).
//
// We model three functions because real i440fx platforms -- which is what
// OVMF's PlatformPei detects and assumes once it sees our host bridge's
// device ID -- always have a PIIX3 ISA bridge (0:1.0) and PIIX4 power
// management function (0:1.3) alongside the host bridge (0:0.0); platform
// code can reasonably assume both unconditionally exist and hang/retry
// waiting for a device that, in an incomplete stub, simply never appears.
unsigned char pciHostBridgeConfig[256] = { 0 }; // 0:0.0 -- i440fx host bridge
unsigned char pciIsaBridgeConfig[256] = { 0 };  // 0:1.0 -- PIIX3 ISA bridge
unsigned char pciPmConfig[256] = { 0 };         // 0:1.3 -- PIIX4 power management
unsigned char pciAhciConfig[256] = { 0 };       // 0:2.0 -- AHCI (SATA) controller
unsigned char pciRtl8139Config[256] = { 0 };    // 0:3.0 -- RTL8139 (primary NIC)
unsigned char pciE1000Config[256] = { 0 };      // 0:5.0 -- Intel 82540EM (backup NIC)
unsigned char pciEhciConfig[256] = { 0 };       // 0:4.0 -- EHCI USB 2.0 controller

// --- EHCI (USB 2.0) host controller ---------------------------------------
//
// WHY: absolute ("tablet") pointing. The PS/2 aux device is inherently RELATIVE
// -- auxSendPacket sends dx/dy deltas -- so the host and guest cursors drift
// apart and can never track 1:1 the way VMware does. Absolute positioning needs
// a device whose reports carry coordinates, and the only way to get one into
// stock Windows with NO guest additions is a USB HID tablet: Windows binds it
// with inbox drivers. Confirmed present in this WinPE via the debugger --
// usbehci, USBXHCI, USBPORT, usbhub, hidparse and mouclass are all loaded.
//
// EHCI rather than xHCI because the register model is far smaller (a capability
// block plus ~10 operational registers) while still being a controller this
// guest already has a driver for. Our tablet is declared HIGH-SPEED so no
// companion controller is needed -- EHCI alone cannot talk to full/low-speed
// devices, and modelling a UHCI/OHCI companion purely to host one HID device
// would double the work for nothing.
#define EHCI_BAR_SIZE   0x1000
#define EHCI_CAPLENGTH  0x20     // operational registers start here, within the BAR
#define GSI_EHCI        20       // matches the _PRT entry for device 4 in acpi/dsdt.asl

UINT32 ehciBarBase = 0;          // guest-programmed BAR0 GPA, once set
int ehciBarMapped = 0;           // BAR0 programmed => MMIO is being decoded
int ehciBarSizing = 0;           // guest wrote 0xFFFFFFFF to probe the BAR size

// Operational register state. Kept as named fields rather than a backing buffer
// because almost every one of them has real semantics (RW1C change bits, a reset
// that must self-clear), and the AHCI ABAR taught us that letting a driver store
// status bits verbatim produces exactly the sort of self-sustaining interrupt
// storm U61 had to unpick.
UINT32 ehciUsbCmd = 0;
UINT32 ehciUsbSts = 0x00001000;  // HCHalted set: the controller starts halted
UINT32 ehciUsbIntr = 0;
UINT32 ehciFrIndex = 0;
UINT32 ehciCtrlDsSegment = 0;
UINT32 ehciPeriodicBase = 0;
UINT32 ehciAsyncBase = 0;
UINT32 ehciConfigFlag = 0;
// PORTSC for our single root port. Powered, with the tablet permanently attached:
// CCS (bit0) = device present, CSC (bit1) = connect change pending so the hub
// driver notices, PP (bit12) = port powered.
UINT32 ehciPortSc = 0x00001003;

// Uncapped, per the standing rule in this file: six times now a capped log has
// produced a wrong conclusion here. "usbehci is talking to us" must be a counter,
// never an inference from how many lines a log happened to print.
long g_ehciMmioReads = 0, g_ehciMmioWrites = 0, g_ehciPortResets = 0;
int g_ehciIrqPending = 0;   // set when a qTD with IOC retires, drained by the run loop
long g_ehciIrqCount = 0, g_ehciLastIrqExit = 0, g_ehciIrqDropped = 0;
UINT64 g_lastIntQh = 0;   // most recent interrupt-endpoint QH seen in the periodic list
long g_ehciUsbStsWrites = 0;        // driver acknowledgements == its ISR running
long g_ehciDoorbells = 0;           // async-advance doorbell rings answered
UINT32 g_ehciLastUsbStsWritten = 0; // what it last wrote there
int pciConfigSpacesInit = 0;

// PM1a_CNT_BLK (pmBase+4): bit0 is SCI_EN ("ACPI mode enabled"). Real
// hardware sets this only after an SMI handler processes a write to the
// SMI_CMD port (0xB2, PIIX4's fixed default) -- we don't emulate SMM at
// all, so a BIOS/OS that writes 0xB2 and then polls PM1a_CNT (or the
// legacy 0xB3 SMI-status port) waiting for that bit would spin forever.
// Setting SCI_EN synchronously on the 0xB2 write stands in for "the SMI
// handler ran instantly."
//
// Our FADT (acpiBuildTables) declares SMI_CMD=0, which per the ACPI spec
// means "ACPI mode is already enabled, no SMM handshake needed" -- so
// SCI_EN must already read as 1 at boot, or anything that checks it
// (confirmed live: SeaBIOS itself, spinning forever reading port 0x604
// with SCI_EN never observed set) hangs waiting for a handshake that will
// never happen since nothing ever writes SMI_CMD=0. Starting this at 1
// keeps the two in sync.
UINT16 pm1aControl = 0x1;

// PM1a_EVT_BLK (pmBase+0): PM1a_STS at +0, PM1a_EN at +2, 4 bytes total to
// match the FADT's PM1_EVT_LEN. This is how a guest learns the power button
// was pressed, and until it existed there was NO way to ask the guest to shut
// itself down -- the manager's "power off" could only kill the process, which
// is a power cut as far as the guest is concerned and lands Windows in
// Automatic Repair on the next boot.
//
// The FADT flags (0x101) deliberately leave PWR_BUTTON clear, which tells the
// OS the power button is FIXED HARDWARE rather than a control-method device --
// so this register pair is the whole interface, with no AML object needed.
UINT16 pm1aStatus = 0;
UINT16 pm1aEnable = 0;
#define PM1_PWRBTN  0x0100   // PWRBTN_STS / PWRBTN_EN are both bit 8
#define PM1_SLP_EN  0x2000   // PM1a_CNT bit 13: "act on SLP_TYP now"
#define GSI_SCI     9        // matches FADT SCI_INT and the MADT override


// PORT_SMI_STATUS (0xB3): SeaBIOS's SMM relocation code
// (smm_relocate_and_restore()) writes 0x01 here, writes 0x00 to
// PORT_SMI_CMD (0xB2) to trigger an SMI, then spins on `while
// (inb(PORT_SMI_STATUS) != 0x00);` waiting for the SMI handler's first
// action (clearing status back to 0) as an acknowledgment. We don't
// emulate SMM/SMI delivery, so the 0xB2 write clears this synchronously.
unsigned char smiStatus = 0;

// --- AHCI (SATA) controller: BAR5/ABAR state ---
// BAR5 (offset 0x24) is the one register pciRegisterIsReadOnly's generic
// "BARs are hardwired to 0" rule doesn't apply to here -- unlike the
// host bridge/PIIX3/PIIX4, this device needs a real, guest-programmable
// memory BAR so the driver can find its register block. See
// ahciHandleBar5Access and pciHandleConfigAccess.
#define AHCI_BAR_SIZE 0x2000  // 8KB: comfortably covers HBA regs (0x100) + one port block (0x80)
int ahciBar5Sizing = 0;       // true after the guest probes the BAR size with an all-1s write
UINT32 ahciAbarBase = 0;      // guest-programmed ABAR GPA, once set
void *ahciAbarMemory = NULL;  // host backing buffer for the mapped ABAR window
int ahciAbarMapped = 0;

// --- RTL8139 NIC -- Phase 1: PCI identity, BAR0 (I/O space) sizing, and
// enough of the register file for a driver to complete reset/init and see
// link-up. No TX/RX data movement or host networking yet (see
// docs/roadmap.md) -- unlike AHCI's BAR (memory-mapped, backed by real
// guest RAM the driver polls directly), RTL8139's BAR0 is I/O-space, so
// register accesses come through the same io-port dispatch switch as
// ATA/UART/PIT etc. rather than through WHvMapGpaRange.
#define RTL8139_IO_SIZE 0x100
int rtl8139Bar0Sizing = 0;    // true after the guest probes the BAR size with an all-1s write
UINT32 rtl8139IoBase = 0;     // guest-programmed BAR0 I/O base, once set (0 = not yet programmed)

// BAR1 -- the MEMORY window onto the same 256-byte register file. A real RTL8139
// exposes both: BAR0 decodes I/O space, BAR1 decodes memory space, and they alias
// the identical registers. We implemented only BAR0, so PnP could never offer the
// driver a memory resource, and two separate measurements pointed here:
//   - as plain 8139 (PCI rev 0x10) the driver read TCR twice over I/O and gave up;
//   - as 8139C+ (rev 0x20) it failed with kernelAccesses=0, never touching a
//     register at all -- C+ is the memory-mapped datapath, so it goes looking for
//     the memory window before it does anything else and quits when there is none.
// Both end in Code 10 / STATUS_UNSUCCESSFUL.
// Left unmapped like the EHCI BAR (not backed by a WHvMapGpaRange buffer) so every
// access faults out and runs the same side effects as the I/O path -- registers
// like ISR and the TSDs must not behave as passive RAM just because they were
// reached through memory instead of ports.
int rtl8139Bar1Sizing = 0;
UINT32 rtl8139MmioBase = 0;   // guest-programmed BAR1 memory base (0 = not programmed)
int g_nicMmioClobbered = 0;   // set if a scratch page ever got mapped over BAR1

// Frame counts at the device layer, so "the guest never transmitted" can be told
// apart from "we transmitted and the backend dropped it".
long g_netTxFrames = 0, g_netRxFrames = 0;
long g_netRxOffered = 0, g_netRxDropSize = 0;
long g_netRxPadded = 0;   // runts padded up to the 60-byte Ethernet minimum
// Frames the device could not take RIGHT NOW but which are not lost: they go on
// the pending queue and are delivered when the driver is ready again. These are
// deferrals, not drops -- the distinction matters, because a drop here is
// unrecoverable (see the queue's own comment).
long g_netRxDeferNoRe = 0, g_netRxDeferNoBuf = 0, g_netRxDeferRingFull = 0;
long g_netRxQueuePeak = 0, g_netRxQueueOverflow = 0;
long g_tcpRingBackpressure = 0; // times we left data in the host socket instead
long g_netTxArp = 0, g_netTxIpv4 = 0, g_netTxIpv6 = 0, g_netTxOther = 0;
long g_nicIrqInjected = 0, g_nicIrqLatched = 0, g_nicIrqMaskedOff = 0;
long g_netArpLogged = 0, g_netArpReplies = 0;
long g_netRingReEnables = 0, g_netRingResetSkipped = 0;

// BAR1 is advertised as a full PAGE rather than the chip's true 256 bytes. WHP can
// only map and protect at page granularity, so a 256-byte BAR would share its page
// with whatever PnP put next to it, and the read-only mapping below would silently
// answer that neighbour's reads out of our buffer. Asking for a page guarantees we
// own it outright. The register file is still 256 bytes; the rest reads as zero.
#define RTL8139_MMIO_SIZE 0x1000

// Reads are served DIRECTLY from this page by the CPU; only writes trap.
//
// This replaces decoding every instruction that touches the window, which was a
// losing game: three different guest instruction forms showed up in three
// consecutive runs (`and word ptr` RMW, `test byte ptr`, `movzx r32, byte ptr`),
// two of them ordinary reads, and each undecodable one cost us the whole BAR
// because the generic fallback maps RAM over it. Letting real silicon execute the
// reads makes every read form work forever -- including their flag effects, which
// hand-emulation kept getting wrong -- and leaves only writes, which genuinely
// need side effects, to be decoded.
//
// rtl8139Regs is re-pointed INTO this page rather than copied, so a register the
// device itself updates (ISR on RX/TX, CBR, BUFE) is visible to the guest the
// instant we write it. A copy would have gone stale between syncs, and the driver
// polls ISR.
void *rtl8139MmioPage = NULL;
int rtl8139MmioMapped = 0;
// The register file. A POINTER, not an array, because once BAR1 is programmed it
// is re-pointed at the page we map into the guest -- see rtl8139MmioPage below.
// Indexing syntax is unchanged everywhere else.
unsigned char rtl8139RegsStorage[RTL8139_IO_SIZE] = { 0 };
unsigned char *rtl8139Regs = rtl8139RegsStorage;

// PCI revision ID. 0x20 = RTL8139C+, paired with the C+ hardware-version word in
// g_rtlHwVerId far below -- see the long note there for why C+ is the identity
// worth reporting. Declared up here because pciInitConfigSpaces() needs it and
// runs earlier in the file. Overridable alongside the version word so the two can
// be swept as a pair: LOCALHOST_RTL_PCIREV=0x10.
unsigned char g_rtlPciRev = 0x20;

// Locally-administered MAC (52:54:00:xx:xx:xx is the same OUI prefix QEMU's
// own emulated NICs use for exactly this purpose -- a safe, real-hardware-
// conflict-free address for an emulated device).
unsigned char rtl8139Mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
unsigned char e1000Mac[6] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x57 };

// --- RTL8139 Phase 2: TX descriptor handling, RX ring buffer, and
// interrupt generation. Deliberately kept separate from the networking
// backend -- rtl8139TransmitFrame (device -> backend) and
// rtl8139ReceiveFrame (backend -> device) are the only two entry points
// that will need to change when the real slirp-style NAT backend replaces
// today's log/test-injection stubs.
//
// TX: RTL8139 has 4 fixed descriptor slots (not a true ring) -- TSAD0-3
// (0x20/0x24/0x28/0x2C, the guest-physical frame address) and TSD0-3
// (0x10/0x14/0x18/0x1C, status+size). A write to a TSDx register is the
// real hardware's "submit this frame" trigger; we do the whole DMA-read +
// hand-to-backend + status-update synchronously in that same write,
// matching how real hardware transmits fast enough that a driver's
// completion poll basically never sees "in progress."
//
// RX: a single circular buffer at RBSTART (0x30), sized by RCR's RBLEN
// bits, that we DMA-write received frames into with the standard 4-byte
// status+length header real hardware prefixes each packet with. CAPR
// (0x38) is the guest's read pointer, CBR (0x3A) mirrors our own write
// pointer.
//
// IRQ: real hardware routes RTL8139 through its assigned PCI INTx line via
// PIRQ/APIC routing we don't model (see injectInterrupt's own comment --
// this whole codebase bypasses PIC/APIC routing and hardcodes vectors for
// every device). IRQ11 (vector 0x73, the standard legacy master-PIC-range
// vector for IRQ11) is unused by every other emulated device, so it's used
// here the same way IRQ14/vector 0x76 is hardcoded for ATA.
UINT32 rtl8139RxWritePos = 0; // our own tracked ring write offset (mirrored into CBR)
int pendingRtl8139Irq = 0;    // same latch-until-IF=1 pattern as pendingAtaIrq

// U108: every [u106-reset] capture found a HEALTHY adapter -- ISR=0x0000, no error
// flags, ring drained, pointers in sync -- so the driver is not reacting to anything
// we reported. That leaves a watchdog, and a watchdog is identifiable by WHAT went
// quiet before it fired. Register READS cannot be observed (BAR1 is mapped read-only
// so the CPU serves them from our page without trapping) and unmapping to trap them
// routes every read through an instruction decoder that can fail -- too risky to run
// unattended. Timestamping the paths we DO own gives the same discrimination safely:
//   TX quiet  -> send watchdog          RX quiet  -> receive watchdog
//   IRQ quiet -> interrupt starvation   none quiet-> a timer, not activity-driven
LARGE_INTEGER g_u108LastTx, g_u108LastRx, g_u108LastIrq, g_u108LastCapr;
double u108MsSince(LARGE_INTEGER then) {
    LARGE_INTEGER now;
    if (!then.QuadPart || !perfFrequency.QuadPart) return -1.0;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - then.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart;
}

void rtl8139MaybeInjectIrq(WHV_PARTITION_HANDLE partition) {
    UINT16 isr = *(UINT16 *)&rtl8139Regs[0x3E];
    UINT16 imr = *(UINT16 *)&rtl8139Regs[0x3C];
    if ((isr & imr) == 0) {
        // Pending but MASKED. Counted separately because this is the state that
        // used to strand a received frame: the cause stays set in ISR, no
        // interrupt is raised, and nothing re-checked when the mask was lifted.
        if (isr) g_nicIrqMaskedOff++;
        return;
    }
    if (guestInterruptsEnabled(partition)) {
        g_nicIrqInjected++; QueryPerformanceCounter(&g_u108LastIrq);
        injectDeviceIrq(partition, GSI_NIC, 0x73);
    } else {
        g_nicIrqLatched++;
        pendingRtl8139Irq = 1;
    }
}

void deliverPendingRtl8139Irq(WHV_PARTITION_HANDLE partition) {
    if (pendingRtl8139Irq && guestInterruptsEnabled(partition)) {
        injectDeviceIrq(partition, GSI_NIC, 0x73);
        pendingRtl8139Irq = 0;
    }
}

// Bytes of ring the guest has NOT yet consumed, derived from CAPR (its own read
// pointer). Needed because nothing ever checked for space: rtl8139ReceiveFrame
// wraps to offset 0 when a frame will not fit, happily overwriting data the
// driver has not read. With the TCP drain loop pushing up to 8 x 1400 bytes per
// poll that is easy to hit, and a single clobbered segment breaks the stream
// permanently -- we have no retransmission, so the guest can never recover the
// missing bytes. Observed as TLS flights arriving incomplete: the guest gets the
// ServerHello, never answers, and the server closes.
UINT32 rtl8139RxRingUsed(void);
UINT32 rtl8139RxRingSize(void) {
    UINT32 rcr = *(UINT32 *)&rtl8139Regs[0x44];
    UINT32 rblen = (rcr >> 11) & 0x3;
    return 8192u << rblen; // RBLEN 00/01/10/11 -> 8K/16K/32K/64K
}

UINT32 rtl8139RxRingUsed(void) {
    UINT32 ringSize = rtl8139RxRingSize();
    if (ringSize == 0) return 0;
    // CAPR is conventionally (read offset - 16).
    UINT16 capr = *(UINT16 *)&rtl8139Regs[0x38];
    UINT32 readPos = ((UINT32)capr + 16) % ringSize;
    UINT32 writePos = rtl8139RxWritePos % ringSize;
    return (writePos - readPos) % ringSize;
}

// Headroom before the ring would overwrite unread data. A generous margin is
// kept free so a maximum-size frame can never straddle the read pointer.
UINT32 rtl8139RxRingFree(void) {
    UINT32 ringSize = rtl8139RxRingSize();
    UINT32 used = rtl8139RxRingUsed();
    if (ringSize <= used) return 0;
    UINT32 freeBytes = ringSize - used;
    return (freeBytes > 4096u) ? (freeBytes - 4096u) : 0u;
}

void rtl8139ReceiveFrame(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len);
void rtl8139FlushRxQueue(WHV_PARTITION_HANDLE partition);
void e1000ReceiveFrame(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len);
void e1000Reset(void);
int e1000RxEnabled(void);
int e1000RxCanAccept(UINT32 bytes);
int e1000HandleMmio(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext);
void e1000HandleBar0Access(WHV_X64_IO_PORT_ACCESS_CONTEXT *io, UINT32 baseOffset, UINT32 accessSize, UINT64 *rax);
void e1000HandleBar1Access(WHV_X64_IO_PORT_ACCESS_CONTEXT *io, UINT32 baseOffset, UINT32 accessSize, UINT64 *rax);
void e1000HandleIoAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext);
void deliverPendingE1000Irq(WHV_PARTITION_HANDLE partition);

// --- Pending RX queue ------------------------------------------------------
// By the time a frame reaches the device layer it has ALREADY been taken out of
// the host socket, so dropping it here is unrecoverable: this model has no
// retransmission, and one missing segment stalls a TCP stream permanently --
// the exact shape of the long-running symptom (the guest takes in a whole
// server flight, then says nothing).
//
// Three separate paths used to drop such a frame outright, and all three fire
// during NORMAL operation, not just under stress:
//   * RE clear -- the driver toggles RE off and on constantly (W37=4 -> W37=C
//     observed repeatedly); every frame handed to us inside one of those
//     windows was destroyed.
//   * RBSTART not yet programmed -- a race at driver init.
//   * ring momentarily full -- the driver simply had not drained yet.
// None of those mean "this frame is bad", they mean "not right now", so queue
// it and deliver it when the driver is ready. Only genuinely undeliverable
// frames (oversized, no guest memory) and a full queue drop.
#define RTL8139_RX_QUEUE_DEPTH 64
#define RTL8139_RX_QUEUE_FRAME 1792
typedef struct {
    UINT32 len;
    unsigned char data[RTL8139_RX_QUEUE_FRAME];
} Rtl8139RxQueued;
Rtl8139RxQueued rtl8139RxQueue[RTL8139_RX_QUEUE_DEPTH];
UINT32 rtl8139RxQueueHead = 0;    // index of the oldest queued frame
UINT32 rtl8139RxQueueCount = 0;

// True when the device can take a frame of this size immediately. Used by the
// TCP drain loop: with the queue in place, "the ring is full" no longer loses
// data, but pulling more out of the host socket than the guest can absorb just
// moves the backlog into our queue instead of leaving it where the real peer's
// own flow control can see it. Keep it in the socket.
int rtl8139RxCanAccept(UINT32 bytes) {
    return rtl8139RxQueueCount == 0 && rtl8139RxRingFree() >= bytes;
}

void netReceiveFrame(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len) {
    // RTL8139 is the primary NIC. The e1000 only sees host RX while the
    // Realtek receiver is off, so the backup Intel card can still DHCP if
    // 8139too is unbound.
    if (rtl8139Regs[0x37] & 0x08) rtl8139ReceiveFrame(partition, frame, len);
    else e1000ReceiveFrame(partition, frame, len);
}

int netRxCanAccept(UINT32 bytes) {
    if (rtl8139Regs[0x37] & 0x08) return rtl8139RxCanAccept(bytes);
    if (e1000RxEnabled()) return e1000RxCanAccept(bytes);
    return rtl8139RxCanAccept(bytes);
}

// =====================================================================
// Phase 3: slirp-style NAT networking backend.
//
// Deliberately kept behind exactly two entry points, same as Phase 2 left
// it: rtl8139TransmitFrame (device -> backend, called when the guest
// submits a TX descriptor) and rtl8139ReceiveFrame (backend -> device,
// injects a frame into the RX ring). Nothing in here reaches back into the
// register file or ring-buffer mechanics directly -- the RTL8139 emulation
// stays completely unaware this backend exists versus, say, a future
// bridged or host-only backend swapped in behind the same two functions.
//
// Portability: the only Windows-specific pieces are socket-library
// startup/teardown and a couple of naming differences (closesocket vs
// close, ioctlsocket vs fcntl) -- isolated in the tiny shim right below.
// Everything else (socket/bind/connect/send/recv/sendto/recvfrom/select)
// is plain BSD sockets, the subset Winsock and POSIX sockets share, so a
// future Linux/macOS build could reuse this section largely unchanged.
//
// Contents, in order: platform shim + the RTL8139<->backend seam
// (g_netTransmit) -> virtual addressing constants -> wire-format read/
// write and checksum helpers -> netSendIpFrame (the shared reply-framing
// helper everything below uses) -> ARP -> ICMP -> DHCP -> general UDP NAT
// (DNS relay is a special case of it) -> TCP NAT -> netHandleIpv4
// (protocol dispatch) -> netSlirpTransmit + rtl8139TransmitFrame (the
// device-facing entry points) -> rtl8139ReceiveFrame (Phase 2, backend ->
// device, unchanged since it's already backend-agnostic).
// =====================================================================

typedef SOCKET netsock_t;
#define NETSOCK_INVALID INVALID_SOCKET
#define netCloseSocket closesocket

int netSetNonBlocking(netsock_t s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}

// Every real socket this backend opens (UDP NAT sessions, TCP NAT
// sessions) is non-blocking and IPv4 -- shared here rather than repeated
// at each call site.
netsock_t netCreateNonBlockingSocket(int type, int proto) {
    netsock_t s = socket(AF_INET, type, proto);
    if (s != NETSOCK_INVALID) netSetNonBlocking(s);
    return s;
}

struct sockaddr_in netMakeSockAddr(UINT32 ip, UINT16 port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ip);
    addr.sin_port = htons(port);
    return addr;
}

int g_netBackendReady = 0;

// The RTL8139<->backend seam: rtl8139TransmitFrame (device layer, near the
// bottom of this section) calls whatever's registered here rather than
// naming a backend directly. netBackendInit registers netSlirpTransmit
// (defined further below, forward-declared here since it's registered
// from up here). A future bridged/host-only backend would register its
// own transmit function the same way -- e.g. gated on a config option --
// without any change to rtl8139TransmitFrame or the RTL8139 device code
// above it. (rtl8139ReceiveFrame, the backend -> device direction, is
// already backend-agnostic: any backend just calls it directly.)
typedef void (*NetTransmitFn)(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len);
void netSlirpTransmit(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len);
NetTransmitFn g_netTransmit = NULL;

void netBackendInit(void) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("[net] WSAStartup failed -- networking backend disabled\n");
        fflush(stdout);
        return;
    }
    g_netBackendReady = 1;
    g_netTransmit = netSlirpTransmit;
    printf("[net] backend initialized\n");
    fflush(stdout);
}

// --- Virtual network addressing (same well-known private range QEMU's own
// slirp backend uses, chosen for the same reason: a safe, conventional
// choice unlikely to collide with anything the guest expects). ---
#define NET_GUEST_IP     0x0A00020FU // 10.0.2.15
#define NET_GATEWAY_IP   0x0A000202U // 10.0.2.2  -- also the NAT router
#define NET_DNS_IP       0x0A000203U // 10.0.2.3  -- DNS relay
#define NET_NETMASK      0xFFFFFF00U // 255.255.255.0
#define NET_BROADCAST_IP 0x0A0002FFU // 10.0.2.255

unsigned char netGatewayMac[6] = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x02 }; // encodes 10.0.2.2
unsigned char netDnsMac[6]     = { 0x52, 0x55, 0x0A, 0x00, 0x02, 0x03 }; // encodes 10.0.2.3
unsigned char netGuestMac[6];   // learned from the guest's first transmitted frame
int netGuestMacKnown = 0;

// --- Wire-format header layouts (packed manually via byte offsets rather
// than struct pragmas, matching this file's existing style for ACPI
// tables/PE headers elsewhere). All multi-byte network fields are
// big-endian ("network byte order"); helpers below make that explicit at
// each read/write site rather than relying on casts.
UINT16 netRd16(const unsigned char *p) { return (UINT16)((p[0] << 8) | p[1]); }
UINT32 netRd32(const unsigned char *p) { return ((UINT32)p[0] << 24) | ((UINT32)p[1] << 16) | ((UINT32)p[2] << 8) | p[3]; }
void netWr16(unsigned char *p, UINT16 v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
void netWr32(unsigned char *p, UINT32 v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }

// Standard 16-bit one's-complement checksum (RFC 1071), used for the IP
// header and, with a pseudo-header prepended, UDP/TCP.
UINT16 netChecksum(const unsigned char *data, UINT32 len, UINT32 seed) {
    UINT32 sum = seed;
    UINT32 i;
    for (i = 0; i + 1 < len; i += 2) sum += netRd16(data + i);
    if (len & 1) sum += (UINT32)data[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (UINT16)~sum;
}

// Pseudo-header checksum seed for UDP/TCP: src IP + dst IP + zero byte +
// protocol + length, summed the same way as the real header/payload.
UINT32 netL4ChecksumSeed(UINT32 srcIp, UINT32 dstIp, unsigned char proto, UINT16 l4Len) {
    unsigned char pseudo[12];
    netWr32(pseudo + 0, srcIp);
    netWr32(pseudo + 4, dstIp);
    pseudo[8] = 0;
    pseudo[9] = proto;
    netWr16(pseudo + 10, l4Len);
    UINT32 sum = 0;
    UINT32 i;
    for (i = 0; i < 12; i += 2) sum += netRd16(pseudo + i);
    return sum;
}

// Builds a complete Ethernet+IPv4 frame around an already-built L4 payload
// (ICMP, or a UDP/TCP header+payload the caller assembled) and hands it to
// the device via netReceiveFrame. Fills in and checksums the IP header;
// the caller is responsible for its own L4 checksum (UDP/TCP need the
// pseudo-header, ICMP doesn't) before calling this.
long g_netIpFrameLogged = 0;

void netSendIpFrame(WHV_PARTITION_HANDLE partition, const unsigned char *srcMac, UINT32 srcIp,
                     UINT32 dstIp, unsigned char proto, const unsigned char *l4, UINT32 l4Len) {
    if (!netGuestMacKnown) return;
    unsigned char frame[1600];
    UINT32 ipLen = 20 + l4Len;
    if (14 + ipLen > sizeof(frame)) return;

    memcpy(frame + 0, netGuestMac, 6);   // dst: guest
    memcpy(frame + 6, srcMac, 6);        // src: gateway/DNS
    netWr16(frame + 12, 0x0800);         // ethertype: IPv4

    unsigned char *ip = frame + 14;
    ip[0] = 0x45; // version 4, IHL 5 (20 bytes, no options)
    ip[1] = 0x00; // DSCP/ECN
    netWr16(ip + 2, (UINT16)ipLen);
    netWr16(ip + 4, 0); // identification
    netWr16(ip + 6, 0x4000); // flags: don't fragment
    ip[8] = 64;   // TTL
    ip[9] = proto;
    netWr16(ip + 10, 0); // checksum, filled below
    netWr32(ip + 12, srcIp);
    netWr32(ip + 16, dstIp);
    netWr16(ip + 10, netChecksum(ip, 20, 0));

    memcpy(frame + 34, l4, l4Len);

    // The UDP checksum was self-checked and proved correct, but the IP HEADER
    // checksum never was -- and a bad one is discarded at the IP layer before
    // anything looks at UDP, which would look identical from outside: relayed,
    // delivered, undropped, and ignored. Verify over the completed header (must
    // be zero) and dump the first few frames so the wire format can be read
    // rather than reasoned about.
    if (g_netIpFrameLogged < 3) {
        g_netIpFrameLogged++;
        UINT16 ipVerify = netChecksum(ip, 20, 0);
        printf("[net-ip] frame %ld: proto=%u len=%u ipcksum-verify=0x%04X %s\n",
               g_netIpFrameLogged, proto, ipLen, ipVerify,
               (ipVerify == 0) ? "OK" : "*** BAD IP CHECKSUM ***");
        printf("[net-ip]   eth+ip+l4[0:48]:");
        UINT32 di;
        for (di = 0; di < 48 && di < 14 + ipLen; di++) printf(" %02X", frame[di]);
        printf("\n");
        fflush(stdout);
    }

    netReceiveFrame(partition, frame, 14 + ipLen);
}

void netHandleArp(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len) {
    if (len < 14 + 28) return;
    const unsigned char *arp = frame + 14;
    UINT16 op = netRd16(arp + 6);
    UINT32 targetIp = netRd32(arp + 24);
    if (op != 1) return; // only handle requests (op=1); we never issue our own requests to need op=2 replies

    const unsigned char *replyMac = NULL;
    if (targetIp == NET_GATEWAY_IP) replyMac = netGatewayMac;
    else if (targetIp == NET_DNS_IP) replyMac = netDnsMac;
    else return; // not asking about a device we emulate

    unsigned char reply[42];
    memcpy(reply + 0, frame + 6, 6);   // dst: requester
    memcpy(reply + 6, replyMac, 6);    // src: us
    netWr16(reply + 12, 0x0806);       // ethertype: ARP

    unsigned char *rarp = reply + 14;
    netWr16(rarp + 0, 1);   // HTYPE: Ethernet
    netWr16(rarp + 2, 0x0800); // PTYPE: IPv4
    rarp[4] = 6;  // HLEN
    rarp[5] = 4;  // PLEN
    netWr16(rarp + 6, 2); // OPER: reply
    memcpy(rarp + 8, replyMac, 6);
    netWr32(rarp + 14, targetIp);
    memcpy(rarp + 18, frame + 6, 6); // target hw addr: requester
    netWr32(rarp + 24, netRd32(arp + 14)); // target IP: requester's own IP (sender IP from the request)

    g_netArpReplies++;
    if (g_netArpLogged < 20) {
        g_netArpLogged++;
        printf("[net-arp] reply  #%ld: %d.%d.%d.%d is-at %02X:%02X:%02X:%02X:%02X:%02X\n",
               g_netArpReplies,
               (targetIp >> 24) & 0xFF, (targetIp >> 16) & 0xFF,
               (targetIp >> 8) & 0xFF, targetIp & 0xFF,
               replyMac[0], replyMac[1], replyMac[2], replyMac[3], replyMac[4], replyMac[5]);
        fflush(stdout);
    }
    netReceiveFrame(partition, reply, sizeof(reply));
}

// ICMP echo (ping) only -- the minimum needed for a guest to consider the
// gateway/Internet reachable at all. Anything else (unreachable, TTL
// exceeded, etc.) is left unimplemented; the guest just won't see those
// diagnostics, which is a reasonable Phase 3 gap since correctness there
// doesn't block "obtain an IP and reach the Internet."
void netHandleIcmp(WHV_PARTITION_HANDLE partition, UINT32 srcIp, UINT32 dstIp,
                    const unsigned char *icmp, UINT32 icmpLen) {
    if (icmpLen < 8 || icmp[0] != 8) return; // not an echo request (type 8)

    unsigned char reply[1500];
    if (icmpLen > sizeof(reply)) return;
    memcpy(reply, icmp, icmpLen);
    reply[0] = 0; // type: echo reply
    reply[1] = 0; // code
    netWr16(reply + 2, 0); // checksum, filled below
    netWr16(reply + 2, netChecksum(reply, icmpLen, 0));

    const unsigned char *replyMac = (dstIp == NET_DNS_IP) ? netDnsMac : netGatewayMac;
    netSendIpFrame(partition, replyMac, dstIp, srcIp, 1 /* ICMP */, reply, icmpLen);
}

// Declared here rather than with the UDP NAT globals further down, because
// netSendUdp below is defined before them.
long g_netUdpCksumChecked = 0;

// Builds a UDP header around payload, computes the pseudo-header checksum,
// and hands off to netSendIpFrame. Shared by DHCP, the DNS relay, and
// general UDP NAT (all added below).
void netSendUdp(WHV_PARTITION_HANDLE partition, const unsigned char *srcMac, UINT32 srcIp, UINT16 srcPort,
                 UINT32 dstIp, UINT16 dstPort, const unsigned char *payload, UINT32 payloadLen) {
    unsigned char udp[1500];
    UINT32 udpLen = 8 + payloadLen;
    if (udpLen > sizeof(udp)) return;
    netWr16(udp + 0, srcPort);
    netWr16(udp + 2, dstPort);
    netWr16(udp + 4, (UINT16)udpLen);
    netWr16(udp + 6, 0); // checksum, filled below
    memcpy(udp + 8, payload, payloadLen);
    UINT32 seed = netL4ChecksumSeed(srcIp, dstIp, 17 /* UDP */, (UINT16)udpLen);
    UINT16 cksum = netChecksum(udp, udpLen, seed);
    if (cksum == 0) cksum = 0xFFFF; // UDP: a computed 0 means "no checksum"; avoid emitting a literal zero
    netWr16(udp + 6, cksum);

    // SELF-CHECK. The guest reports DNS timeouts while we measure a reply
    // delivered for every query in under 50ms with no drops -- so it is receiving
    // these and rejecting them, and a bad checksum is the usual reason a host
    // discards a UDP datagram in silence. Verifying over the completed header
    // must yield zero; if it does not, our arithmetic is wrong and every reply we
    // have ever "delivered" was garbage on arrival.
    if (g_netUdpCksumChecked < 4) {
        g_netUdpCksumChecked++;
        UINT16 verify = netChecksum(udp, udpLen, seed);
        printf("[net-udp] checksum self-check: wrote 0x%04X, verify-over-complete=0x%04X %s"
               " (udpLen=%u srcPort=%u dstPort=%u)\n",
               cksum, verify, (verify == 0) ? "OK" : "*** BAD ***",
               udpLen, srcPort, dstPort);
        fflush(stdout);
    }
    netSendIpFrame(partition, srcMac, srcIp, dstIp, 17, udp, udpLen);
}

// Minimal single-lease DHCP server: always offers/acks NET_GUEST_IP, since
// this backend only ever serves one guest. Real IP pool management isn't
// needed for that. DISCOVER -> OFFER, REQUEST -> ACK; anything else (or a
// malformed/non-BOOTREQUEST packet) is ignored.
void netHandleDhcp(WHV_PARTITION_HANDLE partition, const unsigned char *dhcp, UINT32 dhcpLen) {
    if (dhcpLen < 240) return; // fixed BOOTP header (236) + magic cookie (4)
    if (dhcp[0] != 1) return;  // BOOTREQUEST only
    if (netRd32(dhcp + 236) != 0x63825363) return; // DHCP magic cookie

    UINT32 xid = netRd32(dhcp + 4);
    unsigned char chaddr[6];
    memcpy(chaddr, dhcp + 28, 6);

    unsigned char msgType = 0;
    UINT32 i = 240;
    while (i < dhcpLen) {
        unsigned char opt = dhcp[i];
        if (opt == 255) break;
        if (opt == 0) { i++; continue; }
        if (i + 1 >= dhcpLen) break;
        unsigned char optLen = dhcp[i + 1];
        if (opt == 53 && optLen >= 1 && i + 2 < dhcpLen) msgType = dhcp[i + 2];
        i += 2 + optLen;
    }

    unsigned char replyType;
    if (msgType == 1) replyType = 2;      // DISCOVER -> OFFER
    else if (msgType == 3) replyType = 5; // REQUEST -> ACK
    else return;

    unsigned char reply[300] = { 0 };
    reply[0] = 2; // BOOTREPLY
    reply[1] = 1; // htype: Ethernet
    reply[2] = 6; // hlen
    netWr32(reply + 4, xid);
    netWr32(reply + 16, NET_GUEST_IP);   // yiaddr
    netWr32(reply + 20, NET_GATEWAY_IP); // siaddr (informational)
    memcpy(reply + 28, chaddr, 6);
    netWr32(reply + 236, 0x63825363);

    UINT32 o = 240;
    reply[o++] = 53; reply[o++] = 1; reply[o++] = replyType;
    reply[o++] = 1; reply[o++] = 4; netWr32(reply + o, NET_NETMASK); o += 4;
    reply[o++] = 3; reply[o++] = 4; netWr32(reply + o, NET_GATEWAY_IP); o += 4;
    reply[o++] = 6; reply[o++] = 4; netWr32(reply + o, NET_DNS_IP); o += 4;
    reply[o++] = 51; reply[o++] = 4; netWr32(reply + o, 86400); o += 4; // lease: 1 day
    reply[o++] = 54; reply[o++] = 4; netWr32(reply + o, NET_GATEWAY_IP); o += 4; // server ID
    reply[o++] = 255;

    netSendUdp(partition, netGatewayMac, NET_GATEWAY_IP, 67, NET_BROADCAST_IP, 68, reply, o);
}

// --- General UDP NAT (also carries the DNS relay, as a special case where
// the destination gets translated on the way out and back). One real host
// UDP socket per active guest source port; unconnected (send/recvfrom), so
// a single socket can talk to multiple real destinations if the guest
// reuses one source port for several flows -- recvfrom's fromaddr tells us
// which real peer a given reply came from. ---
// 32, and nothing ever freed a slot -- the same leak just fixed for TCP, but far
// more damaging here. Windows picks a FRESH ephemeral source port for every DNS
// query, so each lookup consumed a slot permanently. After 32 lookups the table
// was full and every subsequent query was dropped with no session at all, which
// is why Edge reports DNS_PROBE_FINISHED_NO_INTERNET while our relay looks
// healthy: the queries it does log are the lucky early ones. (An earlier run
// recorded noSession=8, already climbing.)
#define NET_MAX_UDP_SESSIONS 128
#define NET_UPSTREAM_DNS_IP 0x08080808U // 8.8.8.8 -- fixed, since there's no portable way to
                                         // discover "the host's configured DNS server" without
                                         // platform-specific APIs (see the portability note above)
typedef struct {
    int inUse;
    UINT16 guestPort;
    netsock_t sock;
    // When the guest's query went out. The guest reports DNS timeouts while our
    // relay shows a reply for nearly every query, so the question is whether the
    // answer is LOST or merely LATE -- and those need opposite fixes. We only
    // poll these sockets once per vCPU exit, so a slow loop delays every reply.
    LARGE_INTEGER sentAt;
} NetUdpSession;
NetUdpSession g_udpSessions[NET_MAX_UDP_SESSIONS];
long g_netUdpOut = 0, g_netUdpOutFail = 0, g_netUdpIn = 0, g_netUdpNoSession = 0;
long g_netUdpReclaimed = 0;
long g_netUdpLogged = 0, g_netUdpOver2s = 0;
double g_netUdpWorstMs = 0.0;

// Looks up the session for guestPort, opening a new real UDP socket for it
// if none exists yet. Returns NULL if the table is full or socket()
// itself fails.
NetUdpSession *netFindOrCreateUdpSession(UINT16 guestPort) {
    int i;
    for (i = 0; i < NET_MAX_UDP_SESSIONS; i++) {
        if (g_udpSessions[i].inUse && g_udpSessions[i].guestPort == guestPort) return &g_udpSessions[i];
    }
    for (i = 0; i < NET_MAX_UDP_SESSIONS; i++) {
        if (!g_udpSessions[i].inUse) {
            netsock_t s = netCreateNonBlockingSocket(SOCK_DGRAM, IPPROTO_UDP);
            if (s == NETSOCK_INVALID) return NULL;
            g_udpSessions[i].inUse = 1;
            g_udpSessions[i].guestPort = guestPort;
            g_udpSessions[i].sock = s;
            g_udpSessions[i].sentAt.QuadPart = 0;
            return &g_udpSessions[i];
        }
    }
    // RECLAIM the least recently used slot rather than refusing. A DNS exchange
    // is over in milliseconds, so the oldest entry is certainly finished with --
    // whereas refusing means the guest's lookup simply vanishes.
    {
        NetUdpSession *victim = NULL;
        for (i = 0; i < NET_MAX_UDP_SESSIONS; i++) {
            if (!victim || g_udpSessions[i].sentAt.QuadPart < victim->sentAt.QuadPart)
                victim = &g_udpSessions[i];
        }
        if (victim) {
            g_netUdpReclaimed++;
            if (victim->sock != NETSOCK_INVALID) netCloseSocket(victim->sock);
            netsock_t s = netCreateNonBlockingSocket(SOCK_DGRAM, IPPROTO_UDP);
            if (s == NETSOCK_INVALID) { victim->inUse = 0; return NULL; }
            victim->inUse = 1;
            victim->guestPort = guestPort;
            victim->sock = s;
            victim->sentAt.QuadPart = 0;
            return victim;
        }
    }
    return NULL; // session table full
}

void netHandleUdpGuestPacket(UINT32 dstIp, const unsigned char *udp, UINT32 udpLen) {
    if (udpLen < 8 || !g_netBackendReady) return;
    UINT16 srcPort = netRd16(udp + 0);
    UINT16 dstPort = netRd16(udp + 2);
    const unsigned char *payload = udp + 8;
    UINT32 payloadLen = udpLen - 8;

    // DNS relay: redirect the virtual DNS proxy to a real upstream
    // resolver. Everything else is plain outbound NAT -- the guest's
    // stated destination is used as-is.
    UINT32 realDstIp = (dstIp == NET_DNS_IP && dstPort == 53) ? NET_UPSTREAM_DNS_IP : dstIp;
    UINT16 realDstPort = dstPort;

    NetUdpSession *sess = netFindOrCreateUdpSession(srcPort);
    if (!sess) {
        g_netUdpNoSession++;
        return;
    }

    struct sockaddr_in dst = netMakeSockAddr(realDstIp, realDstPort);
    int sent = sendto(sess->sock, (const char *)payload, (int)payloadLen, 0,
                      (struct sockaddr *)&dst, sizeof(dst));
    if (sent < 0) g_netUdpOutFail++; else g_netUdpOut++;
    QueryPerformanceCounter(&sess->sentAt);

    // DNS is the open failure, and the four ways it can break are
    // indistinguishable from outside: the guest never asks; we fail to send; the
    // upstream never answers; or it answers and the guest rejects our framing.
    // Log the send side with its error code so the first three are separable --
    // a blocked outbound socket (AV/firewall) shows up here as a WSA error, not
    // as silence.
    if (g_netUdpLogged < 40) {
        g_netUdpLogged++;
        printf("[net-udp] OUT guest:%u -> %d.%d.%d.%d:%u (%u bytes)%s sent=%d%s\n",
               srcPort,
               (realDstIp >> 24) & 0xFF, (realDstIp >> 16) & 0xFF,
               (realDstIp >> 8) & 0xFF, realDstIp & 0xFF,
               realDstPort, payloadLen,
               (dstIp == NET_DNS_IP && dstPort == 53) ? " [DNS relay]" : "",
               sent, sent < 0 ? "" : "");
        if (sent < 0) printf("[net-udp] OUT FAILED, WSAGetLastError=%d\n", WSAGetLastError());
        fflush(stdout);
    }
}

// Polls every active UDP session for a reply, non-blocking, and relays
// anything received back to the guest. Called once per main-loop
// iteration, same pattern as ahciProcessPendingCommands/deliverPendingAtaIrq.
void netPollUdpSessions(WHV_PARTITION_HANDLE partition) {
    if (!g_netBackendReady) return;
    int i;
    for (i = 0; i < NET_MAX_UDP_SESSIONS; i++) {
        if (!g_udpSessions[i].inUse) continue;
        unsigned char buf[1500];
        struct sockaddr_in from;
        int fromLen = sizeof(from);
        int n = recvfrom(g_udpSessions[i].sock, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromLen);
        if (n > 0) {
            UINT32 fromIp = ntohl(from.sin_addr.s_addr);
            UINT16 fromPort = ntohs(from.sin_port);
            // Mirror the DNS translation on the way back: a reply from the
            // real upstream resolver should appear to come from the
            // virtual DNS proxy the guest actually queried.
            UINT32 replySrcIp = (fromIp == NET_UPSTREAM_DNS_IP && fromPort == 53) ? NET_DNS_IP : fromIp;
            g_netUdpIn++;
            // How long the guest waited. nslookup gives up at 2000ms, so anything
            // near that is a latency problem in our polling, not packet loss.
            double waitedMs = 0.0;
            if (perfFrequency.QuadPart && g_udpSessions[i].sentAt.QuadPart) {
                LARGE_INTEGER nowQpc; QueryPerformanceCounter(&nowQpc);
                waitedMs = (double)(nowQpc.QuadPart - g_udpSessions[i].sentAt.QuadPart) * 1000.0
                           / (double)perfFrequency.QuadPart;
                if (waitedMs > g_netUdpWorstMs) g_netUdpWorstMs = waitedMs;
                if (waitedMs > 2000.0) g_netUdpOver2s++;
            }
            if (g_netUdpLogged < 40) {
                g_netUdpLogged++;
                printf("[net-udp] IN  %d.%d.%d.%d:%u -> guest:%u (%d bytes) after %.0f ms%s\n",
                       (fromIp >> 24) & 0xFF, (fromIp >> 16) & 0xFF, (fromIp >> 8) & 0xFF, fromIp & 0xFF,
                       fromPort, g_udpSessions[i].guestPort, n, waitedMs,
                       waitedMs > 2000.0 ? "   *** PAST THE GUEST'S 2s TIMEOUT ***" : "");
                fflush(stdout);
            }
            // Answer from the MAC the guest ARPed for. It asked for 10.0.2.3 and
            // was told netDnsMac, so a reply claiming to come from 10.0.2.3 while
            // bearing the gateway's MAC is inconsistent with what the guest was
            // told -- the ICMP path already picks per-address, this one did not.
            const unsigned char *replyMac = (replySrcIp == NET_DNS_IP) ? netDnsMac : netGatewayMac;
            netSendUdp(partition, replyMac, replySrcIp, fromPort, NET_GUEST_IP, g_udpSessions[i].guestPort,
                       buf, (UINT32)n);
        }
    }
}

// --- TCP NAT. One real host TCP socket per active guest connection
// (keyed by guest source port, same simplification as the UDP table --
// reasonable for a single guest whose OS picks distinct ephemeral ports
// per connection). We run a minimal, deliberately non-RFC793-complete TCP
// state machine on the guest-facing side: no retransmission timers, no
// out-of-order/reassembly handling, no real window-size-based flow
// control (we advertise a large fixed window and trust the guest's own
// stack + the real socket's own TCP flow control on the other side to
// keep things reasonable), and teardown just sends our FIN and frees the
// session rather than tracking the full close handshake. All acceptable
// simplifications for "reach the Internet," not full protocol compliance.
// 16 was far too few and nothing ever reclaimed a slot: measured syn=91 with
// tableFull=72, i.e. four out of five connection attempts refused outright. A
// browser opens dozens of connections for a single page, so this alone would
// stall any real browsing regardless of how well the transport works.
#define NET_MAX_TCP_SESSIONS 64
#define NET_TCP_FIN  0x01
#define NET_TCP_SYN  0x02
#define NET_TCP_RST  0x04
#define NET_TCP_PSH  0x08
#define NET_TCP_ACK  0x10

// NET_TCP_PEER_CLOSED: the real peer has closed its side, we have told the guest
// with a FIN, but the SESSION IS KEPT. Destroying it at that moment (as this
// first did) means the guest's in-flight segments arrive for a connection we no
// longer know about and get dropped as "unknown" -- the guest then gets no ack
// for data it believes is outstanding and resorts to RST. Keeping the session
// lets us keep acking until the guest closes properly.
typedef enum { NET_TCP_CONNECTING, NET_TCP_ESTABLISHED, NET_TCP_PEER_CLOSED } NetTcpState;
typedef struct {
    int inUse;
    NetTcpState state;
    UINT16 guestPort;
    UINT32 realDstIp;
    UINT16 realDstPort;
    netsock_t sock;
    UINT32 guestSeq; // next sequence number we expect from the guest (our ack value)
    UINT32 hostSeq;  // next sequence number we send
    // The guest's advertised MSS, taken from its SYN options. Segments larger
    // than this may be dropped by the guest rather than reassembled, so we cap
    // what we hand it instead of assuming our 1400-byte buffer is acceptable.
    UINT32 guestMss;
    // When this session last carried traffic. Slots were never reclaimed, so a
    // half-closed or abandoned connection held its slot forever.
    LARGE_INTEGER lastActive;
    // The highest sequence number the guest has ACKNOWLEDGED receiving from us.
    // Everything measured so far covers guest->us (dup, ooo) or our own delivery
    // into the ring; nothing has ever checked whether the guest actually RECEIVED
    // what we sent. If this trails hostSeq when a connection ends, our host->guest
    // stream had a hole -- and with no retransmission that stalls TLS exactly as
    // observed: the guest takes in a whole server flight and then says nothing.
    UINT32 guestAcked;
    // The guest's advertised receive window, taken from the window field of its
    // most recent segment. NOTHING has ever read that field: the drain loop in
    // netPollTcpSessions paced itself purely by RX ring space, which is not flow
    // control at all. Ring space says what the DRIVER can take, and the driver
    // empties the ring into its own buffers at its own rate, entirely
    // independently of what the guest's TCP STACK is willing to accept. Only the
    // second one governs whether the bytes survive, so on any sustained transfer
    // we ran past the window, Windows silently discarded everything outside it,
    // and -- with no retransmission anywhere in this model -- the stream was dead
    // from that point on. That is exactly the long-running symptom: the guest
    // takes in a whole server flight and then says nothing.
    //
    // Window scaling needs no handling here. Our SYN-ACK goes out with data
    // offset 5 and no options at all, so per RFC 7323 scaling is disabled in BOTH
    // directions and this raw 16-bit value is the true window.
    UINT32 guestWindow;
} NetTcpSession;
NetTcpSession g_tcpSessions[NET_MAX_TCP_SESSIONS];
// TCP NAT was completely uninstrumented, so "no TCP in the log" meant nothing at
// all -- absence of logging, not absence of traffic. These separate the failures
// that look identical from the guest: never attempted / table full / connect
// refused / connected but no data / data flowing.
long g_tcpSyn = 0, g_tcpTableFull = 0, g_tcpSockFail = 0, g_tcpEstablished = 0;
long g_tcpRefused = 0, g_tcpClosed = 0, g_tcpUnknown = 0;
long g_tcpBytesToHost = 0, g_tcpBytesToGuest = 0;
long g_tcpLogged = 0, g_tcpSegLogged = 0;
long g_tcpDupSegments = 0, g_tcpOutOfOrder = 0, g_tcpReclaimed = 0, g_tcpAged = 0;
long g_tcpClosedUnacked = 0; INT32 g_tcpWorstUnacked = 0;
// Flow control, host->guest. windowStalled = polls where the guest's window was
// entirely full so we sent nothing; windowClamped = sends cut short to fit it.
// Both were previously invisible because both were previously not happening --
// we just overran the window and lost the excess.
long g_tcpWindowStalled = 0, g_tcpWindowClamped = 0;
// Flow control, guest->host. shortSend = send() took only part of the buffer;
// sendBlocked = it took none (WSAEWOULDBLOCK). Both used to be counted as fully
// delivered and acknowledged to the guest.
long g_tcpShortSend = 0, g_tcpSendBlocked = 0;

// Closes the real socket and frees the session slot. Does not notify the
// guest -- callers that need the guest informed (RST/FIN) send that
// segment themselves first, since the right flags/seq numbers depend on
// why the session is closing.
void netCloseTcpSession(NetTcpSession *s) {
    if (s->sock != NETSOCK_INVALID) netCloseSocket(s->sock);
    s->inUse = 0;
}

// Looks up an existing session by guest source port. Unlike the UDP
// equivalent, this never creates one -- new TCP sessions are only ever
// created by an incoming SYN, handled explicitly in netHandleTcpGuestPacket.
NetTcpSession *netFindTcpSession(UINT16 guestPort) {
    int i;
    for (i = 0; i < NET_MAX_TCP_SESSIONS; i++)
        if (g_tcpSessions[i].inUse && g_tcpSessions[i].guestPort == guestPort) return &g_tcpSessions[i];
    return NULL;
}

// Builds one TCP segment (20-byte header, no options) and sends it via
// netSendIpFrame. srcIp/srcPort here is what the GUEST sees as the remote
// end -- for a NAT'd connection that's the real destination's own address,
// unchanged, so the guest's TCP stack associates replies with the
// connection it opened (same principle as general UDP's pass-through
// case) -- only DHCP/DNS use a fixed virtual address.
void netSendTcpSegment(WHV_PARTITION_HANDLE partition, UINT32 srcIp, UINT16 srcPort, UINT32 dstIp, UINT16 dstPort,
                        UINT32 seq, UINT32 ack, unsigned char flags, const unsigned char *payload, UINT32 payloadLen) {
    unsigned char seg[1500];
    UINT32 tcpLen = 20 + payloadLen;
    if (tcpLen > sizeof(seg)) return;
    netWr16(seg + 0, srcPort);
    netWr16(seg + 2, dstPort);
    netWr32(seg + 4, seq);
    netWr32(seg + 8, ack);
    seg[12] = (5 << 4); // data offset: 5 words (20 bytes), no options
    seg[13] = flags;
    netWr16(seg + 14, 65535); // window -- fixed/large, see the simplifications note above
    netWr16(seg + 16, 0); // checksum, filled below
    netWr16(seg + 18, 0); // urgent pointer
    if (payloadLen) memcpy(seg + 20, payload, payloadLen);
    UINT32 seed = netL4ChecksumSeed(srcIp, dstIp, 6 /* TCP */, (UINT16)tcpLen);
    netWr16(seg + 16, netChecksum(seg, tcpLen, seed));
    if (g_tcpSegLogged < 40) {
        g_tcpSegLogged++;
        printf("[net-tcp] -> guest:%u  %s%s%s%s%s seq=%u ack=%u len=%u\n", dstPort,
               (flags & NET_TCP_SYN) ? "SYN " : "", (flags & NET_TCP_ACK) ? "ACK " : "",
               (flags & NET_TCP_PSH) ? "PSH " : "", (flags & NET_TCP_FIN) ? "FIN " : "",
               (flags & NET_TCP_RST) ? "RST " : "", seq, ack, payloadLen);
        fflush(stdout);
    }
    netSendIpFrame(partition, netGatewayMac, srcIp, dstIp, 6, seg, tcpLen);
}

void netHandleTcpGuestPacket(WHV_PARTITION_HANDLE partition, UINT32 dstIp, const unsigned char *tcp, UINT32 tcpLen) {
    if (tcpLen < 20 || !g_netBackendReady) return;
    UINT16 srcPort = netRd16(tcp + 0);
    UINT16 dstPort = netRd16(tcp + 2);
    UINT32 seq = netRd32(tcp + 4);
    UINT32 hdrLen = ((tcp[12] >> 4) & 0xF) * 4;
    unsigned char flags = tcp[13];
    if (hdrLen < 20 || hdrLen > tcpLen) return;
    const unsigned char *payload = tcp + hdrLen;
    UINT32 payloadLen = tcpLen - hdrLen;

    if (g_tcpSegLogged < 40) {
        g_tcpSegLogged++;
        printf("[net-tcp] <- guest:%u  %s%s%s%s%s seq=%u ack=%u len=%u\n", srcPort,
               (flags & NET_TCP_SYN) ? "SYN " : "", (flags & NET_TCP_ACK) ? "ACK " : "",
               (flags & NET_TCP_PSH) ? "PSH " : "", (flags & NET_TCP_FIN) ? "FIN " : "",
               (flags & NET_TCP_RST) ? "RST " : "", seq, netRd32(tcp + 8), payloadLen);
        fflush(stdout);
    }

    NetTcpSession *s = netFindTcpSession(srcPort);

    if (flags & NET_TCP_RST) {
        if (s) netCloseTcpSession(s);
        return;
    }

    if ((flags & NET_TCP_SYN) && !s) {
        g_tcpSyn++;
        int i;
        for (i = 0; i < NET_MAX_TCP_SESSIONS; i++) {
            if (!g_tcpSessions[i].inUse) { s = &g_tcpSessions[i]; break; }
        }
        if (!s) {
            // RECLAIM before refusing. Prefer a half-closed session (the peer is
            // already gone), otherwise the least recently active one. Refusing a
            // guest's SYN because we are hoarding dead slots is far worse than
            // dropping the stalest connection.
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            NetTcpSession *victim = NULL;
            int j;
            for (j = 0; j < NET_MAX_TCP_SESSIONS; j++) {
                NetTcpSession *c = &g_tcpSessions[j];
                if (c->state == NET_TCP_PEER_CLOSED) {
                    if (!victim || victim->state != NET_TCP_PEER_CLOSED ||
                        c->lastActive.QuadPart < victim->lastActive.QuadPart) victim = c;
                } else if (!victim && c->state != NET_TCP_PEER_CLOSED) {
                    victim = c;
                } else if (victim && victim->state != NET_TCP_PEER_CLOSED &&
                           c->lastActive.QuadPart < victim->lastActive.QuadPart) {
                    victim = c;
                }
            }
            if (victim) {
                g_tcpReclaimed++;
                netCloseTcpSession(victim);
                s = victim;
            } else {
                g_tcpTableFull++;
                return;
            }
        }

        netsock_t sock = netCreateNonBlockingSocket(SOCK_STREAM, IPPROTO_TCP);
        if (sock == NETSOCK_INVALID) { g_tcpSockFail++; return; }

        // Same DNS relay translation the UDP path does. Without it a resolver
        // falling back to DNS-over-TCP (which happens for answers too large for
        // UDP) tries to open a connection to 10.0.2.3:53 -- a virtual address
        // nothing listens on -- and gets refused. Measured: SYN guest:49518 ->
        // 10.0.2.3:53 followed immediately by REFUSED.
        UINT32 realDstIp = (dstIp == NET_DNS_IP && dstPort == 53) ? NET_UPSTREAM_DNS_IP : dstIp;

        if (g_tcpLogged < 24) {
            g_tcpLogged++;
            printf("[net-tcp] SYN guest:%u -> %d.%d.%d.%d:%u%s\n", srcPort,
                   (dstIp >> 24) & 0xFF, (dstIp >> 16) & 0xFF, (dstIp >> 8) & 0xFF, dstIp & 0xFF,
                   dstPort, (realDstIp != dstIp) ? " [DNS relay]" : "");
            fflush(stdout);
        }
        struct sockaddr_in dst = netMakeSockAddr(realDstIp, dstPort);
        connect(sock, (struct sockaddr *)&dst, sizeof(dst)); // non-blocking: completes async, polled below

        memset(s, 0, sizeof(*s));
        s->inUse = 1;
        s->state = NET_TCP_CONNECTING;
        s->guestPort = srcPort;
        // The GUEST-facing address, deliberately not the one we connected to.
        // Reply segments are built with this as their source, so a relayed DNS
        // connection must still appear to come from 10.0.2.3 or the guest's TCP
        // stack will not match it to the connection it opened.
        s->realDstIp = dstIp;
        s->realDstPort = dstPort;
        s->sock = sock;
        s->guestSeq = seq + 1; // SYN consumes one sequence number
        s->hostSeq = 1000;     // arbitrary ISN
        // Acknowledgements from the guest are counted from OUR ISN, so seed
        // guestAcked with it rather than leaving it at the 0 the memset above
        // wrote. Otherwise inFlight (hostSeq - guestAcked) reads 1001 rather
        // than 1 for the whole window between our SYN-ACK and the guest's first
        // ACK -- a phantom kilobyte of in-flight data. It is normally just
        // conservative, but a guest opening with a window of <= 1001 has that
        // window computed as CLOSED and the drain loop stalls the connection
        // before one byte of payload has moved.
        s->guestAcked = s->hostSeq;
        // The window the guest opens with. Unscaled by definition in a SYN, and
        // unscaled thereafter too, since we never negotiate scaling back.
        s->guestWindow = netRd16(tcp + 14);

        // Read the guest's MSS out of the SYN options (kind 2, length 4). Options
        // live between the fixed 20-byte header and hdrLen.
        s->guestMss = 0;
        {
            UINT32 o = 20;
            while (o + 1 < hdrLen) {
                unsigned char kind = tcp[o];
                if (kind == 0) break;          // end of option list
                if (kind == 1) { o++; continue; } // NOP
                if (o + 1 >= hdrLen) break;
                unsigned char optLen = tcp[o + 1];
                if (optLen < 2 || o + optLen > hdrLen) break;
                if (kind == 2 && optLen == 4) s->guestMss = netRd16(tcp + o + 2);
                o += optLen;
            }
        }
        return;
    }

    if (!s) { g_tcpUnknown++; return; } // packet for a connection we don't know about
    QueryPerformanceCounter(&s->lastActive);

    // Track how much of our stream the guest has acknowledged.
    if (flags & NET_TCP_ACK) {
        UINT32 gack = netRd32(tcp + 8);
        if ((INT32)(gack - s->guestAcked) > 0) s->guestAcked = gack;
    }
    // ...and how much more it is willing to take. Updated from EVERY segment the
    // guest sends, deliberately not just ACK-bearing ones: a bare window update is
    // how a guest reopens a window it had closed, and missing one would leave the
    // session stalled for good.
    s->guestWindow = netRd16(tcp + 14);

    // The peer is gone but the guest may still be talking. Acknowledge whatever
    // it sends so it closes cleanly rather than retransmitting into a void, and
    // release the slot once it finishes.
    if (s->state == NET_TCP_PEER_CLOSED) {
        if (payloadLen > 0) {
            INT32 d = (INT32)(seq - s->guestSeq);
            if (d <= 0 && (UINT32)(-d) < payloadLen) s->guestSeq += payloadLen - (UINT32)(-d);
            netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                               s->hostSeq, s->guestSeq, NET_TCP_ACK, NULL, 0);
        }
        if (flags & NET_TCP_FIN) {
            s->guestSeq += 1;
            netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                               s->hostSeq, s->guestSeq, NET_TCP_ACK, NULL, 0);
            netCloseTcpSession(s);
        }
        return;
    }

    if (payloadLen > 0) {
        // HONOUR THE SEQUENCE NUMBER. This used to append every payload to the
        // socket and add its length to guestSeq unconditionally, which is wrong
        // whenever the guest retransmits or sends an overlapping segment -- and
        // Windows does exactly that during the TLS handshake:
        //
        //   <- seq=658911454 len=6      (forwarded)
        //   <- seq=658911454 len=214    SAME seq, containing those 6 bytes again
        //
        // The real server then received 220 bytes where only 214 existed, with 6
        // duplicated at the front. That corrupts the ClientHello, so no reply
        // ever comes back -- measured as bytes out=657 in=0. We also acked
        // 658911674 when the guest had only sent up to 658911668, i.e. we
        // acknowledged data that was never transmitted.
        //
        // Forward only the genuinely new bytes, and never ack past what arrived.
        INT32 delta = (INT32)(seq - s->guestSeq);   // signed: wrap-safe comparison
        if (delta <= 0) {
            UINT32 alreadyHave = (UINT32)(-delta);
            if (alreadyHave < payloadLen) {
                UINT32 newLen = payloadLen - alreadyHave;
                // HONOUR WHAT send() ACTUALLY TOOK. This socket is non-blocking
                // (netCreateNonBlockingSocket), so send() may accept only part of
                // the buffer, or none of it at all with WSAEWOULDBLOCK once the
                // host send buffer fills. The return value was discarded and
                // guestSeq advanced by the whole newLen regardless -- and the ACK
                // built at the end of this function carries guestSeq, so we then
                // told the guest we had every byte. Those bytes were therefore
                // acknowledged AND thrown away, and an acknowledged byte is one
                // nothing will ever retransmit: unrecoverable, in the same way
                // and for the same reason as every other silent discard here.
                //
                // Advance only by what was accepted. The trailing ACK then carries
                // the shorter guestSeq and the guest retransmits the remainder on
                // its own, which is precisely what TCP is for.
                int sent = send(s->sock, (const char *)payload + alreadyHave, (int)newLen, 0);
                if (sent > 0) {
                    g_tcpBytesToHost += (long)sent;
                    s->guestSeq += (UINT32)sent;
                    if ((UINT32)sent < newLen) g_tcpShortSend++;
                } else {
                    // Nothing taken. Ack nothing new, and the guest resends.
                    g_tcpSendBlocked++;
                }
            } else {
                g_tcpDupSegments++;   // entirely a retransmission of what we have
            }
        } else {
            // Ahead of what we expect: a gap we cannot fill, because this model
            // has no reassembly queue. Drop it and re-ack guestSeq so the guest
            // retransmits from there rather than us stitching a hole together.
            g_tcpOutOfOrder++;
        }
        netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                           s->hostSeq, s->guestSeq, NET_TCP_ACK, NULL, 0);
    }

    if (flags & NET_TCP_FIN) {
        s->guestSeq += 1; // FIN consumes a sequence number
        shutdown(s->sock, SD_SEND); // half-close: no more data to the real peer, but keep reading any reply
        netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                           s->hostSeq, s->guestSeq, NET_TCP_ACK, NULL, 0);
    }
}

// Polls every connecting/established TCP session, non-blocking, same
// per-iteration pattern as netPollUdpSessions.
void netPollTcpSessions(WHV_PARTITION_HANDLE partition) {
    if (!g_netBackendReady) return;
    int i;
    for (i = 0; i < NET_MAX_TCP_SESSIONS; i++) {
        NetTcpSession *s = &g_tcpSessions[i];
        if (!s->inUse) continue;

        // Age out half-closed sessions the guest never finished closing. Without
        // this they hold their slot until something else evicts them.
        if (s->state == NET_TCP_PEER_CLOSED && perfFrequency.QuadPart && s->lastActive.QuadPart) {
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            double idleMs = (double)(now.QuadPart - s->lastActive.QuadPart) * 1000.0
                            / (double)perfFrequency.QuadPart;
            if (idleMs > 15000.0) { g_tcpAged++; netCloseTcpSession(s); continue; }
        }

        if (s->state == NET_TCP_CONNECTING) {
            fd_set writeSet, exceptSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&exceptSet);
            FD_SET(s->sock, &writeSet);
            FD_SET(s->sock, &exceptSet);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 0;
            if (select(0, NULL, &writeSet, &exceptSet, &tv) > 0) {
                if (FD_ISSET(s->sock, &exceptSet)) {
                    g_tcpRefused++;
                    if (g_tcpLogged < 24) {
                        g_tcpLogged++;
                        printf("[net-tcp] REFUSED guest:%u -> %d.%d.%d.%d:%u\n", s->guestPort,
                               (s->realDstIp >> 24) & 0xFF, (s->realDstIp >> 16) & 0xFF,
                               (s->realDstIp >> 8) & 0xFF, s->realDstIp & 0xFF, s->realDstPort);
                        fflush(stdout);
                    }
                    netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                                       s->hostSeq, s->guestSeq, NET_TCP_RST | NET_TCP_ACK, NULL, 0);
                    netCloseTcpSession(s);
                } else if (FD_ISSET(s->sock, &writeSet)) {
                    // A writable socket is NOT proof the connect succeeded --
                    // SO_ERROR is. Without this check a failed connect is taken
                    // for an established one, the first recv() then returns 0,
                    // and we send the guest a FIN before it has even sent its
                    // request (observed: SYN/SYN-ACK/ACK then an unprompted FIN,
                    // with the guest's ClientHello arriving afterwards).
                    int soErr = 0, soLen = (int)sizeof(soErr);
                    if (getsockopt(s->sock, SOL_SOCKET, SO_ERROR, (char *)&soErr, &soLen) == 0 && soErr != 0) {
                        g_tcpRefused++;
                        if (g_tcpLogged < 24) {
                            g_tcpLogged++;
                            printf("[net-tcp] CONNECT FAILED guest:%u -> %d.%d.%d.%d:%u (SO_ERROR=%d)\n",
                                   s->guestPort,
                                   (s->realDstIp >> 24) & 0xFF, (s->realDstIp >> 16) & 0xFF,
                                   (s->realDstIp >> 8) & 0xFF, s->realDstIp & 0xFF,
                                   s->realDstPort, soErr);
                            fflush(stdout);
                        }
                        netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                                           s->hostSeq, s->guestSeq, NET_TCP_RST | NET_TCP_ACK, NULL, 0);
                        netCloseTcpSession(s);
                        continue;
                    }
                    g_tcpEstablished++;
                    if (g_tcpLogged < 24) {
                        g_tcpLogged++;
                        printf("[net-tcp] CONNECTED guest:%u -> %d.%d.%d.%d:%u\n", s->guestPort,
                               (s->realDstIp >> 24) & 0xFF, (s->realDstIp >> 16) & 0xFF,
                               (s->realDstIp >> 8) & 0xFF, s->realDstIp & 0xFF, s->realDstPort);
                        fflush(stdout);
                    }
                    s->state = NET_TCP_ESTABLISHED;
                    netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                                       s->hostSeq, s->guestSeq, NET_TCP_SYN | NET_TCP_ACK, NULL, 0);
                    s->hostSeq += 1;
                }
            }
            continue;
        }

        if (s->state == NET_TCP_ESTABLISHED) {
            // DRAIN, don't take a single bite per poll. One recv per main-loop
            // iteration throttles a server's reply to however often the loop
            // happens to run, which for a multi-KB TLS flight means the peer can
            // finish and close before we have collected what it already sent.
            // Bounded so one busy connection cannot starve the others.
            int drained = 0;
            for (;;) {
                unsigned char buf[1400];
                UINT32 chunk = (s->guestMss && s->guestMss < sizeof(buf)) ? s->guestMss : (UINT32)sizeof(buf);
                // FLOW CONTROL -- the check that was missing entirely. Never put
                // more unacknowledged data in flight than the guest has said it can
                // hold. This is the counterpart to the ring backpressure just below
                // and the two are NOT interchangeable: ring space is what the
                // DRIVER can take right now, the window is what the guest's TCP
                // stack can take, and only the second decides whether the bytes
                // survive. The driver drains the ring into its own buffers at its
                // own rate, so a ring with room tells us nothing about a window
                // that has closed. Overrun it and Windows discards the excess
                // without a word -- and nothing here retransmits, so that stream is
                // finished. It fits the symptom the way nothing else has: it needs
                // VOLUME to trigger, which is why control frames and short replies
                // always looked fine while real page loads died partway in.
                {
                    UINT32 inFlight = s->hostSeq - s->guestAcked;
                    UINT32 windowLeft = (s->guestWindow > inFlight) ? s->guestWindow - inFlight : 0;
                    // Window full. Leave the bytes in the host socket, exactly as
                    // the backpressure path does, so the real peer's own TCP sees
                    // the backlog and slows down. The guest reopens the window with
                    // an ACK or a bare window update -- both of which now refresh
                    // guestWindow -- and the next poll carries on from there.
                    if (windowLeft == 0) { g_tcpWindowStalled++; break; }
                    if (chunk > windowLeft) { chunk = windowLeft; g_tcpWindowClamped++; }
                }
                // BACKPRESSURE. Do not pull data out of the host socket unless the
                // guest's ring can actually hold it. Draining regardless meant we
                // overwrote frames the driver had not read yet, and with no
                // retransmission a single clobbered segment breaks the stream for
                // good. Leaving the bytes in the host socket buffer instead lets
                // the real peer's own TCP flow control slow the sender down --
                // which is what a real NIC's finite FIFO does. The device-side
                // queue would absorb a burst, but absorbing it there only hides
                // the backlog from the sender's own flow control.
                if (!netRxCanAccept(chunk + 64)) { g_tcpRingBackpressure++; break; }
                int n = recv(s->sock, (char *)buf, (int)chunk, 0);
                if (n > 0) {
                    g_tcpBytesToGuest += n;
                    netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                                       s->hostSeq, s->guestSeq, NET_TCP_PSH | NET_TCP_ACK, buf, (UINT32)n);
                    s->hostSeq += (UINT32)n;
                    if (++drained >= 8) break;   // yield to the other sessions
                    continue;
                }
                if (n == 0) {
                    g_tcpClosed++;
                    // Did the guest actually RECEIVE everything we sent? An unacked
                    // tail is a hole in the host->guest stream, which this model
                    // cannot repair (no retransmission) and which stalls the guest
                    // permanently -- the shape we keep seeing, where it takes in a
                    // whole server flight and then says nothing.
                    {
                        UINT32 unacked = s->hostSeq - s->guestAcked;
                        if ((INT32)unacked > 0) {
                            g_tcpClosedUnacked++;
                            if ((INT32)unacked > g_tcpWorstUnacked) g_tcpWorstUnacked = (INT32)unacked;
                        }
                        if (g_tcpLogged < 24) {
                            printf("[net-tcp]   at close: hostSeq=%u guestAcked=%u -> %d UNACKED%s\n",
                                   s->hostSeq, s->guestAcked, (int)(INT32)unacked,
                                   ((INT32)unacked > 0) ? "  *** guest never got the tail ***" : "");
                        }
                    }
                if (g_tcpLogged < 24) {
                    g_tcpLogged++;
                    printf("[net-tcp] PEER CLOSED guest:%u -> %d.%d.%d.%d:%u after %ld bytes in\n",
                           s->guestPort,
                           (s->realDstIp >> 24) & 0xFF, (s->realDstIp >> 16) & 0xFF,
                           (s->realDstIp >> 8) & 0xFF, s->realDstIp & 0xFF, s->realDstPort,
                           g_tcpBytesToGuest);
                    fflush(stdout);
                }
                    // HALF-CLOSE. Tell the guest the peer is done, but keep the
                    // session: its in-flight segments still need acking, and a
                    // connection we have forgotten drops them as "unknown",
                    // which is what drove the guest to RST.
                    netSendTcpSegment(partition, s->realDstIp, s->realDstPort, NET_GUEST_IP, s->guestPort,
                                       s->hostSeq, s->guestSeq, NET_TCP_FIN | NET_TCP_ACK, NULL, 0);
                    s->hostSeq += 1;              // our FIN consumes a sequence number
                    s->state = NET_TCP_PEER_CLOSED;
                    if (s->sock != NETSOCK_INVALID) { netCloseSocket(s->sock); s->sock = NETSOCK_INVALID; }
                }
                break;   // n < 0: nothing more available right now
            }
        }
    }
}

// IPv4 dispatcher. This recognizes ICMP echo (addressed to the gateway/
// DNS), DHCP (addressed to the broadcast, since the client doesn't know
// the server's IP yet during DISCOVER), and routes UDP/TCP through the
// general NAT paths above.
void netHandleIpv4(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len) {
    const unsigned char *ip = frame + 14;
    UINT32 ipLen = len - 14;
    if (ipLen < 20) return;

    // TRUST THE IP HEADER'S TOTAL LENGTH, not the frame length. An Ethernet frame
    // is padded up to 60 bytes, and a bare TCP ACK is only 54 (14 + 20 + 20), so
    // the frame carries 6 bytes of padding after the real packet. Deriving the L4
    // length by subtracting from the FRAME length handed those 6 padding bytes to
    // TCP as though they were payload -- we forwarded them to the real server
    // ahead of the TLS ClientHello, the server saw a corrupt stream and closed
    // the connection, and browsing could never work. Measured as repeated
    // "len=6" segments followed immediately by the server's FIN.
    //
    // DNS never hit this: its payloads are large enough that the frames were
    // never padded, which is exactly why UDP looked healthy while TCP did not.
    UINT32 ipTotalLen = netRd16(ip + 2);
    if (ipTotalLen >= 20 && ipTotalLen < ipLen) ipLen = ipTotalLen;

    UINT32 ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || ipLen < ihl) return;

    unsigned char proto = ip[9];
    UINT32 srcIp = netRd32(ip + 12);
    UINT32 dstIp = netRd32(ip + 16);
    const unsigned char *l4 = ip + ihl;
    UINT32 l4Len = ipLen - ihl;

    if (proto == 1 && (dstIp == NET_GATEWAY_IP || dstIp == NET_DNS_IP)) {
        netHandleIcmp(partition, srcIp, dstIp, l4, l4Len);
        return;
    }
    if (proto == 17 && l4Len >= 8) {
        UINT16 dstPort = netRd16(l4 + 2);
        if (dstPort == 67) {
            netHandleDhcp(partition, l4 + 8, l4Len - 8);
        } else {
            netHandleUdpGuestPacket(dstIp, l4, l4Len);
        }
        return;
    }
    if (proto == 6) {
        netHandleTcpGuestPacket(partition, dstIp, l4, l4Len);
        return;
    }

    printf("[net] IPv4 proto=%u src=%d.%d.%d.%d dst=%d.%d.%d.%d len=%u (unhandled)\n",
           proto, (srcIp >> 24) & 0xFF, (srcIp >> 16) & 0xFF, (srcIp >> 8) & 0xFF, srcIp & 0xFF,
           (dstIp >> 24) & 0xFF, (dstIp >> 16) & 0xFF, (dstIp >> 8) & 0xFF, dstIp & 0xFF, l4Len);
    fflush(stdout);
}

// This backend's implementation of "transmit one guest frame": learns the
// guest's MAC (needed so synthesized ARP/DHCP/etc. replies can address it
// directly -- a bridged backend wouldn't need this, since a real switch
// handles addressing), then dispatches by ethertype. Registered as
// g_netTransmit below; rtl8139TransmitFrame itself never calls this by
// name, only through that pointer.
void netSlirpTransmit(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len) {
    if (!netGuestMacKnown) {
        memcpy(netGuestMac, frame + 6, 6);
        netGuestMacKnown = 1;
        // THE MAC THE GUEST ACTUALLY USES, against the one we put in the EEPROM.
        // The guest reads its address by bit-banging our emulated 93C46, so if
        // that read yields anything else, every unicast reply we send is
        // addressed to a station that does not exist -- while BROADCAST traffic
        // still works. That is exactly the observed split: DHCP (broadcast)
        // succeeds, DNS (unicast) times out despite being relayed, delivered,
        // checksummed correctly and interrupt-signalled.
        int macMatches = (memcmp(netGuestMac, e1000Mac, 6) == 0)
                      || (memcmp(netGuestMac, rtl8139Mac, 6) == 0);
        printf("[net] guest MAC learned: %02X:%02X:%02X:%02X:%02X:%02X | "
               "e1000 %02X:%02X:%02X:%02X:%02X:%02X rtl %02X:%02X:%02X:%02X:%02X:%02X -> %s\n",
               netGuestMac[0], netGuestMac[1], netGuestMac[2],
               netGuestMac[3], netGuestMac[4], netGuestMac[5],
               e1000Mac[0], e1000Mac[1], e1000Mac[2],
               e1000Mac[3], e1000Mac[4], e1000Mac[5],
               rtl8139Mac[0], rtl8139Mac[1], rtl8139Mac[2],
               rtl8139Mac[3], rtl8139Mac[4], rtl8139Mac[5],
               macMatches ? "MATCH" : "*** MISMATCH -- unicast cannot reach the guest ***");
        fflush(stdout);
    }

    UINT16 ethertype = netRd16(frame + 12);
    if (ethertype == 0x0806) {
        // Log which address the guest is asking about. A guest that keeps
        // re-ARPing for the same IP is a guest that is not accepting our reply,
        // and that is indistinguishable from "we never answered" unless the
        // request target is visible.
        g_netTxArp++;
        if (g_netArpLogged < 20) {
            g_netArpLogged++;
            const unsigned char *arp = frame + 14;
            UINT32 tgt = (len >= 14 + 28) ? netRd32(arp + 24) : 0;
            UINT32 spa = (len >= 14 + 28) ? netRd32(arp + 14) : 0;
            printf("[net-arp] request #%ld: who-has %d.%d.%d.%d  tell %d.%d.%d.%d\n",
                   g_netTxArp,
                   (tgt >> 24) & 0xFF, (tgt >> 16) & 0xFF, (tgt >> 8) & 0xFF, tgt & 0xFF,
                   (spa >> 24) & 0xFF, (spa >> 16) & 0xFF, (spa >> 8) & 0xFF, spa & 0xFF);
            fflush(stdout);
        }
        netHandleArp(partition, frame, len);
        return;
    }
    if (ethertype == 0x0800) {
        g_netTxIpv4++;
        netHandleIpv4(partition, frame, len);
        return;
    }
    if (ethertype == 0x86DD) g_netTxIpv6++; else g_netTxOther++;

    printf("[rtl8139] TX %u bytes, dst=%02X:%02X:%02X:%02X:%02X:%02X src=%02X:%02X:%02X:%02X:%02X:%02X ethertype=0x%04X\n",
           len, frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
           frame[6], frame[7], frame[8], frame[9], frame[10], frame[11], ethertype);
    fflush(stdout);
}

// Device-level TX entry point, called by the TSDx write handler -- this is
// the seam a future bridged or host-only backend plugs into. It only
// validates the frame and hands off to whichever backend is registered in
// g_netTransmit; it has no protocol knowledge of its own and never needs
// to change when the backend does.
void rtl8139TransmitFrame(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len) {
    if (len < 14) {
        printf("[rtl8139] TX %u bytes (too short for a full Ethernet header)\n", len);
        fflush(stdout);
        return;
    }
    g_netTxFrames++;
    if (g_netTransmit) g_netTransmit(partition, frame, len);
}

// Backend/test entry point (Phase 2): writes one frame into the RX ring as
// real hardware would on receipt, and raises ROK. Bounds-checked against
// the driver's real allocation (ring size + the 16-byte overflow pad real
// hardware relies on for near-boundary packets) -- if a frame wouldn't fit
// even using that pad, we wrap to the start rather than risk writing past
// what the guest actually allocated; this is a simplification of the exact
// hardware wraparound corner case (undocumented without a spec on hand)
// made in favor of never writing outside guest memory.
// How far past the end of the ring the chip may write when RCR's WRAP bit is
// set. The driver allocates this pad for exactly that purpose (the Linux
// 8139too driver calls it RX_BUF_WRAP_PAD and sizes it 2048).
#define RTL8139_RX_WRAP_PAD 2048

// Copies a packet into the ring at `pos`, honouring the chosen wrap behaviour.
// `linear` = write straight past the end of the ring into the wrap pad;
// otherwise the packet is split and its tail continues at offset 0. Either way
// the CALLER advances the write pointer by (pos + alignedLen) % ringSize --
// which is what the driver independently computes, and the reason neither mode
// is visible to it.
void rtl8139RingPut(unsigned char *ring, UINT32 ringSize, UINT32 pos,
                    const unsigned char *src, UINT32 n, int linear) {
    UINT32 first;
    if (linear || pos + n <= ringSize) { memcpy(ring + pos, src, n); return; }
    first = ringSize - pos;
    memcpy(ring + pos, src, first);
    memcpy(ring, src + first, n - first);
}

// Attempts to put one frame in the RX ring right now.
// Returns  1 = delivered, 0 = not possible YET (caller should queue/retry),
//         -1 = undeliverable, drop it.
int rtl8139DeliverFrame(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len) {
    unsigned char cr = rtl8139Regs[0x37];
    if (!(cr & 0x08)) { g_netRxDeferNoRe++; return 0; }   // RE (receiver enable) not set
    UINT32 rxBase = *(UINT32 *)&rtl8139Regs[0x30];
    if (!guestMemory) return -1;                          // nothing to write into, ever
    if (rxBase == 0) { g_netRxDeferNoBuf++; return 0; }   // RBSTART not programmed yet
    if (len > RTL8139_RX_QUEUE_FRAME) { g_netRxDropSize++; return -1; }

    // Pad runts to the 60-byte Ethernet minimum. A real NIC pads short frames
    // before they ever go on the wire, so a receiver never sees one shorter than
    // this -- and the driver treats a runt as a receive error and resets the
    // receiver rather than consuming it.
    //
    // This is why ARP never completed while DHCP always did: our ARP reply is 42
    // bytes and our DHCP reply is ~300. Every ARP answer we delivered was
    // rejected as a runt (CAPR frozen at 0 while our write pointer moved), so the
    // guest re-ARPed forever and the driver sat toggling RE hundreds of thousands
    // of times. Frame size, not the ring pointers, was the actual defect.
    unsigned char padded[64];
    if (len < 60) {
        if (len > sizeof(padded)) { g_netRxDropSize++; return -1; }
        memset(padded, 0, sizeof(padded));
        memcpy(padded, frame, len);
        frame = padded;
        len = 60;
        g_netRxPadded++;
    }

    UINT32 ringSize = rtl8139RxRingSize();
    // Header + frame + the 4-byte FCS. The FCS is NOT optional bookkeeping: we
    // report it in the length field below (rxLen = len + 4, which is what real
    // hardware does), and the driver derives the NEXT packet's offset from that
    // length. Advancing by only header+frame left our write pointer 4 bytes
    // behind the driver's read pointer for every single packet -- measured as
    // CAPR+16 = 0x144 against ourWritePos = 0x140 -- so the "ring is empty" test
    // below could never match, BUFE was never set, and the driver spun at
    // DISPATCH_LEVEL toggling RE 752,773 times waiting for a drain signal that
    // could not arrive. The guest wedged outright.
    UINT32 totalLen = 4 + len + 4;
    UINT32 alignedLen = (totalLen + 3) & ~3u; // next packet is 4-byte aligned
    if (totalLen > ringSize || totalLen > RTL8139_RX_WRAP_PAD ||
        (UINT64)rxBase + ringSize > guestMemSize) { g_netRxDropSize++; return -1; }

    // NEVER overwrite what the guest has not read. With no retransmission
    // anywhere, a single clobbered segment stalls a TCP stream forever. Real
    // hardware would drop the frame and flag RXOVW here, but a drop is exactly
    // what we cannot afford: defer instead and let the caller queue it, and only
    // report a real overflow once that queue is genuinely full.
    if (rtl8139RxRingUsed() + totalLen + 64 >= ringSize) {
        g_netRxDeferRingFull++;
        return 0;
    }

    UINT32 pos = rtl8139RxWritePos % ringSize;

    // THIS IS THE BUG behind "the guest accepts payload-free SYN-ACKs and ignores
    // every payload-bearing segment". Real silicon never skips to offset 0
    // because a frame did not fit: the driver derives the next packet's offset
    // as (pos + alignedLen) % ringSize and has NO WAY to learn about such a jump,
    // so from the first wrap onwards it read every subsequent packet out of the
    // wrong place -- garbage headers, garbage lengths, and eventually a software
    // reset. It is size-dependent for the obvious reason: 1400-byte data segments
    // reach the ring boundary far sooner than 60-byte control frames do, which is
    // precisely the asymmetry the U104 size ramp measured.
    //
    // Two behaviours are legal, selected by RCR's WRAP bit (bit 7):
    //   WRAP=1, ring < 64K: write the frame CONTIGUOUSLY past the end of the
    //     ring into the pad the driver allocated for it.
    //   WRAP=0, or a 64K ring (no room for a pad in a 16-bit offset): SPLIT the
    //     frame -- its tail continues at offset 0.
    // In both cases the write pointer advances by alignedLen modulo ringSize,
    // which is what the driver expects either way.
    UINT32 rcr = *(UINT32 *)&rtl8139Regs[0x44];
    int linearPad = ((rcr & 0x80) != 0) && ringSize < 65536 &&
                    (UINT64)rxBase + ringSize + RTL8139_RX_WRAP_PAD <= guestMemSize;

    g_netRxFrames++; QueryPerformanceCounter(&g_u108LastRx);   // past every guard: this frame really does go into the ring

    unsigned char *ring = (unsigned char *)guestMemory + rxBase;
    // RX status. ROK alone is NOT what real hardware reports: the upper bits say
    // WHY the frame was accepted -- BAR (bit 13) broadcast, PAM (bit 14) physical
    // address matched, MAR (bit 15) multicast. We reported none of them, so every
    // frame arrived looking like it matched no filter at all.
    //
    // This is the last structural difference between our receive path and real
    // silicon, and it fits the DNS symptom precisely: the wire format, checksums,
    // MAC, latency, interrupts and ring pointers are all now verified correct, yet
    // the guest ignores unicast replies while broadcast DHCP has always worked --
    // and PAM is exactly the bit that distinguishes those two cases.
    UINT16 rxStatus = 0x0001; // ROK
    {
        int isBroadcast = (frame[0] & frame[1] & frame[2] & frame[3] & frame[4] & frame[5]) == 0xFF;
        int isMulticast = (frame[0] & 0x01) != 0;
        if (isBroadcast)                              rxStatus |= 0x2000; // BAR
        else if (isMulticast)                         rxStatus |= 0x8000; // MAR
        else if (memcmp(frame, rtl8139Regs + 0x00, 6) == 0) rxStatus |= 0x4000; // PAM
    }
    UINT16 rxLen = (UINT16)(len + 4); // real hardware includes the 4-byte CRC
    // Assembled contiguously first, then handed to the ring writer -- the header
    // itself can straddle the ring boundary in split mode, so it cannot be poked
    // in directly.
    unsigned char pkt[8 + RTL8139_RX_QUEUE_FRAME];
    pkt[0] = (unsigned char)(rxStatus & 0xFF);
    pkt[1] = (unsigned char)(rxStatus >> 8);
    pkt[2] = (unsigned char)(rxLen & 0xFF);
    pkt[3] = (unsigned char)(rxLen >> 8);
    memcpy(pkt + 4, frame, len);
    // Occupy the 4 FCS bytes we just claimed in rxLen. Zeros: the driver has ROK
    // and does not re-check the checksum, but the SPACE has to be reserved or the
    // next packet lands where the driver expects the CRC to be.
    memset(pkt + 4 + len, 0, 4);
    // PUBLISH THE BODY BEFORE THE HEADER. The 4-byte header carries ROK and the
    // length -- it is the marker that says "a packet is here" -- so writing it
    // first advertises a frame whose bytes have not landed yet. The guest vCPU
    // runs on ANOTHER THREAD, concurrently: if the driver reads the ring inside
    // that window it takes a header claiming 1458 bytes and copies whatever stale
    // ring content is still underneath, then discards the result at the IP/TCP
    // checksum with no dup-ACK to tell us. Real silicon DMAs the payload and
    // commits the status word last, which is why this ordering matters.
    //
    // It fits every measurement: the tear window is the length of the memcpy, so
    // a 1400-byte segment loses the race almost always while a 60-byte SYN-ACK
    // almost always wins it -- and capping segments at 300 bytes made guestAcked
    // advance for the first time without ever making it reliable. One 1440-byte
    // segment did get ACKed (ack=2441), which a hard size limit cannot explain
    // but a race can.
    UINT32 bodyPos = linearPad ? (pos + 4) : ((pos + 4) % ringSize);
    rtl8139RingPut(ring, ringSize, bodyPos, pkt + 4, totalLen - 4, linearPad);
    MemoryBarrier(); // body must be visible to the vCPU before the marker is
    rtl8139RingPut(ring, ringSize, pos, pkt, 4, linearPad);

    rtl8139RxWritePos = (pos + alignedLen) % ringSize;
    *(UINT16 *)&rtl8139Regs[0x3A] = (UINT16)rtl8139RxWritePos; // CBR
    rtl8139Regs[0x37] &= ~0x01; // BUFE clear -- data now available

    *(UINT16 *)&rtl8139Regs[0x3E] |= 0x0001; // ISR: ROK
    rtl8139MaybeInjectIrq(partition);


    // Per-frame logging is bounded: this path now runs at real throughput
    // (thousands of frames per transfer), and an fflush'd printf per frame was
    // itself enough to pace the receive path.
    if (g_netRxFrames <= 32) {
        printf("[rtl8139] RX %u bytes at ring offset 0x%X%s\n", len, pos,
               (pos + totalLen > ringSize) ? (linearPad ? " (into wrap pad)" : " (split at ring end)") : "");
        fflush(stdout);
    }
    return 1;
}

// Delivers as much of the pending queue as the device will currently take,
// oldest first. Order matters: a TCP stream reordered here looks to the guest
// exactly like the loss we are trying to avoid.
void rtl8139FlushRxQueue(WHV_PARTITION_HANDLE partition) {
    while (rtl8139RxQueueCount) {
        Rtl8139RxQueued *q = &rtl8139RxQueue[rtl8139RxQueueHead];
        if (rtl8139DeliverFrame(partition, q->data, q->len) == 0) break; // still not ready
        rtl8139RxQueueHead = (rtl8139RxQueueHead + 1) % RTL8139_RX_QUEUE_DEPTH;
        rtl8139RxQueueCount--;
    }
}

// Backend -> device. Delivers immediately when the device is ready, and queues
// rather than drops when it is not (see the queue's comment for why a drop here
// is unrecoverable).
void rtl8139ReceiveFrame(WHV_PARTITION_HANDLE partition, const unsigned char *frame, UINT32 len) {
    // Counted per REASON, not once at entry. Counting attempts here (as this
    // first did) makes "we delivered 52 frames" indistinguishable from "we were
    // asked to deliver 52 and dropped them all", which is exactly the ambiguity
    // that matters when the guest keeps re-ARPing: it looks like we are answering.
    g_netRxOffered++;

    // Drain what is already waiting first, or this frame would overtake it.
    rtl8139FlushRxQueue(partition);
    if (rtl8139RxQueueCount == 0 && rtl8139DeliverFrame(partition, frame, len) != 0) {
        return;  // delivered, or rejected for a reason retrying cannot fix
    }
    if (len > RTL8139_RX_QUEUE_FRAME) { g_netRxDropSize++; return; }
    if (rtl8139RxQueueCount >= RTL8139_RX_QUEUE_DEPTH) {
        // A real overflow at last: the driver has not drained for long enough to
        // fill the whole queue. THIS is what RXOVW is for -- report it the way
        // hardware would, and let the driver recover.
        g_netRxQueueOverflow++;
        *(UINT16 *)&rtl8139Regs[0x3E] |= 0x0010; // ISR: RXOVW
        rtl8139MaybeInjectIrq(partition);
        return;
    }
    {
        UINT32 slot = (rtl8139RxQueueHead + rtl8139RxQueueCount) % RTL8139_RX_QUEUE_DEPTH;
        rtl8139RxQueue[slot].len = len;
        memcpy(rtl8139RxQueue[slot].data, frame, len);
        rtl8139RxQueueCount++;
        if ((long)rtl8139RxQueueCount > g_netRxQueuePeak) g_netRxQueuePeak = (long)rtl8139RxQueueCount;
    }
}
// PxIS is architecturally RWC (spec 3.3.16) -- clearable by the guest
// writing 1 to a set bit. Our ABAR is plain, untrapped guest RAM (no MMIO
// decode, see ahciProcessPendingCommands), so we can't detect that write
// directly; instead, stale bits from the previous command get cleared when
// the next command starts processing (see ahciProcessPendingCommands),
// which is both simpler and more robust than an earlier "pulse" approach
// that auto-cleared DHRS on the very next host poll -- fast enough that
// the guest's own (much slower, real-time-paced) polling could miss the
// bit being set entirely.

void pciInitConfigSpaces(void) {
    pciHostBridgeConfig[0x00] = 0x86; pciHostBridgeConfig[0x01] = 0x80; // vendor 0x8086
    pciHostBridgeConfig[0x02] = 0x37; pciHostBridgeConfig[0x03] = 0x12; // device 0x1237 (i440fx)
    pciHostBridgeConfig[0x08] = 0x02; // revision ID
    pciHostBridgeConfig[0x0A] = 0x00; // subclass: host bridge
    pciHostBridgeConfig[0x0B] = 0x06; // base class: bridge device
    pciHostBridgeConfig[0x0E] = 0x00; // header type 0, single-function

    pciIsaBridgeConfig[0x00] = 0x86; pciIsaBridgeConfig[0x01] = 0x80; // vendor 0x8086
    pciIsaBridgeConfig[0x02] = 0x00; pciIsaBridgeConfig[0x03] = 0x70; // device 0x7000 (PIIX3)
    pciIsaBridgeConfig[0x0A] = 0x01; // subclass: ISA bridge
    pciIsaBridgeConfig[0x0B] = 0x06; // base class: bridge device
    pciIsaBridgeConfig[0x0E] = 0x80; // header type 0 + multi-function bit (so func 3 gets probed)

    pciPmConfig[0x00] = 0x86; pciPmConfig[0x01] = 0x80; // vendor 0x8086
    pciPmConfig[0x02] = 0x13; pciPmConfig[0x03] = 0x71; // device 0x7113 (PIIX4 PM)
    pciPmConfig[0x0A] = 0x80; // subclass: other bridge type
    pciPmConfig[0x0B] = 0x06; // base class: bridge device
    pciPmConfig[0x0E] = 0x00;

    // Matches a real ICH9 AHCI controller's IDs -- class-code binding means
    // the exact vendor/device mostly doesn't matter to a driver, but
    // matching a real one avoids tripping any vendor-ID-keyed quirk table.
    pciAhciConfig[0x00] = 0x86; pciAhciConfig[0x01] = 0x80; // vendor 0x8086
    pciAhciConfig[0x02] = 0x22; pciAhciConfig[0x03] = 0x29; // device 0x2922 (ICH9 AHCI)
    pciAhciConfig[0x08] = 0x02; // revision ID
    pciAhciConfig[0x09] = 0x01; // prog IF: AHCI 1.0
    pciAhciConfig[0x0A] = 0x06; // subclass: SATA controller
    pciAhciConfig[0x0B] = 0x01; // base class: mass storage controller
    pciAhciConfig[0x0E] = 0x00; // header type 0, single-function
    // U55: interrupt pin. This was never set, so the device reported pin 0 --
    // "generates no interrupt at all" -- and Windows' storahci miniport requires
    // an interrupt to start. PnP had nothing to route through the _PRT, the
    // miniport never started, the boot volume never came online, and the guest sat
    // at the Windows boot spinner forever: heavy reads while the firmware loaded
    // (3988 of them, firmware polls rather than using interrupts, so it was
    // unaffected), then zero writes and an idle CPU in HalProcessorIdle.
    // INTA# matches the _PRT entry for device 2 in acpi/dsdt.asl (-> GSI 16).
    pciAhciConfig[0x3D] = 0x01; // interrupt pin: INTA#

    // 0:5.0 -- Intel 82540EM backup NIC. Inbox Windows driver (e1i65x64/e1k).
    // Device ID 0x100E is the QEMU/Bochs e1000 identity. Subsystem 8086:001E
    // is the copper 82540EM board ID the same driver matches.
    pciE1000Config[0x00] = 0x86; pciE1000Config[0x01] = 0x80; // vendor 0x8086
    pciE1000Config[0x02] = 0x0E; pciE1000Config[0x03] = 0x10; // device 0x100E
    pciE1000Config[0x08] = 0x03; // revision
    pciE1000Config[0x0A] = 0x00; // subclass: ethernet
    pciE1000Config[0x0B] = 0x02; // base class: network
    pciE1000Config[0x0E] = 0x00;
    pciE1000Config[0x2C] = 0x86; pciE1000Config[0x2D] = 0x80; // subsys vendor
    pciE1000Config[0x2E] = 0x1E; pciE1000Config[0x2F] = 0x00; // subsys 0x001E
    pciE1000Config[0x3D] = 0x01; // INTA# -> GSI 21 via the _PRT
    e1000Reset();

    // 0:4.0 -- EHCI USB 2.0 controller (ICH9 USB2 EHCI #1). Class 0C/03/20 is
    // what makes Windows load usbehci.sys against it; prog-IF 0x20 specifically
    // means EHCI (0x00 UHCI, 0x10 OHCI, 0x30 xHCI).
#if LH_USE_XHCI
    // NEC/Renesas uPD720200, the same controller QEMU presents. Deliberately NOT
    // an Intel part: the first attempt used Intel 0x1E31 (7 Series) and Windows
    // wrote bit 16 of USBCMD, which xHCI does not define -- the signature of a
    // chipset-specific quirk path (Intel controllers carry extra port-routing
    // registers like XUSB2PR that we do not model). Matching a real chip is
    // usually the right instinct, and it is what the AHCI and RTL8139 IDs do,
    // but here it invites initialisation we cannot answer. A generic controller
    // binds the same inbox USBXHCI.sys by class code 0C0330 with no quirks.
    pciEhciConfig[0x00] = 0x33; pciEhciConfig[0x01] = 0x10; // vendor 0x1033 (NEC)
    pciEhciConfig[0x02] = 0x94; pciEhciConfig[0x03] = 0x01; // device 0x0194 (uPD720200)
    pciEhciConfig[0x08] = 0x03; // revision ID
    pciEhciConfig[0x09] = 0x30; // prog IF: xHCI
#else
    pciEhciConfig[0x00] = 0x86; pciEhciConfig[0x01] = 0x80; // vendor 0x8086 (Intel)
    pciEhciConfig[0x02] = 0x3A; pciEhciConfig[0x03] = 0x29; // device 0x293A (ICH9 EHCI)
    pciEhciConfig[0x08] = 0x03; // revision ID
    pciEhciConfig[0x09] = 0x20; // prog IF: EHCI
#endif
    pciEhciConfig[0x0A] = 0x03; // subclass: USB controller
    pciEhciConfig[0x0B] = 0x0C; // base class: serial bus controller
    pciEhciConfig[0x0E] = 0x00; // header type 0, single-function
    pciEhciConfig[0x3D] = 0x01; // interrupt pin: INTA# -> GSI 20 via the _PRT
#if LH_USE_XHCI
    // MSI capability. xHCI effectively assumes message-signalled interrupts --
    // the spec's interrupter model is built around them -- and USBXHCI was
    // looping through init/teardown against a function that advertised NO
    // capability list at all (no pointer at 0x34, status bit 4 clear), which is
    // a combination no real xHCI presents.
    //
    // Structure at 0x50: ID(0x05), next(0), Message Control, Address lo/hi,
    // Data. 64-bit capable (bit 7 of control) because that is what real parts
    // report; the guest writes the address and data, and the low byte of the
    // data IS the vector we inject.
    // Message Control is the 16-bit word at 0x52. Bit 0 MSI Enable (guest
    // writes), bits 3:1 Multiple Message Capable = 000 (one vector), bit 7
    // 64-bit Address Capable. With 64-bit capable set, Message Data sits at
    // 0x5C rather than 0x58.
    pciEhciConfig[0x06] |= 0x10;  // status: capabilities list present
    pciEhciConfig[0x34] = 0x50;   // capabilities pointer
    pciEhciConfig[0x50] = 0x05;   // cap ID: MSI
    pciEhciConfig[0x51] = 0x00;   // next: end of list
    pciEhciConfig[0x52] = 0x80;   // control lo: 64-bit capable, MSI disabled
    pciEhciConfig[0x53] = 0x00;   // control hi
#endif

    // Real Realtek RTL8139 IDs -- same reasoning as AHCI above, matches a
    // real chip so nothing keyed off vendor/device ID gets confused.
    pciRtl8139Config[0x00] = 0xEC; pciRtl8139Config[0x01] = 0x10; // vendor 0x10EC (Realtek)
    pciRtl8139Config[0x02] = 0x39; pciRtl8139Config[0x03] = 0x81; // device 0x8139
    // Revision 0x20 = RTL8139C+, matching the TCR hardware-version ID we report
    // (g_rtlHwVerId, 0x74800000) and matching QEMU, which uses
    // RTL8139_PCI_REVID_8139CPLUS = 0x20 by default. These two are one identity:
    // reporting a C+ version word from a rev-0x10 chip is a combination no real
    // card produces, and the driver is entitled to disbelieve it.
    pciRtl8139Config[0x08] = g_rtlPciRev; // revision ID
    // Subsystem vendor/device. Left at 0000:0000 before, which no real card
    // reports. Real 8139s echo the Realtek IDs here and so does QEMU's model --
    // and QEMU's rtl8139 is known to bind Windows' inbox driver successfully,
    // which makes it the reference worth matching. Storage got away without one
    // (pciAhciConfig sets no subsystem either and storahci binds fine), but NDIS
    // miniports inspect more of config space than storage miniports do.
    pciRtl8139Config[0x2C] = 0xEC; pciRtl8139Config[0x2D] = 0x10; // subsys vendor 0x10EC
    pciRtl8139Config[0x2E] = 0x39; pciRtl8139Config[0x2F] = 0x81; // subsys device 0x8139
    pciRtl8139Config[0x0A] = 0x00; // subclass: ethernet controller
    pciRtl8139Config[0x0B] = 0x02; // base class: network controller
    pciRtl8139Config[0x0E] = 0x00; // header type 0, single-function
    pciRtl8139Config[0x3D] = 0x01; // interrupt pin: INTA#

    pciConfigSpacesInit = 1;
}

unsigned char *pciSelectConfigSpace(UINT32 bus, UINT32 dev, UINT32 func) {
    if (bus != 0) return NULL;
    if (dev == 0 && func == 0) return pciHostBridgeConfig;
    if (dev == 1 && func == 0) return pciIsaBridgeConfig;
    if (dev == 1 && func == 3) return pciPmConfig;
    if (dev == 2 && func == 0) return pciAhciConfig;
    if (dev == 3 && func == 0) return pciRtl8139Config;
    if (dev == 4 && func == 0) return pciEhciConfig;
    if (dev == 5 && func == 0) return pciE1000Config;
    return NULL;
}

// Registers that stay fixed no matter what's written: Vendor/Device ID
// (0x00-0x03), Revision/Class (0x08-0x0B), and Header Type (0x0E) -- the
// true identity/class fields per the PCI Type 0 header spec -- plus the six
// BARs (0x10-0x27, unimplemented in this stub), the expansion ROM base
// (0x30), the capabilities pointer (0x34, fixed at 0 -- terminates the list
// immediately, i.e. "no capabilities," so nothing goes looking for a
// capability chain we don't model), and the interrupt pin (0x3D, "uses no
// interrupt"). Command/Status (0x04-0x07), Cache Line Size/Latency Timer
// (0x0C-0x0D), and BIST (0x0F) are genuinely writable on real hardware --
// software needs Command, in particular, to enable I/O/memory/bus-master
// decode -- and were previously (incorrectly) swept into the same
// "0x00-0x0F is read-only" rule as the identity fields, silently
// discarding writes to it.
int pciRegisterIsReadOnly(UINT32 offset) {
    if (offset <= 0x03) return 1;
    if (offset >= 0x08 && offset <= 0x0B) return 1;
    if (offset == 0x0E) return 1;
    if (offset >= 0x10 && offset <= 0x27) return 1;
    if (offset == 0x30 || offset == 0x31 || offset == 0x32 || offset == 0x33) return 1;
    if (offset == 0x34) return 1;
    if (offset == 0x3D) return 1;
    return 0;
}

// Fills a freshly-allocated ABAR buffer with the HBA/port0 register reset
// state a real AHCI 1.3 controller with one SATA disk attached would show:
// one port implemented (PI), that port already showing a communicated,
// present SATA (non-ATAPI) device (PxSSTS/PxSIG) so the driver doesn't need
// to run a real COMRESET/link-training sequence, and PxTFD matching the
// legacy ATA path's own idle status (DRDY|DSC, no error).
void ahciInitAbarRegisters(unsigned char *abar) {
    int p;
    UINT32 pi = 0;
    int implemented = ahciPortsImplemented();
    memset(abar, 0, AHCI_BAR_SIZE);
    // U59: CAP was 0x00200001, which did not match the comment or PI. CAP.NP
    // (bits 4:0) is 0-BASED, so the 1 in the low bits advertised TWO ports while
    // PI declared only port 0 -- an inconsistency storahci can see. CAP.NCS
    // (bits 12:8) is also 0-based and was 0, i.e. a single command slot, which is
    // legal but unlike any real controller and leaves a driver no room to queue.
    // ISS=2 (Gen2, bits 23:20), NCS=31 (32 slots), NP = implemented-1 (0-BASED,
    // and it has to agree with PI or storahci can see the inconsistency).
    // The command-issue path already scans all 32 PxCI bits, so 32 slots is safe.
    if (implemented < 1) implemented = 1;
    *(UINT32 *)(abar + 0x00) = 0x00201F00 | (UINT32)(implemented - 1);
    *(UINT32 *)(abar + 0x04) = 0x00000000; // GHC: AE/HR/IE all clear until guest sets them
    *(UINT32 *)(abar + 0x10) = 0x00010301; // VS: AHCI 1.3.1

    // Only ports with a device behind them are advertised. Declaring a port
    // implemented but empty makes the driver probe something that will never
    // answer, and it would count toward NP above for no reason.
    for (p = 0; p < AHCI_PORT_COUNT; p++) {
        unsigned char *port;
        if (!ahciPorts[p].present) continue;
        pi |= (1u << p);
        port = abar + 0x100 + (UINT32)p * 0x80;
        *(UINT32 *)(port + 0x20) = 0x00000050; // PxTFD: DRDY|DSC, no error
        // PxSIG stays the plain-SATA-disk signature even for an ISO: the ISO is
        // presented as a block device with 2048-byte sectors rather than as an
        // ATAPI packet device, which is what makes EDK2's El Torito parser pick
        // it up. Reporting 0xEB140101 here would promise a packet interface this
        // does not implement.
        *(UINT32 *)(port + 0x24) = 0x00000101;
        *(UINT32 *)(port + 0x28) = 0x00000123; // PxSSTS: DET=3(present), SPD=2(3Gbps), IPM=1(active)
    }
    *(UINT32 *)(abar + 0x0C) = pi ? pi : 0x00000001; // PI
}

// U58: trap the ABAR as real MMIO instead of leaving it as plain guest RAM.
//
// WHY. Several AHCI registers are write-1-to-clear (RW1C): the global IS, the
// per-port PxIS, and PxSERR. With the ABAR mapped as ordinary RAM a guest write
// of 1 simply STORES 1, so an acknowledged interrupt status bit never clears.
// EDK2 tolerated that because it polls PxCI/PxTFD and issues commands, and the
// engine cleared stale bits at each command boundary. storahci is interrupt
// driven: its ISR reads PxIS, writes the same value back to acknowledge, and
// expects the bit to drop. It never did, so with the U56 level-triggered
// injection the controller kept re-asserting an interrupt the driver had already
// acknowledged, and the port never advanced to issuing a command -- exactly the
// observed PxIS stuck at 1 with PxCI never set.
//
// Trapping lets the register file behave like hardware: RW1C bits clear on write,
// read-only bits ignore writes, and everything else stores normally. The command
// list and FIS receive areas live in ordinary guest RAM, not here, so only
// register accesses trap -- a small, bounded volume.
//
// Returns 1 if the access was inside the ABAR window and has been fully handled
// (registers updated, RIP advanced); 0 to let the caller fall through.
// U59: names for the registers storahci touches, so the trace reads as a protocol
// conversation rather than raw offsets.
#define U59_ABAR_LOG_MAX 400
static int g_ahciMmioLogged = 0;
static const char *ahciRegName(UINT32 off) {
    switch (off) {
        case 0x000: return "CAP";
        case 0x004: return "GHC";
        case 0x008: return "IS";
        case 0x00C: return "PI";
        case 0x010: return "VS";
        case 0x024: return "CAP2";
        case 0x100: return "PxCLB";
        case 0x104: return "PxCLBU";
        case 0x108: return "PxFB";
        case 0x10C: return "PxFBU";
        case 0x110: return "PxIS";
        case 0x114: return "PxIE";
        case 0x118: return "PxCMD";
        case 0x120: return "PxTFD";
        case 0x124: return "PxSIG";
        case 0x128: return "PxSSTS";
        case 0x12C: return "PxSCTL";
        case 0x130: return "PxSERR";
        case 0x134: return "PxSACT";
        case 0x138: return "PxCI";
        default:    return "?";
    }
}

// Both defined further down with the I/O APIC MMIO emulation; reused here so the
// two trapped-MMIO devices share one instruction decoder and GPR name table.
extern WHV_REGISTER_NAME ioapicGprNames[16];
int ioapicDecodeMmio(unsigned char *insn, int len, int *isWrite, int *isImm,
                     int *regNum, UINT32 *immVal, int *totalLen);

int ahciHandleAbarMmio(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    UINT64 gpa = exitContext->MemoryAccess.Gpa;
    if (!ahciAbarMapped || !ahciAbarMemory) return 0;
    if (gpa < ahciAbarBase || gpa >= (UINT64)ahciAbarBase + AHCI_BAR_SIZE) return 0;
    UINT32 off = (UINT32)(gpa - ahciAbarBase);

    // Split ABAR traffic by who is driving: firmware, or the Windows kernel once
    // it has been discovered. Windows Setup reports "we couldn't find any drives",
    // and the totals say the guest has read ZERO sectors while the firmware read
    // 109MB -- so the question is whether storahci ever touches this controller at
    // all, or never binds to it. Those need different fixes and the aggregate
    // command counter cannot tell them apart.
    if (g_bpModuleBase) {
        g_ahciAbarGuestAccesses++;
        if (g_ahciAbarGuestAccesses == 1) {
            printf("[ahci-guest] FIRST kernel-side ABAR access at off=0x%X (rip=0x%llX)\n",
                   off, (unsigned long long)exitContext->VpContext.Rip);
            fflush(stdout);
        }
    } else {
        g_ahciAbarFwAccesses++;
    }

    int isWrite = 0, isImm = 0, regNum = 0, insnTotalLen = 0;
    UINT32 immVal = 0;
    if (!ioapicDecodeMmio(exitContext->MemoryAccess.InstructionBytes,
                          exitContext->MemoryAccess.InstructionByteCount,
                          &isWrite, &isImm, &regNum, &immVal, &insnTotalLen)) {
        static int failLog = 0;
        if (failLog++ < 20) {
            printf("[ahci-mmio] undecodable instruction at rip=0x%llX off=0x%X (%d bytes):",
                   (unsigned long long)exitContext->VpContext.Rip, off,
                   exitContext->MemoryAccess.InstructionByteCount);
            int bi;
            for (bi = 0; bi < exitContext->MemoryAccess.InstructionByteCount; bi++)
                printf(" %02X", exitContext->MemoryAccess.InstructionBytes[bi]);
            printf("\n");
            fflush(stdout);
        }
        return 0; // let the generic handler deal with it rather than corrupting state
    }

    unsigned char *ab = (unsigned char *)ahciAbarMemory;
    UINT32 *reg = (UINT32 *)(ab + (off & ~3u));

    // U59: log the driver's register conversation. This is only possible now that
    // the ABAR is trapped -- while it was passive RAM these accesses were entirely
    // invisible, which is why the last few units had to infer state from polled
    // snapshots and got it wrong twice. Gated on the kernel having been discovered
    // so we see storahci's conversation rather than thousands of firmware accesses,
    // and capped so it cannot flood.
    int u59Log = LOCALHOST_VERBOSE_DIAG && (g_bpModuleBase != 0) && (g_ahciMmioLogged < U59_ABAR_LOG_MAX);

    if (isWrite) {
        UINT32 value = immVal;
        if (!isImm) {
            WHV_REGISTER_VALUE regVal = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &regVal);
            value = (UINT32)regVal.Reg64;
        }
        UINT32 aligned = off & ~3u;
        // PORT-RELATIVE, not hardcoded to port 0. Every case below was written
        // when one port existed, so it matched only 0x1xx -- port 0's block.
        // With a second port implemented, port 1's registers (0x180-0x1FF) fell
        // through to the plain-store default: no write-1-to-clear on its PxIS and
        // PxSERR, and its read-only status registers writable by the driver.
        // Normalising the offset applies the same rules to every port.
        if (aligned >= 0x100 && aligned < 0x100 + AHCI_PORT_COUNT * 0x80)
            aligned = 0x100 + ((aligned - 0x100) % 0x80);
        switch (aligned) {
            // Read-only: capabilities, version, ports-implemented, and the port
            // status registers the device owns. Writes are silently dropped, as
            // hardware does.
            case 0x00: case 0x0C: case 0x10: case 0x24:            /* CAP, PI, VS, CAP2 */
            case 0x120: case 0x124: case 0x128: case 0x12C:        /* PxTFD, PxSIG, PxSSTS, PxSCTL(partially) */
                break;
            // Write-1-to-clear. This is the whole point of trapping: a written 1
            // CLEARS the corresponding bit rather than setting it.
            case 0x08:                                             /* global IS */
            case 0x110:                                            /* PxIS */
            case 0x130:                                            /* PxSERR */
                // U61: the driver acknowledging an interrupt. Visible only because
                // U58 trapped the ABAR -- as passive RAM these writes could not be
                // seen at all, which is what made the U56/U57 handshake guesswork.
                if (LOCALHOST_VERBOSE_DIAG && g_bpModuleBase && g_ahciAckLogged < U61_IRQ_LOG_MAX &&
                    (aligned == 0x110 || aligned == 0x08)) {
                    g_ahciAckLogged++;
                    printf("[u61] ACK  #%d: %s write=0x%08X  0x%08X -> 0x%08X\n",
                           g_ahciAckLogged, ahciRegName(aligned), value, *reg, *reg & ~value);
                    fflush(stdout);
                }
                *reg &= ~value;
                break;
            // U61: PxCMD mixes driver-writable control bits with device-owned
            // read-only status. CR (bit 15, command list running), FR (bit 14, FIS
            // receive running) and CCS (bits 12:8, current command slot) are status
            // the device sets -- a driver write must not disturb them.
            //
            // Missing this in U58 caused a self-sustaining interrupt loop: the
            // driver would write PxCMD (with FR reading back as 0 in its copy), we
            // stored that verbatim and cleared FR, the engine then saw "FRE set but
            // FR clear", treated it as a fresh FRE enable, re-posted the U57 initial
            // D2H FIS and re-set PxIS.DHRS. The driver acknowledged correctly every
            // time (visible in the trace) and we immediately re-raised it, ~500
            // exits apart, forever. Preserve the read-only bits instead.
            case 0x118: {                                          /* PxCMD */
                const UINT32 roMask = 0x0000DF00u;                 /* CR | FR | CCS */
                *reg = (*reg & roMask) | (value & ~roMask);
                break;
            }
            default:
                *reg = value;
                break;
        }
        if (u59Log) {
            g_ahciMmioLogged++;
            printf("[u59] WRITE %-10s (0x%03X) value=0x%08X -> now 0x%08X\n",
                   ahciRegName(off & ~3u), off & ~3u, value, *reg);
            fflush(stdout);
        }
        // The DECODED WRITE VALUE, during the boot application's window. The
        // generic MMIO trace logs the register's content BEFORE the write lands,
        // because it runs ahead of this handler -- so it cannot answer the one
        // question that matters here: is the driver actually setting a PxCI bit
        // (a command being issued) or writing zero?
        if (g_bootImgRunning && g_bootImgWriteLogged < 60) {
            UINT32 rawOff = off & ~3u;
            g_bootImgWriteLogged++;
            printf("[bootimg-w] AHCI %-6s (0x%03X) wrote=0x%08X -> now 0x%08X\n",
                   ahciRegName(rawOff), rawOff, value, *reg);
            fflush(stdout);
        }
    } else {
        UINT32 value = *reg;
        WHV_REGISTER_VALUE regVal = { 0 };
        regVal.Reg64 = value;
        WHvSetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &regVal);
        if (u59Log) {
            g_ahciMmioLogged++;
            printf("[u59] read  %-10s (0x%03X) -> 0x%08X\n",
                   ahciRegName(off & ~3u), off & ~3u, value);
            fflush(stdout);
        }
    }

    // As with the IOAPIC path, InstructionLength is not populated for MMIO exits,
    // so advance RIP by the length our own decode determined.
    WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
    WHV_REGISTER_VALUE ripVal = { 0 };
    ripVal.Reg64 = exitContext->VpContext.Rip + (UINT64)insnTotalLen;
    WHvSetVirtualProcessorRegisters(partition, 0, &ripName, 1, &ripVal);
    return 1;
}

// EHCI BAR0 (offset 0x10), same sizing protocol as AHCI's BAR5: write all-ones
// to read back a size mask, then write the real aligned base. Unlike the ABAR we
// never allocate a backing buffer -- every EHCI register has semantics, so the
// GPA is left unmapped and each access faults out to ehciHandleMmio below.
void ehciHandleBar0Access(WHV_X64_IO_PORT_ACCESS_CONTEXT *io, UINT32 baseOffset,
                          UINT32 accessSize, UINT64 *rax) {
    if (io->AccessInfo.IsWrite) {
        if (baseOffset == 0x10 && accessSize >= 4) {
            UINT32 written = (UINT32)io->Rax;
            if (written == 0xFFFFFFFF) {
                ehciBarSizing = 1;
            } else {
                ehciBarSizing = 0;
                UINT32 newBase = written & ~(UINT32)(EHCI_BAR_SIZE - 1);
                if (newBase != 0 && newBase != ehciBarBase) {
                    ehciBarBase = newBase;
                    ehciBarMapped = 1;
                    printf("[ehci] BAR0 at 0x%X -- trapped MMIO (%d bytes, decoded per access)\n",
                           ehciBarBase, EHCI_BAR_SIZE);
                    fflush(stdout);
                }
            }
        }
        return;
    }
    if (baseOffset == 0x10 && accessSize >= 4) {
        // Size mask: 4KB, memory space, 32-bit, non-prefetchable (low bits 0).
        *rax = ehciBarSizing ? (UINT32)(~(EHCI_BAR_SIZE - 1)) : ehciBarBase;
    }
}

// --- USB HID tablet: descriptors -------------------------------------------
//
// A single high-speed device permanently attached to the root port. The report
// descriptor is the whole point: X and Y are declared ABSOLUTE (Input flag 0x02
// = Data,Var,Abs) with a 0..32767 logical range, so each report carries a
// position rather than a delta. That is what lets the guest cursor sit exactly
// where the host cursor is, which a PS/2 mouse can never do.
static const unsigned char usbTabletDeviceDesc[18] = {
    18, 0x01,               // bLength, bDescriptorType = DEVICE
    0x00, 0x02,             // bcdUSB 2.00
    0x00, 0x00, 0x00,       // class/subclass/protocol: defined at interface level
    64,                     // bMaxPacketSize0
    0x27, 0x06,             // idVendor  0x0627 (QEMU's, a known-good HID tablet id)
    0x01, 0x00,             // idProduct 0x0001
    0x00, 0x01,             // bcdDevice 1.00
    1, 2, 0,                // iManufacturer, iProduct, iSerialNumber
    1                       // bNumConfigurations
};

// Config (9) + Interface (9) + HID (9) + Endpoint (7) = 34 bytes.
static const unsigned char usbTabletConfigDesc[34] = {
    9, 0x02,                // bLength, CONFIGURATION
    34, 0x00,               // wTotalLength
    1,                      // bNumInterfaces
    1,                      // bConfigurationValue
    0,                      // iConfiguration
    0xA0,                   // bmAttributes: bus powered, remote wakeup
    50,                     // bMaxPower (100mA)

    9, 0x04,                // bLength, INTERFACE
    0,                      // bInterfaceNumber
    0,                      // bAlternateSetting
    1,                      // bNumEndpoints
    0x03,                   // bInterfaceClass: HID
    0x00,                   // bInterfaceSubClass: no boot protocol -- a boot mouse
                            // is relative by definition, so claiming it would
                            // invite the guest to use the wrong report format
    0x00,                   // bInterfaceProtocol
    0,                      // iInterface

    9, 0x21,                // bLength, HID descriptor
    0x01, 0x01,             // bcdHID 1.01
    0x00,                   // bCountryCode
    1,                      // bNumDescriptors
    0x22,                   // bDescriptorType: REPORT
    70, 0x00,               // wDescriptorLength -- MUST equal the exact item byte
                            // count below. Declaring 74 against a 70-byte item
                            // list sent four trailing 0x00 bytes, which are not
                            // valid HID items: hidparse rejected the descriptor,
                            // the device never started, and Windows halted the
                            // whole controller (RS cleared, HCHalted set, port
                            // PED dropped) right after reading it.

    7, 0x05,                // bLength, ENDPOINT
    0x81,                   // bEndpointAddress: EP1 IN
    0x03,                   // bmAttributes: interrupt
    8, 0x00,                // wMaxPacketSize (our report is 6 bytes)
    // bInterval. This device reports bcdUSB 2.00 and lives on EHCI, so the field
    // is LOGARITHMIC, not a plain frame count: the period is
    // 2^(bInterval-1) microframes x 125us.
    //   10 -> 2^9  = 512 microframes = 64ms  ->  ~15 position updates/sec
    //    8 -> 2^7  = 128 microframes = 16ms  ->  ~62/sec
    //    7 -> 2^6  =  64 microframes =  8ms  ->  ~125/sec
    //
    // MEASURED, with the pointer being moved continuously: the HOST supplies
    // ~79 position updates/sec (g_tabletMoves). That is the ceiling on anything
    // useful -- polling faster only re-sends a position the guest already has.
    //   at 10: sampling a 79/sec source 15 times a second discards ~80% of the
    //          motion, which is why the pointer tracked correctly but felt slow
    //   at  7: 150 reports/sec against 79 real changes = 1.9x duplicates, and
    //          every poll is an EHCI schedule walk on a vCPU thread that is
    //          ALREADY SATURATED (halted=0ms, 0.0% idle). Exits went 6242 -> 9264
    //          per second, and the guest felt WORSE, not better -- the extra
    //          polling stole more time than the extra samples were worth.
    // 8 is the compromise the measurement points to: 4x the original sample rate,
    // still above what the eye needs for smooth motion, at half the polling cost
    // of 7. The field is logarithmic so there is nothing between 62 and 125.
    //
    // If this ever needs to go higher, make the guest cheaper to poll first --
    // the constraint is vCPU time, not the HID stack. (The old ~300/sec that
    // "drowned the HID queue" was re-servicing retired qTDs, a different fault.)
    8                       // bInterval
};

// 6-byte report: buttons(1) + X(2, LE) + Y(2, LE) + wheel(1).
static const unsigned char usbTabletReportDesc[70] = {
    0x05, 0x01,             // Usage Page (Generic Desktop)
    0x09, 0x02,             // Usage (Mouse)
    0xA1, 0x01,             // Collection (Application)
    0x09, 0x01,             //   Usage (Pointer)
    0xA1, 0x00,             //   Collection (Physical)
    0x05, 0x09,             //     Usage Page (Button)
    0x19, 0x01,             //     Usage Minimum (1)
    0x29, 0x03,             //     Usage Maximum (3)
    0x15, 0x00,             //     Logical Minimum (0)
    0x25, 0x01,             //     Logical Maximum (1)
    0x95, 0x03,             //     Report Count (3)
    0x75, 0x01,             //     Report Size (1)
    0x81, 0x02,             //     Input (Data,Var,Abs)
    0x95, 0x01,             //     Report Count (1)
    0x75, 0x05,             //     Report Size (5)
    0x81, 0x01,             //     Input (Constant) -- padding to a byte
    0x05, 0x01,             //     Usage Page (Generic Desktop)
    0x09, 0x30,             //     Usage (X)
    0x09, 0x31,             //     Usage (Y)
    0x15, 0x00,             //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,       //     Logical Maximum (32767)
    0x35, 0x00,             //     Physical Minimum (0)
    0x46, 0xFF, 0x7F,       //     Physical Maximum (32767)
    0x75, 0x10,             //     Report Size (16)
    0x95, 0x02,             //     Report Count (2)
    0x81, 0x02,             //     Input (Data,Var,ABSOLUTE) -- the whole point
    0x05, 0x01,             //     Usage Page (Generic Desktop)
    0x09, 0x38,             //     Usage (Wheel)
    0x15, 0x81,             //     Logical Minimum (-127)
    0x25, 0x7F,             //     Logical Maximum (127)
    0x75, 0x08,             //     Report Size (8)
    0x95, 0x01,             //     Report Count (1)
    0x81, 0x06,             //     Input (Data,Var,Rel)
    0xC0,                   //   End Collection
    0xC0                    // End Collection
};

// --- USB HID tablet: device state ------------------------------------------
int usbTabletAddress = 0;        // address assigned by SET_ADDRESS
int usbTabletPendingAddr = -1;   // takes effect after the status stage completes
int usbTabletConfigured = 0;

// Whether the tablet is allowed to OWN the pointer. NOW ON BY DEFAULT; set
// LOCALHOST_USB_TABLET=0 to go back to the relative PS/2 mouse.
//
// This was off because "report delivery still stalls: the stream runs while the
// pointer is stationary and stops at the first real movement, with USBSTS left
// unacknowledged" -- and since a configured tablet suppresses the PS/2 mouse, a
// stalled tablet would leave the guest with NO pointer at all, strictly worse
// than the relative mouse that already works. That was the right call at the
// time.
//
// Re-measured 2026-08-30 and the stall does not reproduce. Driven through five
// positions (four corners plus centre, 20 movement reports):
//   reports climbed 224,880 -> 228,137 with no pause, moves 1709 -> 1725
//   the guest cursor tracked every point, e.g. requested client (600,80) landed
//   at (604,88) and (100,380) landed at (104,390)
// The residual +4/+8 is the arrow's hotspot, not error: it is CONSTANT across
// 25% and 75% of the client area, so the scale is exact and only the glyph
// origin differs. The stall was presumably fixed by the interrupt work done
// since that comment was written.
//
// Why this matters: PS/2 is a RELATIVE device -- it can only say "moved by this
// much", never "the pointer is now here" -- so the host and guest cursors drift
// apart and never re-converge, which is the pointer mismatch users actually hit.
int g_tabletEnabled = 1;
unsigned char usbTabletSetup[8];         // most recent SETUP packet
unsigned char usbTabletReplyBuf[128];    // staged response for the data stage
UINT32 usbTabletReplyLen = 0, usbTabletReplyPos = 0;
long g_usbSetupPackets = 0, g_usbDescriptorReads = 0, g_usbStalls = 0, g_usbReportsSent = 0;
long g_usbShortReports = 0;   // reports truncated because the qTD asked for < 6 bytes
long g_usbReportLogged = 0;

// Absolute pointer state, fed by the window (0..32767 in both axes).
volatile LONG g_tabletX = 16384, g_tabletY = 16384;
volatile LONG g_tabletButtons = 0, g_tabletWheel = 0;
volatile LONG g_tabletDirty = 0;

// U110: measure the position stream itself rather than inferring from rates.
// "Still skips" can mean two very different things and the fix differs:
//   a long GAP between position changes  -> we are not sampling often enough,
//                                           or something stalls the poll
//   a large JUMP per change              -> positions are being lost upstream,
//                                           and a higher rate will not help
// Recorded per heartbeat window so a burst of movement is not averaged away.
// U112: interval between HID reports actually DELIVERED on the interrupt
// endpoint. This is the discriminator the loop measurement pointed to.
//
// The pointer freezes for 260ms-1.5s while the main loop never stalls, so the
// break is downstream. Two very different causes, and this separates them:
//   report gaps ALSO long  -> the guest stopped polling, or the EHCI path
//                             stopped completing qTDs. Fix the transfer side.
//   report gaps STAY SHORT -> we poll and deliver fine, but the POSITION we
//                             report is not changing -- so the fault is in the
//                             sampling (GetCursorPos, or the guard that ignores
//                             the pointer when it is outside the client area),
//                             not in USB at all.
// Declared HERE, beside the other tablet counters, because the report path that
// updates them sits earlier in the file than usbServiceSchedules does.
double g_repMaxGapMs = 0.0;
long g_repStalls50 = 0, g_repStalls200 = 0, g_repCount = 0;
LARGE_INTEGER g_repLast;

double g_tabMaxGapMs = 0.0;      // worst interval between two CHANGED positions
double g_tabSumGapMs = 0.0;      // for the mean
long   g_tabChanges = 0;         // reports whose coordinates actually differed
long   g_tabMaxJump = 0;         // worst single-step distance, in 0..32767 units
long   g_tabSumJump = 0;
LARGE_INTEGER g_tabLastChange;
volatile LONG g_tabletMoves = 0;

static UINT32 usbMin32(UINT32 a, UINT32 b) { return a < b ? a : b; }

// Builds a USB string descriptor (UTF-16LE) in place.
static UINT32 usbMakeStringDesc(unsigned char *out, const char *ascii) {
    UINT32 n = 0, i;
    UINT32 len = 0;
    while (ascii[len]) len++;
    out[0] = (unsigned char)(2 + len * 2);
    out[1] = 0x03;
    n = 2;
    for (i = 0; i < len; i++) { out[n++] = (unsigned char)ascii[i]; out[n++] = 0; }
    return n;
}

// Handles a control request on endpoint 0. Returns the number of bytes staged
// into usbTabletReplyBuf for the data stage, or -1 to STALL.
int usbTabletHandleControl(const unsigned char *setup) {
    unsigned char bmRequestType = setup[0];
    unsigned char bRequest = setup[1];
    UINT16 wValue = (UINT16)(setup[2] | (setup[3] << 8));
    UINT16 wIndex = (UINT16)(setup[4] | (setup[5] << 8));
    UINT16 wLength = (UINT16)(setup[6] | (setup[7] << 8));

    usbTabletReplyLen = 0;
    usbTabletReplyPos = 0;

    // Log the actual conversation. Enumeration stalls are essentially impossible
    // to diagnose from aggregate counters -- "stalls=1" says nothing about WHICH
    // request the guest gave up on -- and guessing cost a rebuild already.
    static int usbSetupLogged = 0;
    if (usbSetupLogged < 60) {
        usbSetupLogged++;
        printf("[usb-setup #%d] bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%u\n",
               usbSetupLogged, bmRequestType, bRequest, wValue, wIndex, wLength);
        fflush(stdout);
    }

    // Standard device requests
    if ((bmRequestType & 0x60) == 0x00) {
        switch (bRequest) {
            case 0x06: { // GET_DESCRIPTOR
                unsigned char type = (unsigned char)(wValue >> 8);
                UINT32 n = 0;
                g_usbDescriptorReads++;
                if (type == 0x01) {                    // DEVICE
                    n = sizeof(usbTabletDeviceDesc);
                    memcpy(usbTabletReplyBuf, usbTabletDeviceDesc, n);
                } else if (type == 0x02) {             // CONFIGURATION
                    n = sizeof(usbTabletConfigDesc);
                    memcpy(usbTabletReplyBuf, usbTabletConfigDesc, n);
                } else if (type == 0x03) {             // STRING
                    unsigned char idx = (unsigned char)(wValue & 0xFF);
                    if (idx == 0) {                    // supported languages
                        usbTabletReplyBuf[0] = 4; usbTabletReplyBuf[1] = 0x03;
                        usbTabletReplyBuf[2] = 0x09; usbTabletReplyBuf[3] = 0x04; // en-US
                        n = 4;
                    } else if (idx == 1) {
                        n = usbMakeStringDesc(usbTabletReplyBuf, "LocalHost");
                    } else if (idx == 2) {
                        n = usbMakeStringDesc(usbTabletReplyBuf, "LocalHost USB Tablet");
                    } else {
                        g_usbStalls++;
                        return -1;
                    }
                } else if (type == 0x22) {             // HID REPORT descriptor
                    n = sizeof(usbTabletReportDesc);
                    memcpy(usbTabletReplyBuf, usbTabletReportDesc, n);
                } else if (type == 0x21) {             // HID descriptor alone
                    n = 9;
                    memcpy(usbTabletReplyBuf, usbTabletConfigDesc + 18, 9);
                } else if (type == 0x06) {             // DEVICE_QUALIFIER
                    // Required, not optional: we declare bcdUSB 2.00 and operate
                    // at high speed, and a high-speed-capable device MUST answer
                    // this (it describes how the device would look at its OTHER
                    // speed). Stalling it is only correct for a full-speed-only
                    // device -- doing so here cost one STALL during enumeration
                    // and Windows never went on to SET_CONFIGURATION.
                    usbTabletReplyBuf[0] = 10;         // bLength
                    usbTabletReplyBuf[1] = 0x06;       // bDescriptorType
                    usbTabletReplyBuf[2] = 0x00;       // bcdUSB 2.00
                    usbTabletReplyBuf[3] = 0x02;
                    usbTabletReplyBuf[4] = 0x00;       // bDeviceClass
                    usbTabletReplyBuf[5] = 0x00;       // bDeviceSubClass
                    usbTabletReplyBuf[6] = 0x00;       // bDeviceProtocol
                    usbTabletReplyBuf[7] = 64;         // bMaxPacketSize0
                    usbTabletReplyBuf[8] = 1;          // bNumConfigurations
                    usbTabletReplyBuf[9] = 0;          // bReserved
                    n = 10;
                } else if (type == 0x07) {             // OTHER_SPEED_CONFIGURATION
                    // Same layout as our configuration, retyped.
                    n = sizeof(usbTabletConfigDesc);
                    memcpy(usbTabletReplyBuf, usbTabletConfigDesc, n);
                    usbTabletReplyBuf[1] = 0x07;
                } else {
                    g_usbStalls++;
                    return -1;
                }
                usbTabletReplyLen = usbMin32(n, wLength);
                return (int)usbTabletReplyLen;
            }
            case 0x05: // SET_ADDRESS -- applies after the status stage
                usbTabletPendingAddr = wValue & 0x7F;
                return 0;
            case 0x09: // SET_CONFIGURATION
                usbTabletConfigured = (wValue != 0);
                return 0;
            case 0x08: // GET_CONFIGURATION
                usbTabletReplyBuf[0] = (unsigned char)usbTabletConfigured;
                usbTabletReplyLen = usbMin32(1, wLength);
                return (int)usbTabletReplyLen;
            case 0x00: // GET_STATUS
                usbTabletReplyBuf[0] = 0x01; // self-powered
                usbTabletReplyBuf[1] = 0x00;
                usbTabletReplyLen = usbMin32(2, wLength);
                return (int)usbTabletReplyLen;
            case 0x01: case 0x03: // CLEAR_FEATURE / SET_FEATURE
                return 0;
            case 0x0A: // GET_INTERFACE
                usbTabletReplyBuf[0] = 0;
                usbTabletReplyLen = usbMin32(1, wLength);
                return (int)usbTabletReplyLen;
            case 0x0B: // SET_INTERFACE
                return 0;
            default:
                g_usbStalls++;
                return -1;
        }
    }

    // Class (HID) requests
    if ((bmRequestType & 0x60) == 0x20) {
        switch (bRequest) {
            case 0x0A: // SET_IDLE
            case 0x0B: // SET_PROTOCOL
            case 0x09: // SET_REPORT
                return 0;
            case 0x01: { // GET_REPORT -- answer with the current position
                UINT32 n = 6;
                usbTabletReplyBuf[0] = (unsigned char)g_tabletButtons;
                usbTabletReplyBuf[1] = (unsigned char)(g_tabletX & 0xFF);
                usbTabletReplyBuf[2] = (unsigned char)((g_tabletX >> 8) & 0xFF);
                usbTabletReplyBuf[3] = (unsigned char)(g_tabletY & 0xFF);
                usbTabletReplyBuf[4] = (unsigned char)((g_tabletY >> 8) & 0xFF);
                usbTabletReplyBuf[5] = 0;
                usbTabletReplyLen = usbMin32(n, wLength);
                return (int)usbTabletReplyLen;
            }
            case 0x02: // GET_IDLE
                usbTabletReplyBuf[0] = 0;
                usbTabletReplyLen = usbMin32(1, wLength);
                return (int)usbTabletReplyLen;
            case 0x03: // GET_PROTOCOL
                usbTabletReplyBuf[0] = 1; // report protocol
                usbTabletReplyLen = usbMin32(1, wLength);
                return (int)usbTabletReplyLen;
            default:
                g_usbStalls++;
                return -1;
        }
    }

    g_usbStalls++;
    return -1;
}

// --- EHCI schedule execution ------------------------------------------------
static UINT32 ehciMemRead32(UINT64 gpa) {
    if (!guestMemory || gpa + 4 > (UINT64)guestMemSize) return 0;
    return *(UINT32 *)((unsigned char *)guestMemory + gpa);
}
static void ehciMemWrite32(UINT64 gpa, UINT32 v) {
    if (!guestMemory || gpa + 4 > (UINT64)guestMemSize) return;
    *(UINT32 *)((unsigned char *)guestMemory + gpa) = v;
}

// qTD buffers are up to five 4KB pages; only pointer 0 carries a byte offset.
// Transfers here are tiny (<= 64 bytes) but can still straddle a page boundary,
// so walk the pages properly rather than assuming buffer 0 covers it.
static UINT32 ehciQtdCopy(UINT64 qtdGpa, UINT32 offset, unsigned char *hostBuf,
                          UINT32 len, int toGuest) {
    UINT32 done = 0;
    while (done < len) {
        UINT32 cur = offset + done;
        UINT32 page = cur >> 12;
        if (page > 4) break;
        UINT32 bufPtr = ehciMemRead32(qtdGpa + 0x0C + page * 4);
        UINT32 pageOff = (page == 0) ? (bufPtr & 0xFFF) + cur : (cur & 0xFFF);
        UINT64 gpa = (UINT64)(bufPtr & ~0xFFFu) + pageOff;
        UINT32 chunk = 0x1000 - (UINT32)(gpa & 0xFFF);
        if (chunk > len - done) chunk = len - done;
        if (!guestMemory || gpa + chunk > (UINT64)guestMemSize) break;
        unsigned char *p = (unsigned char *)guestMemory + gpa;
        if (toGuest) memcpy(p, hostBuf + done, chunk);
        else         memcpy(hostBuf + done, p, chunk);
        done += chunk;
    }
    return done;
}

// Walks the async (control/bulk) schedule and executes any active qTDs against
// our virtual tablet. Called from the run loop rather than on a register write,
// because a real HC runs the schedule continuously and USBPORT queues transfers
// without touching a doorbell.
// Executes any active qTDs queued on one queue head. Shared by both schedules:
// control/bulk arrive on the async list, HID interrupt transfers on the periodic
// list, and the qTD mechanics are identical either way. Returns 1 if a completion
// interrupt should be raised.
static int ehciRunQueueHead(UINT64 qh) {
    int raisedInterrupt = 0;
    {
        UINT32 epChar = ehciMemRead32(qh + 0x04);
        UINT32 devAddr = epChar & 0x7F;
        UINT32 endpt = (epChar >> 8) & 0xF;

        // Only our device answers, and address 0 stops being us the moment
        // SET_ADDRESS lands. Treating 0 as "always ours" was wrong and is what
        // broke report delivery: USBPORT keeps EMPTY PLACEHOLDER queue heads in
        // the periodic list with devAddr=0, ep=0, mps=0, and we happily adopted
        // one of those as the tablet. Caught by dumping the QH at the stall:
        //   qh=0x585FD000 devAddr=0 ep=0 mps=0 overlayTok=0x40 (Halted=1)
        // So we were servicing a dummy queue -- inflating the report counter to
        // ~300/sec, even marking it Halted -- while the real interrupt endpoint
        // was never serviced at all. Hence "works briefly, then the pointer dies".
        //
        // A placeholder also has a zero max-packet-size, which no real endpoint
        // has, so reject that too rather than relying on the address alone.
        UINT32 maxPacket = (epChar >> 16) & 0x7FF;
        int forUs = (usbTabletAddress == 0)
                        ? (devAddr == 0 && maxPacket != 0)
                        : ((int)devAddr == (UINT32)usbTabletAddress && maxPacket != 0);

        UINT64 qtd = ehciMemRead32(qh + 0x10) & ~0x1Fu;

        // NOTE: an "append detection" pass used to live here -- if the overlay
        // said the queue was drained it followed the Current qTD Pointer's next
        // link to pick up anything the driver had appended. It did not fix the
        // stall it was written for, and it can re-adopt a qTD that was already
        // retired, servicing it over and over. That inflated the report rate to
        // ~300/sec against an endpoint whose bInterval only asks for ~15/sec,
        // which is almost certainly what drowned the guest's HID queue. Removed;
        // if queue-append handling is genuinely needed it has to key off the
        // qTD's Active bit, not merely off the pointer being non-terminal.
        int qtdGuard = 0;
        while (forUs && qtd && !(ehciMemRead32(qh + 0x10) & 0x1u) && qtdGuard < 16) {
            qtdGuard++;
            UINT32 token = ehciMemRead32(qtd + 0x08);
            if (!(token & 0x80u)) break;                 // not Active -- nothing to do
            UINT32 pid = (token >> 8) & 0x3;
            UINT32 total = (token >> 16) & 0x7FFF;
            UINT32 moved = 0;

            if (pid == 2) {                              // SETUP
                unsigned char setup[8];
                ehciQtdCopy(qtd, 0, setup, 8, 0);
                memcpy(usbTabletSetup, setup, 8);
                g_usbSetupPackets++;
                int r = usbTabletHandleControl(setup);
                if (r < 0) {
                    // STALL: halt the qTD so the driver sees the error, exactly
                    // as hardware would, instead of silently completing.
                    token &= ~0x80u;
                    token |= 0x40u;
                    ehciMemWrite32(qtd + 0x08, token);
                    ehciMemWrite32(qh + 0x18, token);
                    // ...and RAISE THE ERROR INTERRUPT. A halted qTD is still a
                    // completed transfer as far as the driver is concerned, and
                    // it only inspects the queue from its ISR. Breaking out here
                    // without signalling left Windows waiting forever on a
                    // perfectly ordinary protocol stall -- measured: enumeration
                    // stopped dead at the Microsoft OS string descriptor (index
                    // 0xEE), which every device is entitled to stall.
                    ehciUsbSts |= 0x2u;   // USBERRINT
                    if (token & 0x8000u) ehciUsbSts |= 0x1u;
                    raisedInterrupt = 1;
                    break;
                }
                moved = total;
            } else if (pid == 1) {                       // IN (device -> host)
                if (endpt == 0) {
                    UINT32 avail = usbTabletReplyLen - usbTabletReplyPos;
                    UINT32 n = usbMin32(avail, total);
                    if (n) {
                        ehciQtdCopy(qtd, 0, usbTabletReplyBuf + usbTabletReplyPos, n, 1);
                        usbTabletReplyPos += n;
                    }
                    moved = n;
                } else {
                    // Interrupt IN on EP1 -- the HID report itself.
                    //
                    // Stay silent unless the tablet owns the pointer. Delivering
                    // reports while PS/2 is also live would move the guest cursor
                    // twice per motion.
                    if (!g_tabletEnabled) break;
                    // NAK-when-idle is what real hardware does, but it does not
                    // work against this stack as modelled: leaving the qTD Active
                    // retires no IOC, so USBPORT gets no completion, never runs
                    // its ISR, never clears USBSTS and never re-queues -- measured
                    // as reports frozen at 2 with USBSTS stuck at 1. Completing
                    // every poll (a duplicate report when nothing moved) is less
                    // faithful but is what actually keeps the queue turning, and
                    // duplicate absolute coordinates are idempotent for the guest.
                    // Revisit if the transfer model ever grows real NAK handling.
                    InterlockedExchange(&g_tabletDirty, 0);
                    // Sample the pointer HERE, at report time, instead of trusting
                    // the position cached by the last WM_MOUSEMOVE.
                    //
                    // WM_MOUSEMOVE is COALESCED: Windows keeps only the most recent
                    // position per message pump, so however fast the mouse reports,
                    // the window sees one position per pump -- and the UI thread is
                    // busy doing a full StretchDIBits every frame. Measured 79
                    // moves/sec from a mouse that reports far more often, which is
                    // motion being thrown away before we ever see it. The tablet
                    // then faithfully reports those few widely-spaced positions,
                    // which is exactly what "the cursor skips" looks like.
                    //
                    // GetCursorPos is not coalesced and does not touch the message
                    // queue, so this samples the CURRENT pointer at our own poll
                    // rate regardless of how backed up the UI thread is.
                    if (g_hwnd) {
                        POINT pt;
                        RECT rc;
                        if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt) &&
                            GetClientRect(g_hwnd, &rc)) {
                            RECT dsp;
                            guestDisplayRect(rc.right - rc.left, rc.bottom - rc.top, &dsp);
                            int w = dsp.right - dsp.left, h = dsp.bottom - dsp.top;
                            int ix = pt.x - dsp.left, iy = pt.y - dsp.top;
                            // Only while the pointer is actually over the guest's
                            // display -- otherwise moving away across the host
                            // desktop would keep dragging the guest cursor along.
                            // The letterbox bars count as "away": they are not
                            // part of the guest's screen.
                            if (w > 0 && h > 0 &&
                                ix >= 0 && iy >= 0 && ix < w && iy < h) {
                                InterlockedExchange(&g_tabletX,
                                    (LONG)(((LONGLONG)ix * 32767) / (w - 1 > 0 ? w - 1 : 1)));
                                InterlockedExchange(&g_tabletY,
                                    (LONG)(((LONGLONG)iy * 32767) / (h - 1 > 0 ? h - 1 : 1)));
                            }
                        }
                    }
                    // U112: interval between DELIVERED reports, regardless of
                    // whether the position changed. Compare against the tablet
                    // motion gaps: if reports keep flowing at ~16ms while the
                    // position sits still, USB is fine and the sampling is at
                    // fault.
                    {
                        LARGE_INTEGER rnow;
                        QueryPerformanceCounter(&rnow);
                        if (g_repLast.QuadPart && perfFrequency.QuadPart) {
                            double rg = (double)(rnow.QuadPart - g_repLast.QuadPart)
                                        * 1000.0 / (double)perfFrequency.QuadPart;
                            if (rg > g_repMaxGapMs) g_repMaxGapMs = rg;
                            if (rg > 50.0)  g_repStalls50++;
                            if (rg > 200.0) g_repStalls200++;
                        }
                        g_repLast = rnow;
                        g_repCount++;
                    }
                    // U110: characterise the stream -- gap between changes, and
                    // how far the pointer moved in each one.
                    {
                        static LONG prevX = -1, prevY = -1;
                        LONG nx = g_tabletX, ny = g_tabletY;
                        if (prevX >= 0 && (nx != prevX || ny != prevY)) {
                            LARGE_INTEGER now;
                            LONG dx = nx > prevX ? nx - prevX : prevX - nx;
                            LONG dy = ny > prevY ? ny - prevY : prevY - ny;
                            LONG jump = dx > dy ? dx : dy;
                            QueryPerformanceCounter(&now);
                            if (g_tabLastChange.QuadPart && perfFrequency.QuadPart) {
                                double gap = (double)(now.QuadPart - g_tabLastChange.QuadPart)
                                             * 1000.0 / (double)perfFrequency.QuadPart;
                                if (gap > g_tabMaxGapMs) g_tabMaxGapMs = gap;
                                g_tabSumGapMs += gap;
                            }
                            g_tabLastChange = now;
                            g_tabChanges++;
                            if (jump > g_tabMaxJump) g_tabMaxJump = jump;
                            g_tabSumJump += jump;
                        }
                        prevX = nx; prevY = ny;
                    }
                    unsigned char rep[6];
                    rep[0] = (unsigned char)g_tabletButtons;
                    rep[1] = (unsigned char)(g_tabletX & 0xFF);
                    rep[2] = (unsigned char)((g_tabletX >> 8) & 0xFF);
                    rep[3] = (unsigned char)(g_tabletY & 0xFF);
                    rep[4] = (unsigned char)((g_tabletY >> 8) & 0xFF);
                    rep[5] = (unsigned char)g_tabletWheel;
                    UINT32 n = usbMin32(6, total);
                    // What we actually put on the wire. "reports" alone cannot
                    // distinguish "never delivered" from "delivered but ignored",
                    // and it says nothing about the coordinates -- the whole
                    // question for an ABSOLUTE device. A short qTD is the specific
                    // hazard: if the guest asks for fewer than 6 bytes the report
                    // is truncated mid-coordinate and the cursor cannot track,
                    // which would look identical to "the guest ignores us".
                    if (n < 6) g_usbShortReports++;
                    if (g_usbReportLogged < 12) {
                        g_usbReportLogged++;
                        printf("[usb-tablet] report #%ld: x=%ld y=%ld btn=0x%lX "
                               "qtdLen=%u wrote=%u%s\n",
                               g_usbReportsSent + 1, (long)g_tabletX, (long)g_tabletY,
                               (unsigned long)g_tabletButtons, total, n,
                               n < 6 ? "  *** TRUNCATED ***" : "");
                        fflush(stdout);
                    }
                    ehciQtdCopy(qtd, 0, rep, n, 1);
                    g_usbReportsSent++;
                    moved = n;
                }
            } else {                                     // OUT
                moved = total;
            }

            // SET_ADDRESS takes effect only once its STATUS stage completes. For
            // a control transfer with NO data stage -- which SET_ADDRESS is --
            // that status stage is a zero-length IN, not an OUT. Applying it only
            // on OUT meant the address was never adopted: the device kept
            // answering on 0, the driver gave up and re-reset the port, and
            // enumeration looped (measured: setups=2, portResets=2, addr=0).
            // Keying on "zero-length transfer on endpoint 0" covers both
            // directions and both control-transfer shapes.
            if (endpt == 0 && total == 0 && usbTabletPendingAddr >= 0) {
                usbTabletAddress = usbTabletPendingAddr;
                usbTabletPendingAddr = -1;
                printf("[usb] tablet address set to %d\n", usbTabletAddress);
                fflush(stdout);
            }

            // Retire the qTD: clear Active, report the residual byte count.
            UINT32 residual = (total > moved) ? (total - moved) : 0;
            token &= ~0x80u;
            token = (token & ~(0x7FFFu << 16)) | (residual << 16);
            ehciMemWrite32(qtd + 0x08, token);

            // Deliberately NOT touching the data toggle. Flipping it in the QH
            // overlay (the DTC=0 case, where hardware nominally owns it) was
            // tried and made things worse: reports flowed briefly then froze at
            // 60 while USBSTS stuck at 1, because USBPORT writes that field too
            // and our flip desynchronised it. We do not model toggle-mismatch
            // rejection anywhere, so there is nothing to gain by maintaining it
            // -- leaving the driver's own value intact is both simpler and what
            // actually keeps reports flowing.
            ehciMemWrite32(qh + 0x18, token);            // mirror into the overlay

            if (token & 0x8000u) raisedInterrupt = 1;    // IOC

            // The QH's Current qTD Pointer (0x0C) is deliberately left alone. We
            // used to write it for the append-detection pass, which has since been
            // removed, so the write is vestigial -- and it is a field USBPORT also
            // reads for its own bookkeeping on this circular two-qTD interrupt
            // queue. Scribbling into a structure the driver owns is exactly the
            // class of bug that had us corrupting its placeholder queue heads.
            UINT32 next = ehciMemRead32(qtd + 0x00);
            ehciMemWrite32(qh + 0x10, next);             // advance the overlay
            if (next & 0x1u) break;                      // T bit -- end of chain
            qtd = next & ~0x1Fu;
        }
    }
    return raisedInterrupt;
}

void ehciProcessAsyncSchedule(void) {
    if (!(ehciUsbCmd & 0x1u)) return;        // Run/Stop clear -- HC halted
    if (!(ehciUsbCmd & 0x20u)) return;       // async schedule not enabled
    if (!ehciAsyncBase || !guestMemory) return;

    UINT64 qh = ehciAsyncBase;
    int raisedInterrupt = 0, qhGuard;

    // The async list is circular; bound the walk so a malformed list cannot spin.
    for (qhGuard = 0; qhGuard < 32; qhGuard++) {
        if (ehciRunQueueHead(qh)) raisedInterrupt = 1;
        UINT32 link = ehciMemRead32(qh + 0x00);
        if (link & 0x1u) break;                          // T bit -- list terminates
        UINT64 nextQh = link & ~0x1Fu;
        if (nextQh == ehciAsyncBase || nextQh == 0) break; // wrapped
        qh = nextQh;
    }

    if (raisedInterrupt) ehciUsbSts |= 0x1u;             // USBINT
}

// Walks the periodic frame list, which is where interrupt endpoints live -- so
// this is what actually delivers HID reports. PERIODICLISTBASE points at 1024
// link pointers; bits 2:1 type-tag each one, and 1 = queue head (the only kind we
// can service; iTD/siTD are isochronous, which a HID tablet never uses).
//
// A real controller consumes exactly one frame entry per millisecond. We advance
// FRINDEX ourselves and service that entry, which keeps the driver's notion of
// time roughly honest without pretending to real-time accuracy the rest of this
// emulation does not have either.
void ehciProcessPeriodicSchedule(void) {
    if (!(ehciUsbCmd & 0x1u)) return;        // halted
    if (!(ehciUsbCmd & 0x10u)) return;       // periodic schedule not enabled
    if (!ehciPeriodicBase || !guestMemory) return;

    UINT32 frame = (ehciFrIndex >> 3) & 0x3FF;
    UINT64 entryGpa = (UINT64)ehciPeriodicBase + frame * 4;
    UINT32 link = ehciMemRead32(entryGpa);

    int raisedInterrupt = 0, guard;
    for (guard = 0; guard < 16; guard++) {
        if (link & 0x1u) break;                          // T bit -- nothing here
        UINT32 type = (link >> 1) & 0x3;
        UINT64 node = link & ~0x1Fu;
        if (node == 0) break;
        if (type == 1) {                                 // queue head
            // Only remember a QH that actually belongs to our device, so the
            // stall dump reports the real endpoint rather than a placeholder.
            UINT32 ep = ehciMemRead32(node + 0x04);
            if ((ep & 0x7F) == (UINT32)usbTabletAddress && ((ep >> 16) & 0x7FF) != 0)
                g_lastIntQh = node;
            if (ehciRunQueueHead(node)) raisedInterrupt = 1;
        }
        UINT32 next = ehciMemRead32(node + 0x00);
        if (next == link) break;                         // self-loop
        link = next;
    }

    ehciFrIndex = (ehciFrIndex + 8) & 0x3FFF;            // advance one frame

    if (raisedInterrupt) ehciUsbSts |= 0x1u;             // USBINT
}

// Dumps the interrupt endpoint's queue head and its qTD chain straight out of
// guest memory.
//
// This exists to settle one question that every remaining hypothesis about the
// report stall depends on, and that our own counters cannot answer: when the
// stream freezes, has the driver stopped QUEUEING work, or have we stopped
// SERVICING work it queued? The Active bit in the qTD tokens says which.
//   - an Active qTD sitting here while reports are frozen  => ours to fix
//   - no Active qTD at all                                 => the driver stopped
//                                                             queueing, and the
//                                                             question moves to
//                                                             why it stopped
//                                                             acknowledging USBSTS
// Reading guest memory directly also sidesteps kd entirely, whose ~75MB/s memory
// drain destabilised the last two attempts to observe this.
static void ehciDumpIntQh(const char *why) {
    if (!g_lastIntQh || !guestMemory) return;
    UINT64 qh = g_lastIntQh;
    UINT32 epChar = ehciMemRead32(qh + 0x04);
    UINT32 cur    = ehciMemRead32(qh + 0x0C);
    UINT32 next   = ehciMemRead32(qh + 0x10);
    UINT32 tok    = ehciMemRead32(qh + 0x18);
    printf("[qh-dump/%s] qh=0x%llX devAddr=%u ep=%u mps=%u | cur=0x%08X next=0x%08X overlayTok=0x%08X (Active=%d Halted=%d bytes=%u)\n",
           why, (unsigned long long)qh, epChar & 0x7F, (epChar >> 8) & 0xF,
           (epChar >> 16) & 0x7FF, cur, next, tok,
           (tok & 0x80) ? 1 : 0, (tok & 0x40) ? 1 : 0, (tok >> 16) & 0x7FFF);

    UINT32 link = next;
    int i;
    for (i = 0; i < 4; i++) {
        if (link & 0x1u) { printf("[qh-dump/%s]   qtd[%d]: T bit -- chain ends\n", why, i); break; }
        UINT64 q = link & ~0x1Fu;
        if (!q) { printf("[qh-dump/%s]   qtd[%d]: null pointer\n", why, i); break; }
        UINT32 t = ehciMemRead32(q + 0x08);
        UINT32 n = ehciMemRead32(q + 0x00);
        printf("[qh-dump/%s]   qtd[%d]=0x%llX tok=0x%08X Active=%d Halted=%d PID=%u bytes=%u next=0x%08X\n",
               why, i, (unsigned long long)q, t, (t & 0x80) ? 1 : 0, (t & 0x40) ? 1 : 0,
               (t >> 8) & 0x3, (t >> 16) & 0x7FFF, n);
        link = n;
    }
    fflush(stdout);
}

// Services both USB schedules on a WALL-CLOCK basis, roughly once per frame.
//
// This must NOT be driven by exit count. A real host controller runs off its own
// 1ms frame timer regardless of what the CPU is doing, and tying it to VM exits
// created a deadlock: the guest goes idle precisely when it is waiting for input,
// idling collapses the exit rate to ~64/sec, so the schedules were serviced about
// once a second and the tablet went dead. Measured: 91 pointer updates arrived at
// the window but only 4 reports reached the guest. The pointer worked for a few
// seconds while the guest was still busy after Setup painted, then froze -- no
// input meant no exits meant no polling meant no input.
static LARGE_INTEGER g_usbLastServiceTick;
// U111: main-loop period, sampled where the USB throttle already measures it.
double g_usbSvcMaxGapMs = 0.0;
long g_usbSvcStalls50 = 0, g_usbSvcStalls200 = 0, g_usbSvcCalls = 0;

// U113: exit attribution. 65536 counters is 256KB, allocated once -- worth it
// for an exact port histogram rather than a lossy top-N guess.
long g_exitReasonCount[16];
UINT32 *g_ioPortHist = NULL;
UINT64 g_mmioPage[8];
long g_ehciOffHist[256];   // U114: byte offset within the EHCI register page
long g_mmioPageHits[8];

void usbServiceSchedules(WHV_PARTITION_HANDLE partition) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_usbLastServiceTick.QuadPart != 0) {
        double ms = (double)(now.QuadPart - g_usbLastServiceTick.QuadPart) * 1000.0 /
                    (double)perfFrequency.QuadPart;
        // U111: is the VM LOOP stalling, or is the guest just not polling?
        //
        // The pointer freezes for 260ms-1.5s and then teleports. The average gap
        // between position changes is already correct (15-21ms at 62Hz), so the
        // latency that is actually felt is these stalls, not the configured rate.
        // This function is called once per main-loop iteration, so the interval
        // between calls IS the main loop's period -- if that shows the same
        // hundreds of milliseconds, the loop itself is stopping and the USB side
        // is innocent. Costs nothing: the interval is already computed here for
        // the 1kHz throttle.
        if (ms > g_usbSvcMaxGapMs) g_usbSvcMaxGapMs = ms;
        if (ms > 50.0)  g_usbSvcStalls50++;
        if (ms > 200.0) g_usbSvcStalls200++;
        g_usbSvcCalls++;
        if (ms < 1.0) return;                            // ~1kHz, one USB frame
    }
    g_usbLastServiceTick = now;

    // PS/2 keyboard/mouse interrupts, on the same wall-clock tick.
    //
    // These used to be delivered ONLY from the halted loop, so they fired only
    // while the guest was idle. That is why typing works (you type at an idle
    // guest) but the mouse never initialised: i8042prt resets the aux device
    // during early boot while the guest is busy and never halts, so IRQ12 never
    // fired, the reset timed out, and Windows retried and gave up. Measured: the
    // guest issues 0xD4+0xFF three times back to back with no reads in between,
    // and auxReportingEnabled never leaves 0.
    //
    // Deliberately TIME-THROTTLED. Delivering these from the main loop on every
    // iteration was tried before and flooded the guest badly enough to wedge
    // firmware boot at ~25000 exits. Once per tick, only when a byte is actually
    // waiting, is enough for a device whose real sample rate is 100Hz.
    // GATED ON THE KERNEL BEING UP (g_bpModuleBase). Firmware POLLS the i8042 and
    // does not want these interrupts at all: delivering them during the firmware
    // phase wedged boot dead at exitCount ~20000 in 0.6s, still spinning on the
    // 0x64 status port -- the same failure a previous attempt at main-loop PS/2
    // IRQ delivery hit. Windows is the only consumer that needs them, and by the
    // time it initialises the aux device the kernel has long since been
    // discovered.
    // PS/2 keyboard/mouse interrupts, retried now that the legacy-vector bug is
    // fixed. Every previous attempt at delivering these from the main loop broke
    // boot, and the suspected reason was that while an IOAPIC entry is still
    // pristine injectDeviceIrq fell back to legacy vector 0x09 -- below 0x10,
    // reserved for CPU exceptions, and meaningless to a Windows kernel in APIC
    // mode. ioapicResolveVector now DROPS those instead (g_guestApicMode), so the
    // conditions that broke it no longer exist.
    //
    // The gate used to be g_bpModuleBase alone, and that DISABLED KEYBOARD INPUT
    // FOR THE ENTIRE ISO BOOT. g_bpModuleBase means "we spotted ntoskrnl in guest
    // memory", which never happens when booting the installer: WinPE runs from the
    // boot.wim the firmware loaded into RAM, so there is no kernel image to
    // discover. Windows Setup was therefore fully up, its IOAPIC entry for GSI 1
    // live at vector 0xA0, keystrokes queued and waiting -- and this branch never
    // ran, so only the halted-loop path ever fired. Measured: 33 keys typed,
    // queue depth climbing 38->61 and never draining, kbIrq=4 for the whole run.
    //
    // g_guestApicMode is the honest gate for "a real OS owns interrupt routing":
    // it is set when the guest programs any IOAPIC redirection entry. Combined
    // with ioapicResolveVector -- which delivers only on an entry the guest has
    // actually programmed, and drops a pristine or masked one -- the original
    // hazard is already covered. That hazard was injecting legacy vectors 0x08/
    // 0x09 while firmware polled the i8042; those are refused now regardless.
    g_ps2GateReached++;
    if (!(g_bpModuleBase || g_guestApicMode)) g_ps2GateNoKernel++;
    else if (!guestInterruptsEnabled(partition)) g_ps2GateIfClear++;
    // Reported on its OWN cadence. Folding this into the change-gated input
    // heartbeat meant the only snapshot I ever read was from early boot, before
    // the guest had even initialised the i8042 -- which is exactly the phase where
    // the gate is legitimately closed, so it looked damning and proved nothing.
    {
        static long lastReported = 0;
        if (g_ps2GateReached - lastReported >= 2000) {
            lastReported = g_ps2GateReached;
            printf("[ps2-gate] reached=%ld passed=%ld blockedNoKernelOrApic=%ld "
                   "blockedIFclear=%ld (bpModuleBase=%d apicMode=%d)\n",
                   g_ps2GateReached, g_ps2GatePassed, g_ps2GateNoKernel,
                   g_ps2GateIfClear, g_bpModuleBase ? 1 : 0, g_guestApicMode);
            fflush(stdout);
        }
    }
    // REVERTED to g_bpModuleBase alone. Widening this to g_guestApicMode was
    // meant to fix input on the ISO boot (where ntoskrnl is never discovered
    // because WinPE runs from a RAM-loaded boot.wim) but it was never shown to
    // help -- and it lets PS/2 interrupts through during the FIRMWARE phase,
    // which is precisely what the original comment warned wedges the boot. With
    // it in place Windows loads its kernel and then stalls: screen frozen on the
    // firmware logo for 150s, guest idle, 155k exits and no Setup. Ruled the RTC
    // latch out first by disabling it (LOCALHOST_RTC_LATCH_MS=0) and reproducing
    // the stall, so this is what is left.
    if (g_bpModuleBase && guestInterruptsEnabled(partition)) {
        g_ps2GatePassed++;
        // Delivery from HERE is required, not an optimisation. Input interrupts
        // were once raised only from the HALTED loop, and once Windows settles
        // into polling the ACPI PM timer (lastPort=0xB008, IRQL 0) it never halts
        // -- so keystrokes sat queued forever with kbPending=1 and the guest
        // simply never read them. Typing appeared to work in testing only because
        // the guest was still busy right after painting and hit HLT in between.
        //
        // The RULE for how often to raise them lives in ps2ServiceOutputIrq: one
        // interrupt per byte the output buffer presents. Four other rules were
        // measured before it and all of them failed on the same axis -- too loud
        // (a 20ms re-assert, ~50/sec, wedged the guest at IRQL 15) or too quiet
        // (per-batch arrival counters stranded every reply byte after the first,
        // so the mouse init handshake never completed). See that function.
        int raised = ps2ServiceOutputIrq(partition);

        // On the first PS/2 interrupt of a run, show whether the guest has an ISR
        // connected for these vectors at all -- with the RTC's vector alongside
        // as a known-working control.
        if (raised) {
            static int idtDumped = 0;
            if (!idtDumped) {
                unsigned char kbVec = 0, msVec = 0;
                idtDumped = 1;
                ioapicResolveVector(GSI_KEYBOARD, 0x09, &kbVec);
                ioapicResolveVector(GSI_MOUSE, 0x74, &msVec);
                printf("[idt] --- first PS/2 interrupt: is anything listening? ---\n");
                dumpIdtGate(partition, kbVec, "keyboard");
                dumpIdtGate(partition, msVec, "mouse");
                dumpIdtGate(partition, 0xD1,  "RTC/ctrl");
            }
        }
    }

    ehciProcessAsyncSchedule();
    ehciProcessPeriodicSchedule();

    // Level-triggered: assert while an enabled status bit is set, and let the
    // driver's write-1-to-clear take it away (see the USBSTS handler).
    // Assert on the RISING edge, then re-assert only slowly while the driver has
    // not acknowledged. Servicing at 1kHz and injecting every time produced 16822
    // injections with USBSTS stuck at 1 -- the guest spent its time in our ISR
    // instead of finishing USB init, and never even enabled the periodic schedule
    // (USBCMD=0x00010021: async on, periodic off), so no report could ever flow.
    // A real level-triggered line is asserted and HELD; it does not re-interrupt
    // every frame. This is the same storm U61 had to unpick for AHCI.
    static LARGE_INTEGER lastIrqTick;
    static int irqAsserted = 0;
    int wantIrq = (ehciUsbSts & ehciUsbIntr & 0x3F) != 0;
    if (!wantIrq) {
        irqAsserted = 0;                                 // driver acknowledged
    } else if (guestInterruptsEnabled(partition)) {
        double sinceIrqMs = lastIrqTick.QuadPart
            ? (double)(now.QuadPart - lastIrqTick.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart
            : 1e9;
        // 20ms re-assert, and it is load-bearing. Tested at 2000ms (effectively
        // rising-edge-only, to see whether our own repeated injections were
        // wedging LAPIC delivery): the driver acknowledged exactly ONCE and never
        // even enabled the periodic schedule (USBCMD=0x00010021, reports=0). A
        // level-triggered line has to persist until acknowledged -- if the guest
        // misses the single edge there is nothing to retry -- so re-assertion is
        // required, and over-injection is NOT the cause of the later stall.
        if (!irqAsserted || sinceIrqMs >= 20.0) {
            lastIrqTick = now;
            irqAsserted = 1;
            g_ehciIrqCount++;
            // injectDeviceIrq returns 0 when the guest has MASKED this GSI, in
            // which case nothing was delivered. Counting attempts made "irqs
            // climbing" look like "interrupts arriving" when it may mean the
            // opposite, so count the two separately.
            int delivered = injectDeviceIrq(partition, GSI_EHCI, 0x75);
            if (!delivered) g_ehciIrqDropped++;
        // Is the interrupt actually landing anywhere? USBSTS staying set while we
        // inject thousands of times means the driver's ISR is not running, and the
        // usual reason is the vector: while an IOAPIC entry is still at its
        // power-on default we fall back to a legacy vector nothing has registered
        // a handler for. Bounded, and only interesting for the first few.
        static int ehciIrqLogged = 0;
        if (ehciIrqLogged < 8) {
            ehciIrqLogged++;
            unsigned char v = 0;
            int ok = ioapicResolveVector(GSI_EHCI, 0x75, &v);
            printf("[usb-irq #%d] GSI=%d resolved=%d vector=0x%02X delivered=%d USBSTS=0x%08X USBINTR=0x%08X\n",
                   ehciIrqLogged, GSI_EHCI, ok, v, delivered, ehciUsbSts, ehciUsbIntr);
            fflush(stdout);
            }
        }
    }

    // Report USB state on a TIMER, not from the heartbeat. The heartbeat fires
    // every 5000 exits, and an idle guest produces ~64 exits/sec -- so it prints
    // roughly once every 78 seconds, and its last line can easily predate the
    // input being tested. That stale reading already made a working run look like
    // "moves=0". Idle is the exact condition the tablet has to survive, so its
    // instrumentation cannot be paced by guest activity either.
    {
        static LARGE_INTEGER lastStatus;
        static long lastMoves = -1, lastReports = -1;
        double sinceMs = lastStatus.QuadPart
            ? (double)(now.QuadPart - lastStatus.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart
            : 1e9;
        if (sinceMs >= 3000.0) {
            lastStatus = now;
            // PS/2 mouse health, printed on the same wall-clock timer so it is
            // visible whether or not the tablet is enabled and regardless of how
            // idle the guest is.
            {
                // Also fires when reporting/port state changes, not just on packet
                // counts. Keyed on counts alone the line went stale the moment the
                // mouse stopped moving, and a stale "reporting=0" printed BEFORE
                // the guest sent 0xF4 read exactly like the enable had never
                // happened.
                static long lastAux = -1, lastAuxGated = -1, lastAuxRead = -1;
                static int lastReporting = -1, lastPortEnabled = -1;
                if (g_auxPackets != lastAux || g_auxPacketsGated != lastAuxGated ||
                    g_auxBytesToGuest != lastAuxRead ||
                    auxReportingEnabled != lastReporting || auxPortEnabled != lastPortEnabled) {
                    lastAux = g_auxPackets;
                    lastAuxGated = g_auxPacketsGated;
                    lastAuxRead = g_auxBytesToGuest;
                    lastReporting = auxReportingEnabled;
                    lastPortEnabled = auxPortEnabled;
                    // kbPending is here because the status register gives the
                    // KEYBOARD priority: while its queue is non-empty the guest
                    // never even sees that aux data is waiting. Undrained keyboard
                    // bytes would therefore block mouse initialisation completely,
                    // no matter how the interrupt is delivered.
                    printf("[ps2-input] kbUser=%ld kbRead=%ld kbIrq=%ld masked=%ld drop=%ld skippedBusy=%ld | "
                           "mousePackets=%ld mouseRead=%ld mouseIrq=%ld masked=%ld gated=%ld coalesced=%ld drop=%ld | "
                           "kbPending=%d auxPending=%d (reporting=%d portEnabled=%d rawInput=%d tabletOwns=%d)\n",
                           (long)g_kbUserBytes, g_kbBytesToGuest, g_kbIrqSent, g_kbIrqMasked,
                           g_kbDropped, g_injectSkippedBusy,
                           g_auxPackets, g_auxBytesToGuest, g_auxIrqSent, g_auxIrqMasked,
                           g_auxPacketsGated, g_auxCoalesced, g_auxDropped,
                           kbHasData() ? 1 : 0, auxHasData() ? 1 : 0,
                           auxReportingEnabled, auxPortEnabled,
                           g_rawMouseAvailable, LH_TABLET_OWNS_POINTER ? 1 : 0);
                    // "masked" above conflates two OPPOSITE situations, and which
                    // one it is decides where the bug lives:
                    //   rte == 0x10000  -> entry still at power-on default. The guest
                    //                      has never routed this GSI, so in APIC mode
                    //                      we drop. Means i8042prt never programmed it.
                    //   rte &  0x10000  -> the guest programmed it and then MASKED it,
                    //                      i.e. it knows about the line and refuses it.
                    //   otherwise       -> routed and live; vector is rte & 0xFF.
                    {
                        UINT64 kbRte = ioapicRteFor(GSI_KEYBOARD);
                        UINT64 msRte = ioapicRteFor(GSI_MOUSE);
                        printf("[ps2-route] kbd gsi=%d rte=0x%016llX (%s) | mouse gsi=%d rte=0x%016llX (%s) "
                               "| apicMode=%d legacyDropped=%ld\n",
                               GSI_KEYBOARD, (unsigned long long)kbRte,
                               kbRte == 0x10000ULL ? "PRISTINE -- guest never routed it"
                                 : (kbRte & 0x10000ULL) ? "MASKED by guest" : "live",
                               GSI_MOUSE, (unsigned long long)msRte,
                               msRte == 0x10000ULL ? "PRISTINE -- guest never routed it"
                                 : (msRte & 0x10000ULL) ? "MASKED by guest" : "live",
                               g_guestApicMode, g_legacyVectorDropped);
                    }
                    {
                        long total = g_kbLastReadCount;
                        long have = total < KB_LASTREAD_MAX ? total : KB_LASTREAD_MAX;
                        long i2;
                        printf("[ps2-lastread] last %ld of %ld scancodes the guest consumed:", have, total);
                        for (i2 = total - have; i2 < total; i2++)
                            printf(" %02X", g_kbLastRead[i2 % KB_LASTREAD_MAX]);
                        printf("\n");
                        total = g_kbLastEnqCount;
                        have = total < KB_LASTREAD_MAX ? total : KB_LASTREAD_MAX;
                        printf("[ps2-lastenq]  last %ld of %ld scancodes WE enqueued: ", have, total);
                        for (i2 = total - have; i2 < total; i2++)
                            printf(" %02X", g_kbLastEnq[i2 % KB_LASTREAD_MAX]);
                        printf("\n");
                        total = g_kbVkLogged;
                        have = total < KB_LASTREAD_MAX ? total : KB_LASTREAD_MAX;
                        printf("[ps2-gate] main-loop delivery reached=%ld passed=%ld "
                               "blockedNoKernelOrApic=%ld blockedIFclear=%ld\n",
                               g_ps2GateReached, g_ps2GatePassed,
                               g_ps2GateNoKernel, g_ps2GateIfClear);
                        printf("[ps2-vkring]   last %ld of %ld VKs reaching WndProc:", have, total);
                        for (i2 = total - have; i2 < total; i2++)
                            printf(" %02X", g_kbVkRing[i2 % KB_LASTREAD_MAX]);
                        printf("\n");
                    }
                    fflush(stdout);
                }
            }
            // Interrupt queue health. deferredTpr is the interesting one: it is
            // how often the guest had the relevant level masked, which used to be
            // ignored entirely and is what deadlocked i8042prt at DIRQL.
            {
                // Keyed on QUEUED too, not just delivered. Keying it on delivered
                // alone made the worst possible state -- nothing being delivered at
                // all -- print nothing at all, which is precisely how a bug that
                // silently withheld every interrupt in the machine stayed invisible.
                static long lastQueued = -1, lastDelivered = -1, lastDeferred = -1;
                static long lastCoalesced = -1;
                if (g_irqQueued != lastQueued || g_irqDelivered != lastDelivered ||
                    g_irqDeferredTpr != lastDeferred || g_irqCoalesced != lastCoalesced) {
                    // Delivery FROZEN while the queue backs up is the state worth
                    // catching, so it is reported explicitly rather than left to be
                    // inferred from two numbers that stopped moving.
                    int frozen = (g_irqDelivered == lastDelivered) && g_irqQueueCount > 0;
                    lastQueued = g_irqQueued;
                    lastDelivered = g_irqDelivered;
                    lastDeferred = g_irqDeferredTpr;
                    lastCoalesced = g_irqCoalesced;
                    printf("[irq-queue] pitTicks=%ld queued=%ld delivered=%ld coalesced=%ld deferredTpr=%ld tprReadFail=%ld full=%ld depth=%d apicOk=%ld apicFail=%ld(hr=0x%lX vec=0x%02X)%s\n",
                           g_pitTicksDelivered,
                           g_irqQueued, g_irqDelivered, g_irqCoalesced, g_irqDeferredTpr,
                           g_irqTprReadFail, g_irqQueueFull, g_irqQueueCount,
                           g_reqIrqOk, g_reqIrqFail, (unsigned long)g_reqIrqLastHr, g_reqIrqLastVector,
                           frozen ? "  *** DELIVERY FROZEN ***" : "");
                    // Why we are declining, and what is actually stuck in there.
                    if (frozen) {
                        int qi;
                        printf("[irq-stuck] slotBusy=%ld cleared=%ld ifClear=%ld getFail=%ld lastSlot=0x%llX (pending=%d vector=0x%02X) queued:",
                               g_irqSlotBusy, g_irqSlotCleared, g_irqIfClear, g_irqGetFail,
                               (unsigned long long)g_irqLastSlot,
                               (int)(g_irqLastSlot & 1ULL),
                               (unsigned)((g_irqLastSlot >> 16) & 0xFF));
                        for (qi = 0; qi < g_irqQueueCount; qi++)
                            printf(" 0x%02X(prio%d)", g_irqQueue[qi].vector, g_irqQueue[qi].prio);
                        printf("\n");
                    }
                    fflush(stdout);
                }
            }
            if ((long)g_tabletMoves != lastMoves || g_usbReportsSent != lastReports) {
                // Evaluate the stall condition against the PREVIOUS sample, before
                // overwriting it -- comparing after the update is always false,
                // which is why the first attempt at this dump never fired.
                int stalled = ((long)g_tabletMoves != lastMoves) &&
                              (g_usbReportsSent == lastReports);
                lastMoves = (long)g_tabletMoves;
                lastReports = g_usbReportsSent;
                // Dump the queue exactly when the symptom is present: pointer
                // updates still arriving but the report stream frozen. Catching
                // it in the act is the whole point -- a dump taken while things
                // are healthy says nothing.
                if (stalled && g_tabletEnabled && usbTabletConfigured) {
                    ehciDumpIntQh("STALLED");
                }
                printf("[usb-status] moves=%ld reports=%ld irqs=%ld x=%ld y=%ld buttons=%ld "
                       "USBCMD=0x%08X USBSTS=0x%08X dropped=%ld ack=%ld(last=0x%X) doorbells=%ld\n",
                       lastMoves, lastReports, g_ehciIrqCount,
                       (long)g_tabletX, (long)g_tabletY, (long)g_tabletButtons,
                       ehciUsbCmd, ehciUsbSts, g_ehciIrqDropped,
                       g_ehciUsbStsWrites, g_ehciLastUsbStsWritten, g_ehciDoorbells);
                fflush(stdout);
            }
        }
    }
}

// Decodes accesses to the EHCI register block. Layout per the EHCI 1.0 spec:
// a read-only capability block at BAR+0, then the operational registers at
// BAR+CAPLENGTH.
int ehciHandleMmio(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    UINT64 gpa = exitContext->MemoryAccess.Gpa;
    if (!ehciBarMapped) return 0;
    if (gpa < ehciBarBase || gpa >= (UINT64)ehciBarBase + EHCI_BAR_SIZE) return 0;
    UINT32 off = (UINT32)(gpa - ehciBarBase);

    int isWrite = 0, isImm = 0, regNum = 0, insnTotalLen = 0;
    UINT32 immVal = 0;
    if (!ioapicDecodeMmio(exitContext->MemoryAccess.InstructionBytes,
                          exitContext->MemoryAccess.InstructionByteCount,
                          &isWrite, &isImm, &regNum, &immVal, &insnTotalLen)) {
        static int ehciFailLog = 0;
        if (ehciFailLog++ < 20) {
            printf("[ehci-mmio] undecodable instruction at rip=0x%llX off=0x%X\n",
                   (unsigned long long)exitContext->VpContext.Rip, off);
            fflush(stdout);
        }
        return 0;
    }

    if (isWrite) g_ehciMmioWrites++; else g_ehciMmioReads++;

    UINT32 value = immVal;
    if (isWrite && !isImm) {
        WHV_REGISTER_VALUE srcVal = { 0 };
        WHvGetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &srcVal);
        value = (UINT32)srcVal.Reg64;
    }

    UINT32 result = 0;
    if (off < EHCI_CAPLENGTH) {
        // Capability registers are read-only; writes are simply dropped.
        switch (off & ~3u) {
            case 0x00:
                // CAPLENGTH (byte 0) + HCIVERSION (word at 2) = EHCI 1.0.
                result = EHCI_CAPLENGTH | (0x0100u << 16);
                break;
            case 0x04:
                // HCSPARAMS: 1 port, port power control supported (PPC, bit 4).
                result = 0x00000011u;
                break;
            case 0x08:
                // HCCPARAMS: 32-bit addressing only, no extended capabilities
                // (EECP = 0, so no BIOS/OS handoff dance is advertised).
                result = 0x00000000u;
                break;
            default:
                result = 0; // HCSP-PORTROUTE and reserved space
                break;
        }
    } else {
        UINT32 op = off - EHCI_CAPLENGTH;
        switch (op) {
            case 0x00: // USBCMD
                if (isWrite) {
                    if (value & 0x2u) {
                        // HCRESET: self-clearing, and it returns the controller
                        // to its powered-on state. The port keeps its connect
                        // status because the device is physically still there.
                        ehciUsbCmd = 0;
                        ehciUsbSts = 0x00001000; // HCHalted
                        ehciPeriodicBase = ehciAsyncBase = 0;
                        ehciConfigFlag = 0;
                        ehciFrIndex = 0;
                        value &= ~0x2u;
                    }
                    // Interrupt on Async Advance Doorbell (bit 6). Software rings
                    // this when it unlinks a queue head from the async schedule
                    // and must WAIT for hardware to answer -- the handshake that
                    // tells it the controller has stopped touching the removed QH.
                    // Hardware answers by setting USBSTS.IAA (bit 5) and clearing
                    // the doorbell.
                    //
                    // We never implemented it, so any ring went unanswered and the
                    // driver was left waiting forever. That matches the observed
                    // stall exactly: acknowledgements and report delivery freeze at
                    // the same instant and never resume, while interrupts keep
                    // being delivered (dropped=0) and the guest stays healthy.
                    // USBPORT unlinks queue heads during normal operation, so this
                    // is not an edge case.
                    if (value & 0x40u) {
                        ehciUsbSts |= 0x20u;   // Interrupt on Async Advance
                        value &= ~0x40u;       // doorbell self-clears once answered
                        g_ehciDoorbells++;
                    }
                    ehciUsbCmd = value & ~0x2u;
                    // Run/Stop drives HCHalted, inverted.
                    if (ehciUsbCmd & 0x1u) ehciUsbSts &= ~0x00001000u;
                    else                   ehciUsbSts |= 0x00001000u;
                }
                result = ehciUsbCmd;
                break;
            case 0x04: // USBSTS -- bits 5:0 are write-1-to-clear
                if (isWrite) {
                    // Every acknowledgement comes through here, so this counter is
                    // a direct measure of "the driver's ISR ran". Reports flowed to
                    // ~1280 before freezing, so the ISR clearly ran early; the
                    // question is whether it stops at the same moment the reports
                    // do. Uncapped, per the standing rule in this file.
                    g_ehciUsbStsWrites++;
                    g_ehciLastUsbStsWritten = value;
                    ehciUsbSts &= ~(value & 0x3Fu);
                }
                result = ehciUsbSts;
                break;
            case 0x08: if (isWrite) ehciUsbIntr = value & 0x3F; result = ehciUsbIntr; break;
            case 0x0C: if (isWrite) ehciFrIndex = value & 0x3FFF; result = ehciFrIndex; break;
            case 0x10: if (isWrite) ehciCtrlDsSegment = value; result = ehciCtrlDsSegment; break;
            case 0x14: if (isWrite) ehciPeriodicBase = value & ~0xFFFu; result = ehciPeriodicBase; break;
            case 0x18: if (isWrite) ehciAsyncBase = value & ~0x1Fu; result = ehciAsyncBase; break;
            case 0x40: if (isWrite) ehciConfigFlag = value & 0x1; result = ehciConfigFlag; break;
            case 0x44: { // PORTSC[0]
                if (isWrite) {
                    // CSC (bit1) and PEC (bit3) are write-1-to-clear; preserve
                    // them unless the driver is explicitly acknowledging.
                    UINT32 rw1c = value & 0x0000002Au;      // CSC | PEC | OCC
                    UINT32 keep = ehciPortSc & ~0x0000002Au;
                    UINT32 next = (value & ~0x0000002Au) | (keep & 0x0000002Au);
                    next &= ~rw1c;
                    if (value & 0x100u) {
                        // Port Reset asserted. A real controller drives reset
                        // while the bit is set; we complete it immediately and
                        // report the outcome for a HIGH-SPEED device: PR clears
                        // and PED (bit2) comes up. A full/low-speed device would
                        // instead clear PED and hand the port to a companion,
                        // which is exactly what we are avoiding by being HS.
                        next &= ~0x100u;
                        next |= 0x4u;
                        g_ehciPortResets++;
                    }
                    // CCS and PP are ours to report, not the driver's to set.
                    next = (next & ~0x1u) | 0x1u;   // still connected
                    next |= 0x1000u;                // still powered
                    ehciPortSc = next;
                }
                result = ehciPortSc;
                break;
            }
            default:
                result = 0;
                break;
        }
    }

    if (!isWrite) {
        WHV_REGISTER_VALUE dstVal = { 0 };
        dstVal.Reg64 = result;
        WHvSetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &dstVal);
    }
    // InstructionLength is not populated for MMIO exits, so advance RIP by the
    // length our own decode determined (same as the IOAPIC/ABAR paths).
    WHV_REGISTER_NAME ehciRipName = WHvX64RegisterRip;
    WHV_REGISTER_VALUE ehciRipVal = { 0 };
    ehciRipVal.Reg64 = exitContext->VpContext.Rip + (UINT64)insnTotalLen;
    WHvSetVirtualProcessorRegisters(partition, 0, &ehciRipName, 1, &ehciRipVal);
    return 1;
}

// Implements the real PCI BAR-sizing protocol for BAR5 (offset 0x24) --
// write 0xFFFFFFFF to probe the size (read back a size mask), write a real
// aligned address to program it. On the first real address write, lazily
// allocates and maps the ABAR backing buffer at that GPA. Every other
// register on this device uses the generic per-function byte array (see
// pciHandleConfigAccess); this one needs real protocol behavior because,
// unlike the host bridge/PIIX3/PIIX4 (which have no real BARs at all), the
// AHCI driver needs to actually find this device's register block.
void ahciHandleBar5Access(WHV_PARTITION_HANDLE partition, WHV_X64_IO_PORT_ACCESS_CONTEXT *io,
                           UINT32 baseOffset, UINT32 accessSize, UINT64 *rax) {
    if (io->AccessInfo.IsWrite) {
        if (baseOffset == 0x24 && accessSize >= 4) {
            UINT32 written = (UINT32)io->Rax;
            if (written == 0xFFFFFFFF) {
                ahciBar5Sizing = 1;
            } else {
                ahciBar5Sizing = 0;
                UINT32 newBase = written & ~(UINT32)(AHCI_BAR_SIZE - 1);
                // RE-BASING. The guard below is `!ahciAbarMapped`, so once the
                // firmware has programmed BAR5 every later write was silently
                // dropped and ahciAbarBase stayed at the firmware's address. That
                // is fatal with a real OS: Windows PnP re-assigns PCI BARs as a
                // matter of course, and when it moves this one we carried on
                // trapping the old GPA, so every storahci access landed in
                // unmapped space. Measured symptom: ABAR touches fw=36212
                // kernel=0 -- the Windows kernel never reached this controller
                // once, so it never bound to it, so Windows Setup reported "we
                // couldn't find any drives".
                //
                // Honour the move. The backing buffer is deliberately never mapped
                // into the guest (U58, so register accesses fault to us and
                // write-1-to-clear behaves like hardware), which means re-basing
                // is just pointing the trap window at the new address.
                // DEFAULT OFF (LOCALHOST_AHCI_REBASE=1 enables). Following the
                // re-base is CORRECT and it does what it claims -- storahci binds
                // and reads the disk for the first time. But it also makes the
                // guest depend on AHCI completion interrupts, and those go through
                // the raw injection slot, which jams (20+ unwedge events a run).
                // Measured: with re-basing on, Windows Setup never reaches its GUI
                // at all, where without it Setup renders in ~30s. Routing device
                // vectors through the APIC instead (LOCALHOST_APIC_DEVICE=1) moves
                // more data -- 4613 sectors vs 2817 -- but still does not get there.
                //
                // So: off by default, because "boots to Setup with no disk" beats
                // "does not boot". Turn it on together with a fix for AHCI
                // interrupt delivery, which is the actual remaining blocker.
                if (g_ahciAllowRebase && newBase != 0 && ahciAbarMapped && newBase != ahciAbarBase) {
                    printf("[ahci-bar] guest RE-BASED ABAR: 0x%08X -> 0x%08X (kernelUp=%d)\n",
                           ahciAbarBase, newBase, g_bpModuleBase ? 1 : 0);
                    fflush(stdout);
                    ahciAbarBase = newBase;
                }
                if (newBase != 0 && !ahciAbarMapped) {
                    ahciAbarBase = newBase;
                    ahciAbarMemory = VirtualAlloc(NULL, AHCI_BAR_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                    if (ahciAbarMemory) {
                        ahciInitAbarRegisters((unsigned char *)ahciAbarMemory);
                        // INSTRUMENTATION (2026-07-18, boot-regression diag): the
                        // HRESULT of this map was previously ignored. When SeaBIOS
                        // places BAR5 at 0xFEBFC000 (sub-4GB MMIO hole near the
                        // APIC/IOAPIC region) instead of 0xC0000000, the disk is
                        // mis-probed as ATAPI and boot fails -- hypothesis is that
                        // this map silently fails at that GPA. Log the result to
                        // confirm/deny. No behavioral change (still sets the flag
                        // and prints as before) -- purely adds the HRESULT to the log.
                        // U58: deliberately NOT mapped into the guest any more.
                        // Leaving the GPA unmapped makes every register access
                        // fault out to ahciHandleAbarMmio, which is what lets
                        // write-1-to-clear and read-only semantics behave like
                        // hardware. Mapping it as RAM is what left PxIS stuck set
                        // after storahci acknowledged an interrupt. The buffer
                        // stays as our backing store; only the guest mapping goes.
                        // Set U58_TRAP_ABAR to 0 to restore the old RAM mapping.
#define U58_TRAP_ABAR 1
#if U58_TRAP_ABAR
                        ahciAbarMapped = 1;
                        printf("[ahci] ABAR at 0x%X -- trapped MMIO (unmapped, decoded per access)\n",
                               ahciAbarBase);
                        fflush(stdout);
#else
                        HRESULT abarMapHr = WHvMapGpaRange(partition, ahciAbarMemory, ahciAbarBase, AHCI_BAR_SIZE,
                                       WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite);
                        ahciAbarMapped = 1;
                        printf("[ahci] ABAR mapped at 0x%X (WHvMapGpaRange hr=0x%lX %s)\n",
                               ahciAbarBase, (unsigned long)abarMapHr,
                               SUCCEEDED(abarMapHr) ? "OK" : "FAILED");
                        fflush(stdout);
#endif
                    }
                }
            }
        }
        // Sub-dword writes to a BAR aren't a pattern real firmware uses; ignored.
    } else {
        UINT32 currentValue = ahciBar5Sizing ? ~(UINT32)(AHCI_BAR_SIZE - 1) : ahciAbarBase;
        UINT32 shift = (baseOffset - 0x24) * 8;
        UINT64 mask = (accessSize >= 4) ? 0xFFFFFFFFULL : (accessSize >= 2 ? 0xFFFFULL : 0xFFULL);
        *rax = (currentValue >> shift) & mask;
    }
}

// Fills the RTL8139 register file with real chip power-on-reset defaults:
// MAC address (IDR0-5, normally EEPROM-loaded on real hardware -- we just
// preload it directly), and BMSR reporting link-up (bit 2) so a driver
// polling for link doesn't spin forever waiting for autonegotiation we
// don't emulate. Called both at startup and whenever the guest issues a
// software reset via CR (see rtl8139HandleIoAccess).
// The chip's hardware version, reported in TCR bits 31:26 (+ 24:22). Every
// Realtek driver reads this to decide WHICH chip it is talking to, and refuses to
// start one it does not recognise. We reported 0 -- not a chip -- which is why
// Windows loaded the inbox "RTL8139/810x Family" driver and then failed it with a
// yellow bang: the driver bound on the PCI ID and then gave up on the hardware.
// Overridable so the revision can be swept without a rebuild: the driver reads
// this register, twice, and then stops -- it is rejecting the revision, and which
// ones it accepts is not documented anywhere we can consult.
// LOCALHOST_RTL_VERID=0x78000000 etc.
//
// 0x74800000 is RTL8139C+, and it is what QEMU's rtl8139 reports:
//     HW_REVID(1, 1, 1, 0, 1, 1, 0)   /* bits 30,29,28,26,23 */
// That matters more than any other candidate, because QEMU's model is the one
// configuration known to bind AND START this same inbox driver, so it is the
// reference to match rather than guess against.
//
// It is also the value the earlier sweep structurally COULD NOT have found. That
// sweep enumerated Linux's rtl_chip_info table from 8139too -- but 8139too does
// not drive C+ chips at all (Linux handles those in the separate 8139cp driver),
// so the C+ ID is absent from the table, and "all eight IDs rejected" only ever
// meant "all eight NON-C+ IDs rejected". Pairs with PCI revision 0x20 below;
// the two halves of the identity go together on real silicon and QEMU sets both.
UINT32 g_rtlHwVerId = 0x74800000U;
#define RTL8139_TCR_HWVERID g_rtlHwVerId

// --- 93C46 serial EEPROM -----------------------------------------------------
//
// A real RTL8139 keeps its MAC address (and its PCI IDs) in a little 64x16-bit
// serial EEPROM, and the driver BIT-BANGS it through CR9346 (0x50) to read them.
// That is not optional: with 0x50 as plain storage the driver clocks out a
// garbage address and never gets a usable MAC.
//
// This is exactly where the driver was observed to be stuck once BAR1 let it get
// this far -- 258 register accesses, every one of them offset 0x50, cycling
//     0x88 (mode=10, EECS)  ->  0x8C (+EESK)  ->  read back for EEDO
// which is the 93C46 clocking protocol and nothing else.
//
// CR9346 bit layout: 7:6 = EEM1:EEM0 (mode; 10 = EEPROM access), 3 = EECS
// (chip select), 2 = EESK (clock), 1 = EEDI (data in), 0 = EEDO (data out,
// device-driven -- the bit the driver is actually reading).
typedef struct {
    UINT16 contents[64];
    int cs, sk;          // last EECS / EESK levels, for edge detection
    UINT32 shiftIn;      // command bits clocked in from EEDI
    int shiftInCount;
    UINT16 shiftOut;     // data being clocked out on EEDO
    int shiftOutCount;
    int eedo;            // current data-out level
} Rtl8139Eeprom;
Rtl8139Eeprom g_rtlEeprom;

// Contents match QEMU's model: signature, the PCI IDs mirrored, and the MAC in
// words 7-9 (little-endian byte pairs) -- the layout every Realtek driver expects.
void rtl8139EepromInit(void) {
    memset(&g_rtlEeprom, 0, sizeof(g_rtlEeprom));
    g_rtlEeprom.contents[0] = 0x8129;                 // 93C46 signature
    g_rtlEeprom.contents[1] = 0x10EC;                 // vendor
    g_rtlEeprom.contents[2] = 0x8139;                 // device
    g_rtlEeprom.contents[7] = (UINT16)(rtl8139Mac[0] | (rtl8139Mac[1] << 8));
    g_rtlEeprom.contents[8] = (UINT16)(rtl8139Mac[2] | (rtl8139Mac[3] << 8));
    g_rtlEeprom.contents[9] = (UINT16)(rtl8139Mac[4] | (rtl8139Mac[5] << 8));
    g_rtlEeprom.contents[10] = 0x10EC;                // subsystem vendor
    g_rtlEeprom.contents[11] = 0x8139;                // subsystem device
    g_rtlEeprom.eedo = 1;
}

// Drives the state machine from a CR9346 write and returns the byte to store,
// with bit 0 replaced by the EEPROM's data-out so a subsequent read of 0x50 sees
// it. Commands are start(1) + opcode(2) + address(6) = 9 bits for a 93C46; on a
// READ (opcode 10) the addressed word is then clocked out MSB-first.
unsigned char rtl8139Eeprom9346Write(unsigned char val) {
    int mode = (val >> 6) & 3;
    int cs   = (val >> 3) & 1;
    int sk   = (val >> 2) & 1;
    int di   = (val >> 1) & 1;

    if (mode != 2) {              // not EEPROM-access mode: leave the chip alone
        g_rtlEeprom.cs = 0; g_rtlEeprom.sk = 0;
        return (unsigned char)((val & 0xFE) | (g_rtlEeprom.eedo & 1));
    }
    if (!cs) {                    // deselected -- abort any command in progress
        g_rtlEeprom.shiftIn = 0; g_rtlEeprom.shiftInCount = 0;
        g_rtlEeprom.shiftOutCount = 0;
        g_rtlEeprom.eedo = 1;
    } else if (sk && !g_rtlEeprom.sk) {           // rising clock edge
        if (g_rtlEeprom.shiftOutCount > 0) {
            g_rtlEeprom.eedo = (g_rtlEeprom.shiftOut >> 15) & 1;
            g_rtlEeprom.shiftOut = (UINT16)(g_rtlEeprom.shiftOut << 1);
            g_rtlEeprom.shiftOutCount--;
        } else {
            g_rtlEeprom.shiftIn = (g_rtlEeprom.shiftIn << 1) | (UINT32)di;
            g_rtlEeprom.shiftInCount++;
            if (g_rtlEeprom.shiftInCount >= 9) {
                UINT32 c = g_rtlEeprom.shiftIn;
                if ((c >> 8) & 1) {                        // start bit present
                    UINT32 op   = (c >> 6) & 3;
                    UINT32 addr = c & 0x3F;
                    if (op == 2) {                         // READ
                        g_rtlEeprom.shiftOut = g_rtlEeprom.contents[addr];
                        g_rtlEeprom.shiftOutCount = 16;
                        g_rtlEeprom.eedo = 0;              // dummy bit before data
                    }
                    // WRITE/EWEN/EWDS are accepted and ignored: nothing in the
                    // guest depends on this EEPROM being writable, and silently
                    // dropping them is better than corrupting the MAC.
                    g_rtlEeprom.shiftIn = 0; g_rtlEeprom.shiftInCount = 0;
                } else if (g_rtlEeprom.shiftInCount > 32) {
                    g_rtlEeprom.shiftIn = 0; g_rtlEeprom.shiftInCount = 0;
                }
            }
        }
    }
    g_rtlEeprom.sk = sk;
    g_rtlEeprom.cs = cs;
    return (unsigned char)((val & 0xFE) | (g_rtlEeprom.eedo & 1));
}

void rtl8139InitRegs(void) {
    memset(rtl8139Regs, 0, RTL8139_IO_SIZE);
    rtl8139EepromInit();
    memcpy(rtl8139Regs + 0x00, rtl8139Mac, 6); // IDR0-5
    rtl8139Regs[0x37] = 0x01; // CR: BUFE (RX buffer empty) set, TE/RE/RST clear
    rtl8139Regs[0x76] = 0x04; // BMSR: bit2 = link status up
    // Registers the driver inspects during start, none of which existed before --
    // they all read back as zero, which is a valid value for none of them.
    *(UINT32 *)(rtl8139Regs + 0x40) = RTL8139_TCR_HWVERID; // TCR: chip version
    *(UINT32 *)(rtl8139Regs + 0x44) = 0x0000000E;          // RCR: accept broadcast/multicast/mine
    // CAPR must start such that readPos == writePos == 0, i.e. an EMPTY ring.
    // The convention is CAPR = readPos - 16, so a freshly-zeroed CAPR implies
    // readPos = 16 while writePos = 0 -- and the unsigned difference then wraps to
    // nearly a whole ring, making an empty ring look completely full. That made
    // the new overflow check drop every single received frame.
    //
    // U103: but ringSize-16 only satisfies readPos==0 for the ring size in effect
    // AT THIS INSTANT -- and at this instant RCR still holds the 8K default set
    // two lines above, giving CAPR=0x1FF0. The driver's very next move is to
    // program RBLEN for 64K (W44=5E0E) WITHOUT rewriting CAPR, so readPos becomes
    // (0x1FF0+16) % 65536 = 8192 while writePos is 0: an empty ring reporting
    // 57344 bytes used, with every frame we wrote landing at 0 while the driver
    // looked for it at 8192. It read garbage, wedged, and issued a software reset.
    // Measured over one boot: CAPR=0x1FF0 was the single most common sampled value
    // (339 hits, >2x any other) alongside 14 driver-issued resets, TCP data frames
    // sitting unread with CAPR frozen, and checksums verifying OK the whole time.
    //
    // -16 as a 16-bit value is what real hardware leaves here, and it is correct
    // for EVERY ring size: (0xFFF0 + 16) mod any power-of-two ringSize == 0.
    *(UINT16 *)&rtl8139Regs[0x38] = (UINT16)-16;
    rtl8139Regs[0x50] = 0x00;   // CR9346: config registers locked (normal state)
    rtl8139Regs[0x51] = 0x00;   // CONFIG0
    rtl8139Regs[0x52] = 0x10;   // CONFIG1: driver-loaded bit, not sleeping
    rtl8139Regs[0x58] = 0x40;   // MSR: link up, 100Mbps, not in low-power
    // U107: THE PHY BLOCK. BMSR (0x64) was never populated, so it read 0x0000 --
    // bit 2 is Link Status and bit 5 is Auto-Negotiation Complete, so every poll
    // told the driver the cable was unplugged and autoneg had never finished.
    // BAR1 is mapped read-only (WHvMapGpaRangeFlagRead), so register READS run
    // natively out of this very buffer and never trap: the driver was reading
    // "link down" straight from us and nothing in any log could show it. That is
    // why every [u106-reset] capture found a perfectly healthy adapter with ISR=0
    // and no error flags -- the trigger was never an error we raised, it was a
    // status we failed to report.
    //
    // The map here had drifted: 0x62 is BMCR (was commented CONFIG3, which is
    // really 0x59) and 0x64 is BMSR (a value was being written to 0x76 instead).
    // Toggleable ONLY so the A/B can run from one build at identical guest RAM:
    // set LOCALHOST_U107_PHY=0 to restore the old (unpopulated PHY) behaviour.
    // Comparing a PHY-on run against a differently-sized baseline proves nothing,
    // and guest RAM is currently dictated by whatever the host has spare.
    {
        const char *off = getenv("LOCALHOST_U107_PHY");
        if (!(off && off[0] == '0')) {
            *(UINT16 *)&rtl8139Regs[0x62] = 0x3100; // BMCR: autoneg on, 100Mbps, full duplex
            *(UINT16 *)&rtl8139Regs[0x64] = 0x782D; // BMSR: ExtCap|LINK UP|ANegAble|ANEG DONE|10H,10F,100H,100F
            *(UINT16 *)&rtl8139Regs[0x66] = 0x01E1; // ANAR: advertise 100F/100H/10F/10H, 802.3
            *(UINT16 *)&rtl8139Regs[0x68] = 0x45E1; // ANLPAR: link partner advertises same, with ACK
        }
    }
    rtl8139Regs[0x62] = 0x00;   // CONFIG3
    rtl8139Regs[0x69] = 0x00;   // CONFIG4
    rtl8139RxWritePos = 0;
    pendingRtl8139Irq = 0;
}

// Implements the real PCI BAR-sizing protocol for BAR0 (offset 0x10), the
// same write-0xFFFFFFFF-to-probe / write-a-real-address-to-program pattern
// as ahciHandleBar5Access, but for an I/O-space BAR rather than a memory
// BAR: bit 0 of both the address and the size mask is fixed at 1 (the PCI
// "this BAR decodes I/O space" indicator), and there's no guest-memory
// backing to allocate/map -- register accesses at the programmed base are
// handled directly by rtl8139HandleIoAccess through the normal io-port
// dispatch switch instead.
void rtl8139HandleBar0Access(WHV_X64_IO_PORT_ACCESS_CONTEXT *io, UINT32 baseOffset, UINT32 accessSize, UINT64 *rax) {
    if (io->AccessInfo.IsWrite) {
        if (baseOffset == 0x10 && accessSize >= 4) {
            UINT32 written = (UINT32)io->Rax;
            if (written == 0xFFFFFFFF) {
                rtl8139Bar0Sizing = 1;
            } else {
                rtl8139Bar0Sizing = 0;
                // Standard PCI BAR discovery writes 0 first (to disable/
                // clear the BAR) before the real probe-then-program
                // sequence -- must not latch that as the final address
                // (same guard AHCI's BAR5 handler uses via its own
                // "newBase != 0" check).
                UINT32 addrPart = written & ~(UINT32)(RTL8139_IO_SIZE - 1);
                if (addrPart != 0 && rtl8139IoBase == 0) {
                    rtl8139IoBase = addrPart | 0x1;
                    rtl8139InitRegs();
                    printf("[rtl8139] I/O BAR mapped at 0x%X\n", rtl8139IoBase & ~0x3);
                    fflush(stdout);
                }
                // FOLLOW A RE-BASE. The guard above is "rtl8139IoBase == 0", so
                // once the firmware programmed this BAR every later write was
                // dropped -- exactly the bug fixed for the AHCI ABAR in e3b482b,
                // sitting in the handler right next to it and missed at the time.
                //
                // Windows PnP re-assigns PCI BARs as a matter of course. When it
                // moved this one we carried on decoding the firmware's old port
                // range, so every register access from the Realtek driver reached
                // nothing at all, its miniport initialisation failed, and Device
                // Manager reported Code 10 -- "this device cannot start".
                //
                // Deliberately does NOT re-init the registers here: re-basing is
                // where the device lives, not a reset, and wiping its state
                // mid-configuration would undo whatever the driver had set up.
                else if (addrPart != 0 && (addrPart | 0x1) != rtl8139IoBase) {
                    printf("[rtl8139] guest RE-BASED I/O BAR: 0x%X -> 0x%X (kernelUp=%d)\n",
                           rtl8139IoBase & ~0x3, addrPart, g_bpModuleBase ? 1 : 0);
                    fflush(stdout);
                    rtl8139IoBase = addrPart | 0x1;
                }
            }
        }
    } else {
        UINT32 currentValue = rtl8139Bar0Sizing ? (~(UINT32)(RTL8139_IO_SIZE - 1) | 0x1) : rtl8139IoBase;
        UINT32 shift = (baseOffset - 0x10) * 8;
        UINT64 mask = (accessSize >= 4) ? 0xFFFFFFFFULL : (accessSize >= 2 ? 0xFFFFULL : 0xFFULL);
        *rax = (currentValue >> shift) & mask;
    }
}

// BAR1 (config offset 0x14) -- the memory-space counterpart of BAR0 above, and
// the resource whose absence PnP could never offer the driver. Same
// probe-then-program protocol; what differs is the low attribute bits, which on a
// memory BAR encode 32-bit / non-prefetchable (all zero) rather than BAR0's fixed
// bit-0 I/O indicator. As with the EHCI BAR nothing is mapped -- the GPA is left
// to fault so rtl8139HandleBar1Mmio can apply real register semantics.
void rtl8139MapBar1(WHV_PARTITION_HANDLE partition, UINT32 newBase) {
    if (rtl8139MmioPage == NULL) {
        rtl8139MmioPage = VirtualAlloc(NULL, RTL8139_MMIO_SIZE,
                                       MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (rtl8139MmioPage == NULL) {
            printf("[rtl8139] BAR1: VirtualAlloc failed -- memory window unavailable\n");
            fflush(stdout);
            return;
        }
        memset(rtl8139MmioPage, 0, RTL8139_MMIO_SIZE);
        // Carry the current register contents over, then re-point the register file
        // at the page so the device and the guest share one copy.
        memcpy(rtl8139MmioPage, rtl8139RegsStorage, RTL8139_IO_SIZE);
        rtl8139Regs = (unsigned char *)rtl8139MmioPage;
    }
    if (rtl8139MmioMapped && rtl8139MmioBase != 0 && rtl8139MmioBase != newBase) {
        WHvUnmapGpaRange(partition, rtl8139MmioBase, RTL8139_MMIO_SIZE);
        rtl8139MmioMapped = 0;
    }
    // READ ONLY. Reads are served by the CPU straight out of this page with no
    // exit; writes have no mapping and therefore fault out to
    // rtl8139HandleBar1Mmio, which is exactly where side effects belong.
    HRESULT hr = WHvMapGpaRange(partition, rtl8139MmioPage, newBase, RTL8139_MMIO_SIZE,
                                WHvMapGpaRangeFlagRead);
    rtl8139MmioBase = newBase;
    rtl8139MmioMapped = SUCCEEDED(hr) ? 1 : 0;
    printf("[rtl8139] memory BAR1 at 0x%X -- reads native, writes trapped (hr=0x%lX %s)\n",
           newBase, (unsigned long)hr, SUCCEEDED(hr) ? "ok" : "FAILED");
    fflush(stdout);
}

void rtl8139HandleBar1Access(WHV_PARTITION_HANDLE partition, WHV_X64_IO_PORT_ACCESS_CONTEXT *io,
                             UINT32 baseOffset, UINT32 accessSize, UINT64 *rax) {
    if (io->AccessInfo.IsWrite) {
        if (baseOffset == 0x14 && accessSize >= 4) {
            UINT32 written = (UINT32)io->Rax;
            if (written == 0xFFFFFFFF) {
                rtl8139Bar1Sizing = 1;
            } else {
                rtl8139Bar1Sizing = 0;
                UINT32 newBase = written & ~(UINT32)(RTL8139_MMIO_SIZE - 1);
                // "newBase != 0" for the same reason as BAR0: PCI discovery writes
                // 0 to clear a BAR before programming the real address, and
                // latching that would leave us decoding at 0.
                if (newBase != 0 && newBase != rtl8139MmioBase) {
                    rtl8139MapBar1(partition, newBase);
                }
            }
        }
        return;
    }
    {
        UINT32 currentValue = rtl8139Bar1Sizing ? (UINT32)(~(UINT32)(RTL8139_MMIO_SIZE - 1))
                                                : rtl8139MmioBase;
        UINT32 shift = (baseOffset - 0x14) * 8;
        UINT64 mask = (accessSize >= 4) ? 0xFFFFFFFFULL : (accessSize >= 2 ? 0xFFFFULL : 0xFFULL);
        *rax = (currentValue >> shift) & mask;
    }
}

// Accounting, shared by both windows onto the register file.
//
// WHO IS DRIVING THIS CARD? Same split that cracked the AHCI case, where
// "kernel touches = 0" proved storahci never bound at all. "The driver never
// reached the registers" and "the driver reached them and gave up" need
// completely different fixes, and for this NIC we have now seen BOTH: rev 0x10
// gave two TCR reads then silence, rev 0x20 gave zero accesses. `via` records
// which BAR the access arrived through, so those stay distinguishable too.
void rtl8139AccountAccess(UINT32 offset, int isWrite, UINT32 val, const char *via) {
    if (g_bpModuleBase) {
        g_nicGuestAccesses++;
        if (g_nicGuestAccesses == 1) {
            printf("[rtl8139-guest] FIRST kernel-side register access: off=0x%02X write=%d via=%s\n",
                   offset, isWrite ? 1 : 0, via);
            fflush(stdout);
        }
        // Ring of the most recent accesses, dumped from the heartbeat. The LAST
        // register the driver touches before it gives up is the interesting one,
        // and a capped log would fill with the first ones instead.
        {
            int slot = (int)(g_nicRingCount % NIC_RING_MAX);
            g_nicRingOff[slot]   = (unsigned char)offset;
            g_nicRingWrite[slot] = (unsigned char)(isWrite ? 1 : 0);
            g_nicRingVal[slot]   = val;
            g_nicRingCount++;
        }
    } else {
        g_nicFwAccesses++;
    }
}

// The register file's real semantics, independent of which BAR the access came
// through. Most registers are plain read/write storage in rtl8139Regs; a handful
// need side effects, and those must NOT degrade into passive RAM merely because
// the driver reached them through the memory window rather than the I/O ports.
// That sharing is the whole reason this is a separate function.
void rtl8139RegWrite(WHV_PARTITION_HANDLE partition, UINT32 offset, UINT32 accessSize, UINT64 written) {
    {
        if (offset == 0x40 && accessSize >= 4) {
            // TCR: the hardware-version bits (31:26 and 24:22) are READ-ONLY on
            // real silicon. The driver writes this register during setup, and
            // letting that write land would erase the chip identity we just
            // reported and leave it looking like an unknown device again.
            UINT32 val = (UINT32)written;
            *(UINT32 *)(rtl8139Regs + 0x40) =
                (val & ~0xFC800000U) | (RTL8139_TCR_HWVERID & 0xFC800000U);
        }
        else if (offset == 0x37 && accessSize >= 1) {
            // CR: RST (bit4) resets instantly and clears itself; TE/RE
            // (bits 2-3) are just tracked. BUFE (bit0) is a status bit the
            // NIC controls (see rtl8139ReceiveFrame / the CAPR handling
            // below), not something the driver's write should change --
            // preserve whatever we currently have there.
            unsigned char val = (unsigned char)written;
            if (val & 0x10) {
                // U106: WHY does the driver reset? This is the one causal link we
                // have never seen. NDIS restarts the adapter ~1.3x/min and every
                // restart discards in-flight RX, which is what leaves guestAcked
                // frozen at ISN+1 -- but nothing has ever captured the state that
                // triggers it. Dump everything BEFORE rtl8139InitRegs() wipes it,
                // plus the register writes that led here.
                {
                    UINT16 isr = *(UINT16 *)&rtl8139Regs[0x3E];
                    UINT16 imr = *(UINT16 *)&rtl8139Regs[0x3C];
                    UINT32 rcr = *(UINT32 *)&rtl8139Regs[0x44];
                    UINT16 capr = *(UINT16 *)&rtl8139Regs[0x38];
                    UINT16 cbr = *(UINT16 *)&rtl8139Regs[0x3A];
                    long total = g_nicRingCount;
                    long have = total < NIC_RING_MAX ? total : NIC_RING_MAX;
                    long i3;
                    printf("[u106-reset] ISR=0x%04X IMR=0x%04X CR=0x%02X RCR=0x%08X"
                           " CAPR=0x%04X CBR=0x%04X writePos=0x%X ringSize=%u"
                           " used=%u queued=%u\n",
                           isr, imr, rtl8139Regs[0x37], rcr, capr, cbr,
                           rtl8139RxWritePos, rtl8139RxRingSize(),
                           rtl8139RxRingUsed(), (unsigned)rtl8139RxQueueCount);
                    // ISR bits that matter: RER(0x02) TER(0x08) RXOVW(0x10)
                    // FOVW(0x40) TimeOut(0x4000) SERR(0x8000). If the driver is
                    // resetting because we reported an error, it is in here.
                    // U108: what went QUIET before the watchdog fired.
                    printf("[u108-quiet]   msSinceTX=%.0f msSinceRX=%.0f"
                           " msSinceIRQ=%.0f msSinceCAPR=%.0f  (-1 = never)\n",
                           u108MsSince(g_u108LastTx), u108MsSince(g_u108LastRx),
                           u108MsSince(g_u108LastIrq), u108MsSince(g_u108LastCapr));
                    printf("[u106-reset]   flags:%s%s%s%s%s%s\n",
                           (isr & 0x0002) ? " RER" : "", (isr & 0x0008) ? " TER" : "",
                           (isr & 0x0010) ? " RXOVW" : "", (isr & 0x0040) ? " FOVW" : "",
                           (isr & 0x4000) ? " TIMEOUT" : "", (isr & 0x8000) ? " SERR" : "");
                    printf("[u106-reset]   last %ld regs:", have);
                    for (i3 = total - have; i3 < total; i3++) {
                        int s3 = (int)(i3 % NIC_RING_MAX);
                        printf(" %s%02X=%X", g_nicRingWrite[s3] ? "W" : "R",
                               g_nicRingOff[s3], g_nicRingVal[s3]);
                    }
                    printf("\n");
                    fflush(stdout);
                }
                rtl8139InitRegs();
                printf("[rtl8139] software reset (CR)\n"); fflush(stdout);
            } else {
                unsigned char oldCr = rtl8139Regs[0x37];
                unsigned char newCr = (val & ~(unsigned char)0x11) | (oldCr & 0x01);
                rtl8139Regs[0x37] = newCr;
                if (!(oldCr & 0x08) && (newCr & 0x08)) {
                    // RE freshly enabled. This used to reset the ring
                    // unconditionally, which DESTROYED frames already queued:
                    // the driver toggles RE off and on during normal operation
                    // (observed W37=4 -> W37=C repeatedly), and every toggle
                    // threw away whatever we had written but the guest had not
                    // yet read. That is why ARP replies never arrived -- we
                    // answered all four "who-has 10.0.2.3" requests, and wiped
                    // each answer before the guest could consume it, leaving
                    // CAPR=0 CBR=0 BUFE=1 with nothing in the ring.
                    //
                    // Real hardware does not clear the receive buffer on an RE
                    // transition either; only a software RESET does that.
                    g_netRingReEnables++;
                    if (rtl8139RxWritePos != 0) g_netRingResetSkipped++;
                    printf("[rtl8139] receiver enabled (ring kept: writePos=0x%X,"
                           " %u frames queued)\n",
                           rtl8139RxWritePos, rtl8139RxQueueCount);
                    fflush(stdout);
                    // Anything that arrived while RE was off is waiting, not lost.
                    // Hand it over now -- the driver toggles RE constantly, and
                    // every one of those windows used to destroy frames.
                    rtl8139FlushRxQueue(partition);
                }
            }
        } else if (offset == 0x50) {
            // CR9346: the EEPROM's serial interface. Must run the 93C46 state
            // machine rather than store the byte, because bit 0 (EEDO) is driven
            // by the device -- as plain storage it just echoed the driver's own
            // write back, which is what left the driver clocking out a garbage
            // MAC address forever.
            rtl8139Regs[0x50] = rtl8139Eeprom9346Write((unsigned char)written);
        } else if (offset == 0x3E) {
            // ISR: write-1-to-clear, not a plain overwrite.
            UINT32 i;
            for (i = 0; i < accessSize && offset + i < RTL8139_IO_SIZE; i++) {
                rtl8139Regs[offset + i] &= ~(unsigned char)((written >> (i * 8)) & 0xFF);
            }
            // The interrupt line is LEVEL-triggered. If the driver acknowledged
            // some causes but others are still pending and unmasked, the line is
            // still asserted and must fire again. Without this, a frame that
            // arrived while the driver was inside its ISR was acknowledged away
            // and never re-signalled.
            rtl8139MaybeInjectIrq(partition);
        } else if (offset == 0x3C) {
            // IMR. Re-evaluate the interrupt line on every mask change.
            //
            // THIS IS THE BUG behind the DNS timeouts. The driver masks
            // interrupts (IMR=0) while it works and re-enables them afterwards --
            // both values were observed at different heartbeat samples. We only
            // evaluated (ISR & IMR) at the instant a frame was delivered, so any
            // frame that arrived during a masked window raised nothing at all,
            // and re-enabling the mask did not re-check. The frame then sat in
            // the ring, complete and undropped, until some unrelated interrupt
            // happened to wake the driver -- which is exactly the shape of the
            // evidence: replies relayed in 9-32ms, RX delivered with zero drops,
            // ring reporting data present, and the guest reporting 2s timeouts.
            //
            // Real hardware holds the line asserted while (ISR & IMR) is nonzero,
            // so unmasking an already-pending cause fires immediately.
            UINT32 i;
            for (i = 0; i < accessSize && offset + i < RTL8139_IO_SIZE; i++) {
                rtl8139Regs[offset + i] = (unsigned char)((written >> (i * 8)) & 0xFF);
            }
            rtl8139MaybeInjectIrq(partition);
        } else if ((offset == 0x10 || offset == 0x14 || offset == 0x18 || offset == 0x1C) && accessSize >= 4) {
            // TSDx: the real "submit this frame" trigger. Store the write
            // first, then synchronously DMA-read the frame via the
            // corresponding TSADx (always 0x10 above its TSDx) and hand it
            // to the backend, matching how fast real hardware transmits
            // relative to any driver completion poll -- there's no
            // meaningful "in progress" state to model here.
            UINT32 i;
            for (i = 0; i < 4; i++) rtl8139Regs[offset + i] = (unsigned char)((written >> (i * 8)) & 0xFF);

            // U109: the driver clearing OWN in TSD is it HANDING the descriptor to
            // us, so the matching TSAD completion bits must go stale immediately --
            // otherwise a stale TOK from the previous packet reads as if this new
            // send had already finished.
            {
                UINT32 d = (offset - 0x10) / 4;
                UINT16 tsad = *(UINT16 *)&rtl8139Regs[0x60];
                tsad &= (UINT16)~(1u << d);          // OWNd: ours now, not complete
                tsad &= (UINT16)~(1u << (12 + d));   // TOKd: this send has not succeeded yet
                *(UINT16 *)&rtl8139Regs[0x60] = tsad;
            }
            UINT32 tsd = *(UINT32 *)&rtl8139Regs[offset];
            UINT32 size = tsd & 0x1FFF; // bits 0-12
            UINT32 txAddr = *(UINT32 *)&rtl8139Regs[offset + 0x10]; // TSADx

            if (size > 0 && size <= 1792 && guestMemory && (UINT64)txAddr + size <= guestMemSize) {
                unsigned char frameBuf[1792];
                memcpy(frameBuf, (unsigned char *)guestMemory + txAddr, size);
                QueryPerformanceCounter(&g_u108LastTx);
                rtl8139TransmitFrame(partition, frameBuf, size);
                *(UINT32 *)&rtl8139Regs[offset] = (tsd & ~(UINT32)0x1FFF) | size | 0x8000 /* TOK */ | 0x2000 /* OWN */;
                // U109: ALSO publish completion in TSAD (0x60), the aggregate
                // "Transmit Status of All Descriptors" register. Marking only the
                // per-descriptor TSD is not enough: the driver polls TSAD to find
                // which sends finished, and it was never written, so it read
                // 0x0000 forever -- "nothing has ever completed". BAR1 is mapped
                // read-only, so that poll runs natively and never trapped; no log
                // could show it, exactly like BMSR.
                //
                // Measured signature that led here: at EVERY software reset the
                // adapter was healthy (ISR=0, no error flags, ring drained) but
                // msSinceTX clustered at 22-24s, while msSinceRX scattered. That
                // is a transmit watchdog firing on a completion that never
                // appears where the driver looks for it.
                //
                // Layout: OWN0-3 in bits 0-3, TABT0-3 in 4-7, TUN0-3 in 8-11,
                // TOK0-3 in bits 12-15.
                {
                    UINT32 d = (offset - 0x10) / 4;          // which of the 4 descriptors
                    UINT16 tsad = *(UINT16 *)&rtl8139Regs[0x60];
                    tsad |= (UINT16)(1u << d);               // OWNd: DMA complete
                    tsad |= (UINT16)(1u << (12 + d));        // TOKd: transmitted OK
                    tsad &= (UINT16)~(1u << (4 + d));        // not aborted
                    tsad &= (UINT16)~(1u << (8 + d));        // no underrun
                    *(UINT16 *)&rtl8139Regs[0x60] = tsad;
                }
                *(UINT16 *)&rtl8139Regs[0x3E] |= 0x0004; // ISR: TOK
                rtl8139MaybeInjectIrq(partition);
            }
        } else if (offset == 0x38 && accessSize >= 2) {
            // CAPR: guest's RX read pointer, conventionally written as
            // (consumed_offset - 16). If that catches up to our write
            // pointer, the ring is drained -- set BUFE.
            //
            // U108: this write is the guest PROVING it consumed a frame, which is
            // the cleanest "the driver is alive and processing RX" signal we have.
            QueryPerformanceCounter(&g_u108LastCapr);
            UINT32 i;
            for (i = 0; i < accessSize && offset + i < RTL8139_IO_SIZE; i++) {
                rtl8139Regs[offset + i] = (unsigned char)((written >> (i * 8)) & 0xFF);
            }
            UINT16 capr = *(UINT16 *)&rtl8139Regs[0x38];
            UINT32 ringSize = rtl8139RxRingSize();
            UINT32 consumedPos = ((UINT32)capr + 16) % ringSize;
            UINT32 writePos = rtl8139RxWritePos % ringSize;
            // "Caught up" must not be an EXACT-equality test. A drain signal that
            // can be stepped over is a hang: when our write pointer trailed the
            // driver's read pointer by 4 bytes per packet, this never matched,
            // BUFE was never set, and the driver spun at DISPATCH_LEVEL forever.
            // The pointer arithmetic is fixed above, but treat "read pointer is
            // at or past the write pointer" as drained regardless, so a future
            // off-by-a-few can cost a stale flag rather than wedge the guest.
            // The window is generous in the wrap direction only up to a packet's
            // worth, so a genuinely full ring is still reported as non-empty.
            UINT32 ahead = (consumedPos - writePos) % ringSize;
            if (consumedPos == writePos || ahead <= 8) {
                rtl8139Regs[0x37] |= 0x01; // BUFE set -- caught up
            }
            // The driver just freed ring space. If frames were deferred for want
            // of it, this is the moment they fit -- and delivering here rather
            // than waiting for the next poll keeps the ROK interrupt coming while
            // the driver is still in its receive path.
            rtl8139FlushRxQueue(partition);
        } else {
            UINT32 i;
            for (i = 0; i < accessSize && offset + i < RTL8139_IO_SIZE; i++) {
                rtl8139Regs[offset + i] = (unsigned char)((written >> (i * 8)) & 0xFF);
            }
        }
    }
}

UINT64 rtl8139RegRead(UINT32 offset, UINT32 accessSize) {
    UINT64 rax = 0;
    UINT32 i;
    for (i = 0; i < accessSize && offset + i < RTL8139_IO_SIZE; i++) {
        rax |= (UINT64)rtl8139Regs[offset + i] << (i * 8);
    }
    // What we ACTUALLY hand back. The access ring records the guest's incoming
    // value, which on a read is the register's content BEFORE the instruction and
    // therefore says nothing about our answer -- the same artefact that misled the
    // AHCI and PM-timer traces. The access SIZE matters too: the chip-version bits
    // live in TCR's top byte, so a 1-byte read of 0x40 returns 0x00 and looks like
    // no chip at all.
    if (g_bpModuleBase && g_nicReadLogged < 40) {
        g_nicReadLogged++;
        printf("[rtl8139-read] off=0x%02X size=%u -> returned 0x%08llX\n",
               offset, accessSize, (unsigned long long)rax);
        fflush(stdout);
    }
    return rax;
}

// BAR0: the I/O window onto the register file, at the guest-programmed base
// (rtl8139IoBase & ~0x3 -- bit 0 is the I/O-space indicator, not address).
void rtl8139HandleIoAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitContext->IoPortAccess;
    UINT32 base = rtl8139IoBase & ~0x3;
    UINT32 offset = io->PortNumber - base;
    UINT32 accessSize = io->AccessInfo.AccessSize ? io->AccessInfo.AccessSize : 1;
    UINT64 rax = 0;

    rtl8139AccountAccess(offset, io->AccessInfo.IsWrite ? 1 : 0, (UINT32)io->Rax, "io");

    if (io->AccessInfo.IsWrite) rtl8139RegWrite(partition, offset, accessSize, io->Rax);
    else                        rax = rtl8139RegRead(offset, accessSize);

    WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
    WHV_REGISTER_VALUE values[2] = { 0 };
    values[0].Reg64 = io->AccessInfo.IsWrite ? 0 : rax;
    values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
    WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
}

// BAR1: the MEMORY window onto the very same registers. The GPA is left unmapped,
// so every access faults out here and is decoded with the shared MMIO decoder --
// the same one the IOAPIC and the AHCI ABAR use.
//
// It MUST claim its range: the MemoryAccess fallback maps a zero-filled scratch
// page over anything unclaimed, which would quietly turn the entire register file
// into zeros -- a driver reading TCR would see "no chip" instead of a failure we
// could observe.
// Minimal decoder for the group-1 READ-MODIFY-WRITE forms, which the shared MMIO
// decoder does not handle -- it only understands plain loads and stores.
//
// This is not a nicety. The C+ datapath's very first act is
//     66 81 A1 E0 00 00 00 FC FF   =  and word ptr [rcx+0xE0], 0xFFFC
// on CpCmd, and an instruction we cannot decode is NOT harmless: it falls through
// to the generic MemoryAccess handler, which maps RAM over the page and silently
// turns the whole register file into storage. Measured: BAR1 died on that first
// instruction and every later MMIO access went into the void.
//
// Returns 1 and fills the outputs when insn is `<alu> r/m, imm` against memory.
// Known gap: EFLAGS are not updated. Drivers do not branch on the flags of an
// MMIO read-modify-write in practice, but it is a real deviation.
// Decodes the STORE forms that can fault on the read-only BAR1 page.
//
// This only has to cover writes, and that is the point of the read-only mapping:
// reads never fault, because the CPU serves them from the mapped page. Chasing
// read forms one at a time is what produced three failed runs in a row (`and`,
// `test`, `movzx`); the set of ways to WRITE memory is small and closed:
//
//   88 /r        MOV  m8,  r8          C6 /0 ib   MOV m8,  imm8
//   89 /r        MOV  m,   r           C7 /0 iz   MOV m,   imm
//   00/08/20/28/30 /r  ADD/OR/AND/SUB/XOR m8, r8
//   01/09/21/29/31 /r  ADD/OR/AND/SUB/XOR m,  r
//   80/81/83 /n        group 1 with an immediate  (the `and word ptr` RMW)
//
// *aluOp is -1 for a plain MOV, otherwise the group-1 operation number.
int rtl8139DecodeStore(const unsigned char *insn, int len, int *aluOp, int *isImm,
                       UINT32 *immVal, int *regNum, int *opSize, int *totalLen) {
    int i = 0, size = 4, rex = 0, byteOp = 0;
    if (i < len && insn[i] == 0x66) { size = 2; i++; }       // operand-size override
    if (i < len && (insn[i] & 0xF0) == 0x40) {               // REX
        rex = insn[i];
        if (rex & 0x08) size = 8;                            // REX.W
        i++;
    }
    if (i >= len) return 0;
    unsigned char op = insn[i++];

    int alu = -1, imm = 0;
    switch (op) {
        case 0x88: byteOp = 1; break;                        // MOV m8, r8
        case 0x89:             break;                        // MOV m, r
        case 0xC6: byteOp = 1; imm = 1; break;               // MOV m8, imm8
        case 0xC7:             imm = 1; break;               // MOV m, imm
        case 0x00: alu = 0; byteOp = 1; break;
        case 0x01: alu = 0; break;
        case 0x08: alu = 1; byteOp = 1; break;
        case 0x09: alu = 1; break;
        case 0x20: alu = 4; byteOp = 1; break;
        case 0x21: alu = 4; break;
        case 0x28: alu = 5; byteOp = 1; break;
        case 0x29: alu = 5; break;
        case 0x30: alu = 6; byteOp = 1; break;
        case 0x31: alu = 6; break;
        case 0x80: byteOp = 1; imm = 1; alu = -2; break;     // group 1, op in ModRM.reg
        case 0x81:             imm = 1; alu = -2; break;
        case 0x83:             imm = 2; alu = -2; break;     // imm8, sign-extended
        default: return 0;
    }
    if (byteOp) size = 1;

    if (i >= len) return 0;
    unsigned char modrm = insn[i++];
    int mod = (modrm >> 6) & 3;
    int reg = (modrm >> 3) & 7;
    int rm  = modrm & 7;
    if (mod == 3) return 0;                                  // register destination: not MMIO
    if (alu == -2) alu = reg;                                // group 1: operation is in reg
    if (rm == 4) { if (i >= len) return 0; i++; }            // SIB byte
    if (mod == 1)                       i += 1;              // disp8
    else if (mod == 2)                  i += 4;              // disp32
    else if (mod == 0 && rm == 5)       i += 4;              // disp32 / RIP-relative
    if (i > len) return 0;

    UINT32 immediate = 0;
    if (imm == 2) {                                          // imm8, sign-extended
        if (i >= len) return 0;
        immediate = (UINT32)(INT32)(signed char)insn[i]; i += 1;
    } else if (imm) {
        if (size == 1)      { if (i + 1 > len) return 0; immediate = insn[i]; i += 1; }
        else if (size == 2) { if (i + 2 > len) return 0;
                              immediate = (UINT32)(insn[i] | (insn[i+1] << 8)); i += 2; }
        else                { if (i + 4 > len) return 0;
                              immediate = (UINT32)insn[i] | ((UINT32)insn[i+1] << 8) |
                                          ((UINT32)insn[i+2] << 16) | ((UINT32)insn[i+3] << 24);
                              i += 4; }
    }

    *aluOp = alu;
    *isImm = imm ? 1 : 0;
    *immVal = immediate;
    // REX.R extends the source register. Without REX, an 8-bit reg of 4..7 means
    // AH/CH/DH/BH -- flagged as +16 so the caller can take the high byte.
    *regNum = reg | ((rex & 0x04) ? 8 : 0);
    if (byteOp && !rex && reg >= 4) *regNum = (reg - 4) + 16;
    *opSize = (size == 8) ? 4 : size;   // no RTL8139 register is wider than 4 bytes
    *totalLen = i;
    return 1;
}

// Gets the faulting instruction's bytes, fetching them from guest memory when WHP
// does not supply them.
//
// WHP fills MemoryAccess.InstructionBytes for a fault on an UNMAPPED GPA, but not
// for a protection fault on a MAPPED range -- there InstructionByteCount is 0.
// Once BAR1 became a real read-only mapping, every write fault arrived with zero
// bytes, so every decoder failed and the fallback mapped RAM over the window. So
// walk the guest's page tables and read the instruction ourselves.
//
// Returns the number of bytes available (up to 16).
int rtl8139FetchInsn(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext,
                     unsigned char *buf) {
    int have = exitContext->MemoryAccess.InstructionByteCount;
    if (have > 0) {
        if (have > 16) have = 16;
        memcpy(buf, exitContext->MemoryAccess.InstructionBytes, (size_t)have);
        return have;
    }
    if (!guestMemory) return 0;
    WHV_GUEST_PHYSICAL_ADDRESS gpa = 0;
    WHV_TRANSLATE_GVA_RESULT tr = { 0 };
    HRESULT hr = WHvTranslateGva(partition, 0, exitContext->VpContext.Rip,
                                 WHvTranslateGvaFlagValidateRead, &tr, &gpa);
    if (FAILED(hr) || tr.ResultCode != WHvTranslateGvaResultSuccess) return 0;
    if (gpa >= guestMemSize) return 0;
    // Never read past the end of guest RAM, and stop at the page boundary: the
    // next page may not be present, and 16 bytes is enough for these forms.
    UINT64 avail = guestMemSize - gpa;
    UINT64 toPageEnd = 0x1000 - (gpa & 0xFFF);
    UINT64 n = 16;
    if (n > avail) n = avail;
    if (n > toPageEnd) n = toPageEnd;
    memcpy(buf, (unsigned char *)guestMemory + gpa, (size_t)n);
    return (int)n;
}

// TEST r/m, r (opcodes 0x84 / 0x85). Reads memory and writes no memory -- but it
// SETS FLAGS, and the driver branches on them immediately (the instruction we hit
// is `test byte ptr [rax+0x5A], r8b` followed by `jz`). Emulating the access while
// leaving RFLAGS alone would send the driver down the wrong branch, which is worse
// than not handling it at all -- so this reports the register operand and the
// caller computes flags exactly as the CPU would.
int rtl8139DecodeTest(const unsigned char *insn, int len, int *regNum,
                      int *opSize, int *totalLen) {
    int i = 0, size = 4, rex = 0;
    if (i < len && insn[i] == 0x66) { size = 2; i++; }
    if (i < len && (insn[i] & 0xF0) == 0x40) {
        rex = insn[i];
        if (rex & 0x08) size = 8;
        i++;
    }
    if (i >= len) return 0;
    unsigned char op = insn[i++];
    if (op != 0x84 && op != 0x85) return 0;
    if (op == 0x84) size = 1;
    if (i >= len) return 0;
    unsigned char modrm = insn[i++];
    int mod = (modrm >> 6) & 3;
    int rm  = modrm & 7;
    int reg = ((modrm >> 3) & 7) | ((rex & 0x04) ? 8 : 0);   // REX.R extends reg
    if (mod == 3) return 0;                                   // register operand: not MMIO
    if (rm == 4) { if (i >= len) return 0; i++; }             // SIB
    if (mod == 1)                 i += 1;
    else if (mod == 2)            i += 4;
    else if (mod == 0 && rm == 5) i += 4;
    if (i > len) return 0;
    *regNum = reg;
    *opSize = (size == 8) ? 4 : size;   // no register here is wider than 4 bytes
    *totalLen = i;
    return 1;
}

int rtl8139HandleBar1Mmio(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    UINT64 gpa = exitContext->MemoryAccess.Gpa;
    if (rtl8139MmioBase == 0) return 0;
    if (gpa < rtl8139MmioBase || gpa >= (UINT64)rtl8139MmioBase + RTL8139_MMIO_SIZE) return 0;
    UINT32 off = (UINT32)(gpa - rtl8139MmioBase);
    // With the page mapped read-only, reads never get here -- the CPU serves them
    // from rtl8139MmioPage. What arrives is writes (and read-modify-writes), which
    // are the accesses that actually need side effects. Offsets past the 256-byte
    // register file are reserved: rtl8139RegRead/Write bound-check, so they read as
    // zero and absorb writes, which is what reserved space does.

    unsigned char insnBuf[16];
    int insnLen = rtl8139FetchInsn(partition, exitContext, insnBuf);

    // TEST: reads memory, writes flags.
    {
        int testReg = 0, testSize = 0, testLen = 0;
        if (rtl8139DecodeTest(insnBuf, insnLen, &testReg, &testSize, &testLen)) {
            UINT64 memVal = rtl8139RegRead(off, (UINT32)testSize);
            WHV_REGISTER_VALUE regVal = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[testReg], 1, &regVal);
            UINT64 mask = (testSize == 1) ? 0xFFULL : (testSize == 2) ? 0xFFFFULL : 0xFFFFFFFFULL;
            UINT64 res = (memVal & regVal.Reg64) & mask;

            WHV_REGISTER_NAME flagsName = WHvX64RegisterRflags;
            WHV_REGISTER_VALUE flags = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, &flagsName, 1, &flags);
            UINT64 f = flags.Reg64;
            f &= ~0x8D5ULL;                              // clear CF,PF,AF,ZF,SF,OF
            if (res == 0) f |= 0x40;                     // ZF
            if (res & (mask ^ (mask >> 1))) f |= 0x80;   // SF: top bit of the operand size
            {                                            // PF: parity of the low byte
                unsigned char b = (unsigned char)(res & 0xFF);
                int bit, ones = 0;
                for (bit = 0; bit < 8; bit++) if (b & (1 << bit)) ones++;
                if ((ones & 1) == 0) f |= 0x04;
            }
            flags.Reg64 = f;
            WHvSetVirtualProcessorRegisters(partition, 0, &flagsName, 1, &flags);

            rtl8139AccountAccess(off, 0, (UINT32)memVal, "mmio-test");
            WHV_REGISTER_NAME tRip = WHvX64RegisterRip;
            WHV_REGISTER_VALUE tRipVal = { 0 };
            tRipVal.Reg64 = exitContext->VpContext.Rip + (UINT64)testLen;
            WHvSetVirtualProcessorRegisters(partition, 0, &tRip, 1, &tRipVal);
            return 1;
        }
    }

    // Stores, including the read-modify-write forms. A rejection here costs us the
    // whole memory window (see rtl8139DecodeStore), so this runs before the shared
    // decoder, which knows neither the 8-bit nor the group-1 forms.
    {
        int aluOp = 0, isImm = 0, srcReg = 0, stSize = 0, stLen = 0;
        UINT32 imm = 0;
        if (rtl8139DecodeStore(insnBuf, insnLen, &aluOp, &isImm, &imm, &srcReg,
                               &stSize, &stLen)) {
            UINT64 src = imm;
            if (!isImm) {
                int hiByte = (srcReg >= 16);
                WHV_REGISTER_VALUE regVal = { 0 };
                WHvGetVirtualProcessorRegisters(partition, 0,
                                                &ioapicGprNames[hiByte ? srcReg - 16 : srcReg],
                                                1, &regVal);
                src = hiByte ? ((regVal.Reg64 >> 8) & 0xFF) : regVal.Reg64;
            }
            UINT64 mask = (stSize == 1) ? 0xFFULL : (stSize == 2) ? 0xFFFFULL : 0xFFFFFFFFULL;
            UINT64 val;
            int writes = 1;
            if (aluOp < 0) {
                val = src;                               // plain MOV
            } else {
                UINT64 cur = rtl8139RegRead(off, (UINT32)stSize);
                switch (aluOp) {
                    case 0: val = cur + src; break;      // ADD
                    case 1: val = cur | src; break;      // OR
                    case 4: val = cur & src; break;      // AND
                    case 5: val = cur - src; break;      // SUB
                    case 6: val = cur ^ src; break;      // XOR
                    case 7: val = cur; writes = 0; break;// CMP -- compares only
                    default: return 0;                   // ADC/SBB: not modelled
                }
            }
            val &= mask;

            rtl8139AccountAccess(off, writes, (UINT32)val, "mmio");
            if (writes) rtl8139RegWrite(partition, off, (UINT32)stSize, val);

            WHV_REGISTER_NAME stRip = WHvX64RegisterRip;
            WHV_REGISTER_VALUE stRipVal = { 0 };
            stRipVal.Reg64 = exitContext->VpContext.Rip + (UINT64)stLen;
            WHvSetVirtualProcessorRegisters(partition, 0, &stRip, 1, &stRipVal);
            return 1;
        }
    }

    int isWrite = 0, isImm = 0, regNum = 0, insnTotalLen = 0;
    UINT32 immVal = 0;
    if (!ioapicDecodeMmio(insnBuf, insnLen,
                          &isWrite, &isImm, &regNum, &immVal, &insnTotalLen)) {
        static int nicMmioFailLog = 0;
        if (nicMmioFailLog++ < 20) {
            printf("[rtl8139-mmio] undecodable instruction at rip=0x%llX off=0x%02X (%d bytes):",
                   (unsigned long long)exitContext->VpContext.Rip, off, insnLen);
            int bi;
            for (bi = 0; bi < insnLen; bi++)
                printf(" %02X", insnBuf[bi]);
            printf("\n");
            fflush(stdout);
        }
        return 0; // let the generic handler deal with it rather than corrupting state
    }

    // The decoder does not report operand width, and the RTL8139's registers are a
    // mix of 1/2/4 bytes. Four is right for every register the C+ datapath drives;
    // clamp so a register near the end of the file cannot read or write past it.
    UINT32 accessSize = (off + 4 <= RTL8139_IO_SIZE) ? 4 : (RTL8139_IO_SIZE - off);

    if (isWrite) {
        UINT32 value = immVal;
        if (!isImm) {
            WHV_REGISTER_VALUE regVal = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &regVal);
            value = (UINT32)regVal.Reg64;
        }
        rtl8139AccountAccess(off, 1, value, "mmio");
        rtl8139RegWrite(partition, off, accessSize, (UINT64)value);
    } else {
        rtl8139AccountAccess(off, 0, 0, "mmio");
        UINT64 value = rtl8139RegRead(off, accessSize);
        WHV_REGISTER_VALUE regVal = { 0 };
        regVal.Reg64 = value;
        WHvSetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &regVal);
    }

    // InstructionLength is not populated for MMIO exits, so advance RIP by the
    // length our own decode determined -- same as the IOAPIC and ABAR paths.
    WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
    WHV_REGISTER_VALUE ripVal = { 0 };
    ripVal.Reg64 = exitContext->VpContext.Rip + (UINT64)insnTotalLen;
    WHvSetVirtualProcessorRegisters(partition, 0, &ripName, 1, &ripVal);
    return 1;
}

#include "e1000_dev.c"

#if LH_USE_XHCI
#include "xhci_dev.c"
#endif

// Handles I/O ports 0xCF8 (CONFIG_ADDRESS) and 0xCFC-0xCFF (CONFIG_DATA) --
// see pciSelectConfigSpace above for what this stub does and doesn't model.
int pciConfigAccessLogCount = 0;

// Post-2026-07-16-part-10 investigation (see
// docs/investigations/post-vppt-boot-stall.md): the capped logging below
// (first 300, then every 50000th) misses whatever PCI config traffic
// immediately precedes a later, rarer stall -- total access counts by
// then are in the tens of thousands, landing in the gap. Keep a small
// ring buffer of the most recent accesses instead, always up to date
// regardless of total count, so the stall watchdog can dump exactly what
// led up to a freeze.
#define PCI_CFG_RING_SIZE 512
typedef struct { UINT32 bus, dev, func, offset; UINT32 isWrite; UINT64 val; } PciCfgRingEntry;
PciCfgRingEntry g_pciCfgRing[PCI_CFG_RING_SIZE];
int g_pciCfgRingPos = 0;
int g_pciCfgRingCount = 0;

void pciHandleConfigAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    if (!pciConfigSpacesInit) pciInitConfigSpaces();

    WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitContext->IoPortAccess;
    UINT16 port = io->PortNumber;
    UINT64 rax = io->Rax;

    if (port == 0xCF8) {
        if (io->AccessInfo.IsWrite) {
            pciConfigAddress = (UINT32)io->Rax;
        } else {
            rax = pciConfigAddress;
        }
    } else { // 0xCFC-0xCFF
        UINT32 bus = (pciConfigAddress >> 16) & 0xFF;
        UINT32 dev = (pciConfigAddress >> 11) & 0x1F;
        UINT32 func = (pciConfigAddress >> 8) & 0x7;
        UINT32 baseOffset = (pciConfigAddress & 0xFC) + (port - 0xCFC);
        UINT32 accessSize = io->AccessInfo.AccessSize ? io->AccessInfo.AccessSize : 4;
        unsigned char *cfg = pciSelectConfigSpace(bus, dev, func);

        if (cfg == pciAhciConfig && baseOffset >= 0x24 && baseOffset <= 0x27) {
            ahciHandleBar5Access(partition, io, baseOffset, accessSize, &rax);
        } else if (cfg == pciE1000Config && baseOffset >= 0x10 && baseOffset <= 0x13) {
            e1000HandleBar0Access(io, baseOffset, accessSize, &rax);
        } else if (cfg == pciE1000Config && baseOffset >= 0x14 && baseOffset <= 0x17) {
            e1000HandleBar1Access(io, baseOffset, accessSize, &rax);
        } else if (cfg == pciRtl8139Config && baseOffset >= 0x10 && baseOffset <= 0x13) {
            rtl8139HandleBar0Access(io, baseOffset, accessSize, &rax);
        } else if (cfg == pciRtl8139Config && baseOffset >= 0x14 && baseOffset <= 0x17) {
            rtl8139HandleBar1Access(partition, io, baseOffset, accessSize, &rax);
        } else if (cfg == pciEhciConfig && baseOffset >= 0x10 && baseOffset <= 0x13) {
#if LH_USE_XHCI
            xhciHandleBar0Access(io, baseOffset, accessSize, &rax);
#else
            ehciHandleBar0Access(io, baseOffset, accessSize, &rax);
#endif
        } else if (cfg != NULL && baseOffset <= 255) {
            if (io->AccessInfo.IsWrite) {
                UINT32 i;
                for (i = 0; i < accessSize && baseOffset + i <= 255; i++) {
                    if (!pciRegisterIsReadOnly(baseOffset + i)) {
                        cfg[baseOffset + i] = (unsigned char)((io->Rax >> (i * 8)) & 0xFF);
                    }
                }
            } else {
                UINT64 value = 0;
                UINT32 i;
                for (i = 0; i < accessSize && baseOffset + i <= 255; i++) {
                    value |= (UINT64)cfg[baseOffset + i] << (i * 8);
                }
                rax = value;
            }
        } else if (!io->AccessInfo.IsWrite) {
            rax = 0xFFFFFFFFULL; // no device here
        }
        // Writes to a nonexistent bus/device/function are simply dropped.

        g_pciCfgRing[g_pciCfgRingPos].bus = bus;
        g_pciCfgRing[g_pciCfgRingPos].dev = dev;
        g_pciCfgRing[g_pciCfgRingPos].func = func;
        g_pciCfgRing[g_pciCfgRingPos].offset = baseOffset;
        g_pciCfgRing[g_pciCfgRingPos].isWrite = io->AccessInfo.IsWrite;
        g_pciCfgRing[g_pciCfgRingPos].val = rax;
        g_pciCfgRingPos = (g_pciCfgRingPos + 1) % PCI_CFG_RING_SIZE;
        if (g_pciCfgRingCount < PCI_CFG_RING_SIZE) g_pciCfgRingCount++;

        // TEMP DIAGNOSTIC: understand a heavy, sustained burst of 0xCFC
        // traffic during Windows 10 boot (millions of exits, PCI enumeration
        // territory) -- log detailed bus/dev/func/offset for the first N
        // accesses, then periodically, to see whether it's a genuinely huge
        // (but bounded/normal) enumeration or something stuck cycling the
        // same slot.
        // EVERY config access to the NIC once the kernel is up. The general log
        // below is capped at 300 and firmware enumeration consumes all of it, so
        // what WINDOWS does with this device -- assigning its I/O range, its
        // interrupt, enabling I/O space and bus mastering in the command register
        // -- has never been visible. The driver reads one register and gives up,
        // so the failure is on the resource side rather than in the device model,
        // and this is where resources are handed out.
        // FUNC and cfg-presence included deliberately. Without them a burst of
        // 0xFFFF vendor-ID reads looks like "the device vanished", when it is
        // almost certainly the normal func=1..7 multifunction probe, where
        // all-ones IS the correct answer. Logging a value without the context
        // that makes it interpretable has produced three wrong readings today.
        if (g_bpModuleBase && (dev == 3 || dev == 5) && g_nicCfgLogged < 80) {
            g_nicCfgLogged++;
            printf("[nic-cfg] func=%u off=0x%02X size=%u write=%d val=0x%llX cfg=%s\n",
                   func, baseOffset, accessSize, io->AccessInfo.IsWrite,
                   (unsigned long long)rax, cfg ? "present" : "absent");
            fflush(stdout);
        }
        pciConfigAccessLogCount++;
        if (pciConfigAccessLogCount <= 300 || pciConfigAccessLogCount % 50000 == 0) {
            printf("[pcicfg #%d] bus=%u dev=%u func=%u off=0x%02X size=%u write=%d val=0x%llX cfg=%s\n",
                   pciConfigAccessLogCount, bus, dev, func, baseOffset, accessSize, io->AccessInfo.IsWrite,
                   (unsigned long long)rax, cfg ? "present" : "absent");
            fflush(stdout);
        }
    }

    WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
    WHV_REGISTER_VALUE values[2] = { 0 };
    values[0].Reg64 = rax;
    values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
    WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
}

// --- AHCI command processing ---
// A real SATA/AHCI driver requires LBA48 ("48-bit Address feature set")
// support to be advertised in IDENTIFY DEVICE before it will issue
// READ/WRITE DMA EXT at all -- without it, AhciBusDxe (or any comparable
// driver) will complete IDENTIFY successfully and then simply never issue
// a follow-up read, exactly as observed empirically. ataFillIdentify()
// itself can't be changed to add this: it's shared with the legacy PIO
// path, whose ataHandleCommand only implements the 28-bit READ/WRITE
// SECTORS commands -- if SeaBIOS saw an LBA48-capable drive it could
// reasonably switch to 48-bit commands that path doesn't handle, and would
// have no other reason to notice this now-24-bit-limited-in-practice
// behavior since it currently works precisely because LBA48 isn't
// advertised. This wrapper overlays only the extra capability words the
// AHCI path needs, layered on top of the same shared base fields.
// sectorSize is a PARAMETER rather than the ataSectorSize global: with two ports
// the ISO (2048-byte sectors) and the hard disk (512) are attached at the same
// time, so "the" sector size no longer exists.
void ahciFillIdentify(unsigned char *buf, UINT64 sectors, UINT32 sectorSize) {
    ataFillIdentify(buf, sectors);
    UINT16 *id = (UINT16 *)buf;
    id[49] |= 0x0100;  // bit8: DMA supported (bit9 LBA already set by ataFillIdentify)
    id[76] = 0x0106;   // SATA capabilities: Gen1 (bit1) + Gen2 (bit2) signaling, NCQ (bit8)
    id[83] = 0x4400;   // bit14: word-valid marker (fixed), bit10: LBA48 supported
    id[86] = 0x0400;   // bit10: LBA48 supported-and-enabled
    id[100] = (UINT16)(sectors & 0xFFFF);
    id[101] = (UINT16)((sectors >> 16) & 0xFFFF);
    id[102] = (UINT16)((sectors >> 32) & 0xFFFF);
    id[103] = (UINT16)((sectors >> 48) & 0xFFFF);

    // Word 106 (PHYSICAL/LOGICAL SECTOR SIZE): bit15=0/bit14=1 (fixed,
    // marks the field valid), bit12=1 (logical sector size exceeds 256
    // words/512 bytes -- words 117-118 give the real size). Real EDK2
    // reads exactly these fields this way (AtaAtapiPassThru.c,
    // BlockSize computation): "(phy_logic_sector_support & (BIT14|BIT15))
    // == BIT14" then "& BIT12", then logic_sector_size_hi/lo (words
    // 117-118, a WORD count) * 2 for bytes. Needed so BlockIo->Media->
    // BlockSize comes out as 2048 for an ISO instead of the 512 default --
    // EDK2's El Torito parser (PartitionDxe/ElTorito.c) hard-requires
    // BlockSize==2048 before it will even attempt to recognize a CD-ROM
    // boot catalog, which is how Windows installer media conventionally
    // gets consumed.
    if (sectorSize != 512) {
        UINT32 sectorSizeWords = sectorSize / 2;
        id[106] = 0x4000 | 0x1000; // bit14 | bit12 (bit15 stays 0)
        id[117] = (UINT16)(sectorSizeWords & 0xFFFF);
        id[118] = (UINT16)((sectorSizeWords >> 16) & 0xFFFF);

        // REMOVABLE MEDIA (word 0, bit 7), so BlockIo->Media->RemovableMedia
        // comes out TRUE and an ISO presents as optical rather than as a fixed
        // disk. Correct modelling -- an ISO IS removable -- but recorded here as
        // MEASURED-NO-EFFECT so nobody re-runs the experiment: with and without
        // this bit the boot is byte-for-byte identical (531 AHCI commands, 54
        // reads of LBA 1407, 4 of LBA 1409, same failure). It is not the reason
        // the ISO will not boot.
        id[0] |= 0x0080;
    }
}

// Copies data out to (scatter) or in from (gather) the PRDT (Physical
// Region Descriptor Table) scatter/gather list a command table points to.
// Each 16-byte PRDT entry is {DBA (low 32 bits of the data buffer GPA),
// DBAU (high 32 bits -- always 0 here, see the CAP.S64A=0 comment in
// ahciInitAbarRegisters), Reserved, DBC (byte count - 1, bits 21-0)}.
UINT32 ahciScatterToPrdt(unsigned char *prdt, UINT16 prdtl, unsigned char *src, UINT32 srcLen) {
    UINT32 copied = 0;
    UINT16 i;
    for (i = 0; i < prdtl && copied < srcLen; i++) {
        unsigned char *entry = prdt + (UINT64)i * 16;
        UINT32 dba = *(UINT32 *)(entry + 0x00);
        UINT32 dbc = (*(UINT32 *)(entry + 0x0C) & 0x3FFFFF) + 1;
        if (dba == 0 || dba >= guestMemSize) break;
        UINT32 chunk = dbc;
        if (chunk > srcLen - copied) chunk = srcLen - copied;
        if ((UINT64)dba + chunk > guestMemSize) chunk = (UINT32)(guestMemSize - dba);
        memcpy((unsigned char *)guestMemory + dba, src + copied, chunk);
        copied += chunk;
    }
    return copied;
}

UINT32 ahciGatherFromPrdt(unsigned char *prdt, UINT16 prdtl, unsigned char *dst, UINT32 dstCap) {
    UINT32 copied = 0;
    UINT16 i;
    for (i = 0; i < prdtl && copied < dstCap; i++) {
        unsigned char *entry = prdt + (UINT64)i * 16;
        UINT32 dba = *(UINT32 *)(entry + 0x00);
        UINT32 dbc = (*(UINT32 *)(entry + 0x0C) & 0x3FFFFF) + 1;
        if (dba == 0 || dba >= guestMemSize) break;
        UINT32 chunk = dbc;
        if (chunk > dstCap - copied) chunk = dstCap - copied;
        if ((UINT64)dba + chunk > guestMemSize) chunk = (UINT32)(guestMemSize - dba);
        memcpy(dst + copied, (unsigned char *)guestMemory + dba, chunk);
        copied += chunk;
    }
    return copied;
}

// Called once per main-loop iteration (like deliverPendingAtaIrq). Checked
// from the host side rather than trapped per-MMIO-access -- see the design
// note in the AHCI plan: WHV's MemoryAccess exit doesn't decode the
// faulting instruction the way IoPortAccess does, so trapping every
// register touch would need a small x86 instruction decoder. Backing ABAR
// with real RAM and polling it here instead works because EDK2's AHCI
// driver itself polls PxCI/PxTFD for command completion during boot rather
// than relying on interrupts.
static void ahciServicePort(WHV_PARTITION_HANDLE partition, unsigned char *abar,
                            int portIndex, AhciPortDevice *dev);

void ahciProcessPendingCommands(WHV_PARTITION_HANDLE partition) {
    int p;
    unsigned char *abar;
    UINT32 ghc;
    if (!ahciAbarMapped || ahciPortsImplemented() == 0) return;
    abar = (unsigned char *)ahciAbarMemory;

    ghc = *(UINT32 *)(abar + 0x04);
    if (ghc & 0x1) { // GHC.HR: HBA reset requested -- completes instantly
        ahciInitAbarRegisters(abar);
        return;
    }
    if (!(ghc & 0x80000000)) return; // GHC.AE not set, controller not enabled yet
    {
        static int aeLogged = 0;
        if (!aeLogged) { aeLogged = 1; printf("[ahci] GHC.AE set -- controller enabled\n"); fflush(stdout); }
    }

    // Every implemented port is serviced on each pass. Each has its own command
    // list, FIS receive area and backing file, so they are entirely independent
    // -- the guest can have a read outstanding on the ISO and a write on the
    // disk at the same time, which is exactly what an install does.
    for (p = 0; p < AHCI_PORT_COUNT; p++) {
        if (!ahciPorts[p].present) continue;
        ahciServicePort(partition, abar, p, &ahciPorts[p]);
    }
}

static void ahciServicePort(WHV_PARTITION_HANDLE partition, unsigned char *abar,
                            int portIndex, AhciPortDevice *dev) {
    unsigned char *port = abar + 0x100 + (UINT32)portIndex * 0x80;
    UINT32 cmd = *(UINT32 *)(port + 0x18); // PxCMD

    // Mirror ST<->CR and FRE<->FR in both directions so "wait for engine
    // running" AND "wait for engine stopped" polling loops both see the
    // state they expect. AhciStopCommand() (called by EDK2's AHCI driver
    // after every single ATA command, including IDENTIFY) clears ST and
    // then polls CR waiting for it to clear -- missing the clear-on-stop
    // direction here left CR stuck at 1 forever once first set, hanging
    // that poll for its full real timeout after every command and
    // preventing any command after the first from ever being reached.
    UINT32 newCmd = cmd;
    if ((cmd & 0x1) && !(cmd & 0x8000)) newCmd |= 0x8000;  // ST -> CR
    else if (!(cmd & 0x1) && (cmd & 0x8000)) {
        newCmd &= ~0x8000u; // !ST -> !CR
        // The driver only clears ST (via AhciStopCommand) *after* it has
        // already read/consumed whatever PxIS state mattered for the
        // command that just ran -- so this transition is the correct,
        // guest-observable moment to clear DHRS too. Doing it here instead
        // of on a fixed real-time delay or "next command" boundary avoids
        // a race we hit with both of those: a fast guest-side retry loop
        // (Stop -> Disable -> Build -> Start, all effectively instant
        // relative to real time) could restart a new command before either
        // a short timer or our own next poll ever got to clear the old
        // bit, so AhciCheckFisReceived kept reading the *previous*
        // command's stale DHRS as this new command's completion, saw the
        // resulting byte count mismatch, and retried forever.
        *(UINT32 *)(port + 0x10) = 0;
    }
    int freJustEnabled = 0;
    if ((cmd & 0x10) && !(cmd & 0x4000)) { newCmd |= 0x4000; freJustEnabled = 1; } // FRE -> FR
    else if (!(cmd & 0x10) && (cmd & 0x4000)) newCmd &= ~0x4000u; // !FRE -> !FR
    if (newCmd != cmd) { *(UINT32 *)(port + 0x18) = newCmd; cmd = newCmd; }

    // U57: deliver the initial Device-to-Host Register FIS when the driver turns
    // on FIS reception.
    //
    // On real hardware the controller posts a D2H Register FIS into the port's FIS
    // receive area as soon as FRE is set, carrying the attached device's signature.
    // That is how a driver learns a device is actually there. We never wrote the
    // receive area at all, so storahci enabled FRE, set ST, waited for a FIS that
    // never arrived, timed out, stopped the port and retried -- observed as PxCMD
    // cycling between 0xC013 (started) and 0x0002 (stopped) with PxCI never once
    // set, i.e. it never got far enough to issue a single command. EDK2's driver
    // was unaffected because it polls PxTFD/PxCI directly rather than waiting on
    // the receive area, which is why firmware disk access always worked.
    //
    // Layout: the D2H Register FIS lives at offset 0x40 in the receive area, is 20
    // bytes, and its sector-count/LBA fields ARE the signature -- count=1, LBA
    // low=1, mid=0, high=0 gives the 0x00000101 of a non-ATAPI SATA disk, matching
    // the PxSIG we already report.
    if (freJustEnabled) {
        UINT32 fb = *(UINT32 *)(port + 0x08); // PxFB (32-bit; CAP.S64A=0)
        if (fb != 0 && fb + 0x60 < guestMemSize) {
            unsigned char *rfis = (unsigned char *)guestMemory + fb + 0x40;
            memset(rfis, 0, 20);
            rfis[0] = 0x34;  // FIS type: Register Device to Host
            rfis[1] = 0x40;  // I bit set -- this FIS raises an interrupt
            rfis[2] = 0x50;  // Status: DRDY | DSC
            rfis[3] = 0x00;  // Error: none
            rfis[4] = 0x01;  // LBA low  -> signature byte
            rfis[5] = 0x00;  // LBA mid
            rfis[6] = 0x00;  // LBA high
            rfis[7] = 0x00;  // Device
            rfis[12] = 0x01; // Sector count low -> signature byte
            *(UINT32 *)(port + 0x20) = 0x00000050; // PxTFD reflects the FIS status
            *(UINT32 *)(port + 0x10) |= 0x1;       // PxIS.DHRS -- D2H FIS received
            // BOUNDED. The driver stops and restarts a port around every
            // command, so this fires constantly -- and when stdout is a console
            // each printf blocks the VM thread on console rendering, which is
            // enough to distort the very boot being measured. A handful per port
            // is all that is needed to confirm the device was announced.
            static int d2hLogged[AHCI_PORT_COUNT];
            if (d2hLogged[portIndex] < 3) {
                d2hLogged[portIndex]++;
                printf("[ahci] port %d: posted initial D2H Register FIS to PxFB=0x%X "
                       "(signature 0x00000101)\n", portIndex, fb);
                fflush(stdout);
            }
        }
    }

    if (!(cmd & 0x1)) return; // ST not set, port not started
    {
        static int stLogged = 0;      // bitmask, one bit per port
        if (!(stLogged & (1 << portIndex))) {
            stLogged |= (1 << portIndex);
            printf("[ahci] PxCMD.ST set -- port %d started (%s)\n",
                   portIndex, dev->path);
            fflush(stdout);
        }
    }

    UINT32 ci = *(UINT32 *)(port + 0x38); // PxCI
    // What the engine SEES each pass while the boot application is running. The
    // driver demonstrably writes PxCI=1, yet no command is ever processed and
    // nothing is dropped -- so the engine must be returning before that point,
    // and this says with which values.
    if (g_bootImgRunning && g_bootImgSvcLogged < 40) {
        g_bootImgSvcLogged++;
        printf("[ahci-svc] port %d: PxCMD=0x%08X (ST=%d) PxCI=0x%08X\n",
               portIndex, cmd, (int)(cmd & 1), ci);
        fflush(stdout);
    }
    if (ci == 0) return;

    int slot = -1, i;
    for (i = 0; i < 32; i++) { if (ci & (1u << i)) { slot = i; break; } }
    if (slot < 0) return;

    // Clear any stale PxIS bits left over from the previous command before
    // processing this one -- see the DHRS comment at the end of this
    // function for why this replaced the old "pulse" approach.
    *(UINT32 *)(port + 0x10) = 0;

    // These two bail-outs used to be SILENT: they clear PxCI and return without
    // counting or logging anything, so a command dropped here is indistinguishable
    // from one that was never issued. That is exactly the state the boot
    // application's failure presents as -- PxCI written with a slot bit, and our
    // command total not moving. Say so.
    UINT32 clb = *(UINT32 *)(port + 0x00); // PxCLB (32-bit only -- CAP.S64A=0)
    if (clb == 0 || clb >= guestMemSize) {
        UINT32 clbu = *(UINT32 *)(port + 0x04);
        g_ahciDropped++;
        if (g_ahciDropped <= 10) {
            printf("[ahci-drop] port %d slot %d: PxCLB=0x%08X PxCLBU=0x%08X out of range "
                   "(guest RAM %lluMB) -- command discarded\n",
                   portIndex, slot, clb, clbu, (unsigned long long)(guestMemSize / (1024 * 1024)));
            fflush(stdout);
        }
        *(UINT32 *)(port + 0x38) &= ~(1u << slot);
        return;
    }

    unsigned char *cmdHeader = (unsigned char *)guestMemory + clb + (UINT64)slot * 32;
    UINT16 prdtl = *(UINT16 *)(cmdHeader + 0x02);
    UINT32 ctba = *(UINT32 *)(cmdHeader + 0x08);

    if (ctba == 0 || ctba >= guestMemSize) {
        g_ahciDropped++;
        if (g_ahciDropped <= 10) {
            printf("[ahci-drop] port %d slot %d: CTBA=0x%08X out of range (PxCLB=0x%08X, "
                   "guest RAM %lluMB) -- command discarded\n",
                   portIndex, slot, ctba, clb, (unsigned long long)(guestMemSize / (1024 * 1024)));
            fflush(stdout);
        }
        *(UINT32 *)(port + 0x38) &= ~(1u << slot);
        return;
    }
    unsigned char *cmdTable = (unsigned char *)guestMemory + ctba;
    unsigned char *cfis = cmdTable; // Register H2D FIS at command table offset 0
    unsigned char ataCmd = cfis[2]; // byte 2 of a Register H2D FIS: the ATA command
    UINT32 lbaLow = (UINT32)cfis[4] | ((UINT32)cfis[5] << 8) | ((UINT32)cfis[6] << 16);
    UINT32 lbaHigh = (UINT32)cfis[8] | ((UINT32)cfis[9] << 8) | ((UINT32)cfis[10] << 16);
    // 28-bit vs 48-bit addressing decide where the HIGH LBA bits live, and getting
    // this wrong silently corrupts the disk rather than failing.
    //
    // 48-bit (EXT) commands put LBA 47:24 in the "expanded" registers cfis[8..10].
    // 28-bit commands do NOT use those at all -- they are zero -- and carry LBA
    // 27:24 in the low nibble of the DEVICE register, cfis[7]. We were reading
    // cfis[8..10] unconditionally, so for a 28-bit command the top four bits were
    // dropped and every access above LBA 0xFFFFFF (8GB) ALIASED back into the
    // first 8GB, overwriting data already written there.
    //
    // Windows' storahci uses 28-bit commands here exclusively -- measured 1306x
    // 0x20, 572x 0xC8, 71x 0xCA and not a single 0x25/0x35 -- and the highest LBA
    // ever observed was exactly 0xFFFFFF, which was us truncating rather than the
    // guest's real request. Symptom: Windows Setup reached 96% and then failed
    // with 0x80070570 (ERROR_FILE_CORRUPT), because files written past the 8GB
    // mark landed on top of earlier ones. The same ISO installs fine under VMware.
    //
    // Sector count is 8-bit for 28-bit commands and 16-bit only for EXT, for the
    // same reason: cfis[13] is an expanded register.
    int is48Bit = (ataCmd == 0x24 || ataCmd == 0x34 || ataCmd == 0x25 || ataCmd == 0x35 ||
                   ataCmd == 0x29 || ataCmd == 0x39 || ataCmd == 0x2A || ataCmd == 0x3A);
    UINT32 sectorCount = is48Bit ? ((UINT32)cfis[12] | ((UINT32)cfis[13] << 8))
                                 : (UINT32)cfis[12];
    // ATA's classic "0 means max" sector-count convention -- but the max
    // differs by addressing mode: 28-bit commands (0x20/0x30/0xC8/0xCA) use
    // an 8-bit-derived count where 0 means 256; 48-bit EXT commands
    // (0x24/0x34/0x25/0x35) use the full 16-bit field where 0 means 65536.
    // Treating every 0 as "1 sector" (the previous behavior) silently
    // under-delivered on any request that legitimately meant the max size
    // -- confirmed via a captured PRDT expecting 131072 bytes (256
    // sectors) for a request whose CFIS sector count was 0, while we were
    // only transferring 512 bytes and leaving the rest of the guest's
    // buffer untouched (effectively garbage), corrupting real file reads.
    if (sectorCount == 0) sectorCount = is48Bit ? 65536 : 256;
    // THE FIX: take LBA 27:24 from the device register for 28-bit commands, and
    // only use the expanded registers for EXT. See the addressing comment above.
    UINT64 lba = is48Bit ? (((UINT64)lbaHigh << 24) | lbaLow)
                         : ((((UINT64)cfis[7] & 0x0F) << 24) | lbaLow);

    unsigned char *prdt = cmdTable + 0x80; // PRDT starts at command table offset 0x80
    UINT32 bytesTransferred = 0;
    int ok = 1;

    {
        // U55: per-opcode tallies. The log line below is capped at 200 entries,
        // which made it look like the guest only ever issued 199 read commands --
        // the real count is thousands. Count reads and writes separately and
        // uncapped, because "has the installer written anything to the disk yet"
        // is the actual question and a truncated log cannot answer it.
        //   0x20/0x24/0xC8/0x25 = READ variants, 0x30/0x34/0xCA/0x35 = WRITE variants
        ahciCmdLogCount++;
        // U62: total sectors moved, split by who issued the command. Windows only
        // issues ~21 commands before going idle, which is suspiciously small -- the
        // installer's boot.wim alone is hundreds of MB -- so knowing whether the
        // guest has read 2MB or 400MB says whether it finished loading and stopped,
        // or never got the bulk of its image at all.
        if (g_bpModuleBase) g_ahciGuestSectors += sectorCount;
        else                g_ahciFwSectors += sectorCount;
        if (ataCmd == 0x20 || ataCmd == 0x24 || ataCmd == 0xC8 || ataCmd == 0x25) g_ahciReads++;
        else if (ataCmd == 0x30 || ataCmd == 0x34 || ataCmd == 0xCA || ataCmd == 0x35) g_ahciWrites++;
        else g_ahciOther++;
        // Cap raised from 200. Firmware consumes every one of the first couple
        // of hundred on ISO9660 volume-descriptor and directory reads, so the
        // END of the conversation -- what the boot manager asked for immediately
        // before returning EFI_TIMEOUT -- has never been visible. Safe to raise
        // because stdout here is redirected to a file, not a console; it is
        // console rendering that blocks the VM thread, not the printf itself.
        if (ahciCmdLogCount < AHCI_CMD_LOG_MAX) {
            // The port index was missing here, which made every LBA in this log
            // ambiguous: the same number means a different byte offset on the
            // 2048-byte ISO than on the 512-byte disk, and there was no way to
            // tell which device a read belonged to at all.
            printf("[ahci] cmd #%d: port %d (%u-byte) ataCmd=0x%02X lba=%llu count=%u prdtl=%u\n",
                   ahciCmdLogCount, portIndex, dev->sectorSize, ataCmd,
                   (unsigned long long)lba, sectorCount, prdtl);
            fflush(stdout);
        }
        // U60: the log above is capped at 200 and firmware consumes every slot, so
        // the commands WINDOWS issues have never been visible. Log those separately,
        // gated on the kernel being loaded, with its own budget. This is the
        // question U60 exists to answer: storahci issues commands now (U59), so
        // which ones, and do they complete cleanly?
        if (LOCALHOST_VERBOSE_DIAG && g_bpModuleBase && g_ahciGuestCmdLogged < U60_GUEST_CMD_LOG_MAX) {
            g_ahciGuestCmdLogged++;
            printf("[u60] guest cmd #%d: ataCmd=0x%02X (%s) lba=%llu count=%u prdtl=%u slot=%d\n",
                   g_ahciGuestCmdLogged, ataCmd, ataCmdName(ataCmd),
                   (unsigned long long)lba, sectorCount, prdtl, slot);
            fflush(stdout);
        }
    }

    if (ataCmd == 0xEC) { // IDENTIFY DEVICE
        unsigned char idBuf[512];
        ahciFillIdentify(idBuf, dev->sectors, dev->sectorSize);
        bytesTransferred = ahciScatterToPrdt(prdt, prdtl, idBuf, 512);
    } else if (ataCmd == 0x25 || ataCmd == 0xC8 || ataCmd == 0x20 || ataCmd == 0x24) {
        // READ DMA EXT / READ DMA / READ SECTORS / READ SECTORS EXT -- AHCI
        // always moves data via the PRDT regardless of which ATA command
        // nominally requested it (there's no real distinction between "PIO"
        // and "DMA" at the AHCI protocol level, only in what the driver
        // calls it), so 0x20/0x24 need the same real read as 0x25/0xC8.
        // AtaBusDxe uses 0x20 for its own PIO-mode reads once it's done its
        // own device probing/mode negotiation -- confirmed via trace after
        // fixing the DHRS staleness bug: without this, those reads
        // silently returned no data at all.
        if ((lba + sectorCount) > dev->sectors || (UINT64)sectorCount * dev->sectorSize > ATA_MAX_TRANSFER) {
            ok = 0;
        } else {
            UINT32 want = sectorCount * dev->sectorSize;
            size_t got;
            _fseeki64(dev->file, (long long)lba * dev->sectorSize, SEEK_SET);
            got = fread(ataDataBuffer, 1, (size_t)want, dev->file);
            // fread's result used to be discarded. A short read leaves the tail of
            // ataDataBuffer holding the PREVIOUS command's bytes, and we would
            // scatter all of it anyway -- handing the guest a buffer that is part
            // fresh data and part stale, with nothing anywhere reporting a problem.
            // Zero the shortfall so the failure is at least honest, and say so.
            if (got < want) {
                memset(ataDataBuffer + got, 0, want - got);
                g_ahciShortReads++;
                if (g_ahciShortReads <= 10) {
                    printf("[ahci-short] port %d lba=%llu count=%u: wanted %u bytes, file gave %llu\n",
                           portIndex, (unsigned long long)lba, sectorCount, want,
                           (unsigned long long)got);
                    fflush(stdout);
                }
            }
            bytesTransferred = ahciScatterToPrdt(prdt, prdtl, ataDataBuffer, want);
            // Same integrity check as the write side, and for the same reason:
            // scattering fewer bytes than the command asked for leaves the tail of
            // the guest's buffer holding whatever was there before, which it will
            // happily treat as file data. Silent until now. This runs for EVERY
            // read, not just the boot window below.
            if (bytesTransferred != want) {
                g_ahciBadReads++;
                if (g_ahciBadReads <= 20) {
                    printf("[ahci-badread] port %d lba=%llu count=%u: asked %u, scattered %u (prdtl=%u)\n",
                           portIndex, (unsigned long long)lba, sectorCount, want,
                           bytesTransferred, prdtl);
                    fflush(stdout);
                }
            }
            // Does what the guest ASKED for (the PRDT's own byte count) match what
            // we actually delivered? A read that "succeeds" while moving the wrong
            // number of bytes is exactly the failure that makes a caller retry the
            // identical run and then give up, which is what this window does.
            if (g_bootImgRunning && g_bootImgReadLogged < 80) {
                UINT32 prdtBytes = 0;
                UINT16 pi2;
                for (pi2 = 0; pi2 < prdtl; pi2++) {
                    unsigned char *e2 = prdt + (UINT64)pi2 * 16;
                    prdtBytes += (*(UINT32 *)(e2 + 0x0C) & 0x3FFFFF) + 1;
                }
                g_bootImgReadLogged++;
                printf("[bootimg-read] lba=%-7llu count=%-3u want=%-6u prdt=%-6u delivered=%-6u %s\n",
                       (unsigned long long)lba, sectorCount, want, prdtBytes, bytesTransferred,
                       (prdtBytes == bytesTransferred && bytesTransferred == want) ? "" : "<-- MISMATCH");
                fflush(stdout);
            }
        }
    } else if (ataCmd == 0x35 || ataCmd == 0xCA || ataCmd == 0x30 || ataCmd == 0x34) {
        // WRITE DMA EXT / WRITE DMA / WRITE SECTORS / WRITE SECTORS EXT -- see
        // the read-side comment above for why the PIO opcodes need the same
        // handling as their DMA counterparts here.
        if ((lba + sectorCount) > dev->sectors || (UINT64)sectorCount * dev->sectorSize > ATA_MAX_TRANSFER) {
            ok = 0;
        } else if (dev->sectorSize != 512) {
            // Read-only medium: an ISO is opened "rb" and must never be written.
            // Reported as an ABRT rather than silently succeeding, so a driver
            // that tries gets a real error instead of believing the write landed.
            ok = 0;
        } else {
            UINT32 want = sectorCount * dev->sectorSize;
            UINT32 gathered = ahciGatherFromPrdt(prdt, prdtl, ataDataBuffer, want);
            size_t wrote;
            _fseeki64(dev->file, (long long)lba * dev->sectorSize, SEEK_SET);
            wrote = fwrite(ataDataBuffer, 1, gathered, dev->file);
            fflush(dev->file);
            // INTEGRITY. Windows Setup fails with 0x80070570 (ERROR_FILE_CORRUPT),
            // and the same ISO installs cleanly under VMware -- so the media is
            // good and we are handing the guest wrong bytes somewhere. Both of
            // these were silent: a PRDT that gathers less than the command asked
            // for writes a short block, and fwrite's return value was never
            // checked at all, so a short write to the host file looked identical
            // to success. Either one corrupts a file the guest later verifies.
            if (gathered != want || wrote != (size_t)gathered) {
                g_ahciBadWrites++;
                if (g_ahciBadWrites <= 20) {
                    printf("[ahci-badwrite] port %d lba=%llu count=%u: asked %u, gathered %u, fwrote %llu (prdtl=%u)\n",
                           portIndex, (unsigned long long)lba, sectorCount, want, gathered,
                           (unsigned long long)wrote, prdtl);
                    fflush(stdout);
                }
            }
            bytesTransferred = gathered;
        }
    } else if (ataCmd == 0xEA || ataCmd == 0xE7) { // FLUSH CACHE EXT / FLUSH CACHE
        fflush(dev->file);
    }
    // Other commands (e.g. SET FEATURES) are silently accepted as no-ops --
    // matches how the boot-time driver doesn't strictly need them to succeed.

    // A command we ABORT is invisible otherwise, and the failure mode it causes
    // is indistinguishable from a hang: the firmware retries, gets ERR|ABRT
    // again, and eventually reports a timeout with no indication that WE
    // refused it. A 1.1MB EFI binary was once lost to exactly this (silently
    // rejected for exceeding the transfer cap), so count and name it.
    if (!ok) {
        g_ahciRejected++;
        if (g_ahciRejected <= 12) {
            printf("[ahci-reject] port %d cmd=0x%02X lba=%llu count=%u (sectorSize=%u, "
                   "bytes=%llu, cap=%u, devSectors=%llu) -- answered ERR|ABRT\n",
                   portIndex, ataCmd, (unsigned long long)lba, sectorCount,
                   dev->sectorSize, (unsigned long long)sectorCount * dev->sectorSize,
                   ATA_MAX_TRANSFER, (unsigned long long)dev->sectors);
            fflush(stdout);
        }
    }

    *(UINT32 *)(cmdHeader + 0x04) = bytesTransferred; // PRDBC: bytes actually transferred
    *(UINT32 *)(port + 0x38) &= ~(1u << slot);         // PxCI: clear -- command complete
    *(UINT32 *)(port + 0x20) = ok ? 0x00000050 : 0x00000451; // PxTFD: DRDY|DSC, or ERR|ABRT
    // U60: pair each logged guest command with how it actually finished, including
    // how many bytes we moved versus what the PRDT asked for. A command that
    // "completes" while transferring the wrong length is exactly the kind of thing
    // that makes a driver retry forever without ever reporting an error.
    if (LOCALHOST_VERBOSE_DIAG && g_bpModuleBase && g_ahciGuestCmdLogged <= U60_GUEST_CMD_LOG_MAX && g_ahciGuestCmdLogged > 0) {
        printf("[u60]   -> %s bytesTransferred=%u PxTFD=0x%08X PxIS=0x%08X\n",
               ok ? "OK" : "ERROR(ABRT)", bytesTransferred,
               *(UINT32 *)(port + 0x20), *(UINT32 *)(port + 0x10));
        fflush(stdout);
    }
    // PxIS: DHRS. Left set (not auto-cleared on the next poll) until the
    // next command starts, unlike the earlier "pulse" approach -- ABAR is
    // untrapped guest RAM polled once per host main-loop iteration, which
    // can run many times faster than the guest's own polling interval
    // (EDK2's AhciWaitUntilFisReceived polls every 100us). Clearing the bit
    // that fast risked the guest never sampling it set at all, which -- for
    // AhciPioTransfer's PIO-read wait on SataFisPioSetup specifically,
    // which AhciCheckFisReceived accepts DHRS as satisfying -- meant
    // IDENTIFY DEVICE's own completion wait timed out, AhciIdentify
    // returned EFI_TIMEOUT, and AhciModeInitialization's per-port loop
    // treated that as EFI_ERROR and skipped the device via `continue`
    // before ever reaching SetFeature or CreateNewDeviceInfo -- the actual
    // root cause of "no READ command after IDENTIFY", confirmed against
    // EDK2's real AhciMode.c source rather than guessed.
    *(UINT32 *)(port + 0x10) |= 0x1;
}

// Handles I/O ports 0x3F8-0x3FF (COM1) -- see uartIer's comment above for
// what this stub does and doesn't model. DLAB (Line Control Register bit
// 7) is honored so baud-rate divisor programming doesn't get misread as
// characters to transmit: while set, 0x3F8/0x3F9 address the divisor
// latch instead of THR/RBR and IER.
void uartHandleAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitContext->IoPortAccess;
    UINT16 port = io->PortNumber;
    UINT64 rax = io->Rax;
    int dlab = (uartLcr & 0x80) != 0;

    if (io->AccessInfo.IsWrite) {
        unsigned char val = (unsigned char)io->Rax;
        switch (port) {
            case 0x3F8: if (dlab) uartDivisorLow = val; else appendToLog((char)val); break;
            case 0x3F9: if (dlab) uartDivisorHigh = val; else uartIer = val; break;
            case 0x3FA: break; // FCR: FIFO control, no real FIFO to configure
            case 0x3FB: uartLcr = val; break;
            case 0x3FC: uartMcr = val; break;
            case 0x3FF: uartScr = val; break;
            default: break; // 0x3FD (LSR), 0x3FE (MSR) are read-only status
        }
    } else {
        switch (port) {
            case 0x3F8: rax = dlab ? uartDivisorLow : 0x00; break; // RBR: no received data available
            case 0x3F9: rax = dlab ? uartDivisorHigh : uartIer; break;
            case 0x3FA: rax = 0x01; break; // IIR: no interrupt pending
            case 0x3FB: rax = uartLcr; break;
            case 0x3FC: rax = uartMcr; break;
            case 0x3FD: rax = 0x60; break; // LSR: THRE|TEMT set, always ready to transmit
            case 0x3FE: rax = 0xB0; break; // MSR: CTS|DSR|DCD asserted
            case 0x3FF: rax = uartScr; break;
            default: rax = 0xFF; break;
        }
    }

    WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
    WHV_REGISTER_VALUE values[2] = { 0 };
    values[0].Reg64 = rax;
    values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
    WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
}

// Runs for the lifetime of the process once a debug session is enabled.
// ConnectNamedPipe blocks (synchronously, on this dedicated thread) until
// WinDbg attaches, then ReadFile blocks until either more bytes arrive or
// the pipe breaks (debugger detached) -- at which point it loops back to
// wait for a fresh connection, so a WinDbg restart doesn't require
// restarting the hypervisor.
DWORD WINAPI kdPipeReaderThread(LPVOID param) {
    (void)param;
    unsigned char buf[256];
    static int outerLoopCount = 0;
    // U68: kdPipe is now an OVERLAPPED handle, so ConnectNamedPipe and ReadFile
    // return immediately with ERROR_IO_PENDING and we wait on the event
    // ourselves. Each call needs its OVERLAPPED zeroed apart from hEvent.
    OVERLAPPED ov;
    HANDLE ovEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ovEvent) return 1;
    for (;;) {
        outerLoopCount++;
        if (outerLoopCount <= 20) {
            printf("[kd-reader] outer loop #%d: calling ConnectNamedPipe\n", outerLoopCount);
            fflush(stdout);
        }
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = ovEvent;
        ResetEvent(ovEvent);
        BOOL connected = ConnectNamedPipe(kdPipe, &ov);
        DWORD connectErr = connected ? 0 : GetLastError();
        if (!connected && connectErr == ERROR_IO_PENDING) {
            WaitForSingleObject(ovEvent, INFINITE);
            DWORD dummy = 0;
            connected = GetOverlappedResult(kdPipe, &ov, &dummy, FALSE);
            connectErr = connected ? 0 : GetLastError();
        }
        if (!connected && connectErr != ERROR_PIPE_CONNECTED) {
            if (outerLoopCount <= 20) {
                printf("[kd-reader] ConnectNamedPipe failed, err=%lu -- sleeping\n", connectErr);
                fflush(stdout);
            }
            Sleep(200);
            continue;
        }
        if (outerLoopCount <= 20) {
            printf("[kd-reader] connected (connectErr=%lu), entering read loop\n", connectErr);
            fflush(stdout);
        }
        printf("[kd] WinDbg connected via \\\\.\\pipe\\LocalHostKD\n");
        fflush(stdout);
        kdClientConnected = 1;
        int innerReadCount = 0;
        for (;;) {
            DWORD bytesRead = 0;
            ZeroMemory(&ov, sizeof(ov));
            ov.hEvent = ovEvent;
            ResetEvent(ovEvent);
            BOOL ok = ReadFile(kdPipe, buf, sizeof(buf), &bytesRead, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                WaitForSingleObject(ovEvent, INFINITE);
                ok = GetOverlappedResult(kdPipe, &ov, &bytesRead, FALSE);
            }
            DWORD readErr = ok ? 0 : GetLastError();
            innerReadCount++;
            if (outerLoopCount <= 20 && innerReadCount <= 60) {
                printf("[kd-reader] ReadFile #%d: ok=%d bytesRead=%lu err=%lu bytes:",
                       innerReadCount, ok, bytesRead, readErr);
                for (DWORD bi = 0; bi < bytesRead; bi++) printf(" %02X", buf[bi]);
                printf("\n");
                fflush(stdout);
            }
            if (!ok || bytesRead == 0) break; // pipe broke -- debugger detached
            EnterCriticalSection(&kdRxLock);
            for (DWORD i = 0; i < bytesRead; i++) {
                int next = (kdRxHead + 1) % (int)sizeof(kdRxBuf);
                if (next != kdRxTail) { kdRxBuf[kdRxHead] = buf[i]; kdRxHead = next; g_kdRxFromPipe++; }
                else g_kdRxDropped++;
            }
            LeaveCriticalSection(&kdRxLock);
        }
        kdClientConnected = 0;
        printf("[kd] WinDbg disconnected -- waiting for reconnect\n");
        fflush(stdout);
        DisconnectNamedPipe(kdPipe);
    }
    return 0;
}

// Drains kdTxBuf (filled by the guest's COM2 transmit-register writes) and
// calls the actual blocking WriteFile on this dedicated thread -- never on
// the main VM thread. WriteFile on a named pipe with no connected client
// blocks indefinitely, and doing that on the thread that also services
// WHvRunVirtualProcessor would freeze the entire guest (and did, before this
// was split out): the guest kept polling COM2 waiting for a reply while the
// host was itself stuck inside the write it needed to complete first.
DWORD WINAPI kdPipeWriterThread(LPVOID param) {
    (void)param;
    unsigned char buf[256];
    OVERLAPPED ov;
    HANDLE ovEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ovEvent) return 1;
    for (;;) {
        WaitForSingleObject(kdTxEvent, INFINITE);
        for (;;) {
            int count = 0;
            EnterCriticalSection(&kdTxLock);
            while (kdTxTail != kdTxHead && count < (int)sizeof(buf)) {
                buf[count++] = kdTxBuf[kdTxTail];
                kdTxTail = (kdTxTail + 1) % (int)sizeof(kdTxBuf);
            }
            LeaveCriticalSection(&kdTxLock);
            if (count == 0) break;

            // Nothing here may block indefinitely. A debugger that has stopped
            // reading (kd after [no_debuggee]) leaves the pipe permanently full;
            // the previous synchronous WriteFile parked this thread inside it
            // forever and silently binned 32231 guest bytes. Bounded wait, then
            // cancel and treat the bytes as a transmit overrun -- which is what
            // a real 16550 does when the far end never asserts CTS.
            DWORD written = 0;
            ZeroMemory(&ov, sizeof(ov));
            ov.hEvent = ovEvent;
            ResetEvent(ovEvent);
            BOOL ok = WriteFile(kdPipe, buf, count, &written, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ovEvent, 250) == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(kdPipe, &ov, &written, FALSE);
                } else {
                    CancelIoEx(kdPipe, &ov);
                    // Reap the cancelled request so the OVERLAPPED can be reused.
                    GetOverlappedResult(kdPipe, &ov, &written, TRUE);
                    ok = FALSE;
                    SetLastError(WAIT_TIMEOUT);
                }
            }
            if (ok) {
                g_kdTxToPipe += (long)written;
            } else {
                g_kdTxWriteFail++;
                g_kdTxLastErr = GetLastError();
            }
        }
    }
    return 0;
}

// Handles I/O ports 0x2F8-0x2FF (COM2) -- same 16550 register layout as
// uartHandleAccess (COM1), but transmit/receive go through the named-pipe
// bridge to a real debugger instead of our text log. See the kdPipe globals'
// comment for why this is a separate port range from COM1.
void uart2HandleAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitContext->IoPortAccess;
    UINT16 port = io->PortNumber;
    UINT64 rax = io->Rax;
    int dlab = (uart2Lcr & 0x80) != 0;

    // TEMP DIAGNOSTIC: confirm the guest is actually touching COM2 at all
    // (vs. relying on an ACPI DBG2 table to discover the debug UART, which
    // we don't provide) before investing further in the KD handshake.
    static int uart2AccessCount = 0;
    static int uart2PostConnectCount = 0;
    int logPostConnect = 0;
    // U63: count DATA-PORT WRITES separately and uncapped, plus tally the KD
    // packet leaders. The 40-entry cap below made "0 KD bytes on COM2" look like a
    // finding twice, when it only meant the log had stopped -- the fifth time a cap
    // has produced a false conclusion in this investigation (after the IOAPIC op
    // counts, PCI accesses after discovery, AHCI command totals and the IRQ
    // injection storm). KD framing: data packets lead with 0x30 ('0' x4), control
    // packets with 0x69 ('i' x4), and a breakin byte is 0x62 ('b').
    if (port == 0x2F8 && io->AccessInfo.IsWrite && !dlab) {
        unsigned char b = (unsigned char)io->Rax;
        g_uart2TxTotal++;
        if (b == 0x30) g_uart2Tx30++;
        else if (b == 0x69) g_uart2Tx69++;
        else if (b == 0x62) g_uart2Tx62++;
    }
    if (uart2AccessCount < 40) {
        uart2AccessCount++;
        printf("[uart2] #%d port=0x%X write=%d val=0x%llX\n", uart2AccessCount, port,
               io->AccessInfo.IsWrite, (unsigned long long)io->Rax);
        fflush(stdout);
    } else if (kdClientConnected && uart2PostConnectCount < 200000) {
        logPostConnect = 1;
    }

    if (io->AccessInfo.IsWrite) {
        unsigned char val = (unsigned char)io->Rax;
        switch (port) {
            case 0x2F8:
                if (dlab) {
                    uart2DivisorLow = val;
                } else if (kdPipe != INVALID_HANDLE_VALUE) {
                    EnterCriticalSection(&kdTxLock);
                    int next = (kdTxHead + 1) % (int)sizeof(kdTxBuf);
                    if (next != kdTxTail) { kdTxBuf[kdTxHead] = val; kdTxHead = next; }
                    else g_kdTxDropped++;
                    LeaveCriticalSection(&kdTxLock);
                    SetEvent(kdTxEvent);
                }
                break;
            case 0x2F9: if (dlab) uart2DivisorHigh = val; else uart2Ier = val; break;
            // FCR: honor FIFO-enable (bit0). The guest (both OVMF and, per
            // the KD investigation, likely the kernel's serial transport
            // too) probes for 16550A FIFO support via FCR=0x07 -- always
            // answering IIR as "no FIFO" (a plain 8250/16450) regardless
            // could push FIFO-aware software onto a more conservative,
            // more heavily-polled non-FIFO code path than necessary.
            case 0x2FA: uart2FifoEnabled = (val & 0x01) ? 1 : 0; break;
            case 0x2FB: uart2Lcr = val; break;
            case 0x2FC: uart2Mcr = val; break;
            case 0x2FF: uart2Scr = val; break;
            default: break;
        }
    } else {
        switch (port) {
            case 0x2F8:
                if (dlab) {
                    rax = uart2DivisorLow;
                } else {
                    rax = 0x00;
                    EnterCriticalSection(&kdRxLock);
                    if (kdRxTail != kdRxHead) {
                        rax = kdRxBuf[kdRxTail];
                        kdRxTail = (kdRxTail + 1) % (int)sizeof(kdRxBuf);
                        g_kdRxToGuest++;
                    }
                    LeaveCriticalSection(&kdRxLock);
                }
                break;
            case 0x2F9: rax = dlab ? uart2DivisorHigh : uart2Ier; break;
            // IIR: bit0=1 (no interrupt pending, matches our polled-only
            // model), bits7:6=11 when FIFO is enabled (16550A signature) --
            // 0xC1 vs 0x01, matching real hardware's own IIR encoding.
            case 0x2FA: rax = uart2FifoEnabled ? 0xC1 : 0x01; break;
            case 0x2FB: rax = uart2Lcr; break;
            case 0x2FC: rax = uart2Mcr; break;
            case 0x2FD: { // LSR: THRE|TEMT always set (writes are synchronous); DR reflects the ring buffer
                unsigned char lsr = 0x60;
                EnterCriticalSection(&kdRxLock);
                if (kdRxTail != kdRxHead) lsr |= 0x01;
                LeaveCriticalSection(&kdRxLock);
                rax = lsr;
                break;
            }
            case 0x2FE: rax = 0xB0; break; // MSR: CTS|DSR|DCD asserted
            case 0x2FF: rax = uart2Scr; break;
            default: rax = 0xFF; break;
        }
    }

    if (logPostConnect) {
        uart2PostConnectCount++;
        unsigned char lowByte = (unsigned char)(io->AccessInfo.IsWrite ? io->Rax : rax);
        char printable = (lowByte >= 0x20 && lowByte < 0x7F) ? (char)lowByte : '.';
        // Only print the interesting events (RBR access, or LSR showing
        // Data-Ready) plus periodic heartbeats -- otherwise this floods
        // with near-identical LSR/MSR poll spam long before anything
        // relevant happens (confirmed: 400 entries wasn't enough window to
        // even reach WinDbg's first real response).
        int interesting = (port == 0x2F8) || (port == 0x2FD && (lowByte & 0x01));
        if (interesting || uart2PostConnectCount % 20000 == 0) {
            printf("[uart2-postconnect] #%d port=0x%X write=%d byte=0x%02X '%c'%s\n", uart2PostConnectCount, port,
                   io->AccessInfo.IsWrite, lowByte, printable, interesting ? " <<<" : "");
            fflush(stdout);
        }
    }

    WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
    WHV_REGISTER_VALUE values[2] = { 0 };
    values[0].Reg64 = rax;
    values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
    WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
}

// --- COM3 / Guest Tools UART (ports 0x3E8-0x3EF) ---------------------------
// Register layout is the same 16550 as COM1/COM2. The interrupt handling is
// not: see the uart3* globals for why this port needs it and COM2 does not.

#define UART3_GSI          4
#define UART3_LEGACY_VEC   0x0C   // legacy PIC mapping: IRQ4 -> 0x08 + 4

// True when the RX ring holds a byte the guest has not consumed yet.
static int uart3RxPending(void) {
    int pending;
    EnterCriticalSection(&gtRxLock);
    pending = (gtRxTail != gtRxHead);
    LeaveCriticalSection(&gtRxLock);
    return pending;
}

// The highest-priority pending AND enabled interrupt source, in 16550 IIR
// encoding. 0x00 means nothing is pending.
static unsigned char uart3PendingIir(void) {
    if ((uart3Ier & 0x01) && uart3RxPending()) return 0x04; // received data available
    if ((uart3Ier & 0x02) && uart3ThreInt)     return 0x02; // transmit holding register empty
    return 0x00;
}

// Asserts IRQ4 when an enabled source is pending.
//
// Called from the main loop rather than from the pipe reader thread on purpose.
// Interrupt injection ends in WHvSetVirtualProcessorRegisters, which must not be
// issued from another thread while the VP is running -- the reader thread only
// fills the ring, and the interrupt is raised here, on the thread that owns the
// vCPU. (rtcCancelThread is the one cross-thread exception in this file, and it
// calls WHvCancelRunVirtualProcessor, which IS documented safe.)
//
// Safe to call every iteration: queueInterrupt coalesces per vector, so a burst
// of received bytes raises one interrupt rather than one per byte.
static void uart3UpdateIrq(WHV_PARTITION_HANDLE partition) {
    // MCR bit 3 (OUT2) gates the interrupt line on a real 16550 -- it is wired
    // to the tri-state buffer between the UART and the PIC. serial.sys sets it
    // as part of opening the port, so honouring it keeps us from interrupting a
    // driver that has not finished attaching yet.
    if (!(uart3Mcr & 0x08)) return;
    if (uart3PendingIir() == 0x00) return;
    if (injectDeviceIrq(partition, UART3_GSI, UART3_LEGACY_VEC)) g_gtIrqRaised++;
}

// Host -> guest. Accepts one tools client on the pipe and feeds everything it
// sends into the RX ring. Overlapped for the same reason the KD bridge is: a
// synchronous handle lets a blocked pipe call park this thread forever.
DWORD WINAPI gtPipeReaderThread(LPVOID param) {
    (void)param;
    unsigned char buf[1024];
    OVERLAPPED ov;
    DWORD i;
    HANDLE ovEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ovEvent) return 1;
    for (;;) {
        BOOL connected;
        DWORD err;
        ZeroMemory(&ov, sizeof(ov));
        ov.hEvent = ovEvent;
        ResetEvent(ovEvent);
        connected = ConnectNamedPipe(gtPipe, &ov);
        err = GetLastError();
        if (!connected && err == ERROR_IO_PENDING) {
            DWORD dummy;
            WaitForSingleObject(ovEvent, INFINITE);
            connected = GetOverlappedResult(gtPipe, &ov, &dummy, FALSE);
        } else if (!connected && err == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        if (!connected) { Sleep(100); continue; }

        gtClientConnected = 1;
        printf("[gt] tools client connected\n");
        fflush(stdout);

        for (;;) {
            DWORD bytesRead = 0;
            BOOL ok;
            ZeroMemory(&ov, sizeof(ov));
            ov.hEvent = ovEvent;
            ResetEvent(ovEvent);
            ok = ReadFile(gtPipe, buf, sizeof(buf), &bytesRead, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                WaitForSingleObject(ovEvent, INFINITE);
                ok = GetOverlappedResult(gtPipe, &ov, &bytesRead, FALSE);
            }
            if (!ok || bytesRead == 0) break;

            EnterCriticalSection(&gtRxLock);
            for (i = 0; i < bytesRead; i++) {
                int next = (gtRxHead + 1) % (int)sizeof(gtRxBuf);
                if (next != gtRxTail) { gtRxBuf[gtRxHead] = buf[i]; gtRxHead = next; g_gtRxFromPipe++; }
                else g_gtRxDropped++;
            }
            LeaveCriticalSection(&gtRxLock);
        }

        gtClientConnected = 0;
        printf("[gt] tools client disconnected\n");
        fflush(stdout);
        DisconnectNamedPipe(gtPipe);
    }
}

// Guest -> host. Drains gtTxBuf (filled by the guest's COM3 transmit-register
// writes) onto the pipe from a DEDICATED thread, never from the VM thread.
// This is the discipline the KD bridge had to learn the hard way: a synchronous
// WriteFile against a pipe whose client has stopped reading blocks forever, and
// doing that on the thread servicing WHvRunVirtualProcessor freezes the whole
// guest. Bounded wait, then cancel and count the bytes as a transmit overrun --
// which is what a real 16550 does when the far end never asserts CTS.
DWORD WINAPI gtPipeWriterThread(LPVOID param) {
    (void)param;
    unsigned char buf[1024];
    OVERLAPPED ov;
    HANDLE ovEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!ovEvent) return 1;
    for (;;) {
        WaitForSingleObject(gtTxEvent, INFINITE);
        for (;;) {
            int count = 0;
            DWORD written = 0;
            BOOL ok;
            EnterCriticalSection(&gtTxLock);
            while (gtTxTail != gtTxHead && count < (int)sizeof(buf)) {
                buf[count++] = gtTxBuf[gtTxTail];
                gtTxTail = (gtTxTail + 1) % (int)sizeof(gtTxBuf);
            }
            LeaveCriticalSection(&gtTxLock);
            if (count == 0) break;

            // No agent running is the NORMAL case, not an error: nothing is
            // listening during boot, during Setup, or on any guest without the
            // tools installed. The host must never block or fail loudly for it.
            if (!gtClientConnected) { g_gtTxDropped += count; continue; }

            ZeroMemory(&ov, sizeof(ov));
            ov.hEvent = ovEvent;
            ResetEvent(ovEvent);
            ok = WriteFile(gtPipe, buf, count, &written, &ov);
            if (!ok && GetLastError() == ERROR_IO_PENDING) {
                if (WaitForSingleObject(ovEvent, 250) == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(gtPipe, &ov, &written, FALSE);
                } else {
                    CancelIoEx(gtPipe, &ov);
                    // Reap the cancelled request so the OVERLAPPED can be reused.
                    GetOverlappedResult(gtPipe, &ov, &written, TRUE);
                    ok = FALSE;
                    SetLastError(WAIT_TIMEOUT);
                }
            }
            if (ok) {
                g_gtTxToPipe += (long)written;
            } else {
                g_gtTxWriteFail++;
                g_gtTxLastErr = GetLastError();
            }
        }
    }
}

void uart3HandleAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitContext->IoPortAccess;
    UINT16 port = io->PortNumber;
    UINT64 rax = io->Rax;
    int dlab = (uart3Lcr & 0x80) != 0;

    if (io->AccessInfo.IsWrite) {
        unsigned char val = (unsigned char)io->Rax;
        switch (port) {
            case 0x3E8:
                if (dlab) {
                    uart3DivisorLow = val;
                } else {
                    if (gtPipe != INVALID_HANDLE_VALUE) {
                        int next;
                        EnterCriticalSection(&gtTxLock);
                        next = (gtTxHead + 1) % (int)sizeof(gtTxBuf);
                        if (next != gtTxTail) { gtTxBuf[gtTxHead] = val; gtTxHead = next; }
                        else g_gtTxDropped++;
                        LeaveCriticalSection(&gtTxLock);
                        SetEvent(gtTxEvent);
                    }
                    // The byte is gone -- queued, or dropped because nobody is
                    // listening. Either way the holding register is free again,
                    // so re-arm the THRE edge.
                    uart3ThreInt = 1;
                }
                break;
            case 0x3E9:
                if (dlab) {
                    uart3DivisorHigh = val;
                } else {
                    uart3Ier = val;
                    // Enabling the THRE interrupt while the transmitter is
                    // already idle has to raise it: serial.sys enables it and
                    // then waits, so without this the first write never starts.
                    if (val & 0x02) uart3ThreInt = 1;
                }
                break;
            case 0x3EA: uart3FifoEnabled = (val & 0x01) ? 1 : 0; break;
            case 0x3EB: uart3Lcr = val; break;
            case 0x3EC: uart3Mcr = val; break;
            case 0x3EF: uart3Scr = val; break;
            default: break;
        }
    } else {
        switch (port) {
            case 0x3E8:
                if (dlab) {
                    rax = uart3DivisorLow;
                } else {
                    rax = 0x00;
                    EnterCriticalSection(&gtRxLock);
                    if (gtRxTail != gtRxHead) {
                        rax = gtRxBuf[gtRxTail];
                        gtRxTail = (gtRxTail + 1) % (int)sizeof(gtRxBuf);
                        g_gtRxToGuest++;
                    }
                    LeaveCriticalSection(&gtRxLock);
                }
                break;
            case 0x3E9: rax = dlab ? uart3DivisorHigh : uart3Ier; break;
            case 0x3EA: {
                // IIR. Reading it reports the highest-priority pending source
                // AND clears a pending THRE -- that is what turns our
                // permanently-empty transmitter into an edge rather than a
                // level that would never deassert.
                unsigned char pending = uart3PendingIir();
                rax = (pending == 0x00) ? 0x01 : pending;  // bit0 set = none pending
                if (pending == 0x02) uart3ThreInt = 0;
                if (uart3FifoEnabled) rax |= 0xC0;         // 16550A signature
                break;
            }
            case 0x3EB: rax = uart3Lcr; break;
            case 0x3EC: rax = uart3Mcr; break;
            case 0x3ED: {
                unsigned char lsr = 0x60;   // THRE|TEMT: our transmit is synchronous
                EnterCriticalSection(&gtRxLock);
                if (gtRxTail != gtRxHead) lsr |= 0x01;   // DR: data ready
                LeaveCriticalSection(&gtRxLock);
                rax = lsr;
                break;
            }
            case 0x3EE: rax = 0xB0; break;  // MSR: CTS|DSR|DCD asserted
            case 0x3EF: rax = uart3Scr; break;
            default: rax = 0xFF; break;
        }
    }

    {
        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
        WHV_REGISTER_VALUE values[2] = { 0 };
        values[0].Reg64 = rax;
        values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
        WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
    }
}

// --- fw_cfg device (ports 0x510 selector / 0x511 data) + minimal ACPI ---
// OVMF does not synthesize ACPI tables itself -- it only builds them from
// data supplied by the platform via fw_cfg (OvmfPkg's QemuFwCfgAcpi /
// AcpiPlatformDxe, which reads a "linker/loader" script telling it how to
// place and cross-link a set of named files). Without fw_cfg present at
// all, that driver has nothing to load and installs no ACPI tables, which
// essentially every x86_64 UEFI-booting kernel needs (RSDP/XSDT/FADT/MADT)
// to enumerate CPUs and fixed hardware. This models just enough of the
// protocol to deliver a minimal table set: signature/ID probes, the file
// directory, and three named files (RSDP, a combined XSDT+FADT+MADT+DSDT
// blob, and the loader script).
#define FWCFG_KEY_SIGNATURE 0x0000
#define FWCFG_KEY_ID        0x0001
#define FWCFG_KEY_FILE_DIR  0x0019
#define FWCFG_KEY_RSDP      0x0020
#define FWCFG_KEY_TABLES    0x0021
#define FWCFG_KEY_LOADER    0x0022
#define FWCFG_KEY_RAMFB     0x0023

// --- Minimal I/O APIC MMIO emulation ---
// Added alongside the MADT I/O APIC entry (see acpiBuildTables) once a live
// guest-memory bugcheck readout (KiBugCheckData via a page-table walk,
// resolved against the real ntkrnlmp.pdb) confirmed the NT kernel's HAL
// unconditionally requires one during HalpInitSystemPhase0. This only needs
// to satisfy that enumeration/initialization contract -- the redirection
// table starts fully masked (mask bit set, matching real hardware's
// power-on default) and nothing here actually routes an interrupt; existing
// IRQ delivery still goes through the direct WHvRegisterPendingInterruption
// injection hack used everywhere else in this file. Real hardware/QEMU also
// place the I/O APIC at this exact fixed physical address.
#define IOAPIC_MMIO_BASE 0xFEC00000ULL
#define IOAPIC_MMIO_SIZE  0x20
// Second I/O APIC, GSI 24-47 -- added as an empirical follow-up once the
// single 24-entry IOAPIC (GSI 0-23) didn't clear bugcheck 0x5C
// (HAL_INITIALIZATION_FAILED, param3=0x19 unchanged across three unrelated
// MADT additions, suggesting the failure doesn't depend on MADT override
// content at all). Hypothesis: something (PCI interrupt routing, absent a
// real AML _PRT since our DSDT has no namespace content) needs a GSI beyond
// 23, which no controller could cover with only one 24-entry IOAPIC.
#define IOAPIC2_MMIO_BASE 0xFEC01000ULL
#define IOAPIC2_GSI_BASE 24

typedef struct {
    UINT32 id;
    UINT32 selectedReg;
    UINT64 redirTable[24];
} IoApicState;

IoApicState ioapic1 = { 1, 0, { 0 } };
IoApicState ioapic2 = { 2, 0, { 0 } };

UINT64 ioapicRteFor(int gsi) {
    IoApicState *ap = (gsi < 24) ? &ioapic1 : &ioapic2;
    int entry = (gsi < 24) ? gsi : (gsi - IOAPIC2_GSI_BASE);
    if (entry < 0 || entry >= (int)(sizeof(ap->redirTable) / sizeof(ap->redirTable[0]))) return 0;
    return ap->redirTable[entry];
}

UINT32 ioapicReadRegister(IoApicState *ap, UINT32 reg) {
    if (reg == 0x00) return (ap->id & 0xF) << 24;
    if (reg == 0x01) return (23u << 16) | 0x11u; // Version 0x11, Max Redirection Entry = 23 (24 entries)
    if (reg == 0x02) return 0; // Arbitration ID
    if (reg >= 0x10 && reg <= 0x3F) {
        int entry = (reg - 0x10) / 2;
        int isHigh = (reg - 0x10) % 2;
        UINT64 val = ap->redirTable[entry];
        return isHigh ? (UINT32)(val >> 32) : (UINT32)(val & 0xFFFFFFFFULL);
    }
    return 0;
}

void ioapicWriteRegister(IoApicState *ap, UINT32 reg, UINT32 value) {
    if (reg == 0x00) { ap->id = (value >> 24) & 0xF; return; }
    if (reg == 0x01 || reg == 0x02) return; // read-only
    if (reg >= 0x10 && reg <= 0x3F) {
        int entry = (reg - 0x10) / 2;
        int isHigh = (reg - 0x10) % 2;
        UINT64 old = ap->redirTable[entry];
        if (isHigh) ap->redirTable[entry] = (old & 0xFFFFFFFFULL) | ((UINT64)value << 32);
        else ap->redirTable[entry] = (old & 0xFFFFFFFF00000000ULL) | value;
        // Every write to the KEYBOARD's entry, unconditionally. The end-of-run
        // snapshot says this line is live while ~99% of keyboard IRQs were refused
        // against a masked 0x100FF, so the entry's timeline -- when it goes live,
        // and whether anything re-masks it -- is the whole question.
        if (ap == &ioapic1 && entry == GSI_KEYBOARD) {
            UINT64 now = ap->redirTable[entry];
            printf("[kbd-rte] write: 0x%016llX -> 0x%016llX (vector 0x%02X, %s) at exit %ld\n",
                   (unsigned long long)old, (unsigned long long)now,
                   (unsigned)(now & 0xFF), (now & 0x10000ULL) ? "MASKED" : "unmasked",
                   g_exitCountForDiag);
            fflush(stdout);
        }
        // The guest programming ANY redirection entry means it has moved to APIC
        // interrupt routing. From that point the legacy PIC-remapped vectors are
        // not just unhelpful, they are illegal: 0x08 (RTC) and 0x09 (keyboard) sit
        // below 0x10, which the local APIC reserves for CPU exceptions -- the APIC
        // refuses to deliver them, and a Windows kernel in APIC mode has no
        // handler registered for them either way. See ioapicResolveVector.
        if (!g_guestApicMode) {
            g_guestApicMode = 1;
            printf("[apic] guest programmed an IOAPIC redirection entry -- APIC mode; "
                   "legacy vector fallback now disabled\n");
            fflush(stdout);
        }
    }
}

// Resolves which vector to actually inject for a legacy ISA IRQ that our
// MADT maps to gsi via an Interrupt Source Override. While the IOAPIC's
// redirection entry for that GSI is still untouched (fully masked, vector
// 0 -- the power-on default), keep using the legacy 8259-remap vector,
// matching real hardware's PIC-compatibility behavior before an OS
// reprograms IOAPIC-based routing. Once the guest has written anything
// else to that entry (confirmed via live kernel tracing: HAL successfully
// configures it, e.g. vector 0xD1 for the clock interrupt, but our
// injection code was still hardcoding the legacy vector and the
// programmed one never fired -- see docs/investigations/vppt-synic-blocker.md
// part 9/10), honor the entry directly: skip injection if masked,
// otherwise use its own vector field.
// U43: per-pass log budget for non-legacy (IOAPIC-programmed) vector resolutions.
// U42 found that pass 2 inherits ioapic1.redir[8] = 0x01000000000008D1 -- GSI 8
// (RTC), vector 0xD1, UNMASKED -- because the port-0x64/0xFE reset restores CPU
// registers only. The branch below then returns 0xD1 rather than the legacy
// 8259-remapped vector, for the whole window between the reset and pass-2 HAL init
// re-masking the entry (~130k exits of firmware/bootloader). Log it to establish
// whether a wrong vector is actually being delivered, rather than assuming it.
long g_resolveNonLegacyLogged[2] = { 0, 0 };
long g_resolveNonLegacyCount[2] = { 0, 0 };
#define U43_RESOLVE_LOG_PER_PASS 25

int ioapicResolveVector(int gsi, unsigned char legacyVector, unsigned char *outVector) {
    IoApicState *ap = (gsi < 24) ? &ioapic1 : &ioapic2;
    int entry = (gsi < 24) ? gsi : (gsi - IOAPIC2_GSI_BASE);
    UINT64 rte = ap->redirTable[entry];
    if (rte == 0x10000ULL) {
        // Entry still at its power-on default. While the guest is in PIC mode
        // (firmware, bootloader) the legacy vector is exactly right and boot
        // depends on it. Once the guest has moved to APIC routing, however, an
        // unprogrammed entry means it has not routed this GSI yet -- so DROP it
        // rather than inject a legacy vector.
        //
        // Injecting anyway was actively harmful: 0x08 and 0x09 are below 0x10,
        // which the local APIC reserves for CPU exceptions. WHvRequestInterrupt
        // rejects them outright (measured: hr=0xC0350005 on vector 0x08 while
        // other vectors succeeded), and even forced in raw they land on a vector
        // no Windows ISR is registered for. This is the most likely reason three
        // separate attempts at main-loop PS/2 IRQ delivery broke the boot.
        if (g_guestApicMode) {
            g_legacyVectorDropped++;
            return 0;
        }
        *outVector = legacyVector;
        return 1;
    }
    if (rte & 0x10000ULL) {
        return 0; // masked -- guest doesn't want this GSI delivered
    }
    *outVector = (unsigned char)(rte & 0xFF);
    { // U43: an IOAPIC-programmed vector is being used instead of the legacy one.
        int p = g_sawReset ? 1 : 0;
        g_resolveNonLegacyCount[p]++;
        if (g_resolveNonLegacyLogged[p] < U43_RESOLVE_LOG_PER_PASS) {
            g_resolveNonLegacyLogged[p]++;
            printf("[u43] pass%d gsi=%d PROGRAMMED vector=0x%02X (legacy would be 0x%02X) rte=0x%016llX kernelFound=%d\n",
                   p + 1, gsi, *outVector, legacyVector,
                   (unsigned long long)rte, g_bpModuleBase ? 1 : 0);
            fflush(stdout);
        }
    }
    return 1;
}

// Fires the RTC's periodic interrupt (IRQ8) when Register B's PIE bit is
// set and the programmed rate's interval has elapsed. Must be called from
// BOTH the halted-CPU wait loop and the main run loop's per-iteration
// pending-IRQ delivery (alongside deliverPendingAtaIrq/deliverPendingRtl8139Irq)
// -- calling it only from the halted branch was the reason the very first
// version of this fix still didn't clear the bugcheck: HalpTimerWaitForPhase0Interrupt
// (see docs/investigations/vppt-synic-blocker.md part 10) waits via
// KeStallExecutionProcessor, a busy-spin loop, not HLT, so the guest never
// enters the halted branch during that exact 3-second window and our
// injection code never got a chance to run at all.
int deliverRtcPeriodicIrq(WHV_PARTITION_HANDLE partition) {
    static int diagCount = 0;
    static int diagSkippedDisabled = 0;
    static int diagFired = 0;
    if (!(cmosRegisters[0x0B] & 0x40)) return 0; // PIE not enabled
    double intervalMs = rtcPeriodicIntervalMs(cmosRegisters[0x0A]);
    if (intervalMs <= 0.0) return 0;
    // RATE CAP, for testing the input deadlock. The guest wedges spinning in
    // KxWaitForSpinLockAndAcquire at IRQL HIGH_LEVEL, with HalpAcquireHighLevelLock
    // and RtlGetInterruptTimePrecise on the stack above KeAcquireSpinLockAtDpcLevel:
    // the HAL's time lock is taken at DPC level and then re-entered at HIGH_LEVEL,
    // which on a ONE-vCPU guest nothing can ever release. An interrupt arriving
    // while that lock is held is the way in, and this RTC fires at ~1kHz straight
    // into the timekeeping path. Capping the rate tests that directly.
    // LOCALHOST_RTC_MIN_MS=0 (default) leaves the guest's programmed rate alone.
    if (g_rtcMinIntervalMs > intervalMs) intervalMs = g_rtcMinIntervalMs;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsedMs = (double)(now.QuadPart - lastRtcPeriodicTick.QuadPart) * 1000.0 / perfFrequency.QuadPart;
    if (rtcPeriodicTickArmed && elapsedMs < intervalMs) return 0;
    lastRtcPeriodicTick = now;
    rtcPeriodicTickArmed = 1;
    cmosRegisters[0x0C] |= 0xC0; // PF (0x40) + IRQF (0x80)
    diagCount++;
    if (!guestInterruptsEnabled(partition)) {
        diagSkippedDisabled++;
        if (diagCount <= 30 || diagCount % 500 == 0) {
            printf("[rtc-diag] tick #%d SKIPPED (interrupts disabled), skippedTotal=%d firedTotal=%d\n",
                   diagCount, diagSkippedDisabled, diagFired);
            fflush(stdout);
        }
        return 0;
    }
    unsigned char rtcVector;
    if (ioapicResolveVector(8, 0x70, &rtcVector)) {
        diagFired++;
        if (diagCount <= 30 || diagCount % 500 == 0) {
            printf("[rtc-diag] tick #%d FIRED vector=0x%02X, skippedTotal=%d firedTotal=%d\n",
                   diagCount, rtcVector, diagSkippedDisabled, diagFired);
            fflush(stdout);
        }
        // Lowest priority: a deferred tick is invisible to the guest, whereas a
        // keystroke displaced by one is lost. Before the queue existed this call
        // clobbered every other device's pending interrupt ~1000 times a second.
        queueInterrupt(rtcVector, IRQ_PRIO_TIMER);
        drainInterruptQueue(partition);
        return 1;
    }
    return 0;
}

// PIT timer IRQ0 delivery outside the halted-CPU wait loop. Confirmed live
// (see docs/investigations/post-vppt-boot-stall.md, PM1a_CNT stall entry):
// a real-mode SeaBIOS delay/calibration loop that busy-waits on elapsed PIT
// ticks WITHOUT halting produces zero VM exits of its own, so
// WHvRunVirtualProcessor never returns and this hypervisor's only IRQ0
// source (the halted-wait branch, deliverPendingAtaIrq/deliverRtcPeriodicIrq
// etc. below) never gets a chance to run -- a permanent hang, confirmed
// reproducible. rtcCancelThread already exists to force periodic returns
// via WHvCancelRunVirtualProcessor precisely for this "CPU-bound busy-spin
// with no exits" case, but was previously gated on the RTC's own
// periodic-interrupt-enable bit, which isn't set this early in POST --
// broadened below to fire unconditionally so this function actually gets
// invoked during a plain PIT-based spin too.
// THE ONLY PLACE vector 0x08 is injected. There were two, with different rules:
// this function (gated) and a copy in the halted loop (not gated at all), and
// they share lastTimerTick -- so whichever ran first suppressed the other. That
// made the gate ineffective (0x08 still reached a running Windows, which is the
// wedge it was added to prevent) and made the tick counter read zero while ticks
// were in fact being delivered by the other path. One entry point so the gate
// and the accounting cannot drift apart again.
static int deliverLegacyTimerTick(WHV_PARTITION_HANDLE partition) {
    // 0x08 is the PIC-remapped IRQ0 firmware expects, and also the double-fault
    // exception vector. Firmware needs it for its entire life; a Windows kernel
    // in APIC mode cannot use it and it wedges the raw injection slot. Gated on
    // the kernel being up rather than on g_guestApicMode, which flips as soon as
    // the guest programs any IOAPIC entry -- something OVMF does during its own
    // init, while still driving its timer from the 8259/8254.
    if (g_bpModuleBase) return 0;
    // ONLY once the guest has said it wants them. Delivering IRQ0 before the
    // firmware has armed its own timer wedged the guest solid (exit count frozen
    // at 8571 for 280s), which is why every previous attempt at this failed.
    if (!pitChannel0Programmed) return 0;   // channel 0 not set up yet
    if (pic1Mask & 0x01) return 0;          // guest has IRQ0 masked
    g_pitTicksDelivered++;
    // The guest's OWN remapped vector, not a hardcoded 0x08 -- read from ICW2.
    injectInterrupt(partition, pic1VectorBase);
    return 1;
}

int deliverPitTimerIrq(WHV_PARTITION_HANDLE partition) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsedMs = (double)(now.QuadPart - lastTimerTick.QuadPart) * 1000.0 / perfFrequency.QuadPart;
    if (elapsedMs < TIMER_TICK_INTERVAL_MS) return 0;
    lastTimerTick = now;
    if (!guestInterruptsEnabled(partition)) return 0;
    // STOPS once the WINDOWS KERNEL is up, not once the guest touches an IOAPIC.
    //
    // Vector 0x08 is the PIC-remapped IRQ0 of real mode -- and it is also the
    // DOUBLE FAULT exception vector. A Windows kernel in APIC mode cannot do
    // anything sensible with it, and it was found permanently wedged in the raw
    // pending-interruption slot, blocking every other interrupt in the machine.
    // This path predates the interrupt queue and writes that slot directly,
    // bypassing ioapicResolveVector's refusal to hand out sub-0x10 vectors.
    //
    // The first attempt gated on g_guestApicMode, and that was WRONG: that flag
    // is set the moment the guest programs ANY IOAPIC redirection entry, which
    // OVMF does during firmware init. It therefore killed the firmware's only
    // timer -- measured, pitTicks=0 across a whole 43s firmware boot -- while
    // OVMF was still using the 8259/8254 for its own timer tick. Everything
    // time-based in BDS then has no clock to run on.
    //
    // g_bpModuleBase is the right gate here: firmware keeps its timer for the
    // whole of its life, and injection stops once Windows is actually running,
    // which is when the wedge was observed.
    return deliverLegacyTimerTick(partition);
}

// x86-64 GPR encoding order (as used by ModRM.reg/rm, REX-extended 0-15).
WHV_REGISTER_NAME ioapicGprNames[16] = {
    WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRbx,
    WHvX64RegisterRsp, WHvX64RegisterRbp, WHvX64RegisterRsi, WHvX64RegisterRdi,
    WHvX64RegisterR8,  WHvX64RegisterR9,  WHvX64RegisterR10, WHvX64RegisterR11,
    WHvX64RegisterR12, WHvX64RegisterR13, WHvX64RegisterR14, WHvX64RegisterR15
};

// Decodes just enough of a `mov` touching memory (0x89 store r32->mem, 0x8B
// load mem->r32, 0xC7 /0 store imm32->mem) to know direction and which
// register/immediate is involved. Deliberately doesn't interpret the
// ModRM/SIB addressing computation at all -- WHV already hands us the exact
// faulting GPA directly, so we only need to walk past those bytes to find
// where the opcode's own operand (reg field, or the 0xC7 immediate) lives.
int ioapicDecodeMmio(unsigned char *insn, int len, int *isWrite, int *isImm, int *regNum, UINT32 *immVal, int *totalLen) {
    int i = 0;
    int rexR = 0, hasRex = 0;
    if (i < len && insn[i] >= 0x40 && insn[i] <= 0x4F) {
        hasRex = 1; rexR = (insn[i] >> 2) & 1; i++;
    }
    if (i >= len) return 0;
    unsigned char opcode = insn[i++];
    if (opcode != 0x89 && opcode != 0x8B && opcode != 0xC7) return 0;
    if (i >= len) return 0;
    unsigned char modrm = insn[i++];
    unsigned char mod = (modrm >> 6) & 3;
    unsigned char regField = (modrm >> 3) & 7;
    unsigned char rm = modrm & 7;
    if (hasRex && rexR) regField += 8;
    if (mod != 3 && rm == 4) i++; // SIB byte present
    if (mod == 1) i += 1;
    else if (mod == 2) i += 4;
    else if (mod == 0 && rm == 5) i += 4; // RIP-relative disp32

    if (opcode == 0xC7) {
        if (i + 4 > len) return 0;
        *isWrite = 1; *isImm = 1;
        memcpy(immVal, insn + i, 4);
        *totalLen = i + 4;
        return 1;
    }
    *isWrite = (opcode == 0x89);
    *isImm = 0;
    *regNum = regField;
    *totalLen = i;
    return 1;
}

// Returns 1 if the fault was inside the I/O APIC's MMIO window and was
// handled (register state updated, RIP advanced); 0 otherwise, so the
// caller can fall back to its generic unmapped-GPA handling.
int ioapicAccessLogCount = 0;

// U42: per-pass IOAPIC register-level trace. U41 showed HAL's IOAPIC conversation
// is what decides whether APIC I/O unit init succeeds (version read twice and
// cross-checked, ID written, then all 24 redirection entries rewritten), so the
// question "what differs about pass 2" is answerable by diffing the two passes'
// register traffic directly rather than inferring it from an exit-biased RIP
// profile -- which is exactly how U40 went wrong.
//
// Logged per pass so pass 2 gets its own budget, and AFTER decode so the register
// and value are known. Reads log the value we RETURN, which is the thing HAL
// actually makes decisions on.
#define U42_LOG_PER_PASS 260   /* ~enough for version+ID+24 entries x2 dwords */
long g_ioapicPassAccesses[2] = { 0, 0 };
long g_ioapicPassLogged[2] = { 0, 0 };

int ioapicHandleMmioAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    UINT64 gpa = exitContext->MemoryAccess.Gpa;
    IoApicState *ap;
    UINT64 pageOff;
    if (gpa >= IOAPIC_MMIO_BASE && gpa < IOAPIC_MMIO_BASE + IOAPIC_MMIO_SIZE) {
        ap = &ioapic1;
        pageOff = gpa - IOAPIC_MMIO_BASE;
    } else if (gpa >= IOAPIC2_MMIO_BASE && gpa < IOAPIC2_MMIO_BASE + IOAPIC_MMIO_SIZE) {
        ap = &ioapic2;
        pageOff = gpa - IOAPIC2_MMIO_BASE;
    } else {
        return 0;
    }

    ioapicAccessLogCount++;
    // U42: the pre-existing log above is a single global counter, so pass 1
    // consumes its whole first-100 budget and pass 2 -- the pass we care about --
    // logs nothing. It also fires BEFORE the instruction is decoded, so it cannot
    // show which register or value was involved. Count per pass instead, and do the
    // informative logging after decode (below).
    g_ioapicPassAccesses[g_sawReset ? 1 : 0]++;
    if (ioapicAccessLogCount <= 100 || ioapicAccessLogCount % 100000 == 0) {
        printf("[ioapic #%d] gpa=0x%llX pageOff=0x%llX rip=0x%llX write=%d insnLen=%d bytes=%d\n",
               ioapicAccessLogCount, (unsigned long long)gpa, (unsigned long long)pageOff,
               (unsigned long long)exitContext->VpContext.Rip, exitContext->MemoryAccess.AccessInfo.AccessType,
               exitContext->VpContext.InstructionLength, exitContext->MemoryAccess.InstructionByteCount);
        fflush(stdout);
    }

    int isWrite = 0, isImm = 0, regNum = 0, insnTotalLen = 0;
    UINT32 immVal = 0;
    if (!ioapicDecodeMmio(exitContext->MemoryAccess.InstructionBytes,
                           exitContext->MemoryAccess.InstructionByteCount,
                           &isWrite, &isImm, &regNum, &immVal, &insnTotalLen)) {
        printf("[ioapic] failed to decode MMIO instruction at rip=0x%llX (%d bytes):",
               (unsigned long long)exitContext->VpContext.Rip, exitContext->MemoryAccess.InstructionByteCount);
        int bi;
        for (bi = 0; bi < exitContext->MemoryAccess.InstructionByteCount; bi++) {
            printf(" %02X", exitContext->MemoryAccess.InstructionBytes[bi]);
        }
        printf("\n");
        fflush(stdout);
        return 0;
    }

    if (isWrite) {
        UINT32 value = immVal;
        if (!isImm) {
            WHV_REGISTER_VALUE regVal = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &regVal);
            value = (UINT32)regVal.Reg64;
        }
        // U42: log the write before it is applied, so selectedReg still shows which
        // register this data write targets.
        {
            int u42p = g_sawReset ? 1 : 0;
            if (g_ioapicPassLogged[u42p] < U42_LOG_PER_PASS) {
                g_ioapicPassLogged[u42p]++;
                if (pageOff == 0x00)
                    printf("[u42] pass%d ioapic%u SELECT reg=0x%02X\n", u42p + 1, ap->id, value & 0xFF);
                else
                    printf("[u42] pass%d ioapic%u WRITE reg=0x%02X value=0x%08X\n",
                           u42p + 1, ap->id, ap->selectedReg, value);
                fflush(stdout);
            }
        }
        if (pageOff == 0x00) ap->selectedReg = value & 0xFF;
        else if (pageOff == 0x10) {
            ioapicWriteRegister(ap, ap->selectedReg, value);
            if (ap->selectedReg >= 0x10 && ap->selectedReg <= 0x3F) {
                static int rteInterestingLogCount = 0;
                int entry = (ap->selectedReg - 0x10) / 2;
                UINT64 rte = ap->redirTable[entry];
                unsigned char vec = (unsigned char)(rte & 0xFF);
                int masked = (rte & 0x10000ULL) ? 1 : 0;
                // Skip the noisy early OVMF/BIOS sweep that just masks
                // every entry with vector 0xFF (mask=1, vector=0xFF) --
                // only log entries that are unmasked or carry a vector
                // other than the default sweep value, i.e. genuinely
                // reprogrammed by something (OVMF's own real routing or,
                // later, Windows HAL).
                if ((!masked || vec != 0xFF) && rteInterestingLogCount < 300) {
                    rteInterestingLogCount++;
                    printf("[ioapic-rte] controller id=%u entry=%d reg=0x%02X value=0x%08X -> full RTE now 0x%016llX (vector=0x%02X masked=%d)\n",
                           ap->id, entry, ap->selectedReg, value,
                           (unsigned long long)rte, vec, masked);
                    fflush(stdout);
                }
            }
        }
    } else {
        UINT32 value = (pageOff == 0x00) ? ap->selectedReg
                      : (pageOff == 0x10) ? ioapicReadRegister(ap, ap->selectedReg)
                      : 0;
        // U42: log the value we hand back -- this is what HAL branches on. The
        // version-register path (+0x5D..+0xA7 in HalpApicInitializeIoUnit) reads
        // twice and bails if the reads disagree or read 0/0xFFFFFFFF.
        {
            int u42p = g_sawReset ? 1 : 0;
            if (g_ioapicPassLogged[u42p] < U42_LOG_PER_PASS) {
                g_ioapicPassLogged[u42p]++;
                printf("[u42] pass%d ioapic%u READ  reg=0x%02X -> 0x%08X%s\n",
                       u42p + 1, ap->id,
                       (pageOff == 0x00) ? 0 : ap->selectedReg, value,
                       (pageOff == 0x00) ? "  (IOREGSEL readback)" : "");
                fflush(stdout);
            }
        }
        WHV_REGISTER_VALUE regVal = { 0 };
        regVal.Reg64 = value;
        WHvSetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &regVal);
    }

    // NOTE: exitContext->VpContext.InstructionLength is NOT populated for
    // MemoryAccess/MMIO exits (confirmed empirically: it read 0 here,
    // unlike I/O port exits where it's always valid) -- WHV expects the
    // caller to determine instruction length itself from InstructionBytes
    // for this exit type. Using it anyway left RIP completely unchanged,
    // so the exact same faulting instruction re-executed and re-faulted
    // forever: confirmed via a counter showing 600,000+ identical
    // [ioapic] log entries all at the same RIP before this fix.
    WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
    WHV_REGISTER_VALUE ripVal = { 0 };
    ripVal.Reg64 = exitContext->VpContext.Rip + (UINT64)insnTotalLen;
    WHvSetVirtualProcessorRegisters(partition, 0, &ripName, 1, &ripVal);
    return 1;
}

UINT16 fwCfgSelector = 0;
UINT32 fwCfgOffset = 0;

unsigned char acpiRsdp[36];
// U49: the DSDT grew from a 36-byte header-only stub to a real 522-byte table
// with an AML namespace, so it no longer fits its old slot at offset 272 (which
// was bounded by DBG2 at 308). It now lives AFTER DBG2, at DSDT_BLOB_OFFSET, and
// the blob was grown to match. Offsets 272..307 are now dead space, deliberately
// left rather than repacking every table and re-deriving all the loader
// commands/checksums for no functional gain.
#include "acpi/dsdt_aml.h"
#define DSDT_BLOB_OFFSET 392                            /* right after DBG2 (308 + 84) */
// U51: the FADT is upgraded from ACPI 1.0 (revision 1, 116 bytes) to ACPI 2.0+
// (revision 3, 244 bytes) so it carries the X_* 64-bit extended address fields.
// 244 bytes will not fit its old home at offset 60, which is bounded by the MADT
// at 176, so it moves past the DSDT. Offsets 60..175 are now dead space, left
// rather than repacking every table and re-deriving all loader commands.
#define FADT_BLOB_OFFSET (DSDT_BLOB_OFFSET + DSDT_AML_SIZE)
#define FADT_LENGTH 244
// U51b: the FACS. We published none (FIRMWARE_CTRL and X_FIRMWARE_CTRL both 0),
// which violates the ACPI spec -- a FACS is mandatory unless HW_REDUCED_ACPI is
// set in the FADT flags, and we do not set it. 64 bytes, and note it is NOT a
// normal SDT: it has no revision/checksum/OEM header, so acpiPutHeader must not
// be used on it and it gets no ADD_CHECKSUM loader command.
#define FACS_BLOB_OFFSET (FADT_BLOB_OFFSET + FADT_LENGTH)
#define FACS_LENGTH 64
#define ACPI_TABLES_SIZE (FACS_BLOB_OFFSET + FACS_LENGTH)
unsigned char acpiTables[ACPI_TABLES_SIZE]; // XSDT(0)+FADT(60)+MADT(176,96)+DBG2(308)+DSDT(392), see acpiBuildTables
int acpiTablesBuilt = 0;

// "etc/ramfb": unlike every other file here, this one is WRITABLE by the
// guest -- OVMF's QemuRamfbDxe writes a RAMFB_CONFIG struct (address,
// FourCC format, flags, width, height, stride, all big-endian) here to
// tell the host where it allocated its own framebuffer (plain guest RAM
// it owns via its normal UEFI memory allocator -- no MMIO/BAR needed on
// our side at all). This is dramatically simpler than a real VBE/Bochs
// VGA device: we don't need to emulate any mode-setting I/O ports, just
// remember where the guest says its pixels are so we can paint them.
// (ramfbConfigWritten/Address/Width/Height/Stride are declared near the
// top of the file, ahead of WndProc, which also needs them.)
unsigned char ramfbConfig[28];

// Real struct is { UINT32 Type; union { ...; UINT8 Padding[124]; } Command; }
// per edk2 OvmfPkg/Include/IndustryStandard/QemuLoader.h -- 4 (Type) + 124
// (Command, explicitly padded) = 128 bytes total. Using 124 here (an
// understandable but wrong guess at the union's own size) made our
// "etc/table-loader" size fail OVMF's `FwCfgSize % sizeof(*LoaderEntry) == 0`
// sanity check, so InstallQemuFwCfgTables() returned EFI_PROTOCOL_ERROR
// before ever reading the file's content -- confirmed by cross-referencing
// the real edk2 source after black-box tracing showed the directory being
// found/scanned correctly but our RSDP/tables/loader fw_cfg keys never
// getting selected at all.
#define ACPI_LOADER_CMD_SIZE 128
// U51b: was 16 slots. Adding the X_DSDT, FIRMWARE_CTRL and X_FIRMWARE_CTRL
// pointer commands pushed the count to 17, which overran this array into the
// globals that follow it -- the guest then failed to boot far enough to even
// discover the kernel. Sized with headroom, and the builder asserts the count
// below so the next addition fails loudly instead of corrupting memory.
#define ACPI_LOADER_MAX_CMDS 32
unsigned char acpiLoader[ACPI_LOADER_MAX_CMDS * ACPI_LOADER_CMD_SIZE];

unsigned char fwCfgFileDir[4 + 4 * (4 + 2 + 2 + 56)];
int fwCfgFileDirBuilt = 0;

void acpiPutU16(unsigned char *buf, UINT32 off, UINT16 val) { memcpy(buf + off, &val, 2); }
void acpiPutU32(unsigned char *buf, UINT32 off, UINT32 val) { memcpy(buf + off, &val, 4); }

// fw_cfg's OWN multi-byte fields (FILE_DIR count/size, unlike everything
// inside the ACPI tables themselves) are big-endian per spec.
void fwCfgPutU32BE(unsigned char *buf, UINT32 off, UINT32 val) {
    buf[off + 0] = (unsigned char)(val >> 24);
    buf[off + 1] = (unsigned char)(val >> 16);
    buf[off + 2] = (unsigned char)(val >> 8);
    buf[off + 3] = (unsigned char)(val);
}

// Fills in one 36-byte ACPI SDT header (used by XSDT/FADT/MADT/DSDT alike).
// Checksum is deliberately left 0 -- OVMF's own loader-script interpreter
// computes and patches real checksums after it copies these bytes into
// guest memory (see acpiBuildLoaderScript's ADD_CHECKSUM commands), so we
// never need to compute one ourselves.
// U51: write a Generic Address Structure (12 bytes) -- the ACPI 2.0+ way of
// describing a register. All-zero means "not present", which is what the unused
// PM1b/PM2/GPE blocks get.
//   spaceId: 0 = system memory, 1 = system I/O
//   accessSize: 0 = undefined, 1 = byte, 2 = word, 3 = dword, 4 = qword
void acpiPutGas(unsigned char *buf, UINT32 off, unsigned char spaceId,
                unsigned char bitWidth, unsigned char bitOffset,
                unsigned char accessSize, UINT64 address) {
    buf[off + 0] = spaceId;
    buf[off + 1] = bitWidth;
    buf[off + 2] = bitOffset;
    buf[off + 3] = accessSize;
    memcpy(buf + off + 4, &address, 8);
}

void acpiPutHeader(unsigned char *buf, UINT32 off, const char *sig, UINT32 length, unsigned char revision) {
    memcpy(buf + off, sig, 4);
    acpiPutU32(buf, off + 4, length);
    buf[off + 8] = revision;
    buf[off + 9] = 0; // checksum, patched by OVMF
    memcpy(buf + off + 10, "LHOST0", 6);   // OEMID
    memcpy(buf + off + 16, "LHOSTTBL", 8); // OEM Table ID
    acpiPutU32(buf, off + 24, 1);          // OEM Revision
    memcpy(buf + off + 28, "LHV1", 4);     // Creator ID
    acpiPutU32(buf, off + 32, 1);          // Creator Revision
}

// Builds the RSDP + combined ACPI table blob. Deferred until the guest
// actually selects one of these fw_cfg files (rather than built once at
// startup) because FADT must embed the *real* PM1a event/control/timer
// I/O port block -- and that address isn't a fixed constant here: it's
// PIIX4's PM function's PMBA register (pciPmConfig offset 0x40), which
// OVMF's own platform code assigns dynamically during boot (empirically
// observed this session settling on 0xB000, not anything we control up
// front). By the time the guest is far enough into DXE to be loading ACPI
// tables via fw_cfg, that assignment has already happened, so reading
// pciPmConfig[0x40] here gets the real value instead of a guess.
void acpiBuildTables(void) {
    UINT32 pmBase = (*(UINT32 *)&pciPmConfig[0x40]) & 0xFFC0;
    printf("[acpi] acpiBuildTables() firing -- pmBase=0x%X\n", pmBase);
    fflush(stdout);

    memset(acpiRsdp, 0, sizeof(acpiRsdp));
    memcpy(acpiRsdp + 0, "RSD PTR ", 8);
    acpiRsdp[8] = 0; // checksum, patched by OVMF
    memcpy(acpiRsdp + 9, "LHOST0", 6);
    acpiRsdp[15] = 2; // revision (2 = ACPI 2.0+, XSDT-capable)
    acpiPutU32(acpiRsdp, 16, 0); // RsdtAddress -- unused, XSDT-only
    acpiPutU32(acpiRsdp, 20, 36); // Length
    // bytes 24-31 (XsdtAddress) and 32 (extended checksum) patched by OVMF

    memset(acpiTables, 0, sizeof(acpiTables));

    // XSDT at blob offset 0 (60 bytes: 36-byte header + 3 8-byte entries)
    acpiPutHeader(acpiTables, 0, "XSDT", 60, 1);
    acpiPutU32(acpiTables, 36, FADT_BLOB_OFFSET);  // entry0: FADT's blob-relative offset (patched to an absolute address by OVMF) -- U51 moved it past the DSDT
    acpiPutU32(acpiTables, 44, 176); // entry1: MADT's blob-relative offset (ditto)
    acpiPutU32(acpiTables, 52, 308); // entry2: DBG2's blob-relative offset (ditto)

    // FADT ("FACP") at FADT_BLOB_OFFSET.
    //
    // U51: was ACPI 1.0 (revision 1, 116 bytes). Measured consequence: acpi.sys's
    // ACPILoadProcessRSDT enumerated the root table, never accepted a FADT, and
    // returned STATUS_ACPI_INVALID_TABLE (0xC0140019), which ACPIInitialize turns
    // into KeBugCheckEx(0xA5, 0x11, ...) -- the reboot loop from U48. Everything
    // else we publish is ACPI 2.0+ (RSDP revision 2, XSDT-only with RsdtAddress=0,
    // MADT revision 3), so a revision-1 FADT reached through an XSDT is an
    // inconsistent pair: it has none of the X_* 64-bit extended address fields a
    // 64-bit OS expects to find. Now revision 3 / 244 bytes with those fields.
    acpiPutHeader(acpiTables, FADT_BLOB_OFFSET, "FACP", FADT_LENGTH, 3);
    // U52: bisect switch. Timeline from the evidence logs -- U49 (real DSDT) and
    // U51 (ACPI 2.0 FADT) both sailed past exitCount 126854 and bugchecked 0xA5
    // later (at 179759 and 163879); U51b, which only added the FACS, stalls AT
    // 126854 in HalpApicInitializeIoUnit. That points at the FACS, so make it
    // switchable to confirm rather than assume.
#define U52_PUBLISH_FACS 1
#if U52_PUBLISH_FACS
    acpiPutU32(acpiTables, FADT_BLOB_OFFSET + 36, FACS_BLOB_OFFSET); // FIRMWARE_CTRL -> FACS (patched to absolute)
#else
    acpiPutU32(acpiTables, FADT_BLOB_OFFSET + 36, 0);                // FIRMWARE_CTRL absent (bisect)
#endif
    acpiPutU32(acpiTables, FADT_BLOB_OFFSET + 40, DSDT_BLOB_OFFSET); // DSDT (32-bit, patched to absolute)
    acpiTables[FADT_BLOB_OFFSET + 45] = 0;              // Preferred_PM_Profile (0 = unspecified)
    acpiPutU16(acpiTables, FADT_BLOB_OFFSET + 46, 9);   // SCI_INT
    acpiPutU32(acpiTables, FADT_BLOB_OFFSET + 48, 0);   // SMI_CMD = 0 -- ACPI mode already enabled, no SMM handshake (we emulate no SMM)
    acpiPutU32(acpiTables, FADT_BLOB_OFFSET + 56, pmBase + 0); // PM1a_EVT_BLK
    acpiPutU32(acpiTables, FADT_BLOB_OFFSET + 64, pmBase + 4); // PM1a_CNT_BLK
    acpiPutU32(acpiTables, FADT_BLOB_OFFSET + 76, pmBase + 8); // PM_TMR_BLK -- matches the PM Timer emulation at PM_BASE+8
    acpiTables[FADT_BLOB_OFFSET + 88] = 4; // PM1_EVT_LEN
    acpiTables[FADT_BLOB_OFFSET + 89] = 2; // PM1_CNT_LEN
    acpiTables[FADT_BLOB_OFFSET + 91] = 4; // PM_TMR_LEN
    // C2/C3 latencies above their "unsupported" thresholds (>100us / >1000us), so
    // the OS does not try to use idle states we do not emulate.
    acpiPutU16(acpiTables, FADT_BLOB_OFFSET + 96, 0x0FFF); // P_LVL2_LAT
    acpiPutU16(acpiTables, FADT_BLOB_OFFSET + 98, 0x0FFF); // P_LVL3_LAT
    acpiTables[FADT_BLOB_OFFSET + 108] = 0x32; // CENTURY: CMOS century register index
    acpiPutU16(acpiTables, FADT_BLOB_OFFSET + 109, 0x0002); // IAPC_BOOT_ARCH: bit1 = 8042 present (we emulate one)
    // Flags: WBINVD supported (bit0) | TMR_VAL_EXT (bit8) -- our PM_TMR_BLK
    // emulation (see the ACPI PM Timer read handler) returns a genuine free-
    // running 32-bit counter, never masked to 24 bits. Leaving TMR_VAL_EXT
    // clear told Windows to treat it as a 24-bit counter (wrapping every
    // ~4.69s at 3.579545MHz) while it actually behaves as 32-bit -- the
    // HAL's PM-timer-based TSC calibration compares successive reads
    // expecting 24-bit wraparound, so the mismatched upper bits broke its
    // convergence, manifesting as an indefinite stall (guest spinning
    // inside a single WHvRunVirtualProcessor call, no further port traps)
    // immediately after a PM Timer read.
    acpiPutU32(acpiTables, FADT_BLOB_OFFSET + 112, 0x00000101);

    // U51: the ACPI 2.0+ tail (offsets 116..243). RESET_REG is left all-zero and
    // the RESET_REG_SUP flag (bit 10) is deliberately NOT set, because we do not
    // emulate a 0xCF9-style reset register -- advertising one we do not implement
    // is how the DSDT/IOAPIC mistakes earlier in this investigation happened.
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 116, 0, 0, 0, 0, 0); // RESET_REG (absent)
    acpiTables[FADT_BLOB_OFFSET + 128] = 0;                        // RESET_VALUE
    acpiTables[FADT_BLOB_OFFSET + 131] = 0;                        // FADT Minor Version
    // X_FIRMWARE_CTRL stays 0 (no FACS). X_DSDT is filled with the blob-relative
    // DSDT offset and patched to an absolute 64-bit address by the table loader,
    // exactly like the 32-bit DSDT field above.
    {
        UINT64 xfacs = U52_PUBLISH_FACS ? FACS_BLOB_OFFSET : 0, xdsdt = DSDT_BLOB_OFFSET;
        memcpy(acpiTables + FADT_BLOB_OFFSET + 132, &xfacs, 8);    // X_FIRMWARE_CTRL -> FACS
        memcpy(acpiTables + FADT_BLOB_OFFSET + 140, &xdsdt, 8);    // X_DSDT
    }
    // Extended register blocks. Widths mirror the *_LEN fields above; the blocks
    // we do not implement (PM1b, PM2, GPE0/GPE1) stay all-zero = not present.
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 148, 1, 32, 0, 2, pmBase + 0); // X_PM1a_EVT_BLK
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 160, 0, 0, 0, 0, 0);           // X_PM1b_EVT_BLK (absent)
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 172, 1, 16, 0, 2, pmBase + 4); // X_PM1a_CNT_BLK
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 184, 0, 0, 0, 0, 0);           // X_PM1b_CNT_BLK (absent)
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 196, 0, 0, 0, 0, 0);           // X_PM2_CNT_BLK (absent)
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 208, 1, 32, 0, 3, pmBase + 8); // X_PM_TMR_BLK
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 220, 0, 0, 0, 0, 0);           // X_GPE0_BLK (absent)
    acpiPutGas(acpiTables, FADT_BLOB_OFFSET + 232, 0, 0, 0, 0, 0);           // X_GPE1_BLK (absent)

    // U51b: FACS at FACS_BLOB_OFFSET. Mandatory whenever HW_REDUCED_ACPI is clear
    // (we do not set it), and we previously published none at all. Deliberately
    // NOT built with acpiPutHeader: the FACS is not a standard SDT -- it has no
    // revision, checksum, OEM ID or creator fields, so those bytes mean other
    // things here. Layout: signature(0), length(4), hardware signature(8),
    // firmware waking vector(12), global lock(16), flags(20), X firmware waking
    // vector(24), version(32), the rest reserved.
    memcpy(acpiTables + FACS_BLOB_OFFSET, "FACS", 4);
    acpiPutU32(acpiTables, FACS_BLOB_OFFSET + 4, FACS_LENGTH);
    acpiPutU32(acpiTables, FACS_BLOB_OFFSET + 8, 0);   // Hardware Signature -- 0, we never S4-resume
    acpiPutU32(acpiTables, FACS_BLOB_OFFSET + 12, 0);  // Firmware Waking Vector
    acpiPutU32(acpiTables, FACS_BLOB_OFFSET + 16, 0);  // Global Lock -- unowned, uncontended
    acpiPutU32(acpiTables, FACS_BLOB_OFFSET + 20, 0);  // Flags (bit0 S4BIOS_F = 0, we support no S4BIOS transition)
    acpiTables[FACS_BLOB_OFFSET + 32] = 2;             // Version 2 (matches the ACPI 2.0+ FADT above)

    // MADT ("APIC") at blob offset 176 (64 bytes: 36-byte header + 4+4
    // fixed fields + one 8-byte Processor Local APIC entry for vCPU 0 + one
    // 12-byte I/O APIC entry). The I/O APIC entry was originally omitted
    // ("advertising a non-functional one would be worse than omitting it")
    // but that turned out to be wrong: the real NT kernel's HAL
    // unconditionally expects to find and initialize at least one I/O APIC
    // during early boot (confirmed via the kernel's own symbols --
    // HalpInterruptParseMadt, HalpApicInitializeIoUnit,
    // HalpInterruptIoApicCount all exist and run during HalpInitSystemPhase0)
    // -- its total absence, not a non-functional one, is what bugchecked
    // 0x5C HAL_INITIALIZATION_FAILED (param4=STATUS_INVALID_PARAMETER),
    // confirmed by walking the guest's own page tables to read RIP/KiBugCheckData
    // live out of guest memory during the resulting stall and resolving them
    // against the exact matching ntkrnlmp.pdb from Microsoft's symbol server.
    // The redirection table entries themselves (see ioapicRedirTable) start
    // fully masked, matching real hardware's power-on state, so this doesn't
    // need to be interrupt-functional yet -- existing IRQs still go through
    // the direct WHvRegisterPendingInterruption injection hack; this only
    // needs to satisfy HAL's enumeration/initialization contract.
    acpiPutHeader(acpiTables, 176, "APIC", 96, 3);
    acpiPutU32(acpiTables, 176 + 36, 0xFEE00000); // Local APIC Address (standard default)
    acpiPutU32(acpiTables, 176 + 40, 0x00000001); // Flags: PCAT_COMPAT (legacy dual-8259 also present)
    acpiTables[176 + 44] = 0; // Type 0: Processor Local APIC
    acpiTables[176 + 45] = 8; // Length
    acpiTables[176 + 46] = 0; // ACPI Processor UID
    acpiTables[176 + 47] = 0; // APIC ID (vCPU 0)
    acpiPutU32(acpiTables, 176 + 48, 0x00000001); // Flags: Enabled
    acpiTables[176 + 52] = 1;  // Type 1: I/O APIC
    acpiTables[176 + 53] = 12; // Length
    acpiTables[176 + 54] = 1;  // I/O APIC ID (distinct from Local APIC ID 0)
    acpiTables[176 + 55] = 0;  // Reserved
    acpiPutU32(acpiTables, 176 + 56, IOAPIC_MMIO_BASE); // I/O APIC Address
    acpiPutU32(acpiTables, 176 + 60, 0); // Global System Interrupt Base
    // Type 2: Interrupt Source Override, for the SCI (FADT.SCI_INT=9 below).
    // Every real ACPI BIOS includes one of these for the SCI: IOAPIC pins
    // default to ISA/EISA conformance (edge-triggered, active-high) but the
    // ACPI SCI is electrically level-triggered/active-low, so without this
    // override HAL's SCI interrupt setup has a mismatched polarity/trigger
    // mode. Added after two rounds of empirical bugcheck 0x5C
    // (HAL_INITIALIZATION_FAILED) evidence -- adding the I/O APIC itself
    // fixed the first failure (params changed from
    // param3=0/param4=STATUS_INVALID_PARAMETER to
    // param3=0x19/param4=STATUS_UNSUCCESSFUL, a different, later check in
    // the same HAL routine, per KiBugCheckData read live from guest memory)
    // but didn't fully resolve HAL init -- this is the next concrete,
    // standard-ACPI gap to close.
    acpiTables[176 + 64] = 2;  // Type 2: Interrupt Source Override
    acpiTables[176 + 65] = 10; // Length
    acpiTables[176 + 66] = 0;  // Bus: 0 = ISA
    acpiTables[176 + 67] = 9;  // Source: ISA IRQ 9 (matches FADT SCI_INT)
    acpiPutU32(acpiTables, 176 + 68, 9); // Global System Interrupt: 9 (same number, only polarity/trigger differ)
    acpiPutU16(acpiTables, 176 + 72, 0x000F); // Flags: Polarity=Active Low(3) | Trigger=Level(3)
    // Type 2: Interrupt Source Override, ISA IRQ0 (PIT) -> GSI 2. Another
    // near-universal real MADT entry: standard PC chipsets don't actually
    // wire the PIT to the I/O APIC's pin 0, so every real BIOS/QEMU MADT
    // reroutes it to pin 2 instead. Flags=0 (conforms to bus -- ISA IRQ0 is
    // genuinely edge-triggered/active-high, unlike the SCI override above).
    acpiTables[176 + 74] = 2;  // Type 2: Interrupt Source Override
    acpiTables[176 + 75] = 10; // Length
    acpiTables[176 + 76] = 0;  // Bus: 0 = ISA
    acpiTables[176 + 77] = 0;  // Source: ISA IRQ 0 (PIT)
    acpiPutU32(acpiTables, 176 + 78, 2); // Global System Interrupt: 2
    acpiPutU16(acpiTables, 176 + 82, 0x0000); // Flags: conforms to bus spec
    // Type 1: second I/O APIC, GSI 24-47 (see IoApicState ioapic2's comment
    // for why -- empirical follow-up to the still-unresolved bugcheck 0x5C
    // param3=0x19, hypothesizing something needs a GSI beyond the first
    // IOAPIC's 0-23 range).
    acpiTables[176 + 84] = 1;  // Type 1: I/O APIC
    acpiTables[176 + 85] = 12; // Length
    acpiTables[176 + 86] = 2;  // I/O APIC ID (distinct from Local APIC ID 0 and IOAPIC1's ID 1)
    acpiTables[176 + 87] = 0;  // Reserved
    acpiPutU32(acpiTables, 176 + 88, IOAPIC2_MMIO_BASE); // I/O APIC Address
    acpiPutU32(acpiTables, 176 + 92, IOAPIC2_GSI_BASE);  // Global System Interrupt Base

    // U49: the real DSDT, copied in whole from acpi/dsdt_aml.h (generated by
    // acpi/build_dsdt.ps1 from acpi/dsdt.asl). iasl emits the 36-byte ACPI
    // header itself, so this is memcpy'd verbatim -- do NOT acpiPutHeader() over
    // it, that would overwrite the compiler's own header and length.
    //
    // Replaces the previous 36-byte header-only stub, which declared no AML
    // namespace at all and made acpi.sys bugcheck 0xA5 ACPI_BIOS_ERROR on every
    // boot (U48: 40/40 boots, zero disk writes).
    memcpy(acpiTables + DSDT_BLOB_OFFSET, dsdtAml, DSDT_AML_SIZE);
    // Zero the checksum field. iasl emits an already-valid checksum, but the
    // table-loader ADD_CHECKSUM command below sums the byte range and stores the
    // result, which only produces the right answer if the field starts at zero --
    // exactly how acpiPutHeader() leaves it for every other table here. Left as
    // iasl wrote it, the range already summed to zero, the loader stored 0x00 over
    // the correct 0x38, and the guest saw a checksum-invalid DSDT: measured
    // directly by u49VerifyAcpiTables (DSDT ... checksum=BAD while every other
    // table read OK), which is what ACPI rejects with 0xA5 ACPI_BIOS_ERROR.
    acpiTables[DSDT_BLOB_OFFSET + 9] = 0;

    // DBG2 ("Debug Port Table 2") at blob offset 308 (84 bytes; shifted from
    // 264 by the MADT's +12-byte growth above). Windows' BlInitializeLibrary
    // very likely discovers its bootdebug serial UART through this table
    // rather than assuming a fixed legacy COM2 address -- confirmed our
    // COM2 emulation was receiving real 16550 register traffic
    // (scratch-register probe, baud divisor programming) but what the guest
    // actually transmitted turned out to be plain ANSI console text
    // (`\x1B[2J`), not the KD wire protocol, meaning bootdebug never
    // actually engaged that port at all without this table telling it to.
    // Layout (offsets relative to the DBG2 table's own start at 308):
    //   0-35   ACPI SDT header
    //   36-39  OffsetDbgDeviceInfo (=44, i.e. right after these two fields)
    //   40-43  NumberDbgDeviceInfo (=1)
    //   44-65  Debug Device Information structure (22-byte fixed part)
    //   66-77  BaseAddressRegister: one Generic Address Structure (12 bytes)
    //   78-81  AddressSize: one UINT32 (=8, the 0x2F8-0x2FF port range)
    //   82-83  NamespaceString: "." + NUL (no ACPI namespace device exists
    //          for this port -- our DSDT has no AML content -- which the
    //          spec explicitly allows via this single-period placeholder)
    acpiPutHeader(acpiTables, 308, "DBG2", 84, 0);
    acpiPutU32(acpiTables, 308 + 36, 44); // OffsetDbgDeviceInfo (table-relative)
    acpiPutU32(acpiTables, 308 + 40, 1);  // NumberDbgDeviceInfo
    acpiTables[308 + 44] = 0;              // Revision
    acpiPutU16(acpiTables, 308 + 45, 40); // Length (of this Debug Device Information structure)
    acpiTables[308 + 47] = 1;              // NumberofGenericAddressRegisters
    acpiPutU16(acpiTables, 308 + 48, 2);  // NameSpaceStringLength (including NUL)
    acpiPutU16(acpiTables, 308 + 50, 38); // NameSpaceStringOffset (relative to this structure's start, i.e. offset 44)
    acpiPutU16(acpiTables, 308 + 52, 0);  // OemDataLength
    acpiPutU16(acpiTables, 308 + 54, 0);  // OemDataOffset
    acpiPutU16(acpiTables, 308 + 56, 0x8000); // PortType: Serial
    acpiPutU16(acpiTables, 308 + 58, 0x0000); // PortSubtype: Full 16550
    acpiPutU16(acpiTables, 308 + 60, 0);      // Reserved
    acpiPutU16(acpiTables, 308 + 62, 22); // BaseAddressRegisterOffset (relative to structure start)
    acpiPutU16(acpiTables, 308 + 64, 34); // AddressSizeOffset (relative to structure start)
    // Generic Address Structure at struct-relative offset 22 (blob offset 308+44+22=342)
    acpiTables[308 + 66] = 1; // AddressSpaceId: SystemIO
    acpiTables[308 + 67] = 8; // RegisterBitWidth
    acpiTables[308 + 68] = 0; // RegisterBitOffset
    acpiTables[308 + 69] = 1; // AccessSize: Byte
    { UINT64 comBase = 0x2F8; memcpy(acpiTables + 308 + 70, &comBase, 8); } // Address
    // AddressSize at struct-relative offset 34 (blob offset 308+44+34=354)
    acpiPutU32(acpiTables, 308 + 78, 8);
    // NamespaceString at struct-relative offset 38 (blob offset 308+44+38=358)
    acpiTables[308 + 82] = '.';
    acpiTables[308 + 83] = 0;

    acpiTablesBuilt = 1;
}

// Emits one 124-byte bios-linker-loader command record (QEMU's fw_cfg ACPI
// linker/loader protocol -- see acpiBuildTables for why we let OVMF do the
// actual pointer/checksum patching instead of precomputing final values
// ourselves: it decides where each blob actually lands in guest memory).
void acpiLoaderAllocate(unsigned char *cmd, const char *file, UINT32 align, unsigned char zone) {
    memset(cmd, 0, ACPI_LOADER_CMD_SIZE);
    acpiPutU32(cmd, 0, 1); // BIOS_LINKER_LOADER_COMMAND_ALLOCATE
    strncpy((char *)cmd + 4, file, 55);
    acpiPutU32(cmd, 4 + 56, align);
    cmd[4 + 56 + 4] = zone; // 1 = high memory
}

void acpiLoaderAddPointer(unsigned char *cmd, const char *destFile, const char *srcFile, UINT32 offset, unsigned char size) {
    memset(cmd, 0, ACPI_LOADER_CMD_SIZE);
    acpiPutU32(cmd, 0, 2); // BIOS_LINKER_LOADER_COMMAND_ADD_POINTER
    strncpy((char *)cmd + 4, destFile, 55);
    strncpy((char *)cmd + 4 + 56, srcFile, 55);
    acpiPutU32(cmd, 4 + 56 + 56, offset);
    cmd[4 + 56 + 56 + 4] = size;
}

void acpiLoaderAddChecksum(unsigned char *cmd, const char *file, UINT32 offset, UINT32 start, UINT32 length) {
    memset(cmd, 0, ACPI_LOADER_CMD_SIZE);
    acpiPutU32(cmd, 0, 3); // BIOS_LINKER_LOADER_COMMAND_ADD_CHECKSUM
    strncpy((char *)cmd + 4, file, 55);
    acpiPutU32(cmd, 4 + 56, offset);
    acpiPutU32(cmd, 4 + 56 + 4, start);
    acpiPutU32(cmd, 4 + 56 + 4 + 4, length);
}

void acpiBuildLoaderScript(void) {
    unsigned char *c = acpiLoader;
    acpiLoaderAllocate(c, "etc/acpi/rsdp", 16, 1); c += ACPI_LOADER_CMD_SIZE;
    acpiLoaderAllocate(c, "etc/acpi/tables", 64, 1); c += ACPI_LOADER_CMD_SIZE;

    // RSDP.XsdtAddress = base of "etc/acpi/tables" (XSDT sits at blob offset 0)
    acpiLoaderAddPointer(c, "etc/acpi/rsdp", "etc/acpi/tables", 24, 8); c += ACPI_LOADER_CMD_SIZE;
    // XSDT entry0/entry1/entry2: patch the blob-relative FADT/MADT/DBG2
    // offsets we pre-filled into absolute addresses (same file as both src
    // and dest -- an internal, self-referential pointer within one blob).
    acpiLoaderAddPointer(c, "etc/acpi/tables", "etc/acpi/tables", 36, 8); c += ACPI_LOADER_CMD_SIZE;
    acpiLoaderAddPointer(c, "etc/acpi/tables", "etc/acpi/tables", 44, 8); c += ACPI_LOADER_CMD_SIZE;
    acpiLoaderAddPointer(c, "etc/acpi/tables", "etc/acpi/tables", 52, 8); c += ACPI_LOADER_CMD_SIZE;
    // FADT.DSDT (32-bit field, unlike the 64-bit XSDT entries above)
    acpiLoaderAddPointer(c, "etc/acpi/tables", "etc/acpi/tables", FADT_BLOB_OFFSET + 40, 4); c += ACPI_LOADER_CMD_SIZE;
    // U51: FADT.X_DSDT, the 64-bit companion added with the ACPI 2.0+ upgrade.
    // Both must be patched -- an OS that prefers X_DSDT would otherwise follow a
    // raw blob-relative offset as if it were a physical address.
    acpiLoaderAddPointer(c, "etc/acpi/tables", "etc/acpi/tables", FADT_BLOB_OFFSET + 140, 8); c += ACPI_LOADER_CMD_SIZE;
    // U51b: FADT.FIRMWARE_CTRL (32-bit) and FADT.X_FIRMWARE_CTRL (64-bit), both
    // pointing at the new FACS and both needing the same blob-relative-to-absolute
    // patch as the DSDT pointers above.
    // Gated with the FACS itself: ADD_POINTER adds the blob's base address to the
    // field, so patching a deliberately-zero FIRMWARE_CTRL would turn it into a
    // bogus non-null pointer rather than leaving it absent.
#if U52_PUBLISH_FACS
    acpiLoaderAddPointer(c, "etc/acpi/tables", "etc/acpi/tables", FADT_BLOB_OFFSET + 36, 4); c += ACPI_LOADER_CMD_SIZE;
    acpiLoaderAddPointer(c, "etc/acpi/tables", "etc/acpi/tables", FADT_BLOB_OFFSET + 132, 8); c += ACPI_LOADER_CMD_SIZE;
#endif

    acpiLoaderAddChecksum(c, "etc/acpi/tables", 9, 0, 60); c += ACPI_LOADER_CMD_SIZE;           // XSDT
    acpiLoaderAddChecksum(c, "etc/acpi/tables", FADT_BLOB_OFFSET + 9, FADT_BLOB_OFFSET, FADT_LENGTH); c += ACPI_LOADER_CMD_SIZE;    // FADT (U51: ACPI 2.0+, moved past the DSDT)
    acpiLoaderAddChecksum(c, "etc/acpi/tables", 176 + 9, 176, 96); c += ACPI_LOADER_CMD_SIZE;   // MADT
    acpiLoaderAddChecksum(c, "etc/acpi/tables", DSDT_BLOB_OFFSET + 9, DSDT_BLOB_OFFSET, DSDT_AML_SIZE); c += ACPI_LOADER_CMD_SIZE;   // DSDT (U49: real AML table, moved past DBG2)
    acpiLoaderAddChecksum(c, "etc/acpi/tables", 308 + 9, 308, 84); c += ACPI_LOADER_CMD_SIZE;   // DBG2
    acpiLoaderAddChecksum(c, "etc/acpi/rsdp", 8, 0, 20); c += ACPI_LOADER_CMD_SIZE;             // RSDP (ACPI 1.0 checksum)
    acpiLoaderAddChecksum(c, "etc/acpi/rsdp", 32, 0, 36); c += ACPI_LOADER_CMD_SIZE;            // RSDP (extended checksum)

    // U51b: fail loudly rather than silently corrupting the globals that follow
    // acpiLoader. Overrunning this array by a single command was enough to stop
    // the guest booting far enough to even load a kernel, with no obvious clue.
    {
        SIZE_T used = (SIZE_T)(c - acpiLoader);
        if (used > sizeof(acpiLoader)) {
            printf("[acpi] FATAL: %llu loader command bytes exceed the %llu-byte buffer (raise ACPI_LOADER_MAX_CMDS)\n",
                   (unsigned long long)used, (unsigned long long)sizeof(acpiLoader));
            fflush(stdout);
            abort();
        }
        printf("[acpi] table-loader: %llu commands, %llu/%llu bytes used\n",
               (unsigned long long)(used / ACPI_LOADER_CMD_SIZE),
               (unsigned long long)used, (unsigned long long)sizeof(acpiLoader));
        fflush(stdout);
    }
}

void fwCfgBuildFileDir(void) {
    unsigned char *d = fwCfgFileDir;
    fwCfgPutU32BE(d, 0, 4); // 4 files
    d += 4;
    struct { const char *name; UINT32 size; UINT16 select; } files[4] = {
        { "etc/acpi/rsdp",   sizeof(acpiRsdp),   FWCFG_KEY_RSDP },
        { "etc/acpi/tables", sizeof(acpiTables), FWCFG_KEY_TABLES },
        { "etc/table-loader", sizeof(acpiLoader), FWCFG_KEY_LOADER },
        { "etc/ramfb", sizeof(ramfbConfig), FWCFG_KEY_RAMFB },
    };
    int i;
    for (i = 0; i < 4; i++) {
        fwCfgPutU32BE(d, 0, files[i].size);
        UINT16 selectBE = (UINT16)((files[i].select >> 8) | (files[i].select << 8));
        memcpy(d + 4, &selectBE, 2);
        d[6] = 0; d[7] = 0; // reserved
        memset(d + 8, 0, 56);
        strncpy((char *)d + 8, files[i].name, 55);
        d += 64;
    }
    fwCfgFileDirBuilt = 1;
}

// Resolves the currently-selected fw_cfg key to a data buffer + length.
// fw_cfg's own multi-byte integer fields (size/select in the file
// directory, and the two 32-bit header fields below) are big-endian per
// spec, unlike everything else in this file.
void fwCfgSelectItem(unsigned char **outData, UINT32 *outLen) {
    static const unsigned char sigBytes[4] = { 'Q', 'E', 'M', 'U' };
    static unsigned char idBytes[4] = { 1, 0, 0, 0 }; // bit0 only: traditional PIO interface, no DMA

    if (!fwCfgFileDirBuilt) fwCfgBuildFileDir();

    switch (fwCfgSelector) {
        case FWCFG_KEY_SIGNATURE:
            *outData = (unsigned char *)sigBytes; *outLen = 4;
            break;
        case FWCFG_KEY_ID:
            *outData = idBytes; *outLen = 4;
            break;
        case FWCFG_KEY_FILE_DIR:
            *outData = fwCfgFileDir; *outLen = sizeof(fwCfgFileDir);
            break;
        case FWCFG_KEY_RSDP:
            if (!acpiTablesBuilt) { acpiBuildTables(); acpiBuildLoaderScript(); }
            *outData = acpiRsdp; *outLen = sizeof(acpiRsdp);
            break;
        case FWCFG_KEY_TABLES:
            if (!acpiTablesBuilt) { acpiBuildTables(); acpiBuildLoaderScript(); }
            *outData = acpiTables; *outLen = sizeof(acpiTables);
            break;
        case FWCFG_KEY_LOADER:
            if (!acpiTablesBuilt) { acpiBuildTables(); acpiBuildLoaderScript(); }
            *outData = acpiLoader; *outLen = sizeof(acpiLoader);
            break;
        case FWCFG_KEY_RAMFB:
            *outData = ramfbConfig; *outLen = sizeof(ramfbConfig);
            break;
        default:
            *outData = NULL; *outLen = 0;
            break;
    }
}

// Handles I/O ports 0x510 (selector) and 0x511 (data). Data reads support
// both plain `in al, dx` and `rep insb`-style string reads -- fw_cfg is
// explicitly a sequential-read register and real firmware commonly bulk
// -reads with a rep-prefixed string instruction rather than one byte at a
// time, the same reasoning that made ataHandlePioDataPort need both forms.
void fwCfgHandleAccess(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext, void *guestMemPtr) {
    WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitContext->IoPortAccess;
    UINT16 port = io->PortNumber;

    if (port == 0x510) {
        UINT64 rax = 0;
        if (io->AccessInfo.IsWrite) {
            fwCfgSelector = (UINT16)io->Rax;
            fwCfgOffset = 0;
            {
                static int selLogCount = 0;
                if (selLogCount < 40) {
                    selLogCount++;
                    printf("[fwcfg] select #%d: selector=0x%X\n", selLogCount, fwCfgSelector);
                    fflush(stdout);
                }
            }
        }
        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
        WHV_REGISTER_VALUE values[2] = { 0 };
        values[0].Reg64 = rax;
        values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
        WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
        return;
    }

    // port == 0x511
    unsigned char *data = NULL;
    UINT32 dataLen = 0;
    fwCfgSelectItem(&data, &dataLen);

    {
        static int readLogCount = 0;
        if (readLogCount < 60) {
            readLogCount++;
            printf("[fwcfg] read #%d: selector=0x%X offset=%u dataLen=%u stringOp=%d rep=%d\n",
                   readLogCount, fwCfgSelector, fwCfgOffset, dataLen,
                   io->AccessInfo.StringOp, io->AccessInfo.RepPrefix);
            fflush(stdout);
        }
    }

    if (!io->AccessInfo.StringOp) {
        UINT64 word = 0;
        if (!io->AccessInfo.IsWrite && data && fwCfgOffset < dataLen) word = data[fwCfgOffset++];
        else if (io->AccessInfo.IsWrite && data && fwCfgOffset < dataLen) data[fwCfgOffset++] = (unsigned char)(io->Rax & 0xFF);
        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
        WHV_REGISTER_VALUE values[2] = { 0 };
        values[0].Reg64 = word;
        values[1].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
        WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
        return;
    }

    UINT32 count = io->AccessInfo.RepPrefix ? (UINT32)io->Rcx : 1;
    if (count == 0) count = 1;
    UINT64 rdiOut = io->Rdi;
    UINT64 rsiOut = io->Rsi;
    if (!io->AccessInfo.IsWrite) {
        UINT64 destAddr = ((UINT64)io->Es.Base + io->Rdi) & 0xFFFFFFFFULL;
        if (destAddr < guestMemSize) {
            unsigned char *guestPtr = (unsigned char *)guestMemPtr + destAddr;
            UINT32 i;
            UINT32 maxCount = (UINT32)((guestMemSize - destAddr < count) ? (guestMemSize - destAddr) : count);
            for (i = 0; i < maxCount; i++) {
                guestPtr[i] = (data && fwCfgOffset < dataLen) ? data[fwCfgOffset++] : 0;
            }
            rdiOut += maxCount;
        }
    } else {
        // rep outsb -- QemuFwCfgWriteBytes()'s non-DMA path (the only path
        // available here, since we advertise no DMA support). Used for
        // "etc/ramfb": OVMF writes a RAMFB_CONFIG struct describing where
        // it allocated its own framebuffer.
        UINT64 srcAddr = ((UINT64)io->Ds.Base + io->Rsi) & 0xFFFFFFFFULL;
        if (srcAddr < guestMemSize && data) {
            unsigned char *guestPtr = (unsigned char *)guestMemPtr + srcAddr;
            UINT32 i;
            UINT32 maxCount = (UINT32)((guestMemSize - srcAddr < count) ? (guestMemSize - srcAddr) : count);
            for (i = 0; i < maxCount && fwCfgOffset < dataLen; i++) {
                data[fwCfgOffset++] = guestPtr[i];
            }
            rsiOut += maxCount;

            if (fwCfgSelector == FWCFG_KEY_RAMFB && fwCfgOffset >= dataLen) {
                ramfbConfigWritten = 1;
                // RAMFB_CONFIG fields are big-endian (the driver explicitly
                // SwapBytes64/32's them before writing) -- see ramfbConfig's
                // declaration.
                ramfbAddress = ((UINT64)ramfbConfig[0] << 56) | ((UINT64)ramfbConfig[1] << 48) |
                               ((UINT64)ramfbConfig[2] << 40) | ((UINT64)ramfbConfig[3] << 32) |
                               ((UINT64)ramfbConfig[4] << 24) | ((UINT64)ramfbConfig[5] << 16) |
                               ((UINT64)ramfbConfig[6] << 8)  | (UINT64)ramfbConfig[7];
                ramfbWidth  = ((UINT32)ramfbConfig[16] << 24) | ((UINT32)ramfbConfig[17] << 16) |
                              ((UINT32)ramfbConfig[18] << 8)  | (UINT32)ramfbConfig[19];
                ramfbHeight = ((UINT32)ramfbConfig[20] << 24) | ((UINT32)ramfbConfig[21] << 16) |
                              ((UINT32)ramfbConfig[22] << 8)  | (UINT32)ramfbConfig[23];
                ramfbStride = ((UINT32)ramfbConfig[24] << 24) | ((UINT32)ramfbConfig[25] << 16) |
                              ((UINT32)ramfbConfig[26] << 8)  | (UINT32)ramfbConfig[27];
                printf("[ramfb] configured: address=0x%llX %ux%u stride=%u\n",
                       (unsigned long long)ramfbAddress, ramfbWidth, ramfbHeight, ramfbStride);
                fflush(stdout);
            }
        }
    }
    WHV_REGISTER_NAME names[4] = { WHvX64RegisterRcx, WHvX64RegisterRdi, WHvX64RegisterRsi, WHvX64RegisterRip };
    WHV_REGISTER_VALUE values[4] = { 0 };
    values[0].Reg64 = (io->AccessInfo.StringOp && io->AccessInfo.RepPrefix) ? 0 : io->Rcx;
    values[1].Reg64 = rdiOut;
    values[2].Reg64 = rsiOut;
    values[3].Reg64 = exitContext->VpContext.Rip + exitContext->VpContext.InstructionLength;
    WHvSetVirtualProcessorRegisters(partition, 0, names, 4, values);
}

// Translates a guest-VIRTUAL address to a guest-PHYSICAL address by walking
// the guest's own 4-level (PML4/PDPT/PD/PT) page tables from CR3, standard
// x86-64 paging (handles 1GB/2MB large pages via the PS bit). Needed because
// once we're executing NT kernel code, RIP is a canonical high half address
// (0xFFFFF800...) -- unlike the earlier OVMF/bootmgfw.efi diagnostics, which
// could treat guest memory as flat/identity-mapped, kernel virtual addresses
// require a real walk to find the backing physical page.
UINT64 kernelTranslateVA(unsigned char *guestMem, UINT64 cr3, UINT64 va) {
    UINT64 pml4Base = cr3 & ~0xFFFULL;
    UINT64 pml4Idx = (va >> 39) & 0x1FF;
    UINT64 pdptIdx = (va >> 30) & 0x1FF;
    UINT64 pdIdx   = (va >> 21) & 0x1FF;
    UINT64 ptIdx   = (va >> 12) & 0x1FF;
    UINT64 offset  = va & 0xFFF;

    if (pml4Base >= guestMemSize) return (UINT64)-1;
    UINT64 pml4e = *(UINT64 *)(guestMem + pml4Base + pml4Idx * 8);
    if (!(pml4e & 1)) return (UINT64)-1;

    UINT64 pdptBase = pml4e & 0x000FFFFFFFFFF000ULL;
    if (pdptBase >= guestMemSize) return (UINT64)-1;
    UINT64 pdpte = *(UINT64 *)(guestMem + pdptBase + pdptIdx * 8);
    if (!(pdpte & 1)) return (UINT64)-1;
    if (pdpte & 0x80) return (pdpte & 0x000FFFFFC0000000ULL) | (va & 0x3FFFFFFFULL); // 1GB page

    UINT64 pdBase = pdpte & 0x000FFFFFFFFFF000ULL;
    if (pdBase >= guestMemSize) return (UINT64)-1;
    UINT64 pde = *(UINT64 *)(guestMem + pdBase + pdIdx * 8);
    if (!(pde & 1)) return (UINT64)-1;
    if (pde & 0x80) return (pde & 0x000FFFFFFFE00000ULL) | (va & 0x1FFFFFULL); // 2MB page

    UINT64 ptBase = pde & 0x000FFFFFFFFFF000ULL;
    if (ptBase >= guestMemSize) return (UINT64)-1;
    UINT64 pte = *(UINT64 *)(guestMem + ptBase + ptIdx * 8);
    if (!(pte & 1)) return (UINT64)-1;

    UINT64 physBase = pte & 0x000FFFFFFFFFF000ULL;
    if (physBase >= guestMemSize) return (UINT64)-1;
    return physBase | offset;
}

// Reads `len` bytes starting at guest-virtual `va`, translating page-by-page
// (a read can span a page boundary whose pages aren't physically adjacent).
// Returns 0 (and leaves `out` partially written) on any unmapped page.
int kernelReadVA(unsigned char *guestMem, UINT64 cr3, UINT64 va, void *out, UINT64 len) {
    UINT64 done = 0;
    while (done < len) {
        UINT64 curVA = va + done;
        UINT64 phys = kernelTranslateVA(guestMem, cr3, curVA);
        if (phys == (UINT64)-1 || phys >= guestMemSize) return 0;
        UINT64 pageRemain = 0x1000 - (curVA & 0xFFF);
        UINT64 chunk = (len - done) < pageRemain ? (len - done) : pageRemain;
        if (phys + chunk > guestMemSize) return 0;
        memcpy((unsigned char *)out + done, guestMem + phys, chunk);
        done += chunk;
    }
    return 1;
}

// Single-byte write through the guest's own page tables -- sufficient
// for INT3-patching (see the live-breakpoint infrastructure above); no
// need for a general multi-byte writer since we only ever patch one
// opcode byte at a time.
int kernelWriteByteVA(unsigned char *guestMem, UINT64 cr3, UINT64 va, unsigned char val) {
    UINT64 phys = kernelTranslateVA(guestMem, cr3, va);
    if (phys == (UINT64)-1 || phys >= guestMemSize) return 0;
    guestMem[phys] = val;
    return 1;
}

// Same technique as rtcDiagIdentifyModule (scan backward for MZ/PE, parse
// the Debug Directory's RSDS CodeView record for the PDB name), but through
// kernelReadVA instead of flat offsets, since we're now resolving a paged
// NT-kernel-mode RIP rather than an identity-mapped pre-kernel one.
int kernelDiagModuleIdentified = 0;
UINT64 kernelDiagModuleBase = 0;

void kernelDiagIdentifyModule(unsigned char *guestMem, UINT64 cr3, UINT64 rip) {
    if (kernelDiagModuleIdentified) return;
    UINT64 scanStart = (rip > 0x4000000) ? rip - 0x4000000 : 0; // search up to 64MB back
    UINT64 va;
    for (va = (rip & ~0xFFFULL); va >= scanStart; va -= 0x1000) {
        unsigned char hdr[2];
        if (kernelReadVA(guestMem, cr3, va, hdr, 2) && hdr[0] == 'M' && hdr[1] == 'Z') {
            UINT32 peOff = 0;
            if (kernelReadVA(guestMem, cr3, va + 0x3C, &peOff, 4)) {
                unsigned char sig[4] = { 0 };
                if (kernelReadVA(guestMem, cr3, va + peOff, sig, 4) &&
                    sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0) {
                    kernelDiagModuleBase = va;
                    printf("[kerneldiag] found PE header for owning module at guest VA 0x%llX (rip=0x%llX, +0x%llX into it)\n",
                           (unsigned long long)va, (unsigned long long)rip, (unsigned long long)(rip - va));
                    UINT32 debugDirRva = 0;
                    kernelReadVA(guestMem, cr3, va + peOff + 184, &debugDirRva, 4);
                    if (debugDirRva == 0) {
                        printf("[kerneldiag] no debug directory RVA in this module's PE header\n");
                        fflush(stdout);
                        kernelDiagModuleIdentified = 1;
                        return;
                    }
                    unsigned char dbgDirBytes[28] = { 0 };
                    kernelReadVA(guestMem, cr3, va + debugDirRva, dbgDirBytes, sizeof(dbgDirBytes));
                    UINT32 dbgType = *(UINT32 *)(dbgDirBytes + 0xC);
                    UINT32 cvRva = *(UINT32 *)(dbgDirBytes + 0x14);
                    printf("[kerneldiag] debug directory: type=%u codeViewRva=0x%X\n", dbgType, cvRva);
                    if (dbgType == 2 /* IMAGE_DEBUG_TYPE_CODEVIEW */ && cvRva != 0) {
                        unsigned char cv[24] = { 0 };
                        kernelReadVA(guestMem, cr3, va + cvRva, cv, sizeof(cv));
                        if (cv[0] == 'R' && cv[1] == 'S' && cv[2] == 'D' && cv[3] == 'S') {
                            unsigned char guidAge[20] = { 0 }; // 16-byte GUID + 4-byte Age
                            kernelReadVA(guestMem, cr3, va + cvRva + 4, guidAge, sizeof(guidAge));
                            char pdbName[128] = { 0 };
                            kernelReadVA(guestMem, cr3, va + cvRva + 4 + 16 + 4, pdbName, sizeof(pdbName) - 1);
                            pdbName[sizeof(pdbName) - 1] = 0;
                            UINT32 g1 = *(UINT32 *)(guidAge + 0);
                            UINT16 g2 = *(UINT16 *)(guidAge + 4);
                            UINT16 g3 = *(UINT16 *)(guidAge + 6);
                            UINT32 age = *(UINT32 *)(guidAge + 16);
                            printf("[kerneldiag] module PDB name: %s guid=%08X-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X age=%u\n",
                                   pdbName, g1, g2, g3,
                                   guidAge[8], guidAge[9], guidAge[10], guidAge[11],
                                   guidAge[12], guidAge[13], guidAge[14], guidAge[15], age);
                        } else {
                            printf("[kerneldiag] CodeView record doesn't start with RSDS (got %02X %02X %02X %02X)\n",
                                   cv[0], cv[1], cv[2], cv[3]);
                        }
                    }
                    fflush(stdout);
                    kernelDiagModuleIdentified = 1;
                    return;
                }
            }
        }
        if (va < 0x1000) break;
    }
    printf("[kerneldiag] no PE header found scanning VA back from rip=0x%llX\n", (unsigned long long)rip);
    fflush(stdout);
    kernelDiagModuleIdentified = 1;
}

// Same PE-header-walk-back + PDB-name technique as kernelDiagIdentifyModule,
// but NOT gated by the one-shot kernelDiagModuleIdentified flag (already
// consumed identifying the unnamed spin-loop module -- see
// docs/investigations/post-vppt-boot-stall.md) and returning the found
// base via an out-param instead of a global, so it can be reused for an
// arbitrary address (e.g. a candidate return address on the stack)
// without disturbing that earlier state. Returns 1 and sets *outBase if a
// PE header was found, 0 otherwise.
int kernelDiagIdentifyModuleAt(unsigned char *guestMem, UINT64 cr3, UINT64 addr, UINT64 *outBase) {
    UINT64 scanStart = (addr > 0x4000000) ? addr - 0x4000000 : 0;
    UINT64 va;
    for (va = (addr & ~0xFFFULL); va >= scanStart; va -= 0x1000) {
        unsigned char hdr[2];
        if (kernelReadVA(guestMem, cr3, va, hdr, 2) && hdr[0] == 'M' && hdr[1] == 'Z') {
            UINT32 peOff = 0;
            if (kernelReadVA(guestMem, cr3, va + 0x3C, &peOff, 4)) {
                unsigned char sig[4] = { 0 };
                if (kernelReadVA(guestMem, cr3, va + peOff, sig, 4) &&
                    sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0) {
                    *outBase = va;
                    printf("[kerneldiag] candidate address 0x%llX resolves to module base 0x%llX (+0x%llX into it)\n",
                           (unsigned long long)addr, (unsigned long long)va, (unsigned long long)(addr - va));
                    UINT32 debugDirRva = 0;
                    kernelReadVA(guestMem, cr3, va + peOff + 184, &debugDirRva, 4);
                    if (debugDirRva == 0) {
                        printf("[kerneldiag] no debug directory RVA in this module's PE header\n");
                        fflush(stdout);
                        return 1;
                    }
                    unsigned char dbgDirBytes[28] = { 0 };
                    kernelReadVA(guestMem, cr3, va + debugDirRva, dbgDirBytes, sizeof(dbgDirBytes));
                    UINT32 dbgType = *(UINT32 *)(dbgDirBytes + 0xC);
                    UINT32 cvRva = *(UINT32 *)(dbgDirBytes + 0x14);
                    printf("[kerneldiag] debug directory: type=%u codeViewRva=0x%X\n", dbgType, cvRva);
                    if (dbgType == 2 && cvRva != 0) {
                        unsigned char cv[24] = { 0 };
                        kernelReadVA(guestMem, cr3, va + cvRva, cv, sizeof(cv));
                        if (cv[0] == 'R' && cv[1] == 'S' && cv[2] == 'D' && cv[3] == 'S') {
                            unsigned char guidAge[20] = { 0 };
                            kernelReadVA(guestMem, cr3, va + cvRva + 4, guidAge, sizeof(guidAge));
                            char pdbName[128] = { 0 };
                            kernelReadVA(guestMem, cr3, va + cvRva + 4 + 16 + 4, pdbName, sizeof(pdbName) - 1);
                            pdbName[sizeof(pdbName) - 1] = 0;
                            UINT32 g1 = *(UINT32 *)(guidAge + 0);
                            UINT16 g2 = *(UINT16 *)(guidAge + 4);
                            UINT16 g3 = *(UINT16 *)(guidAge + 6);
                            UINT32 age = *(UINT32 *)(guidAge + 16);
                            printf("[kerneldiag] module PDB name: %s guid=%08X-%04X-%04X-%02X%02X%02X%02X%02X%02X%02X%02X age=%u\n",
                                   pdbName, g1, g2, g3,
                                   guidAge[8], guidAge[9], guidAge[10], guidAge[11],
                                   guidAge[12], guidAge[13], guidAge[14], guidAge[15], age);
                        }
                    }
                    fflush(stdout);
                    return 1;
                }
            }
        }
        if (va < 0x1000) break;
    }
    printf("[kerneldiag] no PE header found scanning VA back from 0x%llX\n", (unsigned long long)addr);
    fflush(stdout);
    return 0;
}

// Stall watchdog: WHvGetVirtualProcessorRegisters is documented-safe to call
// from a thread other than the one blocked inside WHvRunVirtualProcessor, so
// this thread periodically samples exitCount (updated by the main loop) and,
// if it hasn't moved, reads live RIP/RSP/RAX/RCX/RDX/RBX so we can identify
// what code the guest is actually spinning in during an indefinite stall --
// otherwise invisible, since a stall with zero port traps produces no exits
// at all to log from the main loop itself.
volatile LONG64 g_watchdogExitCount = 0;
WHV_PARTITION_HANDLE g_watchdogPartition = NULL;

// Post-bugcheck-fix (2026-07-16 part 10) follow-up investigation: the new
// stall's RIP sits in a snapshot-and-spin loop polling a single live VA.
// Set once (stall tick 2) so later ticks can re-poll it and show whether
// it's genuinely frozen or just slow -- see the stallWatchdogThread body.
UINT64 g_spinWaitTargetVA = 0;
UINT64 g_spinWaitCr3 = 0;

// See the CreateThread call site's comment for why this exists: forces
// WHvRunVirtualProcessor to return periodically so the RTC periodic
// interrupt (deliverRtcPeriodicIrq) can actually be delivered even while
// the guest is CPU-bound in a busy-spin with no VM exits of its own.
//
// A broadened version of this (unconditionally forcing a cancel every 10ms
// regardless of RTC state, to also cover deliverPitTimerIrq) was tried and
// reverted: it made an unrelated early-boot stall (SeaBIOS spinning on
// PM1a_CNT, port 0x604) reproduce 100% of the time instead of the
// pre-existing intermittent behavior, and bisection proved the stall isn't
// in any of this hypervisor's own per-iteration handlers -- strong evidence
// that calling WHvCancelRunVirtualProcessor this aggressively and
// continuously (not just while something is actually pending) perturbs WHV
// itself rather than helping. Left gated on RTC periodic mode only, as
// originally designed.
DWORD WINAPI rtcCancelThread(LPVOID param) {
    (void)param;
    for (;;) {
        if ((cmosRegisters[0x0B] & 0x40) && rtcPeriodicIntervalMs(cmosRegisters[0x0A]) > 0.0 && g_watchdogPartition) {
            WHvCancelRunVirtualProcessor(g_watchdogPartition, 0, 0);
            Sleep(1);
        } else {
            Sleep(50);
        }
    }
    return 0;
}

DWORD WINAPI stallWatchdogThread(LPVOID param) {
    (void)param;
    LONG64 lastSeen = -1;
    int stallTicks = 0;
    for (;;) {
        Sleep(2000);
        if (!g_watchdogPartition) continue;
        LONG64 current = g_watchdogExitCount;
        if (current == lastSeen && current > 0) {
            stallTicks++;
            // CR8 (added 2026-07-16, post-VPPT-fix stall investigation --
            // see docs/investigations/post-vppt-boot-stall.md): on x64,
            // CR8 directly holds the current IRQL (TPR). Cheapest possible
            // discriminator between the three live hypotheses for that
            // stall -- IRQL >= DISPATCH_LEVEL (2) means the spinning code
            // itself is blocking DPC delivery (points at DPC/scheduler
            // starvation); IRQL <= APC_LEVEL (1) means DPCs would run fine
            // if queued, shifting weight toward a missing interrupt source
            // or a wait on state only some other, unscheduled thread can
            // set. No new hardware emulation, just one more register in
            // this already-existing read.
            WHV_REGISTER_NAME names[8] = {
                WHvX64RegisterRip, WHvX64RegisterRsp, WHvX64RegisterRax,
                WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRbx, WHvX64RegisterCr3,
                WHvX64RegisterCr8
            };
            WHV_REGISTER_VALUE values[8] = { 0 };
            HRESULT hr = WHvGetVirtualProcessorRegisters(g_watchdogPartition, 0, names, 8, values);
            if (SUCCEEDED(hr)) {
                UINT64 irql = values[7].Reg64;
                const char *irqlName = (irql == 0) ? "PASSIVE_LEVEL" :
                                        (irql == 1) ? "APC_LEVEL" :
                                        (irql == 2) ? "DISPATCH_LEVEL" :
                                        (irql < 13) ? "DIRQL(device)" :
                                        (irql == 13) ? "CLOCK_LEVEL" :
                                        (irql == 14) ? "IPI_LEVEL" :
                                        (irql == 15) ? "HIGH_LEVEL" : "unknown";
                printf("[watchdog] STALL #%d (exitCount=%lld unchanged for %ds): "
                       "rip=0x%llX rsp=0x%llX rax=0x%llX rcx=0x%llX rdx=0x%llX rbx=0x%llX cr3=0x%llX cr8(irql)=0x%llX(%s)\n",
                       stallTicks, (long long)current, stallTicks * 2,
                       (unsigned long long)values[0].Reg64, (unsigned long long)values[1].Reg64,
                       (unsigned long long)values[2].Reg64, (unsigned long long)values[3].Reg64,
                       (unsigned long long)values[4].Reg64, (unsigned long long)values[5].Reg64,
                       (unsigned long long)values[6].Reg64, (unsigned long long)irql, irqlName);
                if (stallTicks == 2 && guestMemory) {
                    kernelDiagIdentifyModule((unsigned char *)guestMemory, values[6].Reg64, values[0].Reg64);

                    // Ring buffer of the most recent PCI config accesses --
                    // see its declaration for why this exists. Print in
                    // chronological order (oldest of the retained entries
                    // first).
                    if (g_pciCfgRingCount > 0) {
                        printf("[kerneldiag] last %d PCI config accesses before this stall:\n", g_pciCfgRingCount);
                        int ri;
                        int startIdx = (g_pciCfgRingCount < PCI_CFG_RING_SIZE) ? 0 : g_pciCfgRingPos;
                        for (ri = 0; ri < g_pciCfgRingCount; ri++) {
                            int idx = (startIdx + ri) % PCI_CFG_RING_SIZE;
                            PciCfgRingEntry *e = &g_pciCfgRing[idx];
                            printf("  bus=%u dev=%u func=%u off=0x%02X write=%u val=0x%llX\n",
                                   e->bus, e->dev, e->func, e->offset, e->isWrite, (unsigned long long)e->val);
                        }
                        fflush(stdout);
                    }

                    // Post-2026-07-16-part-10 stall investigation: this is
                    // a NEW stall (the RTC-interrupt fix resolved the
                    // original bugcheck 0x5C) -- everything below this
                    // block assumes a bugchecked/halted kernel, which no
                    // longer applies. Dump raw bytes around the live RIP
                    // directly (no symbol resolution needed/available --
                    // kernelDiagIdentifyModule already reported "no debug
                    // directory" for whatever module this RIP is in) so we
                    // can disassemble offline and see what's actually
                    // executing.
                    {
                        UINT64 dumpStart = (values[0].Reg64 >= 0x40) ? values[0].Reg64 - 0x40 : values[0].Reg64;
                        unsigned char ripBuf[0x300] = { 0 };
                        if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, dumpStart, ripBuf, sizeof(ripBuf))) {
                            char path[512];
                            _snprintf_s(path, sizeof(path), _TRUNCATE,
                                        "C:\\Users\\DELL\\AppData\\Local\\Temp\\claude\\c--Users-DELL-OneDrive-Desktop-LocalHost-py\\5bed7398-339c-497a-9383-821cf9d2769e\\scratchpad\\NewStallRip.bin");
                            FILE *f = fopen(path, "wb");
                            if (f) { fwrite(ripBuf, 1, sizeof(ripBuf), f); fclose(f); }
                            printf("[kerneldiag] dumped %llu bytes around live RIP (starting 0x%llX, RIP itself at offset 0x%llX) to %s\n",
                                   (unsigned long long)sizeof(ripBuf), (unsigned long long)dumpStart,
                                   (unsigned long long)(values[0].Reg64 - dumpStart), path);
                        } else {
                            printf("[kerneldiag] failed to read bytes around live RIP 0x%llX\n", (unsigned long long)values[0].Reg64);
                        }
                        fflush(stdout);
                    }

                    // Disassembly of the dump above (offline) showed RIP
                    // sitting in a snapshot-and-spin loop: capture a live
                    // value once into a local, then busy-poll that SAME
                    // address on every iteration (pause; jmp back) waiting
                    // for it to differ from the snapshot. Both reads
                    // resolve (worked out by hand from the RIP-relative
                    // displacements) to VA = RIP + 0x1060, where RIP here
                    // is this exact tick-2 sample, empirically always
                    // caught at the loop's `jmp` instruction. Record that
                    // address once so later ticks (below) can poll the
                    // live value over time and show whether it's genuinely
                    // frozen or just slow.
                    g_spinWaitTargetVA = values[0].Reg64 + 0x1060;
                    g_spinWaitCr3 = values[6].Reg64;
                    UINT64 initialSpinVal = 0;
                    if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, g_spinWaitTargetVA, &initialSpinVal, 8)) {
                        printf("[kerneldiag] spin-wait target VA=0x%llX initial value=0x%llX -- will re-poll on later stall ticks\n",
                               (unsigned long long)g_spinWaitTargetVA, (unsigned long long)initialSpinVal);

                        // Per an explicit user request: identify where the
                        // watched address is allocated from -- a known
                        // kernel object, driver-owned pool memory, or a
                        // static/global region. It's at a FIXED RVA from
                        // the module base (RIP+0x1060, and RIP is always
                        // module_base+0x1188, so the target is always
                        // module_base+0x21E8 -- same offset every boot,
                        // only the base itself moves with KASLR). A fixed,
                        // small offset directly into the module's own
                        // image -- not a far-away pool address -- is
                        // already suggestive of a static global rather
                        // than a dynamically allocated kernel object.
                        // Confirm precisely by reading the module's own PE
                        // section table and finding which section (and
                        // whether it's a zero-fill/.bss-style region)
                        // contains this RVA.
                        if (kernelDiagModuleBase != 0) {
                            UINT32 peOff = 0;
                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, kernelDiagModuleBase + 0x3C, &peOff, 4)) {
                                UINT16 numSections = 0, sizeOfOptHdr = 0;
                                kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, kernelDiagModuleBase + peOff + 4 + 2, &numSections, 2);
                                kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, kernelDiagModuleBase + peOff + 4 + 16, &sizeOfOptHdr, 2);
                                UINT64 sectionTableVA = kernelDiagModuleBase + peOff + 4 + 20 + sizeOfOptHdr;
                                UINT64 targetRVA = g_spinWaitTargetVA - kernelDiagModuleBase;
                                printf("[kerneldiag] module PE: %u sections, watched target RVA=0x%llX\n",
                                       numSections, (unsigned long long)targetRVA);
                                int si;
                                for (si = 0; si < numSections && si < 20; si++) {
                                    unsigned char sec[40] = { 0 };
                                    if (!kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, sectionTableVA + (UINT64)si * 40, sec, 40)) break;
                                    char secName[9] = { 0 };
                                    memcpy(secName, sec, 8);
                                    UINT32 virtSize = *(UINT32 *)&sec[8];
                                    UINT32 virtAddr = *(UINT32 *)&sec[12];
                                    UINT32 rawSize = *(UINT32 *)&sec[16];
                                    UINT32 rawPtr = *(UINT32 *)&sec[20];
                                    UINT32 characteristics = *(UINT32 *)&sec[36];
                                    int containsTarget = (targetRVA >= virtAddr && targetRVA < (UINT64)virtAddr + virtSize);
                                    int targetInZeroFillTail = containsTarget && rawSize < virtSize &&
                                                                (targetRVA - virtAddr) >= rawSize;
                                    printf("[kerneldiag] section[%d] name=%s VA=0x%X VSize=0x%X RawPtr=0x%X RawSize=0x%X chars=0x%08X%s%s\n",
                                           si, secName, virtAddr, virtSize, rawPtr, rawSize, characteristics,
                                           containsTarget ? "  <-- CONTAINS WATCHED TARGET" : "",
                                           targetInZeroFillTail ? " (in the zero-fill tail beyond SizeOfRawData -- .bss-style, never had explicit initialized content in the image)" : "");
                                }
                            }
                            fflush(stdout);
                        }

                        // No debug directory means no PDB-based symbol
                        // resolution is possible for this module -- dump a
                        // larger chunk from its base so we can grep it
                        // offline for readable strings (driver name,
                        // copyright, etc.), the next best identification
                        // technique available.
                        if (kernelDiagModuleBase != 0) {
                            unsigned char *modBuf = (unsigned char *)malloc(0x4000);
                            if (modBuf && kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, kernelDiagModuleBase, modBuf, 0x4000)) {
                                char path[512];
                                _snprintf_s(path, sizeof(path), _TRUNCATE,
                                            "C:\\Users\\DELL\\AppData\\Local\\Temp\\claude\\c--Users-DELL-OneDrive-Desktop-LocalHost-py\\5bed7398-339c-497a-9383-821cf9d2769e\\scratchpad\\NewStallModule.bin");
                                FILE *mf = fopen(path, "wb");
                                if (mf) { fwrite(modBuf, 1, 0x4000, mf); fclose(mf); }
                                printf("[kerneldiag] dumped 0x4000 bytes of unidentified module (base=0x%llX) to %s\n",
                                       (unsigned long long)kernelDiagModuleBase, path);
                            }
                            if (modBuf) free(modBuf);
                        }

                        // Evidence-driven follow-up per the user's explicit
                        // direction: only inspect the watched address's
                        // surrounding memory if CR8 says DPCs are actually
                        // permitted right now (irql <= APC_LEVEL) -- if
                        // IRQL were raised, that alone would already
                        // explain the stall (DPC/scheduler starvation) and
                        // this extra step wouldn't be needed to distinguish
                        // further. Otherwise, dump enough bytes to
                        // recognize a DISPATCHER_HEADER (Type/flags +
                        // SignalState in the first 8 bytes, a WaitListHead
                        // Flink/Blink pair -- two consecutive plausible
                        // kernel pointers -- in the next 16) versus a plain
                        // shared flag/counter (no such structure, likely
                        // zeros or unrelated data around it).
                        if (irql <= 1) {
                            UINT64 ctxStart = (g_spinWaitTargetVA >= 0x10) ? g_spinWaitTargetVA - 0x10 : g_spinWaitTargetVA;
                            unsigned char ctxBuf[0x40] = { 0 };
                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, ctxStart, ctxBuf, sizeof(ctxBuf))) {
                                printf("[kerneldiag] memory around spin-wait target (IRQL permits DPCs, irql=0x%llX):\n", (unsigned long long)irql);
                                int mi;
                                for (mi = 0; mi < (int)sizeof(ctxBuf); mi += 16) {
                                    printf("  target%+d:", mi - 0x10);
                                    int mj;
                                    for (mj = 0; mj < 16; mj++) printf(" %02X", ctxBuf[mi + mj]);
                                    printf("\n");
                                }
                                UINT64 possibleFlink = *(UINT64 *)&ctxBuf[0x18];
                                UINT64 possibleBlink = *(UINT64 *)&ctxBuf[0x20];
                                int flinkLooksLikePointer = (possibleFlink >= 0xFFFF800000000000ULL);
                                int blinkLooksLikePointer = (possibleBlink >= 0xFFFF800000000000ULL);
                                printf("[kerneldiag] bytes at target+0x08/+0x10 (candidate WaitListHead Flink/Blink) = 0x%llX / 0x%llX -- %s\n",
                                       (unsigned long long)possibleFlink, (unsigned long long)possibleBlink,
                                       (flinkLooksLikePointer && blinkLooksLikePointer) ? "BOTH look like plausible kernel pointers -- consistent with a real dispatcher object (KEVENT/KTIMER/etc.)"
                                       : "does NOT look like a pointer pair -- more consistent with a plain shared flag/counter, not a dispatcher object");
                            } else {
                                printf("[kerneldiag] failed to read memory around spin-wait target\n");
                            }
                        } else {
                            printf("[kerneldiag] IRQL 0x%llX (%s) does not permit DPCs -- deferring memory inspection, this alone may explain the stall\n",
                                   (unsigned long long)irql, irqlName);
                        }

                        // Who raised IRQL to HIGH_LEVEL and called into
                        // this spin routine? The function's own prologue
                        // (already disassembled: push rbp; mov rbp,rsp;
                        // sub rsp,0x10) means the return address sits at
                        // [rsp+0x18] relative to wherever we sampled RSP
                        // inside the loop body (rbp = sampled_rsp+0x10,
                        // return address = [rbp+8]). Resolve its owning
                        // module -- if it's ntoskrnl (which has full PDB
                        // symbols, unlike the unnamed spin-loop module),
                        // this identifies the actual calling routine by
                        // name via the same dbghelp toolchain used
                        // throughout the earlier VPPT investigation.
                        {
                            UINT64 returnAddr = 0;
                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, values[1].Reg64 + 0x18, &returnAddr, 8)) {
                                printf("[kerneldiag] candidate return address (caller) at [rsp+0x18] = 0x%llX\n", (unsigned long long)returnAddr);
                                UINT64 callerModuleBase = 0;
                                if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, values[6].Reg64, returnAddr, &callerModuleBase)) {
                                    unsigned char *callerBuf = (unsigned char *)malloc(0x300);
                                    UINT64 callerDumpStart = (returnAddr >= 0x100) ? returnAddr - 0x100 : returnAddr;
                                    if (callerBuf && kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, callerDumpStart, callerBuf, 0x300)) {
                                        char path[512];
                                        _snprintf_s(path, sizeof(path), _TRUNCATE,
                                                    "C:\\Users\\DELL\\AppData\\Local\\Temp\\claude\\c--Users-DELL-OneDrive-Desktop-LocalHost-py\\5bed7398-339c-497a-9383-821cf9d2769e\\scratchpad\\CallerContext.bin");
                                        FILE *cf = fopen(path, "wb");
                                        if (cf) { fwrite(callerBuf, 1, 0x300, cf); fclose(cf); }
                                        printf("[kerneldiag] dumped 0x300 bytes around return address (starting 0x%llX, retaddr at offset 0x%llX, module base 0x%llX, RVA of retaddr 0x%llX) to %s\n",
                                               (unsigned long long)callerDumpStart, (unsigned long long)(returnAddr - callerDumpStart),
                                               (unsigned long long)callerModuleBase, (unsigned long long)(returnAddr - callerModuleBase), path);
                                    }
                                    if (callerBuf) free(callerBuf);

                                    // Per an explicit user request: continuing
                                    // the producer trace by checking the
                                    // OTHER slots in the same registration
                                    // object our watched driver's callback
                                    // lives in. Offline disassembly (hand-
                                    // verified with PowerShell arithmetic,
                                    // not error-prone manual hex math)
                                    // confirmed three call sites -- offsets
                                    // +0x10 (leads to our watched driver),
                                    // +0x28, +0x40 -- all load their object
                                    // pointer from the SAME RIP-relative
                                    // global, resolving to caller-module RVA
                                    // 0xCA9008. Read that pointer live, then
                                    // dump a broad range of the object's own
                                    // fields directly -- faster and more
                                    // complete than continuing to manually
                                    // catalog individual call sites from
                                    // static disassembly.
                                    UINT64 regObjPtrVA = callerModuleBase + 0xCA9008;
                                    UINT64 regObjAddr = 0;
                                    if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, regObjPtrVA, &regObjAddr, 8)) {
                                        printf("[kerneldiag] registration object pointer at caller+0xCA9008 (VA=0x%llX) = 0x%llX\n",
                                               (unsigned long long)regObjPtrVA, (unsigned long long)regObjAddr);
                                        if (regObjAddr != 0) {
                                            unsigned char regObj[0x200] = { 0 };
                                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, regObjAddr, regObj, sizeof(regObj))) {
                                                printf("[kerneldiag] registration object @0x%llX, slots (offset: value):\n", (unsigned long long)regObjAddr);
                                                int oi;
                                                for (oi = 0; oi < (int)sizeof(regObj); oi += 8) {
                                                    UINT64 slotVal = *(UINT64 *)&regObj[oi];
                                                    if (slotVal != 0) {
                                                        printf("  +0x%03X = 0x%016llX%s\n", oi, (unsigned long long)slotVal,
                                                               (oi == 0x10) ? "  <-- confirmed: leads to our watched driver" :
                                                               (oi == 0x28 || oi == 0x40) ? "  <-- confirmed slot (different callback)" : "");
                                                    }
                                                }
                                            } else {
                                                printf("[kerneldiag] failed to read registration object at 0x%llX\n", (unsigned long long)regObjAddr);
                                            }
                                        }
                                    } else {
                                        printf("[kerneldiag] failed to read registration object pointer at 0x%llX\n", (unsigned long long)regObjPtrVA);
                                    }
                                    fflush(stdout);

                                    // Continuing the producer trace: the
                                    // three confirmed dispatcher wrapper
                                    // blocks (offsets +0x10/+0x28/+0x40)
                                    // are spaced roughly 0x74-0x88 bytes
                                    // apart in the caller's code, suggesting
                                    // many more exist nearby covering the
                                    // rest of the registration object's
                                    // slots, including the two null-slot
                                    // gaps found (+0x048-+0x088,
                                    // +0x158-+0x1A8). Dump a much wider
                                    // stretch of this same function (8KB,
                                    // comfortably covering ~48+ blocks at
                                    // that spacing) for offline
                                    // disassembly, to find which specific
                                    // offsets have their own dispatcher
                                    // block (real, defined callback types
                                    // that are simply unregistered) versus
                                    // offsets never referenced at all.
                                    if (callerModuleBase != 0) {
                                        unsigned char *wideBuf = (unsigned char *)malloc(0x2000);
                                        UINT64 wideDumpStart = callerModuleBase + 0x36D000;
                                        if (wideBuf && kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, wideDumpStart, wideBuf, 0x2000)) {
                                            char path[512];
                                            _snprintf_s(path, sizeof(path), _TRUNCATE,
                                                        "C:\\Users\\DELL\\AppData\\Local\\Temp\\claude\\c--Users-DELL-OneDrive-Desktop-LocalHost-py\\5bed7398-339c-497a-9383-821cf9d2769e\\scratchpad\\CallerWideDump.bin");
                                            FILE *wf = fopen(path, "wb");
                                            if (wf) { fwrite(wideBuf, 1, 0x2000, wf); fclose(wf); }
                                            printf("[kerneldiag] dumped 0x2000 bytes of caller dispatcher code (starting caller+0x36D000, VA=0x%llX) to %s\n",
                                                   (unsigned long long)wideDumpStart, path);
                                        } else {
                                            printf("[kerneldiag] failed to read wide caller dump at 0x%llX\n", (unsigned long long)wideDumpStart);
                                        }
                                        if (wideBuf) free(wideBuf);
                                    }
                                    fflush(stdout);
                                }
                            } else {
                                printf("[kerneldiag] failed to read candidate return address at [rsp+0x18]\n");
                            }

                            // Neither the watched routine's own .text
                            // (exhaustively checked -- zero `mov cr8` and
                            // zero `call` instructions anywhere in its
                            // 4096-byte section) nor the immediate
                            // dispatch-wrapper caller (cli/sti only, no
                            // CR8 touch) raise IRQL. Rather than guessing
                            // among the many CR8-touching helper routines
                            // found in the wide static dump, walk the
                            // guest's own live stack and resolve every
                            // candidate kernel-mode return address to its
                            // owning module + RVA -- this shows exactly
                            // which functions are really on the call
                            // chain, no speculation needed.
                            {
                                printf("[kerneldiag] scanning stack from rsp=0x%llX for candidate return addresses:\n",
                                       (unsigned long long)values[1].Reg64);
                                int si;
                                for (si = 0; si < 192; si++) {
                                    UINT64 slotVA = values[1].Reg64 + (UINT64)si * 8;
                                    UINT64 qval = 0;
                                    if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, slotVA, &qval, 8) &&
                                        qval >= 0xFFFF800000000000ULL) {
                                        UINT64 candModuleBase = 0;
                                        printf("  [rsp+0x%03X] = 0x%llX", si * 8, (unsigned long long)qval);
                                        if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, values[6].Reg64, qval, &candModuleBase)) {
                                            printf("    ^ [rsp+0x%03X] module base 0x%llX RVA 0x%llX\n",
                                                   si * 8, (unsigned long long)candModuleBase, (unsigned long long)(qval - candModuleBase));
                                            // Dump a code window around every
                                            // resolved candidate -- cheap
                                            // (small reads), and lets us
                                            // offline-disassemble whichever
                                            // ones turn out to belong to a
                                            // named module (e.g. one with a
                                            // real PDB) after the fact.
                                            unsigned char candBuf[0x180];
                                            UINT64 candDumpStart = (qval >= 0x100) ? qval - 0x100 : qval;
                                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, candDumpStart, candBuf, sizeof(candBuf))) {
                                                char cpath[512];
                                                _snprintf_s(cpath, sizeof(cpath), _TRUNCATE,
                                                            "C:\\Users\\DELL\\AppData\\Local\\Temp\\claude\\c--Users-DELL-OneDrive-Desktop-LocalHost-py\\5bed7398-339c-497a-9383-821cf9d2769e\\scratchpad\\StackCand_0x%X.bin",
                                                            si * 8);
                                                FILE *cfp = fopen(cpath, "wb");
                                                if (cfp) { fwrite(candBuf, 1, sizeof(candBuf), cfp); fclose(cfp); }
                                                printf("    ^ dumped 0x180 bytes (starting 0x%llX, candidate at offset 0x100) to %s\n",
                                                       (unsigned long long)candDumpStart, cpath);
                                            }
                                        } else {
                                            printf("\n");
                                        }
                                    }
                                }
                                fflush(stdout);
                            }
                            fflush(stdout);
                        }
                    } else {
                        printf("[kerneldiag] failed to read spin-wait target VA=0x%llX\n", (unsigned long long)g_spinWaitTargetVA);
                        g_spinWaitTargetVA = 0;
                    }
                    fflush(stdout);
                }

                if (stallTicks > 2 && g_spinWaitTargetVA != 0 && guestMemory) {
                    UINT64 spinVal = 0;
                    if (kernelReadVA((unsigned char *)guestMemory, g_spinWaitCr3, g_spinWaitTargetVA, &spinVal, 8)) {
                        printf("[kerneldiag] spin-wait target value at tick %d = 0x%llX\n", stallTicks, (unsigned long long)spinVal);
                    }
                    fflush(stdout);
                }

                static int stackDumpRanOnce = 0;
                if (stallTicks == 3 && !stackDumpRanOnce && guestMemory) {
                    stackDumpRanOnce = 1;
                    // HaliHaltSystem is normally reached via a `call`, not a
                    // `jmp` -- dump the stack to recover the return address
                    // chain and find out what actually decided to halt.
                    printf("[kerneldiag] stack dump from rsp=0x%llX (cr3=0x%llX):\n",
                           (unsigned long long)values[1].Reg64, (unsigned long long)values[6].Reg64);
                    int qi;
                    for (qi = 0; qi < 64; qi++) {
                        UINT64 slotVA = values[1].Reg64 + (UINT64)qi * 8;
                        UINT64 qval = 0;
                        if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, slotVA, &qval, 8) &&
                            qval >= 0xFFFF800000000000ULL) {
                            printf("  [rsp+0x%02X] = 0x%llX\n", qi * 8, (unsigned long long)qval);
                        }
                    }
                    // KiBugCheckData RVA (resolved offline via ntkrnlmp.pdb +
                    // dbghelp, same GUID/age this kernel build reported
                    // earlier): a fixed 5x UINT64 global -- [0]=bugcheck
                    // code, [1..4]=its four parameters. The stack showed
                    // KeBugCheck2/KiBugCheckProgress frames, meaning this
                    // ISN'T a calibration stall -- the kernel bugchecked and
                    // HaliHaltSystem is the terminal halt loop that follows.
                    if (kernelDiagModuleBase != 0) {
                        UINT64 bugCheckData[5] = { 0 };
                        UINT64 kiBugCheckDataVA = kernelDiagModuleBase + 0xC2B3E0;
                        if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, kiBugCheckDataVA, bugCheckData, sizeof(bugCheckData))) {
                            printf("[kerneldiag] KiBugCheckData: code=0x%llX params=[0x%llX, 0x%llX, 0x%llX, 0x%llX]\n",
                                   (unsigned long long)bugCheckData[0], (unsigned long long)bugCheckData[1],
                                   (unsigned long long)bugCheckData[2], (unsigned long long)bugCheckData[3],
                                   (unsigned long long)bugCheckData[4]);
                        } else {
                            printf("[kerneldiag] failed to read KiBugCheckData at VA 0x%llX\n", (unsigned long long)kiBugCheckDataVA);
                        }
                        // U40: profile of the pass that just bugchecked. This is the
                        // pass-2 execution picture that breakpoints cannot produce,
                        // since it includes the pre-discovery region.
                        u40Dump(g_sawReset ? 1 : 0, "at bugcheck");
                        u46DumpLapic(g_watchdogPartition, g_sawReset ? "at bugcheck (pass 2)" : "at bugcheck (pass 1)");
                        printf("[u42] IOAPIC accesses at bugcheck: pass1=%ld pass2=%ld\n",
                               g_ioapicPassAccesses[0], g_ioapicPassAccesses[1]);
                        printf("[u43] non-legacy vector resolutions: pass1=%ld pass2=%ld  (RTC PIE bit now 0x%02X, inherited across the reset)\n",
                               g_resolveNonLegacyCount[0], g_resolveNonLegacyCount[1],
                               cmosRegisters[0x0B] & 0x40);
                        fflush(stdout);

                        // Bugcheck 0x139 (KERNEL_SECURITY_CHECK_FAILURE)
                        // seen after the experimental port-0x64/0xFE reset
                        // handler: per its documented parameter layout,
                        // param1=Type (0x3 = FAST_FAIL_CORRUPT_LIST_ENTRY),
                        // param2=trap frame address, param3=EXCEPTION_RECORD
                        // address, param4=reserved. Parse the exception
                        // record directly to find the exact instruction
                        // that detected the corruption -- far more precise
                        // than guessing which reset-incomplete device state
                        // is at fault.
                        if (bugCheckData[0] == 0x139 && bugCheckData[3] != 0) {
                            unsigned char er[0x30] = { 0 };
                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, bugCheckData[3], er, sizeof(er))) {
                                UINT32 exCode = *(UINT32 *)&er[0x00];
                                UINT64 exAddress = *(UINT64 *)&er[0x10];
                                UINT32 numParams = *(UINT32 *)&er[0x18];
                                UINT64 exInfo0 = *(UINT64 *)&er[0x20];
                                UINT64 exInfo1 = *(UINT64 *)&er[0x28];
                                printf("[kerneldiag] EXCEPTION_RECORD @0x%llX: code=0x%X address=0x%llX numParams=%u info[0]=0x%llX info[1]=0x%llX\n",
                                       (unsigned long long)bugCheckData[3], exCode, (unsigned long long)exAddress,
                                       numParams, (unsigned long long)exInfo0, (unsigned long long)exInfo1);

                                UINT64 exModuleBase = 0;
                                if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, values[6].Reg64, exAddress, &exModuleBase)) {
                                    printf("[kerneldiag] fast-fail site: module base 0x%llX, RVA 0x%llX%s\n",
                                           (unsigned long long)exModuleBase, (unsigned long long)(exAddress - exModuleBase),
                                           (exModuleBase == kernelDiagModuleBase) ? "  <-- inside ntoskrnl.exe (real symbol resolvable offline)" : "");

                                    // Dump live guest code around the actual
                                    // fail site so it can be disassembled
                                    // offline with the trustworthy
                                    // ntkrnlmp.pdb -- this tells us exactly
                                    // which register/parameter the
                                    // safe-unlink check was validating,
                                    // which is what we actually need to find
                                    // the corrupted structure itself (the
                                    // EXCEPTION_RECORD's own info[] array
                                    // carries no extra pointer for this
                                    // fast-fail subtype).
                                    unsigned char failSiteBuf[0x200] = { 0 };
                                    UINT64 failSiteStart = (exAddress >= 0x100) ? exAddress - 0x100 : exAddress;
                                    if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, failSiteStart, failSiteBuf, sizeof(failSiteBuf))) {
                                        char fpath[512];
                                        _snprintf_s(fpath, sizeof(fpath), _TRUNCATE,
                                                    "C:\\Users\\DELL\\AppData\\Local\\Temp\\claude\\c--Users-DELL-OneDrive-Desktop-LocalHost-py\\5bed7398-339c-497a-9383-821cf9d2769e\\scratchpad\\FailSite.bin");
                                        FILE *ffp = fopen(fpath, "wb");
                                        if (ffp) { fwrite(failSiteBuf, 1, sizeof(failSiteBuf), ffp); fclose(ffp); }
                                        printf("[kerneldiag] dumped 0x200 bytes around fail site (starting RVA 0x%llX, module base 0x%llX) to %s\n",
                                               (unsigned long long)(failSiteStart - exModuleBase), (unsigned long long)exModuleBase, fpath);
                                    }
                                }

                                // Trap frame (param2): the offset-guessing
                                // approach (assume the classic public
                                // KTRAP_FRAME layout: Rax@0x30, Rcx@0x38,
                                // Rdx@0x40, R8@0x48, R9@0x50, R10@0x58...)
                                // put R10 -- the one register the fail-site
                                // disassembly proves is NEVER reassigned
                                // after function entry (r10 = original rcx,
                                // the subsegment pointer) across all three
                                // safe-unlink checks, so it should still
                                // hold a live, dereferenceable pointer at
                                // the fault -- at an offset that read back
                                // as exactly zero. A zero there for a
                                // register the code demonstrably still
                                // needs means that offset guess is wrong.
                                // Print every qword (not just nonzero ones)
                                // and auto-flag anything that looks like a
                                // real canonical kernel pointer, then dump
                                // a small window of ITS OWN memory content
                                // -- a genuine LIST_ENTRY's Flink/Blink
                                // pair will show two adjacent pointer-sized
                                // fields, letting the actual data identify
                                // itself instead of trusting an offset
                                // guess.
                                if (bugCheckData[2] != 0) {
                                    unsigned char tf[0x200] = { 0 };
                                    if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, bugCheckData[2], tf, sizeof(tf))) {
                                        printf("[kerneldiag] trap frame @0x%llX, ALL qwords:\n", (unsigned long long)bugCheckData[2]);
                                        int ti;
                                        for (ti = 0; ti < (int)sizeof(tf); ti += 8) {
                                            UINT64 qv = *(UINT64 *)&tf[ti];
                                            int looksLikePointer = (qv >= 0xFFFF800000000000ULL);
                                            printf("  +0x%03X = 0x%016llX%s\n", ti, (unsigned long long)qv,
                                                   looksLikePointer ? "  <-- canonical kernel pointer" : "");
                                            if (looksLikePointer) {
                                                unsigned char nearby[0x30] = { 0 };
                                                if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, qv, nearby, sizeof(nearby))) {
                                                    UINT64 f0 = *(UINT64 *)&nearby[0x00];
                                                    UINT64 f8 = *(UINT64 *)&nearby[0x08];
                                                    printf("      -> memory there: +0x00=0x%016llX +0x08=0x%016llX%s\n",
                                                           (unsigned long long)f0, (unsigned long long)f8,
                                                           (f0 >= 0xFFFF800000000000ULL && f8 >= 0xFFFF800000000000ULL) ? "  <-- both look like pointers, plausible LIST_ENTRY" : "");
                                                }
                                            }
                                        }
                                    }
                                }
                            } else {
                                printf("[kerneldiag] failed to read EXCEPTION_RECORD at 0x%llX\n", (unsigned long long)bugCheckData[3]);
                            }
                            fflush(stdout);
                        }

                        // Byte-pattern scanning for the call site (direct
                        // KeBugCheckEx, direct HalBugCheckSystem, and a raw
                        // "mov edx,0x110" search) all came back empty or
                        // pointed only at an unrelated AuthZ/security-
                        // subsystem cold path -- disabled now that it's
                        // served its purpose (confirmed param1=0x110 is a
                        // generic, reused bugcheck-component ID, not
                        // HAL-specific, so it can't disambiguate the real
                        // call site by itself). Trying a different angle
                        // instead: param2 is a POINTER (consistently in the
                        // 0xFFFFF7xxxxxxxxxx range across every capture) --
                        // dump what it actually points to, in case it's a
                        // struct/string that names the failing component.
                        if (bugCheckData[2] != 0) {
                            // Widened from 0x80 to 0x120 bytes (falsification
                            // experiment B, see docs/roadmap.md): the earlier
                            // dump stopped short of offset +0xb8, the one
                            // byte that directly answers "did
                            // HalpTimerInitialize's success path run" --
                            // bit 2 (0x4) is set there ONLY by the
                            // "or dword ptr [rbx+0xb8], 4" instruction right
                            // after HalpTimerInitialize returns non-negative
                            // (confirmed in ColdPath2.disasm.txt). Zero risk
                            // (read-only, reuses the already-proven
                            // kernelReadVA path) versus live INT3 tracing,
                            // which would need the guest kernel's randomized
                            // load base known *before* the target function
                            // runs -- something only discoverable reactively
                            // today, after a stall, i.e. too late to plant a
                            // breakpoint ahead of time without new proactive
                            // module-discovery infrastructure.
                            unsigned char param2Mem[0x120] = { 0 };
                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, bugCheckData[2], param2Mem, sizeof(param2Mem))) {
                                printf("[kerneldiag] memory at param2 (0x%llX):\n", (unsigned long long)bugCheckData[2]);
                                int pi;
                                for (pi = 0; pi < (int)sizeof(param2Mem); pi += 16) {
                                    printf("  +0x%02X:", pi);
                                    int pj;
                                    for (pj = 0; pj < 16; pj++) printf(" %02X", param2Mem[pi + pj]);
                                    printf("  ");
                                    for (pj = 0; pj < 16; pj++) {
                                        unsigned char c = param2Mem[pi + pj];
                                        printf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
                                    }
                                    printf("\n");
                                }
                                unsigned char flagsB8 = param2Mem[0xB8];
                                printf("[kerneldiag] descriptor+0xB8 = 0x%02X -- bit2 (HalpTimerInitialize succeeded) is %s\n",
                                       flagsB8, (flagsB8 & 0x04) ? "SET" : "clear");

                                // Narrow investigation (2026-07-16),
                                // continued: HalpInterruptRemap.disasm.txt
                                // showed its result register defaults to
                                // STATUS_UNSUCCESSFUL at entry and is only
                                // overwritten if the interrupt descriptor's
                                // type field (offset 0 of the structure
                                // pointed to by descriptor+0x120) equals 0
                                // or 3 -- any other value falls straight
                                // through to the untouched default. Chase
                                // that pointer and read the actual type
                                // field to confirm directly.
                                UINT64 remapInfoPtr = *(UINT64 *)&param2Mem[0x120];
                                printf("[kerneldiag] descriptor+0x120 (remap-info pointer) = 0x%llX\n",
                                       (unsigned long long)remapInfoPtr);
                                if (remapInfoPtr != 0) {
                                    unsigned char remapInfo[16] = { 0 };
                                    if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, remapInfoPtr, remapInfo, sizeof(remapInfo))) {
                                        UINT32 typeField = *(UINT32 *)&remapInfo[0];
                                        printf("[kerneldiag] remap-info+0x00 (type field HalpInterruptRemap switches on) = 0x%X (expects 0 or 3; anything else -> unhandled default STATUS_UNSUCCESSFUL)\n",
                                               typeField);
                                        printf("[kerneldiag] remap-info raw: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                                               remapInfo[0], remapInfo[1], remapInfo[2], remapInfo[3], remapInfo[4], remapInfo[5], remapInfo[6], remapInfo[7],
                                               remapInfo[8], remapInfo[9], remapInfo[10], remapInfo[11], remapInfo[12], remapInfo[13], remapInfo[14], remapInfo[15]);
                                    } else {
                                        printf("[kerneldiag] failed to read remap-info at 0x%llX\n", (unsigned long long)remapInfoPtr);
                                    }
                                }
                            } else {
                                printf("[kerneldiag] failed to read memory at param2 (0x%llX)\n", (unsigned long long)bugCheckData[2]);
                            }
                        }

                        // HalpTimerInitSystem (dispatched by a "phase"
                        // parameter) confirmed phase 25 == our param3, with
                        // logic "if (HalpWatchdogTimer != 0) goto <cold
                        // path that bugchecks>". Read HalpWatchdogTimer's
                        // actual live value (real pointer vs. garbage tells
                        // us whether something legitimately registered a
                        // watchdog vs. memory corruption), and dump
                        // HalpTimerInitializeSystemWatchdog (the most
                        // obviously-named candidate for what sets it) plus
                        // the original two orchestrator functions.
                        {
                            UINT64 halpWatchdogTimerVA = kernelDiagModuleBase + 0xC4BE68;
                            UINT64 watchdogTimerValue = 0;
                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, halpWatchdogTimerVA, &watchdogTimerValue, 8)) {
                                printf("[kerneldiag] HalpWatchdogTimer = 0x%llX\n", (unsigned long long)watchdogTimerValue);
                            } else {
                                printf("[kerneldiag] failed to read HalpWatchdogTimer\n");
                            }
                            fflush(stdout);
                        }
                        // Confirmed (via disassembling HalpTimerConfigureInterrupt
                        // and its cold-path failure trampoline) that param3
                        // is HalpTimerLastProblem (RVA 0xC4C2B4), a SHARED
                        // "last recorded problem code" global reused by many
                        // different HAL timer/interrupt failure checks --
                        // meaning multiple different root causes could
                        // produce this exact bugcheck signature, and the
                        // only way to disambiguate which ACTUAL check fired
                        // is to find what specifically wrote the literal
                        // value 25 (0x19) into it. Scan for `mov dword ptr
                        // [rip+disp32], 0x19` (opcode C7 05) resolving to
                        // that exact address.
                        {
                            UINT64 targetVA = kernelDiagModuleBase + 0xC4C2B4;
                            UINT64 scanBase = kernelDiagModuleBase;
                            UINT64 scanLen = 0x2000000;
                            unsigned char *scanBuf = (unsigned char *)malloc(0x1000);
                            int hits = 0;
                            UINT64 pg;
                            for (pg = 0; pg < scanLen && hits < 20; pg += 0x1000) {
                                if (!scanBuf) break;
                                if (!kernelReadVA((unsigned char *)guestMemory, values[6].Reg64,
                                                   scanBase + pg, scanBuf, 0x1000)) {
                                    continue;
                                }
                                int bi;
                                for (bi = 0; bi < 0x1000 - 10 && hits < 20; bi++) {
                                    if (scanBuf[bi] == 0xC7 && scanBuf[bi+1] == 0x05) {
                                        INT32 disp32;
                                        memcpy(&disp32, scanBuf + bi + 2, 4);
                                        UINT32 imm32;
                                        memcpy(&imm32, scanBuf + bi + 6, 4);
                                        UINT64 insnVA = scanBase + pg + bi;
                                        UINT64 nextInsnVA = insnVA + 10;
                                        UINT64 resolvedTarget = nextInsnVA + (INT64)disp32;
                                        if (resolvedTarget == targetVA && imm32 == 0x19) {
                                            UINT64 rva = insnVA - kernelDiagModuleBase;
                                            printf("[kerneldiag] FOUND: 'mov [HalpTimerLastProblem], 0x19' at RVA 0x%llX\n",
                                                   (unsigned long long)rva);
                                            unsigned char ctx[64] = { 0 };
                                            kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, insnVA - 32, ctx, 64);
                                            int ci;
                                            printf("  bytes:");
                                            for (ci = 0; ci < 64; ci++) printf(" %02X", ctx[ci]);
                                            printf("\n");
                                            hits++;
                                        }
                                    }
                                }
                            }
                            if (scanBuf) free(scanBuf);
                            printf("[kerneldiag] HalpTimerLastProblem=0x19 setter scan complete: %d hit(s)\n", hits);
                        }

                        // Narrow investigation (2026-07-16), continued:
                        // descriptor+0xB8 bit6 already set means the
                        // HalpInterruptRemap call at RVA 0x4A4CE7 gets
                        // skipped entirely (test al,0x40; jne <skip> right
                        // before it) -- so descriptor+0x120 being NULL
                        // (confirmed above) is a dead end, not the real
                        // path. Execution actually falls through to
                        // HalpInterruptSetLineState, whose hot path (RVA
                        // 0x3A3AE8, already dumped/disassembled) calls
                        // HalpInterruptLookupController (our IOAPIC) then
                        // HalpInterruptSetLineStateInternal -- but every
                        // failure landing pad is in a cold-path region
                        // around RVA 0x4A5BE0-0x4A5E00, outside that dump.
                        // Dumping that cold-path tail plus
                        // HalpInterruptSetLineStateInternal itself (the
                        // actual hardware-touching call) to find exactly
                        // what it checks.
                        //
                        // Part 5 (2026-07-16): HalpInterruptSetLineStateInternal
                        // dispatches through a function pointer stored at
                        // the *live* interrupt-controller object's own
                        // +0x70 offset (a vtable-style callback). That
                        // object is resolved by HalpInterruptLookupController
                        // (RVA 0x378DD0), never yet dumped itself -- add it
                        // here first so its own disassembly reveals how it
                        // finds the controller object (expected: a
                        // RIP-relative global list/array), which tells us
                        // how to walk to the live object and read +0x70
                        // directly, without any live breakpoint/tracing.
                        {
                            struct { const char *name; UINT64 rva; UINT64 len; } vdumps[] = {
                                { "HalpInterruptSetLineState_ColdPath", 0x4A5BE8, 0x500 },
                                { "HalpInterruptSetLineStateInternal", 0x378C7C, 0x400 },
                                { "HalpInterruptLookupController", 0x378DD0, 0x300 },
                                // Part 6 (2026-07-16): HalpApicSetLineState
                                // (the controller+0x70 callback, dumped live
                                // above) has two failure-relevant cold
                                // branches displaced into this far region:
                                // RVA 0x4957C2 (a mismatch between the line
                                // descriptor's first field and the "self"
                                // context's +8 field -- the leading
                                // candidate for our actual failure) and
                                // 0x4957CC (a different negative-eax case).
                                // Several other HalpApicConvertToRte cold
                                // targets also cluster here (0x495806,
                                // 0x495843, 0x495881, 0x4958F7, 0x495916,
                                // 0x495934, 0x495973). Dump generously to
                                // capture all of them in one pass.
                                { "HalpApicSetLineState_ColdPath", 0x4957C0, 0x400 },
                                // Part 6, continued: HalpInterruptSetLineStateInternal's
                                // OWN failure landing pad (reached when the
                                // controller+0x70 dispatch call, i.e.
                                // HalpApicSetLineState, returns negative) is
                                // at RVA 0x49462E -- never directly
                                // disassembled, only assumed to propagate
                                // the return value unmodified. Confirm that
                                // directly instead of assuming it.
                                { "HalpInterruptSetLineStateInternal_ColdPath", 0x49462E, 0x100 },
                                // Part 8 (2026-07-16): ColdPath.disasm.txt
                                // (captured way back in part 1 of this
                                // investigation, RVA ~0x4A8F00+) shows a
                                // SECOND, separate failure block at RVA
                                // 0x4A9010, gated on descriptor+0xB8 bit4
                                // (clear for us, confirmed since part 3) --
                                // falling straight to "mov eax,0xC0000001"
                                // then storing HalpTimerLastProblem=0x19 --
                                // matching our exact observed values,
                                // without going through
                                // HalpInterruptSetLineState at all. Never
                                // yet dumped HalpTimerInitializeClock's own
                                // HOT path (RVA 0x3AFBF4) to see how/when
                                // this block is reached relative to the
                                // HalpTimerConfigureInterrupt call already
                                // confirmed in part 2 -- do that now to
                                // resolve which failure path is real.
                                { "HalpTimerInitializeClock_HotPath", 0x3AFBF4, 0x400 },
                            };
                            int di;
                            for (di = 0; di < 6; di++) {
                                unsigned char *buf = (unsigned char *)malloc(vdumps[di].len);
                                if (!buf) continue;
                                if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64,
                                                  kernelDiagModuleBase + vdumps[di].rva, buf, vdumps[di].len)) {
                                    char path[512];
                                    _snprintf_s(path, sizeof(path), _TRUNCATE,
                                                "C:\\Users\\DELL\\AppData\\Local\\Temp\\claude\\c--Users-DELL-OneDrive-Desktop-LocalHost-py\\5bed7398-339c-497a-9383-821cf9d2769e\\scratchpad\\%s.bin",
                                                vdumps[di].name);
                                    FILE *f = fopen(path, "wb");
                                    if (f) { fwrite(buf, 1, vdumps[di].len, f); fclose(f); }
                                    printf("[kerneldiag] dumped %s (%llu bytes) to %s\n", vdumps[di].name,
                                           (unsigned long long)vdumps[di].len, path);
                                } else {
                                    printf("[kerneldiag] failed to read %s\n", vdumps[di].name);
                                }
                                free(buf);
                            }
                        }

                        // Part 5, continued: HalpInterruptLookupController's
                        // own disassembly (confirmed live above) is:
                        //   mov rax, [rip+0x8D2B39]   ; rax = list head Flink (global LIST_ENTRY)
                        //   lea r8,  [rip+0x8D2B32]   ; r8  = &list head itself (loop terminator)
                        //   cmp rax, r8 ; je <empty>
                        //   loop: cmp [rax+0xE8], ecx ; jne <next via [rax]>
                        //   match -> return rax
                        // The list head global resolves to RVA 0xC4B910
                        // (0x378DD7 + 0x8D2B39). Walk it live: since our
                        // emulated hardware only ever registers one IOAPIC
                        // controller, the first real (non-head) node is
                        // almost certainly the one HalpInterruptSetLineState
                        // resolves to -- read it directly rather than
                        // reproducing the ecx match key, and sanity-check
                        // via the +0xE8/+0xDC fields already known from
                        // disassembly. Then read +0x70 (callback) and +0x10
                        // (self/context) and dump the callback target
                        // directly from its live pointer -- no live
                        // breakpoint/tracing needed, consistent with the
                        // read-only technique used throughout this
                        // investigation.
                        {
                            UINT64 listHeadVA = kernelDiagModuleBase + 0xC4B910;
                            UINT64 flink = 0, blink = 0;
                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, listHeadVA, &flink, 8) &&
                                kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, listHeadVA + 8, &blink, 8)) {
                                printf("[kerneldiag] HalpInterruptControllerList head @0x%llX: Flink=0x%llX Blink=0x%llX%s\n",
                                       (unsigned long long)listHeadVA, (unsigned long long)flink, (unsigned long long)blink,
                                       (flink != blink) ? " -- MORE THAN ONE controller registered, walking all entries" : " -- single entry");
                                // Part 7 (2026-07-16): Flink != Blink means
                                // more than one controller object is
                                // registered -- walk the WHOLE list (not
                                // just the first entry) and dump every
                                // distinct +0x70 callback, since the earlier
                                // single-entry assumption may have picked
                                // the wrong controller for the clock
                                // interrupt's specific GSI.
                                UINT64 cur = flink;
                                int nodeIndex = 0;
                                UINT64 seenCallbacks[8] = { 0 };
                                int seenCount = 0;
                                while (cur != 0 && cur != listHeadVA && nodeIndex < 8) {
                                    UINT64 controllerVA = cur;
                                    UINT32 idField = 0, flagsDC = 0;
                                    UINT64 selfField = 0, callbackPtr = 0;
                                    UINT64 nextNode = 0;
                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, controllerVA, &nextNode, 8);
                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, controllerVA + 0xE8, &idField, 4);
                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, controllerVA + 0xDC, &flagsDC, 4);
                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, controllerVA + 0x10, &selfField, 8);
                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, controllerVA + 0x70, &callbackPtr, 8);
                                    printf("[kerneldiag] controller[%d] @0x%llX: +0xE8(id)=0x%X +0xDC(flags)=0x%X +0x10(self)=0x%llX +0x70(callback)=0x%llX\n",
                                           nodeIndex, (unsigned long long)controllerVA, idField, flagsDC,
                                           (unsigned long long)selfField, (unsigned long long)callbackPtr);
                                    // Part 8 (2026-07-16): read the
                                    // controller's OWN embedded routing-
                                    // candidate LIST_ENTRY at +0x100 --
                                    // the exact list HalpInterruptFindBestRouting
                                    // walks (part 7 finding) -- directly,
                                    // to confirm empty vs. populated rather
                                    // than continuing to hypothesize.
                                    {
                                        UINT64 candListHeadVA = controllerVA + 0x100;
                                        UINT64 candFlink = 0, candBlink = 0;
                                        if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, candListHeadVA, &candFlink, 8) &&
                                            kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, candListHeadVA + 8, &candBlink, 8)) {
                                            int isEmpty = (candFlink == candListHeadVA);
                                            printf("[kerneldiag] controller[%d] +0x100 (routing-candidate list) @0x%llX: Flink=0x%llX Blink=0x%llX -- %s\n",
                                                   nodeIndex, (unsigned long long)candListHeadVA,
                                                   (unsigned long long)candFlink, (unsigned long long)candBlink,
                                                   isEmpty ? "EMPTY" : "has entries");
                                            if (!isEmpty) {
                                                UINT64 candCur = candFlink;
                                                int candIdx = 0;
                                                while (candCur != 0 && candCur != candListHeadVA && candIdx < 8) {
                                                    UINT32 f10 = 0, f14 = 0, f18 = 0;
                                                    UINT64 candNext = 0;
                                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, candCur, &candNext, 8);
                                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, candCur + 0x10, &f10, 4);
                                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, candCur + 0x14, &f14, 4);
                                                    kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, candCur + 0x18, &f18, 4);
                                                    printf("[kerneldiag] controller[%d] candidate[%d] @0x%llX: +0x10(dest)=0x%X +0x14(prioLo)=0x%X +0x18(prioHi)=0x%X\n",
                                                           nodeIndex, candIdx, (unsigned long long)candCur, f10, f14, f18);
                                                    candCur = candNext;
                                                    candIdx++;
                                                }
                                            }
                                        } else {
                                            printf("[kerneldiag] controller[%d] failed to read +0x100 list head\n", nodeIndex);
                                        }
                                    }
                                    if (callbackPtr != 0) {
                                        INT64 callbackRvaSigned = (INT64)callbackPtr - (INT64)kernelDiagModuleBase;
                                        printf("[kerneldiag] controller[%d] callback RVA = 0x%llX (%s)\n", nodeIndex,
                                               (unsigned long long)callbackRvaSigned,
                                               (callbackRvaSigned > 0 && callbackRvaSigned < 0x2000000) ? "in-module" : "OUTSIDE module range -- likely a different module");
                                        int alreadyDumped = 0, si;
                                        for (si = 0; si < seenCount; si++) {
                                            if (seenCallbacks[si] == callbackPtr) { alreadyDumped = 1; break; }
                                        }
                                        if (!alreadyDumped && seenCount < 8) {
                                            seenCallbacks[seenCount++] = callbackPtr;
                                            unsigned char cbBuf[0x400];
                                            if (kernelReadVA((unsigned char *)guestMemory, values[6].Reg64, callbackPtr, cbBuf, sizeof(cbBuf))) {
                                                char path[512];
                                                _snprintf_s(path, sizeof(path), _TRUNCATE,
                                                            "C:\\Users\\DELL\\AppData\\Local\\Temp\\claude\\c--Users-DELL-OneDrive-Desktop-LocalHost-py\\5bed7398-339c-497a-9383-821cf9d2769e\\scratchpad\\InterruptControllerCallback_%d.bin",
                                                            nodeIndex);
                                                FILE *f = fopen(path, "wb");
                                                if (f) { fwrite(cbBuf, 1, sizeof(cbBuf), f); fclose(f); }
                                                printf("[kerneldiag] dumped controller[%d] callback (%llu bytes from live pointer 0x%llX) to %s\n",
                                                       nodeIndex, (unsigned long long)sizeof(cbBuf), (unsigned long long)callbackPtr, path);
                                            } else {
                                                printf("[kerneldiag] failed to read controller[%d] callback target at 0x%llX\n", nodeIndex, (unsigned long long)callbackPtr);
                                            }
                                        } else if (alreadyDumped) {
                                            printf("[kerneldiag] controller[%d] callback matches an already-dumped one, skipping\n", nodeIndex);
                                        }
                                    }
                                    cur = nextNode;
                                    nodeIndex++;
                                }
                                if (nodeIndex == 0) {
                                    printf("[kerneldiag] HalpInterruptControllerList is empty -- no controller registered\n");
                                }
                            } else {
                                printf("[kerneldiag] failed to read HalpInterruptControllerList head\n");
                            }
                        }
                    }
                    fflush(stdout);
                }
            } else {
                printf("[watchdog] STALL #%d but WHvGetVirtualProcessorRegisters failed: 0x%lx\n", stallTicks, hr);
            }
            fflush(stdout);
        } else {
            stallTicks = 0;
        }
        lastSeen = current;
    }
    return 0;
}

// Measures the HOST's real TSC frequency via a QueryPerformanceCounter-timed
// busy-wait window, so it can be reported to the guest via a synthesized
// CPUID leaf 0x15 (see call site in main). This host CPU's own CPUID leaf
// 0x15 has no crystal-clock Hz value (confirmed empirically: ecx=0, leaf
// 0x16 entirely empty -- see check_cpuid15.c from the RTC-slowdown
// investigation), which is exactly why Windows falls back to a slow,
// real-time RTC-second-rollover calibration loop during boot (observed as
// a multi-minute stall with the guest spinning inside a single
// WHvRunVirtualProcessor call, no port traps, host CPU time climbing).
// Since our hypervisor fully controls what CPUID the guest sees, we can
// supply a directly-measured, accurate frequency and let Windows skip that
// slow path entirely.
UINT64 measureTscFrequency(void) {
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    unsigned __int64 tsc0 = __rdtsc();
    LARGE_INTEGER now;
    do {
        QueryPerformanceCounter(&now);
    } while ((now.QuadPart - t0.QuadPart) * 1000 / freq.QuadPart < 100);
    unsigned __int64 tsc1 = __rdtsc();
    QueryPerformanceCounter(&t1);
    double elapsedSec = (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
    return (UINT64)((double)(tsc1 - tsc0) / elapsedSec);
}

int main(int argc, char *argv[]) {
    // Raise the system timer resolution to 1ms. Windows' default is ~15.6ms, so
    // every Sleep(1) here -- including the one pacing the halted-CPU loop, where
    // an idle guest spends nearly all its time -- sleeps ~15.6ms instead. The
    // comment on that loop already assumed "roughly 1000 times a second", which is
    // only true at 1ms resolution, so the assumption was written down but never
    // established.
    //
    // HONEST SCOPE: this did NOT change the guest's clock rate. I added it
    // expecting it to -- the RTC was delivering 67 ticks/sec against a programmed
    // rate 5 (RegA=0x25) = 2048Hz, and ~64/sec smelled exactly like 15.6ms
    // granularity. Measured after: still 67/sec. So the cap is elsewhere, and the
    // likely explanation is that 67/sec is simply what the guest asks for: it
    // toggles PIE on and off constantly ([cmos-ab] shows RegB 0x42 -> 0x02 ->
    // 0x42), i.e. Windows is using the RTC for occasional profiling, not as its
    // system clock. Kept because a VMM pacing itself on Sleep() should not run at
    // 15.6ms granularity regardless, but it fixes nothing on its own.
    timeBeginPeriod(1);

    netBackendInit();

    // U113: exact I/O port histogram for exit attribution. 65536 counters,
    // 256KB, allocated once. If it fails the histogram is simply skipped --
    // a diagnostic must never be the reason the VM does not start.
    g_ioPortHist = (UINT32 *)calloc(65536, sizeof(UINT32));

    // argv[1]: VM name shown in the window title (defaults to "Guest Display")
    // argv[2]: path to the BIOS image to load (defaults to "bios.bin")
    // argv[3]: path to the raw disk image backing the primary ATA drive (optional)
    // argv[4]: guest RAM in MB (optional)
    // argv[5]: path to an ISO to boot from (optional) -- see the boot-device
    //          selection below for what happens when both this and argv[3] are
    //          supplied.
    //
    // Every one of these is positional and OPTIONAL, so an empty string means
    // "not supplied". The manager passes placeholders to reach later arguments,
    // and treating "" as a real path would make it try to open a file named "".
    const char *biosPath = "bios.bin";
    const char *diskPath = NULL;
    const char *isoPath = NULL;
    if (argc > 1) {
        _snprintf_s(g_windowTitle, sizeof(g_windowTitle), _TRUNCATE,
                    "LocalHost Hypervisor -- %s", argv[1]);
    }
    if (argc > 2 && argv[2][0]) {
        biosPath = argv[2];
    }
    if (argc > 3 && argv[3][0]) {
        diskPath = argv[3];
    }
    if (argc > 5 && argv[5][0]) {
        isoPath = argv[5];
    }

    // UEFI firmware (OVMF/edk2) is identified by a ".fd" extension on the
    // supplied firmware image -- anything else keeps the existing legacy
    // BIOS boot path (1MB guest RAM, firmware loaded at 0xC0000, real-mode
    // reset vector at the top of the first 1MB) completely unchanged.
    int uefiMode = 0;
    size_t biosPathLen = strlen(biosPath);
    if (biosPathLen >= 3 && _stricmp(biosPath + biosPathLen - 3, ".fd") == 0) {
        uefiMode = 1;
    }
    // U67: guest RAM is now settable from argv[4] (in MB).
    //
    // The hardcoded 3GB is committed memory, and on an 8GB host that is already
    // ~11GB committed before the VM starts, it pushes Windows into heavy paging --
    // measured at 0.5GB physical free with the VM NOT running. Add a debugger on
    // top and the machine thrashes until it has to be hard-powered-off, which is
    // what happened three times. Being able to run a smaller guest is the
    // difference between being able to debug this at all on this hardware.
    //
    // Careful with small values: WinPE runs from a 437MB RAM disk (the firmware
    // loads boot.wim into it), so it needs real headroom on top of that. 2048 is
    // the sensible first step down; below ~1536 expect the boot we are debugging to
    // break for reasons that have nothing to do with the bug.
    guestMemSize = uefiMode ? (SIZE_T)UEFI_GUEST_RAM_SIZE : 0x100000;
    if (uefiMode && argc > 4) {
        long mb = atol(argv[4]);
        if (mb >= 256 && mb <= 8192) {
            guestMemSize = (SIZE_T)mb * 1024 * 1024;
            printf("[mem] guest RAM overridden to %ld MB (argv[4])\n", mb);
        } else {
            printf("[mem] ignoring argv[4]='%s' -- expected 256..8192 MB\n", argv[4]);
        }
        fflush(stdout);
    }
    printf("[mem] guest RAM = %llu MB\n", (unsigned long long)(guestMemSize / (1024 * 1024)));
    fflush(stdout);

    // U70: refuse to start a guest the host cannot back with PHYSICAL memory.
    //
    // Guest RAM is committed up front, so starting a 1536MB guest on a host with
    // 800MB available does not fail cleanly -- it forces Windows to page out
    // everything else, and the machine thrashes until it has to be hard
    // powered off. That has now happened FOUR times on this hardware. The commit
    // charge looks healthy while it happens (~75% of limit) because the pagefile
    // has plenty of room; the metric that actually matters is ullAvailPhys, and
    // this box boots with only ~0.8GB of its 7.8GB free.
    //
    // A hypervisor that can wedge the host by being asked for a normal amount of
    // RAM is not finished, so this is a real preflight check rather than a
    // diagnostic. The override exists because "I know, run it anyway" is a
    // legitimate thing to want on a machine with different headroom.
    {
        MEMORYSTATUSEX memStatus;
        memStatus.dwLength = sizeof(memStatus);
        if (GlobalMemoryStatusEx(&memStatus)) {
            const unsigned long long hostReserveMb = 512; // headroom Windows needs to stay responsive
            unsigned long long availMb = memStatus.ullAvailPhys / (1024ULL * 1024ULL);
            unsigned long long guestMb = (unsigned long long)(guestMemSize / (1024 * 1024));
            printf("[mem] host available physical = %llu MB\n", availMb);
            if (guestMb + hostReserveMb > availMb) {
                long long safeMb = (long long)availMb - (long long)hostReserveMb;
                printf("[mem] *** REFUSING TO START ***\n");
                printf("[mem]   guest wants %llu MB, host has %llu MB available\n", guestMb, availMb);
                printf("[mem]   Windows needs ~%llu MB of headroom; starting anyway thrashes the host\n",
                       hostReserveMb);
                if (safeMb >= 256)
                    printf("[mem]   largest safe guest right now: ~%lld MB -- pass it as argv[4]\n", safeMb);
                else
                    printf("[mem]   no safe size right now: close some applications first\n");
                printf("[mem]   set LOCALHOST_ALLOW_LOW_MEMORY=1 to override\n");
                if (!getenv("LOCALHOST_ALLOW_LOW_MEMORY")) {
                    fflush(stdout);
                    return 1;
                }
                printf("[mem]   LOCALHOST_ALLOW_LOW_MEMORY set -- continuing anyway\n");
            }
            fflush(stdout);
        }
    }

    // --- BOOT DEVICE SELECTION -------------------------------------------
    //
    // Both media are attached when both are configured: the ISO on port 0 (which
    // firmware enumerates first, so it boots) and the hard disk on port 1 as
    // somewhere to install TO. That combination is the whole point -- with one
    // port, booting installer media left the installer with no target disk.
    //
    // Falls back to the disk if the ISO cannot be opened, so a stale ISO path in
    // the manager degrades to "boots the hard disk" instead of "boots nothing".
    const char *bootPath = isoPath ? isoPath : diskPath;
    const char *secondPath = NULL;
    if (bootPath && isoPath) {
        FILE *probe = fopen(isoPath, "rb");
        if (!probe) {
            printf("[boot] cannot open ISO %s -- falling back to the hard disk\n", isoPath);
            bootPath = diskPath;
        } else {
            fclose(probe);
            // BOTH get attached now: the ISO boots on port 0, the hard disk
            // rides along on port 1 as somewhere to install TO. Firmware
            // enumerates in port order, so the ISO is tried first without
            // needing a boot-order setting anywhere.
            secondPath = diskPath;
        }
    }

    {
        const char *slots[AHCI_PORT_COUNT] = { bootPath, secondPath };
        int slot;
        for (slot = 0; slot < AHCI_PORT_COUNT; slot++) {
            const char *path = slots[slot];
            AhciPortDevice *dev = &ahciPorts[slot];
            size_t pathLen;
            int isIso;
            UINT64 fileSize;

            if (!path) continue;
            pathLen = strlen(path);
            // Keyed on the EXTENSION, not on which argument it arrived in, so an
            // ISO passed the old way (as argv[3]) behaves exactly as it always has.
            isIso = pathLen >= 4 && _stricmp(path + pathLen - 4, ".iso") == 0;

            // Open .iso images read-only: installer/boot media should never
            // legitimately be written to (UdfDxe and the boot manager only read
            // it), and opening read-write would risk corrupting a multi-GB
            // source ISO if anything upstream ever issued a write.
            dev->file = fopen(path, isIso ? "rb" : "r+b");
            if (!dev->file) {
                printf("Failed to open %s -- port %d left empty\n", path, slot);
                continue;
            }
            _fseeki64(dev->file, 0, SEEK_END);
            fileSize = (UINT64)_ftelli64(dev->file);
            dev->sectorSize = isIso ? 2048 : 512;
            dev->sectors = fileSize / dev->sectorSize;
            dev->present = 1;
            _snprintf_s(dev->path, sizeof(dev->path), _TRUNCATE, "%s", path);

            // Fixed-format VHDs append a 512-byte "conectix" footer after the
            // real disk data. No VHD parsing happens anywhere else -- disk I/O
            // is plain fseek+fread at lba*sectorSize -- so without this check
            // that footer is presented to the guest as one extra, bogus final
            // sector: IDENTIFY would report a capacity one sector too large, and
            // a read of the true last LBA would return VHD metadata instead of
            // disk content. Not applicable to ISOs (they're never VHDs).
            if (!isIso && fileSize >= 512) {
                unsigned char footerCookie[8];
                _fseeki64(dev->file, -512, SEEK_END);
                if (fread(footerCookie, 1, 8, dev->file) == 8 &&
                    memcmp(footerCookie, "conectix", 8) == 0) {
                    dev->sectors -= 1;
                    printf("Detected Fixed VHD footer -- excluding it from disk geometry\n");
                }
            }

            printf("Attached %s to AHCI port %d: %s (%llu sectors of %u bytes)\n",
                   isIso ? "ISO" : "disk", slot, path,
                   (unsigned long long)dev->sectors, dev->sectorSize);

            // DOES THIS DISK ALREADY HAVE AN OS ON IT?
            //
            // This decides whether we answer the "Press any key to boot from CD or
            // DVD" prompt, and getting it wrong is very visible: after Windows was
            // installed, launching the VM dropped straight back into the INSTALLER,
            // because the auto-answer was "first boot of this process" and a fresh
            // process always looks like a first boot. The mid-install reboot case
            // was handled; "the install already finished" was not.
            //
            // Intent is what actually matters: a blank disk means you want to
            // install, a disk with a partition table means you want to boot it. Read
            // the GPT header at LBA 1 -- signature "EFI PART" -- which is what the
            // installer writes and what the firmware boots from.
            if (!isIso && dev->sectorSize == 512 && fileSize >= 2048) {
                unsigned char gptSig[8];
                _fseeki64(dev->file, 512, SEEK_SET);
                if (fread(gptSig, 1, 8, dev->file) == 8 &&
                    memcmp(gptSig, "EFI PART", 8) == 0) {
                    g_diskHasOs = 1;
                    printf("  -> disk carries a GPT (EFI PART): treating it as installed, "
                           "so the CD/DVD prompt will NOT be auto-answered\n");
                } else {
                    printf("  -> no GPT on this disk: blank target, the CD/DVD prompt "
                           "will be auto-answered so an install can start\n");
                }
                fflush(stdout);
            }

            // Port 0 also backs the legacy IDE path and the existing globals.
            if (slot == 0) {
                ataDiskFile = dev->file;
                ataDiskSectors = dev->sectors;
                ataSectorSize = dev->sectorSize;
            }
        }
        if (ahciPortsImplemented() == 0)
            printf("No bootable media attached -- starting without a disk\n");
    }

    QueryPerformanceFrequency(&perfFrequency);
    QueryPerformanceCounter(&lastToggleTime);
    lastTimerTick = lastToggleTime;
    lastRtcPeriodicTick = lastToggleTime;

    InitializeCriticalSection(&logLock);
    // Before createWindowThread: the window thread is the other producer for
    // both PS/2 queues, so the lock has to exist before it can run.
    InitializeCriticalSection(&ps2Lock);
    createWindowThread();

    g_font = CreateFontA(18, 9, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                          DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");

    if (uefiMode) {
        // Standard CMOS extended-memory registers: 0x17/0x18 and their
        // POST-time mirror 0x30/0x31 report KB of RAM above 1MB (capped at
        // 0xFFFF), 0x34/0x35 report 64KB blocks of RAM above 16MB. This is
        // the conventional fallback OVMF's PlatformPei reads for memory
        // size when it isn't running under real QEMU fw_cfg.
        UINT32 ramBytes = (UINT32)guestMemSize;
        UINT32 extKB = (ramBytes > 0x100000) ? (ramBytes - 0x100000) / 1024 : 0;
        UINT16 ext1MTo16M = (UINT16)(extKB > 0xFFFF ? 0xFFFF : extKB);
        cmosRegisters[0x17] = (unsigned char)(ext1MTo16M & 0xFF);
        cmosRegisters[0x18] = (unsigned char)(ext1MTo16M >> 8);
        cmosRegisters[0x30] = cmosRegisters[0x17];
        cmosRegisters[0x31] = cmosRegisters[0x18];

        UINT32 above16M = (ramBytes > 0x1000000) ? (ramBytes - 0x1000000) / 65536 : 0;
        UINT16 above16M64K = (UINT16)(above16M > 0xFFFF ? 0xFFFF : above16M);
        cmosRegisters[0x34] = (unsigned char)(above16M64K & 0xFF);
        cmosRegisters[0x35] = (unsigned char)(above16M64K >> 8);
        // RTC status registers -- see cmosReadRtcField's comment for why
        // these matter (OVMF's PcRtc.c GetTime() driver returns
        // EFI_DEVICE_ERROR, which Windows Boot Manager's BlpTimeInitialize
        // propagates as STATUS_IO_DEVICE_ERROR/0xC0000185, if Register D's
        // VRT bit is clear or the time/date fields don't decode to a valid
        // calendar date). Register A: UIP=0 (bit 7), a plausible divisor/
        // rate in the low bits. Register B: 24-hour + binary mode (bits
        // 1/2 set) -- picked because it needs no BCD encoding, not because
        // real hardware defaults to it; cmosReadRtcField reads this same
        // register live on every access so a guest write to it changes
        // future time reads accordingly. Register D: VRT=1 (bit 7).
        cmosRegisters[0x0A] = 0x26;
        cmosRegisters[0x0B] = 0x06;
        cmosRegisters[0x0D] = 0x80;
    } else {
        cmosRegisters[0x17] = 0x00;
        cmosRegisters[0x18] = 0x00;
        cmosRegisters[0x30] = 0x00;
        cmosRegisters[0x31] = 0x00;
    }

    WHV_PARTITION_HANDLE partition;
    HRESULT hr = WHvCreatePartition(&partition);
    if (FAILED(hr)) { printf("Failed to create partition. HRESULT: 0x%lx\n", hr); return 1; }

    WHV_PARTITION_PROPERTY processorCount = { 0 };
    processorCount.ProcessorCount = 1;
    hr = WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeProcessorCount, &processorCount, sizeof(processorCount));
    if (FAILED(hr)) { printf("Failed to set processor count. HRESULT: 0x%lx\n", hr); return 1; }

    // Legacy path: no LAPIC emulation needed at all. IRQ delivery for our
    // emulated devices (ATA, keyboard, PIT) goes through direct
    // WHvRegisterPendingInterruption injection for now -- routing it
    // through the LAPIC properly is follow-on work, see the plan's
    // out-of-scope section.
    //
    // UEFI path: OVMF's SEC/PEI phase checks for actual LAPIC presence
    // (CPUID.1:EDX.APIC) during CPU init, so it needs a real emulated LAPIC.
    // XApic (not X2Apic): X2Apic + WHvPartitionPropertyCodeSyntheticProcessorFeaturesBanks
    // was tried and reverted -- see the SyntheticProcessorFeaturesBanks
    // comment below for why.
    WHV_X64_LOCAL_APIC_EMULATION_MODE apicMode = uefiMode
        ? WHvX64LocalApicEmulationModeXApic
        : WHvX64LocalApicEmulationModeNone;
    hr = WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeLocalApicEmulationMode, &apicMode, sizeof(apicMode));
    if (FAILED(hr)) { printf("Failed to set APIC emulation mode. HRESULT: 0x%lx\n", hr); return 1; }

    // Synthesize CPUID leaf 0x15 (TSC/core crystal clock) with a directly
    // measured, accurate frequency -- see measureTscFrequency's comment.
    // TSC_freq = Ecx * Ebx / Eax, so Eax=1, Ebx=1, Ecx=measured gives
    // TSC_freq = measured exactly, with no rounding from an assumed ratio.
    // Registered via the static CpuidResultList (WHV answers the guest's
    // CPUID directly, no exit/host round-trip needed).
    UINT64 measuredTscFrequency = measureTscFrequency();
    printf("[cpuid] measured host TSC frequency: %llu Hz\n", (unsigned long long)measuredTscFrequency);
    WHV_X64_CPUID_RESULT cpuidOverrides[3] = { 0 };
    cpuidOverrides[0].Function = 0x15;
    cpuidOverrides[0].Eax = 1;
    cpuidOverrides[0].Ebx = 1;
    cpuidOverrides[0].Ecx = (UINT32)measuredTscFrequency;
    cpuidOverrides[0].Edx = 0;

    // Post-2026-07-16-part-10 investigation (see
    // docs/investigations/post-vppt-boot-stall.md): leaf 1 was never
    // overridden, so WHV answers it with the REAL HOST's values --
    // including EBX[23:16] (logical processor count) and EDX bit 28
    // (HTT/multi-processor capable), which reflect the host machine's
    // actual core count, not our single emulated vCPU. Our MADT only ever
    // declares one Processor Local APIC (vCPU 0); if the host has more
    // than one logical processor, the guest sees a mismatch between what
    // CPUID claims is available and what MADT actually enumerates -- a
    // plausible cause for code that waits for "all processors" to check
    // in to hang forever, since only one vCPU can ever exist. Query the
    // real host leaf 1 first (so family/model/stepping and feature bits
    // ECX/EDX stay accurate) and only correct the processor-count fields.
    int hostCpuid1[4] = { 0 };
    __cpuid(hostCpuid1, 1);
    cpuidOverrides[1].Function = 1;
    cpuidOverrides[1].Eax = (UINT32)hostCpuid1[0];
    cpuidOverrides[1].Ebx = ((UINT32)hostCpuid1[1] & 0x0000FFFFUL) | (1UL << 16); // logical processor count = 1, keep brand index/CLFLUSH size
    cpuidOverrides[1].Ecx = (UINT32)hostCpuid1[2];
    cpuidOverrides[1].Edx = (UINT32)hostCpuid1[3] & ~(1UL << 28); // clear HTT -- not multi-processor capable
    printf("[cpuid] leaf 1 override: host EBX=0x%08X EDX=0x%08X -> guest EBX=0x%08X EDX=0x%08X\n",
           (UINT32)hostCpuid1[1], (UINT32)hostCpuid1[3], cpuidOverrides[1].Ebx, cpuidOverrides[1].Edx);

    // Leaf 0x80000007 EDX bit 8: INVARIANT TSC -- "the TSC ticks at a constant
    // rate regardless of P-state, C-state or turbo, so it is safe as a clock
    // source". It was never advertised, so the guest had no dependable
    // high-resolution clock and fell back to the ACPI PM timer, which lives at
    // an I/O PORT: every single read is a VM exit, and PM timer reads have been
    // measured at roughly 42% of all exits. Exits are the scarce resource here
    // (the vCPU already runs at 0% idle, which is why raising the tablet's poll
    // rate made the pointer feel WORSE rather than better) so this is aimed at
    // cursor latency as much as at throughput.
    //
    // The host really does have it (verified by probe: leaf 0x80000007
    // EDX=0x00000100), so this claims nothing untrue. Leaf 0x15 already gives
    // the guest the exact TSC frequency, which is the other half of what it
    // needs to use the TSC as a timebase.
    //
    // NOT VERIFIED to change the guest's clock-source choice yet -- see the
    // note below about the hypervisor-present bit, which may pre-empt it.
    int hostCpuid7ex[4] = { 0 };
    __cpuid(hostCpuid7ex, 0x80000007);
    cpuidOverrides[2].Function = 0x80000007;
    cpuidOverrides[2].Eax = (UINT32)hostCpuid7ex[0];
    cpuidOverrides[2].Ebx = (UINT32)hostCpuid7ex[1];
    cpuidOverrides[2].Ecx = (UINT32)hostCpuid7ex[2];
    cpuidOverrides[2].Edx = (UINT32)hostCpuid7ex[3] | (1UL << 8);
    printf("[cpuid] leaf 0x80000007: host EDX=0x%08X -> guest EDX=0x%08X (invariant TSC %s on host)\n",
           (UINT32)hostCpuid7ex[3], cpuidOverrides[2].Edx,
           ((UINT32)hostCpuid7ex[3] & (1UL << 8)) ? "present" : "ABSENT -- we are asserting it anyway");

    // KNOWN, UNADDRESSED: leaf 1 ECX bit 31 (hypervisor present) is passed
    // through from the host, where it is SET -- WHP itself runs on Hyper-V --
    // and the host advertises "Microsoft Hv" at leaf 0x40000000. So the guest is
    // told it is running under Hyper-V and will look for Hyper-V's timing
    // enlightenments (reference TSC page, synthetic timers), none of which we
    // implement. If Windows prefers those over the plain TSC, it will find them
    // missing and may keep falling back to the PM timer regardless of the bit
    // set above. The two candidate follow-ups are to hide the hypervisor bit, or
    // to implement the reference TSC page properly. Both need measuring before
    // either is worth shipping.

    hr = WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeCpuidResultList, cpuidOverrides, sizeof(cpuidOverrides));
    if (FAILED(hr)) { printf("Failed to set CPUID result list. HRESULT: 0x%lx\n", hr); return 1; }

    // TRIED AND REVERTED (twice now): WHvPartitionPropertyCodeSyntheticProcessorFeaturesBanks
    // + LocalApicEmulationMode=X2Apic. First attempt changed guest behavior
    // (proved WHV honors the property) but traded the clean, diagnosable
    // bugcheck 0x5C for an unbounded hang. Falsification experiment A (see
    // docs/roadmap.md) retried with a complete flag set -- also adding
    // DirectSyntheticTimers/SyntheticClusterIpi, which QEMU's WHPX
    // accelerator sets alongside the rest of this bank and the first
    // attempt omitted -- in case the hang was an artifact of an incomplete
    // flag set rather than proof transparent SynIC backing doesn't exist.
    // Result: identical hang (same RIP, same register signature, unbounded
    // past 38s). Strengthens rather than weakens the existing conclusion --
    // see docs/investigations/vppt-synic-blocker.md's 2026-07-16 update.

    // REVERTED: WHvMsrActionIgnoreWriteReadZero (tried earlier in this same
    // investigation) didn't change the bugcheck, and on reflection it's
    // actively risky to leave in place -- WHV creates guest partitions as
    // real children of the host's own Hyper-V hypervisor (confirmed
    // separately: our own CPUID overrides for the hypervisor-present bit
    // and the Hyper-V leaves 0x40000000-0x40000006 had ZERO effect on guest
    // behavior despite the API reporting success, meaning the guest's
    // hypervisor identity is enforced beneath the WHV API surface, not
    // something we control). That strongly implies the REAL Hyper-V root
    // may already transparently back genuine Hyper-V synthetic MSRs
    // (SCONTROL/SIMP/SINT/STIMER, VPPT's likely actual interrupt mechanism)
    // for this partition without any involvement from our emulation at
    // all. Forcing ALL unimplemented MSRs to silently discard writes and
    // read zero would have masked any such transparent passthrough,
    // turning working synthetic MSR access into inert no-ops. Left at the
    // architecture default (real #GP on truly unsupported MSRs) so
    // whatever WHV/Hyper-V does handle natively keeps working.

    // Enable #BP (breakpoint trap, vector 3) exception exits for the live
    // INT3 breakpoint infrastructure -- see the block near the top of
    // this file and docs/investigations/post-vppt-boot-stall.md. Only
    // this one vector is requested; every other exception continues to
    // route to the guest's own IDT unmodified.
    //
    // WHvPartitionPropertyCodeExceptionExitBitmap alone is NOT sufficient
    // -- confirmed the hard way (first version of this patch let the
    // guest's own kernel see and mishandle the INT3 as an unhandled
    // exception, bugchecking 0x1E with param1=STATUS_BREAKPOINT and
    // param2 exactly equal to our patched VA -- direct proof the CPU did
    // execute our INT3, but the intercept never routed it to us first).
    // WHV_EXTENDED_VM_EXITS.ExceptionExit must be explicitly enabled via
    // WHvPartitionPropertyCodeExtendedVmExits before the bitmap has any
    // effect.
    WHV_EXTENDED_VM_EXITS extendedExits = { 0 };
    extendedExits.ExceptionExit = 1;
    hr = WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeExtendedVmExits,
                                  &extendedExits, sizeof(extendedExits));
    if (FAILED(hr)) { printf("Failed to enable extended VM exits (ExceptionExit). HRESULT: 0x%lx\n", hr); return 1; }

    // RETARGETED AGAIN: the first attempt at this experiment used an INT3
    // patch (#BP) and got stuck behind the DbgBreakPointWithStatus
    // flooding hazard again -- with #BP interception active, that genuine,
    // unrelated, extremely-high-frequency software INT3 elsewhere in the
    // kernel throttles the guest's real progress so much that 86 real
    // seconds of wall-clock time didn't correspond to reaching anywhere
    // near where pass 2 normally crashes (confirmed by comparing against
    // an unthrottled watchpoint run's timing) -- an inconclusive result,
    // not a negative one. Switched to a HARDWARE EXECUTION breakpoint
    // (DR0-DR3/DR7, R/W=execute) instead of an INT3 patch: same DR
    // mechanism already proven clean for the data watchpoint experiments,
    // traps via #DB (vector 1) rather than #BP (vector 3), and by
    // construction never intercepts genuine software INT3s anywhere in
    // the kernel (those are always vector 3) -- sidesteps the flooding
    // hazard entirely rather than fighting it.
    //
    // Does Phase1InitializationDiscard (the real system-thread entry point
    // that runs Phase 1 init on genuine Windows, per public NT internals --
    // confirmed present in this exact build via SymFromName) get reached
    // at all on pass 2? If not, both pool-heap findings are simply
    // downstream symptoms of a thread that never ran, not independent
    // gaps.
    UINT64 exceptionExitBitmap = (1ULL << WHvX64ExceptionTypeDebugTrapOrFault);
    hr = WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeExceptionExitBitmap,
                                  &exceptionExitBitmap, sizeof(exceptionExitBitmap));
    if (FAILED(hr)) { printf("Failed to set exception exit bitmap. HRESULT: 0x%lx\n", hr); return 1; }

    hr = WHvSetupPartition(partition);
    if (FAILED(hr)) { printf("Failed to setup partition. HRESULT: 0x%lx\n", hr); return 1; }

    hr = WHvCreateVirtualProcessor(partition, 0, 0);
    if (FAILED(hr)) { printf("Failed to create vCPU. HRESULT: 0x%lx\n", hr); return 1; }

    guestMemory = VirtualAlloc(NULL, guestMemSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (guestMemory == NULL) { printf("Failed to allocate guest memory.\n"); return 1; }

    { // I/O APIC redirection tables (both controllers): start fully masked,
      // matching real hardware's power-on default (mask bit, 0x10000, set
      // in every entry).
        int ioapicI;
        for (ioapicI = 0; ioapicI < 24; ioapicI++) {
            ioapic1.redirTable[ioapicI] = 0x10000ULL;
            ioapic2.redirTable[ioapicI] = 0x10000ULL;
        }
    }

    if (!uefiMode) {
        hmaMemory = VirtualAlloc(NULL, A20_WINDOW_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (hmaMemory == NULL) { printf("Failed to allocate HMA memory.\n"); return 1; }
    }

    hr = WHvMapGpaRange(partition, guestMemory, 0, guestMemSize,
                         WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
    if (FAILED(hr)) { printf("Failed to map guest memory. HRESULT: 0x%lx\n", hr); return 1; }

    // TEMP DIAGNOSTIC: A20 aliasing disabled to test whether it's corrupting
    // hypervisor SLAT/EPT state and causing the later rep-movsb stall.
    // updateA20Mapping(partition);
    // (Moot for UEFI regardless -- guest RAM is mapped flat, so there's no
    // real-mode 1MB wraparound to fake.)

    if (uefiMode) {
        // UEFI firmware images (OVMF) are meant to sit at the very top of a
        // 4GB address space, with the architectural x86 reset vector
        // (physical 0xFFFFFFF0) landing inside them -- see the CS.Base
        // setup below. Load the whole file there instead of at 0xC0000.
        FILE *f = fopen(biosPath, "rb");
        if (!f) { printf("Failed to open %s\n", biosPath); return 1; }
        fseek(f, 0, SEEK_END);
        long fwSizeSigned = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fwSizeSigned <= 0) { printf("Invalid UEFI firmware image %s\n", biosPath); fclose(f); return 1; }
        SIZE_T fwSize = (SIZE_T)fwSizeSigned;

        uefiFirmwareMemory = VirtualAlloc(NULL, fwSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (uefiFirmwareMemory == NULL) { printf("Failed to allocate UEFI firmware memory.\n"); fclose(f); return 1; }
        size_t bytesRead = fread(uefiFirmwareMemory, 1, fwSize, f);
        fclose(f);
        printf("Loaded %zu bytes of UEFI firmware %s\n", bytesRead, biosPath);
        fflush(stdout);

        UINT64 fwBase = 0x100000000ULL - fwSize;
        hr = WHvMapGpaRange(partition, uefiFirmwareMemory, fwBase, fwSize,
                             WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
        if (FAILED(hr)) { printf("Failed to map UEFI firmware. HRESULT: 0x%lx\n", hr); return 1; }
        printf("Mapped UEFI firmware at 0x%llX-0xFFFFFFFF, entering run loop\n", (unsigned long long)fwBase);
        fflush(stdout);

        InitializeCriticalSection(&kdRxLock);
        InitializeCriticalSection(&kdTxLock);
        kdTxEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        // FILE_FLAG_OVERLAPPED (U68) is load-bearing, not a style choice. With a
        // synchronous handle, WriteFile blocks until the reader drains the pipe --
        // and a debugger that has given up (kd prints [no_debuggee] after ~7s and
        // stops reading) never drains it. The 4KB buffer fills, the writer thread
        // wedges inside WriteFile forever, and every subsequent guest byte is
        // dropped: measured at 32231 bytes lost and written=0, which is exactly
        // why kd never saw a single KD packet. Real serial hardware cannot
        // deadlock the CPU this way -- it shifts bytes out and overruns if nobody
        // is listening -- so the emulation must not either. See kdPipeWriterThread.
        kdPipe = CreateNamedPipeA("\\\\.\\pipe\\LocalHostKD",
                                   PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 4096, 4096, 0, NULL);
        if (kdPipe != INVALID_HANDLE_VALUE) {
            printf("[kd] named pipe \\\\.\\pipe\\LocalHostKD ready -- attach WinDbg with "
                   "-k com:pipe,port=\\\\.\\pipe\\LocalHostKD,resets=0,reconnect (COM2, ports 0x2F8-0x2FF)\n");
            fflush(stdout);
            {
                const char *apicDevEnv = getenv("LOCALHOST_APIC_DEVICE");
                if (apicDevEnv) g_apicDeviceVectors = (atol(apicDevEnv) != 0);
                printf("[irq] device interrupts via APIC: %s\n",
                       g_apicDeviceVectors ? "YES" : "no (raw slot)");
                fflush(stdout);
            }
            {
                const char *apicTimerEnv = getenv("LOCALHOST_APIC_TIMER");
                if (apicTimerEnv) g_apicTimerVectors = (atol(apicTimerEnv) != 0);
                printf("[irq] timer interrupts via APIC: %s\n",
                       g_apicTimerVectors ? "YES" : "no (raw slot)");
                fflush(stdout);
            }
            {
                // THIS PARSING DID NOT EXIST. The comment on g_rtlHwVerId said the
                // value was "overridable so the revision can be swept without a
                // rebuild", and startvm.bat duly set LOCALHOST_RTL_VERID -- but
                // nothing ever read it, so the eight-chip-ID sweep booted the same
                // compiled-in 0x74000000 eight times and its "all eight rejected"
                // table measured nothing at all. A knob that is documented and
                // plumbed but never read is worse than no knob: it produces
                // confident, entirely fictional results.
                //
                // Hence the printf. Every run now states the identity it is
                // actually presenting, so a sweep can be checked against the log
                // instead of trusted.
                const char *veridEnv = getenv("LOCALHOST_RTL_VERID");
                if (veridEnv) g_rtlHwVerId = (UINT32)strtoul(veridEnv, NULL, 0);
                const char *pciRevEnv = getenv("LOCALHOST_RTL_PCIREV");
                if (pciRevEnv) g_rtlPciRev = (unsigned char)strtoul(pciRevEnv, NULL, 0);
                printf("[rtl8139] identity: TCR hwver=0x%08lX, PCI rev=0x%02X%s\n",
                       (unsigned long)g_rtlHwVerId, g_rtlPciRev,
                       (g_rtlHwVerId == 0x74800000U && g_rtlPciRev == 0x20)
                           ? " (RTL8139C+, matching QEMU)" : "");
                fflush(stdout);
            }
            {
                const char *rebaseEnv = getenv("LOCALHOST_AHCI_REBASE");
                if (rebaseEnv) g_ahciAllowRebase = (atol(rebaseEnv) != 0);
                printf("[ahci] follow guest BAR re-base: %s\n",
                       g_ahciAllowRebase ? "YES (storahci binds and sees the disk)"
                                         : "no (guest will see no disk)");
                fflush(stdout);
            }
            {
                const char *tprTimerEnv = getenv("LOCALHOST_TPR_GATE_TIMER");
                if (tprTimerEnv) g_tprGateTimer = (atol(tprTimerEnv) != 0);
                printf("[irq] TPR gate covers timer vectors: %s\n",
                       g_tprGateTimer ? "YES" : "no (input only)");
                fflush(stdout);
            }
            {
                const char *rtcMinEnv = getenv("LOCALHOST_RTC_MIN_MS");
                if (rtcMinEnv) g_rtcMinIntervalMs = atof(rtcMinEnv);
                printf("[rtc] periodic interrupt floor: %.0f ms%s\n", g_rtcMinIntervalMs,
                       g_rtcMinIntervalMs > 0.0 ? " (capping the guest's programmed rate)" : "");
                fflush(stdout);
            }
            {
                const char *wakeEnv = getenv("LOCALHOST_PS2_WAKE");
                if (wakeEnv) g_ps2WakeEnabled = (atol(wakeEnv) != 0);
                printf("[ps2] wake run loop on keystroke: %s\n",
                       g_ps2WakeEnabled ? "yes" : "no (disabled)");
                fflush(stdout);
            }
            {
                const char *reassertEnv = getenv("LOCALHOST_PS2_REASSERT_MS");
                if (reassertEnv) g_ps2ReassertMs = atof(reassertEnv);
                printf("[ps2] interrupt re-assert interval: %.0f ms\n", g_ps2ReassertMs);
                fflush(stdout);
            }
            {
                const char *latchEnv = getenv("LOCALHOST_RTC_LATCH_MS");
                if (latchEnv) g_rtcLatchMs = atof(latchEnv);
                printf("[rtc] snapshot latch: %.0f ms%s\n", g_rtcLatchMs,
                       g_rtcLatchMs <= 0.0 ? " (DISABLED -- re-sampling every register read)" : "");
                fflush(stdout);
            }
            {
                const char *forceIso = getenv("LOCALHOST_BOOT_ISO");
                if (forceIso) g_forceBootIso = (atol(forceIso) != 0);
                if (g_forceBootIso) {
                    printf("[autokey] LOCALHOST_BOOT_ISO=1 -- answering the CD/DVD prompt even "
                           "though the disk has an OS (deliberate reinstall)\n");
                    fflush(stdout);
                }
            }
            {
                const char *noAutoKey = getenv("LOCALHOST_NO_AUTO_BOOT_KEY");
                g_noAutoBootKey = (noAutoKey && atol(noAutoKey) != 0) ? 1 : 0;
                printf("[autokey] \"Press any key to boot from CD or DVD\": %s\n",
                       g_noAutoBootKey
                         ? "NOT answered (LOCALHOST_NO_AUTO_BOOT_KEY=1) -- press a key yourself"
                         : "answered automatically on the FIRST boot only; later prompts "
                           "(mid-install reboots) are left alone so the install can finish");
                fflush(stdout);
            }
            // Default ON now (see g_tabletEnabled). The variable is still read so
            // the relative PS/2 mouse remains one env var away if the tablet ever
            // regresses -- LOCALHOST_USB_TABLET=0 restores the old behaviour.
            const char *tabletEnv = getenv("LOCALHOST_USB_TABLET");
            if (tabletEnv) g_tabletEnabled = (atol(tabletEnv) != 0);
            if (g_tabletEnabled) {
                printf("[usb] tablet ENABLED -- absolute pointer owns the cursor, PS/2 mouse suppressed\n");
            } else {
                printf("[usb] tablet DISABLED by LOCALHOST_USB_TABLET=0 -- relative PS/2 mouse owns the "
                       "cursor, so the guest pointer will drift out of step with the host one\n");
            }
            fflush(stdout);
            const char *breakinEnv = getenv("LOCALHOST_KD_BREAKIN_SEC");
            if (breakinEnv) {
                g_kdBreakinAtSec = atol(breakinEnv);
                if (g_kdBreakinAtSec > 0)
                    printf("[kd] will inject a break-in %ld seconds in\n", g_kdBreakinAtSec);
            }
            const char *breakinExitsEnv = getenv("LOCALHOST_KD_BREAKIN_EXITS");
            if (breakinExitsEnv) {
                g_kdBreakinAtExits = atol(breakinExitsEnv);
                if (g_kdBreakinAtExits > 0)
                    printf("[kd] will inject a break-in at exitCount %ld (guest progress, not wall time)\n",
                           g_kdBreakinAtExits);
            }
            HANDLE kdThread = CreateThread(NULL, 0, kdPipeReaderThread, NULL, 0, NULL);
            if (kdThread) CloseHandle(kdThread);
            HANDLE kdWriterThread = CreateThread(NULL, 0, kdPipeWriterThread, NULL, 0, NULL);
            if (kdWriterThread) CloseHandle(kdWriterThread);
        } else {
            printf("[kd] failed to create named pipe, GetLastError=%lu\n", GetLastError());
            fflush(stdout);
        }
    } else {
        FILE *f = fopen(biosPath, "rb");
        if (!f) { printf("Failed to open %s\n", biosPath); return 1; }
        size_t bytesRead = fread((char*)guestMemory + 0xC0000, 1, 0x40000, f);
        fclose(f);
        printf("Loaded %zu bytes of %s at 0xC0000\n", bytesRead, biosPath);

        // Real BIOS flash is decoded at two addresses: the classic
        // 0xC0000-0xFFFFF window AND mirrored at the top of the 4GB address
        // space (0xFFFC0000-0xFFFFFFFF for a 256KB image) so 32-bit
        // protected-mode code can reach it without real-mode segment
        // tricks. SeaBIOS relies on this mirror very early in POST --
        // without it, execution wanders into unmapped memory at a GPA like
        // 0xFFFEE913 and (once faulting pages are correctly mapped
        // executable) crashes with an unrecoverable exception on the
        // resulting garbage/zeroed code.
        UINT64 biosMirrorBase = 0x100000000ULL - 0x40000;
        hr = WHvMapGpaRange(partition, (char *)guestMemory + 0xC0000, biosMirrorBase, 0x40000,
                             WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
        if (FAILED(hr)) { printf("Failed to map BIOS mirror. HRESULT: 0x%lx\n", hr); return 1; }
    }

    WHV_REGISTER_NAME regNames[2] = { WHvX64RegisterRip, WHvX64RegisterCs };
    WHV_REGISTER_VALUE regValues[2] = { 0 };
    regValues[0].Reg64 = 0xFFF0;

    WHV_X64_SEGMENT_REGISTER cs = { 0 };
    // Legacy path: CS.Base=0xF0000 puts the reset vector at physical
    // 0xFFFF0, the classic BIOS convention (firmware fits in the last 64KB
    // of the first 1MB). UEFI path: CS.Base=0xFFFF0000 matches real x86
    // hardware reset state, landing at physical 0xFFFFFFF0 -- inside the
    // firmware image mapped at the top of 4GB above.
    cs.Base = uefiMode ? 0xFFFF0000 : 0xF0000;
    cs.Limit = 0xFFFF;
    cs.Selector = 0xF000;
    cs.Attributes = 0x9B;
    regValues[1].Segment = cs;

    hr = WHvSetVirtualProcessorRegisters(partition, 0, regNames, 2, regValues);
    if (FAILED(hr)) { printf("Failed to set registers. HRESULT: 0x%lx\n", hr); return 1; }

    int running = 1;
    long exitCount = 0;
    UINT16 debugLastPort = 0;
    BOOL debugLastWrite = FALSE;
    unsigned char debugLastVal = 0;

    g_watchdogPartition = partition;

    // --- Guest Tools channel (COM3) ----------------------------------------
    // Created unconditionally, unlike the KD pipe: the tools channel has to
    // exist for every guest, not only a UEFI one being debugged. A guest with
    // no agent installed simply never opens the far end, which the writer
    // thread treats as a normal state rather than an error.
    InitializeCriticalSection(&gtRxLock);
    InitializeCriticalSection(&gtTxLock);
    gtTxEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    gtPipe = CreateNamedPipeA("\\\\.\\pipe\\LocalHostGT",
                              PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                              1, 8192, 8192, 0, NULL);
    if (gtPipe != INVALID_HANDLE_VALUE) {
        HANDLE gtReader = CreateThread(NULL, 0, gtPipeReaderThread, NULL, 0, NULL);
        HANDLE gtWriter;
        if (gtReader) CloseHandle(gtReader);
        gtWriter = CreateThread(NULL, 0, gtPipeWriterThread, NULL, 0, NULL);
        if (gtWriter) CloseHandle(gtWriter);
        printf("[gt] Guest Tools channel ready (COM3, ports 0x3E8-0x3EF)\n");
    } else {
        printf("[gt] failed to create the Guest Tools pipe, GetLastError=%lu\n", GetLastError());
    }
    fflush(stdout);
    CreateThread(NULL, 0, stallWatchdogThread, NULL, 0, NULL);

    // The RTC periodic interrupt (see deliverRtcPeriodicIrq) can only be
    // delivered between calls to WHvRunVirtualProcessor -- but guest code
    // that busy-spins without touching any trapped I/O/MMIO (exactly what
    // HalpTimerWaitForPhase0Interrupt's KeStallExecutionProcessor loop
    // does, confirmed live: docs/investigations/vppt-synic-blocker.md part
    // 10) can run for the guest's ENTIRE wait window inside a single
    // WHvRunVirtualProcessor call, generating zero VM exits and giving the
    // main loop no opportunity to check/inject anything at all. Force
    // periodic exits with WHvCancelRunVirtualProcessor (documented-safe to
    // call from another thread) whenever the RTC's periodic interrupt is
    // armed, so the main loop regularly regains control and can deliver it.
    CreateThread(NULL, 0, rtcCancelThread, NULL, 0, NULL);

    while (running) {
        // --- Halted-CPU wait loop ---
        // While the guest is halted, we don't call WHvRunVirtualProcessor at all.
        // Instead we pump window messages and look for something to wake it up
        // (keyboard data or a timer tick). Once we inject something, we fall
        // through and call WHvRunVirtualProcessor once to deliver it.
        while (cpuHalted && running) {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { running = 0; }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (!running) break;

            // U66: throttle to ~60Hz. This used to invalidate on EVERY pass of the
            // halted loop, which with the Sleep(1) below is roughly 1000 times a
            // second -- each one a full 800x600 StretchDIBits blit on the UI thread.
            // The main run loop already throttles its own repaint (exitCount % 5000)
            // but this path did not, and it is the path the guest spends nearly all
            // its time in now that Windows boots and idles in HalProcessorIdle
            // (HLT). So the better the guest behaved, the harder we hammered the
            // host -- which is what made the machine lag badly enough to need two
            // force shutdowns.
            if (g_hwnd) {
                static LARGE_INTEGER lastPaint = { 0 };
                LARGE_INTEGER nowPaint;
                QueryPerformanceCounter(&nowPaint);
                double sincePaintMs = (lastPaint.QuadPart == 0) ? 1e9 :
                    (double)(nowPaint.QuadPart - lastPaint.QuadPart) * 1000.0 / perfFrequency.QuadPart;
                if (sincePaintMs >= 16.0) {
                    lastPaint = nowPaint;
                    InvalidateRect(g_hwnd, NULL, FALSE);
                }
            }

            if (!guestInterruptsEnabled(partition)) {
                // Guest disabled interrupts while halted (unusual, but be safe) --
                // just keep waiting without injecting anything.
                Sleep(1);
                continue;
            }

            int injected = 0;

            // Service USB here too, not just in the outer loop. This is the HLT
            // path -- where the guest sits whenever it is idle, which is exactly
            // when it is waiting for the pointer input we are trying to deliver.
            // Without this the tablet freezes the moment the guest goes quiet.
            usbServiceSchedules(partition);

            // INPUT FIRST. This chain is an else-if, and deliverRtcPeriodicIrq
            // succeeds on essentially every pass once the guest enables the RTC
            // periodic interrupt (PIE=1, ~1kHz) -- so with the keyboard and mouse
            // sitting BELOW it, their branches were never reached and their
            // interrupts were never delivered at all. Keystrokes piled up unread
            // (measured kbPending=1 with the guest idle at IRQL 0) and the mouse
            // could never finish initialising. The clock was starving input.
            //
            // Input is rare and the RTC is constant, so giving input priority costs
            // the timer nothing: at worst a periodic tick is deferred by one pass
            // of this loop, and deliverRtcPeriodicIrq will simply deliver it next
            // time round.
            //
            // Raised through the same one-interrupt-per-presented-byte rule the
            // main loop uses (ps2ServiceOutputIrq). This used to re-inject on
            // EVERY pass of this loop while a byte was pending, and it also
            // always raised the KEYBOARD line even when the byte on offer came
            // from the mouse -- so the guest's keyboard ISR was woken to read a
            // byte that the status register was tagging as the mouse's.
            if (ps2ServiceOutputIrq(partition)) {
                injected = 1;
            } else if (pendingAtaIrq) {
                injectDeviceIrq(partition, GSI_AHCI, 0x76); // U53: IRQ14/AHCI, routed
                pendingAtaIrq = 0;
                injected = 1;
            } else if (deliverRtcPeriodicIrq(partition)) {
                // RTC periodic interrupt (IRQ8). See deliverRtcPeriodicIrq's
                // own comment -- this is what Windows HAL's phase-0 timer
                // test actually waits for.
                injected = 1;
            // (keyboard and mouse are handled at the TOP of this chain now -- see
            // the comment there for why they cannot sit below the RTC)
            } else if (pendingRtl8139Irq) {
                // IRQ11 - RTL8139 NIC. Needed here (not just the outer
                // loop's deliverPendingRtl8139Irq) so a packet arriving
                // while the guest is parked in HLT waiting for one -- the
                // whole point of interrupt-driven RX -- actually wakes it.
                injectDeviceIrq(partition, GSI_NIC, 0x73);
                pendingRtl8139Irq = 0;
                injected = 1;
            } else if (pendingE1000Irq) {
                injectDeviceIrq(partition, GSI_E1000, 0x7B);
                pendingE1000Irq = 0;
                injected = 1;
            } else {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                double elapsedMs = (double)(now.QuadPart - lastTimerTick.QuadPart) * 1000.0 / perfFrequency.QuadPart;
                if (elapsedMs >= TIMER_TICK_INTERVAL_MS) {
                    deliverLegacyTimerTick(partition); // IRQ0 - timer, gated+counted
                    lastTimerTick = now;
                    injected = 1;
                }
            }

            if (injected) {
                cpuHalted = 0; // fall through to WHvRunVirtualProcessor below
            } else {
                // Time spent here is time the guest is HALTED and waiting. See
                // g_haltedMs: this is the measurement that separates "the guest is
                // computing slowly" from "the guest is idle waiting to be woken".
                LARGE_INTEGER hs, he;
                QueryPerformanceCounter(&hs);
                Sleep(1);
                QueryPerformanceCounter(&he);
                if (perfFrequency.QuadPart)
                    g_haltedMs += (double)(he.QuadPart - hs.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart;
            }
        }
        if (!running) break;

        exitCount++;
        g_watchdogExitCount = exitCount;

        if (exitCount % 200 == 0) {
            Sleep(0); // yield periodically -- avoid starving the host scheduler
        }

        // U56b: keep the level-triggered AHCI interrupt asserted while the driver
        // has an unserviced status bit. Cheap -- reads two dwords from our own
        // buffer and returns immediately in the common case.
        // Hand the guest a queued interrupt whenever its pending slot frees up.
        // Cheap (one register read in the common case) and it is what stops a
        // backlog forming behind the RTC.
        drainInterruptQueue(partition);

        // Firmware timer tick. The two other call sites only fire when the guest
        // HALTS or on a cancelled exit, and OVMF's firmware phase is CPU-busy and
        // does neither -- so without this the firmware gets no periodic timer at
        // all (measured: pitTicks=0 across a whole boot).
        //
        // An earlier version of this line wedged the guest at exit 8571, because
        // it injected unconditionally. deliverLegacyTimerTick now refuses until
        // PIT channel 0 is programmed AND the PIC has IRQ0 unmasked, so nothing
        // is delivered before the firmware has asked for it.
        deliverPitTimerIrq(partition);

        ahciServiceLevelInterrupt(partition, exitCount);

        // Run the USB async schedule. A real host controller executes its
        // schedules continuously off the frame timer, and USBPORT queues
        // transfers without ringing any doorbell we could trap -- so this has to
        // be polled rather than driven from a register write. Throttled because
        // walking guest memory on every single exit would dominate the loop.
        usbServiceSchedules(partition);

        if (exitCount % 5000 == 0 && heartbeatDue()) {
            static LARGE_INTEGER startTick = { 0 };
            LARGE_INTEGER nowTick;
            QueryPerformanceCounter(&nowTick);
            if (startTick.QuadPart == 0) startTick = nowTick;
            double elapsedSec = (double)(nowTick.QuadPart - startTick.QuadPart) / perfFrequency.QuadPart;
            // U54: carry RIP (and its module-relative RVA) plus the exit reason.
            // The guest now idles at ~64 exits/sec without ever stalling long
            // enough for the watchdog, and the U40 sampler only sees exit-causing
            // instructions -- so neither instrument answers "where is it?". The
            // heartbeat fires rarely enough that a register read here is free.
            WHV_REGISTER_NAME hbNames[2] = { WHvX64RegisterRip, WHvX64RegisterCr8 };
            WHV_REGISTER_VALUE hbVals[2] = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, hbNames, 2, hbVals);
            UINT64 hbRip = hbVals[0].Reg64;
            printf("[heartbeat: exitCount=%ld elapsedSec=%.1f lastPort=0x%X write=%d val=0x%X rip=0x%llX%s irql=%llu]\n",
                   exitCount, elapsedSec, debugLastPort, debugLastWrite, debugLastVal,
                   (unsigned long long)hbRip,
                   (g_bpModuleBase && hbRip > g_bpModuleBase && hbRip - g_bpModuleBase < 0x2000000ULL)
                       ? "" : " (not in ntoskrnl)",
                   (unsigned long long)hbVals[1].Reg64);
            if (g_bpModuleBase && hbRip > g_bpModuleBase && hbRip - g_bpModuleBase < 0x2000000ULL)
                printf("[heartbeat]   ntoskrnl+0x%llX  (resolve: tools/pdbsym.py near <rva>)\n",
                       (unsigned long long)(hbRip - g_bpModuleBase));
            // U69/U91: fire the scripted break-in once the guest has booted far
            // enough -- measured either in wall-clock seconds or, preferably, in
            // guest exits (see g_kdBreakinAtExits on why seconds mislead).
            int breakinDue = (g_kdBreakinAtSec > 0 && elapsedSec >= (double)g_kdBreakinAtSec) ||
                             (g_kdBreakinAtExits > 0 && exitCount >= g_kdBreakinAtExits);
            if (breakinDue && !g_kdBreakinSent && kdClientConnected) {
                EnterCriticalSection(&kdRxLock);
                int biNext = (kdRxHead + 1) % (int)sizeof(kdRxBuf);
                if (biNext != kdRxTail) { kdRxBuf[kdRxHead] = 0x62; kdRxHead = biNext; }
                LeaveCriticalSection(&kdRxLock);
                g_kdBreakinSent = 1;
                printf("[kd] injected break-in byte 0x62 at %.1fs / exitCount=%ld (sec=%ld exits=%ld)\n",
                       elapsedSec, exitCount, g_kdBreakinAtSec, g_kdBreakinAtExits);
                fflush(stdout);
            }
            // U55: real running totals, not log-capped counts. Three separate
            // conclusions in this investigation were drawn from truncated logs and
            // had to be retracted -- the IOAPIC "260 vs 560 ops" (U42_LOG_PER_PASS
            // cap), "no PCI access after discovery" (pciConfigAccessLogCount <= 300)
            // and "AHCI stops after 199 commands" (ahciCmdLogCount < 200). The
            // counters themselves are uncapped, so print those instead of inferring
            // activity from how many lines happened to be logged.
            printf("[heartbeat]   totals: pciCfgAccesses=%d ahciCmds=%d (reads=%d writes=%d other=%d)\n",
                   pciConfigAccessLogCount, ahciCmdLogCount, g_ahciReads, g_ahciWrites, g_ahciOther);
            // U56b: has storahci actually taken ownership of the controller?
            // The ABAR is mapped as plain guest RAM, so driver accesses never trap
            // and cannot be logged -- but the buffer is ours, so poll it directly.
            // GHC.AE(bit31)/IE(bit1) and PxCMD.ST(bit0)/FRE(bit4) tell us whether a
            // driver has enabled the HBA and started the port; PxIE tells us whether
            // it armed interrupts (which is what the U55b pin fix was meant to
            // unblock). If these stay at their reset values, no driver ever bound.
            if (ahciAbarMemory) {
                unsigned char *ab = (unsigned char *)ahciAbarMemory;
                UINT32 ghc = *(UINT32 *)(ab + 0x04), pi = *(UINT32 *)(ab + 0x0C);
                unsigned char *pt = ab + 0x100;
            // U62: is the guest actually PAINTING? WinPE runs from a RAM disk (the
            // firmware loaded 437MB of boot.wim), so little disk I/O afterwards is
            // normal and the absence of writes proves nothing. What matters is
            // whether Setup is drawing. Checksum a sample of the framebuffer and
            // report whether it changes between heartbeats: changing means the guest
            // is rendering and our display path is the problem; static means it is
            // genuinely stuck before Setup's UI.
            // U97: NOT gated behind LOCALHOST_VERBOSE_DIAG any more. Sampling every
            // 997th byte costs nothing, and "how much of the screen is non-black"
            // is now the primary measurement in this investigation: setup.exe is
            // confirmed running with a healthy win32k message pump, so the open
            // question is purely whether its UI ever reaches the framebuffer.
            if (ramfbConfigWritten && ramfbAddress && guestMemory &&
                ramfbAddress + (UINT64)ramfbStride * ramfbHeight <= guestMemSize) {
                static UINT32 lastFbSum = 0;
                static int fbSumSeen = 0;
                const unsigned char *fb = (const unsigned char *)guestMemory + ramfbAddress;
                UINT64 total = (UINT64)ramfbStride * ramfbHeight;
                UINT32 sum = 0, nonZero = 0;
                UINT64 k;
                for (k = 0; k < total; k += 997) { // prime stride: cheap, spreads the sample
                    sum = sum * 31u + fb[k];
                    if (fb[k]) nonZero++;
                }
                UINT64 samples = total / 997;
                printf("[heartbeat]   framebuffer: sum=0x%08X %s nonBlack=%u/%llu (%.1f%%)\n",
                       sum,
                       !fbSumSeen ? "(first sample)" : (sum != lastFbSum ? "CHANGED -- guest is painting" : "unchanged"),
                       nonZero, (unsigned long long)samples,
                       samples ? (100.0 * (double)nonZero / (double)samples) : 0.0);
                lastFbSum = sum;
                fbSumSeen = 1;
            }
                printf("[heartbeat]   COM2 TX (uncapped): total=%ld  KD leaders: 0x30='0'=%ld 0x69='i'=%ld 0x62='b'=%ld\n",
                       g_uart2TxTotal, g_uart2Tx30, g_uart2Tx69, g_uart2Tx62);
                printf("[heartbeat]   EHCI (uncapped): barBase=0x%X reads=%ld writes=%ld portResets=%ld "
                       "USBCMD=0x%08X USBSTS=0x%08X CONFIGFLAG=%u PORTSC=0x%08X\n",
                       ehciBarBase, g_ehciMmioReads, g_ehciMmioWrites, g_ehciPortResets,
                       ehciUsbCmd, ehciUsbSts, ehciConfigFlag, ehciPortSc);
                printf("[heartbeat]   USB tablet: setups=%ld descriptorReads=%ld stalls=%ld reports=%ld "
                       "addr=%d configured=%d asyncBase=0x%X irqs=%ld moves=%ld periodicBase=0x%X"
                       " shortReports=%ld enabled=%d\n",
                       g_usbSetupPackets, g_usbDescriptorReads, g_usbStalls, g_usbReportsSent,
                       usbTabletAddress, usbTabletConfigured, ehciAsyncBase, g_ehciIrqCount,
                       (long)g_tabletMoves, ehciPeriodicBase, g_usbShortReports, g_tabletEnabled);
                // U110: the position stream itself. Reset each window so a burst
                // of movement is not diluted by the idle time around it.
                if (g_tabChanges > 0) {
                    printf("[heartbeat]   tablet motion: changes=%ld gap avg=%.1fms max=%.1fms |"
                           " step avg=%ld max=%ld (of 32767, so 1%% = 328)\n",
                           g_tabChanges,
                           g_tabSumGapMs / (double)g_tabChanges, g_tabMaxGapMs,
                           g_tabSumJump / g_tabChanges, g_tabMaxJump);
                    g_tabChanges = 0; g_tabSumGapMs = 0.0; g_tabMaxGapMs = 0.0;
                    g_tabSumJump = 0; g_tabMaxJump = 0;
                }
                // U111: the main loop's own period. If maxGap here matches the
                // tablet's stall, the loop is stopping and USB is a bystander.
                // Rates, not maxima, so windows of different length compare.
                printf("[heartbeat]   loop period: calls=%ld max=%.1fms |"
                       " >50ms=%ld (%.2f/s) >200ms=%ld (%.2f/s)\n",
                       g_usbSvcCalls, g_usbSvcMaxGapMs,
                       g_usbSvcStalls50,  g_usbSvcStalls50  / (g_heartbeatMinSec > 0 ? g_heartbeatMinSec : 1.0),
                       g_usbSvcStalls200, g_usbSvcStalls200 / (g_heartbeatMinSec > 0 ? g_heartbeatMinSec : 1.0));
                g_usbSvcMaxGapMs = 0.0; g_usbSvcStalls50 = 0; g_usbSvcStalls200 = 0; g_usbSvcCalls = 0;
                // U112: delivered-report interval. Read this NEXT TO the tablet
                // motion line: reports short + motion long => sampling is at
                // fault, not USB.
                printf("[heartbeat]   report gap: n=%ld max=%.1fms | >50ms=%ld >200ms=%ld\n",
                       g_repCount, g_repMaxGapMs, g_repStalls50, g_repStalls200);
                g_repMaxGapMs = 0.0; g_repStalls50 = 0; g_repStalls200 = 0; g_repCount = 0;
                // U113: where the exits actually come from.
                {
                    long tot = 0; int k;
                    for (k = 0; k < 16; k++) tot += g_exitReasonCount[k];
                    printf("[heartbeat]   exits: total=%ld | mmio=%ld io=%ld halt=%ld cpuid=%ld"
                           " exc=%ld cancel=%ld msr=%ld other=%ld\n", tot,
                           g_exitReasonCount[0], g_exitReasonCount[1],
                           g_exitReasonCount[2], g_exitReasonCount[3],
                           g_exitReasonCount[4], g_exitReasonCount[5],
                           g_exitReasonCount[6], g_exitReasonCount[7]);
                    if (g_ioPortHist) {
                        int top[6] = { -1,-1,-1,-1,-1,-1 };
                        int i2, j2;
                        for (i2 = 0; i2 < 65536; i2++) {
                            if (!g_ioPortHist[i2]) continue;
                            for (j2 = 0; j2 < 6; j2++) {
                                if (top[j2] < 0 || g_ioPortHist[i2] > g_ioPortHist[top[j2]]) {
                                    int m; for (m = 5; m > j2; m--) top[m] = top[m-1];
                                    top[j2] = i2; break;
                                }
                            }
                        }
                        printf("[heartbeat]   top io ports:");
                        for (j2 = 0; j2 < 6; j2++)
                            if (top[j2] >= 0) printf(" 0x%X=%lu", top[j2], (unsigned long)g_ioPortHist[top[j2]]);
                        printf("\n");
                        memset(g_ioPortHist, 0, 65536 * sizeof(UINT32));
                    }
                    printf("[heartbeat]   top mmio pages:");
                    for (k = 0; k < 8; k++)
                        if (g_mmioPageHits[k]) printf(" 0x%llX=%ld", (unsigned long long)(g_mmioPage[k] << 12), g_mmioPageHits[k]);
                    printf("\n");
                    // U114: which EHCI register. Offsets are the operational
                    // registers: 0x00 CAPLENGTH/HCIVERSION, 0x04 HCSPARAMS,
                    // 0x20 USBCMD, 0x24 USBSTS, 0x28 USBINTR, 0x2C FRINDEX,
                    // 0x34 ASYNCLISTADDR, 0x44 PORTSC[0].
                    {
                        int t2[5] = { -1,-1,-1,-1,-1 }; int i3, j3;
                        for (i3 = 0; i3 < 256; i3++) {
                            if (!g_ehciOffHist[i3]) continue;
                            for (j3 = 0; j3 < 5; j3++) {
                                if (t2[j3] < 0 || g_ehciOffHist[i3] > g_ehciOffHist[t2[j3]]) {
                                    int m2; for (m2 = 4; m2 > j3; m2--) t2[m2] = t2[m2-1];
                                    t2[j3] = i3; break;
                                }
                            }
                        }
                        printf("[heartbeat]   top ehci regs:");
                        for (j3 = 0; j3 < 5; j3++)
                            if (t2[j3] >= 0) printf(" +0x%02X=%ld", t2[j3], g_ehciOffHist[t2[j3]]);
                        printf("\n");
                        memset(g_ehciOffHist, 0, sizeof(g_ehciOffHist));
                    }
                    memset(g_exitReasonCount, 0, sizeof(g_exitReasonCount));
                    memset(g_mmioPageHits, 0, sizeof(g_mmioPageHits));
                    memset(g_mmioPage, 0, sizeof(g_mmioPage));
                    fflush(stdout);
                }
                printf("[heartbeat]   net UDP: out=%ld outFail=%ld in=%ld noSession=%ld"
                       " reclaimed=%ld | reply latency worst=%.0f ms, over-2s=%ld\n",
                       g_netUdpOut, g_netUdpOutFail, g_netUdpIn, g_netUdpNoSession,
                       g_netUdpReclaimed, g_netUdpWorstMs, g_netUdpOver2s);
                printf("[heartbeat]   net TCP: syn=%ld connected=%ld refused=%ld tableFull=%ld"
                       " sockFail=%ld closed=%ld unknown=%ld | bytes out=%ld in=%ld"
                       " dup=%ld ooo=%ld reclaimed=%ld aged=%ld closedUnacked=%ld worstUnacked=%d\n",
                       g_tcpSyn, g_tcpEstablished, g_tcpRefused, g_tcpTableFull,
                       g_tcpSockFail, g_tcpClosed, g_tcpUnknown,
                       g_tcpBytesToHost, g_tcpBytesToGuest,
                       g_tcpDupSegments, g_tcpOutOfOrder, g_tcpReclaimed, g_tcpAged,
                       g_tcpClosedUnacked, g_tcpWorstUnacked);
                // TCP flow control, both directions. Before these limits were
                // enforced at all we simply overran them and lost the excess in
                // silence, so every one of these counters reading 0 previously
                // meant nothing whatsoever. A non-zero windowStalled or
                // windowClamped is the proof that the guest's receive window, and
                // not the RX ring, was the binding constraint all along.
                printf("[heartbeat]   net TCP flow: windowStalled=%ld windowClamped=%ld"
                       " | to host: shortSend=%ld sendBlocked=%ld\n",
                       g_tcpWindowStalled, g_tcpWindowClamped,
                       g_tcpShortSend, g_tcpSendBlocked);
                // Deferrals and drops are reported separately on purpose: a
                // deferred frame is still on its way, a dropped one is gone for
                // good and (with no retransmission) has broken a stream.
                printf("[heartbeat]   net TX: total=%ld arp=%ld ipv4=%ld ipv6=%ld other=%ld"
                       " | RX offered=%ld delivered=%ld deferred(noRE=%ld noBuf=%ld ringFull=%ld)"
                       " queued=%u peak=%ld DROPPED(size=%ld overflow=%ld)"
                       " arpReplies=%ld padded=%ld backpressure=%ld\n",
                       g_netTxFrames, g_netTxArp, g_netTxIpv4, g_netTxIpv6, g_netTxOther,
                       g_netRxOffered, g_netRxFrames,
                       g_netRxDeferNoRe, g_netRxDeferNoBuf, g_netRxDeferRingFull,
                       rtl8139RxQueueCount, g_netRxQueuePeak,
                       g_netRxDropSize, g_netRxQueueOverflow,
                       g_netArpReplies, g_netRxPadded, g_tcpRingBackpressure);
                // Written-into-the-ring is NOT the same as consumed-by-the-guest.
                // CAPR is the driver's read pointer; if it stops advancing while our
                // write pointer moves, the guest has stopped draining the ring and
                // every later frame is invisible to it no matter how many we deliver.
                {
                    UINT16 capr = *(UINT16 *)&rtl8139Regs[0x38];
                    UINT16 cbr  = *(UINT16 *)&rtl8139Regs[0x3A];
                    printf("[heartbeat]   net RX ring: CAPR=0x%04X CBR=0x%04X ourWritePos=0x%X "
                           "size=%u CR=0x%02X(RE=%d BUFE=%d) IMR=0x%04X ISR=0x%04X\n",
                           capr, cbr, rtl8139RxWritePos, rtl8139RxRingSize(),
                           rtl8139Regs[0x37], (rtl8139Regs[0x37] >> 3) & 1, rtl8139Regs[0x37] & 1,
                           *(UINT16 *)&rtl8139Regs[0x3C], *(UINT16 *)&rtl8139Regs[0x3E]);
                    printf("[heartbeat]   net RX ring: reEnables=%ld (would have wiped a"
                           " non-empty ring %ld times)\n",
                           g_netRingReEnables, g_netRingResetSkipped);
                    printf("[heartbeat]   net NIC irq: injected=%ld latched=%ld"
                           " pendingButMasked=%ld\n",
                           g_nicIrqInjected, g_nicIrqLatched, g_nicIrqMaskedOff);
                }
                // U68: the two halves of the pipe bridge, so "the guest is
                // transmitting" can be told apart from "the debugger is hearing it".
                printf("[heartbeat]   KD pipe: client=%d | guest->pipe: written=%ld writeFail=%ld (lastErr=%lu) ringDrop=%ld"
                       " | pipe->guest: fromPipe=%ld toGuest=%ld ringDrop=%ld\n",
                       kdClientConnected, g_kdTxToPipe, g_kdTxWriteFail, (unsigned long)g_kdTxLastErr,
                       g_kdTxDropped, g_kdRxFromPipe, g_kdRxToGuest, g_kdRxDropped);
                // WHAT IS THE GUEST ACTUALLY USING AS ITS CLOCK?
                //
                // Setup copies files at ~19 KB/s while the disk sits idle (PxCI=0,
                // every interrupt delivered, nothing stuck), so something paces the
                // GUEST rather than our I/O. Our RTC delivers 67/sec, but the guest
                // toggles PIE constantly, so it is probably not using the RTC as
                // its system clock -- which leaves the WHP-emulated LAPIC timer.
                // Its LVT/InitialCount/CurrentCount say the rate the guest asked
                // for and whether it is actually counting. Sampled on wall clock,
                // not exit count, and only once the kernel is up.
                if (g_bpModuleBase) {
                    static LARGE_INTEGER lastLapicDump;
                    LARGE_INTEGER nowLapic;
                    QueryPerformanceCounter(&nowLapic);
                    if (perfFrequency.QuadPart) {
                        double sinceS = lastLapicDump.QuadPart
                            ? (double)(nowLapic.QuadPart - lastLapicDump.QuadPart) / (double)perfFrequency.QuadPart
                            : 1e9;
                        if (sinceS > 20.0) {
                            lastLapicDump = nowLapic;
                            u46DumpLapic(partition, "periodic (what paces the guest?)");
                        }
                    }
                }
                {
                    long total = g_nicRingCount;
                    long have = total < NIC_RING_MAX ? total : NIC_RING_MAX;
                    long i2;
                    printf("[heartbeat]   nic: fwAccesses=%ld kernelAccesses=%ld  last %ld:",
                           g_nicFwAccesses, g_nicGuestAccesses, have);
                    for (i2 = total - have; i2 < total; i2++) {
                        int s2 = (int)(i2 % NIC_RING_MAX);
                        printf(" %s%02X=%X", g_nicRingWrite[s2] ? "W" : "R",
                               g_nicRingOff[s2], g_nicRingVal[s2]);
                    }
                    printf("\n");
                }
                {
                    double totMs = g_haltedMs + g_runningMs;
                    printf("[heartbeat]   vcpu: running=%.0f ms  halted=%.0f ms  (%.1f%% halted)\n",
                           g_runningMs, g_haltedMs, totMs > 0.0 ? (g_haltedMs * 100.0 / totMs) : 0.0);
                }
                printf("[heartbeat]   ahci integrity: badWrites=%ld badReads=%ld shortReads=%ld rejected=%ld dropped=%ld\\n",
                       g_ahciBadWrites, g_ahciBadReads, g_ahciShortReads, g_ahciRejected, g_ahciDropped);
                printf("[heartbeat]   ahci irq injections (uncapped)=%ld  data: firmware=%ld sectors (%ld MB) guest=%ld sectors (%ld MB)"
                       " | ABAR touches: fw=%ld kernel=%ld\n",
                       g_ahciIrqCount,
                       g_ahciFwSectors, (long)(((UINT64)g_ahciFwSectors * 512) / (1024 * 1024)),
                       g_ahciGuestSectors, (long)(((UINT64)g_ahciGuestSectors * 512) / (1024 * 1024)),
                       g_ahciAbarFwAccesses, g_ahciAbarGuestAccesses);
                printf("[heartbeat]   ahci: GHC=0x%08X (AE=%u IE=%u) PI=0x%X | PxCMD=0x%08X (ST=%u FRE=%u) PxIE=0x%08X PxIS=0x%08X PxCI=0x%08X PxTFD=0x%08X PxSSTS=0x%08X\n",
                       ghc, (ghc >> 31) & 1, (ghc >> 1) & 1, pi,
                       *(UINT32 *)(pt + 0x18), *(UINT32 *)(pt + 0x18) & 1, (*(UINT32 *)(pt + 0x18) >> 4) & 1,
                       *(UINT32 *)(pt + 0x14), *(UINT32 *)(pt + 0x10),
                       *(UINT32 *)(pt + 0x38), *(UINT32 *)(pt + 0x20), *(UINT32 *)(pt + 0x28));
            }
            fflush(stdout);
        }

        if (exitCount % 1000 == 0) {
            MSG msg;
            while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) { running = 0; }
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            if (!running) break;
        }

        // Repaint on WALL CLOCK, not on exit count.
        //
        // "every 5000 exits" silently ties the frame rate to how hard the guest is
        // working, which is backwards: the better the guest behaves, the less it
        // repaints. Latching the RTC removed ~1.3M exits per boot (the guest was
        // spinning re-reading the clock), and the immediate consequence was a
        // window still showing the firmware logo while Windows was already up --
        // the screen looked hung when the guest was fine. Same class of bug as the
        // USB schedules, which had to be moved off exit count for the same reason.
        {
            static LARGE_INTEGER lastPaint;
            LARGE_INTEGER nowPaint;
            QueryPerformanceCounter(&nowPaint);
            if (g_hwnd && perfFrequency.QuadPart) {
                double sincePaintMs = lastPaint.QuadPart
                    ? (double)(nowPaint.QuadPart - lastPaint.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart
                    : 1e9;
                // ~60fps, not 30. The guest draws its own cursor INTO the
                // framebuffer, so the pointer can never look smoother than the
                // rate at which this window repaints -- 30fps put up to 33ms of
                // purely visual lag on top of the tablet's poll interval, and
                // that stack is what still felt laggy after the input rate was
                // fixed. The halted-loop repaint below already uses ~60Hz; this
                // path was the odd one out, and it is the ONLY one that runs,
                // because the guest never halts (measured: halted=0ms, 0.0%).
                if (sincePaintMs >= 16.0) {   // ~60fps
                    lastPaint = nowPaint;
                    InvalidateRect(g_hwnd, NULL, FALSE);
                }
            }
        }

        deliverPendingAtaIrq(partition);
        deliverPendingE1000Irq(partition);
        deliverPendingRtl8139Irq(partition);

        // The guest finished its shutdown and asked to be powered off. Leaving
        // the loop here is a CLEAN exit -- everything is already flushed --
        // unlike killing the process, which is a power cut to the guest.
        if (InterlockedCompareExchange(&g_guestPoweredOff, 1, 1)) {
            printf("[acpi] guest is powered off -- exiting\n");
            fflush(stdout);
            break;
        }

        // Power button pressed (the console window was closed, or the manager
        // asked for a graceful power off). Latch PWRBTN_STS and raise the SCI so
        // the guest's ACPI driver runs its normal shutdown, exactly as a real
        // short press of a real power button would.
        if (InterlockedExchange(&g_powerButtonRequest, 0)) {
            pm1aStatus |= PM1_PWRBTN;
            if (pm1aEnable & PM1_PWRBTN) {
                printf("[acpi] power button pressed -- raising SCI to the guest\n");
                injectDeviceIrq(partition, GSI_SCI, 0);
            } else {
                // No OS has armed PWRBTN_EN yet (still in firmware, or the guest
                // has no ACPI driver running). Nothing would ever consume the
                // event, so say so rather than appearing to hang.
                printf("[acpi] power button pressed, but the guest has not enabled "
                       "PWRBTN_EN (PM1a_EN=0x%04X) -- it cannot shut itself down yet\n",
                       pm1aEnable);
            }
            fflush(stdout);
        }
        // REVERTED: delivering PS/2 keyboard/mouse bytes from here (in addition to
        // the halted-CPU wait branch) looked like the fix for hover feeling
        // stuttery while the guest is busy, but it re-injects on EVERY loop
        // iteration for as long as the queue is non-empty, with none of the
        // natural throttling the halted branch gets. Measured: boot wedged in
        // firmware at exitCount ~25000 (against 20905 PCI config accesses and 4014
        // AHCI commands on the same build without it) -- the firmware was being
        // flooded with interrupts it could not drain.
        //
        // If the stutter is worth fixing later it needs a rate limit and an
        // acknowledgement model (inject, then wait for the guest to actually read
        // port 0x60 before injecting again), not an unconditional per-iteration
        // injection.
        // RTC/PIT periodic-interrupt delivery deliberately NOT called here
        // unconditionally (2026-07-17, post-reboot #GP investigation --
        // see docs/investigations/post-vppt-boot-stall.md). Calling these
        // after every single VM exit (PCI config reads, I/O port probes,
        // MMIO -- whatever the guest happened to be doing at an arbitrary
        // point) injects an interrupt at an essentially random guest RSP.
        // Live-captured evidence: this landed inside OVMF's own exception/
        // interrupt-entry stub with RSP misaligned for its internal FXSAVE,
        // raising #GP -- confirmed via a falsification test (temporarily
        // removing these two calls let the exact same boot sail past the
        // ACPI-table-load point and reach BdsDxe with no crash). Delivery
        // now happens only from the two points proven safe/necessary: the
        // halted-CPU wait loop above (a single, consistent resume RIP each
        // time) and the WHvRunVpExitReasonCanceled case below (the bounded,
        // rtcCancelThread-driven forced-exit mechanism this was built for
        // in the first place). This narrows -- but does not fully close --
        // the original CPU-bound-spin gap: a plain PIT-only busy-spin
        // before the RTC's periodic-interrupt-enable bit is ever armed
        // (rtcCancelThread's only trigger condition) still won't get a
        // forced exit. Left as an accepted, documented gap rather than
        // re-broadening rtcCancelThread -- broadening it unconditionally
        // was already tried once for a different stall and made things
        // worse (see the "2026-07-17 update" section of that doc).
        ahciProcessPendingCommands(partition);
        // Backstop for the two event-driven flush points (RE enable, CAPR write):
        // if the driver freed space without touching either, deferred frames
        // still go out on the next iteration rather than sitting indefinitely.
        rtl8139FlushRxQueue(partition);
        // COM3 receive/transmit interrupts. Raised from this thread, not from
        // the pipe reader, because injection writes vCPU registers -- see
        // uart3UpdateIrq.
        uart3UpdateIrq(partition);
        netPollUdpSessions(partition);
        netPollTcpSessions(partition);

        // TEMP DIAGNOSTIC (disabled): the EFI checkpoint hooks below served
        // their purpose -- they empirically identified BlpTimeInitialize as
        // the function returning STATUS_IO_DEVICE_ERROR, which is now fixed
        // (see the cmosReadRtcField comment). Re-scanning 3GB of guest
        // memory on every "starting Boot0002" occurrence is expensive and,
        // once boot progresses past BlInitializeLibrary into later stages
        // this instrumentation wasn't designed for, caused a severe
        // slowdown (a single main-loop iteration taking 60+ real seconds).
        // Left in place (not deleted) in case this specific diagnostic
        // technique is useful again for a future boot blocker.
        if (0 && uefiMode && efiHookStartingBoot0002Count > efiHookScanAttempted) {
            efiHookScanAttempted = efiHookStartingBoot0002Count;
            tryInstallEfiHooks();
        }

        // TEMP DIAGNOSTIC (disabled): rtcDiagTryFindLoadBase served its
        // purpose -- it identified OVMF's own PcRtc driver as the source of
        // heavy post-fix CMOS polling. Left disabled now that investigation
        // is done: without a one-shot guard here it re-ran a full 3GB
        // memory scan on every single main-loop iteration whenever the
        // needle wasn't found (e.g. against a different Windows version's
        // boot manager build), a real, severe bug caught while testing
        // against Windows 10.
        static int rtcDiagScanAttempted = 0;
        if (0 && uefiMode && !rtcDiagScanAttempted && efiHookStartingBoot0002Count > 0) {
            rtcDiagScanAttempted = 1;
            rtcDiagTryFindLoadBase();
        }

        WHV_RUN_VP_EXIT_CONTEXT exitContext;
        {
            LARGE_INTEGER rs, re;
            QueryPerformanceCounter(&rs);
            hr = WHvRunVirtualProcessor(partition, 0, &exitContext, sizeof(exitContext));
            QueryPerformanceCounter(&re);
            // Wall time the vCPU was actually executing guest code, as opposed to
            // sitting halted in the loop above. Paired with g_haltedMs this says
            // whether the guest is slow or simply idle.
            if (perfFrequency.QuadPart)
                g_runningMs += (double)(re.QuadPart - rs.QuadPart) * 1000.0 / (double)perfFrequency.QuadPart;
        }
        if (FAILED(hr)) { printf("Failed to run vCPU. HRESULT: 0x%lx\n", hr); break; }

        // Recorded here, before any handler runs, so the ring holds what the
        // guest ASKED for rather than what we did about it -- and so nothing can
        // be missed by a handler that returns early.
        g_exitCountForDiag = exitCount;
        if (g_bootImgRunning) bootImgRingRecord(&exitContext, exitCount);

        // The CD/DVD prompt is up (appendToLog saw the guest print it) -- press a
        // key. SPACE, as a make/break pair, because the boot application polls the
        // i8042 output buffer directly and a make with no break leaves the key
        // stuck down for whatever reads it next.
        //
        // ONLY on the very first boot, and only before the guest has reset. That
        // prompt is a SAFETY MECHANISM, not an annoyance: its default is "do NOT
        // boot the CD". Windows Setup reboots the machine partway through the
        // install and must come back up on the DISK to continue -- if the ISO is
        // still attached and something answers the prompt again, Setup restarts
        // from the beginning instead, and the install can never finish. So answer
        // it to START an install, and never again for the life of the VM.
        //
        // g_sawReset is the guest actually resetting (port 0x64/0xFE), which is
        // exactly the mid-install reboot, so it distinguishes "first power-on"
        // from "came back around" without guessing from timing.
        // Also skipped when the disk already carries an OS -- otherwise launching
        // the VM after a successful install drops back into the INSTALLER instead
        // of booting what was installed. See the GPT probe where the disk is
        // attached. LOCALHOST_BOOT_ISO=1 forces the answer for a reinstall.
        if (!g_noAutoBootKey && g_autoBootKeySent == 0 && g_autoBootKeyWanted > 0
            && !g_sawReset && (!g_diskHasOs || g_forceBootIso)) {
            g_autoBootKeySent++;
            // Mark THIS prompt as accounted for, or the branch below immediately
            // reports the very prompt we just answered as a repeat.
            g_autoBootKeyReported = g_autoBootKeyWanted;
            kbEnqueue(0x39);
            kbEnqueue(0xB9);
            printf("[autokey] answered \"Press any key to boot from CD or DVD\" -- "
                   "FIRST BOOT ONLY. Any later prompt (a mid-install reboot) is left "
                   "unanswered so the guest boots the disk and Setup continues.\n");
            fflush(stdout);
        } else if (!g_noAutoBootKey && g_autoBootKeyWanted > g_autoBootKeyReported
                   && (g_autoBootKeySent > 0 || g_sawReset)) {
            // Say why we are deliberately staying quiet, so a boot that "ignores"
            // the prompt does not look like the auto-answer having broken.
            g_autoBootKeyReported = g_autoBootKeyWanted;
            printf("[autokey] CD/DVD prompt shown again (guest reset=%d) -- NOT answering; "
                   "booting from disk so an in-progress install can continue. "
                   "Press a key in the window within ~5s to boot the ISO instead.\n",
                   g_sawReset);
            fflush(stdout);
        }

        // U40: sample RIP. Free -- VpContext.Rip is already populated by the exit
        // above, so no extra WHP register read. Needs no arming, so unlike a
        // breakpoint it is valid from the first exit of each pass, which is the
        // whole point (see the U40 comment block above).
        {
            int u40pass = g_sawReset ? 1 : 0;
            static long g_nextRipSample = 0;
            static long g_nextRipDump = U40_DUMP_INTERVAL;
            if (exitCount >= g_nextRipSample) {
                g_nextRipSample = exitCount + (g_sawReset ? U40_SAMPLE_INTERVAL_P2 : U40_SAMPLE_INTERVAL);
                u40Sample(exitContext.VpContext.Rip, u40pass, exitCount);
            }
            if (exitCount >= g_nextRipDump) {
                g_nextRipDump = exitCount + U40_DUMP_INTERVAL;
                u40Dump(u40pass, "periodic");
            }
            // Dump the instant BDS reports a failed boot. The periodic cadence
            // is 250000 exits and a firmware-only boot produces roughly 135000,
            // so without this the histogram is never printed for the phase we
            // actually need to see.
            {
                static int lastBdsStart = 0, lastBdsFail = 0;
                // Bracket the boot image: clear on "starting", dump on "failed".
                // Without the reset the histogram is dominated by the firmware's
                // own console and PCI enumeration work from before the image ran.
                if (efiHookStartingBoot0002Count != lastBdsStart) {
                    lastBdsStart = efiHookStartingBoot0002Count;
                    u40Reset(u40pass);
                    printf("[u40] histogram reset -- profiling the boot image from here\n");
                    fflush(stdout);
                    // Scan BEFORE the image runs as well as after it fails.
                    // EDK2 unloads an image whose StartImage returned an error,
                    // so finding it absent at the failure proves nothing on its
                    // own -- present here but gone there means it WAS loaded and
                    // did run; absent in both means LoadImage never produced it.
                    u40ScanLargeImages("at BDS start, before StartImage");

                    // TRAP THE BOOT APPLICATION AT ITS ENTRY POINT.
                    //
                    // The RIP profile cannot see this image: it samples on VM
                    // exits and bootmgfw.efi does almost no I/O before returning
                    // EFI_TIMEOUT. A hardware execution breakpoint does not care
                    // about exits, so it catches the entry regardless -- and its
                    // absence would be just as informative.
                    //
                    // DR3 because DR0-DR2 are already spoken for by the ntoskrnl
                    // work; L3 is bit 6 of DR7.
                    if (g_bootImgEntry && !g_bootImgBpArmed) {
                        WHV_REGISTER_NAME bpn[2] = { WHvX64RegisterDr3, WHvX64RegisterDr7 };
                        WHV_REGISTER_VALUE bpv[2] = { 0 };
                        WHV_REGISTER_NAME dr7n = WHvX64RegisterDr7;
                        WHV_REGISTER_VALUE dr7v = { 0 };
                        WHvGetVirtualProcessorRegisters(partition, 0, &dr7n, 1, &dr7v);
                        bpv[0].Reg64 = g_bootImgEntry;
                        bpv[1].Reg64 = dr7v.Reg64 | 0x40ULL;   // L3
                        if (SUCCEEDED(WHvSetVirtualProcessorRegisters(partition, 0, bpn, 2, bpv))) {
                            g_bootImgBpArmed = 1;
                            printf("[bootimg] armed DR3 at entry 0x%llX (image 0x%llX, %uKB)\n",
                                   (unsigned long long)g_bootImgEntry,
                                   (unsigned long long)g_bootImgBase, g_bootImgSize / 1024);
                            fflush(stdout);
                        }
                    }
                }
                if (g_bdsBootFailed != lastBdsFail) {
                    lastBdsFail = g_bdsBootFailed;
                    u40Dump(u40pass, "BDS reported a failed boot");
                    // Was the boot application even loaded? Independent of the
                    // exit-sampled profile above, which cannot see code that
                    // does no I/O.
                    u40ScanLargeImages("at BDS failure");
                }
            }
        }

        // Opportunistic breakpoint-site discovery: as soon as RIP looks
        // like a plausible canonical kernel address and we haven't
        // already located+patched this boot instance's ntoskrnl.exe,
        // try the same backward MZ/PE scan used throughout this
        // investigation. Throttled to once per ~200ms (the scan is a
        // bounded but non-trivial page-table walk) so it can't add
        // meaningful overhead once the guest is executing real kernel
        // code, while still finding the module within a fraction of a
        // second of it starting to run.
        if (!g_wpArmed && exitContext.VpContext.Rip >= 0xFFFF800000000000ULL) {
            static LARGE_INTEGER lastBpDiscoveryAttempt = { 0 };
            LARGE_INTEGER bpNow;
            QueryPerformanceCounter(&bpNow);
            double sinceLastMs = (lastBpDiscoveryAttempt.QuadPart == 0) ? 1e9 :
                (double)(bpNow.QuadPart - lastBpDiscoveryAttempt.QuadPart) * 1000.0 / perfFrequency.QuadPart;
            // U33b: the 200ms throttle is fine on pass 1 but it caps how early
            // the U33 pool-descriptor poll can start on pass 2, because that poll
            // needs g_bpModuleBase and so cannot run until discovery succeeds.
            // Run 1 discovered the pass-2 kernel at exitCount 857744, leaving a
            // ~200ms (~10k exit) blind window after the pass-2 kernel began
            // executing -- and on pass 1 the descriptor init fires only ~10k
            // exits after discovery, i.e. exactly inside a window that size. So a
            // set-then-cleared sequence could hide there. Tighten the throttle
            // once a reset has been seen to shrink that window ~20x.
            if (sinceLastMs >= (g_sawReset ? 10.0 : 200.0)) {
                lastBpDiscoveryAttempt = bpNow;
                WHV_REGISTER_NAME cr3Name = WHvX64RegisterCr3;
                WHV_REGISTER_VALUE cr3Val = { 0 };
                if (SUCCEEDED(WHvGetVirtualProcessorRegisters(partition, 0, &cr3Name, 1, &cr3Val)) && cr3Val.Reg64 != 0) {
                    UINT64 candBase = 0;
                    if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, cr3Val.Reg64, exitContext.VpContext.Rip, &candBase)) {
                        g_bpModuleBase = candBase;
                        g_wpTargetVA = g_bpModuleBase + BP_FAIL_SITE_RVA;
                        g_wpTargetVA0 = g_bpModuleBase + WP_RETURN_RVA;
                        g_wpTargetVA1 = g_bpModuleBase + WP_PHASE1INIT_ENTRY_RVA;
                        g_haliDispatchTargetVA = g_bpModuleBase + HALI_DISPATCH_CALL_RVA;
                        // Four simultaneous hardware EXECUTION breakpoints
                        // (DR0/DR1/DR2/DR3, R/W=00 each) instead of an INT3
                        // patch -- see the exceptionExitBitmap comment
                        // above for why. LEN must be 00 (1 byte) for
                        // instruction breakpoints per architecture. DR7
                        // layout: L0=bit0, L1=bit2, L2=bit4, L3=bit6,
                        // reserved bit10=1; R/W/LEN fields for all four
                        // stay 0 (execute, 1 byte).
                        WHV_REGISTER_NAME wpNames[5] = { WHvX64RegisterDr0, WHvX64RegisterDr1, WHvX64RegisterDr2, WHvX64RegisterDr3, WHvX64RegisterDr7 };
                        WHV_REGISTER_VALUE wpVals[5] = { 0 };
                        wpVals[0].Reg64 = g_wpTargetVA0;
                        wpVals[1].Reg64 = g_wpTargetVA1;
                        wpVals[2].Reg64 = g_wpTargetVA;
                        wpVals[3].Reg64 = g_haliDispatchTargetVA;
                        // U28: DR2 (L2) is NOT armed at discovery -- the hot
                        // GetSubsegment site is armed later, only inside the narrow
                        // pre-crash exitCount window (see windowed-arm block below).
                        // U40: L3 (DR3 at HalpIommuInitSystem's ret) deliberately NOT
                        // set any more. That site is inside the very HAL code the RIP
                        // sampler needs to observe, and per the U21 note an exec
                        // breakpoint on an executed instruction re-faults on resume --
                        // which pinned 30 of pass 2's ~30 kernel samples at
                        // HalpIommuInitSystem+0x1DF in the first U40 run. That tail was
                        // our own instrument, not guest behaviour. L0 (U34 site) and
                        // L1 (KeBugCheckEx) only.
                        // U65: L0 (DR0) no longer armed. It still points at
                        // KiSwInterruptDispatch from U46b, whose question was
                        // answered long ago (U47 fixed the pass-2 0x139 by
                        // quiescing the LAPIC). An armed exec breakpoint on a
                        // function the guest actually executes costs an exit per
                        // hit and, per the U21 note, re-faults on resume. L1
                        // (KeBugCheckEx) stays -- it is one-shot, never hit on a
                        // healthy boot, and is how we still detect bugchecks.
                        wpVals[4].Reg64 = 0x4ULL | (1ULL << 10);
                        if (SUCCEEDED(WHvSetVirtualProcessorRegisters(partition, 0, wpNames, 5, wpVals))) {
                            g_wpArmed = 1;
                            g_wp0Fired = 0;
                            g_wp1Fired = 0;
                            g_wp2Fired = 0;
                            g_haliDispatchArmed = 1;
                            g_haliDispatchHitCount = 0;
                            g_poolCallerArmed = 0; // U65: DR0 site retired (see the DR7 comment above)
                            g_poolCallerHitCount = 0;
                            g_helperEntryArmed = 1;
                            g_helperEntryHitCount = 0;
                            g_loopAdvArmed = 0; // U28: armed later, in-window
                            g_loopAdvHitCount = 0;
                            // U33b: exitCount here dates the start of the U33 poll's
                            // validity, which run 1 could only infer from nearby
                            // heartbeats. Needed to state the blind window exactly.
                            printf("[wp] DISCOVERY at exitCount=%ld (pass %s)\n", exitCount, g_sawReset ? "2" : "1");
                            u49VerifyAcpiTables((unsigned char *)guestMemory, guestMemSize); // U49: is our DSDT really there?
                            g_passModuleBase[g_sawReset ? 1 : 0] = g_bpModuleBase; // U40: for RVA-relative histogram output
                            printf("[wp] armed breakpoints at module base 0x%llX (DR2 LFH-check armed=%d): DR0=+0x%X (U34 ExInitializePoolHeapManagement entry, VA=0x%llX), DR1=+0x%X (KeBugCheckEx, VA=0x%llX), DR2=+0x%X (RtlpHpLfhOwnerMoveSubsegment entry, VA=0x%llX), DR3=+0x%X (HalpIommuInitSystem ret, VA=0x%llX)\n",
                                   (unsigned long long)g_bpModuleBase, g_sawReset ? 1 : 0,
                                   WP_RETURN_RVA, (unsigned long long)g_wpTargetVA0,
                                   WP_PHASE1INIT_ENTRY_RVA, (unsigned long long)g_wpTargetVA1,
                                   BP_FAIL_SITE_RVA, (unsigned long long)g_wpTargetVA,
                                   HALI_DISPATCH_CALL_RVA, (unsigned long long)g_haliDispatchTargetVA);
                            fflush(stdout);
                            // U32: dump ExPoolState pool-descriptor +0x08 and +0x10
                            // fields for pool types 0..7, once per pass, to confirm
                            // the init gap: +0x10 (the heap-context root) is expected
                            // NON-NULL on pass 1 but NULL on pass 2. ExPoolState RVA
                            // 0xC545C0; descriptor table +0x3900 (RVA 0xC57EC0),
                            // stride 0x20c0. +0x08 has a known writer
                            // (ExInitializePoolHeapManagement); +0x10 is the suspect.
                            {
                                printf("[u32] ExPoolState pool-descriptor fields (pass %s):\n", g_sawReset ? "2" : "1");
                                int pi;
                                for (pi = 0; pi < 8; pi++) {
                                    UINT64 descRva = 0xC57EC0ULL + (UINT64)pi * 0x20c0ULL;
                                    unsigned char fld[0x18] = { 0 };
                                    if (kernelReadVA((unsigned char *)guestMemory, cr3Val.Reg64, g_bpModuleBase + descRva, fld, sizeof(fld))) {
                                        UINT64 f08 = *(UINT64 *)&fld[0x08], f10 = *(UINT64 *)&fld[0x10];
                                        printf("[u32]   pool[%d] desc@RVA0x%llX: +0x08=0x%llX +0x10=0x%llX%s\n",
                                               pi, (unsigned long long)descRva, (unsigned long long)f08, (unsigned long long)f10,
                                               (f10 == 0) ? "  <== +0x10 NULL" : "");
                                    }
                                }
                                fflush(stdout);
                            }
                            // U27: one-shot full-kernel image dump (scratchpad
                            // ntoskrnl_dump.bin was purged; re-capture for offline
                            // disasm). Read base..base+0x1000000 page-by-page and
                            // write incrementally. Runs once per process.
                            static int g_kernelDumped = 0;
                            if (!g_kernelDumped) {
                                g_kernelDumped = 1;
                                const char *dpath = KDUMP_PATH;
                                CreateDirectoryA(KDUMP_DIR, NULL); // harmless if it already exists
                                FILE *df = fopen(dpath, "wb");
                                if (!df) {
                                    // Previously this failure was silent, and because the
                                    // old path pointed into a purged session scratchpad it
                                    // failed EVERY run -- the dump simply never reappeared.
                                    printf("[u27] FAILED to open %s for the kernel dump (errno %d) -- offline disasm will be unavailable\n",
                                           dpath, errno);
                                    fflush(stdout);
                                }
                                if (df) {
                                    unsigned char page[0x1000];
                                    UINT64 off; int okPages = 0;
                                    for (off = 0; off < 0x1000000ULL; off += 0x1000) {
                                        if (kernelReadVA((unsigned char *)guestMemory, cr3Val.Reg64, g_bpModuleBase + off, page, sizeof(page)))
                                            okPages++;
                                        else
                                            memset(page, 0, sizeof(page));
                                        fwrite(page, 1, sizeof(page), df);
                                    }
                                    fclose(df);
                                    printf("[u27] dumped kernel image to ntoskrnl_dump.bin (%d/%d pages readable)\n", okPages, 0x1000);
                                    fflush(stdout);
                                }
                            }
                        } else {
                            printf("[wp] failed to arm execution breakpoints -- will retry on next plausible RIP\n");
                            fflush(stdout);
                        }
                    }
                }
            }
        }

        // U33: edge-triggered tracker for the ExPoolState pool-descriptor
        // +0x08/+0x10 fields, replacing U32's every-200k unconditional dump.
        // U32 already proved the gap; the open question is WHICH of the two
        // mechanisms produces it, and 200k-exit granularity cannot tell them
        // apart. See the U33 comment block near GETSUB_RVA for why this is a
        // memory poll rather than a breakpoint on the (single) writer.
        //
        // Reading these fields costs a CR3 fetch + a page-table walk per pool,
        // so poll on an interval but only PRINT on a transition. Each edge is
        // reported with both absolute exitCount and, on pass 2, the offset from
        // the reset -- the reset-relative number is the one comparable across
        // runs, since the crash exitCount itself varies run-to-run (U28).
        if (g_bpModuleBase) {
            static long g_nextPoolPollEC = 0;
            static UINT64 g_lastF08[U33_POOLS], g_lastF10[U33_POOLS];
            // Per-pool, NOT shared: a descriptor page can be unmapped on an early
            // poll, and a shared flag would then mislabel that pool's first
            // successful read as an INIT edge from a baseline it never actually
            // observed. Each pool is primed only by its own first good read.
            static int g_poolPrimed[U33_POOLS];
            static int g_poolPollPass = 0;     /* pass the baselines belong to */
            static int g_poolEdges = 0;
            static int g_anyPrimed = 0;
            int curPass = g_sawReset ? 2 : 1;
            // Re-baseline at the pass boundary: pass 2 runs a freshly KASLR'd
            // kernel, so pass-1 values are not a meaningful comparison point and
            // carrying them over would fake an edge at the first pass-2 poll.
            if (g_anyPrimed && curPass != g_poolPollPass) {
                int rb;
                for (rb = 0; rb < U33_POOLS; rb++) g_poolPrimed[rb] = 0;
                g_anyPrimed = 0;
                g_poolEdges = 0;
                printf("[u33] pass boundary -- re-baselining pool-descriptor tracker for pass %d\n", curPass);
                fflush(stdout);
            }
            if (exitCount >= g_nextPoolPollEC && g_poolEdges < U33_MAX_EDGES) {
                g_nextPoolPollEC = exitCount + U33_POLL_INTERVAL;
                WHV_REGISTER_NAME c3n = WHvX64RegisterCr3; WHV_REGISTER_VALUE c3v = { 0 };
                if (SUCCEEDED(WHvGetVirtualProcessorRegisters(partition, 0, &c3n, 1, &c3v)) && c3v.Reg64) {
                    int pi;
                    for (pi = 0; pi < U33_POOLS; pi++) {
                        UINT64 descVA = g_bpModuleBase + EXPOOLSTATE_DESC_RVA + (UINT64)pi * EXPOOLSTATE_DESC_STRIDE;
                        unsigned char fld[0x18] = { 0 };
                        if (!kernelReadVA((unsigned char *)guestMemory, c3v.Reg64, descVA, fld, sizeof(fld)))
                            continue; // not mapped yet -- normal very early in a pass
                        UINT64 f08 = *(UINT64 *)&fld[0x08], f10 = *(UINT64 *)&fld[0x10];
                        if (g_poolPrimed[pi] && f08 == g_lastF08[pi] && f10 == g_lastF10[pi])
                            continue; // unchanged -- the overwhelmingly common case
                        if (!g_poolPrimed[pi]) {
                            printf("[u33] baseline pass=%d exitCount=%ld pool[%d] +0x08=0x%llX +0x10=0x%llX\n",
                                   curPass, exitCount, pi,
                                   (unsigned long long)f08, (unsigned long long)f10);
                        } else {
                            // Classify each field's edge. null->nonnull = the init
                            // running; nonnull->null = an explicit clear, which is
                            // the hypothesis U33 exists to confirm or kill.
                            const char *e08 = (g_lastF08[pi] == 0 && f08 != 0) ? " +0x08 INIT(null->set)" :
                                              (g_lastF08[pi] != 0 && f08 == 0) ? " +0x08 CLEARED(set->null)" : "";
                            const char *e10 = (g_lastF10[pi] == 0 && f10 != 0) ? " +0x10 INIT(null->set)" :
                                              (g_lastF10[pi] != 0 && f10 == 0) ? " +0x10 CLEARED(set->null)" : "";
                            printf("[u33] EDGE pass=%d exitCount=%ld", curPass, exitCount);
                            if (curPass == 2 && g_resetExitCount)
                                printf(" (reset+%ld)", exitCount - g_resetExitCount);
                            printf(" pool[%d]: +0x08 0x%llX->0x%llX +0x10 0x%llX->0x%llX%s%s\n",
                                   pi,
                                   (unsigned long long)g_lastF08[pi], (unsigned long long)f08,
                                   (unsigned long long)g_lastF10[pi], (unsigned long long)f10,
                                   e08, e10);
                            g_poolEdges++;
                        }
                        g_lastF08[pi] = f08;
                        g_lastF10[pi] = f10;
                        g_poolPrimed[pi] = 1;
                        g_anyPrimed = 1;
                        fflush(stdout);
                    }
                    g_poolPollPass = curPass;
                }
            }
        }

        // U28: windowed arming of the hot GetSubsegment capture (DR2). Once pass 2
        // has run GETSUB_WINDOW_START exits past the reset (approaching the ~crash),
        // point DR2 at GetSubsegment+0x45 and set L2. This bounds the hot-site
        // overhead to the final pre-crash window while still containing the fatal
        // (already-corrupt) subsegment call.
        // U34: disabled. This armed a HOT site (U28 measured 3.4M calls) purely to
        // recover the null-owner subsegment, a question U29-U33 have since answered.
        // Keeping it armed only adds timing distortion to the pass-2 window that U34
        // now needs to observe cleanly. Set to 1 to re-enable if that capture is
        // ever wanted again.
#define U28_GETSUB_WINDOW_ENABLED 0
        if (U28_GETSUB_WINDOW_ENABLED &&
            g_wpArmed && g_sawReset && !g_getsubWindowArmed && g_bpModuleBase &&
            g_resetExitCount && exitCount > g_resetExitCount + GETSUB_WINDOW_START) {
            g_wpTargetVA = g_bpModuleBase + GETSUB_RVA;
            WHV_REGISTER_NAME awNames[2] = { WHvX64RegisterDr2, WHvX64RegisterDr7 };
            WHV_REGISTER_VALUE awVals[2] = { 0 };
            WHV_REGISTER_NAME dr7rn = WHvX64RegisterDr7; WHV_REGISTER_VALUE dr7rv = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, &dr7rn, 1, &dr7rv);
            awVals[0].Reg64 = g_wpTargetVA;
            awVals[1].Reg64 = dr7rv.Reg64 | 0x10ULL; // set L2
            if (SUCCEEDED(WHvSetVirtualProcessorRegisters(partition, 0, awNames, 2, awVals))) {
                g_getsubWindowArmed = 1;
                g_loopAdvArmed = 1;
                g_loopAdvHitCount = 0;
                printf("[u28] windowed GetSubsegment capture armed at exitCount=%ld (reset+%ld), VA=0x%llX\n",
                       exitCount, exitCount - g_resetExitCount, (unsigned long long)g_wpTargetVA);
                fflush(stdout);
            }
        }

        // U113: attribute the exits. The guest sits at ~9000 exits/sec while
        // IDLE, which is the number that silently broke the heartbeat's
        // exit-count trigger (tuned when it was ~64/sec) and is the most likely
        // reason the guest is CPU-saturated and slow to redraw its own cursor.
        // "How many exits" has never been broken down by CAUSE, so there is
        // nothing to aim a fix at. Count by reason, and for I/O by port, so the
        // top consumer can simply be read off.
        {
            // Map the reason to a dense slot. NOT a mask: the WHP values are not
            // contiguous -- Cpuid is 0x1001 and Exception 0x1002, so masking with
            // &15 aliases them onto MemoryAccess and IoPort and silently reports
            // identical counts for unrelated causes. (It did exactly that here.)
            UINT32 r;
            switch (exitContext.ExitReason) {
                case WHvRunVpExitReasonMemoryAccess:    r = 0; break;
                case WHvRunVpExitReasonX64IoPortAccess: r = 1; break;
                case WHvRunVpExitReasonX64Halt:         r = 2; break;
                case WHvRunVpExitReasonX64Cpuid:        r = 3; break;
                case WHvRunVpExitReasonException:       r = 4; break;
                case WHvRunVpExitReasonCanceled:        r = 5; break;
                case WHvRunVpExitReasonX64MsrAccess:    r = 6; break;
                default:                                r = 7; break;
            }
            g_exitReasonCount[r]++;
            if (exitContext.ExitReason == WHvRunVpExitReasonX64IoPortAccess) {
                if (g_ioPortHist) g_ioPortHist[exitContext.IoPortAccess.PortNumber & 0xFFFF]++;
            } else if (exitContext.ExitReason == WHvRunVpExitReasonMemoryAccess) {
                // Bucket MMIO by 4KB page; the address itself is too sparse to
                // histogram and the page is what identifies the device.
                UINT64 pg = exitContext.MemoryAccess.Gpa >> 12;
                int k;
                for (k = 0; k < 8; k++) {
                    if (g_mmioPage[k] == pg) { g_mmioPageHits[k]++; break; }
                    if (g_mmioPageHits[k] == 0) { g_mmioPage[k] = pg; g_mmioPageHits[k] = 1; break; }
                }
                // U114: WHICH register. ~10000 accesses/sec to serve a 62Hz
                // tablet is ~160 reads per useful report, which looks like a
                // spin rather than normal polling -- and a spin has a specific
                // register at the bottom of it. EHCI's operational registers all
                // live in the first 256 bytes, so a byte-offset histogram over
                // the busiest page names it directly.
                if (ehciBarBase && (exitContext.MemoryAccess.Gpa >> 12) == ((UINT64)ehciBarBase >> 12))
                    g_ehciOffHist[exitContext.MemoryAccess.Gpa & 0xFF]++;
            }
        }
        switch (exitContext.ExitReason) {
            case WHvRunVpExitReasonX64Halt: {
                // Don't stop the hypervisor -- park the CPU and wait for an
                // interrupt. The wait loop at the top of the outer while()
                // will wake it up.
                WHV_REGISTER_NAME ripNameH = WHvX64RegisterRip;
                WHV_REGISTER_VALUE ripValH = { 0 };
                WHvGetVirtualProcessorRegisters(partition, 0, &ripNameH, 1, &ripValH);
                if (g_bpPatched) logBpEvent("halt rip=0x%llX", (unsigned long long)ripValH.Reg64);
                printf("[CPU halted at rip=0x%llX -- waiting for interrupt]\n",
                       (unsigned long long)ripValH.Reg64);
                fflush(stdout);
                cpuHalted = 1;
                break;
            }

            case WHvRunVpExitReasonX64IoPortAccess: {
                UINT16 port = exitContext.IoPortAccess.PortNumber;
                BOOL isWrite = exitContext.IoPortAccess.AccessInfo.IsWrite;
                debugLastPort = port;
                debugLastWrite = isWrite;

                if (g_bpPatched) {
                    logBpEvent("io port=0x%03X write=%d val=0x%llX", port, isWrite,
                               (unsigned long long)exitContext.IoPortAccess.Rax);
                }

                // Everything the BOOT APPLICATION touches, and nothing else. The
                // window between its entry point and its return is ~2960 exits,
                // small enough to read in full -- where logging a whole boot is
                // not. Serial output (0x3F8-0x3FF) is skipped: it is the image
                // and firmware printing, which says nothing about what it wants.
                if (g_bootImgRunning) bootImgCountPort(port);
                if (g_bootImgRunning && g_bootImgIoLogged < BOOTIMG_IO_LOG_MAX &&
                    !(port >= 0x3F8 && port <= 0x3FF)) {
                    g_bootImgIoLogged++;
                    printf("[bootimg-io] %s port=0x%03X size=%u val=0x%llX rip=0x%llX\n",
                           isWrite ? "OUT" : "IN ", port,
                           exitContext.IoPortAccess.AccessInfo.AccessSize,
                           (unsigned long long)exitContext.IoPortAccess.Rax,
                           (unsigned long long)exitContext.VpContext.Rip);
                }

                if (port == 0x1F0) {
                    ataHandlePioDataPort(partition, &exitContext, guestMemory);
                    break;
                }

                if (port == 0xCF8 || (port >= 0xCFC && port <= 0xCFF)) {
                    pciHandleConfigAccess(partition, &exitContext);
                    break;
                }

                if (port >= 0x3F8 && port <= 0x3FF) {
                    uartHandleAccess(partition, &exitContext);
                    break;
                }

                if (port >= 0x2F8 && port <= 0x2FF) {
                    uart2HandleAccess(partition, &exitContext);
                    break;
                }

                if (port >= 0x3E8 && port <= 0x3EF) {
                    uart3HandleAccess(partition, &exitContext);
                    break;
                }

                if (port == 0xE2) {
                    efiHookHandleAccess(partition, &exitContext);
                    break;
                }

                if (port == 0x510 || port == 0x511) {
                    fwCfgHandleAccess(partition, &exitContext, guestMemory);
                    break;
                }

                if (e1000IoBase != 0) {
                    UINT32 ebase = e1000IoBase & ~3u;
                    if (port >= ebase && port < ebase + E1000_IO_SIZE) {
                        e1000HandleIoAccess(partition, &exitContext);
                        break;
                    }
                }

                // RTL8139 register file, at whatever I/O base the guest
                // programmed into BAR0 (see rtl8139HandleBar0Access). Same
                // dynamic-base pattern as the PM timer below, since real
                // firmware/OS PCI enumeration assigns this at boot rather
                // than it being a fixed port.
                if (rtl8139IoBase != 0) {
                    UINT32 base = rtl8139IoBase & ~0x3;
                    if (port >= base && port < base + RTL8139_IO_SIZE) {
                        rtl8139HandleIoAccess(partition, &exitContext);
                        break;
                    }
                }

                // ACPI PM Timer: PIIX4's PM I/O block base is whatever the
                // guest programs into the PM function's PMBA register
                // (pciPmConfig offset 0x40, 64-byte aligned), and the timer
                // register (PM_TMR_BLK) conventionally lives at base+8.
                // Before the PM function's config space was writable, this
                // showed up as a fixed, PCD-loaded port (which happened to
                // be plain "6") instead of a real dynamically-assigned one
                // -- same underlying device, just discovered differently
                // now that BAR programming actually sticks. It's a real
                // 32-bit free-running counter at 3.579545MHz; returning
                // only the low 16 bits wraps every ~18ms and can fail a
                // delta/wraparound sanity check on every single read.
                {
                    UINT32 pmBase = (*(UINT32 *)&pciPmConfig[0x40]) & 0xFFC0;
                    if (pmBase != 0 && port == (UINT16)(pmBase + 8) &&
                        !exitContext.IoPortAccess.AccessInfo.IsWrite) {
                        LARGE_INTEGER now;
                        QueryPerformanceCounter(&now);
                        double pmTicks = (double)now.QuadPart * (3579545.0 / (double)perfFrequency.QuadPart);
                        UINT32 pmValue = (UINT32)((UINT64)pmTicks & 0xFFFFFFFFULL);

                        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
                        WHV_REGISTER_VALUE values[2] = { 0 };
                        values[0].Reg64 = pmValue;
                        values[1].Reg64 = exitContext.VpContext.Rip + exitContext.VpContext.InstructionLength;
                        WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
                        break;
                    }
                }

                // SMI_CMD (port 0xB2, PIIX4's fixed default) and PM1a_CNT_BLK
                // (pmBase+4): a BIOS's acpi_enable() writes SMI_CMD to ask an
                // SMI handler to set SCI_EN in PM1a_CNT, then polls PM1a_CNT
                // for that bit. We don't emulate SMM, so the write to 0xB2
                // sets SCI_EN synchronously instead -- see pm1aControl's
                // declaration for why.
                if (port == 0xB2 && isWrite) {
                    pm1aControl |= 0x1;
                    smiStatus = 0;
                    WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
                    WHV_REGISTER_VALUE newRip = { 0 };
                    newRip.Reg64 = exitContext.VpContext.Rip + exitContext.VpContext.InstructionLength;
                    WHvSetVirtualProcessorRegisters(partition, 0, &ripName, 1, &newRip);
                    break;
                }
                if (port == 0xB3) {
                    UINT64 rax = smiStatus;
                    if (isWrite) smiStatus = (unsigned char)exitContext.IoPortAccess.Rax;
                    WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
                    WHV_REGISTER_VALUE values[2] = { 0 };
                    values[0].Reg64 = isWrite ? 0 : rax;
                    values[1].Reg64 = exitContext.VpContext.Rip + exitContext.VpContext.InstructionLength;
                    WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
                    break;
                }
                // PM1a_EVT_BLK: PM1a_STS (pmBase+0) and PM1a_EN (pmBase+2).
                // Accessed as two 16-bit registers or as one 32-bit read of the
                // pair, both of which real OSes do, so both are handled here.
                // PM1a_STS is WRITE-1-TO-CLEAR, not a plain store: writing the
                // bit back is how an OS acknowledges the event, and treating it
                // as a normal write would make the guest unable to ever clear
                // PWRBTN_STS -- leaving the SCI asserted forever.
                {
                    UINT32 pmBase = (*(UINT32 *)&pciPmConfig[0x40]) & 0xFFC0;
                    if (pmBase != 0 && (port == (UINT16)(pmBase + 0) || port == (UINT16)(pmBase + 2))) {
                        int isStatus = (port == (UINT16)(pmBase + 0));
                        int is32 = (exitContext.IoPortAccess.AccessInfo.AccessSize == 4);
                        UINT64 rax;
                        if (isStatus)
                            rax = is32 ? ((UINT32)pm1aStatus | ((UINT32)pm1aEnable << 16)) : pm1aStatus;
                        else
                            rax = pm1aEnable;
                        if (isWrite) {
                            UINT32 v = (UINT32)exitContext.IoPortAccess.Rax;
                            if (isStatus) {
                                pm1aStatus &= (UINT16)~(UINT16)v;      // write-1-to-clear
                                if (is32) pm1aEnable = (UINT16)(v >> 16);
                            } else {
                                pm1aEnable = (UINT16)v;
                            }
                        }
                        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
                        WHV_REGISTER_VALUE values[2] = { 0 };
                        values[0].Reg64 = isWrite ? 0 : rax;
                        values[1].Reg64 = exitContext.VpContext.Rip + exitContext.VpContext.InstructionLength;
                        WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
                        break;
                    }
                }
                {
                    UINT32 pmBase = (*(UINT32 *)&pciPmConfig[0x40]) & 0xFFC0;
                    if (pmBase != 0 && port == (UINT16)(pmBase + 4)) {
                        UINT64 rax = pm1aControl;
                        // SLP_TYP/SLP_EN (bits 10-13) are the only fields real
                        // POST code legitimately writes here before an OS is
                        // even loaded (confirmed live: SeaBIOS writes 0x2000 --
                        // SLP_EN with SLP_TYP=0, i.e. a harmless S0 no-op, not
                        // an actual sleep request). A blind overwrite clobbers
                        // SCI_EN (bit0) back to 0, and since our FADT declares
                        // SMI_CMD=0 ("ACPI already enabled, no handshake"),
                        // nothing will ever write 0xB2 again to re-set it --
                        // any later SCI_EN poll then spins forever (confirmed
                        // live: SeaBIOS hangs at its own PM1a_CNT init read
                        // immediately after this write). Pin SCI_EN through
                        // writes to keep it consistent with that guarantee.
                        if (isWrite) {
                            UINT16 v = (UINT16)exitContext.IoPortAccess.Rax;
                            // SLP_EN with SLP_TYP=5 is the guest saying it has
                            // finished shutting down (\_S5 in our DSDT declares
                            // SLP_TYP 0x05). This is the ONLY safe moment to
                            // exit: the filesystem is flushed and consistent.
                            // SLP_TYP=0 with SLP_EN is a harmless S0 no-op that
                            // POST code writes -- see the note above -- so the
                            // type has to be checked, not just SLP_EN.
                            if ((v & PM1_SLP_EN) && (((v >> 10) & 0x7) == 5)) {
                                printf("[acpi] guest requested S5 (soft off) -- shutting down cleanly\n");
                                fflush(stdout);
                                InterlockedExchange(&g_guestPoweredOff, 1);
                            }
                            pm1aControl = v | 0x1;
                        }
                        WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
                        WHV_REGISTER_VALUE values[2] = { 0 };
                        values[0].Reg64 = isWrite ? 0 : rax;
                        values[1].Reg64 = exitContext.VpContext.Rip + exitContext.VpContext.InstructionLength;
                        WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
                        break;
                    }
                }

                if (isWrite) {
                    unsigned char val = (unsigned char)exitContext.IoPortAccess.Rax;
                    debugLastVal = val;
                    int skipRipAdvance = 0;

                    // Keyboard-controller reset pulse (byte 0xFE to port
                    // 0x64): the classic legacy "reset the CPU" mechanism,
                    // still used as a HAL fallback by modern Windows (e.g.
                    // Setup's mid-installation restart). Confirmed live
                    // (see docs/investigations/post-vppt-boot-stall.md)
                    // that this guest genuinely sends this pulse and then
                    // deadlocks forever waiting for a reset that never
                    // happened, because this hypervisor previously did
                    // nothing for it.
                    //
                    // Two things are needed to honor it correctly, both
                    // confirmed necessary by direct experiment:
                    //
                    // 1. Reset CPU state to the same reset-vector state
                    //    used at cold boot (RIP/CS, matching the existing
                    //    port-0x92 hot-reset mechanism) -- plus CR0/CR3/
                    //    CR4/EFER, since this fires while the guest is deep
                    //    in 64-bit long mode, where CS.Base is architecturally
                    //    ignored for addressing. Without also exiting long
                    //    mode, the RIP/CS reset alone lands at flat linear
                    //    address 0xFFF0 under the guest's still-active page
                    //    tables, not the real reset vector.
                    //
                    // 2. Clear guest RAM above the legacy 16MB boundary.
                    //    Without this, the guest reboots successfully but
                    //    then bugchecks (0x139 KERNEL_SECURITY_CHECK_FAILURE,
                    //    traced to a corrupted LIST_ENTRY inside
                    //    RtlpHpLfhOwnerMoveSubsegment -- the kernel's
                    //    Segment Heap) because the freshly-restarted
                    //    kernel's memory manager finds stale, partially-
                    //    modified heap bookkeeping left behind by the
                    //    aborted prior boot instance in the same physical
                    //    memory. Two independent falsification tests
                    //    (clearing everything, then clearing only above
                    //    16MB) confirmed this is specifically OS-managed
                    //    extended memory, not low/legacy memory -- see the
                    //    investigation doc for the full trace. Real
                    //    hardware's warm reset doesn't clear RAM and
                    //    doesn't hit this, so this is a known, deliberate
                    //    divergence from real hardware, not an attempt at
                    //    full architectural accuracy -- it's what this
                    //    guest's boot path actually needs to get past this
                    //    point.
                    if (port == 0x64 && val == 0xFE) {
                        // Beyond RIP/CS/CR0/CR3/CR4/EFER (already proven
                        // necessary and sufficient to get the guest
                        // restarting), real x86 RESET# also reinitializes
                        // IDTR/GDTR/LDTR/TR, DR7, and the non-CS segment
                        // registers -- none of which this reset has ever
                        // touched. Not yet known whether any of these are
                        // relevant to the still-unsolved bugcheck 0x139
                        // (Segment Heap corruption, see
                        // docs/investigations/post-vppt-boot-stall.md),
                        // but they're genuine, real, well-defined gaps
                        // versus documented CPU reset semantics -- unlike
                        // IOAPIC state (platform/chipset, not CPU
                        // architectural, and already proven harmful to
                        // reset) or RAM content (proven not reliably
                        // relevant despite four attempts). Worth testing
                        // before returning to guessing at heap internals.
                        WHV_REGISTER_NAME resetNames[15] = {
                            WHvX64RegisterRip, WHvX64RegisterCs,
                            WHvX64RegisterCr0, WHvX64RegisterCr3,
                            WHvX64RegisterCr4, WHvX64RegisterEfer,
                            WHvX64RegisterIdtr, WHvX64RegisterGdtr,
                            WHvX64RegisterLdtr, WHvX64RegisterTr,
                            WHvX64RegisterDr7,
                            WHvX64RegisterDs, WHvX64RegisterEs,
                            WHvX64RegisterFs, WHvX64RegisterGs
                        };
                        WHV_REGISTER_VALUE resetValues[15] = { 0 };
                        resetValues[0].Reg64 = 0xFFF0;
                        WHV_X64_SEGMENT_REGISTER resetCs = { 0 };
                        resetCs.Base = uefiMode ? 0xFFFF0000 : 0xF0000;
                        resetCs.Limit = 0xFFFF;
                        resetCs.Selector = 0xF000;
                        resetCs.Attributes = 0x9B;
                        resetValues[1].Segment = resetCs;
                        resetValues[2].Reg64 = 0x60000010; // CR0 reset value (PE=0, PG=0)
                        resetValues[3].Reg64 = 0;           // CR3
                        resetValues[4].Reg64 = 0;           // CR4
                        resetValues[5].Reg64 = 0;           // EFER (clears LME/LMA)
                        resetValues[6].Table.Base = 0;
                        resetValues[6].Table.Limit = 0x3FF; // IDTR: real-mode IVT size
                        resetValues[7].Table.Base = 0;
                        resetValues[7].Table.Limit = 0xFFFF; // GDTR
                        WHV_X64_SEGMENT_REGISTER resetLdtr = { 0 };
                        resetLdtr.Base = 0;
                        resetLdtr.Limit = 0xFFFF;
                        resetLdtr.Selector = 0;
                        resetLdtr.Attributes = 0x82; // system segment, type=2 (LDT descriptor), present
                        resetValues[8].Segment = resetLdtr;  // LDTR
                        WHV_X64_SEGMENT_REGISTER resetTr = { 0 };
                        resetTr.Base = 0;
                        resetTr.Limit = 0xFFFF;
                        resetTr.Selector = 0;
                        resetTr.Attributes = 0x8B; // system segment, type=0xB (busy 64-bit TSS), present -- WHV rejects type=2 here (not a valid TSS type)
                        resetValues[9].Segment = resetTr;    // TR
                        resetValues[10].Reg64 = 0;           // DR7
                        WHV_X64_SEGMENT_REGISTER resetData = { 0 };
                        resetData.Base = 0;
                        resetData.Limit = 0xFFFF;
                        resetData.Selector = 0;
                        resetData.Attributes = 0x93; // present, read/write data segment -- matches real-mode default
                        resetValues[11].Segment = resetData; // DS
                        resetValues[12].Segment = resetData; // ES
                        resetValues[13].Segment = resetData; // FS
                        resetValues[14].Segment = resetData; // GS
                        WHvSetVirtualProcessorRegisters(partition, 0, resetNames, 15, resetValues);
                        skipRipAdvance = 1;

                        // This CPU-register reset reliably gets the guest
                        // past the original frozen spin-wait deadlock --
                        // confirmed across many independent runs. It does
                        // NOT reliably prevent a secondary bugcheck 0x139
                        // (KERNEL_SECURITY_CHECK_FAILURE, Segment Heap list
                        // corruption) that occurs on the second boot pass
                        // in most runs. Four mitigation attempts were tried
                        // and tested live, none reliably: clearing guest RAM
                        // above 16MB (worked once, failed on repeated
                        // trials), a narrower/faster 240MB clear (failed),
                        // resetting rtcPeriodicTickArmed/lastRtcPeriodicTick
                        // alone (failed), and combining the RAM clear with
                        // the rtcPeriodicTickArmed reset (also eventually
                        // failed). None left in -- none earned their
                        // complexity.
                        //
                        // The RTC-timing theory behind all four attempts
                        // was itself directly disproven afterward: live
                        // symbol resolution of the "stall" RIP (via
                        // ntkrnlmp.pdb, added as a one-off diagnostic and
                        // removed once it answered the question) showed it
                        // resolves to HaliHaltSystem -- the HAL's terminal
                        // halt loop, reached only *after* a bugcheck has
                        // already fully completed. The oscillating RIP and
                        // HIGH_LEVEL IRQL are just the ordinary signature
                        // of a frozen post-bugcheck CPU, not a live
                        // interrupt-delivery race -- confirmed further by
                        // that same diagnostic pass showing the guest's
                        // RTC PIE bit never even gets re-enabled before
                        // the bugcheck happens. The real fault is exactly
                        // where the earlier investigation phase already
                        // found it: RtlpHpLfhOwnerMoveSubsegment (Segment
                        // Heap corruption) -- see
                        // docs/investigations/post-vppt-boot-stall.md for
                        // the full trace. This remains an open problem,
                        // not a solved one.

                        // Live-breakpoint infrastructure: this boot
                        // instance's ntoskrnl.exe base (if the first pass
                        // had already been found and patched) is about to
                        // be invalidated by the reset -- KASLR gives the
                        // second pass a new base. Restore the original
                        // byte first if we can still read/write it
                        // (defensive; harmless if the underlying page no
                        // longer maps the same way), then clear discovery
                        // state so it's found fresh on the second pass.
                        if (g_bpPatched && guestMemory) {
                            kernelWriteByteVA((unsigned char *)guestMemory, g_bpCr3, g_bpTargetVA, g_bpOriginalByte);
                        }
                        g_bpModuleBase = 0;
                        g_bpTargetVA = 0;
                        g_bpCr3 = 0;
                        g_bpPatched = 0;
                        g_wpArmed = 0; // DR7 was just zeroed above (real RESET# semantics) -- re-arm fresh on rediscovery
                        g_wp0Fired = 0;
                        g_wp1Fired = 0;
                        g_wp2Fired = 0;
                        g_haliDispatchArmed = 0;
                        g_haliDispatchHitCount = 0;
                        g_poolCallerArmed = 0;
                        g_poolCallerHitCount = 0;
                        g_helperEntryArmed = 0;
                        g_helperEntryHitCount = 0;
                        g_loopAdvArmed = 0;
                        g_loopAdvHitCount = 0;
                        g_sawReset = 1; // U23c: pass 2 begins after this reset
                        g_resetExitCount = exitCount; // U28: window anchor
                        g_getsubWindowArmed = 0;

                        printf("[reset] port 0x64/0xFE keyboard-controller reset pulse honored -- vCPU restarted at reset vector (exitCount=%ld)\n", exitCount);
                        u40Dump(0, "end of pass 1, at reset");  // U40: pass-1 profile baseline to compare pass 2 against
                        u46DumpLapic(partition, "at reset -- state pass 2 will INHERIT");  // U46: LAPIC survives the CPU-only reset
#if U47_RESET_LAPIC
                        u47ResetLapic(partition);  // U47 fix test: quiesce the LAPIC like a real reset
                        u46DumpLapic(partition, "after U47 quiesce");
#endif
                        // U42: pass-1 IOAPIC totals, plus the state pass 2 will
                        // INHERIT -- this reset restores CPU registers only, so
                        // whatever pass 1 left in ioapic1/ioapic2 carries over.
                        printf("[u42] pass1 IOAPIC accesses total=%ld (logged %ld)\n",
                               g_ioapicPassAccesses[0], g_ioapicPassLogged[0]);
                        {
                            int e;
                            printf("[u42] state INHERITED by pass 2: ioapic1.id=%u selectedReg=0x%02X | ioapic2.id=%u selectedReg=0x%02X\n",
                                   ioapic1.id, ioapic1.selectedReg, ioapic2.id, ioapic2.selectedReg);
                            for (e = 0; e < 24; e++) {
                                if (ioapic1.redirTable[e] != 0x10000ULL)
                                    printf("[u42]   ioapic1.redir[%d]=0x%016llX (vec=0x%02X masked=%d) -- NOT at power-on default\n",
                                           e, (unsigned long long)ioapic1.redirTable[e],
                                           (unsigned char)(ioapic1.redirTable[e] & 0xFF),
                                           (ioapic1.redirTable[e] & 0x10000ULL) ? 1 : 0);
                            }
                            for (e = 0; e < 24; e++) {
                                if (ioapic2.redirTable[e] != 0x10000ULL)
                                    printf("[u42]   ioapic2.redir[%d]=0x%016llX (vec=0x%02X masked=%d) -- NOT at power-on default\n",
                                           e, (unsigned long long)ioapic2.redirTable[e],
                                           (unsigned char)(ioapic2.redirTable[e] & 0xFF),
                                           (ioapic2.redirTable[e] & 0x10000ULL) ? 1 : 0);
                            }
                        }
                        fflush(stdout);
                        fflush(stdout);
                    }

                    // Full i8042 write trace. The [aux] lines only cover the
                    // handful of commands that happen to be aux-tagged, which made
                    // the mouse-detection conversation look far emptier than it is
                    // -- and "the guest never sent a device command" is a claim
                    // that needs the WHOLE exchange to support it, not a filtered
                    // view of it. Bounded so it cannot flood.
                    if (port == 0x60 || port == 0x64) {
                        static int i8042LogCount = 0;
                        if (i8042LogCount < 150) {
                            i8042LogCount++;
                            printf("[i8042 #%d] write port=0x%X val=0x%02X\n", i8042LogCount, port, val);
                            fflush(stdout);
                        }
                    }

                    if (port == 0x70) {
                        cmosSelectedReg = val & 0x7F;
                        if (rtcDiagLoadBaseKnown) rtcDiagLogAccess(partition, exitContext.VpContext.Rip, cmosSelectedReg, 1);
                    }
                    else if (port == 0x71) {
                        cmosRegisters[cmosSelectedReg] = val;
                        if (cmosSelectedReg == 0x0A || cmosSelectedReg == 0x0B) {
                            static int cmosAbLogCount = 0;
                            if (cmosAbLogCount < 100) {
                                cmosAbLogCount++;
                                printf("[cmos-ab] write reg=0x%02X value=0x%02X (RegA=0x%02X RegB=0x%02X, PIE=%d)\n",
                                       cmosSelectedReg, val, cmosRegisters[0x0A], cmosRegisters[0x0B],
                                       (cmosRegisters[0x0B] & 0x40) ? 1 : 0);
                                fflush(stdout);
                            }
                        }
                    }
                    // Controller configuration byte, read (0x20) / write (0x60).
                    //
                    // This used to answer 0x20 with a HARDCODED 0x45 and ignore
                    // 0x60 entirely, and that is why the guest never had a working
                    // mouse. i8042prt writes a configuration byte enabling the
                    // second-port interrupt, reads it back to confirm, and sees its
                    // write vanish -- so it concludes there is no usable second
                    // port and never sends the device a single command. Measured
                    // exactly that: the guest issued only controller-level A7/A8
                    // port enables, never 0xD4+0xFF (reset), 0xF2 (identify) or
                    // 0xF4 (enable reporting), so auxReportingEnabled stayed 0 and
                    // every mouse packet was gated (packets=0 gated=272).
                    //
                    // Bits: 0 = port1 IRQ, 1 = port2(aux) IRQ, 2 = system flag,
                    // 4 = port1 clock disable, 5 = port2 clock disable,
                    // 6 = translation. Clock-disable bits are kept in sync with
                    // the A7/A8 enables so the two views cannot contradict.
                    else if (port == 0x64 && val == 0x20) {
                        unsigned char cfg = i8042ConfigByte;
                        if (auxPortEnabled) cfg &= ~0x20; else cfg |= 0x20;
                        kbEnqueue(cfg);
                    }
                    else if (port == 0x64 && val == 0x60) {
                        nextByteIsConfig = 1;
                        printf("[aux] controller: next byte is the configuration byte\n");
                        fflush(stdout);
                    }
                    else if (port == 0x60 && nextByteIsConfig) {
                        nextByteIsConfig = 0;
                        i8042ConfigByte = val;
                        // Bit 5 set means the aux clock is DISABLED.
                        auxPortEnabled = (val & 0x20) ? 0 : 1;
                        printf("[aux] controller: config byte = 0x%02X (port2 IRQ=%d, aux clock %s)\n",
                               val, (val & 0x02) ? 1 : 0, (val & 0x20) ? "disabled" : "enabled");
                        fflush(stdout);
                    }
                    else if (port == 0x64 && val == 0xAA) kbEnqueue(0x55); // controller self-test: 0x55 = passed
                    else if (port == 0x64 && val == 0xAB) kbEnqueue(0x00); // test first PS/2 port: 0x00 = passed
                    else if (port == 0x64 && val == 0xA7) { auxPortEnabled = 0; printf("[aux] controller: disable AUX port\n"); fflush(stdout); }
                    else if (port == 0x64 && val == 0xA8) { auxPortEnabled = 1; printf("[aux] controller: enable AUX port\n"); fflush(stdout); }
                    else if (port == 0x64 && val == 0xA9) { kbEnqueue(0x00); printf("[aux] controller: test AUX port\n"); fflush(stdout); } // reply via keyboard-tagged path, matching 0xAB above
                    else if (port == 0x64 && val == 0xD4) { nextByteTargetsAux = 1; printf("[aux] controller: next byte targets AUX\n"); fflush(stdout); }
                    else if (port == 0x60 && nextByteTargetsAux) {
                        nextByteTargetsAux = 0;
                        auxHandleCommand(val);
                    }
                    else if (port == 0x60 && val == 0xFF) {
                        // Reset the keyboard device itself (as opposed to
                        // the 0xAA/0xAB controller-level tests above): real
                        // hardware sends TWO bytes back in sequence -- 0xFA
                        // (command acknowledged) then, once the reset
                        // finishes, 0xAA (self-test passed). Previously
                        // this fell through to the generic "always just
                        // send 0xFA" handler below, which never sent the
                        // second byte -- a driver waiting on the full
                        // handshake before considering the keyboard usable
                        // would never see it complete.
                        kbEnqueue(0xFA);
                        kbEnqueue(0xAA);
                    }
                    else if (port == 0x60) kbEnqueue(0xFA);
                    else if (port == 0x402) {
                        appendToLog((char)val);
                    }
                    else if (port == 0x92) {
                        port92Value = val & 0xFE;
                        int newA20 = (val & 0x02) ? 1 : 0;
                        if (newA20 != a20Enabled) {
                            a20Enabled = newA20;
                            // TEMP DIAGNOSTIC: remap disabled, see above
                            // updateA20Mapping(partition);
                        }
                        if (val & 0x01) {
                            // Hot-reset bit -- restart the vCPU at the BIOS
                            // reset vector, same as the initial boot setup.
                            // Guest RAM and our emulated device state (CMOS,
                            // ATA, etc.) intentionally survive, matching how
                            // a real warm reset doesn't clear battery-backed
                            // CMOS or physical memory.
                            WHV_REGISTER_NAME resetNames[2] = { WHvX64RegisterRip, WHvX64RegisterCs };
                            WHV_REGISTER_VALUE resetValues[2] = { 0 };
                            resetValues[0].Reg64 = 0xFFF0;
                            WHV_X64_SEGMENT_REGISTER resetCs = { 0 };
                            resetCs.Base = uefiMode ? 0xFFFF0000 : 0xF0000;
                            resetCs.Limit = 0xFFFF;
                            resetCs.Selector = 0xF000;
                            resetCs.Attributes = 0x9B;
                            resetValues[1].Segment = resetCs;
                            WHvSetVirtualProcessorRegisters(partition, 0, resetNames, 2, resetValues);
                            skipRipAdvance = 1;
                            printf("[port 0x92 hot reset -- exitCount=%ld]\n", exitCount); fflush(stdout);
                        }
                    }
                    // Master 8259: track the ICW init sequence far enough to know
                    // which writes to 0x21 are the MASK, and what vector base the
                    // guest remapped IRQ0-7 to. See pic1Mask's comment -- this is
                    // readiness detection, not a PIC.
                    else if (port == 0x20) {
                        if (val & 0x10) {            // ICW1 -- init sequence begins
                            pic1Icw1 = val;
                            pic1InitStage = 1;       // next 0x21 write is ICW2
                        }
                        // Otherwise OCW2/OCW3 (EOI, read register select): ignored,
                        // since no in-service state is modelled.
                    }
                    else if (port == 0x21) {
                        if (pic1InitStage == 1) {
                            pic1VectorBase = val;    // ICW2: IRQ0 lands on this vector
                            // ICW3 present only when cascaded (ICW1 bit1 clear)
                            pic1InitStage = (pic1Icw1 & 0x02) ? ((pic1Icw1 & 0x01) ? 3 : 0) : 2;
                            printf("[pic] master remapped: IRQ0 -> vector 0x%02X\n", pic1VectorBase);
                            fflush(stdout);
                        } else if (pic1InitStage == 2) {
                            pic1InitStage = (pic1Icw1 & 0x01) ? 3 : 0;   // ICW3
                        } else if (pic1InitStage == 3) {
                            pic1InitStage = 0;                          // ICW4
                        } else {
                            unsigned char was = pic1Mask;
                            pic1Mask = val;                             // OCW1
                            if ((was & 0x01) != (val & 0x01)) {
                                printf("[pic] IRQ0 %s (mask=0x%02X)\n",
                                       (val & 0x01) ? "MASKED" : "UNMASKED", val);
                                fflush(stdout);
                            }
                        }
                    }
                    else if (port == 0x43) {
                        // PIT mode/command register.
                        unsigned int chan = (val >> 6) & 0x3;
                        if (chan == 2) {
                            pitChannel2AccessMode = (val >> 4) & 0x3;
                            pitChannel2WritePhase = 0;
                            pitChannel2Loaded = 0;
                        } else if (chan == 0) {
                            // Channel 0 IS the system timer -- the one whose output
                            // drives IRQ0. Previously undecoded, so the firmware
                            // programming its own tick rate was invisible.
                            pitChannel0AccessMode = (val >> 4) & 0x3;
                            pitChannel0WritePhase = 0;
                        }
                    }
                    else if (port == 0x40) {
                        // Channel 0 reload value. The count itself is not used --
                        // ticks are paced off host time at the classic 18.2Hz --
                        // but the WRITE is the signal that the firmware has set its
                        // timer up and is expecting interrupts from it.
                        if (pitChannel0AccessMode == 3 && pitChannel0WritePhase == 0) {
                            pitChannel0WritePhase = 1;   // lobyte of a 2-byte load
                        } else {
                            pitChannel0WritePhase = 0;
                            if (!pitChannel0Programmed) {
                                pitChannel0Programmed = 1;
                                printf("[pit] channel 0 programmed -- system timer armed\n");
                                fflush(stdout);
                            }
                        }
                    }
                    else if (port == 0x42) {
                        // Channel 2 count reload value, byte order depends on
                        // the access mode latched via port 0x43.
                        if (pitChannel2AccessMode == 1) { // lobyte only
                            pitChannel2Reload = val;
                            pitChannel2Loaded = 1;
                            QueryPerformanceCounter(&pitChannel2LoadTime);
                        } else if (pitChannel2AccessMode == 2) { // hibyte only
                            pitChannel2Reload = (UINT16)val << 8;
                            pitChannel2Loaded = 1;
                            QueryPerformanceCounter(&pitChannel2LoadTime);
                        } else { // lobyte/hibyte sequence
                            if (pitChannel2WritePhase == 0) {
                                pitChannel2LowByte = val;
                                pitChannel2WritePhase = 1;
                            } else {
                                pitChannel2Reload = ((UINT16)val << 8) | pitChannel2LowByte;
                                pitChannel2WritePhase = 0;
                                pitChannel2Loaded = 1;
                                QueryPerformanceCounter(&pitChannel2LoadTime);
                            }
                        }
                    }
                    else if (port == 0x61) {
                        unsigned char newGate = val & 0x1;
                        if (newGate && !port61Gate && pitChannel2Loaded) {
                            // Gate rising edge restarts the countdown, matching
                            // the usual "program count, then enable gate" sequence.
                            QueryPerformanceCounter(&pitChannel2LoadTime);
                        }
                        port61Gate = newGate;
                        port61SpeakerData = (val >> 1) & 0x1;
                    }
                    else if (port == 0x1F1) ataFeatures = val;
                    else if (port == 0x1F2) ataSectorCount = val;
                    else if (port == 0x1F3) ataLbaLow = val;
                    else if (port == 0x1F4) ataLbaMid = val;
                    else if (port == 0x1F5) ataLbaHigh = val;
                    else if (port == 0x1F6) ataDriveHead = val;
                    else if (port == 0x1F7) ataHandleCommand(partition, val);
                    else if (port == 0x3F6) {
                        if (val & 0x04) { // SRST -- software reset
                            ataStatus = ATA_ST_DRDY | ATA_ST_DSC;
                            ataError = 0;
                            ataDataLen = 0;
                            ataDataPos = 0;
                            // Real hardware presents the ATA "device passed
                            // diagnostics" signature on these registers after
                            // a reset -- probes rely on this to detect a drive.
                            ataSectorCount = 1;
                            ataLbaLow = 1;
                            ataLbaMid = 0;
                            ataLbaHigh = 0;
                        }
                    }

                    if (!skipRipAdvance) {
                        WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
                        WHV_REGISTER_VALUE newRip = { 0 };
                        newRip.Reg64 = exitContext.VpContext.Rip + exitContext.VpContext.InstructionLength;
                        WHvSetVirtualProcessorRegisters(partition, 0, &ripName, 1, &newRip);
                    }
                } else {
                    UINT16 returnValue = 0xFF;
                    if (port == 0x71) {
                        if (cmosSelectedReg <= 0x09) returnValue = cmosReadRtcField(cmosSelectedReg);
                        else if (cmosSelectedReg == 0x0A) returnValue = cmosRegisters[0x0A] & ~0x80; // UIP always 0
                        else if (cmosSelectedReg == 0x0D) returnValue = cmosRegisters[0x0D] | 0x80;  // VRT always 1
                        else if (cmosSelectedReg == 0x0C) {
                            // Register C (interrupt status/ack): real
                            // hardware clears it and de-asserts IRQ8 on
                            // read -- the RTC ISR's standard acknowledgment.
                            returnValue = cmosRegisters[0x0C];
                            cmosRegisters[0x0C] = 0;
                        }
                        else returnValue = cmosRegisters[cmosSelectedReg];
                        if (rtcDiagLoadBaseKnown) rtcDiagLogAccess(partition, exitContext.VpContext.Rip, cmosSelectedReg, 0);
                        // What the boot application is actually TOLD. The generic
                        // port trace cannot show this: on an IN, Rax still holds
                        // the guest's pre-read value, not our answer. Named
                        // registers, because a wrong or non-advancing clock is
                        // exactly the kind of thing it would retry over.
                        if (g_bootImgRunning && g_bootImgCmosLogged < 80) {
                            static const char *rn[] = {
                                "sec","secAlarm","min","minAlarm","hour","hourAlarm",
                                "dayOfWeek","dayOfMonth","month","year",
                                "regA","regB","regC","regD" };
                            g_bootImgCmosLogged++;
                            printf("[bootimg-cmos] reg 0x%02X %-10s -> 0x%02X (%u)\n",
                                   cmosSelectedReg,
                                   cmosSelectedReg < 14 ? rn[cmosSelectedReg] : "?",
                                   (unsigned)(returnValue & 0xFF), (unsigned)(returnValue & 0xFF));
                        }
                    }
                    else if (port == 0x92) returnValue = port92Value;
                    else if (port == 0x402) {
                        // edk2's PlatformDebugLibIoPortFound() probes this
                        // port with a read and treats the floating-bus value
                        // 0xFF as "no debug port present," silently
                        // discarding every DEBUG() message rather than
                        // writing it -- any other value makes it (correctly,
                        // for our purposes) detect the port as present.
                        returnValue = 0x00;
                    }
                    else if (port == 0x64) {
                        // Bit 0: output buffer full. Bit 5: the buffered byte
                        // came from the AUX (mouse) port rather than the
                        // keyboard. Keyboard takes priority when both have
                        // data, matching the dequeue order below so a status
                        // read immediately followed by a data read always
                        // agree on the source.
                        // Serve whichever byte arrived FIRST, not the keyboard
                        // unconditionally.
                        //
                        // The old "keyboard always wins" rule deadlocked the whole
                        // controller. One unread keyboard byte (kbPending stuck at
                        // 1) meant this register reported "keyboard data waiting"
                        // forever, so the guest could never see the mouse's reply
                        // to its reset -- it retried that reset endlessly, the
                        // i8042 device stack never finished starting, and because
                        // it never finished starting, i8042prt never connected the
                        // keyboard ISR. Which meant the keyboard byte was never
                        // read. Which kept the deadlock alive.
                        if (ps2AuxIsNext()) returnValue = 0x21;   // OBF | AUXB
                        else if (kbHasData()) returnValue = 0x01; // OBF
                        else returnValue = 0x00;
                    }
                    else if (port == 0x60) {
                        // Must match the source the status register just reported,
                        // or the guest reads a byte it will attribute to the wrong
                        // device.
                        if (ps2AuxIsNext()) {
                            g_auxBytesToGuest++;
                            returnValue = auxDequeue();
                        }
                        else if (kbHasData()) {
                            // Counted for the same reason as the aux side: whether
                            // the guest READS a keystroke is the only direct
                            // evidence the keyboard works. Inferring it from
                            // framebuffer changes is unreliable -- Tab does not
                            // always repaint anything visible, which already
                            // produced one false "it works".
                            g_kbBytesToGuest++;
                            returnValue = kbDequeue();
                            // A RING of the last scancodes the guest actually
                            // consumed. Logging the FIRST n is useless here: those
                            // are all i8042prt's boot handshake, and the question is
                            // what it reads once Setup is up and a key is pressed.
                            g_kbLastRead[g_kbLastReadCount % KB_LASTREAD_MAX] = returnValue;
                            g_kbLastReadCount++;
                            // Unbounded, like [ps2-key]. Both rings above are printed
                            // from the change-gated heartbeat, so a stale snapshot was
                            // indistinguishable from a byte that never moved -- which
                            // is exactly the question here.
                            if (ps2Trace()) {
                                printf("[ps2-out] #%ld guest read scancode 0x%02X\n",
                                       g_kbLastReadCount, returnValue);
                                fflush(stdout);
                            }
                        }
                        else if (auxHasData()) {
                            // Count what the guest actually CONSUMES. g_auxPackets
                            // only records what we queued, which says nothing about
                            // whether the driver is reading it -- and "packets=1479
                            // but no cursor" is precisely the case where those two
                            // numbers disagree.
                            g_auxBytesToGuest++;
                            returnValue = auxDequeue();
                        }
                        else returnValue = 0x00;
                        // The buffer just made room, so any motion held back
                        // earlier can go out now. Driving this from the guest's
                        // own reads is what paces the mouse to whatever rate it
                        // is actually willing to consume.
                        auxDrainPendingMotion();

                        // RE-ASSERT NOW if the read left more data behind.
                        //
                        // On real hardware the read itself is what refills the
                        // output buffer: OBF clears, the controller loads the next
                        // byte, OBF sets again and the line re-asserts immediately.
                        // We were not doing that. The next byte's interrupt waited
                        // for ps2ServiceOutputIrq to be called from the run loop --
                        // which only iterates when the guest EXITS, and is throttled
                        // to 1ms on top. So after each keystroke the edge for the
                        // following byte arrived whenever the host happened to get a
                        // turn, and the 50ms re-assert was left as the only thing
                        // reliably producing it.
                        //
                        // That matches every measurement: disabling the re-assert
                        // collapsed delivery to 1/15 (it was carrying the whole
                        // mechanism), keys arrived in fast bursts while the guest was
                        // busy exiting and stalled once it went idle, and identical
                        // runs scattered between 1/15 and 15/15 purely on timing.
                        //
                        // REVERTED, and the reason matters. Doing it HERE means
                        // injecting from inside the I/O exit handler, before the
                        // guest's RIP has been advanced past the IN instruction --
                        // an interrupt taken at that point lands mid-emulation.
                        // Measured against the same test: 16/60 delivered with it,
                        // then 7/75 on a repeat, versus 16/45 without. Worse, twice.
                        //
                        // The hardware reasoning is still right -- a real controller
                        // re-asserts the moment the read refills its output buffer --
                        // so if this is retried, do it AFTER the exit is fully
                        // handled and RIP advanced, not from within the handler.
                        //   if (kbHasData() || auxHasData()) ps2ServiceOutputIrq(partition);
                    }
                    else if (port == 0x61) {
                        LARGE_INTEGER now;
                        QueryPerformanceCounter(&now);
                        double elapsedUs = (double)(now.QuadPart - lastToggleTime.QuadPart) * 1000000.0 / perfFrequency.QuadPart;
                        if (elapsedUs >= 15.0) {
                            refreshToggle ^= 0x10;
                            lastToggleTime = now;
                        }

                        unsigned char channel2Output = 0;
                        if (port61Gate && pitChannel2Loaded) {
                            double loadedElapsedUs = (double)(now.QuadPart - pitChannel2LoadTime.QuadPart) * 1000000.0 / perfFrequency.QuadPart;
                            double elapsedTicks = loadedElapsedUs * (PIT_HZ / 1000000.0);
                            UINT32 reload = pitChannel2Reload == 0 ? 65536 : pitChannel2Reload;
                            if (elapsedTicks >= (double)reload) channel2Output = 0x20;
                        }

                        returnValue = (port61Gate ? 0x01 : 0) | (port61SpeakerData ? 0x02 : 0)
                                      | refreshToggle | channel2Output;
                    }
                    else if (port == 0x1F6) returnValue = ataDriveHead; // readable regardless of which drive is selected
                    // NOTE: these registers are intentionally NOT gated on
                    // ataSlaveSelected() the way commands are. A hard-floated
                    // 0xFF for an absent slave (BSY permanently set) makes
                    // BIOS's "wait for not-busy" polling -- done *before* it
                    // even issues a command, just to see if a device
                    // responds -- hang forever. Status is handled specially
                    // below to give BSY=0 without leaking the master's
                    // possibly-stale DRQ/ERR; everything else can safely
                    // mirror the shared bus. Commands themselves are still
                    // correctly dropped for the slave in ataHandleCommand,
                    // which is what actually prevents a phantom second
                    // drive from appearing.
                    else if (port == 0x1F1) returnValue = ataError;
                    else if (port == 0x1F2) returnValue = ataSectorCount;
                    else if (port == 0x1F3) returnValue = ataLbaLow;
                    else if (port == 0x1F4) returnValue = ataLbaMid;
                    else if (port == 0x1F5) returnValue = ataLbaHigh;
                    else if (port == 0x1F7 || port == 0x3F6) {
                        // BSY must read 0 (else "wait for not-busy" polling
                        // before a command is even issued hangs forever --
                        // see above), but DRQ/ERR must NOT leak the
                        // master's possibly-stale command result, or a
                        // dropped command on the slave looks like it
                        // produced data. DRDY-only is what an idle,
                        // never-been-commanded device looks like.
                        returnValue = ataSlaveSelected() ? ATA_ST_DRDY : ataStatus;
                    }

                    WHV_REGISTER_NAME names[2] = { WHvX64RegisterRax, WHvX64RegisterRip };
                    WHV_REGISTER_VALUE values[2] = { 0 };
                    values[0].Reg64 = returnValue;
                    values[1].Reg64 = exitContext.VpContext.Rip + exitContext.VpContext.InstructionLength;
                    WHvSetVirtualProcessorRegisters(partition, 0, names, 2, values);
                }
                break;
            }

            case WHvRunVpExitReasonX64Cpuid: {
                // Unreachable in current config: no WHvPartitionPropertyCodeCpuidExitList
                // is registered (see the SyntheticProcessorFeaturesBanks setup
                // near WHvSetupPartition for why the CPUID-trap approach this
                // handler used to implement was abandoned). Kept only so the
                // switch has a defined case if that property is ever
                // reintroduced; just answers with WHV's own computed default.
                WHV_REGISTER_NAME names[5] = {
                    WHvX64RegisterRax, WHvX64RegisterRbx, WHvX64RegisterRcx,
                    WHvX64RegisterRdx, WHvX64RegisterRip
                };
                WHV_REGISTER_VALUE values[5] = { 0 };
                values[0].Reg64 = exitContext.CpuidAccess.DefaultResultRax;
                values[1].Reg64 = exitContext.CpuidAccess.DefaultResultRbx;
                values[2].Reg64 = exitContext.CpuidAccess.DefaultResultRcx;
                values[3].Reg64 = exitContext.CpuidAccess.DefaultResultRdx;
                values[4].Reg64 = exitContext.VpContext.Rip + exitContext.VpContext.InstructionLength;
                WHvSetVirtualProcessorRegisters(partition, 0, names, 5, values);
                break;
            }

            case WHvRunVpExitReasonMemoryAccess: {
                if (g_bpPatched) {
                    logBpEvent("memaccess gpa=0x%llX", (unsigned long long)exitContext.MemoryAccess.Gpa);
                }
                // MMIO the boot application touches -- device registers it is
                // probing. Same window and budget as the port log above.
                // Its OWN budget: sharing one with the port log meant the port
                // traffic consumed it all and not a single MMIO line was ever
                // emitted, while MMIO turned out to be half the window.
                if (g_bootImgRunning) {
                    bootImgCountMmio(exitContext.MemoryAccess.Gpa);
                    if (g_bootImgMmioLogged < 60) {
                        UINT64 gpa = exitContext.MemoryAccess.Gpa;
                        g_bootImgMmioLogged++;
                        // Decode the AHCI register, which is the only MMIO block
                        // this window touches.
                        if (gpa >= 0x80000100 && gpa < 0x80001000) {
                            UINT64 off = gpa - 0x80000100;
                            unsigned reg = (unsigned)(off % 0x80);
                            // The register's CURRENT value from our own ABAR
                            // buffer -- what the driver is seeing. Which bits are
                            // set is the whole question for PxCMD (ST/CR/FRE/FR).
                            UINT32 cur = 0;
                            if (ahciAbarMemory && gpa - 0x80000000 + 4 <= AHCI_BAR_SIZE)
                                cur = *(UINT32 *)((unsigned char *)ahciAbarMemory + (gpa - 0x80000000));
                            printf("[bootimg-mmio] AHCI port%u %-5s %s val=0x%08X%s\n",
                                   (unsigned)(off / 0x80),
                                   reg == 0x10 ? "PxIS" : reg == 0x14 ? "PxIE" :
                                   reg == 0x18 ? "PxCMD" : reg == 0x20 ? "PxTFD" :
                                   reg == 0x28 ? "PxSSTS" : reg == 0x30 ? "PxSERR" :
                                   reg == 0x38 ? "PxCI" : "?",
                                   exitContext.MemoryAccess.AccessInfo.AccessType == WHvMemoryAccessWrite ? "W" : "R",
                                   cur,
                                   reg == 0x18 ? (cur & 0x1 ? " ST" : " st") : "");
                        } else {
                            printf("[bootimg-mmio] gpa=0x%llX %s rip=0x%llX\n",
                                   (unsigned long long)gpa,
                                   exitContext.MemoryAccess.AccessInfo.AccessType == WHvMemoryAccessWrite ? "W" : "R",
                                   (unsigned long long)exitContext.VpContext.Rip);
                        }
                    }
                }
                if (ioapicHandleMmioAccess(partition, &exitContext)) {
                    break;
                }
                if (ahciHandleAbarMmio(partition, &exitContext)) { // U58: trapped ABAR
                    break;
                }
#if LH_USE_XHCI
                if (xhciHandleMmio(partition, &exitContext)) { // USB 3.x controller
                    break;
                }
#else
                if (ehciHandleMmio(partition, &exitContext)) { // USB 2.0 controller
                    break;
                }
#endif
                if (e1000HandleMmio(partition, &exitContext)) { // Intel 82540EM
                    break;
                }
                if (rtl8139HandleBar1Mmio(partition, &exitContext)) { // NIC memory window
                    break;
                }
                UINT64 faultAddr = exitContext.MemoryAccess.Gpa;
                UINT64 pageBase = faultAddr & ~0xFFFULL;
                memAccessFaultCount++;
                if (memAccessFaultCount <= 20 || memAccessFaultCount % 1000 == 0) {
                    printf("[memaccess fault #%d gpa=0x%llX]\n", memAccessFaultCount, (unsigned long long)faultAddr);
                    fflush(stdout);
                }
                // Mapping RAM over a device BAR silently converts its registers
                // into passive storage -- side effects stop, and the device looks
                // alive while doing nothing. That is a debugging trap, so say so
                // loudly rather than let it happen quietly. (Seen for real: an
                // undecodable 16-bit read-modify-write on the NIC's CpCmd fell
                // through to here and covered the whole BAR1 page.) We still map,
                // because without a decoded instruction length there is no way to
                // step over the access, and refusing would spin on it forever.
                if (rtl8139MmioBase != 0 &&
                    pageBase == ((UINT64)rtl8139MmioBase & ~0xFFFULL)) {
                    if (!g_nicMmioClobbered) {
                        g_nicMmioClobbered = 1;
                        printf("[rtl8139] *** WARNING: scratch page mapped over BAR1 "
                               "(gpa=0x%llX) -- the NIC's memory window is now plain RAM "
                               "and its registers have no side effects\n",
                               (unsigned long long)faultAddr);
                        fflush(stdout);
                    }
                }
                void *scratchPage = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (scratchPage != NULL) {
                    memset(scratchPage, 0, 0x1000);
                    WHvMapGpaRange(partition, scratchPage, pageBase, 0x1000,
                                   WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute);
                }
                break;
            }

            case WHvRunVpExitReasonCanceled:
                // rtcCancelThread forces these so the main loop regains
                // control and can deliver the RTC/PIT periodic interrupts
                // even while the guest is CPU-bound in a busy-spin with no
                // VM exits of its own. This is now the ONLY unconditional
                // (non-halted) delivery point for those two interrupts --
                // narrowed here from "after every exit reason" specifically
                // because that was landing injections at essentially random
                // guest RSP values and faulting OVMF's own FXSAVE (see
                // docs/investigations/post-vppt-boot-stall.md, 2026-07-17).
                // This exit is bounded and deliberate (rtcCancelThread's own
                // ~1ms cadence while RTC PIE is armed), a much narrower
                // surface than every I/O/MMIO/PCI-config exit combined.
                if (g_bpPatched) logBpEvent("cancel (rtcCancelThread forced exit)");
                deliverRtcPeriodicIrq(partition);
                deliverPitTimerIrq(partition);
                break;

            case WHvRunVpExitReasonException: {
                // Three simultaneous hardware EXECUTION breakpoints
                // (DR0/DR1/DR2, R/W=execute) -- see the WP_RETURN_RVA /
                // WP_PHASE1INIT_ENTRY_RVA / BP_FAIL_SITE_RVA comment block
                // near the top of this file for what each one answers.
                // Currently: DR0/DR1 sit at the entry of
                // ExInitializePagedHeaps/ExInitializePoolHeapManagement
                // (U6 -- who calls them?), DR2 stays on
                // Phase1InitializationDiscard's entry for ordering context.
                // Unlike the earlier data-write watchpoint (trap
                // semantics, RIP past the access), an instruction
                // execution breakpoint is fault-style: RIP is the
                // breakpoint's own address, the instruction has NOT
                // executed yet -- so RIP alone unambiguously identifies
                // which of the three trapped (only one can be "next
                // instruction" at a given exit). Each site is one-shot:
                // once identified, only ITS DR7 L-bit is cleared, leaving
                // the other two still armed so a single pass can capture
                // all three events.
                // Boot application entry point (DR3). Checked before the ntoskrnl
                // sites below because it is a different investigation entirely and
                // g_wpArmed is not set during the firmware phase.
                if (exitContext.VpException.ExceptionType == WHvX64ExceptionTypeDebugTrapOrFault &&
                    g_bootImgBpArmed && exitContext.VpContext.Rip == g_bootImgEntry) {
                    WHV_REGISTER_NAME rn[6] = { WHvX64RegisterRcx, WHvX64RegisterRdx,
                                                WHvX64RegisterRsp, WHvX64RegisterRflags,
                                                WHvX64RegisterCr3, WHvX64RegisterDr7 };
                    WHV_REGISTER_VALUE rv[6] = { 0 };
                    g_bootImgBpHits++;
                    g_bootImgEntryExit = exitCount;
                    WHvGetVirtualProcessorRegisters(partition, 0, rn, 6, rv);
                    printf("[bootimg] *** ENTRY POINT REACHED *** hit #%d at 0x%llX (exit %ld)\n",
                           g_bootImgBpHits, (unsigned long long)exitContext.VpContext.Rip, exitCount);
                    // UEFI entry is (ImageHandle, SystemTable) in RCX/RDX.
                    printf("[bootimg]   ImageHandle=0x%llX SystemTable=0x%llX rsp=0x%llX "
                           "rflags=0x%llX cr3=0x%llX\n",
                           (unsigned long long)rv[0].Reg64, (unsigned long long)rv[1].Reg64,
                           (unsigned long long)rv[2].Reg64, (unsigned long long)rv[3].Reg64,
                           (unsigned long long)rv[4].Reg64);
                    // MOVE THE BREAKPOINT TO THE RETURN ADDRESS.
                    //
                    // At an entry point RSP points at the return address the
                    // caller pushed, so [rsp] is where DxeCore's StartImage
                    // resumes. Trapping there catches the exact instant the boot
                    // manager gives up, with RAX holding the status it returned
                    // -- which settles whether EFI_TIMEOUT comes from this image
                    // or from the firmware around it, and dates the return to an
                    // exact exit count for bisecting the window.
                    {
                        UINT64 rsp = rv[2].Reg64;
                        UINT64 retAddr = 0;
                        if (guestMemory && rsp + 8 <= guestMemSize)
                            retAddr = *(UINT64 *)((unsigned char *)guestMemory + rsp);
                        if (retAddr && retAddr < guestMemSize) {
                            WHV_REGISTER_NAME rbn[2] = { WHvX64RegisterDr3, WHvX64RegisterDr7 };
                            WHV_REGISTER_VALUE rbv[2] = { 0 };
                            rbv[0].Reg64 = retAddr;
                            rbv[1].Reg64 = rv[5].Reg64 | 0x40ULL;   // keep L3 set
                            g_bootImgRetAddr = retAddr;
                            g_bootImgRunning = 1;   // start logging its I/O
                            WHvSetVirtualProcessorRegisters(partition, 0, rbn, 2, rbv);
                            printf("[bootimg]   return address [rsp]=0x%llX -- DR3 moved there\n",
                                   (unsigned long long)retAddr);
                        } else {
                            // Could not read it: disarm rather than leave DR3 on
                            // an address that will never be reached.
                            WHV_REGISTER_NAME dn = WHvX64RegisterDr7;
                            WHV_REGISTER_VALUE dv = { 0 };
                            dv.Reg64 = rv[5].Reg64 & ~0x40ULL;
                            WHvSetVirtualProcessorRegisters(partition, 0, &dn, 1, &dv);
                            g_bootImgBpArmed = 0;
                            printf("[bootimg]   could not read return address at rsp=0x%llX\n",
                                   (unsigned long long)rsp);
                        }
                        fflush(stdout);
                    }
                    break;
                }
                // The boot application returning -- RAX is its status.
                if (exitContext.VpException.ExceptionType == WHvX64ExceptionTypeDebugTrapOrFault &&
                    g_bootImgBpArmed && g_bootImgRetAddr &&
                    exitContext.VpContext.Rip == g_bootImgRetAddr) {
                    WHV_REGISTER_NAME rn2[2] = { WHvX64RegisterRax, WHvX64RegisterDr7 };
                    WHV_REGISTER_VALUE rv2[2] = { 0 };
                    WHvGetVirtualProcessorRegisters(partition, 0, rn2, 2, rv2);
                    printf("[bootimg] *** RETURNED *** at 0x%llX (exit %ld, %ld exits after entry)\n",
                           (unsigned long long)exitContext.VpContext.Rip, exitCount,
                           exitCount - g_bootImgEntryExit);
                    printf("[bootimg]   RAX = 0x%llX  (%s)\n", (unsigned long long)rv2[0].Reg64,
                           efiStatusName(rv2[0].Reg64));
                    printf("[bootimg]   TOTAL while it ran: %ld port accesses, %ld MMIO\n",
                           g_bootImgIoTotal, g_bootImgMmioTotal);
                    {
                        int pi;
                        for (pi = 0; pi < g_bootImgPortsSeen; pi++)
                            printf("[bootimg]     port 0x%03X : %ld\n",
                                   g_bootImgPortNum[pi], g_bootImgPortCount[pi]);
                        if (g_bootImgPortsSeen >= BOOTIMG_PORTS_MAX)
                            printf("[bootimg]     (port table full -- more distinct ports exist)\n");
                        for (pi = 0; pi < g_bootImgMmioSeen; pi++)
                            printf("[bootimg]     mmio 0x%llX : %ld\n",
                                   (unsigned long long)g_bootImgMmioPage[pi],
                                   g_bootImgMmioCount[pi]);
                        if (g_bootImgMmioSeen >= BOOTIMG_PORTS_MAX)
                            printf("[bootimg]     (mmio table full -- more distinct pages exist)\n");
                    }
                    // The tail of the window -- what it was doing as it gave up.
                    bootImgRingDump(BOOTIMG_RING_MAX);
                    g_bootImgRunning = 0;
                    fflush(stdout);
                    {
                        WHV_REGISTER_NAME dn = WHvX64RegisterDr7;
                        WHV_REGISTER_VALUE dv = { 0 };
                        dv.Reg64 = rv2[1].Reg64 & ~0x40ULL;
                        WHvSetVirtualProcessorRegisters(partition, 0, &dn, 1, &dv);
                        g_bootImgBpArmed = 0;
                    }
                    break;
                }
                if (exitContext.VpException.ExceptionType == WHvX64ExceptionTypeDebugTrapOrFault && g_wpArmed) {
                    UINT64 wpRip = exitContext.VpContext.Rip;

                    // DR3 (repurposed for U13): multi-shot tracer at
                    // HalpIommuInitSystem's own return point (its `ret`
                    // instruction). RIP is AT `ret`, not yet executed --
                    // EAX already holds its real, final return value at
                    // this exact point. Logged and resumed WITHOUT
                    // disabling DR3, so every call across the retry loop
                    // gets its return value captured, up to
                    // HALI_DISPATCH_MAX_HITS.
                    if (wpRip == g_haliDispatchTargetVA && g_haliDispatchArmed) {
                        g_haliDispatchHitCount++;
                        WHV_REGISTER_NAME dispatchNames[2] = { WHvX64RegisterRax, WHvX64RegisterRsp };
                        WHV_REGISTER_VALUE dispatchVals[2] = { 0 };
                        if (SUCCEEDED(WHvGetVirtualProcessorRegisters(partition, 0, dispatchNames, 2, dispatchVals))) {
                            INT32 retVal = (INT32)(dispatchVals[0].Reg64 & 0xFFFFFFFFULL);
                            UINT64 rspVA = dispatchVals[1].Reg64;
                            printf("[iommuret] hit #%d: HalpIommuInitSystem returned EAX=0x%08X (%s) rsp=0x%llX exitCount=%ld\n",
                                   g_haliDispatchHitCount, (unsigned)retVal, (retVal < 0) ? "FAILURE" : "success/other",
                                   (unsigned long long)rspVA, exitCount);
                            fflush(stdout);
                        }
                        WHV_REGISTER_NAME dr6ClearName = WHvX64RegisterDr6;
                        WHV_REGISTER_VALUE dr6ClearVal = { 0 };
                        WHvSetVirtualProcessorRegisters(partition, 0, &dr6ClearName, 1, &dr6ClearVal);
                        if (g_haliDispatchHitCount >= HALI_DISPATCH_MAX_HITS) {
                            printf("[iommuret] cap reached (%d hits) -- disabling DR3 for this pass\n", HALI_DISPATCH_MAX_HITS);
                            fflush(stdout);
                            WHV_REGISTER_NAME dr7ReadName2 = WHvX64RegisterDr7;
                            WHV_REGISTER_VALUE dr7ReadVal2 = { 0 };
                            WHvGetVirtualProcessorRegisters(partition, 0, &dr7ReadName2, 1, &dr7ReadVal2);
                            dr7ReadVal2.Reg64 &= ~0x40ULL; // clear L3
                            WHV_REGISTER_NAME dr7WriteName2 = WHvX64RegisterDr7;
                            WHvSetVirtualProcessorRegisters(partition, 0, &dr7WriteName2, 1, &dr7ReadVal2);
                            g_haliDispatchArmed = 0;
                        }
                        break;
                    }

                    // DR0 (U34): one-shot tracer at ExInitializePoolHeapManagement's
                    // entry -- the only writer to the ExPoolState pool-descriptor
                    // array in the image, and the function U33 showed must have run
                    // on pass 1 (descriptors populate) but appears not to on pass 2
                    // (they stay zero through the crash).
                    //
                    // RIP is AT the entry, before the prologue, so [rsp+0x00] is
                    // still the genuine return address and the qwords above it are
                    // the caller frames. Walking them names the Phase-0/1 chain that
                    // reaches this init on pass 1, which is the next rung to
                    // instrument if pass 2 never gets here.
                    //
                    // One-shot: this runs once per boot, and leaving an exec
                    // breakpoint armed on an executed entry re-faults on resume (no
                    // RF handling -- see the U21 note), spewing duplicate hits.
                    if (wpRip == g_wpTargetVA0 && g_poolCallerArmed) {
                        g_poolCallerHitCount++;
                        WHV_REGISTER_NAME poolRegNames[6] = { WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterR8, WHvX64RegisterR9, WHvX64RegisterRsp, WHvX64RegisterCr3 };
                        WHV_REGISTER_VALUE poolRegVals[6] = { 0 };
                        if (SUCCEEDED(WHvGetVirtualProcessorRegisters(partition, 0, poolRegNames, 6, poolRegVals))) {
                            UINT64 iRsp = poolRegVals[4].Reg64, iCr3 = poolRegVals[5].Reg64;
                            printf("[u34] *** " U34_SITE_NAME " ENTERED *** pass=%s exitCount=%ld",
                                   g_sawReset ? "2" : "1", exitCount);
                            if (g_sawReset && g_resetExitCount)
                                printf(" (reset+%ld)", exitCount - g_resetExitCount);
                            // U45: ExAllocatePoolWithTag(rcx=PoolType, rdx=NumberOfBytes,
                            // r8=Tag). Printing these lets us correlate this hit with
                            // the fatal allocation U44 saw at ExAllocateHeapPool
                            // (size ~0xAF7, tag 0).
                            printf("\n[u34]   args PoolType=0x%llX Size=0x%llX Tag=0x%llX r9=0x%llX rsp=0x%llX\n",
                                   (unsigned long long)poolRegVals[0].Reg64, (unsigned long long)poolRegVals[1].Reg64,
                                   (unsigned long long)poolRegVals[2].Reg64, (unsigned long long)poolRegVals[3].Reg64,
                                   (unsigned long long)iRsp);
                            // U45: [rsp+0x00] is the ONLY guaranteed frame -- at a
                            // function entry it is the real return address into the
                            // caller. Read and resolve it explicitly and label it as
                            // such, so it is never conflated with the heuristic scan
                            // below (which is what misled U44).
                            {
                                unsigned char ret0[8] = { 0 };
                                if (kernelReadVA((unsigned char *)guestMemory, iCr3, iRsp, ret0, sizeof(ret0))) {
                                    UINT64 q0 = *(UINT64 *)ret0, cb0 = 0;
                                    printf("[u34]   GUARANTEED caller [rsp+0x00]=0x%llX", (unsigned long long)q0);
                                    if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, iCr3, q0, &cb0))
                                        printf("  (base 0x%llX RVA 0x%llX)", (unsigned long long)cb0, (unsigned long long)(q0 - cb0));
                                    printf("\n");
                                }
                            }
                            // U46b: does pool exist yet at this hit? Read ExPoolState
                            // pooldesc[0] +0x08/+0x10 (RVA 0xC57EC0). +0x10 populated
                            // means pool-heap init has run and an allocation here would
                            // succeed; NULL means it would fault as on pass 2.
                            if (g_bpModuleBase) {
                                unsigned char pd[0x18] = { 0 };
                                UINT64 pdVA = g_bpModuleBase + 0xC57EC0ULL;
                                if (kernelReadVA((unsigned char *)guestMemory, iCr3, pdVA, pd, sizeof(pd))) {
                                    UINT64 f08 = *(UINT64 *)&pd[0x08], f10 = *(UINT64 *)&pd[0x10];
                                    printf("[u46b]   pool state at hit: pooldesc+0x08=0x%llX +0x10=0x%llX -> pool %s\n",
                                           (unsigned long long)f08, (unsigned long long)f10,
                                           (f10 != 0) ? "EXISTS (alloc would succeed)" : "ABSENT (alloc would fault -- pass-2 crash condition)");
                                    fflush(stdout);
                                }
                            }
                            // 0x100 for U44: the allocating subsystem can sit many
                            // frames up from ExAllocateHeapPool (it is called through
                            // ExAllocatePoolWithTag and friends), and non-canonical or
                            // garbage slots are filtered out anyway, so a deeper walk
                            // costs only log lines.
                            unsigned char iStk[0x100] = { 0 };
                            if (kernelReadVA((unsigned char *)guestMemory, iCr3, iRsp, iStk, sizeof(iStk))) {
                                int isk;
                                printf("[u34]   HEURISTIC deeper scan (context only -- stale frames possible, see U44/U45):\n");
                                for (isk = 0; isk < (int)sizeof(iStk); isk += 8) {
                                    UINT64 qv = *(UINT64 *)&iStk[isk];
                                    if (qv >= 0xFFFF800000000000ULL) {
                                        UINT64 cb = 0;
                                        printf("[u34]     [rsp+0x%02X]=0x%llX", isk, (unsigned long long)qv);
                                        if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, iCr3, qv, &cb))
                                            printf("  (base 0x%llX RVA 0x%llX)", (unsigned long long)cb, (unsigned long long)(qv - cb));
                                        printf("\n");
                                    }
                                }
                            }
                            fflush(stdout);
                        }
                        WHV_REGISTER_NAME dr6ClearName3 = WHvX64RegisterDr6;
                        WHV_REGISTER_VALUE dr6ClearVal3 = { 0 };
                        WHvSetVirtualProcessorRegisters(partition, 0, &dr6ClearName3, 1, &dr6ClearVal3);
                        if (g_poolCallerHitCount >= POOL_CALLER_MAX_HITS) {
                            printf("[u34] one-shot fired (%d hit) -- disarming DR0 for this pass\n", POOL_CALLER_MAX_HITS);
                            fflush(stdout);
                            WHV_REGISTER_NAME dr7ReadName3 = WHvX64RegisterDr7;
                            WHV_REGISTER_VALUE dr7ReadVal3 = { 0 };
                            WHvGetVirtualProcessorRegisters(partition, 0, &dr7ReadName3, 1, &dr7ReadVal3);
                            dr7ReadVal3.Reg64 &= ~0x1ULL; // clear L0
                            WHV_REGISTER_NAME dr7WriteName3 = WHvX64RegisterDr7;
                            WHvSetVirtualProcessorRegisters(partition, 0, &dr7WriteName3, 1, &dr7ReadVal3);
                            g_poolCallerArmed = 0;
                        }
                        break;
                    }

                    // DR1 (repurposed for U15): multi-shot tracer at
                    // HalpInitSystemHelper's own entry. RIP is AT the
                    // function's entry, not yet executed -- ECX/EDX/R8 are
                    // its real, live arguments (outer loop index, outer
                    // loop limit, third arg passed through to every
                    // dispatch call). Logged and resumed WITHOUT disabling
                    // DR1, so this directly answers U15: does this
                    // function get freshly re-entered many times (H7), or
                    // called once while stuck internally (H6)?
                    if (wpRip == g_wpTargetVA1 && g_helperEntryArmed) {
                        g_helperEntryHitCount++;
                        // U21: KeBugCheckEx entry. rcx=bugcheck code, rdx/r8/r9=
                        // params 1-3; param 4 at [rsp+0x28]; return address at
                        // [rsp+0x00] = the caller that raised the bugcheck.
                        WHV_REGISTER_NAME bcNames[6] = { WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterR8, WHvX64RegisterR9, WHvX64RegisterRsp, WHvX64RegisterCr3 };
                        WHV_REGISTER_VALUE bcVals[6] = { 0 };
                        if (SUCCEEDED(WHvGetVirtualProcessorRegisters(partition, 0, bcNames, 6, bcVals))) {
                            UINT64 bcRsp = bcVals[4].Reg64, bcCr3 = bcVals[5].Reg64;
                            printf("\n[BUGCHECK] hit #%d: KeBugCheckEx code=0x%llX p1=0x%llX p2=0x%llX p3=0x%llX exitCount=%ld\n",
                                   g_helperEntryHitCount,
                                   (unsigned long long)bcVals[0].Reg64, (unsigned long long)bcVals[1].Reg64,
                                   (unsigned long long)bcVals[2].Reg64, (unsigned long long)bcVals[3].Reg64, exitCount);
                            unsigned char bcStk[0x40] = { 0 };
                            if (kernelReadVA((unsigned char *)guestMemory, bcCr3, bcRsp, bcStk, sizeof(bcStk))) {
                                UINT64 retAddr = *(UINT64 *)&bcStk[0x00];
                                UINT64 p4 = *(UINT64 *)&bcStk[0x28];
                                UINT64 cb = 0;
                                printf("[BUGCHECK]   p4=0x%llX  caller(return)=0x%llX", (unsigned long long)p4, (unsigned long long)retAddr);
                                if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, bcCr3, retAddr, &cb))
                                    printf("  (base 0x%llX RVA 0x%llX)", (unsigned long long)cb, (unsigned long long)(retAddr - cb));
                                // U50: dump the raising module's image, once. U49 disproved the
                                // DSDT hypothesis, and two guesses at what 0xA5 p1=0x11 means have
                                // now cost a cycle each. Stop guessing: capture acpi.sys (the
                                // module that raises it, confirmed via acpi.pdb) so the code around
                                // the bugcheck caller can be disassembled and the actual tested
                                // condition read off. Same technique that worked for ntoskrnl (U27).
                                if (cb && cb != g_bpModuleBase) {
                                    static int g_raiserDumped = 0;
                                    if (!g_raiserDumped) {
                                        g_raiserDumped = 1;
                                        CreateDirectoryA(KDUMP_DIR, NULL);
                                        FILE *rf = fopen(KDUMP_DIR "\\bugcheck_raiser.bin", "wb");
                                        if (rf) {
                                            unsigned char pg[0x1000];
                                            UINT64 o; int ok = 0;
                                            for (o = 0; o < 0x200000ULL; o += 0x1000) {   /* 2MB is ample for acpi.sys */
                                                if (kernelReadVA((unsigned char *)guestMemory, bcCr3, cb + o, pg, sizeof(pg))) ok++;
                                                else memset(pg, 0, sizeof(pg));
                                                fwrite(pg, 1, sizeof(pg), rf);
                                            }
                                            fclose(rf);
                                            printf("\n[u50] dumped raising module (base 0x%llX) to bugcheck_raiser.bin (%d/512 pages readable)",
                                                   (unsigned long long)cb, ok);
                                        }
                                    }
                                }
                                printf("\n[BUGCHECK]   stack (canonical kernel qwords):\n");
                                int bsk;
                                for (bsk = 0; bsk < (int)sizeof(bcStk); bsk += 8) {
                                    UINT64 qv = *(UINT64 *)&bcStk[bsk];
                                    if (qv >= 0xFFFF800000000000ULL) {
                                        UINT64 cb2 = 0;
                                        printf("[BUGCHECK]     [rsp+0x%02X]=0x%llX", bsk, (unsigned long long)qv);
                                        if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, bcCr3, qv, &cb2))
                                            printf("  (RVA 0x%llX)", (unsigned long long)(qv - cb2));
                                        printf("\n");
                                    }
                                }
                            }
                            // U22: for 0x139, KeBugCheckEx(code, p1, p2=trap frame,
                            // p3=exception record). The DETECTOR (code that raised
                            // __fastfail) lives in these, NOT on KeBugCheckEx's own
                            // stack above. EXCEPTION_RECORD.ExceptionAddress (+0x10)
                            // = the faulting instruction; KTRAP_FRAME.Rip (+0x168) /
                            // .Rsp (+0x180) give the real crashing context -- walk
                            // that Rsp for the detector's own call chain.
                            if ((bcVals[0].Reg64 & 0xFFFFFFFFULL) == 0x139) {
                                UINT64 trapFrame = bcVals[2].Reg64;  // p2 = r8
                                UINT64 excRec    = bcVals[3].Reg64;  // p3 = r9
                                UINT64 cb3 = 0;
                                unsigned char erBuf[0x28] = { 0 };
                                if (kernelReadVA((unsigned char *)guestMemory, bcCr3, excRec, erBuf, sizeof(erBuf))) {
                                    UINT64 excAddr = *(UINT64 *)&erBuf[0x10];
                                    printf("[BUGCHECK]   0x139 EXCEPTION_RECORD.ExceptionAddress=0x%llX", (unsigned long long)excAddr);
                                    if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, bcCr3, excAddr, &cb3))
                                        printf("  (RVA 0x%llX)", (unsigned long long)(excAddr - cb3));
                                    printf("  <== DETECTOR\n");
                                }
                                unsigned char tfBuf[0x190] = { 0 };
                                if (kernelReadVA((unsigned char *)guestMemory, bcCr3, trapFrame, tfBuf, sizeof(tfBuf))) {
                                    UINT64 tfRip = *(UINT64 *)&tfBuf[0x168];
                                    UINT64 tfRsp = *(UINT64 *)&tfBuf[0x180];
                                    UINT64 cb4 = 0;
                                    printf("[BUGCHECK]   0x139 TRAP_FRAME.Rip=0x%llX", (unsigned long long)tfRip);
                                    if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, bcCr3, tfRip, &cb4))
                                        printf("  (RVA 0x%llX)", (unsigned long long)(tfRip - cb4));
                                    printf("  Rsp=0x%llX\n", (unsigned long long)tfRsp);
                                    // U23b: dump the whole trap frame as qwords so
                                    // the corrupted LIST_ENTRY pointer + its bad
                                    // neighbor can be located empirically (fixed GPR
                                    // offsets were wrong last time -- only rcx=3 was
                                    // right). For each canonical kernel qword, also
                                    // read the 0x10 bytes it points to (Flink/Blink)
                                    // and flag any whose back-links are inconsistent
                                    // -- that is the corrupted entry, and its bad
                                    // value fingerprints the writer.
                                    // U25b: raw dump of the whole trap frame to
                                    // calibrate GPR offsets (find where rcx=3 lives,
                                    // hence rdx=the subsegment) -- prior fixed guesses
                                    // (rax@0x30 etc.) gave rdx=0 which is impossible
                                    // for the remove-path fault.
                                    printf("[BUGCHECK]   0x139 raw trap-frame qwords 0x00-0xF8:\n");
                                    int rtk;
                                    for (rtk = 0x00; rtk <= 0xF8; rtk += 8) {
                                        UINT64 rv = *(UINT64 *)&tfBuf[rtk];
                                        printf("[BUGCHECK]     tf+0x%02X=0x%016llX%s\n", rtk, (unsigned long long)rv,
                                               (rv == 3) ? "  <== (==3, likely rcx/fastfail code)" :
                                               (rv >= 0xFFFF800000000000ULL ? "  (canonical)" : ""));
                                    }
                                    printf("[BUGCHECK]   0x139 full trap-frame scan (off: value -> [Flink,Blink] consistency):\n");
                                    int tfk;
                                    for (tfk = 0x28; tfk <= 0x180; tfk += 8) {
                                        UINT64 v = *(UINT64 *)&tfBuf[tfk];
                                        if (v < 0xFFFF800000000000ULL) continue;
                                        UINT64 cbv = 0;
                                        printf("[BUGCHECK]     tf+0x%02X=0x%llX", tfk, (unsigned long long)v);
                                        if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, bcCr3, v, &cbv)) { printf(" (RVA 0x%llX)", (unsigned long long)(v-cbv)); printf("\n"); continue; }
                                        unsigned char le[0x10] = { 0 };
                                        if (kernelReadVA((unsigned char *)guestMemory, bcCr3, v, le, sizeof(le))) {
                                            UINT64 fl = *(UINT64 *)&le[0x00], bl = *(UINT64 *)&le[0x08];
                                            unsigned char nb[0x10] = { 0 }; UINT64 flBl = ~v, blFl = ~v;
                                            if (fl >= 0xFFFF800000000000ULL && kernelReadVA((unsigned char *)guestMemory, bcCr3, fl, nb, sizeof(nb))) flBl = *(UINT64 *)&nb[0x08];
                                            if (bl >= 0xFFFF800000000000ULL && kernelReadVA((unsigned char *)guestMemory, bcCr3, bl, nb, sizeof(nb))) blFl = *(UINT64 *)&nb[0x00];
                                            int bad = (fl >= 0xFFFF800000000000ULL && bl >= 0xFFFF800000000000ULL && (flBl != v || blFl != v));
                                            printf(" Flink=0x%llX Blink=0x%llX%s\n", (unsigned long long)fl, (unsigned long long)bl,
                                                   bad ? "  <== CORRUPT LIST_ENTRY (back-links mismatch)" : "");
                                            if (bad)
                                                printf("[BUGCHECK]       Flink.Blink=0x%llX Blink.Flink=0x%llX (both should==0x%llX)\n",
                                                       (unsigned long long)flBl, (unsigned long long)blFl, (unsigned long long)v);
                                        } else printf(" (unreadable)\n");
                                    }
                                    unsigned char csBuf[0x400] = { 0 };
                                    if (kernelReadVA((unsigned char *)guestMemory, bcCr3, tfRsp, csBuf, sizeof(csBuf))) {
                                        printf("[BUGCHECK]   0x139 crashing-thread stack scan (0x400, broadened LIST_ENTRY test):\n");
                                        int csk;
                                        for (csk = 0; csk < (int)sizeof(csBuf); csk += 8) {
                                            UINT64 qv = *(UINT64 *)&csBuf[csk];
                                            if (qv < 0xFFFF800000000000ULL) continue;
                                            UINT64 cb5 = 0;
                                            if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, bcCr3, qv, &cb5)) continue; // skip code ptrs (return addrs)
                                            // Candidate heap pointer. Read its would-be
                                            // LIST_ENTRY and test consistency. BROADENED:
                                            // a link that is non-null but non-canonical
                                            // (garbage/ASCII/poison) is itself corruption,
                                            // as is a canonical link whose back-ptr misses.
                                            unsigned char le[0x10] = { 0 };
                                            if (!kernelReadVA((unsigned char *)guestMemory, bcCr3, qv, le, sizeof(le))) continue;
                                            UINT64 fl = *(UINT64 *)&le[0x00], bl = *(UINT64 *)&le[0x08];
                                            int flCanon = (fl >= 0xFFFF800000000000ULL), blCanon = (bl >= 0xFFFF800000000000ULL);
                                            int flNullOrCanon = (fl == 0) || flCanon, blNullOrCanon = (bl == 0) || blCanon;
                                            // A real linked subsegment has BOTH links canonical. Only
                                            // consider entries that look like list nodes (at least one
                                            // canonical link) to cut noise.
                                            if (!flCanon && !blCanon) continue;
                                            unsigned char nb[0x10] = { 0 }; UINT64 flBl = 0, blFl = 0; int flBlOk = 0, blFlOk = 0;
                                            if (flCanon && kernelReadVA((unsigned char *)guestMemory, bcCr3, fl, nb, sizeof(nb))) { flBl = *(UINT64 *)&nb[0x08]; flBlOk = 1; }
                                            if (blCanon && kernelReadVA((unsigned char *)guestMemory, bcCr3, bl, nb, sizeof(nb))) { blFl = *(UINT64 *)&nb[0x00]; blFlOk = 1; }
                                            int bad = (!flNullOrCanon) || (!blNullOrCanon)
                                                      || (flBlOk && flBl != qv) || (blFlOk && blFl != qv);
                                            if (!bad) continue; // only print suspected-corrupt nodes
                                            printf("[BUGCHECK]     [Rsp+0x%03X]=0x%llX Flink=0x%llX Blink=0x%llX  <== SUSPECT\n",
                                                   csk, (unsigned long long)qv, (unsigned long long)fl, (unsigned long long)bl);
                                            if (flBlOk) printf("[BUGCHECK]         Flink.Blink=0x%llX (want 0x%llX)%s\n", (unsigned long long)flBl, (unsigned long long)qv, (flBl!=qv)?"  MISMATCH":"");
                                            if (blFlOk) printf("[BUGCHECK]         Blink.Flink=0x%llX (want 0x%llX)%s\n", (unsigned long long)blFl, (unsigned long long)qv, (blFl!=qv)?"  MISMATCH":"");
                                        }
                                    }
                                    // U24: recover the corrupted subsegment by frame
                                    // reconstruction (disasm_bucket.py). Caller
                                    // RtlpHpLfhBucketGetSubsegment sets rbx=rcx+0x18
                                    // (bucket list head) and calls with rdx=[rbx].
                                    // RtlpHpLfhOwnerMoveSubsegment's first insn spills
                                    // that rbx at [tfRsp+8] (rsp unchanged on the fast
                                    // path). So subsegment(rdx) = *(*(tfRsp+8)).
                                    unsigned char q8[8] = { 0 };
                                    if (kernelReadVA((unsigned char *)guestMemory, bcCr3, tfRsp + 8, q8, 8)) {
                                        UINT64 rbx = *(UINT64 *)q8;
                                        printf("[BUGCHECK]   0x139 recovered rbx(bucket listhead)=0x%llX\n", (unsigned long long)rbx);
                                        if (rbx >= 0xFFFF800000000000ULL && kernelReadVA((unsigned char *)guestMemory, bcCr3, rbx, q8, 8)) {
                                            UINT64 subseg = *(UINT64 *)q8;
                                            printf("[BUGCHECK]   0x139 recovered SUBSEGMENT(rdx)=0x%llX\n", (unsigned long long)subseg);
                                            unsigned char sb[0x40] = { 0 };
                                            if (subseg >= 0xFFFF800000000000ULL && kernelReadVA((unsigned char *)guestMemory, bcCr3, subseg, sb, sizeof(sb))) {
                                                UINT64 fl = *(UINT64 *)&sb[0x00], bl = *(UINT64 *)&sb[0x08];
                                                UINT64 cbf = 0, cbb = 0;
                                                printf("[BUGCHECK]     subseg.Flink=0x%llX", (unsigned long long)fl);
                                                if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, bcCr3, fl, &cbf)) printf("(RVA 0x%llX)", (unsigned long long)(fl-cbf));
                                                printf("  subseg.Blink=0x%llX", (unsigned long long)bl);
                                                if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, bcCr3, bl, &cbb)) printf("(RVA 0x%llX)", (unsigned long long)(bl-cbb));
                                                printf("\n");
                                                unsigned char nb[0x10] = { 0 };
                                                if (fl >= 0xFFFF800000000000ULL && kernelReadVA((unsigned char *)guestMemory, bcCr3, fl, nb, sizeof(nb)))
                                                    printf("[BUGCHECK]     subseg.Flink.Blink=0x%llX (should==0x%llX)%s\n", (unsigned long long)*(UINT64 *)&nb[0x08], (unsigned long long)subseg, (*(UINT64 *)&nb[0x08]!=subseg)?"  <== MISMATCH":"");
                                                if (bl >= 0xFFFF800000000000ULL && kernelReadVA((unsigned char *)guestMemory, bcCr3, bl, nb, sizeof(nb)))
                                                    printf("[BUGCHECK]     subseg.Blink.Flink=0x%llX (should==0x%llX)%s\n", (unsigned long long)*(UINT64 *)&nb[0x00], (unsigned long long)subseg, (*(UINT64 *)&nb[0x00]!=subseg)?"  <== MISMATCH":"");
                                                printf("[BUGCHECK]     subseg raw 0x40 bytes:\n");
                                                int rb; for (rb = 0; rb < 0x40; rb += 0x10)
                                                    printf("[BUGCHECK]       +0x%02X: %016llX %016llX\n", rb, (unsigned long long)*(UINT64 *)&sb[rb], (unsigned long long)*(UINT64 *)&sb[rb+8]);
                                            }
                                        }
                                    }
                                }
                            }
                            fflush(stdout);
                        }
                        // U21: one-shot per pass -- a bugcheck fires once and the
                        // system halts; disarm DR1 immediately after the first hit
                        // so a resume-flag re-fault at the entry instruction can't
                        // spew thousands of identical duplicates. Re-armed on reset.
                        WHV_REGISTER_NAME dr6ClearName4 = WHvX64RegisterDr6;
                        WHV_REGISTER_VALUE dr6ClearVal4 = { 0 };
                        WHvSetVirtualProcessorRegisters(partition, 0, &dr6ClearName4, 1, &dr6ClearVal4);
                        {
                            WHV_REGISTER_NAME dr7ReadName4 = WHvX64RegisterDr7;
                            WHV_REGISTER_VALUE dr7ReadVal4 = { 0 };
                            WHvGetVirtualProcessorRegisters(partition, 0, &dr7ReadName4, 1, &dr7ReadVal4);
                            dr7ReadVal4.Reg64 &= ~0x4ULL; // clear L1
                            WHV_REGISTER_NAME dr7WriteName4 = WHvX64RegisterDr7;
                            WHvSetVirtualProcessorRegisters(partition, 0, &dr7WriteName4, 1, &dr7ReadVal4);
                            g_helperEntryArmed = 0;
                        }
                        break;
                    }

                    // DR2 (U28): RtlpHpLfhBucketGetSubsegment+0x45 -- rdx = the
                    // subsegment ([rbx]) about to be handed to MoveSubsegment.
                    // Registers read fine here. Validate rdx's LIST_ENTRY; the call
                    // whose subsegment is ALREADY corrupt is the one that fastfails,
                    // so this captures the victim VA + the corrupt values (which
                    // fingerprint the stray writer). Throttled log; loud one-shot on
                    // the corrupt one.
                    if (wpRip == g_wpTargetVA && g_loopAdvArmed) {
                        g_loopAdvHitCount++;
                        // U30: at ExAllocateHeapPool after r10=*(*(rsp+0x58)). r10 =
                        // the LFH heap-context. When it is null, ptrB=[rsp+0x58] is
                        // the pointer whose target (the context slot) holds the null.
                        // Capture ptrB, its RVA/identity, and 0x40 around it + around
                        // the slot it points to -- pins the corrupted structure/field.
                        WHV_REGISTER_NAME gnames[3] = { WHvX64RegisterR10, WHvX64RegisterCr3, WHvX64RegisterRsp };
                        WHV_REGISTER_VALUE gvals[3] = { 0 };
                        if (SUCCEEDED(WHvGetVirtualProcessorRegisters(partition, 0, gnames, 3, gvals))) {
                            UINT64 ctx = gvals[0].Reg64, gCr3 = gvals[1].Reg64, gRsp = gvals[2].Reg64;
                            if (g_loopAdvHitCount <= 8 || (g_loopAdvHitCount % 2000) == 0)
                                printf("[EXHEAP] #%d exitCount=%ld: r10(context)=0x%llX\n", g_loopAdvHitCount, exitCount, (unsigned long long)ctx);
                            if (ctx < 0xFFFF800000000000ULL) {
                                unsigned char q8[8] = { 0 };
                                UINT64 ptrB = 0;
                                if (kernelReadVA((unsigned char *)guestMemory, gCr3, gRsp + 0x58, q8, 8)) ptrB = *(UINT64 *)q8;
                                printf("\n[EXHEAP-NULLCTX] #%d exitCount=%ld: LFH context is NULL (r10=0x%llX). ptrB([rsp+0x58])=0x%llX\n",
                                       g_loopAdvHitCount, exitCount, (unsigned long long)ctx, (unsigned long long)ptrB);
                                UINT64 pcb = 0;
                                if (ptrB >= 0xFFFF800000000000ULL) {
                                    if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, gCr3, ptrB, &pcb)) printf("[EXHEAP-NULLCTX]   ptrB in module RVA 0x%llX (a global/.data ptr array)\n", (unsigned long long)(ptrB - pcb));
                                    else printf("[EXHEAP-NULLCTX]   ptrB is a heap/pool pointer\n");
                                    unsigned char raw[0x40] = { 0 };
                                    if (kernelReadVA((unsigned char *)guestMemory, gCr3, ptrB, raw, sizeof(raw))) {
                                        printf("[EXHEAP-NULLCTX]   *ptrB raw 0x40 (the context slot region -- offset 0 is the null ctx):\n");
                                        int rr; for (rr = 0; rr < 0x40; rr += 0x10)
                                            printf("[EXHEAP-NULLCTX]     +0x%02X: %016llX %016llX\n", rr, (unsigned long long)*(UINT64 *)&raw[rr], (unsigned long long)*(UINT64 *)&raw[rr+8]);
                                    }
                                }
                                fflush(stdout);
                                WHV_REGISTER_NAME dr7r5 = WHvX64RegisterDr7; WHV_REGISTER_VALUE dr7v5 = { 0 };
                                WHvGetVirtualProcessorRegisters(partition, 0, &dr7r5, 1, &dr7v5);
                                dr7v5.Reg64 &= ~0x10ULL;
                                WHV_REGISTER_NAME dr7w5 = WHvX64RegisterDr7;
                                WHvSetVirtualProcessorRegisters(partition, 0, &dr7w5, 1, &dr7v5);
                                g_loopAdvArmed = 0;
                            }
                            fflush(stdout);
                        }
                        WHV_REGISTER_NAME dr6ClearName5 = WHvX64RegisterDr6;
                        WHV_REGISTER_VALUE dr6ClearVal5 = { 0 };
                        WHvSetVirtualProcessorRegisters(partition, 0, &dr6ClearName5, 1, &dr6ClearVal5);
                        break;
                    }

                    const char *wpLabel = NULL;
                    int wpSlot = -1;

                    if (wpSlot >= 0) {
                        g_wpHitCount++;
                        printf("\n[wp] ==== EXECUTION BREAKPOINT HIT #%d: %s (rip=0x%llX) exitCount=%ld ====\n",
                               g_wpHitCount, wpLabel, (unsigned long long)wpRip, exitCount);

                        WHV_REGISTER_NAME wpAllNames[19] = {
                            WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRbx,
                            WHvX64RegisterRsp, WHvX64RegisterRbp, WHvX64RegisterRsi, WHvX64RegisterRdi,
                            WHvX64RegisterR8,  WHvX64RegisterR9,  WHvX64RegisterR10, WHvX64RegisterR11,
                            WHvX64RegisterR12, WHvX64RegisterR13, WHvX64RegisterR14, WHvX64RegisterR15,
                            WHvX64RegisterRflags, WHvX64RegisterCr0, WHvX64RegisterCr3
                        };
                        WHV_REGISTER_VALUE wpAllVals[19] = { 0 };
                        HRESULT wpHr = WHvGetVirtualProcessorRegisters(partition, 0, wpAllNames, 19, wpAllVals);
                        if (SUCCEEDED(wpHr)) {
                            static const char *wpNames2[19] = {
                                "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                                "r8","r9","r10","r11","r12","r13","r14","r15",
                                "rflags","cr0","cr3"
                            };
                            int wni;
                            for (wni = 0; wni < 19; wni++) {
                                printf("  %-6s = 0x%016llX\n", wpNames2[wni], (unsigned long long)wpAllVals[wni].Reg64);
                            }
                            UINT64 wpCr3 = wpAllVals[18].Reg64;
                            printf("  rip    = 0x%016llX\n", (unsigned long long)wpRip);

                            if (wpSlot == 1) {
                                // RIP is AT the function's own entry point,
                                // not yet executed -- the standard x86 CALL
                                // (direct, indirect, or table-based; all
                                // three forms push identically) already put
                                // the return address at [rsp+0x00]. That's
                                // the caller, resolved below by the generic
                                // stack scan.
                                printf("[wp] caller (return address) should be at [rsp+0x00] below\n");
                            }

                            UINT64 wpRsp = wpAllVals[4].Reg64;
                            unsigned char wpStackBuf[0x80] = { 0 };
                            if (kernelReadVA((unsigned char *)guestMemory, wpCr3, wpRsp, wpStackBuf, sizeof(wpStackBuf))) {
                                printf("[wp] scanning stack from rsp=0x%llX:\n", (unsigned long long)wpRsp);
                                int wsi;
                                for (wsi = 0; wsi < (int)sizeof(wpStackBuf); wsi += 8) {
                                    UINT64 qv = *(UINT64 *)&wpStackBuf[wsi];
                                    if (qv >= 0xFFFF800000000000ULL) {
                                        UINT64 candBase3 = 0;
                                        printf("  [rsp+0x%02X] = 0x%llX", wsi, (unsigned long long)qv);
                                        if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, wpCr3, qv, &candBase3)) {
                                            printf("    ^ module base 0x%llX RVA 0x%llX", (unsigned long long)candBase3, (unsigned long long)(qv - candBase3));
                                        }
                                        printf("\n");
                                    }
                                }
                            }
                        } else {
                            printf("[wp] WHvGetVirtualProcessorRegisters failed: 0x%lx\n", wpHr);
                        }
                        printf("[wp] ==== end breakpoint report ====\n\n");
                        fflush(stdout);

                        // Disable ONLY this slot's DR7 L-bit (one-shot),
                        // leaving the other still-armed sites untouched so
                        // this single pass can go on to capture them too.
                        WHV_REGISTER_NAME dr7ReadName = WHvX64RegisterDr7;
                        WHV_REGISTER_VALUE dr7ReadVal = { 0 };
                        WHvGetVirtualProcessorRegisters(partition, 0, &dr7ReadName, 1, &dr7ReadVal);
                        UINT64 lbit = (wpSlot == 0) ? 0x1ULL : (wpSlot == 1) ? 0x4ULL : 0x10ULL;
                        dr7ReadVal.Reg64 &= ~lbit;
                        WHV_REGISTER_NAME clearNames[2] = { WHvX64RegisterDr7, WHvX64RegisterDr6 };
                        WHV_REGISTER_VALUE clearVals[2] = { dr7ReadVal, { 0 } };
                        WHvSetVirtualProcessorRegisters(partition, 0, clearNames, 2, clearVals);

                        if (wpSlot == 0) g_wp0Fired = 1;
                        else if (wpSlot == 1) g_wp1Fired = 1;
                        else g_wp2Fired = 1;

                        if (g_wp0Fired && g_wp1Fired && g_wp2Fired) {
                            g_wpArmed = 0;
                        }
                        break;
                    }

                    // Spurious/unmatched #DB while armed (shouldn't happen
                    // in practice -- each site is disabled the instant it
                    // fires). Don't guess: just clear the sticky DR6
                    // status bits and resume at the same RIP without
                    // touching DR7, so whatever's still legitimately
                    // armed keeps working.
                    printf("[wp] unmatched #DB at rip=0x%llX while armed (wp0=%d wp1=%d wp2=%d) -- clearing DR6 and resuming\n",
                           (unsigned long long)wpRip, g_wp0Fired, g_wp1Fired, g_wp2Fired);
                    fflush(stdout);
                    WHV_REGISTER_NAME dr6ClearName = WHvX64RegisterDr6;
                    WHV_REGISTER_VALUE dr6ClearVal = { 0 };
                    WHvSetVirtualProcessorRegisters(partition, 0, &dr6ClearName, 1, &dr6ClearVal);
                    break;
                }

                // Live breakpoint for the bugcheck-0x139 investigation --
                // see the infrastructure block near the top of this file
                // and docs/investigations/post-vppt-boot-stall.md.
                // WHV reports VpContext.Rip for an exception exit as the
                // breakpoint's OWN address (unlike raw x86 INT3 trap
                // semantics, which would leave RIP one byte past it) --
                // confirmed empirically: the first version of this check
                // assumed +1 and rejected a genuine hit. Resuming at
                // g_bpTargetVA (no adjustment) after restoring the byte
                // is therefore already correct as written below.
                UINT64 hitRip = exitContext.VpContext.Rip;
                if (exitContext.VpException.ExceptionType != WHvX64ExceptionTypeBreakpointTrap ||
                    !g_bpPatched || hitRip != g_bpTargetVA) {
                    // Not our patched breakpoint. WHV apparently classifies
                    // any software "int n" trap (not just genuine INT3) as
                    // ExceptionType==BreakpointTrap -- confirmed live: the
                    // guest's own genuine `int 0x29` fast-fail (a 2-byte
                    // CD 29, not our 1-byte CC) exits here too. Treating
                    // every non-matching hit as fatal would stop the whole
                    // hypervisor on completely unrelated, legitimate traps
                    // elsewhere in the kernel (CFG/WPP/other fast-fails).
                    //
                    // Attempted faithful re-injection first (set a pending
                    // exception event for the real vector, read from the
                    // instruction bytes) -- WHV rejected it with
                    // InvalidVpRegisterValue (likely because #BP/INT-n are
                    // software-generated traps that this generic
                    // hardware-exception pending-event mechanism doesn't
                    // cleanly support without an explicit instruction-length
                    // field this struct doesn't expose). Fell back to the
                    // simpler, pragmatic choice: just skip past the
                    // instruction. Safe and standard for genuine INT3
                    // (0xCC) -- that's exactly what a "no debugger
                    // attached" system does with debug-check stubs anyway.
                    // For other "int n" (0xCD xx, e.g. an unrelated
                    // fast-fail elsewhere in the kernel), this means that
                    // OTHER failure's own detail is lost rather than
                    // properly reported -- an accepted, logged limitation,
                    // not a silent one.
                    unsigned char firstByte = exitContext.VpException.InstructionByteCount > 0
                        ? exitContext.VpException.InstructionBytes[0] : 0xCC;
                    UINT32 instrLen = (firstByte == 0xCD) ? 2 : 1;
                    // Throttled (2026-07-17, ExInitializePoolHeapManagement
                    // investigation): discovered live that some genuine,
                    // unrelated INT3 in the guest fires extremely often
                    // around early pass-2 boot (WPP/ETW-style tracepoint,
                    // most likely) -- this print was previously
                    // unconditional and, at that rate, the printf+fflush
                    // cost alone consumed nearly all wall-clock time
                    // (486,000+ log lines in a 240s run, guest barely
                    // progressing), which looked exactly like a guest-side
                    // stall until traced back to this. Same throttle
                    // pattern used elsewhere in this file for other
                    // high-frequency diagnostics.
                    g_nonBpHitCount++;
                    if (g_nonBpHitCount <= 20 || g_nonBpHitCount % 5000 == 0) {
                        printf("[bp] non-breakpoint #BP-class exception at rip=0x%llX (first bytes: 0x%02X...) -- skipping instruction (%s) [count=%d]\n",
                               (unsigned long long)hitRip, firstByte,
                               (firstByte == 0xCD) ? "WARNING: this was an unrelated int-n, likely losing detail on a different failure" : "genuine INT3, safe to skip",
                               g_nonBpHitCount);
                        fflush(stdout);
                    }

                    WHV_REGISTER_NAME skipName = WHvX64RegisterRip;
                    WHV_REGISTER_VALUE skipVal = { 0 };
                    skipVal.Reg64 = hitRip + instrLen;
                    WHvSetVirtualProcessorRegisters(partition, 0, &skipName, 1, &skipVal);
                    break;
                }

                g_bpHitCount++;

                // Phase1InitializationDiscard is expected to be hit at
                // most once per boot pass (a genuine one-time system
                // thread entry point on real Windows), so unlike the
                // high-frequency sites used earlier this session, every
                // hit gets a full report -- no throttling needed, and no
                // pre-learned instruction bytes to emulate: just restore
                // the original byte and let the real instruction execute
                // normally on resume (simpler and always correct,
                // regardless of what the real entry instruction turns out
                // to be). Does NOT permanently disable #BP interception --
                // g_bpPatched is cleared so discovery can re-arm fresh
                // after the next reset, to check pass 2 the same way.
                printf("\n[bp] ==== BREAKPOINT HIT #%d: Phase1InitializationDiscard ENTRY (RVA 0x%X) -- exitCount=%ld ====\n",
                       g_bpHitCount, BP_FAIL_SITE_RVA, exitCount);

                WHV_REGISTER_NAME pAllNames[19] = {
                    WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx, WHvX64RegisterRbx,
                    WHvX64RegisterRsp, WHvX64RegisterRbp, WHvX64RegisterRsi, WHvX64RegisterRdi,
                    WHvX64RegisterR8,  WHvX64RegisterR9,  WHvX64RegisterR10, WHvX64RegisterR11,
                    WHvX64RegisterR12, WHvX64RegisterR13, WHvX64RegisterR14, WHvX64RegisterR15,
                    WHvX64RegisterRflags, WHvX64RegisterCr0, WHvX64RegisterCr3
                };
                WHV_REGISTER_VALUE pAllVals[19] = { 0 };
                HRESULT pHr = WHvGetVirtualProcessorRegisters(partition, 0, pAllNames, 19, pAllVals);
                if (SUCCEEDED(pHr)) {
                    static const char *pNames[19] = {
                        "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
                        "r8","r9","r10","r11","r12","r13","r14","r15",
                        "rflags","cr0","cr3"
                    };
                    int pni;
                    for (pni = 0; pni < 19; pni++) {
                        printf("  %-6s = 0x%016llX\n", pNames[pni], (unsigned long long)pAllVals[pni].Reg64);
                    }
                    UINT64 pCr3 = pAllVals[18].Reg64;

                    UINT64 pRsp = pAllVals[4].Reg64;
                    unsigned char pStackBuf[0x80] = { 0 };
                    if (kernelReadVA((unsigned char *)guestMemory, pCr3, pRsp, pStackBuf, sizeof(pStackBuf))) {
                        printf("[bp] scanning stack from rsp=0x%llX:\n", (unsigned long long)pRsp);
                        int psi;
                        for (psi = 0; psi < (int)sizeof(pStackBuf); psi += 8) {
                            UINT64 qv = *(UINT64 *)&pStackBuf[psi];
                            if (qv >= 0xFFFF800000000000ULL) {
                                UINT64 candBase4 = 0;
                                printf("  [rsp+0x%02X] = 0x%llX", psi, (unsigned long long)qv);
                                if (kernelDiagIdentifyModuleAt((unsigned char *)guestMemory, pCr3, qv, &candBase4)) {
                                    printf("    ^ module base 0x%llX RVA 0x%llX", (unsigned long long)candBase4, (unsigned long long)(qv - candBase4));
                                }
                                printf("\n");
                            }
                        }
                    }
                } else {
                    printf("[bp] WHvGetVirtualProcessorRegisters failed: 0x%lx\n", pHr);
                }
                printf("[bp] ==== end breakpoint report ====\n\n");
                fflush(stdout);

                if (guestMemory && kernelWriteByteVA((unsigned char *)guestMemory, g_bpCr3, g_bpTargetVA, g_bpOriginalByte)) {
                    g_bpPatched = 0; // allow re-arm on the next reset, for pass 2
                }
                {
                    WHV_REGISTER_NAME ripBackName2 = WHvX64RegisterRip;
                    WHV_REGISTER_VALUE ripBackVal2 = { 0 };
                    ripBackVal2.Reg64 = g_bpTargetVA;
                    WHvSetVirtualProcessorRegisters(partition, 0, &ripBackName2, 1, &ripBackVal2);
                }
                break;
            }

            default:
                printf("\n[Unhandled exit reason: %d -- stopping]\n", exitContext.ExitReason);
                running = 0;
                break;
        }
    }

    if (ataDiskFile) fclose(ataDiskFile);
    VirtualFree(guestMemory, 0, MEM_RELEASE);
    if (hmaMemory) VirtualFree(hmaMemory, 0, MEM_RELEASE);
    if (uefiFirmwareMemory) VirtualFree(uefiFirmwareMemory, 0, MEM_RELEASE);
    WHvDeletePartition(partition);
    return 0;
}



