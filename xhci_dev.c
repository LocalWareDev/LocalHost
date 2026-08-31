// xHCI (USB 3.x host controller) -- PCI 0:4.0, GSI 20.
//
// WHY THIS EXISTS, replacing the EHCI model:
// EHCI's driver learns about completions by READING STATUS REGISTERS. Measured
// on this guest, that was ~10000 MMIO accesses per second to USBCMD (+0x20) and
// USBSTS (+0x24) to serve a 62Hz tablet -- about 160 register reads per useful
// report, and ~43% of ALL VM exits. Every one is a trapped, instruction-decoded
// exit, and the guest is CPU-saturated (halted=0ms, 0.0% idle).
//
// xHCI inverts that. The controller writes completion EVENTS into a ring in
// GUEST MEMORY and raises one interrupt; the driver reads those events out of
// RAM, which never traps. Doorbell writes replace schedule polling, and
// interrupt moderation (IMOD) is part of the spec rather than something we have
// to invent. The register traffic that dominates our exit profile is structural
// to EHCI and simply absent here.
//
// LAYOUT. BAR0 is 4KB, left unmapped so every access faults out to
// xhciHandleMmio. Offsets within it:
//     0x000  capability registers (CAPLENGTH..HCCPARAMS1)
//     0x020  operational registers (USBCMD..PORTSC), CAPLENGTH says so
//     0x600  runtime registers (IMAN/IMOD/ERSTSZ/ERSTBA/ERDP), RTSOFF says so
//     0x800  doorbell array, DBOFF says so
//
// SCOPE. One root port, one interrupter, one slot -- exactly enough to carry the
// HID tablet that EHCI carries today. Everything the spec allows us to omit for
// that (streams, multiple interrupters, bandwidth negotiation, USB3 SuperSpeed
// port pairs) is omitted deliberately rather than forgotten.

#define XHCI_BAR_SIZE     0x1000
#define XHCI_CAPLENGTH    0x20      // operational registers start here
#define XHCI_RTSOFF       0x600     // runtime registers
#define XHCI_DBOFF        0x800     // doorbell array
#define XHCI_MAX_SLOTS    1         // one device: the tablet
#define XHCI_MAX_PORTS    1
#define XHCI_ERST_MAX     1         // one event ring segment

// --- operational register offsets, relative to BAR + CAPLENGTH ---
#define XHCI_OP_USBCMD    0x00
#define XHCI_OP_USBSTS    0x04
#define XHCI_OP_PAGESIZE  0x08
#define XHCI_OP_DNCTRL    0x14
#define XHCI_OP_CRCR      0x18      // 64-bit
#define XHCI_OP_DCBAAP    0x30      // 64-bit
#define XHCI_OP_CONFIG    0x38
// Port register sets start at operational offset 0x400, NOT 0x40 -- the spec
// puts them at 400h + 10h*(n-1), well clear of the other operational registers.
// Writing 0x40 here put PORTSC in reserved space: Windows read our connect
// status from an address that means nothing, found zeros where the real port
// should be, and never advanced to arming the event ring (it never even read
// RTSOFF or DBOFF -- 183 polls of USBCMD and 180 of a reserved offset, then
// nothing).
#define XHCI_OP_PORTSC    0x400     // port 1 register set base

// USBCMD bits
#define XHCI_CMD_RUN      0x00000001
#define XHCI_CMD_HCRST    0x00000002
#define XHCI_CMD_INTE     0x00000004

// USBSTS bits
#define XHCI_STS_HCH      0x00000001   // HCHalted
#define XHCI_STS_HSE      0x00000004
#define XHCI_STS_EINT     0x00000008   // Event Interrupt
#define XHCI_STS_PCD      0x00000010   // Port Change Detect
#define XHCI_STS_CNR      0x00000800   // Controller Not Ready

// PORTSC bits
#define XHCI_PORTSC_CCS   0x00000001   // Current Connect Status
#define XHCI_PORTSC_PED   0x00000002   // Port Enabled/Disabled
#define XHCI_PORTSC_PR    0x00000010   // Port Reset
#define XHCI_PORTSC_PP    0x00000200   // Port Power
#define XHCI_PORTSC_CSC   0x00020000   // Connect Status Change
#define XHCI_PORTSC_PEC   0x00040000   // Port Enabled/Disabled Change
#define XHCI_PORTSC_PRC   0x00200000   // Port Reset Change

// TRB types (control dword bits 15:10)
#define TRB_NORMAL             1
#define TRB_SETUP_STAGE        2
#define TRB_DATA_STAGE         3
#define TRB_STATUS_STAGE       4
#define TRB_LINK               6
#define TRB_ENABLE_SLOT        9
#define TRB_DISABLE_SLOT      10
#define TRB_ADDRESS_DEVICE    11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVALUATE_CONTEXT  13
#define TRB_RESET_ENDPOINT    14
#define TRB_NOOP_CMD          23
#define TRB_TRANSFER_EVENT    32
#define TRB_COMMAND_COMPLETE  33
#define TRB_PORT_STATUS_EVENT 34

// completion codes
#define XHCI_CC_SUCCESS        1
#define XHCI_CC_TRB_ERROR      5
#define XHCI_CC_SHORT_PACKET   13

UINT32 xhciBarBase = 0;          // guest-programmed BAR0 GPA
int    xhciBarMapped = 0;
int    xhciBarSizing = 0;

// operational state
UINT32 xhciUsbCmd = 0;
// HCH only -- NOT CNR. Controller Not Ready means "still initialising after
// power-on"; an emulated controller is ready the instant it exists. Setting it
// at init cost a full debug cycle: the driver wrote USBCMD=0 (not a reset), then
// polled USBSTS forever waiting for CNR to drop, and the only place that cleared
// it was the HCRST path the driver had no reason to take. The trace showed
// R +0x024 -> 0x00000801 repeating without end.
UINT32 xhciUsbSts = XHCI_STS_HCH;
UINT32 xhciDnctrl = 0;
UINT64 xhciCrcr = 0;             // command ring control (pointer + flags)
UINT64 xhciDcbaap = 0;
UINT32 xhciConfig = 0;
UINT32 xhciPortsc = XHCI_PORTSC_PP;   // powered, nothing attached yet

// runtime / interrupter 0
UINT32 xhciIman = 0, xhciImod = 0, xhciErstsz = 0;
UINT64 xhciErstba = 0, xhciErdp = 0;

// our own ring bookkeeping
UINT64 xhciCmdRingPtr = 0;       // dequeue pointer into the guest's command ring
int    xhciCmdCcs = 1;           // consumer cycle state for the command ring
UINT64 xhciEventRingBase = 0;    // first (only) event ring segment
UINT32 xhciEventRingSize = 0;    // in TRBs
UINT64 xhciEventEnqueue = 0;     // where WE write the next event
int    xhciEventPcs = 1;         // producer cycle state
int    xhciSlotId = 0;           // the one slot we hand out
UINT64 xhciDevContext = 0;       // that slot's device context GPA
int    xhciDeviceAddressed = 0;
int    xhciPortAttached = 0;     // have we reported the tablet's arrival yet

// counters, reported from the heartbeat like every other device here
long g_xhciMmioReads = 0, g_xhciMmioWrites = 0;
long g_xhciCmds = 0, g_xhciEvents = 0, g_xhciIrqs = 0;
long g_xhciTransfers = 0, g_xhciDoorbells = 0;

// --- guest memory helpers -------------------------------------------------
// Same bounds discipline as the EHCI model: a malformed pointer from the guest
// must never walk off the end of the mapping.
static UINT32 xhciRead32(UINT64 gpa) {
    if (!guestMemory || gpa + 4 > guestMemSize) return 0;
    return *(UINT32 *)((unsigned char *)guestMemory + gpa);
}
static UINT64 xhciRead64(UINT64 gpa) {
    if (!guestMemory || gpa + 8 > guestMemSize) return 0;
    return *(UINT64 *)((unsigned char *)guestMemory + gpa);
}
static void xhciWrite32(UINT64 gpa, UINT32 v) {
    if (!guestMemory || gpa + 4 > guestMemSize) return;
    *(UINT32 *)((unsigned char *)guestMemory + gpa) = v;
}

// --- event ring -----------------------------------------------------------
// Push one event TRB and raise the interrupt. The cycle bit is what tells the
// driver an entry is valid, so it MUST be written last: publish the payload,
// then the cycle. Same ordering hazard as the RTL8139 receive ring, where
// writing the "here is a packet" marker before the packet let the guest read a
// header over stale bytes.
static void xhciPushEvent(WHV_PARTITION_HANDLE partition,
                          UINT64 param, UINT32 status, UINT32 control) {
    if (!xhciEventRingBase || !xhciEventRingSize) return;
    UINT64 trb = xhciEventEnqueue;
    if (trb + 16 > guestMemSize) return;

    control = (control & ~1u) | (xhciEventPcs ? 1u : 0u);
    // payload first
    xhciWrite32(trb + 0, (UINT32)(param & 0xFFFFFFFF));
    xhciWrite32(trb + 4, (UINT32)(param >> 32));
    xhciWrite32(trb + 8, status);
    MemoryBarrier();
    // then the word carrying the cycle bit
    xhciWrite32(trb + 12, control);
    g_xhciEvents++;

    // advance, wrapping the single segment and toggling the cycle
    xhciEventEnqueue += 16;
    if (xhciEventEnqueue >= xhciEventRingBase + (UINT64)xhciEventRingSize * 16) {
        xhciEventEnqueue = xhciEventRingBase;
        xhciEventPcs = !xhciEventPcs;
    }

    xhciUsbSts |= XHCI_STS_EINT;
    xhciIman |= 1;                       // Interrupt Pending
    if ((xhciUsbCmd & XHCI_CMD_INTE) && (xhciIman & 2)) {   // INTE and IE
        g_xhciIrqs++;
        injectDeviceIrq(partition, GSI_EHCI, 0x73 /* unused here; routed */);
    }
}

// --- command ring ---------------------------------------------------------
static void xhciCompleteCommand(WHV_PARTITION_HANDLE partition,
                                UINT64 cmdTrb, int completionCode, int slotId) {
    xhciPushEvent(partition, cmdTrb,
                  ((UINT32)completionCode << 24),
                  ((UINT32)TRB_COMMAND_COMPLETE << 10) | ((UINT32)slotId << 24));
}

static void xhciRunCommandRing(WHV_PARTITION_HANDLE partition) {
    int guard = 0;
    while (xhciCmdRingPtr && guard++ < 32) {
        UINT32 c3 = xhciRead32(xhciCmdRingPtr + 12);
        int cycle = c3 & 1;
        if (cycle != xhciCmdCcs) break;          // not ours yet -- ring is empty
        int type = (c3 >> 10) & 0x3F;
        UINT64 thisTrb = xhciCmdRingPtr;
        g_xhciCmds++;

        switch (type) {
            case TRB_LINK: {
                UINT64 next = xhciRead64(thisTrb) & ~0xFULL;
                if (c3 & 0x2) xhciCmdCcs = !xhciCmdCcs;   // Toggle Cycle
                xhciCmdRingPtr = next;
                continue;                                  // no event for Link
            }
            case TRB_ENABLE_SLOT:
                xhciSlotId = 1;
                xhciCompleteCommand(partition, thisTrb, XHCI_CC_SUCCESS, xhciSlotId);
                break;
            case TRB_DISABLE_SLOT:
                xhciSlotId = 0; xhciDeviceAddressed = 0;
                xhciCompleteCommand(partition, thisTrb, XHCI_CC_SUCCESS, 0);
                break;
            case TRB_ADDRESS_DEVICE: {
                // The input context's slot/EP0 contexts are at the TRB pointer.
                // We do not police them: there is exactly one device and one
                // configuration, so anything the driver asks for is what it gets.
                UINT64 inputCtx = xhciRead64(thisTrb) & ~0xFULL;
                int slot = (c3 >> 24) & 0xFF;
                if (xhciDcbaap && slot) {
                    xhciDevContext = xhciRead64(xhciDcbaap + (UINT64)slot * 8) & ~0xFULL;
                }
                (void)inputCtx;
                xhciDeviceAddressed = 1;
                xhciCompleteCommand(partition, thisTrb, XHCI_CC_SUCCESS, slot);
                break;
            }
            case TRB_CONFIGURE_ENDPOINT:
            case TRB_EVALUATE_CONTEXT:
            case TRB_RESET_ENDPOINT:
            case TRB_NOOP_CMD:
                xhciCompleteCommand(partition, thisTrb, XHCI_CC_SUCCESS,
                                    (c3 >> 24) & 0xFF);
                break;
            default:
                // Unknown command. Completing it with an error is far better
                // than ignoring it: the driver waits on a completion event and
                // would otherwise hang forever, which is exactly how the EHCI
                // model's missing NAK handling used to stall the queue.
                xhciCompleteCommand(partition, thisTrb, XHCI_CC_TRB_ERROR,
                                    (c3 >> 24) & 0xFF);
                break;
        }
        xhciCmdRingPtr += 16;
    }
}

// --- port ------------------------------------------------------------------
// Report the tablet as already attached once the driver starts the controller.
static void xhciAttachPort(WHV_PARTITION_HANDLE partition) {
    if (xhciPortAttached) return;
    xhciPortAttached = 1;
    xhciPortsc |= XHCI_PORTSC_CCS | XHCI_PORTSC_CSC | XHCI_PORTSC_PED;
    xhciUsbSts |= XHCI_STS_PCD;
    // Port Status Change Event: parameter carries the port number in 31:24.
    xhciPushEvent(partition, ((UINT64)1 << 24), ((UINT32)XHCI_CC_SUCCESS << 24),
                  ((UINT32)TRB_PORT_STATUS_EVENT << 10));
}

// --- MMIO ------------------------------------------------------------------
UINT64 xhciRegRead(UINT32 off, UINT32 size) {
    UINT64 v = 0;
    g_xhciMmioReads++;
    if (off < XHCI_CAPLENGTH) {
        switch (off) {
            case 0x00: v = XHCI_CAPLENGTH | (0x0100u << 16); break;  // CAPLENGTH + HCIVERSION 1.0
            case 0x04: v = (XHCI_MAX_SLOTS) | (1u << 8) | ((UINT32)XHCI_MAX_PORTS << 24); break; // HCSPARAMS1
            case 0x08: v = 0; break;                                  // HCSPARAMS2
            case 0x0C: v = 0; break;                                  // HCSPARAMS3
            // HCCPARAMS1. AC64=0 (32-bit addressing), CSZ=0 (32-byte contexts),
            // and xECP in bits 31:16 pointing at the extended capability list in
            // DWORD units: 0x500/4 = 0x140.
            //
            // xECP is NOT optional in practice. The Supported Protocol capability
            // is how the driver learns which ports speak USB2 and which speak
            // USB3; with xECP=0 there is no such list, so there is no port it is
            // willing to use, and it stops after resetting the controller --
            // which is exactly what happened here (reset logged, then silence,
            // setups=0 configured=0).
            case 0x10: v = (0x140u << 16); break;
            case 0x14: v = XHCI_DBOFF; break;
            case 0x18: v = XHCI_RTSOFF; break;
            default: v = 0; break;
        }
    } else if (off >= 0x500 && off < 0x520) {
        // Extended capabilities: a single Supported Protocol capability (ID 2)
        // declaring port 1 as USB 2.00. Our tablet is declared high-speed, so
        // USB2 is the protocol that matches it; advertising USB3 here would
        // invite the driver to expect SuperSpeed link training we do not model.
        switch (off) {
            // ID=2, next=0 (last in the list), minor=0x00, major=0x02
            case 0x500: v = 0x02000002u; break;
            case 0x504: v = 0x20425355u; break;   // "USB " name string
            // compatible port offset 1, count 1, no protocol speed IDs
            case 0x508: v = 1u | (1u << 8); break;
            case 0x50C: v = 0; break;             // protocol slot type 0
            default: v = 0; break;
        }
    } else if (off >= XHCI_DBOFF) {
        v = 0;                                   // doorbells read as zero
    } else if (off >= XHCI_RTSOFF) {
        UINT32 r = off - XHCI_RTSOFF;
        switch (r) {
            case 0x20: v = xhciIman; break;
            case 0x24: v = xhciImod; break;
            case 0x28: v = xhciErstsz; break;
            case 0x30: v = (UINT32)(xhciErstba & 0xFFFFFFFF); break;
            case 0x34: v = (UINT32)(xhciErstba >> 32); break;
            case 0x38: v = (UINT32)(xhciErdp & 0xFFFFFFFF); break;
            case 0x3C: v = (UINT32)(xhciErdp >> 32); break;
            default: v = 0; break;
        }
    } else {
        UINT32 r = off - XHCI_CAPLENGTH;
        switch (r) {
            case XHCI_OP_USBCMD:   v = xhciUsbCmd; break;
            case XHCI_OP_USBSTS:   v = xhciUsbSts; break;
            case XHCI_OP_PAGESIZE: v = 1; break;         // 4KB pages
            case XHCI_OP_DNCTRL:   v = xhciDnctrl; break;
            case XHCI_OP_CRCR:     v = (UINT32)(xhciCrcr & 0xFFFFFFFF); break;
            case XHCI_OP_CRCR + 4: v = (UINT32)(xhciCrcr >> 32); break;
            case XHCI_OP_DCBAAP:   v = (UINT32)(xhciDcbaap & 0xFFFFFFFF); break;
            case XHCI_OP_DCBAAP+4: v = (UINT32)(xhciDcbaap >> 32); break;
            case XHCI_OP_CONFIG:   v = xhciConfig; break;
            case XHCI_OP_PORTSC:   v = xhciPortsc; break;
            default: v = 0; break;
        }
    }
    if (size == 1) v &= 0xFF;
    else if (size == 2) v &= 0xFFFF;
    else if (size == 4) v &= 0xFFFFFFFF;
    return v;
}

void xhciRegWrite(WHV_PARTITION_HANDLE partition, UINT32 off, UINT32 size, UINT64 val) {
    UINT32 v = (UINT32)val;
    g_xhciMmioWrites++;
    if (off < XHCI_CAPLENGTH) return;            // capability registers are RO

    if (off >= XHCI_DBOFF) {
        // Doorbell. This is the whole point of xHCI for us: the driver TELLS us
        // there is work instead of us polling its schedules, and instead of it
        // polling our status registers.
        g_xhciDoorbells++;
        UINT32 db = (off - XHCI_DBOFF) / 4;
        if (db == 0) xhciRunCommandRing(partition);   // doorbell 0 = command ring
        return;
    }
    if (off >= XHCI_RTSOFF) {
        UINT32 r = off - XHCI_RTSOFF;
        switch (r) {
            case 0x20:
                // IMAN: IP (bit0) is write-1-to-clear, IE (bit1) is read/write.
                if (v & 1) xhciIman &= ~1u;
                xhciIman = (xhciIman & ~2u) | (v & 2u);
                break;
            case 0x24: xhciImod = v; break;
            case 0x28: xhciErstsz = v; break;
            case 0x30: xhciErstba = (xhciErstba & 0xFFFFFFFF00000000ULL) | v; break;
            case 0x34: xhciErstba = (xhciErstba & 0xFFFFFFFFULL) | ((UINT64)v << 32);
                       // ERSTBA's high dword lands last, so the table is complete
                       // here: read segment 0's base and size and arm the ring.
                       if (xhciErstba && xhciErstsz) {
                           xhciEventRingBase = xhciRead64(xhciErstba) & ~0x3FULL;
                           xhciEventRingSize = xhciRead32(xhciErstba + 8) & 0xFFFF;
                           xhciEventEnqueue  = xhciEventRingBase;
                           xhciEventPcs = 1;
                           printf("[xhci] event ring at 0x%llX, %u TRBs\n",
                                  (unsigned long long)xhciEventRingBase, xhciEventRingSize);
                           fflush(stdout);
                       }
                       break;
            case 0x38: xhciErdp = (xhciErdp & 0xFFFFFFFF00000000ULL) | v;
                       if (v & 8) xhciUsbSts &= ~XHCI_STS_EINT;  // EHB write-1-clear
                       break;
            case 0x3C: xhciErdp = (xhciErdp & 0xFFFFFFFFULL) | ((UINT64)v << 32); break;
            default: break;
        }
        return;
    }

    UINT32 r = off - XHCI_CAPLENGTH;
    switch (r) {
        case XHCI_OP_USBCMD:
            xhciUsbCmd = v;
            if (v & XHCI_CMD_HCRST) {
                // Host controller reset: everything back to defaults except the
                // BAR itself. Clear HCRST immediately -- the driver polls it and
                // a reset bit that never clears is an instant hang.
                xhciUsbCmd = 0;
                xhciUsbSts = XHCI_STS_HCH;
                xhciCrcr = xhciDcbaap = 0; xhciConfig = 0;
                xhciCmdRingPtr = 0; xhciCmdCcs = 1;
                xhciEventRingBase = xhciEventEnqueue = 0; xhciEventRingSize = 0;
                xhciEventPcs = 1; xhciSlotId = 0; xhciDeviceAddressed = 0;
                xhciPortAttached = 0; xhciPortsc = XHCI_PORTSC_PP;
                xhciIman = xhciImod = xhciErstsz = 0; xhciErstba = xhciErdp = 0;
                printf("[xhci] host controller reset\n"); fflush(stdout);
                break;
            }
            if (v & XHCI_CMD_RUN) {
                xhciUsbSts &= ~XHCI_STS_HCH;
                xhciAttachPort(partition);
            } else {
                xhciUsbSts |= XHCI_STS_HCH;
            }
            break;
        case XHCI_OP_USBSTS:
            // write-1-to-clear for the change bits
            xhciUsbSts &= ~(v & (XHCI_STS_HSE | XHCI_STS_EINT | XHCI_STS_PCD));
            break;
        case XHCI_OP_DNCTRL: xhciDnctrl = v; break;
        case XHCI_OP_CRCR:
            xhciCrcr = (xhciCrcr & 0xFFFFFFFF00000000ULL) | v;
            break;
        case XHCI_OP_CRCR + 4:
            xhciCrcr = (xhciCrcr & 0xFFFFFFFFULL) | ((UINT64)v << 32);
            xhciCmdRingPtr = xhciCrcr & ~0x3FULL;
            xhciCmdCcs = (int)(xhciCrcr & 1);
            break;
        case XHCI_OP_DCBAAP:
            xhciDcbaap = (xhciDcbaap & 0xFFFFFFFF00000000ULL) | v; break;
        case XHCI_OP_DCBAAP + 4:
            xhciDcbaap = (xhciDcbaap & 0xFFFFFFFFULL) | ((UINT64)v << 32); break;
        case XHCI_OP_CONFIG: xhciConfig = v; break;
        case XHCI_OP_PORTSC: {
            // Change bits are write-1-to-clear; PP and PED are writable; PR
            // starts a reset that completes immediately for an emulated port.
            UINT32 keep = xhciPortsc & ~(XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_PRC);
            xhciPortsc = keep & ~(v & (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_PRC));
            if (v & XHCI_PORTSC_PR) {
                xhciPortsc |= XHCI_PORTSC_PED | XHCI_PORTSC_PRC;
                xhciPortsc &= ~XHCI_PORTSC_PR;
                xhciPushEvent(partition, ((UINT64)1 << 24),
                              ((UINT32)XHCI_CC_SUCCESS << 24),
                              ((UINT32)TRB_PORT_STATUS_EVENT << 10));
            }
            break;
        }
        default: break;
    }
}

// Decode the faulting instruction the same way the EHCI and RTL8139 MMIO paths
// do, then dispatch to the register model above.
int xhciHandleMmio(WHV_PARTITION_HANDLE partition, WHV_RUN_VP_EXIT_CONTEXT *exitContext) {
    UINT64 gpa = exitContext->MemoryAccess.Gpa;
    if (!xhciBarMapped || !xhciBarBase) return 0;
    if (gpa < xhciBarBase || gpa >= (UINT64)xhciBarBase + XHCI_BAR_SIZE) return 0;
    UINT32 off = (UINT32)(gpa - xhciBarBase);

    unsigned char insn[16];
    int len = rtl8139FetchInsn(partition, exitContext, insn);
    int isWrite = 0, isImm = 0, regNum = 0, insnLen = 0;
    UINT32 imm = 0;
    if (!ioapicDecodeMmio(insn, len, &isWrite, &isImm, &regNum, &imm, &insnLen)) {
        static int failLog = 0;
        if (failLog++ < 12) {
            printf("[xhci-mmio] undecodable instruction at rip=0x%llX off=0x%03X\n",
                   (unsigned long long)exitContext->VpContext.Rip, off);
            fflush(stdout);
        }
        return 0;
    }

    // Bounded trace of the conversation, gated on the KERNEL being up.
    //
    // The first version was bounded at 120 entries and ungated, so it filled
    // entirely during the FIRMWARE phase -- OVMF's own xHCI driver polling
    // USBSTS 115 times -- and showed nothing of what Windows did afterwards,
    // even though the log proved Windows got further (it re-based the BAR and
    // reset the controller). Firmware and kernel are two different drivers and
    // only one of them has to work.
    static long xhciTrace = 0;
    int doTrace = (g_bpModuleBase != 0) && (xhciTrace < 400);
    // Collapse runs of the same read. A spin on one register is the signal we
    // are looking for, but printing it 115 times buries everything after it --
    // which is exactly how the firmware phase swallowed the whole first trace.
    static UINT32 lastOff = 0xFFFFFFFF; static long sameCount = 0;

    if (isWrite) {
        UINT32 value = imm;
        if (!isImm) {
            WHV_REGISTER_VALUE rv = { 0 };
            WHvGetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &rv);
            value = (UINT32)rv.Reg64;
        }
        if (doTrace) {
            if (sameCount > 1) printf("[xhci-io]   (previous read x%ld)\n", sameCount);
            sameCount = 0; lastOff = 0xFFFFFFFF;
            printf("[xhci-io] W +0x%03X = 0x%08X\n", off, value);
            fflush(stdout);
            xhciTrace++;
        }
        xhciRegWrite(partition, off, 4, value);
    } else {
        UINT64 v = xhciRegRead(off, 4);
        if (doTrace) {
            if (off == lastOff) {
                sameCount++;
            } else {
                if (sameCount > 1) printf("[xhci-io]   (previous read x%ld)\n", sameCount);
                printf("[xhci-io] R +0x%03X -> 0x%08X\n", off, (UINT32)v);
                fflush(stdout);
                xhciTrace++;
                lastOff = off; sameCount = 1;
            }
        }
        WHV_REGISTER_VALUE rv = { 0 };
        rv.Reg64 = v;
        WHvSetVirtualProcessorRegisters(partition, 0, &ioapicGprNames[regNum], 1, &rv);
    }

    WHV_REGISTER_NAME ripName = WHvX64RegisterRip;
    WHV_REGISTER_VALUE ripVal = { 0 };
    ripVal.Reg64 = exitContext->VpContext.Rip + (UINT64)insnLen;
    WHvSetVirtualProcessorRegisters(partition, 0, &ripName, 1, &ripVal);
    return 1;
}

// BAR0 sizing/programming, same protocol as ehciHandleBar0Access.
void xhciHandleBar0Access(WHV_X64_IO_PORT_ACCESS_CONTEXT *io, UINT32 baseOffset,
                          UINT32 accessSize, UINT64 *rax) {
    if (io->AccessInfo.IsWrite) {
        UINT32 written = (UINT32)io->Rax;
        if (baseOffset == 0x10 && accessSize >= 4) {
            if (written == 0xFFFFFFFF) { xhciBarSizing = 1; return; }
            xhciBarSizing = 0;
            UINT32 base = written & ~0xFu;
            if (base) {
                xhciBarBase = base;
                xhciBarMapped = 1;
                printf("[xhci] BAR0 at 0x%X -- trapped MMIO (%d bytes)\n",
                       xhciBarBase, XHCI_BAR_SIZE);
                fflush(stdout);
            }
        }
        return;
    }
    if (baseOffset == 0x10 && accessSize >= 4) {
        *rax = xhciBarSizing ? (UINT32)(~(XHCI_BAR_SIZE - 1) & ~0xFu)
                             : (xhciBarBase & ~0xFu);
    }
}
