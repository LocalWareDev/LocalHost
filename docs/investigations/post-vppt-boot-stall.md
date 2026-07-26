# Post-VPPT-Fix Boot Stall

**Status (2026-07-17, later update):** The early-boot regression described
below (originally blamed on accumulated Windows Hypervisor Platform host
state, "test via reboot") turned out to have a different, confirmed root
cause, found and fixed after the reboot — see "2026-07-17 update: the
early-boot regression's real root cause" near the end of this doc. Boot
now reliably reaches `BdsDxe` and the guest's own mid-install reset again,
landing back on the original Segment Heap bug this investigation exists to
solve (still open — see below). The breakpoint-infrastructure fixes
mentioned in the superseded status note below (re-arm logic, `SCI_EN`
pinning) are both kept and now verified end-to-end, since boot can get
past them again.

**Status (Segment Heap bug, still open):** Two distinct problems, one solved
reliably and one still open — see the full history below. **Major update,
2026-07-17 (post-regression continuation):** traced the NULL-parameter
chain five further call-chain levels, each independently confirmed against
a downloaded matching `ntkrnlmp.pdb` and a real disassembler (capstone) --
see "2026-07-17 update: the Segment Heap bug, traced five call-chain
levels with real symbols" near the end of this doc for the full trace.
Root *mechanism* now fully confirmed end-to-end:
`ExPoolState+0x3900+(index*0x20C0)+0x10` (a per-pool-type/NUMA-node
heap-descriptor slot) is NULL when `ExAllocateHeapPool` reads it, and that
NULL propagates through `RtlpHpLfhSlotAllocate` →
`RtlpHpLfhBucketGetSubsegment` → `RtlpHpLfhOwnerMoveSubsegment`'s
safe-unlink check, which correctly detects the resulting invariant
violation and fast-fails. **Follow-up survey (same date): all 16 sampled
indices of this array are NULL, not just the one the failing call used**
-- systemic, not a single bad slot. **Further update (same date): located
the boot-time initializer, `ExInitializePoolHeapManagement` (confirmed via
disassembly to write into this exact array), but it populates a
*different* field (`+8`) than the one found NULL (`+0x10`) -- the specific
lazily-created heap variant the failing allocation needs is populated by a
still-unidentified separate path.** No teardown/destroy-pool function
exists anywhere in the public symbol table, weakly favoring "never
(re-)triggered" over "explicitly cleared." **Latest update (same date):
a whole-kernel static scan (16MB image dump + capstone, offline) found no
function anywhere in ntoskrnl.exe that writes the `+0x10` field via its
own fixed address -- and a live survey confirmed the field genuinely IS
populated throughout pass 1's own operation (1/16 entries, stable from
early boot on), ruling out "unexercised slot." `ExAllocateHeapPool`
itself, now fully disassembled, gracefully detects the NULL case and
falls back rather than crashing (a low-address dereference reading `0`
instead of faulting, the same environment quirk seen once before in this
investigation) but never creates the missing variant -- meaning the real
creator receives the slot's address as a parameter from a still-untraced
call chain.** **DECISIVE, 2026-07-17: a hardware data watchpoint (DR0/DR7)
on the exact slot, armed fresh across the entire second boot pass (reset
through crash), never fired.** This directly, empirically proves nothing
ever writes this slot during pass 2 -- not "initialized then cleared"
(conclusively ruled out), but a genuine reset-time regression where
whatever writes it during pass 1 simply never runs a second time. **The
writer is identified: `ExInitializePagedHeaps`** (resolved from the
watchpoint's own hit RIP), the Paged-pool counterpart to the already-known
`ExInitializePoolHeapManagement` (NonPaged). Root cause chain is now
complete, name-to-name, from this one-time boot initializer through to the
`bugcheck 0x139` fast-fail. **Confirmed systemic, not Paged-specific: the
same watchpoint retargeted to the NonPaged slot (`ExInitializePoolHeapManagement`)
shows the identical pattern -- fires once in pass 1, never again in pass
2.** **Earliest divergence point found, 2026-07-17: `Phase1InitializationDiscard`
-- the real NT system thread that runs ALL of Phase 1 kernel
initialization -- itself never runs during pass 2** (confirmed via a
hardware execution breakpoint, clean/unthrottled). Both pool-heap findings
are fully explained as downstream consequences of this single fact, not
independent gaps. This is a general reset-fidelity gap, not a single
divergent code path. **RESOLVED, 2026-07-18: the Phase 1 system thread is
never created (case 1 of 3), and the divergence is earlier still than the
`PsCreateSystemThread` call itself.** Three simultaneous hardware execution
breakpoints -- on `PspInitPhase0`'s `PsCreateSystemThread` call-return site,
on `Phase1Initialization`'s own entry, and on `Phase1InitializationDiscard`'s
entry -- all fired correctly, in order, within 3 VM exits of each other on
pass 1 (`PsCreateSystemThread` returned `STATUS_SUCCESS`). On pass 2, armed
across the entire second pass (reset through bugcheck, ~100,000 VM exits,
the same order of magnitude as pass 1's total), **none of the three ever
fired.** Since the call-return breakpoint alone never fires, `PspInitPhase0`
never even reaches its call to `PsCreateSystemThread` on pass 2 -- the
thread is not merely "created but not scheduled" or "scheduled but
diverted," it is never attempted. **Not yet fixed** -- per explicit
instruction this phase stopped here for architecture reassessment rather
than continuing into what in `PspInitPhase0` (or its own caller chain)
diverts control flow before that point. See "2026-07-18 update: the
three-way breakpoint experiment -- thread creation is never attempted"
near the end of this doc for the full experiment, "2026-07-17 update:
hardware data watchpoint -- decisive, direct proof", "2026-07-17 update:
the writer identified", "2026-07-17 update: systemic, not Paged-specific",
and "2026-07-17 update: the earliest divergence point" for the findings
that led here, "2026-07-17 update: the +0x10 slot IS used on pass 1" for
the disassembly work that preceded those, and "2026-07-17 update: locating
the initializer" for the earlier `ExInitializePoolHeapManagement` work.
**Follow-up, 2026-07-18: the apparent "pass 2 does substantial work despite
Phase 1 never running" tension is substantially resolved** -- directly
examining the pass-2 VM-exit trace (rather than just its length) shows it's
almost entirely pre-kernel bootloader activity, with kernel-space execution
confirmed only in a short span immediately before the crash (which
independently resolves to the standard post-bugcheck HAL halt routine).
**Second follow-up, same date, model-revising: the actual caller of
`ExInitializePagedHeaps`/`ExInitializePoolHeapManagement` is
`MiInitSystem`/`MiInitNucleus` (via `MmInitSystem`), reached from
`InitBootProcessor` *before* `PsInitSystem`/`PspInitPhase0` -- entirely
within Phase 0, independent of the Phase 1 thread.** This overturns the
previously-reported causal chain ("thread never created -> heap managers
never initialized"): both failures are now believed to be parallel
symptoms of one earlier, still-unlocated divergence, likely at or before
`MmInitSystem` itself -- meaningfully earlier than the
`PspInitPhase0+0x86D` point previously reported as "earliest." **Third
follow-up, same date, directly confirmed: `MmInitSystem` itself is never
reached on pass 2 either** (armed live across both passes; fires cleanly
on pass 1, silent on pass 2 through to the bugcheck, which independently
reproduced the identical `HaliHaltSystem+0x12` halt on a different KASLR
base). The earliest confirmed divergence is now `InitBootProcessor+0x575`
(`MmInitSystem`'s call site) or earlier -- whether it's inside
`InitBootProcessor`'s own sequence or further back in
`KiInitializeKernel`/`KiSystemStartup` is the next open question. **Fourth
follow-up, same date, narrowed to a single function's call tree: pass 2
confirmed reaches `HalpApicInitializeIoUnit` (inside `HalInitSystem`,
called from `InitBootProcessor+0x312`), but `HalInitSystem` never returns
to its caller.** A static read suggested a conditional bugcheck-on-failure
branch right after the call; a live breakpoint at that exact check
falsified it directly -- `HalInitSystem` doesn't return `FALSE`, it
doesn't return at all. The divergence is now within `HalInitSystem`'s own
execution, after `HalpApicInitializeIoUnit`. **Fifth follow-up, same date,
now pinned to one specific real Windows HAL function:
`HalpIommuInitSystem` (Intel VT-d / DMAR IOMMU bring-up) never succeeds on
pass 2.** `HalInitSystem` turned out to dispatch through a CFG-guarded,
table-driven loop; a multi-shot live trace of that dispatch call site
showed pass 1 eventually advancing past `HalpIommuInitSystem` to a second
HAL subsystem (proving it terminates normally), while a 3000-hit pass-2
capture never advances at all -- identical target, identical stack depth,
throughout, right up to the bugcheck. Every previously-reported "earliest
divergence" today is now believed to be a downstream symptom of this one
point. **Sixth follow-up, same date, closing the loop: the eventual
`0x139` crash's own call stack does NOT run through `HalpIommuInitSystem`'s
own code -- the immediate caller of `ExAllocatePoolWithTag` right before
the bugcheck is `KiSwInterruptDispatch`, unrelated software-interrupt
dispatch code.** The mechanism is one level more indirect than first
speculated: `HalpIommuInitSystem`'s retry loop prevents `MmInitSystem`
from ever initializing the Segment Heap, leaving *any* pool allocation
that happens to run during that unbounded window vulnerable -- not just
IOMMU-related ones. This directly connects the missing Phase 1 thread, the
uninitialized heap managers, and the `0x139` bugcheck into one single
story. **Seventh follow-up, same date, correcting rather than narrowing:
`HalpIommuInitSystem` was never actually failing.** Live traces of its
real arguments and return value show it returns success on *every* call,
both passes -- and full disassembly shows the parameter being passed (`9`)
never reaches its real IOMMU logic at all (it short-circuits to a
near-no-op return first). The stalled dispatch loop is real and still the
earliest known divergence, but *why* it stalls is now an open loop-control
question, not a hardware-detection one -- hardware-fidelity analysis (ACPI
DMAR tables, CPUID/MSR bits) is no longer the productive next step. See
"2026-07-18 update: resolving the 'substantial work' tension", "2026-07-18
update: U6 resolved -- the heap initializers are Phase 0, not Phase 1",
"2026-07-18 update: U8 resolved -- MmInitSystem itself is never reached on
pass 2", "2026-07-18 update: U9 resolved -- the divergence is inside
HalInitSystem itself", "2026-07-18 update: U10 resolved --
HalpIommuInitSystem never succeeds on pass 2" (superseded by the seventh
follow-up), "2026-07-18 update: U12 resolved -- the crash is a routine
allocation caught by the vulnerability window", and "2026-07-18 update:
U13 corrected, not resolved -- HalpIommuInitSystem was never actually
failing" near the end of this doc, and
[phase0-divergence-summary.md](phase0-divergence-summary.md) for the
consolidated, currently-accurate fact/inference/unknown breakdown.

---

**Status (Segment Heap bug, pre-2026-07-17):** Two distinct problems, one solved reliably and one still open.
The guest is attempting a **system reset** (very plausibly Windows
Setup's own mid-installation restart) via the classic legacy
keyboard-controller reset pulse (byte `0xFE` to port `0x64`); this
hypervisor previously implemented no effect for that write, causing an
unbounded deadlock. **Fixed, reliably, confirmed across many independent
runs:** a CPU-register reset (RIP/CS/CR0/CR3/CR4/EFER back to the same
reset-vector state used at cold boot, reusing the existing port-0x92
hot-reset mechanism) is now permanently implemented and gets the guest
genuinely restarting every time this has been tested.

**Not solved, still open:** honoring the reset exposes a *second*,
different problem — the guest reaches `BdsDxe: starting Boot0002` again
(re-entering the Windows Boot Manager/installer path) but then bugchecks
`0x139` (`KERNEL_SECURITY_CHECK_FAILURE`) in most runs, traced to a
corrupted `LIST_ENTRY` inside `RtlpHpLfhOwnerMoveSubsegment` (the kernel's
Segment Heap). **Four independent mitigation attempts were implemented
and tested live; none proved reliable across repeated trials**: clearing
all guest RAM above the legacy 16MB boundary (worked on the very first
test, but failed on two repeat trials — that first success was a lucky
run, not a fix), a narrower/faster 240MB clear (failed), resetting this
hypervisor's own `rtcPeriodicTickArmed`/`lastRtcPeriodicTick` state alone
(failed), and combining the RAM clear with the `rtcPeriodicTickArmed`
reset (also eventually failed, just later). All four were reverted; the
codebase currently contains only the CPU-register reset, nothing
RAM-related or RTC-related for this path. Also tried and explicitly
**ruled out**: resetting IOAPIC redirection-table state, which made
things reliably *worse* (an immediate, consistent freeze structurally
identical to the pre-fix VPPT/RTC interrupt-delivery gap) — the classic
keyboard-controller reset pulse is a CPU-only reset on real hardware, not
a full platform/chipset reset, so this state is deliberately left alone.

**The "shape matches the original VPPT/RTC bug" theory behind all four
mitigation attempts has since been directly disproven.** Live symbol
resolution of the stall's RIP (via `ntkrnlmp.pdb`, a one-off diagnostic
added and removed after answering the question) shows it resolves to
**`HaliHaltSystem`** — the HAL's terminal halt loop, reached only *after*
`KeBugCheckEx` has already fully completed. The oscillating RIP and
`HIGH_LEVEL` IRQL that looked like a live interrupt-wait race are simply
the ordinary signature of a frozen, already-bugchecked CPU. The same
diagnostic pass confirmed the guest's RTC periodic-interrupt-enable bit
(PIE) never even gets re-enabled before the bugcheck happens — the second
boot pass isn't waiting on an RTC tick at all. **The real, and only,
fault is exactly where the investigation had already correctly found it
before this detour: `RtlpHpLfhOwnerMoveSubsegment` (Segment Heap
corruption).**

**A fifth mitigation — completing the CPU-register reset with
`IDTR`/`GDTR`/`LDTR`/`TR`/`DR7`/segment registers (the remaining
architectural state real x86 `RESET#` reinitializes) — was implemented,
fixed after hitting an invalid-register-value rejection (`TR`'s
descriptor type), and tested across three independent trials.** Bugcheck
`0x139` still occurred every time, with inconsistent timing (~6.5s to
~50s post-reset) — this fix has been kept (it's a real, independent
correctness improvement) but is not a fix for this bug.

**Root cause identified via live INT3 breakpointing.** Built real
breakpoint infrastructure (patch `0xCC` at the fail site's live address,
intercept via WHV's exception-exit mechanism, capture full register state
and correlate with recent hypervisor activity) — see the "live
breakpointing" update below for the engineering detail, including two
non-obvious WHV API gotchas hit along the way. **The timing variability
that looked like a race was a red herring**: two independent captures at
the exact fail site show byte-for-byte identical register state
(`rbx=2, rsi=0x10, r9=8, r10=0, r12=0xFFFFFFFF, r13=0x340, r14=0xFF`,
identical `cr0`/`cr3`/`cr4`) — this is a **fully deterministic** code
path, not a probabilistic one; the varying wall-clock time was just noise
in *when* boot reaches this point, not randomness in *what* happens
there. Live stack-walk + symbol resolution against the trustworthy
`ntkrnlmp.pdb` identified the exact call chain:

```
RtlpHpLfhSlotAllocate+0xCA6        (heap allocation request)
  -> RtlpHpAcquireLockShared+0x2A   (lock acquisition)
  -> RtlpHpLfhBucketGetSubsegment+0x60   (retrieve a subsegment from a bucket)
    -> RtlpHpLfhOwnerMoveSubsegment(subsegment=NULL, owner, ...)   <- fails: NULL subsegment
```

`RtlpHpLfhBucketGetSubsegment` retrieves a subsegment from an LFH bucket
during a heap allocation and finds it unexpectedly `NULL`/empty, passing
that onward to a function whose own safe-unlink check correctly detects
the invariant violation and fires the fast-fail. This points at a
concrete Segment-Heap bucket-bookkeeping inconsistency (e.g. a "this
bucket has N free entries"-style count not matching what's actually
resident) rather than a generic memory-staleness or timing problem — none
of the five earlier state-completeness attempts targeted this
specifically. **Not yet fixed** — next step is locating and understanding
that specific bucket-tracking state. See the "live breakpointing" and
"root cause identified" updates below for the full trace.
**Affects:** WHV backend (`Hypervisor.c`), Windows guests, UEFI/OVMF boot path.
**Follows on from:** [`vppt-synic-blocker.md`](vppt-synic-blocker.md), whose
part 10 fix (RTC periodic-interrupt emulation) resolved bugcheck 0x5C. The
guest now boots past that point and reaches this new, different stall.

## Summary

After the VPPT/SynIC bugcheck fix, the guest progresses further into boot
but then stalls indefinitely (confirmed for 90+ real seconds, no recovery)
in a tight, symbol-less spin loop inside a small kernel-mode module with
no PE debug directory (so not directly nameable via the PDB-based
technique used throughout the prior investigation).

## Symptom

Stall watchdog signature, reproduced across independent boots:

```
rip = <module_base> + 0x1188
rsp = <per-boot address, e.g. 0xFFFF838F3E206930>
rax = 0x0
rcx = 0x64   (100 -- not obviously used by the spin loop itself; likely
              incidental context from before this function was entered)
rdx = 0x0
rbx = 0x0
cr3 = 0x1AD002   (same CR3 as the entire earlier investigation -- this is
                   still kernel/system address space, not a new process)
```

## What's confirmed so far

**1. The stalled code is a snapshot-and-spin busy-wait, disassembled
directly from a live dump (no symbols needed for this part):**

```
push rbp
mov  rbp, rsp
sub  rsp, 0x10
mov  rax, [rip+disp1]      ; rax = live value at target VA (snapshot)
mov  [rbp-8], rax           ; local = snapshot
loop:
  mov  rdx, [rbp-8]         ; rdx = local (the snapshot, unchanged)
  mov  rax, [rip+disp2]     ; rax = live value at target VA, re-read
  cmp  rdx, rax
  jne  done                 ; live value now differs from snapshot -> exit
  pause
  jmp  loop                 ; <-- RIP consistently caught here
done:
  leave
  ret
```

Both RIP-relative displacements resolve (verified independently from two
different instruction addresses, confirmed by hand) to the **same**
target VA — `RIP + 0x1060` relative to the tick-2 RIP sample (which is
consistently caught exactly at the `jmp loop` instruction). This is a
plain "wait until this memory location changes from its value when I
started waiting" spin, with **no bound, no timeout, and no fallback to a
real blocking wait** — if the target value never changes, this loops
forever.

**2. Confirmed a genuine deadlock, not just a slow legitimate wait.**
Polled the live value at the target VA on every stall tick across a
90-second observation window (45 samples, 2s apart): it was `0x0` at the
very first sample and remained exactly `0x0` for the entire window, never
changing. Reproduced with the same signature (RIP module offset, register
pattern) across independent boots.

**3. The containing module has no PE debug directory** — confirmed via
the same PE-header-walk technique used for `kernelDiagIdentifyModule`
elsewhere in this codebase (scan backward from RIP for `MZ`/`PE`, then
check the Debug Directory RVA in the optional header): it's zero, so
there's no CodeView/RSDS record and thus no PDB name to resolve via
dbghelp. RIP sits only `+0x1188` into the module, suggesting a small
image (a minimal driver, not `ntoskrnl.exe`/`hal.dll`, both of which are
large and richly symbol-equipped, as confirmed extensively in the prior
investigation using the exact same identification technique).

**4. Dumped 0x4000 bytes from the module base and searched for readable
strings** — inconclusive so far. That range is mostly `.text` (code), no
clear driver/product name string found; the identifying strings (if any)
are likely further into the image (`.rdata`/`.rsrc`), not yet dumped.

**5. Strong circumstantial lead: PCI configuration-space activity
immediately precedes the stall.** The main loop's heartbeat log (printed
every 5000 VM exits) shows `lastPort=0xCF8`/`0xCFC` (the standard x86 PCI
CONFIG_ADDRESS/CONFIG_DATA ports) right before the exit rate collapses:

```
[heartbeat: exitCount=195000 ... lastPort=0xCF8 write=1 val=0xC]
[heartbeat: exitCount=200000 ... lastPort=0xCF8 write=1 val=0xC]
[heartbeat: exitCount=205000 elapsedSec=15.7 lastPort=0xCF8 write=1 val=0xC]
```

Note the elapsed-time jump: only 5000 more exits between the last two
lines, but 10+ additional real seconds — consistent with the guest
entering the non-exiting spin loop right around this point. The guest was
actively enumerating PCI configuration space (`0xCF8` writes carrying a
small value, `0xC`, in the low byte — consistent with a bus/device/
function/register-offset encoding pointing at register offset `0xC`,
i.e. Cache Line Size/Latency Timer/Header Type/BIST in the standard PCI
header) immediately before stalling.

## Update: PCI enumeration confirmed clean; two hypotheses ruled out

Added a small ring buffer (`g_pciCfgRing`, 512 entries) recording every
PCI config-space access, dumped by the stall watchdog on detection — the
existing capped logging (first 300, then every 50000th) missed the
critical window entirely.

**PCI enumeration completes cleanly.** The ring buffer's absolute last
entry, every time, is `bus=0 dev=31 func=0 off=0x3C` — the final register
of the final device slot of a complete, correct bus-0 sweep (0 through
31). All real devices (host bridge, ISA bridge, PM function, AHCI,
RTL8139) read back sensible values; all empty slots correctly report
`0xFFFFFFFF`. **The deadlock begins immediately after enumeration
finishes, with zero further I/O of any kind before the freeze.** This
doesn't yet identify the cause, but rules out "the guest is stuck
mid-enumeration on a specific device" — it isn't.

**CPUID logical-processor-count mismatch, ruled out.** A concrete,
well-motivated hypothesis: CPUID leaf 1 was never overridden, so WHV
answered it with the *real host's* values, including the logical
processor count (EBX[23:16]) and HTT bit (EDX bit 28) — meaning the guest
could see "N logical processors available" from CPUID while our MADT
only ever declares one Processor Local APIC (vCPU 0). A mismatch like
that is a textbook cause for "wait for all processors to check in" code
to hang forever. Implemented a leaf 1 override (query the real host leaf
1 for accurate family/model/stepping/feature bits, correct only the
processor-count fields to report 1 logical processor, HTT clear) and
confirmed it's applied (guest-visible EBX `0x00010800`, HTT bit cleared).
**The exact same deadlock persists, identical signature.** This hypothesis
is ruled out. The CPUID fix itself is a legitimate, independent
correctness improvement (leaking the host's real core count to the guest
was always wrong) and has been kept regardless.

## Working hypothesis (not yet confirmed)

With PCI enumeration and CPUID processor-count mismatch both ruled out,
the working hypothesis is less certain than before. What's solid: the
guest does zero I/O between finishing PCI enumeration and entering the
frozen spin — whatever it's waiting for is either purely in-memory
(another thread/DPC that itself never runs) or requires an interrupt this
hypervisor never delivers at all (not tied to a specific PCI device, since
enumeration itself was clean). Do not assume any specific mechanism
without further live evidence.

## Update: IRQL classification — decisive result

Per an explicit user request to stay evidence-driven (no further hardware
implementation until another discriminating observation), added CR8 (the
x64 register that directly holds current IRQL) to the stall watchdog's
existing register read — zero new hardware emulation, one more register
in an already-existing `WHvGetVirtualProcessorRegisters` call — with a
plan to follow up with memory inspection around the watched address only
if IRQL permits DPCs.

**Result: CR8 = 0xF (HIGH_LEVEL) — the maximum possible IRQL on x64.**
Confirmed sustained (not a transient sampling artifact) across 10+
consecutive 2-second watchdog samples in one run, and reproduced
identically on a second, independent boot. Per the plan, since IRQL
doesn't permit DPCs, the memory-inspection step was correctly skipped —
the code deferred it and logged why.

**What HIGH_LEVEL specifically implies.** This is a stronger, more
specific signal than a generic "DPC starvation" finding. HIGH_LEVEL is
reserved for a small set of cases in NT — most notably the acquire path
of certain low-level spinlocks, which transiently raise IRQL to
HIGH_LEVEL specifically so they can safely test-and-set a lock word
before spinning on it, expecting **another logical processor** to clear
it. A brief HIGH_LEVEL raise during uncontended lock acquisition is
completely normal and would resolve in nanoseconds; what's abnormal here
is that it's *sustained* — consistent with the spin loop (already
disassembled: snapshot-and-compare against a live address, `pause`+`jmp`,
no timeout) being exactly that spinlock's wait-for-release loop, stuck
because whatever it considers the lock's "owner" never clears it.

**Given this environment only ever runs one vCPU** (WHV VP index 0
exclusively, throughout this entire project), the leading refinement of
the working hypothesis is now: **the guest is spinning in the acquire
path of a genuine SMP spinlock, waiting for a second logical processor
that doesn't exist to release it.** This is a more specific claim than
plain "DPC/scheduler starvation" — it doesn't require a DPC to run at
all, only for another CPU to clear a lock word, which structurally cannot
happen in a single-vCPU environment if the lock is (correctly or
incorrectly) seen as held.

This has **not** been confirmed as the actual mechanism — it's the most
evidence-consistent reading of the CR8 result, not a proven conclusion.
The natural next observation (still no new hardware emulation) would be
identifying the lock word itself and checking its owner/holder encoding,
but per the standing instruction this is flagged for the next round of
investigation rather than pursued immediately.

## Update: lock-word encoding and caller identification

Per an explicit user request to (a) check whether the watched address's
encoding matches a known NT queued-spinlock pattern before assuming an
SMP dependency, and (b) identify who raised IRQL to HIGH_LEVEL, before
any further hardware work.

**The lock word does not match a classic queued-spinlock acquire.** A
real spinlock acquire needs an atomic read-modify-write (`lock cmpxchg`
or equivalent) to actually claim the lock on each attempt. The
disassembled loop (see the spin-loop section above) contains **no
interlocked instruction at all** — it's a plain `mov`/`cmp`/`pause`/`jmp`
sequence that captures a snapshot once, then passively re-reads and
compares against that frozen snapshot. This is a **read-only condition
wait** ("tell me when this changes"), not a lock-acquisition CAS loop.
The watched value itself is `0x0` throughout (both the initial snapshot
and every subsequent poll), which is consistent with either "unlocked/
unsignaled" or simply never-yet-initialized memory — not distinguishable
from the bit pattern alone.

**Identified the immediate caller by resolving the return address.**
The spin routine's own disassembled prologue (`push rbp; mov rbp,rsp;
sub rsp,0x10`) fixes the return address at `[rsp+0x18]` relative to
wherever the watchdog samples RSP inside the loop body. Read that slot
live and confirmed against the existing stack dump (`[rsp+0x18]` was
already the value being captured there). Resolved its owning module via
the same PE-header-walk-back technique used throughout this project —
reproduced identically on two independent boots at RVA `+0x36D16D` into
a large (multi-megabyte) module. **This module's first debug-directory
entry is not a CodeView/RSDS record** (`type=0, codeViewRva=0x0`), so no
PDB name is available this way — it is very likely *not* `ntoskrnl.exe`
(whose debug directory has consistently resolved cleanly as a CodeView
record throughout the entire prior investigation), but rather some other,
still-unidentified system component. Any symbol names our disassembler
prints for this dump (`PnpDeviceCompletionQueueIsEmpty`,
`CmDevicePropertyWrite`, `RtlTestBitEx`, etc.) come from dbghelp matching
against `ntkrnlmp.pdb` regardless of which module this actually is, and
are almost certainly nearest-symbol noise, not real identifications —
already established as a recurring pitfall earlier in this project.
**Do not trust these names without independent confirmation.**

**What the raw caller code reliably shows (independent of any symbol
names).** Disassembled the corrected, realigned dump
(`CallerContext2.disasm.txt`) and found a precise, repeated structural
pattern, several instances of it back-to-back in the same function:

```
mov  rax, [rip+disp]         ; rax = a global table/registration-object pointer
test rax, rax ; je <skip>     ; skip entirely if never registered
cmp  qword ptr [rax+N], 0 ; je <skip>   ; skip if this particular slot is empty
cli                            ; disable maskable interrupts (EFLAGS.IF)
lock inc dword ptr [glob1]     ; atomically increment a reference/rundown counter
lock inc dword ptr [glob2]     ; atomically increment a second counter
mov  ecx, <small constant>     ; 4, 0x100, 0x20, or 1 in different instances -- looks like a notification-type/class ID
call <helper>
mov  rax, [rip+disp2]
...
mov  rax, [rax+N]              ; load a FUNCTION POINTER from the registration object
call rax                       ; <<< call the registered callback -- OUR WATCHED FUNCTION IS CALLED FROM HERE
; -- return address lands here --
mov  rcx/rax, qword ptr gs:[0x18]   ; KPCR self-pointer (reliable, well-known idiom)
lock and dword ptr [rcx+0xE0], ~bit ; atomically clear one flag bit in a per-processor/per-thread struct
lock dec dword ptr [glob2]          ; atomically decrement the counters back down
lock dec dword ptr [glob1]
...
sti                              ; conditionally re-enable interrupts
```

This is the shape of a **registered-callback dispatch wrapper**: check a
registration table, bracket the actual call with `cli`/`sti` and
`lock inc`/`lock dec` reference counts (rundown-protection style —
preventing the callback from being unregistered while in progress, not a
mutual-exclusion lock), then call through a function pointer loaded from
the registration object. Several near-identical copies of this pattern
appear in the same dump with different small integer constants (likely
different notification/callback classes), consistent with a small
dispatch table for several distinct callback types.

**Critically: this caller does not raise IRQL.** It uses `cli`/`sti`
(EFLAGS.IF masking), which is a *different* mechanism from IRQL/CR8 (APIC
TPR-based). Nothing in this caller's disassembly touches CR8. This rules
out the immediate caller as the source of the HIGH_LEVEL IRQL observed at
the stall — **the actual IRQL raise happened somewhere else**: either
earlier in the watched routine's own entry code (not yet confirmed either
way — the dump captured so far starts close to the loop itself, not
necessarily the true function start) or in a still-unresolved, deeper
stack frame above this one.

## Update: where the watched address lives

Per an explicit user request to determine whether the watched address is
a known kernel object, driver-owned (pool) memory, or a static/global
region, and to trace its earliest write if possible.

**It's a fixed offset into the module's own image, every boot.** The
watched VA is always `RIP + 0x1060`, and RIP is always
`module_base + 0x1188`, so the target is always `module_base + 0x21E8` —
identical RVA across every boot observed so far, only the base itself
moving with KASLR. A pool-allocated kernel object (KEVENT/KDPC/KTIMER/
KSPIN_LOCK) would live in NonPagedPool, an entirely separate address
range from the module's own code/data, and its offset relative to the
module base would not be fixed like this. A fixed small RVA directly
into the image is already a strong structural signal against "known
kernel object" or "driver-owned pool memory."

**Confirmed directly by reading the module's own PE section table**
(three sections total — a very small, minimal driver):

```
section[0] name=.text   VA=0x1000 VSize=0x1000 RawPtr=0x1000 RawSize=0x1000 chars=0x60000020
section[1] name=.data   VA=0x2000 VSize=0x1000 RawPtr=0x2000 RawSize=0x1000 chars=0xC0000040  <-- CONTAINS WATCHED TARGET
section[2] name=.reloc  VA=0x3000 VSize=0x1000 RawPtr=0x3000 RawSize=0x1000 chars=0x42000040
```

The watched RVA (`0x21E8`) falls inside `.data`. Its characteristics
(`0xC0000040` = `IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_READ |
IMAGE_SCN_CNT_INITIALIZED_DATA`) mark it a genuine initialized-data
section, not a marked-uninitialized (`IMAGE_SCN_CNT_UNINITIALIZED_DATA`)
one — and `RawSize == VirtualSize` exactly (`0x1000 == 0x1000`), meaning
the *entire* section is backed by real file content with no zero-fill
tail. **This rules out the ".bss merged into .data with a zero-fill
tail" pattern.** The `0` we observe is not runtime zero-fill — it's the
literal byte value the compiler/linker baked into the PE image at this
exact file offset.

**Conclusion: this is a genuine static/global variable owned by this
small driver's own image, explicitly initialized to `0` in the binary
itself — not a known kernel dispatcher object, not pool-allocated
driver memory.**

**On tracing the earliest write:** within this project's established
constraint of read-only observation (no live breakpointing/instruction
tracing), we cannot exhaustively prove a write never occurred at some
instant we didn't happen to sample. What the evidence does show: the
value matches the PE image's own baked-in initializer (`0`) exactly, and
every live sample taken throughout the stall — across multiple
independent boots — shows the same unchanged `0`. No write to this
address has been observed at any point in the captured boot path. The
most evidence-consistent reading is that **whatever code is supposed to
write a real value here — most plausibly during this small driver's own
initialization, or in response to some device/notification event tied to
the registered-callback dispatcher identified as its caller — has simply
never run in this boot.** This reframes the open question productively:
it's less "why won't a lock release" and more "why did this driver's own
expected initialization/notification step never fire" — which does not
presuppose any SMP dependency at all.

## Update: tracing the producer, not the consumer

Per an explicit user request to pivot from the consumer (the spin loop) to
the producer: find every code path that references or writes the watched
global, determine which one is expected to transition it, and trace why
that path never executes — all still without any hardware speculation.

**The entire driver's code was checked, exhaustively, offline.** The PE
section table (already read in the prior update) showed this driver's
*entire* code is a single 4096-byte `.text` section — small enough to
disassemble and check in full. Extracted it from the already-captured
`NewStallModule.bin`, disassembled all 859 instructions, and manually
computed the resolved target address of **every single RIP-relative data
reference** in the function (`target = following_instruction_RVA +
displacement`, verified against the two already-known loop references,
which independently confirmed the method: both resolve to exactly
`0x21E8`, matching the value derived earlier from the live RIP).

**Result: of every RIP-relative reference in this driver's entire code,
only the spin loop's own two reads (the initial snapshot and the
in-loop re-read) ever touch RVA `0x21E8`. Nothing in this module writes
it.** The nearby addresses that *are* written (`0x21A8`, `0x21C0`,
`0x2180`, `0x2188`, `0x2198`, `0x21A0`, `0x21C8`, `0x21D0`, `0x21FA`, and
others) belong to a cluster of sibling globals in the same `.data`
section, several of them clearly populated by a **chipset-identification
routine** in this same driver: it walks the PCI bus list looking up the
host bridge's device ID and branches on it — `0x1237` (Intel 82441FX /
**i440fx — this exact ID matches our own emulated host bridge exactly**),
`0x29C0` (Intel Q35/MCH), and `0xD57`. Confirmed the i440fx branch is the
one taken (matching our emulation), and traced it further: it reads the
PIIX4 PM function's PM base via CF8/CFC config access, clears bit 0, adds
8, and stores the result — the live value at that slot (RVA `0x21A8`) is
**`0xB008`, exactly our own ACPI PM Timer port**, confirming this
detection succeeds correctly against our emulated hardware.

**Confirmed the image's baked-in value at the watched address directly,
byte-precise:** the QWORD at RVA `0x21E8` in the static image is exactly
`0x0000000000000000`, matching every live sample taken throughout this
whole investigation. Its immediate neighbor at RVA `0x21F0`, by contrast,
already holds a **real, canonical kernel-mode pointer**
(`0xFFFFF806...`) — some adjacent, related piece of state *is* already
populated; specifically the watched flag is what's missing, not
everything in this data cluster.

**Conclusion: the producer is not in this module.** Since no instruction
anywhere in the driver's complete code writes RVA `0x21E8`, whatever is
supposed to set it must be external — most plausibly the large,
still-unnamed calling module identified in the previous update, acting
through a *different* slot in the same registered-callback dispatch table
already found there (recall: that dispatcher's disassembly showed several
near-identical call sites, each loading a function pointer from a
different offset — `+0x10`, `+0x28`, `+0x40`, `+0`, `+0x48`, `+0x70`,
`+0x140`, `+0x170` — into the same registration object; only one of these
has been confirmed to call into the watched driver so far). Tracing
*why* the producer's path never executes requires identifying and
checking those other callback slots — a concrete, bounded, still
hardware-agnostic next step, not yet done.

## Update: the registration object's slots, read live

Continuing the producer trace: identified that the three known call sites
(`+0x10` → our watched driver, `+0x28`, `+0x40`) all load their object
pointer from the *same* RIP-relative global in the caller module —
verified precisely with actual arithmetic (PowerShell, not error-prone
manual hex addition, which had produced a wrong intermediate result
earlier in this session before being caught) — resolving to caller-module
RVA `0xCA9008`. Rather than continue cataloging individual call sites
from static disassembly one at a time, read that pointer live and dumped
the registration object's own fields directly (`0x200` bytes, every
8-byte slot).

**The object is large and mostly densely populated** with real,
canonical kernel-mode pointers (`0xFFFFF807...`) across most of its
range (`+0x000` through `+0x150`, and again `+0x1B0` onward). Confirmed
`+0x10`, `+0x28`, and `+0x40` all hold real pointers, consistent with the
three call sites already found. **Two clear gaps of null
(never-registered) slots stand out, both sandwiched between otherwise
fully populated regions:**

- `+0x048` through `+0x088` — 5 consecutive null qword slots
- `+0x158` through `+0x1A8` — 11 consecutive null qword slots (the
  larger and more structurally distinct of the two)

This is the most concrete, actionable lead so far: within an otherwise
comprehensively populated callback/provider table, these are genuine gaps
— not the general shape of "nothing is registered," but specific,
bounded holes. **This has not been proven to be the producer's slot** —
no call site has been found yet that reads specifically from these
offsets, so there's no direct evidence linking either gap to the watched
driver's flag. What it does show is a concrete, narrow, low-effort target
for the next round: identify what notification/callback type each gap
corresponds to (by finding call sites elsewhere that reference these
exact offsets) and confirm whether the driver's flag-setting write would
go through one of them.

## Update: the two gaps ruled out — dispatcher's slot range doesn't reach them

Continuing the search for what the two null-slot gaps represent: dumped a
wide, 8KB window of the caller module's code (`callerModuleBase +
0x36D000`, chosen to bracket the three already-confirmed call sites) and
disassembled it in full, rather than continuing to catalog individual call
sites one at a time.

**Result: the dispatcher is a small, self-contained family of two
functions, and it only ever indexes low offsets.** Tracing every load of
the registration-object pointer (13 total `mov rax, qword ptr [rip+disp]`
instructions, all confirmed via displacement arithmetic to target the same
fixed VA, caller RVA `0xCA9008`) and the `[rax+N]` access immediately
following each one shows the complete set of offsets ever dispatched
through this code is exactly:

```
0x00, 0x08, 0x10, 0x28, 0x30, 0x38, 0x40
```

— a contiguous run from the low end of the object, each guarded by the
same `test rax,rax`/`cmp qword ptr [rax+N],0`/`cli`/`lock inc`-bracketed
call pattern already documented, with a distinct small `ecx` constant per
slot (`1`, `2`, `4`, `0x20`, `0x40`, ...). The last of these blocks ends in
a genuine function boundary — `ret` at RVA `0x36D4EB` followed by 8 bytes
of `int3` padding (the standard MSVC/link.exe function-alignment filler) —
and the code immediately after it is unrelated (a small bit-table lookup
helper, then a `KeRaiseIrql`/interlocked-flag-style helper at RVA
`0x36D528` that *does* touch CR8, but operates on a per-thread flags field
unrelated to the registration object, not the watched routine's frame). No
further load of the registration-object pointer occurs anywhere else in
the 8KB window.

**Conclusion: neither gap (`+0x048`-`+0x088` nor `+0x158`-`+0x1A8`) is ever
referenced by this caller.** This caller module's dispatcher simply
doesn't consume those slots — they may be populated by, and relevant to, a
completely different consumer elsewhere in the system, or may correspond
to notification types this driver/OS build never dispatches through this
particular table at all. Either way, this specific lead is now closed: the
gaps are not "the producer's missing slot" for the watched driver's flag,
at least not via this caller. This is a clean negative result, not an
inconclusive one — the entire relevant address range was exhaustively
disassembled and cross-checked by both a `[rax+0x` grep and a full listing
of every registration-pointer load.

## Update: live stack walk finds the reset-dispatch call chain

Continuing to locate the true IRQL raise site per an explicit user request
to not restrict the search to a literal `mov cr8`, but also consider
helper routines/call sequences, and to find the *earliest* point CR8
transitions to HIGH_LEVEL.

**Neither the watched routine's own code nor its immediate caller can be
the raise site.** The watched driver's entire 4096-byte `.text` section
(already exhaustively disassembled in the prior update) contains **zero**
`mov cr8` instructions and **zero** `call` instructions anywhere in it —
confirmed by grepping the complete, already-captured disassembly. The
immediate caller (the dispatch wrapper, RVA `+0x36D16D`) was already shown
to use `cli`/`sti`, not CR8. So IRQL must already be HIGH_LEVEL before
either of these runs.

**Walked the live stack instead of guessing.** Added a diagnostic that
reads 192 qwords upward from the sampled RSP, resolves every
kernel-space-looking value to its owning module + RVA via the same
PE-header-walk technique used throughout, and dumps a code window around
each resolved candidate. This found the immediate caller again at
`[rsp+0x18]` (as before) plus nine further candidates, all in the same
still-unnamed caller module, at increasing RVAs — plus, notably, **three
candidates in a second, different, real module**: base resolves to a
module with a valid CodeView debug directory and PDB name **`acpi.pdb`**
(Microsoft's ACPI driver), GUID `662B8231-E47D-1FF9-F7E8C8FD1B44A572`
age `1`.

**The acpi.sys lead was checked and ruled out as a false positive.**
Downloaded the exact matching `acpi.pdb` from Microsoft's public symbol
server (the GUID/age let dbghelp resolve real symbol names, unlike the
nearest-match-noise problem hit earlier with the unnamed caller module)
and disassembled a window around each of the three candidate addresses.
All three land inside acpi.sys's **GUID/WPP-trace-data region**
(`GUID_PCI_PME_INTERFACE`, `GUID_INT_ROUTE_INTERFACE_STANDARD`, WPP trace
GUIDs, large zero-filled padding) — not code at all. This is the classic
signature of **stale stack garbage**: a leftover data pointer from some
earlier, unrelated ACPI operation that was never overwritten, not a live
return address on the current call chain. Ruled out; not pursued further.

**The real, unnamed-module candidates form a coherent, traceable chain.**
Disassembling the code windows around the remaining candidates (all
already captured, reused directly) shows they connect via genuine
`call`/return-address relationships:

- `[rsp+0xE8]` (RVA `0x3C0D58`) returns from `call 0x367730` — a small
  `switch`-style dispatcher on an integer parameter.
- Its default path reaches `call 0x36789C` — a function that performs the
  reset attempts described below.
- That function calls `0x36D120` — the **true start** of the dispatch
  wrapper family already mapped in the prior update (confirmed: RVA
  `0x36D126`, immediately inside this function, is the exact `+0x10`
  callback-check block already found, i.e. the one that calls into our
  watched driver). Its return address, `0x36799D`, is exactly
  `[rsp+0x48]` — one of our other live stack candidates, confirming the
  reconstruction.

**Two genuine `mov cr8` sites exist nearby in this same module, but
neither is on the executed path.** One (RVA `0x37C1C3`) sits behind a
branch (`jne`) that the live return address shows was *not* taken. The
other (RVA `0x3C0DA0`) is the entry of a function that is adjacent in
memory to — but not actually called by — the code on our confirmed chain
(the real call at that point targets `0x367730`, not `0x3C0DA0`). Both
were checked and ruled out by tracing the actual `call`/return-address
pairs, not by assumption. The literal raise instruction is still
unlocated, but see below — it stopped being the most important open
question.

## Update: the guest is trying to reset the system, and nothing answers

**The function at RVA `0x36789C` (called from the default path of the
`0x367730` switch, itself called from further up the stack) is a system
reset routine, not driver initialization.** Its disassembly shows, in
sequence, gated by feature-flag checks:

- `lea edx,[rsi+0x70]; mov al,0x0F; out dx,al` then
  `lea edx,[rsi+0x71]; xor eax,eax; out dx,al` — the classic RTC/CMOS
  **shutdown-status-byte** sequence (select register `0x0F`, write `0`).
- `mov edx,0x64; mov al,0xFE; out dx,al` — the classic legacy
  **keyboard-controller reset pulse** (pulse output-port bit 0 low via the
  8042 controller, the oldest x86 "reset the CPU" trick still used as a
  HAL fallback).
- `mov ecx,0x40000003; wrmsr` — a **Hyper-V synthetic reset MSR** write
  (`ecx=0x40000003` is in the Hyper-V synthetic MSR range).
- After attempting these, it calls `0x36D120` (the dispatch wrapper) twice
  — first with a type parameter, then again with `ecx=0` — to notify
  registered callbacks, exactly the registration object already traced to
  the watched driver's slot (`+0x10`). A `hlt`/`jmp $` fallback exists on
  a separate branch for one specific parameter value, and an `int3`
  immediately follows the notification calls on the path that returns
  normally, consistent with "the system should never still be running
  code here — if we get this far, actual reset didn't happen."

**Confirmed live: the guest genuinely executes the keyboard-controller
reset pulse.** Added zero-behavior-change diagnostic logging (a `printf`
only, no new emulated behavior) for exactly two conditions: port `0x70`
write with value `0x0F`, and port `0x64` write with value `0xFE`.
Rebuilt, reran, and observed:

```
[kerneldiag] RESET-WATCH: port 0x64 write val=0xFE (keyboard-controller reset pulse) rip=0xFFFFF8060BEBE982
[kerneldiag] RESET-WATCH: port 0x64 write val=0xFE (keyboard-controller reset pulse) rip=0xFFFFF8061470D419
[watchdog] STALL #1 (exitCount=405811 unchanged for 2s): ... cr8(irql)=0xF(HIGH_LEVEL)
```

Both writes originate from genuine kernel-space RIPs
(`0xFFFFF806...`), and both occur at the **same `exitCount`** as the
first stall detection — i.e., immediately before the guest freezes, not
at some unrelated earlier point in boot. (Two earlier port-`0x70`
val-`0x0F` hits exist from firmware-space RIPs, `0xFFFCEA0D`/`0x83DBEE`,
almost certainly unrelated OVMF/firmware CMOS activity, not this NT
reset path.)

**Conclusion.** This hypervisor implements no actual effect for a
port-`0x64` reset pulse, the RTC shutdown-status byte, or the Hyper-V
synthetic reset MSR — none of them perform a real CPU/system reset today.
The guest's NT kernel is very plausibly attempting a **mid-installation
restart** (a completely normal, expected step for a Windows Setup boot
like this one — `win10_installer.vhd`), tries all its available legacy
and synthetic reset mechanisms in sequence, and — since none of them
actually reset anything — falls through to notifying registered
shutdown/reset callbacks that a reset is imminent. The watched driver's
callback (already proven to be a passive wait on a static flag, never
written by any code in its own module) is very plausibly waiting for
confirmation that the reset actually happened, which of course never
comes, because it never did. This reframes the entire investigation: the
open question is no longer "what callback slot is missing" but **"how
should this hypervisor actually perform a system reset when the guest
requests one."** That is squarely new-hardware-behavior territory (an
actual implementation decision, not further observation), so it's left
here for the user to decide rather than implemented unilaterally.

## Update: experimental reset confirms the hypothesis

Per an explicit user request to implement the smallest possible
experimental reset for the `0x64`/`0xFE` pulse, purely to observe whether
honoring it changes guest behavior (not to be architecturally complete or
permanent).

**Implementation.** Reused the exact mechanism already proven working
elsewhere in this codebase for the port `0x92` hot-reset bit: on detecting
port `0x64` write with value `0xFE`, call `WHvSetVirtualProcessorRegisters`
to restore `Rip`/`Cs` to the same reset-vector state used at cold boot
(`RIP=0xFFF0`, `CS` selector `0xF000`, base `0xFFFF0000` in UEFI mode,
matching real x86 reset semantics). Additionally reset `Cr0` (`0x60000010`
— `PE=0`, `PG=0`), `Cr3` (`0`), `Cr4` (`0`), and `Efer` (`0`) — unlike the
port-0x92 case, this fires while the guest is deep in 64-bit long mode,
where `CS.Base` is architecturally ignored for addressing; without also
exiting long mode, the RIP/CS reset alone would land at flat linear
address `0xFFF0` under the guest's still-active page tables, not the real
reset vector, making the experiment meaningless. Guest RAM and other
emulated device state were deliberately left untouched (matching the
existing port-0x92 precedent, and because RAM legitimately survives a
real warm reset).

**Result: the guest genuinely restarts.** Immediately after the reset is
honored (logged at `exitCount=1399336`), the same firmware-space RTC
port-`0x70`=`0x0F` writes seen during the original cold boot (RIPs
`0xFFFCEA0D`, `0x83DBEE`) appear again — OVMF re-ran its early POST
sequence from the reset vector, confirming the reset genuinely took
effect at the architectural level, not just a register no-op.

**The guest then progresses to a different, new failure — not the
original stall:**

```
[watchdog] STALL #1 (exitCount=1588718 ...): rip=0xFFFFF8070AABE7FB ... rcx=0xF rdx=0xF ... cr8(irql)=0xF(HIGH_LEVEL)
[kerneldiag] KiBugCheckData: code=0x139 params=[0x3, 0xFFFFF80711074CB0, 0xFFFFF80711074C08, 0x0]
```

This is `KERNEL_SECURITY_CHECK_FAILURE` (bugcheck `0x139`, param1=`0x3`,
typically a corrupted-list-entry/fast-fail condition) — a completely
different signature from the original deadlock (different RIP, different
register pattern, and RIP now visibly *moves* between watchdog samples
rather than being frozen solid, i.e. the guest is executing, not spinning).
This never appeared in any prior run of this investigation.

**Conclusion, per the stated success criterion: the guest progressed to a
different state, so this is strong evidence that not honoring the reset
request was the actual missing hardware behavior.** The experimental
reset has been left in place, not reverted. The new bugcheck is most
plausibly a consequence of the experiment's deliberate incompleteness —
only CPU register state is reset; emulated device state (AHCI/ATA
controller state, PCI configuration space, IOAPIC redirection tables,
etc.) still reflects wherever the prior, aborted boot attempt left it,
which a genuinely reset system would not expect.

## Update: bugcheck 0x139 traced to Segment Heap corruption, not device state

Per an explicit user request to narrow down what's actually corrupted
before implementing a broader fix (option 2 of the two choices above).

**Parsed the bugcheck 0x139 parameters per their documented layout**
(`param1`=Type, `param2`=trap frame address, `param3`=`EXCEPTION_RECORD`
address, `param4`=reserved) by reading guest memory at `param3` live and
interpreting it as a real `EXCEPTION_RECORD` structure:

```
[kerneldiag] KiBugCheckData: code=0x139 params=[0x3, 0xFFFFF80710274CB0, 0xFFFFF80710274C08, 0x0]
[kerneldiag] EXCEPTION_RECORD @0xFFFFF80710274C08: code=0xC0000409 address=0xFFFFF8070B944214 numParams=1 info[0]=0x3 info[1]=0x0
[kerneldiag] fast-fail site: module base 0xFFFFF8070B600000, RVA 0x344214  <-- inside ntoskrnl.exe
```

`ExceptionCode` (`0xC0000409`, `STATUS_STACK_BUFFER_OVERRUN`) is the
generic NTSTATUS every `__fastfail()` call raises regardless of subtype;
`ExceptionInformation[0]` (`0x3`) is the actual fast-fail subcode,
confirming `FAST_FAIL_CORRUPT_LIST_ENTRY` and matching `param1` exactly.
Critically, `ExceptionAddress` resolves *inside ntoskrnl.exe* — unlike
almost everything else in this investigation, ntoskrnl.exe's debug
directory is a real CodeView record, so its PDB (`ntkrnlmp.pdb`, already
on hand from earlier in this project) gives a **trustworthy** symbol, not
the nearest-match noise seen for the other, unnamed modules throughout
this investigation.

**Resolved via `ntkrnlmp.pdb`:** RVA `0x344214` is
**`RtlpHpLfhOwnerMoveSubsegment+0xE8`** — part of the kernel's **Segment
Heap / Low-Fragmentation-Heap (LFH)** implementation, specifically the
routine that moves a subsegment between the heap's internal free-lists.
It detected a corrupted `LIST_ENTRY` while doing so.

(A trap-frame `Rip`/`Rsp` read was also attempted at `param2` using an
offset borrowed from elsewhere in this codebase, but returned an
implausible value (`Rip=0x286`) — that offset assumption doesn't hold for
this trap-frame context. Not pursued further since the exception record
already gave a clean, trustworthy answer.)

**Conclusion: this is not an emulated I/O device state problem** (AHCI,
PCI config space, IOAPIC redirection tables — the first guess after the
reset experiment) **— it's kernel heap metadata in guest RAM being
inconsistent across the reset.** The freshly-restarted kernel's memory
manager builds a new Segment Heap and, at some point, tries to unlink or
move a subsegment using list pointers that don't validate — plausibly
because the physical memory it's using for this heap structure still
holds partially-modified bookkeeping from the aborted prior boot's own
heap, rather than the state a genuinely fresh kernel boot would expect
there. Real hardware's warm reset (the same port-0x64 mechanism) does
*not* clear RAM either, and doesn't hit this problem — so the gap is
likely not "RAM should be cleared" in general, but something more
specific about how memory that will become heap-managed is presented
across the reset boundary (e.g. demand-zero/fresh-page semantics the real
memory manager relies on that this hypervisor doesn't provide). Not yet
root-caused further — this is where the investigation currently stands.

## Update: pinpointing the exact fail site (partial success)

Per an explicit user request to trace what Windows expects to survive vs.
reinitialize across the reset, and to identify the minimum responsible
state before adding any broad reset behavior — RAM-clearing to be used
only as a falsification test, not a permanent fix.

**Dumped and disassembled the live fail-site code** (ntoskrnl.exe RVA
`0x344214`, read directly from guest memory this time — not a static file
— then disassembled with the trustworthy `ntkrnlmp.pdb`). This revealed
the exact function body of `RtlpHpLfhOwnerMoveSubsegment` around the fault:
a doubly-linked-list **safe-unlink pattern**, appearing *three times*
in the function (RVA `0x344176`, `0x3441D5`, `0x3441DF`), each performing
the classic `Blink->Flink == &Entry` consistency check before unlinking —
and all three funnel their failure branch to the *same* `mov ecx,3;
int 0x29` fast-fail call at RVA `0x34420F`/`0x344214`. This means the
fault RIP alone doesn't disambiguate which of the three checks actually
failed.

**Attempted to recover the actual corrupted pointer from the trap frame,
with partial success.** The earlier guess at `KTRAP_FRAME`'s `Rip`/`Rsp`
offsets (borrowed from a different trap context elsewhere in this
codebase) was wrong — re-reading the full trap frame as raw qwords shows
`Rip` actually lives at `+0x168` (confirmed: exactly matches the
already-known fault address) and `+0x178` is `EFlags` (`0x286`), not
`Rip`. `+0x038` (`Rcx` in the standard `KTRAP_FRAME` layout) reads `3`,
consistent with the fast-fail subcode already known from the exception
record. However, by the time the trap frame is captured, `Rcx` has already
been overwritten with the literal `3` for the `int 0x29` call itself, so
it no longer holds whatever address was being validated — and with three
candidate checks sharing one failure path, reliably identifying which
register held the *original* corrupted pointer would need live
single-step/breakpoint capability this project doesn't have. This was not
pursued further; see the falsification test below for a more direct
answer instead.

## Update: RAM-clear falsification test — confirms the category of cause

**Implemented as a clearly temporary, `#ifdef FALSIFY_RAM_CLEAR_ON_RESET`-
gated block** immediately after the existing port-`0x64` experimental
reset: a single `memset(guestMemory, 0, guestMemSize)`, compiled in only
via an explicit `/D` build flag for this one test, with an inline comment
stating it is not a fix and would be removed regardless of outcome. Built
once with the flag defined, tested, then the block was deleted from the
source entirely and the binary rebuilt back to the flag-less state — the
codebase contains no RAM-clearing code before or after this update.

**First run was confounded by the experiment's own cost and correctly
recognized as such, not misread as a result.** Zeroing all `0xC0000000`
(3GB) bytes of guest RAM via a single blocking `memset` on the VM's own
execution thread took long enough that the watchdog fired a false-alarm
"STALL #1" at `rip=0xFFF0` *before* the `memset` had even finished (the
"zeroed all bytes" confirmation printed *after* the stall line in the
log). Re-examined the full log rather than trusting the first stall
notification: immediately after the `memset` actually completed, the
guest resumed and hit the identical early-POST RTC sequence seen in every
other run — confirming the apparent freeze was purely an artifact of the
blocking clear, not a genuine guest-side hang.

**With that false alarm set aside, the real result is unambiguous and
positive:**

```
BdsDxe: loading Boot0002 "UEFI LocalHost Virtual Disk LH0001 " from PciRoot(0x0)/Pci(0x2,0x0)/Sata(0x0,0xFFFF,0x0)
BdsDxe: starting Boot0002 "UEFI LocalHost Virtual Disk LH0001 " from PciRoot(0x0)/Pci(0x2,0x0)/Sata(0x0,0xFFFF,0x0)
[heartbeat: exitCount=730000 elapsedSec=78.3 lastPort=0xB008 ...]
```

With RAM genuinely cleared at reset time: the guest re-POSTs, proceeds
through `fwcfg`/`ramfb` setup, reaches `BdsDxe` loading the boot entry
from the virtual disk (i.e. re-enters the Windows Boot Manager/installer
path), and the heartbeat keeps climbing steadily for the remainder of the
90-second observation window — **no bugcheck `0x139` this run, and no
new freeze.** This is a clean, positive falsification result: clearing
RAM measurably changes the outcome, confirming stale RAM content — most
plausibly the prior boot's own heap metadata, matching the
`RtlpHpLfhOwnerMoveSubsegment` fail site — is genuinely the responsible
category of state, not a coincidence and not emulated device state.

**This is deliberately not evidence that "clear all of RAM" should become
the permanent fix.** Real hardware's own warm reset via this exact
mechanism does not clear RAM and does not hit this problem, so the
correct, minimal fix is almost certainly narrower than what this
experiment did — see next steps.

## Update: attempted ground-truth trap-frame layout, then narrowed empirically instead

Per the same user request, continued: tried to definitively resolve the
`KTRAP_FRAME` field layout (rather than guessing) by querying
`ntkrnlmp.pdb`'s own type information through dbghelp
(`SymGetTypeFromNameW` + `SymGetTypeInfo(TI_GET_CHILDREN)`), which — if
present — would give ground-truth field offsets instead of relying on
memory of the "well-known" public layout. **This did not pan out**:
`SymGetTypeFromNameW` resolved `_KTRAP_FRAME` to a suspiciously low type
index (1) with zero reported length, and `TI_GET_CHILDREN` failed outright
— this specific public PDB does not appear to carry full struct layout
for this internal type. Cross-checking the three already-confirmed offsets
(`Rip`@`+0x168`, `EFlags`@`+0x178`, `Rsp`@`+0x180`, all independently
verified against known-good values in the earlier update) against the
classic, widely-documented `KTRAP_FRAME` layout shows all three match
exactly, giving reasonable confidence in the rest of that layout
(`Rax`@`0x30`, `Rcx`@`0x38`, `Rdx`@`0x40`, `R8`@`0x48`, `R9`@`0x50`,
`R10`@`0x58`, `R11`@`0x60`) — but applying it to the actual captured trap
frame produced an inconsistent picture (`Rdx`/`R8`/`R10` all reading `0`,
which doesn't fit any of the three safe-unlink checks needing a non-null
pointer to have gotten this far without an earlier, different kind of
fault). Given three checks share one fail path and register values could
belong to any of them, further register-level reconstruction was not
pursued — an empirical, black-box approach turned out to be more
tractable, below.

## Update: narrowed to OS-managed memory above 16MB

Rather than keep guessing which exact bytes matter, ran a second,
narrower, equally temporary `#ifdef`-gated falsification test: clear only
guest RAM **above** the legacy 16MB boundary (`memset(guestMemory +
0x1000000, 0, guestMemSize - 0x1000000)`), leaving the BIOS data area,
real-mode IVT, and other low-memory legacy structures untouched.

**Result: identical to the full-RAM-clear test.** No bugcheck `0x139`,
the guest re-POSTs and reaches `BdsDxe: starting Boot0002 "UEFI LocalHost
Virtual Disk LH0001"` again, and the heartbeat keeps climbing
(`exitCount=365000` at `elapsedSec=76.4`) with no new freeze through the
rest of the observation window. (The same blocking-`memset` false-alarm
"STALL #1" appeared first, exactly as before, and was set aside the same
way — the real signal is what happens once the clear actually completes.)

**This confirms the responsible state lives specifically in OS-managed
extended memory (above 16MB), not low/legacy memory** — a genuine
narrowing, not just a repeat of the first result. Reverted immediately
after the test, same as the first falsification patch; the codebase
contains no RAM-clearing code of any kind.

## Update: the RAM-clear "fix" did not replicate — four mitigation attempts, all failed

Per an explicit user request to implement a permanent fix rather than
continue narrowing observationally. The RAM-above-16MB clear from the
prior update was made permanent first, then tested repeatedly (the
earlier single successful test had not been confirmed reliable before
being written up as "confirmed" — that framing was premature).

**Trial 1 (full above-16MB clear, made permanent):** bugcheck `0x139`
recurred. Two more repeat trials of the *identical* code: recurred both
times. **The original single successful test was not representative —
it was a lucky run.**

**Trial 2 (narrower/faster 240MB clear, to test whether the ~3GB clear's
several-real-second blocking duration was starving the same
`HalpTimerWaitForPhase0Interrupt`-style 3-real-second timeout the
original VPPT/RTC investigation documented):** still recurred. This
disproved the timing/blocking-duration hypothesis specifically, though
not the broader idea that this is a timing-shaped problem.

**Trial 3 (reset this hypervisor's own `rtcPeriodicTickArmed`/
`lastRtcPeriodicTick` state, no RAM clear):** hypothesis was that a
periodic RTC tick fired shortly before the reset (real wall-clock time)
would leave the delivery-arming check in `deliverRtcPeriodicIrq`
(`rtcPeriodicTickArmed && elapsedMs < intervalMs`) suppressing the second
boot's first expected tick, racing its own timeout depending on
unrelated scheduling jitter. Tested live: still recurred.

**Trial 4 (RAM clear + `rtcPeriodicTickArmed` reset combined):** the
guest got further this time (reached `BdsDxe` a visibly further point
before failing) but still eventually recurred.

**Also tried and explicitly ruled out: resetting IOAPIC redirection-table
state** (mask everything back to the cold-boot default). This made things
reliably *worse* — an immediate, consistent freeze structurally identical
to the pre-fix VPPT/RTC interrupt-delivery gap (`HIGH_LEVEL` IRQL,
oscillating RIP in the same tight range that precedes bugcheck `0x139`
in the other trials). The classic keyboard-controller reset pulse is a
CPU-only reset on real hardware, not a full platform/chipset reset, so
this state is deliberately left untouched going forward.

**Current code state:** only the CPU-register reset (RIP/CS/CR0/CR3/
CR4/EFER) remains, permanently implemented. This reliably fixes the
*original* deadlock — confirmed across every single trial in this
investigation, with no exceptions. Bugcheck `0x139` remains unfixed and
occurs in most (not all) runs after the reset.

**What's still true and still the best lead:** the stall that precedes
bugcheck `0x139` every time (oscillating RIP in a tight ~0x20-byte range,
`HIGH_LEVEL` IRQL, `rcx=rdx=0xF`) has an identical *shape* to the
original pre-fix VPPT/RTC interrupt-delivery-timeout bug this project
already solved once (see `vppt-synic-blocker.md` part 9-10) — strongly
suggesting the second boot pass is hitting a similar "wait for an
interrupt that doesn't arrive in time" failure, not literally the heap
corruption being the root cause (bugcheck `0x139` may be a downstream
symptom of mishandling that timeout, not the primary problem). The exact
mechanism causing this to fail on the *second* boot pass when the
identical wait reliably succeeds on the *first* has not been pinned down
despite four targeted attempts.

## Update: the RTC-timing theory disproven — the stall is just the post-bugcheck halt

Followed the previous update's own advice (get direct observation instead
of another blind mitigation): added time-based diagnostic logging inside
`deliverRtcPeriodicIrq`, active for 20 real seconds starting exactly when
the port-`0x64`/`0xFE` reset fires (a first attempt used a fixed
call-count budget instead of a time budget and got consumed in ~2 real
seconds — blind to the actually-relevant window, which turned out to
start roughly 10+ seconds after the reset; switched to time-based with a
throttled/transition-aware print strategy to keep output bounded over the
full window).

**Result: PIE (Register B's periodic-interrupt-enable bit) never
transitions back to enabled at any point between the reset and the stall,
across the entire 20-second window.** Every single diagnostic line in
that window reads "PIE not enabled" — the second boot pass never even
attempts to re-arm the RTC periodic interrupt before whatever happens
next. This directly rules out an RTC-interrupt-delivery race as the
mechanism, regardless of exact timing.

**Then resolved what the stall's RIP actually is, definitively.** The
existing `stallTicks==2` diagnostic path already resolves the owning
module via `kernelDiagIdentifyModule` — checking that output showed the
module is `ntoskrnl.exe` itself (`ntkrnlmp.pdb`, matching the GUID already
used throughout this investigation) at RVA `0x4BE7F5`. Resolved via the
same reliable `sym_lookup.py` technique used for the earlier bugcheck
`0x139` exception-record trace:

```
RVA 0x4BE7F5 -> HaliHaltSystem+0x25
```

**This is the HAL's terminal halt loop** — reached only after
`KeBugCheckEx` has already fully run and displayed its failure. The
oscillating RIP (cycling through a handful of nearby offsets across
watchdog samples) and `HIGH_LEVEL` IRQL are simply what any frozen,
already-bugchecked CPU looks like — `HaliHaltSystem` is a tiny `cli`/
`hlt`/`jmp $-N` loop, and different watchdog samples catch it at
different points in that loop. **This was never a live "wait for an
interrupt that doesn't arrive in time" race** — by the time the watchdog
notices anything, the kernel has already bugchecked, and `KiBugCheckData`
(already being read out one tick later) simply confirms it.

**This retroactively explains why all four RTC/RAM mitigation attempts
failed**: they were addressing a mechanism (interrupt-delivery timing)
that was never actually in play. The genuinely correct lead was the one
found *before* this whole detour: the fault is `RtlpHpLfhOwnerMoveSubsegment`
(Segment Heap `LIST_ENTRY` corruption), already traced with a real,
symbol-resolved exception record in the "pinpointing the exact fail site"
update above. All RTC diagnostic instrumentation added for this detour has
been removed from the codebase (not left in, dormant or otherwise) now
that it's answered its question.

## Update: trap frame is likely minimal (non-continuable fastfail), full architectural reset tried

Per a continued push to identify the exact corrupted `LIST_ENTRY`: widened
the trap-frame dump to print every qword (not just nonzero ones) and
auto-flag anything that looks like a canonical kernel pointer, then dump
a small window of *its* memory to check for `LIST_ENTRY`-shaped data
(two adjacent pointer fields). Several canonical pointers turned up
(`+0x120`, `+0x128`, `+0x130`, `+0x168`=confirmed `Rip`, `+0x180`=confirmed
`Rsp`, `+0x198`, `+0x1C8`, `+0x1F0`, `+0x1F8`), and `+0x130` in particular
showed a plausible empty-list-head self-reference pattern — but none of
this located `Rdx` (the "owner" parameter, which the fail-site
disassembly proves the code still dereferences on every failing branch,
so it must be live and non-null at the fault). Its assumed classic offset
(`+0x40`, following the standard public `KTRAP_FRAME` layout that
correctly predicted `Rip`/`EFlags`/`Rsp`/`Rcx`) reads exactly zero.
**Working conclusion: `int 0x29`'s trap frame for a fast-fail is very
likely a deliberately minimal, non-continuable one that doesn't preserve
the full GPR set** (unlike a normal, resumable interrupt/exception trap
frame) — fast-fail is explicitly a one-way, non-recoverable termination,
so the OS has no reason to preserve registers it will never restore.
This register-archaeology path is now considered exhausted; recovering
the corrupted structure this way isn't tractable without live
single-step/breakpoint capability this project doesn't have.

**Pivoted to a different, well-motivated angle: complete the CPU-register
reset with the remaining architectural state a genuine x86 RESET# would
reinitialize** (`IDTR`, `GDTR`, `LDTR`, `TR`, `DR7`, `DS`/`ES`/`FS`/`GS`)
— none of which the reset had ever touched, unlike IOAPIC state (platform/
chipset, already proven harmful to reset) or RAM (proven not reliably
relevant). First attempt hit `WHvRunVpExitReasonInvalidVpRegisterValue`
immediately after the reset — `TR`'s segment-type field was set to `2`
("LDT descriptor"), reused from the `LDTR` template, but `TR` requires a
genuine TSS type (`0xB`, "busy 64-bit TSS"); WHV validates this and
rejects the inconsistent state. Fixed by giving `TR` its own, correct
attributes (`0x8B`).

**Result across three independent trials, all with the corrected
register set: bugcheck `0x139` still occurs every time, but with wildly
inconsistent timing** — trial 2 survived ~50 real seconds post-reset
before failing, trial 3 only ~12 seconds, trial 4 only ~6.5 seconds. This
is not the signature of a fixed state gap (which would fail
consistently, at a consistent point) — it's consistent with a genuine
race/probabilistic factor. **The expanded architectural reset has been
kept** (unlike the four earlier reverted mitigations) because it's a
real, independent correctness improvement matching documented x86 RESET#
semantics, causes no regressions across the trials, and there's no
principled reason to revert a genuine correctness fix just because it
doesn't happen to resolve this specific bug.

**Considered and ruled out: the `rtcCancelThread` mechanism** (forces
`WHvCancelRunVirtualProcessor` roughly every 1ms whenever RTC's PIE bit
is set, added for the original VPPT fix so a CPU-bound guest spin can't
starve interrupt delivery) as the source of the corruption. It's
*equally* active during the first boot pass (which never fails, across
every test in this entire project) and is confirmed *inactive* during
the exact pre-bugcheck window on the second pass (PIE stays disabled
there, per the earlier RTC diagnostic pass) — so it can't be the
proximate cause of this specific failure's timing, even though the
volume of forced cancellations it generates was a reasonable thing to
suspect.

## Update: live INT3 breakpointing infrastructure

Per an explicit user request: build real live breakpointing rather than
continue testing reset-state hypotheses. Requirements: break at the
reproducible fail-site RVA, capture full register/control-register state,
identify the corrupted pointer and its producer, correlate with recent
hypervisor activity, and keep the instrumentation isolated enough to
disable once the root cause is found.

**Implementation** (all isolated to one block near the top of
`Hypervisor.c` plus two call sites — the reset handler and the main
exit-handling switch — for clean removal later):

- **Breakpoint site**: RVA `0x34420F` (`mov ecx,3`, immediately before the
  `int 0x29` at `0x344214`) rather than the trap instruction itself —
  `R10` (the subsegment pointer, this function's first parameter) is
  proven by static disassembly to never be reassigned anywhere in the
  function body, so breaking one instruction earlier keeps it live and
  correct regardless of which of the three safe-unlink checks failed.
- **Opportunistic module discovery**: since ntoskrnl.exe's KASLR base
  isn't known in advance, the main loop checks (throttled to ~200ms) on
  ordinary VM exits whether the live RIP looks like a plausible canonical
  kernel address and, if so, runs the same backward MZ/PE scan used
  throughout this investigation to find the module and patch `0xCC`
  there. Verified correct via the original-byte readback: `0xB9` (the
  first byte of `mov ecx,imm32`) on every single patch, both boot passes.
- **Event ring buffer**: a 128-entry, timestamped ring recording recent
  VM exits (I/O port access, memory access, halt, cancellation) and every
  interrupt injection, printed only when the breakpoint fires — near-zero
  steady-state cost (a struct write, no I/O) for full correlation data.
- **Exception interception**: `WHvPartitionPropertyCodeExceptionExitBitmap`
  requesting only vector 3 (`#BP`).

**Two non-obvious WHV API gotchas hit and fixed along the way:**

1. `WHvPartitionPropertyCodeExceptionExitBitmap` alone does nothing —
   confirmed the hard way: the guest's own kernel saw and mishandled the
   `INT3`, bugchecking `0x1E` (`KMODE_EXCEPTION_NOT_HANDLED`) with
   `param1=STATUS_BREAKPOINT` and `param2` exactly equal to the patched
   VA (direct proof the CPU executed our `INT3`, but the intercept never
   routed it to us). `WHV_EXTENDED_VM_EXITS.ExceptionExit` must be
   explicitly enabled via `WHvPartitionPropertyCodeExtendedVmExits`
   first.
2. For an exception exit, `exitContext.VpContext.Rip` is the
   breakpoint's *own* address, not one-past-it as raw x86 `INT3` trap
   semantics would suggest — the first version of the RIP-match check
   assumed `+1` and silently rejected a genuine hit.

**A third issue was design, not a bug**: WHV classifies *any* software
`int n` trap (not just genuine `INT3`) as `ExceptionType==BreakpointTrap`
— confirmed live: the guest's own genuine `int 0x29` fast-fail (a 2-byte
`CD 29`, not our 1-byte `CC`) exits through the same path, and later so
does `DbgBreakPointWithStatus` (a real, well-known NT function —
`KeBugCheckEx`'s own first step, normally silently skipped when no
debugger is attached). Attempted faithful re-injection of the original
vector via `WHvRegisterPendingEvent` — WHV rejected it with
`InvalidVpRegisterValue` (likely because software traps aren't cleanly
supported by this generic hardware-exception pending-event path without
an explicit instruction-length field). Settled on two pragmatic rules
instead: (a) for any non-matching `#BP`-class exception *before* our own
hit, just skip the instruction (safe for genuine `INT3` debug-stubs,
logged as a caveat for other `int n`); (b) immediately after our own
breakpoint fires, disable exception interception entirely
(`WHvSetPartitionProperty` with an empty bitmap) so everything
downstream — including the genuine fast-fail and `KeBugCheckEx`'s own
`DbgBreakPointWithStatus` — goes straight to the guest exactly as it
would without us, preserving the existing `KiBugCheckData` capture path.

## Update: root cause identified — deterministic, not probabilistic

With the infrastructure working, captured the live state at the fail site
across two independent boots.

**The registers were byte-for-byte identical both times**: `rax=0,
rcx=0, rdx=0, rbx=2, rsi=0x10, rdi=0, r8=0, r9=8, r10=0, r11=0,
r12=0xFFFFFFFF, r13=0x340, r14=0xFF, r15=0, rflags=0x286, cr0=0x80050033,
cr3=0x1AD002, cr4=0x370678`, `cr8=2` (`DISPATCH_LEVEL`). **This
retroactively resolves the "wildly inconsistent timing" finding from the
architectural-reset trials**: the failure itself is fully deterministic;
only the wall-clock time to *reach* it varies run to run (ordinary
scheduling/timing jitter in an otherwise-identical boot sequence), not
the failure's own parameters. It also resolves a much older open
question from earlier in this investigation (the search for "what raises
IRQL to HIGH_LEVEL"): the fail site itself runs at `DISPATCH_LEVEL`, not
`HIGH_LEVEL` — IRQL only escalates afterward, as an ordinary, expected
step inside `KeBugCheckEx` itself, not from any earlier raise this
project needed to find.

**`R10` (the subsegment pointer) is `NULL`.** Scanning stack memory above
the captured `RSP` and resolving every canonical-kernel-space value
against the trustworthy `ntkrnlmp.pdb` (the same technique used
throughout this investigation) gives the real call chain:

```
RtlpHpLfhSlotAllocate+0xCA6        (heap allocation request)
  -> RtlpHpAcquireLockShared+0x2A   (lock acquisition)
  -> RtlpHpLfhBucketGetSubsegment+0x60   (retrieve a subsegment from a bucket)
    -> RtlpHpLfhOwnerMoveSubsegment(subsegment=NULL, owner, ...)   <- fails
```

**Conclusion**: `RtlpHpLfhBucketGetSubsegment` retrieves a subsegment
from a specific Segment Heap LFH bucket while `RtlpHpLfhSlotAllocate` is
servicing an allocation, finds that bucket unexpectedly empty (`NULL`),
and passes it onward regardless — where the callee's own safe-unlink
check correctly detects the invariant violation and fires the fast-fail
exactly as designed. This is a genuine, specific bucket-bookkeeping
inconsistency (most plausibly a "this bucket has N free entries"-style
count field not matching what's actually resident there), not generic
memory staleness or an ongoing hypervisor-side race — which is why none
of the five earlier state-completeness attempts (targeting RAM content,
RTC state, or general CPU architectural state, never anything Segment
Heap-specific) reliably fixed it. **Not yet fixed** — the concrete next
step is understanding what specifically populates/maintains that
bucket's bookkeeping and why it's inconsistent across the reset.

## Next concrete steps

1. **Locate the specific bucket-tracking state** `RtlpHpLfhBucketGetSubsegment`
   relies on being consistent. Likely candidates: a per-bucket free-count
   or bitmap field somewhere in the Segment Heap's own global/per-owner
   structures. The `owner` parameter (`RDX`, also `0` in both captures —
   worth double-checking whether this is *also* genuinely `NULL`, or an
   artifact of which safe-unlink branch was taken) would be the natural
   starting point to dump and inspect live, now that the breakpoint
   infrastructure can reliably capture it.
2. **Given the failure is now proven deterministic**, a much cheaper
   verification loop is available for any future fix attempt: a single
   trial is sufficient to confirm whether a change altered the outcome
   (no need for the 3+ trial discipline the probabilistic-looking
   failure required earlier) — though still worth a repeat trial once a
   real fix is found, to confirm it holds.
3. **Deferred, likely no longer relevant:** the exact instruction that
   raises CR8 to HIGH_LEVEL on the *original* (pre-reset) stall's live
   call path was never found. Now understood to be moot for the
   *post-reset* stall specifically (that HIGH_LEVEL is bugcheck's own
   final lockdown, not evidence of an unresolved raise site) — may still
   be worth revisiting for the original stall if that ever resurfaces.
4. Dump a larger/further range of the unidentified caller module (e.g.
   bytes 0x4000-0xC000 from its base) to search for an identifying string
   — still useful context if a name is wanted, but not blocking anything.

## Evidence artifacts

- `bp_trial1.log` — first live-breakpoint run, before the `ExtendedVmExits`
  fix: patch confirmed correct (original byte `0xB9`) on both boot passes,
  but the `#BP` intercept never fired; the guest's own kernel bugchecked
  `0x1E` (`STATUS_BREAKPOINT`, `param2` exactly equal to the patched VA) --
  the evidence that led to finding the missing partition property.
- `bp_trial3.log` — first genuine breakpoint hit after both WHV-API fixes
  (`ExtendedVmExits`, correct RIP comparison): full register capture,
  `r10=0x0`, `cr8=DISPATCH_LEVEL`.
- `bp_trial6.log` — second independent boot, after the
  `DbgBreakPointWithStatus` infinite-loop fix (disable exception
  interception right after our own hit): clean run through to natural
  `KiBugCheckData` capture, byte-for-byte identical register state to
  trial 3 -- the evidence that the failure is deterministic, not
  probabilistic.
- `bp_trial7.log` — with stack-scan caller identification added: resolves
  the full call chain (`RtlpHpLfhSlotAllocate` ->
  `RtlpHpAcquireLockShared` -> `RtlpHpLfhBucketGetSubsegment` ->
  `RtlpHpLfhOwnerMoveSubsegment`) against the trustworthy `ntkrnlmp.pdb` --
  the artifact the root-cause conclusion is based on.
- `trapframe_full_scan.log` — the widened trap-frame dump (every qword,
  not just nonzero, with auto-flagged canonical pointers and their nearby
  memory) that led to the "trap frame is likely minimal" conclusion —
  `Rdx` never turns up anywhere despite the fail-site code needing it live.
- `full_arch_reset_trial1.log` — first attempt at the expanded
  architectural reset; hit `WHvRunVpExitReasonInvalidVpRegisterValue`
  immediately after the reset (`TR`'s descriptor type was invalid).
- `full_arch_reset_trial2.log`/`trial3.log`/`trial4.log` — three
  independent trials of the corrected architectural reset. All three
  eventually hit bugcheck `0x139`, at wildly inconsistent real-time
  offsets from the reset (~50s / ~12s / ~6.5s) — the evidence base for
  concluding this is a probabilistic race, not a fixed state gap.
- `NewStallRip.bin` — raw bytes around the live RIP, disassembled offline
  (no symbols) to find the spin-loop shape described above.
- `NewStallModule.bin` — 0x4000 bytes from the unidentified module's base;
  inconclusive string search so far.
- `spinwait_poll.log` — 90-second observation window showing the polled
  target value frozen at `0x0` across 45 samples.
- `module_id.log` — module-base identification and "no debug directory"
  confirmation.
- `pci_ring_test2.log` — full PCI config-space access ring buffer (512
  entries) confirming a clean, complete bus-0 enumeration sweep ending
  exactly at `dev=31 func=0 off=0x3C`, with zero further I/O before the
  freeze.
- `cpuid_fix_test.log` — confirms the CPUID leaf 1 logical-processor-count
  override is applied correctly (guest-visible EBX `0x00010800`, HTT bit
  cleared) but the identical deadlock still occurs, ruling out that
  hypothesis.
- `irql_classify_test.log`, `irql_repro2.log` — CR8/IRQL capture showing
  `HIGH_LEVEL` (0xF) sustained across 10+ consecutive watchdog samples in
  one run and reproduced identically on a second, independent boot.
- `caller_resolve_test2.log` — resolves the immediate caller's module via
  the return address at `[rsp+0x18]`, reproduced at the same RVA
  (`+0x36D16D`) across two independent boots; confirms it's not
  `ntoskrnl.exe` (no CodeView debug-directory entry).
- `CallerContext2.bin`/`.disasm.txt` — the realigned, disassembled caller
  code showing the `cli`/`sti` + `lock inc`/`lock dec`-bracketed
  registered-callback dispatch pattern described above. Confirms the
  caller itself doesn't touch CR8 — the real IRQL raise is still
  unlocated.
- `section_test.log` — the watched module's own PE section table, showing
  the watched RVA (`0x21E8`) falls inside a fully file-backed `.data`
  section (`RawSize == VirtualSize`, no zero-fill tail), confirming a
  genuine static global rather than a pool-allocated kernel object.
- `ModuleTextSection.bin`/`.disasm.txt` — the driver's complete `.text`
  section (all 4096 bytes, all 859 disassembled instructions), used to
  manually verify every RIP-relative reference in the entire module
  against the watched RVA — confirms nothing but the spin loop's own two
  reads ever touches it, and reveals the PCI host-bridge chipset-ID
  dispatch code (matching our own emulated i440fx exactly) living in the
  same module.
- `regobj_test.log` — the registration object's slots, read live: mostly
  densely populated with real kernel-mode pointers, with two clear gaps
  of null slots at `+0x048`-`+0x088` and `+0x158`-`+0x1A8`.
- `wide_dump_test.log` — confirms the 8KB caller-dispatcher dump
  (`callerModuleBase + 0x36D000`, VA logged) succeeded and no bugcheck
  regression occurred.
- `CallerWideDump.bin`/`.disasm.txt` — the full 8KB disassembled dispatcher
  window; used to enumerate every registration-pointer load and its
  following `[rax+N]` access, showing the complete offset set
  `{0x00, 0x08, 0x10, 0x28, 0x30, 0x38, 0x40}` and the function boundary
  (`ret`+`int3` padding at RVA `0x36D4EB`) after which unrelated code
  begins — the basis for ruling out both null-slot gaps as this caller's
  producer slot. Also contains the family of genuine `mov cr8`
  save/raise/restore helper functions found further in the same module
  (RVA `0x36DB98`-`0x36EFE2`), later shown to be unrelated to the actual
  executed path.
- `stackwalk_test.log` — first live stack walk (192 qwords from RSP),
  resolving every kernel-space-looking candidate to module + RVA; found
  the second module (later identified as acpi.sys) and the cluster of
  candidates in the already-known caller module.
- `stackcand_test.log` — second run, same stack walk plus a code-window
  dump around every resolved candidate; the source for all `StackCand_*`
  files below and the run in which `acpi.pdb`'s GUID/age was captured.
- `StackCand_0x310.bin`/`.disasm.txt`, `StackCand_0x350.bin`/`.disasm.txt`,
  `StackCand_0x360.bin`/`.disasm.txt` — the three acpi.sys stack
  candidates, disassembled with the real, matching `acpi.pdb` (downloaded
  from Microsoft's public symbol server by GUID/age). All three resolve to
  GUID/WPP-trace-data regions, not code — the basis for ruling out acpi.sys
  as a false positive (stale stack data, not a live return address).
- `StackCand_0x48.bin`/`.disasm.txt`, `StackCand_0x88.bin`/`.disasm.txt`,
  `StackCand_0xB8.bin`/`.disasm.txt`, `StackCand_0xE8.bin`/`.disasm.txt` —
  the unnamed-caller-module stack candidates that reconstruct the genuine
  call chain (`0x3C0D58` → `call 0x367730` → default path → `call
  0x36789C` [the reset-attempt function] → `call 0x36D120` [dispatch
  wrapper true entry] → `0x36799D` return, matching `[rsp+0x48]`
  independently). Also contains the two `mov cr8` sites later shown to be
  on not-taken branches.
- `resetwatch_test.log` — zero-behavior-change logging of port `0x70`=
  `0x0F` and port `0x64`=`0xFE` writes. Shows two genuine kernel-space-RIP
  writes of `0xFE` to port `0x64` (the keyboard-controller reset pulse) at
  the same `exitCount` as the first stall detection — live confirmation
  that the guest is attempting a system reset immediately before freezing.
- `resetexperiment_test.log` — **the decisive artifact.** With the
  experimental reset handler in place: shows the reset being honored
  (`exitCount=1399336`), the guest's firmware genuinely re-running its
  early POST sequence (same RTC port-0x70 writes/RIPs as the original cold
  boot), and the guest progressing to a new, different failure —
  `KiBugCheckData: code=0x139` (`KERNEL_SECURITY_CHECK_FAILURE`) — instead
  of the original frozen spin-wait. Confirms the reset-mechanism
  hypothesis experimentally.
- `acpi.pdb` (scratchpad, not copied into evidence — large,
  re-downloadable) — the real Microsoft-signed PDB for the exact acpi.sys
  build in this guest, fetched by GUID/age from
  `https://msdl.microsoft.com/download/symbols/`, used to definitively
  rule out (rather than assume) the acpi.sys stack candidates.
- `bugcheck139_test.log` — with the exception-record parsing code in
  place: shows the parsed `EXCEPTION_RECORD` (`code=0xC0000409`,
  `address=0xFFFFF8070B944214`, `info[0]=0x3`) and its resolution to
  ntoskrnl.exe RVA `0x344214`, which `ntkrnlmp.pdb` names as
  `RtlpHpLfhOwnerMoveSubsegment+0xE8` — the basis for redirecting the
  bugcheck-0x139 root cause from device state to kernel heap metadata.
- `failsite_test2.log` — reproduces the same fault RVA (`0x344214`) on an
  independent boot, plus the full raw trap-frame qword dump used to
  correct the earlier wrong `KTRAP_FRAME` offset guess (`Rip` is at
  `+0x168`, not `+0x178`; `+0x178` is `EFlags`).
- `FailSite.bin`/`.disasm.txt` — 0x200 bytes of live ntoskrnl.exe code
  around the fault RVA, disassembled with the trustworthy `ntkrnlmp.pdb`;
  shows the three safe-unlink checks inside `RtlpHpLfhOwnerMoveSubsegment`
  that all funnel to the same fast-fail call, the basis for why the exact
  corrupted pointer couldn't be pinned from the fault RIP alone.
- `ramclear_falsify_test.log` — the first falsification-test artifact,
  from a temporary, since-fully-reverted `#ifdef`-gated build (clears all
  guest RAM). Shows the false-alarm "STALL #1" caused by the blocking 3GB
  `memset` itself, then the guest resuming and reaching `BdsDxe: starting
  Boot0002` with no bugcheck `0x139` in this one run. **Caveat added
  later: this result did not replicate on repeated trials** (see the
  "RAM-clear fix did not replicate" update) — treat as a single
  data point, not a confirmed finding.
- `ramclear_above16mb_test.log` — the second, narrower falsification-test
  artifact (also temporary, also since-fully-reverted), clearing only
  guest RAM above the legacy 16MB boundary. Reproduced the same one-run
  positive result. **Same caveat: not reproduced on repeat testing** —
  see `reliability_trial1.log` and `narrow_clear_test.log` below.
- `dump_trapframe_layout.py` (scratchpad, not copied — a diagnostic
  script, not evidence data) — attempted to query `_KTRAP_FRAME`'s real
  field layout from `ntkrnlmp.pdb`'s type info via dbghelp; the query
  failed (`TI_GET_CHILDREN` error), indicating this public PDB doesn't
  carry full struct layout for this internal type. Left in scratchpad in
  case a future investigation wants to retry against a different PDB.
- `reliability_trial1.log` — full above-16MB clear made permanent, then
  retested: bugcheck `0x139` recurred, showing the original single success
  was not representative.
- `narrow_clear_test.log` — narrower/faster 240MB clear (to test the
  blocking-duration/timeout-starvation hypothesis): still recurred,
  disproving that specific hypothesis.
- `rtcarmed_isolation_trial1.log` — `rtcPeriodicTickArmed`/
  `lastRtcPeriodicTick` reset alone, no RAM clear: still recurred.
- `combined_fix_trial1.log` — RAM clear + `rtcPeriodicTickArmed` reset
  combined: guest got further before failing, but still eventually
  recurred.
- `final_state_confirm.log` — the final, simplified code state
  (CPU-register reset only, all RAM/RTC mitigations removed): confirms
  the reset itself still reliably works, and bugcheck `0x139` still
  occurs, consistent with this being the honest, current baseline.

## 2026-07-17 update: entry-point retargeting, a real re-arm bug fix, and a new blocking early-boot regression (unresolved)

**Two real bugs found and fixed in the breakpoint infrastructure itself,
independent of the Segment Heap question:**

1. **Retargeted the live breakpoint from the fail site (RVA `0x34420F`) to
   `RtlpHpLfhOwnerMoveSubsegment`'s true entry (RVA `0x34412C`).** Static
   disassembly showed `xor edx,edx` at RVA `0x344195` on an intermediate
   success path could legitimately reassign `RDX` before reaching the fail
   site, so the previously-captured `RDX=0` there was not trustworthy as
   the genuine `owner` parameter. At the true entry, `RCX`/`RDX`/`R8` are
   the genuine, unclobbered parameters (`subsegment`/`owner`/`flags`).
   Since entry is hit on *every* call (not just the failing one, unlike
   the old fail-site address), added re-arm logic: cheap `RCX`-only check,
   log-and-continue for ordinary calls (`RCX != 0`), full report only for
   the known-failing signature (`RCX == 0`).

2. **The re-arm logic itself was broken**: it restored the original byte,
   reset RIP to the target VA, then immediately rewrote `0xCC` *before*
   resuming — so the CPU never actually executed the real instruction
   underneath. Every "call" logged was the same trap retriggering forever,
   not distinct calls (confirmed: 122,954 log lines, all showing the
   identical `subsegment`/`owner` pair). Fixed by emulating the specific
   patched instruction (`mov qword ptr [rsp+8], rbx`, 5 bytes:
   `48 89 5C 24 08`) directly — write `RBX` to `[RSP+8]` in guest memory,
   advance `RIP` by 5 — instead of ever letting hardware execute it. The
   `0xCC` byte is left in place permanently for ordinary calls; only the
   final failing-call report restores the original byte and disables
   exception interception, exactly as before.

**Both fixes are correct and kept.** They were never actually verified
end-to-end (i.e. the entry-point owner dump was never captured) because
rebuilding to apply them exposed an unrelated, pre-existing regression
that now blocks boot before the guest ever reaches kernel code.

### New blocking regression: deterministic early-boot hang polling ACPI PM1a_CNT

**Symptom (14/14 trials after rebuilding, 100% reproducible, independent
of every variable tested):** SeaBIOS hangs during early POST, before
Windows even begins loading, spinning at a fixed RIP reading port `0x604`
(`PM1a_CNT_BLK`, `pmBase+4`):

```
rip=0xF2BC1 rsp=0xFCC rax=0x0 rcx=0xFCC rdx=0x604 rbx=0x0 cr3=0x0 cr8=PASSIVE_LEVEL
```

Crucially, the stall watchdog's `exitCount` (incremented once per
`WHvRunVirtualProcessor` return, for *every* exit reason including
`Canceled`) is **completely frozen** — not climbing rapidly (which a real
guest-side spin with per-iteration I/O traps would produce), not moving at
all, even under repeated forced `WHvCancelRunVirtualProcessor` calls. This
means `WHvRunVirtualProcessor` itself is not returning, for reasons this
investigation could not pin down with print-based diagnosis alone.

**Immediately precedes the freeze, confirmed via targeted diagnostic
(since removed):** SeaBIOS writes `0x2000` (`SLP_EN` with `SLP_TYP=0` — a
harmless S0 no-op, not an actual sleep request) to `PM1a_CNT` at
`rip=0xF2BBD`, 4 bytes before the frozen RIP.

**Hypotheses tested and ruled out, in order:**

1. **SCI_EN clobbering.** The blind write handler
   (`pm1aControl = (UINT16)Rax`) let the `0x2000` write silently clear bit
   0 (`SCI_EN`), which — since the FADT declares `SMI_CMD=0` ("ACPI
   already enabled, no handshake") — could never be re-set, so any poll
   for it would spin forever. **Real bug, fixed** (pin `SCI_EN` through
   writes: `pm1aControl = Rax | 0x1`) **but did not resolve the hang.**
2. **`rep`-prefixed string I/O mishandled.** Ruled out directly: a
   dedicated diagnostic on the exact stalling access confirmed
   `StringOp=0, RepPrefix=0, AccessSize=2` — an ordinary `out dx,ax`/
   `in ax,dx`, not a string instruction.
3. **Missing PIT (IRQ0) interrupt delivery during a non-halted CPU-bound
   spin.** IRQ0 injection previously only happened in the halted-CPU wait
   branch; a genuine zero-VM-exit native busy-wait (common in early BIOS
   calibration code) would never receive a timer tick. **Real gap, fixed**
   (`deliverPitTimerIrq()`, called unconditionally in the main loop
   alongside `deliverRtcPeriodicIrq`) **but did not resolve the hang** —
   and broadening `rtcCancelThread` to force `WHvCancelRunVirtualProcessor`
   unconditionally every 10ms (needed to actually invoke the new delivery
   function during a true zero-exit spin) made the hang *more* consistent
   (13/13 immediate, vs. building up over ~60-76s before), not less —
   **reverted** that broadening back to its original RTC-periodic-gated
   behavior; the delivery function itself was kept (harmless, still a real
   fix for the natural-exit case) but this makes clear that aggressively
   forcing `WHvCancelRunVirtualProcessor` perturbs WHV rather than helping.
4. **Our own per-iteration polling functions** (`deliverPendingAtaIrq`,
   `deliverPendingRtl8139Irq`, `deliverRtcPeriodicIrq`, `deliverPitTimerIrq`,
   `ahciProcessPendingCommands`, `netPollUdpSessions`, `netPollTcpSessions`
   — the last two newly link-viable this session after adding `ws2_32.lib`
   to fix unresolved-symbol errors, making them suspect as never
   previously exercised). **Ruled out via bisection**: sequential markers
   around each call all fired in the one iteration that immediately
   preceded the hang — every one of these functions completes normally.
5. **Host CPU load.** Measured 36%-58% across trials with no correlation
   to outcome (hung identically at both ends of that range).
6. **Guest VHD corruption from repeated unclean process kills.**
   `win10_installer.vhd`'s `LastWriteTime` is untouched by any of today's
   trials (still 2026-07-15), ruling out on-disk guest state as the
   variable.

**Not yet tried:** attaching a real debugger (WinDbg) to `Hypervisor.exe`
to see exactly where the blocked thread is stuck (inside
`WHvRunVirtualProcessor`, inside `WHvCancelRunVirtualProcessor`, or
somewhere else entirely) — print-based diagnosis is exhausted for this
specific question. **Also not yet tried: a full system reboot**, to test
whether this is accumulated Windows Hypervisor Platform host-state (the
current leading hypothesis, given the hang is reproducible regardless of
any code change tested and regardless of host CPU load) rather than
anything in `Hypervisor.c` at all. Investigation paused here at the user's
request to reboot and retry.

**Current code state:** all three real fixes described above are kept
(re-arm instruction emulation, `SCI_EN` pinning, `deliverPitTimerIrq`);
the aggressive `rtcCancelThread` broadening is reverted; all temporary
`[diag]`/`[diag2]`/`[diag3]`/`[bisect]` prints have been removed. The
codebase builds cleanly (`cl.exe /O2`, no errors, only pre-existing
`strncpy`/`fopen` deprecation warnings).

### Evidence artifacts (this update)

- `bp_entry_trial1.log` — first entry-point breakpoint run, before the
  re-arm bug was found: 122,954 lines, one repeating `subsegment`/`owner`
  pair, no `BREAKPOINT HIT` — the artifact that exposed the re-arm bug.
- `bp_entry_trial2.log` through `bp_entry_trial14.log` — successive trials
  through the ACPI stall investigation; all show the identical
  `rip=0xF2BC1` freeze signature once the guest reaches early SeaBIOS PM
  setup. `bp_entry_trial9.log` is the one with the `[diag3]` capture of
  the `0x2000` write immediately preceding the freeze.
- `FailSite.disasm.txt` — re-consulted to confirm the entry instruction
  (`0x34412C: mov qword ptr [rsp+8], rbx`) and its 5-byte length, used for
  the re-arm emulation fix.

## 2026-07-17 update: the early-boot regression's real root cause (not accumulated host state)

Per the user's request to reboot and retry (see the previous status note),
the exact same boot was re-run post-reboot. **The original SeaBIOS/OVMF
`PM1a_CNT` freeze (`rip=0xF2BC1`, frozen `WHvRunVirtualProcessor`) did not
reproduce** — some evidence for the accumulated-host-state theory. But a
different, earlier, 100%-reproducible failure took its place instead: OVMF
itself crashed with a genuine `#GP` (General Protection fault), confirmed
via its own built-in exception dump, immediately after this hypervisor's
`acpiBuildTables()` fired and the guest read the resulting fw_cfg files
(`etc/acpi/rsdp`/`etc/table-loader`, selectors `0x20`/`0x21`).

**Live capture, not static guessing.** Static review of `acpiBuildTables`/
`acpiBuildLoaderScript` (offsets, checksums, pointer-patch ranges, buffer
bounds) turned up nothing wrong by hand. Added a temporary one-shot live
`#GP` intercept (same `WHvPartitionPropertyCodeExceptionExitBitmap`
mechanism already used for the `#BP` infrastructure, extended to also
request vector 13) to capture full register state, the faulting
instruction's raw bytes (available directly from
`WHV_VP_EXCEPTION_CONTEXT.InstructionBytes`, no guest memory read needed),
and a code/stack window via `kernelReadVA`.

**Root cause: a misaligned `FXSAVE`.** The faulting instruction, decoded
from the captured bytes, is `fxsave [rdi]` (`0F AE 07`), preceded by
`push rax`×6, `sub rsp, 0x200`, `mov rdi, rsp` — a routine reserving a
512-byte `FXSAVE_STATE`-sized scratch buffer and saving FPU/SSE state into
it. `RDI` at the fault was `0xBFD4FC68` — `mod 16 == 8`, not 16-byte
aligned, which is exactly what `FXSAVE` requires or it raises `#GP(0)`
(confirmed: `errorCode=0x0`, matching an alignment fault rather than a
segment-selector fault). `RFLAGS.IF=0` at the fault, consistent with this
code running inside an interrupt/exception entry stub (interrupt gates
auto-clear IF). Since the six pushes (48 bytes) and the `sub rsp,0x200`
(512 bytes) are both 16-byte-preserving, the misalignment was inherited
from whatever called into this routine — i.e. an *asynchronous* event
(an injected interrupt) landing on an arbitrary guest `RSP`, not a normal
`call`-boundary-respecting invocation.

**Falsification test — decisive.** The two newest main-loop changes in the
codebase (from earlier this same session, predating the first appearance
of the SeaBIOS hang) are the unconditional, every-VM-exit calls to
`deliverRtcPeriodicIrq()`/`deliverPitTimerIrq()` (see the "2026-07-17
update" entry above this one, part 3: "force periodic VM exits during
CPU-bound spins"). Temporarily removing just those two calls (`#if 0`,
purely for the test) and re-running the identical boot: **no `#GP`, no
stall** — the guest sailed straight through the same ACPI-table-load point
and all the way to `BdsDxe: starting Boot0002`, into real AHCI disk reads,
far past where either failure mode had ever allowed progress. (It did
eventually stall later for an unrelated, expected reason — a guest that
receives *zero* timer interrupts at all eventually gets stuck waiting on a
scheduling tick — not a new bug, just the blunt test running out of
runway.)

**Conclusion.** `injectInterrupt()` bypasses any PIC/APIC model and
directly asserts `WHvRegisterPendingInterruption` on every main-loop tick
regardless of what the guest is doing. Calling `deliverRtcPeriodicIrq`/
`deliverPitTimerIrq` after *every* VM exit (PCI config reads, I/O port
probes, MMIO — whatever exit reason happened to fire) injects at an
essentially random guest `RSP`. About half the time that leaves OVMF's own
exception/interrupt-entry stub's internal stack 8-mod-16 instead of
16-aligned, and its `FXSAVE` step — which doesn't appear to defensively
realign — faults. This also retroactively reframes the pre-reboot
"accumulated WHP host-state" theory as very likely wrong: nothing in the
code differed before/after the reboot (this bug was already present); the
reboot just changed non-deterministic timing enough that the same root
cause manifested as a different symptom (a hang instead of a crash).

**Fix implemented (narrowed delivery, not a full architectural fix).**
Per explicit user direction to gate delivery more narrowly again rather
than either fully reverting to the pre-fix starvation bug or building full
interrupt coalescing/masking: removed the two calls from the unconditional
per-exit block entirely, and instead call them only from the two points
already proven safe/necessary — the halted-CPU wait loop (a single,
consistent resume RIP each time; unchanged) and the
`WHvRunVpExitReasonCanceled` case (the bounded, `rtcCancelThread`-driven
forced-exit mechanism this delivery was originally built for; unchanged
cadence). `rtcCancelThread` itself was deliberately **not** broadened
further — the "2026-07-17 update" entry above this one already found that
broadening its cancellation cadence unconditionally (there, to 10ms) made
a different stall *more* reproducible, not less.

**Accepted residual gap.** A plain PIT-only busy-spin before the RTC's
periodic-interrupt-enable bit is ever armed (`rtcCancelThread`'s only
trigger condition) still won't get a forced exit under this narrower
scheme, since there's no equivalent forced-cancellation source that early.
This is a known, documented trade-off, not an oversight — re-broadening
the cancel thread to cover it was explicitly rejected given the prior
negative result above.

**Verification.** Three independent post-fix boots, no `#GP`, no OVMF
crash-dump text, reliably reaching `BdsDxe` (twice per run — the guest's
own expected mid-install reset) and, in the longer runs, back to the
already-known, pre-existing `bugcheck 0x139` Segment Heap corruption this
investigation was chasing before the regression appeared — confirming the
regression is closed and the project is back on its original, correct
track. The temporary `#GP`-capture diagnostic (exception-bitmap widening,
the capture block itself, `g_gpDiagCaptured`) was removed after use,
matching this project's established pattern for one-shot live captures.

### Evidence artifacts (this update)

- `gp_diag_test1.log`/`gp_diag_test2.log` — the two live `#GP` captures:
  first pass identified the faulting `fxsave [rdi]` and its misaligned
  `RDI`; second pass widened the stack dump (0x280 bytes) which, on closer
  reading, turned out to be OVMF's own partially-built exception-context
  record (CR0/CR2/CR3/CR4/DR6/DR7/GDTR/TR fields all present and
  plausible) rather than a return-address stack — consistent with this
  fxsave being the FPU-context-save tail of OVMF's common exception/
  interrupt entry stub.
- `falsify_no_timer_irq_test1.log` — the decisive falsification test:
  RTC/PIT delivery calls temporarily removed from the unconditional
  per-exit block; boot reaches `BdsDxe` and 90k+ log lines of real AHCI
  disk activity with no `#GP` and no stall.
- `narrowed_delivery_test1.log` — first test of the actual (narrowed,
  Halted+Canceled-only) fix: clean past ACPI, reaches `BdsDxe` twice, then
  the guest's own mid-install reset, landing on the pre-existing bugcheck
  `0x139` — confirms the fix works and the project is back on the original
  Segment Heap investigation.
- `final_confirm_test1.log` — repeat confirmation after removing the temp
  `#GP` diagnostic instrumentation: clean, reaches `BdsDxe` twice, no
  exceptions logged.

## 2026-07-17 update: the Segment Heap bug, traced five call-chain levels with real symbols -- root mechanism identified

With the ACPI regression closed and boot reliably reaching the original
bugcheck `0x139` again, resumed the Segment Heap investigation per an
explicit user request to verify the existing breakpoint capture and keep
following the evidence until root cause or a genuine block. This update
traces the NULL parameter chain five levels further than any prior pass of
this investigation, cross-validated at every step against a **downloaded,
matching `ntkrnlmp.pdb`** (fetched live from Microsoft's public symbol
server by the GUID/age already established) **and a real disassembler**
(Python + capstone, installed this session) rather than continuing on hand
disassembly alone -- both new tooling additions this session, kept in the
scratchpad for reuse.

**Step 1 -- re-verified the existing capture, corrected an assumption.**
Re-ran the already-built live breakpoint at `RtlpHpLfhOwnerMoveSubsegment`'s
true entry (RVA `0x34412C`). Confirmed, at the genuine unclobbered entry:
**both `subsegment` (RCX) and `owner` (RDX) are NULL** -- not just
`subsegment`, correcting the standing assumption from the original pass
(which only ever captured `owner` after an intermediate `xor edx,edx` had
already clobbered it). `[rsp+0x00]` at true entry is the real return
address; resolving it identified the immediate caller precisely as
`RtlpHpLfhBucketGetSubsegment+0x60` (matching the very first, months-old
static-disassembly finding this investigation started from, now confirmed
live) -- and, critically, that the NULL `subsegment` value is that caller's
own **unmodified incoming parameter**, not something that went wrong
inside its own list-walking logic. This pushed the search one frame
higher than the entire investigation had ever looked.

**Step 2 -- retargeted the breakpoint to the grandparent's call site.**
Computed the call instruction's address (return address minus 5) and
retargeted `BP_FAIL_SITE_RVA` there (RVA `0x2382C1`), which required
building genuine `call rel32` emulation for the "ordinary call, re-arm"
path (reading the real rel32 bytes live rather than hardcoding them, then
performing real push-return-address-and-jump semantics) since the
previous re-arm logic only knew how to emulate the old site's `mov`.
Live capture confirmed `rbp+0x58` (read directly from guest memory) is
genuinely `0` at the call, and identified the grandparent function's
return address via a wide code-window dump.

**Step 3 -- downloaded the real PDB and got a genuine disassembler.**
Rather than keep compounding hand-decoded x86-64 (this project's own
history repeatedly flags manual disassembly as error-prone), fetched
`ntkrnlmp.pdb` live from `msdl.microsoft.com` by the GUID/age already on
record (`D9424FC4861E47C10FAD1B35DEC6DCC8`, age `1`) and resolved every
RVA identified so far via `dbghelp.dll` (`SymFromAddr`, called through
Python ctypes -- `scratchpad/resolve_syms.py`). **Result: exact,
independent confirmation of the entire call chain the original
investigation's very first static pass established months ago**,
byte-for-byte matching its own citations:
`RtlpHpLfhSlotAllocate+0xCA6` → `RtlpHpAcquireLockShared+0x2A` →
`RtlpHpLfhBucketGetSubsegment+0x60`. The "grandparent" is
`RtlpHpLfhSlotAllocate` itself -- confirmed, not inferred.

**Step 4 -- installed capstone, disassembled the full function, corrected
a second wrong inference.** Extended the breakpoint capture to dump
`RtlpHpLfhSlotAllocate`'s entire body from its own start through the call
site (0xCA1 bytes, read live from guest memory) and disassembled it
properly (`scratchpad/disasm_slotalloc.py`). This definitively ruled out a
missed conditional branch (none target the lead-up region) and, more
importantly, **overturned this session's own earlier hypothesis that
`R13=0x340` was a corrupted/garbage pointer**: stack-frame arithmetic
(`rbp = entry_rsp - 0x48`) proves `[rbp+0x50]`/`[rbp+0x58]` are simply
`RtlpHpLfhSlotAllocate`'s own two incoming parameters, spilled to their
home slots at entry and never written anywhere else in the function
(12 reads, 0 writes across the whole disassembled span). `0x340` (832) is
just an ordinary size value, not a pointer -- a case of this session
catching and correcting its own mistaken inference via live evidence,
matching this project's established discipline.

**Step 5 -- retargeted to `RtlpHpLfhSlotAllocate`'s own entry, found
`ExAllocateHeapPool`.** Retargeted the breakpoint one level further
(RVA `0x237620`, the function's true entry), which required a third
distinct re-arm emulation (this entry's own patched instruction, `mov
dword ptr [rsp+0x20], r9d`). This site is hit far more often than the
previous ones (called for essentially every small kernel allocation, not
just failing subsegment retrievals -- the failing call this run was
attempt **#82,153** out of many ordinary ones, requiring logging to be
throttled to avoid flooding). The captured return address resolved to
**`ExAllocateHeapPool+0x2B1`** -- a well-known, top-level NT kernel pool
API, not an internal Segment Heap helper. `RCX` (first param, `0x340`) and
`RDX` (second param, `0`) at entry are `RtlpHpLfhSlotAllocate`'s genuine,
unclobbered parameters.

**Step 6 -- disassembled `ExAllocateHeapPool`'s lead-up, found the true
source.** Dumped and disassembled the call site's immediate lead-up, then
the whole function from its own start. The lead-up shows:

```
mov rdx, r10
mov rcx, rdi        ; rdi was set earlier via `lea rdi, [r13 + 0x340]`
call RtlpHpLfhSlotAllocate
```

Since the captured `RCX` at the callee's entry is exactly `0x340` (not
`r13+0x340`), **`r13` itself must be `0` here** -- meaning both corrupted
parameters two frames down are ultimately derived from this single NULL
register, not two independent problems. `R13` is used throughout the
lead-up as a heap-manager object pointer (`[r13+0x18]`, `[r13+0x37c]`).
Searching the full function body for every write to `R13` found exactly
one:

```
imul rcx, r9d, 0x20C0          ; rcx = index * 0x20C0  (per-entry struct size)
lea rax, [rip + 0xA213D0]      ; rax = &GlobalArray
add rcx, rax                    ; rcx = &GlobalArray[index]
...
mov r13, qword ptr [rcx + 2*8]  ; r13 = GlobalArray[index].field[2]
```

Computing the RIP-relative target and resolving it against the real PDB:
**`ExPoolState + 0x3900`** -- `nt!ExPoolState`, the well-known top-level
Executive Pool State global. `R13` (and therefore everything downstream)
is NULL because this specific slot -- `ExPoolState+0x3900 +
(index * 0x20C0) + 0x10`, some per-pool-type/NUMA-node heap-descriptor
array entry -- is empty at the moment `ExAllocateHeapPool` reads it.

**Full chain, confirmed end-to-end with real symbols:**

```
ExPoolState+0x3900 + (index*0x20C0) + 0x10   [NULL heap-descriptor slot]
  -> ExAllocateHeapPool+0x2B1                 [reads it into R13]
    -> RtlpHpLfhSlotAllocate(size=NULL+0x340, heap=NULL)
      -> RtlpHpLfhBucketGetSubsegment(container=NULL)
        -> RtlpHpLfhOwnerMoveSubsegment(subsegment=NULL, owner=NULL)
          -> safe-unlink check correctly detects the invariant violation
          -> fast-fail -> bugcheck 0x139
```

**This reframes the bug's category entirely.** Every step of the actual
fail path (the safe-unlink check, the fast-fail) is Windows working
*correctly* -- it's detecting and reporting a real invariant violation
exactly as designed. The genuine defect is upstream: whatever should have
populated this specific `ExPoolState` slot with a real per-pool-type
Segment Heap instance apparently never did, on this specific (second,
post-reset) boot pass. This is consistent with, and sharpens, everything
this investigation already suspected about the reset boundary -- but
`ExPoolState`'s exact structure and this array's indexing/initialization
semantics are sparsely documented, esoteric Windows-internals territory
beyond what static analysis alone can settle quickly.

**Not yet fixed, and one level short of a fully closed root cause.** What's
proven: exactly which global slot is empty and the complete mechanism by
which that NULL propagates to the fast-fail, independently verified with
real symbols at every hop. What's not yet proven: *why* that slot is empty
-- whether it's a lazy-initialization path that never runs a second time
across this hypervisor's CPU-register-only reset, an index computed
differently on the second pass, or something else.

## 2026-07-17 update: ExPoolState array survey -- systemic, not a single slot

Per the standing instruction to keep following the evidence: rather than
isolate the exact index register used by the failing call (would need a
second, simultaneous breakpoint -- a bigger infrastructure change), took a
faster, more informative path -- surveyed the array directly. Extended the
existing `RtlpHpLfhSlotAllocate`-entry breakpoint report to dump
`ExPoolState+0x3900 + (i*0x20C0) + 0x10` for `i=0..15` in one shot at the
exact moment of the failing call (same nested-call context, no new
breakpoint site needed).

**Result: all 16 surveyed indices are NULL**, not just the one the failing
call happened to use:

```
[ 0] = 0x0   <-- NULL
[ 1] = 0x0   <-- NULL
...
[15] = 0x0   <-- NULL
```

**This rules out "wrong index computed for this specific allocation"**
(which would show one or a few NULL entries against a background of
populated ones) **and instead points at something systemic**: on this
second, post-reset boot pass, this entire per-index heap-descriptor array
-- or at least this specific field across every index -- has not been
populated at all, not even for whichever indices a real, healthy boot
would have already touched by this point. This sharpens, but does not yet
close, the standing "something needs proper re-initialization across the
reset" theory this investigation has circled since the original RAM-clear
falsification tests (which had a real but unreliable effect) -- the target
is now specific and named (`ExPoolState`'s per-index array), not a vague
"bucket bookkeeping" guess.

**Genuinely open, and the natural next phase:** two live possibilities,
not yet distinguished: (a) whatever normally lazily populates each index
on first use simply never runs a second time post-reset for any index
(a broader initialization-path gap than previously suspected), or (b) this
array was legitimately populated during the *first* boot pass and the
guest's own reset-adjacent teardown path explicitly clears it, expecting
a genuine cold boot's memory manager to rebuild it -- which this
hypervisor's CPU-register-only reset (RAM deliberately left untouched,
per the earlier bugcheck-0x139 falsification history) doesn't trigger.
Distinguishing these requires finding and disassembling whatever function
actually writes to this array (not yet located -- a new, open-ended search,
distinct in kind from the call-chain tracing done so far) and/or watching
this exact array across the reset boundary in real time rather than only
at the failure point. Left here as the investigation's current frontier.

### Evidence artifacts (this update)

- `bp_capture_run9.log` — the ExPoolState array survey: all 16 entries'
  `field+0x10` read live as `0x0` at the moment of the failing call,
  confirming the gap is systemic rather than one bad index.

## 2026-07-17 update: locating the initializer -- ExInitializePoolHeapManagement, and a precise correction

Per an explicit user request to locate the function(s) that populate
`ExPoolState`, identify both init and teardown paths, and determine
whether the array is never initialized on the second boot pass or
explicitly cleared without rebuild -- **without changing reset behavior**.

**Static candidate search.** Rather than guess, enumerated the real public
symbol table (`dbghelp.dll` `SymEnumSymbols` against the downloaded
`ntkrnlmp.pdb`, patterns like `*Init*Pool*`, `*ExpTearDown*`,
`*Teardown*`, `*ExpDestroyPool*`) -- no new hypervisor changes needed for
this step, purely offline analysis. Found `ExInitializePoolHeapManagement`
(RVA `0x3C3B64`) as the strongest name-matched candidate. **No matching
teardown/destroy function exists anywhere in the public symbol table** --
checked several plausible name patterns, all either no match or clearly
unrelated subsystems (FsRtl/Psp/Cc/Ps/Ob). This is itself a data point:
if a normal Windows code path explicitly tore down pool state as part of
any routine reinitialization, a named function for it would very plausibly
exist and be findable this way.

**Live confirmation: it runs, at least once, during pass 1.** Retargeted
the breakpoint to this function's entry (required building a third,
distinct re-arm emulation for its own opening instruction, `mov rax, rsp`
-- 3 bytes, register-only, the simplest of the three built this session).
Confirmed hit reliably early in pass 1 across four independent runs.

**Disassembled the function body -- confirms it writes into the ExPoolState
array, but to a *different* field than the one found NULL.** The captured
self-code-dump (1024 bytes from true entry) shows, unambiguously:

```
imul r14, rax(index), 0x20C0
lea  rax, [rip + 0x8942D6]      ; -> RVA 0xC57EC0 = ExPoolState+0x3900 (exact match to the array found earlier)
add  r14, rax                   ; r14 = &array[index]
...
mov  rdi, [rbp+0x6f]            ; rdi = a freshly-allocated object (from a call just above)
mov  qword ptr [r14+8], rdi     ; array[index]+8 = rdi   <-- THE WRITE
```

This confirms `ExInitializePoolHeapManagement` really does populate this
array (same base, same `0x20C0` stride, real symbol-verified) -- but it
writes to **`array[index]+8`**, not the `+0x10` field the live survey
found NULL. Cross-referencing the earlier `ExAllocateHeapPool` disassembly
(the "2026-07-17 update: the Segment Heap bug, traced five call-chain
levels" section above): which field gets *read* (`[rcx + rax*8]`, i.e.
slot `0`/`1`/`2`/...) is selected by allocation-flag bits (`r12d`), and
the specific failing allocation's flags select slot `2` (`+0x10`) -- a
**different slot than the one this initializer populates**.

**Refined conclusion.** `ExInitializePoolHeapManagement` eagerly creates
one heap-variant slot per pool-type index at boot (`+8`). It is *not* the
function responsible for the specific slot (`+0x10`) our failing
allocation needs -- that slot is most plausibly populated by a separate,
lazy, on-first-use initializer for a different flag-selected heap variant,
not yet located. This sharpens, rather than answers, the user's
never-vs-cleared question: the evidence so far (no teardown/destroy
function found anywhere in the symbol table) leans toward "never
(re-)triggered" over "explicitly cleared," but the specific mechanism for
slot `+0x10` remains unidentified.

**A real methodological complication, itself informative.** Attempting to
watch for a second hit of `ExInitializePoolHeapManagement` across the
reset boundary (to directly settle whether *this* function re-runs on
pass 2) ran into a genuine hazard: keeping `#BP` interception
continuously active exposes the capture to `DbgBreakPointWithStatus`
(RVA `0x406E40`, confirmed via the same PDB) -- a real, already-documented,
benign NT function (`KeBugCheckEx`'s own first step / early boot-debugger
check) -- firing at extremely high frequency early in pass 2 (millions of
hits within 145 real seconds, `port 0xA1 val=0xFF` visible alongside it,
consistent with a legacy-PIC-mask sequence). Because this hypervisor
exposes a genuine KD-compatible named pipe (`\\.\pipe\LocalHostKD`,
printed on every boot) with no debugger ever actually attached in these
automated runs, this is very plausibly Windows waiting on a configured
boot debugger connection -- expected behavior, not a new bug -- but
`#BP`-type exception interception is address-agnostic (WHV's exception
bitmap intercepts by vector, not by address), so our own diagnostic
infrastructure cannot avoid being pulled into it once armed, regardless of
which specific address it's actually watching. This made extended live
observation across the whole reset-to-bugcheck window impractical within
this session's time budget; runs either got stuck in this unrelated loop
or completed too fast to reach it, without a clean answer either way. A
temporary logging throttle for this specific print (previously
unconditional, now matching the throttle pattern used elsewhere in this
file) was added regardless, since it was a real, unconditional inefficiency
independent of this investigation.

**Recommendation for the most hardware-accurate approach, pending the
still-unidentified slot-`+0x10` initializer:** do not implement a targeted
"force-populate this slot" workaround even once found -- that would only
be accurate for this one flag/slot combination and would leave the same
category of gap for any other lazily-initialized state this hypervisor's
CPU-register-only reset doesn't know to rebuild. The evidence so far
(components inspected at every hop -- ACPI tables, IOAPIC, RTC, and now
pool-heap state -- consistently point to "software-visible state that a
genuine `RESET#` doesn't expect to already exist" as the recurring shape
of every bug this project has hit post-reset) suggests the real,
general-purpose fix belongs at the reset mechanism itself: either (a) a
more complete state reset that also invalidates/reinitializes
memory-resident OS structures a real cold boot would never see stale
(likely intractable in general, since the hypervisor cannot know which of
the guest's own structures are safe to leave alone), or (b) present the
guest with something closer to what real firmware guarantees after
`RESET#` -- which real x86 hardware achieves by simply not preserving any
of this state in the first place (RAM contents survive, but the OS's own
initialization always runs fully from scratch on every boot, cold or
warm, because Windows doesn't distinguish "warm reset" from "cold boot"
architecturally the way this hypervisor's CPU-only reset implicitly does).
Concretely: the recurring pattern across this entire investigation is that
pass 2 is architecturally supposed to be indistinguishable from a fresh
boot from the OS's own perspective (confirmed again this update: KASLR
gives pass 2 a genuinely new `ntoskrnl.exe` base, proving the loader
really does redo image loading from scratch), yet specific lazily-created,
memory-resident structures from pass 1 persist and get *reused* rather
than *rebuilt* -- not because Windows chooses to reuse them, but because
whatever normally triggers their (re)creation apparently doesn't fire a
second time. This is not yet proven to be fixable from the hypervisor
side at all (it may be intrinsic to how this guest's boot path interacts
with a RAM-preserving reset), and is exactly the kind of hardware-behavior
question this project has consistently deferred to the user rather than
deciding unilaterally -- flagged here for that decision, not implemented.

### Evidence artifacts (this update)

- `enum_pool_syms.py` (scratchpad) — the SymEnumSymbols-based candidate
  search; found `ExInitializePoolHeapManagement` and confirmed no
  teardown/destroy pool function exists in the public symbol table.
- `bp_capture_run10.log` — first hit of the retargeted breakpoint
  (RVA `0x3C3B64`), confirming it fires during pass 1 (one-shot capture).
- `bp_capture_run11.log`/`bp_capture_run12.log` — continuous re-arm
  (`mov rax,rsp` emulation) watching across the reset boundary; run12
  specifically surfaced the `DbgBreakPointWithStatus` interaction hazard.
- `bp_capture_run13.log` + `disasm_pool_init.py` (scratchpad) — the
  throttled-logging run and the disassembly that confirmed the `[r14+8]`
  write into the `ExPoolState` array, and the field-offset mismatch
  against the `+0x10` slot found NULL.

## 2026-07-17 update: the +0x10 slot IS used on pass 1 -- and ExAllocateHeapPool itself never creates it

Per the user's direction to keep pushing on locating the actual `+0x10`
initializer, and specifically to determine why it runs on pass 1 but not
pass 2.

**Step 1 -- exhaustive whole-kernel static scan, offline.** Rather than
keep guessing candidate function names, dumped the *entire* `ntoskrnl.exe`
image (16MB, comfortably covers the real size) to a binary file at the
next available breakpoint hit, then disassembled all of it with capstone
and searched for every RIP-relative reference landing on `ExPoolState`
itself or the array's base address. **Result: only two direct references
in the entire kernel** -- one inside `ExAllocateHeapPool` (already known,
the read site) and one inside `ExHeapQueryPoolUsage` (a diagnostic/query
function; its own nearby `+0x10` writes are into a local output-buffer
struct, unrelated to our array -- a false positive, ruled out by function
purpose). `ExInitializePoolHeapManagement`'s own known reference didn't
show up in this scan due to a self-inflicted artifact (this investigation's
own lingering `0xCC` breakpoint patch, still present at that function's
first byte in the dumped snapshot, desynchronizes capstone's linear
disassembly for that one function specifically -- confirmed by manually
checking the dump bytes at that address, which are otherwise intact).

**Step 2 -- checked whether `+0x10` is EVER populated during pass 1's own
successful operation.** This was the actual missing data point: every
prior survey of this array was taken only at the moment of the *failing*
call (pass 2). Retargeted the breakpoint back to `RtlpHpLfhSlotAllocate`'s
entry (re-using the working re-arm emulation from earlier this session)
and surveyed `ExPoolState[0..15]+0x10` every 2000th call throughout pass
1's own extensive heap activity (82,000+ calls observed). **Result: `1/16`
entries non-NULL, consistently, from the very first survey (very early in
pass 1) through the last one taken before `BdsDxe`'s second appearance.**
This directly disproves the "unexercised slot, not a regression"
hypothesis raised in the previous update -- the slot genuinely is
populated and stays populated throughout all of pass 1. The original
"something pass 2 doesn't do that pass 1 does" framing is back, sharper:
now a concrete, testable question about *one specific* array index's
`+0x10` field, not a vague systemic one.

**Step 3 -- fully disassembled `ExAllocateHeapPool` (the entire 0xC1E-byte
function, not just the tail near the call site) from the authoritative
16MB dump.** This resolved several of this session's own earlier
bookkeeping/arithmetic mixups (all traced back to a hand-arithmetic slip
in scratch commentary, never in the actual capture code or its results --
consistent with this project's established pattern of catching and
correcting its own mistaken inferences via live/authoritative evidence
rather than continuing to compound them) and gave a clean, complete
picture:

- The function reads `array[index]+0x10` into R13 near its start (already
  known), then branches on the *rounded allocation size* to one of several
  internal fast-path handlers. Our failing allocation's size (rounding to
  832 bytes) takes the branch for sizes in `[0x201, 0xf80)`.
- That branch immediately does `mov rcx, [r13+0x28]` -- dereferencing the
  NULL R13. **This does not crash**: per an established finding earlier in
  this investigation (very low guest-virtual addresses read back as zero
  rather than faulting in this environment), `[NULL+0x28]` reads `0`, and
  the code's own `test rcx,rcx; je <fallback>` correctly (from its own
  logic's perspective) treats this as "no lookaside cache list for this
  heap variant yet" and gracefully falls back to a slower, general
  allocation path -- which is what eventually, several calls later, reaches
  `RtlpHpLfhSlotAllocate` with the still-NULL heap parameter.
- The one call further down this same fallback branch
  (`call 0x406EC0`) resolves via the real PDB to
  `ExpInterlockedPopEntrySList` -- an SLIST (lookaside-list) pop, i.e. a
  cache-hit check, not a creation routine.
- **Searched the entire function for any write to `[reg+0x10]`: found
  exactly one, and it's unrelated** -- classic NT-Heap boundary-tag
  manipulation on a completely different structure (heap block header
  fields), a coincidental offset match, not our array.

**Conclusion: `ExAllocateHeapPool` itself never creates the missing heap
variant.** It correctly detects the NULL case and falls back, but the
fallback path doesn't create what's missing either -- it just proceeds
with a bad value, eventually reaching `RtlpHpLfhSlotAllocate` unchecked.
Combined with the whole-kernel static scan finding no other function with
its own RIP-relative reference to the array, **the actual creator must
receive the array-entry's address as a parameter from some other call
chain** -- not something visible from within `ExAllocateHeapPool`'s own
code or discoverable by scanning for direct references to the array's
fixed address. This is a different, open-ended kind of search (tracing
*upward* from whatever calls the real creator, rather than following a
single known chain) than what this investigation has done so far.

**A secondary, now twice-confirmed structural fact worth flagging on its
own:** very low guest-virtual addresses (near `0x0`) read back as `0`
rather than faulting in this environment, at least under this specific
guest CR3/paging context -- first observed with `[0+0x379]` in the
`RtlpHpLfhSlotAllocate` chain, now again with `[0+0x28]` here. Real
Windows relies on the low null-guard page being genuinely unmapped
specifically so accidental NULL dereferences fault immediately and
loudly; if this hypervisor's memory/paging model doesn't reproduce that,
it could be independently significant for other guest behavior beyond
this specific bug, though establishing that is out of scope for the
current investigation.

### Evidence artifacts (this update)

- `ntoskrnl_dump.bin` (scratchpad, 16MB, one-shot capture at hit #1) — the
  full kernel image; the authoritative source for every disassembly in
  this update.
- `find_slot10_writer.py`, `verify_dump.py` (scratchpad) — the whole-image
  RIP-relative reference scan and the dump-integrity check that explained
  the `ExInitializePoolHeapManagement` scan gap.
- `bp_capture_run15.log` (scratchpad) — the pass-1 array survey showing
  `1/16` non-NULL consistently from early pass 1 through 82,000+ calls.
- `disasm_full_exallocatepool.py` (scratchpad) — the complete
  `ExAllocateHeapPool` disassembly (732 instructions) from the
  authoritative dump; the basis for the size-branch trace, the
  `[r13+0x28]` graceful-fallback finding, and the exhaustive `+0x10`
  write search.

## 2026-07-17 update: hardware data watchpoint -- decisive, direct proof

Per the user's explicit direction: since static analysis (whole-kernel
scan) had reached diminishing returns, switched to a fundamentally
different, more direct technique -- a real hardware data watchpoint
(`DR0`/`DR7`), which traps on the actual memory *write* regardless of
which function performs it or how deep the call chain is. This is a
different mechanism from every breakpoint used earlier in this
investigation (those were all `#BP`/`INT3`-based, instruction-address
triggers); a data watchpoint triggers on the *access*, not on reaching a
specific instruction.

**Setup.** First identified exactly *which* of the 16 array indices
matters: extended the existing periodic pass-1 survey to report the index,
not just the count -- **always index 0**, consistent with this being a
single-vCPU/single-processor-index system (not a per-flag or per-random
slot). Target address: `ExPoolState+0x3900+0x10` (index 0, field `+0x10`).
Armed `DR0` = target address, `DR7` = local-enable, write-only, 8-byte
length, requesting `#DB` (vector 1) exception exits. Confirmed the
existing reset handler already zeroes `DR7` as part of its real-`RESET#`-semantics
CPU state reset (implemented earlier in this investigation, before this
specific watchpoint existed) -- so the watchpoint needed re-arming at
every module rediscovery, hooked into the same opportunistic
discovery mechanism already used throughout this investigation for
finding ntoskrnl.exe's KASLR base each pass.

**First attempt hit a real, informative snag.** Running the watchpoint
alongside the still-armed `#BP` infrastructure from the previous update
re-exposed the `DbgBreakPointWithStatus` flooding hazard (a real,
unrelated, extremely-high-frequency genuine `INT3` early in pass 2) --
confirming that hazard is tied to *any* active `#BP` interception, not
specific to which address is being watched. Fixed by removing `#BP`
interception entirely for this experiment (the discovery mechanism's
`INT3`-patching side was also skipped, since an unintercepted `0xCC` would
send the guest into its own genuine bugcheck) -- the watchpoint needed
none of that infrastructure, being a completely independent mechanism.

**Result, decisive.** With `#BP` interception removed, the watchpoint:

```
[wp] armed data watchpoint at 0x...C57ED0 (module base 0x...)          <- pass 1
[wp] ==== DATA WATCHPOINT HIT #1: write to ExPoolState[0]+0x10 ====     <- confirms mechanism works
BdsDxe: starting Boot0002 ...                                           <- pass 2 begins (reset happened)
[wp] armed data watchpoint at 0x...857ED0 (module base 0x...)          <- re-armed fresh, new KASLR base confirmed
[kerneldiag] KiBugCheckData: code=0x139 ...                             <- the familiar crash
```

**No second hit occurs anywhere between the pass-2 re-arm and the
bugcheck.** The watchpoint stayed armed across the guest's *entire* second
pass -- from the moment of reset through to the crash -- and never fired.
This directly, empirically proves (not infers from the absence of a
teardown function, as the previous update's evidence was) that **nothing
ever writes `ExPoolState[0]+0x10` during pass 2**, full stop. The
"initialized then cleared" hypothesis is now conclusively ruled out --
there is no clearing to observe because there is no writing to clear.
This is a genuine reset-time regression: whatever code path performs this
write during pass 1 (still not identified by name/RVA -- the watchpoint
answers *whether*, not yet *who*, since it never got the chance to fire a
second time) simply never executes during pass 2.

**One honest caveat.** A hardware watchpoint set externally by the
hypervisor is, in principle, visible to and potentially overwritable by
the guest's own debug-register management (real Windows kernels save/
restore `DR6`/`DR7` as part of thread-context switching in some
configurations). It's possible, not yet ruled out, that guest activity
silently cleared `DR7` at some point in pass 2 without going through this
hypervisor's reset handler, which would produce the same observed
"never fires" result via a false negative rather than genuine absence.
Weighed against this: pass 1's watchpoint survived from arm-time through
its own successful hit without issue, and this is very early boot
(pre-full-scheduler, single-vCPU, the same phase the whole rest of this
investigation's evidence has been gathered from) where full thread-context
debug-register save/restore is less likely to be active yet -- but this
hasn't been independently confirmed, and is flagged here rather than
overclaimed.

### Evidence artifacts (this update)

- `wp_capture_run1.log` — first watchpoint run: confirms the mechanism
  (hit #1 in pass 1, re-armed post-reset) but re-exposed the
  `DbgBreakPointWithStatus` flooding hazard, running out of observation
  window before reaching pass 2's bugcheck.
- `wp_capture_run2.log` — the decisive run, `#BP` interception removed:
  hit #1 in pass 1, clean re-arm for pass 2, reaches `KiBugCheckData:
  code=0x139` with zero further watchpoint hits -- the core evidence for
  this update's conclusion.

## 2026-07-17 update: the writer identified -- ExInitializePagedHeaps

Resolved `wp_capture_run2.log`'s hit #1 writer RIP (`module+0x3C3DF4`)
against the real PDB: **`ExInitializePagedHeaps+0x84`**. This is a
distinct function from `ExInitializePoolHeapManagement` (found earlier,
RVA `0x3C3B64` -- only `0x20C` bytes apart, adjacent in the binary), and
the naming is immediately, cleanly sensible: `ExInitializePoolHeapManagement`
populates the **NonPaged** pool heap variant (`array[0]+8`, confirmed
earlier via disassembly); `ExInitializePagedHeaps` populates the
**Paged** variant (`array[0]+0x10`, confirmed now via the live watchpoint
hit) -- exactly matching Windows' standard NonPaged/Paged pool split, and
exactly the field this entire investigation has been chasing.

**Combined with the earlier decisive proof** (the same watchpoint, armed
across all of pass 2, never fired again before the crash): **`ExInitializePagedHeaps`
runs during pass 1 and does not run during pass 2.** The Segment Heap bug
is a boot-time initializer that only executes once per process/system
lifetime rather than once per (this hypervisor's) reset -- i.e. exactly
the shape of bug this investigation's very first reframing predicted:
software-visible state a genuine `RESET#` wouldn't expect to already
exist, except here traced all the way to the specific, named routine.

**Root cause chain, now complete end-to-end with real names at every hop:**

```
ExInitializePagedHeaps                       [runs on pass 1, never on pass 2]
  -> writes ExPoolState[0]+0x10                [the Paged pool heap-manager pointer]

ExAllocateHeapPool+0x2B1                      [reads that pointer into R13; NULL on pass 2]
  -> RtlpHpLfhSlotAllocate(size, heap=NULL)
    -> RtlpHpLfhBucketGetSubsegment(container=NULL)
      -> RtlpHpLfhOwnerMoveSubsegment(subsegment=NULL, owner=NULL)
        -> safe-unlink check correctly detects the invariant violation
        -> fast-fail -> bugcheck 0x139
```

**Not yet fixed, and the remaining open question is squarely a design
decision, not further investigation:** *why* `ExInitializePagedHeaps`
doesn't run a second time is very likely simply that it's gated the way
any real one-time kernel initialization routine would be -- called once
from early Phase 0/1 executive init, with no expectation of ever running
twice in a single system's lifetime, because on real hardware a `RESET#`
never leaves old software state behind for it to conflict with. This
hypervisor's CPU-register-only reset (RAM deliberately preserved, an
intentional, already-load-bearing design choice for this project's other
reset-adjacent fixes) is what creates the mismatch. The earlier
recommendation stands, now with a concrete, complete example backing it:
a targeted "call `ExInitializePagedHeaps` again after reset" workaround
would fix this one symptom but leaves the general category of bug (a
whole class of one-time Phase 0/1 initializers this hypervisor's reset
doesn't know need re-running) unaddressed -- the real fix belongs at the
reset mechanism's own design, which is a hardware-behavior decision for
the user to make, not one to implement unilaterally.

### Evidence artifacts (this update)

- `resolve_syms.py` (scratchpad, updated) — resolves `wp_capture_run2.log`'s
  watchpoint hit #1 RIP to `ExInitializePagedHeaps+0x84`.

## 2026-07-17 update: systemic, not Paged-specific -- ExInitializePoolHeapManagement fails the same way

Per the user's explicit request, before any reset-mechanism design
decision: retargeted the identical watchpoint technique to
`ExPoolState[0]+0x08` (the NonPaged slot, `ExInitializePoolHeapManagement`)
to test whether the gap is narrow (Paged-specific) or systemic (any
one-time Phase 0/1 initializer).

**Result: byte-for-byte the same pattern as the Paged slot.**

```
[wp] armed data watchpoint at 0x...057EC8 (ExPoolState[0]+0x08)         <- pass 1
[wp] ==== DATA WATCHPOINT HIT #1: write to ExPoolState[0]+0x08 ====      <- fires once, confirms mechanism
BdsDxe: starting Boot0002 ...                                            <- pass 2 begins (reset)
[wp] armed data watchpoint at 0x...257EC8 (new module base)             <- re-armed cleanly
[kerneldiag] KiBugCheckData: code=0x139 ...                              <- reaches the crash
```

No second hit anywhere between the pass-2 re-arm and the bugcheck --
**`ExInitializePoolHeapManagement` also never runs on pass 2.** This
directly answers the user's question: the gap is **systemic**, not a
divergent Paged-specific code path. Both the NonPaged and Paged one-time
pool-heap initializers fail to re-run after this hypervisor's reset,
despite the guest genuinely reloading a fresh `ntoskrnl.exe` image each
pass (confirmed repeatedly this session via KASLR base changes) and OVMF
genuinely re-running full POST. Whatever real hardware provides that lets
Windows correctly redo (or correctly decide to redo) this whole class of
Phase 0/1 initialization under an equivalent RAM-preserving warm reset is
missing here broadly, not in one isolated function.

**Implication for the design question:** this supports treating the fix
as a general reset-fidelity gap rather than special-casing any single
initializer -- exactly the distinction the user's experiment was designed
to make, and now made with direct evidence rather than inference from one
data point. The next open question, still unresolved, is *which* piece of
hardware/firmware behavior Windows' Phase 0/1 init actually keys off to
decide whether this whole initialization class needs to run -- that's
where investigation would need to go next if pursued further, rather than
patching either initializer individually.

### Evidence artifacts (this update)

- `wp_capture_run3.log` — the NonPaged-slot watchpoint run: hit #1 in
  pass 1, clean re-arm for pass 2, reaches `KiBugCheckData: code=0x139`
  with zero further hits -- the direct evidence that the gap is systemic.

## 2026-07-17 update: the earliest divergence point -- the Phase 1 system thread itself never runs

Per the user's explicit direction to stop tracing individual initializers
and instead work upward through the boot path for the earliest decision
point, understanding-only (no fix): with both `ExInitializePagedHeaps` and
`ExInitializePoolHeapManagement` confirmed to fail identically, the
natural next question is whether they share a common ancestor whose own
failure to run would explain both at once, rather than treating them as
two coincidentally-parallel gaps.

**Found the candidate immediately in data already captured.** The two
watchpoint hits' own stack scans (already gathered, not re-run) contained
canonical-kernel-space candidates resolving to `MmInitSystem+0x8F` and,
strikingly, **`InitBootProcessor+0x57A`** -- the master Phase 0/1
dispatcher for the boot processor, a real, well-known NT internals
function. The NonPaged writer's stack independently pointed at
`MiInitNucleus+0x44B`, a separate but similarly early Mm-phase function.
Both call chains point at early Memory Manager phase-init code as the
immediate context, with `InitBootProcessor` as the probable root.

**Checked whether the classic public-Windows-internals two-phase `MmInitSystem`
pattern applies here.** Real NT boot is well known to call
`MmInitSystem(Phase=0, ...)` very early (linear boot path) and
`MmInitSystem(Phase=1, ...)` later, from a genuine kernel system thread
(`Phase1Initialization`) created once the scheduler is up -- not from the
same linear call sequence. Confirmed via `SymFromName` against the real
PDB that this exact build has all the expected symbols:
`MmInitSystem` (RVA `0xA56F60`), `InitBootProcessor` (RVA `0xA3D054`),
`Phase1Initialization` (RVA `0x7B4350`), and critically
**`Phase1InitializationDiscard`** (RVA `0xA3DC14`) -- the real system-thread
entry point that runs Phase 1 init and discards its own init-segment
memory when done. If *this* thread never runs on pass 2, both pool-heap
findings (and potentially anything else gated behind Phase 1 init) would
be simple downstream consequences of one higher-level fact, not
independent gaps.

**First attempt (INT3-based breakpoint) was inconclusive, not negative.**
Retargeting the existing `#BP`/INT3 mechanism to `Phase1InitializationDiscard`'s
entry re-exposed the `DbgBreakPointWithStatus` flooding hazard (any `#BP`
interception, once active, catches every genuine software `INT3`
elsewhere in the kernel, not just the patched one). 86 real seconds and
2.7M+ VM exits produced zero second hit, but comparing against an
unthrottled watchpoint run's timing (bugcheck reached by ~25s cumulative
in that run) showed this wasn't remotely far enough into pass 2 for the
result to mean anything -- the flooding overhead was distorting the
correspondence between wall-clock time and actual guest progress.

**Fixed by switching to a hardware EXECUTION breakpoint** (`DR0`-`DR3`/`DR7`,
`R/W`=execute) instead of an INT3 memory patch -- the same debug-register
mechanism already proven clean for the two data watchpoints, traps via
`#DB` (vector 1) rather than `#BP` (vector 3), and by construction never
intercepts genuine software `INT3`s anywhere in the kernel (architecturally
always vector 3). This sidesteps the flooding hazard entirely rather than
fighting it. One new wrinkle handled: unlike the data watchpoint's trap
semantics (RIP past the access), an instruction execution breakpoint is
fault-style (RIP *at* the not-yet-executed instruction), so the breakpoint
must be explicitly disabled before resuming or the same instruction
re-traps forever.

**Result: clean and decisive.**

```
[wp] armed execution breakpoint ... RVA 0xA3DC14                         <- pass 1
[wp] ==== EXECUTION BREAKPOINT HIT #1: Phase1InitializationDiscard ENTRY ==== exitCount=111831
BdsDxe: starting Boot0002 ...                                             <- pass 2 begins (reset)
[wp] armed execution breakpoint ... (new module base)                    <- re-armed cleanly
[kerneldiag] KiBugCheckData: code=0x139 ...                               <- reaches the crash
```

No second hit anywhere between the pass-2 re-arm and the bugcheck, and
this time the run completed at normal speed (comparable log length/line
count to the earlier clean watchpoint runs), confirming the guest wasn't
throttled -- this is a genuine negative result, not an inconclusive one
like the INT3 attempt.

**This is the earliest divergence point found so far: the Phase 1 kernel
initialization system thread itself never runs during pass 2.**
`ExInitializePagedHeaps` and `ExInitializePoolHeapManagement` failing
identically is fully explained as a consequence of this single fact --
they were never going to run, because whatever calls them
(`ExInitSystem`/`PspInitPhase1`-adjacent code, per the standard NT
init sequence, itself called from `Phase1Initialization`) never got the
chance to. The remaining open question, one level higher still: does
`InitBootProcessor`/`KiInitializeKernel` even attempt to *create* this
system thread on pass 2, or does thread creation happen but the thread
never gets *scheduled* to run (e.g. because the scheduler itself isn't
fully up, or is waiting on something that itself depends on the same
reset-fidelity gap)? That's the natural next step if this is pursued
further -- watching `InitBootProcessor` itself (or the specific
`PsCreateSystemThread` call site that creates the Phase 1 thread) the same
way, to see whether the divergence is at thread *creation* or thread
*scheduling*.

### Evidence artifacts (this update)

- `resolve_by_name.py` (scratchpad) — `SymFromName`-based exact-address
  resolution for `MmInitSystem`, `InitBootProcessor`, `Phase1Initialization`,
  `Phase1InitializationDiscard`, and related Phase 0/1 symbols, confirming
  they all exist in this exact build.
- `phase1_capture_run1.log` — the INT3-based attempt: hit #1 in pass 1,
  then stuck behind the `DbgBreakPointWithStatus` hazard for the rest of
  the 90s window -- inconclusive, informative about the hazard's scope
  (any `#BP` interception, not address-specific).
- `phase1_capture_run2.log` — the decisive, unthrottled hardware
  execution-breakpoint run: hit #1 in pass 1, clean re-arm for pass 2,
  reaches `KiBugCheckData: code=0x139` with zero further hits, at normal
  (untangled) speed -- the core evidence for this update's conclusion.

### Evidence artifacts (this update)

- `bp_capture_run1.log` — first re-verification of the true-entry
  breakpoint: `subsegment(rcx)=0 owner(rdx)=0`, both genuinely NULL (not
  just subsegment as previously assumed).
- `bp_capture_run2.log` — added caller-code-window capture; identified the
  raw byte sequence at the immediate caller's call site.
- `bp_capture_run3.log`/`bp_capture_run4.log` — grandparent code-window
  capture (retargeted breakpoint at RVA `0x2382C1`) and the RBP/R13-relative
  live memory dump confirming `rbp+0x58=0` directly.
- `resolve_syms.py` (scratchpad) — the dbghelp/ctypes symbol-resolution
  tool built this session; resolves every RVA identified live against the
  downloaded `ntkrnlmp.pdb`. Reusable for future sessions.
- `ntkrnlmp.pdb` (scratchpad, ~8.5MB, re-downloadable via `fetch_pdb.py`)
  — the real, matching PDB fetched from Microsoft's public symbol server.
- `bp_capture_run5.log` + `disasm_slotalloc.py`/`check_writes.py`
  (scratchpad) — the full `RtlpHpLfhSlotAllocate` function dump and its
  proper capstone disassembly; the basis for correcting the `R13=0x340`
  "corrupted pointer" misreading.
- `bp_capture_run6.log` — first hit of the retargeted entry breakpoint
  (RVA `0x237620`), attempt #82,153; identified the caller return address
  later resolved to `ExAllocateHeapPool+0x2B1`.
- `bp_capture_run7.log` + `disasm_exallocate.py` (scratchpad) — the
  `ExAllocateHeapPool` call-site lead-up, showing `rdx=r10`/`rcx=rdi` and
  `rdi=[r13+0x340]`.
- `bp_capture_run8.log` + `disasm_full_exallocate.py` +
  `find_global_array.py` (scratchpad) — the full `ExAllocateHeapPool` body;
  found the single write to R13 and computed the RIP-relative global array
  address, resolved to `ExPoolState+0x3900`.

## 2026-07-18 update: the three-way breakpoint experiment -- thread creation is never attempted

Continuing one level higher than the previous update, with an explicit
stopping condition set by the user: determine which of three cases explains
`Phase1InitializationDiscard` never running on pass 2 -- (1) the Phase 1
system thread is never created, (2) created but never scheduled, or (3)
scheduled and starts running but is diverted before reaching
`Phase1InitializationDiscard` -- identify the earliest missing event, then
stop for architecture reassessment before any fix is proposed or made.

### Static analysis: locating the exact call site

Starting from `PsInitSystem` (RVA `0xA4F444`, confirmed via `SymFromName` --
a small phase dispatcher on ECX = phase 0/1/2/3) and `PspInitPhase0` (RVA
`0xA401D8`, size `0x8C8`, called for phase 0), a full disassembly of
`PspInitPhase0`'s body (`disasm_psinitsystem.py`, extended this session)
located the exact thread-creation sequence:

```
+0x081D: mov rcx, [rip+...]        ; PsInitialSystemProcess
+0x0824: mov [rcx+0x5C0], rax      ; store an earlier ExAllocatePoolWithTag('SePa', 0x200) result
+0x082B: mov rax, [rip+0x2BBA16]   ; -> resolves to PsInitialSystemProcess
+0x0832: mov rcx, [rax+0x5C0]      ; reload it
+0x0839: test rcx, rcx
+0x083C: je  0xA40A9C              ; bail out (return FALSE) if that allocation failed
+0x084A: lea rax, [rip-0x28C6D9]   ; -> resolves to Phase1Initialization (RVA 0x7B4350)
+0x0868: mov edx, 0x1FFFFF         ; THREAD_ALL_ACCESS
+0x086D: call 0x6A92F0             ; PsCreateSystemThread(&Handle, ACCESS, NULL, NULL, NULL, Phase1Initialization, NULL)
+0x0872: test eax, eax             ; NTSTATUS in EAX
+0x0874: js  0xA40A9C              ; bail out (return FALSE) if creation failed
+0x0876: mov rcx, [rbp-0x18]       ; ThreadHandle
+0x087C: call 0x663B20             ; ZwClose(ThreadHandle) -- the thread runs detached
```

(`resolve_bailout.py`, scratchpad.) The `test rcx,rcx; je` guard at +0x0839
is not Phase-1-specific -- it only checks that the *preceding*, unrelated
0x200-byte early-boot allocation into `PsInitialSystemProcess+0x5C0`
succeeded, a mundane allocation with no obvious reason to differ pass 1 vs
pass 2. Both bailout paths (`je`/`js`) land at the same shared
`PspInitPhase0` failure epilogue (`RVA 0xA40A9C`: `xor al,al; jmp <exit>`,
i.e. `return FALSE`), which is not consistent with what pass 2 actually
does (it runs for ~100,000 more VM exits and reaches a *different*,
much later failure -- the Segment Heap bugcheck -- not an immediate Phase 0
failure), suggesting but not proving that `PspInitPhase0` still returns
success on pass 2.

### Live three-way capture

This called for going live rather than reasoning further from statics.
`Hypervisor.c` was extended to arm three simultaneous hardware execution
breakpoints at once (DR0/DR1/DR2, all R/W=execute, LEN=00 byte, DR7 =
L0|L1|L2 + reserved bit10), each independently one-shot (only the L-bit of
whichever one fires gets cleared, leaving the others armed so a single pass
can capture all three events):

- **DR0** = `PspInitPhase0+0x872` (the `test eax,eax` right after the
  `PsCreateSystemThread` call -- RIP is at this not-yet-executed instruction
  under fault-style breakpoint semantics, so EAX still holds the call's raw
  NTSTATUS return value). Answers: was the thread actually created, and did
  creation succeed or fail?
- **DR1** = `Phase1Initialization`'s own entry (RVA `0x7B4350`). Answers:
  does the new thread ever actually get scheduled and start running?
- **DR2** = `Phase1InitializationDiscard`'s entry (RVA `0xA3DC14`, the same
  site as the previous update). Kept for consistency.

Result (`phase1_3way_run1.log`, scratchpad):

```
Pass 1 (module base 0xFFFFF8035E400000):
[wp] armed 3 execution breakpoints ...
[wp] EXECUTION BREAKPOINT HIT #1: PsCreateSystemThread RETURN ... exitCount=103036
[wp] PsCreateSystemThread returned NTSTATUS = 0x00000000 (SUCCESS)
[wp] EXECUTION BREAKPOINT HIT #2: Phase1Initialization ENTRY ... exitCount=103037
[wp] EXECUTION BREAKPOINT HIT #3: Phase1InitializationDiscard ENTRY ... exitCount=103039
...
[reset] port 0x64/0xFE keyboard-controller reset pulse honored (exitCount=570444)
[wp] armed 3 execution breakpoints at module base 0xFFFFF80359800000 ...   <- pass 2, fresh KASLR base
                                                                             <- ~100,000 more VM exits follow
[kerneldiag] KiBugCheckData: code=0x139 params=[0x3, ...]                  <- reaches the crash
```

All three breakpoints fired correctly, in order, within 3 VM exits of each
other on pass 1, with `PsCreateSystemThread` returning `STATUS_SUCCESS`
(`0x0`). On pass 2, armed fresh immediately after the reset and left armed
across the entire second pass (reset at exitCount 570444 through the
bugcheck at exitCount 670823 -- ~100,000 VM exits, the same order of
magnitude as the ~103,000 exits pass 1 needed to reach and clear all three
breakpoints) **none of the three ever fired.** This is not a coverage or
timing question -- pass 2 ran comparably long and reached its own eventual
failure point without ever coming close to any of the three watched
addresses.

### Conclusion: case 1, and earlier than expected

Since the *call-return* breakpoint (DR0) never fires, `PspInitPhase0` never
even reaches the instruction immediately after its call to
`PsCreateSystemThread` on pass 2 -- meaning the call is never made (or, far
less likely given no intervening exception was observed, made but somehow
never returns to be observed, which would itself be a separate, worse
problem). This rules out case 3 outright (a thread that starts running and
gets diverted would have to have been created and scheduled first, and
DR1/`Phase1Initialization`'s entry never fires either). It also rules out
plain case 2 ("created but not scheduled") in favor of the stronger case 1:
**the Phase 1 system thread is never created**, and the earliest missing
event is further upstream than the call site itself -- somewhere in
`PspInitPhase0` (or its own caller chain) diverts control flow before
`PspInitPhase0+0x86D` is ever reached on pass 2.

Per the explicit stopping condition for this phase, no further
investigation into *why* that earlier divergence happens, and no fix, was
attempted here -- this is handed back for architecture reassessment.

### Evidence artifacts (this update)

- `resolve_bailout.py` (scratchpad) — resolved the bailout target
  (`PspInitPhase0+0x8C4`, a shared `return FALSE` epilogue) and the global
  at the `+0x82B` load (`PsInitialSystemProcess`), confirming the null-check
  at `+0x0839` guards an unrelated preceding allocation, not
  `PsCreateSystemThread` itself.
- `disasm_psinitsystem.py` (scratchpad, extended) — the full
  `PspInitPhase0` disassembly that located the exact
  `PsCreateSystemThread(..., Phase1Initialization, ...)` call site and its
  surrounding control flow.
- `phase1_3way_run1.log`/`phase1_3way_run1_err.log` (scratchpad) — the
  decisive live capture: all three breakpoints hit cleanly on pass 1 in the
  expected order with `PsCreateSystemThread` returning success; zero hits
  on pass 2 despite ~100,000 VM exits of runway before the bugcheck.

## 2026-07-18 update: resolving the "substantial work" tension -- pass 2's runway is mostly bootloader, not kernel

The previous update's `PspInitPhase0` finding raised an apparent
contradiction: if the Phase 1 system thread genuinely never runs, most
downstream NT subsystems shouldn't be available either, yet pass 2 seemed
to keep running for a long time (~100,000 VM exits) after the reset before
crashing. Rather than build further conclusions on that assumption, this
update tested it directly by examining what the pass-2 window's VM exits
actually *are*, instead of just counting them.

### The pass-2 window is bootloader activity, not kernel activity

`categorize_windows.py` (scratchpad) tagged every log line in
`phase1_3way_run1.log` between the reset (`exitCount=570444`) and the
bugcheck (`exitCount=670823`, ~100,379 exits) and compared it against the
equivalent-sized window from pass 1 (process start through the first
`PspInitPhase0` breakpoint hit at `exitCount=103036`). Pass 1's window is
dominated by `pcicfg`/`ahci`/`ioapic`/`fwcfg` firmware device-enumeration
traffic (300/202/100/83 lines respectively) -- classic cold-boot POST.
Pass 2's window shows **zero** `pcicfg`/`ahci`/`ioapic` lines at all.

Reading the actual pass-2 content directly (lines 1055-1101 of the log,
i.e. right after the reset) shows why: a short burst of `fwcfg`
selector reads, a `ramfb`/`cmos-ab` reconfiguration, `BdsDxe` loading and
starting `Boot0002` (the disk boot option) at ~24.4s wall-clock, then a
long stretch -- roughly 57,000 VM exits, ~24.4s to ~26.6s -- of heartbeat
ticks all reporting the same I/O port (`0xB008`) with an unchanging value,
consistent with a bootloader-level poll/wait loop (plausibly a disk-read
wait), not varied kernel activity. This reads as a **much leaner warm
reboot** than pass 1's original boot -- consistent with real firmware
skipping full PCI/AHCI/IOAPIC re-enumeration on a warm reset the way real
hardware does, rather than doing a second full cold-boot POST.

### Kernel entry and the freeze happen back-to-back

Right at the end of that window:

```
[heartbeat: exitCount=670000 elapsedSec=30.2 lastPort=0xCF8 write=1 val=0x0]
[kerneldiag] candidate address 0xFFFFF80359BA57DD resolves to module base 0xFFFFF80359800000 (+0x3A57DD into it)
[kerneldiag] debug directory: type=2 codeViewRva=0x40970
[kerneldiag] module PDB name: ntkrnlmp.pdb guid=D9424FC4-861E-47C1-0FAD1B35DEC6DCC8 age=1
[wp] armed 3 execution breakpoints at module base 0xFFFFF80359800000: ...
[watchdog] STALL #1 (exitCount=670823 unchanged for 2s): rip=0xFFFFF80359CBE7E2 ...
```

The `[kerneldiag]` lines are the hypervisor's own live module-identification
scan succeeding -- confirming RIP is genuinely executing inside the real
`ntoskrnl.exe` (PE debug directory read from guest memory matches the real
PDB name/GUID), not a stale artifact. The three-way breakpoints get armed
immediately after. Then, within the same ~823-exit span, `exitCount` stops
advancing entirely and never resumes. Resolving the frozen RIP
(`resolve_spin_rip.py`, scratchpad) gives **`HaliHaltSystem+0x12`** -- the
HAL routine `KeBugCheckEx` hands off to once bugcheck processing completes
and IRQL is raised to `HIGH_LEVEL`. This independently confirms (separately
from the earlier direct `KiBugCheckData` memory scan) that this is the
ordinary post-bugcheck halt, and that it happens essentially immediately
after -- not long after -- confirmed kernel-space execution begins.

**This substantially resolves the tension.** Pass 2 was never observed
doing "substantial Setup/driver-level work" in the first place -- the
original framing inferred that from a large raw exit count without
checking what those exits actually were. Most of them are pre-kernel
bootloader/firmware activity; genuine kernel execution is only confirmed
within a short window of the crash, with all three breakpoints already
armed and still silent throughout. There's no longer an unexplained "how
did it get that far" to reconcile.

### A loose end pulled on, and left dangling: who calls the heap initializers?

Since the crash now looks like it happens very early in kernel bring-up --
plausibly near where the Phase 0/Phase 1 boundary would fall -- it was
worth directly checking whether `ExInitializePagedHeaps`/
`ExInitializePoolHeapManagement` might actually be reachable from Phase 0
(before thread creation) rather than genuinely gated behind Phase 1, which
would have reopened the causal chain established in the previous update.
`ExInitSystem` (called once, early, from `InitBootProcessor`, well before
`PspInitPhase0`'s thread creation) turned out to itself be a two-phase
dispatcher, calling `ExpInitSystemPhase0` (RVA `0xA6C1F4`) and
`ExpInitSystemPhase1` (RVA `0xA3F42C`) -- mirroring `PsInitSystem`'s own
phase-dispatch structure. Full disassembly of both
(`check_exinitsystem.py`/`trace_heap_init_callers.py`, scratchpad) found
**neither calls either heap initializer directly.**
`ExpInitSystemPhase0`'s one heap-adjacent call, `RtlHpGlobalsInitialize`
(RVA `0x3C40B0`, disassembled in full at 0x4F bytes), only seeds RNG state
used elsewhere in the heap subsystem -- not the caller either.

This is a negative result, not a positive one: it rules out the two most
obvious "maybe the heap initializers are actually Phase-0-gated" candidate
call sites, but the *actual* caller of `ExInitializePagedHeaps`/
`ExInitializePoolHeapManagement` is still unidentified. That caller is now
the most direct lever on whether the previous update's causal chain
(thread never created -> heap managers never initialized -> NULL read ->
bugcheck) is fully correct, or needs revision.

See
[phase0-divergence-summary.md](phase0-divergence-summary.md) for the
updated fact/inference/unknown breakdown (facts 12-15, inferences I5-I6,
new unknowns U6-U7).

### Evidence artifacts (this update)

- `categorize_windows.py` (scratchpad) — tagged and counted log-line
  categories in the pass-1 pre-Phase0 window versus the pass-2
  reset-to-crash window from `phase1_3way_run1.log`; the basis for
  identifying the pass-2 window as bootloader-dominated.
- `resolve_crash_rva.py` (scratchpad) — confirmed the crash-time exception
  address resolves to `RtlpHpLfhOwnerMoveSubsegment+0xE8`, consistent with
  the previously-established root mechanism.
- `resolve_spin_rip.py` (scratchpad) — resolved the frozen watchdog-reported
  RIP to `HaliHaltSystem+0x12`, confirming the freeze is the ordinary
  post-bugcheck halt.
- `check_exinitsystem.py` (scratchpad) — disassembled `ExInitSystem`,
  `MmInitSystem`, and `KeInitSystem`, finding `ExInitSystem`'s own
  `ExpInitSystemPhase0`/`ExpInitSystemPhase1` dispatch split.
- `trace_heap_init_callers.py` (scratchpad) — resolved
  `ExInitializePagedHeaps` (RVA `0x3C3D70`) and
  `ExInitializePoolHeapManagement` (RVA `0x3C3B64`) exact addresses and
  confirmed neither `ExpInitSystemPhase0` nor `ExpInitSystemPhase1` calls
  either directly.
- `check_rtlhpglobals.py` (scratchpad) — full disassembly of
  `RtlHpGlobalsInitialize`, ruling it out as the heap initializers' caller.

## 2026-07-18 update: U6 resolved -- the heap initializers are Phase 0, not Phase 1, and the causal chain needs revision

A whole-image static scan (`find_heap_init_callers.py`, scratchpad --
windowed, overlapping, same technique as the earlier `Phase1Initialization`
address-reference search) for direct `CALL` instructions or RIP-relative
address references to `ExInitializePagedHeaps` (RVA `0x3C3D70`) or
`ExInitializePoolHeapManagement` (RVA `0x3C3B64`) anywhere in the 16MB
kernel dump found **zero matches of either kind**. Whatever calls them does
so indirectly -- a register loaded some other way, or a function-pointer
table -- which a byte-pattern scan can't reliably resolve.

### Live capture: the return address is unambiguous regardless of call form

Every `CALL` variant (direct, indirect, table-based) pushes the return
address onto the stack identically, so an execution breakpoint at each
function's own entry sidesteps the whole ambiguity: whatever is at
`[rsp+0x00]` when the breakpoint fires *is* the caller, full stop. The
existing three-way breakpoint infrastructure (DR0/DR1/DR2, same mechanism
as the previous two updates) was retargeted: DR0 = `ExInitializePagedHeaps`
entry, DR1 = `ExInitializePoolHeapManagement` entry, DR2 kept on
`Phase1InitializationDiscard`'s entry for relative-ordering context.
Armed on pass 1 (`heap_caller_run1.log`, scratchpad), all three fired
cleanly:

```
[wp] EXECUTION BREAKPOINT HIT #1: ExInitializePoolHeapManagement ENTRY ... exitCount=102760
  [rsp+0x00] = 0xFFFFF80714E458EF  -> RVA 0xA458EF
[wp] EXECUTION BREAKPOINT HIT #2: ExInitializePagedHeaps ENTRY ... exitCount=102765
  [rsp+0x00] = 0xFFFFF80714E570DF  -> RVA 0xA570DF
[wp] EXECUTION BREAKPOINT HIT #3: Phase1InitializationDiscard ENTRY ... exitCount=102795
```

Resolving both return addresses (`resolve_heap_callers.py`, scratchpad):

- `ExInitializePoolHeapManagement`'s caller: **`MiInitNucleus+0x44B`**
- `ExInitializePagedHeaps`'s caller: **`MiInitSystem+0x8F`**

Neither is anywhere near `Phase1Initialization`. Both are Memory Manager
(`Mi`) functions. Full disassembly of `MmInitSystem` (already available
from the previous update's `check_exinitsystem.py`) shows it calls
`MiInitSystem` directly (twice) and `MiInitNucleus` directly (once) --
these are the real, immediate callers, one level up from the two heap
initializers.

### The ordering rules out the previous causal model

`InitBootProcessor`'s own resolved call list (`initboot_calls.txt`, from
the previous update) shows it calls `MmInitSystem` at file offset `+0x575`
-- chronologically *before* its call to `PsInitSystem` at `+0x969`, the
call that eventually reaches `PspInitPhase0`'s `PsCreateSystemThread`.
This is confirmed live too, in the exact same capture: both heap-initializer
hits (`exitCount` 102760, 102765) fire ~30-35 VM exits *before*
`Phase1InitializationDiscard`'s own entry (`exitCount` 102795) -- which
itself requires the Phase 1 thread to already exist and be scheduled.

**This means the causal chain reported after the previous update -- "Phase
1 thread never created -> heap managers never initialized -> NULL read ->
bugcheck" -- is not a real chain.** `MmInitSystem` and its descendants run
entirely within Phase 0, on the original boot-processor thread, well
before `PsInitSystem`/`PspInitPhase0` is even reached. The heap
initializers never depended on the Phase 1 thread existing in the first
place. Since both the heap initializers (confirmed silent on pass 2 by the
earlier data-watchpoint updates) and the `PsCreateSystemThread` call
(confirmed silent on pass 2 by the three-way experiment) fail, and
`MmInitSystem` precedes `PsInitSystem`, the more likely explanation is a
single earlier divergence -- at or before `MmInitSystem`'s own execution --
that independently prevents both downstream outcomes, rather than one
causing the other.

**Not yet tested:** whether `MmInitSystem` itself is reached at all on
pass 2. That's the direct, mechanical next step -- the same
entry-breakpoint-plus-stack-read method used here, retargeted one level
higher. See
[phase0-divergence-summary.md](phase0-divergence-summary.md) (facts 16-18,
inference I3′, unknown U8) for the full updated model.

### Evidence artifacts (this update)

- `find_heap_init_callers.py` (scratchpad) — whole-image windowed scan for
  direct calls/address references to both heap initializers; found none,
  motivating the live-capture approach.
- `heap_caller_run1.log`/`heap_caller_run1_err.log` (scratchpad) — the
  live capture: both heap initializers' entry breakpoints and
  `Phase1InitializationDiscard`'s all fire on pass 1, in that order.
- `resolve_heap_callers.py` (scratchpad) — resolved the genuine `[rsp+0x00]`
  return addresses to `MiInitNucleus+0x44B` and `MiInitSystem+0x8F`.

## 2026-07-18 update: U8 resolved -- MmInitSystem itself is never reached on pass 2

Direct, mechanical continuation of the previous update's method,
retargeted one level higher: the same three-DR-register infrastructure was
retargeted to `InitBootProcessor`'s own entry (RVA `0xA3D054`),
`MmInitSystem`'s own entry (RVA `0xA56F60` -- the U8 question), and
`PspInitPhase0`'s own entry (RVA `0xA401D8`, distinct from its `+0x86D`
call site already known never to fire on pass 2). Armed across both
passes (`u8_run1.log`, scratchpad):

```
Pass 1 (module base 0xFFFFF8014D800000):
[wp] armed 3 execution breakpoints ...
[wp] EXECUTION BREAKPOINT HIT #1: MmInitSystem ENTRY (U8) ... exitCount=203916
[wp] EXECUTION BREAKPOINT HIT #2: PspInitPhase0 ENTRY ... exitCount=204067
  (InitBootProcessor's own entry breakpoint never fired -- see below)
...
[reset] port 0x64/0xFE keyboard-controller reset pulse honored (exitCount=1303718)
[wp] armed 3 execution breakpoints at module base 0xFFFFF80516C00000 ...   <- pass 2, fresh KASLR base
[kerneldiag] candidate address ... ntkrnlmp.pdb                            <- kernel module identified
[watchdog] STALL #1 (exitCount=1516046 unchanged for 2s): rip=0xFFFFF805170BE7E2 ...
```

**Pass 1:** `MmInitSystem` and `PspInitPhase0` both fired cleanly, 151 VM
exits apart -- two solid positive controls. `InitBootProcessor`'s own
entry did *not* fire, which is explained rather than concerning: the
opportunistic arming mechanism only triggers on the first VM exit with a
canonical RIP, and evidently that first canonical-RIP exit landed just
after `InitBootProcessor`'s own first instruction but before its call to
`MmInitSystem` -- bracketed on both sides by the two hits that did fire.
This specific checkpoint is inherently unreliable with this method and
shouldn't be read either way, on either pass.

**Pass 2:** armed immediately after the reset. **None of the three
fired**, all the way through to the bugcheck. The same pattern from the
previous update reproduced almost exactly in this independent run
(different KASLR base, different absolute exit counts): `fwcfg` replay,
`BdsDxe` loading `Boot0002`, a long single-port poll, kernel module
identified, then the exit count froze within the same watchdog tick the
module was found in. The frozen RIP resolved to RVA `0x4BE7E2` --
`HaliHaltSystem+0x12`, the exact same halt-routine offset as the previous
update's capture, on a completely different KASLR base. Same crash, same
halt routine, reproduced independently.

**This confirms U8's answer directly rather than by inference:**
`MmInitSystem` is never reached on pass 2. The earliest confirmed
divergence moves back from `PspInitPhase0+0x86D` to
`InitBootProcessor+0x575` (`MmInitSystem`'s call site) -- or earlier still,
since `InitBootProcessor`'s own entry couldn't be tested reliably this
round. Two things now look well-supported independent of exactly where the
divergence lands: it's a single cause producing every downstream symptom
checked so far (not a chain), and it happens early and consistently across
independent runs with different KASLR bases.

**Not yet tested:** whether `KiInitializeKernel`/`KiSystemStartup` --
earlier than `InitBootProcessor` -- are reached on pass 2, which would
distinguish a kernel-entry-level divergence from one inside
`InitBootProcessor`'s own sequence before `+0x575`. See
[phase0-divergence-summary.md](phase0-divergence-summary.md) (fact 19,
revised I4, new unknown U9) for the full updated model.

### Evidence artifacts (this update)

- `u8_run1.log`/`u8_run1_err.log` (scratchpad) — the live capture:
  `MmInitSystem` and `PspInitPhase0` entries both fire cleanly on pass 1;
  none of the three (including `InitBootProcessor`'s own entry) fire on
  pass 2, despite the same bootloader-then-crash pattern reproducing.

## 2026-07-18 update: U9 resolved -- the divergence is inside HalInitSystem itself

Two more rounds, continuing directly from U8's result, bracketing the
divergence down from "somewhere before `MmInitSystem`" to "within one
function's call tree."

### Round 1: bisecting InitBootProcessor's own sequence

Before arming anything new, the RVA our discovery mechanism itself
observed as the *current RIP* at the moment it identified the kernel
module on pass 2 (logged identically in both the U8 run and the original
three-way run: `0x3A57DD`) was resolved (`resolve_discovery_rva.py`,
scratchpad): **`HalpApicInitializeIoUnit+0x5D`** -- a genuine, early HAL
routine (I/O APIC initialization), not the eventual post-bugcheck halt
loop (`HaliHaltSystem+0x12`, resolved separately for comparison). This
matters methodologically too: it confirms discovery catches real early
pass-2 execution, not merely whatever RIP happens to be visible once
already frozen -- reassuring, if incomplete, evidence against the U4
concern.

`InitBootProcessor`'s own resolved call list places `HalInitSystem`
(which `HalpApicInitializeIoUnit` is presumably called from) at file
offset `+0x312` -- well before `MmInitSystem`'s `+0x575`. So pass 2 is
now known to reach at least `InitBootProcessor+0x312`'s subtree. Two
breakpoints bisected the gap: `CmInitSystem0` entry (`+0x348`) and
`KeInitSystem` entry (`+0x370`), with `MmInitSystem` entry kept as the
known-negative anchor. Result (`u9_run1.log`, scratchpad): all three fire
cleanly on pass 1 (`CmInitSystem0` and `KeInitSystem` as fresh positive
controls; `KeInitSystem`/`MmInitSystem` each fire a second time later in
pass 1, consistent with a two-phase Phase-0/Phase-1 dispatch pattern like
`ExInitSystem`'s, not investigated further); **none fire on pass 2**. The
divergence is now bracketed to a 54-byte stretch of `InitBootProcessor`'s
own linear code: `+0x312` (reached) to `+0x348` (not reached).

### Round 2: disassembly finds a branch, and a live test falsifies the obvious reading

Disassembling that 54-byte stretch (`disasm_bracket.py`, scratchpad) found
the answer immediately:

```
InitBootProcessor+0x312: call HalInitSystem
InitBootProcessor+0x317: test al, al
InitBootProcessor+0x319: je <bailout>
```

A direct conditional branch on `HalInitSystem`'s own return value. The
bailout target (`resolve_halinit_bailout.py`, scratchpad) loads
`ecx = 0x5C` before its first call -- `0x5C` is the real NT bugcheck code
`HAL_INITIALIZATION_FAILED`, strongly suggesting `KeBugCheckEx`. That
looked like the answer, but it predicts something the rest of this
investigation contradicts: an *immediate* bugcheck, not the long runway
before the eventual `0x139` crash that's been observed throughout. Rather
than accept the static reading, a breakpoint was armed directly at
`InitBootProcessor+0x317` (the `test al,al` instruction itself -- AL at
that exact point holds `HalInitSystem`'s raw return value, since the
fault-style breakpoint fires before `test` executes). Result
(`halinit_run1.log`, scratchpad):

```
Pass 1: [wp] HalInitSystem returned AL = 0x01 (TRUE/success)   <- fires normally
Pass 2: (never fires -- not once, despite the full reset-to-bugcheck window)
```

**This falsifies the "returns failure, branch taken" hypothesis.**
`HalInitSystem`'s call doesn't return `FALSE` on pass 2 -- it doesn't
return to its caller *at all*, in any form current instrumentation can
observe. Combined with round 1's confirmation that `HalpApicInitializeIoUnit`
(inside `HalInitSystem`'s own call tree) does run, the divergence is now
bracketed to *within* `HalInitSystem`'s own execution, sometime after
`HalpApicInitializeIoUnit` runs and before it would otherwise return.

### A speculative but coherent unifying possibility

Not yet tested, but worth recording: `HalInitSystem` runs *before*
`MmInitSystem` (the function confirmed to actually initialize the Segment
Heap descriptor slots, per the earlier "U6 resolved" update). If
something in `HalInitSystem`'s own call tree -- ACPI/PNP resource
enumeration is architecturally plausible -- needs pool memory before that
initialization has run, it would hit the exact same NULL-heap-descriptor
condition already established as the bugcheck's proximate mechanism. If
so, the "long runway" before the `0x139` crash observed throughout this
investigation wouldn't be unrelated bootloader/idle time at all -- it
would be `HalInitSystem`'s own legitimately lengthy resource-enumeration
work, right up until the fatal allocation. This would unify "why doesn't
the Phase 1 thread get created" and "why does the heap read NULL" into one
continuous story. See
[phase0-divergence-summary.md](phase0-divergence-summary.md) (facts 20-22,
revised I4, new inference I7, new unknown U10) for the full updated model.

### Evidence artifacts (this update)

- `resolve_discovery_rva.py` (scratchpad) — resolved the discovery
  mechanism's own observed RIP to `HalpApicInitializeIoUnit+0x5D`,
  confirming discovery catches genuine early pass-2 execution.
- `initboot_calls_resolved.txt` (scratchpad) — full resolved
  `InitBootProcessor` call list, used to place `HalInitSystem`,
  `CmInitSystem0`, and `KeInitSystem` relative to `MmInitSystem`.
- `u9_run1.log`/`u9_run1_err.log` (scratchpad) — the bisection capture:
  `CmInitSystem0` and `KeInitSystem` both fire cleanly on pass 1, neither
  fires on pass 2.
- `disasm_bracket.py` (scratchpad) — disassembled the 54-byte bracket,
  finding the `call HalInitSystem; test al,al; je <bailout>` sequence.
- `resolve_halinit_bailout.py` (scratchpad) — resolved and disassembled
  the bailout target, finding the `ecx=0x5C` bugcheck-code setup.
- `halinit_run1.log`/`halinit_run1_err.log` (scratchpad) — the decisive
  live capture: `HalInitSystem` returns `TRUE` on pass 1;
  `InitBootProcessor+0x317` never fires at all on pass 2, falsifying the
  "returns failure" hypothesis.

## 2026-07-18 update: U10 resolved -- HalpIommuInitSystem never succeeds on pass 2

Direct continuation of the previous update: what happens inside
`HalInitSystem`, after `HalpApicInitializeIoUnit`, that prevents it from
ever returning on pass 2?

### Static: HalInitSystem is a table-driven dispatch loop, not a call chain

`HalInitSystem` (52 bytes) is itself a tiny Phase-0/Phase-1 dispatcher --
the same pattern as `PsInitSystem` and `ExInitSystem` -- calling
`HalpInitSystemPhase0` or `HalpInitSystemPhase1` (`disasm_halinitsystem.py`,
scratchpad). Both of those (`disasm_halinitphase0.py`, scratchpad) call a
single shared function, `HalpInitSystemHelper` (141 bytes), which itself
makes exactly **one** call: to `guard_dispatch_icall`, Control Flow
Guard's indirect-call trampoline. This is standard real-HAL architecture:
a table of function pointers, one per platform sub-init routine (ACPI,
IOMMU, timer, etc.), dispatched indirectly through a CFG-checked call. It
also retroactively explains why the much earlier whole-image static scan
(the very first attempt at U6) found no direct references to anything --
a CFG-guarded indirect call doesn't leave a literal `call rel32` or a
matching address load at the call site for a byte-pattern scan to find.

### Live: tracing the dispatch loop directly

Per the standard MSVC/CFG x64 ABI, the actual call target is loaded into
RAX immediately before `call guard_dispatch_icall`. A new, dedicated
fourth hardware breakpoint (DR3) was added to the existing infrastructure
-- unlike DR0-DR2 (one-shot), DR3 stays armed across repeated hits within
the same pass, since a dispatch loop calls through the same site once per
table entry. First run (cap of 40 hits) immediately showed something
unexpected: the dispatch target was **identical** across all 40 hits, same
RSP throughout -- resolved (`resolve_dispatch_targets.py`, scratchpad) to
real HAL subsystem initializers: `HalpIommuInitSystem` in the first arm
cycle, `HalpAcpiInitSystem` in a second. Confirmed this was a real,
table-driven loop, not a stuck instruction -- but 40 hits wasn't enough to
see either subsystem's loop actually terminate. The cap was raised to
3000 and the capture rerun end-to-end across both passes
(`haldispatch_run2.log`, scratchpad; analyzed with
`analyze_dispatch_pass2.py`):

```
Pass 1: target=HalpIommuInitSystem for 3000 hits (capped) -> re-arm -> target=HalpAcpiInitSystem for 3000 hits (capped)
        (the target CHANGING at all proves HalpIommuInitSystem terminates and HalInitSystem returns on pass 1)

Pass 2: target=HalpIommuInitSystem for all 3000 captured hits, IDENTICAL RSP throughout, zero change
        ... then KiBugCheckData: code=0x139
```

**On pass 2, `HalpIommuInitSystem` never advances and never returns, all
the way to the bugcheck.** This is the earliest and most specific
divergence point found in this entire investigation -- every previously
reported "earliest divergence" (the missing Phase 1 thread, the
uninitialized heap managers, `PspInitPhase0+0x86D`, `MmInitSystem`,
"somewhere in `HalInitSystem`") sits downstream of this single point:
`InitBootProcessor` calls `HalInitSystem`, which never returns, because
its IOMMU (Intel VT-d / DMAR) bring-up subsystem never completes.

### What HalpIommuInitSystem actually does

Full disassembly (`disasm_iommuinit.py`, scratchpad) shows a substantial,
mostly-linear IOMMU bring-up sequence -- `HalpIommuInitializeDmar`,
`IommuInitializeLibrary`, `HalpIommuInitInterrupts`,
`HalpIommuProcessReservations`, `HalpIommuInitializeAll`, and others, each
gated by a conditional bailout on failure. No single obvious spin
instruction inside the function itself; the repeated identical-RSP
dispatch hits are consistent with the *caller* of `HalpInitSystemHelper`
re-invoking it for the same table entry (a retry loop one level up), not a
tight loop inside `HalpIommuInitSystem` itself.

Worth flagging: several of these callees are architecturally very likely
to allocate pool memory -- before `MmInitSystem` has initialized the
Segment Heap descriptors. This strengthens (but doesn't yet prove) the
speculative unifying hypothesis from the previous update: that this exact
retry loop might be the *direct* proximate cause of the `0x139` bugcheck,
not a separate, earlier, otherwise-unrelated problem. Not yet tested --
the crash's own call stack, captured live at the fault site, hasn't been
checked for whether it runs back through `HalpIommuInitSystem`. See
[phase0-divergence-summary.md](phase0-divergence-summary.md) (facts 23-25,
resolved I4, strengthened I7, new unknowns U11-U13) for the full updated
model.

### Evidence artifacts (this update)

- `disasm_halinitsystem.py` (scratchpad) — disassembled `HalInitSystem`,
  finding its own Phase-0/Phase-1 dispatch split.
- `disasm_halinitphase0.py` (scratchpad) — traced
  `HalpInitSystemPhase0`/`Phase1` down to the shared `HalpInitSystemHelper`
  and its single `guard_dispatch_icall` call.
- `haldispatch_run1.log` (scratchpad) — first live trace (cap of 40),
  establishing the dispatch site fires with a resolvable, real HAL
  function as its target.
- `resolve_dispatch_targets.py` (scratchpad) — resolved the two observed
  dispatch targets to `HalpIommuInitSystem` and `HalpAcpiInitSystem`.
- `haldispatch_run2.log`/`haldispatch_run2_err.log` (scratchpad) — the
  decisive full-run capture (cap raised to 3000): pass 1 advances past
  `HalpIommuInitSystem` to a second subsystem; pass 2 never advances.
- `analyze_dispatch_pass2.py` (scratchpad) — parsed and compared the
  pass-1 vs pass-2 dispatch-target sequences from `haldispatch_run2.log`.
- `disasm_iommuinit.py` (scratchpad) — full disassembly of
  `HalpIommuInitSystem`, enumerating its own callees for the next round.

## 2026-07-18 update: U12 resolved -- the crash is a routine allocation caught by the vulnerability window, not HalpIommuInitSystem's own code

Direct test of I7 (does the eventual `0x139` crash's own call stack run
back through `HalpIommuInitSystem`?), in two rounds -- the first hit a
precision limit, the second used a more reliable technique already proven
earlier in this investigation.

### Round 1: a deep heuristic stack scan at the exact fault site

A one-shot breakpoint was armed at the exact faulting instruction
(`RtlpHpLfhOwnerMoveSubsegment+0xE8`, RVA `0x344214`) with a much deeper
stack scan than previous rounds (1024 bytes vs 80). It fired cleanly on
pass 2 (`u12_crashstack_run1.log`, scratchpad) with the full, real call
stack still intact (the fault hasn't happened yet at this exact RIP).
Resolving every candidate address found (`resolve_crash_stack.py`,
scratchpad) confirmed and extended the already-known allocator chain by
one level: `ExAllocatePoolWithTag+0x64` -> `ExAllocateHeapPool+0x2B1` ->
`RtlpHpLfhSlotAllocate+0xCA6` -> `RtlpHpLfhBucketGetSubsegment+0x60` ->
the fault site. Several higher-offset stack entries also resolved to
plausible-sounding HAL interrupt-registration functions
(`HalpInterruptRegisterController`, `HalpInterruptRegisterLine`,
`HalpPicInitializeIoUnit`) -- but checking their own disassembly
(`check_iommuinitinterrupts.py`, `check_allocatekinterrupt.py`,
`check_halpmmalloc.py`, scratchpad) showed none of them call
`ExAllocatePoolWithTag` (they call a different function,
`HalpMmAllocateMemoryInternal`, which itself makes no further calls) --
ruling them out as the *direct* caller and marking them as more likely
stale stack data than live call-chain members. This is the heuristic
stack scan's own precision limit: scanning for "any canonical-looking
qword" can't reliably distinguish a genuine return address from stale data
left over from earlier, already-returned calls.

### Round 2: the proven technique -- multi-shot breakpoint at the allocator's own entry

Rather than keep pushing the heuristic scan, the same reliable technique
already proven for U6 was reused: a breakpoint at `ExAllocatePoolWithTag`'s
own entry (RVA `0x9B7010`), made multi-shot (like the DR3 HAL-dispatch
tracer), reading the guaranteed-real return address at `[rsp+0x00]` on
every call -- not a heuristic guess, since every x86 `CALL` pushes it
there identically. Capped at 200 hits per pass. Result
(`poolcaller_run1.log`, scratchpad):

```
Pass 1 (first 200 calls, early in boot): every single call from PsInitializeQuotaSystem+0x8F
                                          -- ordinary, expected Ps-subsystem init.

Pass 2 (200 calls immediately preceding the bugcheck): every single call from KiSwInterruptDispatch+0x91
                                          -- generic software-interrupt (DPC/APC-level) dispatch code.
```

(`resolve_poolcallers.py`, scratchpad, for both resolutions.)

**Neither `HalpIommuInitializeDmar` nor `IommuInitializeLibrary` (the
callees I7 originally speculated about) appear anywhere in this trace.**
The pass-2 caller is unrelated software-interrupt dispatch code, not
anything in `HalpIommuInitSystem`'s own call tree. This pass-2 capture's
start lines up almost exactly with where the DR3 HAL-dispatch tracer's own
3000-hit cap ended in the same run -- best explained by DR3 monopolizing
every VM exit while armed (only one hardware breakpoint can fire per
exit), not a causal relationship between the two loops.

**I7 is confirmed, but one level more indirect than originally proposed.**
`HalpIommuInitSystem`'s retry loop doesn't need to allocate pool memory
itself to cause the crash -- it only needs to prevent `HalInitSystem` from
ever returning, which prevents `MmInitSystem` from ever initializing the
Segment Heap descriptors. That leaves *every* pool allocation attempt
during the entire (apparently unbounded) retry window vulnerable,
including ones with no relationship to IOMMU at all -- in this captured
instance, the guest's own software-interrupt/DPC dispatch code. Pass 1's
equivalent early allocations succeed simply because `MmInitSystem` has
already run by the time they happen; pass 2's don't, because it never
does. The crash is a consequence of timing (allocating during an
unbounded vulnerability window), not of any specific code path being
wrong.

This closes the loop this investigation opened with: the missing Phase 1
thread, the uninitialized heap managers, and the `0x139` bugcheck are now
all explained as consequences of one single fact -- `HalpIommuInitSystem`
never completes on pass 2. See
[phase0-divergence-summary.md](phase0-divergence-summary.md) (facts 26-27,
refined I7, resolved U12, new unknown U14) for the full updated model. The
sole remaining highest-priority open question (U13) is *why*
`HalpIommuInitSystem` fails to make progress on pass 2 -- the first point
in this investigation where hardware-fidelity analysis is no longer
premature.

### Evidence artifacts (this update)

- `u12_crashstack_run1.log`/`_err.log` (scratchpad) — live capture at the
  exact faulting instruction with a deep (1024-byte) stack scan.
- `resolve_crash_stack.py` (scratchpad) — resolved every candidate address
  from the deep stack scan; confirmed the allocator chain, ruled out the
  interrupt-registration functions as the direct caller.
- `check_iommuinitinterrupts.py`, `check_allocatekinterrupt.py`,
  `check_halpmmalloc.py` (scratchpad) — traced whether
  `HalpIommuInitInterrupts` (one of `HalpIommuInitSystem`'s own callees)
  transitively reaches the functions found on the stack; found it doesn't,
  within the levels checked.
- `poolcaller_run1.log`/`_err.log` (scratchpad) — the decisive multi-shot
  capture: pass 1's early `ExAllocatePoolWithTag` callers are
  `PsInitializeQuotaSystem`; pass 2's are `KiSwInterruptDispatch`.
- `resolve_poolcallers.py` (scratchpad) — resolved both caller RVAs.

## 2026-07-18 update: U13 corrected, not resolved -- HalpIommuInitSystem was never actually failing

Direct continuation of U12: why does `HalpIommuInitSystem` fail to make
progress on pass 2? The answer overturns the question's own premise
rather than answering it as posed.

### Static: the earlier "one call" reading missed a real loop

Full disassembly of `HalpIommuInitializeDmar`, `IommuInitializeLibrary`,
and related callees (`disasm_dmar_and_library.py`, scratchpad) found no
hardware-facing instructions (no `rdmsr`/`cpuid`/`in`/`out`) at that
level -- expected, since real code usually pushes hardware access into
leaf functions, but it meant the "why does it fail" question needed to
move up a level. Re-examining `HalpInitSystemHelper` in full
(`disasm_helper_full.py`, scratchpad) -- not just the calls the earlier
(U10) scan had listed -- revealed the earlier scan had missed the real
structure: `HalpInitSystemHelper` is a genuine nested loop (outer index
from its `ecx`/`edx` arguments, inner index 0-20 over a 21-entry table,
`HalSubComponents`, stride 0x10 bytes), calling one function pointer per
slot via CFG dispatch and advancing to the next slot on any non-negative
return -- only a *negative* result aborts the whole function. The earlier
scan reported "one call" because it only listed distinct call
*instructions* found by linear disassembly; a loop body containing one
`call` instruction still executes that instruction many times at runtime,
which a static instruction listing can't show.

### Live: HalpIommuInitSystem succeeds every time -- the loop just doesn't advance

Two new multi-shot breakpoints (reusing DR0 and DR3's proven multi-shot
infrastructure) traced `HalpIommuInitSystem` directly: one at its own
entry (capturing RCX/RDX/R8/R9, its real arguments) and one at its own
return point (capturing EAX, its real return value). Result
(`iommu_argsret_run1.log`, scratchpad; merged and sorted chronologically
with `merge_iommu_trace.py` to see past the two breakpoints' different hit
caps):

```
Pass 1: every call -- rcx=0x9 rdx=0x0 r9=0x1 (r8 a stable per-pass pointer); every return -- EAX=0 (success)
Pass 2: every call -- rcx=0x9 rdx=0x0 r9=0x1 (same pattern); every return -- EAX=0 (success)
```

**`HalpIommuInitSystem` returns success on every single call, on both
passes.** The earlier (U10) framing -- "it never succeeds on pass 2" --
was wrong. What's real: the same call repeats with no observed
advancement to a different table slot, on pass 2; on pass 1, *something*
eventually does advance (already established, U10) to `HalpAcpiInitSystem`.

### Static, again: parameter 9 never reaches real IOMMU logic at all

Given every call succeeds, the natural next question is what that success
actually represents. Full, unfiltered disassembly of `HalpIommuInitSystem`
itself (`disasm_iommuinit_full.py`, scratchpad, with backward-jump
detection to rule out an internal loop -- none found) shows it's a
dispatcher on its own first parameter (`edi`, copied from `rcx`). Verified
precisely with exact offset arithmetic (`verify_switch_offsets.py`,
scratchpad) rather than estimation:

```
+0x0037: cmp edi, 8
+0x003A: jne +0x56        <- taken for edi=9 (9 != 8)
+0x0056: xor eax, eax     <- returns 0 almost immediately
+0x0058: jmp +0x1CB       <- straight to the epilogue
```

The real DMAR/interrupt/library initialization code
(`HalpIommuInitializeDmar`, `IommuInitializeLibrary`,
`HalpIommuInitInterrupts`) is gated behind a *different* switch, reachable
only for `edi` values `0`, `0x11`, `0x13`, or `0x20` -- none of which is
`9`. **The observed "success" never represented successful hardware or
ACPI detection. It's a near-no-op return for a parameter value this
function has no specific handling for.**

### What this means for the model

The causal chain built over the last several updates -- stalled dispatch
loop -> `HalInitSystem` never returns -> `MmInitSystem` never initializes
the heap -> the Phase 1 thread never gets created -> an unrelated
allocation eventually crashes -- is **unaffected** by this correction; all
of that rested on the loop never advancing, which remains true. What
changes is the explanation for *why* it doesn't advance: not "IOMMU
hardware detection fails on pass 2" (there's no evidence any hardware
detection is even attempted in the calls observed), but an as-yet-unknown
loop-control or dispatch-advancement issue happening *above*
`HalpIommuInitSystem`'s own logic. Every function in the visible call
chain above it (`HalpInitSystemHelper`, `HalpInitSystemPhase0`/`Phase1`,
`HalInitSystem`) has been individually confirmed non-looping by its own
disassembly, so the actual mechanism driving the repetition is still
unidentified -- see
[phase0-divergence-summary.md](phase0-divergence-summary.md) (facts 28-30,
corrected I4, new unknowns U15-U16, new hypotheses H6-H7) for the full
updated model and the proposed next live capture (breakpointing
`HalpInitSystemHelper`'s own loop-control registers directly, rather than
inferring them).

### Evidence artifacts (this update)

- `disasm_dmar_and_library.py` (scratchpad) — checked
  `HalpIommuInitializeDmar`/`IommuInitializeLibrary`/etc. for
  hardware-facing instructions; found none at that level.
- `disasm_dmar_full.py` (scratchpad) — full unfiltered disassembly of the
  small IOMMU functions, examining what feeds their branches.
- `disasm_helper_full.py` (scratchpad) — the full `HalpInitSystemHelper`
  disassembly that revealed the real nested-loop structure missed by the
  earlier scan.
- `iommu_argsret_run1.log`/`_err.log` (scratchpad) — the decisive live
  capture: `HalpIommuInitSystem`'s real arguments and return value on
  every call, both passes.
- `merge_iommu_trace.py` (scratchpad) — merged and chronologically sorted
  the entry/return event streams, revealing the two breakpoints' different
  caps as the explanation for the apparent "entry-only then return-only"
  pattern in the raw log.
- `disasm_iommuinit_full.py` (scratchpad) — full, backward-jump-checked
  disassembly of `HalpIommuInitSystem`, ruling out an internal loop and
  finding the parameter-9 short-circuit path.
- `verify_switch_offsets.py` (scratchpad) — precise offset arithmetic
  confirming the parameter-9 branch never reaches the real IOMMU logic.

## 2026-07-18 update: U15 -- pass-2 HalpIommuInitSystem calls do NOT come from HalpInitSystemHelper

Eighth follow-up, same date. This one overturns the U10-U13 pass-2 model.

### Process fix first: the VM must boot in UEFI mode
Nearly an hour was lost here to a launch-argument mistake. The VM had been
relaunched with `bios.bin` (legacy SeaBIOS), which gives only 1MB guest RAM
-- Windows 10 cannot boot at all. Symptoms were a SeaBIOS POST stall, a
keyboard self-test failure, and the AHCI disk being mis-probed as an ATAPI
CD-ROM ("could not read the boot disk", ABAR at 0xFEBFC000). All of these
are downstream of the wrong boot mode, not real bugs. The whole
investigation actually runs in **UEFI mode** with OVMF firmware:
`Hypervisor.exe <title> C:\Users\DELL\Downloads\RELEASEX64_OVMF.fd win10_installer.vhd`
-- the `.fd` extension selects uefiMode (3GB RAM); OVMF places the AHCI
ABAR at 0xC0000000 and boots normally. Confirmed by the good log's own
header line ("Loaded 4194304 bytes of UEFI firmware ... RELEASEX64_OVMF.fd").
An ignored-HRESULT diagnostic was added to the ABAR-mapping code
(instrumentation only) and confirmed the map itself always succeeds
(hr=0x0), so the AHCI symptom was never a mapping failure -- it was purely
the wrong firmware/RAM size.

### The measurement
A DR1 execution breakpoint was retargeted to `HalpInitSystemHelper`'s own
entry (RVA 0x99E7A8), multi-shot, logging ecx/edx/r8 + a hit counter
(`[helperentry]`), running alongside the existing DR0/DR3 tracers on
`HalpIommuInitSystem`'s entry/return. Caps: helper & iommuret 3000,
iommuargs 200. Per-pass hit counts, split at the `[reset]` marker
(exitCount 391352), from scratchpad/u15_uefi_run1.log:

    PASS 1 (before reset):  helperentry 3001 (capped)  iommuargs 201  iommuret 3001 (capped)
    PASS 2 (after reset):   helperentry 0              iommuargs 201  iommuret 3001 (capped)

On pass 2, DR1 was confirmed armed at the correct new KASLR base
(0xFFFFF8002BC00000, VA 0xFFFFF8002C59E7A8) -- the same arm block that
armed DR0/DR3, which both fired. So `helperentry = 0` is genuine, not a
re-arm miss. Pass-2 iommu calls begin at exitCount 483932 (well past the
reset), all with rcx=9, identical to pass 1's parameter.

### What it means
On pass 2 the HAL Phase-0 dispatch loop (`HalpInitSystemHelper`) **never
runs**, yet `HalpIommuInitSystem(rcx=9)` is still called thousands of
times. Therefore the pass-2 repeated calls come from a **different caller**
than pass 1's Phase-0 loop. This retires the U10-U13 framing ("a dispatch
loop inside HalInitSystem never advances on pass 2") -- that loop is not
the pass-2 culprit. Both H6 (helper looping internally, would show >=1
entry) and H7 (external re-invocation of the helper, would show ~1:1
entries) are ruled out. The pass-2 stall is at **IRQL 0xF (HIGH_LEVEL)** =
interrupt/DPC context, not PASSIVE_LEVEL boot init, pointing at an
interrupt-context storm rather than the HAL init path -- plausibly linked
to the U12 KiSwInterruptDispatch -> ExAllocatePoolWithTag chain. The
success-every-call and parameter-9 short-circuit facts are unchanged; the
crash mechanism (heap never initialized) is unaffected.

New open question **U17**: identify the actual pass-2 caller of
HalpIommuInitSystem -- breakpoint its entry and read the return address at
[rsp+0x00] (or walk the stack), and correlate with the HIGH_LEVEL IRQL.

### Evidence artifacts (this update)
- `u15_uefi_run1.log` (scratchpad) -- the decisive per-pass capture
  (helperentry 3001/0, iommu 3001/3001), including both re-arm lines.
- `disasm_helper_full.py` (scratchpad) -- confirmed HalpInitSystemHelper
  entry RVA 0x99E7A8 and its nested-loop body (ecx/edx outer, edi inner).
- ABAR-map HRESULT instrumentation in Hypervisor.c (ahciHandleBar5Access)
  -- confirmed WHvMapGpaRange succeeds even at 0xFEBFC000.

## 2026-07-18 update: U17 -- corrects U15, caller IS HalpInitSystemHelper

Breakpointed HalpIommuInitSystem entry on pass 2 and read [rsp+0x00] (the
guaranteed real return address). Result: RVA 0x99E7F7 = HalpInitSystemHelper+0x4F,
the instruction right after its 'call guard_dispatch_icall' (+0x4A). So the
pass-2 caller IS HalpInitSystemHelper -- U15's 'different caller' was an
artifact of the DR1 entry breakpoint being armed AFTER Phase-0 was already
entered on pass 2. HalpInitSystemHelper is entered once (pre-rearm), then
stuck in its internal inner dispatch loop re-calling HalpIommuInitSystem; the
entry (+0x00) never re-fires (helperentry=0) but the dispatch site (+0x4A)
fires thousands of times. This is H6 (single entry, internal loop stuck).
Open question U18: why the inner loop never advances despite EAX=0 every
call; reconcile with the HIGH_LEVEL IRQL at the stall. Evidence: scratchpad/u17_run1.log.

## 2026-07-19 update: U18/U19 -- pass-2 loop is advancing, re-driven externally, not counter-stuck

U18: repurposed DR2 to HalpInitSystemHelper+0x5A (RVA 0x99E802, inc edi after
the dispatch call). On pass 2 it fired 3000+ times (capped) with steadily
progressing exitCounts -- the inner loop ADVANCES (edi increments), it is not
frozen in place.

U19: added ebp (outer loop limit) to the DR0 loopstate capture. Result: at the
IOMMU dispatch, ebx=9, ebp(limit)=0x10, edi=0xB -- and these are IDENTICAL on
pass 1 (which completes) and pass 2 (which hangs). So the exit condition is
normal (loop exits at ebx>0x10), NOT corrupt, and the hang is invisible in the
helper's own loop counters.

Paradox: on pass 2, HalpIommuInitSystem is re-dispatched thousands of times
always at ebx=9, yet helperentry=0 (function not re-entered) and ebx never
changes. Normal control flow cannot reset the inner loop without bumping ebx
or re-entering HalpInitSystemHelper -- neither happens. Therefore something
OUTSIDE the loop's normal control flow keeps redirecting execution back into
it. This fits the HIGH_LEVEL (IRQL 0xF) stall: the pass-2 loop is being
re-driven by an interrupt/exception storm, not malfunctioning on its own terms.
U20: capture the interrupt/exception vector firing during the pass-2 loop.
Evidence: scratchpad/u18_run2.log, u19_run2.log.

## 2026-07-19 update: U20 -- MAJOR reframing, the HAL loop was a detour

Logging edi at HalpInitSystemHelper+0x5A showed edi=0xA, ebx=9 CONSTANT on
BOTH pass 1 (completes) and pass 2 (hangs). Every register measurement
(U18/U19/U20) of the HalpInitSystemHelper / HalpIommuInitSystem loop is
IDENTICAL across the two passes. So that loop is NORMAL boot activity, not
the pass-2 divergence -- the U10-U20 focus on 'why the HAL dispatch loop
stalls on pass 2' was chasing normal behavior. The 3000-cap is hit on both
passes.

The watchdog 'stall' (frozen exitCount, tight HIGH_LEVEL spin at RVA
0x4BE7D0-0x4BE7EA) resolves via PDB (scratchpad/resolve_stall.py) to
HaliHaltSystem+0x0..+0x25 -- the HAL's POST-BUGCHECK CPU halt, the aftermath
of the 0x139 crash, NOT a hang and NOT the cause. Real sequence: pass 2
boots through HAL init much like pass 1, then bugchecks 0x139 around
exitCount ~770563, then parks forever in HaliHaltSystem. The ~283k-exit gap
between the (normal) IOMMU loop and the halt is where the actual crash
lives.

Next: abandon the HAL-loop thread; refocus on the original 0x139 Segment-
Heap crash site (RtlpHpLfhOwnerMoveSubsegment / ExAllocatePoolWithTag, U12)
in the window just before the HaliHaltSystem halt. Evidence: u20_run1.log,
resolve_stall.py.

## 2026-07-19 update: U21 -- both bugchecks captured at KeBugCheckEx

Breakpointed KeBugCheckEx entry (RVA 0x3FD6F0, one-shot per pass, re-armed on
reset) reading rcx=code, rdx/r8/r9=params 1-3, [rsp+0]=caller.

PASS 1: code=0xA5 (ACPI_BIOS_ERROR) p1=0x11 p2=0x3 at exitCount ~212236.
This does NOT trigger the reset -- the port-0x64/0xFE reset is at exitCount
1088868 (~876k exits later; Windows Setup keeps running after the 0xA5 hit).
Pass-1 0xA5's role/fatality is a separate open thread; its [rsp+0] landed in
a no-public-symbol region (inconclusive).

PASS 2: code=0x139 (KERNEL_SECURITY_CHECK_FAILURE) p1=0x3 = LIST_ENTRY
corruption, at exitCount ~1253109. [rsp+0]=KiBugCheckDispatch+0x69 confirms
the __fastfail -> KiBugCheckDispatch -> KeBugCheckEx path. Subcode 3 (a
corrupted doubly-linked list caught by an integrity check) is the classic
Segment Heap free-list corruption signature -- ties directly back to the
original RtlpHpLfhOwnerMoveSubsegment crash-site finding.

Note: hardware exec breakpoints on a genuinely-executed entry re-fault on
resume (no RF handling) and spew thousands of identical duplicate hits; use
one-shot (disarm after first hit) for such sites.

Next (U22): walk up the pass-2 stack from KiBugCheckDispatch to the DETECTOR
(the heap/list routine that raised __fastfail(3)); the 0x139 trap frame (p2)
and exception record (p3) can seed a proper walk. Evidence: u21_run2.log,
resolve3.py.

## 2026-07-19 update: U22 -- pass-2 0x139 crash mechanism fully resolved

For 0x139, KeBugCheckEx(code, p1, p2=trap frame, p3=exception record). Read
EXCEPTION_RECORD.ExceptionAddress (+0x10) and KTRAP_FRAME.Rip (+0x168)/.Rsp
(+0x180), then walked the crashing-thread stack (not KeBugCheckEx's own).
Detector call chain, all ntoskrnl:
  RtlpHpLfhOwnerMoveSubsegment+0xE8 (RVA 0x344214)  <== faulting instruction
    called from RtlpHpLfhBucketGetSubsegment+0x60 (RVA 0x343B64)
      up to RtlpHpAcquireLockShared+0x2A (RVA 0x29F612)

So pass-2 0x139 = Segment Heap LFH (Low-Fragmentation Heap) subsegment
doubly-linked-list corruption: RtlpHpLfhBucketGetSubsegment ->
RtlpHpLfhOwnerMoveSubsegment walks the subsegment LIST_ENTRY list, finds it
corrupted, and __fastfail(3) -> KiBugCheckDispatch+0x69 -> KeBugCheckEx(0x139).
This is the exact RtlpHpLfhOwnerMoveSubsegment+0xE8 site the whole
investigation started from, now proven end-to-end via the proper trap-frame/
exception-record path (not heuristic stack scanning). safecrt_mbtowc/_xmm
frames are nearest-public-symbol noise; the RtlpHpLfh* frames are reliable.

Next (U23, ROOT CAUSE): what corrupts the LFH subsegment LIST_ENTRY on pass 2
but not pass 1 -- a stale/reset-inconsistent heap write, or use-before-init.
Breakpoint the subsegment-list writers or watchpoint the specific LIST_ENTRY
(its address is on the crashing stack via p2/p3). Evidence: u22_run1.log,
resolve4.py.

## 2026-07-19 update: U23 -- root-cause hunt hit a wall (simple techniques exhausted)

Goal: pinpoint what corrupts the LFH subsegment LIST_ENTRY that
RtlpHpLfhOwnerMoveSubsegment detects. Three approaches, all blocked:
  1. Per-call exec BP on RtlpHpLfhOwnerMoveSubsegment entry (read rdx=
     subsegment, validate links) -- sees the entry fine, but the function is
     far too HOT; even pass-2-only, exitCount ballooned to ~24M and the crash
     was never reached (timing distortion). Unworkable.
  2. Fault-time trap frame GPRs -- CLOBBERED: __fastfail does mov ecx,3; int
     0x29, so at the fault rcx=3, rax/rdx/r11=0. Entry unrecoverable. (Rip@
     0x168, Rsp@0x180 in KTRAP_FRAME confirmed working.)
  3. Zero-overhead scan of crashing-thread stack (tfRsp, first 0x200B) for a
     qword pointing to a corrupt LIST_ENTRY -- subsegment not present there;
     shallow-stack ptrs are stack-internal, rdx lives in a non-volatile reg
     the caller saves deeper.

Disasm (disasm_lfh.py): RtlpHpLfhOwnerMoveSubsegment fault at +0xE8 = int 0x29
(ecx=3), reached from 3 LIST_ENTRY back-link checks (+0x4D insert entry=rcx,
+0xAD/+0xB6 remove entry=rdx).

U24 options: (a) proper PDB-unwind of the crashing stack to recover the
caller's saved non-volatile regs (the subsegment); (b) instrument the free/
write path that corrupts the list, not the walk; (c) data watchpoint on the
LFH bucket struct (more stable offset than the per-boot subsegment addr).
Evidence: u23_run*.log, disasm_lfh.py.

## 2026-07-19 update: U24 -- manual frame reconstruction attempted, inconclusive

Disassembled caller RtlpHpLfhBucketGetSubsegment (disasm_bucket.py): sets
rbx=rcx+0x18 (bucket list head), rdx=[rbx], calls RtlpHpLfhOwnerMoveSubsegment
(rcx=rdi, rdx=subsegment, r8=2) at +0x5B (return +0x60). Detector's first insn
is mov [rsp+8],rbx with no rsp change on the fast path, so expected corrupted
subsegment = *(*(tfRsp+8)). But at the crash [tfRsp+8]=0x18 (not a pointer) --
the offset assumption is wrong (int 0x29 trap-frame Rsp vs the saved-rbx slot
don't line up as assumed). Recovery abandoned; the corrupter's exact identity
remains OPEN.

Confirmed: [tfRsp+0]=RtlpHpLfhBucketGetSubsegment+0x60 return addr; KTRAP_FRAME
Rip@0x168, Rsp@0x180.

U25 heavier options: (a) parse PDB/.pdata UNWIND_INFO for a correct stack
unwind to recover the caller's live non-volatile regs; (b) instrument the LFH
FREE/deposit path that writes the subsegment list; (c) hardware data watchpoint
on the subsegment VA once known. Evidence: u24_run1.log, disasm_bucket.py.

## 2026-07-19 update: U25 -- feasible techniques exhausted; corrupter needs heavier tooling

- Crash-state register recovery DEAD: full raw KTRAP_FRAME dump at the 0x139
  (calibrated via rcx=3 @ tf+0x38 -> standard offsets) shows rax@0x30, rdx@
  0x40, r8@0x48, r10@0x58, r11@0x60 ALL ZERO. The int 0x29 fastfail path
  leaves no usable GPRs. (XMM area held an unrelated device-ID string.)
- Broadened crashing-stack scan (0x400, flags garbage links): subsegment not
  on the shallow stack. Manual frame reconstruction (U24): [tfRsp+8]=0x18, not
  the modeled saved rbx -- abandoned.
- Enumerated all RtlpHpLfh* writers (enum_lfh.py): AddSubsegment(0x343DB8),
  SubsegmentFree(0x34421C), SlotAddSubsegment(0x3440B4), BucketGetSubsegment
  (0x343B04), MoveSubsegment(0x34412C).
- DR2 -> RtlpHpLfhBucketAddSubsegment, pass-2 gated: fired 0 times before the
  crash. So the corruption is NOT from an in-window subsegment-add; it happens
  very early on pass 2 (before breakpoints re-arm after reset) or via the free
  path / a stray write.

Real next step (bigger, distinct effort): (a) arm instrumentation from the
FIRST pass-2 instruction (discover module base immediately at/after reset,
before the kernel touches the heap); or (b) hypervisor page-write-protection
on the heap/pool range (WHvMapGpaRange read-only + fault handler) to trap the
corrupting write directly. Per-call BPs on hot LFH funcs and crash-state
inspection are both proven inadequate. Evidence: u25_run*.log, enum_lfh.py.

## 2026-07-19 update: U25c -- corruption is an OUT-OF-BAND STRAY WRITE

DR2 -> RtlpHpLfhSubsegmentFree (unlink path), pass-2 gated: fired 0 times
before the crash, exactly like AddSubsegment. So NEITHER the add nor the free
path runs in the window where the corruption occurs, yet the subsegment
LIST_ENTRY still gets corrupted. This rules out an LFH subsegment-list LOGIC
bug and points at an external write -- a heap overflow / use-after-free from
unrelated code clobbering the adjacent subsegment metadata.

Definitive next tool (approach b): hypervisor page-write-protection on the
heap/pool range -- map the target GPA read-only (WHvMapGpaRange), handle the
MemoryAccess VM exit, log the writing RIP (the corrupter), re-apply the write,
continue. Only tool left that can catch a stray write. Evidence: u25_run4.log.

## 2026-07-23 update: U26 -- fastfail-preamble capture built; register anomaly

Breakpointed RtlpHpLfhOwnerMoveSubsegment+0xE3 (0x34420F, mov ecx,3 before int
0x29), reached ONLY via the failed-check jne's (+0x4D/+0xAD/+0xB6) -> fires
exactly once on the corrupt call, zero overhead. Good instrument. But live regs
there (rip/cr3/rsp all verified valid) read rcx=0 rdx=0 rax=0 r11=0 rdi=0 rbp=0
r8=0 r10=0; rbx=2 (=the r8 flag GetSubsegment passes), rsi=0x10, r9=8, r13=
0x340, r14=0xFF, r12=0xFFFFFFFF. Deterministic. Zero rcx/rdx/rax is IMPOSSIBLE
for the 3 list-check paths (all deref a nonzero rcx/rdx before +0xE3) -- so
either WHP does not expose volatile GPRs at this #DB exit, or live code differs
from the static dump. Same all-zero pattern as the KTRAP_FRAME (U25). [rsp+8]
(saved incoming rbx) small/~0x18, hinting a near-null LFH owner. Subsegment
addr still unrecovered; the register anomaly is the new blocker. Evidence:
u26_run3.log.

## 2026-07-23 update: U26b -- leading hypothesis: use-after-free / premature decommit

Raw stack at MoveSubsegment+0xE3: [rsp+0]=GetSubsegment+0x60, [rsp+8]=0x18,
[rsp+0x10]=0, [rsp+0x18]=0xFFFFFFFFFFFFFFFF, [rsp+0x20]=1. Everything around
the corrupted subsegment is small/sentinel (0x18/0/all-Fs/1), volatile regs
zeroed, derived owner ~null. Signature of the subsegment's LIST_ENTRY having
been ZEROED while still linked -- pages reclaimed/decommitted (candidate
RtlpHpLfhSubsegmentDecommitPages 0x29FFA0) or freed, while still in the
bucket's list; MoveSubsegment walks it, hits the zeroed entry, __fastfails.
Consistent with U25c (out-of-band write). [rsp+8]=0x18 blocks *(*(rsp+8))
recovery so exact subseg addr unconfirmed, but UAF/decommit is now best-
supported. Next: instrument RtlpHpLfhSubsegmentDecommitPages /
RtlpHpLfhSubsegmentFreeBlock (0x29F620) on pass 2 for a still-linked
subsegment being decommitted. Evidence: u26_run4.log.

## 2026-07-24 update: U27 -- tooling restored; decommit ruled out; PURE STRAY WRITE

Restored purged scratchpad tooling: re-fetched ntkrnlmp.pdb from
msdl.microsoft.com (guid+age -> 8547328 bytes) and re-captured ntoskrnl_dump.bin
(16MB) via a new one-shot full-kernel dump in Hypervisor.c (arm block, reads
base..+0x1000000 page-by-page). Both verified by disasm.

Then instrumented RtlpHpLfhSubsegmentDecommitPages (0x29FFA0, rdx=subsegment),
pass-2 gated, checking if the decommitted subsegment is still linked: fired 0
times before the crash -- like Add and Free. So NONE of the LFH structural ops
(add/free/decommit) run in the corruption window. UAF-via-decommit is wrong.
The corruption is a genuine stray write from non-LFH code (heap overflow / wild
pointer) clobbering the subsegment LIST_ENTRY.

Only remaining tool: page-write-protection on the victim GPA (approach b).
Blocker: need the subsegment VA, which MoveSubsegment+0xE3 can't read (WHP
zeroes volatile GPRs at that #DB; [rsp+8]=0x18 blocks frame-walk). Evidence:
u27_run1.log.

## 2026-07-24 update: U28 -- pathology traced to a NULL LFH owner in RtlpHpLfhSlotAllocate

Windowed capture (DR2 at RtlpHpLfhBucketGetSubsegment+0x45 = RVA 0x343B49,
armed only ~40k exits after the pass-2 reset since the site is hot; one-shot
on first null-owner hit) caught the pathology: GetSubsegment is called with
rbx(=owner+0x18)=0x18, i.e. owner=NULL, so subseg=[0x18]=0. This is the real
value behind the rdx=0 'anomaly' at MoveSubsegment+0xE3 -- the subsegment
genuinely IS 0, not a WHP quirk.

Caller chain (from [rsp+0x58]): RtlpHpLfhSlotAllocate+0xCA6 (RVA 0x2382C6, fn
base 0x237620) -> GetSubsegment. Disasm at the call site:
  +0xC94: mov r13,[rbp+0x50]   (valid heap ptr)
  +0xC98: mov rcx,[rbp+0x58]   <-- the OWNER, NULL
  +0xC9C: movzx edx,[r13+0x39]
  +0xCA1: call GetSubsegment
So the corrupted datum is the pointer at [rbp+0x58] in RtlpHpLfhSlotAllocate's
frame, while adjacent [rbp+0x50] is intact -- a precise single-field null.
Under instrumentation this is a tight retry loop (3.4M GetSubsegment(owner=0)
calls); uninstrumented it reaches the 0x139 fastfail.

Next (U29): trace where RtlpHpLfhSlotAllocate sets [rbp+0x58] (walk back from
+0xC98); is it loaded from a corrupted structure field or a stale local? Then
watch that source for the stray write. Evidence: u28_run3.log, disasm_slot.py.

## 2026-07-24 update: U29 -- full chain to a null LFH heap-context in ExAllocateHeapPool

[rbp+0x58] in RtlpHpLfhSlotAllocate is its own arg2 (rdx, homed at entry via
lea rbp,[rsp-8]) -- the null owner is passed IN. Windowed capture at
RtlpHpLfhSlotAllocate entry (RVA 0x237620, rdx=arg2=owner) caught arg2=NULL,
arg1(rcx)=0x340 (a size). Caller ([rsp+0]) = ExAllocateHeapPool+0x2B1 (RVA
0x236CA1, fn base 0x2369F0). Call site: mov rdx,r10; mov rcx,rdi; call
SlotAllocate -- owner=r10, size=rdi. r10 provenance: +0x25C mov r10,[rsp+0x58];
+0x265 mov r10,[r10] => r10 = *(*(rsp+0x58)) = a NULL per-allocation LFH
heap-context pointer.

Full chain: 0x139 fastfail (MoveSubsegment) <- GetSubsegment(owner=0) <-
RtlpHpLfhSlotAllocate(owner=NULL, size~0x340) <- ExAllocateHeapPool passes
rdx=r10=0 where r10=*(*(rsp+0x58)). The null heap-context is the likely root,
and it connects back to the ORIGINAL theme (I3': heap not fully initialized on
pass 2). Next (U30): capture [rsp+0x58] and *[rsp+0x58] at ExAllocateHeapPool
to identify which heap structure/field holds the null context, and whether
uninitialized vs stray-nulled. Evidence: u29_run1.log, rc2.py, rc3.py.

## 2026-07-24 update: U30 -- ExAllocateHeapPool local [rsp+0x58]=0x678 (garbage) yields null context

Breakpointed ExAllocateHeapPool at 0x236C58 (after r10=*(*(rsp+0x58))): caught
r10(context)=0 and [rsp+0x58]=0x678, a small GARBAGE value (not a pointer). So
r10=[rsp+0x58]=0x678, then r10=[0x678]=0. The corrupt datum is the local
[rsp+0x58] holding 0x678 instead of a context-slot pointer. Recurring small
garbage across the chain (0x340 size, 0x678, 0xFF) suggests a structure region
overwritten with size-like small values.

Next (U31): disasm ExAllocateHeapPool for the write to [rsp+0x58] and verify
rsp is stable between that write, the +0x25C load, and 0x236C58 -- reveals what
computes the bad 0x678 (likely a corrupted heap descriptor/PerCpu read).
CAUTION: confirm [rsp+0x58] at 0x236C58 is the same slot loaded at +0x25C.
Evidence: u30_run1.log.

## 2026-07-24 update: U31 -- ROOT CAUSE FOUND (ExPoolState pooldesc +0x10 = NULL); loop closed

[rsp+0x58]=0x678 traced statically: =rax (ExAllocateHeapPool+0x23C), rax=lea
[rdi+idx*8], rdi=r13+0x340. 0x678=0x340+0x338 ==> r13=0. r13=*(poolDesc+0x10)
(+0x129: mov r13,[rcx+rax*8], rax=2 -> +0x10), poolDesc = ExPoolState+0x3900 +
pooltype*0x20c0 (global RVA 0xC57EC0 = ExPoolState+0x3900).

ROOT CAUSE: the ExPoolState pool-descriptor +0x10 field is NULL on pass 2 --
the exact field the 2026-07-17 scan flagged (one writer to +8 =
ExInitializePoolHeapManagement; NO writer to +0x10; found NULL). Full chain:
ExPoolState[pool].+0x10=NULL -> r13=0 -> ExAllocateHeapPool bogus ctx-slot
0x678 -> LFH ctx r10=0 -> RtlpHpLfhSlotAllocate(owner=NULL) ->
GetSubsegment(owner=0)->subseg=0 -> MoveSubsegment(subseg=0) -> LIST_ENTRY
fastfail(3) -> KeBugCheckEx(0x139).

Mechanism is an INITIALIZATION GAP, not a stray write (corrects U25c). +0x10 is
never populated on pass 2 -- reconnects to original I3' (heap not initialized
on pass 2). The pass1-works/pass2-crashes divergence = a pool-heap init step
(populating ExPoolState pooldesc+0x10) runs on pass 1 but is skipped after the
guest mid-install reset. Next (U32): find what writes pooldesc+0x10 (the
skipped initializer) and why the reset skips it -- the fix locus. Evidence:
rc7.py/rc8.py/rc9.py (static), u30_run1.log.

## 2026-07-26 update: U32 -- INIT GAP EMPIRICALLY PROVEN

Milestone dump of ExPoolState pool-descriptor fields, read directly from
kernel memory across both passes:
  PASS 1 @ exit 200k: pool[0] +0x08=0xFFFFE287BFE02000, +0x10=0xFFFF840F82200000 (BOTH populated)
  PASS 2 @ exit 444k (post-reset): pool[0] +0x08=0x0, +0x10=0x0 (BOTH NULL)

So the entire ExPoolState pool-descriptor init runs on pass 1 but is completely
skipped on pass 2. The +0x10 null (crash root) is one symptom of the whole
pool-heap init not running on pass 2. Confirms the init-gap mechanism and ties
back to original I3' (heap/pool managers never initialized on pass 2) -- now
proven with the exact global (ExPoolState), field (+0x10), and full crash chain.

Understanding COMPLETE. Remaining (fix-only, U33): which init step
(ExInitializePoolHeapManagement + its Phase-0/1 caller) is skipped on pass 2
and why the guest reset truncates it. Evidence: u32_run2.log.
