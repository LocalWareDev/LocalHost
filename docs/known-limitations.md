# Known Limitations — WHV Backend

## RESOLVED: Windows guests bugchecked during HAL timer init (0x5C)

**Status: fixed 2026-07-16, confirmed reproducible across independent
boots.** Previously, real Windows installer/boot environments crashed
with `BUGCHECK 0x5C (HAL_INITIALIZATION_FAILED)` while HAL was bringing
up its timer. The investigation went through several incorrect
hypotheses before landing on the real cause — full history in
[`docs/investigations/vppt-synic-blocker.md`](investigations/vppt-synic-blocker.md),
worth reading if a related timer/interrupt issue ever resurfaces.

**Root cause:** HAL's phase-0 timer-configuration test configures a real
RTC (Real-Time Clock, legacy IRQ8) periodic interrupt and busy-waits up
to 3 seconds for it to actually fire. This hypervisor emulated RTC
time/date *register reads* but had **no periodic-interrupt emulation at
all** — Register A rate selection, Register B's PIE bit, and Register C
status/ack didn't exist, so the interrupt HAL waited for could never
arrive. This had nothing to do with SynIC, Hyper-V enlightenment, or
interrupt-controller routing/capability checks — all of those were
confirmed working correctly during the investigation.

**The fix** (`Hypervisor.c`): implemented real RTC periodic-interrupt
emulation, delivered through the guest's own IOAPIC redirection-table
programming (a new `ioapicResolveVector()` helper, generally reusable for
other legacy IRQs later). This surfaced two further, more fundamental
gaps in this hypervisor's interrupt-delivery architecture, both also
fixed: interrupts were previously only ever delivered while the guest was
halted (`HLT`), never during a CPU-bound busy-spin wait — and even after
fixing that, a pure busy-spin with no trapped I/O could occupy an entire
`WHvRunVirtualProcessor` call with zero VM exits, so a background thread
now periodically calls `WHvCancelRunVirtualProcessor` to force the main
loop to regain control while a periodic interrupt is armed.

- **Confirmed on:** Windows 10 22H2 installer environment, UEFI/OVMF boot
  path.
- **Full investigation, evidence, and disassembly:**
  [`docs/investigations/vppt-synic-blocker.md`](investigations/vppt-synic-blocker.md)
  (see part 10 for the fix itself).

The guest now progresses past this point to a new, later, different
stall — a separate, not-yet-investigated problem. See
[`docs/roadmap.md`](roadmap.md).
