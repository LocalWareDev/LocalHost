# VPPT / Hyper-V Synthetic Timer Boot Blocker

**Status:** RESOLVED (2026-07-16, part 10). Root cause: HAL configures a
real RTC (Real-Time Clock, IRQ8) periodic interrupt for its phase-0 timer
test, but this hypervisor never emulated RTC periodic interrupts at all.
Implemented RTC periodic-interrupt emulation plus two supporting fixes to
this hypervisor's own interrupt-delivery architecture (delivery during
CPU-bound busy-spins, not just HLT). Confirmed fixed and reproducible
across independent boots — bugcheck 0x5C no longer occurs. This was never
a SynIC gap; read part 10 for the full fix, and part 9 for how the
(wrong) SynIC hypothesis was finally ruled out.
**Affects:** WHV backend (`Hypervisor.c`), Windows guests.
**Confirmed on:** Windows 10 22H2 installer environment (`win10_installer.vhd`), UEFI/OVMF boot path.

## Summary

A real, unmodified Windows installer environment reaches HAL timer
initialization and then bugchecks with `0x5C` (`HAL_INITIALIZATION_FAILED`).
Deep disassembly of the guest kernel, cross-referenced against the matching
`ntkrnlmp.pdb`, traced this to Windows' HAL choosing **VPPT** ("Virtual
Platform Performance Timer") — a Hyper-V-enlightened timer that depends on
genuine Hyper-V synthetic interrupt controller (SynIC) infrastructure — and
that infrastructure not being functional under WHV as currently configured.
Two follow-up experiments (documented below) narrowed this from "a bug we
might be able to work around" to "a structural gap requiring a real SynIC
implementation to close."

## Symptom

```
BUGCHECK 0x5C (HAL_INITIALIZATION_FAILED)
  param1 = 0x110              (generic component ID, reused across many failure sites)
  param2 = <pointer>          (the VPPT timer descriptor)
  param3 = 0x19               (HalpTimerLastProblem, a shared "last problem" global)
  param4 = 0xC0000001         (STATUS_UNSUCCESSFUL)
```

## Why Windows tries VPPT at all

WHV creates guest partitions as real children of the host's own Hyper-V root
partition. CPUID leaf 1 bit 31 (hypervisor present) is therefore always set,
and it is **not overridable by us** — confirmed by four independent attempts
(static `CpuidResultList` leaf-1 override, dynamic `CpuidExitList` trap +
clear-bit-31, hiding the Hyper-V leaf range 0x40000000-0x40000006 via the
same trap, and toggling `WHvPartitionPropertyCodeUnimplementedMsrAction`) —
all confirmed via rebuild+retest to have **zero effect** on guest behavior.
A diagnostic placed inside the CPUID exit handler itself never fired,
proving WHV doesn't even generate an exit for these leaves on this host; it
answers them internally. Since Windows always sees a Hyper-V-compatible
hypervisor, it always attempts Hyper-V-enlightened timer initialization.

## Root cause, from disassembly

Reading `HalpTimerInitializeHypervisorTimer` directly (captured in
`ColdPath.disasm.txt` / `ColdPath2.disasm.txt`, disassembled with symbol
resolution against the matching PDB), its control flow is:

1. CPUID leaf `0x40000006` (Hyper-V hardware-features leaf), `bt eax, 8` —
   abort immediately if clear. WHV answers this leaf internally; we cannot
   influence it (see above).
2. `HalpFindTimer` — locate a timer descriptor tagged as Hyper-V-capable.
3. `HalpTimerInitialize(descriptor)` — must succeed.
4. **`HalpTimerTestHypervisorTimer(descriptor, ...)`** — a live functional
   test of the actual Hyper-V synthetic timer, not a capability query. Must
   succeed or the function aborts.
5. Only *after* step 4 passes does the function branch into either an
   "enlightened" path (`HalpHvBuildDeviceId`, building a VMBus-style
   synthetic device ID) or a "legacy" path (`HalpInterruptLookupController`
   + `HalpInterruptSetRemappedLineStateInternal`, conventional GSI/IOAPIC
   routing — the same machinery our own IOAPIC emulation participates in).
   Both paths, on success, set bit 4 of the descriptor's `+0xb8` flags word.
6. Later, `HalpTimerConfigureInterrupt` checks that bit. If it was never
   set — because step 4 (or an earlier step) aborted — it fails immediately
   with `STATUS_UNSUCCESSFUL` and `HalpTimerLastProblem = 0x19`, which flows
   directly into the `KeBugCheckEx(0x5C, 0x110, descriptor, 0x19,
   STATUS_UNSUCCESSFUL)` call observed in the crash — the exact bytes of
   this call site are visible in the disassembly (`mov edx,0x110` /
   `mov ecx,0x5c` / `call KeBugCheckEx`).

**Key structural fact:** there is no code path in this function that
reaches conventional interrupt routing (step 5's "legacy" branch) without
*first* passing a real Hyper-V synthetic-timer functionality test (step 4).
Conventional hardware emulation sits downstream of the SynIC requirement,
not as an alternative to it.

## Corroborating experiment: granting SynIC access

To test whether WHV's real underlying Hyper-V hypervisor would back these
synthetic MSRs transparently, we set
`WHvPartitionPropertyCodeSyntheticProcessorFeaturesBanks` (granting
`HypervisorPresent`, `Hv1`, `AccessSynicRegs`, `AccessSyntheticTimerRegs`,
`AccessIntrCtrlRegs`, `AccessHypercallRegs`, `AccessVpIndex`,
`AccessPartitionReferenceCounter`, `AccessPartitionReferenceTsc`) plus
`LocalApicEmulationMode = X2Apic` (required together, per QEMU's WHPX
accelerator — the only substantial public reference implementation for this
corner of the API).

**Result:** guest behavior changed materially — the clean bugcheck 0x5C
disappeared entirely (`KiBugCheckData` was never populated), and execution
progressed measurably further into boot, consistent with step 4 above now
partially succeeding. But it then **hard-hung indefinitely**: 122+ seconds
with zero further VM exits, a tight spin-loop with no I/O/MSR/CPUID trap at
all, in a different, unidentified module upstream of VPPT's own code.

This proves two things: WHV *does* honor the property (not a no-op like the
CPUID overrides), and granting bare MSR *access* is not sufficient — actual
synthetic interrupt/message delivery (SINT/STIMER completion) needs active
participation from the VMM in the exit loop that WHV does not provide
transparently just because the property is set.

## Corroborating experiment: `bcdedit useplatformclock=true`

To rule out that this was merely a matter of Windows *choosing* VPPT over a
conventional timer, we set `useplatformclock=true` (+
`disabledynamictick=yes`) on the guest's BCD, a well-precedented community
workaround for VPPT-related bugchecks under third-party hypervisors. This
forces PM-timer selection regardless of hypervisor detection.

**Result:** no effect. The guest hung at the exact same relative code
offset as the SynIC-only test (confirmed via matching low-order RIP bits
across different KASLR bases). This proves the hang happens **before** HAL
timer selection runs at all — it's triggered by the synthetic-feature
exposure itself, upstream of and unrelated to VPPT's specific timer choice.
A timer-selection override structurally cannot route around it.

Both experiments were reverted; the codebase is back to the bounded,
diagnosable bugcheck 0x5C baseline.

## Conclusion

This is a structural limitation of the current WHV backend, not a
conventional-hardware-emulation gap. Fixing it for real would mean
implementing a genuine SynIC subsystem: a message pump for SINT delivery,
STIMER emulation wired into the VM exit loop, hypercall page setup, and the
VMBus-style device-ID plumbing `HalpHvBuildDeviceId` expects — a
feature-sized project, not a targeted fix. Given LocalHost's goal of
booting unmodified OS images (no kernel/HAL patching), and that Windows
cannot be made to skip this detection (the hypervisor-present CPUID bit is
enforced beneath the WHV API surface), this is being tracked as a known
limitation rather than pursued further right now.

**Confidence: high (~85%).** The control-flow structure and the exact
bugcheck parameter derivation are confirmed at the instruction level against
the correct PDB, and corroborated by two independent live experiments. The
residual uncertainty is *which exact step (1-4 above) fails first* in our
unmodified baseline — most likely step 4 given how it explains the SynIC
experiment's behavior change, but this hasn't been confirmed by a live
call-site trace. See `docs/roadmap.md` for a low-cost experiment that would
pin this down and could, in principle, disprove the conclusion if the real
failure turns out to be an earlier, non-SynIC step.

## Update 2026-07-16: independent reconfirmation, unchanged

Re-verified from scratch (fresh live boot trace, not just re-reading this
doc) after the PS/2 mouse and RTL8139/NAT work — neither touches anything
relevant here (CPUID, partition properties, APIC mode), and the bugcheck is
unchanged: identical `code=0x5C, params=[0x110, <ptr>, 0x19,
0xC0000001]`, same PDB match, every single test run across both features.

New detail from this pass: the log's IOAPIC-access counter is capped at
100 prints, and two full heartbeat intervals (exit count 200000 -> 210000)
elapse between the last *printed* IOAPIC access and the freeze -- meaning
tens of thousands of additional, unlogged IOAPIC redirection-table
accesses happened in between. This is a sustained retry loop against the
IOAPIC (consistent with the legacy `HalpInterruptSetRemappedLineStateInternal`
branch already documented above), not a quick isolated probe -- and it's
the *only* activity visible in the final stretch before the freeze; no
other device or port is touched. Doesn't change the conclusion, but
sharpens it slightly: whatever the guest is retrying, it's specifically
IOAPIC-facing, adding a little more color to "legacy path retries then
gives up" versus a hang somewhere unrelated.

**Architectural point worth stating explicitly, since it wasn't spelled
out before:** bugcheck 0x5C fires during kernel Phase 0/1 init, which
HAL init is part of -- this happens before the Plug and Play manager starts
and before any boot-start or PnP-discovered driver's `DriverEntry` runs.
The AHCI reads visible earlier in every boot log are `winload.efi`
*loading* files into memory, not drivers *executing*. This means "the next
hardware requirement for PnP" is not independently observable right now by
any amount of instrumentation -- nothing downstream of HAL init executes
at all while this blocker persists. Discovering what PnP needs next
requires first getting past VPPT, one way or another.

## Update 2026-07-16, part 2: falsification experiments — conclusion revised

Ran the two low-cost experiments this doc's original "Confidence" section
proposed, per an explicit user request to either weaken or strengthen the
conclusion before committing to a full SynIC implementation.

**Experiment A — complete synthetic-feature flag set (per docs/roadmap.md).**
Retried the `SyntheticProcessorFeaturesBanks` + `LocalApicEmulationMode=X2Apic`
experiment from earlier in this doc, this time also setting
`DirectSyntheticTimers` and `SyntheticClusterIpi` (both of which QEMU's WHPX
accelerator sets alongside the rest of the bank, and the first attempt
omitted). **Result: identical unbounded hang** — same RIP low-order bits,
same register signature (`rax=0, rcx=0x64, rdx=0, rbx=0`), confirmed
running past 38 seconds with zero further VM exits. Completing the flag set
changed nothing. Reverted back to baseline afterward.

**Experiment B — read what the crash's own descriptor already says.**
Rather than live INT3 call-site tracing (the original plan), used a safer,
zero-risk approach: the bugcheck's `param2` already points at the VPPT
timer descriptor, and the existing `kerneldiag` memory dump (previously
0x80 bytes, widened to 0x120) was extended to decode offset `+0xB8`. Per
the disassembly, bit 2 there is set only by `HalpTimerInitialize`'s success
path, and — critically — **bit 6 is set only by the code that runs after
`HalpInterruptSetRemappedLineStateInternal` (the legacy IOAPIC remap call)
succeeds, immediately before calling `HalpTimerEnableHypervisorTimer`.**

**Result, reproduced identically across two independent boots (different
KASLR bases, same value both times): `descriptor+0xB8 = 0x46`** — bits 1,
2, and 6 all set. Bit 6 being set means the full success path completed:
`HalpTimerInitializeHypervisorTimer` — CPUID gate, `HalpFindTimer`,
`HalpTimerInitialize`, `HalpTimerTestHypervisorTimer`, the legacy IOAPIC
interrupt-remap, and finally `HalpTimerEnableHypervisorTimer` — is not
where this fails. **The VPPT/Hyper-V timer appears to initialize
successfully as the main system clock.**

So what actually triggers the bugcheck? Re-examined the exact
`HalpTimerConfigureInterrupt` call site that leads to `KeBugCheckEx` —
**correction, made on an even closer re-read (see "part 3" below): this is
RVA `0x4A8FDB`, not `0x4A91B9` as first reported.** The disassembler's own
symbol resolution on the instruction immediately after it (`0x4A8FE8`,
reached by straight-line fallthrough with no branch in between) resolves
to **`HalpTimerInitializeClock+0xF93F4`** — this whole block, including the
hardcoded `mov ecx,0x5c` right before the `KeBugCheckEx` call, is inside
**`HalpTimerInitializeClock`**, not `HalpTimerInitializeProfiling`. (A
different, nearby call at `0x4A91B9` *is* inside
`HalpTimerInitializeProfiling`, but that one does not bugcheck on
failure — it just continues past it. That's the call this doc originally,
incorrectly, pointed to.) `HalpTimerInitializeClock` calls
`HalpTimerConfigureInterrupt` to hook up the **main system clock-tick
interrupt** (vector `0xD3` — passed as `edx` right before the call, and a
well-known Windows x64 clock-interrupt vector) on the same timer descriptor
`HalpTimerInitializeHypervisorTimer` already successfully enabled — and
*this* call is what returns failure and triggers the bugcheck.

**This still meaningfully weakens/reframes the original conclusion**, just
with the correct culprit: the failure is very likely not gated behind full
Hyper-V SynIC functionality at all — the SynIC-dependent part (enabling the
timer) appears to already work. The actual blocker looks like a narrower,
more conventional problem: whatever `HalpTimerConfigureInterrupt` needs
from the interrupt-remap path to route the clock-tick interrupt (vector
`0xD3`) isn't currently satisfied by our IOAPIC emulation — a materially
smaller, more tractable target than a full SynIC implementation, though
not yet root-caused to a specific fix. See "part 3" below for the direct
disassembly of `HalpTimerConfigureInterrupt` itself.

**Per the two-experiment protocol going in:** Experiment A pointed toward
"no change" (consistent with the old conclusion); Experiment B pointed
toward a substantively different conclusion. They did **not** agree, so per
the stated criteria this does *not* clear the bar for committing to the
full SynIC milestone — it instead opens a new, narrower investigation
thread (the clock-tick interrupt's remap/routing requirements) that should
be explored before any large SynIC investment.

**Confidence in this update: moderate-high (~75%).** The bit-6 semantics
and the `HalpTimerInitializeClock` attribution are both read directly from
disassembly already confirmed at the instruction level (the latter
double-checked after an initial mis-attribution to
`HalpTimerInitializeProfiling` — see "part 3" below), and bit-6 reproduced
identically across two independent boots. The residual uncertainty: this
infers the *previous* `HalpTimerInitializeHypervisorTimer` invocation
succeeded from static descriptor state observed *after* the later failure,
not from a direct live trace of that earlier call succeeding in real time —
in principle something else could have set those bits under a different
sequence of events, though no plausible alternative sequence was found
while re-reading the disassembly for this update.

## Update 2026-07-16, part 3: HalpTimerConfigureInterrupt's own logic, read directly

Per an explicit user request to narrow this to precise evidence rather than
further inference: dumped `HalpTimerConfigureInterrupt`'s own function body
straight from live guest memory (RVA `0x3A2574`, 1024 bytes, via the
existing `kernelReadVA` + dump-to-`.bin` mechanism already used elsewhere
in `kerneldiag`) and disassembled it with symbol resolution against the
matching PDB. This also caught and corrected the `HalpTimerInitializeClock`
vs `HalpTimerInitializeProfiling` mis-attribution above — see that
correction inline in part 2.

**The function's actual logic (`HalpTimerConfigureInterrupt.disasm.txt`),
confirmed at the instruction level, no longer inferred:**

```
0x3A262C: mov  eax, [r14+0xE0]      ; r14 = timer descriptor (1st param)
0x3A2633: bt   eax, 0xB             ; test bit 11
0x3A2637: jae  0x4A4A88             ; CLEAR -> jump to the cold/failing path (not in this dump)
0x3A263D: mov  rcx, r14
0x3A2640: call HalpTimerGetInternalData
0x3A2645: mov  rcx, rax
0x3A2648: mov  edx, r15d            ; r15d = vector (0xD3 for the clock, per the caller)
0x3A264B: mov  rax, [r14+0xA8]      ; a function pointer stored ON the descriptor
0x3A2652: call guard_dispatch_icall ; call through it, passing the vector
0x3A265E..0x3A266A: call HalpInterruptSetIdtEntry
0x3A266F: xor  edi, edi             ; return 0 (success)
```

This is exactly the bit-11 gate the very first pass at this investigation
(before this session) described from indirect evidence — now confirmed
directly. **If bit 11 of descriptor `+0xE0` is set, the function calls a
pre-registered function pointer at descriptor `+0xA8` with the vector,
sets the IDT entry, and returns success — no IOAPIC/MSI/interrupt-remap
calls anywhere in this path at all.** If bit 11 is clear, it jumps to a
cold path outside this dump (RVA `0x4A4A88` — in the same neighborhood as
`0x4A4A72`/`0x4A4A81`, flagged much earlier in this investigation's history
and initially dismissed as unrelated "AuthZ" code before being confirmed
relevant; consistent with that thread). That cold path is presumably where
the real MSI/interrupt-remap machinery (`HalpInterruptRemap`,
`HalpInterruptIsMsiSupported`, etc., referenced in this doc's very first
pass) lives, and where `STATUS_UNSUCCESSFUL` ultimately comes from.

**Cross-checking against the already-confirmed `descriptor+0xB8 = 0x46`:**
bit 4 (`0x10`) is clear, bit 6 (`0x40`) is set. Per `ColdPath2.disasm.txt`,
that exact pattern is produced by the **legacy IOAPIC branch** inside
`HalpTimerInitializeHypervisorTimer` (`HalpInterruptLookupController` +
`HalpInterruptSetRemappedLineStateInternal`, clearing bit 4 and setting
bit 6) — not the "enlightened" branch (`HalpHvBuildDeviceId`, which sets
bit 4 *and* bit 6 together). So: **our guest takes the legacy IOAPIC
branch for the timer's own enable step, and that branch succeeds** (the
timer gets enabled) **but does not populate whatever `HalpTimerConfigureInterrupt`
later needs** (descriptor `+0xA8`'s function pointer, `+0xE0` bit 11) **for
the clock-tick vector specifically.**

Checked whether `HalpTimerEnableHypervisorTimer` (called right after
either branch, previously dumped as
`HalpTimerEnableHypervisorTimer.bin`/`.disasm.txt`) sets either field —
**it doesn't.** It's a short function that checks one global "already
registered" flag and returns; it never touches the descriptor's `+0xA8` or
`+0xE0` at all. So whatever populates the fast-dispatch capability is set
up somewhere in the *enlightened* (`HalpHvBuildDeviceId`) branch itself,
which our guest doesn't take — not yet directly disassembled.

**Precise, narrow open question going forward:** either (a) find and
disassemble whatever sets descriptor `+0xA8`/`+0xE0` bit 11 (most likely
inside or downstream of `HalpHvBuildDeviceId`, RVA `0x9AA1F8`, and
determine whether it's genuinely SynIC-dependent or something more
conventional we could satisfy), or (b) disassemble the cold path at RVA
`0x4A4A88` to see exactly what the "legacy" MSI-remap path needs from our
IOAPIC emulation and why it returns `STATUS_UNSUCCESSFUL` — either finding
would directly resolve this. Both are precise, bounded next steps using
the same dump-and-disassemble technique already proven in this update, not
a return to broad SynIC-stack speculation.

## Update 2026-07-16, part 4: the full call chain, traced to a vtable dispatch

Followed option (b) above per an explicit user request to stay on the path
the guest actually takes rather than the untaken enlightened branch,
tracing every decision precisely rather than inferring. Each step below is
read directly from a fresh live-memory dump + disassembly (evidence files
named per step), not carried over from earlier, less precise passes.

**1. `HalpTimerConfigureInterrupt`'s cold path (RVA `0x4A4A88`,
`ColdPath_4A4A88.disasm.txt`).** First check: `bt eax, 0xA` (bit 10 of
descriptor `+0xE0`) — if clear, skip straight past `HalpInterruptIsMsiSupported`
and the whole MSI-remap attempt. Our descriptor's `+0xE0` (already read in
part 2: `0x00210131`) has bit 10 clear, so **MSI-remap is never even
attempted for the clock interrupt.** Execution falls into a second, separate
legacy GSI/IOAPIC fallback starting at `0x4A4B78`.

**2. The legacy fallback (same file).** Checks bits 9/8 of `+0xE0`
(`HalpInterruptGsiToLine` or a synthesized default GSI), calls
`HalpInterruptSetIdtEntry` unconditionally, then — since our vector (`0xD3`)
isn't `0xD1` and descriptor `+0xB8` bit 4 is clear — falls into
`HalpInterruptApplyOverrides` → `HalpInterruptLineToGsi`. **Immediately
before the next step (a call to `HalpInterruptRemap` at RVA `0x4A4CE7`),
there's a check on descriptor `+0xB8` bit 6: `test al,0x40; jne <skip>`.
Since our descriptor already has bit 6 set (from `HalpTimerInitializeHypervisorTimer`'s
earlier success, per part 2), that `HalpInterruptRemap` call is skipped
entirely** — an earlier read of `descriptor+0x120` (the pointer
`HalpInterruptRemap` would have dereferenced) confirmed it's `0x0` anyway,
which would otherwise be a null-pointer fault, not a graceful
`STATUS_UNSUCCESSFUL` — consistent with this call genuinely never
happening for us. Execution instead falls straight through to
`HalpInterruptSetLineState`.

**3. `HalpInterruptSetLineState` (RVA `0x3A3AE8`,
`HalpInterruptSetLineState.disasm.txt` + cold-path tail in
`HalpInterruptSetLineState_ColdPath.disasm.txt`).** Calls
`HalpInterruptApplyOverrides`, then `HalpInterruptLookupController` (this
resolves to *our emulated IOAPIC*, represented in the guest kernel as an
object HAL builds during MADT parsing). Checks the controller object's own
`+0xE0` bit 1 (clear for us — good, avoids a *different* immediate
`KeBugCheckEx` call in the cold path with `param1=0x202`, confirmed not
ours since our bugcheck always shows `param1=0x110`). Calls
`HalpInterruptFindLines`, `HalpInterruptDestinationToTarget`,
`HalpInterruptFindBestRouting` — all on the success path for us (no error
codes matching what we'd need to reach their failure branches). Finally
calls **`HalpInterruptSetLineStateInternal`**, and if *that* returns
negative, the surrounding code (`0x4A5C8B` in the cold path) does **not**
overwrite the return value — it just copies some line-descriptor fields
and falls through to the epilogue, meaning whatever
`HalpInterruptSetLineStateInternal` returns propagates **unmodified** all
the way back up.

**4. `HalpInterruptSetLineStateInternal` (RVA `0x378C7C`,
`HalpInterruptSetLineStateInternal.disasm.txt`) — the actual hardware-facing
call.** After a small amount of flag manipulation on the line descriptor,
it checks the controller object's `+0xDC` field against mask `0x200`; if
clear (our case, inferred from reaching this point at all — the `jne`
alternative is a separate cold region not yet dumped), it does:

```
0x378CC1: mov rax, [rcx+0x70]      ; rcx = controller object; rax = a function pointer
0x378CC5: mov rcx, [rcx+0x10]      ; rcx = controller's own "self"/context field
0x378CC9: call guard_dispatch_icall
0x378CCE: test eax, eax
0x378CD0: js <fail, return eax unmodified>
```

**This is a genuine vtable-style dispatch**: the controller object HAL
built to represent our emulated IOAPIC carries a function pointer at
`+0x70` (called with `+0x10` as context) that's presumably HAL's *actual*
"program this controller's redirection entry" routine — i.e., real Windows
code that would, in the course of doing its job, issue the real MMIO
writes to our IOAPIC to route vector `0xD3`. **If that call returns
negative, its return value is what ultimately surfaces as our observed
`STATUS_UNSUCCESSFUL`, with essentially no further transformation between
here and the bugcheck.**

**Where this leaves us:** the failure is not a Windows-HAL-level policy
decision or a SynIC-gated check at all by this point — it's (or is
immediately downstream of) a real interrupt-controller callback that, if
it works the way it looks like it should, ends up touching our emulated
IOAPIC directly. The controller object's `+0x70` value is a **runtime
pointer** (populated when HAL built this controller object, not a fixed
PDB symbol), so finding it requires either walking `HalpInterruptLookupController`'s
global controller list live (its body, in `HalpInterruptSetLineStateInternal.disasm.txt`'s
neighborhood, reads a RIP-relative global list head and walks entries
comparing a `+0xE8` field against the requested index) or capturing the
live register value at the call site — both read-only, no code-patching
risk, following the same pattern already used successfully throughout this
update. That's the natural next step, not yet done.

## Update 2026-07-16, part 7: the definitive root cause

Per an explicit user request to resolve the controller `+0x70` callback's
live target (rather than guessing statically) and trace it to the exact
hardware capability our implementation fails to satisfy, this update
identifies the precise instruction that generates our observed
`STATUS_UNSUCCESSFUL`, with the full chain confirmed instruction-by-
instruction rather than inferred at any hop.

**Resolving the controller object live.** `HalpInterruptLookupController`
(RVA `0x378DD0`) turned out to be a trivial global-list walk:

```
0x378DD0: mov rax, [rip+0x8D2B39]   ; rax = list head Flink
0x378DD7: lea r8,  [rip+0x8D2B32]   ; r8  = &list head (loop terminator)
0x378DDE: cmp rax, r8 ; je <empty>
0x378DE3: mov rdx, rax
0x378DE6: mov rax, [rax]            ; advance
0x378DE9: cmp [rdx+0xE8], ecx ; jne <next>   ; match node's +0xE8 (id) against requested id
0x378DF1: mov rax, rdx ; ret
```

The list head resolves to a global `LIST_ENTRY` at RVA `0xC4B910`. Walking
it live (read-only, no breakpoints) found **four** registered controller
objects, not one — an important correction to this update's own earlier,
too-hasty assumption that grabbing the first (`Flink`) entry was
sufficient:

| # | id (`+0xE8`) | flags (`+0xDC`) | callback (`+0x70`) |
|---|---|---|---|
| 0 | `0x1` | `0xFF` | RVA `0x37D580` (`HalpApicSetLineState`) |
| 1 | `0x2` | `0xFF` | RVA `0x37D580` (`HalpApicSetLineState`, same function) |
| 2 | `0xB000` | `0x0` | RVA `0x4DD370` (different function) |
| 3 | `0xB001` | `0x0` | RVA `0x4DD370` (same as #2) |

Controllers `0xB000`/`0xB001` are a distinct pair gated by an explicit
sentinel-ID check inside `HalpInterruptApplyOverrides`
(`lea eax,[rcx-0xB000]; cmp eax,1; ja <skip>`) — a special-cased legacy
range unrelated to our normal GSI-based clock interrupt, and out of scope
per the "stay on the executed legacy IOAPIC path" instruction governing
this update. Controllers `0x1`/`0x2` share identical code
(`HalpApicSetLineState`), so which of the two actually backs our clock
line doesn't change which instructions run — only the per-instance data.

**The callback: `HalpApicSetLineState` → `HalpApicConvertToRte`.** Dumped
and disassembled directly from the live pointer. `HalpApicSetLineState`
calls `HalpApicConvertToRte` and, on failure, returns its result completely
unmodified (`test eax,eax; js <epilogue>` — the epilogue never touches
`eax`). `HalpApicConvertToRte` itself has two concrete failure literals —
`0xC000000D` (`STATUS_INVALID_PARAMETER`, when the line descriptor's first
field mismatches the per-line config array's `+8` field) and `0xC00000BB`
(`STATUS_NOT_SUPPORTED`, the function's default value, returned when
capability bits at the per-line array entry's `+0x30`/`+0x10` fields don't
satisfy specific constraints) — **neither of which is our observed
`0xC0000001`.**

**Confirming unmodified propagation at every hop, directly, not by
assumption.** Re-checked (rather than continuing to assume) every landing
pad between the callback and the bugcheck:
- `HalpInterruptSetLineStateInternal`'s own failure landing pad (RVA
  `0x49462E`, reached from `js` right after the controller `+0x70`
  dispatch) stamps diagnostic metadata (subcode `7`, source line `0xC4E`)
  into a diagnostic record but never touches `eax` — confirmed by direct
  disassembly, not inferred.
- `HalpInterruptSetLineState`'s cold-path landing pad (RVA `0x4A5C8B`)
  does an SSE-based struct copy but likewise never touches `eax`/`edi`.
- `HalpTimerConfigureInterrupt`'s cold path (RVA `0x4A4A88`) calls
  `HalpInterruptSetLineState`, stores the result in `edi`, and returns
  `edi` completely unchanged through the epilogue (`test edi,edi; js
  <fail>`).

Since none of `HalpApicSetLineState`/`HalpApicConvertToRte`'s own literals
match `0xC0000001`, and propagation is now confirmed unmodified end to
end, the real failure had to be a call **within** this chain whose return
value *also* propagates through the same `0x4A5C8B` landing pad — and
there is one: `HalpInterruptFindBestRouting` (RVA `0x378CF0`), called from
`HalpInterruptSetLineState` *before* `HalpInterruptSetLineStateInternal`,
whose own failure check (`js 0x4A5C8B`) targets the exact same landing pad
already ruled out as a transform site.

**`HalpInterruptFindBestRouting` — the actual source of `STATUS_UNSUCCESSFUL`.**
Fully disassembled (hot path already captured in
`HalpInterruptSetLineStateInternal.disasm.txt`, cold path newly captured
in `HalpInterruptSetLineStateInternal_ColdPath.disasm.txt` at RVA
`0x49462E`+). Its logic:

```
mov ecx, [rcx]                 ; controller id, from the line descriptor
call HalpInterruptLookupController
xor ecx, ecx                   ; ecx = 0 (success value, if we get there)
test rax, rax ; je <0xC000000D path>
lea r8, [rax+0x100]            ; r8 = &controller's OWN embedded LIST_ENTRY at +0x100
mov ebx, ecx                   ; ebx = 0 (candidate counter)
mov r11, [r8]                  ; r11 = Flink
mov rdx, r11
cmp r11, r8 ; je <end-of-walk check>
loop:
  inc ebx                       ; count every node visited
  cmp [rdx+0x10], r9d ; jne <next>   ; r9d = requested destination
  cmp [rdx+0x14], edi ; jg  <next>   ; edi = requested priority/target
  cmp [rdx+0x18], edi ; jle <next>
  ; MATCH: store r9 into *r10 (output), return ecx=0 (success)
  next: advance to Flink; loop while rdx != r8
end-of-walk (no match found):
  cmp ebx, 1 ; je <single-candidate fallback: use it anyway, return success>
  ; otherwise (ebx==0, list totally empty; OR ebx>1, multiple non-matches):
  mov ecx, 0xC0000001            ; STATUS_UNSUCCESSFUL  <-- THE INSTRUCTION
  ; (also stamps subcode 0x17, source line 0xDEE, into the diagnostic record)
  return ecx
```

This is the exact, confirmed origin of our bugcheck's `param4`. **The
interrupt controller object carries its own internal, second `LIST_ENTRY`
at offset `+0x100` — a list of "routing candidate" entries, each
presumably tying a destination (CPU/APIC target) and a priority/vector
range to a valid routing outcome. `HalpInterruptFindBestRouting` walks
that list looking for an entry whose destination and priority window
satisfy the clock interrupt's request (vector `0xD3`). If it finds no
match: an empty list (zero candidates) or an ambiguous multi-candidate
non-match both fail outright with `STATUS_UNSUCCESSFUL`. Only the single
special case of exactly one candidate (`ebx==1`) is treated leniently and
accepted as a fallback regardless of whether it actually matches.**

Given a single-vCPU, minimally-described emulated environment, the
overwhelmingly likely scenario is that this candidate list is **completely
empty** (`ebx` never incremented) — meaning HAL never populated *any*
routing-candidate entry for this controller/destination combination in
the first place. This list is HAL-internal bookkeeping, built during
controller/topology setup — the natural, standard source for this kind of
per-controller "valid destination + priority" data is the guest's ACPI
MADT (Processor Local APIC/x2APIC entries and their affinity, and/or
Interrupt Source Override structures) or an equivalent legacy topology
description. **This has not yet been confirmed by directly reading the
controller's `+0x100` list live (a natural, bounded next step — read
`controller+0x100`, check whether Flink equals the head, i.e. genuinely
empty, before committing to the ACPI/MADT hypothesis)**, so it is reported
as the leading hypothesis, not yet a proven fix target.

**Confidence: high (~85%)** for the instruction-level chain (every hop
confirmed directly from live memory, zero remaining "assumed" propagation
points). **Moderate (~55%)** for the specific ACPI/MADT-shaped root cause
of the empty candidate list — plausible and consistent with everything
observed, but not yet directly confirmed by reading `controller+0x100`
live or by tracing what populates that list during controller setup.

## Update 2026-07-16, part 8: part 7's hypothesis directly contradicted by live data

Per the natural next step flagged at the end of part 7 — read
`controller+0x100` live before committing to it as the root cause —
walked that list for all four registered controllers. Every one of them
came back **non-empty, with exactly one candidate entry** (`Flink ==
Blink`, both pointing to the same single node):

| controller | candidate dest (`+0x10`) | prioLo (`+0x14`) | prioHi (`+0x18`) |
|---|---|---|---|
| id=1 | `0xFFFFFFFF` | `0x1` | `0x7` |
| id=2 | `0xFFFFFFFF` | `0x1` | `0x7` |
| id=0xB000 | `0xFFFFFFFF` | `0x1` | `0x2` |
| id=0xB001 | `0xB000` | `0x2` | `0x3` |

Re-reading `HalpInterruptFindBestRouting`'s own logic against this data:
the `ebx` counter increments once per node visited, and the cold-path
check `cmp ebx,1; je <fallback>` means **a list with exactly one entry
always succeeds** — either via a direct field match, or via the lenient
single-candidate fallback (which discards the match requirement entirely
and unconditionally returns success). With every controller carrying
exactly one candidate, `HalpInterruptFindBestRouting` cannot actually
reach the `0xC0000001` literal found in part 7 for any of them. **This
directly contradicts part 7's conclusion.** The instruction and its
literal value are real and confirmed to exist in the binary — the error
was in believing our live conditions could reach it.

## Update 2026-07-16, part 9: the actual root cause — a real hardware interrupt never arrives

Resolving the part 8 contradiction required dumping something this
investigation had never actually looked at directly: **`HalpTimerInitializeClock`'s
own hot path** (RVA `0x3AFBF4`). Every prior update worked backward from
the cold-path tail (`ColdPath.disasm.txt`, captured in part 1) without
ever confirming what the hot path leading into it actually does. That
gap is exactly what produced the part 7 misattribution.

**The real flow, confirmed directly:**

```
0x3AFC81: call HalpTimerConfigureInterrupt      ; vector 0xD1 (edx last set at 0x3AFC41)
0x3AFC86: test eax, eax
0x3AFC88: js 0x4A8FE8                            ; failure -> straight to KeBugCheckEx (part 2's call site)
0x3AFC8E: mov rcx, [rip+0x89C2B3] ; test rcx,rcx ; jne 0x4A8FB0   (not our path)
0x3AFC9E: mov eax, [rbx+0xE0] ; test al,0x50 ; je 0x4A9009  (not our path -- 0x50 bits ARE set for us)
0x3AFCC7: call HalpSetTimer
0x3AFCCC: test eax, eax ; js 0x4A8FE8            ; failure -> same bugcheck
0x3AFCD7: call HalpTimerWaitForPhase0Interrupt
0x3AFCDC: test al, al
0x3AFCDE: je 0x4A9010                             ; RETURNED FALSE -- the bit4-gated block from part 8's contradiction
0x3AFCE4: xor eax, eax                            ; (success path, not taken by us)
0x3AFCE6: test eax, eax ; js 0x4A8FE8
```

**`HalpTimerConfigureInterrupt` genuinely succeeds for us** — fully
consistent with part 8's finding that the routing-candidate lists are
populated. The entire `HalpInterruptSetLineState` →
`HalpInterruptFindBestRouting` → `HalpApicSetLineState` →
`HalpApicConvertToRte` chain traced in parts 4-7 is real, confirmed code
that genuinely executes on our boot — it just isn't where our failure
comes from, because it succeeds. That work isn't wasted: it's now
positive evidence that our IOAPIC emulation and interrupt-remap/routing
logic are all working correctly at the HAL level.

**The actual failure is `HalpTimerWaitForPhase0Interrupt`** (RVA
`0x3AFD00`, captured in the same dump):

```
0x3AFD0D: mov dword ptr [rcx+0x40], 0    ; clear descriptor+0x40
0x3AFD19: xor ebx, ebx                    ; loop counter = 0
loop:
  0x3AFD1B: mov eax, [rdi+0x40] ; test eax,eax ; jne <success>   ; poll descriptor+0x40
  0x3AFD22: mov ecx, 0x2710 ; call KeStallExecutionProcessor      ; busy-wait 10,000us = 10ms
  0x3AFD2C: add ebx, 0xA ; cmp ebx, 0xBB8 ; jb loop                ; loop while ebx < 3000
0x3AFD37: jmp 0x4A9082                     ; TIMEOUT after 3 seconds -- returns FALSE
0x3AFD3C: mov al, 1 ; ret                  ; SUCCESS -- returns TRUE
```

**This function busy-polls `descriptor+0x40` for up to 3 real seconds
(300 × 10ms), waiting for it to become non-zero — which only happens if
the interrupt HAL just configured (vector `0xD1`, via the
`HalpTimerConfigureInterrupt` call immediately before) actually fires and
its ISR increments that field.** If the wait times out, the function
returns `FALSE`, landing on the bit4-gated block found in part 8: since
our descriptor `+0xB8` bit 4 is clear (confirmed since part 3, value
`0x46`), that block immediately sets `eax = 0xC0000001` and
`HalpTimerLastProblem = 0x19` — exactly our observed values — with zero
further dependency on the interrupt-controller/routing machinery.

**Root cause, stated precisely: `HalpTimerConfigureInterrupt` successfully
programs the guest's IDT and our emulated IOAPIC for interrupt vector
`0xD1`, but that interrupt never actually arrives and gets acknowledged
within HAL's 3-second patience window.** This is the first hardware
behavior our implementation genuinely fails to satisfy in this whole
chain: real, timely interrupt delivery for a HAL-configured vector, not
any policy/capability/routing check.

**Correction to part 2:** that update attributed vector `0xD3` to this
call site, reasoning from context that turned out to be a different,
unrelated cold-path block (`HalpTimerInitializeClockPn`'s own IDT setup
at RVA `0x4A9246`, coincidentally nearby in the same cold-path trampoline
region). Now that the hot path is directly visible, the vector is
confirmed as `0xD1` (last explicitly set at RVA `0x3AFC41`, unchanged
through the call at `0x3AFC81`).

**Next concrete step, not yet started:** determine why an interrupt
configured for vector `0xD1` on our emulated IOAPIC/LAPIC never reaches
the guest's ISR — likely something in the WHV interrupt-injection path,
IOAPIC redirection-entry programming, or LAPIC vector delivery that's
missing or incorrect for this specific vector/line, as opposed to a
guest-side HAL policy gap. This is a substantially smaller, more concrete
target than anything in parts 1-8: a single missing or incorrect
interrupt-delivery behavior, not a capability/routing/topology gap.

**Confidence: high (~85%).** The full hot-path flow is now confirmed
directly from live memory with no remaining unconfirmed jumps between
`HalpTimerConfigureInterrupt`'s success and the final bugcheck. The
residual uncertainty is *why* vector `0xD1` doesn't arrive — that's the
next investigation's job, not yet touched.

## Update 2026-07-16, part 10: root cause fixed — bugcheck 0x5C resolved

Per an explicit user request to implement the smallest possible fix once
the prerequisite was understood, this update implements and confirms the
actual fix for the root cause identified in part 9. **The bugcheck no
longer occurs — reproduced clean across two independent boots.**

**What was actually missing.** Live tracing of the guest's own IOAPIC
redirection-table writes (added targeted logging to `Hypervisor.c`'s
`ioapicWriteRegister` call site) found the precise entry HAL programs:

```
controller id=1 entry=8 reg=0x20 value=0x000008D1 -> vector=0xD1 masked=0
```

**GSI 8 — the classic MC146818 RTC (Real-Time Clock) IRQ, not the PIT
(IRQ0) as first assumed.** This codebase already emulated RTC time/date
*register reads* (`cmosReadRtcField`, for `PcRtcGetTime`), but had **no
periodic-interrupt emulation at all** — no Register A rate tracking, no
Register B PIE (Periodic Interrupt Enable) handling, no Register C
status/ack, and no IRQ8 ever fired. `HalpTimerWaitForPhase0Interrupt`
(part 9) was waiting for an interrupt that could structurally never
arrive.

**The fix, in three parts (all in `Hypervisor.c`):**

1. **RTC periodic interrupt emulation.** Added `rtcPeriodicIntervalMs()`
   (standard MC146818 divider formula: `2^(rate-1) / 32768` seconds for
   Register A rate values 3-15), and `deliverRtcPeriodicIrq()`, which
   checks Register B's PIE bit, fires at the programmed rate, sets
   Register C's PF/IRQF bits, and delivers through the new
   `ioapicResolveVector()` helper (added in this same update) — which
   honors whatever vector the guest's own IOAPIC redirection entry
   specifies once reprogrammed, falling back to the legacy 8259-remap
   vector only while the entry is still at its untouched power-on
   default. Register C reads now clear-on-read, matching real hardware's
   interrupt-acknowledgment behavior.

2. **Deliver on every main-loop iteration, not just while halted.** The
   first version of this fix only called `deliverRtcPeriodicIrq` from the
   halted-CPU wait loop and still didn't clear the bugcheck. Live
   diagnostic counters showed why: `HalpTimerWaitForPhase0Interrupt`
   waits via `KeStallExecutionProcessor` — a **busy-spin loop**, not
   `HLT`. The guest CPU never halts during that exact 3-second window, so
   a callback wired only into the halted branch never got a chance to
   run. Moved the call to also run from the main loop, alongside the
   existing `deliverPendingAtaIrq`/`deliverPendingRtl8139Irq`.

3. **Force periodic VM exits during CPU-bound spins.** Still not enough:
   a pure busy-spin with no trapped I/O/MMIO can run for the guest's
   *entire* wait window inside a single `WHvRunVirtualProcessor` call,
   generating zero VM exits and giving the main loop no opportunity to
   check anything at all. Added a small background thread
   (`rtcCancelThread`) that calls `WHvCancelRunVirtualProcessor` roughly
   every millisecond while the RTC's periodic interrupt is armed,
   forcing the main loop to regain control regularly. This surfaced one
   more gap: the resulting `WHvRunVpExitReasonCanceled` exit reason
   wasn't handled by the main dispatch switch, whose `default` case
   treated any unrecognized reason as fatal and stopped the whole
   hypervisor — added an explicit no-op case for it.

**Verification.** Live diagnostic counters confirm the interrupt actually
firing at the expected rate (e.g. "firedTotal=104" by the 500th
scheduling check in one run), and — most importantly — **the bugcheck's
own diagnostic block, which fires on every stall regardless of cause,
found no `KiBugCheckData` at all in two independent post-fix boots**
(previously present and identical in literally every single boot across
this entire session). The guest now progresses to a new, later, different
stall (confirmed reproducible: same RIP/register signature — `rax=0,
rcx=0x64, rdx=0, rbx=0`, a stack pointer pattern consistent with a
materially later boot stage with real heap allocations — across both
post-fix runs) — a new, separate problem, explicitly out of scope for
this investigation, whose entire purpose was resolving bugcheck 0x5C.

**This closes out the VPPT/SynIC investigation.** The original hypothesis
(a missing full SynIC implementation) was wrong. The real, final answer
had nothing to do with Hyper-V synthetic timers, interrupt remapping
policy, or interrupt-controller capability/routing checks (all of which
were confirmed working correctly along the way) — it was a genuinely
missing hardware feature (RTC periodic interrupts) combined with two
real gaps in this hypervisor's own interrupt-delivery architecture
(halted-only delivery, and no mechanism to interrupt a CPU-bound guest
spin).

## Evidence artifacts

- `ColdPath.disasm.txt`, `ColdPath2.disasm.txt` — disassembly of
  `HalpTimerInitializeHypervisorTimer` and `HalpTimerConfigureInterrupt`'s
  cold paths, symbol-resolved against `ntkrnlmp.pdb` (GUID
  `D9424FC4-861E-47C1-0FAD1B35DEC6DCC8`, age 1).
- `HalpTimerConfigureInterrupt.bin`/`.disasm.txt` — the function's actual
  body (RVA `0x3A2574`), confirming the bit-11 gate directly (part 3
  update above).
- `HalpTimerEnableHypervisorTimer.bin`/`.disasm.txt` — ruled out as the
  source of the `+0xA8`/`+0xE0` fast-dispatch setup (part 3 update above).
- `ColdPath_4A4A88.disasm.txt`,
  `HalpTimerConfigureInterrupt_ColdPath_4A4A88.bin` — the cold path taken
  when bit 11 is clear, confirming the bit-10 MSI-skip and the
  `HalpInterruptRemap` skip via descriptor `+0xB8` bit 6 (part 4 update).
- `HalpInterruptLineToGsi.bin`/`.disasm.txt` — ruled out (only returns `0`
  or `0xC000000D`, never `0xC0000001`) (part 4 update).
- `HalpInterruptRemap.bin`/`.disasm.txt` — confirmed real but a dead end for
  our path, since the call to it is skipped (part 4 update).
- `HalpInterruptSetLineState.bin`/`.disasm.txt`,
  `HalpInterruptSetLineState_ColdPath.bin`/`.disasm.txt` — traces the final
  hop into `HalpInterruptSetLineStateInternal` and confirms the cold-path
  tail's two `KeBugCheckEx` calls don't match our `param1=0x110` (part 4
  update).
- `HalpInterruptSetLineStateInternal.bin`/`.disasm.txt` — the definitive
  finding: a vtable-style dispatch through the IOAPIC controller object's
  own `+0x70` function pointer (part 4 update).
- `HalpInterruptLookupController.bin` — confirms the global controller
  `LIST_ENTRY` walk and its RVA `0xC4B910` head (part 7 update).
- `dump_full_controller_list.log` — the live walk of all four registered
  controller objects (part 7 update).
- `InterruptControllerCallback_0.bin`/`.disasm.txt` — the controller
  `+0x70` callback dumped directly from its live pointer, resolving to
  `HalpApicSetLineState`/`HalpApicConvertToRte` (part 7 update).
- `InterruptControllerCallback_2.bin` — the `0xB000`/`0xB001` sentinel
  controllers' callback, confirmed out of scope for our path (part 7
  update).
- `HalpApicSetLineState_ColdPath.bin`, `HalpApicSetLineState_ColdPath2.bin`/`.disasm.txt`
  — the callback's cold branches, including the `0xC000000D` literal at
  RVA `0x4957C2` (part 7 update).
- `HalpInterruptSetLineStateInternal_ColdPath.bin`/`.disasm.txt` — contains
  both the `0x49462E` diagnostic-stamp landing pad and, critically, the
  `HalpInterruptFindBestRouting` cold path at RVA `0x494662`-`0x494696`
  with the definitive `mov ecx, 0xC0000001` instruction at RVA `0x494684`
  (part 7 update).
- `dump_routing_candidates.log` — live read of all four controllers'
  `+0x100` routing-candidate lists, each non-empty with exactly one entry
  — the evidence that directly contradicted part 7 (part 8 update).
- `HalpTimerInitializeClock_HotPath.bin`/`.disasm.txt` — the function's
  actual hot path (RVA `0x3AFBF4`), never previously dumped, showing the
  real call sequence (`HalpTimerConfigureInterrupt` → `HalpSetTimer` →
  `HalpTimerWaitForPhase0Interrupt`) and definitively identifying the
  latter's timeout as the true root cause (part 9 update).
- `rte_write_trace2.log` — the live IOAPIC redirection-table write trace
  that identified GSI 8 (RTC)/vector `0xD1` as the real target, correcting
  the earlier GSI 0/PIT guess (part 10 update).
- `cmos_ab_trace.log` — confirms the guest genuinely enables Register B's
  PIE bit before the bugcheck point, ruling out "PIE never gets set" as
  an explanation for the first (incomplete) fix attempt (part 10 update).
- `rtc_cancel_test2.log`, `rtc_confirm_repro.log` — post-fix boot logs
  showing the RTC interrupt firing at rate and, critically, no
  `KiBugCheckData` at all, reproduced across two independent runs (part
  10 update).
- `resolve_kernel_symbols.py`, `disasm_hal_dump.py` — the dbghelp/capstone
  tooling used to produce the above.
- `synic_test.log`, `platclock_test.log` — boot logs from the two
  corroborating experiments in part 1.
