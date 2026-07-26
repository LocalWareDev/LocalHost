# Boot-Divergence Architecture Summary (2026-07-18) — now centered on HalpIommuInitSystem

This is a consolidated, continuously-updated snapshot of the `bugcheck
0x139` investigation. It exists to separate what has been **directly
observed**, what is **inferred** from those observations, and what remains
**unknown**, ahead of deciding the next phase of work. No fix is proposed
here. For the full blow-by-blow evidence trail (log excerpts, disassembly,
scratchpad script listings), see
[post-vppt-boot-stall.md](post-vppt-boot-stall.md); this document only
cites that evidence, it doesn't reproduce it. (Originally titled for
`PspInitPhase0`, where this investigation started — kept as the filename
for stable linking, since the actual divergence has since been traced much
earlier.)

**UPDATE (2026-07-18, U15 — model overturned for pass 2):** direct live
measurement now shows the pass-2 repeated `HalpIommuInitSystem` calls do
**NOT** originate from `HalInitSystem`'s Phase-0 dispatch loop
(`HalpInitSystemHelper`). A DR1 execution breakpoint on
`HalpInitSystemHelper`'s own entry (RVA 0x99E7A8), armed at the correct
pass-2 KASLR base (0xFFFFF8002BC00000, re-arm confirmed in the log),
fired **3001 times on pass 1 but ZERO times on pass 2**, while
`HalpIommuInitSystem` fired 3000+ times (hit its cap) on **both** passes
(all pass-2 calls rcx=9, starting exitCount 483932; reset was 391352). So
on pass 2 the HAL Phase-0 dispatch loop never runs, yet
`HalpIommuInitSystem(rcx=9)` is still invoked thousands of times — from a
*different* caller. The pass-2 stall is observed at **IRQL 0xF
(HIGH_LEVEL)** = interrupt/DPC context, not PASSIVE_LEVEL boot init. This
retires the U10–U13 framing that "a dispatch loop inside `HalInitSystem`
never advances on pass 2": that loop is not the pass-2 culprit, and both
H6 (helper looping internally) and H7 (external re-invocation of the
helper) are ruled out. New open question **U17**: who calls
`HalpIommuInitSystem` repeatedly on pass 2, and how does it tie to the
HIGH_LEVEL interrupt storm and the U12 `KiSwInterruptDispatch`→
`ExAllocatePoolWithTag` chain? The success-every-call and parameter-9
short-circuit facts (I4) remain accurate; the downstream crash mechanism
(fact 27) is unaffected. Evidence: scratchpad/u15_uefi_run1.log.

**PROCESS NOTE:** this VM must be launched with OVMF **UEFI** firmware
(`RELEASEX64_OVMF.fd` as argv[2]) → 3GB guest RAM. Launching with
`bios.bin` gives 1MB RAM and Windows never boots (SeaBIOS POST stall;
AHCI mis-probed as ATAPI → "could not read the boot disk"); those are
wrong-boot-mode symptoms, not real bugs. U15 was blocked for ~an hour by
this mistake before the good log's OVMF line was spotted.

**Current state (2026-07-18, latest — corrected):** the divergence is
pinned to a CFG-guarded, table-driven dispatch loop inside `HalInitSystem`
that repeatedly invokes `HalpIommuInitSystem` and never advances past it
on pass 2 — **but `HalpIommuInitSystem` itself is not failing.** Every
call, on both passes, returns success; the parameter value being used
(`9`) never reaches the function's real IOMMU logic at all (it
short-circuits to a near-no-op return first). The earlier framing
("`HalpIommuInitSystem` never succeeds on pass 2") was wrong and has been
corrected — see I4. The open question is now why the *dispatch loop*
never advances despite every call succeeding, not why IOMMU
detection fails. Every other previously-reported "earliest divergence" in
this document (the missing Phase 1 thread, the uninitialized heap
managers) remains a true downstream symptom of this stalled loop, and the
crash mechanism (an unrelated allocation crashing because the heap was
never initialized, fact 27) is unaffected by this correction. See "Status
after the 2026-07-18 U13 resolution" near the end of this document.

## Established facts

Each of these has direct, live-captured or statically-verified supporting
evidence (breakpoint hit, register/memory dump, or symbol-confirmed
disassembly).

1. **The guest triggers its own mid-install reboot.** A Windows 10
   installer VM, partway through Setup, writes `0xFE` to port `0x64`
   (the standard keyboard-controller reset pulse) — this is guest-issued,
   not hypervisor-injected, and is a normal part of Windows Setup's
   second-stage reboot.
2. **Pass 1 (before the reset) completes without incident.** No bugcheck.
3. **Pass 2 (after the reset) reliably bugchecks with STOP `0x139`**
   (`KERNEL_SECURITY_CHECK_FAILURE`, a Segment Heap corruption guard).
4. **The proximate mechanism is a NULL heap-descriptor read.** Live
   breakpoint captures at the fast-fail site traced the failure backward:
   `RtlpHpLfhOwnerMoveSubsegment(NULL, NULL)` ← `RtlpHpLfhBucketGetSubsegment(NULL)`
   ← `RtlpHpLfhSlotAllocate(heap=NULL)` ← `ExAllocateHeapPool` reading a
   NULL pointer out of `ExPoolState[index]+0x10`.
5. **Nothing writes that memory location during pass 2.** A hardware data
   watchpoint on the exact slot, armed continuously from reset through
   crash, never fired on pass 2. It fires once on pass 1.
6. **The pass-1 writer is `ExInitializePagedHeaps`** (identified from the
   watchpoint's own hit RIP, `+0x84` into that function).
7. **The same pattern holds for the NonPaged counterpart.** The analogous
   slot (`ExPoolState[index]+0x08`, written by `ExInitializePoolHeapManagement`)
   shows an identical fire-once-on-pass-1 / never-on-pass-2 pattern —
   systemic, not specific to the Paged variant.
8. **`Phase1InitializationDiscard` — the real NT system-thread routine that
   performs all of Phase 1 kernel initialization — never executes on pass 2.**
   A hardware execution breakpoint on its entry point, armed continuously
   across all of pass 2, never fired. It fires once on pass 1.
9. **`PspInitPhase0` calls `PsCreateSystemThread` with `Phase1Initialization`
   as the start routine.** Confirmed by static disassembly and symbol
   resolution: the call site is at file offset `+0x86D`, immediately
   preceded by a check that an unrelated, earlier allocation succeeded, and
   immediately followed (`+0x872`) by a check of the call's own `NTSTATUS`
   return value.
10. **Three simultaneous hardware execution breakpoints — on the
    instruction right after that call, on `Phase1Initialization`'s own
    entry, and on `Phase1InitializationDiscard`'s entry — were armed
    together across both passes:**
    - *Pass 1:* all three fired, in order, within 3 VM exits of each
      other; `PsCreateSystemThread` returned `STATUS_SUCCESS`.
    - *Pass 2:* armed immediately after the reset, left armed for
      ~100,000 VM exits (the same order of magnitude pass 1 needed to
      reach and clear all three) through to the bugcheck — **none of the
      three fired.**
11. **`ntoskrnl.exe` loads at a different virtual base on pass 2 than on
    pass 1**, consistent with KASLR re-randomizing a genuinely freshly
    loaded kernel image, not a resumed or stale one.
12. **The pass-2 reset-to-crash window (fact 10's ~100,000 VM exits) is
    dominated by pre-kernel bootloader/firmware activity, not NT-kernel or
    driver-level activity.** Categorizing every log line in that window
    (`categorize_windows.py`, scratchpad) shows: `fwcfg` config-selector
    replay, a `ramfb`/`cmos-ab` reconfiguration, `BdsDxe` loading and
    starting `Boot0002` (the disk boot option), then a stretch of roughly
    57,000 VM exits (wall-clock ~24.4s–26.6s into the run) repeatedly
    touching a single I/O port (`0xB008`) with an unchanging value —
    consistent with a bootloader-level poll/wait loop, not varied kernel
    activity. **No `[pcicfg]`/`[ahci]`/`[ioapic]` device-enumeration
    traffic recurs at all** in this window, versus 300/202/100 such lines
    respectively in pass 1's comparably-sized early-boot window.
13. **The hypervisor's own live kernel-module identification (a backward
    MZ/PE scan that reads and verifies the loaded image's PE debug
    directory — PDB name/GUID — against the real `ntoskrnl.exe`) succeeds
    at essentially the same VM-exit position where the guest's exit count
    permanently stops advancing.** Both fall within the same ~823-exit span
    between the last heartbeat tick (exitCount 670000) and the first
    stall detection (exitCount 670823, unchanging thereafter).
14. **The frozen RIP resolves to `HaliHaltSystem+0x12`** — the HAL routine
    `KeBugCheckEx` transfers control to once bugcheck processing completes
    and IRQL is raised to `HIGH_LEVEL`. This independently confirms (apart
    from the earlier direct `KiBugCheckData` memory scan) that the freeze
    is the normal post-bugcheck halt state, not a live stall or
    synchronization wait.
15. **Neither `ExpInitSystemPhase0` nor `ExpInitSystemPhase1`** (`ExInitSystem`'s
    own two-phase dispatch targets — `ExInitSystem` itself is called once,
    early, from `InitBootProcessor`, well before `PspInitPhase0`'s thread
    creation) **directly calls `ExInitializePagedHeaps` or
    `ExInitializePoolHeapManagement`**, confirmed by full disassembly of
    both. `ExpInitSystemPhase0`'s one heap-adjacent call,
    `RtlHpGlobalsInitialize`, was itself fully disassembled (0x4F bytes)
    and only seeds RNG state used elsewhere in the heap subsystem — ruled
    out as the caller.
16. **U6 resolved directly: the actual callers are `MiInitNucleus` and
    `MiInitSystem`, not anything in the Phase 1 tree.** Two hardware
    execution breakpoints at the two initializers' own entry points, live
    on pass 1, captured the genuine return address at `[rsp+0x00]` on
    each hit (reliable regardless of how the call was made — every x86
    `CALL` form pushes identically):
    - `ExInitializePoolHeapManagement`'s caller resolves to
      `MiInitNucleus+0x44B`.
    - `ExInitializePagedHeaps`'s caller resolves to `MiInitSystem+0x8F`.
17. **Both `MiInitNucleus` and `MiInitSystem` are called directly from
    `MmInitSystem`** (confirmed by full disassembly of `MmInitSystem`:
    it calls `MiInitSystem` twice and `MiInitNucleus` once, all as direct,
    unconditional calls from its own body — no intermediate function).
18. **`InitBootProcessor` calls `MmInitSystem` at its own file offset
    `+0x575`, chronologically before its call to `PsInitSystem` at
    `+0x969`** (the call that, several levels down, leads to
    `PspInitPhase0`'s `PsCreateSystemThread` call). Confirmed two ways:
    statically, by the two calls' relative position in `InitBootProcessor`'s
    linear disassembly; and live, in the same pass-1 capture that found
    fact 16 — both heap-initializer hits fire ~30–35 VM exits *before*
    `Phase1InitializationDiscard`'s own entry is reached, which itself
    requires the Phase 1 thread to already exist and be scheduled.
19. **U8 resolved: `MmInitSystem` is not reached on pass 2 either.** Three
    hardware execution breakpoints — `InitBootProcessor`'s own entry,
    `MmInitSystem`'s own entry, and `PspInitPhase0`'s own entry — were
    armed together across both passes. *Pass 1:* `MmInitSystem` and
    `PspInitPhase0` both fired cleanly, 151 VM exits apart
    (`InitBootProcessor`'s own entry did not fire — explained below, not
    a contradiction). *Pass 2:* armed immediately after the reset;
    **none of the three fired**, all the way through to the bugcheck. The
    same back-to-back kernel-entry/freeze pattern from facts 12–14
    reproduced in this independent run: bootloader replay, a long
    single-port poll, kernel module identified, then the exit count froze
    within the same watchdog tick. The frozen RIP resolved to the same
    `HaliHaltSystem+0x12` as before (RVA `0x4BE7E2`, matching exactly).
    `InitBootProcessor`'s own entry not firing on *either* pass is a known
    artifact of this method, not new evidence: breakpoints arm
    opportunistically on the first VM exit with canonical RIP, which on
    pass 1 evidently landed just after `InitBootProcessor`'s own first
    instruction but before its call to `MmInitSystem` (151-VM-exit
    positive controls on both sides bracket it) — so this specific
    checkpoint is inherently unreliable and shouldn't be read either way.
20. **The pass-2 discovery mechanism's own RIP, at the moment it first
    identified the kernel module, resolved to `HalpApicInitializeIoUnit+0x5D`
    — identically, across two independent runs with different KASLR
    bases.** This is a genuine, legitimate, early HAL routine (I/O APIC
    initialization), not the eventual post-bugcheck halt loop — confirming
    discovery catches real early pass-2 execution, not just whatever RIP
    happens to be visible once already frozen. `InitBootProcessor`'s own
    resolved call list places `HalInitSystem` (which
    `HalpApicInitializeIoUnit` is presumably called from) at file offset
    `+0x312` — before `MmInitSystem`'s `+0x575`. So pass 2 is confirmed to
    reach at least `InitBootProcessor+0x312`'s subtree.
21. **U9 bisection: `CmInitSystem0` (`+0x348`) and `KeInitSystem`
    (`+0x370`) are both never reached on pass 2 either**, narrowing the
    bracket to a 54-byte stretch of `InitBootProcessor`'s own linear code
    (`+0x312` reached, `+0x348` not). Confirmed the same way: both fired
    cleanly on pass 1 (positive controls), neither fired on pass 2 despite
    the reset-to-bugcheck window being armed throughout.
22. **Disassembling that 54-byte stretch found a direct conditional
    branch on `HalInitSystem`'s own return value** —
    `call HalInitSystem; test al,al; je <bailout>` — where the bailout
    target loads bugcheck code `0x5C` (`HAL_INITIALIZATION_FAILED`) before
    its first call, suggesting an immediate bugcheck if taken. **Directly
    tested and falsified as the mechanism**, 2026-07-18: a breakpoint at
    `InitBootProcessor+0x317` (the `test al,al` instruction itself, so AL
    holds `HalInitSystem`'s raw return value) fired cleanly on pass 1
    (AL=1, `TRUE`/success) but **never fired at all on pass 2** — not
    once, despite the reset-to-bugcheck window being armed throughout.
    This means `HalInitSystem`'s call doesn't merely return `FALSE` on
    pass 2; **it never returns to its caller at all**, in any form this
    instrumentation can observe. Combined with fact 20 (pass 2 does reach
    `HalpApicInitializeIoUnit`, inside `HalInitSystem`'s own subtree), the
    divergence is now bracketed to *within* `HalInitSystem`'s own
    execution, sometime after `HalpApicInitializeIoUnit` runs.
23. **U10, static: `HalInitSystem` is itself a tiny Phase-0/Phase-1
    dispatcher** (52 bytes, mirroring `PsInitSystem`/`ExInitSystem`'s
    pattern), calling `HalpInitSystemPhase0`/`HalpInitSystemPhase1`, both
    of which call a single shared `HalpInitSystemHelper`.
    `HalpInitSystemHelper` (141 bytes) makes exactly **one** call: to
    `guard_dispatch_icall`, Control Flow Guard's indirect-call trampoline
    — confirming this is a table-driven dispatch loop over per-subsystem
    HAL init routines (standard real-HAL architecture), not a direct call
    chain. This explains why the much earlier whole-image static scan
    (U6's first attempt) found no direct references to anything: a
    CFG-guarded indirect call doesn't leave a literal `call rel32` or
    matching address load at the call site.
24. **U10, live, decisive: a multi-shot breakpoint at the CFG-dispatch
    call site, capturing RAX (the dispatch target, per the standard
    MSVC/CFG x64 ABI) on every hit, shows the divergence directly.**
    - *Pass 1:* the dispatch target starts at `HalpIommuInitSystem`
      (RVA `0x9A16B0`), repeats identically (same RSP, meaning the same
      stack depth — the same call site being re-invoked, not new code)
      for the full 3000-hit capture window, then — after re-arming, which
      requires `InitBootProcessor+0x317`/`KeInitializeClock`/`MmInitSystem`
      to have all fired in between — the target changes to
      `HalpAcpiInitSystem` (RVA `0x9A2BD0`), again repeating for a full
      3000-hit window. The dispatch target changing at all, combined with
      the known-negative anchors having fired, proves `HalpIommuInitSystem`
      eventually terminates and `HalInitSystem` eventually returns on
      pass 1.
    - *Pass 2:* armed immediately after the reset. All 3000 captured hits
      are `HalpIommuInitSystem` (RVA `0x9A16B0`) with the **identical
      RSP** throughout — no change, no advance to a second subsystem —
      right up to the bugcheck. The dispatch never resolves.
25. **`HalpIommuInitSystem`'s own body (Intel VT-d / DMAR IOMMU bring-up)
    is a substantial, mostly-linear sequence** —
    `HalpIommuInitializeDmar`, `IommuInitializeLibrary`,
    `HalpIommuInitInterrupts`, `HalpIommuProcessReservations`,
    `HalpIommuInitializeAll`, and others — each call gated by a
    conditional bailout (`js`/`je` to a shared failure path at `+0x1879`)
    if it fails. No single obvious spin instruction inside the function
    itself; the repeated identical-RSP dispatch hits are consistent with
    the *caller* of `HalpInitSystemHelper` re-invoking it for the same
    table entry, not a tight loop inside `HalpIommuInitSystem`.
26. **U12: a live breakpoint at the exact faulting instruction
    (`RtlpHpLfhOwnerMoveSubsegment+0xE8`) with a deep (1024-byte)
    heuristic stack scan extended the known allocator chain by one level —
    `ExAllocatePoolWithTag+0x64` — but did not reliably identify
    `ExAllocatePoolWithTag`'s own caller.** Several higher-offset entries
    resolved to plausible-looking HAL interrupt-registration functions
    (`HalpInterruptRegisterController`, `HalpInterruptRegisterLine`,
    `HalpPicInitializeIoUnit`), but checking their own disassembly showed
    none of them call `ExAllocatePoolWithTag` (they call a different
    function, `HalpMmAllocateMemoryInternal`, which itself makes no
    further calls) — ruling them out as the *direct* caller and marking
    them as more likely stale stack data than live call-chain members.
    This heuristic method (scanning for any canonical-looking stack qword)
    lacks the precision of real unwind-based stack walking.
27. **U12, more reliable: a multi-shot breakpoint at `ExAllocatePoolWithTag`'s
    own entry, reading the guaranteed-real return address at `[rsp+0x00]`
    on every call (same proven technique as U6), traced the actual caller
    sequence directly.**
    - *Pass 1 (first 200 captured calls, early in boot):* every single
      call comes from `PsInitializeQuotaSystem+0x8F` — ordinary,
      expected early-boot Process-subsystem initialization.
    - *Pass 2 (200 captured calls immediately preceding the bugcheck):*
      every single call comes from `KiSwInterruptDispatch+0x91` — generic
      software-interrupt (DPC/APC-level) dispatch code, not anything
      IOMMU-specific and not part of the `InitBootProcessor` boot
      sequence this investigation has been tracing. This pattern starts
      immediately where the DR3 HAL-dispatch tracer's own 3000-hit cap
      ends in the same capture, which is best explained by DR3
      monopolizing every VM exit while armed (only one hardware
      breakpoint can fire per exit) rather than a causal link between the
      two loops.
28. **U13, static: `HalpInitSystemHelper`'s full body (not just its one
    visible `call guard_dispatch_icall`) is a genuine nested loop** —
    outer index from `ecx`/`edx` arguments, inner index 0–20 (21 slots)
    over a table (`HalSubComponents`, stride 0x10 bytes) — that calls a
    per-slot function pointer via CFG dispatch, advancing to the next slot
    on success (`test eax,eax; js <bail>` — only a *negative* return value
    aborts the whole function; a non-negative return continues the loop
    normally). This was missed in the earlier (U10) scan because that scan
    only listed distinct `call` *instructions* found by a linear
    disassembly pass, not runtime iteration counts — the loop body
    contains exactly one `call guard_dispatch_icall`, so it was correctly
    reported as "one call," which is true statically but doesn't reveal
    that the same instruction executes many times at runtime.
29. **U13, live: `HalpIommuInitSystem` is called with identical arguments
    (`rcx=0x9, rdx=0x0, r9=0x1`, `r8` a stable per-pass pointer) and
    returns `EAX=0` (non-negative) on *every single call*, on both
    passes.** A multi-shot breakpoint at its own entry (capturing
    RCX/RDX/R8/R9) and a second at its own return point (capturing EAX)
    confirmed this directly — not a heuristic reading. The apparent
    "entry-only, then return-only" pattern in the raw log is an artifact
    of the two breakpoints having different hit caps (200 vs 3000), not
    evidence the function fails to return; merging and sorting both
    traces by `exitCount` shows a clean, uninterrupted alternation once
    both are read together.
30. **U13, static: parameter value `9` (the observed `rcx`) does not
    reach `HalpIommuInitSystem`'s substantive IOMMU logic at all.**
    Full, unfiltered disassembly of the function (verified with precise
    offset arithmetic, not estimation) shows its first parameter (`edi`,
    copied from `ecx`) is checked against `8` very early
    (`+0x37: cmp edi,8; jne +0x56`); since `9 != 8`, execution jumps
    directly to `+0x56: xor eax,eax; jmp +0x1CB` — an almost-immediate
    return of `0`/success that never reaches the real DMAR/interrupt/library
    initialization code (`HalpIommuInitializeDmar`,
    `IommuInitializeLibrary`, `HalpIommuInitInterrupts`), which are gated
    behind a *different* switch reachable only for `edi` values `0x11`,
    `0x13`, `0x20`, or `0`. **The "success" observed in fact 29 does not
    represent successful hardware/ACPI detection — it's a near-no-op
    return for a parameter value the function doesn't have specific
    handling for.**

## Inferences

These follow from the facts above but are not themselves things a
breakpoint directly witnessed. Flagged explicitly so they aren't mistaken
for observations.

- **I1 — The `PsCreateSystemThread` call is never reached on pass 2.**
  Because the breakpoint sitting one instruction after the call (fact 10)
  never fires, `PspInitPhase0` does not get there in any form the current
  instrumentation can see. The alternative reading — the call executes but
  something prevents the return from ever being observed — has no
  supporting evidence (no intervening exception was seen) and is treated
  as the less likely explanation, but it hasn't been positively excluded.
- **I2 — The Phase 1 system thread is never created**, following directly
  from I1: no successful `PsCreateSystemThread` call means no thread, which
  is consistent with `Phase1Initialization`'s and
  `Phase1InitializationDiscard`'s entries also never firing. **Still true,
  but see I3′ below — this is no longer believed to be the cause of the
  heap-manager failures.**
- **I3 — REFUTED, 2026-07-18 (was: "the two heap-manager failures are
  downstream consequences of I2").** Facts 16–18 directly identify the
  real callers — `MiInitNucleus` and `MiInitSystem`, both reached via
  `MmInitSystem`, called from `InitBootProcessor` *before* its call to
  `PsInitSystem`/`PspInitPhase0`. The heap initializers do not depend on
  the Phase 1 thread at all; they run earlier, entirely within Phase 0, on
  the original boot-processor thread. The general-NT-architecture
  assumption I3 rested on was wrong for this specific pair of functions.
- **I3′ (replaces I3) — The heap-manager failures and the missing Phase 1
  thread are two symptoms of the same earlier cause, not a cause-and-effect
  pair.** Since `MmInitSystem` (and therefore the heap initializers) is
  reached *before* `PsInitSystem` in `InitBootProcessor`'s own sequence,
  and neither the heap initializers (facts 5, 7) nor the
  `PsCreateSystemThread` call (fact 10) ever fire on pass 2, the most
  parsimonious explanation is a single divergence at or before
  `MmInitSystem`'s own execution that prevents *both* downstream outcomes
  — not a chain where the missing thread causes the missing heap init.
  **Directly tested and confirmed, 2026-07-18 (fact 19): `MmInitSystem`
  itself is never reached on pass 2 either.** I3′ moves from "the natural
  reading of the evidence" to a directly-tested result.
- **I4 — CORRECTED, 2026-07-18: "`HalpIommuInitSystem` never succeeds" was
  wrong. It succeeds (trivially) on every single call, on both passes —
  the loop that dispatches to it is what never advances.** Facts 29–30
  directly overturn the previous wording of this inference: the observed
  repeated calls all use a parameter value (`9`) that skips
  `HalpIommuInitSystem`'s real IOMMU logic entirely and returns success
  almost immediately (fact 30) — so "success" here never meant "hardware
  responded correctly," and "the retry loop" was never actually retrying
  a *failing* operation. What's actually true, still solidly established:
  `HalInitSystem` never returns to its caller on pass 2 (fact 22), and the
  same dispatch target repeats with no observed advancement (fact 24) —
  but *why* is now a different, more specific question: not "why does
  IOMMU init fail," but "why does whatever drives this dispatch loop keep
  re-invoking the same (trivially-succeeding) slot instead of advancing to
  the next one, on pass 2 specifically." This is still the earliest and
  most specific divergence point found in this investigation — every
  previously-reported "earliest divergence" was a true but downstream
  symptom of this single point — but the *mechanism* is now understood to
  be a loop-control/dispatch-advancement issue, not a hardware-detection
  failure. See U15.
- **I7 — REFINED, 2026-07-18: indirect rather than direct — the crash
  isn't `HalpIommuInitSystem`'s own allocation, but the retry loop still
  causes it.** Facts 26–27 directly checked the crash's live call stack
  and found the immediate caller of `ExAllocatePoolWithTag` right before
  the bugcheck is `KiSwInterruptDispatch+0x91` — generic software-interrupt
  dispatch code, not anything in `HalpIommuInitSystem`'s own call tree
  (`HalpIommuInitializeDmar`, `IommuInitializeLibrary`, etc., the original
  speculation). The revised reading: `HalpIommuInitSystem`'s retry loop
  doesn't need to allocate pool memory itself to cause the crash — it only
  needs to prevent `HalInitSystem` from ever returning, which prevents
  `MmInitSystem` from ever initializing the Segment Heap descriptors
  (I3′), which leaves *every* pool allocation attempt during that entire
  window vulnerable — including ones with no relationship to IOMMU at all,
  like this software-interrupt dispatch path. Pass 1's equivalent
  allocations (fact 27, `PsInitializeQuotaSystem`) succeed simply because
  `MmInitSystem` has already run by the time they happen; pass 2's don't,
  because it never does. This still connects the retry loop to the
  bugcheck causally, just one level more indirectly than originally
  speculated — via a shared precondition (heap not initialized), not a
  shared code path.
- **I5 — Pass 2 does not perform substantial NT-kernel or driver-level
  work after the kernel loads (revises the assumption U5 was originally
  built on).** Facts 12–14 show the bulk of pass 2's ~100,000-VM-exit
  runway is pre-kernel bootloader/firmware activity; genuine kernel-space
  execution is only confirmed within a short span (≤823 VM exits) of the
  freeze, with the three-way breakpoints already confirmed armed
  throughout that span and still never firing. There's no observed
  "substantial work" left to explain — the original U5 framing assumed a
  large exit count implied deep Setup/driver activity, which the actual
  trace content doesn't support.
- **I6 — The crash most likely occurs very early in kernel bring-up on
  pass 2 (revised, weaker claim than before).** Facts 12–14 still support
  "very early," but I3′ means "at or near the Phase 0/Phase 1 boundary"
  overstates what's known — the divergence could be as early as
  `MmInitSystem`, well inside Phase 0, not necessarily near the boundary
  with Phase 1.

## Remaining unknowns

- **U1 — Exact divergence location (narrowed but not resolved).** Was
  "before `PspInitPhase0+0x86D`"; per I3′/I4, now believed to be at or
  before `MmInitSystem` (`InitBootProcessor+0x575`), earlier in
  `InitBootProcessor`'s own sequence than previously thought — but this
  hasn't been directly tested (see U8), so `PspInitPhase0+0x86D` remains
  the *latest confirmed point* something is known to be wrong by, not the
  location itself.
- **U2 — Divergence mechanism.** Nothing currently points at a specific
  cause — e.g., whether it's a conditional branch keyed on some piece of
  CPU/memory state that differs between passes, or something else
  entirely.
- **U3 — Whether `PspInitPhase0` is even invoked at all on pass 2**, versus
  invoked and diverging partway through. All instrumentation so far starts
  *inside* `PspInitPhase0`'s body; nothing upstream of it (`PsInitSystem`,
  `InitBootProcessor`, `KiInitializeKernel`) has been directly watched.
  **Superseded in priority by U8**, which asks the same style of question
  at the newly-identified, likely-earlier `MmInitSystem` checkpoint.
- **U4 — A methodological gap shared by every experiment in this
  investigation, not just this update.** Breakpoints arm opportunistically,
  gated on the hypervisor's main loop observing a VM exit at a canonical
  kernel-space RIP. Between any two VM exits the guest can run an arbitrary
  amount of unobserved code. If the entire sequence from kernel entry
  through `Phase1InitializationDiscard` could execute in one uninterrupted
  burst with zero intervening VM exits, arming would happen too late and
  every "never fires" result in this document would be a false negative.
  This has **not** been directly ruled out in general. It's judged unlikely
  — early NT boot is normally rich in I/O-port traffic (PIC/PIT/RTC/ACPI
  programming) that reliably causes VM exits, and pass 1 itself needed
  ~103,000 VM exits to reach the equivalent point — but "unlikely by
  pattern" isn't the same as "excluded by direct evidence." **Partial,
  case-specific reassurance, 2026-07-18:** for the final stretch of the
  captured run, fact 13 shows arming succeeded within ≤823 VM exits of the
  earliest confirmed kernel-space activity, and the freeze itself (facts
  13–14) happens *after* arming, while all three breakpoints are
  confirmed live — so for this run, the "never fires" result isn't
  explained by an unmonitored gap at the end, at least. This doesn't rule
  out a gap earlier in the sequence, and per the user's own read, isn't
  being treated as the leading explanation absent evidence of unusually
  long uninterrupted guest runs on pass 2 versus pass 1 — which hasn't
  been found.
- **U5 — Substantially addressed, 2026-07-18 (see I5/I6).** The original
  framing assumed pass 2's large VM-exit count implied it was reaching
  substantial Setup/driver-level activity despite Phase 1 apparently never
  running, which would have been a real contradiction. Directly examining
  the trace content (facts 12–14) doesn't support that assumption: most of
  the runway is pre-kernel bootloader activity, and kernel-space execution
  is only confirmed very close to the freeze. The apparent tension was
  built on an unexamined assumption about what a large exit count implies,
  not on a genuine conflict between two established facts. **Caveats:**
  originally based on a single captured run; the U8 capture (fact 19)
  independently reproduced the same bootloader-replay / single-port-poll /
  back-to-back kernel-entry-and-freeze pattern (including the identical
  frozen RVA), which is reassuring corroboration, though the specific
  ~57,000-exit port-`0xB008` poll and ~3.3-second slowdown are still not
  individually explained (plausibly ordinary bootloader disk-read waiting,
  but not confirmed) — see U7.
- **U6 — RESOLVED, 2026-07-18 (see facts 16–18, I3′).** The actual callers
  are `MiInitNucleus` and `MiInitSystem`, both reached via `MmInitSystem`
  from `InitBootProcessor`, entirely within Phase 0 — not the `Mm`
  subsystem's own guess-worthy alternative, but confirmed by direct live
  capture of the real return address at each function's entry. This
  refuted I3 rather than confirming it.
- **U7 — The two unexplained activity stretches in pass 2's bootloader
  window** (fact 12: the ~57,000-exit port-`0xB008` poll, and the
  ~3.3-second slowdown around exitCount 650000) haven't been individually
  characterized. Plausibly ordinary — disk I/O wait and a slower read,
  respectively — but not confirmed, and not compared against whether
  pass 1's *own* equivalent bootloader activity (before its first kernel
  entry) shows the same pattern. Lower priority than U8.
- **U8 — RESOLVED, 2026-07-18 (see fact 19, I4).** `MmInitSystem` is never
  reached on pass 2. The earliest confirmed divergence moves from
  `PspInitPhase0+0x86D` back to `InitBootProcessor+0x575`
  (`MmInitSystem`'s call site) or earlier. Superseded by **U9**.
- **U9 — RESOLVED, 2026-07-18 (see facts 20–22, I4).** Bisected in two
  rounds: `CmInitSystem0`/`KeInitSystem` (`+0x348`/`+0x370`) never fire on
  pass 2, while `HalInitSystem`'s subtree (`+0x312`) is confirmed reached
  via `HalpApicInitializeIoUnit`. The apparent answer — a conditional
  branch on `HalInitSystem`'s own return value — was directly tested and
  **falsified**: the return-value check point never fires either, meaning
  `HalInitSystem` never returns to its caller at all on pass 2, not just
  "returns failure." Superseded by **U10**.
- **U10 — RESOLVED, 2026-07-18 (see facts 23–25, I4).** `HalInitSystem`
  dispatches through a CFG-guarded, table-driven loop
  (`HalpInitSystemHelper`); a multi-shot breakpoint at the dispatch call
  site traced it directly. `HalpIommuInitSystem` is the specific
  subsystem: pass 1's dispatch loop eventually advances past it to a
  second subsystem (`HalpAcpiInitSystem`); pass 2's captured 3000 hits
  never advance, same target, same stack depth, throughout. This is the
  earliest and most specific divergence point found in this investigation
  so far.
- **U12 — RESOLVED (refined answer), 2026-07-18 (see facts 26–27, I7).**
  The crash's live call stack does *not* run back through
  `HalpIommuInitSystem`'s own callees — the immediate caller of
  `ExAllocatePoolWithTag` right before the bugcheck is
  `KiSwInterruptDispatch+0x91`, unrelated software-interrupt dispatch
  code. I7 is confirmed in a weaker, indirect form: the retry loop causes
  the crash by leaving the heap permanently uninitialized, not by making
  the fatal allocation itself.
- **U14 (new) — Is `KiSwInterruptDispatch+0x91`'s repeated
  `ExAllocatePoolWithTag` call the guest's own periodic-timer-interrupt
  DPC handler** (this hypervisor is already known, from much earlier in
  this investigation, to deliver periodic RTC/PIT interrupts to the guest
  continuously) **, and does the identical call site also appear on pass 1
  once `MmInitSystem` has already run** (which would confirm it's an
  entirely ordinary, routine allocation whose only problem on pass 2 is
  *timing* — happening during the vulnerability window — not anything
  unusual about the call itself)? Lower priority than U13 — doesn't change
  the causal model either way, just confirms/fills in a detail of it.
- **U13 — SUPERSEDED, 2026-07-18 (see facts 28–30, corrected I4).**
  Originally framed as "why does `HalpIommuInitSystem` fail to make
  progress" — resolved in a way that dissolves the original framing
  rather than answering it: it doesn't fail, it trivially no-ops on every
  call (parameter `9` doesn't reach real IOMMU logic). ACPI DMAR
  state/CPUID/MSR checks (the original candidates) are no longer
  plausible causes, since the calls we're observing never reach the code
  that would check any of those. Replaced by **U15** and **U16**.
- **U15 (new, highest priority) — What actually drives the repeated
  dispatch, and why does it stop advancing on pass 2?**
  `HalpInitSystemHelper`'s own loop logic (fact 28) should advance to the
  next table slot after any non-negative return — which every observed
  `HalpIommuInitSystem` call produces (fact 29) — yet no advancement is
  observed on pass 2. Since `HalpInitSystemHelper`, `HalpInitSystemPhase0`/
  `Phase1`, and `HalInitSystem` itself are all confirmed non-looping by
  their own disassembly, the repeated invocation must originate from
  somewhere not yet identified: either a caller of `HalInitSystem` beyond
  `InitBootProcessor`'s single confirmed call site (not yet searched for
  exhaustively), or a misread of the loop mechanics that a targeted live
  capture (e.g., breakpointing `HalpInitSystemHelper`'s own inner-loop
  increment at `+0x5A`, or its own entry, to capture `ebx`/`edi` directly
  rather than inferring them) would resolve directly.
- **U16 (new) — Why is parameter `9` specifically what's being passed,
  on both passes, unchanged?** If `9` is simply the correct, intended
  table index for whatever `HalpIommuInitSystem`'s "slot 9" represents on
  this specific platform (as opposed to a corrupted/wrong value), then
  the trivial-no-op behavior for `9` might be entirely by design — real
  Windows may simply not use this particular slot for meaningful IOMMU
  work on this hardware profile, and the *actual* divergence is purely
  about loop advancement (U15), unrelated to what value `9` represents.
  Lower priority than U15 — worth revisiting only if U15's answer doesn't
  fully explain the stall on its own.
- **U11 — Still open, lower priority.** Does whatever repeats retry
  *forever* on pass 2, or just for a very long but finite time? Not
  critical to resolve before U15 — either way, the practical effect
  (heap never initialized before the guest's own allocations crash it) is
  the same.

## Candidate hypotheses for why the dispatch loop never advances on pass 2

**H1/H2/H5 below are now considered unlikely, kept for history rather than
as live candidates.** They were all built on "IOMMU init fails because it
can't get a straight answer from hardware/ACPI/CPUID" — facts 28–30 show
the observed calls never reach any code that checks hardware, ACPI, CPUID,
or MSR state at all (parameter `9` short-circuits before any of that).
Whatever's wrong is upstream of `HalpIommuInitSystem`'s own logic, in the
loop/dispatch mechanism itself (U15). None of H1–H5 have ever had direct
supporting evidence; they're kept organized here for reference, not as
findings.

- **H1 — Reset-fidelity gap in CPU/APIC/timer/DMA-remapping state.**
  Likely obsolete in its original form (no hardware state is being read by
  the calls observed) — could still apply if reframed around whatever
  state actually drives the *loop's* advancement decision, but that's not
  yet identified.
- **H2 — Stale host-side state surviving the reset is misread as
  "already initialized."** Same status as H1 — plausible in general shape,
  not yet connected to a specific piece of state given facts 28–30.
- **H3 — An early-exit/degraded-boot branch unrelated to reset fidelity.**
  Unlikely — the loop repeats rather than short-circuiting once.
- **H4 — RETIRED, 2026-07-18.** Superseded by I3′/U6's direct resolution.
- **H5 — Likely obsolete.** Built on "IOMMU hardware claims to exist but
  isn't responding" — facts 28–30 show the observed calls don't check for
  IOMMU hardware at all (parameter `9` skips that logic entirely).
- **H6 (new) — A caller-side state variable (not IOMMU/hardware-related)
  that controls loop advancement is itself corrupted, uninitialized, or
  reset-inconsistently by the time this loop runs on pass 2.** This is the
  most natural reframing of H1/H2 given facts 28–30: something (perhaps
  the outer loop bound `ebp`/`edx`-argument, or a shared counter/flag
  `HalpInitSystemHelper` or its caller relies on) fails to advance for
  reasons unrelated to IOMMU specifically. Directly testable via U15's
  proposed live capture of `HalpInitSystemHelper`'s own loop-control
  registers.
- **H7 (new) — There is no single "loop" at all; `HalInitSystem` (or
  something above it) is being called repeatedly by a genuinely separate
  mechanism** (a retry-until-ready DPC/timer, or multiple call sites not
  yet found), and each fresh invocation happens to reach the same
  parameter-9 slot first. Would explain the observed pattern equally well
  without requiring `HalpInitSystemHelper`'s own inner loop to be stuck.
  Distinguishable from H6 by U15's proposed capture: H6 predicts
  `HalpInitSystemHelper`'s own `edi`/`ebx` never change across hits; H7
  predicts `HalpInitSystemHelper` itself gets freshly entered each time.

## Status after the 2026-07-18 U12 resolution

Eight unknowns resolved today (U6, U8, U9, U10, U12, U13, in sequence —
plus U15/U16 opened to replace U13), each pushing the model earlier and
tighter, and the last round corrected a mischaracterization rather than
just narrowing further:

**A CFG-guarded, table-driven dispatch loop inside `HalInitSystem`
(called early in `InitBootProcessor`'s own Phase-0 sequence) repeatedly
invokes `HalpIommuInitSystem` with the same parameters, and never advances
to the next table entry on pass 2 — but, corrected this round,
`HalpIommuInitSystem` is not failing.** Every observed call, on both
passes, returns success. The parameter value being passed (`9`) doesn't
reach the function's real IOMMU/DMAR logic at all — full disassembly shows
it short-circuits to a near-no-op return before any hardware, ACPI, or
CPUID check happens. So the original framing ("why does IOMMU init fail
on pass 2") was answered by dissolving it: nothing about IOMMU is
failing. The real, still-open question is why the *dispatch loop itself*
never advances past this slot on pass 2, despite every call succeeding —
a loop-control question, not a hardware-detection one. This is still the
earliest and most specific divergence point found in this investigation;
it's why `HalInitSystem` never returns, why `MmInitSystem` never runs, and
why the Phase 1 thread never gets created — but *why the loop stalls* is
now a different, more specific question than before.

**The crash itself remains understood precisely, and independent of
today's correction.** A live breakpoint at `ExAllocatePoolWithTag`'s own
entry showed the caller immediately before the bugcheck is
`KiSwInterruptDispatch+0x91` — ordinary software-interrupt dispatch code.
The mechanism is still: the stalled loop prevents `MmInitSystem` from ever
initializing the Segment Heap, and *any* allocation that happens to run
during that unbounded window crashes, regardless of what it's for. This
part of the model doesn't depend on whether `HalpIommuInitSystem`'s own
calls are meaningful or trivial.

Three things remain well-supported regardless of today's correction:
(1) single cause, not a chain; (2) it happens early and consistently
across independent captures; (3) the crash is a consequence of *timing*
(allocating during an unbounded vulnerability window), not of any specific
allocation being wrong.

## Next-phase framing (not a decision)

**U15** is now the clear, sole highest-priority thread: what actually
drives the repeated dispatch, and why does it stop advancing on pass 2?
The proposed next live capture is a breakpoint at `HalpInitSystemHelper`'s
own loop-control point (e.g. its inner-loop increment at `+0x5A`, or its
own entry) to directly read `ebx`/`edi` (the outer/inner loop indices)
rather than inferring them — this would distinguish H6 (the loop is
genuinely stuck, same registers, same iteration) from H7 (the whole
function is being freshly re-entered by something outside it). Hardware
fidelity analysis (H1/H2/H5) is no longer the productive next step, having
been undercut by facts 28–30 — whatever's wrong doesn't involve hardware,
ACPI, or CPUID state, at least not at the point currently visible. U11,
U14, and U16 remain open but lower priority.
