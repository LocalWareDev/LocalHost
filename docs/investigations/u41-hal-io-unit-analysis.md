# U41: static analysis of the pass-2 HAL I/O unit init, and a correction to U40

Date: 2026-07-28

## Summary

U40's RIP sampler put pass 2's last kernel activity in `HalpApicInitializeIoUnit`
and then `HalpPicInitializeIoUnit`, and I read that as "APIC I/O unit init spins,
gives up, and falls back to legacy PIC init". **Disassembling both functions shows
that reading was wrong on every count.** The underlying observation stands; the
interpretation does not.

## What the disassembly shows

`HalpApicInitializeIoUnit` (RVA 0x3A5780, 0x148 bytes). `rbx` is the IOAPIC MMIO
window, so `[rbx]` is IOREGSEL and `[rbx+0x10]` is IOWIN:

- `+0x3B` `jne +0x5D` — skips `HalMapIoSpace` when the cached window `[rcx+0x10]`
  is already non-zero.
- `+0x5D..+0xA7` — selects register 1 (version) and reads it **twice**, requiring
  that it is neither 0 nor 0xFFFFFFFF, and that both reads agree in the version
  byte and in the max-redirection-entry field. Our `ioapicReadRegister` returns a
  constant `(23<<16)|0x11`, so this passes.
- `+0xB5..+0xC6` — selects register 0 and writes the IOAPIC ID.
- `+0xC9` `je +0x134` — the redirection-table loop runs only when `[rdi+0x1a]` is
  non-zero.
- `+0xD7..+0x113` — the loop: for each entry, write `0x100FF` (masked, vector
  0xFF) to the low dword and `0` to the high dword.

`+0x101` and `+0x103` — the two RIPs U40 saw repeatedly — are the loop's
`mov [rbx], esi` and `mov [rbx+0x10], 0`. They repeat because the loop iterates 24
entries, and each MMIO write causes a VM exit. **That is normal progress, not a
spin or a hang.**

Note also that this loop *overwrites every redirection entry* with the masked
default. So stale redirection-table contents carried over from pass 1 cannot by
themselves be what breaks pass 2 — HAL clobbers them here regardless. The
delivery-mode bail at `+0xE8..+0xF4` (`(entry & 0x700) == 0x200`) is unreachable on
this path, because it is guarded by `[rdi+0x1a] == 0` while the loop itself only
runs when that byte is non-zero.

`HalpPicInitializeIoUnit` (RVA 0x3A75D0, 0x50 bytes) is a textbook 8259
initialization: `ICW1=0x11`, `ICW2=0xD8`, `ICW3` (4 or 2), `ICW4=0x1`, `OCW1=0xFF`,
then `ret`. `+0x42`, U40's final sample, is just the ICW4 `out`. The function
completes.

Neither function has a direct `E8` caller (`tools/findcalls.py`), so both are
reached indirectly, through the HAL init dispatch table — i.e. the
`HalpInitSystemHelper` -> `guard_dispatch_icall` loop identified in U17. They are
two consecutive dispatch entries. **`HalpPicInitializeIoUnit` is not a fallback
from a failed APIC init**; our MADT advertises PCAT_COMPAT (dual 8259 present), so
initializing both controllers is expected.

## The instrument's blind spot

The sampler reads `VpContext.Rip`, which only exists **at a VM exit**. It therefore
samples only instructions that cause exits — MMIO accesses and `out` — and is
completely blind to code that runs without exiting.

Consequences for U40's wording:

- "Pass 2's kernel runs only ~270 exits" is true, and the ~270 exits between
  discovery and the bugcheck is a real, short window (~5ms wall clock). But it is a
  count of *exits*, not of instructions or code executed. Arbitrarily much code can
  run between two exits.
- "The kernel begins at `HalpApicInitializeIoUnit`" is wrong. That is just the
  first kernel-VA code that happened to *exit*. `KiSystemStartup`,
  `KiInitializeKernel` and friends do little MMIO and are largely invisible.
- Module discovery has the same bias: it needs a canonical kernel RIP *at an exit*,
  so the kernel may have been executing well before we could discover it. This is a
  second, independent reason the U36/U39 breakpoint-arming floor exists.

## What still stands

- Pass 2 bugchecks ~270 exits (~5ms) after the first kernel-VA exit.
- Those exits are HAL I/O unit init: IOAPIC MMIO writes, then the 8259 `out`
  sequence, both of which run to completion.
- The crash lands shortly after PIC init returns, back in the dispatch loop.
- U40's structural correction of U38 is unaffected: `InitBootProcessor` is not
  reached (`KiInitializeKernel` calls `HalpInitSystemPhase1` at `+0x5C9` and
  `InitBootProcessor` only at `+0x5E1`), so MM init and pool init never run and the
  null pool descriptor is correct behaviour for that point in boot.

## Re-examining the earlier "IOAPIC reset is harmful" verdict

`post-vppt-boot-stall.md` (lines 192, 1144) rules out resetting IOAPIC
redirection-table state, reporting that it made things "reliably worse -- an
immediate, consistent freeze structurally identical to the pre-fix VPPT/RTC
interrupt-delivery gap (`HIGH_LEVEL` IRQL, oscillating RIP)".

That signature is exactly what U20 later proved to be `HaliHaltSystem`, the HAL's
**post-bugcheck** halt loop. So the observed effect was not a freeze; it was an
*earlier bugcheck*. The verdict rests on a diagnostic interpretation the same
document elsewhere records as disproven, so "IOAPIC state is proven harmful to
reset" is not sound evidence and should not be treated as closing the question.

That said, the redirection-table finding above argues the fix is not simply
re-masking entries: HAL rewrites them all anyway. Better candidates for stale
per-pass state are `IoApicState.id` (writable by the guest via register 0, and
never restored) and `IoApicState.selectedReg`.

## U42 result: the IOAPIC is exonerated as the divergence

Added a per-pass, post-decode IOAPIC register trace (the pre-existing log used one
global counter, so pass 1 consumed its entire first-100 budget and pass 2 logged
nothing; it also printed before decode, so it could not show register or value).

Measured, one run:

| | pass 1 | pass 2 |
|---|---|---|
| total IOAPIC accesses | 274 | 262 |
| logged ops on ioapic1 | 131 | 131 |
| logged ops on ioapic2 | 129 | 129 |
| completes both controllers | yes | yes |
| programs clock entry 8 -> vector 0xD1 | yes | never reached |

Both passes issue the *same* sequence to the *same* registers with the *same*
values: version register read twice (returning `0x00170011` both times, satisfying
the cross-check at `+0x89..+0xA7`), ID read then written back, then every
redirection entry set to `0x100FF` low / `0` high, across both controllers. The
only difference is the value read *back* from entries — stale `0x000100FF` on
pass 2 versus power-on `0x00010000` on pass 1 — and HAL overwrites every entry
regardless, so that difference is behaviourally inert.

**So the IOAPIC register conversation is not where the passes diverge.** The
hypothesis that pass 2's APIC I/O unit init fails because of stale emulated IOAPIC
state is disproved. The 12-access difference in the totals is simply pass 1
continuing on to program the clock interrupt afterwards, which pass 2 never reaches
because it bugchecks first.

## A real stale-state bug found on the way

Pass 2 inherits `ioapic1.redir[8] = 0x01000000000008D1` — GSI 8 (the RTC),
**vector 0xD1, unmasked**, destination 0x01 — because the reset restores CPU
registers only. That entry stays unmasked from the reset until pass-2 HAL init
re-masks it, a window of roughly 130k exits spent in firmware and the bootloader.

`ioapicResolveVector` deliberately returns the IOAPIC-programmed vector once an
entry has been reprogrammed, so any RTC interrupt injected during that window is
delivered as vector **0xD1** to firmware/bootloader code that has no handler for
it, instead of the legacy 8259-remapped vector. That is a concrete wrong-vector
injection bug, independent of whether it is what causes the 0x139.

## Next steps

1. ~~Log IOAPIC MMIO accesses per pass and diff pass 1 against pass 2~~ — done
   above; the IOAPIC is exonerated.
1b. ~~Test the inherited-unmasked-RTE hazard~~ — done, see U43 below. Negative.

## U43 result: the inherited RTE is latent, not causal

Logged every non-legacy (IOAPIC-programmed) vector resolution, per pass:

| | pass 1 | pass 2 |
|---|---|---|
| non-legacy vector resolutions | 63 (all GSI 8 -> 0xD1, `kernelFound=1`) | **0** |
| RTC PIE bit (`cmosRegisters[0x0B] & 0x40`) at bugcheck | — | `0x00` |

Pass 2 never resolves a programmed vector even once. `deliverRtcPeriodicIrq`
returns immediately unless the RTC's PIE bit is set, and that bit reads `0x00`
throughout pass 2, so the RTC periodic path never runs, `ioapicResolveVector(8, …)`
is never called, and the inherited unmasked `0xD1` entry is never consulted.

The wrong-vector bug identified in U42 is therefore **real in the code but never
exercised on this path** — latent, not causal. It is still worth fixing defensively
(a pass-2 guest that *did* enable PIE before HAL re-masked the entry would receive
vector 0xD1 in firmware), but it does not explain the 0x139.

Pass 1's 63 resolutions all carry `kernelFound=1`, i.e. they happen after the
kernel is up and has legitimately programmed the clock interrupt. That is correct
behaviour, and it is the mechanism the resolver was written for.

Combined with U42, interrupt routing and IOAPIC state are both exonerated.

## Where this leaves the investigation

Still solid:
- Pass 2 bugchecks 0x139 shortly after HAL's IOAPIC and 8259 init both complete.
- `InitBootProcessor` is never reached, so MM init and pool init never run, and the
  null pool descriptor is correct for that point in boot.
- The crash is a genuine pool allocation made when no pool exists yet.

The sharp unanswered question is therefore **who allocates pool that early on
pass 2, when pass 1 does not**. U28-U31 traced the allocation *downward*
(`ExAllocateHeapPool` -> `RtlpHpLfhSlotAllocate` -> `GetSubsegment` -> fastfail) but
never identified the *originating* caller. Answered in U44 below.

## U44 result: the HAL allocates pool during interrupt-line registration

Breakpointed `ExAllocateHeapPool` (RVA 0x2369F0), one-shot per pass. Both passes
hit it, and the callers are entirely different subsystems.

**Pass 1 (control)** — `rdx=0x1000`, tag `r8=0x20206D4D` = `'Mm  '`:
```
ExAllocateHeapPool
  <- ExpAllocatePoolWithTagFromNode+0x5F
  <- MiAllocatePool+0x86
  <- MiInitializePteInfo+0xB1
  <- MiInitializeUltraSpace+0x33
  <- MiCreateTopLevelUltraMappings+0x73
```
The memory manager allocating during its own initialization, i.e. after pool
exists. Entirely normal, and it is pass 1's *first* pool allocation.

**Pass 2** — `rdx=0xAF7` (2807 bytes), tag `r8=0x0` (untagged):
```
ExAllocateHeapPool
  <- ExAllocatePoolWithTag+0x64
  <- HalpAllocPhysicalMemory+0x4B
  <- HalpInterruptRegisterLine+0xFB
```
The HAL, registering an interrupt line, during HAL init — which runs long before
`MmInitSystem` creates the pool.

Confidence: `[rsp+0x00]` is a guaranteed return address, and
`HalpAllocPhysicalMemory+0x4B` sits at `[rsp+0x10]` consistent with a real frame.
`HalpInterruptRegisterLine+0xFB` appears twice (`+0x80`, `+0xD0`), which is good
corroboration. A `KiSwInterruptDispatch+0x91` slot also appeared once; treat that
as unconfirmed — the walk scans the stack, so single hits can be stale values.

### Why this path allocates from pool

`HalpInterruptRegisterLine` has ten direct callers, and four of them are in
**`HalpPicDiscover`** (`+0x1AA`, `+0x1D5`, `+0x216`, `+0x240`), with the rest in
`HalpApicDescribeLines` / `HalpApicDescribeLocalLines`. U40 independently observed
pass 2 executing `HalpPicInitializeIoUnit`, so the PIC branch of HAL init is active
on pass 2.

`HalpAllocPhysicalMemory` chooses its allocator on a two-global test:
```
mov eax, [HalpAllocationDescriptorArraySize]
sub eax, [HalpUsedAllocDescriptors]
cmp eax, 3
jbe  <other path>                      ; few descriptors left
call HalpAllocPhysicalMemoryInternal   ; else -- the path that reaches pool
```
Pass 2's captured return address is `+0x4B`, immediately after that `call`, so the
`jbe` was **not** taken and it went through `HalpAllocPhysicalMemoryInternal`, which
ends in `ExAllocatePoolWithTag`.

Note both globals live in the freshly loaded pass-2 kernel image, so they are not
stale carry-over in the way the IOAPIC state was. The divergence is more likely
*inside* `HalpAllocPhysicalMemoryInternal`, which presumably prefers the loader
block / early pages while those are available and falls back to pool otherwise.

### Next

1. Poll `HalpAllocationDescriptorArraySize` and `HalpUsedAllocDescriptors` on both
   passes (arming-free, U33-style) and compare — this is cheap and directly tests
   whether the branch condition differs.
2. Disassemble `HalpAllocPhysicalMemoryInternal` to find its loader-block-vs-pool
   branch, and identify the state that makes pass 2 take the pool arm.
3. Establish whether pass 1 reaches `HalpInterruptRegisterLine` at all during its
   own HAL init, and if so what allocator it gets served by.

## U45 result: U44 was wrong — the allocator is a software-interrupt handler, not the HAL

**U44's pass-2 caller chain was stale-frame noise and is retracted.** Static
analysis then a reliable re-run establish the real caller.

Static refutation of U44:
- `HalpAllocPhysicalMemoryInternal` is a leaf that only carves the loader
  memory-descriptor list; it makes no calls and never touches pool. So
  `HalpAllocPhysicalMemory+0x4B` (the return site of `call
  HalpAllocPhysicalMemoryInternal`) cannot lead to `ExAllocatePoolWithTag`.
- `HalpInterruptRegisterLine`'s only allocation path is
  `HalpMmAllocateMemoryInternal -> HalpAllocPhysicalMemory` — also loader-descriptor,
  not pool. And `+0xFB` (the U44 frame) is the return of a `call memset`, not an
  allocation.
- So every frame U44 reported above `[rsp+0x00]` was stale. Only the guaranteed
  `[rsp+0x00] = ExAllocatePoolWithTag+0x64` survived, and it is verified to sit
  right after `call ExAllocateHeapPool`.

Reliable re-run: breakpoint `ExAllocatePoolWithTag`'s **entry** (RVA 0x9B7010),
where `[rsp+0x00]` is a guaranteed return address naming its true caller.

| | pass 1 (control) | pass 2 (fatal) |
|---|---|---|
| PoolType | 0x1 (paged) | 0x200 (NonPagedPoolNx) |
| Size | 0x300 | 0xAF7 |
| Tag | `'PsQt'` | 0 (obfuscated to 0 this run) |
| guaranteed caller `[rsp+0]` | `PsInitializeQuotaSystem+0x8F` | `KiSwInterruptDispatch+0x8C` |

The pass-2 caller is confirmed beyond doubt: `KiSwInterruptDispatch+0x85` is
`add rdx, 0xaf7` immediately before the `call ExAllocatePoolWithTag` at `+0x8C`,
and `0xAF7` is exactly the size captured at runtime. The static call site computes
the observed argument.

### What `KiSwInterruptDispatch` is doing

Disassembly from its entry (0x3DC890):
```
push rbp/rbx/rsi/rdi/r12-r15 ; sub rsp,0x98      ; real function prologue
test [rdi+0x994], 0x100000 ; call KeExitRetpoline  ; speculation-control gate
rdtsc                                              ; timestamp entropy
movabs rsi, 0x7010008004002001                     ; magic constant
ror/xor/mul/xor/and 0xf                             ; hash the timestamp
mov ecx, 0x200                                     ; PoolType = NonPagedPoolNx
lea rax,[rip+0x836656] ; mov r8d,[rax+hash*4]      ; Tag = obfuscated table lookup
mov rdx,[rdi+0xa90] ; add rdx,0xaf7                 ; Size = base + 0xAF7
call ExAllocatePoolWithTag
```
It has exactly one caller, `KiSwInterrupt+0x35D`, and `KiSwInterrupt` has no direct
callers — it is reached through the IDT as a **software-interrupt vector**.
`KiSwInterrupt`'s body around `+0x35D` is self-referential `call` soup (calls to
its own `+0x22E`, `+0x120`, …), i.e. deliberately obfuscated control flow.

An `rdtsc`-seeded hash driving a `NonPagedPoolNx` allocation with an obfuscated,
table-indexed tag, dispatched from an obfuscated software-interrupt handler, is the
signature of **PatchGuard / Kernel Patch Protection**, not any HAL init routine.
This is a hypothesis on the *identity*; the *mechanism* below is what is confirmed.

### The reframe

- Pass 1's first pool allocation is `PsInitializeQuotaSystem` (normal init
  sequence), at discovery+4817 exits.
- Pass 2's first pool allocation is this software-interrupt handler, at
  discovery+272 exits — far earlier relative to discovery, and off the normal init
  sequence entirely.

So on pass 2 a **software interrupt fires very early and its handler allocates
NonPagedPool before pool exists**. The 0x139 is the correct fastfail on that
premature allocation. The open question is no longer "who allocates" but **why this
software interrupt fires during pass-2 early boot when pass 1 does not take it
then** — which points back at interrupt/APIC state our port-0x64/0xFE reset leaves
behind (a pending software interrupt or IRR/ISR bit), consistent with this whole
project's recurring interrupt-delivery theme.

## U46 result: the reset leaves a dirty LAPIC, but nothing is pending

The LAPIC is WHP-emulated (`WHvX64LocalApicEmulationModeXApic`), so its state lives
in the virtual processor. Our port-0x64/0xFE reset calls only
`WHvSetVirtualProcessorRegisters` for CPU registers — it never touches the LAPIC.
Read the xAPIC register page (`WHvGetVirtualProcessorInterruptControllerState`) at
the reset, i.e. the state pass 2 inherits:

```
TPR=0xF0  PPR=0x0  SVR=0x1DF
LVT Timer=0x300FD (vector 0xFD, PERIODIC, masked, InitCnt=0 CurCnt=0)
IRR: no pending vectors
```

Two readings:
- **Negative:** IRR is clean. No interrupt is pending at the reset, so the pass-2
  software interrupt is *raised during pass 2*, not inherited-pending. The
  "surviving pending interrupt" form of the hypothesis is disproved.
- **Positive:** the LAPIC is nonetheless dirty. A real reset clears it to cold-boot
  state — APIC disabled (`SVR` bit 8 clear), `TPR=0`, LVTs masked with vector 0.
  Ours leaves pass 1's whole configuration: APIC **software-enabled**, `TPR=0xF0`
  (HIGH_LEVEL, pass 1's post-bugcheck `HaliHaltSystem` priority), timer armed at
  vector 0xFD periodic. Pass 2 begins with a warm LAPIC where a cold boot expects a
  reset one.

(The at-bugcheck LAPIC read returned `0x80370308` — the VP state is not readable at
that instant. Not pursued; the reset snapshot is the informative one.)

### Next

1. **U46b — decisive timing test.** One-shot breakpoint `KiSwInterruptDispatch`
   (0x3DC890) on *both* passes, logging exitCount relative to discovery and whether
   `ExPoolState` pooldesc+0x10 is populated (pool exists) at that instant. This
   separates the two live explanations:
   - the software interrupt fires *pass-2-specifically early* (pass 1 does not take
     it at that point), vs
   - it is normal early-boot code that runs in both passes before pool init, and
     pass 1 only survives because its pool init is reached and pass 2's is not.
2. Given the dirty-LAPIC finding, test clearing the LAPIC to cold-boot state in the
   port-0x64/0xFE handler (`WHvSetVirtualProcessorInterruptControllerState`),
   measured with the U40/U42/U45/U46 instruments — well-motivated as "make the warm
   reset clear the interrupt controller the way real hardware does," independent of
   whether it fixes the 0x139.

## U46b result: the software interrupt is pass-2-specific, and fires with pool absent

One-shot `KiSwInterruptDispatch` (0x3DC890) on both passes:
- **Pass 1: never hit.** No `pass=1` entry across the whole ~1M-exit window from
  pass-1 discovery to the reset. `KiSwInterruptDispatch` does not execute on pass 1
  in the observed window — so this is *not* normal early-boot code both passes run.
- **Pass 2: hit at reset+165744, pool absent.** `pooldesc+0x10 = 0` at the hit, i.e.
  the allocation this handler is about to make cannot succeed — the exact crash
  condition. Guaranteed caller `KiSwInterrupt+0x362` (return after the `+0x35D`
  call), consistent with U45.

So pass 2 takes a software interrupt that pass 1 does not. Combined with U46: pass 2
inherited an LVT timer programmed to **vector 0xFD, periodic** from pass 1, and a
warm, software-enabled LAPIC. The leading mechanism is that this inherited APIC
timer (or the warm LAPIC generally) drives an early interrupt into `KiSwInterrupt`
on pass 2, whose handler allocates `NonPagedPool` before pool init has run.

## U47 result: FIXED. Quiescing the LAPIC at reset resolves the pass-2 0x139

`u47ResetLapic` was added to the port-0x64/0xFE handler: read-modify-write the
xAPIC page to mask every LVT, stop and zero the timer, clear ISR/TMR/IRR and ICR,
and drop TPR to 0 — quiescing the local APIC the way a real hardware reset does.

Result, reproduced **4/4 runs** (the RAM-clear attempt that this project previously
tried was only 1/3, so reproducibility was checked deliberately):

- **No `0x139`.** The KERNEL_SECURITY_CHECK_FAILURE that blocked pass 2 for the
  entire U15→U46b investigation does not occur.
- **`KiSwInterruptDispatch` never fires on pass 2.** With the inherited armed timer
  gone, the early software interrupt is not raised, so its premature pool allocation
  never happens.
- **Pool init now runs on pass 2.** The U33 tracker shows `pooldesc+0x08` and
  `+0x10` both go null→set (`INIT`) at ~reset+145k — `MmInitSystem` executes, which
  it never did before.
- **Pass 2 boots ~442k exits and triggers a second reset** (Setup's normal next
  reboot), i.e. real forward progress into the next install phase.

The only remaining bugcheck is the pre-existing pass-1 `0xA5` (ACPI_BIOS_ERROR),
documented in U21 as benign — Setup continues past it, unchanged by this fix.

### Root cause, end to end

1. Our port-0x64/0xFE warm reset restored CPU registers only and left the
   WHP-emulated LAPIC untouched (U46).
2. So pass 2 inherited pass 1's LAPIC: software-enabled, `TPR=0xF0`, LVT timer armed
   at vector 0xFD periodic.
3. That inherited timer drove an early software interrupt into `KiSwInterrupt` →
   `KiSwInterruptDispatch` on pass 2, which pass 1 never takes at that point (U46b).
4. `KiSwInterruptDispatch` allocates `NonPagedPool` — but this fired before
   `MmInitSystem`/pool-heap init had run, so the pool descriptor's `+0x10` (LFH
   context root) was still null (U31–U33, U45).
5. The null context yielded a null LFH owner → `subseg=0` → the LIST_ENTRY fastfail
   in `RtlpHpLfhOwnerMoveSubsegment` → `KeBugCheckEx(0x139, 3)` (U22).

The 0x139 / null-pool-descriptor chain was a *symptom*; the root cause was the warm
reset failing to quiesce the interrupt controller. Every earlier "downward" trace
(U15–U33) was following the symptom; U40's arming-free sampler, U44/U45's reliable
caller walk, and U46/U46b's LAPIC snapshot walked it back up to the cause.

### Follow-up (not blocking)

- The fix is minimal (`u47ResetLapic` + one call). The heavy diagnostics
  (U33/U40/U42/U43/U46 logging, the DR0 one-shot) should be gated behind a debug
  flag for a clean build.
- Consider whether the reset should also fully cold-reset the LAPIC (disable via SVR)
  rather than only quiesce; the quiesce is sufficient for the 0x139 and lower-risk.

## U48: fix is durable across 41 boots, but Setup still cannot install

Long run (~9 minutes, 32M exits) to see how far Setup gets with the U47 fix in.

**The U47 fix holds, durably:**

| | |
|---|---|
| kernel boots (discovery) | 41 |
| `0x139` bugchecks | **0** |
| pool INIT edges (`MmInitSystem` ran) | 40 — every post-reset boot |

Pool init running on *every* boot is the strongest confirmation yet: it never
happened once across U32-U46b.

**But the guest is in a clean reboot loop and installs nothing.** Every boot is the
same cycle:

```
reset -> pool init (+~150k exits) -> bugcheck 0xA5 (+~213k) -> ~750k exits -> reset
```

- **40/40 boots bugcheck `0xA5` (ACPI_BIOS_ERROR, p1=0x11).**
- **Zero AHCI write commands** across the whole run — the guest has never written a
  sector, so nothing has ever been installed.

### Correction to U21

U21 recorded the pass-1 `0xA5` as benign, reasoning that Setup "keeps running" for
~876k exits afterwards. This run shows those following exits are the
bugcheck/restart sequence, not Setup working: the reset always follows the `0xA5` at
a consistent ~750k-exit distance. The `0xA5` is what drives the reboot loop, and it
fires on **every** boot including pass 1 — it was never pass-2-specific. Fixing the
`0x139` did not reveal a working installer; it revealed the next blocker, which the
crash had been hiding.

### Next blocker: our DSDT has no AML namespace

From `acpiBuildTables` in Hypervisor.c:

```
// DSDT at blob offset 272 (36 bytes -- header only, no AML namespace
```

We present a **36-byte DSDT: a bare header with no namespace at all** — no `\_SB_`,
no `PCI0` device, no `_HID`/`_CID`/`_ADR`/`_CRS`/`_PRT`, no processor objects.
Windows' `ACPI.sys` needs a real namespace to enumerate the platform, and
`ACPI_BIOS_ERROR` is what it raises when it cannot. This matches the pre-existing
note in the source about "absent a real AML `_PRT` since our DSDT has no namespace
content", and is consistent with the `0x5C` HAL_INITIALIZATION_FAILED history that
led to the second IOAPIC being added.

**U49 (next project):** author a minimal valid DSDT — `\_SB_.PCI0` with
`_HID`/`_CID`/`_ADR`/`_CRS` and a `_PRT` describing PCI interrupt routing, plus a
processor object — compile to AML (`iasl`), and embed it in place of the header-only
table. Bounded, well-understood work rather than an open investigation.
2. Check whether `[rcx+0x10]` (the cached IOAPIC window) is non-zero on pass 2 at
   `+0x3B`, i.e. whether `HalMapIoSpace` is being skipped.
3. Get an exit-independent view of where pass 2 actually is. Options: single-step
   via `WHvRunVirtualProcessor`'s trap flag over a bounded window, or breakpoint
   `KiSystemStartup`/`KiInitializeKernel` (whose entries may still be pre-discovery,
   in which case only the sampler-independent approach works).
