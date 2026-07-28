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
1b. Test the inherited-unmasked-RTE hazard: log which vector is actually injected
   for GSI 8 during pass 2 before HAL init re-masks it. If 0xD1 is being delivered
   into firmware, restoring the redirection tables to power-on default at reset
   becomes well-motivated — and worth retrying *with* the U40/U42 instruments,
   since the earlier attempt's "made things worse" verdict is unsound (see above).
2. Check whether `[rcx+0x10]` (the cached IOAPIC window) is non-zero on pass 2 at
   `+0x3B`, i.e. whether `HalMapIoSpace` is being skipped.
3. Get an exit-independent view of where pass 2 actually is. Options: single-step
   via `WHvRunVirtualProcessor`'s trap flag over a bounded window, or breakpoint
   `KiSystemStartup`/`KiInitializeKernel` (whose entries may still be pre-discovery,
   in which case only the sampler-independent approach works).
