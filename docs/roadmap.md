# Backend Roadmap

Grounded in the current implementation state of `Hypervisor.c` as of this
writing:

**Implemented:** ATA + AHCI storage (over an emulated PCI bus), dual IOAPIC
(GSI 0-47) with redirection-table-aware interrupt delivery via
`ioapicResolveVector()`, PIT channel 2, CMOS/RTC including periodic
interrupt emulation (Register A/B/C), dual 16550 UART (COM1 as a log, COM2
bridged for kernel debugging), PS/2 keyboard + mouse, RTL8139 NIC with a
self-contained slirp-style NAT backend (ARP/ICMP/DHCP/DNS relay/UDP+TCP
NAT), a minimal fw_cfg device + ACPI table set (XSDT/FADT/MADT/DSDT/DBG2),
a `ramfb`-style framebuffer rendered via `StretchDIBits`, and both UEFI
(OVMF) and legacy boot paths.

**Not implemented:** USB, audio, VMBus/Guest Tools (clipboard, shared
folders, dynamic display resizing). The VPPT/Hyper-V-timer boot blocker
(see [`known-limitations.md`](known-limitations.md)) is **RESOLVED** as of
2026-07-16 — it was never a SynIC gap; the real cause was missing RTC
periodic-interrupt emulation plus two gaps in this hypervisor's own
interrupt-delivery timing (see the investigation doc's part 10 update).
The guest now boots past this point, triggers a legacy keyboard-controller
system reset (port `0x64`=`0xFE`, likely Windows Setup's mid-installation
restart) which this hypervisor now honors reliably (CPU-register reset,
confirmed across every test run), but the second boot pass then hits a
**still-unresolved** bugcheck `0x139` (`KERNEL_SECURITY_CHECK_FAILURE`,
Segment Heap list corruption) in most runs. Four targeted mitigation
attempts were tried and disproven by live testing; see
[`investigations/post-vppt-boot-stall.md`](investigations/post-vppt-boot-stall.md)
for the full trial record. This is an open problem, not solved.

## Priority-ordered recommendations

### 1. PS/2 mouse — done

Implemented and verified end-to-end (real synthesized Windows input driving
the actual `WM_MOUSEMOVE`/button messages through to correct PS/2 packets).

### 2. Network device — done

RTL8139 + slirp-style NAT backend implemented and verified with real
internet traffic (live DNS resolution, a real HTTP request/response over
TCP). See `docs/investigations` history and the RTL8139 device code for
detail. The device and backend are cleanly separated behind
`rtl8139TransmitFrame`/`rtl8139ReceiveFrame` (device) and a swappable
`g_netTransmit` function pointer (backend), so a future bridged or
host-only backend can be added without touching the device code.

### 3. Guest Tools / driver package — the multiplier

Becomes necessary as soon as any device without an in-box Windows driver
exists (starting with network, later storage/balloon if virtio is chosen
there too). This is driver-signing and packaging work as much as backend
emulation, but it's what unlocks clipboard integration, dynamic display
resizing, and shared folders later — worth scoping early even if built
incrementally alongside item 2, rather than bolted on afterward.

### 4. USB controller — defer unless a concrete need appears

Keyboard and mouse (once item 1 lands) are covered by PS/2, and storage by
AHCI, so USB isn't blocking "boot and use an unmodified OS" the way
network is. The xHCI spec is large; if USB becomes necessary, prefer an
older, smaller controller (UHCI/OHCI/EHCI) sized to the actual guest need
rather than building full xHCI up front.

### 5. Audio — lowest priority

Nice-to-have, not blocking for a general-purpose "boot and use unmodified
OS" tool. Revisit after 1-3 are solid.

## Falsification experiments for the SynIC conclusion — completed 2026-07-16

Both experiments below have been run; see
`investigations/vppt-synic-blocker.md`'s "part 2" update for full detail.
Kept here for the historical record of what was planned and why.

**A. Complete the synthetic-feature flag set and retest.** Added the
previously-omitted `DirectSyntheticTimers`/`SyntheticClusterIpi` bits (both
of which QEMU's WHPX accelerator sets alongside the rest of the bank) to
the earlier `SyntheticProcessorFeaturesBanks` experiment and retested.
**Result: identical unbounded hang**, same RIP/register signature as
before — completing the flag set changed nothing. Reverted.

**B. Determine what's actually failing, safely.** Rather than the
originally-planned live INT3 call-site tracing (which would have required
new WHV exception-interception infrastructure and precise timing to plant
breakpoints before a randomized-load-base function ran — real engineering
cost and correctness risk), used a cheaper, zero-risk alternative: widened
the existing bugcheck `param2` memory dump and decoded a specific bit
(descriptor offset `+0xB8`, bit 6) already known from disassembly to mark
successful completion of the *entire* VPPT main-clock init path, including
the legacy IOAPIC interrupt-remap step. **Result, reproduced identically
across two independent boots: that bit is set** — the VPPT/Hyper-V timer
appears to *enable* successfully at the hardware level. Cross-referencing
the actual `KeBugCheckEx` call site's disassembly (double-checked after an
initial mis-attribution to `HalpTimerInitializeProfiling`) shows it's
reached from `HalpTimerInitializeClock`, which calls
`HalpTimerConfigureInterrupt` to route the main clock-tick interrupt
(vector `0xD3`) on that same already-enabled timer — not from the
SynIC-dependent enable path at all.

**Outcome:** the two experiments did not agree (A: no change; B:
substantive reframing), so per the original protocol this does not clear
the bar for committing to a full SynIC implementation. Next step is
narrowing down what `HalpTimerConfigureInterrupt`'s interrupt-remap call
specifically needs that our IOAPIC emulation doesn't currently provide —
likely a smaller, more conventional target than a SynIC stack, though not
yet root-caused to a specific fix.

**Update 2026-07-16, part 4:** fully traced that interrupt-remap call
chain on the actual path the guest takes (legacy GSI/IOAPIC fallback — MSI
is never attempted since descriptor `+0xE0` bit 10 is clear). It bottoms
out at `HalpInterruptSetLineStateInternal` calling through a runtime
function pointer at the emulated IOAPIC's own controller-object `+0x70`
offset; a negative return there propagates unmodified up to the observed
bugcheck. That target function's address is not yet resolved (it's set at
runtime, not a fixed symbol) — resolving it and disassembling it is the
next concrete step. See `investigations/vppt-synic-blocker.md`'s "part 4"
update for the full chain.

**Update 2026-07-16, part 7 (superseded by part 9):** resolved the
controller `+0x70` pointer live and traced a chain down to a literal
`mov ecx, 0xC0000001` inside `HalpInterruptFindBestRouting`. Part 8 then
read the controller's routing-candidate list live and found it always
non-empty (exactly one entry per controller), which per the function's
own logic means it can never actually return that failure for us — this
whole chain (parts 4-7) turned out to be confirmed-*working* code, not
the failure source. Left here for the historical record; read part 9 for
what's actually happening.

**Update 2026-07-16, part 9 — actual root cause confirmed:** dumped
`HalpTimerInitializeClock`'s own hot path for the first time (RVA
`0x3AFBF4`) and found the real flow: `HalpTimerConfigureInterrupt`
(vector `0xD1`, corrected from an earlier wrong `0xD3` attribution) and
`HalpSetTimer` both succeed, but `HalpTimerWaitForPhase0Interrupt` times
out after busy-polling for 3 real seconds waiting for the just-configured
interrupt's ISR to signal it fired. **The interrupt is never delivered.**
This is a genuine interrupt-delivery gap — something in WHV injection,
IOAPIC redirection-entry programming, or LAPIC vector delivery for this
specific vector/line — not a capability, routing, topology, or SynIC gap.
Next concrete step: figure out why vector `0xD1`, successfully configured
on our emulated IOAPIC, never reaches the guest's ISR. See
`investigations/vppt-synic-blocker.md`'s "part 9" update for the full
instruction-level chain.

**Update 2026-07-16, part 10 — FIXED.** Vector `0xD1` is the RTC's IRQ8,
and this hypervisor never emulated RTC periodic interrupts at all.
Implemented that (Register A/B/C), routed through the guest's own IOAPIC
programming, and fixed two supporting gaps in interrupt-delivery timing
(delivery during busy-spins, and forcing VM exits via
`WHvCancelRunVirtualProcessor` so a CPU-bound guest spin can't starve
interrupt delivery entirely). Confirmed: bugcheck 0x5C no longer occurs,
reproduced across independent boots. See part 10 in the investigation doc
for the full fix and verification.
